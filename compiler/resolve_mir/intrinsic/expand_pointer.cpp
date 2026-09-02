#include "expand_internal.h"

namespace mir {

using namespace intrinsic;

/*
 * The raw pointer - `pointerLoad` and `pointerStore` in `op.def`.
 *
 * Both are **places** rather than operations, which is why they are the first two roles this stage
 * implements: the MIR already has an address root and a read and a write through one, so the whole
 * of each expansion is naming the place and reusing the node every other access uses. Nothing about
 * ownership, loans or drop elaboration needs a case for them - a read of an address root is a read.
 *
 * `doc/spec/native.md` is where the safety argument stops: a raw pointer carries no lifetime, no
 * exclusivity and no promise that what it names is initialized. `Place::Address` is a root with no
 * parent for exactly that reason, and it is the only place in this stage where the checked model is
 * left behind.
 */

/*
 * `*p` - a read of what the pointer names.
 *
 * The place's occurrence is the *result's*, substituted: `fn *(it: %a) -> a` answers an owned `a`,
 * and at a call through a `%I32` that is an owned `I32`. Taking it from the result rather than
 * unwrapping the argument's pointer type is deliberate - the two are the same type by `op.def`'s
 * `pointeeOf(0)`, and reading it off the position the value actually fills is what keeps the
 * contract right as well as the type.
 *
 * `readMode` decides what the read *is*, which is the same function every other read asks: a copy
 * where the pointee is trivially copyable, and a move out of the place where it is not. The second
 * is what makes `*p` at an owning type unsafe rather than ill-formed - it takes the value and leaves
 * bytes behind that nothing tracks - and it is Native's to be unsafe, not this function's to refuse.
 */
Node* expandPointerLoad(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source) {
    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);
    auto place = builder.addressPlace(*arguments[0], result, false);

    auto wanted = bindingRequest(Access::Owned);
    return builder.addRead(source, *place, readMode(*builder.program.decl, *place, wanted), wanted);
}

/*
 * `store(p, ->v)` - a write of what the pointer names.
 *
 * The value arrives owned, because the declaration says `->` and `checkRoles` refused it otherwise.
 * That is the whole reason the shape check exists: the pointee holds the value afterwards, so the
 * frame that owned it stops owing it a teardown, and a declaration that wrote a plain parameter
 * there would leave the obligation with the caller and the value with the pointee.
 *
 * `Destination::Assign`, and it releases nothing - which is the thing to be clear about rather than
 * to leave implied. A store does not drop its own destination in this IR: the release an assignment
 * would run is a step the drop analysis places, over move paths, and an address root has no move
 * path because it is not storage this frame owns. So what the pointee held before is neither
 * released nor tracked, which is precisely what makes a raw store unsafe - and is Native's to be
 * unsafe rather than this function's to refuse.
 *
 * `Init` is not available and should not be: it asserts a root the frame owns, and it means "this
 * slot was empty", which nothing here is in a position to claim.
 *
 * The result is `{}`, which the declared result already says - so it is built from the substituted
 * result occurrence rather than from a unit written here.
 */
Node* expandPointerStore(Builder& builder, Function& callee, Node* const* arguments,
                         const Substitution& with, LocationId source) {
    auto pointee = valueOccurrence(*arguments[1]);
    auto place = builder.addressPlace(*arguments[0], requalified(pointee, ownedContract()), true);

    builder.addWrite(source, *place, *arguments[1], Destination::Assign);

    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);
    return builder.addConst(source, unitConstant(result.type));
}

} // namespace mir
