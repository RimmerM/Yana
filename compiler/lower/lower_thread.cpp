#include "lower_thread.h"
#include "lower_builder.h"

/*
 * See lower_thread.h for the shape and the four preconditions. This file is the recognizer, the
 * phi that repairs the reads below the join, and the edge surgery.
 */

namespace {

// A predecessor whose edge answers nothing, in the arrays below.
static constexpr U32 kUndecided = maxLimit<U32>;

/*
 * Dominance, asked only where the question has an answer.
 *
 * `LowerBlock::dominates` walks the tree from the queried block's postorder position, and a block no
 * postorder walk reached has none - `kNullBlock` is not an index into that list. Every caller here is
 * a refusal where the answer is no, so an unreachable block answers no and the pass declines to
 * reason about a region nothing can run.
 */
bool dominatesBlock(LowerBlock* block, LowerBlock* other, const DominatorTree& dominators) {
    if(block == other) return true;
    if(block->postIndex == kNullBlock || other->postIndex == kNullBlock) return false;

    return block->dominates(other, dominators);
}

// How many bits of a value of this type are the value, which is what a comparison of two literals
// has to be stated in - the same rule lower_fold.cpp's own arithmetic is stated at.
U32 widthOf(LowerType type) {
    return type == LowerType::Int32 ? 32 : 64;
}

U64 maskOf(U32 bits) {
    return bits >= 64 ? maxLimit<U64> : (U64(1) << bits) - 1;
}

bool constantOf(LowerBase base, LowerValue* value, U64& into) {
    auto inst = value->inst();
    if(inst->kind != LowerInst::Imm || !isInt(value->type)) return false;

    into = ((LowerImm*)inst)->i & maskOf(widthOf(value->type));
    return true;
}

I64 signedValue(U64 value, U32 bits) {
    if(bits >= 64) return I64(value);

    auto spare = 64 - bits;
    return I64(value << spare) >> spare;
}

/*
 * Whether the relation holds between two literals of a given width.
 *
 * The two float codes are declined rather than answered: `uno` and `ord` are questions about a NaN,
 * and `constantOf` never hands over a float in the first place - so a branch on one of them reaches
 * here only if something above changed, and refusing is the answer that cannot be wrong.
 */
bool compareConstants(LowerCmp cmp, U64 a, U64 b, U32 bits, bool& into) {
    auto sa = signedValue(a, bits);
    auto sb = signedValue(b, bits);

    switch(cmp) {
        case LowerCmp::eq:  into = a == b;   return true;
        case LowerCmp::neq: into = a != b;   return true;
        case LowerCmp::gt:  into = a > b;    return true;
        case LowerCmp::ge:  into = a >= b;   return true;
        case LowerCmp::lt:  into = a < b;    return true;
        case LowerCmp::le:  into = a <= b;   return true;
        case LowerCmp::igt: into = sa > sb;  return true;
        case LowerCmp::ige: into = sa >= sb; return true;
        case LowerCmp::ilt: into = sa < sb;  return true;
        case LowerCmp::ile: into = sa <= sb; return true;
        default:                             return false;
    }
}

// The branch of a block, and the phi its condition is a function of.
struct Condition {
    LowerInstPhi* phi = nullptr;

