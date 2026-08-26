#pragma once

#include "builder.h"

/*
 * Copying one instruction into another body - the half of it that is the same wherever it happens.
 *
 * Two things in this compiler copy a resolve instruction, and neither is the other:
 *
 *  - `cloneInstruction` in generic.cpp builds a *specialization*, substituting the type arguments as
 *    it goes, so an instruction over an `a` becomes one over an `Int` and several of them stop
 *    existing entirely - a move of unit relocates nothing, an aggregate of no elements writes
 *    nothing;
 *  - `cloneInstruction` in opt/opt_inline.cpp *grafts* a callee into its caller, so the types are
 *    already what they will be and what changes instead is the storage: a place rooted in a
 *    parameter is re-rooted at whatever the site passed.
 *
 * Those are genuinely different jobs, and the arms where they differ are still written twice, in the
 * two files, next to the reasoning for each. What was written twice and did *not* differ is
 * everything else: forty-odd kinds whose copy is "the same instruction with each operand replaced",
 * and there is one statement of that here.
 *
 * ## Why it is worth a shared file
 *
 * The two lists had drifted, which is what a list stated twice does. `Native::relocates` was carried
 * across a specialization and dropped by a graft - so a `moveInit$` inlined on the managed target
 * silently became a per-property deep copy of a value the source was about to stop owning - and
 * `select` and `tableslot` had no arm in the specializer at all, where the default is a diagnostic
 * telling the user the compiler cannot do something it very nearly can.
 *
 * The failure mode is the one this compiler's instruction table exists to remove: an instruction
 * added to the IR reaches one copier and not the other, and nothing says so. A kind added below now
 * reaches both, and a kind added to *neither* is a `nullptr` that both callers already handle - the
 * specializer with a diagnostic, the inliner with an assertion.
 *
 * ## What a policy supplies
 *
 * Everything the two jobs disagree about, and nothing else:
 *
 *  - `module`, `function`, `block` are where the copy is built. The three arguments `createInst`
 *    takes, which is what makes this a shared body rather than a shared template of one.
 *  - `value(operand)` maps an operand to its counterpart in the new body. It may answer null - a
 *    substitution can take a value away - and every arm here passes that answer straight through,
 *    because an instruction over a value that stopped existing is one its own caller declines.
 *  - `type(type)` substitutes, or is the identity. It is asked about `InstTypeMetric::of` as well as
 *    about the result type, which is what makes `sizeOf(a)` in a generic body a constant in the
 *    specialization and leaves it alone in a graft.
 *  - `callee(function)` maps a `Symbol`'s callee: a continuation lifted out of the body being
 *    specialized needs a clone of its own, and a graft's callee is the same function it was.
 *  - `placed(inst)` is called with the finished instruction. The specializer appends it to the
 *    current block; the inliner collects it and inserts the whole run at once.
 *
 * The kinds that are *not* here are the ones with a reason, and each reason lives with the caller
 * that has it: the allocations, the writes, the ownership four, the three calls, the terminators and
 * the phi. `VZeroUpper` is not here either and that is the one absence to state - it is
 * `kInstAnchored`, so the inliner may not copy one anywhere, and giving the specializer an arm for a
 * kind no generic body has ever held would be an untested path bought for symmetry.
 */
// The concrete type an arm builds, as an argument rather than as a template argument - which is what
// lets the three fixed parameters `createInst` takes be written once below instead of per arm.
template<class T> struct CloneTag { using type = T; };

