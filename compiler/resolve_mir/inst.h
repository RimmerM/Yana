#pragma once

#include "place.h"
#include "op.h"

namespace mir {

struct Block;
struct Body;
struct Function;

/*
 * How one node uses one input - Analysis-IR.md §5.3.
 *
 * This is the field the whole representation turns on. Every ownership-sensitive input carries one
 * when it is built, so no later pass has to work out what the source meant: `Take` is a transfer
 * wherever it appears, and an ordinary read cannot be mistaken for one because a mode is not
 * optional.
 *
 *  - `Inspect` reads without creating an owner or a loan - arithmetic on a value already in hand.
 *  - `BorrowShared` is a plain binding or parameter: read access for the loan's extent.
 *  - `BorrowUnique` is `&`: exclusive write access.
 *  - `Take` is `->`: the ownership lineage moves to this use.
 *  - `CopyTrivial` creates a second trivial owner, and requires `TrivialCopy` evidence. Distinct
 *    from an authored `copy(x)`, which is a call and stays observable (§5.8).
 *  - `Forward` is the unresolved last-use candidate: a bare owned name in sink position, which the
 *    language moves when it is dead afterwards and borrows when it is not. A builder walking one
 *    block at a time cannot decide that (§5.4), so it records the question. Liveness answers it, and
 *    no `Forward` survives into SSA.
 */
enum class UseMode: U8 {
    Inspect,
    BorrowShared,
    BorrowUnique,
    Take,
    CopyTrivial,
    Forward,

    Count,
};

/// Where an operand's value comes from. Distinct from `UseMode`: the mode says what is done to the
/// source, this says what the source is.
enum class SourceKind: U8 {
    Result,    // another node's result, by node ID
    Place,     // a place, by place ID
    Constant,  // an entry in the body's constant table, by index
};

/*
 * One ownership-bearing input: a source and what is done to it, in four bytes - §8.3.
 *
 * Packed rather than a struct of two fields because operands are the most numerous thing in the
 * representation, and a byte beside a four-byte handle rounds the pair to eight. The high bit is
 * left clear so that a list of these can use `SmallList`'s embedded encoding, which marks a present
 * entry by setting exactly that bit.
 *
 * There is no constructor from a bare ID. That is deliberate and is the invariant §2.1 asks for: a
 * raw handle must not be accepted where an ownership-bearing operand is required, and the way to
 * enforce it is for the only way to make one to name a mode.
 */
struct Operand {
    static constexpr U32 kModeBits = 3;
    static constexpr U32 kSourceBits = 2;
    static constexpr U32 kIdBits = 26;
    static constexpr U32 kMaxId = (1u << kIdBits) - 1;

    Operand() = default;

    Operand(SourceKind source, U32 id, UseMode mode) {
        assertTrue(id <= kMaxId);
        assertTrue(U32(mode) < U32(UseMode::Count));

        packed = U32(mode) | (U32(source) << kModeBits) | (id << (kModeBits + kSourceBits));
    }

    UseMode mode() const { return UseMode(packed & ((1u << kModeBits) - 1)); }
    SourceKind source() const { return SourceKind((packed >> kModeBits) & ((1u << kSourceBits) - 1)); }
    U32 id() const { return packed >> (kModeBits + kSourceBits); }

    /*
     * The identity, in the domain the source kind says it is in.
     *
     * Three accessors rather than one `id()` everywhere, so that a caller which meant "the node this
     * names" cannot silently be handed a place index by an operand whose source is something else.
     * Each asserts its own source kind; `id()` is still there for the walks that are genuinely
     * generic over the three, such as printing and the existence check.
     */
    NodeId nodeId() const { assertTrue(source() == SourceKind::Result); return NodeId(id()); }
    PlaceId placeId() const { assertTrue(source() == SourceKind::Place); return PlaceId(id()); }
    ConstantId constantId() const {
        assertTrue(source() == SourceKind::Constant);
        return ConstantId(id());
    }

    // The same source under a different mode - what liveness does when it resolves a `Forward`.
    Operand withMode(UseMode mode) const { return Operand(source(), id(), mode); }

    U32 packed = 0;
};

static_assert(sizeof(Operand) == 4, "an Operand no longer fits a SmallList element");

/*
 * A list of operands - one edge's block arguments, and one call's arguments when calls arrive.
 *
 * Without `SmallList`'s embedded encoding, deliberately, even though `Operand` leaves its high bit
 * clear so that the encoding would work. The encoding marks a present entry by setting that bit *in
 * the element*, so an embedded entry is not what the list holds and cannot be handed out by
 * reference - which is exactly what resolving a `Forward` in an edge argument would want to do.
 * A list that is read by value and written through `set` has no such trap; §13.2 lists the operand
 * encoding as a choice to settle by measurement, and this is the safe end of it until then.
 */
using OperandList = MirList<Operand, false>;

/*
 * What one operand *is to its node* - Analysis-IR.md §2.1.
 *
 * The generic walk exposes the operands; without this it does not expose what any of them means, so
 * a structural verifier can check that a source exists and nothing else. "A read names a place", "a
 * write's destination is a writable place held uniquely", "the value and the destination agree about
 * type" are all statements about a *role*, and a walk with no roles cannot make one - which is
 * exactly the gap that stops the mutation testing in §12.3 from finding anything.
 *
 * Declared beside the fields on each node struct, like the walk itself, so that a kind cannot
 * acquire an operand without saying what it is for.
 */
enum class OperandRole: U8 {
    Value,        // an ordinary input, read for its value
    ReadSource,   // the place a `read` turns into a value
    WriteTarget,  // the place a `write` stores into
    Stored,       // the value a `write` installs
    Condition,    // the value a `branch` tests
    Selector,     // the value a `map` or a `switch` chooses by
    Returned,     // the value a `return` hands back
    EdgeArgument, // an operand supplied to a successor's parameter, on an outgoing edge
    Argument,     // an operand supplied to a callee's parameter, on a call
    Callee,       // the function value a call goes through, where it is not a declaration
    Component,    // an operand installed in one field of an aggregate
};

StringView operandRoleName(OperandRole role);

/*
 * What a pass may ask about a node *kind* - see inst.def, where each is decided per row.
 *
 * Two, and deliberately not three. There used to be a `kMirSuspends` here, and it was wrong in a way
 * worth recording: whether a call may suspend is a property of the *callee's published summary*
 * (Analysis-Sharing.md §3.3), not of the fact that the node is a call. A kind-level bit can only
 * ever answer "every call suspends" or "no call does", and both are wrong, so the question is asked
 * of `Summary::suspension` through the selected callee - see `maySuspend` on the call node when
 * calls arrive.
 */
static constexpr U16 kMirResult = 1;
static constexpr U16 kMirTerminator = 2;

/*
 * One MIR node.
 *
 * Append-only. A body is built, sealed and verified, then read by the checking passes and the SSA
 * conversion, and nothing rewrites it - which is why there is no use list, no block backpointer and
 * no editor here. §8.5 and §8.6 are the two halves of that: analysis state lives in dense side
 * tables indexed by `id`, and a field one pass reads does not enlarge every node for the whole
 * compilation.
 *
 * **There is no occurrence here.** It used to be on this base, which put twelve bytes of result type
 * and contract on every store, every jump, every branch and every return - none of which produce a
 * value at all. §8.3's exact-payload rule is what that violated, and the fix is the `NodeResult`
 * layer below: a kind with `kMirResult` derives from it and a kind without does not, so the field
 * exists exactly where it means something and `hasResult` is what licenses reaching for it.
 *
 * `id` stays, and is *not* the duplicate of the dense node table it looks like. The table maps an ID
 * to a node; this is the other direction, which every operand construction and every side-table
 * write needs from a node in hand, and recovering it by searching the table would turn a constant
 * into a scan.
 */
struct Node {
    enum Kind: U8 {
#define YANA_MIR_INST(kind, Struct, mnemonic, flags) kind,
#include "inst.def"
#undef YANA_MIR_INST
        KindCount,
    };

