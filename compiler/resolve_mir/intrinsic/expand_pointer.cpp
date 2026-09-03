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
 * `addressOf(x)` - the address of the storage a value occupies.
 *
 * One node and nothing else, and everything that makes it safe enough to have is in the
 * *declaration*: `fn addressOf(it: source'a) -> %source'a` borrows and publishes, so the ordinary
 * call path has already read the argument as a loan of a place and the origin environment already
 * knows the pointer is in `source`. Written `it: a` the position would be an ordinary unpublished
 * one, `readMode` would answer `CopyTrivial` at every scalar, and what came back would be the
 * address of a copy - which is both the wrong storage and an address rooted in nothing.
 *
 * The occurrence is the *declared result's*, substituted, exactly as `pointerLoad` below takes its
 * pointee from the result: the two are the same type by `op.def`'s `pointerTo(0)`, and reading it
 * off the position the value fills is what carries the published interior origin as well as the
 * type.
 */
Node* expandAddressOf(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source) {
    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);
    return builder.addAddressOf(source, *arguments[0], result);
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
    auto wanted = requestOf(result.contract);
    auto place = builder.addressPlace(*arguments[0], requalified(result, ownedContract()),
                                      wanted.access == Access::Unique);

    return builder.addRead(source, *place, readMode(*builder.program.decl, *place, wanted), wanted);
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

    auto address = builder.addBitcast(source, *arguments[0], width);
    auto moved = builder.addBinary(source, op, *address, *offset);

    return builder.addBitcast(source, *moved, pointer.type);
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
    // where that is written down, and the field list in `prelude/Native/Pointer.yana`.
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
Node* expandPointerLoad(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source) {
    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);
    auto place = builder.addressPlace(*arguments[0], result, false);

    auto wanted = bindingRequest(Access::Owned);
    return builder.addRead(source, *place, readMode(*builder.program.decl, *place, wanted), wanted);
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
    auto place = builder.addressPlace(*arguments[0], requalified(pointee, ownedContract()), true);

    builder.addWrite(source, *place, *arguments[1], Destination::Assign);

    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);
    return builder.addConst(source, unitConstant(result.type));
}

} // namespace mir
