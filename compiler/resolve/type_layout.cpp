/*
 * Layout: what recursion through a type's members says about it.
 *
 * Two walks that look alike and are not. Breaking cycles *changes* a type - a back edge becomes a
 * box, which is what makes a recursive record layout-able at all - and proving acyclicity only
 * reports. The bit packing below is the third thing that walk decides: which fields of a tuple fit
 * in one scalar, at what offsets, and how wide the whole thing then is.
 */

#include "type_internal.h"
#include "generic.h"
#include "module.h"
#include "name.h"
#include "index.h"

/*
 * Automatic indirection - Design.md's "Representation and layout" and doc/spec/repr.md.
 *
 * A type whose layout is cyclic through *inline containment* cannot be laid out at all, so the
 * compiler breaks the cycle with an indirection and nothing in the source names it. This is where
 * the edge is chosen, and it is in resolve rather than in a backend for the reason the whole file
 * is: which edge gets the pointer is a fact about the program, true of every target at once, and two
 * code generators picking it separately could pick differently.
 *
 * ## What is not an edge
 *
 * A field whose type is a *handle* has a size independent of its target, so it breaks the cycle
 * before this ever sees it: a pointer, a borrow and a function value are the three the walk stops
 * at, and an already-boxed field or constructor is a fourth - `@box` written by the programmer is
 * the manual override on the cut below, and the compiler's own box is what a previous walk left.
 *
 * ## Where the cut lands
 *
 * At the back edge - the type reference naming a declaration that is already on the layout stack -
 * *at whatever depth it appears inside generic arguments*. For
 *
 *     data Tree(a) = Branch {left: Maybe(Tree(a)), right: Maybe(Tree(a))} | Leaf(a)
 *
 * the reference to `Tree` inside `Tree`'s own body is that edge, and it is `Maybe(Tree)`'s `Just`
 * payload rather than `Branch.left`. Cutting there is what produces a one-word child - the box is a
 * non-null pointer, so `Nothing` folds into its null niche by the ordinary niche search - where
 * cutting at `Branch.left` would make an *absent* child cost a heap-allocated `Maybe`.
 *
 * That is why there are two flags rather than one. `Field::boxed` is the edge inside a tuple, which
 * is what `data List(a) = Nil | Cons {head: a, tail: List(a)}` needs; `Constructor::boxed` is the
 * edge that *is* a constructor's whole payload, which is what `Just(a)` is and what a positional
 * `data Loop = L(Loop)` is.
 *
 * ## Why the answer is uniform, and therefore not part of the type
 *
 * `Maybe(Tree)` has infinite inline size, so every `Maybe(Tree)` in the program - local, field,
 * argument, generic instantiation - must be a pointer to a `Tree`. There is no context that could
 * want it otherwise, so there is no variant and nothing for two contexts to disagree about. That is
 * what makes it legal to leave out of the type: type identity exists to keep two things that
 * *differ* from being confused, and these do not differ. `Maybe(Tree)` is one logical type with one
 * Repr, and passing `node.left` to `fn f(m: Maybe(Tree))` needs no coercion.
 *
 * A *tuple* is rewritten rather than mutated, because tuples are interned structurally and `{Tree}`
 * boxed has to be a different type from `{Tree}` unboxed. A *record* is mutated in place, because it
 * is nominal: `Maybe(Tree)` is one instance and the only thing that could observe the change is
 * `Maybe(Tree)` itself.
 */

// One walk's state. `incomplete` is what decides whether the answer may be remembered - see
// RecordType::layoutBroken.
struct LayoutWalk {
    TypeList stack;
    bool incomplete = false;
};

static TypePtr breakCycles(Module& module, TypePtr type, LayoutWalk& walk, LocationId source);

// Whether this type is one the walk is currently inside, which is the whole definition of a back
// edge: reaching it again means laying it out needs its own size.
static bool onLayoutStack(LayoutWalk& walk, TypePtr type) {
    for(auto entry: walk.stack) {
        if(entry == type) return true;
    }

    return false;
}

