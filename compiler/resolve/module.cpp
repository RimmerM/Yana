#include "module.h"
#include "analyze.h"
#include "core.h"
#include "expr.h"
#include "generic.h"
#include "name.h"
#include "native.h"
#include "witness.h"
#include "../parse/ast.h"

/*
 * Declaration resolution.
 *
 * A module is read in passes because each one needs the previous to have finished for the whole
 * module: a record's constructors may name a type declared later, a class signature needs every
 * type, an instance needs its class, and a function body needs every signature. The passes are
 * listed in prepareModule() below.
 *
 * Imports are resolved depth-first before the importing module is read at all, so by the time a
 * name is looked up every module it could come from is complete.
 */

// The Nth declaration as a pointer rather than a value, so a function can keep its own AST node
// and resolve its body once every signature in the program is known.
static ast::ParsePtr<ast::Decl> declAt(ast::DeclList decls, Size index) {
    return ast::ParsePtr<ast::Decl>(decls.list.p.offset + U32(sizeof(ast::Decl) * index));
}

/*
 * `@platform(js)` / `@platform(native)` - Analysis-JS.md §2.4, and the mechanism two implementation
 * documents already cite as though it existed.
 *
 * A target-selected declaration, and deliberately nothing more than that: an excluded declaration
 * is not resolved, so it contributes no name, no type, no instance and no body. That is what makes
 * `Storage(a)` be a host `Array` on JS and a length/capacity/address record on native without the
 * compiler knowing that `push` means `.push` - the alternative §2.4 rejects, because host knowledge
 * in codegen can neither be tested nor extended.
 *
 * Selection happens here rather than at any later stage for the same reason: a declaration the
 * target does not have must not be *resolvable* on it. A JS build that could still call the native
 * `Storage.reserve` would have two implementations of one name and would pick by accident.
 *
 * Multiple platforms may be listed (`@platform(js, native)` is every target and therefore
 * pointless, but `@platform` written twice on one declaration reads as "and", so all of them have
 * to accept). An unknown name is reported rather than silently excluding the declaration, since a
 * typo would otherwise delete a declaration from every build.
 *
 * `report` is false for every pass after the first. A module is read in eight passes and each of
 * them asks this question of each declaration, so reporting from all of them would say the same
 * thing eight times about one attribute.
 */
static bool platformEnabled(Module& module, const ast::Decl& decl, bool report = false) {
    auto attributes = decl.attributes;
    if(attributes.isEmpty()) return true;

    auto& context = module.context;
    auto platform = context.addUnqualifiedName("platform", 8);
    auto js = context.addUnqualifiedName("js", 2);
    auto native = context.addUnqualifiedName("native", 6);
    auto targetIsJs = isJsMode(context.settings.mode);
    auto enabled = true;

    for(auto attribute: attributes.contents(module.parse)) {
        if(attribute.name != platform) continue;

        if(attribute.args.isEmpty()) {
            if(report) {
                context.diagnostics.error("`@platform` needs at least one target - `@platform(js)` or `@platform(native)`"_v,
                                          attribute.source);
            }

            continue;
        }

        auto matched = false;
        for(auto arg: attribute.args.contents(module.parse)) {
            if(arg.value.kind != ast::Expr::Var) {
                if(report) {
                    context.diagnostics.error("a `@platform` target is a bare name - `js` or `native`"_v,
                                              arg.value.source);
                }

                continue;
            }

            if(arg.value.var == js) {
                matched = matched || targetIsJs;
            } else if(arg.value.var == native) {
                matched = matched || !targetIsJs;
            } else if(report) {
                context.diagnostics.error("unknown platform %@ - expected `js` or `native`"_v,
                                          arg.value.source, context.findName(arg.value.var));
            }
        }

        enabled = enabled && matched;
    }

    return enabled;
}

Program::Program(Context& context, Size typeMemory, Size irMemory):
    context(context), types(typeMemory), arena(irMemory) {}

Program::~Program() {
    for(auto module: modules) delete module;
    for(auto ast: embeddedAsts) delete ast;
}

Module* Program::addModule(StringId name, ast::ParseBase parse) {
    auto module = new Module(*this, name, parse);
    modules.push(module);
    return module;
}

Module* Program::findModule(StringId name) {
    for(auto module: modules) {
        if(module->name == name) return module;
    }

    return nullptr;
}

Module::Module(Program& program, StringId name, ast::ParseBase parse):
    program(program), context(program.context), types(program.types), arena(program.arena),
    scalar(program.scalar), coreClasses(program.coreClasses), name(name), parse(parse) {}

Function* Module::addFunction(StringId functionName, LocationId source) {
    auto found = functions.add(functionName);
    if(found.existed) {
        context.diagnostics.error("duplicate function %@"_v, source, context.findName(functionName));
        return (*arena)[*found.value];
    }

    auto function = new (arena) Function(this, functionName);
    function->source = source;
    *found.value = function - *arena;
    functionOrder.push(arena, function - *arena);

    function->addBlock(*this);
    return function;
}

Global* Module::addGlobal(StringId globalName, LocationId source) {
    auto found = globals.add(globalName);
    if(found.existed) {
        context.diagnostics.error("duplicate global %@"_v, source, context.findName(globalName));
        return (*arena)[*found.value];
    }

    auto global_ = new (arena) Global(this, globalName);
    global_->source = source;
    *found.value = global_ - *arena;
    globalOrder.push(arena, global_ - *arena);

    return global_;
}

// A function that is reachable through something other than its name - a class instance's
// implementation. It still gets a unique name so that printed and lowered output can tell two
// instances of the same method apart.
Function* addAnonymousFunction(Module& module, StringId functionName, LocationId source) {
    auto function = new (module.arena) Function(&module, functionName);
    function->source = source;
    module.functionOrder.push(module.arena, function - *module.arena);
    function->addBlock(module);
    return function;
}

Block* Module::entry(Function& function) {
    return (*arena)[function.blocks.get(*arena, 0)];
}

Block* Function::addBlock(Module& module, StringId blockName) {
    auto base = *module.arena;
    auto block = new (module.arena) Block(this - base, blockName, U16(blocks.size()));

    blocks.push(module.arena, block - base);
    return block;
}

Arg* Function::addArg(Module& module, StringId argName, TypePtr type, LocationId source) {
    auto base = *module.arena;
    auto arg = new (module.arena) Arg(module.entry(*this) - base, type, U16(args.size()));

    arg->name = argName;
    arg->source = source;
    arg->id = valueCounter++;
    args.push(module.arena, arg - base);

    return arg;
}

U32 Function::addLocal(Module& module, TypePtr type, StringId localName, ModulePtr<Value> value,
                       ast::BindType convention, bool borrowed, bool closureEnv) {
    auto index = U32(locals.size());
    locals.push(module.arena, Local {
        type, localName, value, convention, StorageClass::Stack, borrowed, closureEnv,
    });

    return index;
}

/*
 * Generic contexts.
 */

/*
 * Builds the generic context of one declaration: its declared type variables, then the class
 * constraints written over them. Constraint *classes* are resolved in a later pass, since a class
 * may be declared after the type that constrains itself by it.
 *
 * `open` says whether a type variable the constraints mention introduces itself. A function and an
 * instance have no declared variable list, so it has to - and it has to be set *here* rather than
 * by the caller afterwards, because a constraint's own types are resolved below: `f: (a) -> Int`
 * mentions `a` before the signature that would otherwise have introduced it, and a context that
 * only opened after this returned would have rejected it.
 */
