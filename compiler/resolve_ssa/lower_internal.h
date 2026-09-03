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
 *  - `lower_inst.cpp` one instruction at a time, and the teardown walk that a drop expands into;
 *  - `erase.cpp`      the ABI a generic body compiled *once* is called across - Analysis-IR.md's G5;
 *  - `lower_fun.cpp`  the two words a function value is, and the thunk that makes them uniform;
 *  - `lower_width.cpp` what a value narrower than its register costs - the seam, and the normal form.
 *
 * The split is where the *questions* differ. Everything in the first file is about the shape a
 * function has before any of its code exists - which is also everything that has to be decided
 * before the first call can be built, since a call names a signature another function declared. The
 * third is every number the erased form turns on - the environment's word size, the descriptor's
 * fields, where the environment sits in an argument list - and it is a file rather than a section
 * because both ends of each of those numbers are in it.
 */
/*
 * A constant as the bits its manifestation holds - the one place a `mir::Constant` becomes a number.
 *
 * Two callers, and that is why it is here rather than beside either: a global's initial bytes and an
 * immediate in a function body are the same question asked at two moments, and the second used to
 * answer it by reading `Constant::bits` directly. That was right while every kind held its own
 * value; `Constant::Enum` holds an *index* into a constructor list, so the direct read silently
 * emitted the position of a `@value(n)`-pinned constructor instead of the number it was pinned to.
 *
 * Little-endian and **scalar**. An aggregate is not a number and never reaches here: it is a tree of
 * values, and `materialize` beside this is the walk that turns one into bytes with the layout every
 * other value of that type is laid out with. Which of the two a caller wants is the kind, and the
 * two callers that could be handed either ask it.
 *
 * A float is **not** answered here - its bits depend on the declared width, which `materialize`
 * folds in and an immediate carries as a `double`. Callers that build an immediate ask the kind
 * first, which is the one branch this cannot take for them.
 */
U64 constantBits(DeclBase decl, const mir::Constant& constant);

/*
 * One constant's bytes, written where the manifestation says they go - see the definition.
 *
 * Declared here rather than kept to `lower.cpp` because both placements need it: a global's initial
 * contents, and the read-only data an aggregate constant inside a *body* is placed in.
 */
void materialize(Layout layout, DeclBase decl, const mir::Occurrence& occurrence,
                 const mir::Constant& constant, Byte* into);

/*
 * A declared width that does not fill the register holding it - `lower_width.cpp`, which is the only
 * file that builds one and where the whole of the rule is written.
 *
 * Both widths travel, because both are needed and neither can be recovered from the other: `bits` is
 * what the value means and `registerBits` is what holds it, and the distance between them is the
 * shift that puts a signed value back in normal form. `type` is the register's own lane, carried so
 * that a caller building a mask does not ask the manifestation table a second time and get it from a
 * different occurrence.
 *
 * The null value means **the value fills its register**, which is the ordinary case and the one with
 * nothing to do, so every caller reads it as a condition.
 */
struct NarrowWidth {
    U32 bits = 0;
    U32 registerBits = 0;
    bool isSigned = false;
    LowerType type;

    explicit operator bool() const { return bits != 0; }
};

/*
 * Whether an operation's result has to be put back into the declared width - `op.def`'s column, read
 * as the one question the emitter asks after building it.
 *
 * `Logical` is here beside `Wraps` because it is `Wraps` **and** something in front: `shr` needs its
 * operand put in range before the shift as well, which is asked separately and at the one place that
 * has an operand to put there. `Rotates` is not, because the expansion establishes the normal form
 * itself - see `narrowRotate`, whose last act is exactly this.
 */
inline bool wrapsAtDeclaredWidth(mir::WidthEffect effect) {
    return effect == mir::WidthEffect::Wraps || effect == mir::WidthEffect::Logical;
}

struct Lowering {
    Context& context;
    mir::Program& program;
    Converted& converted;
    LowerModule& to;

    DeclBase decl;
    SsaBase ssa;
    LowerBase lower;

    /*
     * What the machine this program is for answers, rather than what this compiler's host does, and
     * the declarations a type is read out of - see `Layout`, which is why they are one value.
     *
     * `layout.base` is `decl` above. The pair exists as its own value because that is what the
     * manifestation table takes: `decl` is how this walk reaches anything at all, and a *layout*
     * question is the one place the machine has a say in the answer.
     */
    Layout layout;

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

