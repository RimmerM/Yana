#include <alloca.h>
#include "type.h"
#include "../parse/ast.h"
#include "module.h"
#include "Mem/Hash.h"

UnitType unitType;
ErrorType errorType;
StringType stringType;

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

Byte* describePtrType(PtrType* type, Byte* buffer, Byte* max) {
    return describeType(type->to, buffer, max);
}

Byte* describeType(Type* type, Byte* buffer, Byte* max) {
    if(type->descriptorLength > 0) {
        copyMem(type->descriptor, buffer, type->descriptorLength);
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
        case Type::Ptr:
            buffer = describePtrType((PtrType*)type, buffer, max);
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

    auto descriptor = (Byte*)(arena ? arena->alloc(length) : hAlloc(length));
    copyMem(buffer, descriptor, length);
    type->descriptor = descriptor;
    type->descriptorLength = (U16)length;
}

StringId typeName(Type* type) {
    if(type->kind == Type::Alias) {
        return ((AliasType*)type)->name;
    } else if(type->kind == Type::Record) {
        return ((RecordType*)type)->name;
    }

    return 0;
}

static Type* findTuple(Context* context, Module* module, ast::Type* type) {
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
    auto field = type->tup;

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
    field = type->tup;
    while(field) {
        auto fieldType = resolveType(context, module, field->item.type);
        copyMem(fieldType->descriptor, p, fieldType->descriptorLength);
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

    Tritium::Hasher hasher;
    hasher.addBytes(buffer, descriptorLength);
    auto hash = hasher.get();

    if(auto tuple = module->usedTuples.get(hash)) {
        return tuple.unwrap();
    }

    auto tuple = new (module->memory) TupType(virtualSize);
    tuple->count = fieldCount;
    tuple->fields = fields;
    tuple->named = named;

    for(i = 0; i < fieldCount; i++) {
        fields[i].container = tuple;
    }

    auto descriptor = (Byte*)module->memory.alloc(descriptorLength);
    copyMem(buffer, descriptor, descriptorLength);
    tuple->descriptor = descriptor;
    tuple->descriptorLength = (U16)descriptorLength;

    module->usedTuples.add(hash, tuple);
    return tuple;
}

static Type* findType(Context* context, Module* module, ast::Type* type) {
    switch(type->kind) {
        case ast::Type::Error:
            return &errorType;
        case ast::Type::Unit:
            return &unitType;
        case ast::Type::Ptr: {
            auto ast = (ast::PtrType*)type;
            auto content = resolveType(context, module, ast->type);
            return getPtr(module, content);
        }
        case ast::Type::Val: {
            auto ast = (ast::ValType*)type;
            return resolveType(context, module, ast->type);
        }
        case ast::Type::Tup:
            return findTuple(context, module, (ast::TupType*)type);
        case ast::Type::Con: {
            auto con = ((ast::ConType*)type)->con;
            auto found = findType(context, module, con);
            if(!found) {
                context->diagnostics.error("unresolved type name %@"_v, type, context->findName(con));
                return &errorType;
            }

            resolveDefinition(context, module, found);
            return found;
        }
        case ast::Type::Fun: {
            auto ast = (ast::FunType*)type;
            auto ret = resolveType(context, module, ast->ret);
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
                    args[i].type = resolveType(context, module, arg->item.type);
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
            auto content = resolveType(context, module, ast->type);
            return getArray(module, content);
        }
        case ast::Type::Map: {
            auto ast = (ast::MapType*)type;
            auto from = resolveType(context, module, ast->from);
            auto to = resolveType(context, module, ast->to);
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

        auto to = findType(context, module, ast->target);
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

        auto conAst = ast->cons;
        for(U32 i = 0; i < type->conCount; i++) {
            auto contentAst = conAst->item.content;
            if(contentAst) {
                Type* content = nullptr;

                // For tuples with a single element, we inline the contents into the record.
                // This is also needed to generate the correct type for the single-constructor shorthand syntax.
                if(contentAst->kind == ast::Type::Tup) {
                    auto tup = (ast::TupType*)contentAst;
                    if(tup->fields && !tup->fields->next && !tup->fields->item.name) {
                        content = findType(context, module, tup->fields->item.type);
                    }
                }

                if(!content) {
                    content = findType(context, module, contentAst);
                }

                // The unit type as only content of a constructor is equivalent to an empty constructor.
                if(content->kind != Type::Unit) {
                    type->cons[i].content = content;

                    filledCount++;
                    if(content->virtualSize > maxSize) {
                        maxSize = content->virtualSize;
                    }
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

Type* getRef(Module* module, Type* to) {
    if(!to->derived) {
        to->derived = new (module->memory) DerivedTypes(module, to);
    }

    return &to->derived->ptr;
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

Type* resolveType(Context* context, Module* module, ast::Type* type) {
    auto found = findType(context, module, type);
    return found;
}

bool compareTypes(Context* context, Type* lhs, Type* rhs) {
    if(lhs->kind == Type::Alias) lhs = ((AliasType*)lhs)->to;
    if(rhs->kind == Type::Alias) rhs = ((AliasType*)rhs)->to;
    if(lhs == rhs) return true;

    if(lhs->descriptorLength != rhs->descriptorLength) return false;
    return compareMem(lhs->descriptor, rhs->descriptor, lhs->descriptorLength) == 0;
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
        copyMem(fieldType->descriptor, p, fieldType->descriptorLength);
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
        return tuple.unwrap();
    }

    auto tuple = new (module->memory) TupType(virtualSize);
    tuple->count = count;
    tuple->fields = fields;
    tuple->named = named;

    for(U32 i = 0; i < count; i++) {
        fields[i].container = tuple;
    }

    auto descriptor = (Byte*)module->memory.alloc(descriptorLength);
    copyMem(buffer, descriptor, descriptorLength);
    tuple->descriptor = descriptor;
    tuple->descriptorLength = (U16)descriptorLength;

    module->usedTuples.add(hash, tuple);
    return tuple;
}

Type* canonicalType(Type* type) {
    switch(type->kind) {
        case Type::Alias:
            return canonicalType(((AliasType*)type)->to);
        default:
            return type;
    }
}

Type* rValueType(Type* type) {
    type = canonicalType(type);

    switch(type->kind) {
        case Type::Ptr:
            return ((PtrType*)type)->to;
        default:
            return type;
    }
}

DerivedTypes::DerivedTypes(Module* module, Type *type) :
        ptr(type),
        arrayTo(type) {
    auto arena = module ? &module->memory : nullptr;

    createDescriptor(&ptr, arena);
    createDescriptor(&arrayTo, arena);
}