#include "lower_split.h"
#include "lower_inst.h"
#include "lower_builder.h"
#include "lower_promote.h"

namespace {

// "Not one of the slots this pass is looking at", which is what both ends of most copies are.
constexpr Size kNoSlot = maxLimit<Size>;

/*
 * Two bounds, and neither is a judgement about what is worth splitting.
 *
 * They are there so that one memory operation can never become a hundred: a slot wider than the
 * first is left whole, and one that comes apart into more pieces than the second is put back. What
 * the front end emits for a record reaches neither - the largest aggregate in the resolve corpus is
 * 64 bytes over six fields - so what they bound is a function this compiler does not write.
 */
constexpr U32 kMaxSlotBytes = 512;
constexpr Size kMaxFields = 16;

// The constant an operand carries, where it is one.
Maybe<U64> constantOf(LowerBase base, LowerPtr<LowerValue> value) {
    auto inst = base[value]->inst();
    if(inst->kind != LowerInst::Imm) return Nothing();

    return Just(((LowerImm*)inst)->i);
}

// One load or store of a slot, at the offset the address walk arrived at it with. A copy is not one
// of these - it names two places and belongs to both of them, so it is an Edge below.
struct Access {
    LowerPtr<LowerInst> inst = nullptr;
    U32 offset = 0;
    U32 size = 0;
    bool writes = false;
    LowerType type = LowerType::Int64;
};

/*
 * One cell of a slot's partition, and everything the rewrite has to know about it.
 *
 * `live` is whether anything can observe what it holds - see the header, where the rule that a copy
 * from outside makes a cell live is argued. `type` is the register the cell's accesses agree on,
 * gathered from this slot and from every slot a copy relates it to; `conflicted` is two of them
 * disagreeing, which is not an error and only means the cell is moved as bytes.
 */
struct Field {
    U32 offset = 0;
    U32 size = 0;
    bool live = false;
    bool typed = false;
    bool conflicted = false;

    /*
     * Whether anything anywhere puts a value into this cell - a store of its own, or a copy carrying
     * one in from a cell that has one.
     *
     * The cells this is false for are the payload of an absent `Option` and the branches of a sum
     * nobody took: live, because a copy relates them to a cell something does read, and holding
     * nothing, because no path writes them. Left alone they become an allocation and a load of the
     * frame's own leftovers, which is what the slot already was and is one thing promotion will not
     * take - a register cannot hold what was never put in it, so `surveySlot` refuses the whole cell
     * and the chain stops there.
     *
     * So a cell nothing writes is written once, with zero, where it is allocated. The bytes were
     * unspecified and are now zero, which no program can tell apart, and the difference it makes is
     * that the cell promotes: the copy that read it folds to a constant and the phi below it says 0
     * on the arm that never had a value.
     */
    bool written = false;
    LowerType type = LowerType::Int64;

    // The allocation this cell became, once the rewrite has made one. Null while the cell is dead.
    LowerPtr<LowerValue> address = nullptr;
};

struct Slot {
    LowerPtr<LowerValue> address = nullptr;
    LowerPtr<LowerInst> allocation = nullptr;
    U32 size = 0;
    U32 alignment = 1;

    // Whether this is still a slot the pass will split. Cleared by the address walk for a slot whose
    // address escapes, and by the validation below for one whose partition does not fit its own
    // accesses - and a slot that stops being split is one its copy partners then treat as ordinary
    // memory, which is why the two decisions are taken to a fixpoint rather than once.
    bool active = true;