    // The generic environment each erased function receives, on the same terms and for the same
    // reason: whether a function has one is a question two ends ask, and a table that answers it
    // once is what keeps them from disagreeing about the argument list's shape.
    HashMap<U32, LowerPtr<LowerValue>> environmentPlaces;

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
    Manifestation manifestOf(Value& value) { return manifestNative(layout, value.occurrence); }

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

    // A sum's derived teardown, which branches on the discriminant and therefore ends in a block of
    // its own - see the definition, and `emitTeardown`, which reaches it through a call.
    LowerBlock* emitSumTeardown(LowerFunction& function, LowerBlock& entry,
                                LowerPtr<LowerValue> address, DeclPtr<mir::Type> type,
                                bool runDrop, bool runReclaim, LocationId source);

    // An address plus a byte offset, or the address itself where the offset is zero. The one place
    // that arithmetic is written, so that a field, a discriminant and a cell all produce the same
    // shape.
    LowerPtr<LowerValue> offsetAddress(LowerBlock& block, LowerPtr<LowerValue> address, U32 offset);

    // The address of one field of the record at `address`. The offset is the manifestation's answer
    // and is asked here and in no other walk - see `fieldOffsetNative`.
    // `shape` is the interior structure of the *occurrence* the record is reached through, which is
    // what decides whether a field holds its value or a reference to somebody else's - see
    // `fieldOffsetNative`. Null everywhere a value owns what is inside it.
    LowerPtr<LowerValue> fieldAddress(LowerBlock& block, LowerPtr<LowerValue> address,
                                      DeclPtr<mir::Type> record, U16 field,
                                      DeclPtr<mir::OwnershipSchema> shape = nullptr);

    /*
     * The address of element `index` of the container at `address` - the base, plus the index times
     * the element's stride.
     *
     * **Where the base is** is the container's manifestation and not the caller's: a fixed array's
     * storage *is* its elements, so the address handed in is already the base; a window is two
     * words, and its base is the first of them - a real load, because what the frame holds is the
     * descriptor and not the run. That fork is the one place the two container spellings differ
     * below the type layer, and it is `kWindowItemsOffset` rather than a number written at a use.
     *
     * One function because two instructions want it and they want the same thing: `ElementAddress`
     * is this, and `Window` is this for the element a subslice starts at. Written twice they were
     * twenty identical lines, which is how one of them comes to scale by a size rather than a
     * stride.
     *
     * There is no bounds check. `doc/spec/collections.md` makes an out-of-range subscript a trap
     * through a library function, and this stage has none to call - the gap is recorded in
     * `resolve_mir/README.md` rather than papered over.
     */
    LowerPtr<LowerValue> elementAddress(LowerBlock& block, LowerPtr<LowerValue> address,
                                        DeclPtr<mir::Type> container, LowerPtr<LowerValue> index);

    // Storage in this frame, for a value that has no register to be in: an aggregate under
    // construction, a copy, a call's hidden result. One `alloca` with the manifestation's size and
    // alignment, which is the only form of it this stage builds.
    LowerPtr<LowerValue> frameStorage(LowerBlock& block, const mir::Occurrence& occurrence);

    // Installing a value into storage: a store where it is a register value and a block copy where
    // it lives in memory. One function, because "install this here" is one operation and the
    // manifestation is what decides its machine form.
    void install(LowerBlock& block, LowerPtr<LowerValue> address, Value& value);

    /*
     * erase.cpp - the erased ABI.
     */

    /*
     * The generic context of the body being lowered, and the environment it was handed.
     *
     * Both null for every ordinary function, which is what makes an erased read fail loudly rather
     * than quietly index a numbering that is not there: a slot is only meaningful relative to one
     * schema, and the schema is the *body's* rather than any callee's.
     */
    DeclPtr<mir::GenContext> currentGen = nullptr;
    LowerPtr<LowerValue> currentEnvironment = nullptr;

    // Interned by what they describe - a type, a filled environment, a callee under one erased
    // signature. Two calls that need one table find one table.
    HashMap<U64, LowerPtr<LowerGlobal>> descriptors;
    HashMap<U32, LowerPtr<LowerGlobal>> environments;
    HashMap<U64, LowerPtr<LowerFunction>> teardowns;
    HashMap<U64, LowerPtr<LowerFunction>> thunks;
    HashMap<U64, LowerPtr<LowerGlobal>> witnesses;
    HashMap<U64, LowerPtr<LowerGlobal>> properties;

