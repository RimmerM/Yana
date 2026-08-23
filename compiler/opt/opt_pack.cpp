#include "opt_pass.h"
#include "../resolve/witness.h"

/*
 * Packed field access, expanded into arithmetic.
 *
 * This is the repr-lower step, and it is the one place in `compiler/opt` that asks the target
 * anything. Everything above it reasons about places and values; this turns
 *
 *      %v = load %h@Header.version : @bits(4) U32
 *      init %h@Header.length, %w
 *
 * into a load of the word those fields share, a shift and a mask, and a read-modify-write of the
 * same word - which are ordinary instructions that the passes above already know how to fold, forward
 * and eliminate. Running them a second time afterwards is what collapses a record built out of nine
 * literals from nine read-modify-writes into one store.
 *
 * ## Why up here rather than in each backend
 *
 * Because both backends were already doing exactly this, in the same three operations, from the same
 * `FieldRepr` - `decodePackedBits` in resolve/lower.cpp and `decodeRange` in codegen/js/place.cpp -
 * and neither had anything to fold it with. The arithmetic is the *same* arithmetic; only the numbers
 * come from the target, and a `ReprTable` is a target parameter rather than a target. So one
 * expansion serves both, and what it produces is IR rather than output.
 *
 * See Analysis-Optimization.md §5 for the seam. Below is what it means in code.
 *
 * ## What stays in the backends, and why
 *
 *  - **A unit wider than 32 bits.** At 32 and under the two targets agree operation for operation. JS
 *    has no 32-bit operator that reaches a bit above 31, so above that it reads a field by multiplying
 *    by a negative power of two and flooring, which is a different expansion and the measured one.
 *    Emitting shifts there would compile - `wide.cpp` has the operators - at the cost of the form
 *    that was tuned. So the wide half keeps `$pNN$get`/`$pNN$set` and native keeps its 64-bit
 *    shift-and-mask, and re-fusing a wide shift into the multiply form is what would let this take
 *    the rest.
 *
 *  - **A reference that carries its shift.** Tier 2 is dynamic by construction: the callee was
 *    compiled once and the shift is the caller's. There is nothing constant to expand.
 *
 *  - **Tags.** A folded discriminant is not stored anywhere and a bit tag is a different encode; both
 *    are intercepted before the packed path in both backends and are left there.
 *
 * ## The two things that make it safe
 *
 * **One word is one place.** Two co-packed fields expand to *the same* unit place, because the field
 * index in front of the unit projection is canonicalized to the lowest one sharing the storage.
 * Without that, opt_place.cpp would see two different `Field` projections - which it is entitled to
 * say do not alias, that being true of the fields and false of the word - and a write of one would
 * not kill a read of the other.
 *
 * **A word is expanded for every access or for none.** If one access to a word is declined - a field
 * type this cannot compute in, a whole scalar record read out of the middle of one - then every
 * access to that word is left alone too. A word that is half expanded is a word where one half is a
 * `Unit` place and the other is a `Field` place, and those two do not alias each other either.
 */

namespace {

// The widest unit this expands, which is the widest one both targets take apart with the same
// operators. See the seam above.
constexpr U32 kMaxUnitBits = 32;

/*
 * Which bits of which word a place names, and where the path has to be cut to name the word.
 *
 * A faithful port of `packedAccess` in resolve/lower.cpp - deliberately so, because the question
 * "is this a packed access" has to get the same answer here as it does there. A place this declines
 * is one the backend also treats as an ordinary address, so there is no word hiding behind a
 * disagreement.
 *
 * What is added is `cut` and `canonical`: where the path first entered a bit range, and which of the
 * fields sharing that word names it. Everything after the cut contributes bit offsets and no address,
 * which is exactly why the cut is sound - see lowerPlace, where a field inside a scalar aggregate
 * adds an offset of zero.
 */
struct PackedAccess {
    U32 unitBits = 0;
    U32 bitOffset = 0;
    U32 bitWidth = 0;
    TypePtr type = nullptr;

    Size cut = 0;
    U16 canonical = 0;

