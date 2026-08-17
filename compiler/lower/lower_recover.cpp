#include "lower_recover.h"
#include "lower_builder.h"

/*
 * See lower_recover.h for the shape and what it is for. This file is the recognizer, the move, and
 * the two substitutions - one that lets the chain be *matched* past the load it starts at, and one
 * that lets it be *moved*.
 */

namespace {

// The same coarse memory model the CSE has, and it has to be the same one: what this pass exists to
// recover is exactly what that one retired. See `writesStorage` in lower_cse.cpp.
bool writesAnything(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Store:
        case LowerInst::Copy:
        case LowerInst::SetPattern:
        case LowerInst::Call:
            return true;
        default:
            return false;
    }
}

bool blockWrites(LowerBase base, LowerBlock* block) {
    for(auto offset: block->instructions.contents(base)) {
        if(writesAnything(base[offset])) return true;
    }

    return false;
}

/*
 * The guard, the two arms and the join, or nothing.
 *
 * Recognized from the join, because that is where the redundant instruction is and because the
 * join's predecessor list is what says the whole shape. Either arm may be the block the guard
 * branches through or the guard itself - `if c: grow(...)` with nothing on the other side is a
 * triangle, and the same source after the front end has given the empty side a block of its own is a
 * diamond. Both are here because both occur: the container guard in the library is written as the
 * first and lowered as the second.
 *
 * `clean` is null exactly when the guard branches straight to the join.
 */
struct Diamond {
    LowerBlock* guard = nullptr;
    LowerBlock* clean = nullptr;
    LowerBlock* dirty = nullptr;
};

// A predecessor that is a whole arm: reached one way, leaving one way, and leaving to the join.
// Answers the block it is reached from, or null.
LowerBlock* armGuard(LowerBase base, LowerBlock* side, LowerBlock* join) {
    if(side->incoming.size() != 1) return nullptr;
    if(base[side->terminator]->kind != LowerInst::Jmp) return nullptr;
    if(base[((LowerInstJmp*)base[side->terminator])->then] != join) return nullptr;

    return base[side->incoming.get(base, 0)];
}

// Whether `guard` chooses between exactly these two, in either order.
bool choosesBetween(LowerBase base, LowerBlock* guard, LowerBlock* a, LowerBlock* b) {
    if(base[guard->terminator]->kind != LowerInst::Je) return false;

    auto je = (LowerInstJe*)base[guard->terminator];
    auto then = base[je->then];
    auto otherwise = base[je->otherwise];

    return (then == a && otherwise == b) || (then == b && otherwise == a);
}

bool findDiamond(LowerBase base, LowerBlock* join, Diamond& out) {
    if(join->incoming.size() != 2) return false;

    auto a = base[join->incoming.get(base, 0)];
    auto b = base[join->incoming.get(base, 1)];
    if(a == b || a == join || b == join) return false;

    auto guardOfA = armGuard(base, a, join);
    auto guardOfB = armGuard(base, b, join);

    LowerBlock* guard = nullptr;
    LowerBlock* sideA = nullptr;
    LowerBlock* sideB = nullptr;

    if(guardOfA && guardOfA == guardOfB) {
        // Both predecessors are arms of one branch, which is the diamond.
        guard = guardOfA;
        sideA = a;
        sideB = b;
        if(!choosesBetween(base, guard, a, b)) return false;
    } else if(guardOfA == b) {
        // One arm, and the other edge is the branch itself: the triangle, `a` being the arm.
        guard = b;
        sideA = a;
        sideB = nullptr;
        if(!choosesBetween(base, guard, a, join)) return false;
    } else if(guardOfB == a) {
        guard = a;
        sideA = nullptr;
        sideB = b;
        if(!choosesBetween(base, guard, b, join)) return false;
    } else {
        return false;
    }

    // Which arm invalidated the guard's loads and which did not. Exactly one may have: with two the
    // guard's value is unavailable either way and there is nothing to recover, and with none the CSE
    // has already unified whatever this would have found.
    auto writesA = sideA && blockWrites(base, sideA);
    auto writesB = sideB && blockWrites(base, sideB);
    if(writesA == writesB) return false;

    out.guard = guard;
    out.dirty = writesA ? sideA : sideB;
    out.clean = writesA ? sideB : sideA;
    return true;
}