// A tuple, with whichever of its fields turned out to be back edges boxed. Returns the interned
// tuple to use in place of this one, which is this one where nothing changed.
static TypePtr breakTupleCycles(Module& module, TupType& tuple, LayoutWalk& walk, LocationId source) {
    auto base = *module.types;
    auto self = (Type*)&tuple - base;

    // A tuple cannot be its own back edge - reaching one means a record on the way round, and the
    // record is where the cut belongs - but the guard keeps the recursion bounded regardless.
    if(onLayoutStack(walk, self)) return self;

    walk.stack.push(self);

    SmallArray<Field, 8> fields;
    auto changed = false;

    for(auto field: tuple.fields.contents(base)) {
        auto updated = field;

        if(field.boxed) {
            // Already an indirection, whether the programmer wrote `@box` or a previous walk did.
            // Either way this edge is not part of any cycle.
        } else if(onLayoutStack(walk, field.type)) {
            updated.boxed = true;
            changed = true;
        } else {
            auto rewritten = breakCycles(module, field.type, walk, source);
            if(rewritten != field.type) {
                updated.type = rewritten;
                changed = true;
            }
        }

        fields.push(updated);
    }

    walk.stack.pop();
    if(!changed) return self;

    // The pin travels with the fields: a `@layout(c)` tuple that needed an indirection is still a
    // `@layout(c)` tuple, and a boxed field under that pin is a pointer member, which is exactly how
    // a C struct with one is modelled.
    return (Type*)resolveTupleType(module, toBuffer(fields), source, tuple.layout) - base;
}

static void breakRecordCycles(Module& module, RecordType& record, LayoutWalk& walk, LocationId source) {
    auto base = *module.types;
    auto self = (Type*)&record - base;

    if(record.layoutBroken || onLayoutStack(walk, self)) return;
    if(!record.definitionReady) walk.incomplete = true;

    walk.stack.push(self);

    for(Size i = 0; i < record.constructors.size(); i++) {
        auto constructor = record.constructors.get(base, i);
        if(!constructor.content || constructor.boxed) continue;

        if(onLayoutStack(walk, constructor.content)) {
            // The payload *is* the back edge - `Just(Tree)` inside `Tree`. There is no field to
            // mark, so the constructor carries it.
            constructor.boxed = true;
            record.constructors.set(base, i, constructor);
            continue;
        }

        auto rewritten = breakCycles(module, constructor.content, walk, source);
        if(rewritten == constructor.content) continue;

        constructor.content = rewritten;
        record.constructors.set(base, i, constructor);
    }

    walk.stack.pop();

    // Only where the walk saw the whole graph. A record reached while another declaration was still
    // being defined has not been checked against that declaration, and remembering it would make the
    // cut depend on which module-level phase happened to reach it first.
    if(!walk.incomplete) record.layoutBroken = true;
}

static TypePtr breakCycles(Module& module, TypePtr type, LayoutWalk& walk, LocationId source) {
    if(!type) return type;

    auto base = *module.types;
    auto value = base[type];

    // Reaching a value through one of these costs a load rather than containment, so the layout of
    // what is on the other side cannot make this one infinite.
    if(value->kind == Type::Ptr || value->kind == Type::Borrow || value->kind == Type::Fun) {
        return type;
    }

    if(value->kind == Type::Tup) return breakTupleCycles(module, *(TupType*)value, walk, source);

    if(value->kind == Type::Record) {
        breakRecordCycles(module, *(RecordType*)value, walk, source);
        return type;
    }

    /*
     * A fixed array is walked through and never rewritten - Implementation-Containers.md §6.
     *
     * `[T *n]` has no indirection to insert. The elements are `n` of the type at a stride and the
     * inline run's address is computed from the owner rather than stored, so there is no edge here
     * that a pointer could be put on: boxing the element would make `[Tree *4]` four pointers, which
     * is a different type from the one that was written, and it would do it silently.
     *
     * So this only descends, and a cycle that reaches back through one is left to checkAcyclic to
     * report. That is the right split - `data T {kids: [T *4]}` has no finite size *whatever* the
     * compiler does to it, so the honest answer is a diagnostic rather than a representation the
     * program did not ask for.
     */
    if(value->kind == Type::Array) {
        breakCycles(module, ((ArrayType*)value)->content, walk, source);
        return type;
    }

    return type;
}