    // The comparison standing between the phi and the branch, or null where the branch reads the
    // phi itself. Its other operand is `constant`, and `phiOnLeft` says which side the phi was on -
    // which matters for every relation but equality.
    LowerInstCmp* compare = nullptr;
    U64 constant = 0;
    bool phiOnLeft = true;
};

bool isPhiOf(LowerBase base, LowerBlock* block, LowerValue* value, LowerInstPhi*& into) {
    auto inst = value->inst();
    if(inst->kind != LowerInst::Phi || base[inst->block] != block) return false;

    into = (LowerInstPhi*)inst;
    return true;
}

/*
 * The branch at the end of a block, read back as "one phi, and what is asked of it".
 *
 * Two shapes. `je %tag` reads the phi directly, which is what a `Bool` carried out of a diamond is;
 * `%c = cmp_eq %tag, 1 ; je %c` is what a discriminant test is, and is the one every level of a
 * nested `Outcome` produces. Anything else - a condition computed from two values, a comparison
 * something else also reads - is declined, since skipping the block would have to skip the
 * computation with it.
 */
bool readCondition(LowerBase base, LowerBlock* block, Condition& into) {
    auto terminator = base[block->terminator];
    if(!terminator || terminator->kind != LowerInst::Je) return false;

    auto je = (LowerInstJe*)terminator;

    // A branch already carrying its comparison in the flags is a form this cannot reason about: the
    // condition register it names is not what decides the edge. Nothing above the backend produces
    // one, and refusing here is what keeps that true rather than assumed.
    if(je->getEmbeddedCmp()) return false;

    auto cond = base[je->cond];

    if(isPhiOf(base, block, cond, into.phi)) {
        return block->instructions.size() == 0;
    }

    auto inst = cond->inst();
    if(inst->kind != LowerInst::Cmp || base[inst->block] != block) return false;
    if(block->instructions.size() != 1 || cond->uses.size() != 1) return false;

    auto compare = (LowerInstCmp*)inst;
    auto lhs = base[compare->lhs];
    auto rhs = base[compare->rhs];

    if(isPhiOf(base, block, lhs, into.phi)) {
        if(!constantOf(base, rhs, into.constant)) return false;
        into.phiOnLeft = true;
    } else if(isPhiOf(base, block, rhs, into.phi)) {
        if(!constantOf(base, lhs, into.constant)) return false;
        into.phiOnLeft = false;
    } else {
        return false;
    }

    into.compare = compare;
    return true;
}

/*
 * Whether this edge may be pointed at that successor at all, which is a question about the *edge*
 * rather than about the value it carries.
 *
 * Two shapes are refused, and both are ways for one predecessor to end up naming one block twice.
 *
 *  - **A predecessor that is also the successor.** A block whose loop back edge enters the join and
 *    whose arm re-enters it is the shape; threading it points the block at itself, which is a loop
 *    with no way out rather than a shorter path to one.
 *  - **A predecessor whose other arm already leads there.** `je %c, join, X` threaded onto `X` is
 *    `je %c, X, X`, which is not a branch and which `LowerBlock::addInst` asserts against. Turning
 *    it into a `jmp` would be right and is a second rewrite; declining leaves it to the round that
 *    folds the condition instead.
 */
bool canRedirect(LowerBase base, LowerBlock* from, LowerBlock* block, LowerBlock* to) {
    if(from == to) return false;

    auto terminator = base[from->terminator];
    if(!terminator || terminator->kind != LowerInst::Je) return true;

    auto je = (LowerInstJe*)terminator;
    auto other = je->then == block - base ? je->otherwise : je->then;

    return base[other] != to;
}

// Which arm the branch takes when the condition phi arrives holding this value, or nothing where the
// value is not a literal.
bool decideEdge(LowerBase base, const Condition& condition, LowerValue* incoming, bool& taken) {
    U64 value;
    if(!constantOf(base, incoming, value)) return false;

    if(!condition.compare) {
        taken = value != 0;
        return true;
    }

    auto bits = widthOf(base[condition.compare->lhs]->type);
    auto a = condition.phiOnLeft ? value : condition.constant;
    auto b = condition.phiOnLeft ? condition.constant : value;

    return compareConstants(condition.compare->getCmp(), a, b, bits, taken);
}

/*
 * Where a read of a value happens, for the dominance question.
 *
 * A phi does not read its alternatives where it stands - it reads each one on the edge that offers
 * it - so the block that has to be dominated is the *source* of the entry naming the value, and a
 * phi naming one value on two edges asks the question twice.
 */
template<class F>
void eachReadingBlock(LowerBase base, LowerValue* value, LowerInst* user, F&& f) {
    if(user->kind != LowerInst::Phi) {
        f(base[user->block]);
        return;
    }

    auto phi = (LowerInstPhi*)user;
    auto used = phi->used();
    auto sources = phi->sources();

    for(Size i = 0; i < used.length; i++) {
        if(base[used.ptr[i]] == value) f(base[sources[i]]);
    }
}

/*
 * Whether every read of every phi of this block is one the repair can reach.
 *
 * Inside the block it has to be the branch or the comparison the branch reads, both of which go with
 * the block. Outside it, it has to sit under one of the two successors, which is the fourth
 * precondition in lower_thread.h: a read at a join *below* the two arms is dominated by this block
 * and by neither of them, so there is no one place a phi answering it could go.
 */
bool readsAreRepairable(LowerBase base, LowerBlock* block, const Condition& condition,
                        const DominatorTree& dominators) {
    auto terminator = base[block->terminator];
    auto then = base[block->outgoing[0]];
    auto otherwise = base[block->outgoing[1]];

    for(auto phiPtr: block->phis.contents(base)) {
        auto phi = base[phiPtr];
        auto result = ((LowerInstSingle*)phi)->created().ptr;
        auto repairable = true;

        for(auto userPtr: result->uses.contents(base)) {
            auto user = base[userPtr];

            if(base[user->block] == block) {
                if(user != terminator && user != (LowerInst*)condition.compare) repairable = false;
                continue;
            }

            eachReadingBlock(base, result, user, [&](LowerBlock* from) {
                if(!dominatesBlock(then, from, dominators) &&
                   !dominatesBlock(otherwise, from, dominators)) {
                    repairable = false;
                }
            });
        }

        if(!repairable) return false;
    }

    return true;
}

// What one phi of the threaded block offers on the edge from a given predecessor.
LowerPtr<LowerValue> incomingFrom(LowerBase base, LowerInstPhi* phi, LowerBlock* from) {
    auto used = phi->used();
    auto sources = phi->sources();

    for(Size i = 0; i < used.length; i++) {
        if(base[sources[i]] == from) return used.ptr[i];
    }

    return nullptr;
}

// One edge moved off the threaded block and onto the successor it had already chosen - the
// terminator's own target field, the predecessor's successor list and both blocks' edge lists, which
// is the three places an edge lives.
void redirectEdge(LowerBase base, Region<LowerRegion>& arena, LowerBlock* from, LowerBlock* block,
                  LowerBlock* to) {
    auto terminator = base[from->terminator];

    if(terminator->kind == LowerInst::Je) {
        auto je = (LowerInstJe*)terminator;
        if(je->then == block - base) je->then = to - base;
        if(je->otherwise == block - base) je->otherwise = to - base;
    } else if(terminator->kind == LowerInst::Jmp) {
        auto jmp = (LowerInstJmp*)terminator;
        if(jmp->then == block - base) jmp->then = to - base;
    }

    for(auto& successor: from->outgoing) {
        if(successor == block - base) successor = to - base;
    }

    for(Size i = 0; i < block->incoming.size(); i++) {
        if(base[block->incoming.get(base, i)] != from) continue;

        block->incoming.remove(base, i);
        break;
    }

    to->incoming.push(arena, from - base);
}

// Taking the threaded block out of the function once nothing reaches it. Renumbering is not
// optional: `index` is a position in this list and half the analyses index arrays by it.
void dropBlock(LowerBase base, LowerFunction& fun, LowerBlock* block) {
    for(auto successorPtr: block->outgoing) {
        if(!successorPtr) continue;

        auto successor = base[successorPtr];
        for(Size i = 0; i < successor->incoming.size(); i++) {
            if(base[successor->incoming.get(base, i)] != block) continue;

            successor->incoming.remove(base, i);
            break;
        }
    }

    for(auto phiPtr: block->phis.contents(base)) detach(base, (LowerInst*)base[phiPtr]);
    for(auto instPtr: block->instructions.contents(base)) detach(base, base[instPtr]);
    if(block->terminator) detach(base, base[block->terminator]);

    for(Size i = 0; i < fun.blocks.size(); i++) {
        if(fun.blocks.get(base, i) != block - base) continue;

        fun.blocks.remove(base, i);
        break;
    }

    for(Size i = 0; i < fun.blocks.size(); i++) base[fun.blocks.get(base, i)]->index = BlockIndex(i);
}

/*
 * A phi that says nothing, removed - the same rule `removeTrivialPhis` applies during promotion,
 * with one alternative more admitted than that one takes.
 *
 * Threading produces these by construction and they are the point rather than a tidy-up: both arms
 * of a discriminant test carry the *same* literal out to the next level, so the phi that merges them
 * is that literal - and only once it is written as one does the level above read a constant and come
 * apart in its turn. Answers whether anything changed, so the caller knows to go round again.
 *
 * A self-reference is not an answer, for the reason that function gives: a loop-carried phi whose
 * only other alternative is one value is that value.
 *
 * **The two arms hold two `imm 1`, not one.** An immediate is placed in the block that reads it, so
 * the arms of a diamond that both answer `Just` each build their own - and a rule comparing
 * alternatives by identity finds a phi of two different values and leaves it standing, which is
 * exactly what the first draft of this did and why the chain stopped coming apart after one level.
 * `eliminateCommonValues` unifies the pair eventually and runs far below here. So equal constants
 * count as agreeing, and the answer is a *fresh* immediate in the phi's own block: neither arm's
 * dominates the paths through the other.
 */
bool collapseTrivialPhis(LowerBase base, LowerModule& module, LowerFunction& fun) {
    auto& arena = module.arena;
    auto collapsed = false;
    auto changed = true;

    while(changed) {
        changed = false;

        for(auto blockPtr: fun.blocks.contents(base)) {
            auto block = base[blockPtr];

            for(Size at = 0; at < block->phis.size(); at++) {
                auto phi = base[block->phis.get(base, at)];
                auto result = ((LowerInstSingle*)phi)->created().ptr;
                auto operands = phi->used();

                LowerPtr<LowerValue> only = nullptr;
                U64 constant = 0;
                auto agree = true;
                auto allConstant = true;

                for(Size i = 0; i < operands.length; i++) {
                    auto value = operands.ptr[i];
                    if(base[value] == result) continue;

                    U64 held;
                    auto isConstant = constantOf(base, base[value], held) &&
                                      base[value]->type == result->type;

                    if(!only) {
                        only = value;
                        allConstant = isConstant;
                        constant = held;
                        continue;
                    }

                    if(only != value) agree = false;
                    if(!isConstant || held != constant) allConstant = false;
                }

                if(!only || (!agree && !allConstant)) continue;

                LowerValue* answer;

                if(agree) {
                    answer = base[only];
                } else {
                    auto imm = block->addInst(base, new (arena) LowerImm(StringId(), result->type,
                                                                        constant));
                    answer = ((LowerInstSingle*)imm)->created().ptr;

                    /*
                     * To the front of the block, which is not tidiness.
                     *
                     * The phi being replaced stood above everything, so a reader of it may be the
                     * block's own first instruction - the comparison the branch reads is exactly
                     * that - and an immediate appended at the end would be a definition below its
                     * use. `addInst` only appends, so the rotation is how the position is said.
                     */
                    auto& list = block->instructions;
                    for(Size i = list.size() - 1; i > 0; i--) {
                        auto above = list.get(base, i - 1);
                        list.set(base, i - 1, list.get(base, i));
                        list.set(base, i, above);
                    }
                }

                detach(base, (LowerInst*)phi);
                replaceUses(base, arena, result - base, answer - base);
                block->phis.remove(base, at--);

                changed = true;
                collapsed = true;
            }
        }
    }

    return collapsed;
}

/*
 * A branch on a literal, turned into the jump it is.
 *
 * The companion of the collapse above and not a separate concern: the whole of what the two levels
 * of a nested `Outcome` become once the phis merging them say one number is `cmp_eq 1, 1 ; je`, and
 * neither the fold below this pass nor the backend takes a decided branch out of the graph. The arm
 * that stops being reachable is left to the sweep, which is already here for threading's own sake.
 *
 * Answers whether anything changed.
 */
bool foldConstantBranches(LowerBase base, LowerModule& module, LowerFunction& fun) {
    auto folded = false;

    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];
        auto terminator = base[block->terminator];
        if(!terminator || terminator->kind != LowerInst::Je) continue;

