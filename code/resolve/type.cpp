#include <alloca.h>
#include "type.h"
#include "../parse/ast.h"
#include "module.h"

Type unitType{Type::Unit, 0};
Type errorType{Type::Error, 0};
Type stringType{Type::String, 1};

FloatType floatTypes[FloatType::KindCount] = {
    {16, FloatType::F16},
    {32, FloatType::F32},
    {64, FloatType::F64}
};

IntType intTypes[IntType::KindCount] = {
    {1, IntType::Bool},
    {32, IntType::Int},
    {64, IntType::Long}
};

auto descriptorBuilder = []() -> bool {
    createDescriptor(&unitType, nullptr);
    createDescriptor(&errorType, nullptr);
    createDescriptor(&stringType, nullptr);

    unitType.derived = new DerivedTypes(nullptr, &unitType);
    errorType.derived = new DerivedTypes(nullptr, &errorType);
    stringType.derived = new DerivedTypes(nullptr, &stringType);

    for(U32 i = 0; i < IntType::KindCount; i++) {
        createDescriptor(intTypes + i, nullptr);
        intTypes[i].derived = new DerivedTypes(nullptr, &intTypes[i]);
    }

    for(U32 i = 0; i < FloatType::KindCount; i++) {
        createDescriptor(floatTypes + i, nullptr);
        floatTypes[i].derived = new DerivedTypes(nullptr, &floatTypes[i]);
    }

    return true;
}();

Byte* put16(Byte* buffer, Byte* max, U16 v) {
    auto b = (U16*)buffer;
    *b++ = v;
    return (Byte*)b;
}

Byte* put32(Byte* buffer, Byte* max, U32 v) {
    auto b = (U32*)buffer;
    *b++ = v;
    return (Byte*)b;
}

Byte* describeType(Type* type, Byte* buffer, Byte* max);

Byte* describeFunType(FunType* type, Byte* buffer, Byte* max) {
    buffer = put16(buffer, max, (U16)type->argCount);
    for(U32 i = 0; i < type->argCount; i++) {
        buffer = describeType(type->args[i].type, buffer, max);
    }

    buffer = describeType(type->result, buffer, max);
    return buffer;
}

Byte* describeTupType(TupType* type, Byte* buffer, Byte* max, bool writeNames) {
    buffer = put16(buffer, max, (U16)type->count);
    if(writeNames && type->named) {
        *buffer++ = 1;
        for(U32 i = 0; i < type->count; i++) {
            buffer = put32(buffer, max, type->fields[i].name);
        }
    } else {
        *buffer++ = 0;
    }

    for(U32 i = 0; i < type->count; i++) {
        auto field = type->fields[i];
        buffer = describeType(field.type, buffer, max);
    }

    return buffer;
}

Byte* describeRecordType(RecordType* type, Byte* buffer, Byte* max) {
    // TODO: Make sure that this contains the fully qualified name.
    buffer = put32(buffer, max, type->name);
    return buffer;
}

Byte* describeRefType(RefType* type, Byte* buffer, Byte* max) {
    buffer = describeType(type->to, buffer, max);

    Byte props = 0;
    if(type->isTraced) props |= 1;
    if(type->isLocal) props |= 2;
    if(type->isMutable) props |= 4;
    *buffer++ = props;

    return buffer;
}

Byte* describeType(Type* type, Byte* buffer, Byte* max) {
    if(type->descriptorLength > 0) {
        memcpy(buffer, type->descriptor, type->descriptorLength);
        return buffer + type->descriptorLength;
    }

    if(type->kind == Type::Alias) {
        return describeType(((AliasType*)type)->to, buffer, max);
    }

    *buffer++ = type->kind;

    switch(type->kind) {
        case Type::Int:
            buffer = put16(buffer, max, ((IntType*)type)->bits);
            break;
        case Type::Float:
            buffer = put16(buffer, max, ((FloatType*)type)->bits);
            break;
        case Type::Ref:
            buffer = describeRefType((RefType*)type, buffer, max);
            break;
        case Type::Fun:
            buffer = describeFunType((FunType*)type, buffer, max);
            break;
        case Type::Array:
            buffer = describeType(((ArrayType*)type)->content, buffer, max);
            break;
        case Type::Map:
            buffer = describeType(((MapType*)type)->from, buffer, max);
            buffer = describeType(((MapType*)type)->to, buffer, max);
            break;
        case Type::Tup:
            buffer = describeTupType((TupType*)type, buffer, max, true);
            break;
        case Type::Record:
            buffer = describeRecordType((RecordType*)type, buffer, max);
            break;
    }

    return buffer;
}

