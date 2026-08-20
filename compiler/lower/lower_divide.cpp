#include "lower_divide.h"
#include "lower_builder.h"

namespace {

U32 widthOf(LowerType type) {
    return type == LowerType::Int32 ? 32 : 64;
}

U64 maskOf(U32 bits) {
    return bits >= 64 ? maxLimit<U64> : (U64(1) << bits) - 1;
}

bool isDivision(LowerInst::Kind kind) {
    return kind == LowerInst::Div || kind == LowerInst::IDiv
        || kind == LowerInst::Rem || kind == LowerInst::IRem;
}

// The same builder `Reducer` in lower_strength.cpp is, and separate for the same reason its is
// separate: what a pass emits goes into the list it is rebuilding, in front of the instruction it
// came from, and that list is the pass's own.
struct Guard {
    LowerBase base;
    LowerModule& module;
    LowerPtr<LowerBlock> block;
    LocationId source;
    SmallArray<LowerPtr<LowerInst>, 32>& into;

    LowerValue* place(LowerInst* inst) {
        inst->block = block;
        inst->source = source;

        for(auto use: inst->used()) base[use]->uses.push(module.arena, inst - base);

        into.push(inst - base);
        return &((LowerInstSingle*)inst)->result;
    }

    LowerValue* imm(LowerType type, U64 value) {
        return place(new (module.arena) LowerImm(StringId {}, type, value));
    }

    // Written as an equality against a literal, which is the only comparison this pass makes. The
    // literal is placed first because it is an operand of the comparison: `place` pushes in the
    // order the block will run.
    LowerValue* equalsImm(LowerValue* value, U64 literal) {
        auto against = imm(value->type, literal);
        return place(new (module.arena) LowerInstCmp(StringId {}, value - base, against - base,
                                                     LowerCmp::eq));
    }

    LowerValue* op(LowerInst::Kind kind, LowerValue* lhs, LowerValue* rhs,
                   StringId name = StringId {}) {
        return place(new (module.arena) LowerInstBinary(name, lhs->type, lhs - base, rhs - base, kind));
    }

    LowerValue* select(LowerValue* condition, LowerValue* whenTrue, LowerValue* whenFalse,
                       StringId name = StringId {}) {
        return place(new (module.arena) LowerInstSelect(name, whenTrue - base, whenFalse - base,
                                                        condition - base, whenTrue->type));
    }

