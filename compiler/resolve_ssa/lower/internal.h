#pragma once

#include "gen.h"
#include "manifest.h"
#include "../../lower/lower_inst.h"
#include "../../lower/lower_builder.h"

namespace ssa {

/*
 * What a release site knows about where a boxed field's storage came from - the question
 * `known.def`'s `isHeapAddress` is two answers to.
 *
 * A box is an allocation, and handing it back is only correct where the program made it. Which of
 * these applies is decided by *how the release site reached the value*, not by the value's type:
 *
 *  - `Owned` - the placement is visible here. A value this frame owns, or one reached through a
 *    descriptor built for a placement, has interior boxes on the heap by construction, so the
 *    release is an unconditional `freeHeap` and there is nothing to test.
 *  - `Unknown` - the site was handed bytes and a type and nothing else, which is what an overwrite
 *    through a `&` borrow is: the caller may have placed the value in a region, and its boxes with
 *    it. Which of the two compilation modes answers it is `storesBoxProvenance` - a stored bit, or
 *    a question put to the allocator about the address.
 *  - `Foreign` - the placement is visible and is *not* the program's to hand back, which is a region
 *    discharging everything inside it in bulk. Nothing is emitted.
 *
 * Threaded through the teardown walk rather than derived inside it, because the walk is written
 * against a type and this is not a fact about the type. Two values of one type can reach a release
 * under two of these, and a build that decided it from the type would be answering the wrong
 * question in whichever of them it did not see.
 */
enum class BoxRelease: U8 {
    Owned,
    Unknown,
    Foreign,
};

/*
 * SSA to the shared lower IR, across the seven files it is written in.
 *
 *  - `gen.cpp`   the module: globals, signatures, block structure, phis, and the driver;
 *  - `inst.cpp`  one instruction at a time, and the teardown walk that a drop expands into;
 *  - `erase.cpp` the ABI a generic body compiled *once* is called across - Analysis-IR.md's G5;
 *  - `fun.cpp`   the two words a function value is, and the thunk that makes them uniform;
 *  - `width.cpp` what a value narrower than its register costs - the seam, and the normal form;
 *  - `pack.cpp`  what a component with no address costs - the four operations over one;
 *  - `box.cpp`   what a component with an address of its own costs - `@box`, and its release.
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
 * A declared width that does not fill the register holding it - `width.cpp`, which is the only
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

/*
 * The register a value of this many bytes is taken apart in.
 *
 * Derived from the **width** rather than read off a manifestation, and the difference is the one
 * narrow value that is not a scalar: a record this target scalarized is still reached through
 * storage, so its manifestation says `Pointer`, and its bits are an integer like any other's. Every
 * site that shifts or masks a component asks this, so that a decode and the encode that undoes it
 * cannot compute in two different registers.
 */
inline LowerType packLane(U32 bytes) { return bytes > 4 ? LowerType::Int64 : LowerType::Int32; }

/*
 * One cell of a record that travels in a register of its own - `convention.cpp`.
 *
 * Where the bytes are in the record, how many, and the register type they are read and written at.
 */
struct CarriedPart {
    U32 offset = 0;
    U32 bytes = 0;
    LowerType type;
};

// Where one declared parameter's parts are in `Lowering::shapeParts`; a count of zero is a parameter
// passed as it manifests.
struct ShapeArg {
    U32 start = 0;
    U32 count = 0;
};

/*
 * One cell of a compiler-built table, in bytes - and the one number two files have to agree on.
 *
 * Here rather than in `erase.cpp`, where the encoding is argued at length, because `fun.cpp` reads
 * a cell that is in no table: the four bytes in front of a lifted lambda are written and read the
 * way a table's address cell is, and a second `4` written there is a second number that can drift.
 */
constexpr U32 kSlotBytes = 4;

/*
 * A declared function's aggregate convention - which parameters arrive as parts, and whether the
 * result leaves as parts instead of through a hidden pointer. Only functions that split something
 * have one, so every reader treats a missing shape as the convention `README.md` describes.
 *
 * Indices into flat tables on `Lowering` rather than lists of its own: every shape is computed
 * before the first body is lowered, and read far more often than it is built.
 */
struct CallShape {
    U32 firstArg = 0;
    U32 argCount = 0;
    U32 resultStart = 0;
    U32 resultCount = 0;

    bool splitsResult() const { return resultCount != 0; }
};

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

    // Global initializers are filled before function symbols are declared. Function-address
    // relocations are recorded here and completed after that declaration pass.
    struct PendingFunctionRelocation {
        LowerPtr<LowerGlobal> global;
        U32 relocation;
        U32 function;
    };
    Array<PendingFunctionRelocation> pendingFunctionRelocations;

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

    /*
     * convention.cpp - the aggregate half of the calling convention, one shape per function that
     * splits anything, keyed by the function's offset.
     */
    Array<CallShape> shapes;
    Array<ShapeArg> shapeArgs;
    Array<CarriedPart> shapeParts;
    HashMap<U32, U32> shapeIndex;

