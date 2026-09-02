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

        // Not a fallback. A row with no arm is a role the tables accept and the compiler cannot
        // build, which is a mistake in this directory rather than anything a program can provoke.
        case Op::Count: break;
    }

    assertTrue(false);
    return nullptr;
}

} // namespace mir
