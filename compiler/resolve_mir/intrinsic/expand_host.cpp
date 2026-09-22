#include "expand_internal.h"

namespace mir {

using namespace intrinsic;

/*
 * The host - `lib/Native/Host.js.yana`, `host.def`, and Analysis-IR-JS.md §3.10.
 *
 * Two shapes, and which one a role has is a decision rather than a division of labour:
 *
 *  - seventeen of the roles are a **node that passes through** - `Node::Host`, one `host.def` row
 *    each, the member name read off the literal here and the operand modes read off the declaration.
 *    Nothing below this tier knows what one means until JSIR does.
 *  - four of them are a **place** - `hostRead`, `hostWrite`, `hostAt` and `hostAtMut`, which
 *    `op.def` already calls the four faces of one thing. A host element is `Place::Index` over a
 *    `Place::Address` root, so one form gives the read, the write and both borrows, and the borrow
 *    the two `At` roles hand back is rooted by exactly the machinery that roots `borrow`'s. A node
 *    would have needed a second rooting rule in `analyze_loan.cpp` to say what `self: %source'a`
 *    already says.
 *
 * Nothing here knows a host name. The member is an argument, `ShapeTerm::Text` is what says the
 * emitter reads its constant, and `memberName` below is the whole of the dependency.
 */

/*
 * The member the call named, as a literal.
 *
 * A diagnostic and not a fallback, for the reason `sha1rnds4`'s round group is a diagnostic: what a
 * computed name would mean in the emitted source is `o[k]` and a string object built at run time,
 * which is a different operation from the one the role describes. This is the last place the
 * argument exists as something an author wrote, so it is the only place that can name it.
 *
 * `Constant::Text` on both targets, and on JavaScript that is the *whole* of what a literal is -
 * see `Resolver::stringConstant`, whose JS arm leaves the text where it is.
 */
static bool memberName(Builder& builder, Function& callee, Node& argument, LocationId source,
                       StringId& name) {
    if(argument.kind == Node::Const) {
        auto constant = builder.body.constant(builder.base(), ((NodeConst&)argument).constant);
        if(constant.kind == Constant::Text) {
            name = constant.asText();
            return true;
        }
    }

    builder.context.diagnostics.error(
        "%@ names a member of the host, so that argument must be a written string literal - a computed one is a property lookup rather than the operation this is"_v,
        source, builder.context.findName(callee.name));
    return false;
}

/*
 * A host operation, as the node `host.def` describes.
 *
 * `nameAt` is the position of the member argument, or `kNoMember` on a row that has none. The
 * argument is dropped from the operand list - it was spent building the node, and a name left as a
 * value would be a run-time string in every block.
 *
 * The modes are the **declaration's**, which is the whole of §3.10's item 3 and the one thing that
 * makes this different from `nativeOperation`: `argumentMode` over the substituted parameter
 * occurrence is the same rule a call's arguments are read under, so `hostWrite`'s `->value` arrives
 * as a move and the ownership passes see one without knowing what a host is.
 */
static constexpr U32 kNoMember = 0xffffffff;

static Node* hostOperation(Builder& builder, Function& callee, HostOp op, Node* const* arguments,
                           Size count, U32 nameAt, const Substitution& with, LocationId source) {
    auto base = builder.declBase();
    StringId member {};

    if(nameAt != kNoMember && !memberName(builder, callee, *arguments[nameAt], source, member)) {
        return nullptr;
    }

    SmallArray<Node*, 6> operands;
    SmallArray<UseMode, 6> modes;

    for(Size i = 0; i < count; i++) {
        if(i == nameAt) continue;

        auto& parameter = *base[callee.args.get(base, U32(i))];
        auto occurrence = substituteOccurrence(builder.context, builder.program,
                                               parameter.occurrence, with);

        operands.push(arguments[i]);
        modes.push(argumentMode(occurrence.contract, SourceKind::Result));
    }

    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);
    return builder.addHost(source, op, member, operands.pointer(), modes.pointer(), operands.size(),
                           result);
}

Node* expandHostField(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source) {
    return hostOperation(builder, callee, HostOp::Field, arguments, 2, 1, with, source);
}

/*
 * `self.member(args...)` - one role per arity, because a declaration has one arity and there is no
 * variadic form to write, and **one row** for the four: what differs between them is the count,
 * which is already the node's operand count. Passing the arity twice is what `host.def`'s note on
 * this row removed.
 */
Node* expandHostCall(Builder& builder, Function& callee, Node* const* arguments, Size count,
                     const Substitution& with, LocationId source) {
    return hostOperation(builder, callee, HostOp::Call, arguments, count, 1, with, source);
}

// `Path.of.a.name(args)` - the member is the whole dotted path and is written literally into the
// emitted source, so there is no receiver among the operands at all.
Node* expandHostGlobal(Builder& builder, Function& callee, Node* const* arguments, Size count,
                       const Substitution& with, LocationId source) {
    return hostOperation(builder, callee, HostOp::Global, arguments, count, 0, with, source);
}

// `lhs <operator> rhs`, whose member name is the operator's own spelling.
Node* expandHostBinary(Builder& builder, Function& callee, Node* const* arguments,
                       const Substitution& with, LocationId source) {
    return hostOperation(builder, callee, HostOp::Binary, arguments, 3, 1, with, source);
}

// `throw message` - the message is the member, which is what puts the text in `lib` rather than in a
// code generator holding a string a reader would look for there.
Node* expandHostThrow(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source) {
    return hostOperation(builder, callee, HostOp::Throw, arguments, 1, 0, with, source);
}