static GlobalPtr<GenEnv> prepareGenEnv(Module& module, GenEnv::Kind kind,
                                       ast::ParseList<StringId> variables,
                                       ast::ConstraintList constraints, bool open = false) {
    auto env = new (module.types) GenEnv(kind);
    auto pointer = env - *module.types;
    env->open = open;

    auto addVariable = [&](StringId variableName, LocationId source) -> GlobalPtr<GenType> {
        for(auto existing: env->types.contents(*module.types)) {
            if((*module.types)[existing]->name == variableName) return existing;
        }

        auto type = new (module.types) GenType(pointer, variableName, U16(env->types.size()));
        auto typePointer = type - *module.types;
        env->types.push(module.types, typePointer);
        return typePointer;
    };

    for(auto variable: variables.contents(module.parse)) addVariable(variable, kNullLocation);

    for(auto constraint: constraints.contents(module.parse)) {
        switch(constraint.kind) {
            case ast::Constraint::Error:
                break;
            case ast::Constraint::Any:
                addVariable(constraint.name, constraint.source);
                break;
            case ast::Constraint::Class: {
                ClassConstraint entry;
                entry.name = constraint.type.name;
                entry.source = constraint.source;

                auto args = constraint.type.kind;
                for(auto arg: args.contents(module.parse)) {
                    auto variable = addVariable(arg, constraint.source);
                    entry.args.push(module.types, (Type*)(*module.types)[variable] - *module.types);
                }

                env->classes.push(module.types, entry);
                break;
            }
            case ast::Constraint::Field: {
                // `a.field: b` - a relation between two slots of *this* context rather than a fact
                // attached globally to `a`, which is what makes `Serialize`-shaped constraints
                // expressible at all. What satisfies it is a PropertyWitness for that one field.
                auto owner = addVariable(constraint.field.typeName, constraint.source);

                PropertyConstraint entry;
                entry.owner = (Type*)(*module.types)[owner] - *module.types;
                entry.field = constraint.field.fieldName;
                entry.source = constraint.source;
                entry.result = resolveType(module, *module.parse[constraint.field.type], env);

                env->properties.push(module.types, entry);
                break;
            }
            case ast::Constraint::Function: {
                // `f: (a) -> b`. The signature is a real function type, so the conventions and the
                // `return` group it promises survive into the requirement instead of being dropped
                // at the boundary - see FunArg.
                FunctionConstraint entry;
                entry.name = constraint.fun.name;
                entry.source = constraint.source;
                entry.signature = resolveType(module, *module.parse[constraint.fun.type], env);

                env->functions.push(module.types, entry);
                break;
            }
        }
    }

    return pointer;
}

static void resolveConstraintClasses(Module& module, GenEnv& env) {
    for(Size i = 0; i < env.classes.size(); i++) {
        auto constraint = env.classes.get(*module.types, i);
        if(constraint.typeClass || !constraint.name) continue;

        constraint.typeClass = findClass(module, constraint.name, constraint.source);
        if(!constraint.typeClass) {
            module.context.diagnostics.error("unknown class %@"_v, constraint.source,
                                             module.context.findName(constraint.name));
        }

        env.classes.set(*module.types, i, constraint);
    }
}

/*
 * Data, alias and class declarations.
 */

/*
 * A field default, reduced to the bits its field holds.
 *
 * The same literal shapes a global's initializer takes, plus a nullary constructor - which is what
 * `True` and `False` are, and so what a flags type is made of. The difference from declareGlobal is
 * that the type is already known here - it is the field's - so a literal is checked against it
 * rather than deciding it, and there is no `:: Type` form to write.
 */
static bool fieldDefaultBits(Module& module, const ast::Expr& expr, TypePtr type, U64& bits) {
    auto global = *module.types;
    auto& diagnostics = module.context.diagnostics;

    // A record whose constructors all carry nothing is its discriminant, so a nullary constructor
    // of one is as much a constant as a number is - and it is what `Bool` is made of, which makes
    // `read: Bool = False` the case this whole feature exists for.
    if(expr.kind == ast::Expr::Con) {
        auto& construct = *module.parse[expr.con];
        auto args = construct.args;

        if(construct.type.kind == ast::Type::Con && args.isEmpty()) {
            auto found = findConstructor(module, construct.type.name, expr.source);
            auto record = found ? (RecordType*)global[found.unwrap().record] : nullptr;

            if(record && record->layout == RecordType::Enum && (Type*)record - global == type) {
                bits = found.unwrap().index;
                return true;
            }
        }
    }

    if(!ast::isLiteral(expr)) {
        diagnostics.error("a field default must be a literal or a nullary constructor"_v, expr.source);
        return false;
    }

    auto literal = ast::Literal::Kind(expr.kind - ast::Expr::Lit);

    // A pointer field's default is an address written as an integer, which is the same spelling a
    // null pointer global takes.
    if(literal == ast::Literal::Int && (isInteger(global, type) || isPointer(global, type))) {
        bits = expr.lit.i();
        return true;
    }

    if(isFloat(global, type) &&
       (literal == ast::Literal::Int || literal == ast::Literal::Float || literal == ast::Literal::Double)) {
        auto number = literal == ast::Literal::Int  ? F64(expr.lit.i())
                    : literal == ast::Literal::Float ? F64(expr.lit.f)
                                                     : expr.lit.d();

        // Written as the bits the field will occupy, so that nothing has to convert again later.
        bits = floatBits(global, type, number);
        return true;
    }

    diagnostics.error("a field of type %@ cannot have this default"_v, expr.source,
                      describeType(module.context, global, type));
    return false;
}

/*
 * Records the defaults written in a record's constructors.
 *
 * A pass of its own, after every record has been defined, because a default may name a nullary
 * constructor of a record declared further down and the layout that makes one a constant is
 * decided by defineRecord. Instantiations inherit their declaration's constructors whole, so a
 * generic record's defaults reach `Maybe(Int)` with nothing to do here.
 */
static void declareRecordDefaults(Module& module, ast::Decl& decl) {
    auto global = *module.types;
    auto found = module.namedTypes.get(decl.data.type.name);
    if(!found || global[found.unwrap()]->kind != Type::Record) return;

    auto record = (RecordType*)global[found.unwrap()];
    Size at = 0;

    for(auto con: decl.data.cons.contents(module.parse)) {
        if(at >= record->constructors.size()) break;

        auto index = at++;
        auto constructor = record->constructors.get(global, index);
        auto content = constructor.content;
        if(!con.content) continue;

        // Only a tuple content has named fields to write a default on; a constructor wrapping one
        // unnamed type has nothing to leave out.
        auto& astContent = *module.parse[con.content];
        if(astContent.kind != ast::Type::Tup || !content || global[content]->kind != Type::Tup) continue;

        auto tuple = (TupType*)global[content];
        auto astFields = astContent.tup.fields;
        U16 field = 0;
        auto added = false;

        for(auto astField: astFields.contents(module.parse)) {
            if(astField.def && field < tuple->fields.size()) {
                U64 bits = 0;
                auto type = tuple->fields.get(global, field).type;

                if(fieldDefaultBits(module, *module.parse[astField.def], type, bits)) {
                    constructor.defaults.push(module.types, FieldDefault { field, bits });
                    added = true;
                }
            }

            field++;
        }

        if(added) record->constructors.set(global, index, constructor);
    }
}

// The record a declaration introduces, with its own type variables but no constructors yet. Shared
// by `data` and by the newtype an `alias qualified` declares, which differ only in what they then
// put in it.
static RecordType* declareRecordType(Module& module, ast::SimpleType& type, ast::ConstraintList constraints,
                                     bool qualified, LocationId source) {
    auto found = module.namedTypes.add(type.name);
    if(found.existed) {
        module.context.diagnostics.error("duplicate type %@"_v, source, module.context.findName(type.name));
        auto existing = *found.value;

        return existing && (*module.types)[existing]->kind == Type::Record
            ? (RecordType*)(*module.types)[existing]
            : nullptr;
    }

    auto record = new (module.types) RecordType(type.name);
    record->qualified = qualified;
    *found.value = (Type*)record - *module.types;

    auto variables = type.kind;
    if(variables.isNotEmpty() || constraints.isNotEmpty()) {
        record->gen = prepareGenEnv(module, GenEnv::Record, variables, constraints);
        record->generic = (*module.types)[record->gen]->types.isNotEmpty();
    }

    return record;
}

// Registers one constructor of `record`, and makes it reachable by its own name. A qualified
// record's constructors are addressed only as `Record.Constructor`, so they are not added to the
// module's flat constructor table.
static void declareConstructor(Module& module, RecordType& record, StringId name, U32 index,
                               bool qualified, LocationId source) {
    record.constructors.push(module.types, Constructor { name, nullptr, index });
    if(qualified) return;

    auto inserted = module.constructors.add(name);
    if(inserted.existed) {
        module.context.diagnostics.error("duplicate constructor %@"_v, source, module.context.findName(name));
    } else {
        *inserted.value = ConstructorRef { &record - *module.types, U16(index) };
    }
}

static void declareRecord(Module& module, ast::Decl& decl) {
    auto record = declareRecordType(module, decl.data.type, decl.data.constraints, decl.qualified, decl.source);
    if(!record) return;

    U32 index = 0;
    for(auto con: decl.data.cons.contents(module.parse)) {
        declareConstructor(module, *record, con.name, index++, decl.qualified, con.source);
    }
}