void createDescriptor(Type* type, Arena* arena) {
    Byte buffer[Limits::maxTypeDescriptor];
    auto length = describeType(type, buffer, buffer + Limits::maxTypeDescriptor) - buffer;

    auto descriptor = (Byte*)(arena ? arena->alloc(length) : malloc(length));
    memcpy(descriptor, buffer, length);
    type->descriptor = descriptor;
    type->descriptorLength = (U16)length;
}

static Type* findTuple(Context* context, Module* module, ast::TupType* type, GenContext* gen) {
    Byte buffer[Limits::maxTypeDescriptor];
    Byte* p = buffer;
    Byte* max = buffer + Limits::maxTypeDescriptor;
    U32 fieldCount = 0;
    U32 virtualSize = 0;
    bool named = false;

    // Generate the tuple descriptor.
    *p++ = Type::Tup;

    // Reserve space for the number of fields.
    auto descriptorFieldCount = (U16*)p;
    p = put16(p, max, 0);

    // Describe the field names, if the tuple uses named fields.
    auto field = type->fields;

    if(field->item.name) {
        *p++ = 1;
        named = true;

        while(field) {
            p = put32(p, max, field->item.name);
            fieldCount++;
            field = field->next;
        }
    } else {
        *p++ = 0;
        while(field) {
            fieldCount++;
            field = field->next;
        }
    }

    auto fields = (Field*)module->memory.alloc(sizeof(Field) * fieldCount);

    // Describe the field types.
    U32 i = 0;
    field = type->fields;
    while(field) {
        auto fieldType = resolveType(context, module, field->item.type, gen);
        memcpy(p, fieldType->descriptor, fieldType->descriptorLength);
        p += fieldType->descriptorLength;

        fields[i].name = field->item.name;
        fields[i].type = fieldType;
        fields[i].index = i;

        i++;
        virtualSize += fieldType->virtualSize;
        field = field->next;
    }

    // Update the number of fields to the final value.
    *descriptorFieldCount = (U16)fieldCount;

    // Check if the tuple was defined already.
    auto descriptorLength = p - buffer;

    Hasher hasher;
    hasher.addBytes(buffer, descriptorLength);
    auto hash = hasher.get();

    if(auto tuple = module->usedTuples.get(hash)) {
        return *tuple;
    }

    auto tuple = new (module->memory) TupType(virtualSize);
    tuple->count = fieldCount;
    tuple->fields = fields;
    tuple->named = named;

    for(i = 0; i < fieldCount; i++) {
        fields[i].container = tuple;
    }

    auto descriptor = (Byte*)module->memory.alloc(descriptorLength);
    memcpy(descriptor, buffer, descriptorLength);
    tuple->descriptor = descriptor;
    tuple->descriptorLength = (U16)descriptorLength;

    module->usedTuples.add(hash, tuple);
    return tuple;
}

static Type* findGen(Context* context, ast::GenType* type, GenContext* gen) {
    auto searchName = type->con;
    while(gen) {
        for(U32 i = 0; i < gen->count; i++) {
            if(gen->types[i].name == searchName) {
                return gen->types + i;
            }
        }

        gen = gen->parent;
    }

    return nullptr;
}

static Type* findType(Context* context, Module* module, ast::Type* type, GenContext* gen);