    bool carriedCells(const mir::Occurrence& occurrence, SmallArray<CarriedPart, 16>& cells);
    bool joinCells(Value& argument, SmallArray<CarriedPart, 16>& cells);
    void planCallShape(DeclPtr<mir::Function> function);
    const CallShape* callShapeOf(DeclPtr<mir::Function> function);

    // A call's argument for declared position `position`: the address itself, or the parts loaded
    // out of it where the callee takes that parameter apart.
    void passCarried(LowerBlock& block, const CallShape* shape, U32 position,
                     LowerPtr<LowerValue> address, SmallArray<LowerPtr<LowerValue>, 8>& passed);

    // One part, read out of or written into the record at `storage`.
    LowerPtr<LowerValue> loadCarried(LowerBlock& block, const CarriedPart& part, LowerPtr<LowerValue> storage);
    void storeCarried(LowerBlock& block, const CarriedPart& part, LowerPtr<LowerValue> value,
                      LowerPtr<LowerValue> storage);

    void storeReturnedParts(LowerBlock& block, const CallShape& shape, LowerInst* call,
                            LowerPtr<LowerValue> storage);

    const CarriedPart& resultPart(const CallShape& shape, U32 i) { return shapeParts[shape.resultStart + i]; }

    bool carriedDuplicate(Inst& copy, Value& result);

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

    /*
     * A block argument held in memory that crosses its edges as its cells - `gen.cpp`'s
     * `joinCells`, which is where the argument is. Its phis are `count` consecutive entries of
     * `phis` from the one `phiOf` names, and its cells are the same run of `joinParts`.
     */
    struct JoinSplit {
        U32 start = 0;
        U32 count = 0;
    };

    HashMap<U32, JoinSplit> joinSplits;
    Array<CarriedPart> joinParts;

    // The hidden result pointer of the function being lowered, or null where its result fits a
    // register. Set by `lowerBody`, read by every `ret` in it.
    LowerPtr<LowerValue> currentReturnPlace = nullptr;

    /*
     * The function whose blocks are being written, for the one instruction whose expansion needs
     * more of them than it was given - `Inst::Switch`, which becomes a chain of tests.
     *
     * A member rather than a parameter threaded through `lowerInst`, because it is the *walk's*
     * state and not the instruction's: `emitSumTeardown` takes one because its caller is the erased
     * thunk writer rather than the block walk, and everything here is inside one body.
     */
    LowerFunction* currentLowerFunction = nullptr;

    // The aggregate convention of the function being lowered, or null where it splits nothing -
    // read by every `ret` in it, beside `currentReturnPlace`.
    const CallShape* currentShape = nullptr;

    /*
     * gen.cpp.
     */

    /*
     * One constant's bytes, written where the manifestation says they go - see the definition.
     *
     * A member rather than a free function because a leaf of a constant may be the **address of a
     * global**, which is a relocation on the global being filled and a lookup of the one being
     * named - two things only the lowering has. `owner` is that global and `into` a byte inside its
     * contents; `owner` is null for the scratch buffer a co-packed field is built in, where an
     * address cannot appear at all.
     *
     * Both placements need it: a global's initial contents, and the read-only data an aggregate
     * constant inside a *body* is placed in.
     */
    void materialize(const mir::Occurrence& occurrence, const mir::Constant& constant,
                     LowerGlobal* owner, Byte* into);

    // Declaring every global before any of them is filled - see `run`, where the order is argued.
    void declareGlobal(DeclPtr<mir::Global> pointer);
    void fillGlobal(DeclPtr<mir::Global> pointer);
    void lowerBody(mir::Function& source, LowerFunction& target);
    void hintDeclaredRanges(Body& body);
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
     * inst.cpp.
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
    /*
     * Answers the block to carry on in, which is `&block` wherever nothing split. A sum's teardown
     * is a *call* and the guard in front of it is a branch, so a walk that emits one ends somewhere
     * else - and `home` is where the two blocks that takes go. A caller with no function to hand
     * over (a `Drop` in the middle of a body) passes none and gets the walk exactly as it was.
     */
    LowerBlock* emitTeardown(LowerBlock& block, LowerPtr<LowerValue> address,
                             DeclPtr<mir::Type> type, bool runDrop, bool runReclaim,
                             LocationId source, BoxRelease boxes = BoxRelease::Owned,
                             LowerFunction* home = nullptr);

    // The tag of a sum at an address, and the one constructor of it that owes anything - the pair
    // the guard above is built from. See the definitions in inst.cpp.
    LowerPtr<LowerValue> discriminantOf(LowerBlock& block, LowerPtr<LowerValue> address,
                                        DeclPtr<mir::Type> type, LowerType& tagLower);
    Maybe<U64> soleOwingConstructor(DeclPtr<mir::Type> type);