    bool exists() const { return bitWidth != 0; }
};

// What a place is rooted in, over the shared walk - see resolve/place.h.
TypePtr placeRootedType(OptContext& opt, const Place& place) {
    return placeRootType(*opt.program.core, *opt.function, place);
}

// The lowest field of one aggregate whose storage is the same word as this one's. Both targets name
// the word by its byte offset rather than by a field - an address there, a property named after the
// offset here - so any of them would reach it; this picks one so that two accesses produce one path.
U16 canonicalFieldOf(OptContext& opt, TypePtr owner, U16 index, const FieldRepr& field) {
    auto tuple = (TupType*)opt.global[owner];

    for(U16 i = 0; i < U16(tuple->fields.size()); i++) {
        auto other = opt.repr.fieldOf(owner, i);
        if(other && other->isPacked() && other->sharesStorageWith(field)) return i;
    }

    return index;
}

PackedAccess packedAccessOf(OptContext& opt, const Place& place) {
    PackedAccess access;
    auto declined = false;
    auto type = placeRootedType(opt, place);

    // The path, over the shared walk - see resolve/place.h. What is this one's own is the bit range
    // and the cut; every placement question it asks is about `step.owner`.
    walkPlace(*opt.program.core, *opt.function, place, [&](const PlaceStep& step) {
        auto decline = [&]() {
            declined = true;
            return false;
        };

        if(step.broken) return decline();
        type = step.type;

        switch(step.kind) {
            case ProjectionKind::Field: {
                // A function value's words are laid out by FunValueLayout rather than by Repr, and
                // are never packed.
                if(opt.global[step.owner]->kind == Type::Fun) {
                    if(access.exists()) return decline();
                    break;
                }

                auto field = opt.repr.fieldOf(step.owner, step.index);
                if(!field) return decline();

                if(access.exists()) {
                    // Already inside a bit range, so this field's placement is relative to it. An
                    // unpacked field of a scalar aggregate is the whole of it and contributes no
                    // offset, so the width is the value's own.
                    access.bitOffset += field->bitOffset;
                    access.bitWidth = field->isPacked()
                        ? field->bitWidth
                        : valueWidth(opt.global, field->type, opt.repr.target.integers).logical;

                    if(!access.bitWidth) return decline();
                } else if(field->isPacked()) {
                    access.unitBits = U32(field->wordBytes) * 8;
                    access.bitOffset = field->bitOffset;
                    access.bitWidth = field->bitWidth;
                    access.cut = step.at + 1;
                    access.canonical = canonicalFieldOf(opt, step.owner, step.index, *field);
                }

                // A boxed field is a whole pointer: never inside a packed word, and never the word
                // itself. The same guard lower.cpp's copy of this walk carries, for the same reason.
                if(step.crossedBox && access.exists()) return decline();
                break;
            }
            case ProjectionKind::Downcast:
                // A payload inside a bit range can only be a single-constructor record's, whose
                // payload begins where the record does.
                if(access.exists() && opt.repr.of(step.owner).payloadOffset) return decline();
                break;

            case ProjectionKind::Discriminant:
                if(opt.repr.of(step.owner).isBitTagged()) return decline();
                break;

            case ProjectionKind::Deref:
                if(access.exists()) return decline();
                break;

            default:
                return decline();
        }

        return true;
    });

    if(declined) return {};

    access.type = type;
    return access;
}

struct Expander {
    OptContext& opt;

    // The type every intermediate is computed in: unsigned, register-filling, and the same width on
    // both targets. Unsigned matters on JS, where it is what keeps a packed word a non-negative
    // pattern rather than one that has been through `| 0` - a write through a reference adds to that
    // pattern and would leave the word 2^32 short of itself otherwise.
    TypePtr unit = nullptr;

    // The unit places of words that must not be expanded, because something names one of their
    // fields in a way this cannot rewrite.
    Array<Place> blocked;

    // The unit places whose storage the target zeroes when it allocates it - see zeroedAtAllocation.
    SmallArray<Place, 8> zeroed;

    U64 lowMask(U32 bits) const { return bits >= 64 ? maxLimit<U64> : (U64(1) << bits) - 1; }