static Type* instantiateType(Context* context, Module* module, Type* type, Type** args, U32 count) {
    switch(type->kind) {
        case Type::Error:
        case Type::Unit:
        case Type::Int:
        case Type::Float:
        case Type::String:
            return type;
        case Type::Gen:
            return args[((GenType*)type)->index];
        case Type::Ref: {
            auto ref = (RefType*)type;
            auto instantiated = instantiateType(context, module, ref->to, args, count);
            if(instantiated == ref->to) {
                return ref;
            } else {
                return getRef(module, instantiated, ref->isTraced, ref->isLocal, ref->isMutable);
            }
        }
        case Type::Alias:
            return instantiateAlias(context, module, (AliasType*)type, args, count);
        case Type::Record:
            return instantiateRecord(context, module, (RecordType*)type, args, count);
        case Type::Array: {
            auto content = ((ArrayType*)type)->content;
            auto instantiated = instantiateType(context, module, content, args, count);

            if(instantiated == content) {
                return type;
            } else {
                return getArray(module, instantiated);
            }
        }
        case Type::Map: {
            auto map = (MapType*)type;
            auto from = instantiateType(context, module, map->from, args, count);
            auto to = instantiateType(context, module, map->to, args, count);

            if(from == map->from && to == map->to) {
                return map;
            } else {
                auto instantiated = new (module->memory) MapType(from, to);
                createDescriptor(instantiated, &module->memory);
                return instantiated;
            }
        }
        case Type::Tup: {
            auto tup = (TupType*)type;
            auto fields = tup->fields;
            auto fieldCount = tup->count;
            auto instanceFields = (Field*)alloca(sizeof(Field) * fieldCount);

            U32 changedCount = 0;
            for(U32 i = 0; i < fieldCount; i++) {
                auto t = instantiateType(context, module, fields[i].type, args, count);
                instanceFields[i].type = t;
                instanceFields[i].name = fields[i].name;

                if(t != fields[i].type) {
                    changedCount++;
                }
            }

            if(changedCount == 0) {
                return tup;
            } else {
                return resolveTupType(context, module, instanceFields, fieldCount);
            }
        }
        case Type::Fun: {
            auto fun = (FunType*)type;
            auto funArgs = fun->args;
            auto funCount = fun->argCount;

            auto instanceReturn = instantiateType(context, module, fun->result, args, count);
            auto instanceArgs = (FunArg*)alloca(sizeof(FunArg) * funCount);

            U32 changedCount = 0;
            for(U32 i = 0; i < funCount; i++) {
                instanceArgs[i].index = i;
                instanceArgs[i].name = funArgs[i].name;
                instanceArgs[i].type = instantiateType(context, module, funArgs[i].type, args, count);

                if(instanceArgs[i].type != funArgs[i].type) {
                    changedCount++;
                }
            }

            if(changedCount == 0 && instanceReturn == fun->result) {
                return fun;
            } else {
                auto finalArgs = (FunArg*)module->memory.alloc(sizeof(FunArg) * funCount);
                memcpy(finalArgs, instanceArgs, sizeof(FunArg) * funCount);

                auto instance = new (module->memory) FunType();
                instance->args = finalArgs;
                instance->result = instanceReturn;
                instance->argCount = funCount;
                createDescriptor(instance, &module->memory);
                return instance;
            }
        }
    }
}

AliasType* instantiateAlias(Context* context, Module* module, AliasType* type, Type** args, U32 count) {
    if(count != type->genCount) {
        context->diagnostics.error("incorrect number of arguments to type", nullptr, nullptr);
        return type;
    }

    auto to = instantiateType(context, module, type->to, args, count);
    auto alias = new (module->memory) AliasType;

    alias->instanceOf = type;
    alias->ast = nullptr;
    alias->genCount = 0;
    alias->name = type->name;
    alias->to = to;
    alias->virtualSize = to->virtualSize;
    alias->descriptor = to->descriptor;
    alias->descriptorLength = to->descriptorLength;

    return alias;
}