    Node(Kind kind, LocationId source): source(source), kind(kind) {}

    // The dense function-local ID. Assigned by the builder in creation order; it is what an operand
    // names, what a side table is indexed by, and what a dump prints.
    NodeId id;

    LocationId source = kNullLocation;
    Kind kind;
};

/*
 * A node that defines a value - the base of exactly the kinds inst.def marks `kMirResult`.
 *
 * The occurrence is all three of what the result is: its logical type, the contract this occurrence
 * holds it under, and the interned shape of what is borrowed inside it. Not just a type, and not
 * just a type and an access: a node producing a shared window into a sequence and one producing the
 * owned sequence agree on the first and differ on the other two, and a tier that recorded only the
 * type would have to re-derive the difference at every consumer.
 */
struct NodeResult: Node {
    NodeResult(Kind kind, LocationId source): Node(kind, source) {}

    Occurrence occurrence;
};

/*
 * A block parameter - §5.9.
 *
 * No operands: what it holds arrives on the predecessors' edges, and each predecessor says there
 * what it did to hand it over. That is the whole reason for block arguments over phis, so the
 * absence of an operand list here is the representation making its point rather than an omission.
 */
struct NodeArgument: NodeResult {
    NodeArgument(LocationId source, U32 index): NodeResult(Argument, source), index(index) {}

    // Position in the owning block's parameter list, which is what an edge's argument list is
    // matched against.
    U32 index;

    template<class F> void eachOperand(MirBase, F&&) {}
    template<class F> void eachEdge(MirBase, F&&) {}
};

// A literal named by a body: an index into `Body::constants`.
struct NodeConst: NodeResult {
    NodeConst(LocationId source, ConstantId constant):
        NodeResult(Const, source), constant(constant) {}

    ConstantId constant;

    template<class F> void eachOperand(MirBase, F&&) {}
    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * One atomic operation - inst.def's `Atomic` row, and op.def's atomic block for the rows.
 *
 * The operands are the shape `AtomicForm` states, positionally, with the location first. Their modes
 * are `Inspect` throughout but one: `IntoValue` takes the atomic over, which is the declaration's
 * `->self` and is what makes owning it the proof that nobody is sharing it.
 */
struct NodeAtomic: NodeResult {
    NodeAtomic(LocationId source, AtomicOp op, AtomicOrder order):
        NodeResult(Atomic, source), op(op), order(order), failure(order) {}

    AtomicOp op;

    // What the operation performs; for a compare-exchange, what the *successful* one performs.
    AtomicOrder order;

    /*
     * And what a failed comparison performs - §3.5.
     *
     * Equal to `order` for everything else, so that no reader has to ask which row it is holding
     * before reading it. The library's plain form derives it through `failureOrderFor` and
     * `Advanced.compareExchange` states it.
     */
    AtomicOrder failure;

    // Whether a spurious failure is permitted - `compareExchangeWeak`. Meaningful on `Compare` and
    // false everywhere else.
    bool weak = false;

    OperandList arguments;

    // By reference, for `NodeNative::eachOperand`'s reason.
    template<class F> void eachOperand(MirBase base, F&& f) {
        auto items = arguments.contents(base);
        for(Size i = 0; i < items.size(); i++) f(*items.pointerAt(i), OperandRole::Argument);
    }

    template<class F> void eachEdge(MirBase, F&&) {}
};

// `a * b + c` at most once rounded - inst.def's `Fma` row, where the argument for a node is. Every
// operand is the result's type and is `Inspect`, a lane being a machine primitive.
struct NodeFma: NodeResult {
    NodeFma(LocationId source, Operand lhs, Operand rhs, Operand addend):
        NodeResult(Fma, source), lhs(lhs), rhs(rhs), addend(addend) {}

    Operand lhs, rhs, addend;

    template<class F> void eachOperand(MirBase, F&& f) {
        f(lhs, OperandRole::Argument);
        f(rhs, OperandRole::Argument);
        f(addend, OperandRole::Argument);
    }

    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * A branch-free choice - inst.def's `Select` row, where the argument for it being a node is.
 *
 * The condition is a `Bool` or a mask of the arms' shape, and the two arms are the result's type.
 * Every operand is `Inspect`: what reaches this is a value already in hand, and both arms are
 * computed whichever is chosen.
 */
struct NodeSelect: NodeResult {
    NodeSelect(LocationId source, Operand condition, Operand whenTrue, Operand whenFalse):
        NodeResult(Select, source), condition(condition), whenTrue(whenTrue), whenFalse(whenFalse) {}

    Operand condition, whenTrue, whenFalse;