/*
 * `alias qualified Id = Int` - a newtype.
 *
 * A distinct type wrapping one other type, under a constructor of its own name. There is no way to
 * spell a different constructor name, and no ambiguity to resolve: what follows `=` in an `alias`
 * is a type, whereas under `data` the same words would be a constructor list.
 *
 * Everything past the declaration is an ordinary single-constructor record, so `Id(5)`, `Id(v)` in
 * a pattern, and every class instance over `Id` work with nothing further to teach them.
 */
static void declareNewtype(Module& module, ast::Decl& decl) {
    auto record = declareRecordType(module, decl.alias.type, {}, false, decl.source);
    if(!record) return;

    declareConstructor(module, *record, decl.alias.type.name, 0, false, decl.source);
}

// Fills in a record's constructor contents, once every type name in the module is registered so
// that one may name a type declared further down.
static void defineRecordContent(Module& module, RecordType& record, LocationId source,
                                Buffer<const ast::Type*> contents) {
    auto env = record.gen ? (*module.types)[record.gen] : nullptr;
    record.resolvingRepr = true;

    for(Size index = 0; index < contents.length && index < record.constructors.size(); index++) {
        auto stored = record.constructors.get(*module.types, index);
        stored.content = contents[index] ? resolveType(module, *contents[index], env) : module.scalar.unit;
        record.constructors.set(*module.types, index, stored);
    }

    record.resolvingRepr = false;
    computeRecordLayout(*module.types, record);
    record.definitionReady = true;
    if(!record.generic) finishRecordRepr(module, record, source);
}

// The record a data or qualified-alias declaration introduced, or null when the name turned out to
// be something else (a duplicate that lost, or a plain alias).
static RecordType* declaredRecord(Module& module, StringId name) {
    auto found = module.namedTypes.get(name);
    if(!found || (*module.types)[found.unwrap()]->kind != Type::Record) return nullptr;

    return (RecordType*)(*module.types)[found.unwrap()];
}

static void defineRecord(Module& module, ast::Decl& decl) {
    auto record = declaredRecord(module, decl.data.type.name);
    if(!record) return;

    Array<const ast::Type*> contents;
    for(auto con: decl.data.cons.contents(module.parse)) {
        contents.push(con.content ? module.parse[con.content] : nullptr);
    }

    defineRecordContent(module, *record, decl.source, toBuffer(contents));
}

static void defineNewtype(Module& module, ast::Decl& decl) {
    auto record = declaredRecord(module, decl.alias.type.name);
    if(!record) return;

    const ast::Type* content = &decl.alias.target;
    defineRecordContent(module, *record, decl.source, { &content, 1 });
}

static void declareAlias(Module& module, ast::Decl& decl, ast::ParsePtr<ast::Decl> pointer) {
    auto found = module.aliases.add(decl.alias.type.name);
    if(found.existed || module.namedTypes.get(decl.alias.type.name)) {
        module.context.diagnostics.error("duplicate type %@"_v, decl.source,
                                         module.context.findName(decl.alias.type.name));
        return;
    }

    TypeAlias alias;
    alias.name = decl.alias.type.name;
    alias.module = &module;
    alias.ast = pointer;
    alias.source = decl.source;

    auto variables = decl.alias.type.kind;
    if(variables.isNotEmpty()) alias.gen = prepareGenEnv(module, GenEnv::Alias, variables, {});

    *found.value = alias;
}

/*
 * A module-level `let`.
 *
 * There is no program point at which module-level code would run, so a global's initializer is a
 * constant rather than an expression: a literal, or a literal coerced to the type the global is
 * meant to have. `let &heapNext = 0 :: %U8` is therefore both the declaration and the whole of
 * what it starts as, which is what a runtime's static state actually needs and no more.
 */
static void declareGlobal(Module& module, ast::Decl& decl) {
    auto parse = module.parse;
    auto& context = module.context;

    if(decl.stmt.kind != ast::Expr::Decl) {
        context.diagnostics.error("only a `let` declaration can appear at module level"_v, decl.source);
        return;
    }

    for(auto declaration: decl.stmt.decl.contents(parse)) {
        if(declaration.pat.kind != ast::Pat::Var) {
            context.diagnostics.error("a global must be declared as a single name"_v, declaration.pat.source);
            continue;
        }

        if(declaration.bind != ast::BindType::Borrow && declaration.bind != ast::BindType::Ref) {
            context.diagnostics.error("a global is either plain or `&` mutable"_v, declaration.pat.source);
            continue;
        }

        if(!declaration.content) {
            context.diagnostics.error("a global requires a constant initializer"_v, declaration.pat.source);
            continue;
        }

        // `0 :: %U8` names the type as well as the value, which is the only way a global says
        // what it is: a `let` pattern carries no type annotation.
        auto& content = *parse[declaration.content];
        auto value = &content;
        TypePtr type = nullptr;

        if(content.kind == ast::Expr::Coerce) {
            auto& coerce = *parse[content.coerce];
            type = resolveType(module, coerce.type);
            value = &coerce.target;
        }

        if(!ast::isLiteral(*value)) {
            context.diagnostics.error("a global's initializer must be a literal, optionally written `literal :: Type`"_v,
                                      value->source);
            continue;
        }

        auto literal = ast::Literal::Kind(value->kind - ast::Expr::Lit);
        U64 initial = 0;

        switch(literal) {
            case ast::Literal::Int:
                if(!type) type = module.scalar.int_;
                initial = value->lit.i();
                break;
            case ast::Literal::Double:
            case ast::Literal::Float: {
                if(!type) type = module.scalar.float_;

                // A float's initial value is its storage, so it is written as the bits it will
                // occupy rather than as a number the emitter would have to convert again.
                auto number = literal == ast::Literal::Float ? F64(value->lit.f) : value->lit.d();
                initial = floatBits(*module.types, type, number);
                break;
            }
            default:
                context.diagnostics.error("a global's initializer must be a number"_v, value->source);
                continue;
        }

        if(!isDirectType(*module.types, type) && !isMemoryType(*module.types, type)) {
            context.diagnostics.error("a global cannot have this type"_v, declaration.pat.source);
            continue;
        }

        auto global_ = module.addGlobal(declaration.pat.var, declaration.pat.source);
        global_->type = type;
        global_->initial = initial;
        global_->mut = declaration.bind == ast::BindType::Ref;
    }
}

static void declareClass(Module& module, ast::Decl& decl, ast::ParsePtr<ast::Decl> pointer) {
    auto found = module.classes.add(decl.trait.type.name);
    if(found.existed) {
        module.context.diagnostics.error("duplicate class %@"_v, decl.source,
                                         module.context.findName(decl.trait.type.name));
        return;
    }

    auto variables = decl.trait.type.kind;
    auto env = prepareGenEnv(module, GenEnv::Class, variables, decl.trait.constraints);
    auto typeClass = new (module.types) TypeClass(decl.trait.type.name, env);

    typeClass->module = &module;
    typeClass->ast = pointer;
    typeClass->source = decl.source;
    *found.value = typeClass - *module.types;

    if((*module.types)[env]->types.isEmpty()) {
        module.context.diagnostics.error("class %@ must take at least one type argument"_v, decl.source,
                                         module.context.findName(decl.trait.type.name));
    }

    U16 index = 0;
    auto decls = decl.trait.decls;

    for(auto member: decls.contents(module.parse)) {
        if(member.kind != ast::Decl::Fun) {
            module.context.diagnostics.error("a class body may only contain function signatures"_v, member.source);
            continue;
        }

        // Two functions of one class may share a name when their arities differ. That is what
        // lets `Num` declare both the binary and the unary `-`, which is the shape the language
        // already has whether or not the class machinery accommodates it.
        for(auto existing: typeClass->functions.contents(*module.types)) {
            if(existing.name != member.fun.name) continue;
            if(existing.arity != U16(member.fun.args.size())) continue;

            module.context.diagnostics.error("duplicate class function %@"_v, member.source,
                                             module.context.findName(member.fun.name));
        }

        typeClass->functions.push(module.types, ClassFun { member.fun.name, nullptr, index, U16(member.fun.args.size()) });
        module.classFunctions.push(ClassFunRef { typeClass - *module.types, member.fun.name, index });
        index++;
    }
}

