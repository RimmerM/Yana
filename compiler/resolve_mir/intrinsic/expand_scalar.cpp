#include "expand_internal.h"

namespace mir {

using namespace intrinsic;

/*
 * The roles that are one arithmetic node over their operands, and nothing else.
 *
 * A file of its own rather than arms written into `expand.cpp`, on the terms every other emitter
 * file here has: the roster is a switch that names an emitter per row, and an emitter written into
 * the switch is a row whose expansion a reader has to find twice. What is common to this file is not
 * a shape of node - `expand_native.cpp` is that - but a *question*: each of these is an operation
 * the language declares over a scalar, whose whole content is which node it becomes and at which
 * widths a machine has one.
 */

/*
 * `mulHigh(a, b)` - the top half of a product twice as wide as the operands.
 *
 * `a * b` is the low half and this is the high one, so the two together are the whole double-width
 * product. What wants it is `lib/Core/Hash.yana`'s multiply-fold, `lo ^ hi`, which is wyhash's and
 * xxh3's core.
 *
 * **Refused below the widths a machine has one at**, which is the one thing here that is not the
 * node. A narrow operand is held in a wider register - a `U8` is eight bits of a thirty-two-bit one
 * - so the high half of a *register's* product is not the high half of the product the declaration
 * names, and every tier below this one works in register widths. Rather than emit an operation that
 * answers zero for every `U8` pair, the call is reported: this is the same set `Bits` is instanced
 * over and for the same reason, which is that the machine's instruction is what the operation is.
 *
 * The target's word joins whichever end of its bound it lands on, since both 32 and 64 are in the
 * set - so `Size` and `USize` are admitted without this having to know which.
 */
Node* expandMulHigh(Builder& builder, Function& callee, Node* const* arguments,
                    const Substitution& with, LocationId source) {
    auto& context = builder.context;
    auto type = substituteType(context, builder.program, callee.result.type, with);

    if(!hasMachineWidth(builder.declBase(), type)) {
        context.diagnostics.error("the top half of a product is the machine's instruction at 32 and 64 bits and this is %@, whose product's high half would be a question about a register rather than about the value - widen the operands and take the high half of that"_v,
                                  source, typeText(context, builder.declBase(), type));
        return nullptr;
    }

    return builder.addBinary(source, BinaryOp::MulHi, *arguments[0], *arguments[1]);
}

} // namespace mir
