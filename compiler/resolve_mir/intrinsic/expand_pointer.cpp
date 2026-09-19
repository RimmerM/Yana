#include "expand_internal.h"

namespace mir {

using namespace intrinsic;

/*
 * The raw pointer - `pointerLoad` and `pointerStore` in `op.def`.
 *
 * Both are **places** rather than operations, which is why they are the first two roles this stage
 * implements: the MIR already has an address root and a read and a write through one, so the whole
 * of each expansion is naming the place and reusing the node every other access uses. Nothing about
 * ownership, loans or drop elaboration needs a case for them - a read of an address root is a read.
 *
 * `doc/spec/native.md` is where the safety argument stops: a raw pointer carries no lifetime, no
 * exclusivity and no promise that what it names is initialized. `Place::Address` is a root with no
 * parent for exactly that reason, and it is the only place in this stage where the checked model is
 * left behind.
 */

/*
 * The storage an operand that arrived as a *value* does not have.
 *
 * `addAddressOf` takes the value a borrowing read produced, because an address is the address of
 * somewhere and an owned node result is nowhere - the builder asserts it. That leaves the case where
 * the argument really is only a value, and this is what the language does with it: an unnamed local
 * of the frame, written once, and the address is that slot's. `resolve/expr_construct.cpp`'s
 * `materialize` is the shipping stage's same answer, and `test/resolve/Global.yana` is where the
 * meaning is written down - the address of a constant is the address of a temporary holding its
 * value, "which is the only reading available for a value that cannot be assigned to".
 *
 * Either declaration reaches it. `lib/Native/Pointer.yana` writes `fn addressOf(it: a) -> %a`, so a
 * trivially copyable argument is copied into the call and every call of it lands here; written
 * `it: source'a` an argument that names storage would arrive as a loan of it and only a literal or a
 * call result would land here. The published spelling says *whose* storage the pointer is in and
 * the library deliberately does not use it (`test/resolve/Global.yana` is what pins the other
 * meaning); what this function supplies is that there *is* storage, which both spellings need and
 * neither can state.
 *
 * The read is the **declared parameter's own request, published**, and both halves of that are
 * load-bearing. Published, because an unpublished request is answered `CopyTrivial` at every scalar
 * and a copy is the very step that lost the storage on the way in. The declaration's, because the
 * loan then carries the origin the signature named and is the *same* loan `addressOf(v)` of a name
 * takes - so the two paths into this role differ in where the storage came from and in nothing the
 * checker can see afterwards. Taking a fresh shared request instead would leave a temporary holding
 * a call-bounded loan where a name holds a published one, which is a difference nothing wanted.
 *
 * The slot is writable, because the pointer handed back is a `%a` and a store through one reaches
 * it - `test/resolve/Global.yana`'s `addressOfConstant` is that program. Nothing checks the store; a
 * `Place::Address` is a root with no parent by construction, which is where `doc/spec/native.md`
 * stops. The capability is on the storage so that the elaboration below the checker agrees.
 */
static Node* materialize(Builder& builder, const Occurrence& declared, Node& value,
                         LocationId source) {
    auto local = builder.addLocal(StringId(), owned(valueOccurrence(value).type), source, true);
    auto place = builder.localPlace(*local);

    builder.addWrite(source, *place, value, Destination::Init);

    // The declared position's interior shape is dropped: it describes what a value *filling that
    // position* holds inside it, and this read is of a slot this function just filled with the whole
    // value, which owns its own interior.
    auto wanted = requestOf(declared);
    wanted.published = true;
    wanted.shape = nullptr;

    /*
     * And the loan is **exclusive**, whatever the declaration asked for.
     *
     * `published` alone is not enough and the reason is the manifestation: a shared, call-bounded
     * loan of a trivially copyable scalar *is* the scalar, in a register (`isReferenceValue`), so
     * `addressOf(9)` under `lib`'s unpublished spelling read the slot back as a number and the SSA
     * conversion reported the address of a value that is not reached through storage. An exclusive
     * loan of a slot this function just made is reached through it, which is the same thing the
     * paragraph above says about the capability from the other end - the pointer handed back is a
     * `%a` and a store through one reaches this storage.
     */
    wanted.access = Access::Unique;

    return builder.addRead(source, *place, readMode(*builder.program.decl, builder.base(), builder.body, *place, wanted), wanted);
}

/*
 * `addressOf(x)` - the address of the storage a value occupies.
 *
 * One node, over a place: `fn addressOf(it: source'a) -> %source'a` borrows and publishes, so the
 * ordinary call path has already read the argument as a loan of a place and the origin environment
 * already knows the pointer is in `source`. Written `it: a` the position is an ordinary unpublished
 * one, `readMode` answers `CopyTrivial` at every scalar, and what arrives is a value rather than a
 * loan - `materialize` above is what that gets, and why the two library spellings both work.
 *
 * The occurrence is the *declared result's*, substituted, exactly as `pointerLoad` below takes its
 * pointee from the result: the two are the same type by `op.def`'s `pointerTo(0)`, and reading it
 * off the position the value fills is what carries the published interior origin as well as the
 * type.
 */
Node* expandAddressOf(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source) {
    auto base = *builder.program.decl;
    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);
    auto& of = *arguments[0];