    /*
     * Whether a value of this type is one the expansion can compute with.
     *
     * An integer held in a 32-bit register, or an enum - which is one. `Bool` is the common case and
     * is an enum record rather than an integer type, so this is asked of the Repr's own reading
     * rather than of `Type::Int`.
     *
     * A wider register is declined for the same reason a wider unit is: on JS a `WideInt` is a
     * number the host has no operators for, and the intermediates below are 32-bit ones.
     */
    bool computableType(TypePtr type) {
        if(!type || isMemoryType(opt.global, type)) return false;

        if(opt.global[type]->kind == Type::Int) {
            auto integer = (IntType*)opt.global[type];
            return integer->registerBitsOn(opt.repr.target.integers) <= kMaxUnitBits;
        }

        auto record = opt.global[type]->kind == Type::Record ? (RecordType*)opt.global[type] : nullptr;
        return record && record->layout == RecordType::Enum;
    }

    /*
     * The type the extracted value is named at.
     *
     * A `@bits` refinement's unrefined form, because the value this produces is already inside the
     * refinement's width - the mask below is what put it there - so the two types are the same
     * register with the same contents. Naming the unrefined one is what lets the folder compute in
     * it: `foldableInt` declines a refinement, on the grounds that what width its *arithmetic* wraps
     * at is a question the targets answer differently, and this is not that question.
     */
    TypePtr valueTypeOf(TypePtr type) {
        auto canonical = canonicalType(opt.global, type);
        return canonical ? canonical : type;
    }

    /*
     * A change of type on its own, where the two ends are not already the same one.
     *
     * Every operation this pass builds reads and writes `unit`, and the conversions are pushed to
     * the ends rather than folded into the first and last operation. That is not tidiness: the
     * folder's identities are stated over one type - `x & ~0` is `x` only when `x` is already that
     * wide - so an `and` whose result was the field's one-bit type and whose operand was the whole
     * word read as a mask that could be dropped, and a `Bool` field came back holding its
     * neighbour's bit. The suite caught it; homogeneous operands are what stops it recurring.
     */
    ModulePtr<Value> convert(Block& block, LocationId source, StringId name, InstList& into,
                             ModulePtr<Value> value, TypePtr to) {
        if(opt.local[value]->type == to) return value;

        auto cast = createInst<InstUnary>(*opt.module, *opt.function, block, source, name, to,
                                          Value::Cast, value);
        into.push(cast);
        return valueOf(cast);
    }

    bool expandable(const PackedAccess& access, TypePtr valueType) {
        if(!access.exists()) return false;
        if(access.unitBits == 0 || access.unitBits > kMaxUnitBits) return false;
        if(access.bitOffset + access.bitWidth > access.unitBits) return false;

        return computableType(valueType);
    }

    /*
     * Only storage this frame owns.
     *
     * A place rooted in a reference is where the shift stops being a constant - the callee has the
     * pointee type and nothing else - and both backends have an ABI for exactly that. Declining the
     * root rather than asking whether the pointee is narrow keeps this from having to agree with two
     * different spellings of that question, `isNarrowRepr` natively and `isNarrowJsValue` on JS.
     */
    bool rootedHere(const Place& place) {
        if(place.root == PlaceRoot::Global) return true;
        if(place.root != PlaceRoot::Local) return false;
        if(place.local >= opt.function->localCount()) return false;

        // The slot behind a `&` parameter holds the reference the caller passed rather than storage
        // of its own.
        return !opt.function->localAt(opt.local, place.local).borrowed;
    }

    // The word a packed place names, as a place: the path up to and including the field that entered
    // the bit range, with that field canonicalized, and the unit projection saying how wide it is.
    Place unitPlace(Place& place, const PackedAccess& access) {
        Place result = place;
        result.projections = {};

        for(Size i = 0; i < access.cut; i++) {
            auto projection = place.projections.get(opt.local, i);
            if(i == access.cut - 1) projection.index = access.canonical;

            result.projections.push(opt.program.arena, projection);
        }

        result.projections.push(opt.program.arena, Projection {
            ProjectionKind::Unit, U16(access.unitBits), nullptr
        });

        return result;
    }

