#include "expand_internal.h"

namespace mir {

using namespace intrinsic;

/*
 * The roles whose implementation is the library's own body - see `opExpands`, where the reason is.
 *
 * A list and not a `case` in the roster below, because these are calls that never reach it: what
 * they are is a fact about the *dispatch*, decided before an emitter is chosen, and an arm in the
 * roster would be one that could only assert it is unreachable.
 */
bool opExpands(Op op) {
    switch(op) {
        /*
         * `newRun(n)` - `lib/Native/Run.native.yana`'s body.
         *
         * `compiler/resolve` expanded it into an allocation with a count whose *placement* its
         * escape analysis decided, writing that decision into the run's tag; this stage has no
         * placement analysis (D6), so a run it built would always be a heap one - which is what the
         * library body already says, in the language, with the allocator's refusal handled.
         * Expanding it here would be the compiler spelling out a record's three fields in C++ to
         * reach the same answer for a run whose *size is computed*, which is the one case a
         * literal's `RunSlots` does not cover - see `known.def`'s note above `runSlots`.
         */
        case Op::newRun:
            return false;

        default:
            return true;
    }
}

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
        case Op::volatileLoad: return expandVolatilePointerLoad(builder, callee, arguments, with, source);
        case Op::volatileStore: return expandVolatilePointerStore(builder, callee, arguments, with, source);
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

        /*
         * The whole-vector transfers - `expand_pointer.cpp`, where the argument for their being the
         * two roles above at a vector type is.
         *
         * Two emitters for three rows: `vectorAt` and `vectorPast` are one load and differ by the
         * tail-read flag, which is a column here for the reason the atomic updates' operation is one.
         */
        case Op::vectorAt:    return expandVectorAt(builder, callee, arguments, with, source);
        case Op::vectorPast:  return expandVectorPast(builder, callee, arguments, with, source);
        case Op::setVectorAt: return expandSetVectorAt(builder, callee, arguments, with, source);
        case Op::vecLoadUpTo: return expandVecLoadUpTo(builder, callee, arguments, with, source);
        case Op::vecStoreUpTo: return expandVecStoreUpTo(builder, callee, arguments, with, source);

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

        case Op::cloneThread:
            return expandCloneThread(builder, callee, arguments, with, source);

        case Op::stackInit:
            return expandStackInit(builder, callee, arguments, with, source);
        case Op::stackSwitch:
            return expandStackSwitch(builder, callee, arguments, with, source);