    if(valueOccurrence(of).contract.isOwned()) {
        /*
         * The storage the argument was *copied out of*, where the copy is one this position made.
         *
         * `let &status = 0 :: Int` followed by `waitProcess(id, addressOf(status), 0)` is the
         * program, and under `lib`'s unpublished spelling the ordinary argument path answers it
         * `CopyTrivial` - so what reaches here is a scalar in a register and the address of a slot
         * holding a *copy* of the exit code, which the callee then writes and nobody reads. The
         * shipping stage does not do that: `resolve/expr_construct.cpp`'s `materialize` asks
         * `findPlace` first and hands back the storage the value was loaded from. This is that same
         * question, and it is what keeps the two stages saying one thing about one program.
         *
         * **Storage this frame owns**, and that is the line rather than an economy: "the memory a
         * pointer names is always mutable, the pointer itself is not" is
         * `lib/Native/Pointer.yana`'s own sentence, so `let node = Node {...}` followed by
         * `copyMemory(addressOf(node), ...)` writes that record - which is
         * `test/resolve/Pointer.yana`'s `blockOperations`, and what the shipping stage's `findPlace`
         * has always answered. Whether the *binding* may be assigned is a different question and was
         * the test here until 2026-09-07; it refused a plain `let` whose storage is this frame's and
         * addressed a copy of it instead.
         *
         * The **occurrence's access** is what decides, because the loan below is exclusive: a `%a`
         * is an address a store reaches, and an address is what a shared loan of a scalar is
         * precisely not (`isReferenceValue`). A place this frame only *shares* - a borrowed
         * parameter, a field of one - has no exclusive loan to give, so it keeps the copy, which is
         * also the only reading available for storage the callee does not own. An owned local and a
         * `&` parameter both do, which is `viaBorrowMut` in `intrinsic/Pointer.yana` and
         * `blockOperations` in `Pointer.yana` - the two ends this line has to admit.
         *
         * What `test/resolve/Global.yana` records for `addressOf` of a *constant* survives this, and
         * it was never the writability that answered it: an immutable global whose initializer is a
         * scalar constant reads **as** that constant (`readPlace`, and `Local::initial` one scope
         * down), so there is no place here at all and the copy below is what it gets.
         */
        if(of.kind == Node::Read) {
            auto& read = (NodeRead&)of;

            if(read.place.source() == SourceKind::Place) {
                auto place = builder.body.place(builder.base(), read.place.placeId());

                if(place->occurrence.contract.access() != Access::Shared) {
                    auto wanted = Request { Access::Unique, true, kNoOriginSlot,
                                            AccessForm::WholeValue };
                    auto mode = readMode(*builder.program.decl, builder.base(), builder.body,
                                         *place, wanted);

                    return builder.addAddressOf(source, *builder.addRead(source, *place, mode, wanted),
                                                result);
                }
            }
        }

        auto declared = substituteOccurrence(builder.context, builder.program,
                                             base[callee.args.get(base, 0)]->occurrence, with);
        return builder.addAddressOf(source, *materialize(builder, declared, of, source), result);
    }

    return builder.addAddressOf(source, of, result);
}

