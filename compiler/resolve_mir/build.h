#pragma once

#include "decl.h"

namespace mir {

/*
 * The one way an MIR body is constructed - Analysis-IR.md §2.1.
 *
 * Every invariant a single append-only pass *can* decide is decided here, when the node is made,
 * rather than by a verifier that runs afterwards and by then has to guess what was meant. The list
 * is short on purpose: a mode is required wherever one is meaningful, a place is interned so two
 * spellings of one projection are one ID, a destination says whether it initializes or replaces, and
 * a block gets exactly one terminator. Everything that needs the finished CFG - where a `Forward`
 * resolves to, where a loan ends, whether a path initializes - is not attempted here and is not
 * silently assumed either.
 *
 * There is no editor and no rewriting. A sealed body is read by the checking passes and by the SSA
 * conversion and by nothing that mutates it, except liveness rewriting a `Forward` operand in place,
 * which is the one answer the builder deliberately left open.
 */
struct Builder {
    Builder(Context& context, Program& program, Function& function);

    Context& context;
    Program& program;
    Function& function;
    Body& body;

    MirArena& arena() { return program.mir; }
    MirBase base() { return *program.mir; }
    DeclBase declBase() { return *program.decl; }

    /*
     * Structure.
     */

    Block* addBlock(StringId name);
    Local* addLocal(StringId name, Occurrence occurrence, LocationId source, bool writable);

    /*
     * Continuation regions - `Continuation`, and the three calls its two ends need.
     *
     * `enterRegion` opens one and makes every block created afterwards part of it; the first of
     * those is its entry. `leaveRegion` goes back to the enclosing one. `rejoinRegion` says where a
     * region hands control back, and is separate from the other two because the block it names is
     * made *after* the region closes - a lens's continuation rejoins at the cleanup that follows the
     * `yield`, which is emitted next and in the enclosing region.
     */
    ContinuationId enterRegion(Continuation::Kind kind, Continuation::Multiplicity multiplicity);
    void leaveRegion();
    void rejoinRegion(ContinuationId region, Block& destination);

    ContinuationId region() const { return currentRegion; }

    /*
     * The storage one declared parameter names.
     *
     * A parameter is an ordinary local whose contents the *call* established, so it is created here
     * rather than by a `write.init` in the body - see `Local::parameter`. Taking the `Arg` rather
     * than its pieces is the point: the name, the occurrence and the position are one declaration,
     * and a builder that took three of them could be handed a set that does not belong together.
     *
     * Must be called for every parameter, in order, before anything reads one: `Function::args` and
     * the leading entries of `Body::locals` are the same list seen from two sides, and the SSA
     * conversion pairs them by index.
     */
    Local* addParameter(Arg& declared);

    // A block argument, as the `Argument` node it is. Occurrences rather than types: an owned merge
    // and a guaranteed one are the same shape and different obligations, and the successor is where
    // the obligation is declared.
    Node* addBlockArgument(Block& block, Occurrence occurrence, LocationId source);

    // Where subsequent nodes are appended. A block is finished when it is given a terminator; adding
    // to a finished block is a builder bug rather than a program error, and is asserted.
    void setBlock(Block& block);
    Block* currentBlock() { return current; }

    // Whether the current block already ends. The resolver reads it to stop emitting after a
    // `return` and to leave the jump off an arm that left by itself, which is a question about the
    // block rather than about the statement it just built.
    bool isTerminated();

    /*
     * Places. Interned: the same root and the same projection path are one ID, which is what makes
     * a prefix test the whole of an overlap test.
     */

    Place* localPlace(Local& local);
    Place* globalPlace(DeclPtr<Global> global);
    /*
     * One field of a record place.
     *
     * The occurrence is *derived* and is not a parameter: a field of an owned record is owned and a
     * field of a shared one is shared, and re-deriving that at every projection site is the walk
     * this representation exists to delete. Taking it from a caller would be the same rule written
     * once per caller, which is how two of them come to disagree - which is the whole of §1.2.
     */
    Place* fieldPlace(Place& parent, U16 field);

    /*
     * The object a place holding a reference names - Analysis-IR.md §5.2's `Deref` step.
     *
     * There is exactly one way to get one and it is `fieldPlace` above: a record field declared
     * `a'T` holds a loan rather than an object, so the storage the field *is* and the storage the
     * field *names* are two places and reaching the second is a load. Public because the checking
     * passes have to be able to tell the two apart, and not called by the resolver, which asks for a
     * field and gets whichever of the two the declaration says it meant.
     */
    Place* derefPlace(Place& parent, Occurrence occurrence, bool writable);