        auto je = (LowerInstJe*)terminator;
        if(je->getEmbeddedCmp()) continue;

        // Either the condition is already a literal, or it is the comparison of two - which is what
        // the collapse above leaves behind at every level it answers, since the tag it made a literal
        // was being tested against one.
        auto cond = base[je->cond];
        U64 value;
        bool answer;

        if(constantOf(base, cond, value)) {
            answer = value != 0;
        } else if(cond->inst()->kind == LowerInst::Cmp) {
            auto compare = (LowerInstCmp*)cond->inst();
            auto lhs = base[compare->lhs];
            auto rhs = base[compare->rhs];

            U64 a, b;
            if(!constantOf(base, lhs, a) || !constantOf(base, rhs, b)) continue;
            if(!compareConstants(compare->getCmp(), a, b, widthOf(lhs->type), answer)) continue;
        } else {
            continue;
        }

        auto taken = answer ? je->then : je->otherwise;
        auto dropped = answer ? je->otherwise : je->then;

        // Both edges out of the three places an edge lives, since `addInst` records the taken one
        // again when the jump goes in and asserts that it is not already there.
        auto unlink = [&](LowerBlock* successor) {
            for(Size i = 0; i < successor->incoming.size(); i++) {
                if(base[successor->incoming.get(base, i)] != block) continue;

                successor->incoming.remove(base, i);
                break;
            }
        };