// Resolves one function signature against a generic context, producing a body-less Function.
static Function* resolveSignature(Module& module, ast::Decl& decl, GenEnv* env, StringId name, bool anonymous) {
    auto function = anonymous ? addAnonymousFunction(module, name, decl.source)
                              : module.addFunction(name, decl.source);

    if(decl.fun.kind != ast::FunKind::Plain) {
        module.context.diagnostics.error("lens and iter functions are not available yet"_v, decl.source);
    }

    function->returnType = decl.fun.ret ? resolveType(module, *module.parse[decl.fun.ret], env)
                                        : module.scalar.unit;

    U16 index = 0;
    auto roots = 0u;

    // Markers that were written, valid or not. A signature whose only marker was rejected has
    // already been told what is wrong with it, and "you must mark an argument" would be the second
    // diagnostic about the same line saying the opposite of the first.
    auto written = 0u;
    auto allRootsMutable = true;

    for(auto arg: decl.fun.args.contents(module.parse)) {
        if(!arg.type) {
            module.context.diagnostics.error("function arguments require an explicit type"_v, arg.source);
            function->addArg(module, arg.name, module.scalar.error, arg.source);
            index++;
            continue;
        }

        auto type = resolveType(module, *module.parse[arg.type], env);
        auto declared = function->addArg(module, arg.name, type, arg.source);
        declared->convention = arg.bind;
        declared->returnRoot = arg.returnRoot;

        // What may carry the marker is one rule shared with a written function type - see
        // checkReturnRoot, which is where it and its diagnostics live.
        if(arg.returnRoot) {
            written++;

            if(checkReturnRoot(module, type, arg.bind, index, arg.source)) {
                roots++;
                if(arg.bind != ast::BindType::Ref) allRootsMutable = false;
            } else {
                declared->returnRoot = false;
            }
        }

        index++;
    }

    /*
     * What makes a returned borrow exclusive.
     *
     * `&T` in type position says a borrow and not which kind, because the answer is not a property
     * of the result: Design.md's rule is that "a returned mutable borrow must be rooted in a
     * `return &` mutable parameter", so the result is exclusive exactly when every member of the
     * group it may be rooted in is. A mixed group yields the weaker capability, which is the same
     * rule read the other way - an immutable result may be rooted in either kind.
     */
    if(isBorrow(*module.types, function->returnType)) {
        if(!roots) {
            if(!written) {
                module.context.diagnostics.error("a function returning a borrow must mark the argument it is rooted in with `return`"_v,
                                                 decl.source);
            }
        } else {
            function->returnType = applyReturnRootMutability(module, function->returnType, allRootsMutable);
        }
    }

    return function;
}

/*
 * Class default implementations.
 *
 * A default is a body the class writes for one of its own signatures, used by every instance that
 * does not supply that function. What it is, exactly, is a generic function over the class's type
 * variables carrying the class itself as a requirement - `fn (Eq(a)) !=(lhs: a, rhs: a) -> Bool`
 * written inside `class Eq(a)` - which is structurally what a parametric instance's implementation
 * already is. So the body is resolved once against those variables and specialized per instance
 * that inherited it, and nothing about overloading changes: a default is a fallback body for a
 * signature that is already a member of the set, not a new member of it.
 *
 * A default body is part of the class's exported contract. Changing one changes behavior for every
 * instance that did not override it, in every module that compiled against the class.
 */

// `Eq.!=`, the name a default is printed and specialized under. It is not addressable in source;
// what it needs is to be unique and to say where it came from.
static StringId classDefaultName(Module& module, TypeClass& typeClass, StringId method) {
    StringBuilder text;
    text << module.context.findName(typeClass.name) << '.' << module.context.findName(method);
    return module.context.addQualifiedName(text.pointer(), text.size(), 1);
}

static ModulePtr<Function> resolveClassDefault(Module& module, TypeClass& typeClass, ast::Decl& member,
                                               ast::ParsePtr<ast::Decl> pointer, Function& signature) {
    auto global = *module.types;
    auto classEnv = global[typeClass.gen];

    // The context holds the class's own variables rather than copies of them, so the signature's
    // types can be reused as they are - substitution and matching both go by variable index, and
    // these are the indices an instance selection binds. Only the requirements differ: the class's
    // superclasses come along, which is what lets `Num`'s unary `-` default to `0 - value` through
    // the `FromInt` its class already declares, and the class itself is added, since that is what a
    // default calling a sibling needs and what pushing onto the class's own context would instead
    // have turned into a further superclass.
    auto env = new (module.types) GenEnv(GenEnv::Function);
    for(auto variable: classEnv->types.contents(global)) env->types.push(module.types, variable);

    auto addConstraint = [&](GlobalPtr<TypeClass> target, StringId name, LocationId source, Buffer<TypePtr> args) {
        ClassConstraint constraint;
        constraint.typeClass = target;
        constraint.name = name;
        constraint.source = source;
        for(auto arg: args) constraint.args.push(module.types, arg);
        env->classes.push(module.types, constraint);
    };

    for(auto constraint: classEnv->classes.contents(global)) {
        Array<TypePtr> args;
        for(auto arg: constraint.args.contents(global)) args.push(arg);
        addConstraint(constraint.typeClass, constraint.name, constraint.source, toBuffer(args));
    }

    Array<TypePtr> own;
    for(auto variable: classEnv->types.contents(global)) own.push((Type*)global[variable] - global);
    addConstraint((TypeClass*)&typeClass - global, typeClass.name, member.source, toBuffer(own));

    auto function = addAnonymousFunction(module, classDefaultName(module, typeClass, member.fun.name), member.source);
    function->gen = env - global;
    function->returnType = signature.returnType;
    function->ast = pointer;

    for(auto argPointer: signature.args.contents(*module.arena)) {
        auto arg = (*module.arena)[argPointer];
        auto copied = function->addArg(module, arg->name, arg->type, arg->source);
        copied->convention = arg->convention;
        copied->returnRoot = arg->returnRoot;
    }

    return function - *module.arena;
}

/*
 * The rank rule.
 *
 * Haskell's known hazard is a pair of defaults that call each other - `==` as `not (a /= b)` and
 * `/=` as `not (a == b)` - where an instance supplying neither compiles and hangs. The answer here
 * is a check rather than an informal pragma, since the language's bias is one primitive operation
 * per class rather than a choice of which one to implement:
 *
 *   A function with no default has rank 0; a default may only call class functions of strictly
 *   lower rank than its own.
 *
 * Ranks are inferred rather than written, so what the rule asks is that the defaults of one class
 * do not depend on each other in a circle. That is decidable at the declaration, which is where it
 * is decided - before any body has been resolved and so before any of them could be instantiated.
 */

// One name written in call position in a default body, by the key an overload set is arranged by.
struct DefaultCall {
    StringId name = 0;
    U16 arity = 0;
};

static void collectCalls(ast::ParseBase parse, ast::Expr expr, Array<DefaultCall>& target);

static void collectCallee(ast::ParseBase parse, ast::Expr callee, U16 arity, Array<DefaultCall>& target) {
    if(callee.kind == ast::Expr::Var) target.push(DefaultCall { callee.var, arity });
    else collectCalls(parse, callee, target);
}

/*
 * Every name one default body could be calling.
 *
 * Deliberately syntactic and over-approximate. A name in call position counts as a call whether or
 * not selection would have chosen the class function of that name, so a default that shadows a
 * sibling with a plain function of the same name is refused rather than ranked as if it had not
 * called it. Rejecting a declaration that would have worked is a cost; ranking one that hangs is
 * not a cost this check is allowed to have.
 */
