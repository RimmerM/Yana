#include "expand_internal.h"

namespace mir {

using namespace intrinsic;

/*
 * `swap` and `exchange` - `doc/spec/core.md`'s Exchange, and the shipping stage's `emitSwap` and
 * `emitExchange` in `resolve/core.cpp`.
 *
 * Both take a value out of a place without leaving a hole: the place holds a live value before and
 * after, so there is no state for the drop analysis to track and nothing for a move path to hold.
 * That is exactly what a raw pointer's load and store already are in this IR - `expandPointerLoad`
 * and `expandPointerStore`, a read and a write through a `Place::Address` root, which `analyze_live`
 * exempts from the move-path rule because "the pointee is storage nothing here tracks" - so each of
 * these is that pair of nodes over the address of what the `&` argument names, and no node of its
 * own. What separates them from `load(addressOf(x))` written by hand is one bit,
 * `NodeAddress::exchange`, which is what the loan checker's element exemption is keyed on.
 *
 * The arguments arrive as `checkRoles` matched them against the declaration: two exclusive loans
 * for a swap, an exclusive loan and an owned value for an exchange. `swap(x, x)` is therefore two
 * exclusive loans of one place and is refused by the rule that predates both - the exemption is for
 * two *elements* of one container, and `exchangeOnlyLoan` says so.
 */

// The raw address of the storage a `&` argument names, marked as an exchange's. The pointer's own
// occurrence is an owned `%a` at the argument's type - a scalar, as every raw pointer is.
static Node* exchangedAddress(Builder& builder, Node& argument, LocationId source) {
    auto pointer = builder.program.types.pointerType(valueOccurrence(argument).type);
    auto address = builder.addAddressOf(source, argument, owned(pointer));
    ((NodeAddress*)address)->exchange = true;
    return address;
}

// The place behind that address, at the pointee's own occurrence and writable - the operation is
// what decides that, as it does for a store.
static Place* exchangedPlace(Builder& builder, Node& address, const Occurrence& held) {
    return builder.addressPlace(address, requalified(held, ownedContract()), true);
}

// A read that takes the value out - `readMode`'s answer for an owned request, which is a copy of a
// trivial value and a move of any other. The move is from storage nothing tracks, which is what
// makes it a relocation rather than a hole; see `analyze_live.cpp`'s raw-pointer arm.
static Node* relocate(Builder& builder, Place& place, LocationId source) {
    auto wanted = bindingRequest(Access::Owned);
    return builder.addRead(source, place, readMode(*builder.program.decl, builder.base(), builder.body, place, wanted), wanted);
}

/*
 * The temporary - a local of the frame the value is parked in before its storage is written.
 *
 * Not a nicety. An owned aggregate read out of storage is, below this tier, the *address* of that
 * storage until the node that consumes it copies the bytes - which is what lets `let ->a = x` move
 * a record with no copy at all - so a value read out of a place and consumed after that place is
 * written again is the new contents and not the old. Both operations here write the place they
 * read before the value they read is consumed, and the temporary is what the spec's "relocates
 * through a temporary" names. What it costs a trivial value is a copy the optimizer folds.
 */
static Node* park(Builder& builder, Node& value, LocationId source) {
    auto local = builder.addLocal(StringId(), valueOccurrence(value), source, false);
    auto place = builder.localPlace(*local);

    builder.addWrite(source, *place, value, Destination::Init);
    return relocate(builder, *place, source);
}

/*
 * `swap(&left, &right)` - three relocations through a temporary, in the order the spec states: both
 * places are read before either is written, so the two naming one element is a no-op rather than a
 * loss. The left value goes to the temporary, the right value to the left place, and the temporary
 * to the right place.
 */
Node* expandSwap(Builder& builder, Function& callee, Node* const* arguments,
                 const Substitution& with, LocationId source) {
    auto left = exchangedAddress(builder, *arguments[0], source);
    auto right = exchangedAddress(builder, *arguments[1], source);

    auto leftPlace = exchangedPlace(builder, *left, valueOccurrence(*arguments[0]));
    auto rightPlace = exchangedPlace(builder, *right, valueOccurrence(*arguments[1]));

    auto held = park(builder, *relocate(builder, *leftPlace, source), source);
    auto moved = relocate(builder, *rightPlace, source);

    builder.addWrite(source, *leftPlace, *moved, Destination::Assign);
    builder.addWrite(source, *rightPlace, *held, Destination::Assign);

    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);
    return builder.addConst(source, unitConstant(result.type));
}

/*
 * `exchange(&slot, ->value)` - the old value out to the temporary, the replacement in. What comes
 * out is the declared result, an owned `a`, and the place is read at that occurrence for
 * `expandPointerLoad`'s reason: the position the value fills is what says what it is. The
 * replacement arrived owned, because the declaration says `->`, and the write consumes it exactly
 * as a store does. The caller's binding then takes the temporary, which is the second relocation
 * the spec counts and a copy the optimizer forwards.
 */
Node* expandExchange(Builder& builder, Function& callee, Node* const* arguments,
                     const Substitution& with, LocationId source) {
    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);

    auto address = exchangedAddress(builder, *arguments[0], source);
    auto place = builder.addressPlace(*address, result, true);

    auto old = park(builder, *relocate(builder, *place, source), source);
    builder.addWrite(source, *place, *arguments[1], Destination::Assign);

    return old;
}

} // namespace mir
