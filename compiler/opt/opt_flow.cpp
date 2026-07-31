#include "opt_pass.h"

/*
 * The three things a pass asks about the shape of a function rather than about one instruction:
 * which blocks dominate which, which blocks form a loop, and which locals a callee could reach.
 *
 * All three were written where they were first needed and all three are now wanted twice - dominance
 * by the common-subexpression pass and the loop pass, containment by place forwarding and the loop
 * pass - so they live here, and the file each of them used to be in says what it does with the
 * answer rather than how the answer is computed.
 *
 * Dominance is the textbook set fixpoint rather than the near-linear algorithm, on the same grounds
 * codegen/js/flow.cpp gives for its copy: a function's blocks are counted in tens, and being
 * obviously correct matters more than being fast when the failure mode is a value used where it has
 * not been computed.
 */

void computeDominance(OptContext& opt, Dominance& result) {
    auto& blocks = opt.function->blocks;
    auto count = blocks.size();

    for(Size i = 0; i < count; i++) {
        result.blocks.push(blocks.get(opt.local, i));
        result.immediate.push(Dominance::kNone);
        result.children.push(Array<U32>());

        Array<U8> row;
        for(Size j = 0; j < count; j++) row.push(i == 0 ? U8(i == j) : U8(1));
        result.dominators.push(::move(row));
    }

    auto changed = true;
    while(changed) {
        changed = false;

        for(Size i = 1; i < count; i++) {
            auto block = opt.local[result.blocks[i]];

            Array<U8> next;
            auto first = true;

            for(auto predecessor: block->incoming.contents(opt.local)) {
                auto from = opt.local[predecessor]->index;

                if(first) {
                    for(Size j = 0; j < count; j++) next.push(result.dominators[from][j]);
                    first = false;
                } else {
                    for(Size j = 0; j < count; j++) next[j] &= result.dominators[from][j];
                }
            }

            // A block nothing reaches is dominated by itself and nothing else, which keeps it out of
            // every scope below without needing a case of its own.
            if(first) for(Size j = 0; j < count; j++) next.push(0);
            next[i] = 1;

            for(Size j = 0; j < count; j++) {
                if(result.dominators[i][j] == next[j]) continue;

                result.dominators[i][j] = next[j];
                changed = true;
            }
        }
    }

    // The immediate dominator is the one strict dominator that every other strict dominator also
    // dominates, which is the same as the one with the most dominators of its own.
    for(Size i = 1; i < count; i++) {
        auto best = Dominance::kNone;
        Size bestCount = 0;

        for(Size j = 0; j < count; j++) {
            if(i == j || !result.dominators[i][j]) continue;

            Size own = 0;
            for(Size k = 0; k < count; k++) own += result.dominators[j][k];

            if(best != Dominance::kNone && own <= bestCount) continue;

            best = U32(j);
            bestCount = own;
        }

        result.immediate[i] = best;
        if(best != Dominance::kNone) result.children[best].push(U32(i));
    }

    // A visit order in which a block comes before everything it dominates, which is what a pass that
    // moves an instruction needs: two values with a dependency between them are in dominance order
    // by construction, so laying them out in this order lays the definition out before the use.
    result.preorder.reserve(count);
    for(Size i = 0; i < count; i++) result.preorder.push(0);

    Array<U32> stack;
    if(count) stack.push(0);

    U32 next = 0;
    while(stack.size()) {
        auto index = stack.pop().unwrap();

        result.preorder[index] = next++;
        for(auto child: result.children[index]) stack.push(child);
    }
}

/*
 * The natural loops, innermost first.
 *
 * A back edge is an edge whose target dominates its source, and the loop it closes is the target
 * plus every block that reaches the source without leaving through the target. That definition is
 * the one worth having here because of what it guarantees: the header is the *only* way in, so a
 * block outside the loop that jumps to it is a place every iteration is preceded by.
 *
 * Irreducible flow - a cycle with two entries - simply produces no back edge and therefore no loop,
 * which is the right answer rather than a missing case. Nothing this front end emits is irreducible
 * anyway, since `while`, `for` and the guard forms all build a single header.
 *
 * Two back edges to one header are one loop. Sorting by size afterwards is what puts an inner loop
 * in front of the outer one containing it, so a hoist walks outwards a level per round rather than
 * having to know how deeply anything is nested.
 */