static void collectCalls(ast::ParseBase parse, ast::Expr expr, Array<DefaultCall>& target) {
    auto walk = [&](ast::Expr child) { collectCalls(parse, child, target); };
    auto walkPointer = [&](ast::ParsePtr<ast::Expr> child) { if(child) walk(*parse[child]); };
    auto walkArgs = [&](ast::ParseList<ast::TupArg> args) {
        for(auto arg: args.contents(parse)) walk(arg.value);
    };

    switch(expr.kind) {
        case ast::Expr::Multi:
            for(auto child: expr.multi.contents(parse)) walk(child);
            break;
        case ast::Expr::App:
        case ast::Expr::Sub: {
            auto& app = *parse[expr.kind == ast::Expr::App ? expr.app : expr.sub];
            collectCallee(parse, app.callee, U16(app.args.size()), target);
            walkArgs(app.args);
            break;
        }
        case ast::Expr::Fun: {
            auto& fun = *parse[expr.fun];
            for(auto arg: fun.args.contents(parse)) walkPointer(arg.def);
            walk(fun.body);
            break;
        }
        case ast::Expr::Infix: {
            auto& infix = *parse[expr.infix];
            collectCallee(parse, infix.op, 2, target);
            walk(infix.lhs);
            walk(infix.rhs);
            break;
        }
        case ast::Expr::Prefix: {
            auto& prefix = *parse[expr.prefix];
            collectCallee(parse, prefix.op, 1, target);
            walk(prefix.on);
            break;
        }
        case ast::Expr::If: {
            auto& branch = *parse[expr.singleIf];
            walk(branch.cond);
            walk(branch.then);
            if(branch.otherwise) walk(branch.otherwise.unwrap());
            break;
        }
        case ast::Expr::MultiIf:
            for(auto branch: expr.multiIf.contents(parse)) {
                walk(branch.cond);
                walk(branch.then);
            }
            break;
        case ast::Expr::Decl:
            for(auto var: expr.decl.contents(parse)) {
                walkPointer(var.content);
                walkPointer(var.in);
                for(auto alt: var.alts.contents(parse)) walk(alt.expr);
            }
            break;
        case ast::Expr::While: {
            auto& loop = *parse[expr.whileLoop];
            walk(loop.cond);
            walk(loop.body);
            break;
        }
        case ast::Expr::For: {
            auto& loop = *parse[expr.forLoop];
            walk(loop.from);
            walkPointer(loop.to);
            walkPointer(loop.step);
            walk(loop.body);
            break;
        }
        case ast::Expr::Assign: {
            auto& assign = *parse[expr.assign];
            walk(assign.target);
            walk(assign.value);
            break;
        }
        case ast::Expr::Nested:
            walkPointer(expr.nested);
            break;
        case ast::Expr::Coerce:
            walk(parse[expr.coerce]->target);
            break;
        case ast::Expr::Field: {
            auto& field = *parse[expr.field];
            walk(field.target);
            walk(field.field);
            break;
        }
        case ast::Expr::Con:
            walkArgs(parse[expr.con]->args);
            break;
        case ast::Expr::Tup:
            walkArgs(expr.tup);
            break;
        case ast::Expr::TupUpdate: {
            auto& update = *parse[expr.tupUpdate];
            walk(update.value);
            for(auto arg: update.args.contents(parse)) walk(arg.value);
            break;
        }
        case ast::Expr::Array:
            for(auto child: expr.arr.contents(parse)) walk(child);
            break;
        case ast::Expr::Map:
            for(auto entry: expr.map.contents(parse)) {
                walk(entry.key);
                walk(entry.value);
            }
            break;
        case ast::Expr::Format:
            for(auto chunk: expr.format.contents(parse)) walkPointer(chunk.format);
            break;
        case ast::Expr::Match: {
            auto& match = *parse[expr.match];
            walk(match.pivot);
            for(auto alt: match.alts.contents(parse)) walk(alt.expr);
            break;
        }
        case ast::Expr::Range: {
            auto& range = *parse[expr.range];
            walk(range.from);
            walk(range.to);
            break;
        }
        case ast::Expr::Ret:
            walkPointer(expr.ret);
            break;
        case ast::Expr::Yield:
            walkPointer(expr.yield);
            break;
        case ast::Expr::Break:
            walkPointer(expr.breakValue);
            break;
        case ast::Expr::Is:
            walk(parse[expr.is]->value);
            break;
        default:
            // Error, Var, Continue and the literals call nothing. A bare name is not a call today:
            // function values are rejected by the resolver, so a class function's name can only
            // reach it from one of the positions above.
            break;
    }
}

// Ranks one default, depth first, reporting the circle if its body leads back to a default that is
// still being ranked. A default that takes part in one loses it, so that what follows is an
// instance that has to write the function rather than a compiler that instantiates forever.
static void rankDefault(Module& module, TypeClass& typeClass, Size index, Array<U8>& state) {
    auto global = *module.types;
    auto entry = typeClass.functions.get(global, index);
    if(state[index] || !entry.defaultFun) return;

    state[index] = 1;

    auto& decl = *module.parse[(*module.arena)[entry.defaultFun]->ast];
    Array<DefaultCall> calls;
    if(decl.fun.body) collectCalls(module.parse, *module.parse[decl.fun.body], calls);

    U16 rank = 1;
    auto circular = false;

    for(auto& call: calls) {
        for(Size other = 0; other < typeClass.functions.size(); other++) {
            auto called = typeClass.functions.get(global, other);
            if(called.name != call.name || called.arity != call.arity) continue;

            if(state[other] == 1) {
                if(other == index) {
                    module.context.diagnostics.error(
                        "the default for %@ calls %@ - a default may only call class functions of strictly lower rank than its own, and nothing is lower than itself"_v,
                        decl.source, module.context.findName(entry.name), module.context.findName(called.name));
                } else {
                    module.context.diagnostics.error(
                        "the default for %@ calls %@, whose own default leads back to it - an instance supplying neither would have nothing to run. A default may only call class functions of strictly lower rank, so one of the two has to be left for every instance to write"_v,
                        decl.source, module.context.findName(entry.name), module.context.findName(called.name));
                }

                circular = true;
                break;
            }

            rankDefault(module, typeClass, other, state);

            auto ranked = typeClass.functions.get(global, other).rank;
            if(ranked + 1 > rank) rank = U16(ranked + 1);
            break;
        }

        if(circular) break;
    }

    entry.rank = circular ? 0 : rank;
    if(circular) entry.defaultFun = nullptr;

    typeClass.functions.set(global, index, entry);
    state[index] = 2;
}

static void checkDefaultRanks(Module& module, TypeClass& typeClass) {
    Array<U8> state;
    for(Size i = 0; i < typeClass.functions.size(); i++) state.push(0);
    for(Size i = 0; i < typeClass.functions.size(); i++) rankDefault(module, typeClass, i, state);
}

static void resolveClassSignatures(Module& module, TypeClass& typeClass) {
    if(typeClass.ready) return;
    typeClass.ready = true;

    auto env = (*module.types)[typeClass.gen];
    resolveConstraintClasses(module, *env);

    auto& decl = *module.parse[typeClass.ast];
    auto decls = decl.trait.decls;
    Size index = 0;

    for(Size memberIndex = 0; memberIndex < decls.size(); memberIndex++) {
        auto pointer = declAt(decls, memberIndex);
        auto& member = *module.parse[pointer];

        if(member.kind != ast::Decl::Fun) continue;
        if(index >= typeClass.functions.size()) break;

        if(!member.fun.ret) {
            module.context.diagnostics.error("a class function requires an explicit return type"_v, member.source);
        }

        auto signature = resolveSignature(module, member, env, member.fun.name, true);
        signature->instanceOf = (TypeClass*)&typeClass - *module.types;
        signature->signature = true;

        auto stored = typeClass.functions.get(*module.types, index);
        stored.fun = signature - *module.arena;
        if(member.fun.body) stored.defaultFun = resolveClassDefault(module, typeClass, member, pointer, *signature);
        typeClass.functions.set(*module.types, index, stored);
        index++;
    }

    checkDefaultRanks(module, typeClass);
}

/*
 * Instances.
 */

StringId instanceFunctionName(Module& module, TypeClass& typeClass, Buffer<TypePtr> args, StringId method) {
    StringBuilder text;
    text << module.context.findName(typeClass.name) << '(';
    describeTypes(module.context, *module.types, args, text);
    text << ")." << module.context.findName(method);

    return module.context.addQualifiedName(text.pointer(), text.size(), 1);
}

// Whether one variable of a generic context is reachable inside a type. Instance selection binds
// by variable index, so occurrence is asked by index too rather than by identity.
static bool mentionsVariable(GlobalBase global, TypePtr type, U16 index) {
    if(!isGeneric(global, type)) return false;

    switch(global[type]->kind) {
        case Type::Gen:
            return ((GenType*)global[type])->index == index;
        case Type::Ptr:
            return mentionsVariable(global, ((PtrType*)global[type])->to, index);
        case Type::Tup: {
            auto tuple = (TupType*)global[type];

            for(Size i = 0; i < tuple->fields.size(); i++) {
                if(mentionsVariable(global, tuple->fields.get(global, i).type, index)) return true;
            }

            return false;
        }
        case Type::Record: {
            auto record = (RecordType*)global[type];

            for(auto arg: record->instanceArgs.contents(global)) {
                if(mentionsVariable(global, arg, index)) return true;
            }

            return false;
        }
        default:
            return false;
    }
}

/*
 * One instance declaration.
 *
 * The head is resolved in an open generic context of its own, so `instance Ord(Ptr(a))` introduces
 * `a` by using it exactly as a function signature does. A head that used no variable leaves the
 * context empty and the instance is the concrete one it always was; a head that used one is
 * selected by matching instead of by equality (see matchInstance), and each of its implementations
 * becomes a generic function over that context, specialized for what a selection bound.
 */
