#include "expand_internal.h"

namespace mir {

using namespace intrinsic;

/*
 * The operations a machine has and the IR does not - `Node::Native`, and native.def's three rows.
 *
 * Every emitter here is the same two lines: take the arguments the ordinary call path prepared, and
 * name which operation they are for. That is the whole of the file, and it is what a `Native`
 * operation *is* at this tier - a fixed operation over a flat argument list whose meaning belongs to
 * the backend, checked before it arrives by the declaration that claimed the role.
 *
 * The result type is the declared one, substituted, exactly as the pointer family reads its own:
 * `op.def` says a system call answers an integer and the two block forms answer `{}`, and which
 * integer is the declaration's business rather than this file's.
 */
static Node* nativeOperation(Builder& builder, Function& callee, Node* const* arguments, Size count,
                             const Substitution& with, LocationId source, NativeOp op) {
    auto result = substituteType(builder.context, builder.program, callee.result.type, with);
    return builder.addNative(source, op, arguments, count, result);
}

Node* expandBlockCopy(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source) {
    return nativeOperation(builder, callee, arguments, 3, with, source, NativeOp::CopyMemory);
}

Node* expandSetMemory(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source) {
    return nativeOperation(builder, callee, arguments, 3, with, source, NativeOp::SetMemory);
}

/*
 * `syscall0` through `syscall6` - seven roles, one node.
 *
 * The arity is a *declaration's* and not an operation's: a call's argument count is what decides
 * which registers the kernel reads, so there is a declaration per arity and no variadic form to
 * write. What arrives here is already that many arguments, and the number is operand zero because
 * that is where the lower IR's own system-call form has it.
 */
Node* expandSyscall(Builder& builder, Function& callee, Node* const* arguments, Size count,
                    const Substitution& with, LocationId source) {
    return nativeOperation(builder, callee, arguments, count, with, source, NativeOp::Syscall);
}

/*
 * `cloneThread(flags, stackTop, entry, argument, threadId)` - the same two lines, and the reason it
 * is worth its own function is what it does *not* do.
 *
 * The shipping stage splits the entry's `{code, env}` pair here, into two loads in front of the
 * instruction, and hands the backend six operands. This stage keeps the five the declaration wrote
 * and splits the pair in the lowering instead - see native.def's row, and
 * `resolve_ssa/lower/inst.cpp`, where the same pair is already read for a call through a function
 * value. What that buys is that MIR says what the author wrote, and that the one place that knows a
 * function value is two words at two offsets stays one place.
 */
Node* expandCloneThread(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source) {
    return nativeOperation(builder, callee, arguments, 5, with, source, NativeOp::CloneThread);
}

/*
 * `stackInit(stackTop, argument, entry)` and `stackSwitch(target, save, transfer)` - the two lines
 * again, and the entry pair kept whole for `expandCloneThread`'s reason directly above.
 *
 * What is worth saying here is what these do *not* do, because both are suspension points and
 * neither says so at this tier. `maySuspend` in `inst.cpp` answers a **call** from its callee's
 * published summary, and a call to one of these is exactly that: the declaration in `lib/Native` is
 * bodyless, so `summary.bodyKnown` is false, so `suspension()` has no answer, so `get(true)` is what
 * the caller reads - the conservative direction, arrived at by the rule that already covers every
 * foreign declaration rather than by a bit written here. A `Native` node the expansion leaves behind
 * is not itself a suspension point and does not need to be: what the across-suspend rule has to see
 * is the *call*, and the call is what the loan checker walks.
 */
Node* expandStackInit(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source) {
    return nativeOperation(builder, callee, arguments, 3, with, source, NativeOp::StackInit);
}

Node* expandStackSwitch(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source) {
    return nativeOperation(builder, callee, arguments, 3, with, source, NativeOp::StackSwitch);
}

/*
 * The x86 instructions a program reaches by name - native.def's twelve rows.
 *
 * The same two lines every emitter in this file is, with the row passed in: what an
 * `@platform(sha)` declaration promises about its operands was checked by `checkRoles` against
 * op.def, and what the instruction does is the backend's. `count` is passed rather than read off
 * the row for `expandSyscall`'s reason - the arity belongs to the declaration.
 */
Node* expandX86Native(Builder& builder, Function& callee, Node* const* arguments, Size count,
                      const Substitution& with, LocationId source, NativeOp op) {
    return nativeOperation(builder, callee, arguments, count, with, source, op);
}

/*
 * `sha1rnds4(abcd, e, group)` - the one of the nine with an argument this tier has to hold to a
 * rule, and the reason the group is four rows rather than an operand.
 *
 * The group is the instruction's **trailing byte**, so a computed one is not an instruction; this
 * is the only place that can say so, because it is the only place the argument still exists as
 * something an author wrote. Below here it is a row, exactly as `LowerSha` has it.
 *
 * `constantRound` and not a general constant fold: what is accepted is a written literal, which is
 * what the declaration's comment promises and what every caller in `lib/Digest` writes.
 */
Node* expandX86Sha1Rounds(Builder& builder, Function& callee, Node* const* arguments,
                          const Substitution& with, LocationId source) {
    auto& group = *arguments[2];
    Maybe<U64> round = Nothing();

    if(group.kind == Node::Const) {
        auto constant = builder.body.constant(builder.base(), ((NodeConst&)group).constant);
        if(constant.kind == Constant::Int && constant.asInt() < 4) round = Just(constant.asInt());
    }

    if(!round) {
        builder.context.diagnostics.error("a sha1rnds4 round group must be a constant 0, 1, 2 or 3 - it is the instruction's trailing byte, and a computed one is not an instruction"_v,
                                          source);
        return nullptr;
    }

    static const NativeOp rows[4] = { NativeOp::Sha1Rounds0, NativeOp::Sha1Rounds1,
                                      NativeOp::Sha1Rounds2, NativeOp::Sha1Rounds3 };

    // Two operands, not three: the third *is* the row, and a node carrying it as well would be the
    // same fact in two places for the lowering to keep in step.
    return nativeOperation(builder, callee, arguments, 2, with, source, rows[round.unwrap()]);
}

/*
 * The literal an AES-NI or PCLMULQDQ trailing byte is written as - `constantRound`'s rule above,
 * and `immediate` on the node rather than a row for the reason native.def gives.
 */
static Maybe<U64> literalArgument(Builder& builder, Node& argument) {
    if(argument.kind != Node::Const) return Nothing();

    auto constant = builder.body.constant(builder.base(), ((NodeConst&)argument).constant);
    if(constant.kind != Constant::Int) return Nothing();
    return Just(constant.asInt());
}

// `aeskeygenassist(key, roundConstant)` - any byte, since the instruction reads all eight bits.
Node* expandX86AesKeygenAssist(Builder& builder, Function& callee, Node* const* arguments,
                               const Substitution& with, LocationId source) {
    auto constant = literalArgument(builder, *arguments[1]);

    if(!constant || constant.unwrap() > 255) {
        builder.context.diagnostics.error("an aeskeygenassist round constant must be a constant from 0 to 255 - it is the instruction's trailing byte, and a computed one is not an instruction"_v,
                                          source);
        return nullptr;
    }

    auto result = substituteType(builder.context, builder.program, callee.result.type, with);
    return builder.addNative(source, NativeOp::AesKeygenAssist, arguments, 1, result,
                             U8(constant.unwrap()));
}

// `pclmulqdq(a, b, selector)` - one of the four values the instruction reads: bit 0 picks the first
// operand's quadword, bit 4 the second's.
Node* expandX86CarrylessMultiply(Builder& builder, Function& callee, Node* const* arguments,
                                 const Substitution& with, LocationId source) {
    auto selector = literalArgument(builder, *arguments[2]);
    auto value = selector ? selector.unwrap() : ~U64(0);

    if(value != 0x00 && value != 0x01 && value != 0x10 && value != 0x11) {
        builder.context.diagnostics.error("a pclmulqdq selector must be a constant 0x00, 0x01, 0x10 or 0x11 - bit 0 picks the first operand's quadword and bit 4 the second's, and it is the instruction's trailing byte"_v,
                                          source);
        return nullptr;
    }

    auto result = substituteType(builder.context, builder.program, callee.result.type, with);
    return builder.addNative(source, NativeOp::CarrylessMultiply, arguments, 2, result, U8(value));
}

} // namespace mir