/*
 * Which instructions of the guard a load may still be answered from.
 *
 * Everything below the guard's last write is stale by the time the join runs - the arm is not the
 * only thing that can have written, and a guard that stores and then branches has invalidated its
 * own earlier loads. Computations that are not loads are unaffected and have no floor.
 */
Size loadFloorOf(LowerBase base, LowerBlock* guard) {
    Size floor = 0;
    auto list = guard->instructions.contents(base);

    for(Size i = 0; i < list.size(); i++) {
        if(writesAnything(base[list[i]])) floor = i + 1;
    }

    return floor;
}

/*
 * One result and no side effects, which is what may be moved and merged - and, for everything that
 * is not a load, worth merging at all.
 *
 * The second half is `worthHoisting`'s in lower_licm.cpp and is the same argument arriving from a
 * different pass. An address is free where it stands: `foldAddresses` puts `add %self, 8` into the
 * addressing mode of the instruction that reads it, so the join never executes it - and merging one
 * turns a thing that costs nothing into a phi, which is a register carried across the branch. A
 * comparison is the same argument one register further on, its readers being branches that carry it
 * in the flags.
 *
 * A *load* is admitted whatever it produces, the data pointer of a container being a load of pointer
 * type and exactly what this is for, and it is the only kind admitted with a floor.
 */
bool isMovable(LowerInst* inst) {
    if(inst->kind == LowerInst::Load) return true;
    if(!isRepeatable(inst)) return false;
    if(inst->createdCount != 1) return false;
    if(inst->kind == LowerInst::Set || inst->kind == LowerInst::Cmp) return false;

    return ((LowerInstSingle*)inst)->result.type != LowerType::Pointer;
}

/*
 * The two readings of a phi this pass inserted.
 *
 * `guardValue` is what it is on the edge that skipped the dirty arm, and is what an instruction
 * below it has to be compared against to find the guard's own copy. `dirtyValue` is what it is on
 * the other edge, and is what that instruction has to *read* once it has been moved there - which is
 * the half that would otherwise be an SSA violation: the phi is defined in the join, and the arm
 * runs before it.
 */
struct Recovered {
    LowerPtr<LowerValue> phi;
    LowerPtr<LowerValue> guardValue;
    LowerPtr<LowerValue> dirtyValue;
};

struct Substitution {
    SmallArray<Recovered, 8> entries;

    LowerPtr<LowerValue> inGuard(LowerPtr<LowerValue> value) const {
        for(auto& e: entries) if(e.phi == value) return e.guardValue;
        return value;
    }

    LowerPtr<LowerValue> inDirtyArm(LowerPtr<LowerValue> value, bool& known) const {
        for(auto& e: entries) if(e.phi == value) { known = true; return e.dirtyValue; }
        return value;
    }
};

/*
 * Whether two values compute the same thing, which cannot be asked by identity alone.
 *
 * Two things stand in the way of it, and both are deliberate elsewhere. An immediate is an
 * instruction here and it is placed where it is read, so the guard's `add %self, 8` and the join's
 * read two different `8`s - `eliminateCommonValues` leaves a constant where it stands on purpose,
 * materializing one twice being cheaper than carrying it in a register between the two. And the
 * *address* of the field is the same story one level up: `foldAddresses` folds it into the access,
 * so the join has an `add` of its own rather than a register holding the guard's.
 *
 * So the comparison recurses, through pure arithmetic and to a small depth - the CSE's
 * `sameOperand`, restated for this pass's substitution. It stops at a **load**, which is not a
 * detail but the whole question: whether two loads of one address answer the same thing is what this
 * pass is deciding, and a comparison that assumed it would prove itself.
 */