    /*
     * Constants. Interned per body, so the dense index a packed operand carries stays small.
     */

    ConstantId addConstant(Constant constant);

    /*
     * Operands. The only three ways to make one, and each names its mode.
     *
     * `resultOf` takes the producing node rather than an ID so that a caller cannot pass a number it
     * got from somewhere else, and asserts that the node actually produces a result.
     */

    Operand resultOf(Node& node, UseMode mode);
    Operand placeOperand(Place& place, UseMode mode);
    Operand constantOperand(ConstantId constant, UseMode mode);

    /*
     * Nodes.
     */

    Node* addConst(LocationId source, Constant constant);

    /*
     * A record built out of one value per field - §5.7.
     *
     * The components arrive as the *values* that fill them, exactly as a call's arguments do, so a
     * resolver states which field it is filling and never what it is doing to its own value: the
     * mode is `argumentMode`'s answer for an owned position, which is `Take` for a value in hand.
     * A field position is owned by definition - a record holds its members - so there is no contract
     * to read off the declaration and no way for one component to be handed over differently.
     *
     * The count and each component's type are the record's, and both are asserted here and restated
     * by `verifyBody`.
     */
    Node* addAggregate(LocationId source, DeclPtr<Type> record, Node* const* components, Size count);

    // Reading a place under a mode. The result's occurrence follows from the mode: `CopyTrivial` and
    // `Take` produce an owned value, a borrow produces a loan of the place's own origin. One rule in
    // one place, which is what stops a later pass from having a second opinion.
    Node* addRead(LocationId source, Place& place, UseMode mode);

    /*
     * A scalar operation. The result type follows from the operation and the operand type through
     * `binaryResult`, so a caller states what it wants computed and never what comes out.
     *
     * The operand types are asserted equal, and asserted legal for the operation's category: a
     * mixed-type or ill-typed operation is a resolver bug by the time it reaches here, because the
     * resolver is what has the source position to report it at.
     */
    Node* addBinary(LocationId source, BinaryOp op, Node& lhs, Node& rhs);
    Node* addUnary(LocationId source, UnaryOp op, Node& value);

    /*
     * A call, whose argument modes come from the callee's signature and from nowhere else - §5.6.
     *
     * The arguments arrive as the *values* that fill them rather than as operands, and that is the
     * whole design of this call: the mode is `argumentMode`'s answer, so a resolver cannot state one
     * and therefore cannot state a wrong one. When a milestone needs an argument that is a place -
     * a borrow of a name rather than of a value read out of it - this gains a source variant beside
     * the value one, not a mode parameter.
     *
     * The count, each argument's type and the result's occurrence are all the signature's, and all
     * three are asserted here and restated by `verifyBody`: a call that disagrees with its callee is
     * a resolver bug that would otherwise arrive as a wrong register at the machine boundary.
     */
    Node* addCall(LocationId source, Function& callee, Node* const* arguments, Size count);

    /*
     * Storing a value into a place.
     *
     * The value arrives as the node that produced it rather than as an operand, for the reason
     * `addCall` and `addAggregate` take theirs that way: the mode is `argumentMode`'s answer for the
     * position the storage is, so a resolver cannot state one and therefore cannot state a wrong one.
     * The position is owned for every write that fills storage somebody owns, and is the place's own
     * contract for the one write that does not - the `Init` that *establishes* a binding which is
     * itself a loan, where what is installed is the loan rather than a value.
     *
     * `Destination::Assign` asserts the place is writable - see `Place::writable` - because an IR
     * that could express writing an immutable binding would be an IR the loan checker has to have an
     * opinion about.
     */
    Node* addWrite(LocationId source, Place& place, Node& value, Destination mode);

    /*
     * Handing a value back, under the mode the *result declaration* asks for.
     *
     * The value rather than an operand, for the reason `addCall` takes its arguments that way: the
     * result is a declared position like any other, so `argumentMode` answers what filling it does
     * to the value and a resolver has no mode to state. An owned result takes it, a published loan
     * borrows it, and the two ends of the handover read one function.
     */
    Node* addReturn(LocationId source, Node& value);
    Node* addReturnUnit(LocationId source);
    Node* addJump(LocationId source, Block& target, const Operand* arguments, Size count);