/*
 * `borrow(p)` and `borrowMut(p)` - the one place the checked world is re-entered from the unchecked
 * one.
 *
 * A read of the address root, exactly as `*p` below is, and the whole of the difference is the mode:
 * `*p` asks for an owned value and these ask for a loan. What makes the loan mean anything is again
 * the declaration - `fn borrow(it: %source'a) -> source'a` - so the result is in the caller's origin
 * set and gets the caller's extent instead of the call's, which is `op.def`'s two shape axes and why
 * they are two.
 *
 * Nothing here verified that the pointer is valid, that what it names is initialized, or that
 * nothing else writes through it while the borrow is live. That is `Native`'s to be unsafe about and
 * `doc/spec/native.md` is where it says so; what this function is responsible for is that the loan
 * the checker then reasons about is the one the signature promised.
 */
Node* expandPointerBorrow(Builder& builder, Function& callee, Node* const* arguments,
                          const Substitution& with, LocationId source) {
    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);

    /*
     * **One emitter for both roles**, because the difference between them is entirely in the
     * declaration and this reads it: `-> source'a` asks for a shared loan and `-> &source'a` for an
     * exclusive one, so whether a write through the place is legal is `result.contract`'s answer
     * and not a second argument that could disagree with it. `requestOf` is the one function that
     * turns a declared position into what a read of it asks for.
     */
    auto wanted = requestOf(result);

    /*
     * The storage keeps the result's **form**, and `ownedContract()` alone did not.
     *
     * A place reached through a raw pointer is storage, and the loan taken from it inherits the
     * storage's form (`grantedContract`). Built at `WholeValue` the loan was one too, and inside a
     * generic body `substitutedContract` then turned it into the *view* at `a := Array(I32)` - so
     * `getMut(&self, index) = borrowMut(valuesMut(self) + index)` handed back a descriptor where its
     * declared `-> &source'v` names the element's own storage. The declared result is the one place
     * that says which of the two this is, and this reads it rather than restating it.
     */
    auto storage = Contract { Access::Owned, Origin::Here, result.contract.form(), 0 };
    auto place = builder.addressPlace(*arguments[0], requalified(result, storage),
                                      wanted.access == Access::Unique);

    return builder.addRead(source, *place, readMode(*builder.program.decl, builder.base(), builder.body, *place, wanted), wanted);
}

/*
 * `p + n` and `p - n` - arithmetic in **elements** rather than in bytes.
 *
 * The scale is a `stride` node and not a number, for the reason that node exists: at a type parameter
 * the answer is a load out of the caller's descriptor, and folding a literal here would be this
 * stage deciding a layout it cannot see. `Native.strideOf` is the same measurement the source can
 * ask for, so a program that walks storage by hand and this operation agree by construction.
 *
 * **A reinterpretation, an integer displacement and a reinterpretation back**, rather than an `add`
 * with a pointer on one side. `op.def` says the operation is heterogeneous and the language agrees -
 * that is why pointer arithmetic is not `Num` - but an IR whose `OpCategory` table is built on
 * operands agreeing has no room for a node that is heterogeneous, and loosening it would put a case
 * in every reader of `binaryResult`. Writing the reinterpretation out is what an address *is* at
 * this boundary, it costs nothing (a bitcast is no instruction), and it keeps the one rule.
 *
 * The arithmetic is at the **count's** width, which is what the declaration states: `advance(it: %a,
 * count: Size)` says the offset is a `Size`, so the multiply and the displacement are both that and
 * neither is a width this function chose.
 */
static Node* displace(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source, BinaryOp op) {
    auto pointer = substituteOccurrence(builder.context, builder.program, callee.result, with);
    auto element = ((PointerType*)builder.declBase()[pointer.type])->to;
    auto width = valueOccurrence(*arguments[1]).type;

    auto stride = builder.addMetric(source, TypeMetric::Stride, element, width);
    auto offset = builder.addBinary(source, BinaryOp::Mul, *arguments[1], *stride);

    // On the pointer itself rather than through an integer, so that what the pointer refers to is
    // still on the result - `Builder::addDisplace`, where that is argued.
    return builder.addDisplace(source, op, *arguments[0], *offset, pointer);
}