    // A sum's derived teardown, which branches on the discriminant and therefore ends in a block of
    // its own - see the definition, and `emitTeardown`, which reaches it through a call.
    LowerBlock* emitSumTeardown(LowerFunction& function, LowerBlock& entry,
                                LowerPtr<LowerValue> address, DeclPtr<mir::Type> type,
                                bool runDrop, bool runReclaim, LocationId source,
                                BoxRelease boxes = BoxRelease::Owned);

    // An address plus a byte offset, or the address itself where the offset is zero. The one place
    // that arithmetic is written, so that a field, a discriminant and a cell all produce the same
    // shape.
    LowerPtr<LowerValue> offsetAddress(LowerBlock& block, LowerPtr<LowerValue> address, U32 offset);

    // The address of one field of the record at `address`. The offset is the layout table's answer
    // and is read here and in no other walk - `reprOf`, whose `fields` is where a field's position
    // is decided.
    // `shape` is the interior structure of the *occurrence* the record is reached through, which is
    // what decides whether a field holds its value or a reference to somebody else's, and is why
    // `reprOf` takes the pair. Null everywhere a value owns what is inside it.
    LowerPtr<LowerValue> fieldAddress(LowerBlock& block, LowerPtr<LowerValue> address,
                                      DeclPtr<mir::Type> record, U16 field,
                                      DeclPtr<mir::OwnershipSchema> shape = nullptr);

    /*
     * The address of element `index` of the container at `address` - the base, plus the index times
     * the element's stride.
     *
     * **A fixed array only.** Its storage *is* its elements, so the address handed in is already
     * the base. A window's element is the `Index` instance's to find, through the view record's
     * own fields, and never reaches here - which is Analysis-Manifestations.md Part 8's rule that
     * the compiler reads no field of a view.
     *
     * There is no bounds check. `doc/spec/collections.md` makes an out-of-range subscript a trap
     * through a library function, and a fixed array's element is a place of the compiler's; the
     * check on one is `resolve_mir`'s to insert, and the gap is recorded in its README.
     */
    LowerPtr<LowerValue> elementAddress(LowerBlock& block, LowerPtr<LowerValue> address,
                                        DeclPtr<mir::Type> container, LowerPtr<LowerValue> index);

    // Storage in this frame, for a value that has no register to be in: an aggregate under
    // construction, a copy, a call's hidden result. One `alloca` with the manifestation's size and
    // alignment, which is the only form of it this stage builds.
    // An allocation on the heap, at the point a frame slot would have been - see the definition.
    LowerPtr<LowerValue> heapStorage(LowerBlock& block, const mir::Occurrence& occurrence,
                                     LocationId source);

    // `whole` is the caller promising that the first thing to write the slot writes every byte of
    // it, which is what makes the `hasPaddedWord` zeroing a write nothing could read.
    LowerPtr<LowerValue> frameStorage(LowerBlock& block, const mir::Occurrence& occurrence,
                                      bool whole = false);
    void zeroStorage(LowerBlock& block, LowerPtr<LowerValue> storage, U32 size,
                     DeclPtr<mir::Type> type = nullptr);

    /*
     * Where a memory-resident value is **going**, decided before its producer runs - §5.6's
     * destination passing.
     *
     * The position decides the placement, which is the same sentence D17 applies to a closure
     * environment: a value with exactly one consumer, and a consumer that is going to copy it into
     * storage it knows the address of, does not need a frame slot of its own at all. What it needs
     * is that address, and the two consumers that have one are the `return` - the caller's hidden
     * result pointer - and an `aggregate` component, whose address is a field of the record being
     * built or the box that field is.
     *
     * `storageFor` is the entry point and is what every producer of a memory value asks; it answers
     * the destination where there is one and a frame slot where there is not, and records the answer
     * so that the consumer knows not to copy. `destinationFor` is the search behind it and recurses:
     * the aggregate whose field is the destination may itself be destined for its own consumer, so
     * `Tree {left: Just(build(..))}` builds the child inside the box inside the caller's storage.
     *
     * Left to a later pass this is `compiler/lower`'s `forwardCopyDestinations`, and it costs 28.2 G
     * instructions on the library where the shipping stage pays 0.09 G (findings.md §78.6) - a
     * quadratic chase after copies that were never necessary. It also cannot reach the box: the
     * allocation is emitted *after* the temporary is filled, so the destination does not dominate
     * the writes and the copy has to stay.
     */
    LowerPtr<LowerValue> storageFor(LowerBlock& block, Value& value);
    LowerPtr<LowerValue> destinationFor(LowerBlock& block, Value& value);
    bool readInsidePlacement(Block& block, Value& value, SsaPtr<Value> root);

    /*
     * The values `storageFor` placed at a destination rather than in a slot of their own, and where.
     *
     * Read by the consumer that supplied the address: an `install` of a value that is already in the
     * storage it would be installed into is a self-copy, and a boxed component whose box was made
     * early must not be given a second one. Per function, like `values` beside it.
     */
    HashMap<U32, LowerPtr<LowerValue>> destinations;

