#include <alloca.h>
#include "module.h"
#include "../parse/ast.h"

AliasType* defineAlias(Context* context, Module* in, Id name, Type* to) {
    if(in->types.get(name)) {
        // TODO: Error
    }

    auto alias = new (in->memory) AliasType;
    alias->instanceOf = nullptr;
    alias->ast = nullptr;
    alias->name = name;
    alias->to = to;
    in->types[name] = alias;
    return alias;
}

RecordType* defineRecord(Context* context, Module* in, Id name, U32 conCount, bool qualified) {
    if(in->types.get(name)) {
        // TODO: Error
    }

    auto r = new (in->memory) RecordType;
    r->instanceOf = nullptr;
    r->instance = nullptr;
    r->ast = nullptr;
    r->name = name;
    r->qualified = qualified;
    r->genCount = 0;
    r->conCount = conCount;
    if(conCount > 0) {
        r->cons = (Con*)in->memory.alloc(sizeof(Con) * conCount);
    }

    in->types[name] = r;
    return r;
}

Con* defineCon(Context* context, Module* in, RecordType* to, Id name, U32 index) {
    if(in->cons.get(name)) {
        // TODO: Error
    }

    auto con = to->cons + index;
    con->name = name;
    con->content = nullptr;
    con->index = index;
    con->parent = to;
    con->codegen = nullptr;

    if(!to->qualified) {
        in->cons.add(name, con);
    }
    return con;
}

TypeClass* defineClass(Context* context, Module* in, Id name) {
    if(in->typeClasses.get(name)) {
        // TODO: Error
    }

    auto c = &in->typeClasses[name];
    c->ast = nullptr;
    c->name = name;
    return c;
}

ClassInstance* defineInstance(Context* context, Module* in, TypeClass* to, Type** args) {
    auto a = in->classInstances.add(to->name);
    auto map = a.value;

    if(!a.isExisting) {
        new (map) InstanceMap;
        map->forClass = to;
        map->genCount = to->argCount;
    }

    U32 insertPosition = 0;
    for(U32 i = 0; i < map->instances.size(); i++) {
        Type** types = &*map->instances[i]->forTypes;

        int cmp;
        for(U32 j = 0; j < map->genCount; j++) {
            auto lhs = types[j];
            auto rhs = args[j];

            if(lhs->descriptorLength < rhs->descriptorLength) {
                cmp = -1;
            } else if(lhs->descriptorLength > rhs->descriptorLength) {
                cmp = 1;
            } else {
                cmp = memcmp(lhs->descriptor, rhs->descriptor, lhs->descriptorLength);
            }

            if(cmp != 0) {
                break;
            }
        }

        if(cmp > 0) {
            insertPosition = i + 1;
            break;
        } else if(cmp == 0) {
            context->diagnostics.error("an instance for this class has already been defined", nullptr, nullptr);
        }
    }

    auto instance = new (in->memory) ClassInstance;
    instance->forTypes = args;
    instance->typeClass = to;
    instance->module = in;
    instance->instances = (Function**)in->memory.alloc(sizeof(Function*) * to->funCount);
    memset(instance->instances, 0, sizeof(Function*) * to->funCount);

    if(insertPosition >= map->instances.size()) {
        map->instances.push(instance);
    } else {
        map->instances.insert(insertPosition, instance);
    }

    return instance;
}

Function* defineFun(Context* context, Module* in, Id name) {
    if(in->functions.get(name) || in->foreignFunctions.get(name)) {
        // TODO: Error
    }

    auto f = &in->functions[name];
    f->module = in;
    f->name = name;
    return f;
}

Function* defineAnonymousFun(Context* context, Module* in) {
    auto f = new (in->memory) Function;
    f->module = in;
    f->name = 0;
    return f;
}

ForeignFunction* defineForeignFun(Context* context, Module* in, Id name, FunType* type) {
    if(in->functions.get(name) || in->foreignFunctions.get(name)) {
        // TODO: Error
    }

    auto f = &in->foreignFunctions[name];
    f->module = in;
    f->name = name;
    f->externalName = name;
    f->from = 0;
    f->type = type;
    return f;
}

Global* defineGlobal(Context* context, Module* in, Id name) {
    if(in->globals.get(name)) {
        // TODO: Error
    }

    auto g = &in->globals[name];
    g->kind = Value::Global;
    g->name = name;
    g->block = nullptr;
    g->type = nullptr;
    return g;
}