void breakLayoutCycles(Module& module, TypePtr type, LocationId source) {
    // A generic declaration has no layout to be cyclic: `List(a)` is a shape rather than a type, and
    // the indirection belongs to `List(Int)`, which is where the walk will find it.
    if(!type || isGeneric(*module.types, type)) return;

    LayoutWalk walk;
    breakCycles(module, type, walk, source);
}

/*
 * The backstop, and the one layout question that could still be a *source* error.
 *
 * Everything reachable is expected to have been broken by the walk above, so this reports what the
 * walk could not fix rather than what the programmer wrote. It is kept because the alternative to a
 * diagnostic here is an infinite recursion in whichever pass asks for a size next, and because the
 * two walks share the definition of what an edge is - the same three handle kinds, plus a boxed
 * field or constructor, which is a pointer the compiler or the programmer already inserted.
 */
static bool checkAcyclic(Module& module, TypePtr type, TypeList& stack, LocationId source) {
    if(!type) return true;

    auto base = *module.types;
    auto value = base[type];

    // Reaching a value through one of these costs a load rather than containment, so the layout of
    // what is on the other side cannot make this one infinite.
    if(value->kind == Type::Ptr || value->kind == Type::Borrow || value->kind == Type::Fun) {
        return true;
    }

    if(value->kind != Type::Tup && value->kind != Type::Record && value->kind != Type::Array) {
        return true;
    }

    for(auto entry: stack) {
        if(entry != type) continue;

        module.context.diagnostics.error(
            "%@ contains itself without an indirection, so it has no finite size"_v, source,
            describeType(module.context, base, type));
        return false;
    }

    stack.push(type);
    auto ok = true;

    if(value->kind == Type::Tup) {
        for(auto field: ((TupType*)value)->fields.contents(base)) {
            if(field.boxed) continue;
            ok = checkAcyclic(module, field.type, stack, source) && ok;
        }
    } else if(value->kind == Type::Array) {
        // A containment edge like a field's, and the one the walk above deliberately declined to
        // cut: `data T {kids: [T *4]}` is four of a type that contains four of itself, and there is
        // no indirection to insert that would leave it the type that was written. An empty one
        // contains nothing, so it is finite whatever its element is - which is what makes
        // `data T {kids: [T *0]}` a legal, useless declaration rather than a diagnostic.
        auto array = (ArrayType*)value;
        if(writtenCount(base, array->count).from(1)) ok = checkAcyclic(module, array->content, stack, source) && ok;
    } else {
        for(auto constructor: ((RecordType*)value)->constructors.contents(base)) {
            if(constructor.boxed) continue;
            ok = checkAcyclic(module, constructor.content, stack, source) && ok;
        }
    }

    stack.pop();
    return ok;
}

bool checkTypeAcyclic(Module& module, TypePtr type, LocationId source) {
    if(!type || isGeneric(*module.types, type)) return true;

    TypeList stack;
    return checkAcyclic(module, type, stack, source);
}

/*
 * Layout is a property of the declaration, not of one instantiation.
 *
 * A generic body projects into `Maybe(a)` before any `a` is known, so the projection it emits has
 * to be the one every instantiation uses. Deciding Enum/Single/Multi from the constructor list
 * alone gives that: the answer does not move when the arguments are substituted.
 *
 * The one place this costs something is a type variable substituted by `()`. `Box(())` keeps its
 * declaration's Multi layout and a zero-sized payload rather than collapsing to a discriminant,
 * which is a slightly larger value in exchange for `Box(a)` meaning one thing everywhere.
 */
void computeRecordLayout(GlobalBase base, RecordType& record) {
    if(record.constructors.size() == 1) {
        record.layout = RecordType::Single;
        return;
    }

    for(auto constructor: record.constructors.contents(base)) {
        // A generic content counts as a payload: what it substitutes to cannot change the shape.
        if(constructor.content && !isUnit(base, constructor.content)) {
            record.layout = RecordType::Multi;
            return;
        }
    }

    record.layout = RecordType::Enum;
}

U32 naturalStorageBits(U32 bits) {
    if(bits <= 8) return 8;
    if(bits <= 16) return 16;
    if(bits <= 32) return 32;
    return 64;
}

static U32 alignBitsTo(U32 value, U32 unit) {
    return unit ? (value + unit - 1) & ~(unit - 1) : value;
}

