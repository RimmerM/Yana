#include "module.h"
#include "../parse/ast.h"

AliasType* defineAlias(Module* in, Id name, Type* to) {
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

RecordType* defineRecord(Module* in, Id name) {
    if(in->types.get(name)) {
        // TODO: Error
    }

    auto r = new (in->memory) RecordType;
    r->ast = nullptr;
    r->name = name;
    in->types[name] = r;
    return r;
}

Con* defineCon(Module* in, RecordType* to, Id name, Type* content) {
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

TypeClass* defineClass(Module* in, Id name) {
    if(in->typeClasses.get(name)) {
        // TODO: Error
    }

    auto c = &in->typeClasses[name];
    c->ast = nullptr;
    c->name = name;
    return c;
}

Function* defineFun(Module* in, Id name) {
    if(in->functions.get(name)) {
        // TODO: Error
    }

    auto f = &in->functions[name];
    f->module = in;
    f->name = name;
    return f;
}

static void prepareGens(Module* module, ast::SimpleType* type, Array<Type*>& gens) {
    auto kind = type->kind;
    U32 i = 0;
    while(kind) {
        gens.push(new (module->memory) GenType(i));
        kind = kind->next;
        i++;
    }
}

static void prepareCons(Module* module, RecordType* type, List<ast::Con>* con) {
    while(con) {
        defineCon(module, type, con->item.name, nullptr);
        con = con->next;
    }
}

static void prepareImports(Module* module, ast::Import* imports, Size count) {
    for(Size i = 0; i < count; i++) {

    }
}

static void resolveTypes(Module* module, ast::Decl** decls, Size count) {
    // Prepare by adding all defined types.
    for(Size i = 0; i < count; i++) {
        auto decl = decls[i];
        if(decl->kind == ast::Decl::Alias) {
            auto ast = (ast::AliasDecl*)decl;
            auto alias = defineAlias(module, ast->type->name, nullptr);
            alias->ast = ast;
            prepareGens(module, ast->type, alias->gens);
        } else if(decl->kind == ast::Decl::Data) {
            auto ast = (ast::DataDecl*)decl;
            auto record = defineRecord(module, ast->type->name);
            record->ast = ast;
            prepareGens(module, ast->type, record->gens);
            prepareCons(module, record, ast->cons);
        } else if(decl->kind == ast::Decl::Class) {
            auto ast = (ast::ClassDecl*)decl;
            auto c = defineClass(module, ast->type->name);
            c->ast = ast;
            prepareGens(module, ast->type, c->parameters);
        }
    }

    // When all names are defined, start resolving the types.
    for(auto type: module->types) {

    }
}

static void resolveStatements(Module* module, ast::Decl** decls, Size count) {
    for(Size i = 0; i < count; i++) {

    }
}

static void resolveFunctions(Module* module, ast::Decl** decls, Size count) {
    for(Size i = 0; i < count; i++) {
        auto decl = decls[i];
        if(decl->kind == ast::Decl::Fun) {
            auto ast = (ast::FunDecl*)decl;
            auto fun = defineFun(module, ast->name);
        } else if(decl->kind == ast::Decl::Foreign) {

        }
    }
}

Module* resolveModule(ast::Module* ast) {
    auto module = new Module;
    module->name = ast->name;

    // Resolve the module contents in usage order.
    // Types use imports but nothing else, globals use types and imports, functions use everything.
    // Note that the initialization of globals is handled in the function pass,
    // since this requires knowledge of the whole module.
    prepareImports(module, ast->imports.pointer(), ast->imports.size());
    resolveTypes(module, ast->decls.pointer(), ast->decls.size());
    resolveFunctions(module, ast->decls.pointer(), ast->decls.size());
    resolveStatements(module, ast->decls.pointer(), ast->decls.size());
}
