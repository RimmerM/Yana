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

        // `length(xs)` - `Builder::addLength`, which is where the whole of it is: what a window
        // holds is a number, and the builder answers with the one the window was built from where
        // there is one.
        case Op::sequenceLength:
            return builder.addLength(source, *arguments[0],
                                     substituteType(context, builder.program, callee.result.type,
                                                    with));

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
         * Three groups, and they are held up by different things. The **pointer** family needs two
         * MIR nodes this IR has not got - an address of a place, and a type's stride - and is the
         * next one to go. The **machine** family (the vector transfers, the movemask, the widening
         * multiply, the system call, the thread clone) needs vector types and a native-operation
         * node. The **library** family (`newRun`, the string's two words, the scan limits) needs
         * the allocator and the real `Core`, which is milestone 8. The **host** family needs the
         * JavaScript pipeline `Analysis-IR-JS.md` describes, and is the one group that may never
         * arrive here as written: this stage does not compile for that target at all yet, so what
         * these arms record is that the roles exist rather than that they are nearly done.
         */
        case Op::addressOf:
        case Op::pointerBorrow:
        case Op::pointerBorrowMut:
        case Op::pointerAdd:
        case Op::pointerSub:
        case Op::pointerDifference:
        case Op::typeTraits:
        case Op::vectorAt:
        case Op::vectorPast:
        case Op::setVectorAt:
        case Op::maskBits:
        case Op::mulHigh:
        case Op::blockCopy:
        case Op::setMemory:
        case Op::syscall0:
        case Op::syscall1:
        case Op::syscall2:
        case Op::syscall3:
        case Op::syscall4:
        case Op::syscall5:
        case Op::syscall6:
        case Op::cloneThread:
        case Op::stringData:
        case Op::stringDataMut:
        case Op::stringFromData:
        case Op::newRun:
        case Op::swap:
        case Op::exchange:
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
        case Op::atomicNew:
        case Op::atomicIntoValue:
        case Op::atomicLoad:
        case Op::atomicStore:
        case Op::atomicExchange:
        case Op::atomicFetchAdd:
        case Op::atomicFetchSub:
        case Op::atomicFetchAnd:
        case Op::atomicFetchOr:
        case Op::atomicFetchXor:
        case Op::atomicCompareExchange:
        case Op::atomicCompareExchangeWeak:
        case Op::atomicComparePair:
        case Op::atomicComparePairWeak:
        case Op::atomicFence:
        case Op::atomicSpinHint:
        case Op::atomicAt:

        // The vector surface, which needs vector types in this stage's IR before any of it can be
        // written - the same thing `op.def`'s `anyAt()` positions there are waiting on.
        case Op::vecSplat:
        case Op::vecLane:
        case Op::vecWithLane:
        case Op::vecZero:
        case Op::vecIota:
        case Op::vecLanes:
        case Op::vecHorizontalSum:
        case Op::vecHorizontalProduct:
        case Op::vecHorizontalMin:
        case Op::vecHorizontalMax:
        case Op::vecMin:
        case Op::vecMax:
        case Op::vecAbs:
        case Op::vecSqrt:
        case Op::vecFma:
        case Op::vecTrunc:
        case Op::vecFloor:
        case Op::vecCeil:
        case Op::vecRound:
        case Op::vecUnpackLow:
        case Op::vecUnpackHigh:
        case Op::vecPackLanes:
        case Op::vecShuffle:
        case Op::vecShuffle2:
        case Op::vecAlignLanes:
        case Op::vecReverse:
        case Op::vecRotate:
        case Op::vecInterleaveLow:
        case Op::vecInterleaveHigh:
        case Op::maskAny:
        case Op::maskAll:
        case Op::maskNone:
        case Op::maskCount:
        case Op::maskFirstSet:
        case Op::maskUpTo:

        // The bulk operations, which are a *choice between two library bodies* rather than a node -
        // so what this needs is not an emitter but the ability to reach `Reduce.yana` at all.
        case Op::bulkSum:
        case Op::bulkProduct:
        case Op::bulkMaximum:
        case Op::bulkMinimum:
        case Op::bulkOccurrences:
        case Op::bulkIndexOf:
        case Op::bulkItemsEqual:

        // The CPU instructions, which need the vector types above and a native-operation node.
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