    template<class F> void eachOperand(MirBase, F&& f) {
        f(condition, OperandRole::Argument);
        f(whenTrue, OperandRole::Argument);
        f(whenFalse, OperandRole::Argument);
    }

    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * One entry of a `map` - the number the pivot carries for this arm, and what the arm answers.
 *
 * `selector` is `I64` and not the tag's own width, because `@value(n)` may pin a constructor outside
 * thirty-two bits and the numbers an enumeration is pinned to are somebody else's ABI. Whether every
 * entry is representable at the selector's declared type is a verifier rule rather than a
 * representation choice, so a truncation is reported rather than performed.
 */
struct MapEntry {
    I64 selector = 0;
    Constant value;
};

/*
 * A `match` over constants, as the mapping it states - inst.def's `Map` row, where the argument for
 * a node is.
 *
 * The selector is `Inspect`: the pivot's discriminant is a number already read, and a table of
 * literals reads nothing else. The entries are ascending by `selector` and pairwise distinct, which
 * is what lets every form below - a range test, a bit mask, an affine step, a packed word, a placed
 * table - be derived by walking them once.
 */
struct NodeMap: NodeResult {
    NodeMap(LocationId source, Operand selector, bool closed):
        NodeResult(Map, source), selector(selector), closed(closed) {}

    Operand selector;

    // Non-embedding for `OperandList`'s reason and one more: a `MapEntry` is twenty-four bytes, and
    // `SmallList`'s embedded encoding tags a bit inside the element.
    MirList<MapEntry, false> entries;

    // What a selector outside the listed ones answers, and set exactly when the domain is open. A
    // record's own constructors are the whole domain of a match over them, so the overwhelmingly
    // common case carries nothing here at all.
    Constant fallback;

    // Whether the listed selectors *are* the domain - see inst.def.
    bool closed = false;

    template<class F> void eachOperand(MirBase, F&& f) { f(selector, OperandRole::Selector); }
    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * A scalar in every lane - inst.def's `VecSplat` row.
 *
 * The operand is the lane's own type, converted where it had to be: `splat(1) :: Vec(I16)` is an
 * `I16` in every lane and the emitter is what puts the literal at that type. `Inspect`, like every
 * other vector operand: a lane is a machine primitive, so a vector's operands are values already in
 * hand and none of them owes anything.
 */
struct NodeVecSplat: NodeResult {
    NodeVecSplat(LocationId source, Operand from): NodeResult(VecSplat, source), from(from) {}

    Operand from;

    template<class F> void eachOperand(MirBase, F&& f) { f(from, OperandRole::Argument); }
    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * One lane read, and one lane written - inst.def's `VecLane` and `VecWithLane`, which share this.
 *
 * `value` is set for the second and null for the first, which is the same arrangement the lower IR's
 * pair has and for the same reason: the write is the read plus what to write, and a struct per kind
 * would be a field list copied.
 *
 * The **index is a field**, checked constant and in range where the call was written - see inst.def.
 */
struct NodeVecLane: NodeResult {
    NodeVecLane(Kind kind, LocationId source, Operand from, U16 lane):
        NodeResult(kind, source), from(from), lane(lane) {}

    Operand from;

    // Set only for `VecWithLane`. `Operand`'s default is a source of `Result` and an id of zero,
    // which is a real operand - so `writesLane` reads the *kind* rather than testing this.
    Operand value;

    U16 lane;

    bool writesLane() const { return kind == VecWithLane; }

    template<class F> void eachOperand(MirBase, F&& f) {
        f(from, OperandRole::Argument);
        if(writesLane()) f(value, OperandRole::Argument);
    }

    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * Lanes selected from two vectors by a constant pattern - inst.def's `VecShuffle` row.
 *
 * The pattern is a list of lane numbers, one per lane of the result, in the MIR region beside the
 * node. A list rather than a fixed field because it is as long as the result has lanes, which is up
 * to the widest register any target has.
 */
struct NodeVecShuffle: NodeResult {
    NodeVecShuffle(LocationId source, Operand left, Operand right):
        NodeResult(VecShuffle, source), left(left), right(right) {}

    Operand left, right;

    // One entry per lane of the result, each in `[0, 2 * sources)` - see inst.def, where what a
    // number past the first vector's lanes means is stated.
    MirList<U8, false> pattern;

    template<class F> void eachOperand(MirBase, F&& f) {
        f(left, OperandRole::Argument);
        f(right, OperandRole::Argument);
    }

    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * The lane numbers `0, 1, ... n - 1` of a vector this body cannot count - inst.def's `VecIota` row.
 *
 * No operands and no fields: the result type is the whole of it, and a substitution is what turns
 * the count into a number.
 */
struct NodeVecIota: NodeResult {
    explicit NodeVecIota(LocationId source): NodeResult(VecIota, source) {}

    template<class F> void eachOperand(MirBase, F&&) {}
    template<class F> void eachEdge(MirBase, F&&) {}
};

// Every lane folded into one scalar - inst.def's `VecReduce` row, and op.def's reduction block for
// which folds there are and what order each runs in.
struct NodeVecReduce: NodeResult {
    NodeVecReduce(LocationId source, Operand from, ReduceOp reduce):
        NodeResult(VecReduce, source), from(from), reduce(reduce) {}

    Operand from;
    ReduceOp reduce;

    template<class F> void eachOperand(MirBase, F&& f) { f(from, OperandRole::Argument); }
    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * The second answer of a compare-exchange - inst.def's `AtomicOk` row, where the argument is.
 *
 * `from` names the `Atomic` node it projects and is read `Inspect`: a projection of an answer
 * neither consumes it nor borrows anything, which is the same thing every other reader of a
 * computed value does.
 */
struct NodeAtomicOk: NodeResult {
    NodeAtomicOk(LocationId source): NodeResult(AtomicOk, source) {}

    Operand from;

    template<class F> void eachOperand(MirBase, F&& f) { f(from, OperandRole::Argument); }
    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * The value of one const parameter - `VariableType::constType`, read in an expression.
 *
 * It names the parameter rather than a number, because the body is resolved once and the number is
 * what differs per instantiation. That is the same sentence `NodeCall` makes about a callee and a
 * property place makes about a field index: **a body names a requirement, never a number**, and the
 * point at which the requirement becomes an answer is the point at which the substitution is known.
 */
struct NodeGenValue: NodeResult {
    NodeGenValue(LocationId source, DeclPtr<Type> variable):
        NodeResult(GenValue, source), variable(variable) {}