    bool samePlace(Place& a, Place& b) {
        if(a.root != b.root) return false;
        if(a.root == PlaceRoot::Local && a.local != b.local) return false;
        if(a.root == PlaceRoot::Global && a.global != b.global) return false;
        if(a.projections.size() != b.projections.size()) return false;

        for(Size i = 0; i < a.projections.size(); i++) {
            auto left = a.projections.get(opt.local, i);
            auto right = b.projections.get(opt.local, i);
            if(left.kind != right.kind || left.index != right.index) return false;
        }

        return true;
    }

    bool isBlocked(Place& place) {
        for(auto& other: blocked) {
            if(samePlace(other, place)) return true;
        }

        return false;
    }

    // Every access this will not rewrite, remembered by the word it belongs to. Only reads and writes
    // rooted here: everything else either clobbers on sight in opt_place.cpp - a drop, a swap, a call
    // - or is rooted in a reference, which that pass already refuses to disambiguate.
    void collectBlocked(Function& function) {
        for(auto blockPointer: function.blocks.contents(opt.local)) {
            auto block = opt.local[blockPointer];

            for(auto pointer: block->instructions(opt.local)) {
                auto instruction = opt.local[pointer];

                Place* place = nullptr;
                TypePtr valueType = nullptr;

                if(instruction->kind == Value::LoadPlace) {
                    place = &((InstLoadPlace*)instruction)->place;
                    valueType = instruction->type;
                } else if(instruction->kind == Value::Init || instruction->kind == Value::Assign) {
                    auto write = (InstInit*)instruction;
                    place = &write->place;
                    valueType = opt.local[write->value]->type;
                } else {
                    continue;
                }

                if(!rootedHere(*place)) continue;

                auto access = packedAccessOf(opt, *place);
                if(!access.exists() || expandable(access, valueType)) continue;

                // A word too wide to expand has no unit place worth remembering: nothing will produce
                // one for it, so nothing can half-expand it either.
                if(access.unitBits == 0 || access.unitBits > kMaxUnitBits) continue;

                blocked.push(unitPlace(*place, access));
            }
        }
    }

    /*
     * Storage the target zeroes on the way in, said out loud.
     *
     * `hasPaddedWord` is why the zeroing exists at all: a packed word's leftover bits *are* the
     * niche, and a packed write is a read-modify-write that preserves what it does not own, so the
     * bits nothing writes have to start out right. Native spends a `zeroStorage` at the allocation
     * for it and JS builds the property at zero.
     *
     * Neither of those is in this IR, and that is the gap: a record built out of literals reads a
     * word whose contents are known and folds nothing, so `Nine {a: False, ...}` came out as a mask
     * of a value instead of as the constant it is. One store of zero after the allocation is the
     * whole fix - forwarding answers every read from it, the arithmetic folds, and the store itself
     * is then overwritten by the constructed value and removed.
     *
     * Asked of the *type* rather than of the target directly, and only where it is true: a word this
     * says nothing about keeps its load, which is the conservative answer and not a wrong one.
     */
    bool zeroedAtAllocation(Place& place) {
        if(place.root != PlaceRoot::Local || place.local >= opt.function->localCount()) return false;

        auto slot = opt.function->localAt(opt.local, place.local);
        if(!slot.value || !slot.type) return false;
        if(opt.local[slot.value]->kind != Value::Alloc) return false;

        return opt.repr.hasPaddedWord(slot.type);
    }

    void publishZero(Place& place) {
        for(auto& other: zeroed) {
            if(samePlace(other, place)) return;
        }

        zeroed.push(place);
    }

    void emitZeroes() {
        for(auto& place: zeroed) {
            auto storage = opt.function->localAt(opt.local, place.local).value;
            auto block = opt.local[opt.local[storage]->block];

            for(Size i = 0; i < block->instructionCount(); i++) {
                if(block->instructionAt(opt.local, i) != (ModulePtr<Inst>)storage) continue;

                InstList written;
                written.push(createInst<InstInit>(
                    *opt.module, *opt.function, *block, opt.local[storage]->source, StringId(),
                    opt.program.scalar.unit, place,
                    makeConstant(opt, *opt.local[storage], unit, 0), Value::Init));

                opt.ir().insert(*block, i + 1, written);
                break;
            }
        }
    }