Node* expandPointerAdd(Builder& builder, Function& callee, Node* const* arguments,
                       const Substitution& with, LocationId source) {
    return displace(builder, callee, arguments, with, source, BinaryOp::Add);
}

Node* expandPointerSub(Builder& builder, Function& callee, Node* const* arguments,
                       const Substitution& with, LocationId source) {
    return displace(builder, callee, arguments, with, source, BinaryOp::Sub);
}

/*
 * `difference(from, to)` - how many elements apart two addresses are.
 *
 * The byte distance divided by the stride, which is the same two facts `p + n` uses in the other
 * order, and through the same reinterpretation for the same reason: two pointers subtract to a
 * *number*, so the operands are read as numbers rather than the node being given a result type its
 * operands do not have.
 *
 * `to - from` and not the reverse, because the declaration names the operands that way round - so a
 * positive answer is a `to` that is further along.
 *
 * A division and not a shift. The stride is not a power of two in general, and turning one into a
 * shift where it is belongs to the optimizer.
 */
Node* expandPointerDifference(Builder& builder, Function& callee, Node* const* arguments,
                              const Substitution& with, LocationId source) {
    auto width = substituteType(builder.context, builder.program, callee.result.type, with);
    auto& held = valueOccurrence(*arguments[0]);
    auto element = ((PointerType*)builder.declBase()[held.type])->to;

    auto to = builder.addBitcast(source, *arguments[1], width);
    auto from = builder.addBitcast(source, *arguments[0], width);

    auto bytes = builder.addBinary(source, BinaryOp::Sub, *to, *from);
    auto stride = builder.addMetric(source, TypeMetric::Stride, element, width);

    return builder.addBinary(source, BinaryOp::Div, *bytes, *stride);
}

/*
 * `traits()` - everything this compilation decided about a type, as a `TypeTraits(a)`.
 *
 * **The type comes from the position, not from a value**: `a` is bound out of the result, exactly as
 * `null() :: %U8` binds a pointee, so `traits() :: TypeTraits(Entry(k, v))` asks about a type nothing
 * has an instance of. That is what the `sizeOf(it: a)` this replaced could not do at all.
 *
 * One `Node::Metric` per field and an ordinary aggregate over them - the value the language would
 * have built had a program written `TypeTraits {size: ..., ...}` out by hand. There is no second
 * construction path here and nothing about this record the rest of the stage knows: at a concrete
 * type every component folds to an immediate and the record dissolves wherever it is used, and at an
 * erased one every component is a read of the descriptor the caller supplied.
 *
 * **The field list and metric.def are held to each other**, in order and in count. They are two
 * lists in two files that have to agree, and nothing but this check makes them: a sixth field added
 * in source with no row behind it would otherwise be built out of whatever the fifth left behind.
 * Reported rather than asserted, because what makes this a `TypeTraits` is a declaration in the
 * library and a library that has drifted is a broken *installation* rather than a broken program.
 */
Node* expandTypeTraits(Builder& builder, Function& callee, Node* const*, const Substitution& with,
                       LocationId source) {
    auto& context = builder.context;
    auto result = substituteType(context, builder.program, callee.result.type, with);

    auto record = builder.declBase()[result]->kind == Type::Record
        ? (RecordType*)builder.declBase()[result] : nullptr;

    if(!record || record->arguments.size() != 1 || record->cons.size() != 1) {
        context.diagnostics.error("internal: traits() answers something that is not a TypeTraits"_v,
                                  source);
        return nullptr;
    }

    if(record->fields.size() != kTypeMetricCount) {
        context.diagnostics.error(
            "internal: TypeTraits declares %@ fields and the compiler fills %@"_v, source,
            Size(record->fields.size()), Size(kTypeMetricCount));
        return nullptr;
    }

    auto of = record->arguments.get(builder.declBase(), 0);

    // In declaration order, which is the only thing this and the source agree by - see metric.def,
    // where that is written down, and the field list in `lib/Native/Pointer.yana`.
    Node* components[kTypeMetricCount];
    for(Size i = 0; i < kTypeMetricCount; i++) {
        auto field = record->fields.get(builder.declBase(), i);
        components[i] = builder.addMetric(source, TypeMetric(i), of, field.occurrence.type);
    }

    return builder.addAggregate(source, result, 0, components, kTypeMetricCount);
}

