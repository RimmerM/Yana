#include "opt_internal.h"

namespace jsir {

void Optimizer::run() {
    sharedParameter.reset(body.values.size());

    if(body.abi.valid() && body.abi.index < target.abis.size()) {
        auto& abi = target.abi(body.abi);
        auto count = body.parameters.size();

        auto lead = abi.takesGenEnv ? 1u : 0u;
        forEachPosition(abi, [&](U32 position, U32, const JsPassing& parameter, U16) {
            if(!parameter.shared || lead + position >= count) return;

            auto value = base[body.parameters.get(base, lead + position)];
            sharedParameter.set(value->id.index, true);
        });
    }

    /*
     * Forwarding first, then aliasing, then the dead form both leave - and then once more,
     * because a forwarded field turns a clone whose result went into an object literal into a
     * clone of a clone, which is a shape the second pass can alias.
     *
     * Two rounds rather than a worklist to a fixed point: the second round is what the
     * measurement asked for and a third changed nothing over `test/base`, so a fixpoint here
     * would be a loop that always runs twice.
     */
    countPlaceRefs();
    buildUses();

    /*
     * Sinking first, because the two rewrites below it are asked in one block: `adoptInitialStores`
     * wants the zero and the store together, and the zero is in the entry block until this has
     * run. `md5Answers` was already in one block and `normalizeForm`'s `runs` slot was not.
     */
    sinkAllocations();

    countPlaceRefs();
    buildUses();
    adoptInitialStores();
    countPlaceRefs();

    // The capacity question answered before anything asks about the box its guarded call
    // names - see `foldConstantBranches`.
    buildUses();
    foldConstants();
    foldConstantBranches();
    countPlaceRefs();

    for(U32 round = 0; round < 2; round++) {
        buildUses();
        numberValues();

        buildUses();
        forwardFields();

        buildUses();
        aliasCopies(round == 0);

        /*
         * The two copy rewrites that need no whole-body argument, **after** `aliasCopies` has
         * been applied and the index rebuilt. `moveLastUses` must see an alias that pass made
         * as an alias - it is a name for the slot's object, or a field shared with another
         * record - and never as the `clone` it was when the round began. The reverse order is
         * sound too, since `aliasCopies` in the next round follows a moved copy through its
         * `alias` like any other.
         */
        buildUses();
        moveLastUses();

        buildUses();
        readThroughCopies();

        buildUses();
        updatesAsLiterals();

        buildUses();
        dropWriteOnlyObjects();
        dropDead();
        dropDeadLoads();
        dropDead();

        // After the dead forms are gone, since a box is a variable only once every other name
        // for it is.
        countPlaceRefs();
        buildUses();
        unboxLocals();
        countPlaceRefs();

        /*
         * Scalar replacement, after the dead forms are gone for `unboxLocals`'s reason: a literal
         * is fresh for `splitRecordVariables` only once nothing else names it. Splitting leaves
         * scalar stores, forwarding leaves them unread, and the dead-store walk takes them and the
         * zeros the conversion wrote; `dropDead` then takes the literals.
         *
         * Inside the two rounds rather than as a loop of its own to a fixed point, for the rounds'
         * own reason: what one split enables - a clone of a field variable, a literal no longer
         * named - is a question the second round's first half answers, and nothing measured
         * asked for a third.
         */
        buildUses();
        splitRecordVariables();
        countPlaceRefs();
        buildUses();
        storeLiteralFields();
        countPlaceRefs();
        buildUses();
        forwardVariables();
        countPlaceRefs();

        /*
         * Folding again, and the branches it decides, because a forwarded variable is the other
         * way a literal reaches an operation: `let x = 7` then `f(x)` is a load of `$0` until the
         * line above, and the first `foldConstants` saw the load (J136).
         */
        buildUses();
        foldConstants();
        foldConstantBranches();
        countPlaceRefs();

        buildUses();
        dropDeadStores();

        // Once more, because a forwarded variable and this round's aliased copies both leave a
        // literal read through an `alias` the round's first `forwardFields` never saw - and
        // only once the store that forwarded it is gone, since that store still keeps it.
        countPlaceRefs();
        buildUses();
        forwardFields();
        buildUses();
        dropDead();
        dropDeadLoads();
        dropDead();
        countPlaceRefs();
    }

    // And once more, because a forwarded field and a removed literal take uses away: an
    // allocation whose remaining users all sit in one arm is one this could not see before.
    buildUses();
    sinkAllocations();
}

bool optimizeBody(JsTarget& target, JsBody& body) {
    Optimizer(target, body).run();
    return true;
}

} // namespace jsir
