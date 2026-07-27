#include "module.h"
#include "core.h"
#include "expr.h"
#include "generic.h"
#include "name.h"
#include "native.h"
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

U32 Function::addLocal(Module& module, TypePtr type, StringId localName, ModulePtr<Value> value) {
    auto index = U32(locals.size());
    locals.push(module.arena, Local { type, localName, value });
    return index;
}

/*
 * Generic contexts.
 */

// Builds the generic context of one declaration: its declared type variables, then the class
// constraints written over them. Constraint *classes* are resolved in a later pass, since a
// class may be declared after the type that constrains itself by it.
static GlobalPtr<GenEnv> prepareGenEnv(Module& module, GenEnv::Kind kind,
                                       ast::ParseList<StringId> variables,
                                       ast::ConstraintList constraints) {
    auto env = new (module.types) GenEnv(kind);
    auto pointer = env - *module.types;

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
            case ast::Constraint::Field:
            case ast::Constraint::Function:
                // Both need a witness passed at runtime rather than a type substituted at
                // compile time, which is the erased half of the generic model.
                module.context.diagnostics.error(
                    "field and function constraints require property witnesses, which are not available yet"_v,
                    constraint.source);
                break;
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
        if(((FloatType*)global[type])->width == FloatType::Float) {
            auto single = F32(number);
            copy((const Byte*)&single, (Byte*)&bits, sizeof(single));
        } else {
            copy((const Byte*)&number, (Byte*)&bits, sizeof(number));
        }

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

                if(isFloat(*module.types, type) &&
                   ((FloatType*)(*module.types)[type])->width == FloatType::Float) {
                    auto single = F32(number);
                    copy((const Byte*)&single, (Byte*)&initial, sizeof(single));
                } else {
                    copy((const Byte*)&number, (Byte*)&initial, sizeof(number));
                }

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

    for(auto arg: decl.fun.args.contents(module.parse)) {
        if(!arg.type) {
            module.context.diagnostics.error("function arguments require an explicit type"_v, arg.source);
            function->addArg(module, arg.name, module.scalar.error, arg.source);
            continue;
        }

        // Both halves of an argument's ownership contract parse today and neither is modelled
        // yet: the convention belongs on FunArg and the return-root marker in the function type,
        // which is the ownership milestone's work (Implementation-IR.md part 3).
        if(arg.bind != ast::BindType::Borrow) {
            module.context.diagnostics.error("binding conventions are deferred until the ownership resolver"_v, arg.source);
        }

        if(arg.returnRoot) {
            module.context.diagnostics.error("return-root markers are deferred until the ownership resolver"_v, arg.source);
        }

        function->addArg(module, arg.name, resolveType(module, *module.parse[arg.type], env), arg.source);
    }

    return function;
}