    ModulePtr<Value> valueOf(Inst* instruction) {
        return (ModulePtr<Value>)(instruction - opt.local);
    }

    /*
     * Reading a packed field: bring the range to bit zero and discard everything else.
     *
     * An unsigned field is a shift and a mask. A signed one is the same two plus the two that turn a
     * masked pattern back into a value - `(x ^ s) - s` for the range's own sign bit - which is
     * spelled arithmetically rather than as a shift pair because the intermediates are unsigned and
     * an arithmetic shift of one would bring down the wrong bits.
     *
     * **The mask is only for the bits *above* the range.** A field that ends where its unit does has
     * none, because the shift that brought it to zero pushed them all out - the word was loaded
     * unsigned at exactly `unitBits`, so `x >> bitOffset` already has nothing but the field in it.
     * Asking whether the field is narrower than its unit is the wrong question and emitted a mask
     * that is the identity: the top field of every packed word paid an `and` for it, and the smaller
     * the field the more obviously pointless - a one-bit flag at the top of a word decoded to
     * `(x >> 31) & 1`.
     *
     * Only the folder could have removed it afterwards, and it cannot: `x & 1` is the identity only
     * for an `x` whose range is known, which needs a known-bits pass. Not emitting it is the whole
     * fix and it is one comparison.
     *
     * The last operation carries the field's type, so the value that comes out is named the same
     * thing the load that used to produce it was.
     */
    ModulePtr<Value> decode(Block& block, LocationId source, StringId name, InstList& into,
                            ModulePtr<Value> word, const PackedAccess& access, TypePtr type) {
        auto value = word;
        auto isSigned = opt.global[type]->kind == Type::Int && ((IntType*)opt.global[type])->isSigned;

        auto binary = [&](Value::Kind kind, ModulePtr<Value> lhs, U64 rhs) {
            auto instruction = createInst<InstBinary>(
                *opt.module, *opt.function, block, source, StringId(), unit, kind, lhs,
                makeConstant(opt, *opt.local[word], unit, rhs));

            into.push(instruction);
            return valueOf(instruction);
        };

        if(access.bitOffset) value = binary(Value::Shr, value, access.bitOffset);

        if(access.bitOffset + access.bitWidth < access.unitBits) {
            value = binary(Value::And, value, lowMask(access.bitWidth));
        }

        if(isSigned) {
            // The range's own sign bit, moved out of the value and then subtracted back off - which
            // sign-extends without an arithmetic shift, and therefore without the intermediates
            // having to be signed for one operation in the middle of a chain of unsigned ones.
            auto sign = U64(1) << (access.bitWidth - 1);
            value = binary(Value::Xor, value, sign);
            value = binary(Value::Sub, value, sign);
        }

        return convert(block, source, name, into, value, valueTypeOf(type));
    }

    /*
     * Writing one back: clear the range, put the value in it, keep everything else.
     *
     * The incoming value is masked rather than checked, which is the same choice `@bits` makes at
     * every other store - the mask is what makes the surrounding niche true, so a range check could
     * not replace it. A signed value arrives sign-extended and its high bits are ones exactly when
     * they must not be stored, which is the other reason the mask is not optional.
     */
    ModulePtr<Value> encode(Block& block, LocationId source, InstList& into,
                            ModulePtr<Value> word, const PackedAccess& access,
                            ModulePtr<Value> value) {
        auto mask = lowMask(access.bitWidth);

        auto binary = [&](Value::Kind kind, ModulePtr<Value> lhs, ModulePtr<Value> rhs) {
            auto instruction = createInst<InstBinary>(*opt.module, *opt.function, block, source, StringId(),
                                                      unit, kind, lhs, rhs);
            into.push(instruction);
            return valueOf(instruction);
        };

        auto immediate = [&](U64 value) { return makeConstant(opt, *opt.local[word], unit, value); };

        auto placed = binary(Value::And, convert(block, source, StringId(), into, value, unit), immediate(mask));
        if(access.bitOffset) placed = binary(Value::Shl, placed, immediate(access.bitOffset));

        auto cleared = binary(Value::And, word, immediate(~(mask << access.bitOffset)));
        return binary(Value::Or, cleared, placed);
    }

