#include "build.h"

namespace mir {

/*
 * The body, its own continuation region and its entry block, made together.
 *
 * The region before the block, because a block names one - see `Block::region` - and the body's own
 * is the root, so it has no parent and its entry is the block made immediately after it. A function
 * without somewhere to start is not a state a builder should be able to be in for even one
 * statement, and neither is a block in no region at all.
 */
static Body& createBody(Program& program, Function& function) {
    auto root = new (program.mir)
        Continuation(ContinuationId(0), Continuation::Body, Continuation::Once, ContinuationId());

    auto entry = new (program.mir) Block(BlockId(0), StringId(), root->id);
    auto body = new (program.mir) Body(entry - *program.mir);

    // The context the body is resolved in - see `Body::gen`. Carried on the body so that a place
    // naming a requirement of it can be read with nothing but the body in hand.
    body->gen = function.gen;

    body->regions.push(program.mir, root - *program.mir);
    body->blocks.push(program.mir, entry - *program.mir);
    root->entry = BlockId(0);

    function.mir = body - *program.mir;
    return *body;
}

Builder::Builder(Context& context, Program& program, Function& function):
    context(context), program(program), function(function), body(createBody(program, function))
{
    current = base()[body.entry];
    currentRegion = ContinuationId(0);
}

Block* Builder::addBlock(StringId name) {
    auto index = BlockId(body.blocks.size());
    auto block = new (arena()) Block(index, name, currentRegion);
    body.blocks.push(arena(), block - base());

    // The first block made inside a region is its entry - `Continuation::entry`. Here rather than
    // an argument to `enterRegion`, because the block does not exist when the region is opened and
    // a caller that had to make one first would be making it in the region it is leaving.
    auto region = body.region(base(), currentRegion);
    if(region && !region->entry.valid()) region->entry = index;

    return block;
}

/*
 * A nested region, and the two ends that are not stated together.
 *
 * `enterRegion` and `leaveRegion` bracket the *emission* - every block made between them is in the
 * region, which is a span rather than a per-block decision, and asking a caller to name the region
 * at each `addBlock` is how one of them ends up in the wrong one. `rejoinRegion` is separate from
 * both because the block it names does not exist while the region is open: a continuation rejoins
 * at the cleanup that follows the `yield`, which is emitted after the continuation and in the
 * enclosing region, and that ordering is the whole shape of the construct.
 */
ContinuationId Builder::enterRegion(Continuation::Kind kind, Continuation::Multiplicity multiplicity) {
    auto id = ContinuationId(U32(body.regions.size()));
    auto region = new (arena()) Continuation(id, kind, multiplicity, currentRegion);
    body.regions.push(arena(), region - base());

    currentRegion = id;
    return id;
}

void Builder::leaveRegion() {
    auto region = body.region(base(), currentRegion);
    assertTrue(region && region->parent.valid());
    currentRegion = region->parent;
}

void Builder::rejoinRegion(ContinuationId id, Block& destination) {
    auto region = body.region(base(), id);
    assertTrue(region && region->parent.valid());

    // Outside this region and not in it, which is what makes it a rejoin rather than an edge the
    // nesting rule would have accepted anyway.
    assertTrue(destination.region != id);
    region->destination = destination.index;
}

Local* Builder::addLocal(StringId name, Occurrence occurrence, LocationId source, bool writable) {
    auto local = new (arena()) Local(name, occurrence, source, writable);
    local->index = LocalId(body.locals.push(arena(), local - base()));

    // The region that is open, which is the frame that owns the storage - see `Local::region`. Here
    // rather than a parameter for the reason `addBlock` stamps the same field: it is a span the
    // builder is already inside, and a caller that had to name it is a caller that can name a
    // different one.
    local->region = currentRegion;

    /*
     * Storage this frame owes a teardown for has a place from the moment it exists.
     *
     * A drop is placed on a *path*, and a path exists because a place names the storage it releases
     * (`analyze_path.cpp`) - so a local no expression in the body ever mentions had nowhere to put
     * its teardown and was quietly leaked. That is the defect `fieldPlace` already fixes one level
     * down for the surviving fields of a partially moved record, and this is the same sentence about
     * the record itself.
     *
     * The one local that can reach it is a **parameter**: everything else is established by a
     * `write.init` through a place, so it has one by construction. `fn eat(->g: Guard): return` is
     * the shape - the call handed the guard over, the body never says the name, and both frames
     * therefore believed the other would release it. It is stated for every local rather than for
     * parameters alone because the rule is about what the storage owes and not about where it came
     * from; a construct that makes a local and does not write it is then not a second leak.
     *
     * Only where there is something to release. `isTrackedAt` is exactly what `buildPaths` asks
     * before it makes a path, so a local that gets a place here is one that gets a path, and a
     * scalar costs nothing.
     *
     * Of the *occurrence* and not of the type, because a qualified type argument is the case where
     * the two disagree: a `Maybe('v)` holds nothing it owns, so a local of one has nothing to
     * release and nothing a second reader could duplicate wrongly. Asking the type here and the
     * occurrence at the drop would give it a move path with no teardown at the end of it, which is
     * how it came to refuse a second use of a value that copies freely.
     */
    if(occurrence.contract.isOwned() && isTrackedAt(declBase(), occurrence)) {
        localPlace(*local);
    }

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

    if(occurrence.contract.isOwned() && isTrackedAt(declBase(), occurrence)) {
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
/*
 * `x.field` where `x`'s type is a variable and a requirement says it has one - `GenProperty`.
 *
 * A projection like `fieldPlace`'s and simpler in exactly one way: there is no field list to read,
 * so there is nothing to derive and nothing to do for the *sibling* fields. Partial ownership is why
 * `fieldPlace` walks them - moving `p.left` leaves `p.right` owing a release - and a requirement
 * names one component of a subject whose other components this body has no name for, so there are no
 * siblings here to give a path to.
 *
 * The occurrence is the requirement's declared result, qualified by the way the subject was reached:
 * a field of a record this frame only shares is a shared view, which is the one rule both kinds of
 * projection share and the reason `requalified` is called here too.
 */
Place* Builder::propertyPlace(Place& parent, U16 property, Occurrence declared) {
    assertTrue(declBase()[parent.occurrence.type]->kind == Type::Variable);

    auto occurrence = requalified(declared, parent.occurrence.contract);
    return body.place(base(), internPlace(Place::Property, &parent, property, occurrence,
                                          parent.writable));
}

/*
 * `xs[i]` - one element of a fixed array or of a window.
 *
 * **A projection of the container's own place, and never pointer arithmetic.** That is the whole of
 * this function and it is the correction the rewrite exists to make: the shipping stage reaches an
 * element as `borrowMut(self.run.items + index)`, so what the ownership passes see is a place rooted
 * in a raw address - and rule one there is that two pointer-rooted places never conflict. The two
 * consequences were both real. A `[T *n]` local's teardown was placed nowhere, because `rootLocal`
 * of an address is nothing and the local therefore never became owned; and `f(&xs[i], &xs[j])` was
 * accepted with no check of any kind, `xs[0], xs[0]` included, which is reachable as a
 * use-after-free in safe code. Rooted at the parent, `place.h`'s stated rule applies by
 * construction: "`Index` steps of one parent are not [disjoint] unless a separately justified rule
 * proves it".
 *
 * The element's qualification is the container's, which is the same sentence `fieldPlace` makes: an
 * element of an array this frame only shares is shared, and an element of one it owns is owned. The
 * *form* is dropped - `WholeValue` - because a window is a capability over the sequence and not over
 * one element: what `xs[i]` names is a `T`, and `T` has no extent to be a window of.
 *
 * No move path is interned for the elements the way `fieldPlace` interns a record's fields, and the
 * reason is that there is no *set* to intern: the index is a value, so "every element" is a run
 * whose length may be a const parameter, and a body that moved one of them out would leave the rest
 * in a state a path per element could not describe anyway. What holds instead is that a move out of
 * an element is refused - see `readPlace`.
 */
Place* Builder::elementPlace(Place& parent, Node& index) {
    auto element = elementTypeOf(declBase(), program, parent.occurrence.type);
    assertTrue(element != nullptr);

    auto held = parent.occurrence.contract;
    auto contract = Contract { held.access(), held.origin(), AccessForm::WholeValue, held.slot() };

    // The element's own shape is empty: `ArrayType::element` and a sequence's argument are bare
    // types, so there is no interior position for a loan argument to have renumbered.
    auto occurrence = Occurrence { element, contract, nullptr };

    return body.place(base(), internPlace(Place::Index, &parent, index.id.index, occurrence,
                                          parent.writable));
}

Place* Builder::fieldPlace(Place& parent, U16 field) {
    /*
     * The record the field is **on**, which is the occurrence's answer and not the type's.
     *
     * A window is `Array(a)` under a view contract and its bytes are `ArrayView(a)`'s, so the field
     * list a projection walks is the view's - `fieldRecordOf` is the one place that is decided, and
     * this is one of its readers. A view's fields are not writable places: a window describes
     * storage, and to change what it describes a body builds another one.
     */
    auto viewed = viewRecordOf(context, program, parent.occurrence) != nullptr;
    auto recordType = fieldRecordOf(context, program, parent.occurrence);
    assertTrue(recordType != nullptr);

    auto record = (RecordType*)declBase()[recordType];
    assertTrue(record->kind == Type::Record);
    assertTrue(field < record->fields.size());

    auto writable = parent.writable && !viewed;

    auto placeOf = [&](U16 index) {
        auto declared = record->field(declBase(), index);
        auto held = parent.occurrence.contract;

        /*
         * What this position *is*, and there are two sources of the answer in one order.
         *
         * The **declaration**'s contract says whether the record holds a member or refers to
         * somebody else's storage. The **occurrence**'s shape says the same thing where the use site
         * wrote it - a loan argument renumbering `Reader(a')`'s position into this signature's
         * scope, or a qualified type argument taking `Maybe('v)`'s payload away from the record - and
         * it is the more specific of the two. `fieldContract` is that order, written once.
         *
         * Which is also why the slot is not looked up separately any more: a shape's slot is already
         * numbered in the scope the parent was written in, so reading the contract *is* reading the
         * renumbering.
         */
        auto position = fieldContract(declBase(), viewed ? nullptr : parent.occurrence.shape,
                                      declared);
        auto member = Occurrence { declared.occurrence.type, position, declared.occurrence.shape };

        if(position.isOwned()) {
            return internPlace(Place::Field, &parent, index, requalified(member, held), writable);
        }

        /*
         * An external field: the slot, and then the object it names.
         *
         * The capability is the position's, narrowed by the way the record was reached - an `&'T`
         * field of a record this frame only shares is a shared view, because a shared borrow of a
         * record may not hand out exclusive access to what it refers to.
         */
        auto access = position.access();
        if(held.access() == Access::Shared && access == Access::Unique) access = Access::Shared;

        auto occurrence = requalified(member, Contract { access, position.origin(), position.form(),
                                                         position.slot() });

        auto reference = body.place(base(), internPlace(Place::Field, &parent, index, occurrence,
                                                        writable));
        return derefPlace(*reference, occurrence, writable && access == Access::Unique)->id;
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
     *
     * **One constructor's fields** and not every field the record declares. Two arms of a sum are
     * alternatives over one storage, so interning the others' would build move paths for values that
     * are not there - and a value of the arm this projection is inside is the only thing the match
     * that reached here proved. `constructorOf` is what the range comes from, and it is the whole
     * field list for a record with one constructor.
     */
    if(hasTeardown(declBase(), recordType) &&
       Shapes::isEmpty(shapeOf(declBase(), parent.occurrence))) {
        auto arm = record->cons.get(declBase(), record->constructorOf(declBase(), field));

        // In field order, the requested one included, because the interning order is what
        // `buildPaths` reads a record's children in - and that order is the order its fields are
        // released in. A walk that put the asked-for field first would release a record's members
        // back to front for no reason the author could see.
        auto wanted = PlaceId();
        for(U16 i = arm.first; i < U16(arm.first + arm.count); i++) {
            auto id = placeOf(i);
            if(i == field) wanted = id;
        }

        return body.place(base(), wanted);
    }

    return body.place(base(), placeOf(field));
}

/*
 * `x.#` - which constructor a sum currently is.
 *
 * The occurrence is the tag's own integer type, requalified by the way the record was reached, which
 * is the same rule every other projection is under: reading the tag of a record this frame shares is
 * a shared read. What makes it never an owned position is the type - an integer owes nothing - so a
 * path built for one places no teardown and a read of one copies rather than moves.
 */
Place* Builder::discriminantPlace(Place& parent) {
    auto tag = tagTypeOf(declBase(), program.types, parent.occurrence.type);
    assertTrue(tag);

    auto occurrence = requalified(owned(tag), parent.occurrence.contract);
    return body.place(base(), internPlace(Place::Discriminant, &parent, 0, occurrence,
                                          parent.writable));
}

Place* Builder::derefPlace(Place& parent, Occurrence occurrence, bool writable) {
    return body.place(base(), internPlace(Place::Deref, &parent, 0, occurrence, writable));
}

Place* Builder::addressPlace(Node& address, Occurrence occurrence, bool writable) {
    // The producing node's ID is the payload, so two reads through one pointer value are one place
    // and two reads through two are not. That is as much as interning can promise here: whether two
    // *different* addresses name one object is not a question this representation answers, which is
    // the whole of what makes a raw pointer unsafe.
    assertTrue(hasResult(address.kind));
    return body.place(base(), internPlace(Place::Address, nullptr, address.id.index, occurrence,
                                          writable));
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

    /*
     * A type with **one value** is that value, and never a reference to the node that computed it.
     *
     * The same rule `Resolver::readPlace` states for a place, on the other side of the seam: `{}`
     * occupies no bytes, so a function that answers one returns no register and a node whose result
     * is one produces nothing an operand could name. The value is the constant, which any consumer
     * of a `{}` can mint for itself - so this is where the node stops being the value's identity,
     * and it is here rather than at each producer so that a call, a block argument and a `const`
     * node all answer the same way.
     *
     * The producing node is still emitted and still runs: `nothing()` is a call whose effects are
     * the point even though its answer carries nothing. What changes is only what names its result.
     */
    auto& occurrence = valueOccurrence(node);
    if(occurrence.type && declBase()[occurrence.type]->kind == Type::Unit) {
        return constantOperand(addConstant(unitConstant(occurrence.type)), mode);
    }

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
 * `n` in an expression - the value of a const parameter, named rather than computed.
 *
 * Owned and trivial like a literal, because that is what it becomes: the number it stands for is
 * decided by the substitution the body is converted under, and everything about the value except
 * *which* number is settled here.
 */
Node* Builder::addGenValue(LocationId source, DeclPtr<Type> variable) {
    auto& declared = *(VariableType*)declBase()[variable];
    assertTrue(declared.isConst());

    auto node = new (arena()) NodeGenValue(source, variable);
    node->occurrence = owned(declared.constType);
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
Node* Builder::addRead(LocationId source, Place& place, UseMode mode, Request wanted) {
    // The mode *is* the request read against this place - `readMode`, which is the one place the
    // four answers are decided. Both travel because the resolver needs the mode in hand to report
    // the two positions it refuses, and this is the assertion that says they cannot disagree.
    assertTrue(mode == readMode(declBase(), place, wanted));

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
         * The two that produce a loan, whose origin is the storage's or the *position's* - see
         * `grantedContract`. A borrow of `p: a'T` is still in `a`, one of a local is bounded by this
         * frame, and one taken for a position that published is published however ordinary the
         * storage it came from was.
         */
        case UseMode::BorrowShared:
            contract = grantedContract(place.occurrence.contract, Access::Shared, wanted);
            break;
        case UseMode::BorrowUnique:
            contract = grantedContract(place.occurrence.contract, Access::Unique, wanted);
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
     * duplication and owes no teardown - `isTrackedAt` is false for exactly those - so there is no
     * lifetime for a lineage to be about. The day a *non-trivial* copy exists it is a second object
     * and mints one here, which is the case this arm is shaped to take.
     */
    if(moved.valid()) body.nodeObject.set(base(), node->id.index, moved.index);

    return node;
}

Node* Builder::addBinary(LocationId source, BinaryOp op, Node& lhs, Node& rhs) {
    auto type = valueType(lhs);
    assertTrue(sameType(type, valueType(rhs)));
    assertTrue(operandTypeAllowed(program, binaryCategory(op), type));

    auto node = new (arena()) NodeBinary(source, op, resultOf(lhs, UseMode::Inspect),
                                         resultOf(rhs, UseMode::Inspect));
    node->occurrence = owned(binaryResult(context, program, op, type));
    return append(node);
}

/*
 * A pointer moved by a byte offset - `p + n`, after the stride has been applied.
 *
 * A `Binary` node whose left operand is the pointer and whose result is the pointer's own
 * occurrence, and not an integer add between two bitcasts, for one reason: **provenance**. The
 * pointee origin a pointer carries - `%source'a`'s one interior position - has to survive the
 * arithmetic, or `borrow(advance(values(xs), i))` would be a borrow of nothing anyone can name.
 * `analyze_loan.cpp` reads the operand's pointee origins into the result's; an integer in the middle
 * has no position for them to travel in. The category check is skipped on purpose - `op.cpp` keeps
 * pointer arithmetic out of `Num` - and this is the one producer that makes such a node.
 */
Node* Builder::addDisplace(LocationId source, BinaryOp op, Node& pointer, Node& offset,
                           Occurrence result) {
    assertTrue(op == BinaryOp::Add || op == BinaryOp::Sub);
    assertTrue(declBase()[valueType(pointer)]->kind == Type::Pointer);
    assertTrue(declBase()[valueType(offset)]->kind == Type::Int);
    assertTrue(declBase()[result.type]->kind == Type::Pointer);

    auto node = new (arena()) NodeBinary(source, op, resultOf(pointer, UseMode::Inspect),
                                         resultOf(offset, UseMode::Inspect));
    node->occurrence = result;
    return append(node);
}

Node* Builder::addUnary(LocationId source, UnaryOp op, Node& value) {
    auto type = valueType(value);
    auto retyping = op == UnaryOp::Convert || op == UnaryOp::Bitcast;
    assertTrue(!retyping && operandTypeAllowed(program, unaryCategory(op), type));

    auto node = new (arena()) NodeUnary(source, op, resultOf(value, UseMode::Inspect));
    node->occurrence = owned(type);
    return append(node);
}

/*
 * What may stand at either end of a retyping node, which is where a **type parameter** is admitted
 * and nowhere else.
 *
 * A body compiled once crosses between a type it can see and one it cannot: the `fromInt` of the
 * generated `Num(Vec(a, n))` converts a `Long` to the lane `a`, which is a variable until a
 * specialization binds it. That is not a hole in the typing - what may stand at `a` is the head's
 * own requirement, `FromInt(a)`, discharged where the instance was selected.
 *
 * `addUnary`'s assertion stays strict, and the split is the point: `neg` and `not` compute at a type
 * they were handed, so a variable there would be a body assuming something about `a` that no
 * constraint promised. A retyping node computes nothing - it restates a value at the type its
 * position named, and the position is what has already been checked.
 */
static bool retypeEnd(Program& program, UnaryCategory category, DeclPtr<Type> type) {
    if((*program.decl)[type]->kind == Type::Variable) return true;
    return operandTypeAllowed(program, category, type);
}

// The two retyping unary nodes - one body, since the difference between them is the enumerator and
// what the manifestation makes of it. Both take their result type from the position rather than
// from the operand, which is the whole reason neither goes through `addUnary`.
Node* Builder::addRetype(LocationId source, UnaryOp op, Node& value, DeclPtr<Type> to) {
    assertTrue(retypeEnd(program, unaryCategory(op), valueType(value)));
    assertTrue(retypeEnd(program, unaryCategory(op), to));

    auto node = new (arena()) NodeUnary(source, op, resultOf(value, UseMode::Inspect));
    node->occurrence = owned(to);
    return append(node);
}

/*
 * A view record, as the occurrence it manifests - `UnaryOp::Represent`.
 *
 * The pairing is checked here and nowhere else: the operand's type has to be the record the view
 * table pairs with the result's type at the result's access and form, which `viewRecordOf` answers.
 * Inspected rather than taken, because a view record is trivially copyable by `checkViewRecord`'s
 * rule and the result shares its storage - what the result carries is the position's contract, and
 * the loan it names is the position's to state.
 */
Node* Builder::addRepresent(LocationId source, Node& record, Occurrence view) {
    assertTrue(view.contract.form() == AccessForm::View);
    assertTrue(viewRecordOf(context, program, view) == valueType(record));

    auto node = new (arena()) NodeUnary(source, UnaryOp::Represent, resultOf(record, UseMode::Inspect));
    node->occurrence = view;
    return append(node);
}

Node* Builder::addConvert(LocationId source, Node& value, DeclPtr<Type> to) {
    return addRetype(source, UnaryOp::Convert, value, to);
}

Node* Builder::addBitcast(LocationId source, Node& value, DeclPtr<Type> to) {
    return addRetype(source, UnaryOp::Bitcast, value, to);
}

/*
 * A function value - the note in build.h.
 *
 * One node for a named function and for a lifted lambda, because they are one value: what differs is
 * only whether the environment word points at anything, and that is not something a holder of a pair
 * may ask.
 */
Node* Builder::addFunValue(LocationId source, DeclPtr<Function> callee, DeclPtr<Type> type,
                           Node* environment) {
    auto node = new (arena()) NodeFunValue(source, callee);

    /*
     * Owned at the root, and external at the one position inside it - `Shapes::closure`.
     *
     * The root has to stay owned, and the reason is physical rather than philosophical: the pair is
     * sixteen bytes wherever it goes, and a borrowed occurrence of a type that lives in memory
     * manifests as *its address* (`isReferenceValue`). A closure described as a borrow would
     * therefore be one pointer where the language has two words, and every call through it would
     * load the wrong thing. What refers to somebody else's storage is the environment word, which is
     * exactly what an interior position is for.
     */
    node->occurrence = Occurrence { type, ownedContract(),
                                    environment ? program.shapes.closure() : nullptr };

    if(environment) {
        node->capturing = true;
        node->environment = resultOf(*environment, UseMode::Inspect);
    }

    append(node);

    // A function value is tracked for the reason `ownershipOf` gives the kind: it refuses
    // duplication, so it is followed like any other value whose uses are moves.
    body.nodeObject.set(base(), node->id.index, mintObject(type, source).index);
    return node;
}

// The argument is in build.h, beside the declaration.
Node* Builder::addAddressOf(LocationId source, Node& of, Occurrence pointer) {
    assertTrue(!valueOccurrence(of).contract.isOwned());
    assertTrue(declBase()[pointer.type]->kind == Type::Pointer);

    auto node = new (arena()) NodeAddress(source, resultOf(of, UseMode::Inspect));
    node->occurrence = pointer;

    append(node);
    return node;
}

// The argument is in build.h, beside the declaration.
Node* Builder::addMetric(LocationId source, TypeMetric metric, DeclPtr<Type> of, DeclPtr<Type> as) {
    auto node = new (arena()) NodeMetric(source, metric, of);
    node->occurrence = owned(as);

    append(node);
    return node;
}

// The argument is in build.h, beside the declaration.
Node* Builder::addNative(LocationId source, NativeOp op, Node* const* arguments, Size count,
                         DeclPtr<Type> result) {
    auto node = new (arena()) NodeNative(source, op);
    node->occurrence = owned(result);
    node->arguments.reserve(arena(), count);

    for(Size i = 0; i < count; i++) {
        node->arguments.push(arena(), resultOf(*arguments[i], UseMode::Inspect));
    }

    append(node);
    return node;
}

// The argument is in build.h, beside the declaration.
Node* Builder::addAtomic(LocationId source, AtomicOp op, AtomicOrder order, AtomicOrder failure,
                         bool weak, Node* const* arguments, Size count, DeclPtr<Type> result) {
    assertTrue(count == atomicOperandCount(atomicForm(op)));

    auto node = new (arena()) NodeAtomic(source, op, order);
    node->failure = failure;
    node->weak = weak;
    node->occurrence = owned(result);
    node->arguments.reserve(arena(), count);

    for(Size i = 0; i < count; i++) {
        // The location of an `IntoValue`, and nothing else - see build.h, where the reason the one
        // handover in this family is exactly that operand is written.
        auto mode = (i == 0 && op == AtomicOp::IntoValue) ? UseMode::Take : UseMode::Inspect;
        node->arguments.push(arena(), resultOf(*arguments[i], mode));
    }

    append(node);
    return node;
}

Node* Builder::addAtomicOk(LocationId source, Node& cas, DeclPtr<Type> boolean) {
    auto node = new (arena()) NodeAtomicOk(source);
    node->occurrence = owned(boolean);
    node->from = resultOf(cas, UseMode::Inspect);

    append(node);
    return node;
}

Node* Builder::addFma(LocationId source, Node& lhs, Node& rhs, Node& addend) {
    auto type = valueType(lhs);
    assertTrue(sameType(type, valueType(rhs)) && sameType(type, valueType(addend)));
    assertTrue(operandTypeAllowed(program, UnaryCategory::Float, type));

    auto node = new (arena()) NodeFma(source, resultOf(lhs, UseMode::Inspect),
                                      resultOf(rhs, UseMode::Inspect),
                                      resultOf(addend, UseMode::Inspect));
    node->occurrence = owned(type);

    append(node);
    return node;
}

Node* Builder::addSelect(LocationId source, Node& condition, Node& whenTrue, Node& whenFalse) {
    auto type = valueType(whenTrue);
    assertTrue(sameType(type, valueType(whenFalse)));

    auto node = new (arena()) NodeSelect(source, resultOf(condition, UseMode::Inspect),
                                         resultOf(whenTrue, UseMode::Inspect),
                                         resultOf(whenFalse, UseMode::Inspect));
    node->occurrence = owned(type);

    append(node);
    return node;
}

// The four are in build.h, beside their declarations.
Node* Builder::addVecSplat(LocationId source, Node& from, DeclPtr<Type> vector) {
    assertTrue(declBase()[vector]->kind == Type::Vector);
    assertTrue(sameType(valueType(from), ((VectorType*)declBase()[vector])->lane));

    auto node = new (arena()) NodeVecSplat(source, resultOf(from, UseMode::Inspect));
    node->occurrence = owned(vector);

    append(node);
    return node;
}

Node* Builder::addVecLane(LocationId source, Node& from, U16 lane) {
    auto vector = valueType(from);
    assertTrue(lane < vectorLanes(declBase(), vector));

    auto node = new (arena()) NodeVecLane(Node::VecLane, source, resultOf(from, UseMode::Inspect),
                                          lane);
    node->occurrence = owned(laneTypeOf(declBase(), vector));

    append(node);
    return node;
}

Node* Builder::addVecWithLane(LocationId source, Node& from, U16 lane, Node& value) {
    auto vector = valueType(from);
    assertTrue(lane < vectorLanes(declBase(), vector));
    assertTrue(sameType(valueType(value), laneTypeOf(declBase(), vector)));

    auto node = new (arena()) NodeVecLane(Node::VecWithLane, source,
                                          resultOf(from, UseMode::Inspect), lane);
    node->value = resultOf(value, UseMode::Inspect);
    node->occurrence = owned(vector);

    append(node);
    return node;
}

Node* Builder::addVecShuffle(LocationId source, Node& left, Node& right, const U8* pattern,
                             Size count, DeclPtr<Type> result) {
    assertTrue(count == vectorLanes(declBase(), result));

    auto node = new (arena()) NodeVecShuffle(source, resultOf(left, UseMode::Inspect),
                                             resultOf(right, UseMode::Inspect));
    node->occurrence = owned(result);
    node->pattern.reserve(arena(), count);
    for(Size i = 0; i < count; i++) node->pattern.push(arena(), pattern[i]);

    append(node);
    return node;
}

Node* Builder::addVecReduce(LocationId source, Node& from, ReduceOp reduce, DeclPtr<Type> result) {
    auto node = new (arena()) NodeVecReduce(source, resultOf(from, UseMode::Inspect), reduce);
    node->occurrence = owned(result);

    append(node);
    return node;
}

/*
 * A call through a function value - the note in build.h.
 *
 * The target is read `Inspect`: calling a function value neither consumes it nor takes a loan of
 * anything inside it. What the call reads is two words that are *copied* out of the pair - the code
 * address and the environment pointer - and the environment stays the value's, which is the same
 * relationship `Take` would have got wrong in both directions.
 */
Node* Builder::addCallValue(LocationId source, Node& target, Node* const* arguments, Size count) {
    auto& shape = *(FunType*)declBase()[valueType(target)];
    assertTrue(count == shape.params.size());

    auto node = new (arena()) NodeCall(source, nullptr);
    node->occurrence = shape.result;
    node->target = resultOf(target, UseMode::Inspect);
    node->arguments.reserve(arena(), count);

    for(Size i = 0; i < count; i++) {
        auto declared = shape.params.get(declBase(), U32(i)).occurrence;
        assertTrue(sameType(valueType(*arguments[i]), declared.type));
        node->arguments.push(arena(), resultOf(*arguments[i],
                                               argumentMode(declared.contract, SourceKind::Result)));
    }

    append(node);

    if(shape.result.contract.isOwned() && isTrackedAt(declBase(), shape.result)) {
        body.nodeObject.set(base(), node->id.index, mintObject(shape.result.type, source).index);
    }

    return node;
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
Node* Builder::addCall(LocationId source, Function& callee, Node* const* arguments, Size count,
                       const Substitution& with) {
    assertTrue(count == callee.args.size());
    assertTrue(with.isEmpty() || with.owner == callee.gen);

    auto result = substituteOccurrence(context, program, callee.result, with);

    auto node = new (arena()) NodeCall(source, &callee - declBase());
    node->occurrence = result;
    node->arguments.reserve(arena(), count);
    node->typeArguments.reserve(arena(), with.count);

    for(Size i = 0; i < with.count; i++) node->typeArguments.push(arena(), with.arguments[i]);

    for(Size i = 0; i < count; i++) {
        auto& parameter = *declBase()[callee.args.get(declBase(), U32(i))];
        auto declared = substituteType(context, program, parameter.occurrence.type, with);
        assertTrue(sameType(valueType(*arguments[i]), declared));

        auto mode = argumentMode(parameter.occurrence.contract, SourceKind::Result);
        node->arguments.push(arena(), resultOf(*arguments[i], mode));
    }

    append(node);

    // An owned result is an object this frame receives and is answerable for. A borrowed one is
    // somebody else's, and `Value::loan` is where *that* relation is recorded. Asked of the
    // substituted type, because what this frame receives is what the call actually answers.
    if(result.contract.isOwned() && isTrackedAt(declBase(), result)) {
        body.nodeObject.set(base(), node->id.index, mintObject(result.type, source).index);
    }

    return node;
}

/*
 * A call typed against a **callable requirement** rather than against the callee's own signature -
 * `NodeCall::genCallable`.
 *
 * Everything a call needs is here and none of it comes from the callee: the parameter types and the
 * result are the requirement's, and so are the modes. A requirement writes no conventions - see
 * `readCallableRequirement`, which refuses one - so every parameter binds and the result is owned,
 * which is what the two contracts below say.
 *
 * The type arguments are deliberately empty. Which arguments the callee is reached at depends on the
 * substitution the *enclosing* body is compiled under and on the callee's own signature, and neither
 * is known here - `callTarget` binds them by matching, at the point the substitution exists.
 */
Node* Builder::addRequiredCall(LocationId source, DeclPtr<Function> callee, U16 requirement,
                               DeclPtr<Type> result, Node* const* arguments, Size count) {
    auto node = new (arena()) NodeCall(source, callee);
    node->occurrence = owned(result);
    node->genCallable = requirement;
    node->arguments.reserve(arena(), count);

    auto mode = argumentMode(borrowedContract(Access::Shared), SourceKind::Result);
    for(Size i = 0; i < count; i++) node->arguments.push(arena(), resultOf(*arguments[i], mode));

    append(node);

    if(isTracked(declBase(), result)) {
        body.nodeObject.set(base(), node->id.index, mintObject(result, source).index);
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
Node* Builder::addAggregate(LocationId source, DeclPtr<Type> record, U16 constructor,
                            Node* const* components, Size count, DeclPtr<OwnershipSchema> shape) {
    auto kind = declBase()[record]->kind;
    assertTrue(kind == Type::Record || kind == Type::Array);

    // A quantified count has no components to check against, and nothing builds one: a literal
    // states its length by being written, so a `[T *n]` value is only ever produced by a call.
    assertTrue(kind == Type::Array ||
               constructor < ((RecordType*)declBase()[record])->cons.size());
    assertTrue(count == aggregateComponentCount(declBase(), record, constructor));

    auto node = new (arena()) NodeAggregate(source);

    /*
     * Owned, with whatever the destination said is inside it.
     *
     * The root is this frame's whatever the interior is - `Maybe('v)` is an owned `Maybe` holding a
     * reference, which is §2.4's first line - so the shape is the second half of the occurrence and
     * not a weakening of the first.
     */
    node->occurrence = Occurrence { record, ownedContract(), shape };
    node->constructor = constructor;
    node->components.reserve(arena(), count);

    for(Size i = 0; i < count; i++) {
        auto declared = aggregateComponent(declBase(), record, constructor, i);
        assertTrue(sameType(valueType(*components[i]), declared.type));

        /*
         * The contract this position is filled under - the destination's where it named one, and the
         * field's own otherwise.
         *
         * The field's own is owned for a member and a loan for an external position, so a component
         * filling `held: a'Guard` is borrowed into the record rather than taken. A *qualified type
         * argument* is the same statement made at the use instead of at the declaration, and it has
         * to be read here for the same reason: what the component is taken under is what the
         * position holds, and there is one position.
         */
        auto contract = aggregateComponentContract(declBase(), record, constructor, i, shape);
        auto mode = argumentMode(contract, SourceKind::Result);
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
    if(isTrackedAt(declBase(), node->occurrence)) {
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
        /*
         * A local, or a field of one - and a **global**, which is the one slot outside this frame
         * that has a first write at all.
         *
         * The root module's top level is where a dynamic global's storage begins
         * (Analysis-Initialization.md stage B), and the declaring write is an `Init` for the reason
         * every other `Init` is one: what is already in the slot is nothing, so there is nothing to
         * pre-drop and nothing to release. That the slot is not this frame's changes none of it -
         * `analyze_path` already answers "not this function's to release" for a global root, so the
         * write establishes storage the program owns and no move path is opened for it.
         *
         * A **raw address** is the third, and it is the one root for which the claim is a promise
         * rather than a fact: `Place::Address` is where the checked model stops, so what says the
         * slot held nothing is whoever produced the pointer. One thing does - a growable array
         * literal, filling the slots the library just told it are live - and it is the same shape
         * `analyze_path` already answers for, opening no move path and placing no drop, because
         * storage reached through a pointer is never this frame's to release.
         */
        auto root = &place;
        while(root->parent) root = base()[root->parent];
        assertTrue(root->kind == Place::Local || root->kind == Place::Global ||
                   root->kind == Place::Address);
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

/*
 * One outgoing edge, filled - the half `jump` and `yield` are the same.
 *
 * Here rather than written twice because the lineage rule below is the interesting half and a rule
 * stated twice is a rule that will be changed once. What the two kinds differ in is what the edge
 * *means* - an ordinary successor against handing control to a continuation region - and that is the
 * node kind, not the edge.
 */
void Builder::fillEdge(Edge& edge, Block& target, const Operand* arguments, Size count) {
    // The successor's parameter list is what an edge has to satisfy, and the count is checkable the
    // moment the edge is built rather than at the next verifier boundary.
    assertTrue(count == target.arguments.size());

    edge.target = &target - base();
    for(Size i = 0; i < count; i++) edge.arguments.push(arena(), arguments[i]);

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
}

Node* Builder::addJump(LocationId source, Block& target, const Operand* arguments, Size count) {
    auto node = new (arena()) NodeJump(source);
    fillEdge(node->to, target, arguments, count);
    return append(node);
}

/*
 * `break` out of the region this block is in - the same edge as falling out, under the kind that
 * says it is the last one.
 *
 * No arguments, and not because none happened to be wanted: an edge that leaves a continuation
 * carries nothing, because the block on the far side is another function's under the CPS lowering.
 * The one thing it may carry is the exit signal, which is the kind.
 */
Node* Builder::addBreak(LocationId source, Block& target) {
    auto node = new (arena()) NodeJump(source);
    node->kind = Node::Break;
    fillEdge(node->to, target, nullptr, 0);
    return append(node);
}

/*
 * Handing control to a continuation region - the callee-side half of a lens call.
 *
 * An edge like any other, which is the point of §10.1: the values a `yield` hands over travel as
 * the region entry's block arguments under the modes `argumentMode` decided, so liveness, the loan
 * extents and drop elaboration walk it without knowing what a lens is. What is not like any other
 * edge is recorded on the *region* rather than here - it is entered at this point and rejoins at its
 * destination - so the node carries the region it opens and nothing else.
 */
Node* Builder::addLensCall(LocationId source, Function& callee, ContinuationId region,
                           Block& entry, Node* const* arguments, Size count,
                           const Substitution& with, Block* otherwise) {
    assertTrue(count == callee.writtenArity());
    assertTrue(with.isEmpty() || with.owner == callee.gen);

    // The kind is the callee's signature and not the caller's choice - `checkLensCall` restates it.
    assertTrue((otherwise != nullptr) == (callee.failure != nullptr));

    auto node = new (arena()) NodeLensCall(source, &callee - declBase(), region);
    if(otherwise) node->kind = Node::LensTry;
    node->arguments.reserve(arena(), count);
    node->typeArguments.reserve(arena(), with.count);

    for(Size i = 0; i < with.count; i++) node->typeArguments.push(arena(), with.arguments[i]);

    /*
     * The written arguments, under the modes the *signature* decided - `argumentMode`, read here
     * for the third time and from the same place. A lens's arguments are ordinary arguments: what
     * differs is only how long the call lasts, and that is the region's business rather than the
     * operands'.
     */
    for(Size i = 0; i < count; i++) {
        auto& parameter = *declBase()[callee.args.get(declBase(), U32(i))];
        auto declared = substituteType(context, program, parameter.occurrence.type, with);
        assertTrue(sameType(valueType(*arguments[i]), declared));

        auto mode = argumentMode(parameter.occurrence.contract, SourceKind::Result);
        node->arguments.push(arena(), resultOf(*arguments[i], mode));
    }

    /*
     * The edge, filled by hand - one of the two edges in the IR that supply no arguments to a target
     * that declares one, the other being a `lens.try`'s skip edge below.
     *
     * `fillEdge` is the wrong shape here for a reason rather than by accident: what arrives at the
     * region's entry is produced *inside* the call this terminator is, so there is no value in this
     * block for the edge to name. What the parameter is checked against instead is
     * `Continuation::handover` - the declaration - which is what `checkLensCall` does and what an
     * opaque callee gives a caller in place of storage.
     */
    node->to.target = &entry - base();

    /*
     * The skip edge, filled by hand for the reason the entry's is: what travels it is the callee's
     * failure value, which is produced inside this call and has no name in this block.
     */
    if(otherwise) {
        node->otherwise.target = otherwise - base();
        node->reason = owned(substituteType(context, program, callee.failure, with));
    }

    auto opened = body.region(base(), region);
    assertTrue(opened && opened->kind == Continuation::Handover);
    assertTrue(opened->entry == entry.index);

    return append(node);
}

Node* Builder::addBranch(LocationId source, Operand condition, Block& whenTrue, Block& whenFalse,
                         const Operand* arguments, Size count) {
    // Two arms that are one block is a jump written as a branch. Refused here rather than folded,
    // because the lower IR asserts the same thing and a resolver that produced one would be finding
    // that out three phases later.
    assertTrue(&whenTrue != &whenFalse);

    auto node = new (arena()) NodeBranch(source, condition);
    fillEdge(node->whenTrue, whenTrue, arguments, count);
    fillEdge(node->whenFalse, whenFalse, arguments, count);
    return append(node);
}

Node* Builder::addUnreachable(LocationId source) {
    return append(new (arena()) NodeUnreachable(source));
}

void Builder::pruneUnreachable() {
    auto count = body.blocks.size();

    SmallArray<U8, 32> reached;
    for(U32 i = 0; i < count; i++) reached.push(0);

    SmallArray<U32, 32> pending;
    reached[0] = 1;
    pending.push(0);

    while(pending.size()) {
        auto index = pending[pending.size() - 1];
        pending.resize(U32(pending.size() - 1));

        auto block = body.block(base(), BlockId(index));
        auto nodes = block->nodes.size();
        if(!nodes) continue;

        // A block with no terminator has no successors to follow. `finish` gives every one of them
        // a terminator before this runs, so what is left here is a body that failed earlier.
        auto last = base()[block->nodes.get(base(), nodes - 1)];
        if(!isTerminator(last->kind)) continue;

        forEachEdge(*last, [&](Edge& edge) {
            if(!edge.target) return;

            auto target = base()[edge.target]->index.index;
            if(reached[target]) return;

            reached[target] = 1;
            pending.push(target);
        });
    }

    // Old index to new, compacting as it goes. `Block::index` is the block's own answer to where it
    // is, so it moves with it and nothing else has to be told.
    SmallArray<U32, 32> moved;
    U32 next = 0;

    for(U32 i = 0; i < count; i++) {
        if(!reached[i]) {
            moved.push(U32(-1));
            continue;
        }

        moved.push(next);

        auto pointer = body.blocks.get(base(), i);
        body.blocks.set(base(), next, pointer);
        base()[pointer]->index = BlockId(next);
        next++;
    }

    if(next == count) return;
    body.blocks.count = next;

    auto renumber = [&](BlockId id) {
        if(!id.valid() || moved[id.index] == U32(-1)) return BlockId();
        return BlockId(moved[id.index]);
    };

    for(U32 i = 0; i < body.regions.size(); i++) {
        auto region = body.region(base(), ContinuationId(i));
        region->entry = renumber(region->entry);
        region->destination = renumber(region->destination);
    }
}

void Builder::seal() {
    assertTrue(!body.sealed);

    pruneUnreachable();

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