RecordType* instantiateRecord(Context* context, Module* module, RecordType* type, Type** args, U32 count) {
    if(count != type->genCount) {
        context->diagnostics.error("incorrect number of arguments to type", nullptr, nullptr);
        return type;
    }

    auto record = new (module->memory) RecordType;
    record->ast = nullptr;
    record->instanceOf = type;
    record->conCount = type->conCount;
    record->name = type->name;
    record->genCount = 0;
    record->kind = type->kind;
    record->qualified = type->qualified;

    auto instance = (Type**)module->memory.alloc(sizeof(Type*) * count);
    memcpy(instance, args, sizeof(Type*) * count);
    record->instance = instance;
    record->genCount = count;

    auto cons = (Con*)module->memory.alloc(sizeof(Con) * type->conCount);
    record->cons = cons;

    for(U32 i = 0; i < type->conCount; i++) {
        auto& con = type->cons[i];
        auto instanceCon = &cons[i];
        instanceCon->parent = record;
        instanceCon->count = con.count;
        instanceCon->name = con.name;
        instanceCon->index = con.index;
        instanceCon->exported = con.exported;

        auto fields = (Field*)module->memory.alloc(sizeof(Field) * con.count);
        instanceCon->fields = fields;

        for(U32 f = 0; f < con.count; f++) {
            fields[f].index = con.fields[f].index;
            fields[f].name = con.fields[f].name;
            fields[f].container = record;
            fields[f].type = instantiateType(context, module, con.fields[f].type, args, count);
        }
    }

    return record;
}

static Type* resolveApp(Context* context, Module* module, ast::AppType* type, GenContext* gen) {
    // Find the base type and instantiate it for these arguments.
    auto base = findType(context, module, type->base, gen);
    if(!base) return nullptr;

    U32 argCount = 0;
    auto arg = type->apps;
    while(arg) {
        argCount++;
        arg = arg->next;
    }

    auto args = (Type**)alloca(sizeof(Type*) * argCount);
    arg = type->apps;
    for(U32 i = 0; i < argCount; i++) {
        args[i] = resolveType(context, module, arg->item, gen);
        arg = arg->next;
    }

    if(base->kind == Type::Alias) {
        return instantiateAlias(context, module, (AliasType*)base, args, argCount);
    } else if(base->kind == Type::Record) {
        return instantiateRecord(context, module, (RecordType*)base, args, argCount);
    } else {
        context->diagnostics.error("type is not a higher order type", type->base, nullptr);
        return nullptr;
    }
}

static Type* findType(Context* context, Module* module, ast::Type* type, GenContext* gen) {
    switch(type->kind) {
        case ast::Type::Error:
            return &errorType;
        case ast::Type::Unit:
            return &unitType;
        case ast::Type::Ptr: {
            auto ast = (ast::PtrType*)type;
            auto content = resolveType(context, module, ast->type, gen);
            return getRef(module, content, false, false, true);
        }
        case ast::Type::Ref: {
            auto ast = (ast::RefType*)type;
            auto content = resolveType(context, module, ast->type, gen);
            return getRef(module, content, true, false, true);
        }
        case ast::Type::Val: {
            auto ast = (ast::ValType*)type;
            return resolveType(context, module, ast->type, gen);
        }
        case ast::Type::Tup:
            return findTuple(context, module, (ast::TupType*)type, gen);
        case ast::Type::Gen:
            return findGen(context, (ast::GenType*)type, gen);
        case ast::Type::App:
            return resolveApp(context, module, (ast::AppType*)type, gen);
        case ast::Type::Con: {
            auto found = findType(context, module, ((ast::ConType*)type)->con);
            if(!found) {
                context->diagnostics.error("unresolved type name", type, nullptr);
                return &errorType;
            }

            return found;
        }
        case ast::Type::Fun: {
            auto ast = (ast::FunType*)type;
            auto ret = resolveType(context, module, ast->ret, gen);
            U32 argc = 0;
            auto arg = ast->args;
            while(arg) {
                argc++;
                arg = arg->next;
            }

            FunArg* args = nullptr;
            if(argc > 0) {
                args = (FunArg*)module->memory.alloc(sizeof(FunArg) * argc);
                arg = ast->args;
                for(U32 i = 0; i < argc; i++) {
                    args[i].type = resolveType(context, module, arg->item.type, gen);
                    args[i].index = i;
                    args[i].name = arg->item.name;
                    arg = arg->next;
                }
            }

            auto fun = new (module->memory) FunType();
            fun->args = args;
            fun->result = ret;
            fun->argCount = argc;
            createDescriptor(fun, &module->memory);
            return fun;
        }
        case ast::Type::Arr: {
            auto ast = (ast::ArrType*)type;
            auto content = resolveType(context, module, ast->type, gen);
            return getArray(module, content);
        }
        case ast::Type::Map: {
            auto ast = (ast::MapType*)type;
            auto from = resolveType(context, module, ast->from, gen);
            auto to = resolveType(context, module, ast->to, gen);
            auto map = new (module->memory) MapType(from, to);
            createDescriptor(map, &module->memory);
            return map;
        }
    }
}