    Array<LowerPtr<LowerInst>> offsets;  // the constant-offset arithmetic over the address
    Array<Access> accesses;
    Array<U32> points;                   // the cut offsets, ascending, 0 and `size` included
    Array<Field> fields;
};

// One copy with at least one end inside a slot. Both ends are recorded whichever they are, since
// what makes the rewrite of one end correct is what the other end turned out to be.
struct Edge {
    LowerPtr<LowerInst> inst = nullptr;
    Size to = kNoSlot;
    Size from = kNoSlot;
    U32 toOffset = 0;
    U32 fromOffset = 0;
    U32 count = 0;
};

/*
 * Every use of a slot's address, checked for being one this pass can account for and rewrite in
 * full, and recorded as it goes.
 *
 * The shape is `walkAddress` in lower_forward.cpp and the conclusion is the same: an access through
 * the address is what the storage is for, a constant offset from it is a further address and is
 * followed, and everything else is the address leaving - as a call argument, as a value stored into
 * memory, as a copy's byte count, as an operand of arithmetic that is not a constant offset. What
 * differs is that this one keeps what it saw, because the offsets are what the partition is made of
 * and the accesses are what has to fit inside it.
 *
 * `location` is written into as the walk goes and is *not* undone when the walk then refuses. An
 * entry naming a slot that turned out not to be split is read everywhere as "ordinary memory",
 * which is exactly what a refused slot is, so a partial closure costs nothing but the lookup.
 */
bool collectUses(LowerBase base, Slot& slot, Size which, HashMap<U32, U64>& location,
                 LowerValue* address, U32 offset, U32 depth) {
    // A pointer this far from its allocation is not a shape lowering emits, and a budget is what
    // keeps one question from walking a dataflow graph.
    if(depth > 8) return false;

    location.add(U32(address - base), (U64(which) << 32) | offset);
    auto self = address - base;

    for(auto userPtr: address->uses.contents(base)) {
        auto user = base[userPtr];

        if(user->kind == LowerInst::Add) {
            auto add = (LowerInstBinary*)user;

            // Either operand may be the address: nothing has canonicalized the order yet, and a
            // pointer add is written both ways round by different parts of lowering.
            auto step = add->lhs == self ? constantOf(base, add->rhs)
                      : add->rhs == self ? constantOf(base, add->lhs)
                      : Nothing();

            if(step.isNothing() || step.unwrap() >= slot.size - offset) return false;

            slot.offsets.push(userPtr);
            if(!collectUses(base, slot, which, location, add->created().ptr,
                            offset + U32(step.unwrap()), depth + 1)) {
                return false;
            }

            continue;
        }

        if(user->kind == LowerInst::Load) {
            auto loadInst = (LowerInstLoad*)user;

            // A load that reads past what it names, deliberately, is the one thing a cell-sized
            // allocation cannot promise - see LowerInstLoad::isOverread.
            if(loadInst->isOverread()) return false;

            auto width = loadInst->getWidth();
            if(width == 0 || width > slot.size - offset) return false;

            slot.accesses.push(Access { userPtr, offset, width, false,
                                        loadInst->created().ptr->type });
            continue;
        }

        if(user->kind == LowerInst::Store) {
            auto storeInst = (LowerInstStore*)user;

            // Storing the address *into* the slot is the address escaping into memory, and is not
            // the same thing as writing the slot.
            if(storeInst->to != self || storeInst->value == self) return false;

            auto width = storeInst->getWidth();
            if(width == 0 || width > slot.size - offset) return false;

            slot.accesses.push(Access { userPtr, offset, width, true,
                                        base[storeInst->value]->type });
            continue;
        }

        if(user->kind == LowerInst::Copy) {
            auto copyInst = (LowerInstCopy*)user;

            // The address used as a *number* is an address that outlives the storage, and a copy of
            // a place onto itself is not a move between two of them.
            if(copyInst->count == self) return false;
            if(copyInst->to == self && copyInst->from == self) return false;

            // A run-time length is memory by construction: nothing here can say which cells it
            // lands on. The edge itself is recorded once both ends are known.
            auto count = constantOf(base, copyInst->count);
            if(count.isNothing() || count.unwrap() == 0 || count.unwrap() > slot.size - offset) {
                return false;
            }

            continue;
        }

        /*
         * Everything else, which is the address leaving - and one thing that is not.
         *
         * A `setpattern` fills the slot with a byte, and splitting it would mean one fill per cell
         * and a cell that then has a value nothing typed. It is declined rather than handled because
         * the front end writes a zeroed record as a run of stores (`zeroStorage` in lower_gen.cpp),
         * so the whole of what this gives up is one instruction in one program.
         */
        return false;
    }

    return true;
}

/*
 * The slots worth trying, which are the fixed-size allocations whose address does not escape.
 *
 * Every one of them is kept, including the ones the walk refused, so that a slot index means the
 * same thing before and after a refusal - and so that an address in a refused slot's closure still
 * resolves, to a slot that everything below reads as ordinary memory.
 */
void collectSlots(LowerBase base, LowerFunction& fun, Array<Slot>& slots,
                  HashMap<U32, U64>& location) {
    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];