bool equivalent(LowerBase base, LowerValue* a, LowerValue* b, const Substitution& subst, U32 depth) {
    if(a - base == subst.inGuard(b - base)) return true;
    if(depth == 0) return false;

    auto left = a->inst();
    auto right = b->inst();

    if(left->kind != right->kind) return false;
    if(a->type != b->type || left->flags != right->flags) return false;

    if(left->kind == LowerInst::Imm) return ((LowerImm*)left)->i == ((LowerImm*)right)->i;
    if(!isRepeatable(left) || left->kind == LowerInst::Load) return false;

    auto x = left->used();
    auto y = right->used();
    if(x.length != y.length) return false;

    for(Size i = 0; i < x.length; i++) {
        if(!equivalent(base, base[x.ptr[i]], base[y.ptr[i]], subst, depth - 1)) return false;
    }

    return true;
}

/*
 * Whether the guard's `candidate` computes what `inst` computes, given what the phis inserted so far
 * stand for.
 *
 * Exact identity of operands rather than the CSE's structural comparison, and that is not a
 * weakening: `eliminateCommonValues` has already run, so two instructions in the guard that compute
 * one thing are one instruction, and anything the join could match structurally it can match by
 * name. What the substitution adds is the one name the join has that the guard does not.
 */
bool sameGivenSubstitution(LowerBase base, LowerInst* candidate, LowerInst* inst,
                           const Substitution& subst)
{
    if(candidate->kind != inst->kind) return false;
    if(candidate->createdCount != 1 || inst->createdCount != 1) return false;

    if(((LowerInstSingle*)candidate)->result.type != ((LowerInstSingle*)inst)->result.type) return false;

    // The width and signedness of an access, the relation a comparison carries - everything a kind
    // stores outside its operand list is in `flags`, so comparing it is what stops two instructions
    // of one kind and two meanings from unifying.
    if(candidate->flags != inst->flags) return false;

    auto a = candidate->used();
    auto b = inst->used();
    if(a.length != b.length) return false;

    for(Size i = 0; i < a.length; i++) {
        if(!equivalent(base, base[a.ptr[i]], base[b.ptr[i]], subst, 4)) return false;
    }

    return true;
}

// Takes an instruction out of one block's list. The value it defines and everything it reads are
// untouched: this is a change of position, and the operand argument for it is in the header.
void unlink(LowerBase base, LowerBlock* from, LowerInst* inst) {
    for(Size i = 0; i < from->instructions.size(); i++) {
        if(base[from->instructions.get(base, i)] != inst) continue;

        from->instructions.remove(base, i);
        return;
    }

    assertTrue(false); // the instruction was not in the block it says it is in
}