    // The `VariableType` of the enclosing signature's context, which `isConst()` answers true for.
    DeclPtr<Type> variable;

    template<class F> void eachOperand(MirBase, F&&) {}
    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * Reading a place - the one node that turns storage into a value.
 *
 * The mode on `place` is the whole of what this node means. `CopyTrivial` produces a second trivial
 * owner; `BorrowShared` produces a loan whose extent the checker decides; `Take` moves the object
 * out and leaves the place uninitialized. There is no representation-dependent middle case: the
 * current resolver's `LoadPlace` is a scalar load for one representation and a view of storage for
 * another, and telling those apart afterwards is §1.2's failure.
 */
struct NodeRead: NodeResult {
    NodeRead(LocationId source, Operand place): NodeResult(Read, source), place(place) {}

    Operand place;

    /*
     * The loan this read took, or none where it took none.
     *
     * A field rather than a table the analysis rebuilds, because the relation is one-to-one and the
     * builder is what knows it: a borrowing read *is* a loan, and a copy or a move is not. Four
     * bytes on the one node kind that can carry one, which is §8.3's exact-payload rule - a store or
     * a terminator has no loan field to be null.
     */
    LoanId loan;

    /*
     * That this read deliberately takes up to a vector's width past the extent of what the place
     * names - Design-Vector §5.4, Implementation-Vector.md §3.3.
     *
     * A flag on the read rather than a node of its own, because what it changes is not what the
     * operation *is*: `vectorPast(p)` reads a whole vector at an address exactly as `vectorAt(p)`
     * does, and the difference is a promise about the bytes at the far end. Nothing between here
     * and the machine has to branch on it - it travels to `LowerInstLoad::isOverread`, where the
     * three passes that must not treat "reads past the end" as a bug read it.
     *
     * `Native.vectorPast` is the only thing in the language that sets it, which is what keeps the
     * reasoning about when the tail-read guarantee may be spent in one place. No write ever carries
     * it: a store past the end of an object is not a read of unspecified bytes but the destruction
     * of whatever follows.
     */
    bool overread = false;

    template<class F> void eachOperand(MirBase, F&& f) { f(place, OperandRole::ReadSource); }
    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * A scalar operation over two values - see op.def.
 *
 * Both operands are `Inspect`: arithmetic reads what is already in hand and creates neither an owner
 * nor a loan (§5.3). The result is a fresh owned value of the type `binaryResult` decides, and a
 * type that may not stand here was refused by the resolver before the node was built.
 */
struct NodeBinary: NodeResult {
    NodeBinary(LocationId source, BinaryOp op, Operand lhs, Operand rhs):
        NodeResult(Binary, source), lhs(lhs), rhs(rhs), op(op) {}

    Operand lhs, rhs;
    BinaryOp op;

    template<class F> void eachOperand(MirBase, F&& f) {
        f(lhs, OperandRole::Value);
        f(rhs, OperandRole::Value);
    }

    template<class F> void eachEdge(MirBase, F&&) {}
};

struct NodeUnary: NodeResult {
    NodeUnary(LocationId source, UnaryOp op, Operand value):
        NodeResult(Unary, source), value(value), op(op) {}

    Operand value;
    UnaryOp op;

    template<class F> void eachOperand(MirBase, F&& f) { f(value, OperandRole::Value); }
    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * Building a record out of one operand per field - Analysis-IR.md §5.7.
 *
 * **Every component is an ordinary `Operand`**, which is the whole of what this row is for. The
 * current resolver has an aggregate-only handover walk that decides per component whether the value
 * moved, and it is a second answer to a question the generic operand schema already asks: here the
 * ownership checker, the verifier and §12.3's mutation testing all see the same `Take` or
 * `CopyTrivial` that every other position carries, because there is nowhere else for one to be.
 *
 * One operand per declared field and in field order, so the component list and the record's fields
 * are one list read from two sides. A field left out is not an under-specified aggregate that a
 * later pass completes - it is refused, because a record whose construction was partial is a value
 * whose initialization dataflow has to have a state for, and `Destination::Init` on a whole place is
 * the statement this node makes.
 *
 * There is deliberately no *update* form yet. `{v | x: 1}` is one base operand plus the replaced
 * components, and §5.7 says the components are the same operands - what it needs beyond this is the
 * rule for what happens to the base's other fields, which is a partial move and is the move-path
 * machinery rather than a second node.
 */
struct NodeAggregate: NodeResult {
    NodeAggregate(LocationId source): NodeResult(Aggregate, source) {}

    OperandList components;

    /*
     * Which constructor is being built - zero for every record with one, and the arm for a sum.
     *
     * On the node rather than derived from the component count, because two arms may have the same
     * number of fields and because a nullary constructor has none at all: what a construction picks
     * is a *name*, and the number it becomes is the one thing the lowering needs to write the
     * discriminant. The components are that constructor's fields in its own order, which is the same
     * statement the single-constructor case has always made.
     */
    U16 constructor = 0;

    /*
     * One component filling **every** slot of a fixed array whose count is a const parameter -
     * `[0] :: [U8 *n]`, and only that.
     *
     * A written number is spread by the resolver, so this bit is never set where there is one: the
     * component list is the array's elements everywhere else, which is what keeps every consumer
     * reading one shape. What it is for is the case where the number does not exist yet - the SSA
     * conversion is the first point that can count the slots, and it repeats the one operand there.
     */
    bool fill = false;

    // By reference, for the reason `NodeCall::eachOperand` gives: a `Forward` in a component is
    // rewritten in place by liveness, and a masked list has no address to rewrite through.
    template<class F> void eachOperand(MirBase base, F&& f) {
        auto items = components.contents(base);
        for(Size i = 0; i < items.size(); i++) f(*items.pointerAt(i), OperandRole::Component);
    }

    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * A named function as a value - inst.def's `FunValue` row.
 *
 * The declaration and nothing else. Which *code* stands behind it is a lowering question - a plain
 * function's convention does not match the one a function value is called across, so what the pair's
 * first word points at is a thunk - and a node that named the thunk would be naming something no
 * stage above the lowering has.
 *
 * `occurrence` is the function type built from the declaration's own positions, so the node's result
 * type is exactly what a position declared `(I32) -> I32` accepts. `funTypeOf` is the one function
 * that builds it, for the reason every "one decision in one place" note here gives: a signature read
 * as a type at two sites is two readings that eventually disagree about a convention.
 */
struct NodeFunValue: NodeResult {
    NodeFunValue(LocationId source, DeclPtr<Function> callee):
        NodeResult(FunValue, source), callee(callee) {}

