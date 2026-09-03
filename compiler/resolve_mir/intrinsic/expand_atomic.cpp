#include "expand_internal.h"

namespace mir {

using namespace intrinsic;

/*
 * The `Atomic` module's compiler half - Analysis-Atomics.md §3, and `doc/spec/concurrency.md` for
 * the memory model these are written against.
 *
 * `lib/Atomic/Atomic.native.yana` is the whole of the written surface and this is the whole of what
 * stands behind it. There is nothing here that could have been written in Yana: an atomic load is an
 * instruction, and a body over some lower-level intrinsic would move the question down one level and
 * add a call at every use in an unoptimized build.
 *
 * ## The order is read here, and it must be a constant
 *
 * §3.3 requires an ordering argument to be a compile-time constant and this is where that is
 * enforced. The reason is not convenience: an order changes which instructions are generated and
 * which motions the optimizer may perform, so it is not useful run-time state, and requiring it to
 * be settled at the call site is what guarantees good code without an optimizer having run. Passing
 * one through a generic helper stays possible wherever this stage specializes that helper - the
 * argument is a constant *there*, which is the point at which this asks.
 *
 * The four ordering *types* collapse to one `AtomicOrder` on the way in. They exist so that
 * `store(x, LoadAcquire)` does not typecheck, which is a statement about what a caller may write;
 * once the pairing is settled there is one fact left, which is how strong the edge is.
 *
 * ## And the conventions are already applied
 *
 * This family is the one `expand.h` names as the reason an intrinsic call is an ordinary call up to
 * the last step: `intoValue(->self: Atomic(a))` consumes its subject because `argumentMode` read the
 * `->` off the declaration, not because an emitter remembered to sink it. The shipping stage's four
 * emitters applied that by hand and one of them did not, so `intoValue(c)` twice compiled.
 */

/*
 * The four compare-exchange rows are **not here**, and `OpForm::Branch` is why: their expansion is
 * a branch joining what the instruction answered into the one value the library declares, and a
 * value the surrounding walk is holding has to travel that branch's edges. `resolve_intrinsic.cpp`
 * is where they are, beside the `Carry` that knows what is being held.
 *
 * What *is* shared with them is everything below - the order tables, the ordinal read, and
 * `contentOf` - which is why those are not `static` here.
 */

/*
 * The ordinal behind a payload-free constructor, where the call site wrote one.
 *
 * Every ordering type is an enumeration with no payload, so a written `LoadAcquire` is a
 * `Constant::Enum` by the time it reaches here - which is what makes "is this a constant" the same
 * question as "did the caller write one of the constructors". Anything else is an order computed at
 * run time, and there is deliberately no fallback that picks the strongest: a program that reaches
 * this has an ordering decision it thinks it is making and is not.
 */
Maybe<U16> constantOrdinal(Builder& builder, Node& node) {
    if(node.kind != Node::Const) return Nothing();

    auto constant = builder.body.constant(builder.base(), ((NodeConst&)node).constant);
    if(constant.kind != Constant::Enum) return Nothing();

    return Just(constant.asConstructor());
}

/*
 * The three or four each ordering type lists, in declaration order - which is the order the
 * constructors are numbered in, so the ordinal is the index into these.
 *
 * Four tables and not one, because the library's four types are four different sets: `StoreOrder`'s
 * second constructor is `StoreRelease` and `LoadOrder`'s is `LoadAcquire`, and a single table would
 * have made the two mean each other. Which table a position takes is the *declaration's* answer,
 * carried here by which emitter is reading it.
 */
const AtomicOrder kLoadOrders[] = {
    AtomicOrder::Relaxed, AtomicOrder::Acquire, AtomicOrder::Sequential,
};

const AtomicOrder kStoreOrders[] = {
    AtomicOrder::Relaxed, AtomicOrder::Release, AtomicOrder::Sequential,
};

const AtomicOrder kUpdateOrders[] = {
    AtomicOrder::Relaxed, AtomicOrder::Acquire, AtomicOrder::Release,
    AtomicOrder::AcquireRelease, AtomicOrder::Sequential,
};

const AtomicOrder kFenceOrders[] = {
    AtomicOrder::Acquire, AtomicOrder::Release, AtomicOrder::AcquireRelease,
    AtomicOrder::Sequential,
};

Maybe<AtomicOrder> orderArgumentIn(Builder& builder, Node& node, const AtomicOrder* table,
                                   Size count, StringView which, LocationId source) {
    auto ordinal = constantOrdinal(builder, node);

    if(!ordinal || ordinal.unwrap() >= count) {
        builder.context.diagnostics.error(
            "the %@ of an atomic operation is written at the call site - an order decides which instructions are generated, so it cannot be computed at run time"_v,
            source, which);
        return Nothing();
    }

    return Just(AtomicOrder(table[ordinal.unwrap()]));
}

// The `a` of the `Atomic(a)` an operation was handed, taken from the argument rather than from the
// substituted type arguments - so one emitter serves a signature whose result is the content and one
// whose result is not.
DeclPtr<Type> contentOf(Builder& builder, Node& self) {
    auto type = valueOccurrence(self).type;
    if(!type || builder.declBase()[type]->kind != Type::Atomic) return nullptr;

    return ((AtomicType*)builder.declBase()[type])->content;
}

/*
 * `atomic(initial)` - §3.1.
 *
 * One node, whose result *is* the new location: the storage and the initializing write are the
 * lowering's, on the same terms an aggregate's are, and neither of them is ordered. Nothing can be
 * sharing this location yet - what a sharer holds is a borrow, and a borrow of a value still being
 * constructed does not exist - so an ordering edge here would publish to nobody.
 *
 * An atomic is deliberately not implicitly zeroed. `atomic(0)` is already explicit, works for
 * pointers and flags alike, and avoids a second initialization rule for generic storage.
 */
Node* expandAtomicNew(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source) {
    auto result = substituteType(builder.context, builder.program, callee.result.type, with);
    if(!result) return nullptr;

    return builder.addAtomic(source, AtomicOp::New, AtomicOrder::Relaxed, AtomicOrder::Relaxed,
                             false, arguments, 1, result);
}

/*
 * `intoValue(->self)` - §3.1, and the one operation here that emits no atomic instruction.
 *
 * Owning the value proves there is no outstanding borrow and therefore no concurrent accessor, so
 * reading it is an ordinary load. That is what makes it the clean teardown path for a containing
 * structure, and the way a test inspects a private atomic after every worker has joined.
 *
 * The handover is the declaration's and is applied before this runs - `op.def`'s `handoverAt()`,
 * which `checkRoles` holds the declaration to. What this adds is the `Take` on the operand, which
 * `addAtomic` puts there for this row and for no other.
 */
Node* expandAtomicIntoValue(Builder& builder, Function&, Node* const* arguments,
                            const Substitution&, LocationId source) {
    auto content = contentOf(builder, *arguments[0]);
    if(!content) return nullptr;

    return builder.addAtomic(source, AtomicOp::IntoValue, AtomicOrder::Relaxed,
                             AtomicOrder::Relaxed, false, arguments, 1, content);
}

Node* expandAtomicLoad(Builder& builder, Function&, Node* const* arguments, const Substitution&,
                       LocationId source) {
    auto content = contentOf(builder, *arguments[0]);
    if(!content) return nullptr;

    auto order = orderArgument(builder, *arguments[1], kLoadOrders, "order"_v, source);
    if(!order) return nullptr;

    return builder.addAtomic(source, AtomicOp::Load, order.unwrap(), order.unwrap(), false,
                             arguments, 1, content);
}

Node* expandAtomicStore(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source) {
    auto order = orderArgument(builder, *arguments[2], kStoreOrders, "order"_v, source);
    if(!order) return nullptr;

    auto result = substituteType(builder.context, builder.program, callee.result.type, with);

    return builder.addAtomic(source, AtomicOp::Store, order.unwrap(), order.unwrap(), false,
                             arguments, 2, result);
}

/*
 * The exchange and the five fetch operations, which are one shape: a location, a value, an order,
 * and the value from *before* the update as the answer.
 *
 * The `fetch` prefix in the library's names says so, matches what x86 and LLVM naturally return, and
 * keeps the family consistent. Addition and subtraction wrap at the stored width.
 */
Node* expandAtomicUpdate(Builder& builder, AtomicOp op, Node* const* arguments, LocationId source) {
    auto content = contentOf(builder, *arguments[0]);
    if(!content) return nullptr;

    auto order = orderArgument(builder, *arguments[2], kUpdateOrders, "order"_v, source);
    if(!order) return nullptr;

    return builder.addAtomic(source, op, order.unwrap(), order.unwrap(), false, arguments, 2,
                             content);
}

/*
 * `fence(order)` and `spinHint()` - §3.6, and the two operations attached to no location.
 *
 * A fence orders accesses it does not name and does not by itself synchronize two threads: the
 * surrounding atomic operations still have to establish the relation. A spin hint orders nothing at
 * all and only says that the loop around it is polling, which is why its order field is `Relaxed` -
 * every node in this family carries one so that each reads alike, and `Relaxed` is the one that says
 * "no edge".
 */
Node* expandAtomicFence(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source) {
    auto order = orderArgument(builder, *arguments[0], kFenceOrders, "order"_v, source);
    if(!order) return nullptr;

    auto result = substituteType(builder.context, builder.program, callee.result.type, with);
    return builder.addAtomic(source, AtomicOp::Fence, order.unwrap(), order.unwrap(), false,
                             nullptr, 0, result);
}

Node* expandAtomicSpinHint(Builder& builder, Function& callee, Node* const*,
                           const Substitution& with, LocationId source) {
    auto result = substituteType(builder.context, builder.program, callee.result.type, with);
    return builder.addAtomic(source, AtomicOp::SpinHint, AtomicOrder::Relaxed, AtomicOrder::Relaxed,
                             false, nullptr, 0, result);
}

} // namespace mir
