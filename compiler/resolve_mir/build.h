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
    Place* fieldPlace(Place& parent, U16 field, Occurrence occurrence);

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

    // Storing into a place. `Destination::Assign` asserts the place is writable - see
    // `Place::writable` - because an IR that could express writing an immutable binding would be an
    // IR the loan checker has to have an opinion about.
    Node* addWrite(LocationId source, Place& place, Operand value, Destination mode);

    Node* addReturn(LocationId source, Operand value);
    Node* addReturnUnit(LocationId source);
    Node* addJump(LocationId source, Block& target, const Operand* arguments, Size count);
    Node* addBranch(LocationId source, Operand condition, Block& whenTrue, Block& whenFalse);
    Node* addUnreachable(LocationId source);

    /*
     * Finishing.
     *
     * Fills each block's predecessor list from the edges and marks the body sealed. Nothing derived
     * is written by the node-adding calls above: a builder that maintained both directions would be
     * two chances to disagree about the CFG, and the backward direction is only needed once the
     * forward one is complete.
     */
    void seal();

private:
    Block* current = nullptr;

    Node* append(Node* node);

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