/*
 * `*p` - a read of what the pointer names.
 *
 * The place's occurrence is the *result's*, substituted: `fn *(it: %a) -> a` answers an owned `a`,
 * and at a call through a `%I32` that is an owned `I32`. Taking it from the result rather than
 * unwrapping the argument's pointer type is deliberate - the two are the same type by `op.def`'s
 * `pointeeOf(0)`, and reading it off the position the value actually fills is what keeps the
 * contract right as well as the type.
 *
 * `readMode` decides what the read *is*, which is the same function every other read asks: a copy
 * where the pointee is trivially copyable, and a move out of the place where it is not. The second
 * is what makes `*p` at an owning type unsafe rather than ill-formed - it takes the value and leaves
 * bytes behind that nothing tracks - and it is Native's to be unsafe, not this function's to refuse.
 */
/*
 * The place-level middle of every address `Read` - `expandPointerLoad`, `expandVectorAt` and
 * `expandVectorPast` share exactly this and nothing else: a place at `address`, the same
 * copy-or-move choice `readMode` makes for every other read, and an owned request. `overread` is a
 * parameter rather than always `false` here because it is `NodeRead`'s own bit and not a second
 * decision this function makes - `expandPointerLoad` supplies `false` for the same reason
 * `expandVectorAt` does, a plain `*p` never claiming the tail-read guarantee, and `expandVectorPast`
 * supplies `true`. Keeping it out would not remove a decision, only push the same call to `addRead`
 * back into three places.
 *
 * What stays with the callers is `asVectorAddress`'s bitcast: it is the one step that is not this
 * operation at every type, only at a vector one, so folding it in here would make the pointer path
 * carry a vector concern it does not have.
 */
static Node* readThroughAddress(Builder& builder, Node& address, const Occurrence& result,
                                LocationId source, bool overread, bool isVolatile = false) {
    auto place = builder.addressPlace(address, result, false);

    auto wanted = bindingRequest(Access::Owned);
    return builder.addRead(source, *place, readMode(*builder.program.decl, builder.base(), builder.body, *place, wanted), wanted,
                           overread, isVolatile);
}

// The place-level middle of every address `Write` - `expandPointerStore`'s comment on
// `Destination::Assign` is what this still does not do (release, track) and why: an address root
// has no move path, so neither call above nor below this point may claim one.
static void writeThroughAddress(Builder& builder, Node& address, const Occurrence& held, Node& value,
                                LocationId source, bool isVolatile = false) {
    auto place = builder.addressPlace(address, requalified(held, ownedContract()), true);
    builder.addWrite(source, *place, value, Destination::Assign, isVolatile);
}

Node* expandPointerLoad(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source) {
    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);
    return readThroughAddress(builder, *arguments[0], result, source, false);
}

/*
 * `store(p, ->v)` - a write of what the pointer names.
 *
 * The value arrives owned, because the declaration says `->` and `checkRoles` refused it otherwise.
 * That is the whole reason the shape check exists: the pointee holds the value afterwards, so the
 * frame that owned it stops owing it a teardown, and a declaration that wrote a plain parameter
 * there would leave the obligation with the caller and the value with the pointee.
 *
 * `Destination::Assign`, and it releases nothing - which is the thing to be clear about rather than
 * to leave implied. A store does not drop its own destination in this IR: the release an assignment
 * would run is a step the drop analysis places, over move paths, and an address root has no move
 * path because it is not storage this frame owns. So what the pointee held before is neither
 * released nor tracked, which is precisely what makes a raw store unsafe - and is Native's to be
 * unsafe rather than this function's to refuse.
 *
 * `Init` is not available and should not be: it asserts a root the frame owns, and it means "this
 * slot was empty", which nothing here is in a position to claim.
 *
 * The result is `{}`, which the declared result already says - so it is built from the substituted
 * result occurrence rather than from a unit written here.
 */
Node* expandPointerStore(Builder& builder, Function& callee, Node* const* arguments,
                         const Substitution& with, LocationId source) {
    auto pointee = valueOccurrence(*arguments[1]);
    writeThroughAddress(builder, *arguments[0], pointee, *arguments[1], source);

    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);
    return builder.addConst(source, unitConstant(result.type));
}