    // A function value's code word, interned on the declaration it stands for - `lower_fun.cpp`. A
    // name used as a value in ten places is one thunk, on `thunks`' terms one line up.
    HashMap<U32, LowerPtr<LowerFunction>> funThunks;

    /*
     * The read-only data an **aggregate constant reached in a body** was placed in, interned on the
     * constant - see `placeConstant`, where the placement is argued.
     *
     * Keyed on the type and the handle together, which is what a `Constant`'s own equality is: two
     * aggregates of one shape at two types share a table entry and are two values.
     */
    HashMap<U64, LowerPtr<LowerGlobal>> placedConstants;
    U32 placedConstantCount = 0;

    // The one empty teardown every descriptor with nothing to run points at - see `emptyTeardown`.
    LowerPtr<LowerFunction> trivialTeardown = nullptr;

    void emitReturn(LowerBlock& block, const LowerPtr<LowerValue>* values, Size count);
    StringId erasedName(const String& text);
    LowerGlobal* addTable(StringId name, U32 bytes);
    void writeNumber(LowerGlobal& table, U32 offset, U32 bytes, U64 value);
    void writeCell(LowerGlobal& table, U32 offset, U32 value);
    LowerPtr<LowerGlobal> imageAnchor();

    /*
     * Where a constant with no register form goes - see the definition.
     *
     * This is the placement decision `Constant` deliberately does not make: resolve says what the
     * value is and this tier, which is the first one that knows the target, says where it lives.
     */
    LowerPtr<LowerGlobal> placeConstant(const mir::Constant& constant,
                                        const mir::Occurrence& occurrence);
    void addPointer(LowerGlobal& table, U32 offset, LowerPtr<LowerFunction> function,
                    LowerPtr<LowerGlobal> global);

    LowerPtr<LowerFunction> emptyTeardown();
    // The teardown asked for by which halves it runs - `teardownFor`'s answer once the placement has
    // been read. See the definition, which is where the two callers are told apart.
    LowerPtr<LowerFunction> teardownHalves(DeclPtr<mir::Type> type, bool runDrop, bool runReclaim,
                                           bool release, LocationId source);

    LowerPtr<LowerFunction> teardownFor(DeclPtr<mir::Type> type, mir::StorageClass storage,
                                        LocationId source);
    LowerPtr<LowerGlobal> descriptorFor(DeclPtr<mir::Type> type, mir::StorageClass storage,
                                        LocationId source);

    LowerPtr<LowerFunction> thunkFor(U64 promise, DeclPtr<mir::Function> concrete,
                                     const mir::Occurrence* declared, Size count,
                                     const mir::Occurrence& declaredResult, LocationId source);

    LowerPtr<LowerGlobal> witnessFor(DeclPtr<mir::Class> klass, mir::GenWitness witness,
                                     LocationId source);
    LowerPtr<LowerGlobal> propertyWitnessFor(DeclPtr<mir::Type> subject, U16 field,
                                             LocationId source);
    void writePropertySlot(LowerGlobal& table, U32 offset, DeclPtr<mir::Type> subject, U16 field,
                           PropertyForm form, LocationId source);
    LowerPtr<LowerGlobal> environmentFor(DeclPtr<mir::GenEnvironment> environment,
                                         LocationId source);

    U32 variableSlot(DeclPtr<mir::Type> variable);
    U32 requirementSlot(mir::GenSlotKind kind, U16 requirement);

    /*
     * One slot of the environment this body was handed, in the two shapes a slot comes in.
     *
     * Two functions rather than one with a `type`, because the *encoding* differs and not merely the
     * access: a number cell holds its number - and as many cells as the count's type needs - while
     * an address cell holds one signed offset from the image anchor. One function taking a type
     * would have been one a caller could ask for a pointer from and get four bytes of arithmetic
     * read as one.
     */
    LowerPtr<LowerValue> numberSlot(LowerBlock& block, U32 slot, LowerType type);
    LowerPtr<LowerValue> slotPointer(LowerBlock& block, U32 slot);
    // The shared address arithmetic, a plain word of a stated width, and a table's *address* cell -
    // which is four anchor-relative bytes rather than a pointer. See `kSlotBytes` in erase.cpp.
    LowerPtr<LowerValue> cellAddress(LowerBlock& block, LowerPtr<LowerValue> table, U32 offset);
    LowerPtr<LowerValue> wordAt(LowerBlock& block, LowerPtr<LowerValue> table, U32 offset,
                                LowerType type, U32 width);
    LowerPtr<LowerValue> cellPointer(LowerBlock& block, LowerPtr<LowerValue> table, U32 offset);