        for(auto instPtr: block->instructions.contents(base)) {
            auto inst = base[instPtr];
            if(inst->kind != LowerInst::Alloca) continue;

            // A variable-sized allocation is memory by construction, and one too large to be a
            // record is not what this pass is for.
            auto allocation = (LowerInstAlloca*)inst;
            auto size = constantOf(base, allocation->byteCount);
            if(size.isNothing() || size.unwrap() == 0 || size.unwrap() > kMaxSlotBytes) continue;

            Slot slot;
            slot.address = allocation->created().ptr - base;
            slot.allocation = instPtr;
            slot.size = U32(size.unwrap());
            slot.alignment = allocation->alignment;

            auto which = slots.size();
            slot.active = collectUses(base, slot, which, location,
                                      allocation->created().ptr, 0, 0);
            slots.push(::move(slot));
        }
    }
}

// Every copy that names a slot at either end, with each end resolved to the slot and offset it lands
// on. A copy whose length is not a constant cannot name an accepted slot - the walk above refused
// one that did - so there is nothing to reject here and nothing to record either.
void collectEdges(LowerBase base, LowerFunction& fun, Array<Slot>& slots,
                  HashMap<U32, U64>& location, Array<Edge>& edges, HashMap<U32, U32>& edgeOf) {
    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];

        for(auto instPtr: block->instructions.contents(base)) {
            auto inst = base[instPtr];
            if(inst->kind != LowerInst::Copy) continue;

            auto copyInst = (LowerInstCopy*)inst;
            auto to = location.getValue(U32(copyInst->to));
            auto from = location.getValue(U32(copyInst->from));
            if(to.isNothing() && from.isNothing()) continue;

            auto count = constantOf(base, copyInst->count);
            if(count.isNothing()) continue;

            Edge edge;
            edge.inst = instPtr;
            edge.count = U32(count.unwrap());

            if(to) {
                edge.to = Size(to.unwrap() >> 32);
                edge.toOffset = U32(to.unwrap());
            }

            if(from) {
                edge.from = Size(from.unwrap() >> 32);
                edge.fromOffset = U32(from.unwrap());
            }

            // A copy from one part of a slot into another part of the same slot moves bytes within
            // storage that is about to stop existing, and the two ends' cells need not line up.
            if(edge.to != kNoSlot && edge.to == edge.from) slots[edge.to].active = false;

            edgeOf.add(U32(instPtr), U32(edges.size()));
            edges.push(::move(edge));
        }
    }
}

// One cut offset, kept in ascending order so that a cell is a pair of neighbours.
bool addPoint(Array<U32>& points, U32 at) {
    for(Size i = 0; i < points.size(); i++) {
        if(points[i] == at) return false;
        if(points[i] > at) {
            points.insert(i, at);
            return true;
        }
    }

    points.push(at);
    return true;
}

// Every cut point of one end of a copy, carried across to the other. This is what gives a slot the
// structure of a record it never names a field of - see the header.
bool mirrorPoints(Slot& from, U32 fromOffset, Slot& to, U32 toOffset, U32 count) {
    auto changed = false;

    for(Size i = 0; i < from.points.size(); i++) {
        auto point = from.points[i];
        if(point <= fromOffset || point >= fromOffset + count) continue;

        changed |= addPoint(to.points, toOffset + (point - fromOffset));
    }

    return changed;
}

/*
 * The partition, as the least fixpoint of "a cut point on either end of a copy is a cut point on
 * the other".
 *
 * Rebuilt from nothing each time a slot is refused rather than being edited, because a refused slot
 * stops contributing its structure to its partners: the cuts it carried across are cuts nothing asks
 * for any more, and leaving them in would split a partner's field for no reason and refuse a slot
 * that had nothing wrong with it.
 */
