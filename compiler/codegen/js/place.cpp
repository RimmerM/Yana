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

            if(isBool(g, value.type)) return number(g, bits ? 1 : 0);

            if(auto integer = intType(g, value.type)) {
                if(isLong(g, value.type)) return bigInt(g, bits, integer->isSigned);

                /*
                 * Read back at the type's own width rather than at 32.
                 *
                 * The constant arrives as a raw bit pattern, so producing the `number` that denotes
                 * it is a sign extension from bit `bits - 1` for a signed type and a mask for an
                 * unsigned one. `I32`/`U32` did both at once for the 32-bit tower and are exactly
                 * wrong for the 33-to-53-bit one, where the value does not fit a 32-bit integer.
                 */
                auto width = integer->bits;
                auto mask = width >= 64 ? ~U64(0) : (U64(1) << width) - 1;
                auto masked = bits & mask;

                if(integer->isSigned && width < 64 && (masked & (U64(1) << (width - 1)))) {
                    return number(g, -F64((mask - masked) + 1));
                }

                return number(g, F64(masked));
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
 * The parts of a narrow reference, from whichever of the two forms it is in.
 *
 * A flattened one has them in variables and there is no object to read; anything else is the
 * `{$o,$k,$s}` object and they are its properties. Every consumer goes through here rather than
 * projecting the object itself, which is what keeps flattening a decision about the *convention*
 * rather than something each use has to know about.
 */
RefParts refPartsOfExpr(Gen& g, JsPtr<Expr> reference) {
    RefParts parts;
    parts.owner = field(g, reference, g.refObject);
    parts.key = field(g, reference, g.refKey);
    if(narrowRefCarriesShift(g)) parts.shift = field(g, reference, g.refShift);

    return parts;
}

RefParts refPartsOf(Gen& g, ModulePtr<Value> reference) {
    if(auto found = g.flatRefs.get(U32(reference))) return found.unwrap();
    return refPartsOfExpr(g, useValue(g, reference));
}

// The object form, for the uses flattening cannot cover: JS has no multi-value return, so a
// reference that is returned, stored or captured has to become one value again.
JsPtr<Expr> materializeRef(Gen& g, RefParts parts) {
    auto pair = make<ObjectExpr>(g);
    pair->properties.push(g.file.arena, Property { g.refObject, parts.owner });
    pair->properties.push(g.file.arena, Property { g.refKey, parts.key });
    if(parts.shift) pair->properties.push(g.file.arena, Property { g.refShift, parts.shift });

    return asExpr(g, pair);
}

/*
 * The walk both placeExpr and placeType are.
 *
 * The property chain is built only for a caller that asked for one, but the walk itself is the same
 * either way and deliberately so: the two answers have to agree about which projection is a property
 * and which is free, and a second copy of this loop is exactly where they would stop agreeing.
 */
namespace {

TypePtr walkPlace(Gen& g, const Place& place, JsPtr<Expr>* expr, Size limit = maxLimit<Size>,
                  PlaceBits* bits = nullptr) {
    TypePtr type = nullptr;
    PlaceBits within;

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
            /*
             * A reference to a narrow value is the (object, property, shift) triple a place into a
             * bit range is - the pair reified, plus where inside the named word the value starts.
             *
             * The shift is what makes one compiled body serve a field of a scalarized record, a
             * co-packed field and a whole local: the callee has only the pointee type, so the *mask*
             * is a constant it can compute and the *shift* is the one thing it cannot. See
             * genNarrowRef, which is the other half.
             *
             * Asked before the value itself, because a flattened reference *has* no single value -
             * its parts are three variables and there is no object anywhere to ask `useValue` for.
             */
            if(isNarrowJsValue(g, type)) {
                auto parts = refPartsOf(g, place.pointer);
                *expr = elementAt(g, parts.owner, parts.key);

                if(parts.shift) {
                    within.shift = parts.shift;
                    within.width = narrowWidth(g, type);
                }
            } else {
                *expr = useValue(g, place.pointer);

                if(!isJsObject(g, type) && !g.aliasBorrows.contains(U32(place.pointer))) {
                    // An alias is the storage under a second name, so there is no box to read
                    // through - see prepareLocals. Everything else that is not an object arrived
                    // as one.
                    *expr = field(g, *expr, g.boxField);
                }
            }
        }
    } else if(place.local < g.function->localCount()) {
        auto root = g.function->localAt(g.local, place.local);
        type = root.type;

        if(expr) {
            // The slot behind a `&` parameter of narrow type holds one of those triples rather than
            // storage of its own - and holds it as three variables where it arrived flattened, which
            // is why this is asked before the value is.
            if(root.borrowed && isNarrowJsValue(g, type)) {
                auto parts = refPartsOf(g, root.value);
                *expr = elementAt(g, parts.owner, parts.key);

                if(parts.shift) {
                    within.shift = parts.shift;
                    within.width = narrowWidth(g, type);
                }
            } else {
                *expr = useValue(g, root.value);
                if(place.local < g.boxed.size() && g.boxed[place.local]) {
                    *expr = field(g, *expr, g.boxField);
                }
            }
        }
    } else {
        if(expr) *expr = nullValue(g);
        return nullptr;
    }

    auto projections = place.projections;
    Size walked = 0;

    for(auto projection: projections.contents(g.local)) {
        // `limit` stops before the trailing Property projection, which is how the *owner* of a
        // constrained field is asked for: the field is reached by calling the witness with that
        // owner rather than by naming a property of it. See propertySlotOf in inst.cpp.
        if(walked++ >= limit) break;

        switch(projection.kind) {
            case ProjectionKind::Discriminant: {
                // An enum *is* its discriminant, so there is nothing to project out of it.
                auto record = recordType(g, type);
                if(record && record->layout != RecordType::Enum) {
                    // Neither is a folded record, for the stronger reason: its tag is not stored
                    // anywhere at all. The place stays on the payload and the load and the store
                    // intercept - see PlaceBits::foldedTag.
                    if(g.repr.of(type).isNicheFolded()) {
                        within.foldedTag = type;
                    } else if(expr) {
                        *expr = field(g, *expr, g.tagField);
                    }
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
                 * Anything else is one property, since a bare payload has no field names to flatten -
                 * and neither does a tuple payload the Repr made one number, which is one value and
                 * therefore one property however many fields went into it.
                 *
                 * Free in the strongest sense for a folded record, which *is* its payload: there is
                 * nothing anywhere to read, since what the walk has in hand already is the payload or
                 * the pattern that says there is none. Native spends a payload offset of zero here for
                 * exactly the same reason.
                 */
                if(expr && !g.repr.of(type).isNicheFolded() &&
                   record->layout == RecordType::Multi && content &&
                   !isUnit(g.global, content) &&
                   (g.global[content]->kind != Type::Tup || !isJsObject(g, content))) {
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

                    /*
                     * A field of a record the Repr made one scalar. There is no property to descend
                     * into - the owner *is* the value - so the walk stays where it is and accumulates
                     * where inside that number this field sits.
                     *
                     * The accumulation is the part with teeth. `t.f.a` reaches here twice: once for
                     * `f`, two bits at bit zero or two of `Two`, and once for `a`, one bit at bit
                     * zero of `Flags`. Neither offset is written anywhere and only their sum names
                     * the bit, which is why a walk that reported the last projection's offset would
                     * read a neighbour and still produce a value of the right type.
                     */
                    if(!isJsObject(g, type)) {
                        if(auto placed = g.repr.fieldOf(type, projection.index)) {
                            within.offset += placed->bitOffset;
                            within.width = placed->bitWidth ? placed->bitWidth
                                                            : g.repr.of(entry.type).scalarBits;
                            type = entry.type;
                            break;
                        }
                    }

                    /*
                     * A field of a record that stayed an object, which is a property - and, where this
                     * target co-packed it, a bit range of one shared with its neighbours.
                     *
                     * The bit range *replaces* whatever the walk had accumulated rather than adding to
                     * it, because descending into a property is descending into a different value. It
                     * can only be empty here in any case: reaching an object-shaped tuple means
                     * nothing before it was a bit range, since a scalarized record holds only narrow
                     * fields and an object is not one.
                     */
                    auto property = fieldProperty(g, type, projection.index);
                    if(expr) *expr = field(g, *expr, property.name);

                    within = PlaceBits {};
                    if(property.isPacked()) {
                        within.offset = property.bitOffset;
                        within.width = property.bitWidth;
                    }

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

    if(bits) *bits = within;
    return type;
}

} // namespace

/*
 * Reading a bit range out of the number that holds it.
 *
 * `>>>` and `&` rather than anything wider, which is the whole of why the JS packing budget is 32
 * bits and not the 53 a `number` holds: above 32 these stop being the operations JS has.
 *
 * Nothing needs converting on the way out, and that is the point of `Bool` being 0 or 1 rather than a
 * host boolean: an enum, a `@bits` integer, a nested scalar record and a `Bool` are all numbers, so
 * the bits *are* the value and this is the same two operations for every one of them.
 */
// The total shift: the constant offsets this body walked, plus whatever a reference brought with it.
static JsPtr<Expr> shiftOf(Gen& g, PlaceBits bits) {
    if(!bits.shift) return bits.offset ? number(g, F64(bits.offset)) : nullptr;
    if(!bits.offset) return bits.shift;
    return binary(g, BinaryOp::Add, bits.shift, number(g, F64(bits.offset)));
}

JsPtr<Expr> decodeBits(Gen& g, JsPtr<Expr> owner, PlaceBits bits, TypePtr type) {
    auto value = owner;
    if(auto shift = shiftOf(g, bits)) value = binary(g, BinaryOp::Shr, value, shift);

    if(bits.width >= 32) return binary(g, BinaryOp::And, value, number(g, F64(~U32(0))));

    /*
     * A signed field widens by *sign*-extension, which masking cannot do: `& 15` on a `@bits(4) I32`
     * holding -4 answers 12, and the field's four bits are all the information there is to tell the
     * two apart. Shifting the field's top bit up to bit 31 and arithmetically back down truncates
     * and sign-extends in the same pair - the shape `decodePackedField` uses natively, and the
     * reason the two targets agreed on everything here except the sign.
     */
    auto integer = intType(g, type);
    if(integer && integer->isSigned) {
        auto distance = number(g, F64(32 - bits.width));
        return binary(g, BinaryOp::Sar, binary(g, BinaryOp::Shl, value, distance), distance);
    }

    return binary(g, BinaryOp::And, value, number(g, F64((U32(1) << bits.width) - 1)));
}

/*
 * Putting one back, which is a read-modify-write because the neighbours share the word.
 *
 * `(owner & ~(mask << shift)) | ((value & mask) << shift)`. The inner mask is not redundant: a
 * `@bits(4)` value that arrived wider would otherwise write over the field above it, and a signed one
 * arrives sign-extended, so its high bits are ones exactly when they must not be stored.
 */
JsPtr<Expr> encodeBits(Gen& g, JsPtr<Expr> owner, PlaceBits bits, TypePtr, JsPtr<Expr> value) {
    auto mask = bits.width >= 32 ? ~U32(0) : (U32(1) << bits.width) - 1;

    value = binary(g, BinaryOp::And, value, number(g, F64(mask)));

    auto shift = shiftOf(g, bits);
    if(shift) value = binary(g, BinaryOp::Shl, value, shift);

    // The hole the field occupies. Constant where nothing brought a shift, and computed where a
    // reference did - a callee cannot fold `~(mask << r.$s)` and has to build it.
    auto hole = shift ? unary(g, UnaryOp::BitNot,
                              binary(g, BinaryOp::Shl, number(g, F64(mask)), shift))
                      : number(g, F64(U32(~mask)));

    return binary(g, BinaryOp::Or, binary(g, BinaryOp::And, owner, hole), value);
}

TypePtr foldedPayload(Gen& g, TypePtr record) {
    auto& repr = g.repr.of(record);
    auto value = recordType(g, record);
    if(!value || !repr.isNicheFolded()) return nullptr;

    return value->constructors.get(g.global, repr.encoding.payloadConstructor).content;
}

/*
 * Reading a folded tag: which constructor this value is.
 *
 * Two shapes, from the two kinds of niche, and they are the same sentence about different things. An
 * absent niche is `null`, so the test is `v === null` and there is nothing else it could be - `fits`
 * admitted exactly one non-payload constructor. A pattern niche is a range the payload's own bits
 * cannot leave, so the test is a comparison, and it is a comparison of a `number` because that is the
 * only kind of value a pattern niche is ever donated by on this target - see ReprTable::hostNiche.
 *
 * Which makes this the same select native emits rather than a branch, and for the same reason: a
 * folded `Maybe` is meant to be cheaper than the tag word it replaced and not merely smaller.
 */
JsPtr<Expr> decodeNicheTag(Gen& g, JsPtr<Expr> value, TypePtr record) {
    auto& repr = g.repr.of(record);
    auto& encoding = repr.encoding;
    auto& niche = encoding.niche;

    auto payloadIndex = U64(encoding.payloadConstructor);
    auto payload = number(g, F64(payloadIndex));

    if(niche.isAbsent()) {
        auto other = number(g, F64(payloadIndex == 0 ? 1 : 0));
        return ternary(g, binary(g, BinaryOp::Eq, value, nullValue(g)), other, payload);
    }

    /*
     * The word is read up to three times below, so anything that is not already a name gets one.
     * `useValue` hands back a variable for most places a scrutinee comes from, and the declaration is
     * what keeps a property chain from being walked once per comparison.
     */
    auto kind = g.base[value]->kind;
    if(kind != Expr::Var && kind != Expr::Number && g.body) {
        value = declare(g, generatedName(g, "tag"_v, g.labelCounter++), value);
    }

    // `v >= validStart && v <= validEnd`, with the first half gone for the usual niche, whose valid
    // range starts at zero. Ordinary number comparisons: every pattern this target folds into is a
    // small integer, so there is nothing to do about wrapping.
    JsPtr<Expr> inRange = binary(g, BinaryOp::Le, value, number(g, F64(niche.validEnd)));
    if(niche.validStart) {
        inRange = binary(g, BinaryOp::LogicalAnd,
                         binary(g, BinaryOp::Ge, value, number(g, F64(niche.validStart))), inRange);
    }

    auto constructors = recordType(g, record)->constructors.size();

    // Two constructors is the shape this exists for, and there the pattern carries no information
    // beyond "not the payload one". No arithmetic, then: one of two constants.
    if(constructors == 2) {
        return ternary(g, inRange, payload, number(g, F64(payloadIndex == 0 ? 1 : 0)));
    }

    /*
     * More than two, so which pattern it is decides which constructor it is. The patterns were handed
     * out to the non-payload constructors in index order, so recovering the ordinal recovers the
     * index - except that the payload constructor is missing from that sequence, which the last step
     * puts back.
     */
    auto first = number(g, F64(encoding.firstPattern));
    auto ordinal = encoding.ascending ? binary(g, BinaryOp::Sub, value, first)
                                      : binary(g, BinaryOp::Sub, first, value);

    auto name = generatedName(g, "ord"_v, g.labelCounter++);
    auto bound = g.body ? declare(g, name, ordinal) : ordinal;

    // `ordinal >= payloadConstructor` means this constructor was written after the payload one, so its
    // index is one higher than its position in the pattern sequence.
    auto shifted = binary(g, BinaryOp::Ge, bound, number(g, F64(payloadIndex)));
    auto adjusted = ternary(g, shifted, binary(g, BinaryOp::Add, bound, number(g, 1)), bound);

    return ternary(g, inRange, payload, adjusted);
}

/*
 * Writing one, which for the payload constructor is writing nothing at all.
 *
 * That is not an optimization but the definition: the payload constructor *is* the payload's own
 * value, so the only thing that could make it identifiable is the payload being written, which the
 * constructor's own field initializations do. Every other constructor has no payload to write, so its
 * pattern is the whole value.
 */
void encodeNicheTag(Gen& g, JsPtr<Expr> target, TypePtr record, U64 constructor) {
    auto& encoding = g.repr.of(record).encoding;
    if(constructor == encoding.payloadConstructor) return;

    auto pattern = encoding.niche.isAbsent() ? nullValue(g)
                                             : number(g, F64(encoding.patternOf(U16(constructor))));

    emitExpr(g, assign(g, target, pattern));
}

JsPtr<Expr> placeExpr(Gen& g, const Place& place, Size limit) {
    JsPtr<Expr> expr = nullptr;
    PlaceBits bits;
    auto type = walkPlace(g, place, &expr, limit, &bits);

    // Every reader wants the value rather than the word it sits in, so the decode is applied here and
    // the one caller that cannot use it - the store below - asks placeOwner instead.
    if(bits.foldedTag) return decodeNicheTag(g, expr, bits.foldedTag);
    if(bits.valid()) return decodeBits(g, expr, bits, type);
    return expr;
}

JsPtr<Expr> placeOwner(Gen& g, const Place& place, PlaceBits& bits, Size limit) {
    JsPtr<Expr> expr = nullptr;
    walkPlace(g, place, &expr, limit, &bits);
    return expr;
}

// What a place holds. Most callers want one or the other rather than both, so this skips building
// the chain rather than building one nobody reads.
TypePtr placeType(Gen& g, const Place& place, Size limit) {
    return walkPlace(g, place, nullptr, limit);
}

JsPtr<Expr> referenceTo(Gen& g, TypePtr type, JsPtr<Expr> value) {
    if(type && !isJsObject(g, type)) return boxOf(g, value);
    return value;
}

JsPtr<Expr> referenceTo(Gen& g, const Place& place, Size limit) {
    JsPtr<Expr> expr = nullptr;
    auto type = walkPlace(g, place, &expr, limit);
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