        unlink(base[je->then]);
        unlink(base[je->otherwise]);

        detach(base, terminator);
        block->terminator = nullptr;
        block->outgoing[0] = nullptr;
        block->outgoing[1] = nullptr;

        block->addInst(base, new (module.arena) LowerInstJmp(taken));

        /*
         * And the alternative the untaken arm's phis held for this block.
         *
         * The sweep below only reaches an arm that nothing at all can enter. One still reachable by
         * another path keeps its phis, and a phi naming a predecessor that no longer branches here
         * is an alternative on an edge that does not exist - which `validateLowerFunction` reports
         * and every consumer of a phi's source list would read as an edge.
         */
        SmallArray<LowerBlock*, 8> removed;
        removed.push(block);

        narrowBlockPhis(base, module.arena, base[dropped], removed);

        folded = true;
    }

    return folded;
}

/*
 * The blocks nothing reaches any more, and the edges into the ones that survive.
 *
 * Threading produces these, and produces them *correctly*: a loop whose preheader edge answers the
 * header's own test is a loop entered at the exit, and what is left behind is the whole body with no
 * way in. Nothing below this point sweeps one - `buildPostorder` simply does not visit it, which
 * leaves every array indexed by postorder position with a hole in it and `Eliminator::between`
 * reading a predecessor's `kNullBlock` as a subscript.
 *
 * So the sweep belongs to the pass that creates the case. It runs in the order the two halves
 * require: the phis of the survivors first, since a source that is about to stop existing has to
 * come out of the alternative list before the block holding the value is detached, and the blocks
 * themselves second.
 */