/*
 * The recursion, with a depth limit.
 *
 * An aggregate's width is its fields' widths, so this walks the type graph, and a type that reaches
 * itself by inline containment would walk it forever. Resolve reports such a type against its
 * declaration (see checkAcyclic) but resolution continues afterwards so that the rest of the module
 * still produces diagnostics, so this can be asked about one. Answering "not narrow" past a depth no
 * real declaration reaches is what keeps that a reported error rather than a hang - the same reason
 * ReprTable::of carries an in-progress set.
 */
static ValueWidth valueWidthAt(GlobalBase base, TypePtr type, U32 depth);

/*
 * Whether a value of this type may be co-packed at all, before asking whether it is narrow enough.
 *
 * A type whose lifetime or whose copy is a *call* may not: every one of those calls takes the address
 * of the value, and a field that shares a word has no address of its own to give. Handing over the
 * word's would call the operation on the neighbour's bits - and for a record of two such fields, twice
 * on the same ones. So an authored `Copy`, `Sink`, `Reclaim` or `Drop` anywhere inside a type keeps it
 * out of a shared word, and costs it the byte it would have saved.
 *
 * `ownershipReady` is what makes this askable here at all: ownership is a whole-program property
 * cached on the type, and by the time a target lays anything out every reachable type has one. A
 * caller during resolution may see one that does not yet, and gets the permissive answer - which is
 * the safe direction, since a target may always pack fewer fields than it was offered.
 */
static bool packableValue(GlobalBase base, TypePtr type) {
    if(!type) return false;

    auto value = base[type];
    if(!value->ownershipReady) return true;

    auto& ownership = value->ownership;
    return ownership.trivialCopy && ownership.trivialSink && !ownership.needsTeardown();
}

// The bit offsets of every field of an aggregate that has a scalar form, or nothing where it has
// none. Shared by `valueWidth`, which needs only the span, and by repr, which needs the offsets.
static bool scalarBits(GlobalBase base, TupType& tuple, U32 depth, PackedRun& run, PackOffsets* offsets) {
    // A pinned layout has no scalar form. Its whole purpose is that its fields sit where a C
    // compiler put them, and a scalar is the compiler choosing.
    if(tuple.layout != TypeLayout::Auto) return false;

    auto count = tuple.fields.size();
    if(!count) return false;

    // Every field has to be narrow, because a scalar aggregate *is* a run of co-packed fields and a
    // field that fills its own storage is not one. That is what keeps `{a: U8, b: U8}` two bytes
    // rather than two bit-fields of a word, and it is why `data Small = A(U8) | B(U8)` - a tag bit
    // over a full-width payload - is a separate feature rather than this one.
    PackOrder order;
    for(U16 i = 0; i < count; i++) {
        auto field = tuple.fields.get(base, i);

        // A boxed field is a whole pointer, which is not narrow on any target this compiler emits
        // for - so an aggregate holding one has no scalar form. Answering here rather than through
        // `valueWidthAt` is also what keeps this walk finite over a recursive type: what is on the
        // other side of a box has no bearing on the width of the thing holding it.
        if(field.boxed) return false;
        if(!packableValue(base, field.type)) return false;
        if(!valueWidthAt(base, field.type, depth + 1).isNarrow()) return false;

        order.push(i);
    }

    packOrder(base, tuple, order);

    PackOffsets placed;
    run = packBits(base, tuple, toBuffer(order), kMaxPackBits, offsets ? &placed : nullptr);

    // A run too long for one word is not a scalar, and one that exactly fills its storage is not a
    // *narrow* one - it has no bits left to be packed into a neighbour with, so calling it narrow
    // would only cost every borrow of it a shift it does not need.
    if(run.count != count || run.span >= naturalStorageBits(run.span)) return false;

    // Reported by field index rather than in placement order, so that a caller laying the fields out
    // needs nothing from the ordering above. Two places deciding the same permutation is two places
    // that can disagree about where a field went.
    if(offsets) {
        for(Size i = 0; i < count; i++) offsets->push(0);
        for(Size at = 0; at < placed.size(); at++) (*offsets)[order[at]] = placed[at];
    }

    return true;
}