    DeclPtr<Function> callee;

    /*
     * The borrow of the storage this value's captures live in, where it has any.
     *
     * A **shared loan of a local of this frame**, and that is the whole of what makes a closure
     * checkable: the environment is ordinary storage, the value refers to it, and every rule about
     * a value that refers to somebody's storage already exists. The pair the lowering builds holds
     * that address in its second word, so what the loan says here and what the machine does are one
     * statement rather than two that have to be kept in step.
     *
     * `capturing` rather than a null `Operand`, because a default-constructed operand decodes as
     * node zero - which is a real node. `NodeReturn::hasValue` is the same flag for the same reason.
     */
    Operand environment;
    bool capturing = false;

    template<class F> void eachOperand(MirBase, F&& f) {
        if(capturing) f(environment, OperandRole::Value);
    }

    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * The address of the storage a loan names - see inst.def.
 *
 * One operand, `Inspect`: this reshapes a borrow the way `Window` does, so the loan that keeps the
 * storage live is the `read`'s and this node takes none of its own.
 */
struct NodeAddress: NodeResult {
    NodeAddress(LocationId source, Operand of): NodeResult(Address, source), of(of) {}

    Operand of;

    /*
     * Whether this address is a `swap` or `exchange` relocating through it - `expand_exchange.cpp`.
     *
     * The loan checker's one exemption reads it: two exclusive loans of elements of one container
     * may be live at once when each is used by nothing but a relocation that reads both places
     * before it writes either. That is a property of the *use* and not of the loan, so it is
     * recorded on the node the use reaches the storage through, where `exchangeOnlyLoan` finds it.
     * Printed as `address.exchange`.
     */
    bool exchange = false;

    template<class F> void eachOperand(MirBase, F&& f) { f(of, OperandRole::Value); }
    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * One measurement of one type - see inst.def and metric.def.
 *
 * No operands at all, which is what separates it from every other `kMirResult` row: the question is
 * about a *type* and not about a value, so there is nothing here for an ownership mode to be about.
 * That is also why a pointer's arithmetic cannot simply project an element place - a place is
 * storage somebody has, and a stride is a number about a type nothing may hold a value of.
 */
struct NodeMetric: NodeResult {
    NodeMetric(LocationId source, TypeMetric metric, DeclPtr<Type> of):
        NodeResult(Metric, source), of(of), metric(metric) {}

    // The type being measured, which is never the result's - the result is a number or a `Bool`.
    DeclPtr<Type> of;
    TypeMetric metric;

    template<class F> void eachOperand(MirBase, F&&) {}
    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * An operation the machine has and this IR does not - see inst.def and native.def.
 *
 * A flat operand list and a column, which is the whole structure: what these have in common is that
 * their meaning belongs to the backend, so the only thing this tier can say about one is which it is
 * and what it was given.
 */
struct NodeNative: NodeResult {
    NodeNative(LocationId source, NativeOp op): NodeResult(Native, source), op(op) {}

    NativeOp op;
    OperandList arguments;

    // By reference, for the reason `NodeCall::eachOperand` gives: a `Forward` in an argument is
    // rewritten in place by liveness, and a masked list has no address to rewrite through.
    template<class F> void eachOperand(MirBase base, F&& f) {
        auto items = arguments.contents(base);
        for(Size i = 0; i < items.size(); i++) f(*items.pointerAt(i), OperandRole::Argument);
    }

    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * A call to a function this program declares - Analysis-IR.md §5.6.
 *
 * The callee is a **declaration-region** `Function` and never another body's node, which is the rule
 * the two body arenas depend on (§8.1): the MIR region is released whole after conversion, and a
 * call that named something inside it would have to be rewritten first.
 *
 * The arguments are ordinary operands, one per declared parameter and in parameter order. Their
 * modes are not the caller's choice - `argumentMode` reads them off the selected signature, once,
 * and that is what makes a direct, class, dynamic or specialized call one local representation
 * rather than four. There is no separate handover walk and no convention table consulted later: what
 * the callee receives each argument as is in the node.
 *
 * **A call through a value is this kind with a different target, not a kind of its own** - G7. What
 * differs is where the callee comes from, which is the question `mir::CallThrough` already answers,
 * and the argument list, the result and the summary question are unchanged. So the value is an
 * *operand* with a mode of its own (`target`) and `callee` is null, rather than one field that is
 * sometimes a declaration and sometimes an operand - which would be exactly the
 * two-meanings-in-one-slot problem this representation exists to remove. Its summary facts join over
 * a flow set rather than naming one declaration; see Analysis-Sharing.md §3.5.
 *
 * There is also deliberately **no destination field yet**. §5.6 says a call may initialize a
 * destination directly rather than manufacture an owner and move it, and that is worth having - a
 * function value is the first address-only result this stage produces, so the case is reachable
 * rather than hypothetical now. The place it attaches is here, beside the arguments, and the
 * decision that fills it is the placement analysis rather than the resolver.
 */
struct NodeCall: NodeResult {
    NodeCall(LocationId source, DeclPtr<Function> callee):
        NodeResult(Call, source), callee(callee) {}

    DeclPtr<Function> callee;
    OperandList arguments;

    /*
     * Two calls the compiler made rather than the program: `xs[i]`, which is the `Index` member
     * `elementAccess` reaches for a subscript, and the derivation `adaptToView` calls to turn an
     * owner into the view a position holds. Printed as `call.subscript` and `call.derivation`.
     *
     * Two bits, because one rule reads them: `doc/spec/core.md`'s exemption for `swap(xs[i], xs[j])`
     * is about *elements*, and on a window an element is what the accessor hands back rather than a
     * projection the place walk can see. `exchangeOnlyLoan` follows the container's loan through
     * these two calls - the derivation is a path from `self` and nothing else, Manifestations §5.4,
     * and the accessor is where the loan becomes an element's - and through no other call.
     */
    bool subscript = false;
    bool derivation = false;