void removeUnreachableBlocks(LowerBase base, Region<LowerRegion>& arena, LowerFunction& fun) {
    SmallArray<LowerBlock*, 32> reachable;
    SmallArray<LowerBlock*, 32> pending;

    auto entry = base[fun.blocks.get(base, 0)];
    pending.push(entry);
    reachable.push(entry);

    auto known = [&](LowerBlock* block) {
        for(auto seen: reachable) {
            if(seen == block) return true;
        }

        return false;
    };

    while(pending.size()) {
        auto block = pending.pop().unwrap();

        for(auto successorPtr: block->outgoing) {
            if(!successorPtr) continue;

            auto successor = base[successorPtr];
            if(known(successor)) continue;

            reachable.push(successor);
            pending.push(successor);
        }
    }

    if(reachable.size() == fun.blocks.size()) return;

    SmallArray<LowerBlock*, 8> gone;
    for(auto blockPtr: fun.blocks.contents(base)) {
        if(!known(base[blockPtr])) gone.push(base[blockPtr]);
    }

    for(auto block: reachable) {
        auto stale = false;
        for(Size i = 0; i < block->incoming.size(); i++) {
            for(auto dead: gone) {
                if(base[block->incoming.get(base, i)] == dead) { stale = true; break; }
            }
        }

        if(!stale) continue;

        narrowBlockPhis(base, arena, block, gone);

        for(Size i = 0; i < block->incoming.size(); ) {
            auto dead = false;
            for(auto removed: gone) {
                if(base[block->incoming.get(base, i)] == removed) { dead = true; break; }
            }

            if(dead) block->incoming.remove(base, i);
            else i++;
        }
    }

    for(auto block: gone) {
        for(auto phiPtr: block->phis.contents(base)) detach(base, (LowerInst*)base[phiPtr]);
        for(auto instPtr: block->instructions.contents(base)) detach(base, base[instPtr]);
        if(block->terminator) detach(base, base[block->terminator]);
    }

    for(auto block: gone) {
        for(Size i = 0; i < fun.blocks.size(); i++) {
            if(fun.blocks.get(base, i) != block - base) continue;

            fun.blocks.remove(base, i);
            break;
        }
    }

    for(Size i = 0; i < fun.blocks.size(); i++) base[fun.blocks.get(base, i)]->index = BlockIndex(i);
}