    /*
     * The box targets whose teardown is being expanded in place right now - `emitBoxTeardown`.
     *
     * A boxed field's release is written inline where it can be, so that one frame walks one level
     * of a recursive structure rather than two: `Maybe(Tree)`'s teardown frees the node itself and
     * calls its children's, where a call to a second per-type function made `teardown(Maybe(Tree))`
     * and `box(Tree)` alternate down the tree. This is what stops that expansion from being
     * infinite - a type whose box edge is its own, `data Chain {next: @box Chain}`, is on the stack
     * when the walk reaches it again and takes the call instead.
     *
     * A stack rather than a visited set: two boxed fields of one record are two expansions and
     * neither is inside the other, and a set would refuse the second for the first's sake.
     */
    SmallArray<DeclPtr<mir::Type>, 8> boxExpansion;

    // Installing a value into storage: a store where it is a register value and a block copy where
    // it lives in memory. One function, because "install this here" is one operation and the
    // manifestation is what decides its machine form.
    void install(LowerBlock& block, LowerPtr<LowerValue> address, Value& value, bool isVolatile = false);

    // A fixed array's components as one `setpattern`, where every one of them is the same byte -
    // the block fill, and false where they are not. See the definition for the four things that
    // have to hold.
    bool uniformFill(LowerBlock& block, LowerPtr<LowerValue> storage, InstAggregate& written, U32 stride);

    // A record's runs of zero fields as `setpattern`s, with the fields each run covers marked in
    // `filled` so that the field walk skips them. See the definition for what bounds a run.
    void zeroFieldRuns(LowerBlock& block, LowerPtr<LowerValue> storage, InstAggregate& written,
                       mir::RecordType& declared, U16 first, Repr& repr,
                       SmallArray<bool, 32>& filled);

    /*
     * box.cpp - `@box`, and the allocation an indirect field is.
     */

    /*
     * The function filling one of the compiler's roles, already lowered, or null with a report.
     *
     * Every heap operation this file emits goes through it, because the failure it reports is the
     * one Design-Allocator.md §18.2 wrote roles for: a build whose library declares no allocator
     * used to turn the heap off silently, and a program that needed one got a wrong answer rather
     * than a message. Here the message names the role and the type whose storage wanted it.
     */
    LowerPtr<LowerFunction> roleFunction(intrinsic::Known role, LocationId source);

    /*
     * `-check-locations` - the located check this call becomes, or null.
     *
     * **The substitution is here and not in the resolver**, which is the one decision this feature
     * turns on. A located call carries three or four constant arguments, and every rule that reads a
     * check - the discharge of a branch whose arm calls `checkFailed`, the fold that removes a bound
     * it has proved, the memory pass that keeps storage live across one - was written against the
     * shape with none. Emitting the located form up in MIR makes each of those a second pattern to
     * recognize, which is what the shipping stage's `isCheckCall` exists to paper over. Emitted
     * *here*, after every one of those passes has run, the optimizer only ever sees one shape and
     * the switch cannot change what a program computes.
     *
     * The other half is `Conversion::graftCall`, which is what makes the location the **subscript's**
     * rather than the library's: `Index.get` writes its own bounds test and `@inline(always)` grafts
     * it, so the call this substitutes for was written in `lib/Core/Array.yana` and the site an
     * author wants named is the one it was grafted into.
     */
    DeclPtr<mir::Function> locatedCheck(DeclPtr<mir::Function> callee);

    // Its four trailing arguments - the module's bytes, their count, the line and the column, each
    // at the width the located declaration wrote for it. `from` is the first of the four.
    bool checkLocationArguments(LowerBlock& block, mir::Function& located, U32 from,
                                LocationId source, SmallArray<LowerPtr<LowerValue>, 8>& passed);

    // The bytes of one module's name, as a constant global - one per module, since every check in a
    // file names the same file.
    LowerPtr<LowerGlobal> moduleNameBytes(StringId name, U32& count);

    /*
     * A boxed field's storage, made - `allocateHeap` at the target's size, the pointer stored into
     * the field's own slot, and the provenance bit written where this build stores one.
     *
     * Answers the address of the **target**, which is what everything after a construction wants:
     * the value is installed through it, and the slot it came out of is never read again until the
     * value dies.
     *
     * `record` and `field` rather than a bare slot address, because the flag bit is a property of
     * the enclosing record's layout and this is the one walk that has both in hand.
     */
    LowerPtr<LowerValue> createBox(LowerBlock& block, LowerPtr<LowerValue> owner,
                                   DeclPtr<mir::Type> record, U16 field,
                                   DeclPtr<mir::OwnershipSchema> shape, LocationId source);

    // The target of a boxed field: the pointer in its slot, loaded. The one place that load is
    // written, so that a read, a write and a release all reach the value the same way.
    LowerPtr<LowerValue> boxTarget(LowerBlock& block, LowerPtr<LowerValue> slot);