static void resolveClassSignatures(Module& module, TypeClass& typeClass) {
    if(typeClass.ready) return;
    typeClass.ready = true;

    auto env = (*module.types)[typeClass.gen];
    resolveConstraintClasses(module, *env);

    auto& decl = *module.parse[typeClass.ast];
    auto decls = decl.trait.decls;
    Size index = 0;

    for(auto member: decls.contents(module.parse)) {
        if(member.kind != ast::Decl::Fun) continue;
        if(index >= typeClass.functions.size()) break;

        if(member.fun.body) {
            module.context.diagnostics.error(
                "class functions cannot have a default body yet - every instance must supply one"_v, member.source);
        }

        if(!member.fun.ret) {
            module.context.diagnostics.error("a class function requires an explicit return type"_v, member.source);
        }

        auto signature = resolveSignature(module, member, env, member.fun.name, true);
        signature->instanceOf = (TypeClass*)&typeClass - *module.types;
        signature->signature = true;

        auto stored = typeClass.functions.get(*module.types, index);
        stored.fun = signature - *module.arena;
        typeClass.functions.set(*module.types, index, stored);
        index++;
    }
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
    auto genPointer = prepareGenEnv(module, GenEnv::Instance, {}, decl.instance.constraints);
    auto gen = (*module.types)[genPointer];
    gen->open = true;

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

            function->addArg(module, declared.name ? declared.name : classArg->name, expectedType, declared.source);
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

        module.context.diagnostics.error("instance of %@ does not implement %@"_v, decl.source,
                                         module.context.findName(className),
                                         module.context.findName(typeClass->functions.get(*module.types, i).name));
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
        if(decl.kind == ast::Decl::Data) defineRecord(module, decl);
        else if(decl.kind == ast::Decl::Alias && decl.qualified) defineNewtype(module, decl);
    }

    for(auto decl: decls.contents(parse)) {
        if(decl.kind == ast::Decl::Data) declareRecordDefaults(module, decl);
    }

    completePendingInstances(module);

    for(auto decl: decls.contents(parse)) {
        auto newtype = decl.kind == ast::Decl::Alias && decl.qualified;
        if(decl.kind != ast::Decl::Data && !newtype) continue;

        auto record = declaredRecord(module, newtype ? decl.alias.type.name : decl.data.type.name);
        if(record && !record->generic) finishRecordRepr(module, *record, decl.source);
    }

    for(auto decl: decls.contents(parse)) {
        if(decl.kind == ast::Decl::Stmt) declareGlobal(module, decl);
    }

    for(auto& entry: module.classes) {
        resolveClassSignatures(module, *(*module.types)[entry]);
    }

    for(Size i = 0; i < decls.size(); i++) {
        auto pointer = declAt(decls, i);
        auto& decl = *parse[pointer];
        if(decl.kind != ast::Decl::Fun) continue;

        // Every function is resolved in an open context: a type variable in the signature is what
        // makes the function generic, and the constraints written in front of it are the ones the
        // body does not have to prove. The context is dropped again when nothing used it.
        auto env = prepareGenEnv(module, GenEnv::Function, {}, decl.fun.constraints);
        (*module.types)[env]->open = true;

        auto function = resolveSignature(module, decl, (*module.types)[env], decl.fun.name, false);
        function->ast = pointer;

        if((*module.types)[env]->types.isNotEmpty()) {
            function->gen = env;
            resolveConstraintClasses(module, *(*module.types)[env]);
        } else if((*module.types)[env]->classes.isNotEmpty()) {
            module.context.diagnostics.error("%@ has constraints but no type variables"_v, decl.source,
                                             module.context.findName(decl.fun.name));
        }
    }

    for(auto decl: decls.contents(parse)) {
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

static void markReachable(Program& program, Array<ModulePtr<Function>>& pending) {
    ModuleBase local = *program.arena;

    while(pending.isNotEmpty()) {
        auto function = local[pending.pop().unwrap()];

        auto reach = [&](ModulePtr<Function> callee) {
            if(!callee || local[callee]->used) return;

            local[callee]->used = true;
            pending.push(callee);
        };

        for(auto blockPointer: function->blocks.contents(local)) {
            for(auto instructionPointer: local[blockPointer]->instructions.contents(local)) {
                auto& instruction = *local[instructionPointer];

                switch(instruction.kind) {
                    case Value::Call:
                        reach(((InstCall&)instruction).callee);
                        break;
                    case Value::GenCall:
                        reach(((InstGenCall&)instruction).callee);
                        break;
                    case Value::LoadPlace:
                        markPlace(local, ((InstLoadPlace&)instruction).place);
                        break;
                    case Value::Init:
                        markPlace(local, ((InstInit&)instruction).place);
                        break;
                    case Value::Address:
                        markPlace(local, ((InstAddress&)instruction).place);
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

    for(auto module: program.modules) {
        for(auto function: module->functionOrder.contents(local)) {
            local[function]->used = module->root;
            if(module->root) pending.push(function);
        }

        for(auto global_: module->globalOrder.contents(local)) local[global_]->used = module->root;
    }

    markReachable(program, pending);
}

Ptr<Program> resolveProgram(Context& context, ast::Module& root, ModuleProvider* provider) {
    auto program = Ptr<Program>(new Program(context));
    defineCore(*program);
    defineNative(*program);

    auto module = program->addModule(root.name, *root.region);
    module->root = true;
    program->root = module;

    resolveModuleDecls(*module, root, provider);

    // Bodies come last, and for every module at once: a Core instance may call a function that
    // only the root module's signatures made resolvable.
    for(auto entry: program->modules) resolveModuleBodies(*entry);

    markProgramReachable(*program);
    return program;
}