/*
 * One successor's share of the repair: a phi at the join for every value the threaded edges have
 * just stopped passing through the block, and the reads below it pointed at that phi.
 *
 * The entry from the block itself is present exactly when the block survives. Where it does not, the
 * only paths into this successor are the threaded ones, and an alternative for an edge that no longer
 * exists would be a phi with a source no predecessor list names.
 */
void repairSuccessor(LowerBase base, Region<LowerRegion>& arena, LowerBlock* block, LowerBlock* to,
                     const SmallArray<LowerBlock*, 8>& threaded, bool blockSurvives,
                     const DominatorTree& dominators)
{
    for(auto phiPtr: block->phis.contents(base)) {
        auto phi = base[phiPtr];
        auto result = ((LowerInstSingle*)phi)->created().ptr;

        // Which reads this successor answers for. Collected before anything is built, because the
        // phi below becomes a reader itself and the walk would otherwise find it.
        SmallArray<LowerInst*, 8> readers;

        for(auto userPtr: result->uses.contents(base)) {
            auto user = base[userPtr];
            if(base[user->block] == block) continue;

            auto answers = false;
            eachReadingBlock(base, result, user, [&](LowerBlock* from) {
                if(dominatesBlock(to, from, dominators)) answers = true;
            });

            if(answers) readers.push(user);
        }

        if(readers.isEmpty()) continue;

        auto alternatives = U32(threaded.size()) + (blockSurvives ? 1 : 0);

        /*
         * One way in is not a phi at all.
         *
         * This is the common case rather than a corner: an `if` whose two arms each thread one edge
         * leaves each arm with a single threaded predecessor, so a phi here would be a phi with one
         * alternative in a block with one predecessor. `collapseTrivialPhis` would take it a round
         * later; not building it is what lets the *next* round see the literal it stands for, which
         * is what unwinds a chain of these in one pass rather than one level per round.
         */
        LowerValue* answer;

        if(alternatives == 1 && !blockSurvives) {
            answer = base[incomingFrom(base, phi, threaded[0])];
        } else {
            auto repair = makePhi(arena, result->type, alternatives);
            repair->source = phi->source;

            auto used = repair->used();
            auto sources = repair->sources();
            Size at = 0;

            if(blockSurvives) {
                used[at] = result - base;
                sources[at] = block - base;
                at++;
            }

            for(auto from: threaded) {
                used[at] = incomingFrom(base, phi, from);
                sources[at] = from - base;
                at++;
            }

            to->addInst(base, repair);
            answer = &repair->result;
        }

        for(auto reader: readers) {
            auto operands = reader->used();

            // Every operand naming the value, and only where this successor answers for it: a phi
            // reading the same value on two edges may have one of them under this arm and one under
            // the other, and rewriting both would answer for a path this phi does not stand on.
            if(reader->kind == LowerInst::Phi) {
                auto readerPhi = (LowerInstPhi*)reader;
                auto readerSources = readerPhi->sources();

                for(Size i = 0; i < operands.length; i++) {
                    if(base[operands.ptr[i]] != result) continue;
                    if(!dominatesBlock(to, base[readerSources[i]], dominators)) continue;

                    setOperand(base, arena, reader, operands.ptr[i], answer);
                }

                continue;
            }

            for(Size i = 0; i < operands.length; i++) {
                if(base[operands.ptr[i]] != result) continue;

                setOperand(base, arena, reader, operands.ptr[i], answer);
            }
        }
    }
}