    /*
     * The callee's type arguments, in the **enclosing body's** terms - `gen.h`.
     *
     * Empty for a call to a plain function, which allocates nothing at the overwhelmingly common
     * call. For a generic callee it is one entry per variable of the callee's context, types of the
     * *caller's* world: concrete in an ordinary body, and possibly the caller's own variables in a
     * generic one, which is what `middle` calling `smaller` writes down.
     *
     * The node names the generic callee and not a specialization, deliberately. A specialization is
     * a `Function` with a bound signature, and which one this call reaches depends on the
     * substitution the *enclosing* body is being compiled under - so a node that named one would be
     * a node with one answer where the body has several. `callTarget` is the one function that turns
     * the pair into a callee, and reachability, summaries and conversion all ask it rather than each
     * other.
     */
    MirList<DeclPtr<Type>> typeArguments;

    /*
     * `f: (a) -> b` - which callable requirement of the *enclosing* context typed this call, and
     * `kNoCallable` for every ordinary one.
     *
     * A body may call something whose declared signature it does not match: `fn (grow: (a) -> a)`
     * types `grow(x)` against the promise rather than against whatever `grow` happens to be
     * declared as, which is the whole of what a function witness is for. So the node records which
     * promise it was typed against, and `callTarget` re-derives the callee from the substituted
     * signature - by the same name-and-arity rule the call site used, at the argument list the
     * specialization supplies.
     *
     * `callee` is still set, and to the declaration that name reached. One plain function per name
     * and arity means there is exactly one, so recording it costs nothing and every reader that
     * only wants "which declaration is this about" keeps working unchanged.
     */
    static constexpr U16 kNoCallable = 0xffff;
    U16 genCallable = kNoCallable;

    /*
     * The **value** this call goes through, for a call to a function value - `f(x)` where `f` is a
     * local rather than a declaration.
     *
     * An operand rather than a second node kind, because everything else about the call is the same
     * statement: the same argument list, the same result, the same summary question. What differs is
     * where the callee comes from, which is what `mir::CallThrough` already exists to say - a class
     * slot, a function slot, a declaration, and now a value in hand.
     *
     * Zero-initialized `Operand` is a `Take` of node 0 and is never a legal callee here, so
     * `callsValue()` is the test rather than a second flag: node 0 of any body is a block argument
     * of the entry block, and a call whose callee is one would have had to name it through a read.
     * `callee` stays set to the declaration a *direct* call named, and is null for this form.
     */
    Operand target;

    bool callsValue() const { return callee == nullptr; }

    /*
     * Handed over **by reference**, which is what `OperandList`'s non-embedding encoding is for.
     *
     * Liveness rewrites a `Forward` in place, and a masked list's entries have no address to rewrite
     * through - `pointerAt` static-asserts on one, so the choice is enforced by the compiler rather
     * than remembered. Range-for over `contents` yields entries *by value* whatever the encoding,
     * which is the trap `util/README.md` records, so the index form is the correct one here.
     */
    template<class F> void eachOperand(MirBase base, F&& f) {
        // The callee first, because it is read before the arguments are: a walk that ordered them
        // the other way would place a loan of the value after the loans of what it is passed, and
        // liveness reads this order as the order of use.
        if(callsValue()) f(target, OperandRole::Callee);

        auto items = arguments.contents(base);
        for(Size i = 0; i < items.size(); i++) f(*items.pointerAt(i), OperandRole::Argument);
    }

    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * What a write does to what is already at its destination - §5.5.
 *
 * The distinction the current resolver does not make and this one is built around: an assignment
 * never silently means an initialization, and an initialization can never overwrite a live object.
 * `Reinit` is the normalized third - a path on which the destination was moved from becoming owned
 * again - and is declared here rather than added later because the drop-flag dataflow that produces
 * it has to have somewhere to put its answer.
 */
enum class Destination: U8 {
    Init,
    Assign,
    Reinit,
};

/*
 * Storing a value into a place.
 *
 * The destination is a `BorrowUnique` operand, not a bare place ID: writing is exclusive access, and
 * saying so with the same mode a `&` argument uses is what lets one overlap check see both. The
 * value's mode is the ordinary sink question - `Take` for a value in hand, `CopyTrivial` for a
 * trivial one, `Forward` for a bare owned name whose last use this may be.
 */
struct NodeWrite: Node {
    NodeWrite(LocationId source, Operand destination, Operand value, Destination mode):
        Node(Write, source), destination(destination), value(value), mode(mode) {}

    Operand destination;
    Operand value;
    Destination mode;

    template<class F> void eachOperand(MirBase, F&& f) {
        f(destination, OperandRole::WriteTarget);
        f(value, OperandRole::Stored);
    }

    template<class F> void eachEdge(MirBase, F&&) {}
};

/*
 * A terminator's outgoing edge - §5.9.
 *
 * The operands a predecessor supplies for a successor's block arguments live on the predecessor,
 * which is the point: ownership changes hands on the edge that performs the transfer, so a drop can
 * never be inserted between a `take` and a merge that already assumed it happened. A neutral phi in
 * the successor has no place to say which arm moved.
 */
struct Edge {
    MirPtr<Block> target = nullptr;
    OperandList arguments;
};

struct NodeReturn: Node {
    NodeReturn(LocationId source, Operand value, bool hasValue):
        Node(Return, source), value(value), hasValue(hasValue) {}

    Operand value;

    // A function whose result is `{}` returns nothing rather than returning a unit value. The flag
    // rather than a null operand, because `Operand` has no null: every one of them names a source.
    bool hasValue;

    template<class F> void eachOperand(MirBase, F&& f) { if(hasValue) f(value, OperandRole::Returned); }
    template<class F> void eachEdge(MirBase, F&&) {}
};

struct NodeJump: Node {
    NodeJump(LocationId source): Node(Jump, source) {}

    Edge to;