    /*
     * The teardown of one boxed field - the target's own obligations, and then its storage.
     *
     * A **call** rather than an expansion, and for two reasons at once. It needs control flow that
     * `emitTeardown` has none of - the release is conditional wherever the placement is not visible
     * - and a recursive type reaches its own teardown through exactly this edge, so the memo on the
     * generated function is what makes `data List = Nil | Cons {tail: List}` terminate instead of
     * inlining forever.
     */
    LowerBlock* emitBoxTeardown(LowerBlock& block, LowerPtr<LowerValue> owner,
                                DeclPtr<mir::Type> record, U16 field,
                                DeclPtr<mir::OwnershipSchema> shape, bool runDrop, bool runReclaim,
                                BoxRelease release, LocationId source,
                                LowerFunction* home = nullptr);

    // The generated function `emitBoxTeardown` calls, interned on everything that distinguishes one
    // - see the definition, where the four-part key is written out.
    LowerPtr<LowerFunction> boxTeardownFor(DeclPtr<mir::Type> target, bool runDrop, bool runReclaim,
                                           BoxRelease release, LocationId source);

    // Hand one allocation back. `freeHeap` and nothing else, so that the role is looked up once.
    void emitFreeHeap(LowerBlock& block, LowerPtr<LowerValue> address, LocationId source);

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

    // The per-boxed-field teardowns, on `teardowns`' reasoning with one more thing in the key - see
    // `boxTeardownFor`, where the four parts are written out. A separate table rather than a wider
    // key in that one, because the two produce functions of different *signatures*: a conditional
    // release in the stored mode takes the flag as a second argument.
    HashMap<U64, LowerPtr<LowerFunction>> boxTeardowns;
    HashMap<U64, LowerPtr<LowerFunction>> thunks;
    HashMap<U64, LowerPtr<LowerGlobal>> witnesses;

    /*
     * The `Call` requirement's one-cell tables and their entries, keyed on the pair each is about -
     * the concrete subject, and which of the promised positions the calling body erased.
     *
     * Tables of their own because neither holds an instance's members: `witnesses`' key is built out
     * of exactly those, and `thunks`' name is the callee's symbol, which is not an identity here at
     * all - one closure reached at two promises is two entries and one symbol.
     */
    HashMap<U64, LowerPtr<LowerGlobal>> callWitnesses;
    HashMap<U64, LowerPtr<LowerFunction>> callThunks;
    HashMap<U64, LowerPtr<LowerGlobal>> properties;

    // A function value's code word, interned on the declaration it stands for - `fun.cpp`. A
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

    // `-check-locations`: one bytes global per module name - see `moduleNameBytes`.
    HashMap<U32, LowerPtr<LowerGlobal>> checkModuleNames;
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

    // A constant as a value - see the definition, and `lowerMap`, which is the second caller.
    LowerPtr<LowerValue> emitConstant(LowerBlock& block, const mir::Constant& constant,
                                      const mir::Occurrence& occurrence);

    /*
     * A `match` over constants, as the shape the target wants - map.cpp, where the six forms are.
     *
     * The one place the algebra of a mapping is decided, so that x64 and LLVM cannot disagree about
     * what `indexOf` costs. What is left below it is the memory form of a residual table, which is a
     * global and a load and needs no instruction of its own.
     */
    void lowerMap(LowerBlock& block, InstMap& written);

    /*
     * The table a residual `map` was placed in, interned on its **contents** - see `lowerMap`.
     *
     * Keyed on the bytes rather than on the entry list, and the reason is where the copies come
     * from: the inliner clones a `map` per call site, so one `nameOf` opened three times is three
     * instructions holding three equal tables. Their entry lists are three different lists in three
     * different regions and their bytes are one run, which is what a reader of the image sees.
     */
    Array<LowerPtr<LowerGlobal>> placedMapTables;
    void addPointer(LowerGlobal& table, U32 offset, LowerPtr<LowerFunction> function,
                    LowerPtr<LowerGlobal> global);

    LowerPtr<LowerFunction> emptyTeardown();

    /*
     * D17's one cell and the three functions behind it - `fun.cpp`, where they are argued.
     *
     * `attachClosureHeader` puts the cell in front of a lifted function, `closureBlockTeardown` is
     * what a heap-placed environment's cell names, and `funTeardown` is the one shared function
     * every drop of a `Fun` calls.
     */
    void attachClosureHeader(LowerPtr<LowerFunction> code, LowerPtr<LowerFunction> teardown);
    LowerPtr<LowerValue> emitClosureBlock(LowerBlock& block, DeclPtr<mir::Type> environment,
                                          LowerPtr<LowerValue> from, LocationId source);
    LowerPtr<LowerFunction> closureBlockTeardown(DeclPtr<mir::Type> environment, LocationId source);
    LowerPtr<LowerFunction> funTeardown(LocationId source);