void computePoints(Array<Slot>& slots, Array<Edge>& edges) {
    for(auto& slot: slots) {
        slot.points.clear();
        if(!slot.active) continue;

        slot.points.push(0);
        slot.points.push(slot.size);

        for(auto& access: slot.accesses) {
            addPoint(slot.points, access.offset);
            addPoint(slot.points, access.offset + access.size);
        }
    }

    for(auto& edge: edges) {
        if(edge.to != kNoSlot && slots[edge.to].active) {
            addPoint(slots[edge.to].points, edge.toOffset);
            addPoint(slots[edge.to].points, edge.toOffset + edge.count);
        }

        if(edge.from != kNoSlot && slots[edge.from].active) {
            addPoint(slots[edge.from].points, edge.fromOffset);
            addPoint(slots[edge.from].points, edge.fromOffset + edge.count);
        }
    }

    auto changed = true;
    while(changed) {
        changed = false;

        for(auto& edge: edges) {
            if(edge.to == kNoSlot || edge.from == kNoSlot) continue;
            if(!slots[edge.to].active || !slots[edge.from].active) continue;

            auto& to = slots[edge.to];
            auto& from = slots[edge.from];

            changed |= mirrorPoints(to, edge.toOffset, from, edge.fromOffset, edge.count);
            changed |= mirrorPoints(from, edge.fromOffset, to, edge.toOffset, edge.count);
        }
    }
}

/*
 * Which slots the partition actually fits, and whether any of them changed their mind.
 *
 * Two things refuse a slot. An access that straddles a cell is one the split cannot reproduce -
 * reading a word over two fields written separately, which is what a `@bits` pair reaches this stage
 * as - and a partition of one cell is the slot it already was, so splitting it would rename storage
 * and buy nothing.
 */
bool validateSlots(Array<Slot>& slots) {
    auto stable = true;

    for(auto& slot: slots) {
        if(!slot.active) continue;

        auto cells = slot.points.size() - 1;
        auto usable = cells >= 2 && cells <= kMaxFields;

        for(Size i = 0; usable && i < slot.accesses.size(); i++) {
            auto& access = slot.accesses[i];
            auto found = slot.points.findIndex(access.offset);

            usable = found.isJust() && slot.points[found.unwrap() + 1] == access.offset + access.size;
        }

        if(usable) continue;

        slot.active = false;
        stable = false;
    }

    return stable;
}

Size fieldAt(Slot& slot, U32 offset) {
    for(Size i = 0; i < slot.fields.size(); i++) {
        if(slot.fields[i].offset == offset) return i;
    }

    // A partition every access and every copy end was cut to fit has a cell starting wherever one of
    // them starts, so arriving here is this pass having lost track of one of its own offsets.
    assertTrue(false);
    return 0;
}

// One access's opinion about which register the cell belongs in. A type too narrow to hold the whole
// cell is not one - a four-byte value stored into an eight-byte cell would be, and there is no such
// access, but the rewrite must not be the thing that finds that out.
void mergeFieldType(Field& field, LowerType type) {
    if(field.conflicted) return;

    if(registerBits(type) < field.size * 8) {
        field.conflicted = true;
        field.typed = false;
        return;
    }

    if(!field.typed) {
        field.typed = true;
        field.type = type;
        return;
    }

    if(field.type == type) return;

    field.conflicted = true;
    field.typed = false;
}

/*
 * What the *other* end of a copy says the register is, where that end is a slot this pass collected
 * and is not splitting.
 *
 * A slot that is not being split still says what its bytes are - it is one cell as far as anything
 * here is concerned, and its own loads and stores name the register they go through. Reading that is
 * what keeps the move this pass writes from disagreeing with the accesses on the other side, and the
 * disagreement is not a small thing: two types on one slot is exactly what stops promotion taking
 * it, so a move typed by the default would turn a slot that promoted into one that does not.
 *
 * `Box(a)` is the everyday shape. The pointer is built in an eight-byte temporary, copied into the
 * payload of a 24-byte record and copied out again into the local the pattern bound - and neither of
 * those two is split, one for being a single cell and the other for the same reason, so between them
 * the payload had nothing at all to say about being a pointer.
 */