    template<class F> void eachOperand(MirBase, F&&) {}
    template<class F> void eachEdge(MirBase, F&& f) { f(to); }
};

/*
 * A lens call - Analysis-IR.md §10.1 and §11.4's W-B.
 *
 * A terminator and not a `call`, and the reason is the one §10.1 states: the continuation is the
 * **caller's** code, so the split between "before the hand-over" and "the continuation" is in the
 * CFG from construction rather than being a call to a lambda a later pass explodes. Control leaves
 * this node for the region's entry, where the callee's hand-over arrives as a block argument; it
 * comes back at `Continuation::destination` when the continuation falls out; and it may not come
 * back at all, because a `return` written in the continuation is a `return` of this function.
 * Liveness, the loan extents and drop elaboration walk all of that without knowing what a lens is.
 *
 * **The callee is opaque here, and the continuation is not an operand.** What the callee's own body
 * does - the statements before its `yield`, its cleanup after it - is its own, resolved and analysed
 * once like any other function's. What the *conversion* makes of the pair is where the two lowerings
 * differ (`resolve_ssa/convert.cpp`): the CPS form outlines the region into a function and passes it
 * as an ordinary argument, and the embed form copies the analysed callee body around it. Neither is
 * a fact about this node, which is why it holds a region rather than a value.
 *
 * The declared hand-over occurrence is on the region (`Continuation::handover`) rather than here,
 * because that is what the entry's block argument is checked against and there is one of it however
 * many terminators could reach the region.
 */
struct NodeLensCall: Node {
    NodeLensCall(LocationId source, DeclPtr<Function> callee, ContinuationId region):
        Node(LensCall, source), callee(callee), region(region) {}

    DeclPtr<Function> callee;

    // The arguments the *source* wrote - `Function::writtenArity`. The continuation parameter the
    // desugaring appended is filled by the conversion and has no operand here.
    OperandList arguments;

    // The callee's type arguments, in the enclosing body's terms, exactly as `NodeCall` carries
    // them and read by `callTarget` the same way.
    MirList<DeclPtr<Type>> typeArguments;

    // The continuation region this opens. Its `entry` is `to.target` and its `destination` is where
    // control resumes, both asserted where the node is built.
    ContinuationId region;

    Edge to;

    /*
     * Where control resumes when a *skipping* callee never handed over - `LensTry` only, and unused
     * on a `LensCall`.
     *
     * The reason it failed with rides this edge as its one argument, so the block on the far side
     * has a parameter and `reason` below is what that parameter is checked against.
     */
    Edge otherwise;

    // What the callee's `| E` declared, substituted into this call's terms. Held here rather than
    // read back off the callee for the reason `Continuation::handover` is held on the region: it is
    // what an edge's argument is checked against, and a check whose expected type is re-derived at
    // every reader is one that can be derived differently.
    Occurrence reason;

    template<class F> void eachOperand(MirBase base, F&& f) {
        auto items = arguments.contents(base);
        for(Size i = 0; i < items.size(); i++) f(*items.pointerAt(i), OperandRole::Argument);
    }

    // The kind and not a validity test on the edge, because "this call can skip" is a fact about the
    // callee's signature and stays true for a `lens.try` whose skip block is unreachable.
    template<class F> void eachEdge(MirBase, F&& f) {
        f(to);
        if(kind == LensTry) f(otherwise);
    }
};

struct NodeBranch: Node {
    NodeBranch(LocationId source, Operand condition): Node(Branch, source), condition(condition) {}

    Operand condition;
    Edge whenTrue;
    Edge whenFalse;

    template<class F> void eachOperand(MirBase, F&& f) { f(condition, OperandRole::Condition); }
    template<class F> void eachEdge(MirBase, F&& f) { f(whenTrue); f(whenFalse); }
};

/*
 * One case of a `switch` - the number the selector must equal, and where control goes.
 *
 * `I64` for `MapEntry`'s reason: `@value(n)` may pin a constructor outside thirty-two bits, and
 * whether every case is representable at the selector's declared type is a verifier rule rather
 * than a representation choice.
 */
struct SwitchCase {
    I64 value = 0;
    Edge edge;
};

/*
 * A multi-way branch - inst.def's `Switch` row, where the argument for two nodes is.
 *
 * The cases are ascending by value and pairwise distinct, and the ordinal order of the edges is case
 * order then the fallback - which is what `Editor::redirectEdge` and `addEdgeArgument` index by, so
 * it is part of the contract rather than an implementation detail.
 */
struct NodeSwitch: Node {
    NodeSwitch(LocationId source, Operand selector): Node(Switch, source), selector(selector) {}

    Operand selector;

    // Non-embedding: a `SwitchCase` is an `I64` beside an `Edge`, and `SmallList`'s embedded
    // encoding tags a bit inside the element.
    MirList<SwitchCase, false> cases;

    Edge fallback;

    template<class F> void eachOperand(MirBase, F&& f) { f(selector, OperandRole::Selector); }

    template<class F> void eachEdge(MirBase base, F&& f) {
        auto items = cases.pointer(base);
        for(Size i = 0; i < cases.size(); i++) f(items[i].edge);

        f(fallback);
    }
};

struct NodeUnreachable: Node {
    explicit NodeUnreachable(LocationId source): Node(Unreachable, source) {}