void resolveAlias(Context* context, Module* module, AliasType* type) {
    auto ast = type->ast;
    if(ast) {
        type->ast = nullptr;

        GenContext gen{nullptr, type->gens, type->genCount};
        auto to = findType(context, module, ast->target, &gen);
        type->to = to;
        type->virtualSize = to->virtualSize;
        type->descriptorLength = to->descriptorLength;
        type->descriptor = to->descriptor;
    }
}

void resolveRecord(Context* context, Module* module, RecordType* type) {
    auto ast = type->ast;
    if(ast) {
        type->ast = nullptr;

        U32 filledCount = 0;
        U32 maxSize = 0;

        GenContext gen{nullptr, type->gens, type->genCount};
        auto conAst = ast->cons;
        for(U32 i = 0; i < type->conCount; i++) {
            if(conAst->item.content) {
                auto content = findType(context, module, conAst->item.content, &gen);
                if(content->kind == Type::Tup) {
                    auto tup = (TupType*)content;
                    type->cons[i].fields = tup->fields;
                    type->cons[i].count = tup->count;
                } else {
                    auto field = new (module->memory) Field;
                    field->type = content;
                    field->name = 0;
                    field->index = 0;
                    field->container = type;
                    type->cons[i].fields = field;
                    type->cons[i].count = 1;
                }

                filledCount++;
                if(content->virtualSize > maxSize) {
                    maxSize = content->virtualSize;
                }
            }
            conAst = conAst->next;
        }

        type->virtualSize = 1 + maxSize;
        if(filledCount == 0) {
            type->kind = RecordType::Enum;
        } else if(type->conCount == 1) {
            type->kind = RecordType::Single;
        }
    }
}

Type* getRef(Module* module, Type* to, bool traced, bool local, bool mut) {
    if(!to->derived) {
        to->derived = new (module->memory) DerivedTypes(module, to);
    }

    if(traced && mut) {
        return &to->derived->tracedMutableRef;
    } else if(traced) {
        return &to->derived->tracedImmutableRef;
    } else if(local && mut) {
        return &to->derived->localMutableRef;
    } else if(local) {
        return &to->derived->localImmutableRef;
    } else {
        return &to->derived->untracedRef;
    }
}

Type* getArray(Module* module, Type* to) {
    if(!to->derived) {
        to->derived = new (module->memory) DerivedTypes(module, to);
    }

    return &to->derived->arrayTo;
}

Type* resolveDefinition(Context* context, Module* module, Type* type) {
    if(type->kind == Type::Alias) {
        resolveAlias(context, module, (AliasType*)type);
    } else if(type->kind == Type::Record) {
        resolveRecord(context, module, (RecordType*)type);
    }

    return type;
}

Type* resolveType(Context* context, Module* module, ast::Type* type, GenContext* gen) {
    auto found = findType(context, module, type, gen);
    if(
        (found->kind == Type::Alias && ((AliasType*)found)->genCount > 0) ||
        (found->kind == Type::Record && ((RecordType*)found)->genCount > 0 && ((RecordType*)found)->instance == nullptr)
    ) {
        context->diagnostics.error("cannot use a generic type here", type, nullptr);
    }

    return found;
}