/*
 * One block, threaded if it can be. Answers whether anything moved.
 *
 * The preconditions are lower_thread.h's four, in the order that makes the cheap ones first: what a
 * block holds, then what its successors are, then which edges answer, and only then the walk over
 * every read.
 */
bool threadBlock(LowerBase base, LowerModule& module, LowerFunction& fun, LowerBlock* block,
                 const DominatorTree& dominators)
{
    if(block->index == 0) return false;
    if(block->phis.isEmpty() || block->incoming.size() < 2) return false;

    // A block the postorder walk did not reach has no position in the dominator tree, so nothing
    // below can be classified against it. Unreachable code is somebody else's sweep.
    if(block->postIndex == kNullBlock) return false;

    Condition condition;
    if(!readCondition(base, block, condition)) return false;

    auto then = base[block->outgoing[0]];
    auto otherwise = base[block->outgoing[1]];

    // Both arms have to be reached only from here, which is what makes this block dominate them and
    // so makes "the reads to repair" exactly "the reads the arm dominates"; and neither may carry a
    // phi of its own, whose alternatives a new edge would have to be given room in. A block that is
    // its own successor is refused outright - the repair would be answering for its own edge.
    if(then == block || otherwise == block) return false;
    if(then->incoming.size() != 1 || otherwise->incoming.size() != 1) return false;
    if(!then->phis.isEmpty() || !otherwise->phis.isEmpty()) return false;

    // The predecessors that answer, in the block's own edge order.
    SmallArray<LowerBlock*, 8> sources;
    SmallArray<U32, 8> decided;
    Size answered = 0;

    for(auto incoming: block->incoming.contents(base)) {
        auto from = base[incoming];
        sources.push(from);

        auto value = incomingFrom(base, condition.phi, from);
        bool taken;

        if(value && decideEdge(base, condition, base[value], taken) &&
           canRedirect(base, from, block, taken ? then : otherwise))
        {
            decided.push(taken ? 0u : 1u);
            answered++;
        } else {
            decided.push(kUndecided);
        }
    }

    if(answered == 0) return false;

    /*
     * An arm that would be left with no way in at all.
     *
     * If every predecessor answers and they all answer the same way, threading all of them makes the
     * other arm unreachable - and the reads under it, which the fourth precondition allowed on the
     * strength of this block dominating them, would be left naming a phi that no longer exists.
     * Removing an unreachable region is a reachability sweep and is not this pass; keeping one edge
     * on the block is, and costs the one test that was going to be there anyway.
     */
    if(answered == sources.size()) {
        auto toThen = false, toElse = false;
        for(auto which: decided) (which == 0 ? toThen : toElse) = true;

        if(!toThen || !toElse) {
            for(Size i = 0; i < decided.size(); i++) {
                if(decided[i] == kUndecided) continue;

                decided[i] = kUndecided;
                answered--;
                break;
            }
        }
    }

    if(answered == 0) return false;
    if(!readsAreRepairable(base, block, condition, dominators)) return false;

    auto& arena = module.arena;
    auto survives = answered < sources.size();

    SmallArray<LowerBlock*, 8> toThen;
    SmallArray<LowerBlock*, 8> toElse;
    SmallArray<LowerBlock*, 8> moved;

    for(Size i = 0; i < sources.size(); i++) {
        if(decided[i] == kUndecided) continue;

        (decided[i] == 0 ? toThen : toElse).push(sources[i]);
        moved.push(sources[i]);
    }

    // The repair before the edges move, because both of them are written against the phis as they
    // stand: which reads an arm answers for is a dominance question about the graph the walk above
    // asked it of, and what a threaded edge offers is the alternative that edge still carries.
    if(toThen.isNotEmpty()) {
        repairSuccessor(base, arena, block, then, toThen, survives, dominators);
    }

    if(toElse.isNotEmpty()) {
        repairSuccessor(base, arena, block, otherwise, toElse, survives, dominators);
    }

    for(Size i = 0; i < sources.size(); i++) {
        if(decided[i] == kUndecided) continue;

        redirectEdge(base, arena, sources[i], block, decided[i] == 0 ? then : otherwise);
    }

    if(survives) {
        narrowBlockPhis(base, arena, block, moved);
    } else {
        // Nothing may still be naming a phi of a block that is about to stop existing. The two
        // preconditions together are what promises it - every read is under an arm and both arms
        // took an edge - and this is that promise checked rather than assumed, since the failure is
        // otherwise an operand pointing into a block no list holds.
        for(auto phiPtr: block->phis.contents(base)) {
            auto result = ((LowerInstSingle*)base[phiPtr])->created().ptr;

            for(auto userPtr: result->uses.contents(base)) {
                assertTrue(base[base[userPtr]->block] == block);
            }
        }

        dropBlock(base, fun, block);
    }

    return true;
}

} // namespace