void computeLoops(OptContext& opt, Dominance& dominance, Array<Loop>& loops) {
    auto count = dominance.blocks.size();

    for(Size i = 0; i < count; i++) {
        auto block = opt.local[dominance.blocks[i]];

        for(auto successor: block->outgoing) {
            if(!successor) continue;

            auto header = opt.local[successor]->index;

            // The entry block heads no loop: nothing branches into a function, so an edge back to it
            // would be a second entry and the body below would be the whole function.
            if(header == 0 || !dominance.dominators[i][header]) continue;

            auto existing = maxLimit<Size>;
            for(Size l = 0; l < loops.size(); l++) {
                if(loops[l].header == header) existing = l;
            }

            if(existing == maxLimit<Size>) {
                existing = loops.size();

                Loop fresh;
                fresh.header = header;
                fresh.contains.reserve(count);
                for(Size j = 0; j < count; j++) fresh.contains.push(0);

                fresh.contains[header] = 1;
                loops.push(::move(fresh));
            }

            // Backwards from the source of the back edge, stopping at the header - which is already
            // marked, so it needs no test of its own.
            auto& loop = loops[existing];
            Array<U32> pending;
            if(!loop.contains[i]) {
                loop.contains[i] = 1;
                pending.push(U32(i));
            }

            while(pending.size()) {
                auto index = pending.pop().unwrap();

                for(auto predecessor: opt.local[dominance.blocks[index]]->incoming.contents(opt.local)) {
                    auto from = opt.local[predecessor]->index;
                    if(loop.contains[from]) continue;

                    loop.contains[from] = 1;
                    pending.push(from);
                }
            }
        }
    }

    for(auto& loop: loops) {
        for(U32 i = 0; i < count; i++) {
            if(loop.contains[i]) loop.blocks.push(i);
        }

        /*
         * The one block outside the loop that jumps straight to the header.
         *
         * Both halves matter. It has to be the *only* outside predecessor, or an instruction put
         * there would run on one way in and not the other; and it has to have the header as its
         * only successor, or the instruction would also run on a path that never enters the loop -
         * which for a pure value is a waste rather than a fault, but for a load is a read that the
         * program did not ask for.
         *
         * Where there is no such block the loop is simply left alone. Making one means splitting an
         * edge, which is CFG surgery with phis attached, and every loop this front end emits already
         * has one: `while` builds the test as a block of its own with a single `jmp` into it.
         */
        for(auto predecessor: opt.local[dominance.blocks[loop.header]]->incoming.contents(opt.local)) {
            auto from = opt.local[predecessor]->index;
            if(loop.contains[from]) continue;

            if(loop.preheader != Loop::kNone) {
                loop.preheader = Loop::kNone;
                break;
            }

            auto block = opt.local[predecessor];
            auto single = block->outgoing[0] == dominance.blocks[loop.header] && !block->outgoing[1];
            loop.preheader = single ? from : Loop::kNone;

            if(loop.preheader == Loop::kNone) break;
        }
    }

    for(Size i = 1; i < loops.size(); i++) {
        for(Size j = i; j > 0 && loops[j].blocks.size() < loops[j - 1].blocks.size(); j--) {
            swap(loops[j], loops[j - 1]);
        }
    }
}

/*
 * Which locals a callee could not reach.
 *
 * The use list answers this exactly, for the reason opt_scalar.cpp gives at greater length: every
 * instruction naming a place rooted in a local is recorded as a user of that local's `Alloc`, so a
 * local whose users are all reads and writes *of its own place* has had its address handed to
 * nothing. It was not borrowed, it was not pointed at, it was not passed and it was not captured,
 * because each of those is an instruction and each would be in the list.
 *
 * Four kinds are admitted and everything else declines. `Borrow` and `Address` hand out the address
 * that is the whole question; `Drop` runs a teardown that receives one; `Move`, `Swap` and
 * `Exchange` are ownership transfers the analyses already decided and not something to reason about
 * the storage of here.
 *
 * The alloc appearing as an *operand* rather than as a place root is the whole-aggregate read a call
 * argument is - `call g, %v2` hands the record itself on - and is the one way a local on the list
 * above still escapes. opt_arg.cpp found the same case from the other side.
 *
 * Flow-insensitive on purpose: a local borrowed anywhere is treated as reachable everywhere, which
 * costs the forwarding before the borrow and needs no reasoning about where a loan ends.
 */
void computeContainment(OptContext& opt, Array<U8>& contained) {
    contained.clear();

    for(U32 i = 0; i < opt.function->localCount(); i++) {
        auto slot = opt.function->localAt(opt.local, i);

        // A `&` parameter's slot is the caller's storage and a closure environment is the function
        // value's; neither is contained by this frame whatever this frame does.
        auto ok = slot.value && opt.local[slot.value]->kind == Value::Alloc &&
                  !slot.borrowed && !slot.closureEnv;

        if(ok) {
            for(auto user: opt.local[slot.value]->uses.contents(opt.local)) {
                auto& instruction = *opt.local[user];

                switch(instruction.kind) {
                    case Value::Init:
                    case Value::Assign:
                    case Value::LoadPlace:
                    case Value::Copy:
                        break;
                    default:
                        ok = false;
                }

                if(!ok) break;

                eachOperand(opt.local, instruction, [&](ModulePtr<Value> operand) {
                    if(operand == slot.value) ok = false;
                });

                if(!ok) break;
            }
        }

        contained.push(U8(ok));
    }
}

// The blocks control can actually get to. Needed because the analysis below starts from the
// optimistic end, and a cycle nothing enters would otherwise convince itself that it holds whatever
// it likes - there being no path into it to contradict the claim.
void computeReachable(OptContext& opt, Array<U8>& reachable) {
    auto count = opt.function->blocks.size();
    for(Size i = 0; i < count; i++) reachable.push(0);
    if(!count) return;

    Array<U32> pending;
    reachable[0] = 1;
    pending.push(0);

    while(pending.size()) {
        auto index = pending.pop().unwrap();

        for(auto successor: opt.local[opt.function->blocks.get(opt.local, index)]->outgoing) {
            if(!successor) continue;

            auto to = opt.local[successor]->index;
            if(reachable[to]) continue;

            reachable[to] = 1;
            pending.push(to);
        }
    }
}

bool staysInFrame(OptContext& opt, Array<U8>& contained, const Place& place) {
    if(place.root != PlaceRoot::Local) return false;
    if(place.local >= contained.size() || !contained[place.local]) return false;

    // `get` is a read that the list spells as a mutation, which is why every walk of a projection
    // path in this directory casts the constness off rather than taking a copy of the path.
    auto& projections = const_cast<Place&>(place).projections;

    for(Size i = 0; i < projections.size(); i++) {
        switch(projections.get(opt.local, i).kind) {
            case ProjectionKind::Field:
            case ProjectionKind::Downcast:
            case ProjectionKind::Discriminant:
            case ProjectionKind::Unit:
                break;
            default:
                return false;
        }
    }

    return true;
}