Arg* defineArg(Context* context, Function* fun, Id name, Type* type) {
    auto index = (U32)fun->args.size();
    auto a = new (fun->module->memory) Arg;
    a->kind = Value::Arg;
    a->type = type;
    a->name = name;
    a->block = nullptr;
    a->index = index;

    fun->args.push(a);
    return a;
}

ClassFun* defineClassFun(Context* context, Module* module, TypeClass* typeClass, Id name, U32 index) {
    typeClass->funNames[index] = name;
    module->classFunctions.add(name, ClassFun{typeClass, index, name});
}

U32 testImport(Identifier* importName, Identifier* searchName) {
    if(importName->segmentCount > searchName->segmentCount - 1) return 0;

    U32 i = 0;
    for(; i < importName->segmentCount; i++) {
        if(importName->segmentHashes[i] != searchName->segmentHashes[i]) return 0;
    }

    return i;
}

template<class T, class F>
T* findHelper(Context* context, Module* module, F find, Identifier* name) {
    auto v = find(module, name, 0);
    if(v) return v;

    // Imports have equal weight, so multiple hits here is an error.
    T* candidate = nullptr;

    for(Import& import: module->imports) {
        // Handle qualified names.
        // TODO: Handle module import includes and excludes.
        if(name->segmentCount >= 2) {
            auto start = testImport(&context->find(import.localName), name);
            v = find(import.module, name, start);
        }

        // Handle unqualified names, if the module can be used without one.
        if(!import.qualified) {
            auto uv = find(import.module, name, 0);
            if(!v) {
                v = uv;
            } else {
                // TODO: Error
            }
        }

        if(v) {
            if(candidate) {
                // TODO: Error
            }
            candidate = v;
        }
    }

    return candidate;
}

Type* findType(Context* context, Module* module, Id name) {
    auto identifier = &context->find(name);
    return findHelper<Type>(context, module, [=](Module* m, Identifier* id, U32 start) -> Type* {
        if(id->segmentCount - 1 > start) return nullptr;

        auto type = m->types.get(id->getHash(start));
        return type ? *type : nullptr;
    }, identifier);
}

Con* findCon(Context* context, Module* module, Id name) {
    auto identifier = &context->find(name);
    return findHelper<Con>(context, module, [=](Module* m, Identifier* id, U32 start) -> Con* {
        if(id->segmentCount - 1 == start) {
            auto con = m->cons.get(id->getHash(start));
            return con ? *con : nullptr;
        } else if(id->segmentCount >= 2 && id->segmentCount - 2 == start) {
            // For qualified identifiers, look up the corresponding type in case its constructors are qualified.
            Type** type = m->types.get(id->getHash(start));
            if(!type || (*type)->kind != Type::Record) return nullptr;

            auto record = (RecordType*)*type;
            auto conName = id->getHash(start + 1);
            for(U32 i = 0; i < record->conCount; i++) {
                if(record->cons[i].name == conName) {
                    return record->cons + i;
                }
            }

            return nullptr;
        } else {
            return nullptr;
        }
    }, identifier);
}

FoundFunction findFun(Context* context, Module* module, Id name) {
    FoundFunction found;

    auto identifier = &context->find(name);
    auto find = [&](Module* m, Identifier* id, U32 start) -> bool {
        if(id->segmentCount - 1 == start) {
            auto hash = id->getHash(start);

            // Handle free functions.
            // findHelper will automatically make sure that there are no conflicts in imports.
            auto f = m->functions.get(hash);
            if(f) {
                found.kind = FoundFunction::Static;
                found.function = f;
                return true;
            }

            auto fo = m->foreignFunctions.get(hash);
            if(fo) {
                found.kind = FoundFunction::Foreign;
                found.foreignFunction = fo;
                return true;
            }

            auto c = m->classFunctions.get(hash);
            if(c) {
                found.kind = FoundFunction::Class;
                found.classFun = *c;
                return true;
            }
        } else if(id->segmentCount >= 2 && id->segmentCount - 2 == start) {
            // TODO: Handle type instances.
            // Type instances have priority over classes (in case the resolver allows name conflicts between them).
            TypeClass* c = m->typeClasses.get(id->getHash(start - 1));
            if(c) {
                auto src = id->getHash(start);
                for(U32 i = 0; i < c->funCount; i++) {
                    if(c->funNames[i] == src) {
                        found.kind = FoundFunction::Class;
                        found.classFun = ClassFun{c, i, src};
                        return true;
                    }
                }
            }

            return false;
        }

        return false;
    };

    auto v = find(module, identifier, 0);
    if(v) {
        found.found = true;
        return found;
    }

    // Imports have equal weight, so multiple hits here is an error.
    for(Import& import: module->imports) {
        // Handle qualified names.
        // TODO: Handle module import includes and excludes.
        auto f = false;
        if(identifier->segmentCount >= 2) {
            auto start = testImport(&context->find(import.localName), identifier);
            f = find(import.module, identifier, start);
        }

        // Handle unqualified names, if the module can be used without one.
        if(!import.qualified) {
            auto uv = find(import.module, identifier, 0);
            if(!f) {
                f = uv;
            } else {
                // TODO: Error
            }
        }

        if(f) {
            if(found.found) {
                // TODO: Error with current found contents.
            }
            found.found = true;
        }
    }

    return found;
}