void threadDecidedBranches(LowerBase base, LowerModule& module, LowerFunction& fun) {
    /*
     * A block at a time, with the dominator tree rebuilt between them.
     *
     * Threading changes which blocks dominate which, and the repair is written against that answer -
     * so a second block judged from a tree the first invalidated would be one whose reads were
     * classified against a graph the function no longer has. The tree is a postorder walk and two
     * passes over the blocks, and the count below is bounded by the number of joins in the function,
     * so this is a walk per thread rather than a walk per block.
     *
     * The bound is a cap rather than a termination proof, on the same terms as opt.cpp's rounds:
     * every application either removes a block or moves an edge onto a block strictly closer to a
     * successor, so it ends on its own. What the cap turns into a slow compile is a future change
     * that oscillates.
     */
    auto limit = fun.blocks.size() * 4 + 16;

    for(Size round = 0; round < limit; round++) {
        auto dominators = fun.buildDominatorTree(base);
        auto moved = false;

        // A copy of the block list, because threading removes from it.
        SmallArray<LowerBlock*, 32> blocks;
        for(auto blockPtr: fun.blocks.contents(base)) blocks.push(base[blockPtr]);

        for(auto block: blocks) {
            if(!threadBlock(base, module, fun, block, dominators)) continue;

            moved = true;
            break;
        }

        /*
         * The two cleanups, and they are what makes the *next* round see anything.
         *
         * A level of the chain comes apart into two arms that carry the same literal out, so the
         * collapse is what turns the phi merging them back into that literal and the fold is what
         * turns the level above's test on it into a jump. Run once each round rather than once at
         * the end, because the round after this is the one that reads them.
         */
        auto cleaned = collapseTrivialPhis(base, module, fun);
        cleaned = foldConstantBranches(base, module, fun) || cleaned;

        if(!moved && !cleaned) return;

        // Immediately, rather than once at the end: the next round asks for a dominator tree, and
        // that walk assumes every predecessor of a reachable block has a position in it.
        removeUnreachableBlocks(base, module.arena, fun);
    }
}
