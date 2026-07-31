#include "opt_pass.h"

/*
 * Loop-invariant code motion: a computation whose answer cannot change between iterations is moved
 * to the block in front of the loop, where it happens once.
 *
 * Two kinds of instruction move and the second is the one worth having:
 *
 *  - a *pure* value all of whose operands are defined outside the loop. Free of any question about
 *    order or effect, and mostly what the passes in front of this one leave behind: a packed field's
 *    mask, a `TypeMetric`, a `Symbol`, the arithmetic of an index that does not depend on the
 *    induction variable;
 *  - a *read* of a place nothing in the loop writes. This is the one the source actually contains -
 *    a bound compared against on every iteration, a field of a record the body only reads - and it
 *    is the reason this pass needs an aliasing answer rather than only a dominance one.
 *
 * ## Why a read may be hoisted at all
 *
 * Moving a load ahead of the loop makes it happen on a path where it might not have: a loop whose
 * test fails immediately never ran its body. That is only sound where the read cannot fail, which is
 * the same question `isDeadRead` in opt_value.cpp answers for removing one, and it gets the same
 * answer - a local or a global is storage the checker proved is there, while a pointer or a borrow
 * root is an address the program computed and the fault it may take is the program's to take.
 *
 * The preheader requirement carries the other half. `computeLoops` only names a block that leads
 * *only* into the header, so an instruction put there runs exactly when the loop is entered rather
 * than on some path that skips it.
 *
 * ## What is deliberately not moved
 *
 * Nothing with an effect and none of the ownership instructions - `Init`, `Assign`, `Move`, `Copy`,
 * `Drop`, `Swap`, `Exchange`, `Borrow`. A `Borrow` is the tempting one and it is exactly the one to
 * leave alone: hoisting a loan out of a loop extends it over iterations the borrow checker never
 * agreed to, which is a decision this stage has no standing to take. See opt.h.
 *
 * A call is not moved either, however invariant its arguments look. That needs an effect summary of
 * the callee, and `computeEffects` in resolve is the thing to reach for when it does - inventing a
 * second notion of purity here is risk 3 in Analysis-Optimization.md §7.
 */

namespace {

struct Hoister {
    OptContext& opt;
    Dominance& dominance;
    Array<U8>& contained;

    // Whether anything in this loop writes storage a hoisted read could see, decided once per loop
    // rather than per candidate - see `scanEffects`.
    bool anyClobber = false;
    Array<Place> written;

    /*
     * Whether a value is computed inside the loop.
     *
     * A constant is not, whatever block it names. One belongs to no block's instruction list - see
     * `eachFunctionValue` in opt.cpp - and `makeConstant` gives it the block of whatever it was
     * built for, so a folded constant inside a loop would otherwise look like a definition there and
     * pin everything that reads it.
     *
     * An argument is not either, for the same reason with a different cause: its block is the entry,
     * which no loop contains.
     */
    bool definedIn(Loop& loop, ModulePtr<Value> value) {
        if(!value) return false;

        auto& definition = *opt.local[value];
        switch(definition.kind) {
            case Value::ConstInt: case Value::ConstFloat: case Value::ConstDouble:
            case Value::Arg:
                return false;
            default:
                break;
        }

        if(!definition.block) return false;

        auto index = opt.local[definition.block]->index;
        return index < loop.contains.size() && loop.contains[index];
    }

    bool operandsOutside(Loop& loop, Value& instruction) {
        auto outside = true;
        eachOperand(opt.local, instruction, [&](ModulePtr<Value> operand) {
            if(definedIn(loop, operand)) outside = false;
        });

        return outside;
    }

    /*
     * What the loop does to storage, in one walk.
     *
     * `written` collects every place the loop writes through an `Init` or an `Assign`, and
     * `anyClobber` records whether anything else that may write storage ran at all. The two are
     * separate because they are declined differently: a write is compared against a candidate and
     * may miss it, while a call is only survivable by a place a callee cannot reach.
     *
     * A place slot on a clobbering instruction is collected as a write as well. A `Move` out of a
     * contained local is the case: `anyClobber` does not save the candidate from it, because the
     * local being contained is exactly what makes `anyClobber` not apply.
     */
    void scanEffects(Loop& loop) {
        anyClobber = false;
        written.clear();

        for(auto index: loop.blocks) {
            auto block = opt.local[dominance.blocks[index]];

            for(auto pointer: block->instructions.contents(opt.local)) {
                auto& instruction = *opt.local[pointer];

                switch(instruction.kind) {
                    case Value::Init:
                    case Value::Assign:
                        written.push(((InstInit&)instruction).place);
                        continue;

                    // Reads and fresh storage, exactly as opt_place.cpp's `clobbers` classifies
                    // them. An immutable borrow cannot be written through; a mutable one can, and
                    // whatever it was taken of is not a contained local anyway.
                    case Value::Alloc:
                    case Value::LoadPlace:
                    case Value::Copy:
                        continue;
                    default:
                        break;
                }

                if(isPureValue(instruction)) continue;

                anyClobber = true;
                eachPlace(instruction, [&](const Place& place) { written.push(place); });
            }
        }
    }

