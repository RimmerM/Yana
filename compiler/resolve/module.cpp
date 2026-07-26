#include "module.h"
#include "core.h"
#include "expr.h"
#include "name.h"
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
    delete coreAst;
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
    scalar(program.scalar), name(name), parse(parse) {}

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
                module.context.diagnostics.error(
                    "field and function constraints arrive with generic functions"_v, constraint.source);
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

static RecordType* declareRecord(Module& module, ast::Decl& decl) {
    auto found = module.namedTypes.add(decl.data.type.name);
    if(found.existed) {
        module.context.diagnostics.error("duplicate type %@"_v, decl.source,
                                         module.context.findName(decl.data.type.name));
        auto existing = *found.value;

        return existing && (*module.types)[existing]->kind == Type::Record
            ? (RecordType*)(*module.types)[existing]
            : nullptr;
    }

    auto record = new (module.types) RecordType(decl.data.type.name);
    record->qualified = decl.qualified;
    *found.value = (Type*)record - *module.types;

    auto variables = decl.data.type.kind;
    if(variables.isNotEmpty() || decl.data.constraints.isNotEmpty()) {
        record->gen = prepareGenEnv(module, GenEnv::Record, variables, decl.data.constraints);
        record->generic = (*module.types)[record->gen]->types.isNotEmpty();
    }

    U32 index = 0;
    for(auto con: decl.data.cons.contents(module.parse)) {
        record->constructors.push(module.types, Constructor { con.name, nullptr, index });

        // A qualified record's constructors are addressed only as `Record.Constructor`, so they
        // are not added to the module's flat constructor table.
        if(!decl.qualified) {
            auto inserted = module.constructors.add(con.name);
            if(inserted.existed) {
                module.context.diagnostics.error("duplicate constructor %@"_v, con.source,
                                                 module.context.findName(con.name));
            } else {
                *inserted.value = ConstructorRef { record - *module.types, U16(index) };
            }
        }

        index++;
    }

    return record;
}