template<class Policy>
Inst* cloneComputation(Policy& policy, Value& from) {
    auto source = from.source;
    auto name = from.name;
    auto type = policy.type(from.type);

    auto value = [&](ModulePtr<Value> operand) { return policy.value(operand); };

    auto make = [&](auto* built) {
        policy.placed(built);
        return (Inst*)built;
    };

    // Spelled once, because the three arguments in front of every one of these are the same three.
    auto build = [&](auto tag, auto&&... args) {
        using T = typename decltype(tag)::type;
        return createInst<T>(policy.module, policy.function, policy.block, source, name, type,
                             forward<decltype(args)>(args)...);
    };

    switch(from.kind) {
        case Value::Cast:
        case Value::Bitcast:
        case Value::Neg:
        case Value::Not:
        case Value::ByteSwap:
        case Value::CountBits:
        case Value::LeadingZeros:
        case Value::TrailingZeros:
        case Value::Sqrt:
        case Value::Abs:
        case Value::Trunc:
        case Value::Floor:
        case Value::Ceil:
        case Value::Round:
            return make(build(CloneTag<InstUnary>(), from.kind, value(((InstUnary&)from).from)));

        case Value::Add:
        case Value::Sub:
        case Value::Mul:
        case Value::MulHi:
        case Value::Div:
        case Value::Rem:
        case Value::Shl:
        case Value::Shr:
        case Value::Sar:
        case Value::Rol:
        case Value::Ror:
        case Value::And:
        case Value::Or:
        case Value::Xor:
        case Value::BitsUpTo:
        case Value::GatherBits:
        case Value::ScatterBits:
        case Value::Crc32: {
            auto& binary = (InstBinary&)from;
            return make(build(CloneTag<InstBinary>(), from.kind, value(binary.lhs), value(binary.rhs)));
        }

        // The comparison, which is a binary with its operator in a field - so not the arm above,
        // which would copy the operands and leave every `cmp` an `Eq`.
        case Value::Cmp: {
            auto& compare = (InstCmp&)from;
            return make(build(CloneTag<InstCmp>(), value(compare.lhs), value(compare.rhs), compare.cmp));
        }

        case Value::Select: {
            auto& select = (InstSelect&)from;
            return make(build(CloneTag<InstSelect>(), value(select.cond), value(select.whenTrue),
                              value(select.whenFalse)));
        }

        // Three operands, so not the binary arm: reading one as a Binary would copy two operands and
        // drop the third, which is a use nobody records and a value nothing reads.
        case Value::Fma: {
            auto& fma = (InstFma&)from;
            return make(build(CloneTag<InstFma>(), value(fma.a), value(fma.b), value(fma.c)));
        }

        case Value::Sha256Rounds: {
            auto& rounds = (InstSha256Rounds&)from;
            return make(build(CloneTag<InstSha256Rounds>(), value(rounds.state), value(rounds.feed),
                              value(rounds.keys)));
        }

        case Value::ShaBinary: {
            auto& sha = (InstShaBinary&)from;
            return make(build(CloneTag<InstShaBinary>(), value(sha.lhs), value(sha.rhs), sha.op));
        }

        /*
         * The vector kinds. `type` is the policy's answer, which is what carries the natural form's
         * deferral through a specialization: a `Vec(a)` in a generic body has no lane count, and the
         * copy of an instruction over one is an instruction over the resolved vector because the
         * substitution resolved it. Nothing here has to know that happened.
         */
        case Value::VecSplat:
            return make(build(CloneTag<InstVecSplat>(), value(((InstVecSplat&)from).from)));

        case Value::VecLane: {
            auto& lane = (InstVecLane&)from;
            return make(build(CloneTag<InstVecLane>(), value(lane.from), lane.lane));
        }

        // The two share a struct and not a constructor: the write takes the value as well, and the
        // constructor is what sets the kind apart.
        case Value::VecWithLane: {
            auto& lane = (InstVecLane&)from;
            return make(build(CloneTag<InstVecLane>(), value(lane.from), lane.lane, value(lane.value)));
        }

        case Value::VecShuffle: {
            auto& shuffle = (InstVecShuffle&)from;
            auto cloned = build(CloneTag<InstVecShuffle>(), value(shuffle.left), value(shuffle.right));

            for(auto entry: shuffle.pattern) cloned->pattern.push(entry);
            return make(cloned);
        }

        case Value::VecReduce: {
            auto& reduce = (InstVecReduce&)from;
            return make(build(CloneTag<InstVecReduce>(), value(reduce.from), reduce.reduce));
        }

        // The measured type goes through the policy like the result type does, which is what turns
        // `sizeOf(x)` in a generic body from a load out of a descriptor into a constant in the
        // specialization, and leaves it exactly as it was in a graft.
        case Value::TypeMetric: {
            auto& metric = (InstTypeMetric&)from;
            return make(build(CloneTag<InstTypeMetric>(), policy.type(metric.of), metric.metric));
        }

        // `slot` is a numbering witness.h owns rather than anything of either body's, so it travels
        // as it stands. The table it is read out of is an ordinary operand.
        case Value::TableSlot: {
            auto& read = (InstTableSlot&)from;
            return make(build(CloneTag<InstTableSlot>(), value(read.table), read.slot));
        }

        case Value::Symbol: {
            auto& symbol = (InstSymbol&)from;
            return make(build(CloneTag<InstSymbol>(), policy.callee(symbol.callee), symbol.global));
        }

        /*
         * A host node, which owns nothing: `op` and `method` are what to emit and the argument list
         * is uses, so there is no state here that means anything about the function it sits in.
         *
         * `relocates` included, and it is the field this file was worth writing for: it says the
         * source of a block copy is dead the moment the copy returns, only `moveInit$` glue carries
         * it, and only the managed target can tell the difference. A graft used to drop it, which
         * turned a move into a per-property deep copy of a value nothing was going to read again.
         */
        case Value::Native: {
            auto& native = (InstNative&)from;
            auto cloned = build(CloneTag<InstNative>(), native.op, native.method);
            cloned->relocates = native.relocates;

            for(auto argument: native.args.contents(*policy.module.arena)) {
                cloned->args.push(policy.module.arena, value(argument));
            }

            return make(cloned);
        }

        /*
         * The atomics, with the three fields that say what they mean - which operation, how strongly
         * it orders, and how strongly a failed comparison does.
         *
         * Copyable on `Native`'s terms: a fixed operation over a flat argument list, running once
         * wherever the copy runs. What an atomic orders is other threads' writes, and neither copier
         * moves it relative to anything in its own body.
         */
        case Value::Atomic: {
            auto& atomic = (InstAtomic&)from;
            auto cloned = build(CloneTag<InstAtomic>(), atomic.kind, atomic.order);

            cloned->failure = atomic.failure;
            cloned->weak = atomic.weak;

            for(auto argument: atomic.args.contents(*policy.module.arena)) {
                cloned->args.push(policy.module.arena, value(argument));
            }

            return make(cloned);
        }

        // Its one operand is the compare-exchange it selects the second result of, so the value map
        // is the whole of what keeps the two together.
        case Value::AtomicOk:
            return make(build(CloneTag<InstAtomicOk>(), value(((InstAtomicOk&)from).cas)));

        default:
            return nullptr;
    }
}