    LowerPtr<LowerValue> witnessMember(LowerBlock& block, LowerPtr<LowerValue> table, U16 member);

    /*
     * A field requirement the body reached through a temporary - the two `PropertyForm` shapes that
     * are not a plain address.
     *
     * Recorded per SSA value rather than expanded in place because the two halves are two
     * instructions: `field.addr` is where the subject and the slot are known, and the `store` that
     * may follow it is where there is something to commit. Design.md's tier 1 is exactly this pair,
     * and an `Offset` slot is never one of these - there the body wrote through the field itself.
     */
    struct PropertyAccess {
        LowerPtr<LowerValue> owner = nullptr;
        LowerPtr<LowerValue> temporary = nullptr;
        U32 slot = 0;
        PropertyForm form = PropertyForm::Witness;
        mir::Occurrence held;
    };

    HashMap<U32, PropertyAccess> propertyAccesses;

    // The four values a narrow field's read and its write both need - see `narrowAccess`.
    struct NarrowAccess {
        LowerPtr<LowerValue> address = nullptr;
        LowerPtr<LowerValue> unit = nullptr;
        LowerPtr<LowerValue> shift = nullptr;
        LowerPtr<LowerValue> mask = nullptr;
        LowerPtr<LowerValue> value = nullptr;
        LowerType type = LowerType(LowerLane::Int32, 0, false);
        U32 unitBytes = 0;
    };

    NarrowAccess narrowAccess(LowerBlock& block, LowerPtr<LowerValue> owner,
                              LowerPtr<LowerValue> cell, const mir::Occurrence& held);

    LowerPtr<LowerValue> readProperty(LowerBlock& block, LowerPtr<LowerValue> owner, U32 slot,
                                      const mir::Occurrence& held, PropertyForm form);
    void commitProperty(LowerBlock& block, const PropertyAccess& access);

    LowerPtr<LowerValue> noEnvironment(LowerBlock& block);

    /*
     * lower_width.cpp - the width seam.
     */

    NarrowWidth narrowWidthOf(DeclPtr<mir::Type> type);
    LowerPtr<LowerValue> widthMask(LowerBlock& block, NarrowWidth width);
    LowerPtr<LowerValue> maskToWidth(LowerBlock& block, LowerPtr<LowerValue> value,
                                     NarrowWidth width);
    LowerPtr<LowerValue> zeroExtended(LowerBlock& block, LowerPtr<LowerValue> value,
                                      NarrowWidth width);
    LowerPtr<LowerValue> putInRange(LowerBlock& block, LowerPtr<LowerValue> value, NarrowWidth width,
                                    StringId name);
    LowerPtr<LowerValue> narrowRotate(LowerBlock& block, LowerPtr<LowerValue> lhs,
                                      LowerPtr<LowerValue> rhs, NarrowWidth width, bool left,
                                      StringId name);

    /*
     * lower_fun.cpp - function values.
     */

    LowerPtr<LowerFunction> funThunkFor(DeclPtr<mir::Function> concrete, LocationId source);
    LowerPtr<LowerValue> emitFunValue(LowerBlock& block, DeclPtr<mir::Function> callee,
                                      LowerPtr<LowerValue> environment,
                                      LocationId source);
    void storeWord(LowerBlock& block, LowerPtr<LowerValue> address, U32 offset,
                   LowerPtr<LowerValue> value);

    // A constant as a value - `LowerImm` plus the downcast reading it back needs.
    LowerPtr<LowerValue> immediate(LowerBlock& block, LowerType width, U64 value);

    LowerPtr<LowerValue> sizeValue(LowerBlock& block, const mir::Occurrence& occurrence);
    LowerPtr<LowerValue> dynamicStorage(LowerBlock& block, const mir::Occurrence& occurrence);

    // One measurement of `of`, as a value of `width` - a literal where this compilation laid the
    // type out, and a read of the caller's descriptor where it did not. See the definition.
    LowerPtr<LowerValue> metricValue(LowerBlock& block, mir::TypeMetric metric,
                                     const mir::Occurrence& occurrence, LowerType width);
    void emitErasedTeardown(LowerBlock& block, LowerPtr<LowerValue> address,
                            DeclPtr<mir::Type> variable);
};

} // namespace ssa