Node* expandVolatilePointerLoad(Builder& builder, Function& callee, Node* const* arguments,
                                const Substitution& with, LocationId source) {
    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);
    return readThroughAddress(builder, *arguments[0], result, source, false, true);
}

Node* expandVolatilePointerStore(Builder& builder, Function& callee, Node* const* arguments,
                                 const Substitution& with, LocationId source) {
    auto pointee = valueOccurrence(*arguments[1]);
    writeThroughAddress(builder, *arguments[0], pointee, *arguments[1], source, true);

    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);
    return builder.addConst(source, unitConstant(result.type));
}

/*
 * The whole-vector transfers - `vectorAt`, `vectorPast` and `setVectorAt`, `lib/Native/Pointer.native.yana`.
 *
 * **Each is `*p` or `store(p, v)` at a vector type, and nothing else.** The address the caller hands
 * over is an element pointer - a `%U8` into a run of bytes - and what a whole-register transfer
 * wants is that same address named at the vector's type. A pointer's *value* does not depend on what
 * it is said to point at, so the whole address computation is a `Bitcast`: no arithmetic, and no
 * alignment claim either, a vector access being unaligned on both of this compiler's backends by
 * construction.
 *
 * So there is no node here that the pointer path does not already have, and that is the decision
 * worth recording: a dedicated `vector.load` would be a second spelling of `Read` at an address
 * root, and every pass that reasons about places - the loan checker, the drop elaboration, the
 * conversion below - would need an arm for it that said the same thing. `vectorAt(p, i)` therefore
 * reads through a pointer loan exactly as `pointerLoad` does, and `setVectorAt` writes through a `%`
 * with `pointerStore`'s rule, because they *are* those two operations.
 *
 * `vectorPast` is the one bit of difference: the same load with the tail-read guarantee spent - see
 * `NodeRead::overread`, which is where the promise is written down. No store ever carries it.
 */
static Node* asVectorAddress(Builder& builder, Node& pointer, DeclPtr<Type> vector,
                             LocationId source) {
    if(!vector || builder.declBase()[vector]->kind != Type::Vector) {
        builder.context.diagnostics.error("a whole-vector transfer needs a vector type, and %@ is not one"_v,
                                          source, typeText(builder.context, builder.declBase(), vector));
        return nullptr;
    }

    return builder.addBitcast(source, pointer, builder.program.types.pointerType(vector));
}

// One emitter for the load and the overreading load, because the flag is the whole difference and a
// second copy of these four lines is what would let the two drift apart. Past the bitcast, this is
// `expandPointerLoad`'s own read - `readThroughAddress` is where the two meet.
static Node* loadVector(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source, bool overread) {
    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);

    auto address = asVectorAddress(builder, *arguments[0], result.type, source);
    if(!address) return nullptr;

    return readThroughAddress(builder, *address, result, source, overread);
}

Node* expandVectorAt(Builder& builder, Function& callee, Node* const* arguments,
                     const Substitution& with, LocationId source) {
    return loadVector(builder, callee, arguments, with, source, false);
}

Node* expandVectorPast(Builder& builder, Function& callee, Node* const* arguments,
                       const Substitution& with, LocationId source) {
    return loadVector(builder, callee, arguments, with, source, true);
}

/*
 * `setVectorAt(p, v)` - the store, which never overreads.
 *
 * The vector type comes from the *value* rather than from the result, the result being `{}`: what
 * width is written is what the caller had in hand. `Destination::Assign` and it releases nothing,
 * exactly as `pointerStore` does and for the same reason - an address root has no move path, so
 * what the memory held before is neither dropped nor tracked, and that is Native's to be unsafe
 * about.
 */
Node* expandSetVectorAt(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source) {
    auto held = valueOccurrence(*arguments[1]);

    auto address = asVectorAddress(builder, *arguments[0], held.type, source);
    if(!address) return nullptr;

    writeThroughAddress(builder, *address, held, *arguments[1], source);

    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);
    return builder.addConst(source, unitConstant(result.type));
}

} // namespace mir