OpProperties* findOp(Context* context, Module* module, Id name) {
    auto identifier = &context->find(name);
    return findHelper<OpProperties>(context, module, [=](Module* m, Identifier* id, U32 start) -> OpProperties* {
        if(id->segmentCount - 1 > start) return nullptr;
        return m->ops.get(id->getHash(start));
    }, identifier);
}

Global* findGlobal(Context* context, Module* module, Id name) {
    auto identifier = &context->find(name);
    return findHelper<Global>(context, module, [=](Module* m, Identifier* id, U32 start) -> Global* {
        if(id->segmentCount - 1 > start) return nullptr;
        return m->globals.get(id->getHash(start));
    }, identifier);
}

TypeClass* findClass(Context* context, Module* module, Id name) {
    auto identifier = &context->find(name);
    return findHelper<TypeClass>(context, module, [=](Module* m, Identifier* id, U32 start) -> TypeClass* {
        if(id->segmentCount - 1 > start) return nullptr;
        return m->typeClasses.get(id->getHash(start));
    }, identifier);
}

ClassInstance* findInstance(Context* context, Module* module, TypeClass* typeClass, U32 index, Type** args) {
    auto find = [=](Module* m) -> ClassInstance* {
        InstanceMap* instances = m->classInstances.get(typeClass->name);
        if(!instances) return nullptr;

        // TODO: Handle generic and higher-kinded types.
        // TODO: Binary search.
        for(U32 i = 0; i < instances->instances.size(); i++) {
            auto it = instances->instances[i];
            auto forTypes = it->forTypes;

            bool equal = true;
            for(U32 j = 0; j < instances->genCount; j++) {
                auto lhs = forTypes[j];
                auto rhs = args[j];
                if(lhs->descriptorLength != rhs->descriptorLength) {
                    equal = false;
                    break;
                }

                if(memcmp(lhs->descriptor, rhs->descriptor, lhs->descriptorLength) != 0) {
                    equal = false;
                    break;
                }
            }

            if(equal) {
                return it;
            }
        }

        return nullptr;
    };

    auto v = find(module);
    if(v) return v;

    // Imports have equal weight, so multiple hits here is an error.
    ClassInstance* candidate = nullptr;

    for(Import& import: module->imports) {
        if(!import.qualified) {
            auto uv = find(import.module);
            if(!v) {
                v = uv;
            } else {
                // TODO: Error
            }
        }

        if(v) {
            if(candidate) {
                // TODO: Error
            }
            candidate = v;
        }
    }

    return candidate;
}

static U32 prepareGens(Context* context, Module* module, ast::SimpleType* type, GenType*& gens) {
    auto kind = type->kind;
    U32 count = 0;
    while(kind) {
        kind = kind->next;
        count++;
    }

    kind = type->kind;
    gens = (GenType*)module->memory.alloc(sizeof(GenType) * count);
    for(U32 i = 0; i < count; i++) {
        new (gens + i) GenType(kind->item, i);
        kind = kind->next;
    }

    return count;
}

// Tries to load any imported modules.
// Returns true if all modules were available; otherwise, compilation should pause until they are.
static bool prepareImports(Context* context, Module* module, ModuleHandler* handler, ast::Import* imports, Size count) {
    U32 missingImports = 0;
    for(Size i = 0; i < count; i++) {
        auto& import = imports[i];
        auto importModule = handler->require(context, module, import.from);
        if(!importModule) {
            missingImports++;
            continue;
        }

        auto it = &module->imports[import.localName];
        it->qualified = import.qualified;
        it->localName = import.localName;
        it->module = importModule;

        auto include = import.include;
        while(include) {
            it->includedSymbols.push(include->item);
            include = include->next;
        }

        auto exclude = import.exclude;
        while(exclude) {
            it->excludedSymbols.push(exclude->item);
            exclude = exclude->next;
        }
    }

    // Implicitly import Prelude if the module doesn't do so by itself.
    auto preludeId = context->addUnqualifiedName("Prelude", 7);
    auto hasPrelude = module->imports.get(preludeId) != nullptr;
    if(!hasPrelude) {
        auto prelude = handler->require(context, module, preludeId);
        if(!prelude) return false;

        auto import = &module->imports[preludeId];
        import->module = prelude;
        import->localName = prelude->id;
        import->qualified = false;
    }

    return missingImports == 0;
}