        /*
         * The x86 family - `lib/Native/Intrinsic/X86.yana`, a `native.def` row each.
         *
         * One arm apiece although eight of them share an emitter, because the row is what the arm
         * decides: a table from `Op` to `NativeOp` would be a second place for the pairing to live
         * and the switch is what `-Wswitch` checks. `sha1rnds4` is the one with an argument to
         * hold to a rule, so it has an emitter of its own - see it.
         */
        case Op::x86Crc32:
            return expandX86Native(builder, callee, arguments, 2, with, source, NativeOp::Crc32);
        case Op::x86VZeroUpper:
            return expandX86Native(builder, callee, arguments, 0, with, source,
                                   NativeOp::VZeroUpper);
        case Op::x86VZeroAll:
            return expandX86Native(builder, callee, arguments, 0, with, source,
                                   NativeOp::VZeroAll);
        case Op::x86Nop:
            return expandX86Native(builder, callee, arguments, 0, with, source, NativeOp::X86Nop);
        case Op::x86Lretq:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86Lretq);
        case Op::x86Cpuid:
            return expandX86Native(builder, callee, arguments, 2, with, source, NativeOp::X86Cpuid);
        case Op::x86Rdtscp:
            return expandX86Native(builder, callee, arguments, 0, with, source, NativeOp::X86Rdtscp);
        case Op::x86Rdtsc:
            return expandX86Native(builder, callee, arguments, 0, with, source, NativeOp::X86Rdtsc);
        case Op::x86MFence:
            return expandX86Native(builder, callee, arguments, 0, with, source, NativeOp::X86MFence);
        case Op::x86LFence:
            return expandX86Native(builder, callee, arguments, 0, with, source, NativeOp::X86LFence);
        case Op::x86SFence:
            return expandX86Native(builder, callee, arguments, 0, with, source, NativeOp::X86SFence);
        case Op::x86Pause:
            return expandX86Native(builder, callee, arguments, 0, with, source, NativeOp::X86Pause);
        case Op::x86Prefetch:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86Prefetch);
        case Op::x86PrefetchNta:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86PrefetchNta);
        case Op::x86Clflush:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86Clflush);
        case Op::x86Invlpg:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86Invlpg);
        case Op::x86Hlt:
            return expandX86Native(builder, callee, arguments, 0, with, source, NativeOp::X86Hlt);
        case Op::x86Cli:
            return expandX86Native(builder, callee, arguments, 0, with, source, NativeOp::X86Cli);
        case Op::x86Sti:
            return expandX86Native(builder, callee, arguments, 0, with, source, NativeOp::X86Sti);
        case Op::x86Swapgs:
            return expandX86Native(builder, callee, arguments, 0, with, source, NativeOp::X86Swapgs);
        case Op::x86Rdmsr:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86Rdmsr);
        case Op::x86Wrmsr:
            return expandX86Native(builder, callee, arguments, 3, with, source, NativeOp::X86Wrmsr);
        case Op::x86Xgetbv:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86Xgetbv);
        case Op::x86In8:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86In8);
        case Op::x86In16:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86In16);
        case Op::x86In32:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86In32);
        case Op::x86Out8:
            return expandX86Native(builder, callee, arguments, 2, with, source, NativeOp::X86Out8);
        case Op::x86Out16:
            return expandX86Native(builder, callee, arguments, 2, with, source, NativeOp::X86Out16);
        case Op::x86Out32:
            return expandX86Native(builder, callee, arguments, 2, with, source, NativeOp::X86Out32);
        case Op::x86Ins32:
            return expandX86Native(builder, callee, arguments, 3, with, source, NativeOp::X86Ins32);
        case Op::x86Outs32:
            return expandX86Native(builder, callee, arguments, 3, with, source, NativeOp::X86Outs32);
        case Op::x86Interrupt:
            return expandX86Interrupt(builder, callee, arguments, with, source);
        case Op::x86Lidt:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86Lidt);
        case Op::x86Lgdt:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86Lgdt);
        case Op::x86MovDs:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86MovDs);
        case Op::x86MovEs:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86MovEs);
        case Op::x86MovFs:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86MovFs);
        case Op::x86MovGs:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86MovGs);
        case Op::x86MovSs:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86MovSs);
        case Op::x86ReadCr0:
            return expandX86Native(builder, callee, arguments, 0, with, source, NativeOp::X86ReadCr0);
        case Op::x86ReadCr2:
            return expandX86Native(builder, callee, arguments, 0, with, source, NativeOp::X86ReadCr2);
        case Op::x86ReadCr3:
            return expandX86Native(builder, callee, arguments, 0, with, source, NativeOp::X86ReadCr3);
        case Op::x86ReadCr4:
            return expandX86Native(builder, callee, arguments, 0, with, source, NativeOp::X86ReadCr4);
        case Op::x86WriteCr0:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86WriteCr0);
        case Op::x86WriteCr3:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86WriteCr3);
        case Op::x86WriteCr4:
            return expandX86Native(builder, callee, arguments, 1, with, source, NativeOp::X86WriteCr4);
        case Op::x86Sha1Message1:
            return expandX86Native(builder, callee, arguments, 2, with, source,
                                   NativeOp::Sha1Message1);
        case Op::x86Sha1Message2:
            return expandX86Native(builder, callee, arguments, 2, with, source,
                                   NativeOp::Sha1Message2);
        case Op::x86Sha1NextE:
            return expandX86Native(builder, callee, arguments, 2, with, source,
                                   NativeOp::Sha1NextE);
        case Op::x86Sha1Rounds:
            return expandX86Sha1Rounds(builder, callee, arguments, with, source);
        case Op::x86Sha256Message1:
            return expandX86Native(builder, callee, arguments, 2, with, source,
                                   NativeOp::Sha256Message1);
        case Op::x86Sha256Message2:
            return expandX86Native(builder, callee, arguments, 2, with, source,
                                   NativeOp::Sha256Message2);
        case Op::x86Sha256Rounds:
            return expandX86Native(builder, callee, arguments, 3, with, source,
                                   NativeOp::Sha256Rounds);

        // AES-NI and PCLMULQDQ, the two-block forms on the same rows: the width is the operand
        // type's, which the declaration fixed. The two with a trailing byte have emitters of their
        // own, for `sha1rnds4`'s reason.
        case Op::x86AesEncrypt:
        case Op::x86AesEncrypt2:
            return expandX86Native(builder, callee, arguments, 2, with, source, NativeOp::AesEncrypt);
        case Op::x86AesEncryptLast:
        case Op::x86AesEncryptLast2:
            return expandX86Native(builder, callee, arguments, 2, with, source,
                                   NativeOp::AesEncryptLast);
        case Op::x86AesDecrypt:
        case Op::x86AesDecrypt2:
            return expandX86Native(builder, callee, arguments, 2, with, source, NativeOp::AesDecrypt);
        case Op::x86AesDecryptLast:
        case Op::x86AesDecryptLast2:
            return expandX86Native(builder, callee, arguments, 2, with, source,
                                   NativeOp::AesDecryptLast);
        case Op::x86AesInverseMix:
            return expandX86Native(builder, callee, arguments, 1, with, source,
                                   NativeOp::AesInverseMix);
        case Op::x86AesKeygenAssist:
            return expandX86AesKeygenAssist(builder, callee, arguments, with, source);
        case Op::x86CarrylessMultiply:
        case Op::x86CarrylessMultiply2:
            return expandX86CarrylessMultiply(builder, callee, arguments, with, source);

        // The AVX-512 float operations - `lib/Native/Intrinsic/X86.yana`'s last block.
        case Op::x86GetExponent:
            return expandX86FloatOp(builder, callee, arguments, 1, with, source, NativeOp::GetExponent);
        case Op::x86GetMantissa:
            return expandX86FloatOp(builder, callee, arguments, 1, with, source, NativeOp::GetMantissa);
        case Op::x86ReduceFloat:
            return expandX86FloatOp(builder, callee, arguments, 1, with, source, NativeOp::ReduceFloat);
        case Op::x86ScaleFloat:
            return expandX86FloatOp(builder, callee, arguments, 2, with, source, NativeOp::ScaleFloat);
        case Op::x86RangeFloat:
            return expandX86FloatOp(builder, callee, arguments, 2, with, source, NativeOp::RangeFloat);

        // IFMA's two - `expandMulAccumulate` serves them and the portable dot products both, the
        // shape it checks being the same one.
        case Op::x86MulAdd52Low:
            return expandMulAccumulate(builder, callee, NativeOp::MulAdd52Low, arguments, with, source);
        case Op::x86MulAdd52High:
            return expandMulAccumulate(builder, callee, NativeOp::MulAdd52High, arguments, with, source);

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
        case Op::atomicVolatileLoad:
            return expandAtomicVolatileLoad(builder, callee, arguments, with, source);
        case Op::atomicVolatileStore:
            return expandAtomicVolatileStore(builder, callee, arguments, with, source);

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
         * `atomicAt(address)` - Analysis-Atomics.md §3.7's unchecked entrance, and it is
         * `expandPointerBorrow` with **no difference at all**.
         *
         * That is the design of it rather than an economy. What the two emitters would have to say
         * is the same sentence - the place is the memory the pointer names, read at the occurrence
         * the declared result states, shared or exclusive as that result says - and `atomicAt`'s
         * declaration states `-> source'Atomic(a)` where `borrow`'s states `-> source'a`. So the
         * unchecked entrance *reuses* the semantics rather than defining a second set, and there is
         * no path on which an externally provided location behaves differently from an owned one.
         *
         * No reinterpretation of the address, which `compiler/resolve` needed and this does not: a
         * place here is what its *occurrence* says it is, and the pointer is a root rather than a
         * type. Keeping the argument is also what keeps the origin the signature published - a
         * bitcast's result is `owned(to)` and would have dropped the pointee's own position, so the
         * loan checker would have refused a result the declaration promised.
         *
         * Nothing here checks §3.7's four promises and nothing could: the address is a run-time
         * value, and whether every other accessor of those bytes uses a view of this width is a
         * fact about a program this compiler is not reading. `Native` is where the language says so.
         */
        case Op::atomicAt:
            return expandPointerBorrow(builder, callee, arguments, with, source);

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

        // The two saturating lane operations, which are an ordinary binary node apiece - the lane
        // check is `expandVecSaturating`'s, for the reason every argument check in this file is made
        // here: it is the last place that can name the argument.
        case Op::vecSaturatingAdd:
            return expandVecSaturating(builder, callee, BinaryOp::SaturatingAdd, arguments, with, source);
        case Op::vecSaturatingSub:
            return expandVecSaturating(builder, callee, BinaryOp::SaturatingSub, arguments, with, source);

        case Op::vecDotBytes:
            return expandMulAccumulate(builder, callee, NativeOp::VecDotBytes, arguments, with, source);
        case Op::vecDotWords:
            return expandMulAccumulate(builder, callee, NativeOp::VecDotWords, arguments, with, source);
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

        // The lane movements whose control is a value - native.def's three portable rows.
        case Op::vecCompress:
            return expandX86Native(builder, callee, arguments, 2, with, source, NativeOp::VecCompress);
        case Op::vecExpand:
            return expandX86Native(builder, callee, arguments, 2, with, source, NativeOp::VecExpand);
        case Op::vecLookup: return expandVecLookup(builder, callee, arguments, with, source);

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

        // The movemask, which is `expandVecReduce` again and the one reduction kind that is a
        // machine's rather than the language's - `Native.bits`, and `op.def`'s reduction block for
        // why it is a row at all.
        case Op::maskBits:
            return expandVecReduce(builder, callee, ReduceOp::Bits, arguments, with, source);

        // The map's probe limit - `expand_container.cpp`, and one emitter for the two rows because
        // what differs is where the key is rather than what is computed from it.
        case Op::scanLimitFor:
            return expandScanLimit(builder, callee, true, arguments, with, source);
        case Op::scanLimitForKeys:
            return expandScanLimit(builder, callee, false, arguments, with, source);

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

        /*
         * And the roles this stage answers with the *declaration's own body* - `opExpands`, which
         * `finishCall` asks before it comes here, and where the reason each is on that list is.
         *
         * An arm rather than nothing, for the roster's reason: a row with no `case` anywhere is
         * `-Wswitch`, and the assertion is what says the dispatch above already took it.
         */
        case Op::newRun:
            assertTrue(false);
            break;

        /*
         * The roles this stage has not reached, reported at the call.
         *
         * **Not a fallback**, and the difference is what keeps the roster a roster: each of these
         * is written out, so a row added to `op.def` with no arm anywhere is still `-Wswitch` at
         * this function rather than a program silently taking the "unimplemented" path. What the
         * list says is a fact about *this stage* and not about the table - `compiler/resolve`
         * implemented every one of them before J173 deleted it - so it is an ordinary "not
         * implemented yet", spelled the way every other construct this stage has not reached is
         * spelled, and it shrinks from the top as the emitters arrive.
         *
         * One group now, and it is held up by a pipeline rather than by a gap.
         *
         * The **machine** family is gone from this list. The three whole-vector transfers and the
         * movemask are emitters above - what their blocker had been was vector types, and what it
         * took once those arrived was a declaration each in `lib/Native/Pointer.native.yana` and one
         * reduction row - and `cloneThread` joined them with a `native.def` row of its own, whose
         * entry pair is split where the lowering already splits one.
         *
         * The **library** family is gone from this list entirely. `newRun` and the seven bulk
         * operations went to `opExpands` - the library's own body is what this stage calls, and what
         * is lost is a placement decision and a vectorization rather than an operation - and the two
         * scan limits and the unchecked atomic entrance are emitters above.
         *
         * The **CPU** instructions are gone from this list too - a `native.def` row apiece, and the
         * `sha1rnds4` round group as four rows rather than an operand, since it is the
         * instruction's trailing byte. Each stays `@platform`-gated at its declaration, so a build
         * below the level that has one never resolves the name.
         *
         * And the **host** family is gone from it since 2026-09-21, which is what emptied the list:
         * `expand_host.cpp` is the twenty-one emitters and `host.def` the rows. There is no
         * "not implemented yet" arm left anywhere in this roster, which is worth noticing rather
         * than deleting the paragraph over - every `op.def` row now has an emitter.
         */

        /*
         * The host - `expand_host.cpp`, `host.def`, Analysis-IR-JS.md §3.10.
         *
         * One arm apiece although four of them share an emitter, for the x86 family's reason: the
         * row is what the arm decides. What tells `hostCall0` from `hostCall3` below the signature
         * is how many arguments the declaration wrote, which is the node's operand count - so the
         * four expand to one `HostOp::Call` row rather than to four that say the arity twice.
         */
        case Op::hostField:  return expandHostField(builder, callee, arguments, with, source);
        case Op::hostCall0: return expandHostCall(builder, callee, arguments, 2, with, source);
        case Op::hostCall1: return expandHostCall(builder, callee, arguments, 3, with, source);
        case Op::hostCall2: return expandHostCall(builder, callee, arguments, 4, with, source);
        case Op::hostCall3: return expandHostCall(builder, callee, arguments, 5, with, source);
        case Op::hostGlobal0: return expandHostGlobal(builder, callee, arguments, 1, with, source);
        case Op::hostGlobal1: return expandHostGlobal(builder, callee, arguments, 2, with, source);
        case Op::hostBinary: return expandHostBinary(builder, callee, arguments, with, source);
        case Op::hostThrow:  return expandHostThrow(builder, callee, arguments, with, source);
        case Op::hostViewRead:
            return expandHostViewRead(builder, callee, arguments, with, source);
        case Op::hostViewWrite:
            return expandHostViewWrite(builder, callee, arguments, with, source);

        // A fixed array's row is its own storage, so this is `addressOf` with the declared result
        // deciding the pointee - see `hostRow` in `op.def`, which is where the two differ.
        case Op::hostRow:    return expandAddressOf(builder, callee, arguments, with, source);

        case Op::hostArray:  return expandHostArray(builder, callee, arguments, with, source);
        case Op::hostGrow:   return expandHostGrow(builder, callee, arguments, with, source);
        case Op::hostFixedCapacity:
            return expandHostFixedCapacity(builder, callee, arguments, with, source);

        // The element - a place, which is why `hostAt` and `hostAtMut` are one emitter: the declared
        // result is what says whether the loan is shared or exclusive.
        case Op::hostRead:  return expandHostRead(builder, callee, arguments, with, source);
        case Op::hostWrite: return expandHostWrite(builder, callee, arguments, with, source);
        case Op::hostAt:
        case Op::hostAtMut:
            return expandHostAt(builder, callee, arguments, with, source);

        case Op::hostVector:
            return expandHostVector(builder, callee, arguments, 2, with, source, HostOp::Vector);
        case Op::hostVectorUpTo:
            return expandHostVector(builder, callee, arguments, 3, with, source, HostOp::VectorUpTo);
        case Op::hostSetVector:
            return expandHostVector(builder, callee, arguments, 3, with, source, HostOp::SetVector);

        // Not a fallback either, and for the opposite reason: `Count` is not a role.
        case Op::Count: break;
    }

    assertTrue(false);
    return nullptr;
}

} // namespace mir