void mergeAcrossEdge(Slot& into, U32 offset, Slot& other, U32 otherOffset, U32 count) {
    for(auto& field: into.fields) {
        if(field.offset < offset || field.offset >= offset + count) continue;

        auto at = otherOffset + (field.offset - offset);
        for(auto& access: other.accesses) {
            if(access.offset != at || access.size != field.size) continue;

            mergeFieldType(field, access.type);
        }
    }
}

// What two cells a copy relates have to agree about: that either is observable if the other is, and
// which register they are both read into.
bool shareField(Field& a, Field& b) {
    auto changed = false;

    if(a.live != b.live) {
        a.live = b.live = true;
        changed = true;
    }

    if(a.conflicted != b.conflicted) {
        a.conflicted = b.conflicted = true;
        a.typed = b.typed = false;
        return true;
    }

    if(a.conflicted) return changed;

    if(a.typed && !b.typed) {
        b.typed = true;
        b.type = a.type;
        return true;
    }

    if(b.typed && !a.typed) {
        a.typed = true;
        a.type = b.type;
        return true;
    }

    if(a.typed && a.type != b.type) {
        a.conflicted = b.conflicted = true;
        a.typed = b.typed = false;
        return true;
    }

    return changed;
}

/*
 * The cells themselves: what each one holds, and whether anything can tell.
 *
 * A cell an access names is live and typed by it. A cell a copy from *outside* lands on is live
 * whatever else is true of it, because what it holds then came from somewhere this pass cannot see
 * and is not the frame's own uninitialized bytes - which is the one rule the header's induction
 * rests on. Everything else is what the copies then relate, taken to a fixpoint: liveness and the
 * register type both travel across a copy, in both directions.
 */
void computeFields(Array<Slot>& slots, Array<Edge>& edges) {
    for(auto& slot: slots) {
        slot.fields.clear();
        if(!slot.active) continue;

        for(Size i = 0; i + 1 < slot.points.size(); i++) {
            Field field;
            field.offset = slot.points[i];
            field.size = slot.points[i + 1] - slot.points[i];
            slot.fields.push(::move(field));
        }

        for(auto& access: slot.accesses) {
            auto& field = slot.fields[fieldAt(slot, access.offset)];
            field.live = true;
            field.written |= access.writes;
            mergeFieldType(field, access.type);
        }
    }

    // A copy that brings bytes in from somewhere this pass cannot see. Both things it says are said
    // here: the cells it lands on hold what a sender meant by them, so they are live, and they hold
    // something rather than nothing, so they are written.
    for(auto& edge: edges) {
        if(edge.to == kNoSlot || !slots[edge.to].active) continue;
        if(edge.from != kNoSlot && slots[edge.from].active) continue;

        for(auto& field: slots[edge.to].fields) {
            if(field.offset < edge.toOffset) continue;
            if(field.offset >= edge.toOffset + edge.count) continue;

            field.live = true;
            field.written = true;
        }
    }

    // And what the far end of such a copy reads its bytes through, where it is a slot at all - see
    // mergeAcrossEdge. Before the fixpoint, so that a type arriving this way then travels the copies
    // like any other.
    for(auto& edge: edges) {
        auto toActive = edge.to != kNoSlot && slots[edge.to].active;
        auto fromActive = edge.from != kNoSlot && slots[edge.from].active;
        if(toActive == fromActive) continue;

        auto other = toActive ? edge.from : edge.to;
        if(other == kNoSlot) continue;

        if(toActive) {
            mergeAcrossEdge(slots[edge.to], edge.toOffset, slots[other], edge.fromOffset, edge.count);
        } else {
            mergeAcrossEdge(slots[edge.from], edge.fromOffset, slots[other], edge.toOffset, edge.count);
        }
    }

    auto changed = true;
    while(changed) {
        changed = false;

        for(auto& edge: edges) {
            if(edge.to == kNoSlot || edge.from == kNoSlot) continue;
            if(!slots[edge.to].active || !slots[edge.from].active) continue;

            auto& to = slots[edge.to];
            auto& from = slots[edge.from];

            for(Size i = 0; i < to.fields.size(); i++) {
                auto offset = to.fields[i].offset;
                if(offset < edge.toOffset || offset >= edge.toOffset + edge.count) continue;

                auto other = fieldAt(from, edge.fromOffset + (offset - edge.toOffset));
                changed |= shareField(to.fields[i], from.fields[other]);

                // Being written travels the way the bytes do and no further: a copy gives the
                // destination whatever the source had, and says nothing about the source.
                if(from.fields[other].written && !to.fields[i].written) {
                    to.fields[i].written = true;
                    changed = true;
                }
            }
        }
    }
}

