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

void createDescriptor(Type* type, Arena* arena);

auto descriptorBuilder = [] -> bool {
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
    put16(buffer, max, (U16)type->argCount);
    for(U32 i = 0; i < type->argCount; i++) {
        buffer = describeType(type->args[i].type, buffer, max);
    }

    buffer = describeType(type->result, buffer, max);
    return buffer;
}

Byte* describeTupType(TupType* type, Byte* buffer, Byte* max, bool writeNames) {
    put16(buffer, max, (U16)type->count);
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

template<class I>
TupLookup* findTupLayout(Module* module, TupLookup* lookup, I fields) {
    if(fields.has()) {
        auto type = fields.get(module);

        // If there already was a table for this type, continue in that one.
        if(auto t = lookup->next.get((Size)type)) {
            return findTupLayout(module, t, fields.next());
        }

        // Otherwise, create a new table first.
        auto next = &lookup->next[(Size)type];
        auto depth = lookup->depth + 1;
        auto layout = (Type**)module->memory.alloc(sizeof(Type*) * depth);
        next->depth = depth;
        next->layout = layout;
        next->virtualSize = lookup->virtualSize + type->virtualSize;

        memcpy(layout, lookup->layout, (depth - 1) * sizeof(Type*));
        layout[depth - 1] = type;

        return findTupLayout(module, next, fields.next());
    } else {
        return lookup;
    }
}

TupLookup* findTupLayout(Context* context, Module* module, Value** fields, U32 count) {
    struct Iterator {
        Context* context;
        Value** fields;
        Value** max;

        bool has() {
            return fields < max;
        }

        Type* get(Module* m) {
            return fields[0]->type;
        }

        Iterator next() {
            return {context, fields + 1, max};
        }
    };

    return findTupLayout(module, &module->usedTuples, Iterator{context, fields, fields + count});
}

static Type* findTuple(Context* context, Module* module, ast::TupType* type) {
    struct Iterator {
        Context* context;
        List<ast::TupField>* current;

        bool has() {
            return current != nullptr;
        }

        Type* get(Module* m) {
            auto ast = current->item.type;
            return resolveType(context, m, ast);
        }

        Iterator next() {
            return {context, current->next};
        }
    };

    auto layout = findTupLayout(module, &module->usedTuples, Iterator{context, type->fields});
    auto count = layout->depth;
    auto fields = (Field*)module->memory.alloc(sizeof(Field) * count);
    auto tuple = new (module->memory) TupType(layout->virtualSize);
    tuple->count = count;
    tuple->layout = layout->layout;
    tuple->fields = fields;

    auto f = type->fields;
    for(U32 i = 0; i < count; i++) {
        fields[i].type = layout->layout[i];
        fields[i].container = tuple;
        fields[i].name = f->item.name;
        fields[i].index = i;
        f = f->next;
    }

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
            return getRef(module, content, false, false, true);
        }
        case ast::Type::Ref: {
            auto ast = (ast::RefType*)type;
            auto content = resolveType(context, module, ast->type);
            return getRef(module, content, true, false, true);
        }
        case ast::Type::Val: {
            auto ast = (ast::ValType*)type;
            return resolveType(context, module, ast->type);
        }
        case ast::Type::Tup:
            return findTuple(context, module, (ast::TupType*)type);
        case ast::Type::Gen:
            return nullptr;
        case ast::Type::App:
            return nullptr;
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
            return new (module->memory) MapType(from, to);
        }
    }
}

void resolveAlias(Context* context, Module* module, AliasType* type) {
    auto ast = type->ast;
    if(ast) {
        type->ast = nullptr;
        type->to = findType(context, module, ast->target);
        type->virtualSize = type->to->virtualSize;
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
            if(conAst->item.content) {
                auto content = findType(context, module, conAst->item.content);
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

Type* resolveType(Context* context, Module* module, ast::Type* type) {
    auto found = findType(context, module, type);
    if(
        (found->kind == Type::Alias && ((AliasType*)found)->genCount > 0) ||
        (found->kind == Type::Record && ((RecordType*)found)->genCount > 0)
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