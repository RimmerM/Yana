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
    if(in->functions.get(name)) {
        // TODO: Error
    }

    auto f = &in->functions[name];
    f->module = in;
    f->name = name;
    return f;
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

Module* resolveModule(Context* context, ast::Module* ast) {
    auto module = new Module;
    module->name = &context->find(ast->name);

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