static ValueWidth valueWidthAt(GlobalBase base, TypePtr type, U32 depth) {
    if(!type || depth > 8) return {};

    auto value = base[type];
    switch(value->kind) {
        case Type::Int: {
            auto bits = U32(((IntType*)value)->bits);
            return ValueWidth { bits, naturalStorageBits(bits) };
        }
        case Type::Tup: {
            PackedRun run;
            auto tuple = (TupType*)value;
            if(!scalarBits(base, *tuple, depth, run, nullptr)) return {};

            return ValueWidth { run.span, naturalStorageBits(run.span) };
        }
        case Type::Record: {
            auto record = (RecordType*)value;

            // A payload-free sum *is* its discriminant, so what it needs is the bits its constructor
            // count needs against the storage those bits are held in - one byte for a `Bool`, the
            // same rule an integer of that width answers by, and the same one Repr sizes an enum at.
            if(record->layout == RecordType::Enum) {
                auto count = record->constructors.size();
                U32 bits = 1;
                while((Size(1) << bits) < count) bits++;

                return ValueWidth { bits, naturalStorageBits(bits) };
            }

            /*
             * A single-constructor record is its content, which is what makes a record of two
             * `Bool`s a two-bit value and `Maybe(Flags)` one byte.
             *
             * A record with several *payload-carrying* constructors is not: its discriminant would
             * have to become a bit range of the same word and its constructors would have to overlap
             * inside it, which is a second feature and not this one. A payload-free sum took the
             * branch above, so what falls through here is the case with something to overlap.
             */
            if(record->layout != RecordType::Single) return {};

            auto constructors = record->constructors.contents(base);
            if(!constructors.size()) return {};

            // A boxed payload makes the newtype a pointer, which is not narrow - and asking about
            // its target would walk a recursive declaration forever.
            if(constructors[0].boxed) return {};

            return valueWidthAt(base, constructors[0].content, depth + 1);
        }
        case Type::Vector: {
            /*
             * A vector states its width and is never narrow, which is the answer that matters.
             *
             * `isNarrow` is what decides whether a value may be co-packed into a word with its
             * neighbours and whether a `&` of it carries a shift, and a vector is neither: it fills
             * its own storage by construction and it is not a word at all. Its own arm rather than
             * the `default` below, because falling through to "no answer" would be the same *result*
             * arrived at by not having thought about it - and the two look identical until a target
             * with a wider pack budget raises kMaxPackBits.
             */
            auto vector = (VectorType*)value;
            auto bits = U32(laneStride(base, vector->content) * 8 * constValue(base, vector->count));
            return ValueWidth { bits, bits };
        }
        default:
            return {};
    }
}

ValueWidth valueWidth(GlobalBase base, TypePtr type) {
    return valueWidthAt(base, type, 0);
}

/*
 * Whether a field of a pinned aggregate is a bit-field at all.
 *
 * Under `C` only a written refinement is one - `@bits(4) Int` is `int x: 4`, and a `Bool` is a whole
 * `_Bool` rather than one bit of a byte. That distinction does not exist under `Auto`, where every
 * narrow value is a candidate and a `Bool` costing a bit rather than a byte is the point; it exists
 * here because a C header has both spellings and they lay out differently.
 */
static bool isBitField(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Int && ((IntType*)base[type])->canonical != nullptr;
}

U32 declaredUnitBits(GlobalBase base, TypePtr type) {
    if(!isBitField(base, type)) return 0;

    auto canonical = canonicalType(base, type);
    if(base[canonical]->kind != Type::Int) return 0;

    return naturalStorageBits(U32(((IntType*)base[canonical])->bits));
}

// The unit a bit-field is allocated in. Its own natural storage, or - under a pinned layout - the
// storage of the type it was written as a refinement of, which is the unit C uses.
static U32 packUnitBits(GlobalBase base, TupType& tuple, TypePtr type, U32 bits) {
    if(tuple.layout != TypeLayout::C) return naturalStorageBits(bits);

    auto declared = declaredUnitBits(base, type);
    return declared ? declared : naturalStorageBits(bits);
}