    // The load of the word, which both directions begin with. Deliberately built fresh at each
    // access rather than shared: Design.md's rule is that a packed word is read at commit time, and
    // it is the *forwarding* pass that is allowed to notice two reads are the same one.
    Inst* loadUnit(Block& block, LocationId source, Place place) {
        return createInst<InstLoadPlace>(*opt.module, *opt.function, block, source, StringId(), unit, place);
    }

    bool rewriteLoad(Block& block, Size index, ModulePtr<Inst> pointer) {
        auto& load = (InstLoadPlace&)*opt.local[pointer];

        if(!rootedHere(load.place)) return false;

        auto access = packedAccessOf(opt, load.place);
        if(!expandable(access, load.type)) return false;

        auto place = unitPlace(load.place, access);
        if(isBlocked(place)) return false;
        if(zeroedAtAllocation(load.place)) publishZero(place);

        InstList replacement;
        auto word = loadUnit(block, load.source, place);
        replacement.push(word);

        auto value = decode(block, load.source, load.name, replacement, valueOf(word), access,
                            load.type);

        opt.ir().insert(block, index, replacement);

        opt.ir().replaceValue((ModulePtr<Value>)pointer, value);
        opt.ir().eraseInstruction(pointer);
        return true;
    }

    bool rewriteStore(Block& block, Size index, ModulePtr<Inst> pointer) {
        auto& store = (InstInit&)*opt.local[pointer];

        if(!rootedHere(store.place)) return false;

        auto access = packedAccessOf(opt, store.place);
        if(!expandable(access, opt.local[store.value]->type)) return false;

        auto place = unitPlace(store.place, access);
        if(isBlocked(place)) return false;
        if(zeroedAtAllocation(store.place)) publishZero(place);

        InstList replacement;
        auto word = loadUnit(block, store.source, place);
        replacement.push(word);

        auto merged = encode(block, store.source, replacement, valueOf(word), access, store.value);

        replacement.push(createInst<InstInit>(*opt.module, *opt.function, block, store.source, StringId(),
                                              opt.program.scalar.unit, place, merged, store.kind));

        opt.ir().insert(block, index, replacement);
        opt.ir().eraseInstruction(pointer);
        return true;
    }

    bool run() {
        // `U32` rather than a type this could invent, because a type has to be one the whole pipeline
        // already knows how to lay out, print and coerce. A build whose core module has not defined
        // it has bigger problems than an unexpanded packed field.
        if(!opt.program.core) return false;

        auto found = opt.program.core->namedTypes.get(opt.context.addQualifiedName("U32", 3, 1));
        if(!found) return false;

        unit = found.unwrap();

        collectBlocked(*opt.function);

        auto changed = false;

        for(auto blockPointer: opt.function->blocks.contents(opt.local)) {
            auto block = opt.local[blockPointer];

            for(Size i = 0; i < block->instructionCount(); i++) {
                auto pointer = block->instructionAt(opt.local, i);
                auto kind = opt.local[pointer]->kind;

                auto before = block->instructionCount();
                auto rewritten = kind == Value::LoadPlace ? rewriteLoad(*block, i, pointer)
                    : (kind == Value::Init || kind == Value::Assign) ? rewriteStore(*block, i, pointer)
                    : false;

                if(!rewritten) continue;

                // The replacements went in at `i` and the original came out, so the next instruction
                // this has not seen is where the replacements end.
                i += block->instructionCount() - before;
                changed = true;
            }
        }

        // After the walk, so that a word is only published as zero where something actually reads or
        // writes it as a unit - a store into storage nothing else touches would be one this added.
        emitZeroes();
        return changed;
    }
};

}

bool expandPacking(OptContext& opt) {
    Expander expander { opt };
    return expander.run();
}