    /*
     * `yield` - handing control to a continuation region, with what it hands over on the edge.
     *
     * The arguments arrive as operands rather than as values, unlike every other call above, and
     * that is the merge rule rather than an exception: what a region's entry parameter is filled
     * with is `argumentMode`'s answer for the *parameter*, exactly as a block argument on any other
     * edge is - see `closeArm`, which reads the same rule. The region must already be open at its
     * entry, which is asserted rather than assumed.
     */
    Node* addYield(LocationId source, ContinuationId region, Block& target,
                   const Operand* arguments, Size count);
    Node* addBranch(LocationId source, Operand condition, Block& whenTrue, Block& whenFalse);
    Node* addUnreachable(LocationId source);

    /*
     * Object lineage - `Object`, and the two rules that make it one.
     *
     * `mintObject` starts a lineage and `uniteObjects` merges two into one. Every producer of an
     * owner calls the first and every move calls the second, which is what "moving preserves a
     * lineage; copying starts a lineage" *is* when it is written down rather than described. They
     * are on the builder rather than in an analysis because a move is something the source said:
     * `UseMode::Take` is the statement, it is already decided by the time a node exists, and a pass
     * that worked it out again from the operands would be the second answer.
     */
    ObjectId mintObject(DeclPtr<Type> type, LocationId source);
    void uniteObjects(ObjectId a, ObjectId b);

    // The lineage a place's root local is part of, started if it has none yet. Null where the root
    // is not a local this frame owns - a global, an address - which owns no lineage here.
    ObjectId objectOfPlace(Place& place);

    /*
     * Finishing.
     *
     * Drops the blocks the entry cannot reach, fills each block's predecessor list from the edges,
     * and marks the body sealed. Both are derived from the same edges and neither is written by the
     * node-adding calls above: a builder that maintained the backward direction as it went would be
     * two chances to disagree about the CFG, and it is only needed once the forward one is complete.
     */
    void seal();

private:
    Block* current = nullptr;
    ContinuationId currentRegion;

    /*
     * The blocks nothing reaches, removed - and this is a rewrite, which is why it is here and not
     * a pass.
     *
     * Flattening a lens produces them as a matter of course: a continuation every path of which
     * leaves is a hand-over nothing resumes from, so the frame's resume and the caller's join are
     * both live code that has become unreachable at exactly one call site (see `resolveLensCall`).
     * The old resolver never built one, so nothing downstream is prepared for one either - the x64
     * block ordering asserts that its postorder covers the function - and an unreachable block is
     * also a hole in every dense per-block analysis, since a forward walk never assigns it a state.
     *
     * At the seal because that is where the CFG stops being appended to and starts being read, which
     * makes it the one point where "reachable" is both answerable and still cheap to act on. The
     * blocks are compacted rather than left as holes so that `BlockId` stays dense, and the regions'
     * `entry` and `destination` are renumbered with them; edges name blocks by pointer and need
     * nothing.
     */
    void pruneUnreachable();

    Node* append(Node* node);
    void fillEdge(Edge& edge, Block& target, const Operand* arguments, Size count);

    // `parent` is the projection's parent record, or null for a root; it is taken as a pointer
    // rather than a handle because the key below needs its dense ID and asking for both would be two
    // chances to hand over a mismatched pair.
    PlaceId internPlace(Place::Kind kind, Place* parent, U32 payload, Occurrence occurrence,
                        bool writable);

    /*
     * The interning key: the three fields that identify a place, in full and with nothing dropped.
     *
     * The parent is its **dense place ID** rather than its arena offset, and that is what makes the
     * three fit. An offset is 32 bits of a 4 GB region and a payload is 32 bits of a declaration
     * region, so a key that packed both had to truncate one - this key used to keep 24 bits of the
     * payload, which silently interned two distinct globals as one place past 16 MiB of
     * declarations, with no assertion to say so. A place ID is dense in one body, so 29 bits of it
     * is a bound no program reaches and `internPlace` asserts rather than assumes.
     *
     * The kind occupies the top three bits and never reaches seven, so the key is never all ones -
     * which is `HashMap`'s empty-key sentinel and would otherwise make one place uninsertable.
     */
    static constexpr U32 kParentBits = 29;
    static constexpr U32 kMaxParentId = (1u << kParentBits) - 2;

    static U64 placeKey(Place::Kind kind, PlaceId parent, U32 payload) {
        auto encoded = U64(parent.valid() ? parent.index + 1 : 0);
        return (U64(kind) << 61) | (encoded << 32) | U64(payload);
    }

    // Function-local and thrown away with the builder; the interned records themselves are the
    // body's.
    HashMap<U64, U32> placeIds;
};

} // namespace mir
