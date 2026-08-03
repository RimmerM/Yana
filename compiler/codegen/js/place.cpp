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
        case Value::ConstString:
            // A string literal is a host string constant here and nothing else - see ConstString.
            // The escaping is `writeStringLiteral`'s, which is the same one every property name and
            // program constant already goes through.
            return asExpr(g, make<StringExpr>(g, ((ConstString&)value).text));
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

    /*
     * A value of unit type, which is nothing and is named by nothing.
     *
     * `define` binds no variable for one - there is nothing to hold - so anything that reads one
     * arrives here having found no entry. That is not the failure below: it is the model working,
     * and `null` is this target's spelling of a value with no representation. The positions where a
     * unit *matters* leave it out before asking - a parameter that does not exist, a `return` with
     * nothing to return - so what reaches here is a use in a position that will discard it anyway.
     *
     * Reached by a generic body specialized at `{}`: `match m: Just(inner) -> inner` binds a payload
     * that occupies nothing, and the arm has to produce it.
     */
    if(isUnit(g.global, value.type)) return nullValue(g);

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
    if(narrowRefCarriesScale(g)) parts.scale = field(g, reference, g.refScale);

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
    if(parts.scale) pair->properties.push(g.file.arena, Property { g.refScale, parts.scale });

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

    /*
     * Whether this reference names a *run* of values rather than one.
     *
     * `Place::atPointer(p)` with no path is the single value `p` names, which on this target is the
     * box that stands in for one where the value is not an object. The same root with an `Index` in
     * front of it is element `i` of what `p` names - Implementation-Containers.md §14.1 - and there
     * is no box anywhere in that: `p` is the host array and the index reaches into it.
     *
     * Read off the *path* rather than off the type, because the type is the element's either way.
     * That is the same distinction native draws by adding a scaled offset to the address instead of
     * loading through it.
     *
     * Asked of the whole path rather than of the prefix `limit` selects, because it is a fact about
     * what the *root* names: a caller walking as far as the index alone - which is what builds the
     * `$o` half of a reference to an element - still wants the array rather than a box of it.
     */
    auto indexedRoot = [&]() {
        auto projections = place.projections;
        if(!projections.size()) return false;

        return projections.get(g.local, 0).kind == ProjectionKind::Index;
    }();

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
            /*
             * A raw pointer is asked as though it were immutable, which keeps the two roots'
             * answers where they were: a `%a` is not a borrow and carries no mutability, and what a
             * non-indexed one names on this target is a box exactly as it always did. An *indexed*
             * one is neither - it is the host array itself, and the projection below reaches into
             * it - which is what `indexedRoot` takes out of both branches.
             */
            auto mut = place.root == PlaceRoot::Borrow && ((BorrowType*)g.global[referenced])->mut;

            if(!indexedRoot && refIsTriple(g, type, mut)) {
                auto parts = refPartsOf(g, place.pointer);
                *expr = elementAt(g, parts.owner, parts.key);

                // Only a narrow pointee is a bit range. A whole value occupies what it names, so the
                // scale it was handed is one and there is nothing to shift out of anything.
                if(parts.scale && isNarrowValue(g.global, type)) {
                    within.scale = parts.scale;
                    within.width = narrowWidth(g, type);
                    within.word = maxWordBits(g);
                }
            } else {
                *expr = useValue(g, place.pointer);

                if(!indexedRoot && !isJsObject(g, type) && !g.aliasBorrows.contains(U32(place.pointer))) {
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
            // The triple is the *mutable* `&`'s form, which is the one `refIsFlattened` sends as
            // three arguments. An immutable one arrives as a box or as the value itself, and both of
            // those are read below.
            if(root.borrowed && root.convention == ast::BindType::Ref && refIsTriple(g, type, true)) {
                auto parts = refPartsOf(g, root.value);
                *expr = elementAt(g, parts.owner, parts.key);

                if(parts.scale && isNarrowValue(g.global, type)) {
                    within.scale = parts.scale;
                    within.width = narrowWidth(g, type);
                    within.word = maxWordBits(g);
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

                // A boxed payload holds a reference to its content rather than the content, so what
                // the walk has now is a `%content` and the Deref that follows is what reaches the
                // value. Same statement placeType makes in resolve - see Constructor::boxed.
                if(record->constructors.get(g.global, projection.index).boxed) {
                    type = resolvePointerType(*g.program.core, type);
                }

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
                            // The *outermost* scalar is the word, so this is recorded on the way in
                            // and left alone on the way further down: `t.f.a` lives in `t`'s number
                            // however narrow `Flags` happens to be, and it is `t`'s width that says
                            // whether the host's 32-bit operators can reach the bit.
                            if(!within.width) within.word = g.repr.of(type).scalarBits;

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
                        within.word = property.wordBits;
                    }

                    // Likewise for a boxed field: the property holds a reference. A boxed field is
                    // never packed and never inside a scalarized record, so this is the only one of
                    // the branches above it can reach - see packCandidate and scalarBits.
                    type = entry.boxed ? resolvePointerType(*g.program.core, entry.type) : entry.type;
                }

                break;
            }
            case ProjectionKind::Deref:
                // The reference stored here becomes what the rest of the path is relative to.
                type = pointeeType(g.global, type);
                if(expr && type && !isJsObject(g, type)) *expr = field(g, *expr, g.boxField);
                break;
            case ProjectionKind::Index:
                /*
                 * `owner[i]` - and this target is the one where that is the *whole* of an element
                 * access, since a host array is indexed rather than addressed
                 * (Implementation-Containers.md §14.1). The native side spends a stride and an add
                 * here; there is nothing to spend one on.
                 *
                 * The type follows the same rule resolve's own walk states: a `[T *n]` steps *into*
                 * the array and everything else - a run of elements reached through a reference -
                 * steps *along* it and is already the element's type.
                 */
                if(expr) *expr = elementAt(g, *expr, useValue(g, projection.value));
                if(type && g.global[type]->kind == Type::Array) type = ((ArrayType*)g.global[type])->content;
                break;
            case ProjectionKind::Unit:
                /*
                 * The whole word rather than the field the walk just entered - see
                 * ProjectionKind::Unit. The expression is already the right one, because a packed
                 * field is a bit range *of* the value the walk is holding rather than a property of
                 * it, so all this does is drop the range.
                 *
                 * Which turns the place back into a location, and that is the point: `bits.valid()`
                 * is what the load and the store branch on, so both of them take the plain path and
                 * the arithmetic the expansion emitted is what carries the shift and the mask.
                 */
                within = PlaceBits {};
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
 * Reading a bit range out of the number that holds it, and putting one back.
 *
 * Two forms, and which one a site uses is decided by `PlaceBits::word` rather than by the field:
 *
 *  - **at 32 bits and below**, `>>>`, `&`, `<<` and `|` - the operators JS actually has. This is the
 *    whole of what a packed word was until the budget moved, and it is left exactly as it was.
 *  - **above 32**, those stop working. A shift count is masked to five bits, so `mask << 32` is
 *    `mask` and a word of 40 bits would clear its own low field while writing its high one. What
 *    replaces them is division and multiplication by a power of two, which are exact on a double for
 *    every width a `number` holds, plus a 32-bit mask of the *result* - which is safe however wide
 *    the word was, since `ToInt32` keeps the low 32 bits and the field is inside them.
 *
 * Nothing needs converting on the way out, and that is the point of `Bool` being 0 or 1 rather than a
 * host boolean: an enum, a `@bits` integer, a nested scalar record and a `Bool` are all numbers, so
 * the bits *are* the value and this is the same shift and mask for every one of them.
 */

/*
 * Which of the two forms a range is reached by.
 *
 * A runtime scale always takes the wide one, whatever this target's words are: it arrived with a
 * reference, and a reference is the one place the word's width is not known - see maxWordBits. A
 * constant position takes the wide form only where the word genuinely needs it.
 */
static bool needsWideForm(PlaceBits bits) {
    if(bits.scale) return true;
    return bits.word > 32 && bits.offset + bits.width > 32;
}

/*
 * A range that *is* the word it lives in, which is a single-field record: the one field fills the
 * scalar, so reading it is reading the number and writing it is writing the number.
 *
 * Asked only of a word too wide for the host's operators, where the alternative is a remainder and a
 * re-sign - three operations to recover a value nothing has touched. Below 32 the mask is one cheap
 * operation and stays, which also keeps the two ends of a narrower word agreeing about whether it
 * holds the value or the pattern.
 *
 * Both directions have to take this together or neither may: what makes the identity sound is that
 * the word holds whatever the store put there, so the store must not be masking what the load will
 * not.
 */
static bool coversWord(PlaceBits bits) {
    return !bits.scale && bits.word > 32 && bits.offset == 0 && bits.width >= bits.word;
}

// The shift, for the 32-bit form. Null where the range starts at bit zero of what it names, and
// never asked of a range that arrived with a scale.
static JsPtr<Expr> shiftOf(Gen& g, PlaceBits bits) {
    return bits.offset ? number(g, F64(bits.offset)) : nullptr;
}

// The same position as a *multiplier*, for the wide form: `2**offset` folded into whatever scale a
// reference arrived with. Null means one, which is what lets a range at bit zero cost nothing.
static JsPtr<Expr> scaleOf(Gen& g, PlaceBits bits) {
    if(!bits.scale) return bits.offset ? number(g, powerOfTwo(bits.offset)) : nullptr;
    if(!bits.offset) return bits.scale;

    return binary(g, BinaryOp::Mul, bits.scale, number(g, powerOfTwo(bits.offset)));
}

/*
 * Where a range sits, as the two numbers the wide form multiplies by.
 *
 * `down` brings the bits at and above the range to bit zero and `up` puts a field back, and the two
 * are reciprocals - so a position could be one number and is two because of *which* number each end
 * has. A constant offset knows both and multiplies twice, which is what a `Math.floor` wants in
 * front of it. A reference carries the forward scale alone, and deriving its reciprocal would be a
 * divide per access either way, so that end divides instead. `divides` is that difference and it is
 * the only thing separating the two families of helper below.
 */
struct Position {
    JsPtr<Expr> down = nullptr;   // multiply the word by this; null means one
    JsPtr<Expr> up = nullptr;     // multiply the field by this; null means one
    bool divides = false;         // `word / up` rather than `word * down`
};

static Position positionOf(Gen& g, PlaceBits bits) {
    Position position;

    if(bits.scale) {
        position.up = scaleOf(g, bits);
        position.divides = true;
        return position;
    }

    if(bits.offset) {
        position.down = number(g, 1.0 / powerOfTwo(bits.offset), false);
        position.up = number(g, powerOfTwo(bits.offset));
    }

    return position;
}

// The bits at and above the range, as an integer. Exact for every value a `number` holds, since
// scaling by a power of two only moves the exponent.
static JsPtr<Expr> shiftedDown(Gen& g, JsPtr<Expr> owner, Position position) {
    if(position.divides) {
        return hostCall(g, "Math"_v, "floor"_v, binary(g, BinaryOp::Div, owner, position.up));
    }

    if(!position.down) return owner;
    return hostCall(g, "Math"_v, "floor"_v, binary(g, BinaryOp::Mul, owner, position.down));
}

// The low `width` bits of an already-shifted value, read as the field's own type. Shared by both
// forms, because once the range is at bit zero it is inside 32 bits either way.
static JsPtr<Expr> maskToField(Gen& g, JsPtr<Expr> value, U32 width, bool isSigned) {
    if(width >= 32) return binary(g, BinaryOp::And, value, number(g, F64(~U32(0))));

    /*
     * A signed field widens by *sign*-extension, which masking cannot do: `& 15` on a `@bits(4) I32`
     * holding -4 answers 12, and the field's four bits are all the information there is to tell the
     * two apart. Shifting the field's top bit up to bit 31 and arithmetically back down truncates
     * and sign-extends in the same pair - the shape `decodePackedField` uses natively, and the
     * reason the two targets agreed on everything here except the sign.
     */
    if(isSigned) {
        auto distance = number(g, F64(32 - width));
        return binary(g, BinaryOp::Sar, binary(g, BinaryOp::Shl, value, distance), distance);
    }

    return binary(g, BinaryOp::And, value, number(g, F64((U32(1) << width) - 1)));
}

static JsPtr<Expr> decodeRange(Gen& g, JsPtr<Expr> owner, PlaceBits bits, Position position,
                               bool wide, bool isSigned) {
    /*
     * A range entirely below bit 32 takes the 32-bit form whatever the word is, because `>>>` reads
     * its operand as `ToUint32` - the low half - and the field is inside that half by construction.
     * Only a *write* has to care about the bits it is dropping.
     */
    if(!wide) {
        auto value = owner;
        if(auto shift = shiftOf(g, bits)) value = binary(g, BinaryOp::Shr, value, shift);

        return maskToField(g, value, bits.width, isSigned);
    }

    auto value = shiftedDown(g, owner, position);
    if(bits.width < 32) return maskToField(g, value, bits.width, isSigned);

    /*
     * A field of 32 bits or more, which no 32-bit operator can cut down: the remainder is the only
     * thing that reduces a `number` to a width the host has no mask for. `%` is a genuine
     * floating-point remainder and the slowest thing in this file, which is why it is reached only
     * by the one shape that has no alternative - a field wider than the operators, in a word wider
     * than the operators.
     */
    auto reduced = binary(g, BinaryOp::Rem, value, number(g, powerOfTwo(bits.width)));
    if(!isSigned) return reduced;

    // Read back as a signed value. The binding is `resignExpr`'s requirement rather than a tidiness
    // measure: it mentions its operand three times.
    if(g.body) reduced = declare(g, generatedName(g, "bits"_v, g.labelCounter++), reduced);
    return resignExpr(g, reduced, bits.width);
}

/*
 * Putting one back, which is a read-modify-write because the neighbours share the word.
 *
 * At 32 bits and below, `(owner & ~(mask << shift)) | ((value & mask) << shift)`. The inner mask is
 * not redundant: a `@bits(4)` value that arrived wider would otherwise write over the field above it,
 * and a signed one arrives sign-extended, so its high bits are ones exactly when they must not be
 * stored.
 *
 * Above 32 the same sentence becomes arithmetic: `owner + (wanted - held) * 2**offset`, where `held`
 * is the field's current contents. Additive rather than mask-and-or because there is no mask - the
 * hole is above bit 31 as often as not - and correct for the same reason the mask was: the difference
 * only ever moves the bits the field owns, and every intermediate stays under 2^53 because the word
 * does.
 *
 * Which form is chosen from is the *word* rather than the range, and that is where this differs from
 * the load: a field entirely below bit 32 can still be read with `>>>`, because reading only has to
 * find the bits it wants, while writing has to leave alone every bit it does not - and `owner & hole`
 * drops the whole high half of a word that has one.
 */
static JsPtr<Expr> encodeRange(Gen& g, JsPtr<Expr> owner, PlaceBits bits, Position position,
                               bool wide, JsPtr<Expr> value) {
    auto mask = bits.width >= 32 ? ~U32(0) : (U32(1) << bits.width) - 1;

    if(!wide) {
        value = binary(g, BinaryOp::And, value, number(g, F64(mask)));

        auto shift = shiftOf(g, bits);
        if(shift) value = binary(g, BinaryOp::Shl, value, shift);

        // The hole the field occupies, as a constant - the shift is one, and so is the mask.
        auto hole = shift ? unary(g, UnaryOp::BitNot,
                                  binary(g, BinaryOp::Shl, number(g, F64(mask)), shift))
                          : number(g, F64(U32(~mask)));

        auto written = binary(g, BinaryOp::Or, binary(g, BinaryOp::And, owner, hole), value);

        /*
         * A word that reaches bit 31 is read back unsigned, which is what makes "a packed word is a
         * non-negative pattern" true rather than usually true.
         *
         * It matters one level away. A write *through a reference* cannot know how wide the word it
         * was handed is, so it uses the arithmetic form below - and that form only stays inside the
         * word if what it is adding to is the pattern rather than the pattern minus 2^32. Narrower
         * words cannot be negative in the first place and are left alone.
         */
        if(bits.word < 32) return written;
        return binary(g, BinaryOp::Shr, written, number(g, 0));
    }

    // Both readings are of the *pattern* rather than of the value, so a signed field is reduced the
    // unsigned way at both ends and the subtraction below cancels exactly.
    auto reduce = [&](JsPtr<Expr> from) -> JsPtr<Expr> {
        if(bits.width < 32) return binary(g, BinaryOp::And, from, number(g, F64(mask)));
        if(bits.width == 32) return binary(g, BinaryOp::Shr, from, number(g, 0));

        return wideCallAt(g, WideOp::Wrap, bits.width, false, from, nullptr);
    };

    auto held = reduce(shiftedDown(g, owner, position));
    auto difference = binary(g, BinaryOp::Sub, reduce(value), held);

    if(position.up) difference = binary(g, BinaryOp::Mul, difference, position.up);
    return binary(g, BinaryOp::Add, owner, difference);
}

/*
 * The two above as functions, for the shapes that are worth calling rather than writing out.
 *
 * A range reached with the host's own operators is two or three of them against literals -
 * `p.$p4 >>> 20 & 4095` - and a call could only make that longer. What is worth naming is the
 * *arithmetic* form, which exists at all because a word wider than 32 bits has no mask: the read is
 * a multiply, a floor and a mask, and the write is that plus the arithmetic to put the field back
 * with the read appearing inside it a second time. `flip(&Bool)` was ninety characters of it, and a
 * program that borrows narrow values or holds records above 32 bits does the same thing everywhere.
 *
 * Two families, because the two ends hold the position differently and neither should take the
 * other's cost - see Position. A **packed** helper is handed the reciprocal and the scale, both
 * literals at the site, and multiplies; a **borrowed** one is handed the reference's own scale and
 * divides. Both are interned per (width, signedness, direction), so every offset of a given width
 * shares one, which is what keeps this from becoming a helper per field.
 *
 * Interned the way wide.cpp's operators are and for the same second reason: the width stays a
 * literal inside the body, where the engine's own fast paths want to see it. What deliberately does
 * *not* move inside is the `owner[key]` indexing of a reference - one element access in one shared
 * helper would be a single inline cache for every borrowed word in the program.
 */
static U32 bitHelperKey(U32 bits, bool isSigned, bool store, bool packed) {
    return (bits << 3) | (U32(packed) << 2) | (U32(isSigned) << 1) | U32(store);
}

static Name bitHelper(Gen& g, U32 bits, bool isSigned, bool store, bool packed) {
    // A store reads and writes the *pattern* at both ends, so the sign is not one of its questions -
    // and keying on it anyway produced two helpers with identical bodies.
    if(store) isSigned = false;

    auto key = bitHelperKey(bits, isSigned, store, packed);
    if(auto found = g.bitHelpers.get(key)) return found.unwrap();

    // `$p20i$get` for a packed one and `$b20i$get` for a borrowed one, on wide.cpp's pattern: the
    // `$` prefix is the compiler's own convention and `uniqueName` handles a program that declared
    // the same identifier itself.
    char buffer[32];
    Size length = 0;
    buffer[length++] = '$';
    buffer[length++] = packed ? 'p' : 'b';
    length += show(U64(bits), buffer + length, sizeof(buffer) - length);
    buffer[length++] = isSigned ? 'i' : 'u';
    buffer[length++] = '$';

    auto suffix = store ? "set"_v : "get"_v;
    copy(suffix.ptr, buffer + length, suffix.length);
    length += suffix.length;

    auto name = uniqueName(g, StringView { buffer, length }, false);
    g.bitHelpers.add(key, name);
    g.bitHelperOrder.push(BitHelper { name, U16(bits), isSigned, store, packed });
    return name;
}

static JsPtr<Expr> namedParameter(Gen& g, StringView text, Name& into) {
    into = literalName(g, text);
    return variable(g, into);
}

void emitBitHelpers(Gen& g) {
    if(g.bitHelperOrder.size() == 0) return;

    auto heading = make<CommentStmt>(g, internText(g,
        "bit ranges of a word wider than the host's operators - see codegen/js/place.cpp"_v));
    g.bitHelperComment = asStmt(g, heading);
    emit(g, heading);

    for(Size i = 0; i < g.bitHelperOrder.size(); i++) {
        auto helper = g.bitHelperOrder[i];
        auto function = make<FunStmt>(g, helper.name);

        function->body = collect(g, [&] {
            Name wordName, downName, upName, valueName;
            auto word = namedParameter(g, "w"_v, wordName);
            auto down = namedParameter(g, "i"_v, downName);
            auto up = namedParameter(g, "s"_v, upName);
            auto value = namedParameter(g, "v"_v, valueName);

            /*
             * The parameters are the position, which is the whole of what the site knows and the
             * body does not. A packed helper takes both directions because it multiplies in both;
             * a borrowed one takes the scale alone and divides with it, and a load through one
             * therefore needs no second parameter at all.
             */
            Position position;
            function->args.push(g.file.arena, wordName);

            if(helper.packed) {
                position.down = down;
                function->args.push(g.file.arena, downName);

                if(helper.store) {
                    position.up = up;
                    function->args.push(g.file.arena, upName);
                }
            } else {
                position.up = up;
                position.divides = true;
                function->args.push(g.file.arena, upName);
            }

            if(helper.store) function->args.push(g.file.arena, valueName);

            // The word is as wide as this target packs, since the body serves every range of this
            // width - and it is the arithmetic form for all of them by construction.
            PlaceBits bits;
            bits.width = helper.bits;
            bits.word = maxWordBits(g);
            bits.scale = helper.packed ? nullptr : up;

            emit(g, make<ReturnStmt>(g, helper.store
                ? encodeRange(g, word, bits, position, true, value)
                : decodeRange(g, word, bits, position, true, helper.isSigned)));
        });

        emit(g, function);
    }
}

/*
 * Pure in exactly the sense `Math.imul` and the wide helpers are: it reads nothing and writes
 * nothing, and the store's own assignment is at the call site rather than inside it. Saying so is
 * what lets a one-use binding holding one collapse into its use, and what lets a read whose result
 * nothing wanted go away - a reborrow computes the field's value only to describe where it is, and
 * without this the emitted body kept two loads it never looked at.
 */
static JsPtr<Expr> bitCall(Gen& g, Name name, Buffer<const JsPtr<Expr>> args) {
    auto node = make<CallExpr>(g, variable(g, name));
    for(auto arg: args) node->args.push(g.file.arena, arg);

    node->pure = true;
    return asExpr(g, node);
}

/*
 * The two entry points, which decide between the call and the expression.
 *
 * `type` is the field's own, because a `Bool` is a number rather than a host boolean here and a
 * signed field widens by sign extension rather than by masking - and neither is visible from the
 * width alone.
 */
static bool fieldIsSigned(Gen& g, TypePtr type) {
    auto integer = intType(g, type);
    return integer && integer->isSigned;
}

/*
 * Whether a range is worth calling for rather than writing out.
 *
 * A reference always is: its position is a variable, so there is nothing to fold and the expression
 * is the same length at every site. A constant one is worth it only where it is actually *at* an
 * offset - at bit zero the position is the identity, the arithmetic form is already three or four
 * operations, and a helper would add a multiply by one and a floor of it to every access of the low
 * field of every record.
 */
static bool worthCalling(PlaceBits bits, bool wide) {
    return wide && (bits.scale || bits.offset != 0);
}

JsPtr<Expr> decodeBits(Gen& g, JsPtr<Expr> owner, PlaceBits bits, TypePtr type) {
    if(coversWord(bits)) return owner;

    auto isSigned = fieldIsSigned(g, type);
    auto wide = needsWideForm(bits);
    auto position = positionOf(g, bits);
    if(!worthCalling(bits, wide)) return decodeRange(g, owner, bits, position, wide, isSigned);

    auto name = bitHelper(g, bits.width, isSigned, false, !bits.scale);
    JsPtr<Expr> args[] = { owner, bits.scale ? position.up : position.down };
    return bitCall(g, name, toBuffer(args));
}

JsPtr<Expr> encodeBits(Gen& g, JsPtr<Expr> owner, PlaceBits bits, TypePtr type, JsPtr<Expr> value) {
    if(coversWord(bits)) return value;

    auto wide = bits.scale || bits.word > 32;
    auto position = positionOf(g, bits);
    if(!worthCalling(bits, wide)) return encodeRange(g, owner, bits, position, wide, value);

    auto name = bitHelper(g, bits.width, fieldIsSigned(g, type), true, !bits.scale);

    if(bits.scale) {
        JsPtr<Expr> args[] = { owner, position.up, value };
        return bitCall(g, name, toBuffer(args));
    }

    JsPtr<Expr> args[] = { owner, position.down, position.up, value };
    return bitCall(g, name, toBuffer(args));
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