    // One per environment record, and one for the whole program - see the two definitions.
    HashMap<U32, LowerPtr<LowerFunction>> closureTeardowns;
    LowerPtr<LowerFunction> funRelease = nullptr;
    // The teardown asked for by which halves it runs - `teardownFor`'s answer once the placement has
    // been read. See the definition, which is where the two callers are told apart.
    LowerPtr<LowerFunction> teardownHalves(DeclPtr<mir::Type> type, bool runDrop, bool runReclaim,
                                           bool release, LocationId source,
                                           BoxRelease boxes = BoxRelease::Owned);

    LowerPtr<LowerFunction> teardownFor(DeclPtr<mir::Type> type, mir::StorageClass storage,
                                        LocationId source);
    LowerPtr<LowerGlobal> descriptorFor(DeclPtr<mir::Type> type, mir::StorageClass storage,
                                        LocationId source);

    LowerPtr<LowerFunction> thunkFor(U64 promise, DeclPtr<mir::Function> concrete,
                                     const mir::Occurrence* declared, Size count,
                                     const mir::Occurrence& declaredResult, LocationId source);

    LowerPtr<LowerGlobal> witnessFor(DeclPtr<mir::Class> klass, mir::GenWitness witness,
                                     LocationId source);

    /*
     * The one-cell table a **`Call`** requirement's slot points at - `CallThrough::CallWitness`,
     * Design-Iterator-Objects.md §4.
     *
     * Not `witnessFor` with a case in it, because the two are filled from different things: an
     * ordinary witness is an *instance*'s member list, and there is no instance here at all. What
     * goes in the cell is read off the subject - a site type's lifted function, or a shim over the
     * pair - and the promise it is built against is the constraint's own written signature.
     */
    LowerPtr<LowerGlobal> callWitnessFor(DeclPtr<mir::Type> declaredSubject,
                                         DeclPtr<mir::Type> declaredShape, mir::GenWitness witness,
                                         LocationId source);

    // The entry a `Call` witness holds - `callWitnessFor`, where the two subjects are argued.
    LowerPtr<LowerFunction> callThunkFor(DeclPtr<mir::Type> subject, DeclPtr<mir::Function> named,
                                         U64 mask, const mir::Occurrence* declared, Size count,
                                         const mir::Occurrence& declaredResult, LocationId source);

    U64 erasedSignatureMask(const mir::Occurrence* declared, Size count,
                            const mir::Occurrence& declaredResult);
    String callSubjectText(DeclPtr<mir::Type> subject, U64 mask);
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
    // which is four anchor-relative bytes rather than a pointer. See `kSlotBytes` above, and the
    // note in erase.cpp where the encoding is argued.
    LowerPtr<LowerValue> cellAddress(LowerBlock& block, LowerPtr<LowerValue> table, U32 offset);
    LowerPtr<LowerValue> wordAt(LowerBlock& block, LowerPtr<LowerValue> table, U32 offset,
                                LowerType type, U32 width);
    LowerPtr<LowerValue> cellPointer(LowerBlock& block, LowerPtr<LowerValue> table, U32 offset);

    LowerPtr<LowerValue> witnessMember(LowerBlock& block, LowerPtr<LowerValue> table, U16 member);

    /*
     * A field requirement the body reached through a **witness** - the one `PropertyForm` whose
     * value is copied out and has to be put back.
     *
     * Recorded per SSA value rather than expanded in place because the two halves are two
     * instructions: `field.addr` is where the subject and the slot are known, and the `store` that
     * may follow it is where there is something to commit. Design.md's tier 1 is exactly this pair.
     *
     * The other two are not here. An `Offset` slot is the field's own address, and a `Narrow` one is
     * a shift this body was handed - which is a `VirtualPlace`, written at the moment of the write
     * rather than copied and committed.
     */
    struct PropertyAccess {
        LowerPtr<LowerValue> owner = nullptr;
        LowerPtr<LowerValue> temporary = nullptr;
        U32 slot = 0;
        PropertyForm form = PropertyForm::Witness;
        mir::Occurrence held;
    };

    HashMap<U32, PropertyAccess> propertyAccesses;

    /*
     * pack.cpp - the components a record has that have **no address**.
     *
     * A field that shares a machine word with its neighbours and a discriminant folded into a
     * pattern its payload cannot produce are the same problem seen twice: `field.addr` produced a
     * value the tier above treats as an address, and there is nothing at that address to load. So
     * the projection records what the component really is and the `load` or `store` that follows
     * spends it - which is the same shape `PropertyAccess` above has, for the same reason: the two
     * halves are two instructions, and only the second one knows which direction it is going.
     *
     * Recorded per SSA value rather than expanded in place, which is also what makes the nested case
     * work without a temporary: `p.flags.a` projects out of a value that is itself packed, and the
     * inner projection composes its shift onto the outer one rather than dereferencing anything.
     */
    struct VirtualPlace {
        enum Kind: U8 {
            Packed, /// A field sharing a word with its neighbours - a shift and a mask.
            Folded, /// A discriminant that *is* a pattern of the payload's storage.
        };