static void resolveInstance(Module& module, ast::Decl& decl) {
    auto& type = decl.instance.type;
    StringId className = 0;
    Array<TypePtr> args;

    // The constraints are read first, since they name the variables the head is written over -
    // and a constraint may name one the head does not, which is what the check below reports.
    auto genPointer = prepareGenEnv(module, GenEnv::Instance, {}, decl.instance.constraints, true);
    auto gen = (*module.types)[genPointer];

    if(type.kind == ast::Type::App) {
        auto& app = *module.parse[type.app];
        if(app.base.kind != ast::Type::Con) {
            module.context.diagnostics.error("an instance must name a class"_v, decl.source);
            return;
        }

        className = app.base.name;
        auto appArgs = app.args;
        for(auto arg: appArgs.contents(module.parse)) args.push(resolveType(module, arg, gen));
    } else {
        module.context.diagnostics.error(
            "type-namespaced instances are not available yet - write `instance Class(Type)`"_v, decl.source);
        return;
    }

    auto classPointer = findClass(module, className, decl.source);
    if(!classPointer) {
        module.context.diagnostics.error("unknown class %@"_v, decl.source, module.context.findName(className));
        return;
    }

    // The two implicit classes have no instances to write: whether a type is TrivialCopy is decided
    // by whether every one of its members is, and an instance saying otherwise would be a claim the
    // compiler has already contradicted. What a type that needs different behaviour writes is
    // `Copy` or `Sink`, which is exactly what those are for.
    if(classPointer == module.coreClasses.trivialCopy || classPointer == module.coreClasses.trivialSink) {
        module.context.diagnostics.error("%@ is decided structurally and has no instances to write - a type that cannot be duplicated or relocated bitwise says so by writing `Copy` or `Sink`"_v,
                                         decl.source, module.context.findName(className));
        return;
    }

    auto typeClass = (*module.types)[classPointer];
    resolveClassSignatures(*typeClass->module, *typeClass);

    auto expected = (*module.types)[typeClass->gen]->types.size();
    if(args.size() != expected) {
        module.context.diagnostics.error("class %@ takes %@ arguments but this instance gives %@"_v, decl.source,
                                         module.context.findName(className), U32(expected), U32(args.size()));
        return;
    }

    // A variable the head does not mention is one nothing could ever bind, so a constraint over it
    // says something no selection can check.
    for(auto variable: gen->types.contents(*module.types)) {
        auto index = (*module.types)[variable]->index;
        auto mentioned = args.contains([&](TypePtr arg) { return mentionsVariable(*module.types, arg, index); });
        if(mentioned) continue;

        module.context.diagnostics.error("type variable %@ does not appear in the instance head"_v, decl.source,
                                         module.context.findName((*module.types)[variable]->name));
        return;
    }

    // Closed once the head has been read: from here on a lowercase name in the body means one of
    // the variables the head introduced, and anything else is the error it would be in a function.
    gen->open = false;
    resolveConstraintClasses(module, *gen);
    auto parametric = gen->types.isNotEmpty();

    auto instance = new (module.arena) ClassInstance(classPointer);
    instance->module = &module;
    instance->source = decl.source;
    instance->gen = parametric ? genPointer : nullptr;
    for(auto arg: args) instance->forTypes.push(module.arena, arg);
    for(Size i = 0; i < typeClass->functions.size(); i++) instance->functions.push(module.arena, nullptr);

    // Two instances one of which is no more specific than the other would make selection depend on
    // declaration order, whether they are written for the same types or for heads that mean the
    // same thing - `Eq(Ptr(a))` twice, under two names for `a`.
    Array<ModulePtr<ClassInstance>> existing;
    findInstances(module, classPointer, existing);

    for(auto other: existing) {
        auto& previous = *(*module.arena)[other];
        if(!instanceCovers(module, previous, *instance) || !instanceCovers(module, *instance, previous)) continue;

        module.context.diagnostics.error("duplicate instance of %@ for these types"_v, decl.source,
                                         module.context.findName(className));
        return;
    }

    auto decls = decl.instance.decls;
    for(Size memberIndex = 0; memberIndex < decls.size(); memberIndex++) {
        auto memberPointer = declAt(decls, memberIndex);
        auto& member = *module.parse[memberPointer];

        if(member.kind != ast::Decl::Fun) {
            module.context.diagnostics.error("an instance body may only contain function definitions"_v, member.source);
            continue;
        }

        Size index = maxLimit<Size>;
        for(Size i = 0; i < typeClass->functions.size(); i++) {
            auto entry = typeClass->functions.get(*module.types, i);
            if(entry.name != member.fun.name || entry.arity != U16(member.fun.args.size())) continue;

            index = i;
            break;
        }

        if(index == maxLimit<Size>) {
            module.context.diagnostics.error("class %@ has no function %@"_v, member.source,
                                             module.context.findName(className),
                                             module.context.findName(member.fun.name));
            continue;
        }

        if(instance->functions.get(*module.arena, index)) {
            module.context.diagnostics.error("duplicate implementation of %@"_v, member.source,
                                             module.context.findName(member.fun.name));
            continue;
        }

        auto signature = (*module.arena)[typeClass->functions.get(*module.types, index).fun];
        if(!signature) continue;

        // The implementation's signature is the class signature with the instance's types
        // substituted in, so an instance body does not have to repeat types the class already
        // fixed. Anything the source does write is checked against it rather than replacing it.
        auto function = addAnonymousFunction(
            module, instanceFunctionName(module, *typeClass, toBuffer(args), member.fun.name), member.source);

        function->instanceOf = classPointer;
        function->gen = instance->gen;
        for(auto arg: args) function->instanceArgs.push(module.arena, arg);
        function->returnType = substituteType(module, signature->returnType, toBuffer(args), member.source);
        function->ast = nullptr;

        auto declaredArgs = member.fun.args.contents(module.parse);
        if(declaredArgs.size() != signature->args.size()) {
            module.context.diagnostics.error("%@ takes %@ arguments in class %@, but %@ here"_v, member.source,
                                             module.context.findName(member.fun.name), U32(signature->args.size()),
                                             module.context.findName(className), U32(declaredArgs.size()));
            continue;
        }

        for(Size i = 0; i < signature->args.size(); i++) {
            auto classArg = (*module.arena)[signature->args.get(*module.arena, i)];
            auto expectedType = substituteType(module, classArg->type, toBuffer(args), member.source);
            auto declared = declaredArgs[i];

            if(declared.type) {
                auto written = resolveType(module, *module.parse[declared.type], gen);
                if(!sameType(written, expectedType)) {
                    module.context.diagnostics.error("argument %@ has type %@ here but %@ in class %@"_v, declared.source,
                                                     module.context.findName(declared.name),
                                                     describeType(module.context, *module.types, written),
                                                     describeType(module.context, *module.types, expectedType),
                                                     module.context.findName(className));
                }
            }

            // The convention is as much of the contract as the type is: an instance whose `drop`
            // borrows what the class said it consumes would be called by every generic caller as
            // though the value were gone. It is taken from the class rather than from what the
            // implementation wrote, and disagreeing is reported the same way a type does.
            if(declared.bind != classArg->convention) {
                module.context.diagnostics.error("argument %@ is declared %@ here but %@ in class %@"_v, declared.source,
                                                 module.context.findName(declared.name),
                                                 conventionName(declared.bind), conventionName(classArg->convention),
                                                 module.context.findName(className));
            }

            auto implArg = function->addArg(module, declared.name ? declared.name : classArg->name,
                                            expectedType, declared.source);
            implArg->convention = classArg->convention;
            implArg->returnRoot = classArg->returnRoot;
        }

        if(member.fun.ret) {
            auto written = resolveType(module, *module.parse[member.fun.ret], gen);
            if(!sameType(written, function->returnType)) {
                module.context.diagnostics.error("%@ returns %@ here but %@ in class %@"_v, member.source,
                                                 module.context.findName(member.fun.name),
                                                 describeType(module.context, *module.types, written),
                                                 describeType(module.context, *module.types, function->returnType),
                                                 module.context.findName(className));
            }
        }

        function->ast = memberPointer;
        instance->functions.set(*module.arena, index, function - *module.arena);
    }

    for(Size i = 0; i < typeClass->functions.size(); i++) {
        if(instance->functions.get(*module.arena, i)) continue;

        // A signature the class wrote a body for asks nothing of the instance. The default stands
        // in the slot as it is - generic over the class's variables - and is specialized for these
        // types the first time a call reaches it; see emitInstanceCall.
        auto entry = typeClass->functions.get(*module.types, i);
        if(entry.defaultFun) {
            instance->functions.set(*module.arena, i, entry.defaultFun);
            continue;
        }

        module.context.diagnostics.error("instance of %@ does not implement %@"_v, decl.source,
                                         module.context.findName(className),
                                         module.context.findName(entry.name));
    }

    module.instances.push(instance - *module.arena);
}

