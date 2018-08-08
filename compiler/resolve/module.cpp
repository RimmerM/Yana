#include <alloca.h>
#include "module.h"
#include "../parse/ast.h"

static constexpr U32 kMaxGens = 64;

AliasType* defineAlias(Context* context, Module* in, Id name, Type* to) {
    if(in->types.get(name)) {
        context->diagnostics.error("redefinition of type %@"_buffer, nullptr, noSource, context->findName(name));
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
        context->diagnostics.error("redefinition of type %@"_buffer, nullptr, noSource, context->findName(name));
    }

    auto r = new (in->memory) RecordType;
    r->instanceOf = nullptr;
    r->instance = nullptr;
    r->ast = nullptr;
    r->name = name;
    r->qualified = qualified;
    r->conCount = conCount;
    if(conCount > 0) {
        r->cons = (Con*)in->memory.alloc(sizeof(Con) * conCount);
    }

    in->types[name] = r;
    return r;
}

Con* defineCon(Context* context, Module* in, RecordType* to, Id name, U32 index) {
    if(in->cons.get(name)) {
        context->diagnostics.error("redefinition of constructor %@"_buffer, nullptr, noSource, context->findName(name));
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

TypeClass* defineClass(Context* context, Module* in, Id name, U32 funCount) {
    if(in->typeClasses.get(name)) {
        context->diagnostics.error("redefinition of class %@"_buffer, nullptr, noSource, context->findName(name));
    }

    auto c = &in->typeClasses[name];
    c->ast = nullptr;
    c->name = name;
    c->funCount = (U16)funCount;
    c->argCount = 0;
    c->functions = (ClassFun*)in->memory.alloc(sizeof(ClassFun) * funCount);
    return c;
}

ClassInstance* defineInstance(Context* context, Module* in, TypeClass* to, Type** args) {
    auto a = in->classInstances.add(to->name);
    auto map = a.value;

    if(!a.existed) {
        new (map) InstanceMap;
        map->forClass = to;
        map->genCount = to->argCount;
    }

    U32 insertPosition = 0;
    for(U32 i = 0; i < map->instances.size(); i++) {
        Type** types = &*map->instances[i]->forTypes;

        int cmp = 0;
        for(U32 j = 0; j < map->genCount; j++) {
            auto lhs = types[j];
            auto rhs = args[j];

            if(lhs->descriptorLength < rhs->descriptorLength) {
                cmp = -1;
            } else if(lhs->descriptorLength > rhs->descriptorLength) {
                cmp = 1;
            } else {
                cmp = compareMem(lhs->descriptor, rhs->descriptor, lhs->descriptorLength);
            }

            if(cmp != 0) {
                break;
            }
        }

        if(cmp > 0 || cmp == 0) {
            if(cmp == 0) {
                context->diagnostics.error("an instance for this class has already been defined"_buffer, nullptr, noSource);
            }

            insertPosition = i + 1;
            break;
        }
    }

    auto instance = new (in->memory) ClassInstance;
    instance->forTypes = args;
    instance->typeClass = to;
    instance->module = in;
    instance->instances = (Function**)in->memory.alloc(sizeof(Function*) * to->funCount);
    set(instance->instances, to->funCount, 0);

    if(insertPosition >= map->instances.size()) {
        map->instances.push(instance);
    } else {
        map->instances.insert(insertPosition, instance);
    }

    return instance;
}

InstanceList* defineTypeInstance(Context* context, Module* in, Type* to) {
    Hasher hasher;
    hasher.addBytes(to->descriptor, to->descriptorLength);

    auto a = in->typeInstances.add(hasher.get());
    if(!a.existed) {
        auto list = new (in->memory) InstanceList;
        list->type = to;
        *a.value = list;

        if(auto name = typeName(to)) {
            in->namedTypeInstances[name] = list;
        }
    }

    return *a.value;
}

Function* defineFun(Context* context, Module* in, Id name) {
    if(in->functions.get(name) || in->foreignFunctions.get(name)) {
        context->diagnostics.error("redefinition of function named %@"_buffer, nullptr, noSource, context->findName(name));
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
        context->diagnostics.error("redefinition of function named %@"_buffer, nullptr, noSource, context->findName(name));
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
        context->diagnostics.error("redefinition of identifier %@"_buffer, nullptr, noSource, context->findName(name));
    }

    auto g = &in->globals[name];
    g->kind = Value::Global;
    g->name = name;
    g->block = nullptr;
    g->type = nullptr;
    return g;
}

Arg* defineArg(Context* context, Function* fun, Block* block, Id name, Type* type) {
    auto index = (U32)fun->args.size();
    auto a = new (fun->module->memory) Arg;
    a->kind = Value::Arg;
    a->type = type;
    a->name = name;
    a->block = block;
    a->index = index;
    a->id = 0;

    fun->args.push(a);
    return a;
}

ClassFun* defineClassFun(Context* context, Module* module, TypeClass* typeClass, Id name, U32 index) {
    auto f = typeClass->functions + index;
    new (f) ClassFun;

    f->index = index;
    f->name = name;
    f->typeClass = typeClass;
    f->fun = defineAnonymousFun(context, module);
    f->fun->ast = nullptr;

    if(module->classFunctions.get(name)) {
        context->diagnostics.error("redefinition of class function named %@"_buffer, nullptr, noSource, context->findName(name));
    } else {
        module->classFunctions.add(name, f);
    }

    return f;
}

U32 testImport(Identifier* importName, Identifier* searchName) {
    if(importName->segmentCount > searchName->segmentCount - 1) return 0;

    U32 i = 0;
    for(; i < importName->segmentCount; i++) {
        if(importName->getHash(i) != searchName->getHash(i)) return 0;
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
        return type ? *type.unwrap() : nullptr;
    }, identifier);
}

Con* findCon(Context* context, Module* module, Id name) {
    auto identifier = &context->find(name);
    return findHelper<Con>(context, module, [=](Module* m, Identifier* id, U32 start) -> Con* {
        if(id->segmentCount - 1 == start) {
            auto con = m->cons.get(id->getHash(start));
            return con ? *con.unwrap() : nullptr;
        } else if(id->segmentCount >= 2 && id->segmentCount - 2 == start) {
            // For qualified identifiers, look up the corresponding type in case its constructors are qualified.
            Type** type = m->types.get(id->getHash(start)).unwrap();
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
                found.function = f.unwrap();
                return true;
            }

            auto fo = m->foreignFunctions.get(hash);
            if(fo) {
                found.kind = FoundFunction::Foreign;
                found.foreignFunction = fo.unwrap();
                return true;
            }

            auto c = m->classFunctions.get(hash);
            if(c) {
                found.kind = FoundFunction::Class;
                found.classFun = *c.unwrap();
                return true;
            }
        } else if(id->segmentCount >= 2 && id->segmentCount - 2 == start) {
            // Type instances have priority over classes (in case the resolver allows name conflicts between them).
            auto segment = id->getHash(start);
            auto src = id->getHash(start + 1);

            InstanceList** list = m->namedTypeInstances.get(segment).unwrap();
            if(list) {
                for(Function& fun: (*list)->functions) {
                    if(fun.name == src) {
                        found.kind = FoundFunction::Static;
                        found.function = &fun;
                        return true;
                    }
                }
            }

            TypeClass* c = m->typeClasses.get(segment).unwrap();
            if(c) {
                for(U32 i = 0; i < c->funCount; i++) {
                    if(c->functions[i].name == src) {
                        found.kind = FoundFunction::Class;
                        found.classFun = &c->functions[i];
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

Function* findInstanceFun(Context* context, Module* module, Type* fieldType, Id name) {
    Hasher hasher;
    hasher.addBytes(fieldType->descriptor, fieldType->descriptorLength);
    auto hash = hasher.get();
    auto identifier = &context->find(name);

    return findHelper<Function>(context, module, [=](Module* m, Identifier* id, U32 start) -> Function* {
        if(id->segmentCount -1 > start) return nullptr;
        InstanceList** list = m->typeInstances.get(hash).unwrap();
        if(!list) return nullptr;

        auto src = id->getHash(start);
        for(Function& fun: (*list)->functions) {
            if(fun.name == src) {
                return &fun;
            }
        }

        return nullptr;
    }, identifier);
}

OpProperties* findOp(Context* context, Module* module, Id name) {
    auto identifier = &context->find(name);
    return findHelper<OpProperties>(context, module, [=](Module* m, Identifier* id, U32 start) -> OpProperties* {
        if(id->segmentCount - 1 > start) return nullptr;
        return m->ops.get(id->getHash(start)).unwrap();
    }, identifier);
}

Global* findGlobal(Context* context, Module* module, Id name) {
    auto identifier = &context->find(name);
    return findHelper<Global>(context, module, [=](Module* m, Identifier* id, U32 start) -> Global* {
        if(id->segmentCount - 1 > start) return nullptr;
        return m->globals.get(id->getHash(start)).unwrap();
    }, identifier);
}

TypeClass* findClass(Context* context, Module* module, Id name) {
    auto identifier = &context->find(name);
    return findHelper<TypeClass>(context, module, [=](Module* m, Identifier* id, U32 start) -> TypeClass* {
        if(id->segmentCount - 1 > start) return nullptr;
        return m->typeClasses.get(id->getHash(start)).unwrap();
    }, identifier);
}

ClassInstance* findInstance(Context* context, Module* module, TypeClass* typeClass, U32 index, Type** args) {
    auto find = [=](Module* m) -> ClassInstance* {
        InstanceMap* instances = m->classInstances.get(typeClass->name).unwrap();
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

                if(compareMem(lhs->descriptor, rhs->descriptor, lhs->descriptorLength) != 0) {
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

static void prepareGens(Context* context, Module* module, GenEnv* env, Node* where, List<Id>* gens, List<ast::Arg>* args, List<ast::Constraint*>* constraints) {
    GenType* genList[kMaxGens];
    ClassConstraint classList[kMaxGens];
    GenField fieldList[kMaxGens];
    GenFun funList[kMaxGens];

    auto genCount = 0u;
    auto classCount = 0u;
    auto fieldCount = 0u;
    auto funCount = 0u;
    auto gen = gens;

    auto addSymbol = [&](Id name) -> GenType* {
        for(U32 i = 0; i < genCount; i++) {
            if(genList[i]->name == name) return genList[i];
        }

        auto type = new (module->memory) GenType(env, name, genCount);

        if(genCount == kMaxGens) {
            context->diagnostics.error("too many generic types in this context. Maximum number allowed is %@"_buffer, where, noSource, kMaxGens);
        } else {
            genList[genCount++] = type;
        }

        return type;
    };

    auto addType = [&](ast::Type* type) {
        if(!type) return;

        Id typeNames[kMaxGens];
        Size count = 0;
        findGenerics(context, toBuffer(typeNames), count, type);

        for(Size i = 0; i < count; i++) {
            addSymbol(typeNames[i]);
        }
    };

    auto addClass = [&](ast::ClassConstraint* c) {
        // We do not check if the class was already used in a constraint.
        // There are valid use cases where the same class will be used in multiple different constraints,
        // while having two equivalent constraints is not really an error - only one will end up getting used by the actual generated code.
        if(classCount == kMaxGens) {
            context->diagnostics.error("too many class constraints in this context. Maximum number allowed is %@"_buffer, c, noSource, kMaxGens);
            return;
        }

        auto count = 0u;
        GenType* typeList[kMaxGens];
        auto kind = c->type->kind;
        while(kind) {
            if(count == kMaxGens) {
                context->diagnostics.error("too many arguments to this class. Maximum number allowed is %@"_buffer, c, noSource, kMaxGens);
                break;
            }

            typeList[count++] = addSymbol(kind->item);
            kind = kind->next;
        }

        auto constraint = &classList[classCount];
        constraint->ast = c->type->name;
        constraint->classType = nullptr;
        constraint->forTypes = { (GenType**)module->memory.alloc(sizeof(GenType*)), count };
        copy(typeList, constraint->forTypes.ptr, count);

        classCount++;
    };

    auto addField = [&](ast::FieldConstraint* field) {
        auto name = field->fieldName;
        for(U32 i = 0; i < fieldCount; i++) {
            if(name == fieldList[i].fieldName && field->typeName == fieldList[i].container->name) {
                context->diagnostics.error("duplicate field constraint on type %@"_buffer, field, noSource, context->findName(field->typeName));
                return;
            }
        }

        if(fieldCount == kMaxGens) {
            context->diagnostics.error("too many field constraints in this context. Maximum number allowed is %@"_buffer, field, noSource, kMaxGens);
            return;
        }

        auto f = &fieldList[fieldCount];
        f->fieldName = name;
        f->mut = false;
        f->container = addSymbol(field->typeName);
        f->fieldType = nullptr;
        f->ast = field->type;

        addType(field->type);
        fieldCount++;
    };

    auto addFunction = [&](ast::FunctionConstraint* fun) {
        auto name = fun->name;
        for(U32 i = 0; i < fieldCount; i++) {
            if(name == funList[i].name) {
                context->diagnostics.error("duplicate function constraint in this context %@"_buffer, fun, noSource, context->findName(fun->name));
                return;
            }
        }

        if(funCount == kMaxGens) {
            context->diagnostics.error("too many function constraints in this context. Maximum number allowed is %@"_buffer, fun, noSource, kMaxGens);
            return;
        }

        auto f = &funList[funCount];
        f->name = fun->name;
        f->ast = &fun->type;
        f->type = nullptr;

        addType(&fun->type);
        funCount++;
    };

    auto addConstraint = [&](ast::Constraint* constraint) {
        switch(constraint->kind) {
            case ast::Constraint::Error:
                // AST error nodes are handled while parsing, we can ignore them here.
                break;
            case ast::Constraint::Any:
                // Any-constraints don't do anything special, but are implicitly added to the env if not already there.
                addSymbol(((ast::AnyConstraint*)constraint)->name);
                break;
            case ast::Constraint::Class:
                addClass((ast::ClassConstraint*)constraint);
                break;
            case ast::Constraint::Field:
                addField((ast::FieldConstraint*)constraint);
                break;
            case ast::Constraint::Function:
                addFunction((ast::FunctionConstraint*)constraint);
                break;
        }
    };

    // Add explicitly listed symbols.
    while(gen) {
        addSymbol(gen->item);
        gen = gen->next;
    }

    // Add implicitly used symbols from constraints.
    auto constraint = constraints;
    while(constraint) {
        addConstraint(constraint->item);
        constraint = constraint->next;
    }

    // Add any generic symbols from the argument list.
    while(args) {
        addType(args->item.type);
        args = args->next;
    }

    auto types = (GenType**)module->memory.alloc(sizeof(GenType*) * genCount);
    copy(genList, types, genCount);

    auto classes = (ClassConstraint*)module->memory.alloc(sizeof(ClassConstraint) * classCount);
    copy(classList, classes, classCount);

    auto fields = (GenField*)module->memory.alloc(sizeof(GenField) * fieldCount);
    copy(fieldList, fields, fieldCount);

    auto funs = (GenFun*)module->memory.alloc(sizeof(GenFun) * funCount);
    copy(funList, funs, funCount);

    env->types = types;
    env->typeCount = (U16)genCount;
    env->classes = classes;
    env->classCount = (U16)classCount;
    env->fields = fields;
    env->fieldCount = (U16)fieldCount;
    env->funs = funs;
    env->funCount = (U16)funCount;
}

static Buffer<GenType*> prepareArgs(Context* context, Module* module, List<Id>* args, GenEnv* env) {
    auto count = 0u;
    auto arg = args;
    while(arg) {
        count++;
        arg = arg->next;
    }

    Buffer<GenType*> types = { (GenType**)module->memory.alloc(sizeof(GenType*) * count), count };
    arg = args;
    while(arg) {
        bool found = false;
        for(auto i = 0u; i < env->typeCount; i++) {
            if(env->types[i]->name == arg->item) {
                types.ptr[i] = env->types[i];
                found = true;
                break;
            }
        }

        // The types should always be found, since the environment is partly created from the args list.
        assertTrue(found == true);
        arg = arg->next;
    }

    return types;
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

    // Implicitly import Core if the module doesn't do so by itself.
    auto coreId = context->addUnqualifiedName("Core", 4);
    auto hasCore = module->imports.get(coreId).isJust();
    if(!hasCore) {
        auto core = handler->require(context, module, coreId);
        if(!core) return false;

        auto import = &module->imports[coreId];
        import->module = core;
        import->localName = core->id;
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
                prepareGens(context, module, &fun->gen, ast, nullptr, ast->args, ast->constraints);
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
                    context->diagnostics.error("not implemented: foreign globals"_buffer, decl, noSource);
                }
                break;
            }
            case ast::Decl::Stmt: {
                auto ast = (ast::StmtDecl*)decl;

                if(ast->expr->type == ast::Expr::Decl && ((ast::DeclExpr*)ast->expr)->isGlobal) {
                    auto d = ((ast::DeclExpr*)ast->expr)->decls;
                    while(d) {
                        auto name = getDeclName(&d->item);
                        if(name) {
                            auto global = defineGlobal(context, module, name);
                            global->ast = &d->item;
                        }

                        counts.statements++;
                        d = d->next;
                    }
                } else {
                    counts.statements++;
                }

                break;
            }
            case ast::Decl::Alias: {
                auto ast = (ast::AliasDecl*)decl;
                auto alias = defineAlias(context, module, ast->type->name, nullptr);
                alias->ast = ast;
                prepareGens(context, module, &alias->gen, ast, ast->type->kind, nullptr, nullptr);

                auto args = prepareArgs(context, module, ast->type->kind, &alias->gen);
                alias->argCount = (U16)args.length;
                alias->args = args.ptr;
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
                prepareGens(context, module, &record->gen, ast, ast->type->kind, nullptr, ast->constraints);

                auto args = prepareArgs(context, module, ast->type->kind, &record->gen);
                record->argCount = (U16)args.length;
                record->args = args.ptr;

                con = ast->cons;
                for(U32 j = 0; j < conCount; j++) {
                    defineCon(context, module, record, con->item.name, j);
                    con = con->next;
                }

                break;
            }
            case ast::Decl::Class: {
                auto ast = (ast::ClassDecl*)decl;
                auto funCount = 0u;
                auto fun = ast->decls;
                while(fun) {
                    funCount++;
                    fun = fun->next;
                }

                auto c = defineClass(context, module, ast->type->name, funCount);
                c->ast = ast;
                prepareGens(context, module, &c->gen, ast, ast->type->kind, nullptr, ast->constraints);

                auto args = prepareArgs(context, module, ast->type->kind, &c->gen);
                c->argCount = (U16)args.length;
                c->args = args.ptr;

                fun = ast->decls;
                for(auto j = 0u; j < funCount; j++) {
                    auto f = defineClassFun(context, module, c, fun->item->name, j);
                    f->fun->ast = fun->item;
                    fun = fun->next;
                }

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

static void resolveClassFun(Context* context, Module* m, TypeClass* c, ClassFun* fun) {
    auto f = fun->fun;

    auto ast = f->ast;
    if(!ast) return;

    f->gen.parent = &c->gen;
    resolveFun(context, f, false);

    auto usedArgs = (bool*)alloca(sizeof(bool) * c->argCount);
    set(usedArgs, c->argCount, 0);
    U32 usedArgCount = 0;

    auto visitor = [&](Type* type) {
        if(type->kind == Type::Gen) {
            auto gen = (GenType*)type;
            if(gen->env == &c->gen) {
                if(!usedArgs[gen->index]) {
                    usedArgCount++;
                    usedArgs[gen->index] = true;
                }
            }
        }
    };

    for(auto& arg: f->args) {
        visitType(arg->type, visitor);
    }

    visitType(f->returnType, visitor);

    if(usedArgCount < c->argCount) {
        context->diagnostics.error("class functions must use each class type argument in their implementation"_buffer, ast, noSource);
    }
}

static void resolveClass(Context* context, Module* module, TypeClass* c) {
    auto ast = c->ast;
    if(!ast) return;

    resolveGens(context, module, &c->gen);

    for(U32 i = 0; i < c->funCount; i++) {
        resolveClassFun(context, module, c, c->functions + i);
    }

    c->ast = nullptr;
}

void resolveFun(Context* context, Function* fun, bool requireBody) {
    // Check if the function was resolved already.
    auto ast = fun->ast;
    if(!ast || fun->resolving) {
        if(!fun->returnType) {
            // If the function has no explicit return type, this can happen if it is called recursively or mutually recursively.
            // We cannot find out the type without implementing fully generic ML-style type inference.
            // Currently, we just require the user to explicitly provide a return type for these cases.
            assertTrue(fun->resolving);
            context->diagnostics.error("function %@ is called recursively and needs an explicit return type"_buffer, nullptr, noSource, context->findName(fun->name));
            fun->returnType = &errorType;
        }
        return;
    }

    // Set the flag for recursion detection.
    fun->resolving = true;

    // Resolve final types in the generic environment.
    resolveGens(context, fun->module, &fun->gen);

    auto startBlock = block(fun);

    // Add the function arguments.
    auto arg = ast->args;
    while(arg) {
        auto& a = arg->item;
        Type* type;

        if(a.type) {
            type = resolveType(context, fun->module, a.type, &fun->gen);
        } else {
            context->diagnostics.error("function argument has no type"_buffer, &a, noSource);
            type = &errorType;
        }

        auto v = defineArg(context, fun, startBlock, a.name, type);

        // A val-type argument is copied but can be mutated within the function.
        if(a.type->kind == ast::Type::Val) {
            auto p = alloc(startBlock, a.name, type, true, true);
            store(startBlock, 0, p, v);
        }

        arg = arg->next;
    }

    // Set the return type, if explicitly provided.
    if(ast->ret) {
        fun->returnType = resolveType(context, fun->module, ast->ret, &fun->gen);
    }

    if(ast->body) {
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
                context->diagnostics.error("types of return statements in function don't match"_buffer, ast, noSource);
            }

            previous = r->type;
        }

        if(fun->returnType && fun->returnType->kind != Type::Error) {
            if(!compareTypes(context, fun->returnType, previous)) {
                context->diagnostics.error("declared type and actual type of function don't match"_buffer, ast, noSource);
            }
        } else {
            fun->returnType = previous;
        }

        builder.exprMem.reset();
    } else {
        if(!fun->returnType) {
            fun->returnType = &unitType;
        }

        if(requireBody) {
            context->diagnostics.error("function has no body"_buffer, ast, noSource);
        }
    }

    fun->resolving = false;
    fun->ast = nullptr;
}

static void resolveGlobals(Context* context, Module* module, ast::Decl** decls, Size count) {
    auto staticInit = defineFun(context, module, context->addQualifiedName("@init", 5));
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

static void resolveTypeInstance(Context* context, Module* module, ast::InstanceDecl* decl) {
    auto type = resolveType(context, module, decl->type, nullptr);
    auto list = defineTypeInstance(context, module, type);

    auto f = decl->decls;
    while(f) {
        if(f->item->kind != ast::Decl::Fun) continue;
        auto fun = (ast::FunDecl*)f->item;

        bool found = false;
        for(auto& existing: list->functions) {
            if(existing.name == fun->name) {
                context->diagnostics.error("redefinition of type instance function"_buffer, fun, noSource);
                found = true;
            }
        }

        if(!found) {
            auto& function = *list->functions.push();
            function.name = fun->name;
            function.module = module;
            function.ast = fun;
        }

        f = f->next;
    }
}

static void resolveInstanceFunction(Context* context, Module* module, ClassInstance* instance, ast::FunDecl* decl) {
    auto typeClass = instance->typeClass;
    int index = -1;

    for(U32 i = 0; i < typeClass->funCount; i++) {
        if(typeClass->functions[i].name == decl->name) {
            index = i;
            break;
        }
    }

    if(index < 0) {
        context->diagnostics.error("instance function doesn't match any class function"_buffer, decl, noSource);
        return;
    }

    auto fun = new (module->memory) Function;
    fun->module = module;
    fun->name = decl->name;
    fun->ast = decl;
    instance->instances[index] = fun;

    resolveFun(context, fun);
}

static void resolveClassInstance(Context* context, Module* module, ast::InstanceDecl* decl, TypeClass* forClass, List<ast::Type*>* types) {
    U32 typeCount = 0;
    auto t = types;
    while(t) {
        typeCount++;
        t = t->next;
    }

    if(typeCount != forClass->argCount) {
        context->diagnostics.error("class instances must have a type for each class argument"_buffer, decl->type, noSource);
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
            resolveInstanceFunction(context, module, instance, f);
        }

        d = d->next;
    }

    for(U32 i = 0; i < forClass->funCount; i++) {
        if(!instance->instances[i]) {
            context->diagnostics.error("class instance doesn't implement function"_buffer, decl, noSource);
        }
    }
}

static void resolveInstances(Context* context, Module* module, ast::Decl** decls, Size count) {
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
            context->diagnostics.error("internal error: foreign function doesn't have function type"_buffer, fun.ast, noSource);
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

    for(InstanceList* instance: module->typeInstances) {
        for(auto& fun: instance->functions) {
            resolveFun(context, &fun);
        }
    }

    for(Function& fun: module->functions) {
        resolveFun(context, &fun);
    }

    return module;
}

Id getDeclName(ast::VarDecl* expr) {
    Id name = 0;
    if(expr->pat->kind == ast::Pat::Var) {
        name = ((ast::VarPat*)expr->pat)->var;
    } else if(expr->pat->asVar) {
        name = expr->pat->asVar;
    }

    return name;
}