/*
 * The register a cell is moved through, where there is one.
 *
 * This is `promotableSlot` asked in advance, and the two have to be the same question: a cell moved
 * as a load and a store is a cell whose storage promotion will then hold in a register, and a cell
 * it would decline is one there is nothing to gain by typing. A cell nothing types at all still gets
 * an integer of its width, which is what turns a chain that merely passes a payload along into a
 * chain of registers - there is no access anywhere to say the bits are anything else.
 */
Maybe<LowerType> moveType(const Field& field) {
    if(field.conflicted) return Nothing();

    if(field.typed) {
        return promotableSlot(field.size, field.type) ? justType(field.type) : Nothing();
    }

    if(field.size == 1 || field.size == 2 || field.size == 4) return justType(LowerType::Int32);
    if(field.size == 8) return justType(LowerType::Int64);

    return Nothing();
}

// What the byte at this offset was already aligned to, which is the most the split may promise about
// it - and at least what a value of the cell's own width asks for, since a fresh allocation is free
// to be aligned better than the storage it was cut out of.
U32 fieldAlignment(const Slot& slot, const Field& field) {
    auto inherited = field.offset == 0
        ? slot.alignment
        : min(slot.alignment, field.offset & ~(field.offset - 1));

    auto natural = U32(1);
    while(natural < field.size && natural < 16) natural *= 2;

    return max(U32(1), max(inherited, natural));
}

/*
 * The first of the two rewrites: one allocation per live cell, where the slot's own allocation was.
 *
 * Its position is what makes the rest of it work. An `alloca` dominates every use of the address it
 * names, so cutting the replacements in at exactly that point leaves every one of them dominating
 * the accesses that are about to name it - including the ones in other blocks, and including the
 * ones a copy relating two slots reaches from the second slot's side.
 *
 * All of them before any access is rewritten, for the same reason and one more: a copy names two
 * slots, and the block it stands in is not required to be either allocation's.
 */
void createFields(LowerBase base, LowerModule& module, LowerBlock& block, Array<Slot>& slots,
                  HashMap<U32, U32>& allocaOf) {
    SmallArray<LowerPtr<LowerInst>, 32> original;
    auto touched = false;

    for(auto instPtr: block.instructions.contents(base)) {
        original.push(instPtr);
        if(allocaOf.getValue(U32(instPtr))) touched = true;
    }

    if(!touched) return;
    block.instructions.clear();

    for(auto instPtr: original) {
        auto found = allocaOf.getValue(U32(instPtr));
        if(found.isNothing()) {
            block.instructions.push(module.arena, instPtr);
            continue;
        }

        auto inst = base[instPtr];
        auto& slot = slots[found.unwrap()];
        detach(base, inst);

        for(auto& field: slot.fields) {
            if(!field.live) continue;

            auto count = new (module.arena) LowerImm(StringId(), LowerType::Int64, U64(field.size));
            count->source = inst->source;
            block.addInst(base, count);

            auto storage = new (module.arena) LowerInstAlloca(
                StringId(), count->created().ptr - base, fieldAlignment(slot, field));
            storage->source = inst->source;
            block.addInst(base, storage);

            field.address = storage->created().ptr - base;

            // A cell nothing ever puts a value into, given one - see Field::written. Only where a
            // register holds it, since the whole of the point is that promotion then takes it, and
            // an initializer promotion declines is a store this pass has added for nothing. And a
            // scalar one: a vector zero is a `vsplat` rather than an immediate, and a vector cell
            // nothing writes is not a shape any of this arose from.
            auto type = field.written ? Nothing() : moveType(field);
            if(type.isNothing() || isVectorLike(type.unwrap())) continue;

            auto zero = new (module.arena) LowerImm(StringId(), type.unwrap(), U64(0));
            zero->source = inst->source;
            block.addInst(base, zero);

            auto initial = new (module.arena) LowerInstStore(
                field.address, zero->created().ptr - base, field.size);
            initial->source = inst->source;
            block.addInst(base, initial);
        }
    }
}