/*
 * Defaults and superclasses.
 */

// `default Class = Type`. The type is checked here rather than where a literal settles on it, so
// that a default which nothing implements is reported against the declaration that wrote it.
static void resolveDefault(Module& module, ast::Decl& decl) {
    auto& context = module.context;
    auto classPointer = findClass(module, decl.defaultType.className, decl.source);

    if(!classPointer) {
        context.diagnostics.error("unknown class %@"_v, decl.source,
                                  context.findName(decl.defaultType.className));
        return;
    }

    auto typeClass = (*module.types)[classPointer];

    // A default answers for the class everywhere it is used, so it has to be declared where the
    // class is - the same coherence rule that keeps one instance per type from being a matter of
    // which module you are looking from.
    if(typeClass->module != &module) {
        context.diagnostics.error("a default must be declared in the module that declares %@"_v, decl.source,
                                  context.findName(decl.defaultType.className));
        return;
    }

    if((*module.types)[typeClass->gen]->types.size() != 1) {
        context.diagnostics.error("only a class with one type argument can have a default"_v, decl.source);
        return;
    }

    if(typeClass->defaultType) {
        context.diagnostics.error("duplicate default for %@"_v, decl.source,
                                  context.findName(decl.defaultType.className));
        return;
    }

    auto type = resolveType(module, decl.defaultType.target, nullptr);
    if((*module.types)[type]->kind == Type::Error) return;

    if(isGeneric(*module.types, type)) {
        context.diagnostics.error("a default must name a concrete type"_v, decl.source);
        return;
    }

    if(!findInstance(module, classPointer, { &type, 1 })) {
        context.diagnostics.error("%@ is the default of %@ but has no instance of it"_v, decl.source,
                                  describeType(context, *module.types, type),
                                  context.findName(decl.defaultType.className));
        return;
    }

    typeClass->defaultType = type;
    typeClass->defaultSource = decl.source;
}

// Every superclass its class declares has to hold for the instance's own types. Without this,
// `class (FromInt(a)) Num(a)` promises generic code something an `instance Num(Metres)` need not
// have delivered, and the promise would only fail much later inside a body nobody wrote that way.
static void checkSuperclasses(Module& module, ClassInstance& instance) {
    auto& context = module.context;
    auto global = *module.types;
    auto typeClass = global[instance.typeClass];

    Array<TypePtr> args;
    for(auto arg: instance.forTypes.contents(*module.arena)) args.push(arg);

    for(auto constraint: global[typeClass->gen]->classes.contents(global)) {
        if(!constraint.typeClass) continue;

        Array<TypePtr> concrete;
        for(auto arg: constraint.args.contents(global)) {
            concrete.push(substituteType(module, arg, toBuffer(args), instance.source));
        }

        if(findInstance(module, constraint.typeClass, toBuffer(concrete))) continue;

        // A parametric head answers for a superclass with what it requires of its own variables as
        // well as with what has an instance: `instance (Ord(a)) Foo(Ptr(a))` has `Eq(a)` in hand
        // because `Ord` declares it, exactly as a generic function's body does.
        if(instance.gen && provesClass(module, *global[instance.gen], constraint.typeClass, toBuffer(concrete))) {
            continue;
        }

        StringBuilder text;
        describeTypes(context, global, toBuffer(concrete), text);

        context.diagnostics.error("%@ requires %@, and there is no instance of it for (%@)"_v, instance.source,
                                  context.findName(typeClass->name),
                                  context.findName(global[constraint.typeClass]->name), text.view());
    }
}

/*
 * Whole-module passes.
 */

void resolveModuleDecls(Module& module, ast::Module& ast, ModuleProvider* provider, bool importsResolved) {
    auto parse = module.parse;
    if(!importsResolved) resolveImports(module, ast, provider);

    for(auto fixity: ast.ops.contents(parse)) {
        *module.operatorPrecedence.add(fixity.op).value = U8(fixity.precedence);
    }

    auto decls = ast.decls;

    for(Size i = 0; i < decls.size(); i++) {
        auto pointer = declAt(decls, i);
        auto& decl = *parse[pointer];

        // The one pass that reports: every declaration passes through it, and it is the first.
        if(!platformEnabled(module, decl, true)) continue;

        switch(decl.kind) {
            case ast::Decl::Data:
                declareRecord(module, decl);
                break;
            case ast::Decl::Alias:
                // A qualified alias is a newtype rather than a name for another type, so it
                // declares a record and never reaches the alias table.
                if(decl.qualified) {
                    declareNewtype(module, decl);
                } else {
                    declareAlias(module, decl, pointer);
                }
                break;
            case ast::Decl::Trait:
                declareClass(module, decl, pointer);
                break;
            case ast::Decl::Fun:
            case ast::Decl::Instance:
            case ast::Decl::Default:
                break;
            case ast::Decl::Stmt:
                // Deferred: a global's type may name a record declared after it.
                break;
            default:
                module.context.diagnostics.error("this declaration is not available yet"_v, decl.source);
                break;
        }
    }

    for(auto decl: decls.contents(parse)) {
        if(!platformEnabled(module, decl)) continue;
        if(decl.kind == ast::Decl::Data) defineRecord(module, decl);
        else if(decl.kind == ast::Decl::Alias && decl.qualified) defineNewtype(module, decl);
    }

    for(auto decl: decls.contents(parse)) {
        if(!platformEnabled(module, decl)) continue;
        if(decl.kind == ast::Decl::Data) declareRecordDefaults(module, decl);
    }

    completePendingInstances(module);

    for(auto decl: decls.contents(parse)) {
        if(!platformEnabled(module, decl)) continue;

        auto newtype = decl.kind == ast::Decl::Alias && decl.qualified;
        if(decl.kind != ast::Decl::Data && !newtype) continue;

        auto record = declaredRecord(module, newtype ? decl.alias.type.name : decl.data.type.name);
        if(record && !record->generic) finishRecordRepr(module, *record, decl.source);
    }

    for(auto decl: decls.contents(parse)) {
        if(!platformEnabled(module, decl)) continue;
        if(decl.kind == ast::Decl::Stmt) declareGlobal(module, decl);
    }

    for(auto& entry: module.classes) {
        resolveClassSignatures(module, *(*module.types)[entry]);
    }

    for(Size i = 0; i < decls.size(); i++) {
        auto pointer = declAt(decls, i);
        auto& decl = *parse[pointer];
        if(decl.kind != ast::Decl::Fun) continue;
        if(!platformEnabled(module, decl)) continue;

        // Every function is resolved in an open context: a type variable in the signature is what
        // makes the function generic, and the constraints written in front of it are the ones the
        // body does not have to prove. The context is dropped again when nothing used it.
        auto env = prepareGenEnv(module, GenEnv::Function, {}, decl.fun.constraints, true);

        auto function = resolveSignature(module, decl, (*module.types)[env], decl.fun.name, false);
        function->ast = pointer;

        auto& context = *(*module.types)[env];

        if(context.types.isNotEmpty()) {
            function->gen = env;
            resolveConstraintClasses(module, context);
        } else if(context.classes.isNotEmpty() || context.properties.isNotEmpty() ||
                  context.functions.isNotEmpty()) {
            // Every kind, not only the classes. A context with no variables is never instantiated,
            // so there is no site at which any of its requirements would be proved - and a
            // requirement nothing proves is decoration, which is what proveRequirements exists to
            // stop this compiler from accepting.
            module.context.diagnostics.error("%@ has constraints but no type variables"_v, decl.source,
                                             module.context.findName(decl.fun.name));
        }
    }

    for(auto decl: decls.contents(parse)) {
        if(!platformEnabled(module, decl)) continue;
        if(decl.kind == ast::Decl::Instance) resolveInstance(module, decl);
    }

    // Core's instances are generated after its source has been read, so it runs this pass itself
    // once they exist rather than here (see defineCore).
    if(&module != module.program.core) checkModuleClasses(module, ast);

    completePendingInstances(module);
}

