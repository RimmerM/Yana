#include "module.h"
#include "builtins.h"
#include "expr.h"
#include "../parse/ast.h"

// The Nth declaration as a pointer rather than a value. A function keeps its own AST node so
// that its body can be resolved once every signature in the module is known.
static ast::ParsePtr<ast::Decl> declAt(ast::DeclList& decls, Size index) {
    return ast::ParsePtr<ast::Decl>(decls.list.p.offset + U32(sizeof(ast::Decl) * index));
}

template<class T, class... Args>
static TypePtr addScalar(Module& module, StringView name, Args&&... args) {
    auto value = new (module.types) T(forward<Args>(args)...);
    auto pointer = (Type*)value - *module.types;

    if(name.length) {
        module.namedTypes.add(module.context.addUnqualifiedName(name.ptr, name.length), pointer);
    }

    return pointer;
}

Module::Module(Context& context, StringId name, ast::ParseBase parse, Size typeMemory, Size irMemory):
    context(context), name(name), types(typeMemory), arena(irMemory), parse(parse)
{
    scalar.error = addScalar<Type>(*this, ""_v, Type::Error, 0);
    scalar.unit = addScalar<Type>(*this, "Unit"_v, Type::Unit, 0);
    scalar.int_ = addScalar<IntType>(*this, "Int"_v, 32, IntType::Int, true);
    scalar.long_ = addScalar<IntType>(*this, "Long"_v, 64, IntType::Long, true);
    scalar.float_ = addScalar<FloatType>(*this, "Float"_v, FloatType::Float);
    scalar.double_ = addScalar<FloatType>(*this, "Double"_v, FloatType::Double);

    // Bool is the ordinary `False | True` two-constructor record it will be in the standard
    // library, rather than a primitive: that is what lets `match` treat it like any other ADT.
    auto boolName = context.addUnqualifiedName("Bool", 4);
    auto trueName = context.addUnqualifiedName("True", 4);
    auto falseName = context.addUnqualifiedName("False", 5);
    auto boolType = new (types) RecordType(boolName);
    auto boolPointer = boolType - *types;

    boolType->constructors.push(types, Constructor { falseName, scalar.unit, 0 });
    boolType->constructors.push(types, Constructor { trueName, scalar.unit, 1 });
    boolType->definitionReady = true;
    finishRecordRepr(context, *this, *boolType, kNullLocation);

    scalar.bool_ = (Type*)boolType - *types;
    namedTypes.add(boolName, scalar.bool_);
    constructors.add(falseName, ConstructorRef { boolPointer, 0 });
    constructors.add(trueName, ConstructorRef { boolPointer, 1 });

    defineBuiltins(*this);
}