bool recoverJoin(LowerBase base, LowerModule& module, LowerBlock* join) {
    Diamond shape;
    if(!findDiamond(base, join, shape)) return false;

    auto guardList = shape.guard->instructions.contents(base);
    auto floor = loadFloorOf(base, shape.guard);
    auto changed = false;

    Substitution subst;

    // A copy, because the list is edited underneath: an instruction that moves leaves it, and the
    // phi that replaces it joins the block's phi list rather than this one.
    SmallArray<LowerPtr<LowerInst>, 32> body;
    for(auto offset: join->instructions.contents(base)) body.push(offset);

    for(auto offset: body) {
        auto inst = base[offset];

        // A write ends the walk rather than skipping it: every load below one reads storage the
        // guard's copy no longer describes, and the arithmetic below it is reached through the load.
        if(writesAnything(inst)) break;
        if(!isMovable(inst)) continue;

        auto isLoad = inst->kind == LowerInst::Load;

        /*
         * And everything that is not a load has to be reading one that was recovered.
         *
         * What this pass is for is the load; the arithmetic hanging off it comes with it because the
         * guard computed that too, and stopping at the load would leave half the win. Arithmetic
         * that reads nothing recovered is a different proposition - the join computes it once, the
         * merge would compute it once and add a phi, and what that trades is an instruction for a
         * copy. Declining costs nothing and keeps the pass to the thing it measured.
         */
        if(!isLoad) {
            auto readsRecovered = false;
            for(auto used: inst->used()) {
                auto known = false;
                subst.inDirtyArm(used, known);
                if(known) { readsRecovered = true; break; }
            }

            if(!readsRecovered) continue;
        }
        LowerInst* found = nullptr;

        for(Size i = guardList.size(); i-- > (isLoad ? floor : 0);) {
            auto candidate = base[guardList[i]];
            if(!sameGivenSubstitution(base, candidate, inst, subst)) continue;

            found = candidate;
            break;
        }

        if(!found) continue;

        auto moved = &((LowerInstSingle*)inst)->result;
        auto kept = &((LowerInstSingle*)found)->result;

        /*
         * The merge, built before the move so that the readers are pointed at it while the value it
         * merges is still where they can be found from. The phi is given its alternatives, then the
         * readers are moved onto it, and only then is it added to the block - adding it is what
         * registers its own reads, so a phi attached first would be rewritten into a reader of
         * itself by the replacement.
         */
        auto phi = makePhi(module.arena, moved->type, 2);
        phi->source = inst->source;

        auto cleanEdge = shape.clean ? shape.clean : shape.guard;
        phi->used().ptr[0] = kept - base;
        phi->used().ptr[1] = moved - base;
        phi->sources()[0] = cleanEdge - base;
        phi->sources()[1] = shape.dirty - base;

        replaceUses(base, module.arena, moved - base, &phi->result - base);
        join->addInst(base, phi);

        /*
         * And the move, which is where every operand is re-read.
         *
         * Nothing defined in the join may be read from the arm, since the arm runs first - and the
         * match above has just named a replacement for every operand that is defined there. An
         * operand standing for one of this pass's phis takes what that phi holds on the edge being
         * moved onto, and every other operand takes the guard's own, which the match proved equal
         * and which is available in the arm because the guard is above it.
         */
        auto used = inst->used();
        auto guardOperands = found->used();

        for(Size i = 0; i < used.length; i++) {
            auto known = false;
            auto replacement = subst.inDirtyArm(used.ptr[i], known);

            if(!known) {
                /*
                 * A literal gets one of its own rather than the guard's, and that is not tidiness.
                 * Sharing one makes it a value read from two blocks, which is enough for the loop
                 * hoister above to lift it into a preheader and for the allocator to then spill it -
                 * `and $0x3fffffff, %esi` measured as `and 0x2c(%rsp), %esi` on `Sieve`'s fill loop,
                 * which is worse than the instruction this pass had just removed. A constant is free
                 * where it is read and belongs there.
                 */
                auto operand = base[used.ptr[i]];
                auto definition = operand->inst();

                if(definition->kind == LowerInst::Imm) {
                    auto copy = new (module.arena) LowerImm(
                        StringId(), operand->type, ((LowerImm*)definition)->i
                    );

                    shape.dirty->addInst(base, copy);
                    replacement = &copy->result - base;
                } else {
                    replacement = guardOperands.ptr[i];
                }
            }

            setOperand(base, module.arena, inst, used.ptr[i], base[replacement]);
        }

        unlink(base, join, inst);
        inst->block = nullptr;
        shape.dirty->addInst(base, inst);

        subst.entries.push(Recovered { &phi->result - base, kept - base, moved - base });
        changed = true;
    }

    return changed;
}

} // namespace

bool recoverPartialLoads(LowerBase base, LowerModule& module, LowerFunction& fun) {
    auto changed = false;

    for(auto offset: fun.blocks.contents(base)) {
        if(recoverJoin(base, module, base[offset])) changed = true;
    }

    return changed;
}
