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

Con* defineCon(Context* context, Module* in, RecordType* to, Id name, U32 index, Field* fields, U32 count) {
    if(in->cons.get(name)) {
        // TODO: Error
    }

    auto con = to->cons + index;
    con->name = name;
    con->fields = fields;
    con->count = count;
    con->index = index;
    con->parent = to;

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

static InstanceLookup* findInstance(Module* module, TypeClass* to, InstanceLookup* lookup, Type** args, Size count) {
    if(count > 0) {
        auto type = args[0];

        // If there already was a table for this type, continue in that one.
        if(auto t = lookup->next.get((Size)type)) {
            return findInstance(module, to, t, args + 1, count - 1);
        }

        // Otherwise, create a new table first.
        auto next = &lookup->next[(Size)type];
        auto depth = lookup->depth + 1;

        next->depth = depth;
        next->instance.module = module;
        next->instance.forTypes = nullptr;
        next->instance.typeClass = to;
        next->instance.instances = nullptr;

        return findInstance(module, to, next, args + 1, count - 1);
    } else {
        return lookup;
    }
}

ClassInstance* defineInstance(Context* context, Module* in, TypeClass* to, Type** args) {
    auto instance = findInstance(in, to, &in->classInstances[to->name], args, to->argCount);
    instance->instance.forTypes = args;
    instance->instance.instances = (Function**)in->memory.alloc(sizeof(Function*) * to->funCount);
    return &instance->instance;
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
    auto a = fun->args.push();
    a->kind = Value::Arg;
    a->type = type;
    a->name = name;
    a->block = nullptr;
    a->index = index;
    return &*a;
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
            auto start = testImport(import.localName, name);
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

Function* findFun(Context* context, Module* module, Id name) {
    auto identifier = &context->find(name);
    return findHelper<Function>(context, module, [=](Module* m, Identifier* id, U32 start) -> Function* {
        if(id->segmentCount - 1 == start) {
            // Handle free functions.
            // findHelper will automatically make sure that there are no conflicts in imports.
            return m->functions.get(id->getHash(start));
        } else if(id->segmentCount >= 2 && id->segmentCount - 2 == start) {
            // TODO: Handle qualified functions, such as type instances.
            // Type instances have priority over classes (in case the resolver allows name conflicts between them).
            return nullptr;
        }

        return nullptr;
    }, identifier);
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
        it->localName = &context->find(import.localName);
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
        import->localName = prelude->name;
        import->qualified = false;
    }

    return missingImports == 0;
}

struct SymbolCounts {
    U32 statements; // The number of free statements in the module.
};

static SymbolCounts prepareSymbols(Context* context, Module* module, ast::Decl** decls, Size count) {
    SymbolCounts counts = {0};

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
                    defineCon(context, module, record, con->item.name, i, nullptr, 0);
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

    auto args = (FunArg*)m->memory.alloc(sizeof(FunArg) * argCount);
    arg = ast->args;
    for(U32 i = 0; i < argCount; i++) {
        args[i].name = arg->item.name;
        args[i].index = i;
        args[i].type = resolveType(context, m, arg->item.type, &gen);
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

    FunBuilder builder(fun, startBlock, *context, fun->module->memory);
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
}

void resolveGlobals(Context* context, Module* module, ast::Decl** decls, Size count) {
    auto staticInit = defineFun(context, module, context->addQualifiedName("$init", 5));
    staticInit->returnType = &unitType;
    auto startBlock = block(staticInit);

    FunBuilder builder(staticInit, startBlock, *context, staticInit->module->memory);

    for(Size i = 0; i < count; i++) {
        if(decls[i]->kind == ast::Decl::Stmt) {
            auto decl = (ast::StmtDecl*)decls[i];
            resolveExpr(&builder, decl->expr, 0, false);
        }
    }

    for(auto block: staticInit->blocks) {
        if(!block->complete) {
            ret(block, nullptr);
        }
    }

    module->staticInit = staticInit;
}

Module* resolveModule(Context* context, ModuleHandler* handler, ast::Module* ast) {
    auto module = new Module;
    module->id = ast->name;
    module->name = &context->find(ast->name);

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

    if(counts.statements > 0) {
        resolveGlobals(context, module, ast->decls.pointer(), ast->decls.size());
    }

    for(Function& fun: module->functions) {
        resolveFun(context, &fun);
    }

    return module;
}