Function* Module::addFunction(StringId functionName, LocationId source) {
    auto found = functions.add(functionName);
    if(found.existed) {
        context.diagnostics.error("duplicate function %@"_v, source, context.findName(functionName));
        return (*arena)[*found.value];
    }

    auto function = new (arena) Function(functionName);
    function->source = source;
    *found.value = function - *arena;
    functionOrder.push(arena, function - *arena);

    function->addBlock(*this);
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

static RecordType* declareRecord(Context& context, Module& module, ast::Decl& decl) {
    if(decl.data.type.kind.isNotEmpty()) {
        context.diagnostics.error("generic data declarations are deferred until the generic resolver milestone"_v, decl.source);
    }

    auto found = module.namedTypes.add(decl.data.type.name);
    if(found.existed) {
        context.diagnostics.error("duplicate type %@"_v, decl.source, context.findName(decl.data.type.name));
        auto existing = *found.value;

        return existing && (*module.types)[existing]->kind == Type::Record
            ? (RecordType*)(*module.types)[existing]
            : nullptr;
    }

    auto record = new (module.types) RecordType(decl.data.type.name);
    record->qualified = decl.qualified;
    *found.value = (Type*)record - *module.types;

    U32 index = 0;
    for(auto con: decl.data.cons.contents(module.parse)) {
        record->constructors.push(module.types, Constructor { con.name, nullptr, index });

        auto inserted = module.constructors.add(con.name);
        if(inserted.existed) {
            context.diagnostics.error("duplicate constructor %@"_v, con.source, context.findName(con.name));
        } else {
            *inserted.value = ConstructorRef { record - *module.types, U16(index) };
        }

        index++;
    }

    return record;
}

// The constructors' content types are resolved in a second pass, so that a record may refer to
// one declared after it.
static void defineRecord(Context& context, Module& module, ast::Decl& decl) {
    auto found = module.namedTypes.get(decl.data.type.name);
    if(!found || (*module.types)[found.unwrap()]->kind != Type::Record) return;

    auto record = (RecordType*)(*module.types)[found.unwrap()];
    record->resolvingRepr = true;
    Size index = 0;

    for(auto con: decl.data.cons.contents(module.parse)) {
        auto content = con.content ? resolveType(context, module, *module.parse[con.content]) : module.scalar.unit;

        auto stored = record->constructors.get(*module.types, index);
        stored.content = content;
        record->constructors.set(*module.types, index, stored);
        index++;
    }

    record->resolvingRepr = false;
    record->definitionReady = true;
    finishRecordRepr(context, module, *record, decl.source);
}

static bool declareFunction(Context& context, Module& module, ast::ParsePtr<ast::Decl> declPointer) {
    auto& decl = *module.parse[declPointer];

    if(decl.fun.kind != ast::FunKind::Plain) {
        context.diagnostics.error("lens and iter functions are not available in the aggregate resolver"_v, decl.source);
        return false;
    }

    auto function = module.addFunction(decl.fun.name, decl.source);
    function->ast = declPointer;

    if(!decl.fun.ret) {
        context.diagnostics.error("scalar functions require an explicit return type"_v, decl.source);
        function->returnType = module.scalar.error;
    } else {
        function->returnType = resolveType(context, module, *module.parse[decl.fun.ret]);
    }

    for(auto arg: decl.fun.args.contents(module.parse)) {
        if(!arg.type) {
            context.diagnostics.error("scalar function arguments require an explicit type"_v, arg.source);
            function->addArg(module, arg.name, module.scalar.error, arg.source);
            continue;
        }

        // Both halves of an argument's ownership contract parse today and neither is modelled
        // yet: the convention belongs on FunArg and the return-root marker in the function type,
        // which is Milestone 3's work (Implementation-IR.md part 3).
        if(arg.bind != ast::BindType::Borrow) {
            context.diagnostics.error("binding conventions are deferred until the ownership resolver"_v, arg.source);
        }

        if(arg.returnRoot) {
            context.diagnostics.error("return-root markers are deferred until the ownership resolver"_v, arg.source);
        }

        function->addArg(module, arg.name, resolveType(context, module, *module.parse[arg.type]), arg.source);
    }

    return true;
}

// Declarations are read in four passes because each one needs the previous to have finished for
// the whole module: a record's constructors may name a type declared later, a record's layout
// needs every content type, and a function signature needs every layout.
Ptr<Module> resolveModule(Context& context, ast::Module& ast) {
    auto parse = *ast.region;
    auto module = Ptr<Module>(new Module(context, ast.name, parse));

    for(auto fixity: ast.ops.contents(parse)) {
        *module->operatorPrecedence.add(fixity.op).value = U8(fixity.precedence);
    }

    auto decls = ast.decls;
    for(auto decl: decls.contents(parse)) {
        switch(decl.kind) {
            case ast::Decl::Data:
                declareRecord(context, *module, decl);
                break;
            case ast::Decl::Fun:
                break;
            default:
                context.diagnostics.error("only function and data declarations are available in the aggregate resolver"_v, decl.source);
                break;
        }
    }

    for(auto decl: decls.contents(parse)) {
        if(decl.kind == ast::Decl::Data) defineRecord(context, *module, decl);
    }

    for(auto decl: decls.contents(parse)) {
        if(decl.kind != ast::Decl::Data) continue;

        auto found = module->namedTypes.get(decl.data.type.name);
        if(found && (*module->types)[found.unwrap()]->kind == Type::Record) {
            finishRecordRepr(context, *module, *(RecordType*)(*module->types)[found.unwrap()], decl.source);
        }
    }

    for(Size i = 0; i < decls.size(); i++) {
        auto declPointer = declAt(decls, i);
        if(parse[declPointer]->kind == ast::Decl::Fun) declareFunction(context, *module, declPointer);
    }

    resolveFunctions(context, *module, ast);
    return module;
}
