#include "build.h"

namespace mir {

// The body and its entry block, made together: a function without somewhere to start is not a state
// a builder should be able to be in for even one statement.
static Body& createBody(Program& program, Function& function) {
    auto entry = new (program.mir) Block(BlockId(0), StringId());
    auto body = new (program.mir) Body(entry - *program.mir);
    body->blocks.push(program.mir, entry - *program.mir);

    function.mir = body - *program.mir;
    return *body;
}

Builder::Builder(Context& context, Program& program, Function& function):
    context(context), program(program), function(function), body(createBody(program, function))
{
    current = base()[body.entry];
}

Block* Builder::addBlock(StringId name) {
    auto index = BlockId(body.blocks.size());
    auto block = new (arena()) Block(index, name);
    body.blocks.push(arena(), block - base());
    return block;
}

Local* Builder::addLocal(StringId name, Occurrence occurrence, LocationId source, bool writable) {
    auto local = new (arena()) Local(name, occurrence, source, writable);
    local->index = LocalId(body.locals.push(arena(), local - base()));
    return local;
}

Node* Builder::addBlockArgument(Block& block, Occurrence occurrence, LocationId source) {
    // Arguments are declared before the block is entered, because a predecessor's edge operand list
    // has to match a count that already exists.
    assertTrue(block.nodes.size() == 0);

    auto node = new (arena()) NodeArgument(source, block.arguments.size());
    node->occurrence = occurrence;
    node->id = NodeId(body.nodes.push(arena(), asBase<Node>(node - base())));

    // Into the block's parameter list and not its instruction list - see inst.def. `append` is
    // therefore deliberately not used here, and neither is the terminator check it performs.
    block.arguments.push(arena(), asBase<Node>(node - base()));
    return node;
}

void Builder::setBlock(Block& block) {
    current = &block;
}

bool Builder::isTerminated() {
    if(!current) return true;

    auto count = current->nodes.size();
    if(!count) return false;

    return isTerminator(base()[current->nodes.get(base(), count - 1)]->kind);
}

PlaceId Builder::internPlace(Place::Kind kind, Place* parent, U32 payload, Occurrence occurrence,
                             bool writable) {
    auto parentId = parent ? parent->id : PlaceId();

    // The bound the key's parent field has room for. Asserted rather than masked: this is exactly
    // where a truncated identity turns two places into one, and a body of half a billion places is
    // not a program - it is the assertion having found a corrupt ID.
    assertTrue(!parentId.valid() || parentId.index <= kMaxParentId);

    // The occurrence is derived from the three key fields - a field of a given parent has one type,
    // one contract and one shape - so it is not part of the key, and a second lookup that disagreed
    // about it would be a bug rather than a different place.
    auto entry = placeIds.add(placeKey(kind, parentId, payload));

    if(!entry.existed) {
        auto handle = parent ? MirPtr<Place>(parent - base()) : MirPtr<Place>(nullptr);
        auto place = new (arena()) Place(kind, handle, payload, occurrence, writable);
        place->id = PlaceId(body.places.push(arena(), place - base()));
        *entry.value = place->id.index;
    }

    return PlaceId(*entry.value);
}

Place* Builder::localPlace(Local& local) {
    return body.place(base(), internPlace(Place::Local, nullptr, local.index.index,
                                          local.occurrence, local.writable));
}

Place* Builder::globalPlace(DeclPtr<Global> global) {
    auto declaration = declBase()[global];

    /*
     * The place's contract is not the global's.
     *
     * A global owns its value; a *place* naming it is storage this function may read, and where the
     * global is immutable it is storage nothing may write. The occurrence recorded here is what an
     * access through the place may do, which is the question every later pass asks of a place - see
     * `Place::occurrence`.
     */
    auto occurrence = declaration->occurrence;
    return body.place(base(), internPlace(Place::Global, nullptr, global.offset, occurrence,
                                          declaration->writable));
}

Place* Builder::fieldPlace(Place& parent, U16 field, Occurrence occurrence) {
    // A projection is writable exactly when its parent is: reaching a field of storage nothing may
    // write does not acquire permission to write it.
    return body.place(base(), internPlace(Place::Field, &parent, field, occurrence,
                                          parent.writable));
}

/*
 * Interned per body, linearly.
 *
 * A scan rather than a map, and it stays a scan: a body's constant table is the *distinct* literals
 * one function names, which is a handful, and a hash map for it would be a heap allocation per
 * function to save a walk over four entries. When a body appears whose table is long enough to
 * matter, the measurement belongs beside this comment.
 */
ConstantId Builder::addConstant(Constant constant) {
    for(U32 i = 0; i < body.constants.size(); i++) {
        if(body.constants.get(base(), i) == constant) return ConstantId(i);
    }

    return ConstantId(body.constants.push(arena(), constant));
}

Operand Builder::resultOf(Node& node, UseMode mode) {
    // A raw ID is not accepted here for exactly this reason: nothing that does not produce a value
    // may be named as one, and taking the node itself is what makes that checkable at the call.
    assertTrue(hasResult(node.kind));
    return Operand(SourceKind::Result, node.id.index, mode);
}

Operand Builder::placeOperand(Place& place, UseMode mode) {
    return Operand(SourceKind::Place, place.id.index, mode);
}

Operand Builder::constantOperand(ConstantId constant, UseMode mode) {
    assertTrue(constant.index < body.constants.size());
    return Operand(SourceKind::Constant, constant.index, mode);
}

Node* Builder::append(Node* node) {
    assertTrue(current != nullptr);

    // A finished block takes nothing more. The terminator is last by construction rather than by a
    // later sort, which is what lets `verifyBody` state the invariant as a fact about position.
    auto count = current->nodes.size();
    if(count) {
        auto last = base()[current->nodes.get(base(), count - 1)];
        assertTrue(!isTerminator(last->kind));
    }

    node->id = NodeId(body.nodes.push(arena(), node - base()));
    current->nodes.push(arena(), node - base());
    return node;
}

Node* Builder::addConst(LocationId source, Constant constant) {
    assertTrue(constant.isSet());

    auto index = addConstant(constant);
    auto node = new (arena()) NodeConst(source, index);
    node->occurrence = owned(constant.type);
    return append(node);
}

/*
 * What reading a place produces, decided once.
 *
 * The mode is the input and the result's contract is the output, and the mapping is here rather than
 * at each call site because it is one rule: taking or trivially copying makes a new owner, borrowing
 * makes a loan that the place's own origin keeps alive, and inspecting produces a value nobody owns
 * beyond the expression. §5.3 is the source-form half of the same table.
 */
Node* Builder::addRead(LocationId source, Place& place, UseMode mode) {
    Contract contract;

    switch(mode) {
        case UseMode::CopyTrivial:
        case UseMode::Take:
            contract = ownedContract();
            break;
        case UseMode::BorrowShared:
            contract = borrowedContract(Access::Shared);
            break;
        case UseMode::BorrowUnique:
            contract = borrowedContract(Access::Unique);
            break;
        case UseMode::Inspect:
            contract = borrowedContract(Access::Shared);
            break;

        // A read is a decided operation. `Forward` is the *unresolved* one, and it belongs to a
        // sink position - an argument, a return - rather than to the act of reading storage.
        case UseMode::Forward:
        case UseMode::Count:
            assertTrue(false);
            break;
    }

    // Writing through a place the function only shares is the one thing a builder can refuse here,
    // and it is worth refusing here: the alternative is a diagnostic from the loan checker about an
    // IR that should not have been constructible.
    if(mode == UseMode::BorrowUnique) {
        assertTrue(place.occurrence.contract.access() != Access::Shared);
    }

    // The root's qualification changes and the interior shape does not: taking a `Pair(a'T, U)` out
    // of storage produces an owned pair that still has a borrow inside it, and a read that rebuilt
    // the occurrence from the type alone would lose exactly that.
    auto node = new (arena()) NodeRead(source, placeOperand(place, mode));
    node->occurrence = requalified(place.occurrence, contract);
    return append(node);
}

Node* Builder::addBinary(LocationId source, BinaryOp op, Node& lhs, Node& rhs) {
    auto type = valueType(lhs);
    assertTrue(sameType(type, valueType(rhs)));
    assertTrue(operandTypeAllowed(declBase(), binaryCategory(op), type));

    auto node = new (arena()) NodeBinary(source, op, resultOf(lhs, UseMode::Inspect),
                                         resultOf(rhs, UseMode::Inspect));
    node->occurrence = owned(binaryResult(program.types, op, type));
    return append(node);
}

Node* Builder::addUnary(LocationId source, UnaryOp op, Node& value) {
    auto type = valueType(value);
    assertTrue(operandTypeAllowed(declBase(), unaryCategory(op), type));

    auto node = new (arena()) NodeUnary(source, op, resultOf(value, UseMode::Inspect));
    node->occurrence = owned(type);
    return append(node);
}

Node* Builder::addWrite(LocationId source, Place& place, Operand value, Destination mode) {
    // What the current resolver cannot say and this one is built to: an assignment is not an
    // initialization. The place's own permission answers only the first of the two - a `let x` place
    // is written once, by the `Init` that establishes it, and never again.
    if(mode != Destination::Init) assertTrue(place.writable);

    // A write is exclusive access whatever the destination said, so a place the function only shares
    // is not a destination at all. The same check the verifier restates over a sealed body; here it
    // stops the node from being built.
    assertTrue(place.occurrence.contract.access() != Access::Shared);

    auto destination = placeOperand(place, UseMode::BorrowUnique);
    return append(new (arena()) NodeWrite(source, destination, value, mode));
}

Node* Builder::addReturn(LocationId source, Operand value) {
    return append(new (arena()) NodeReturn(source, value, true));
}

Node* Builder::addReturnUnit(LocationId source) {
    return append(new (arena()) NodeReturn(source, Operand(), false));
}

Node* Builder::addJump(LocationId source, Block& target, const Operand* arguments, Size count) {
    // The successor's parameter list is what an edge has to satisfy, and the count is checkable the
    // moment the edge is built rather than at the next verifier boundary.
    assertTrue(count == target.arguments.size());

    auto node = new (arena()) NodeJump(source);
    node->to.target = &target - base();
    for(Size i = 0; i < count; i++) node->to.arguments.push(arena(), arguments[i]);

    return append(node);
}

Node* Builder::addBranch(LocationId source, Operand condition, Block& whenTrue, Block& whenFalse) {
    // Arguments on a two-way edge need the caller to supply both lists; until a milestone produces
    // one, a branch to a block with parameters is a builder bug.
    assertTrue(whenTrue.arguments.size() == 0 && whenFalse.arguments.size() == 0);

    // Two arms that are one block is a jump written as a branch. Refused here rather than folded,
    // because the lower IR asserts the same thing and a resolver that produced one would be finding
    // that out three phases later.
    assertTrue(&whenTrue != &whenFalse);

    auto node = new (arena()) NodeBranch(source, condition);
    node->whenTrue.target = &whenTrue - base();
    node->whenFalse.target = &whenFalse - base();
    return append(node);
}

Node* Builder::addUnreachable(LocationId source) {
    return append(new (arena()) NodeUnreachable(source));
}

void Builder::seal() {
    assertTrue(!body.sealed);

    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(base(), BlockId(i));
        auto count = block->nodes.size();
        if(!count) continue;

        auto last = base()[block->nodes.get(base(), count - 1)];
        if(!isTerminator(last->kind)) continue;

        forEachEdge(*last, [&](Edge& edge) {
            if(!edge.target) return;
            base()[edge.target]->incoming.push(arena(), block - base());
        });
    }

    body.sealed = true;
}

} // namespace mir
