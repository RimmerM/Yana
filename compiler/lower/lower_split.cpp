#include "lower_split.h"
#include "lower_inst.h"
#include "lower_builder.h"
#include "lower_promote.h"
#include "lower_store.h"
#include "fold_arith.h"

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

// The widest cell nothing names that is left whole - see the end of `computePoints`.
constexpr U32 kWordCutAbove = 32;

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

    // Whether an access narrower than the cell writes it - `isPrefixAccess`. Such a store is a
    // read-modify-write of the cell once `promoteStackSlots` holds it in a register, so the cell is
    // given a value where it is allocated whatever else writes it.
    bool narrowWrite = false;

    LowerType type = LowerType::Int64;

    // The allocation this cell became, once the rewrite has made one. Null while the cell is dead.
    LowerPtr<LowerValue> address = nullptr;
};

// A zero fill of part of a slot. It names a range rather than a cell, so it is neither an Access -
// which has to fit one - nor an Edge, and it is rewritten as a zero per cell it covers.
struct Fill {
    LowerPtr<LowerInst> inst = nullptr;
    U32 offset = 0;
    U32 size = 0;
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
    Array<Fill> fills;
    Array<U32> points;                   // the cut offsets, ascending, 0 and `size` included

    // The widest integer access at each offset, here or through a copy - see coveredByWider.
    struct Wide { U32 offset; U32 size; };
    Array<Wide> wide;
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
 * Whether the address leaves at all - as a call argument, as a value stored into memory, as a copy's
 * byte count, as an operand of arithmetic that is not a constant offset - is `addressContained`'s
 * question (lower_store.h), which `collectSlots` asks beside this walk since J163 and which is the
 * slot's answer. What this walk adds is what the partition is made of: it keeps the offsets and the
 * accesses it saw, refuses an access that does not fit inside the slot, and refuses what it could
 * not rewrite - a volatile or overreading access, a copy of run-time length, a fill of anything but
 * zero.
 *
 * It still **stops** at the uses that are the address leaving, although `addressContained` has
 * refused the slot for them already, because where it stops decides which addresses `location`
 * names. J163 first dropped those three tests as redundant, and the walk then went on past an
 * escape and recorded more of a refused slot's closure: `test/base` changed in three functions,
 * since a copy between a refused slot's address and an accepted slot is an edge the partition
 * reads. The closure recorded is exactly the one before J163.
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

            // Device reads must remain memory accesses in their original order.
            if(loadInst->isVolatile()) return false;

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

            // Device writes must remain memory accesses in their original order.
            if(storeInst->isVolatile()) return false;

            // Not the address stored *as* a value - `addressContained` refuses that too, and it is
            // asked here as well because where this walk stops decides which addresses `location`
            // names, and the partition reads those (see the comment above).
            if(storeInst->to != self || storeInst->value == self) return false;

            auto width = storeInst->getWidth();
            if(width == 0 || width > slot.size - offset) return false;