bool compareTypes(Context* context, Type* lhs, Type* rhs) {
    if(lhs->kind == Type::Alias) lhs = ((AliasType*)lhs)->to;
    if(rhs->kind == Type::Alias) rhs = ((AliasType*)rhs)->to;
    if(lhs == rhs) return true;

    // TODO: Remaining type kinds.
    switch(lhs->kind) {
        case Type::Error:
            // Error types are compatible with everything, in order to prevent a cascade of errors.
            return true;
        case Type::Unit:
            return rhs->kind == Type::Unit;
        case Type::Int:
            return rhs->kind == Type::Int && ((IntType*)lhs)->width == ((IntType*)rhs)->width;
        case Type::Float:
            return rhs->kind == Type::Float && ((FloatType*)lhs)->width == ((FloatType*)rhs)->width;
        case Type::String:
            return rhs->kind == Type::String;
        case Type::Ref: {
            if(rhs->kind != Type::Ref) return false;
            auto a = (RefType*)lhs;
            auto b = (RefType*)rhs;

            if(a->isTraced != b->isTraced) return false;
            if(a->isLocal != b->isLocal) return false;
            if(a->isMutable != b->isMutable) return false;

            return compareTypes(context, ((RefType*)lhs)->to, ((RefType*)rhs)->to);
        }
        case Type::Array:
            return rhs->kind == Type::Array && compareTypes(context, ((ArrayType*)lhs)->content, ((ArrayType*)rhs)->content);
        case Type::Map: {
            if(rhs->kind != Type::Map) return false;
            auto a = (MapType*)lhs, b = (MapType*)rhs;
            return compareTypes(context, a->from, b->from) && compareTypes(context, a->to, b->to);
        }
    }

    return false;
}

TupType* resolveTupType(Context* context, Module* module, Field* sourceFields, U32 count) {
    Byte buffer[Limits::maxTypeDescriptor];
    Byte* p = buffer;
    Byte* max = buffer + Limits::maxTypeDescriptor;
    U32 virtualSize = 0;
    bool named = false;

    // Generate the tuple descriptor.
    *p++ = Type::Tup;
    p = put16(p, max, (U16)count);

    // Describe the field names, if the tuple uses named fields.
    if(sourceFields->name) {
        *p++ = 1;
        named = true;
        for(U32 i = 0; i < count; i++) {
            p = put32(p, max, sourceFields[i].name);
        }
    } else {
        *p++ = 0;
    }

    auto fields = (Field*)module->memory.alloc(sizeof(Field) * count);

    // Describe the field types.
    for(U32 i = 0; i < count; i++) {
        auto fieldType = sourceFields[i].type;
        memcpy(p, fieldType->descriptor, fieldType->descriptorLength);
        p += fieldType->descriptorLength;

        fields[i].name = sourceFields[i].name;
        fields[i].type = fieldType;
        fields[i].index = i;

        virtualSize += fieldType->virtualSize;
    }

    // Check if the tuple was defined already.
    auto descriptorLength = p - buffer;

    Hasher hasher;
    hasher.addBytes(buffer, descriptorLength);
    auto hash = hasher.get();

    if(auto tuple = module->usedTuples.get(hash)) {
        return *tuple;
    }

    auto tuple = new (module->memory) TupType(virtualSize);
    tuple->count = count;
    tuple->fields = fields;
    tuple->named = named;

    for(U32 i = 0; i < count; i++) {
        fields[i].container = tuple;
    }

    auto descriptor = (Byte*)module->memory.alloc(descriptorLength);
    memcpy(descriptor, buffer, descriptorLength);
    tuple->descriptor = descriptor;
    tuple->descriptorLength = (U16)descriptorLength;

    module->usedTuples.add(hash, tuple);
    return tuple;
}

Type* canonicalType(Type* type) {
    switch(type->kind) {
        case Type::Ref:
            return ((RefType*)type)->to;
        case Type::Record: {
            auto t = (RecordType*)type;
            if(t->conCount == 1 && t->cons[0].count == 1) {
                return t->cons[0].fields[0].type;
            } else {
                return type;
            }
        }
        case Type::Alias:
            return ((AliasType*)type)->to;
        default:
            return type;
    }
}

DerivedTypes::DerivedTypes(Module* module, Type *type) :
        tracedMutableRef(type, true, false, true),
        tracedImmutableRef(type, true, false, false),
        localMutableRef(type, false, true, true),
        localImmutableRef(type, false, true, false),
        untracedRef(type, false, false, true),
        arrayTo(type) {
    auto arena = module ? &module->memory : nullptr;

    createDescriptor(&tracedMutableRef, arena);
    createDescriptor(&tracedImmutableRef, arena);
    createDescriptor(&localMutableRef, arena);
    createDescriptor(&localImmutableRef, arena);
    createDescriptor(&untracedRef, arena);
    createDescriptor(&arrayTo, arena);
}