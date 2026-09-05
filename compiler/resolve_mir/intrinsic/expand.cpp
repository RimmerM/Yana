#include "expand_internal.h"

namespace mir {

using namespace intrinsic;

/*
 * The roster: one arm per `op.def` row.
 *
 * A switch and not a table of function pointers, and the difference is what `-Wswitch` does with it:
 * a row added to `op.def` without an emitter is a compiler warning here, at the one place that has
 * to decide, rather than a null entry discovered by the first program that calls it.
 */
Node* expandIntrinsic(Builder& builder, Context& context, Op op, Function& callee,
                      Node* const* arguments, Size count, const Substitution& with,
                      LocationId source) {
    // The arity `op.def` states, which `checkRoles` matched against the declaration - so an emitter
    // indexes `arguments` without a bound of its own.
    assertTrue(count == opDef(op).count);

    switch(op) {
        case Op::pointerLoad:  return expandPointerLoad(builder, callee, arguments, with, source);
        case Op::pointerStore: return expandPointerStore(builder, callee, arguments, with, source);
        case Op::addressOf:    return expandAddressOf(builder, callee, arguments, with, source);
        case Op::pointerAdd:   return expandPointerAdd(builder, callee, arguments, with, source);
        case Op::pointerSub:   return expandPointerSub(builder, callee, arguments, with, source);

        // One emitter: the two differ in the access their declared result names, which it reads.
        case Op::pointerBorrow:
        case Op::pointerBorrowMut:
            return expandPointerBorrow(builder, callee, arguments, with, source);

        case Op::pointerDifference:
            return expandPointerDifference(builder, callee, arguments, with, source);
        case Op::mulHigh:
            return expandMulHigh(builder, callee, arguments, with, source);
        case Op::typeTraits:
            return expandTypeTraits(builder, callee, arguments, with, source);

        // The two relocations through a place - `expand_exchange.cpp`, and `analyze_loan.cpp` for
        // the one exemption they are granted.
        case Op::swap:     return expandSwap(builder, callee, arguments, with, source);
        case Op::exchange: return expandExchange(builder, callee, arguments, with, source);

        // The machine's own operations. `count` is the declaration's, which for a system call is the
        // whole difference between the seven roles - see `expandSyscall`.
        case Op::blockCopy: return expandBlockCopy(builder, callee, arguments, with, source);
        case Op::setMemory: return expandSetMemory(builder, callee, arguments, with, source);

        case Op::syscall0:
        case Op::syscall1:
        case Op::syscall2:
        case Op::syscall3:
        case Op::syscall4:
        case Op::syscall5:
        case Op::syscall6:
            return expandSyscall(builder, callee, arguments, count, with, source);

        /*
         * The atomics - `lib/Atomic/Atomic.native.yana`, and `expand_atomic.cpp` for the family.
         *
         * The six read-modify-writes are one emitter with the operation as a parameter, because
         * they are one shape and one instruction: `op.def` has a row each because a *declaration*
         * each is what the library writes, and what tells them apart below the signature is which
         * arithmetic the instruction folds in.
         */
        case Op::atomicNew:
            return expandAtomicNew(builder, callee, arguments, with, source);
        case Op::atomicIntoValue:
            return expandAtomicIntoValue(builder, callee, arguments, with, source);
        case Op::atomicLoad:
            return expandAtomicLoad(builder, callee, arguments, with, source);
        case Op::atomicStore:
            return expandAtomicStore(builder, callee, arguments, with, source);

        case Op::atomicExchange:
            return expandAtomicUpdate(builder, AtomicOp::Exchange, arguments, source);
        case Op::atomicFetchAdd:
            return expandAtomicUpdate(builder, AtomicOp::Add, arguments, source);
        case Op::atomicFetchSub:
            return expandAtomicUpdate(builder, AtomicOp::Sub, arguments, source);
        case Op::atomicFetchAnd:
            return expandAtomicUpdate(builder, AtomicOp::And, arguments, source);
        case Op::atomicFetchOr:
            return expandAtomicUpdate(builder, AtomicOp::Or, arguments, source);
        case Op::atomicFetchXor:
            return expandAtomicUpdate(builder, AtomicOp::Xor, arguments, source);

        case Op::atomicFence:
            return expandAtomicFence(builder, callee, arguments, with, source);
        case Op::atomicSpinHint:
            return expandAtomicSpinHint(builder, callee, arguments, with, source);

        /*
         * The portable vector set - `expand_vector.cpp`, and Design-Vector §3.3 for what each is.
         *
         * Several of the rows share an emitter with a column, because a row here is a *declaration*
         * and what tells two of them apart below the signature is which fold, which half or how many
         * sources - see the declarations, where that division is stated.
         */
        case Op::vecSplat:   return expandVecSplat(builder, callee, arguments, with, source);
        case Op::vecZero:    return expandVecZero(builder, callee, arguments, with, source);
        case Op::vecIota:    return expandVecIota(builder, callee, arguments, with, source);
        case Op::vecLane:    return expandVecLane(builder, callee, arguments, with, source);
        case Op::vecWithLane: return expandVecWithLane(builder, callee, arguments, with, source);
        case Op::vecLanes:   return expandVecLanes(builder, callee, arguments, with, source);

        case Op::vecHorizontalSum:
            return expandVecReduce(builder, callee, ReduceOp::Add, arguments, with, source);
        case Op::vecHorizontalProduct:
            return expandVecReduce(builder, callee, ReduceOp::Mul, arguments, with, source);
        case Op::vecHorizontalMin:
            return expandVecReduce(builder, callee, ReduceOp::Min, arguments, with, source);
        case Op::vecHorizontalMax:
            return expandVecReduce(builder, callee, ReduceOp::Max, arguments, with, source);

        case Op::vecMin:
            return expandVecMinMax(builder, callee, BinaryOp::Lt, arguments, with, source);
        case Op::vecMax:
            return expandVecMinMax(builder, callee, BinaryOp::Gt, arguments, with, source);
        case Op::vecAbs: return expandVecAbs(builder, callee, arguments, with, source);

        case Op::vecSqrt:   return expandVecFloatUnary(builder, UnaryOp::Sqrt, arguments, source);
        case Op::vecTrunc:  return expandVecFloatUnary(builder, UnaryOp::Trunc, arguments, source);
        case Op::vecFloor:  return expandVecFloatUnary(builder, UnaryOp::Floor, arguments, source);
        case Op::vecCeil:   return expandVecFloatUnary(builder, UnaryOp::Ceil, arguments, source);
        case Op::vecRound:  return expandVecFloatUnary(builder, UnaryOp::Round, arguments, source);
        case Op::vecFma: return expandVecFma(builder, callee, arguments, with, source);

        case Op::vecUnpackLow:
            return expandVecUnpack(builder, callee, false, arguments, with, source);
        case Op::vecUnpackHigh:
            return expandVecUnpack(builder, callee, true, arguments, with, source);
        case Op::vecPackLanes:
            return expandVecPackLanes(builder, callee, arguments, with, source);

        case Op::vecShuffle:
            return expandVecShuffle(builder, callee, 1, arguments, with, source);
        case Op::vecShuffle2:
            return expandVecShuffle(builder, callee, 2, arguments, with, source);
        case Op::vecAlignLanes:
            return expandVecAlignLanes(builder, callee, arguments, with, source);
        case Op::vecReverse: return expandVecReverse(builder, callee, arguments, with, source);
        case Op::vecRotate:  return expandVecRotate(builder, callee, arguments, with, source);
        case Op::vecInterleaveLow:
            return expandVecInterleave(builder, callee, false, arguments, with, source);
        case Op::vecInterleaveHigh:
            return expandVecInterleave(builder, callee, true, arguments, with, source);

        // The mask readings, which are `expandVecReduce` again: what tells `count` from `any` is the
        // result type the declaration named, so a second emitter would be one that decides nothing.
        case Op::maskAny:
            return expandVecReduce(builder, callee, ReduceOp::Or, arguments, with, source);
        case Op::maskAll:
            return expandVecReduce(builder, callee, ReduceOp::And, arguments, with, source);
        case Op::maskCount:
            return expandVecReduce(builder, callee, ReduceOp::Add, arguments, with, source);
        case Op::maskFirstSet:
            return expandVecReduce(builder, callee, ReduceOp::FirstSet, arguments, with, source);
        case Op::maskNone: return expandMaskNone(builder, callee, arguments, with, source);
        case Op::maskUpTo: return expandMaskUpTo(builder, callee, arguments, with, source);

        /*
         * The `Flow` roles - `!`, `&&` and `||` - are the resolver's, because what they expand to
         * is a branch with an operand resolved *under* it, and the block structure and the values
         * that cross it are the resolver's to lay out. `Resolver::climb` and `resolvePrefix` read
         * the role and never reach here; a written call `&&(a, b)` is an ordinary call of the body
         * Core writes, which `finishCall` makes by reading the row's form. So an arm here would be
         * a third expansion of one operator, and the assertion is what says there are two.
         */
        case Op::truthNot:
        case Op::truthAnd:
        case Op::truthOr:

        /*
         * And the four `Branch` roles, for the same reason one tier further out: what a
         * compare-exchange expands to is a branch joining the instruction's two answers, and a
         * value the walk is holding has to travel its edges. `Resolver::expandBranchingIntrinsic`
         * is where they are; `finishCall` reads the row's form and never reaches here with one.
         */
        case Op::atomicCompareExchange:
        case Op::atomicCompareExchangeWeak:
        case Op::atomicComparePair:
        case Op::atomicComparePairWeak:
            assertTrue(false);
            break;

        /*
         * The roles this stage has not reached, reported at the call.
         *
         * **Not a fallback**, and the difference is what keeps the roster a roster: each of these is
         * written out, so a row added to `op.def` with no arm anywhere is still `-Wswitch` at this
         * function rather than a program silently taking the "unimplemented" path. What the list
         * says is a fact about *this stage* and not about the table - the shipping resolver
         * implements every one of them - so it is an ordinary "not implemented yet", spelled the
         * way every other construct this stage has not reached is spelled, and it shrinks from the
         * top as the emitters arrive.
         *
         * Four groups, and they are held up by different things.
         *
         * The **machine** family - the vector transfers, the movemask, the thread clone - is the
         * one whose blocker went: vector types are here, and what is left of it is a declaration
         * each in `prelude/Native` and, for the clone, a backend intrinsic rather than either of
         * `Node::Native`'s two shapes. The widening multiply was the first of them out, and what
         * it took was that declaration and one `op.def` row - see `expandMulHigh`.
         *
         * The **library** family - `newRun`, the string's two words, the scan limits, and the bulk
         * operations, which are a choice between two library bodies rather than a node - needs the
         * allocator, the container classes and the real `Core`.
         *
         * The **host** family needs the JavaScript pipeline `Analysis-IR-JS.md` describes, and is
         * the one group that may never arrive here as written: this stage does not compile for that
         * target at all yet, so what these arms record is that the roles exist rather than that they
         * are nearly done.
         *
         * The **CPU** instructions are their own case: each is `@platform`-gated at a declaration
         * `prelude/` does not carry, so a role nothing claims is a role nothing can call, and the
         * arm is what would answer if one did.
         */
        case Op::vectorAt:
        case Op::vectorPast:
        case Op::setVectorAt:
        case Op::maskBits:
        case Op::cloneThread:
        case Op::newRun:
        case Op::scanLimitFor:
        case Op::scanLimitForKeys:
        case Op::hostField:
        case Op::hostCall0:
        case Op::hostCall1:
        case Op::hostCall2:
        case Op::hostCall3:
        case Op::hostGlobal0:
        case Op::hostGlobal1:
        case Op::hostBinary:
        case Op::hostThrow:
        case Op::hostViewRead:
        case Op::hostViewWrite:
        case Op::hostArray:
        case Op::hostGrow:
        case Op::hostFixedCapacity:
        case Op::hostRead:
        case Op::hostWrite:
        case Op::hostAt:
        case Op::hostAtMut:
        case Op::hostVector:
        case Op::hostVectorUpTo:
        case Op::hostSetVector:
        case Op::atomicAt:

        // The bulk operations, whose blocker is reaching `Reduce.yana` at all rather than an
        // emitter - the group above.
        case Op::bulkSum:
        case Op::bulkProduct:
        case Op::bulkMaximum:
        case Op::bulkMinimum:
        case Op::bulkOccurrences:
        case Op::bulkIndexOf:
        case Op::bulkItemsEqual:

        // The CPU instructions - see the group above, which is where the reason they are here is.
        case Op::x86Crc32:
        case Op::x86VZeroUpper:
        case Op::x86Sha1Message1:
        case Op::x86Sha1Message2:
        case Op::x86Sha1NextE:
        case Op::x86Sha1Rounds:
        case Op::x86Sha256Message1:
        case Op::x86Sha256Message2:
        case Op::x86Sha256Rounds:
            context.diagnostics.error("the ownership resolver does not implement %@ yet"_v, source,
                                      context.findName(callee.name));
            return nullptr;

        // Not a fallback either, and for the opposite reason: `Count` is not a role.
        case Op::Count: break;
    }

    assertTrue(false);
    return nullptr;
}

} // namespace mir
