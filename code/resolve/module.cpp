#include "module.h"
#include "../parse/ast.h"

AliasType* defineAlias(Context* context, Module* in, Id name, Type* to) {
    if(in->types.get(name)) {
        // TODO: Error
    }

    auto alias = new (in->memory) AliasType;
    alias->ast = nullptr;
    alias->name = name;
    alias->to = to;
    in->types[name] = alias;
    return alias;
}

RecordType* defineRecord(Context* context, Module* in, Id name, bool qualified) {
    if(in->types.get(name)) {
        // TODO: Error
    }

    auto r = new (in->memory) RecordType;
    r->ast = nullptr;
    r->name = name;
    r->qualified = qualified;
    in->types[name] = r;
    return r;
}

Con* defineCon(Context* context, Module* in, RecordType* to, Id name, Type* content) {
    if(in->cons.get(name)) {
        // TODO: Error
    }

    auto con = &*to->cons.push();
    con->name = name;
    con->content = content;
    con->index = (U32)(con - to->cons.pointer());
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
    g->module = in;
    g->name = name;
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

        auto type = m->types.get(id->segmentHashes[start]);
        return type ? *type : nullptr;
    }, identifier);
}

Con* findCon(Context* context, Module* module, Id name) {
    auto identifier = &context->find(name);
    return findHelper<Con>(context, module, [=](Module* m, Identifier* id, U32 start) -> Con* {
        if(id->segmentCount - 1 == start) {
            auto con = m->cons.get(id->segmentHashes[start]);
            return con ? *con : nullptr;
        } else if(id->segmentCount >= 2 && id->segmentCount - 2 == start) {
            // For qualified identifiers, look up the corresponding type in case its constructors are qualified.
            Type** type = m->types.get(id->segmentHashes[start]);
            if(!type || (*type)->kind != Type::Record) return nullptr;

            auto record = (RecordType*)*type;
            auto conName = id->segmentHashes[start + 1];
            for(Con& con: record->cons) {
                if(con.name == conName) return &con;
            }

            return nullptr;
        } else {
            return nullptr;
        }
    }, identifier);
}

OpProperties* findOp(Context* context, Module* module, Id name) {
    auto identifier = &context->find(name);
    return findHelper<OpProperties>(context, module, [=](Module* m, Identifier* id, U32 start) -> OpProperties* {
        if(id->segmentCount - 1 > start) return nullptr;

        auto op = m->ops.get(id->segmentHashes[start]);
        return op ? op : nullptr;
    }, identifier);
}

static void prepareGens(Context* context, Module* module, ast::SimpleType* type, Array<Type*>& gens) {
    auto kind = type->kind;
    U32 i = 0;
    while(kind) {
        gens.push(new (module->memory) GenType(i));
        kind = kind->next;
        i++;
    }
}

static void prepareCons(Context* context, Module* module, RecordType* type, List<ast::Con>* con) {
    while(con) {
        defineCon(context, module, type, con->item.name, nullptr);
        con = con->next;
    }
}

// Tries to load any imported modules.
// Returns true if all modules were available; otherwise, compilation should pause until they are.
static bool prepareImports(Context* context, Module* module, ModuleHandler* handler, ast::Import* imports, Size count) {
    U32 missingImports = 0;
    for(Size i = 0; i < count; i++) {
        auto& import = imports[i];
        auto name = &context->find(import.from);
        auto importModule = handler->require(context, module, name);
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
        auto preludeName = &context->find(preludeId);
        auto prelude = handler->require(context, module, preludeName);
        if(!prelude) return false;

        auto import = &module->imports[preludeId];
        import->module = prelude;
        import->localName = preludeName;
        import->qualified = false;
    }

    return missingImports == 0;
}

static void prepareSymbols(Context* context, Module* module, ast::Decl** decls, Size count) {
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
                    auto global = defineGlobal(context, module, expr->name);
                    global->ast = expr;
                }
                break;
            }
            case ast::Decl::Alias: {
                auto ast = (ast::AliasDecl*)decl;
                auto alias = defineAlias(context, module, ast->type->name, nullptr);
                alias->ast = ast;
                prepareGens(context, module, ast->type, alias->gens);
                break;
            }
            case ast::Decl::Data: {
                auto ast = (ast::DataDecl*)decl;
                auto record = defineRecord(context, module, ast->type->name, ast->qualified);
                record->ast = ast;
                prepareGens(context, module, ast->type, record->gens);
                prepareCons(context, module, record, ast->cons);
                break;
            }
            case ast::Decl::Class: {
                auto ast = (ast::ClassDecl*)decl;
                auto c = defineClass(context, module, ast->type->name);
                c->ast = ast;
                prepareGens(context, module, ast->type, c->parameters);
                break;
            }
        }
    }
}

void resolveFun(Context* context, Function* fun) {
    // Check if the function was resolved already.
    auto ast = fun->ast;
    if(!ast) return;

    // Set the flag for recursion detection.
    fun->resolving = true;

    // Add the function arguments.
    auto arg = ast->args;
    while(arg) {
        auto a = arg->item;
        auto type = resolveType(context, fun->module, a.type);
        defineArg(context, fun, a.name, type);
        arg = arg->next;
    }

    // Set the return type, if explicitly provided.
    Type* expectedReturn = nullptr;
    if(ast->ret) {
        expectedReturn = resolveType(context, fun->module, ast->ret);
    }

    bool resultUsed = true;
    if(ast->body->type == ast::Expr::Multi) {
        resultUsed = false;
    }

    FunBuilder builder(fun, &*fun->blocks.push(), *context, fun->module->memory);
    auto body = resolveExpr(&builder, ast->body, 0, resultUsed);
    if(resultUsed && body->kind != Inst::InstRet) {
        // The function is an expression - implicitly return the result if needed.
        ret(body->block, body);
    } else if(!body->block->complete) {
        // The function is a block - implicitly return void if needed.
        ret(body->block, nullptr);
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

Module* resolveModule(Context* context, ModuleHandler* handler, ast::Module* ast) {
    auto module = new Module;
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
    prepareSymbols(context, module, ast->decls.pointer(), ast->decls.size());

    for(auto type: module->types) {
        resolveDefinition(context, module, type);
    }

    for(ForeignFunction& fun: module->foreignFunctions) {
        auto type = resolveType(context, module, fun.ast->type);
        if(type->kind != Type::Fun) {
            context->diagnostics.error("internal error: foreign function doesn't have function type", fun.ast, nullptr);
            return module;
        }

        fun.type = (FunType*)type;
        fun.ast = nullptr;
    }

    for(Function& fun: module->functions) {
        resolveFun(context, &fun);
    }
}
