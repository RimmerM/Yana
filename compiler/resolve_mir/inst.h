#pragma once

#include "place.h"
#include "op.h"

namespace mir {

struct Block;
struct Body;

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
    Returned,     // the value a `return` hands back
    EdgeArgument, // an operand supplied to a successor's parameter, on an outgoing edge
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

    template<class F> void eachOperand(F&&) {}
    template<class F> void eachEdge(F&&) {}
};

// A literal named by a body: an index into `Body::constants`.
struct NodeConst: NodeResult {
    NodeConst(LocationId source, ConstantId constant):
        NodeResult(Const, source), constant(constant) {}

    ConstantId constant;

    template<class F> void eachOperand(F&&) {}
    template<class F> void eachEdge(F&&) {}
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

    template<class F> void eachOperand(F&& f) { f(place, OperandRole::ReadSource); }
    template<class F> void eachEdge(F&&) {}
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

    template<class F> void eachOperand(F&& f) {
        f(lhs, OperandRole::Value);
        f(rhs, OperandRole::Value);
    }

    template<class F> void eachEdge(F&&) {}
};

struct NodeUnary: NodeResult {
    NodeUnary(LocationId source, UnaryOp op, Operand value):
        NodeResult(Unary, source), value(value), op(op) {}

    Operand value;
    UnaryOp op;

    template<class F> void eachOperand(F&& f) { f(value, OperandRole::Value); }
    template<class F> void eachEdge(F&&) {}
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

    template<class F> void eachOperand(F&& f) {
        f(destination, OperandRole::WriteTarget);
        f(value, OperandRole::Stored);
    }

    template<class F> void eachEdge(F&&) {}
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

    template<class F> void eachOperand(F&& f) { if(hasValue) f(value, OperandRole::Returned); }
    template<class F> void eachEdge(F&&) {}
};

struct NodeJump: Node {
    NodeJump(LocationId source): Node(Jump, source) {}

    Edge to;

    template<class F> void eachOperand(F&&) {}
    template<class F> void eachEdge(F&& f) { f(to); }
};

struct NodeBranch: Node {
    NodeBranch(LocationId source, Operand condition): Node(Branch, source), condition(condition) {}

    Operand condition;
    Edge whenTrue;
    Edge whenFalse;

    template<class F> void eachOperand(F&& f) { f(condition, OperandRole::Condition); }
    template<class F> void eachEdge(F&& f) { f(whenTrue); f(whenFalse); }
};

struct NodeUnreachable: Node {
    explicit NodeUnreachable(LocationId source): Node(Unreachable, source) {}

    template<class F> void eachOperand(F&&) {}
    template<class F> void eachEdge(F&&) {}
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
 */
template<class F>
void forEachOperand(Node& node, F&& f) {
    visitNode(node, [&](auto& n) { n.eachOperand(f); });
}

template<class F>
void forEachEdge(Node& node, F&& f) {
    visitNode(node, [&](auto& n) { n.eachEdge(f); });
}

StringView nodeMnemonic(Node::Kind kind);
U16 nodeFlags(Node::Kind kind);

inline bool hasResult(Node::Kind kind) { return (nodeFlags(kind) & kMirResult) != 0; }
inline bool isTerminator(Node::Kind kind) { return (nodeFlags(kind) & kMirTerminator) != 0; }

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

// What a binary operation of this category answers, given operands of `operandType`. Here rather
// than in the resolver so that the MIR builder and the type check read one rule.
DeclPtr<Type> binaryResult(Types& types, BinaryOp op, DeclPtr<Type> operandType);

} // namespace mir