    template<class F> void eachOperand(MirBase, F&&) {}
    template<class F> void eachEdge(MirBase, F&&) {}
};

// The concrete type of a node, from inst.def, so that the kind list and the cast list are one
// statement. Every generic walk - printing, verification, conversion - goes through this.
template<class F>
auto visitNode(Node& node, F&& f) -> decltype(f((NodeConst&)node)) {
    switch(node.kind) {
#define YANA_MIR_INST(kind, Struct, mnemonic, flags) case Node::kind: return f((Struct&)node);
#include "inst.def"
#undef YANA_MIR_INST
        case Node::KindCount: break;
    }

    // Not a fallback: a kind outside the table is a corrupt node rather than one this walk has no
    // answer for. The cast is to a concrete type so that `f` is never instantiated with a bare
    // `Node&` - which is what makes the two walks below compile only for a kind that declared its
    // schema.
    assertTrue(false);
    return f((NodeConst&)node);
}

/*
 * Every operand this node stores, and every edge it leaves by.
 *
 * Declared beside the fields on each node struct rather than as a column of inst.def, because a
 * column listing field names would be a second spelling of the struct and the two would drift. What
 * this arrangement buys is that a node kind which *forgets* to declare its schema does not compile:
 * `visitNode` never passes a bare `Node&`, and `Node` has no `eachOperand`. §2.1's "every structural
 * operand is visible to generic walks" is enforced by the compiler rather than by a verifier that
 * has to be remembered.
 *
 * The operand is handed over by reference: liveness rewrites `Forward` in place once it knows
 * whether the source is dead afterwards, and that is the one rewrite a sealed body permits. Its
 * `OperandRole` comes with it, so a walk that has to know what an operand is *for* - which is every
 * structural check worth making - does not have to switch on the kind and re-derive it.
 *
 * The base is a parameter because an operand may live in a *list* rather than in a field: a call's
 * arguments do, and a walk with no base could not reach them at all. That is not a detail about
 * calls - a kind whose operands are invisible to this walk is a kind the role checks and §12.3's
 * mutation testing simply do not cover, so the alternative was a node whose arguments no verifier
 * ever looked at. `ssa::forEachUse` takes one for the same reason.
 */
template<class F>
void forEachOperand(MirBase base, Node& node, F&& f) {
    visitNode(node, [&](auto& n) { n.eachOperand(base, f); });
}

template<class F>
void forEachEdge(MirBase base, Node& node, F&& f) {
    visitNode(node, [&](auto& n) { n.eachEdge(base, f); });
}

StringView nodeMnemonic(Node::Kind kind);
U16 nodeFlags(Node::Kind kind);

inline bool hasResult(Node::Kind kind) { return (nodeFlags(kind) & kMirResult) != 0; }
inline bool isTerminator(Node::Kind kind) { return (nodeFlags(kind) & kMirTerminator) != 0; }

/*
 * Both kinds that call a lens - the two rows sharing `NodeLensCall`.
 *
 * A named predicate rather than a pair of comparisons at each of the eight readers, for the reason
 * every other question about a kind is one: whether a third row ever joins them is a question with
 * one answer, and eight `||` chains would be eight places to remember to widen.
 */
inline bool isLensCall(Node::Kind kind) { return kind == Node::LensCall || kind == Node::LensTry; }

/*
 * The result of a node that has one.
 *
 * The cast is what `kMirResult` licenses: every kind with the flag derives from `NodeResult` with
 * `Node` first, and a kind without it has no occurrence to hand back. Asserted rather than checked,
 * because a caller reaching for the result of a store is a bug in the caller and not a case with an
 * answer - `hasResult` is how a generic walk asks first.
 */
inline NodeResult& asResult(Node& node) {
    assertTrue(hasResult(node.kind));
    return (NodeResult&)node;
}

inline const Occurrence& valueOccurrence(Node& node) { return asResult(node).occurrence; }
inline DeclPtr<Type> valueType(Node& node) { return asResult(node).occurrence.type; }

StringView useModeName(UseMode mode);
StringView destinationName(Destination mode);

// The mode an operand carries when it fills a parameter declared under `parameter` - §5.6. One rule
// in one place, so that a call site states which signature it selected and never states what it is
// doing to its own value; see inst.cpp, where the source half of the question is written out.
UseMode argumentMode(Contract parameter, SourceKind source);

/*
 * The mode a *read of storage* carries, given what the position asks the value for.
 *
 * The other half of the same rule `argumentMode` states, and here for the same reason: a read is
 * what turns storage into a value, and what it produces - a second trivial owner, a loan, the object
 * itself - is decided once from the place and the position rather than at each of the resolver's
 * half-dozen read sites. `doc/spec/ownership.md`'s "implicit copies" and "a second name for an owned
 * value" are the two sections this function is; there is no second reading of either anywhere.
 *
 * Four answers, in the order they are asked:
 *
 *  - A trivially copyable type is copied wherever a copy satisfies the position - see `Request`,
 *    which is where the two positions that refuse one are written down. That a copy also satisfies a
 *    plain borrow is not an approximation: a trivial value's loan and its copy are physically the
 *    same thing, and the spec says so by listing "default and `->`" as the positions the copy
 *    happens at. It is also why the two remain different modes rather than one.
 *  - A position that asked for exclusive access gets a unique loan. A copy cannot satisfy one and
 *    neither can a shared loan, so this is the one answer that does not depend on the place - and
 *    whether the place *can* grant it is `Resolver::readPlace`'s report rather than a mode.
 *  - Storage this function does not own has nothing to hand over whatever the position wanted, so it
 *    is borrowed from. A `Forward` out of a shared parameter would be a move out of the caller's
 *    value. The one exception is `lendsItsContents` below, where the storage is writable and the
 *    frame owes a refill.
 *  - Otherwise: a borrow position borrows - which is what makes `let d = c` the shared loan the spec
 *    says it is - and a sink position gets `Forward`, the bare owned name the language moves when it
 *    is dead afterwards, which §5.4 insists a single forward pass may not decide.
 */
UseMode readMode(DeclBase base, MirBase mir, Body& body, const Place& place, Request wanted);

/*
 * Whether the contents of this place may be **taken out and put back** - `doc/spec/ownership.md`'s
 * "taking a value out of borrowed storage", and the rule `acc = acc ++ part` through a `&` is.
 *
 * The storage is somebody else's and this frame may write it, so the value in it may fill a position
 * that consumes provided something is written back before the frame lets go of the reference. Two
 * properties of the language make the window between the two unobservable, and both are properties
 * rather than assumptions: an exclusive loan is exclusive while it is live, so no second reader
 * exists to look into the hole, and nothing unwinds, so no teardown runs over one on the way out.
 *
 * A **`&` parameter** and nothing else, which is narrower than "an exclusive loan" and deliberately
 * so. `let &d = c` re-lends storage this frame is *also* tracking under `c`'s own move path, and a
 * take through the alias would empty one place while the other still said it held a value - two
 * answers to one question, which is the arrangement this stage exists to not have. A parameter's
 * lender is the caller, whose own analysis already holds the argument exclusively for the length of
 * the call, so there is no second path here to disagree with.
 *
 * `isTrackedAt` is asked because the answer has to be one the analysis can carry: an untracked type
 * has no move path, so there would be nowhere to record the hole or the refill - and a trivially
 * copyable one is copied by the first arm of `readMode` long before this.
 */
bool lendsItsContents(DeclBase base, MirBase mir, Body& body, const Place& place);

// Whether this node may suspend the enclosing fiber, from the callee's published summary rather than
// from the node's kind - Analysis-Sharing.md §3.3, and the reason inst.def has no such column.
bool maySuspend(DeclBase base, Node& node);

// What a binary operation of this category answers, given operands of `operandType`. Here rather
// than in the resolver so that the MIR builder and the type check read one rule.
DeclPtr<Type> binaryResult(Context& context, Program& program, BinaryOp op,
                           DeclPtr<Type> operandType);

} // namespace mir
