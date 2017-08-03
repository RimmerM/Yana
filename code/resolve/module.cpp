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

RecordType* defineRecord(Context* context, Module* in, Id name) {
    if(in->types.get(name)) {
        // TODO: Error
    }

    auto r = new (in->memory) RecordType;
    r->ast = nullptr;
    r->name = name;
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
    in->cons.add(name, con);
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
    if(in->functions.get(name)) {
        // TODO: Error
    }

    auto f = &in->functions[name];
    f->module = in;
    f->name = name;
    return f;
}

template<class T, class F>
T* findHelper(Context* context, Module* module, F find, Id name) {
    // Lookup:
    // - If the name is unqualified, start by searching the current scope.
    // - For qualified names, check if we have an import under that qualifier, then search that.
    // - If nothing is found, search the parent scope while retaining any qualifiers.
    auto n = context->find(name);
    if(!n.qualifier) {
        auto v = find(module, n.hash);
        if(v) return v;
    }

    // Imports have equal weight, so multiple hits here is an error.
    T* candidate = nullptr;

    for(auto& import: module->imports) {
        // TODO: Handle nested modules.
        T* v = nullptr;
        if(import.qualifier == n.qualifier->hash) {
            v = find(module, n.hash);
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
    return findHelper<Type>(context, module, [=](Module* m, Id n) -> Type* {
        auto type = m->types.get(n);
        return type ? *type : nullptr;
    }, name);
}

Con* findCon(Context* context, Module* module, Id name) {
    return findHelper<Con>(context, module, [=](Module* m, Id n) -> Con* {
        auto con = m->cons.get(n);
        return con ? *con : nullptr;
    }, name);
}

OpProperties* findOp(Context* context, Module* module, Id name) {
    return findHelper<OpProperties>(context, module, [=](Module* m, Id n) -> OpProperties* {
        auto op = m->ops.get(n);
        return op ? op : nullptr;
    }, name);
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

static void prepareImports(Context* context, Module* module, ast::Import* imports, Size count) {
    for(Size i = 0; i < count; i++) {

    }
}

static void prepareSymbols(Context* context, Module* module, ast::Decl** decls, Size count) {
    // Prepare by adding all defined types, functions and statements.
    for(Size i = 0; i < count; i++) {
        auto decl = decls[i];
        switch(decl->kind) {
            case ast::Decl::Fun: {
                auto ast = (ast::FunDecl*)decl;
                auto fun = defineFun(context, module, ast->name);
                break;
            }
            case ast::Decl::Foreign: {
                break;
            }
            case ast::Decl::Stmt: {

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
                auto record = defineRecord(context, module, ast->type->name);
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

Module* resolveModule(Context* context, ast::Module* ast) {
    auto module = new Module;
    module->name = ast->name;

    // Resolve the module contents in usage order.
    // Types use imports but nothing else, globals use types and imports, functions use everything.
    // Note that the initialization of globals is handled in the function pass,
    // since this requires knowledge of the whole module.
    prepareImports(context, module, ast->imports.pointer(), ast->imports.size());
    prepareSymbols(context, module, ast->decls.pointer(), ast->decls.size());

    for(auto type: module->types) {
        resolveDefinition(context, module, type);
    }
}