struct SymbolCounts {
    U32 statements; // The number of free statements in the module.
    U32 instances; // The number of class instances in the module.
};

static SymbolCounts prepareSymbols(Context* context, Module* module, ast::Decl** decls, Size count) {
    SymbolCounts counts = {0, 0};

    for(Size i = 0; i < count; i++) {
        auto decl = decls[i];
        switch(decl->kind) {
            case ast::Decl::Fun: {
                auto ast = (ast::FunDecl*)decl;
                auto fun = defineFun(context, module, ast->name);
                fun->ast = ast;
                break;
            }
            case ast::Decl::Foreign: {
                auto ast = (ast::ForeignDecl*)decl;
                if(ast->type->kind == ast::Type::Fun) {
                    auto fun = defineForeignFun(context, module, ast->localName, nullptr);
                    fun->ast = ast;
                    fun->externalName = ast->externName;
                    fun->from = ast->from;
                } else {
                    context->diagnostics.error("not implemented: foreign globals", decl, nullptr);
                }
                break;
            }
            case ast::Decl::Stmt: {
                auto ast = (ast::StmtDecl*)decl;
                if(ast->expr->type == ast::Expr::Decl) {
                    auto expr = (ast::DeclExpr*)ast->expr;
                    if(expr->isGlobal) {
                        auto global = defineGlobal(context, module, expr->name);
                        global->ast = expr;
                    }
                }

                counts.statements++;
                break;
            }
            case ast::Decl::Alias: {
                auto ast = (ast::AliasDecl*)decl;
                auto alias = defineAlias(context, module, ast->type->name, nullptr);
                alias->ast = ast;
                alias->genCount = prepareGens(context, module, ast->type, alias->gens);
                break;
            }
            case ast::Decl::Data: {
                auto ast = (ast::DataDecl*)decl;
                U32 conCount = 0;
                auto con = ast->cons;
                while(con) {
                    conCount++;
                    con = con->next;
                }

                auto record = defineRecord(context, module, ast->type->name, conCount, ast->qualified);
                record->ast = ast;
                record->genCount = prepareGens(context, module, ast->type, record->gens);

                con = ast->cons;
                for(U32 i = 0; i < conCount; i++) {
                    defineCon(context, module, record, con->item.name, i);
                    con = con->next;
                }

                break;
            }
            case ast::Decl::Class: {
                auto ast = (ast::ClassDecl*)decl;
                auto c = defineClass(context, module, ast->type->name);
                c->ast = ast;
                c->argCount = (U16)prepareGens(context, module, ast->type, c->args);
                break;
            }
            case ast::Decl::Instance: {
                counts.instances++;
                break;
            }
        }
    }

    return counts;
}

static void resolveClassFun(Context* context, Module* m, TypeClass* c, U32 index, ast::FunDecl* ast) {
    auto type = new (c->functions + index) FunType;

    U32 argCount = 0;
    auto arg = ast->args;
    while(arg) {
        argCount++;
        arg = arg->next;
    }

    GenContext gen{nullptr, c->args, c->argCount};

    auto usedArgs = (bool*)alloca(sizeof(bool) * c->argCount);
    memset(usedArgs, 0, sizeof(bool) * c->argCount);
    U32 usedArgCount = 0;

    auto args = (FunArg*)m->memory.alloc(sizeof(FunArg) * argCount);
    arg = ast->args;
    for(U32 i = 0; i < argCount; i++) {
        auto t = resolveType(context, m, arg->item.type, &gen);
        if(t->kind == Type::Gen) {
            auto id = ((GenType*)t)->index;
            if(!usedArgs[id]) {
                usedArgs[id] = true;
                usedArgCount++;
            }
        }

        args[i].name = arg->item.name;
        args[i].index = i;
        args[i].type = t;
        arg = arg->next;
    }

    Type* expectedReturn = nullptr;
    if(ast->ret) {
        // Set the return type, if explicitly provided.
        expectedReturn = resolveType(context, m, ast->ret, &gen);
        type->result = expectedReturn;
    } else if(!ast->body) {
        // If not, the function must have a default implementation.
        context->diagnostics.error("class functions must have either an explicit type or a default implementation", ast, nullptr);
    }

    // TODO: Resolve default implementation.

    if(type->result->kind == Type::Gen) {
        auto id = ((GenType*)type->result)->index;
        if(!usedArgs[id]) {
            usedArgCount++;
        }
    }

    if(usedArgCount < c->argCount) {
        context->diagnostics.error("class functions must use each class type argument in their implementation", ast, nullptr);
    }

    type->args = args;
    type->argCount = argCount;
    createDescriptor(type, &m->memory);
}