// An address a constant distance from another, for the end of a copy that is not a slot: there the
// cells are the other end's and this side is written into wherever they land.
LowerPtr<LowerValue> offsetAddress(LowerBase base, LowerModule& module, LowerBlock& block,
                                   LowerPtr<LowerValue> address, U32 step, LocationId source) {
    if(step == 0) return address;

    auto offset = new (module.arena) LowerImm(StringId(), LowerType::Int64, U64(step));
    offset->source = source;
    block.addInst(base, offset);

    return binary<LowerInst::Add>(base, module, block, base[address], offset->created().ptr,
                                  LowerType::Pointer, StringId())->created().ptr - base;
}

// One cell, moved. Through a register where the cell has one, and as bytes where it does not - see
// moveType, which is the whole of the decision.
void moveField(LowerBase base, LowerModule& module, LowerBlock& block, LowerPtr<LowerValue> to,
               LowerPtr<LowerValue> from, const Field& field, LocationId source) {
    if(auto type = moveType(field)) {
        auto loaded = load(base, module, block, base[from], field.size, false, type.unwrap(),
                           StringId());
        loaded->source = source;

        auto stored = new (module.arena) LowerInstStore(to, loaded->created().ptr - base, field.size);
        stored->source = source;
        block.addInst(base, stored);
        return;
    }

    auto count = new (module.arena) LowerImm(StringId(), LowerType::Int64, U64(field.size));
    count->source = source;
    block.addInst(base, count);

    auto copied = new (module.arena) LowerInstCopy(to, from, count->created().ptr - base);
    copied->source = source;
    block.addInst(base, copied);
}

/*
 * One copy, rewritten as a move per cell.
 *
 * Either end may be the one that names the cells - they are the same cells wherever both ends are
 * slots, which is what the partition fixpoint established - so the enumeration is over whichever end
 * is being split and the other side is addressed at the same relative offsets.
 *
 * The dead cells are simply not moved. Where the destination is a slot that is what they are for;
 * where it is not, this is the copy leaving the destination's own bytes where they were, which the
 * header argues is not a difference a program can observe.
 */
bool expandCopy(LowerBase base, LowerModule& module, LowerBlock& block, Array<Slot>& slots,
                Edge& edge) {
    auto toActive = edge.to != kNoSlot && slots[edge.to].active;
    auto fromActive = edge.from != kNoSlot && slots[edge.from].active;
    if(!toActive && !fromActive) return false;

    auto copyInst = (LowerInstCopy*)base[edge.inst];
    auto to = copyInst->to;
    auto from = copyInst->from;
    auto source = copyInst->source;

    detach(base, copyInst);

    auto& lead = toActive ? slots[edge.to] : slots[edge.from];
    auto leadOffset = toActive ? edge.toOffset : edge.fromOffset;

    for(Size i = 0; i < lead.fields.size(); i++) {
        auto offset = lead.fields[i].offset;
        if(offset < leadOffset || offset >= leadOffset + edge.count) continue;
        if(!lead.fields[i].live) continue;

        auto step = offset - leadOffset;
        auto destination = toActive
            ? slots[edge.to].fields[fieldAt(slots[edge.to], edge.toOffset + step)].address
            : offsetAddress(base, module, block, to, step, source);
        auto origin = fromActive
            ? slots[edge.from].fields[fieldAt(slots[edge.from], edge.fromOffset + step)].address
            : offsetAddress(base, module, block, from, step, source);

        moveField(base, module, block, destination, origin, lead.fields[i], source);
    }

    return true;
}