        Kind kind = Packed;

        bool isBytewise() const { return kind == Packed && !wordBytes; }

        // The exact byte range used by the fallback. A normal packed place has one legal access
        // word; a bytewise place derives its range from the field's bits instead.
        U32 spanBytes() const {
            return isBytewise() ? (bitOffset + bitWidth + 7) / 8 : wordBytes;
        }

        // The storage the bits live in: the containing word for a packed field, and the payload
        // itself for a folded tag, whose niche offset is part of the record's encoding.
        LowerPtr<LowerValue> word = nullptr;

        // `Packed`: how wide the access word is (zero means exact byte-wise access), how far up it
        // the field sits, how many bits it owns, and whether its value reaches below zero - which
        // decides between a shift-and-mask and a pair of shifts that sign-extend.
        U32 wordBytes = 0;
        U32 bitOffset = 0;
        U32 bitWidth = 0;
        bool isSigned = false;

        /*
         * A shift this body was **handed** rather than one it knows, and the whole field's is this
         * plus `bitOffset` above.
         *
         * Two sources, and they are the same shape for the same reason: a body compiled once has a
         * subject whose layout it cannot see, so where a field sits comes out of a property slot -
         * and a `&` of a narrow value carries its shift alongside the address, so where the pointee
         * sits comes out of the reference. In both cases `bitOffset` is what the *body* knows on top
         * of that - `f.a` through a `&Flags` is the reference's shift plus `a`'s own offset inside a
         * `Flags`, which is a constant this body read out of its own layout table.
         *
         * Null is the ordinary case: a projection against a record this body can see, whose shift is
         * entirely `bitOffset`. Keeping the two apart rather than materializing a constant is what
         * lets the concrete case keep folding its masks.
         */
        LowerPtr<LowerValue> shift = nullptr;

        /*
         * `Folded`: the record whose `NicheEncoding` says which pattern means which constructor,
         * and the interior shape it is reached through.
         *
         * The shape is part of the key rather than a detail: `Maybe(a'Guard)` holds a reference
         * where `Maybe(Guard)` holds the payload, so one of them folds into an address's pattern
         * zero and the other stores a tag word. Reading the record's own answer for a shaped
         * occurrence is a tag decoded out of the wrong representation.
         */
        DeclPtr<mir::Type> record = nullptr;
        DeclPtr<mir::OwnershipSchema> shape = nullptr;
    };

    HashMap<U32, VirtualPlace> virtualPlaces;

    /*
     * A narrow reference **copied into storage of its own**, and the word it has to go back into.
     *
     * Design.md's tier 1, kept for the one boundary tier 2 cannot cross: a body compiled once
     * reaches a value of a type parameter through a plain address, so a reference carrying a shift
     * has to be flattened before it can be passed to one. `materializeNarrow` is the copy in and
     * `commitPackedBorrow` the copy out.
     *
     * Not a table keyed on the borrow, which is what this was when *every* narrow borrow was a
     * temporary. The commit belongs to the **call** - a write through a reference becomes visible
     * when the callee returns - so the list of them is the call's own and lives for as long as the
     * one instruction that owes them.
     */
    struct PackedBorrow {
        VirtualPlace place;
        LowerPtr<LowerValue> temporary = nullptr;
        U32 bytes = 0;
    };

    /*
     * The bits a co-packed component is written *from*, as a register.
     *
     * A packed field's type may still be an aggregate - a record this target scalarized - and such a
     * value is reached through storage, so the bits have to be read out of it before they can be
     * merged into the word. For everything else the value already is the bits. One function because
     * both writers ask it: the store through a projection, and the construction that fills a record.
     */
    LowerPtr<LowerValue> packedBits(LowerBlock& block, Value& value);

    /*
     * Design.md's tier 2 - a `&` of a narrow value, which is an address with the shift folded into
     * the bits above it, and the same reference taken back apart at the other end.
     *
     * `borrowNarrow` builds one at the borrow site, splitting the field's constant bit offset
     * against the unit the callee will load in; `unpackNarrowRef` is what every access through one
     * starts with. They are declared together because they are an encoding, and the mask that
     * separates the address from the shift has to be the same number in both.
     */
    LowerPtr<LowerValue> borrowNarrow(LowerBlock& block, Value& result, const VirtualPlace& place,
                                      LocationId source);
    LowerPtr<LowerValue> packNarrowRef(LowerBlock& block, LowerPtr<LowerValue> address, U32 shift);
    LowerPtr<LowerValue> packNarrowRef(LowerBlock& block, LowerPtr<LowerValue> address,
                                       LowerPtr<LowerValue> shift);
    LowerPtr<LowerValue> taggedAddress(LowerBlock& block, LowerPtr<LowerValue> address,
                                       LowerPtr<LowerValue> tag);
    bool seeNarrowRef(LowerPtr<LowerValue> reference, VirtualPlace& place);
    VirtualPlace unpackNarrowRef(LowerBlock& block, LowerPtr<LowerValue> reference,
                                 DeclPtr<mir::Type> pointee);