static void resolveClass(Context* context, Module* module, TypeClass* c) {
    auto ast = c->ast;
    U32 funCount = 0;

    auto decl = ast->decls;
    while(decl) {
        funCount++;
        decl = decl->next;
    }

    auto funNames = (Id*)module->memory.alloc(sizeof(Id) * funCount);
    auto functions = (FunType*)module->memory.alloc(sizeof(FunType) * funCount);

    c->funCount = funCount;
    c->funNames = funNames;
    c->functions = functions;

    decl = ast->decls;
    for(U32 i = 0; i < funCount; i++) {
        auto f = decl->item;
        funNames[i] = f->name;
        resolveClassFun(context, module, c, i, f);

        if(module->classFunctions.add(f->name, ClassFun{c, i, f->name})) {
            context->diagnostics.error("redefinition of class function", f, nullptr);
        }
        decl = decl->next;
    }

    c->ast = nullptr;
}

void resolveFun(Context* context, Function* fun) {
    // Check if the function was resolved already.
    auto ast = fun->ast;
    if(!ast || fun->resolving) return;

    // Set the flag for recursion detection.
    fun->resolving = true;

    auto startBlock = block(fun);

    // Add the function arguments.
    // TODO: Handle the GenContext for function arguments.
    auto arg = ast->args;
    while(arg) {
        auto a = arg->item;
        auto type = resolveType(context, fun->module, a.type, nullptr);
        auto v = defineArg(context, fun, a.name, type);

        // A val-type argument is copied but can be mutated within the function.
        if(a.type->kind == ast::Type::Val) {
            auto p = alloc(startBlock, a.name, type, true, true);
            store(startBlock, 0, p, v);
        }

        arg = arg->next;
    }

    // Set the return type, if explicitly provided.
    Type* expectedReturn = nullptr;
    if(ast->ret) {
        expectedReturn = resolveType(context, fun->module, ast->ret, nullptr);
    }

    bool implicitReturn = ast->implicitReturn;

    FunBuilder builder(fun, startBlock, *context, fun->module->memory, context->exprArena);
    auto body = resolveExpr(&builder, ast->body, 0, implicitReturn);
    if(implicitReturn && body && body->kind != Inst::InstRet) {
        // The function is an expression - implicitly return the result if needed.
        ret(body->block, body);
    } else {
        // The function is a block - implicitly return void from any incomplete blocks if needed.
        for(auto block: fun->blocks) {
            if(!block->complete) {
                ret(block, nullptr);
            }
        }
    }

    Type* previous = nullptr;
    for(InstRet* r: fun->returnPoints) {
        if(previous && !compareTypes(context, previous, r->type)) {
            context->diagnostics.error("types of return statements in function don't match", ast, nullptr);
        }

        previous = r->type;
    }

    if(expectedReturn && !compareTypes(context, expectedReturn, previous)) {
        context->diagnostics.error("declared type and actual type of function don't match", ast, nullptr);
    }

    fun->returnType = previous;
    fun->resolving = false;
    fun->ast = nullptr;

    builder.exprMem.reset();
}

void resolveGlobals(Context* context, Module* module, ast::Decl** decls, Size count) {
    auto staticInit = defineFun(context, module, context->addQualifiedName("%init", 5));
    staticInit->returnType = &unitType;
    auto startBlock = block(staticInit);

    FunBuilder builder(staticInit, startBlock, *context, module->memory, context->exprArena);

    for(Size i = 0; i < count; i++) {
        if(decls[i]->kind == ast::Decl::Stmt) {
            auto decl = (ast::StmtDecl*)decls[i];
            resolveExpr(&builder, decl->expr, 0, false);
            builder.exprMem.reset();
        }
    }

    for(auto block: staticInit->blocks) {
        if(!block->complete) {
            ret(block, nullptr);
        }
    }

    module->staticInit = staticInit;
}

