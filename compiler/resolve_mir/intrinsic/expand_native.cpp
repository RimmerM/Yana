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

} // namespace mir
