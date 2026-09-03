#include "resolve_internal.h"
#include "intrinsic/expand_internal.h"

namespace mir {

using namespace intrinsic;

/*
 * The intrinsics whose expansion makes blocks - `OpForm::Branch`, and the four compare-exchanges are
 * the whole of the list.
 *
 * Here rather than in `intrinsic/` because of what crosses a branch. `expandIntrinsic`'s contract is
 * that an emitter receives prepared values and answers one node; an emitter that made blocks would
 * satisfy neither half of what §5.9 asks of one - a value the surrounding walk is holding is live
 * across the blocks it made and has to travel their edges as a block argument, and `Held`/`Carry`
 * is the walk's own state rather than anything a `Builder` can see. That is the same division the
 * `Flow` roles are already under: `&&` is laid out by the operator walk for the same reason.
 *
 * What this file does *not* hold is anything about atomics that is not the block structure. The
 * order tables, the ordinal read and the content type are `expand_atomic.cpp`'s and are declared in
 * `expand_internal.h`, so which constructor of `LoadOrder` means acquire is stated once.
 */

/*
 * `compareExchange` and its three siblings - Analysis-Atomics.md §3.4 and §3.5.
 *
 * The instruction answers two things and the library answers one, so what is built here is the
 * branch that joins them: `ExchangeResult(a)` is `Exchanged` where the store happened and
 * `Unchanged(previous)` where it did not.
 *
 * The flag is read out of the instruction rather than reconstructed by comparing `previous` against
 * `expected`, and the weak form is why. It may fail spuriously - answering the value it was handed
 * and *not* having stored - and a caller that compared would read that as a win. `NodeAtomicOk` is
 * the projection that reads it, and it exists because one node has one result.
 *
 * `HeldValue` around `previous` is the load-bearing line. It is produced before the branch and read
 * inside the second alternative, so it crosses a block boundary - and registering it is what makes
 * `armsValue` declare a parameter for it on the blocks it creates and point the handle at what
 * arrived. Without it the second alternative would name a value defined in another block, which
 * `checkValueScope` reports.
 */
Node* Resolver::expandCompareExchange(Builder& builder, Function& callee, Node* const* arguments,
                                      const Substitution& with, AtomicOrder success,
                                      AtomicOrder failure, bool weak, LocationId source) {
    auto content = contentOf(builder, *arguments[0]);
    if(!content) return nullptr;

    auto result = substituteType(context, program, callee.result.type, with);
    auto record = result && decl()[result]->kind == Type::Record
        ? (RecordType*)decl()[result] : nullptr;

    /*
     * Two constructors, the second carrying what was actually there.
     *
     * Reported rather than asserted, because what makes the result that shape is a declaration in a
     * library: a program compiled against an `Atomic` module whose `ExchangeResult` has been changed
     * is a broken installation, and an assertion would call it a compiler bug.
     */
    if(!record || record->cons.size() != 2) {
        context.diagnostics.error(
            "a compare-exchange answers %@, and what it needs is a result with two constructors - one for the exchange happening and one carrying what was there"_v,
            source, typeText(context, decl(), result));
        return nullptr;
    }

    auto previous = builder.addAtomic(source, AtomicOp::Compare, success, failure, weak, arguments,
                                      3, content);
    if(!previous) return nullptr;

    auto exchanged = builder.addAtomicOk(source, *previous, booleanType(program));
    if(!exchanged) return nullptr;

    // See the note above: `previous` crosses into the second alternative, and this is what carries
    // it there. `exchanged` does not - the test is emitted in the block this call is standing in.
    HeldValue held(*this, previous);

    return armsValue(builder, source, 2, result, true,
        [&](U32 index, Block* onFail, DeclPtr<Type>, Carry& carry) -> Node* {
            if(index == 0) {
                // The store happened: `Exchanged`, which carries nothing. `failIfNot` is the same
                // refutable test a pattern emits, so the second alternative is reached by the same
                // edge a `match` would have used.
                failIfNot(builder, *exchanged, *onFail, carry, source);
                return builder.addAggregate(source, result, 0, nullptr, 0);
            }

            // And `Unchanged(previous)`, whose one field is what the location actually held - which
            // is what lets a retry loop feed the answer straight back in as the next attempt's
            // expectation.
            Node* components[] = { held.held };
            return builder.addAggregate(source, result, 1, components, 1);
        });
}

/*
 * The dispatch for the form, which is one switch over the rows that have it.
 *
 * A `default` arm and a reported error, which is where this **departs** from `expandIntrinsic`: that
 * function names every row of `op.def` and gets `-Wswitch` for its trouble, and this one names four
 * of two hundred. Enumerating the rest to buy the same warning would be two hundred lines saying
 * "not this one", so what stands in its place is the diagnostic below - a row given `OpForm::Branch`
 * with no arm here says so at the call, rather than silently becoming an ordinary call of a
 * declaration that has no body to run.
 */
Node* Resolver::expandBranchingIntrinsic(Builder& builder, Op op, Function& callee,
                                         Node* const* arguments, const Substitution& with,
                                         LocationId source) {
    switch(op) {
        /*
         * The plain forms, whose failure order is §3.5's projection of the success order - a
         * failure performs no write, so a release half has nothing to order and is dropped.
         */
        case Op::atomicCompareExchange:
        case Op::atomicCompareExchangeWeak: {
            auto success = orderArgument(builder, *arguments[3], kUpdateOrders, "order"_v, source);
            if(!success) return nullptr;

            return expandCompareExchange(builder, callee, arguments, with, success.unwrap(),
                                         failureOrderFor(success.unwrap()),
                                         op == Op::atomicCompareExchangeWeak, source);
        }

        // And `Advanced`'s, which state both and are held to §3.5's table.
        case Op::atomicComparePair:
        case Op::atomicComparePairWeak: {
            auto success = orderArgument(builder, *arguments[3], kUpdateOrders, "success order"_v,
                                         source);
            if(!success) return nullptr;

            auto failure = orderArgument(builder, *arguments[4], kLoadOrders, "failure order"_v,
                                         source);
            if(!failure) return nullptr;

            if(!isLegalFailureOrder(success.unwrap(), failure.unwrap())) {
                context.diagnostics.error(
                    "the failure order of a compare-exchange may not be stronger than its success order - a failed comparison performs no write, so there is nothing for the stronger one to order"_v,
                    source);
                return nullptr;
            }

            return expandCompareExchange(builder, callee, arguments, with, success.unwrap(),
                                         failure.unwrap(), op == Op::atomicComparePairWeak, source);
        }

        default:
            break;
    }

    context.diagnostics.error("internal: %@ is laid out by the resolver and has no expansion"_v,
                              source, opDef(op).role);
    return nullptr;
}

} // namespace mir