static void defineRecord(Module& module, ast::Decl& decl) {
    auto found = module.namedTypes.get(decl.data.type.name);
    if(!found || (*module.types)[found.unwrap()]->kind != Type::Record) return;

    auto record = (RecordType*)(*module.types)[found.unwrap()];
    auto env = record->gen ? (*module.types)[record->gen] : nullptr;
    record->resolvingRepr = true;
    Size index = 0;

    for(auto con: decl.data.cons.contents(module.parse)) {
        auto content = con.content ? resolveType(module, *module.parse[con.content], env) : module.scalar.unit;

        auto stored = record->constructors.get(*module.types, index);
        stored.content = content;
        record->constructors.set(*module.types, index, stored);
        index++;
    }

    record->resolvingRepr = false;
    record->definitionReady = true;
    if(!record->generic) finishRecordRepr(module, *record, decl.source);
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

// The printed name of one instance implementation: `Num(Int).+`. Instances are not addressable
// by name in source, but every function reaching the backend needs a unique one.
static StringId instanceFunctionName(Module& module, TypeClass& typeClass, Buffer<TypePtr> args, StringId method) {
    Array<char> text;
    auto className = module.context.findName(typeClass.name);
    appendText(text, StringView { className.text(), className.size() });
    text.push('(');

    for(Size i = 0; i < args.length; i++) {
        if(i) appendText(text, ", "_v);
        describeType(module.context, *module.types, args[i], text);
    }

    appendText(text, ")."_v);
    auto methodName = module.context.findName(method);
    appendText(text, StringView { methodName.text(), methodName.size() });

    return module.context.addQualifiedName(text.pointer(), text.size(), 1);
}

static void resolveInstance(Module& module, ast::Decl& decl) {
    auto& type = decl.instance.type;
    StringId className = 0;
    Array<TypePtr> args;

    if(type.kind == ast::Type::App) {
        auto& app = *module.parse[type.app];
        if(app.base.kind != ast::Type::Con) {
            module.context.diagnostics.error("an instance must name a class"_v, decl.source);
            return;
        }

        className = app.base.name;
        auto appArgs = app.args;
        for(auto arg: appArgs.contents(module.parse)) args.push(resolveType(module, arg, nullptr));
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

    for(auto arg: args) {
        if(isGeneric(*module.types, arg)) {
            module.context.diagnostics.error(
                "instances for generic types arrive with generic functions"_v, decl.source);
            return;
        }
    }

    auto instance = new (module.arena) ClassInstance(classPointer);
    instance->module = &module;
    instance->source = decl.source;
    for(auto arg: args) instance->forTypes.push(module.arena, arg);
    for(Size i = 0; i < typeClass->functions.size(); i++) instance->functions.push(module.arena, nullptr);

    // Two instances for the same arguments would make selection depend on declaration order.
    Array<ModulePtr<ClassInstance>> existing;
    findInstances(module, classPointer, existing);

    for(auto other: existing) {
        auto equal = true;
        for(Size i = 0; i < args.size(); i++) {
            if((*module.arena)[other]->forTypes.get(*module.arena, i) != args[i]) {
                equal = false;
                break;
            }
        }

        if(equal) {
            module.context.diagnostics.error("duplicate instance of %@ for these types"_v, decl.source,
                                             module.context.findName(className));
            return;
        }
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
                auto written = resolveType(module, *module.parse[declared.type], nullptr);
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
            auto written = resolveType(module, *module.parse[member.fun.ret], nullptr);
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
 * Whole-module passes.
 */

static void resolveImports(Module& module, ast::Module& ast, ModuleProvider* provider);

void resolveModuleDecls(Module& module, ast::Module& ast, ModuleProvider* provider) {
    auto parse = module.parse;
    resolveImports(module, ast, provider);

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
                declareAlias(module, decl, pointer);
                break;
            case ast::Decl::Trait:
                declareClass(module, decl, pointer);
                break;
            case ast::Decl::Fun:
            case ast::Decl::Instance:
                break;
            default:
                module.context.diagnostics.error("this declaration is not available yet"_v, decl.source);
                break;
        }
    }

    for(auto decl: decls.contents(parse)) {
        if(decl.kind == ast::Decl::Data) defineRecord(module, decl);
    }

    completePendingInstances(module);

    for(auto decl: decls.contents(parse)) {
        if(decl.kind != ast::Decl::Data) continue;

        auto found = module.namedTypes.get(decl.data.type.name);
        if(found && (*module.types)[found.unwrap()]->kind == Type::Record) {
            auto record = (RecordType*)(*module.types)[found.unwrap()];
            if(!record->generic) finishRecordRepr(module, *record, decl.source);
        }
    }

    for(auto& entry: module.classes) {
        resolveClassSignatures(module, *(*module.types)[entry]);
    }

    for(Size i = 0; i < decls.size(); i++) {
        auto pointer = declAt(decls, i);
        auto& decl = *parse[pointer];
        if(decl.kind != ast::Decl::Fun) continue;

        if(decl.fun.constraints.isNotEmpty()) {
            module.context.diagnostics.error("generic functions arrive in the next milestone"_v, decl.source);
        }

        auto function = resolveSignature(module, decl, nullptr, decl.fun.name, false);
        function->ast = pointer;
    }

    for(auto decl: decls.contents(parse)) {
        if(decl.kind == ast::Decl::Instance) resolveInstance(module, decl);
    }

    completePendingInstances(module);
}

static void resolveImports(Module& module, ast::Module& ast, ModuleProvider* provider) {
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

Ptr<Program> resolveProgram(Context& context, ast::Module& root, ModuleProvider* provider) {
    auto program = Ptr<Program>(new Program(context));
    defineCore(*program);

    auto module = program->addModule(root.name, *root.region);
    module->root = true;
    program->root = module;

    resolveModuleDecls(*module, root, provider);

    // Bodies come last, and for every module at once: a Core instance may call a function that
    // only the root module's signatures made resolvable.
    for(auto entry: program->modules) resolveModuleBodies(*entry);

    return program;
}
