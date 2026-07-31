#include "build.h"

/*
 * Control flow.
 *
 * JS has no `goto`, so a CFG has to be recovered as `if`, `for(;;)` and labelled `break`/
 * `continue`. Two facts make that a small pass rather than a Relooper: the resolver only produces
 * reducible graphs, because it only produces them from structured source; and the immediate
 * post-dominator of a branch *is* the block the two arms join at, which is the same question the
 * native side answers with a label.
 *
 * The whole algorithm is: a branch becomes an `if` whose arms are emitted up to the branch's
 * post-dominator, and a block with an edge back to it becomes a labelled `for(;;)` whose body is
 * emitted up to *its* post-dominator. Reaching the stopping block means falling out of the
 * construct, which is what makes `break` and fall-through the same statement.
 */

namespace js {

namespace {

U32 blockOf(Gen& g, ModulePtr<Block> pointer) {
    auto found = g.blockIndex.get(U32(pointer));
    return found ? found.unwrap() : kNoBlock;
}

void successorsOf(Gen& g, U32 block, U32* target, U32& count) {
    count = 0;
    auto terminator = g.local[g.blocks[block]]->terminator;
    if(!terminator) return;

    auto& instruction = *g.local[terminator];
    if(instruction.kind == Value::Je) {
        target[count++] = blockOf(g, ((InstJe&)instruction).thenBlock);
        target[count++] = blockOf(g, ((InstJe&)instruction).elseBlock);
    } else if(instruction.kind == Value::Jmp) {
        target[count++] = blockOf(g, ((InstJmp&)instruction).target);
    }
}

/*
 * The dominance fixpoint, over whichever direction the caller walks in.
 *
 * `root` is the node whose set is itself alone - the entry for dominators, the exit for post-
 * dominators - and `neighbours` yields what the intersection runs over: predecessors one way,
 * successors the other. Everything else about the two questions is the same question, which is why
 * it is written once.
 *
 * The textbook set fixpoint rather than the near-linear algorithm, because a function's blocks are
 * counted in tens: the quadratic form is a few microseconds and is obviously correct, and being
 * obviously correct is worth more here than being fast, since a wrong answer is a mis-structured
 * loop rather than a slow one.
 */
template<class F>
void dominanceSets(IndexSetList& sets, IndexSet& next, Size count, U32 root, F&& neighbours) {
    sets.reset(count, count);
    if(!count) return;

    for(Size i = 0; i < count; i++) {
        if(U32(i) != root) sets[i].fill();
    }

    sets[root].reset(count);
    sets[root].set(root, true);

    auto changed = true;
    while(changed) {
        changed = false;

        for(Size i = 0; i < count; i++) {
            if(U32(i) == root) continue;

            // A block nothing reaches is dominated by itself alone, which keeps it out of every
            // other block's answer rather than poisoning the intersection with the full set - so
            // this starts empty and the first neighbour is what fills it.
            next.reset(count);
            auto any = false;

            neighbours(U32(i), [&](U32 neighbour) {
                if(any) next.intersectWith(sets[neighbour]);
                else next.copyFrom(sets[neighbour]);

                any = true;
            });

            next.set(i, true);

            if(next.equals(sets[i])) continue;

            sets[i].copyFrom(next);
            changed = true;
        }
    }
}

// The immediate dominator is the closest one, and the closest is the one whose own set is largest:
// it dominates everything that dominates this block except this block itself.
U32 closestOf(IndexSetList& sets, Size block, U32 fallback) {
    auto count = sets.size();
    auto best = fallback;
    Size bestSize = 0;

    for(Size j = 0; j < count; j++) {
        if(j == block || !sets[block][j]) continue;

        Size size = 0;
        for(Size k = 0; k < count; k++) if(sets[j][k]) size++;

        if(size > bestSize) {
            bestSize = size;
            best = U32(j);
        }
    }

    return best;
}

void computePostDominators(Gen& g) {
    auto count = g.blocks.size();
    auto exit = U32(count);

    // Straight into where it is kept, and with the fixpoint's own scratch alongside it: both
    // outlive the function being emitted, so the next one reuses the rows rather than building them.
    auto& sets = g.postDominators;
    dominanceSets(sets, g.flowScratch, count + 1, exit, [&](U32 block, auto&& yield) {
        U32 successors[2];
        U32 successorCount;
        successorsOf(g, block, successors, successorCount);

        auto any = false;
        for(U32 s = 0; s < successorCount; s++) {
            if(successors[s] == kNoBlock) continue;

            yield(successors[s]);
            any = true;
        }

        // A `ret` block: its only successor is the function's exit.
        if(!any) yield(exit);
    });

    g.ipdom.clear();
    for(Size i = 0; i < count; i++) g.ipdom.push(closestOf(sets, i, exit));
}

// The same fixpoint the other way round, and the reason it is needed rather than a block-order
// heuristic: a `match` compiles to a decision tree whose arms fall through to the *next* test, so
// an edge from a later block to an earlier one is routine and is not a back edge.
void computeDominators(Gen& g) {
    auto count = g.blocks.size();

    auto& sets = g.dominators;
    dominanceSets(sets, g.flowScratch, count, 0, [&](U32 block, auto&& yield) {
        auto incoming = g.local[g.blocks[block]]->incoming;

        for(auto predecessor: incoming.contents(g.local)) {
            auto p = blockOf(g, predecessor);
            if(p != kNoBlock) yield(p);
        }
    });

    g.idom.clear();
    for(Size i = 0; i < count; i++) g.idom.push(closestOf(sets, i, kNoBlock));

    // A back edge is an edge whose target dominates its source, which is the definition rather than
    // an approximation of one - and it is what makes the loop headers here exactly the loops.
    g.loopHeader.reset(count);

    for(Size i = 0; i < count; i++) {
        U32 successors[2];
        U32 successorCount;
        successorsOf(g, U32(i), successors, successorCount);

        for(U32 s = 0; s < successorCount; s++) {
            auto target = successors[s];
            if(target != kNoBlock && sets[i][target]) g.loopHeader.set(target, true);
        }
    }
}

// Whether this block is the header of a loop currently being emitted, which is what tells entering
// one from re-entering it.
bool loopOpen(Gen& g, U32 block) {
    for(auto& exit: g.exits) {
        if(exit.loop && exit.block == block) return true;
    }

    return false;
}

Exit* findExit(Gen& g, U32 block) {
    for(Size i = g.exits.size(); i > 0; i--) {
        if(g.exits[i - 1].block != block) continue;

        // The first entry into a loop header is the header itself rather than a jump back to it.
        if(g.exits[i - 1].loop && !g.emitted[block]) return nullptr;
        return &g.exits[i - 1];
    }

    return nullptr;
}

U32 predecessorCount(Gen& g, U32 block) {
    auto incoming = g.local[g.blocks[block]]->incoming;
    U32 count = 0;
    for(auto predecessor: incoming.contents(g.local)) {
        if(blockOf(g, predecessor) != kNoBlock) count++;
    }

    return count;
}

bool dominates(Gen& g, U32 owner, U32 block) {
    while(block != kNoBlock && block < g.idom.size()) {
        if(block == owner) return true;
        block = g.idom[block];
    }

    return false;
}

// Whether this block owns a merge that will become a labelled block - the static half of
// ownedMerges, asked of a block that is not being emitted yet.
bool ownsMerge(Gen& g, U32 owner) {
    for(Size i = 0; i < g.blocks.size(); i++) {
        if(g.idom[i] != owner || U32(i) == g.ipdom[owner]) continue;
        if(g.loopHeader[i] || predecessorCount(g, U32(i)) < 2) continue;

        return true;
    }

    return false;
}

/*
 * Whether a labelled block will be opened inside this one's arms whose escape lands on this block's
 * own join.
 *
 * That is the case the join needs a label for, and it is one level further out than the rule beside
 * it: the merge is owned by an inner block, so the inner block is the one that labels it, and an arm
 * that skips past it lands at a join the inner block does not own and cannot label. `ipdom[inner] ==
 * ipdom[block]` is what says the escape lands *here* - a nested region whose own join is somewhere
 * inside is closed by the structure that contains it and needs nothing from this one.
 */
bool nestedMerge(Gen& g, U32 block) {
    for(Size i = 0; i < g.blocks.size(); i++) {
        auto inner = U32(i);
        if(inner == block || g.ipdom[inner] != g.ipdom[block]) continue;
        if(!dominates(g, block, inner)) continue;

        if(ownsMerge(g, inner)) return true;
    }

    return false;
}

/*
 * The join points this block owns, outermost first.
 *
 * A block with several predecessors that is not an `if` join and not a loop header is what a
 * `match` produces: alternative N's test falls through to alternative N+1, and so does the previous
 * one's failure. It is reached from inside two different constructs, so it cannot be nested inside
 * either - it becomes a labelled block that both of them `break` out to, with its own code after.
 *
 * The block that owns it is its immediate dominator, which is the only block from which every path
 * to it is visible. Ordering is by post-dominance: the merge the others eventually reach is the
 * outermost, since everything else has to be able to leave through it.
 */
void ownedMerges(Gen& g, U32 block, U32 stopAt, Array<U32>& target) {
    auto available = [&](U32 candidate) {
        if(candidate >= g.blocks.size() || candidate == block) return false;
        if(candidate == stopAt || g.emitted[candidate]) return false;
        if(g.loopHeader[candidate]) return false;

        for(auto& exit: g.exits) if(exit.block == candidate) return false;
        return true;
    };

    for(Size i = 0; i < g.blocks.size(); i++) {
        if(g.idom[i] != block || !available(U32(i))) continue;
        if(predecessorCount(g, U32(i)) < 2) continue;

        // Not the branch's own join: that one is where the `if` ends, and an `if` needs no label to
        // end at its own next statement.
        if(U32(i) == g.ipdom[block]) continue;

        target.push(U32(i));
    }

    /*
     * The branch's own join needs a label too, but only once something else does.
     *
     * On its own it is where control was going anyway - the `if` ends and the next statement is it,
     * with nothing to break out of. Once there is a merge nested inside, an arm that skips straight
     * past that merge has to leave both, so the join becomes the outer of the two labelled blocks
     * rather than the statement after them.
     */
    if((target.isNotEmpty() || nestedMerge(g, block)) && available(g.ipdom[block])) {
        target.push(g.ipdom[block]);
    }

    for(Size i = 1; i < target.size(); i++) {
        for(Size j = i; j > 0; j--) {
            // `target[j]` belongs further out when it post-dominates its neighbour.
            if(!g.postDominators[target[j - 1]][target[j]]) break;

            auto swap = target[j - 1];
            target[j - 1] = target[j];
            target[j] = swap;
        }
    }
}

// The phi copies one edge owes. A phi is a value the *predecessors* produce, so the assignment
// belongs on the edge rather than at the join - which is exactly what makes it disappear when the
// two arms of an `if` are emitted in place.
void genPhiCopies(Gen& g, U32 from, U32 to) {
    if(to == kNoBlock) return;

    auto source = g.blocks[from];
    auto target = g.local[g.blocks[to]];

    for(auto phiPointer: target->phis.contents(g.local)) {
        auto& phi = *g.local[phiPointer];
        auto found = g.phis.get(U32(phiPointer));
        if(!found) continue;

        for(auto input: phi.inputs.contents(g.local)) {
            if(input.block != source) continue;
            emitExpr(g, assign(g, variable(g, found.unwrap()), useValue(g, input.value)));
            break;
        }
    }
}

void emitTerminator(Gen& g, U32 block, U32& next, U32 stopAt, bool& done) {
    auto terminator = g.local[g.blocks[block]]->terminator;
    done = false;

    if(!terminator) {
        done = true;
        return;
    }

    auto& instruction = *g.local[terminator];

    switch(instruction.kind) {
        case Value::Ret: {
            auto& returned = (InstRet&)instruction;
            auto value = returned.value && !isUnit(g.global, g.local[returned.value]->type)
                ? useValue(g, returned.value) : JsPtr<Expr>(nullptr);

            emit(g, make<ReturnStmt>(g, value));
            done = true;
            break;
        }
        case Value::Jmp: {
            auto target = blockOf(g, ((InstJmp&)instruction).target);
            genPhiCopies(g, block, target);
            next = target;
            break;
        }
        case Value::Je: {
            auto& branch = (InstJe&)instruction;
            auto cond = useValue(g, branch.cond);
            auto thenBlock = blockOf(g, branch.thenBlock);
            auto elseBlock = blockOf(g, branch.elseBlock);
            auto join = g.ipdom[block];

            /*
             * The two arms rejoin at this branch's post-dominator, and that is where emission of
             * each of them stops - unless the post-dominator is outside the region being emitted,
             * which is what happens when a `match` arm jumps past several enclosing labelled blocks
             * at once. Then the arms stop where the region does and reaching the join is a `break`
             * that findExit issues, with nothing left to emit after the branch.
             */
            auto arms = join;
            auto continues = true;

            if(join != stopAt && (join >= g.blocks.size() || g.emitted[join] || findExit(g, join))) {
                arms = stopAt;
                continues = false;
            }

            auto then = collect(g, [&] {
                genPhiCopies(g, block, thenBlock);
                emitChain(g, thenBlock, arms);
            });

            auto otherwise = collect(g, [&] {
                genPhiCopies(g, block, elseBlock);
                emitChain(g, elseBlock, arms);
            });

            emit(g, make<IfStmt>(g, cond, then, otherwise));

            if(!continues) {
                done = true;
                break;
            }

            next = join;
            break;
        }
        default:
            g.context.diagnostics.error("internal error: unexpected terminator in JS codegen"_v,
                                        instruction.source);
            done = true;
            break;
    }
}

} // namespace

void prepareCfg(Gen& g, Function& function) {
    g.blocks.clear();
    g.blockIndex.reset();
    g.exits.clear();

    for(auto blockPointer: function.blocks.contents(g.local)) {
        g.blockIndex.add(U32(blockPointer), U32(g.blocks.size()));
        g.blocks.push(blockPointer);
    }

    computePostDominators(g);
    computeDominators(g);

    g.emitted.reset(g.blocks.size());
}

void emitChain(Gen& g, U32 block, U32 stopAt) {
    while(block != stopAt && block < g.blocks.size()) {
        // An edge out of a construct this emission is inside. Falling through is the same statement
        // where the construct's end is where control was going anyway, which is why `stopAt` is
        // checked first and emits nothing.
        if(auto exit = findExit(g, block)) {
            if(exit->loop) {
                emit(g, make<ContinueStmt>(g, exit->label));
            } else {
                emit(g, make<BreakStmt>(g, exit->label));
            }

            return;
        }

        if(g.loopHeader[block] && !g.emitted[block] && !loopOpen(g, block)) {
            auto follow = g.ipdom[block];
            auto label = generatedName(g, "L"_v, g.labelCounter++);

            g.exits.push(Exit { block, label, true });
            auto body = collect(g, [&] {
                emitChain(g, block, follow);

                // Falling off the end of the body means the loop is finished: everything that
                // repeats said so with an explicit `continue`.
                emit(g, make<BreakStmt>(g, label));
            });
            g.exits.pop();

            emit(g, make<LabelledStmt>(g, label, asStmt(g, make<ForeverStmt>(g, body))));
            block = follow;
            continue;
        }

        // A join this block owns but that is neither its `if` join nor a loop - see ownedMerges.
        // One labelled block per merge, outermost first; the recursion picks up the rest.
        Array<U32> merges;
        ownedMerges(g, block, stopAt, merges);

        if(merges.isNotEmpty()) {
            auto merge = merges[0];
            auto label = generatedName(g, "B"_v, g.labelCounter++);

            g.exits.push(Exit { merge, label, false });
            auto body = collect(g, [&] { emitChain(g, block, merge); });
            g.exits.pop();

            emit(g, make<LabelledStmt>(g, label, asStmt(g, make<BlockStmt>(g, body))));
            block = merge;
            continue;
        }

        if(g.emitted[block]) {
            g.context.diagnostics.error("internal error: the JS backend could not structure this function's control flow"_v,
                                        g.local[g.blocks[block]]->source);
            return;
        }

        g.emitted.set(block, true);

        for(auto instruction: g.local[g.blocks[block]]->instructions.contents(g.local)) {
            genInstruction(g, instruction);
        }

        U32 next = kNoBlock;
        auto done = false;
        emitTerminator(g, block, next, stopAt, done);
        if(done) return;

        block = next;
    }
}

} // namespace js
