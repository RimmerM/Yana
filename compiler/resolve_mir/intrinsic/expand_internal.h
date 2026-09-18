#pragma once

#include "expand.h"

namespace mir {

/*
 * The emitters, one declaration per `op.def` row, grouped into files by family.
 *
 * Here rather than in `expand.h` because they are not the interface: what the resolver calls is
 * `expandIntrinsic`, which is one switch over the role enum in `expand.cpp`. Splitting them this way
 * is what keeps that switch a *roster* - a family's file holds its emitters and nothing else, and
 * adding a family adds a file and a set of arms rather than growing one of them.
 *
 * Every one of them takes the same four things and nothing more: the values the ordinary call path
 * prepared, the callee whose declared signature they fill, the substitution the call bound, and
 * where it was written. An emitter that wanted anything else would be one reaching past what a call
 * site is - see the note on `expandIntrinsic`.
 */

// expand_pointer.cpp - the raw pointer, `doc/spec/native.md`.
Node* expandAddressOf(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source);
// Both borrow roles: which one is `callee.result`'s to say, so there is one emitter.
Node* expandPointerBorrow(Builder& builder, Function& callee, Node* const* arguments,
                          const Substitution& with, LocationId source);
Node* expandPointerAdd(Builder& builder, Function& callee, Node* const* arguments,
                       const Substitution& with, LocationId source);
Node* expandPointerSub(Builder& builder, Function& callee, Node* const* arguments,
                       const Substitution& with, LocationId source);
Node* expandPointerDifference(Builder& builder, Function& callee, Node* const* arguments,
                              const Substitution& with, LocationId source);
Node* expandPointerLoad(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source);
Node* expandPointerStore(Builder& builder, Function& callee, Node* const* arguments,
                         const Substitution& with, LocationId source);

// `traits()` - the one emitter with no arguments at all: its type comes from the result position.
Node* expandTypeTraits(Builder& builder, Function& callee, Node* const* arguments,
                       const Substitution& with, LocationId source);

/*
 * The whole-vector transfers, which are the two above at a vector type - `lib/Native/Pointer.native.yana`.
 *
 * Here and not in `expand_vector.cpp` because what they are is a load and a store through an address
 * root; the vector is the *type* they transfer, not an operation they perform. Two emitters for the
 * three roles: `vectorAt` and `vectorPast` differ by the tail-read flag alone.
 */
Node* expandVectorAt(Builder& builder, Function& callee, Node* const* arguments,
                     const Substitution& with, LocationId source);
Node* expandVectorPast(Builder& builder, Function& callee, Node* const* arguments,
                       const Substitution& with, LocationId source);
Node* expandSetVectorAt(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source);


// expand_scalar.cpp - the roles that are one arithmetic node, at the widths a machine has one.
Node* expandMulHigh(Builder& builder, Function& callee, Node* const* arguments,
                    const Substitution& with, LocationId source);


// expand_native.cpp - the operations a machine has and the IR does not, `native.def`.
Node* expandBlockCopy(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source);
Node* expandSetMemory(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source);

// The two vector transfers that stop at a lane count - `native.def`'s `VecLoadUpTo` and `VecStoreUpTo`.
Node* expandVecLoadUpTo(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source);
Node* expandVecStoreUpTo(Builder& builder, Function& callee, Node* const* arguments,
                         const Substitution& with, LocationId source);

// The one emitter with an arity, because there are seven declarations and one operation.
Node* expandSyscall(Builder& builder, Function& callee, Node* const* arguments, Size count,
                    const Substitution& with, LocationId source);

Node* expandCloneThread(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source);

// The two halves of a stack context - `native.def`'s `StackInit` and `StackSwitch` rows.
Node* expandStackInit(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source);
Node* expandStackSwitch(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source);

/*
 * The x86 family - `lib/Native/Intrinsic/X86.yana`, one `native.def` row apiece.
 *
 * One shared emitter covers the fixed-operand rows, while the software interrupt has a small
 * literal check of its own because its vector is encoded in the instruction rather than read from
 * a register. `op` at the call site rather than a switch inside is the same arrangement
 * `expandAtomicUpdate` has.
 */
Node* expandX86Native(Builder& builder, Function& callee, Node* const* arguments, Size count,
                      const Substitution& with, LocationId source, NativeOp op);
Node* expandX86Interrupt(Builder& builder, Function& callee, Node* const* arguments,
                         const Substitution& with, LocationId source);
Node* expandX86AesKeygenAssist(Builder& builder, Function& callee, Node* const* arguments,
                               const Substitution& with, LocationId source);
Node* expandX86CarrylessMultiply(Builder& builder, Function& callee, Node* const* arguments,
                                 const Substitution& with, LocationId source);
Node* expandX86Sha1Rounds(Builder& builder, Function& callee, Node* const* arguments,
                          const Substitution& with, LocationId source);


// expand_atomic.cpp - `lib/Atomic/Atomic.native.yana`, Analysis-Atomics.md §3.
Node* expandAtomicNew(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source);
Node* expandAtomicIntoValue(Builder& builder, Function& callee, Node* const* arguments,
                            const Substitution& with, LocationId source);
Node* expandAtomicLoad(Builder& builder, Function& callee, Node* const* arguments,
                       const Substitution& with, LocationId source);
Node* expandAtomicStore(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source);

// The exchange and the five fetch operations, which are one shape told apart by the `op` - see the
// definition, which is where the argument for that being a parameter rather than six emitters is.
Node* expandAtomicUpdate(Builder& builder, AtomicOp op, Node* const* arguments, LocationId source);

Node* expandAtomicFence(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source);

Node* expandAtomicSpinHint(Builder& builder, Function& callee, Node* const* arguments,
                           const Substitution& with, LocationId source);

/*
 * What the four `OpForm::Branch` rows in this family share with the emitters above - the order
 * tables, the ordinal read and the content type.
 *
 * Declared here although their caller is the *resolver* rather than an emitter, because they belong
 * to this family and not to the walk that happens to lay one of them out: which constructor of
 * `LoadOrder` means acquire is a fact about `lib/Atomic`, and a second copy of it beside the
 * resolver is the one that would go stale.
 */
extern const AtomicOrder kLoadOrders[3];
extern const AtomicOrder kStoreOrders[3];
extern const AtomicOrder kUpdateOrders[5];
extern const AtomicOrder kFenceOrders[4];

Maybe<AtomicOrder> orderArgumentIn(Builder& builder, Node& node, const AtomicOrder* table,
                                   Size count, StringView which, LocationId source);

// The table's own length, so a call site names the table and nothing else - a row added to one of
// the four would otherwise be a number to update at each of its readers.
template<Size N>
Maybe<AtomicOrder> orderArgument(Builder& builder, Node& node, const AtomicOrder (&table)[N],
                                 StringView which, LocationId source) {
    return orderArgumentIn(builder, node, table, N, which, source);
}

// The `a` of the `Atomic(a)` an operation was handed, read off the argument rather than off the
// substitution - so one reader serves a signature whose result is the content and one whose is not.
DeclPtr<Type> contentOf(Builder& builder, Node& self);


/*
 * expand_vector.cpp - the portable vector set, `lib/Core/Vector.yana` and Design-Vector §3.3.
 *
 * Several of these take an extra parameter where `op.def` has a row per declaration and the
 * *operation* is one: four reductions differ in which fold, two rearrangements in which half, two
 * unpacks in which end, and the two shuffles in how many sources they read. A row each because a
 * declaration each is what the library writes; one emitter each because what tells them apart is a
 * column rather than a shape.
 */
Node* expandVecSplat(Builder& builder, Function& callee, Node* const* arguments,
                     const Substitution& with, LocationId source);
Node* expandVecZero(Builder& builder, Function& callee, Node* const* arguments,
                    const Substitution& with, LocationId source);
Node* expandVecIota(Builder& builder, Function& callee, Node* const* arguments,
                    const Substitution& with, LocationId source);
Node* expandVecLane(Builder& builder, Function& callee, Node* const* arguments,
                    const Substitution& with, LocationId source);
Node* expandVecWithLane(Builder& builder, Function& callee, Node* const* arguments,
                        const Substitution& with, LocationId source);
Node* expandVecLanes(Builder& builder, Function& callee, Node* const* arguments,
                     const Substitution& with, LocationId source);

Node* expandVecReduce(Builder& builder, Function& callee, ReduceOp reduce, Node* const* arguments,
                      const Substitution& with, LocationId source);
Node* expandVecMinMax(Builder& builder, Function& callee, BinaryOp op, Node* const* arguments,
                      const Substitution& with, LocationId source);
Node* expandVecAbs(Builder& builder, Function& callee, Node* const* arguments,
                   const Substitution& with, LocationId source);

// `sqrt` and the four roundings, which are one emitter and a column - the column being which unary
// node it is, and that row is where the name the diagnostic uses comes from too.
Node* expandVecFloatUnary(Builder& builder, UnaryOp op, Node* const* arguments, LocationId source);
Node* expandVecFma(Builder& builder, Function& callee, Node* const* arguments,
                   const Substitution& with, LocationId source);

Node* expandVecShuffle(Builder& builder, Function& callee, U32 sources, Node* const* arguments,
                       const Substitution& with, LocationId source);
Node* expandVecReverse(Builder& builder, Function& callee, Node* const* arguments,
                       const Substitution& with, LocationId source);
Node* expandVecRotate(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source);
Node* expandVecAlignLanes(Builder& builder, Function& callee, Node* const* arguments,
                          const Substitution& with, LocationId source);
Node* expandVecInterleave(Builder& builder, Function& callee, bool high, Node* const* arguments,
                          const Substitution& with, LocationId source);

Node* expandVecUnpack(Builder& builder, Function& callee, bool high, Node* const* arguments,
                      const Substitution& with, LocationId source);
Node* expandVecPackLanes(Builder& builder, Function& callee, Node* const* arguments,
                         const Substitution& with, LocationId source);

// `lookup`'s index width is checked here; `compress` and `expand` go straight to `expandX86Native`.
Node* expandVecLookup(Builder& builder, Function& callee, Node* const* arguments,
                      const Substitution& with, LocationId source);

Node* expandMaskNone(Builder& builder, Function& callee, Node* const* arguments,
                     const Substitution& with, LocationId source);
Node* expandMaskUpTo(Builder& builder, Function& callee, Node* const* arguments,
                     const Substitution& with, LocationId source);

/*
 * expand_container.cpp - what a container asks the compiler about its own key type.
 *
 * One emitter and a column for the two scan-limit rows, because what differs between them is where
 * the key is - inside an `Entry` or at the pointer itself - and not what is computed from it.
 */
Node* expandScanLimit(Builder& builder, Function& callee, bool unwrapEntry, Node* const* arguments,
                      const Substitution& with, LocationId source);

// expand_exchange.cpp - `swap` and `exchange`, `doc/spec/core.md`'s Exchange.
Node* expandSwap(Builder& builder, Function& callee, Node* const* arguments,
                 const Substitution& with, LocationId source);
Node* expandExchange(Builder& builder, Function& callee, Node* const* arguments,
                     const Substitution& with, LocationId source);

} // namespace mir