    /*
     * Whether two places may reach the same storage.
     *
     * The same structural answer opt_place.cpp gives, and deliberately the weaker half of it: this
     * only has to say *no* correctly, and every case it is unsure about it answers yes. Two
     * different fields of one aggregate are the separation that matters, since that is what lets a
     * loop writing `p.x` keep a hoisted read of `p.y`.
     */
    bool mayAlias(const Place& a, const Place& b) {
        if(a.root == PlaceRoot::Pointer || b.root == PlaceRoot::Pointer) return true;
        if(a.root == PlaceRoot::Borrow || b.root == PlaceRoot::Borrow) return true;

        if(a.root != b.root) return false;
        if(a.root == PlaceRoot::Local && a.local != b.local) return false;
        if(a.root == PlaceRoot::Global && a.global != b.global) return false;

        auto& leftPath = const_cast<Place&>(a).projections;
        auto& rightPath = const_cast<Place&>(b).projections;

        auto count = min(leftPath.size(), rightPath.size());
        for(Size i = 0; i < count; i++) {
            auto left = leftPath.get(opt.local, i);
            auto right = rightPath.get(opt.local, i);

            if(left.kind != right.kind) return true;

            switch(left.kind) {
                case ProjectionKind::Field:
                case ProjectionKind::Property:
                    if(left.index != right.index) return false;
                    break;
                case ProjectionKind::Downcast:
                    if(left.index != right.index) return true;
                    break;
                case ProjectionKind::Index: {
                    auto leftIndex = constantValueOf(opt, left.value);
                    auto rightIndex = constantValueOf(opt, right.value);
                    if(!leftIndex || !rightIndex) return true;
                    if(leftIndex.unwrap() != rightIndex.unwrap()) return false;
                    break;
                }
                case ProjectionKind::Deref:
                    return true;
                default:
                    break;
            }
        }

        return true;
    }

    /*
     * A read the loop cannot change the answer to.
     *
     * Four conditions, and each rules out one way of being wrong: the storage has to be one the read
     * cannot fault on, the path has to be one the loop does not compute (an index that moves is a
     * different element each time), nothing in the loop may write it, and where the loop calls
     * anything at all the storage has to be a local no callee can name.
     */
    bool invariantRead(Loop& loop, InstLoadPlace& load) {
        if(load.place.root != PlaceRoot::Local && load.place.root != PlaceRoot::Global) return false;

        // The value has to *be* the answer rather than name storage holding it, which is the same
        // test forwarding applies before it replaces a load with a value.
        if(!load.type || isUnit(opt.global, load.type) || isMemoryType(opt.global, load.type)) return false;

        for(Size i = 0; i < load.place.projections.size(); i++) {
            auto projection = load.place.projections.get(opt.local, i);
            if(definedIn(loop, projection.value)) return false;
        }

        if(anyClobber && !staysInFrame(opt, contained, load.place)) return false;

        for(auto& place: written) {
            if(mayAlias(place, load.place)) return false;
        }

        return true;
    }

    // Taking one instruction out of its block and putting it at the end of the preheader's, which is
    // in front of that block's terminator because a terminator is not in the list.
    void moveToPreheader(Loop& loop, ModulePtr<Inst> pointer) {
        auto value = opt.local[pointer];
        auto source = opt.local[value->block];

        for(Size i = 0; i < source->instructions.size(); i++) {
            if(source->instructions.get(opt.local, i) != pointer) continue;

            source->instructions.remove(opt.local, i);
            break;
        }

        auto target = dominance.blocks[loop.preheader];
        opt.local[target]->instructions.push(opt.program.arena, pointer);
        value->block = target;

        opt.changed = true;
    }

    /*
     * One loop, in dominator preorder.
     *
     * The order is what makes a single walk enough: two candidates with a dependency between them
     * are in dominance order already - a use that is not a phi is dominated by its definition - so
     * visiting blocks in that order appends the definition to the preheader before the use, and a
     * value that became invariant *because* its operand was just hoisted is hoisted in the same
     * walk rather than on the next round.
     */
    void run(Loop& loop) {
        if(loop.preheader == Loop::kNone) return;

        scanEffects(loop);

        Array<U32> order;
        for(auto index: loop.blocks) order.push(index);

        for(Size i = 1; i < order.size(); i++) {
            for(Size j = i; j > 0 && dominance.preorder[order[j]] < dominance.preorder[order[j - 1]]; j--) {
                swap(order[j], order[j - 1]);
            }
        }

        for(auto index: order) {
            auto block = opt.local[dominance.blocks[index]];

            for(Size i = 0; i < block->instructions.size(); i++) {
                auto pointer = block->instructions.get(opt.local, i);
                auto& instruction = *opt.local[pointer];

                auto movable = isPureValue(instruction)
                    ? operandsOutside(loop, instruction)
                    : instruction.kind == Value::LoadPlace &&
                      operandsOutside(loop, instruction) &&
                      invariantRead(loop, (InstLoadPlace&)instruction);

                if(!movable) continue;

                /*
                 * The read is leaving the loop, so it stops being one of the loop's own accesses -
                 * which matters where two reads of one place are both candidates and the first has
                 * already gone. Nothing has to be added to `written`: a hoisted instruction writes
                 * nothing, which is the whole of what made it movable.
                 */
                moveToPreheader(loop, pointer);
                i--;
            }
        }
    }
};

}

void hoistLoopValues(OptContext& opt) {
    if(opt.function->blocks.isEmpty()) return;

    Dominance dominance;
    computeDominance(opt, dominance);

    Array<Loop> loops;
    computeLoops(opt, dominance, loops);
    if(loops.isEmpty()) return;

    Array<U8> contained;
    computeContainment(opt, contained);

    Hoister hoister { opt, dominance, contained };

    // Innermost first, which `computeLoops` sorted for. A value hoisted out of an inner loop lands
    // in a block the outer one contains, so the outer loop's own walk - or the next driver round,
    // where the two are not nested directly - carries it the rest of the way out.
    for(auto& loop: loops) hoister.run(loop);
}
