#include "type.h"
#include "module.h"

static U32 alignTo(U32 value, U32 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static TypePtr errorType(Module& module, LocationId source, StringView message) {
    module.context.diagnostics.error(message, source);
    return module.scalar.error;
}

static TypePtr resolveTupleAst(Context& context, Module& module, const ast::Type& type) {
    auto parseBase = module.parse;
    Array<Field> fields;
    auto astFields = type.tup.fields;

    for(auto astField: astFields.contents(parseBase)) {
        fields.push(Field {
            resolveType(context, module, astField.type),
            astField.name,
            0,
        });
    }

    return (Type*)resolveTupleType(context, module, toBuffer(fields), type.source) - *module.types;
}

TypePtr resolveType(Context& context, Module& module, const ast::Type& type) {
    switch(type.kind) {
        case ast::Type::Error:
            return module.scalar.error;
        case ast::Type::Unit:
            return module.scalar.unit;
        case ast::Type::Con: {
            auto found = module.namedTypes.get(type.name);
            if(found) return found.unwrap();

            context.diagnostics.error("unknown scalar type %@"_v, type.source, context.findName(type.name));
            return module.scalar.error;
        }
        case ast::Type::Tup:
            return resolveTupleAst(context, module, type);
        default:
            return errorType(module, type.source, "type is not available in the aggregate resolver"_v);
    }
}

TupType* resolveTupleType(Context& context, Module& module, Buffer<Field> requested, LocationId source) {
    auto base = *module.types;

    for(auto tuplePointer: module.tupleTypes.contents(base)) {
        auto tuple = base[tuplePointer];
        if(tuple->fields.size() != requested.length) continue;

        auto equal = true;
        for(Size i = 0; i < requested.length; i++) {
            auto existing = tuple->fields.get(base, i);
            if(existing.type != requested[i].type || existing.name != requested[i].name) {
                equal = false;
                break;
            }
        }

        if(equal) return tuple;
    }

    auto tuple = new (module.types) TupType;
    auto named = false;

    for(auto field: requested) {
        named = named || field.name != 0;
        tuple->fields.push(module.types, field);
    }

    tuple->named = named;
    module.tupleTypes.push(module.types, tuple - base);
    finishTupleRepr(context, module, *tuple, source);
    return tuple;
}

bool finishTupleRepr(Context& context, Module& module, TupType& tuple, LocationId source) {
    if(tuple.reprReady) return true;

    if(tuple.resolvingRepr) {
        context.diagnostics.error("recursive tuple representation requires indirection"_v, source);
        return false;
    }

    tuple.resolvingRepr = true;
    auto base = *module.types;
    U32 size = 0;
    U32 alignment = 1;
    auto ready = true;

    for(Size i = 0; i < tuple.fields.size(); i++) {
        auto field = tuple.fields.get(base, i);
        if(field.type && base[field.type]->kind == Type::Record) {
            ready = finishRecordRepr(context, module, *(RecordType*)base[field.type], source) && ready;
        } else if(field.type && base[field.type]->kind == Type::Tup) {
            ready = finishTupleRepr(context, module, *(TupType*)base[field.type], source) && ready;
        }

        if(!ready) continue;

        auto fieldAlign = typeAlign(base, field.type);
        size = alignTo(size, fieldAlign);
        field.offset = size;
        tuple.fields.set(base, i, field);
        size += typeSize(base, field.type);
        alignment = max(alignment, fieldAlign);
    }

    tuple.resolvingRepr = false;
    if(!ready) return false;
    tuple.repr = { alignTo(size, alignment), alignment };
    tuple.virtualSize = U16(tuple.repr.size);
    tuple.reprReady = true;

    return true;
}

bool finishRecordRepr(Context& context, Module& module, RecordType& record, LocationId source) {
    if(record.reprReady) return true;
    if(!record.definitionReady) return false;

    if(record.resolvingRepr) {
        context.diagnostics.error("recursive records require indirection, which is not available in Milestone 2"_v, source);
        record.repr = { 0, 1 };
        return false;
    }

    record.resolvingRepr = true;
    auto base = *module.types;
    auto constructors = record.constructors.contents(base);

    if(constructors.size() == 1) {
        record.layout = RecordType::Single;
        auto content = constructors[0].content;

        if(content && base[content]->kind == Type::Record) {
            if(!finishRecordRepr(context, module, *(RecordType*)base[content], source)) {
                record.resolvingRepr = false;
                return false;
            }
        } else if(content && base[content]->kind == Type::Tup) {
            if(!finishTupleRepr(context, module, *(TupType*)base[content], source)) {
                record.resolvingRepr = false;
                return false;
            }
        }

        record.repr = content ? base[content]->repr : Repr {};
        record.payloadOffset = 0;
    } else {
        U32 payloadSize = 0;
        U32 payloadAlign = 1;
        auto hasPayload = false;

        for(auto constructor: constructors) {
            if(!constructor.content || isUnit(base, constructor.content)) continue;
            if(base[constructor.content]->kind == Type::Record) {
                if(!finishRecordRepr(context, module, *(RecordType*)base[constructor.content], source)) {
                    record.resolvingRepr = false;
                    return false;
                }
            } else if(base[constructor.content]->kind == Type::Tup) {
                if(!finishTupleRepr(context, module, *(TupType*)base[constructor.content], source)) {
                    record.resolvingRepr = false;
                    return false;
                }
            }

            hasPayload = true;
            payloadSize = max(payloadSize, typeSize(base, constructor.content));
            payloadAlign = max(payloadAlign, typeAlign(base, constructor.content));
        }

        record.layout = hasPayload ? RecordType::Multi : RecordType::Enum;
        record.payloadOffset = alignTo(4, payloadAlign);
        record.repr.align = max(4u, payloadAlign);
        record.repr.size = alignTo(record.payloadOffset + payloadSize, record.repr.align);

        if(!hasPayload) record.repr.size = 4;
    }

    record.virtualSize = U16(record.repr.size);
    record.resolvingRepr = false;
    record.reprReady = true;
    return true;
}

bool sameType(TypePtr lhs, TypePtr rhs) {
    return lhs == rhs;
}

bool isUnit(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Unit;
}

bool isBool(GlobalBase base, TypePtr type) {
    if(!type || base[type]->kind != Type::Record) return false;
    return ((RecordType*)base[type])->name == Context::nameHash("Bool", 4);
}

bool isInteger(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Int;
}

bool isFloat(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Float;
}

bool isNumeric(GlobalBase base, TypePtr type) {
    return isInteger(base, type) || isFloat(base, type);
}

bool isDirectType(GlobalBase base, TypePtr type) {
    if(!type || isUnit(base, type)) return false;

    auto value = base[type];
    if(value->kind == Type::Int || value->kind == Type::Float) return true;

    return value->kind == Type::Record && ((RecordType*)value)->layout == RecordType::Enum;
}

bool isMemoryType(GlobalBase base, TypePtr type) {
    return type && !isUnit(base, type) && !isDirectType(base, type);
}

U32 typeSize(GlobalBase base, TypePtr type) {
    return type ? base[type]->repr.size : 0;
}

U32 typeAlign(GlobalBase base, TypePtr type) {
    return type ? base[type]->repr.align : 1;
}

String describeType(Context& context, GlobalBase base, TypePtr type) {
    if(type && base[type]->kind == Type::Record) return context.findName(((RecordType*)base[type])->name);

    auto name = typeName(base, type);
    return String(name.ptr, name.length);
}

StringView typeName(GlobalBase base, TypePtr type) {
    if(!type) return "<none>"_v;

    switch(base[type]->kind) {
        case Type::Error:
            return "<error>"_v;
        case Type::Unit:
            return "Unit"_v;
        case Type::Int: {
            switch(((IntType*)base[type])->width) {
                case IntType::Int: return "Int"_v;
                case IntType::Long: return "Long"_v;
                default: break;
            }
            break;
        }
        case Type::Float:
            return ((FloatType*)base[type])->width == FloatType::Float ? "Float"_v : "Double"_v;
        case Type::Tup:
            return "{...}"_v;
        case Type::Record: {
            auto name = ((RecordType*)base[type])->name;
            // Type names are interned in Context, but this helper intentionally has no
            // Context parameter. Builtin names are the only ones needed without one.
            if(name == Context::nameHash("Bool", 4)) return "Bool"_v;
            return "<record>"_v;
        }
        default:
            break;
    }

    return "<unsupported>"_v;
}