// The cell an address names, where it names one of a slot being split.
Maybe<LowerPtr<LowerValue>> fieldOf(Array<Slot>& slots, HashMap<U32, U64>& location,
                                    LowerPtr<LowerValue> address) {
    auto found = location.getValue(U32(address));
    if(found.isNothing()) return Nothing();

    auto& slot = slots[Size(found.unwrap() >> 32)];
    if(!slot.active) return Nothing();

    auto& field = slot.fields[fieldAt(slot, U32(found.unwrap()))];

    // An access is what made its cell live, so a cell an access names has storage by construction.
    assertTrue(field.address != nullptr);
    return Just(field.address);
}

/*
 * The second rewrite: every access moved onto the cell it landed in, every copy expanded, and the
 * arithmetic that computed the offsets dropped.
 *
 * The list is rebuilt rather than edited because a copy may need several instructions in its place
 * and there is nowhere to put them otherwise - `addInst` appends, and appending during the rebuild
 * is exactly the position the copy occupied.
 */
void rewriteBlock(LowerBase base, LowerModule& module, LowerBlock& block, Array<Slot>& slots,
                  Array<Edge>& edges, HashMap<U32, U64>& location, HashMap<U32, U32>& edgeOf,
                  HashMap<U32, U8>& offsetOf) {
    SmallArray<LowerPtr<LowerInst>, 32> original;
    for(auto instPtr: block.instructions.contents(base)) original.push(instPtr);

    block.instructions.clear();

    for(auto instPtr: original) {
        auto inst = base[instPtr];

        if(offsetOf.getValue(U32(instPtr))) {
            detach(base, inst);
            continue;
        }

        if(inst->kind == LowerInst::Load) {
            auto loadInst = (LowerInstLoad*)inst;
            if(auto field = fieldOf(slots, location, loadInst->from)) {
                setOperand(base, module.arena, inst, loadInst->from, base[field.unwrap()]);
            }
        } else if(inst->kind == LowerInst::Store) {
            auto storeInst = (LowerInstStore*)inst;
            if(auto field = fieldOf(slots, location, storeInst->to)) {
                setOperand(base, module.arena, inst, storeInst->to, base[field.unwrap()]);
            }
        } else if(inst->kind == LowerInst::Copy) {
            auto found = edgeOf.getValue(U32(instPtr));
            if(found && expandCopy(base, module, block, slots, edges[found.unwrap()])) continue;
        }

        block.instructions.push(module.arena, instPtr);
    }
}

} // namespace

void splitAggregateSlots(LowerBase base, LowerFunction& fun) {
    auto& module = *fun.module;

    HashMap<U32, U64> location;
    Array<Slot> slots;
    collectSlots(base, fun, slots, location);
    if(slots.isEmpty()) return;

    Array<Edge> edges;
    HashMap<U32, U32> edgeOf;
    collectEdges(base, fun, slots, location, edges, edgeOf);

    // Until nothing changes its mind. Each round that is not the last refuses at least one slot, so
    // the number of them is bounded by the number of slots and is one for almost every function.
    do {
        computePoints(slots, edges);
    } while(!validateSlots(slots));

    HashMap<U32, U32> allocaOf;
    HashMap<U32, U8> offsetOf;

    for(Size i = 0; i < slots.size(); i++) {
        if(!slots[i].active) continue;

        allocaOf.add(U32(slots[i].allocation), U32(i));
        for(auto offset: slots[i].offsets) offsetOf.add(U32(offset), 1);
    }

    if(allocaOf.size() == 0) return;

    computeFields(slots, edges);

    for(auto blockPtr: fun.blocks.contents(base)) {
        createFields(base, module, *base[blockPtr], slots, allocaOf);
    }

    for(auto blockPtr: fun.blocks.contents(base)) {
        rewriteBlock(base, module, *base[blockPtr], slots, edges, location, edgeOf, offsetOf);
    }

    /*
     * Nothing may still be holding the address of storage that no longer exists, or an offset from
     * it. An instruction left referring to a deleted one is not caught downstream - the validator
     * asks which *function* a value belongs to rather than whether its instruction is still in a
     * block - so it is asked here, where the answer identifies which use was missed.
     */
    for(auto& slot: slots) {
        if(!slot.active) continue;

        assertTrue(base[slot.address]->uses.isEmpty());
        for(auto offset: slot.offsets) {
            assertTrue(base[offset]->created().ptr->uses.isEmpty());
        }
    }
}