    // The flattening one, and the write-back that ends it - see `PackedBorrow`.
    PackedBorrow materializeNarrow(LowerBlock& block, LowerPtr<LowerValue> reference,
                                   const mir::Occurrence& held);
    void commitPackedBorrow(LowerBlock& block, const PackedBorrow& borrow);

    // One word read as another. Pointers and integers are distinct in the lower IR, so a reference
    // that carries a number in it has to say when it is being one.
    LowerPtr<LowerValue> reinterpret(LowerBlock& block, LowerPtr<LowerValue> value, LowerType type);

    // What an address operand names where it does not name storage - a projection this body made,
    // or a reference that carried one. See the definition.
    DeclPtr<mir::Type> narrowPointee(Value& value);
    Maybe<VirtualPlace> narrowPlace(LowerBlock& block, Value& value, LowerPtr<LowerValue> address);

    // ...and reading it, in the four shapes a result can want. Also see the definition.
    void spendPlace(LowerBlock& block, Value& result, const VirtualPlace& place, LocationId source);

    /*
     * The pair, and they are written next to each other on purpose: an encode that does not undo its
     * decode is the bug this file exists to make visible.
     *
     * `decodePacked` answers a register holding the field's value in normal form; `encodePacked`
     * reads the word, replaces the field's bits and writes it back. The load in the write is
     * deliberately at commit time rather than hoisted - Design.md's write-back rule is what stops
     * two co-packed fields written across one call from losing an update, and it holds because the
     * second write reads what the first one stored.
     */
    LowerPtr<LowerValue> decodePacked(LowerBlock& block, const VirtualPlace& place, LowerType type);
    void encodePacked(LowerBlock& block, const VirtualPlace& place, LowerPtr<LowerValue> value);
    LowerPtr<LowerValue> decodePackedBytes(LowerBlock& block, const VirtualPlace& place,
                                           LowerType type);
    void encodePackedBytes(LowerBlock& block, const VirtualPlace& place,
                           LowerPtr<LowerValue> value);

    // How far up the word the component starts - the constant this body knows plus the shift it was
    // handed. Shared by the pair above, because two answers to it is a write into the wrong bits.
    LowerPtr<LowerValue> placeShift(LowerBlock& block, const VirtualPlace& place);

    // Where a bit tag sits inside the record it belongs to - see the definition, which is where the
    // three readers of one are named.
    VirtualPlace discriminantPlace(LowerBlock& block, LowerPtr<LowerValue> address,
                                   const Repr& repr);

    // The other pair. Reading a folded tag is a range test over the payload's own bits and a select;
    // writing one is a store of a single impossible pattern, or - for the constructor that owns the
    // payload - nothing at all.
    LowerPtr<LowerValue> decodeFolded(LowerBlock& block, const VirtualPlace& place, LowerType type);
    void encodeFolded(LowerBlock& block, LowerPtr<LowerValue> payload, DeclPtr<mir::Type> record,
                      DeclPtr<mir::OwnershipSchema> shape, U16 constructor);

    /*
     * A packed component read out into storage of its own - the one case a decode cannot answer with
     * a register, because the field's type is an aggregate that this target scalarized.
     *
     * Read-only, and that is what makes it sound without a write-back: a write reaches a field
     * through the projection rather than through the value a load produced, so the copy this makes
     * is never the thing anything assigns to. A `&` of one *would* be, and is refused where the
     * projection is lowered.
     */
    LowerPtr<LowerValue> decodeIntoStorage(LowerBlock& block, const VirtualPlace& place,
                                           const mir::Occurrence& held);

    // Whether every user of a projection is one this stage can serve without the component having an
    // address - see the definition, which is where the refusal is reported.
    bool checkAddressable(Value& result, DeclPtr<mir::Type> record, U16 field, LocationId source);

    LowerPtr<LowerValue> readProperty(LowerBlock& block, LowerPtr<LowerValue> owner, U32 slot,
                                      const mir::Occurrence& held, PropertyForm form);

    // A narrow field requirement, as a place - the third source of a shift a body was handed, and
    // deliberately the same value the other two produce. See the definition.
    VirtualPlace propertyPlace(LowerBlock& block, LowerPtr<LowerValue> owner, U32 slot,
                               const mir::Occurrence& held);

    void commitProperty(LowerBlock& block, const PropertyAccess& access);

    LowerPtr<LowerValue> noEnvironment(LowerBlock& block);

    /*
     * width.cpp - the width seam.
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
    LowerPtr<LowerValue> narrowByteSwap(LowerBlock& block, LowerPtr<LowerValue> value,
                                        NarrowWidth width, StringId name);

    /*
     * fun.cpp - function values.
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