    LowerValue* neg(LowerValue* from) {
        return place(new (module.arena) LowerInstUnary(LowerInst::Neg, StringId {}, from->type,
                                                       from - base));
    }
};

/*
 * Whether something above this block has already decided that the divisor is not zero.
 *
 * The two shapes worth catching put the proof in the *immediate* predecessor, so this walks the
 * single-entry chain upwards rather than consulting a dominator tree - there is none built at this
 * point in the pipeline, and building one here is the duplicated work LoopAnalysis exists to stop.
 * Four steps, which covers a guard written directly around a division and the branch an inlined
 * `checkCondition` leaves; anything deeper is a range analysis and is not this.
 *
 *     if d != 0 then x / d else 0     -- `cmp_neq %d, 0`, and the division is the `then` arm
 *     ... after an earlier check ...  -- `cmp_eq %d, 0`, and the division is the `else` arm
 *
 * The fact belongs to the *edge*, so every block on the way up has to have that edge as its only
 * way in - the same rule `provenNonZero` in opt/opt_range.cpp states for the resolve-IR half of
 * this, and for the same reason: a second predecessor is a path the comparison was never made on.
 */
bool divisorKnownNonZero(LowerBase base, LowerBlock* block, LowerValue* divisor) {
    auto current = block;

    for(Size step = 0; step < 4; step++) {
        if(!current || current->incoming.size() != 1) return false;

        auto previous = base[current->incoming.get(base, 0)];
        if(!previous || !previous->terminator) return false;

        auto& terminator = *base[previous->terminator];
        if(terminator.kind != LowerInst::Je) {
            current = previous;
            continue;
        }

        auto& branch = (LowerInstJe&)terminator;
        if(!branch.cond) return false;

        auto condition = base[branch.cond]->inst();
        if(condition->kind == LowerInst::Cmp) {
            auto& compare = *(LowerInstCmp*)condition;
            auto cmp = compare.getCmp();

            if(cmp == LowerCmp::eq || cmp == LowerCmp::neq) {
                auto left = base[compare.lhs];
                auto right = base[compare.rhs];

                LowerValue* tested = nullptr;
                if(right->inst()->kind == LowerInst::Imm && ((LowerImm*)right->inst())->i == 0) {
                    tested = left;
                } else if(left->inst()->kind == LowerInst::Imm && ((LowerImm*)left->inst())->i == 0) {
                    tested = right;
                }

                // The arm on which the test failed to find a zero, and it has to be the one this
                // walk actually came down.
                if(tested == divisor) {
                    auto proving = cmp == LowerCmp::eq ? branch.otherwise : branch.then;
                    if(proving && base[proving] == current) return true;
                }
            }
        }

        current = previous;
    }

    return false;
}

/*
 * What one division becomes, or null where it is already total.
 *
 * The replacement is a *new* division rather than an edit of this one, so that the selects reading
 * it are not also readers of the value being replaced - `replaceUses` moves every reader at once and
 * has no way to spare the one instruction that must keep pointing at the old value. Building the
 * chain fresh and retiring the original is the shape lower_strength.cpp already uses, and it avoids
 * the question entirely.
 */
LowerValue* guardDivision(Guard& g, LowerInstBinary* inst) {
    auto type = inst->result.type;

    // Integers only: `isInt` is false for a float, a vector and a mask, which is exactly the three
    // things with no fault to prevent. A float divides to an infinity IEEE 754 defines, and no
    // backend accepts a packed integer division for a vector to arrive here as.
    if(!isInt(type)) return nullptr;

    auto x = g.base[inst->lhs];
    auto b = g.base[inst->rhs];
    if(x->type != type || b->type != type) return nullptr;

    // A literal divisor that is not zero cannot fault - including -1, which lower_strength.cpp has
    // already turned into a negation by the time this runs. A literal zero is guarded like any other
    // divisor rather than special-cased: the fold behind this pass collapses the whole chain to the
    // constant it is, and one path through this function is easier to trust than two.
    if(b->inst()->kind == LowerInst::Imm && (((LowerImm*)b->inst())->i & maskOf(widthOf(type))) != 0) {
        return nullptr;
    }

    auto isSigned = inst->kind == LowerInst::IDiv || inst->kind == LowerInst::IRem;
    auto isRemainder = inst->kind == LowerInst::Rem || inst->kind == LowerInst::IRem;

    auto mayBeZero = !divisorKnownNonZero(g.base, g.base[inst->block], b);

    // An unsigned division whose divisor is already known not to be zero faults on nothing, so
    // there is nothing to build and the instruction stands as it is. This is the whole payoff of
    // the walk above: `if d != 0 then x / d else 0` costs exactly what it reads as.
    if(!mayBeZero && !isSigned) {
        inst->setTrustsDivisorTest();
        return nullptr;
    }

    // The divisors the machine refuses. Zero where it is still possible, and the signed pair
    // besides: `idiv` raises on the type's minimum over -1, the true quotient being one past the
    // maximum.
    LowerValue* dividesByZero = mayBeZero ? g.equalsImm(b, 0) : nullptr;
    LowerValue* dividesByNegativeOne = nullptr;
    auto refused = dividesByZero;

    if(isSigned) {
        dividesByNegativeOne = g.equalsImm(b, maskOf(widthOf(type)));
        refused = mayBeZero ? g.op(LowerInst::Or, dividesByZero, dividesByNegativeOne)
                            : dividesByNegativeOne;
    }

    // One, because it is the divisor that answers both refused cases usefully rather than merely
    // safely: `x / 1` is `x` and `x % 1` is 0, and the second of those is already what a remainder
    // by -1 has to be.
    auto safe = g.select(refused, g.imm(type, 1), b);
    auto result = g.op(inst->kind, x, safe);

    // The division that came out of this is total only where the test that proved the divisor is
    // reachable, so it is marked as belonging below that test. See LowerInstBinary::kTrustsDivisorTest.
    if(!mayBeZero) ((LowerInstBinary*)result->inst())->setTrustsDivisorTest();

    if(isRemainder) {
        // `x % 0` is `x`, which the identity `x == (x / b) * b + (x % b)` forces once `x / 0` is 0.
        // Nothing is selected for the signed pair: the `x % 1` computed just above is the 0 that
        // `x % -1` wants - so a signed remainder with a divisor known non-zero needs no select at
        // all, and what is left is the safe divisor and the operation.
        if(mayBeZero) result = g.select(dividesByZero, x, result, inst->result.name);
    } else {
        // `x / -1` is `neg x`, and on the type's minimum that wraps back to the minimum - which is
        // the answer, signed overflow being defined to wrap. Innermost, so that a zero divisor
        // still wins: -1 and 0 cannot both hold, but the reader should not have to prove that to
        // read the nesting.
        if(isSigned) result = g.select(dividesByNegativeOne, g.neg(x), result);
        if(mayBeZero) result = g.select(dividesByZero, g.imm(type, 0), result);
    }

    // The name is put on whatever came out last rather than threaded through the arms, which would
    // be four spellings of the same question about which of them is the final one.
    result->name = inst->result.name;
    return result;
}

} // namespace

void makeDivisionTotal(LowerBase base, LowerModule& module, LowerFunction& fun) {
    for(auto blockPtr: fun.blocks.contents(base)) {
        auto block = base[blockPtr];

        SmallArray<LowerPtr<LowerInst>, 32> kept;
        auto rewrote = false;

        for(auto instPtr: block->instructions.contents(base)) {
            auto inst = base[instPtr];

            if(!isDivision(inst->kind)) {
                kept.push(instPtr);
                continue;
            }

            Guard guard { base, module, blockPtr, inst->source, kept };
            auto replacement = guardDivision(guard, (LowerInstBinary*)inst);

            if(!replacement) {
                kept.push(instPtr);
                continue;
            }

            detach(base, inst);
            replaceUses(base, module.arena, ((LowerInstSingle*)inst)->created().ptr - base,
                        replacement - base);
            rewrote = true;
        }

        if(!rewrote) continue;

        block->instructions.clear();
        for(auto instPtr: kept) block->instructions.push(module.arena, instPtr);
    }
}