void checkModuleClasses(Module& module, ast::Module& ast) {
    // Superclasses and defaults are checked once every instance of the module exists, so that
    // neither depends on the order the declarations were written in.
    auto instanceCount = module.instances.size();
    for(Size i = 0; i < instanceCount; i++) {
        checkSuperclasses(module, *(*module.arena)[module.instances[i]]);
    }

    auto decls = ast.decls;
    for(auto decl: decls.contents(module.parse)) {
        if(!platformEnabled(module, decl)) continue;
        if(decl.kind == ast::Decl::Default) resolveDefault(module, decl);
    }
}

void resolveImports(Module& module, ast::Module& ast, ModuleProvider* provider) {
    // Core is visible everywhere without being written, and is the one module that does not
    // import itself.
    if(module.program.core && &module != module.program.core) {
        auto& core = *module.imports.push();
        core.module = module.program.core;
        core.localName = module.program.core->name;
    }

    // Collections is visible everywhere for the same reason: `[a]` and `[1, 2, 3]` are grammar
    // rather than library, so what they mean has to be reachable without being asked for. It is
    // built after Core and Native, so neither of those is handed one of these.
    if(module.program.collections && &module != module.program.collections) {
        auto& collections = *module.imports.push();
        collections.module = module.program.collections;
        collections.localName = module.program.collections->name;
    }

    for(auto imported: ast.imports.contents(module.parse)) {
        if(imported.from == module.name) {
            module.context.diagnostics.error("a module cannot import itself"_v, imported.source);
            continue;
        }

        auto target = module.program.findModule(imported.from);

        if(!target) {
            auto source = provider ? provider->getModule(imported.from) : nullptr;
            if(!source) {
                module.context.diagnostics.error("cannot find module %@"_v, imported.source,
                                                 module.context.findName(imported.from));
                continue;
            }

            target = module.program.addModule(imported.from, *source->region);
            resolveModuleDecls(*target, *source, provider);
        }

        auto duplicate = false;
        for(auto& existing: module.imports) {
            if(existing.module == target && existing.localName == (imported.localName ? imported.localName : imported.from)) {
                module.context.diagnostics.error("duplicate import of %@"_v, imported.source,
                                                 module.context.findName(imported.from));
                duplicate = true;
            }
        }

        if(duplicate) continue;

        auto& entry = *module.imports.push();
        entry.module = target;
        entry.localName = imported.localName ? imported.localName : imported.from;
        entry.qualified = imported.qualified;

        for(auto symbol: imported.include.contents(module.parse)) entry.include.push(symbol);
        for(auto symbol: imported.exclude.contents(module.parse)) entry.exclude.push(symbol);
    }
}

/*
 * What the program actually contains.
 *
 * `used` is set on a function as each call to it is resolved, which answers "was this called" and
 * not "can this run". The two differ as soon as a module's functions call each other: Native's
 * allocateHeap calls heapClassOf whether or not any program allocates, so every one of them would
 * be marked and an unimported runtime would be emitted into every binary.
 *
 * The answer is rebuilt here as a closure over what the root module can reach, once every body
 * exists. Globals come along the same way - a global is part of the program exactly when
 * something that runs reads or writes it.
 */
static void markPlace(ModuleBase local, const Place& place) {
    if(place.root == PlaceRoot::Global && place.global) local[place.global]->used = true;
}

/*
 * `tables` is the compiler-built constants reached so far, and walking them is not optional: a table
 * holds addresses, and an address is a reason for what it names to exist. A TypeDesc naming the glue
 * that tears its type down is the only thing keeping that glue alive, and dropping the relocation
 * instead would leave a slot the emitted code calls through holding zero.
 */
static void markReachable(Program& program, Array<ModulePtr<Function>>& pending,
                          Array<ModulePtr<Global>>& tables) {
    ModuleBase local = *program.arena;

    auto reachFunction = [&](ModulePtr<Function> callee) {
        if(!callee || local[callee]->used) return;

        local[callee]->used = true;
        pending.push(callee);
    };

    auto reachTable = [&](ModulePtr<Global> table) {
        if(!table || local[table]->used) return;

        local[table]->used = true;
        tables.push(table);
    };

    while(pending.isNotEmpty() || tables.isNotEmpty()) {
        while(tables.isNotEmpty()) {
            auto table = local[tables.pop().unwrap()];

            for(auto relocation: table->relocations.contents(local)) {
                reachFunction(relocation.function);
                reachTable(relocation.global);
            }
        }

        if(pending.isEmpty()) continue;
        auto function = local[pending.pop().unwrap()];
        auto& reach = reachFunction;

        for(auto blockPointer: function->blocks.contents(local)) {
            for(auto instructionPointer: local[blockPointer]->instructions.contents(local)) {
                auto& instruction = *local[instructionPointer];

                // A global is part of the program exactly when something that runs names storage
                // rooted in it, and which places an instruction names is one list - see
                // instructionPlaces. What the switch below is about is everything else an
                // instruction can reach: a callee, a table, a teardown.
                eachPlace(instruction, [&](const Place& place) { markPlace(local, place); });

                switch(instruction.kind) {
                    case Value::Call:
                        reach(((InstCall&)instruction).callee);
                        break;
                    case Value::GenCall:
                        reach(((InstGenCall&)instruction).callee);
                        reachTable(((InstGenCall&)instruction).env);

                        for(auto fill: ((InstGenCall&)instruction).fill.contents(local)) {
                            reachTable(fill.constant);
                        }

                        break;
                    case Value::Symbol:
                        // The address of a function or of a table, taken as a value: a function
                        // value's code word, and the environment descriptor it carries.
                        reach(((InstSymbol&)instruction).callee);
                        reachTable(((InstSymbol&)instruction).global);
                        break;
                    case Value::Move:
                        reach(((InstMove&)instruction).sink);
                        break;
                    case Value::Copy:
                        reach(((InstCopy&)instruction).copy);
                        break;
                    case Value::Drop:
                        // Both teardown implementations are reached from here and from nowhere
                        // else: a derived glue function has no call site in the source at all, and
                        // an authored instance may have none either. The same goes for the release
                        // of heap storage, which lowering emits as a call nothing in the IR names.
                        reach(((InstDrop&)instruction).drop);
                        reach(((InstDrop&)instruction).reclaim);
                        if(((InstDrop&)instruction).releaseStorage) reach(program.freeHeap);
                        break;
                    case Value::Alloc:
                        if(((InstAlloc&)instruction).storage == StorageClass::Heap) {
                            reach(program.allocateHeap);
                        }

                        break;
                    default:
                        break;
                }
            }
        }
    }
}

static void markProgramReachable(Program& program) {
    ModuleBase local = *program.arena;
    Array<ModulePtr<Function>> pending;

    Array<ModulePtr<Global>> tables;

    for(auto module: program.modules) {
        for(auto function: module->functionOrder.contents(local)) {
            local[function]->used = module->root;
            if(module->root) pending.push(function);
        }

        // The root module's tables are seeded alongside its functions rather than being taken as
        // already-reached, because what a table *holds* is the point: marking one used without
        // walking it would keep the bytes and drop everything their relocations name.
        for(auto global_: module->globalOrder.contents(local)) {
            local[global_]->used = module->root;
            if(module->root) tables.push(global_);
        }
    }

    markReachable(program, pending, tables);
}

Ptr<Program> resolveProgram(Context& context, ast::Module& root, ModuleProvider* provider,
                            Program::Specialization specialization) {
    auto program = Ptr<Program>(new Program(context));

    // Set before anything is resolved, and never after: which form a call site takes has to be the
    // same answer for every call site in one compilation, or the two would not be comparable.
    // Core, Native and Collections are built under it too - they are where most generic code is.
    program->specialization = specialization;

    defineCore(*program);
    defineNative(*program);
    defineCollections(*program);

    auto module = program->addModule(root.name, *root.region);
    module->root = true;
    program->root = module;

    resolveModuleDecls(*module, root, provider);

    // Bodies come last, and for every module at once: a Core instance may call a function that
    // only the root module's signatures made resolvable.
    for(auto entry: program->modules) resolveModuleBodies(*entry);

    // The generic environments come before ownership, because filling them generates real
    // functions - the erased entry thunk of every class method a witness holds - and those need
    // drops inserted like any other body. They come after every body is resolved for the opposite
    // reason: a slot number is derived from a finished context, and a body collects requirements
    // while it is being resolved.
    prepareGenericCalls(*program);

    // Ownership runs over the finished program rather than per module, because a generic
    // function's specializations only exist once every body that calls one has been resolved -
    // and it is the specializations, not the generic body, that get drops.
    runProgramOwnership(*program);

    markProgramReachable(*program);
    return program;
}