// The `DataView` accessors - the name is the accessor's, which is where the width lives.
Node* expandHostViewRead(Builder& builder, Function& callee, Node* const* arguments,
                         const Substitution& with, LocationId source) {
    return hostOperation(builder, callee, HostOp::ViewRead, arguments, 4, 1, with, source);
}

Node* expandHostViewWrite(Builder& builder, Function& callee, Node* const* arguments,
                          const Substitution& with, LocationId source) {
    return hostOperation(builder, callee, HostOp::ViewWrite, arguments, 5, 1, with, source);
}

// The host array's own three, none of which names a member.
Node* expandHostArray(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source) {
    return hostOperation(builder, callee, HostOp::Array, arguments, 0, kNoMember, with, source);
}

Node* expandHostGrow(Builder& builder, Function& callee, Node* const* arguments,
                     const Substitution& with, LocationId source) {
    return hostOperation(builder, callee, HostOp::Grow, arguments, 2, kNoMember, with, source);
}

Node* expandHostFixedCapacity(Builder& builder, Function& callee, Node* const* arguments,
                              const Substitution& with, LocationId source) {
    return hostOperation(builder, callee, HostOp::FixedCapacity, arguments, 1, kNoMember, with,
                         source);
}

// The three whole-vector transfers - `host.def` argues why they are rows rather than `lanes` element
// places written out here.
Node* expandHostVector(Builder& builder, Function& callee, Node* const* arguments, Size count,
                       const Substitution& with, LocationId source, HostOp op) {
    return hostOperation(builder, callee, op, arguments, count, kNoMember, with, source);
}

/*
 * And the element - the four faces of one place.
 *
 * `Place::Index` over `Place::Address`, which is what `op.def`'s own comment says a host element is:
 * an lvalue, so one form gives the read, the write and the two borrows, and every pass above the
 * backend sees it in the position a native element is in. The element's occurrence is the pointee's,
 * which is what a `%a` says and the only thing that does.
 *
 * The borrows are `expandPointerBorrow` with the index step added, deliberately down to the
 * spelling: the declared result is what says whether the loan is shared or exclusive
 * (`-> source'a` against `-> &source'a`), `requestOf` turns it into what a read of it asks for, and
 * the storage keeps the result's *form* so that a generic body handing back an element of a
 * container gets the descriptor its declaration named rather than a whole value.
 */
static Place* hostElement(Builder& builder, Node& array, Node& index, const Occurrence& result,
                          bool writable, LocationId source) {
    /*
     * What the root denotes is the **container** - the `array` role at the element - and the reason
     * is `lib/Native/Host.js.yana`'s own first paragraph: a `%a` on this target is an opaque
     * reference to storage rather than an address elements are counted from. So the handle names
     * the array, the index projects an element of it, and the conversion types the projection off
     * that. A `[a *n]` would have been the other spelling and would have had to invent an `n`.
     */
    auto role = builder.program.roles.record(intrinsic::Known::array);
    auto element = result.type;
    auto container = role ? instantiateRecord(builder.context, builder.program, role, &element, 1,
                                              source)
                          : nullptr;
    if(!container) {
        builder.context.diagnostics.error(
            "the standard library does not declare anything with the role array, which a host element needs"_v,
            source);
        return nullptr;
    }

    auto storage = Contract { Access::Owned, Origin::Here, result.contract.form(), 0 };
    return builder.hostElementPlace(array, index, container, requalified(result, storage), writable);
}

Node* expandHostRead(Builder& builder, Function& callee, Node* const* arguments,
                     const Substitution& with, LocationId source) {
    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);
    auto place = hostElement(builder, *arguments[0], *arguments[1], result, false, source);
    if(!place) return nullptr;

    // Owned, exactly as `*p` is: `hostRead` answers the element **by value**, which is what makes
    // `let ->doomed = hostRead(xs, i)` a move out of the array and is what a container's teardown
    // and its `remove` are written with.
    auto wanted = bindingRequest(Access::Owned);
    return builder.addRead(source, *place,
                           readMode(*builder.program.decl, builder.base(), builder.body, *place,
                                    wanted), wanted);
}

/*
 * `hostWrite(self, index, ->value)` - an assignment through the place, which is the whole reason
 * there is no `hostPush`.
 *
 * The ownership passes read a hand-over out of an assignment into a place and not out of an operand,
 * so `arr[arr.length] = item` leaves the frame owing nothing where `arr.push(item)` would have left
 * it owning a value the array is holding. `Array.js.yana`'s own note says the same thing from the
 * other side. `Destination::Assign` and not `Init` for `expandPointerStore`'s reason: an address root
 * has no move path, so there is nothing to claim was empty.
 */
Node* expandHostWrite(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source) {
    auto held = valueOccurrence(*arguments[2]);
    auto place = hostElement(builder, *arguments[0], *arguments[1],
                             requalified(held, ownedContract()), true, source);
    if(!place) return nullptr;

    builder.addWrite(source, *place, *arguments[2], Destination::Assign);

    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);
    return builder.addConst(source, unitConstant(result.type));
}

// The two borrows: one emitter, because the difference between them is entirely in the declaration
// and this reads it.
Node* expandHostAt(Builder& builder, Function& callee, Node* const* arguments,
                   const Substitution& with, LocationId source) {
    auto result = substituteOccurrence(builder.context, builder.program, callee.result, with);
    auto wanted = requestOf(result);
    auto place = hostElement(builder, *arguments[0], *arguments[1], result,
                             wanted.access == Access::Unique, source);
    if(!place) return nullptr;

    return builder.addRead(source, *place,
                           readMode(*builder.program.decl, builder.base(), builder.body, *place,
                                    wanted), wanted);
}

} // namespace mir
