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

/*
 * A parameter's storage.
 *
 * Its occurrence is the declaration's, whole: what the callee received - owned, a shared loan, a
 * unique loan, in which origin slot - is what the place holds, and re-deriving any of that from the
 * type at a use site is the walk this representation exists to delete.
 *
 * `writable` is the `&` half of the binding and not the access half. They are separate questions
 * for the reason `Local::writable` gives one level up: a `->x: T` parameter is owned and is not
 * assignable, and an occurrence that folded the two would make ownership and mutability one field
 * again.
 */
Local* Builder::addParameter(Arg& declared) {
    auto local = addLocal(declared.name, declared.occurrence, declared.source,
                          declared.occurrence.contract.access() == Access::Unique);

    // The two lists are one declaration seen from two sides, so this is where they are held to it:
    // a parameter created out of order would make the SSA conversion pair the wrong incoming value
    // with the wrong slot, and nothing below would have a way to notice.
    assertTrue(local->index.index == declared.index);

    local->parameter = declared.index;
    return local;
}

Node* Builder::addBlockArgument(Block& block, Occurrence occurrence, LocationId source) {
    // Arguments are declared before the block is entered, because a predecessor's edge operand list
    // has to match a count that already exists.
    assertTrue(block.nodes.size() == 0);

    auto node = new (arena()) NodeArgument(source, block.arguments.size());
    node->occurrence = occurrence;
    node->id = NodeId(body.nodes.push(arena(), asBase<Node>(node - base())));

    /*
     * Dense over node IDs like every other node's entry - `Body::nodeObject`.
     *
     * `append` is what keeps the table dense for the instruction list, and an argument does not go
     * through it (see below), so the one place that pushes a node outside `append` is also the one
     * place that has to say so. An owned merge starts a lineage of its own; the arms unite theirs
     * into it at the edges, in `addJump`.
     */
    body.nodeObject.push(arena(), ObjectId::kNone);

    if(occurrence.contract.isOwned() && isTracked(declBase(), occurrence.type)) {
        body.nodeObject.set(base(), node->id.index, mintObject(occurrence.type, source).index);
    }

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

/*
 * A field of a record place.
 *
 * Two things are derived and neither is a parameter. The *contract* is the parent's combined with
 * the field's declaration, and the combination is one sentence: a field of an owned record is owned,
 * a field reached through a loan is held under that same loan because reaching into storage does not
 * change whose it is, and a field the record itself only *refers* to is held under the record's own
 * origin however the record was reached. And a projection is writable exactly when its parent is -
 * reaching a field of storage nothing may write does not acquire permission to write it - except
 * that an external field is writable only where the declaration said `&`.
 *
 * An external field returns a **`Deref`** of the slot rather than the slot, because the two are
 * different storage: the field holds a reference and the object is somewhere else. That is what the
 * kind is for, and it is the only thing that makes one.
 */
Place* Builder::fieldPlace(Place& parent, U16 field) {
    auto record = (RecordType*)declBase()[parent.occurrence.type];
    assertTrue(record->kind == Type::Record);
    assertTrue(field < record->fields.size());

    // A shape on the *occurrence* is qualification a generic application varied, and there is no
    // application yet - a nominal record's own interior structure is `RecordType::schema` and is
    // read from the declaration below. The first higher-kinded `Maybe(a'T)` is what changes this.
    assertTrue(Shapes::isEmpty(parent.occurrence.shape));

    auto placeOf = [&](U16 index) {
        auto declared = record->field(declBase(), index);
        auto held = parent.occurrence.contract;

        if(declared.contract.origin() != Origin::Slot) {
            Occurrence occurrence { declared.type, held, nullptr };
            return internPlace(Place::Field, &parent, index, occurrence, parent.writable);
        }

        /*
         * An external field: the slot, and then the object it names.
         *
         * The capability is the declaration's, narrowed by the way the record was reached - an
         * `&'T` field of a record this frame only shares is a shared view, because a shared borrow
         * of a record may not hand out exclusive access to what it refers to.
         */
        auto access = declared.contract.access();
        if(held.access() == Access::Shared && access == Access::Unique) access = Access::Shared;

        Occurrence occurrence {
            declared.type, slotContract(access, declared.contract.slot()), nullptr
        };

        auto slot = body.place(base(), internPlace(Place::Field, &parent, index, occurrence,
                                                   parent.writable));
        auto writable = parent.writable && access == Access::Unique;
        return derefPlace(*slot, occurrence, writable)->id;
    };

    /*
     * Every field of a record that owes a teardown, not just the one asked for.
     *
     * Partial ownership is why. Moving `p.left` out leaves `p.right` where it was and this frame
     * still owes its release - and a drop is placed on a *path*, which exists because a place names
     * the storage it releases. A body that moves one field and never mentions the other therefore
     * had nowhere to put the surviving field's teardown, and quietly leaked it: `analyze_path.cpp`
     * built children for the fields the body happened to write down, which is every field a fixture
     * that reads them all has and not the one a fixture that does not.
     *
     * Interning the whole set here rather than materializing the missing ones in the analysis keeps
     * places something only the builder makes, and keeps the sealed body the analysis reads exactly
     * the body the verifier checked.
     *
     * Only where there is a teardown to place: a record whose fields are copied bit for bit has
     * nothing to leak, and interning its whole field set for one projection would be rows bought for
     * nothing. `hasTeardown` is the same question `placeDrops` asks before emitting anything.
     */
    if(hasTeardown(declBase(), parent.occurrence.type) &&
       Shapes::isEmpty(schemaOf(declBase(), parent.occurrence.type))) {
        // In field order, the requested one included, because the interning order is what
        // `buildPaths` reads a record's children in - and that order is the order its fields are
        // released in. A walk that put the asked-for field first would release a record's members
        // back to front for no reason the author could see.
        auto wanted = PlaceId();
        for(U16 i = 0; i < U16(record->fields.size()); i++) {
            auto id = placeOf(i);
            if(i == field) wanted = id;
        }

        return body.place(base(), wanted);
    }

    return body.place(base(), placeOf(field));
}

Place* Builder::derefPlace(Place& parent, Occurrence occurrence, bool writable) {
    return body.place(base(), internPlace(Place::Deref, &parent, 0, occurrence, writable));
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

    // The lineage table stays dense over node IDs - see `Body::nodeObject`. Filled in here rather
    // than by each producer, so that a node kind added later has an entry whether or not whoever
    // added it thought about objects.
    body.nodeObject.push(arena(), ObjectId::kNone);
    return node;
}

ObjectId Builder::mintObject(DeclPtr<Type> type, LocationId source) {
    auto id = ObjectId(U32(body.objects.size()));
    auto object = new (arena()) Object(id, type, source);
    body.objects.push(arena(), object - base());
    return id;
}

void Builder::uniteObjects(ObjectId a, ObjectId b) {
    auto left = body.objectRoot(base(), a);
    auto right = body.objectRoot(base(), b);
    if(!left.valid() || !right.valid() || left == right) return;

    // The younger into the older, which keeps the surviving root the one a later reader is likelier
    // to already hold. Nothing depends on which survives - they are one object either way.
    if(left.index < right.index) {
        body.object(base(), right)->parent = left;
    } else {
        body.object(base(), left)->parent = right;
    }
}

ObjectId Builder::objectOfPlace(Place& place) {
    auto root = &place;
    while(root->parent) root = base()[root->parent];

    if(root->kind != Place::Local) return ObjectId();

    auto local = body.local(base(), LocalId(root->payload));
    if(!local->occurrence.contract.isOwned()) return ObjectId();

    // Started on demand: a parameter's object arrived with the call and a local's arrives with the
    // write that fills it, and both are the first time this frame has anything to say about it.
    if(!local->object.valid()) {
        local->object = mintObject(local->occurrence.type, local->source);
    }

    return local->object;
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
    // The lineage this read may carry out of the place, taken *before* the node exists so that a
    // root local with no object yet has one - see `objectOfPlace`.
    auto moved = mode == UseMode::Take || mode == UseMode::Forward ? objectOfPlace(place) : ObjectId();

    Contract contract;

    switch(mode) {
        /*
         * The three that produce an owner, and `Forward` is one of them.
         *
         * A `Forward` read is a bare owned name in a sink position over a type that refuses
         * duplication, so the only two answers are the move it becomes and the diagnostic it is -
         * there is no borrow that would satisfy an owning position and no implicit copy to fall back
         * on. The result is therefore owned whichever way liveness settles it, and what liveness
         * rewrites is the *operand's* mode rather than anything about this value.
         */
        case UseMode::CopyTrivial:
        case UseMode::Take:
        case UseMode::Forward:
            contract = ownedContract();
            break;
        /*
         * The two that produce a loan, whose origin is the *storage's* - see `reborrowedContract`.
         * A borrow of `p: a'T` is still in `a`, and one of a local is bounded by this frame.
         */
        case UseMode::BorrowShared:
            contract = reborrowedContract(place.occurrence.contract, Access::Shared);
            break;
        case UseMode::BorrowUnique:
            contract = reborrowedContract(place.occurrence.contract, Access::Unique);
            break;

        // Reading a value already in hand, which creates neither an owner nor a loan (§5.3). No
        // read produces one today - `readMode` has no answer that is `Inspect` - and the arm is here
        // because the mode exists rather than because a caller reaches it.
        case UseMode::Inspect:
            contract = borrowedContract(Access::Shared);
            break;

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

    /*
     * The loan, where the read took one.
     *
     * This is the only place a `Loan` is made, which is what makes "every borrow has a place and a
     * capability" a property of the representation rather than a rule the analyses have to agree
     * about. The three modes that produce an owner produce none: a copy is a second value, a move is
     * the same value somewhere else, and neither leaves the lender with anything to be careful of.
     */
    if(mode == UseMode::BorrowShared || mode == UseMode::BorrowUnique) {
        auto access = mode == UseMode::BorrowUnique ? Access::Unique : Access::Shared;
        auto id = LoanId(body.loans.push(arena(), Loan(place.id, access, source)));

        body.loan(base(), id)->id = id;
        node->loan = id;
    }

    append(node);

    /*
     * The other half of "moving preserves a lineage": what came out is the same object.
     *
     * A `CopyTrivial` deliberately mints nothing. It is a second value of a type that refuses no
     * duplication and owes no teardown - `isTracked` is false for exactly those - so there is no
     * lifetime for a lineage to be about. The day a *non-trivial* copy exists it is a second object
     * and mints one here, which is the case this arm is shaped to take.
     */
    if(moved.valid()) body.nodeObject.set(base(), node->id.index, moved.index);

    return node;
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

/*
 * A call, against the signature it selected.
 *
 * Everything checked here is a statement about the *pair* - the call and its callee - and each is
 * checked once, here, rather than by whichever later pass first happens to need it. The modes are
 * not checked at all, because they are not supplied: `argumentMode` produces them from the same
 * signature the arity and the types are read off, so an argument that fills a `->` parameter is a
 * `Take` by construction and there is no spelling for anything else.
 *
 * The result's occurrence is the callee's declared one, and for `-> a'T` that is a statement about
 * the *callee's* schema scope carried down unchanged - which is right, and is what the note on
 * `Contract::slot` explains: the caller's own origins are the origin set the loan analysis computes
 * from the arguments bound to `a`, and what the contract has to preserve here is that the result is
 * a published loan, because a published loan and a frame-bounded one do not manifest the same way.
 */
Node* Builder::addCall(LocationId source, Function& callee, Node* const* arguments, Size count) {
    assertTrue(count == callee.args.size());

    auto node = new (arena()) NodeCall(source, &callee - declBase());
    node->occurrence = callee.result;
    node->arguments.reserve(arena(), count);

    for(Size i = 0; i < count; i++) {
        auto& parameter = *declBase()[callee.args.get(declBase(), U32(i))];
        assertTrue(sameType(valueType(*arguments[i]), parameter.occurrence.type));

        auto mode = argumentMode(parameter.occurrence.contract, SourceKind::Result);
        node->arguments.push(arena(), resultOf(*arguments[i], mode));
    }

    append(node);

    // An owned result is an object this frame receives and is answerable for. A borrowed one is
    // somebody else's, and `Value::loan` is where *that* relation is recorded.
    if(callee.result.contract.isOwned() && isTracked(declBase(), callee.result.type)) {
        body.nodeObject.set(base(), node->id.index,
                            mintObject(callee.result.type, source).index);
    }

    return node;
}

/*
 * An aggregate, against the record it builds.
 *
 * Everything checked here is a statement about the pair - the node and the declaration - and is
 * checked once here rather than by whichever later pass first needs it. The modes are not checked
 * because they are not supplied: `argumentMode` produces them from an owned position, so a component
 * is a `Take` of a value in hand by construction and there is no spelling for anything else.
 */
Node* Builder::addAggregate(LocationId source, DeclPtr<Type> record, Node* const* components,
                            Size count) {
    auto declaration = (RecordType*)declBase()[record];
    assertTrue(declaration->kind == Type::Record);
    assertTrue(count == declaration->fields.size());

    auto node = new (arena()) NodeAggregate(source);
    node->occurrence = owned(record);
    node->components.reserve(arena(), count);

    for(Size i = 0; i < count; i++) {
        auto declared = declaration->field(declBase(), U16(i));
        assertTrue(sameType(valueType(*components[i]), declared.type));

        // The field's own contract, which is owned for a member and a loan for an external position
        // - so a component filling `held: a'Guard` is borrowed into the record rather than taken.
        auto mode = argumentMode(declared.contract, SourceKind::Result);
        node->components.push(arena(), resultOf(*components[i], mode));
    }

    append(node);

    /*
     * A new object - this is where one comes into existence, and there is no lineage to preserve.
     *
     * Its *components* keep theirs: a field taken into the record is the same object at a new
     * address, which is what the writes below unite. Nothing does that yet because a component's
     * object and the record's are related through the record's own storage, and §10.4's interior
     * placement is what decides whether that relation is a merge or a containment. Stated here so
     * that the day it is answered, it is answered in the place that mints the record.
     */
    if(isTracked(declBase(), record)) {
        body.nodeObject.set(base(), node->id.index, mintObject(record, source).index);
    }

    return node;
}

Node* Builder::addWrite(LocationId source, Place& place, Node& value, Destination mode) {
    // What the current resolver cannot say and this one is built to: an assignment is not an
    // initialization. The place's own permission answers only the first of the two - a `let x` place
    // is written once, by the `Init` that establishes it, and never again.
    if(mode != Destination::Init) assertTrue(place.writable);

    /*
     * What the write is *to*, and the two are different questions.
     *
     * An `Init` establishes a slot this frame owns - a local, or a field of one - and what is
     * already in it is nothing, so the contract of the *contents* has no bearing on it: `let d = c`
     * establishes a slot holding a shared loan, and a check that read the shared access there would
     * refuse every borrow a name is ever bound to. Every other write goes *through* the place to
     * what is in it, which is exclusive access and is refused for a place the function only shares.
     */
    if(mode == Destination::Init) {
        auto root = &place;
        while(root->parent) root = base()[root->parent];
        assertTrue(root->kind == Place::Local);
    } else {
        assertTrue(place.occurrence.contract.access() != Access::Shared);
    }

    /*
     * What filling this storage does to the value, from `argumentMode` and nowhere else.
     *
     * The position is owned wherever the storage holds a value somebody owns, which is every write
     * but one: the `Init` that establishes a binding which is itself a loan installs the *loan*, and
     * a `Take` there would be a transfer of something this frame never received.
     */
    auto position = mode == Destination::Init && !place.occurrence.contract.isOwned()
        ? place.occurrence.contract
        : ownedContract();

    auto stored = resultOf(value, argumentMode(position, SourceKind::Result));
    auto destination = placeOperand(place, UseMode::BorrowUnique);
    auto node = append(new (arena()) NodeWrite(source, destination, stored, mode));

    /*
     * **Moving preserves a lineage** - this is the line that says so.
     *
     * A write that *takes* its value puts the object that value names into this storage, and the two
     * are then one object: `let b = a` gives `b` a name for `a`'s object, not a second one, so
     * whatever is decided about where it lives is decided once. A write that copies does not unite
     * anything, and the copy's own producer minted the second lineage - which is the other half of
     * the rule, stated by the absence of a merge rather than by a second condition.
     */
    if(stored.mode() == UseMode::Take || stored.mode() == UseMode::Forward) {
        uniteObjects(objectOfPlace(place), body.objectOf(base(), value.id));
    }

    return node;
}

Node* Builder::addReturn(LocationId source, Node& value) {
    auto mode = argumentMode(function.result.contract, SourceKind::Result);
    return append(new (arena()) NodeReturn(source, resultOf(value, mode), true));
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

    /*
     * An owner handed along an edge is the same object on the other side of the merge.
     *
     * The transfer happens on the *predecessor's* terminator - that is the whole reason blocks have
     * arguments here rather than phis - so this is where the lineage crosses. Both arms of an `if`
     * unite into the successor's parameter, which is what makes `let g = if c then x else y` one
     * object with one placement rather than two that a later pass has to notice are the same.
     */
    for(Size i = 0; i < count; i++) {
        if(arguments[i].mode() != UseMode::Take && arguments[i].mode() != UseMode::Forward) continue;
        if(arguments[i].source() != SourceKind::Result) continue;

        auto parameter = base()[target.arguments.get(base(), i)];
        uniteObjects(body.objectOf(base(), parameter->id),
                     body.objectOf(base(), arguments[i].nodeId()));
    }

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