void resolveTypeInstance(Context* context, Module* module, ast::InstanceDecl* decl) {

}

void resolveClassFunction(Context* context, Module* module, ClassInstance* instance, ast::FunDecl* decl) {
    auto typeClass = instance->typeClass;
    int index = -1;

    for(U32 i = 0; i < typeClass->funCount; i++) {
        if(typeClass->funNames[i] == decl->name) {
            index = i;
            break;
        }
    }

    if(index < 0) {
        context->diagnostics.error("instance function doesn't match any class function", decl, nullptr);
        return;
    }

    auto fun = new (module->memory) Function;
    fun->module = module;
    fun->name = decl->name;
    fun->ast = decl;
    instance->instances[index] = fun;

    resolveFun(context, fun);
}

void resolveClassInstance(Context* context, Module* module, ast::InstanceDecl* decl, TypeClass* forClass, List<ast::Type*>* types) {
    U32 typeCount = 0;
    auto t = types;
    while(t) {
        typeCount++;
        t = t->next;
    }

    if(typeCount != forClass->argCount) {
        context->diagnostics.error("class instances must have a type for each class argument", decl->type, nullptr);
    }

    auto args = (Type**)module->memory.alloc(sizeof(Type*) * typeCount);
    t = types;
    for(U32 i = 0; i < typeCount; i++) {
        args[i] = resolveType(context, module, t->item, nullptr);
        t = t->next;
    }

    auto instance = defineInstance(context, module, forClass, args);

    auto d = decl->decls;
    while(d) {
        if(d->item->kind == ast::Decl::Fun) {
            auto f = (ast::FunDecl*)d->item;
            resolveClassFunction(context, module, instance, f);
        }

        d = d->next;
    }

    for(U32 i = 0; i < forClass->funCount; i++) {
        if(!instance->instances[i]) {
            context->diagnostics.error("class instance doesn't implement function", decl, nullptr);
        }
    }
}

void resolveInstances(Context* context, Module* module, ast::Decl** decls, Size count) {
    for(Size i = 0; i < count; i++) {
        if(decls[i]->kind == ast::Decl::Instance) {
            auto decl = (ast::InstanceDecl*)decls[i];

            bool wasClass = false;
            if(decl->type->kind == ast::Type::App) {
                auto a = (ast::AppType*)decl->type;
                if(a->base->kind == ast::Type::Con) {
                    auto con = (ast::ConType*)a->base;
                    auto c = findClass(context, module, con->con);
                    if(c) {
                        resolveClassInstance(context, module, decl, c, a->apps);
                        wasClass = true;
                    }
                }
            }

            if(!wasClass) {
                resolveTypeInstance(context, module, decl);
            }
            context->exprArena.reset();
        }
    }
}

Module* resolveModule(Context* context, ModuleHandler* handler, ast::Module* ast) {
    auto module = new Module;
    module->id = ast->name;

    // Load any imported modules.
    // These are loaded asynchronously; this function will be called again when all are loaded.
    if(!prepareImports(context, module, handler, ast->imports.pointer(), ast->imports.size())) {
        return module;
    }

    // Resolve the module contents in usage order.
    // Types use imports but nothing else, globals use types and imports, functions use everything.
    // Note that the initialization of globals is handled in the function pass,
    // since this requires knowledge of the whole module.
    auto counts = prepareSymbols(context, module, ast->decls.pointer(), ast->decls.size());

    for(auto type: module->types) {
        resolveDefinition(context, module, type);
    }

    for(TypeClass& c: module->typeClasses) {
        resolveClass(context, module, &c);
    }

    for(ForeignFunction& fun: module->foreignFunctions) {
        auto type = resolveType(context, module, fun.ast->type, nullptr);
        if(type->kind != Type::Fun) {
            context->diagnostics.error("internal error: foreign function doesn't have function type", fun.ast, nullptr);
            return module;
        }

        fun.type = (FunType*)type;
        fun.ast = nullptr;
    }

    if(counts.instances > 0) {
        resolveInstances(context, module, ast->decls.pointer(), ast->decls.size());
    }

    if(counts.statements > 0) {
        resolveGlobals(context, module, ast->decls.pointer(), ast->decls.size());
    }

    for(Function& fun: module->functions) {
        resolveFun(context, &fun);
    }

    return module;
}
