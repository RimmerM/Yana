#include "build.h"

/*
 * Values and places.
 *
 * The whole of the difference between this backend and the native one is here: a place stays a
 * `(object, property)` chain instead of becoming a base address plus a constant offset. Every
 * projection is a property, an array index, or - for a Downcast, which native spends a payload
 * offset on - nothing at all.
 *
 * The erased half is at the bottom, and is the one place this backend reads a *layout* rather than a
 * structure: a compiler-built constant table is an array of 32-bit cells, so the load native writes
 * as `[base + 24]` is `table[6]` here.
 */

namespace js {

JsPtr<Expr> constantValue(Gen& g, Value& value) {
    switch(value.kind) {
        case Value::ConstInt: {
            auto bits = ((ConstInt&)value).value;

            if(isBool(g, value.type)) return boolean(g, bits != 0);

            if(auto integer = intType(g, value.type)) {
                if(integer->width == IntType::Long) return bigInt(g, bits, integer->isSigned);

                if(integer->isSigned) return number(g, F64(I64(I32(U32(bits)))));
                return number(g, F64(U32(bits)));
            }

            // A null pointer written as an integer, which is how `cast(0)` reaches here. Nothing
            // else can be a raw address at compile time.
            if(value.type && g.global[value.type]->kind == Type::Ptr) {
                return bits ? number(g, F64(bits)) : nullValue(g);
            }

            return number(g, F64(bits));
        }
        case Value::ConstFloat:
            return number(g, F64(((ConstFloat&)value).value), false);
        case Value::ConstDouble:
            return number(g, ((ConstDouble&)value).value, false);
        default:
            return nullValue(g);
    }
}

JsPtr<Expr> useValue(Gen& g, ModulePtr<Value> pointer) {
    if(!pointer) return nullValue(g);
    if(auto found = g.values.get(U32(pointer))) return found.unwrap();

    auto& value = *g.local[pointer];
    if(isConstant(value)) {
        auto result = constantValue(g, value);
        g.values.add(U32(pointer), result);
        return result;
    }

    g.context.diagnostics.error("internal error: a resolve value was used before it was generated"_v,
                                value.source);
    return nullValue(g);
}

JsPtr<Expr> globalValue(Gen& g, ModulePtr<Global> pointer) {
    auto found = g.globalNames.get(U32(pointer));
    return variable(g, found ? found.unwrap() : Name {});
}

/*
 * The walk both placeExpr and placeType are.
 *
 * The property chain is built only for a caller that asked for one, but the walk itself is the same
 * either way and deliberately so: the two answers have to agree about which projection is a property
 * and which is free, and a second copy of this loop is exactly where they would stop agreeing.
 */
namespace {

TypePtr walkPlace(Gen& g, const Place& place, JsPtr<Expr>* expr) {
    TypePtr type = nullptr;

    if(place.root == PlaceRoot::Global) {
        type = g.local[place.global]->type;
        if(expr) *expr = globalValue(g, place.global);
    } else if(place.root == PlaceRoot::Pointer || place.root == PlaceRoot::Borrow) {
        // A borrow and a raw pointer are the same reference with different amounts of knowledge
        // behind them, and neither has a representation of its own here: it is the object it names,
        // or the box that stands in for one.
        auto referenced = g.local[place.pointer]->type;

        type = place.root == PlaceRoot::Borrow
            ? ((BorrowType*)g.global[referenced])->to
            : pointeeType(g.global, referenced);

        if(expr) {
            *expr = useValue(g, place.pointer);

            // An alias is the storage under a second name, so there is no box to read through -
            // see prepareLocals. Everything else that is not an object arrived as one.
            if(!isJsObject(g, type) && !g.aliasBorrows.contains(U32(place.pointer))) {
                *expr = field(g, *expr, g.boxField);
            }
        }
    } else if(place.local < g.function->localCount()) {
        auto root = g.function->localAt(g.local, place.local);
        type = root.type;

        if(expr) {
            *expr = useValue(g, root.value);

            if(place.local < g.boxed.size() && g.boxed[place.local]) {
                *expr = field(g, *expr, g.boxField);
            }
        }
    } else {
        if(expr) *expr = nullValue(g);
        return nullptr;
    }

    auto projections = place.projections;

    for(auto projection: projections.contents(g.local)) {
        switch(projection.kind) {
            case ProjectionKind::Discriminant: {
                // An enum *is* its discriminant, so there is nothing to project out of it.
                auto record = recordType(g, type);
                if(expr && record && record->layout != RecordType::Enum) {
                    *expr = field(g, *expr, g.tagField);
                }

                type = g.program.scalar.int_;
                break;
            }
            case ProjectionKind::Downcast: {
                auto record = recordType(g, type);
                if(!record) break;

                auto content = record->constructors.get(g.global, projection.index).content;

                /*
                 * Free, like the native offset it corresponds to: a tuple payload is flattened into
                 * the record's own object, so the constructor's fields are already properties of it.
                 * Anything else is one property, since a bare payload has no field names to flatten.
                 */
                if(expr && record->layout == RecordType::Multi && content &&
                   !isUnit(g.global, content) && g.global[content]->kind != Type::Tup) {
                    *expr = field(g, *expr, g.payloadField);
                }

                type = content;
                break;
            }
            case ProjectionKind::Field: {
                if(!type) break;

                /*
                 * A closure header is a compiler-built table here, exactly as it is bytes there, so
                 * a field of it is a cell rather than a property. Recognized by its type, because
                 * that is what makes it this table rather than an ordinary tuple that happens to
                 * have two addresses in it - see closureHeaderPlaceType.
                 */
                if(type == g.headerType) {
                    auto entry = g.repr.fieldOf(type, projection.index);
                    if(!entry) break;
                    if(expr) *expr = tableCell(g, *expr, entry->offset);
                    type = entry->type;
                    break;
                }

                /*
                 * The two words of a function value, which are not two properties here.
                 *
                 * The code word *is* the value: a function value is a host function, so what a
                 * native reader would load out of the first word is the thing it would have loaded
                 * it from. The environment is what the closure closed over, and it is reachable at
                 * all only where something attached it - see genFunValueWord.
                 */
                if(g.global[type]->kind == Type::Fun) {
                    if(expr && projection.index == FunValueLayout::kEnv) {
                        *expr = field(g, *expr, g.envField);
                    } else if(expr && projection.index == FunValueLayout::kHeader) {
                        *expr = field(g, *expr, g.headerField);
                    }

                    type = funValueFieldType(*g.program.core, projection.index);
                    break;
                }

                if(g.global[type]->kind == Type::Tup) {
                    auto entry = ((TupType*)g.global[type])->fields.get(g.global, projection.index);
                    if(expr) *expr = field(g, *expr, fieldName(g, entry.name, projection.index));
                    type = entry.type;
                }

                break;
            }
            case ProjectionKind::Deref:
                // The reference stored here becomes what the rest of the path is relative to.
                type = pointeeType(g.global, type);
                if(expr && type && !isJsObject(g, type)) *expr = field(g, *expr, g.boxField);
                break;
            case ProjectionKind::Index:
                if(expr) *expr = elementAt(g, *expr, useValue(g, projection.value));
                break;
            default:
                break;
        }
    }

    return type;
}

} // namespace

JsPtr<Expr> placeExpr(Gen& g, const Place& place) {
    JsPtr<Expr> expr = nullptr;
    walkPlace(g, place, &expr);
    return expr;
}

// What a place holds. Most callers want one or the other rather than both, so this skips building
// the chain rather than building one nobody reads.
TypePtr placeType(Gen& g, const Place& place) {
    return walkPlace(g, place, nullptr);
}

JsPtr<Expr> referenceTo(Gen& g, TypePtr type, JsPtr<Expr> value) {
    if(type && !isJsObject(g, type)) return boxOf(g, value);
    return value;
}

JsPtr<Expr> referenceTo(Gen& g, const Place& place) {
    JsPtr<Expr> expr = nullptr;
    auto type = walkPlace(g, place, &expr);
    return referenceTo(g, type, expr);
}

/*
 * The erased half - Implementation-Generics.md, read through §3.4.
 */

/*
 * One slot of a compiler-built table, which here is one element of an array.
 *
 * This is the whole of the JS materialization of resolve/witness.h's tables, and it is this short
 * because a slot number is already an index. There is nothing to divide by and nothing to skip: a
 * host array has no padding, and an address is a name rather than eight bytes. Reading a native
 * blob back at native offsets - which is what this used to do - made every table here a
 * transcription of an x64 memory image, with a null in every second cell where the high half of a
 * pointer would have been.
 */
JsPtr<Expr> tableCell(Gen& g, JsPtr<Expr> table, U16 slot) {
    return index(g, table, slot);
}

JsPtr<Expr> genSlot(Gen& g, U16 slot) {
    return tableCell(g, g.genEnv, GenEnvFields::slot(slot));
}

JsPtr<Expr> genWitness(Gen& g, U16 slot, ModuleList<U32, false> path) {
    auto witness = genSlot(g, slot);
    for(auto step: path.contents(g.local)) witness = tableCell(g, witness, U16(step));

    return witness;
}

JsPtr<Expr> genTypeDesc(Gen& g, TypePtr type) {
    if(!g.genEnv || !type || !isGeneric(g.global, type)) return nullptr;

    auto slot = genTypeSlot(*g.genModule, *g.genContext, type);
    if(slot == maxLimit<U16>) return nullptr;

    return genSlot(g, slot);
}

} // namespace js