            slot.accesses.push(Access { userPtr, offset, width, true,
                                        base[storeInst->value]->type });
            continue;
        }

        if(user->kind == LowerInst::Copy) {
            auto copyInst = (LowerInstCopy*)user;

            // The address as the byte count is the address leaving (and stops the walk, as above),
            // and a copy of a place onto itself is not a move between two of them.
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
         * A zero fill of a constant length - which is what a zeroed record is (`zeroStorage` and
         * `zeroFieldRuns` in resolve_ssa/lower/inst.cpp), and what this used to see as a run of
         * stores. It cuts the partition at the words those stores wrote (see computePoints) and
         * writes every cell it covers, and the rewrite gives each live one a zero of the register
         * it is moved through.
         *
         * Only a zero. Another byte would have to be replicated to each cell's width and
         * reinterpreted for a floating one, and nothing writes one into a record.
         */
        if(user->kind == LowerInst::SetPattern) {
            auto fill = (LowerInstSetPattern*)user;
            if(fill->to != self || fill->count == self || fill->pattern == self) return false;

            auto count = constantOf(base, fill->count);
            auto pattern = constantOf(base, fill->pattern);
            if(count.isNothing() || pattern.isNothing() || (pattern.unwrap() & 0xff) != 0) return false;
            if(count.unwrap() == 0 || count.unwrap() > slot.size - offset) return false;

            slot.fills.push(Fill { userPtr, offset, U32(count.unwrap()) });
            continue;
        }

        // Everything else - which `addressContained` has already refused as the address leaving, and
        // which this pass could not rewrite either.
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

            // Whether the address leaves is `addressContained`'s question (lower_store.h), asked
            // first; `collectUses` keeps the part of the walk that is this pass's own - every
            // access inside the slot, a constant copy length and a zero fill. It still walks a
            // refused slot, so that the addresses in its closure resolve (see the comment above).
            auto which = slots.size();
            auto contained = addressContained(base, allocation->created().ptr, 0);
            slot.active = collectUses(base, slot, which, location,
                                      allocation->created().ptr, 0, 0) && contained;
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

using foldArith::isPowerOfTwo;

/*
 * Whether an access is the low bytes of a wider cell rather than a cell of its own - J152.
 *
 * A sum's arms name the same bytes at different widths: `Result(FileError, I64)` is a one-byte error
 * at eight and an eight-byte value at eight. Cutting at every access's end made that a one-byte cell
 * and a seven-byte one, so the value arm was carried as a byte and a `copy 7` and read back as one
 * eight-byte load over both - a store-forwarding stall, and a slot promotion could not take. Where an
 * access starts where a wider one of the same slot starts, its end is not a cut: it reads or writes
 * the low bytes of that cell, which is what a narrower access is on this little-endian machine, and
 * `promoteStackSlots` answers it with a mask (a load) or a merge into the register (a store).
 *
 * Only integers, of power-of-two widths, within one register - the shapes a mask describes.
 */
bool isPrefixAccess(const Access& access, U32 cellSize) {
    return access.size < cellSize && cellSize <= 8 && isPowerOfTwo(cellSize) &&
           isPowerOfTwo(access.size) && isInt(access.type);
}

/*
 * Whether a wider integer access starts where this one does - in this slot, or in one a copy relates
 * it to at the same relative offset.
 *
 * The copies matter as much as the slot. A sum is built in one slot and read in another: the join
 * that stores `Result(FileError, FileInfo)`'s value word, and the temporary a `match` copies it into
 * and reads the error byte of. Asked of the temporary alone, the byte is not covered and cuts at
 * nine, the cut crosses the copy, and the join's word straddles it - so the question is asked of the
 * widths the copies carry, to the same fixpoint the cut points reach.
 */
bool raiseWide(Array<Slot::Wide>& wide, U32 offset, U32 size) {
    for(auto& entry: wide) {
        if(entry.offset != offset) continue;
        if(entry.size >= size) return false;

        entry.size = size;
        return true;
    }

    wide.push(Slot::Wide { offset, size });
    return true;
}

bool carryWide(Slot& from, U32 fromOffset, Slot& to, U32 toOffset, U32 count) {
    auto changed = false;

    for(Size i = 0; i < from.wide.size(); i++) {
        auto entry = from.wide[i];
        if(entry.offset < fromOffset || entry.offset + entry.size > fromOffset + count) continue;

        changed |= raiseWide(to.wide, toOffset + (entry.offset - fromOffset), entry.size);
    }

    return changed;
}

void computeWide(Array<Slot>& slots, Array<Edge>& edges) {
    for(auto& slot: slots) {
        slot.wide.clear();
        if(!slot.active) continue;

        for(auto& access: slot.accesses) {
            if(isInt(access.type) && access.size <= 8 && isPowerOfTwo(access.size)) {
                raiseWide(slot.wide, access.offset, access.size);
            }
        }
    }

    auto changed = true;
    while(changed) {
        changed = false;

        for(auto& edge: edges) {
            if(edge.to == kNoSlot || edge.from == kNoSlot) continue;
            if(!slots[edge.to].active || !slots[edge.from].active) continue;

            changed |= carryWide(slots[edge.from], edge.fromOffset, slots[edge.to], edge.toOffset, edge.count);
            changed |= carryWide(slots[edge.to], edge.toOffset, slots[edge.from], edge.fromOffset, edge.count);
        }
    }
}

bool coveredByWider(const Slot& slot, const Access& access) {
    for(auto& entry: slot.wide) {
        if(entry.offset == access.offset) return isPrefixAccess(access, entry.size);
    }

    return false;
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

// Whether an access of this slot, or of a slot a copy relates `[at, end)` to, starts inside that
// range, or a fill covers it - the cell is then a field something names.
bool namesCell(Array<Slot>& slots, Array<Edge>& edges, Size which, U32 at, U32 end) {
    auto& slot = slots[which];
    for(auto& access: slot.accesses) {
        if(access.offset >= at && access.offset < end) return true;
    }

    for(auto& fill: slot.fills) {
        if(fill.offset < end && fill.offset + fill.size > at) return true;
    }

    for(auto& edge: edges) {
        if(edge.to != which && edge.from != which) continue;

        auto mine = edge.to == which ? edge.toOffset : edge.fromOffset;
        auto other = edge.to == which ? edge.from : edge.to;
        auto theirs = edge.to == which ? edge.fromOffset : edge.toOffset;
        if(other == kNoSlot || other == which || !slots[other].active) continue;
        if(mine > at || mine + edge.count < end) continue;

        auto from = theirs + (at - mine);
        auto to = from + (end - at);
        for(auto& access: slots[other].accesses) {
            if(access.offset >= from && access.offset < to) return true;
        }
    }

    return false;
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
    computeWide(slots, edges);

    for(auto& slot: slots) {
        slot.points.clear();
        if(!slot.active) continue;

        slot.points.push(0);
        slot.points.push(slot.size);

        for(auto& access: slot.accesses) {
            addPoint(slot.points, access.offset);
            if(!coveredByWider(slot, access)) addPoint(slot.points, access.offset + access.size);
        }

        /*
         * A fill cuts where a zeroing written a word at a time would have: widest word first, from
         * where it starts. That is where a record's fields sit whenever each is aligned to its own
         * width, and it is the structure this pass saw when `zeroStorage` wrote those words - the
         * only structure a temporary a duplicate is copied through has, since nothing else here
         * names a field of it.
         */
        for(auto& fill: slot.fills) {
            auto at = fill.offset;
            auto end = fill.offset + fill.size;

            addPoint(slot.points, at);
            while(at < end) {
                auto left = end - at;
                at += left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1;
                addPoint(slot.points, at);
            }
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

    /*
     * A wide cell nothing names a field of - J160.
     *
     * A cell no access reads or writes is only ever copied: the bytes of a value moved between
     * places that are not slots here - an out-parameter a call filled, a field of a record behind a
     * pointer - through a temporary, and cut from the rest only where something touched one field of
     * it (a padded word's zero, a tag). Left whole it is an allocation of its own and a block copy in
     * and out, and `expandBlockOperations` writes those as the widest transfers the machine has. For
     * a value that was just written a field at a time that is a 32-byte load over eight narrow
     * stores, which the store buffer cannot forward - the load waits for all of them to retire - and
     * the next copy of the same temporary reads it across the overlapping tail the last one left,
     * which is a second wait. `test/bench/lexer`'s iterator variants paid that on every token an
     * `iter fn` handed over and its consumer copied on: `iter` went from 160 to 222 cycles a token
     * and `idiom/iter` from 176 to 206 (`doc/progress/2026-09-25-J160-iter-regression.md`).
     *
     * Before J152 the whole-value zero fill gave such a slot its words; since the fill became the
     * padded words alone the boundaries were meant to "arrive across the copy", which they do only
     * where a partner has fields to give. So such a cell gets the words a zeroing would have written
     * - the fills' rule above - and crosses in registers, as it did before.
     *
     * **Only above 32 bytes**, which is where a block copy stops being one vector transfer. Cutting
     * every unnamed cell wider than a register - a 16-byte view moved as one `movdqu` - grew
     * `test/base`'s `.text` by 37 KB; above 32 it moved it by nothing measurable and above 16 it
     * shrank it by 256 bytes, while the lexer's 60-byte token needs the words either way. Not where
     * an access *is* the cell - a vector - here or in a slot a copy relates it to, whose access the
     * words would straddle; and not where the words would take the slot past `kMaxFields`, which
     * would refuse the whole slot rather than keep one wide cell.
     */
    auto added = false;
    for(Size i = 0; i < slots.size(); i++) {
        auto& slot = slots[i];
        if(!slot.active) continue;

        for(Size k = 0; k + 1 < slot.points.size(); k++) {
            auto at = slot.points[k];
            auto end = slot.points[k + 1];
            if(end - at <= kWordCutAbove || namesCell(slots, edges, i, at, end)) continue;

            // Whole words and a narrowing tail - never more than four pieces past the words.
            auto pieces = (end - at) / 8 + 3;
            if(slot.points.size() - 1 + pieces > kMaxFields + 1) continue;

            auto step = at;
            while(step < end) {
                auto left = end - step;
                step += left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1;
                addPoint(slot.points, step);
            }

            added = true;
            k = slot.points.findIndex(end).unwrap() - 1;
        }
    }

    // And across the copies once more, to the partners the words were cut for - which are cells
    // nothing names either, or `namesCell` would have said so.
    changed = added;
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
            if(found.isNothing()) { usable = false; break; }

            // Exactly one cell, or the low bytes of one - see isPrefixAccess.
            auto cell = slot.points[found.unwrap() + 1] - access.offset;
            usable = cell == access.size || isPrefixAccess(access, cell);
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
 * A cell an access names is live and typed by it. A cell something wrote that a copy carries *out*
 * to somewhere this pass cannot see is live, because the destination may be read by anyone - which
 * is the one rule the header's argument rests on. Everything else is what the copies then relate,
 * taken to a fixpoint: liveness and the register type both travel across a copy, in both
 * directions.
 *
 * A copy *in* from outside does not make a cell live. It used to, and that kept every padding byte of
 * a value read out of a heap block alive through every temporary it passed - `Maybe(Size)`'s seven
 * bytes above its tag, copied three times per `exchange` as two overlapping four-byte moves each, and
 * the store-forwarding stall that cost an external iterator's step two-thirds of its time. What such
 * a cell holds is the sender's, and nobody here reads it or hands it on.
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

            // A prefix access says the cell is an integer of the cell's width, and nothing about
            // which of the two - the wide access beside it says that.
            if(access.size < field.size) {
                field.narrowWrite |= access.writes;
                if(!field.typed && !field.conflicted) {
                    mergeFieldType(field, field.size > 4 ? LowerType::Int64 : LowerType::Int32);
                }
                continue;
            }

            mergeFieldType(field, access.type);
        }

        // A fill writes every cell it covers, and reads none of them.
        for(auto& fill: slot.fills) {
            for(auto& field: slot.fields) {
                if(field.offset >= fill.offset && field.offset < fill.offset + fill.size) field.written = true;
            }
        }
    }

    for(auto& edge: edges) {
        auto toActive = edge.to != kNoSlot && slots[edge.to].active;
        auto fromActive = edge.from != kNoSlot && slots[edge.from].active;

        // A copy that brings bytes in from somewhere this pass cannot see: the cells it lands on hold
        // something rather than nothing, so they are written. Not live - see above.
        if(toActive && !fromActive) {
            for(auto& field: slots[edge.to].fields) {
                if(field.offset < edge.toOffset) continue;
                if(field.offset >= edge.toOffset + edge.count) continue;

                field.written = true;
            }
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

    auto settle = [&]() {
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
    };

    settle();

    /*
     * A copy that takes bytes out to somewhere this pass cannot see, which may read every one of
     * them: the cells it leaves from are live - where something wrote them.
     *
     * After the fixpoint rather than beside the copies in, because *written* is what the fixpoint
     * settles. A cell nothing wrote holds this frame's own unspecified bytes wherever it is copied
     * to, so leaving the destination's bytes alone is not a difference a program can see; making it
     * live anyway gave it an allocation and a zero to carry out. Liveness it adds here then travels
     * the copies like any other, which is the second settle.
     */
    auto carriedOut = false;

    for(auto& edge: edges) {
        auto toActive = edge.to != kNoSlot && slots[edge.to].active;
        auto fromActive = edge.from != kNoSlot && slots[edge.from].active;
        if(!fromActive || toActive) continue;

        for(auto& field: slots[edge.from].fields) {
            if(field.offset < edge.fromOffset) continue;
            if(field.offset >= edge.fromOffset + edge.count) continue;
            if(!field.written || field.live) continue;

            field.live = true;
            carriedOut = true;
        }
    }

    if(carriedOut) settle();
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
/*
 * The zero a cell of this register type starts from - an immediate, except where the cell holds a
 * **pointer**, which no immediate may be (`validateImm`): there it is the integer zero restated as
 * the pointer, the `bitcast 0` promotion already writes for a null address.
 *
 * A pointer cell arrives here wherever a record holding an address is zeroed before anything is
 * stored into it - an iterator object's nested member is zeroed by `f$new` and its captured window
 * is written only when the outer step enters the region, so a program that split the record met a
 * pointer-typed immediate and was refused as invalid lower IR (J131).
 */
LowerPtr<LowerValue> zeroCell(LowerBase base, LowerModule& module, LowerBlock& block, LowerType type,
                              LocationId source) {
    auto immediate = isFloat(type) ? new (module.arena) LowerImm(StringId(), type, F64(0))
                   : isPtr(type)   ? new (module.arena) LowerImm(StringId(), LowerType::Int64, U64(0))
                                   : new (module.arena) LowerImm(StringId(), type, U64(0));
    immediate->source = source;
    block.addInst(base, immediate);

    auto zero = immediate->created().ptr - base;
    if(!isPtr(type)) return zero;

    auto cast = new (module.arena) LowerInstUnary(LowerInst::Bitcast, StringId(), type, zero);
    cast->source = source;
    block.addInst(base, cast);
    return cast->created().ptr - base;
}

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
            auto type = field.written && !field.narrowWrite ? Nothing() : moveType(field);
            if(type.isNothing() || isVectorLike(type.unwrap())) continue;

            auto zero = zeroCell(base, module, block, type.unwrap(), inst->source);

            auto initial = new (module.arena) LowerInstStore(field.address, zero, field.size);
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
 * The dead cells are simply not moved. Between two slots that is bytes nothing reads. Into a
 * destination that is not a slot it is bytes nothing wrote - a copy out makes every written cell it
 * carries live - so what the destination keeps is its own unspecified bytes instead of this frame's.
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

/*
 * One zero fill, rewritten as a zero per live cell it covers - an immediate stored through the
 * register the cell is moved through, which is what lets promotion take the cell, and a fill of the
 * cell's own bytes where there is no such register. A dead cell is not written, for the reason a
 * dead cell is not moved.
 */
bool expandFill(LowerBase base, LowerModule& module, LowerBlock& block, Array<Slot>& slots,
                HashMap<U32, U64>& location, LowerInstSetPattern* fill) {
    auto found = location.getValue(U32(fill->to));
    if(found.isNothing()) return false;

    auto& slot = slots[Size(found.unwrap() >> 32)];
    if(!slot.active) return false;

    auto offset = U32(found.unwrap());
    auto size = U32(constantOf(base, fill->count).unwrap());
    auto source = fill->source;
    detach(base, fill);

    for(auto& field: slot.fields) {
        if(field.offset < offset || field.offset >= offset + size || !field.live) continue;

        auto type = moveType(field);
        if(type && !isVectorLike(type.unwrap())) {
            auto zero = zeroCell(base, module, block, type.unwrap(), source);
            auto stored = new (module.arena) LowerInstStore(field.address, zero, field.size);
            stored->source = source;
            block.addInst(base, stored);
            continue;
        }

        auto count = new (module.arena) LowerImm(StringId(), LowerType::Int64, U64(field.size));
        count->source = source;
        block.addInst(base, count);

        auto pattern = new (module.arena) LowerImm(StringId(), LowerType::Int32, U64(0));
        pattern->source = source;
        block.addInst(base, pattern);

        auto filled = new (module.arena) LowerInstSetPattern(field.address, count->created().ptr - base,
                                                             pattern->created().ptr - base);
        filled->source = source;
        block.addInst(base, filled);
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
        } else if(inst->kind == LowerInst::SetPattern) {
            if(expandFill(base, module, block, slots, location, (LowerInstSetPattern*)inst)) continue;
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
