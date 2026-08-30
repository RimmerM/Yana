#pragma once

#include "lower.h"
#include "manifest.h"
#include "../lower/lower_inst.h"
#include "../lower/lower_builder.h"

namespace ssa {

/*
 * SSA to the shared lower IR, across the two files it is written in.
 *
 *  - `lower.cpp`      the module: globals, signatures, block structure, phis, and the driver;
 *  - `lower_inst.cpp` one instruction at a time, and the teardown walk that a drop expands into.
 *
 * The split is where the *questions* differ. Everything in the first file is about the shape a
 * function has before any of its code exists - which is also everything that has to be decided
 * before the first call can be built, since a call names a signature another function declared.
 */
struct Lowering {
    Context& context;
    mir::Program& program;
    Converted& converted;
    LowerModule& to;

    DeclBase decl;
    SsaBase ssa;
    LowerBase lower;

    HashMap<U32, LowerPtr<LowerGlobal>> globals;
    HashMap<U32, LowerPtr<LowerFunction>> functions;

    /*
     * The hidden result pointer each memory-returning function receives.
     *
     * A value too wide for a register comes back through storage the *caller* supplies, so it is a
     * leading parameter on the callee and a leading argument at every call. Held per function rather
     * than recovered from the argument list, because "does this function have one" is the same
     * question as "is its result address-only" asked in two places, and a table that answers it once
     * is what keeps the two ends of the convention from disagreeing.
     */
    HashMap<U32, LowerPtr<LowerValue>> returnPlaces;

    // SSA value ID to the lower value it became, and SSA block index to the lower block. Per
    // function, reset rather than rebuilt - the containers guidance's rule, applied where it costs
    // one allocation per compilation instead of one per function.
    HashMap<U32, LowerPtr<LowerValue>> values;
    HashMap<U32, LowerPtr<LowerBlock>> blocks;

    /*
     * The phis a block's parameters became, and one alternative each edge owes them.
     *
     * A phi has to have one alternative per incoming edge and its alternatives are values in blocks
     * that have not been lowered yet, so it is built detached, filled from the edges as they arrive,
     * and added to its block last - which is also when its uses are registered. `lower_promote.cpp`
     * builds one the same way and says why.
     */
    struct PendingPhi {
        LowerInstPhi* phi = nullptr;
        LowerPtr<LowerBlock> block = nullptr;
        U32 filled = 0;
    };

    Array<PendingPhi> phis;

    // SSA block argument value ID to its index in `phis`.
    HashMap<U32, U32> phiOf;

    // The hidden result pointer of the function being lowered, or null where its result fits a
    // register. Set by `lowerBody`, read by every `ret` in it.
    LowerPtr<LowerValue> currentReturnPlace = nullptr;

    /*
     * lower.cpp.
     */

    void lowerGlobal(DeclPtr<mir::Global> pointer);
    void lowerBody(mir::Function& source, LowerFunction& target);
    void supplyEdge(LowerBlock& from, Edge& edge);
    void run();

    LowerValue* mapped(SsaPtr<Value> value);

    /*
     * How a value manifests - the whole occurrence, from the value itself.
     *
     * Every one of these used to pass `ownedContract()`, because an `ssa::Value` carried only its
     * logical type and there was nothing else to pass. That is a representation failure with a
     * silent wrong answer at the end of it: a shared window and its owner are one logical type, so
     * the table was asked a question that could only be answered as the owner. `Value::occurrence`
     * is what closes it, and this is the one place that reads it.
     */
    Manifestation manifestOf(Value& value) { return manifestNative(decl, value.occurrence); }

    /*
     * lower_inst.cpp.
     */

    void lowerInst(LowerBlock& block, Inst& inst);

    /*
     * What runs when the object at `address` dies - §6.7's two halves.
     *
     * `Drop` runs the effect at last use and `Reclaim` releases the value's own storage, and this is
     * the file where they stop being one instruction: `InstDrop` says the life ends, and what that
     * expands into is a target question. The two halves are carried separately through the walk
     * because an authored one *replaces* the derived walk for its half alone - a record with an
     * authored `Drop` still has its members reclaimed, and running the whole walk for both halves
     * would release every member twice.
     */
    void emitTeardown(LowerBlock& block, LowerPtr<LowerValue> address, DeclPtr<mir::Type> type,
                      bool runDrop, bool runReclaim, LocationId source);

    // The address of one field of the record at `address`. The offset is the manifestation's answer
    // and is asked here and in no other walk - see `fieldOffsetNative`.
    LowerPtr<LowerValue> fieldAddress(LowerBlock& block, LowerPtr<LowerValue> address,
                                      DeclPtr<mir::Type> record, U16 field);

    // Storage in this frame, for a value that has no register to be in: an aggregate under
    // construction, a copy, a call's hidden result. One `alloca` with the manifestation's size and
    // alignment, which is the only form of it this stage builds.
    LowerPtr<LowerValue> frameStorage(LowerBlock& block, const Manifestation& manifest);

    // Installing a value into storage: a store where it is a register value and a block copy where
    // it lives in memory. One function, because "install this here" is one operation and the
    // manifestation is what decides its machine form.
    void install(LowerBlock& block, LowerPtr<LowerValue> address, Value& value);
};

} // namespace ssa