PackedRun packBits(GlobalBase base, TupType& tuple, Buffer<const U16> order, U32 maxBits,
                   PackOffsets* offsets) {
    PackedRun run;

    for(auto index: order) {
        auto type = tuple.fields.get(base, index).type;
        auto width = valueWidth(base, type);
        if(!width.logical) break;

        auto unit = packUnitBits(base, tuple, type, width.logical);
        auto at = run.span;

        // Bumped to the next unit boundary where it would otherwise cross one. Measured before the
        // budget check, since the bump is what decides whether the field still fits.
        if(at / unit != (at + width.logical - 1) / unit) at = alignBitsTo(at, unit);
        if(at + width.logical > maxBits) break;

        if(offsets) offsets->push(at);
        run.span = at + width.logical;
        run.count++;
    }

    return run;
}

void packOrder(GlobalBase base, TupType& tuple, PackOrder& into) {
    if(tuple.layout != TypeLayout::Auto) return;

    // Insertion sort, descending by width and stable within one - the lists are a handful of fields
    // long, and stability is what makes the layout of two same-width fields the declaration's
    // business rather than the sort's.
    for(Size i = 1; i < into.size(); i++) {
        auto index = into[i];
        auto width = valueWidth(base, tuple.fields.get(base, index).type).logical;
        auto at = i;

        while(at > 0 && valueWidth(base, tuple.fields.get(base, into[at - 1]).type).logical < width) {
            into[at] = into[at - 1];
            at--;
        }

        into[at] = index;
    }
}

bool packCandidate(GlobalBase base, TupType& tuple, U16 index) {
    auto count = tuple.fields.size();
    if(index >= count) return false;

    auto narrowAt = [&](Size at) {
        auto field = tuple.fields.get(base, at);

        // A boxed field is a pointer with an address of its own, which is most of what boxing one is
        // for. Co-packing it would take that address away, and there is nothing narrow about it to
        // pack in the first place.
        if(field.boxed) return false;
        return packableValue(base, field.type) && valueWidth(base, field.type).isNarrow();
    };

    if(!narrowAt(index)) return false;

    /*
     * A pinned layout keeps the declaration's order, so a bit-field's neighbours are the ones it was
     * written next to - `{a: @bits(4), b: U64, c: @bits(4)}` allocates two units, as C does - and only
     * a written refinement shares a unit with anything at all.
     */
    if(tuple.layout == TypeLayout::C) {
        auto fieldAt = [&](Size at) {
            auto type = tuple.fields.get(base, at).type;
            return isBitField(base, type) && valueWidth(base, type).isNarrow();
        };

        if(!fieldAt(index)) return false;
        return (index > 0 && fieldAt(index - 1)) || (Size(index) + 1 < count && fieldAt(index + 1));
    }

    // A `@layout(js)` record keeps one property per field, which is the whole content of the pin, so
    // nothing in it shares with anything. `placementOrder` already declines to group a pinned tuple,
    // but this is the answer the *borrow* tier asks for - see expr_construct's use - and a field that
    // is not packed must not be borrowed as though it were.
    if(tuple.layout == TypeLayout::Js) return false;

    // An auto layout reorders, so anything else narrow in the tuple is a neighbour.
    for(Size at = 0; at < count; at++) {
        if(at != index && narrowAt(at)) return true;
    }

    return false;
}

// The scalar form of an aggregate, for whoever is laying it out. The span and the offsets come from
// the same placement `valueWidth` reported, which is what makes the mask a callee applies to a
// reference to the whole aggregate the same width the fields were placed within.
bool scalarLayout(GlobalBase base, TupType& tuple, PackedRun& run, PackOffsets* offsets) {
    return scalarBits(base, tuple, 0, run, offsets);
}

U64 floatBits(GlobalBase base, TypePtr type, F64 value) {
    U64 bits = 0;

    if(isFloat(base, type) && ((FloatType*)base[type])->width == FloatType::Float) {
        auto single = F32(value);
        copy((const Byte*)&single, (Byte*)&bits, sizeof(single));
    } else {
        copy((const Byte*)&value, (Byte*)&bits, sizeof(value));
    }

    return bits;
}

F64 floatFromBits(GlobalBase base, TypePtr type, U64 bits) {
    if(isFloat(base, type) && ((FloatType*)base[type])->width == FloatType::Float) {
        F32 single;
        copy((const Byte*)&bits, (Byte*)&single, sizeof(single));
        return F64(single);
    }

    F64 number;
    copy((const Byte*)&bits, (Byte*)&number, sizeof(number));
    return number;
}
