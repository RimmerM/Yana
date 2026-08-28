#include "atomic.h"
#include "core.h"
#include "intrinsic.h"
#include "name.h"
#include "expr.h"

/*
 * The `Atomic` module's compiler half - Analysis-Atomics.md §3.
 *
 * `lib/Atomic/Atomic.native.yana` is the whole of the written surface; this is the whole of what the
 * compiler has to supply behind it, which is every operation. There is nothing here that could have
 * been written in Yana: an atomic load is an instruction, and a body over some lower-level intrinsic
 * would only move the question down one level and add a call at every use in an unoptimized build.
 *
 * ## The order is read here, and it must be a constant
 *
 * §3.3 requires an ordering argument to be a compile-time constant, and this is where that is
 * enforced. The reason is not convenience: an order changes which instructions are generated and
 * which motions the optimizer may perform, so it is not useful runtime state, and requiring it to be
 * settled at the call site is what guarantees good code without an optimizer having run. Passing one
 * through a generic helper stays possible wherever the compiler specializes that helper - the
 * argument is a constant *there*, which is the point at which this asks.
 *
 * The four ordering types collapse to one `AtomicOrder` on the way in. They exist so that
 * `store(x, LoadAcquire)` does not typecheck, which is a statement about what a caller may write;
 * once the pairing is settled there is one fact left, which is how strong the edge is.
 */

namespace {

/*
 * The number behind a payload-free constructor, where the call site wrote one.
 *
 * Every ordering type is an enumeration with no payload, so a written `LoadAcquire` is an integer by
 * the time it reaches here - which is what makes "is this a constant" the same question as "did the
 * caller write one of the constructors". Anything else is an order computed at run time, and there
 * is deliberately no fallback that picks the strongest: a program that reaches this has an ordering
 * decision it thinks it is making and is not.
 */
Maybe<U64> constantOrdinal(ExprResolver& resolver, ModulePtr<Value> value) {
    auto instruction = resolver.local[value];
    if(!instruction || instruction->kind != Value::ConstInt) return Nothing();

    return Just(((ConstInt*)instruction)->value);
}

// The three each ordering type lists, in declaration order - which is the order the constructors are
// numbered in, so the ordinal is the index into these.
const AtomicOrder kLoadOrders[] = {
    AtomicOrder::Relaxed, AtomicOrder::Acquire, AtomicOrder::Sequential,
};

const AtomicOrder kStoreOrders[] = {
    AtomicOrder::Relaxed, AtomicOrder::Release, AtomicOrder::Sequential,
};

const AtomicOrder kUpdateOrders[] = {
    AtomicOrder::Relaxed, AtomicOrder::Acquire, AtomicOrder::Release,
    AtomicOrder::AcquireRelease, AtomicOrder::Sequential,
};

const AtomicOrder kFenceOrders[] = {
    AtomicOrder::Acquire, AtomicOrder::Release, AtomicOrder::AcquireRelease, AtomicOrder::Sequential,
};

Maybe<AtomicOrder> orderArgument(ExprResolver& resolver, ModulePtr<Value> value,
                                 Buffer<const AtomicOrder> table, StringView which, LocationId source) {
    auto ordinal = constantOrdinal(resolver, value);

    if(!ordinal || ordinal.unwrap() >= table.length) {
        resolver.context.diagnostics.error("the %@ of an atomic operation must be written at the call site - an order decides which instructions are generated, so it cannot be computed at run time"_v,
                                           source, which);
        return Nothing();
    }

    return Just(AtomicOrder(table[Size(ordinal.unwrap())]));
}

/*
 * §3.5's projection: what a failed comparison performs, given what a successful one does.
 *
 * A failure performs no write, so a release half has nothing to order and is dropped. This is the
 * same projection C++ uses for its one-order overload, and it covers the pairs an algorithm normally
 * wants - release/relaxed and acquire-release/acquire - without asking every caller two questions.
 */
AtomicOrder failureOrderFor(AtomicOrder success) {
    switch(success) {
        case AtomicOrder::Relaxed:        return AtomicOrder::Relaxed;
        case AtomicOrder::Acquire:        return AtomicOrder::Acquire;
        case AtomicOrder::Release:        return AtomicOrder::Relaxed;
        case AtomicOrder::AcquireRelease: return AtomicOrder::Acquire;
        case AtomicOrder::Sequential:     return AtomicOrder::Sequential;
    }

    return AtomicOrder::Sequential;
}

// §3.5's table. The failure order may not be stronger than the success order, where stronger is
// `relaxed < acquire < sequential`: a compare-exchange whose *loss* published more than its win is
// something no algorithm wants and no target implements without the strong form.
bool isLegalFailureOrder(AtomicOrder success, AtomicOrder failure) {
    switch(success) {
        case AtomicOrder::Relaxed:
        case AtomicOrder::Release:
            return failure == AtomicOrder::Relaxed;
        case AtomicOrder::Acquire:
        case AtomicOrder::AcquireRelease:
            return failure == AtomicOrder::Relaxed || failure == AtomicOrder::Acquire;
        case AtomicOrder::Sequential:
            return true;
    }

    return false;
}

/*
 * The location an operation names, as an address.
 *
 * Every atomic instruction takes a **pointer** to the atomic rather than the atomic itself, and that
 * is forced rather than chosen: an `Atomic(a)` is a memory type, so what a body holds of one is
 * storage reached through a place, and the lowering has no *value* to hand an instruction. Taking
 * the address here is the same step `atomic` and `intoValue` take, so all three reach the location
 * the same way.
 */
ModulePtr<Value> atomicAddress(ExprResolver& resolver, ModulePtr<Value> self, LocationId source) {
    return resolver.addressOf(resolver.materialize(self, source), source);
}

// The `a` of the `Atomic(a)` this operation was handed. Taken from the argument rather than from the
// substituted type arguments, so that one emitter serves a signature whose result is the content and
// one whose result is not.
TypePtr contentType(ExprResolver& resolver, ModulePtr<Value> self) {
    auto type = resolver.valueType(self);
    if(!type || resolver.global[type]->kind != Type::Atomic) return nullptr;

    return ((AtomicType*)resolver.global[type])->content;
}

/*
 * `atomic(initial)` - §3.1.
 *
 * Storage of the atomic's own type, with the initial value written into it through a pointer at the
 * *content's* type. Three things about that are deliberate.
 *
 * It **allocates**, because an atomic is a location and not a value: what every operation on one
 * takes is a borrow, and a borrow has to name storage. That is the same reason `Atomic(a)` is a
 * memory type rather than a direct one - a body that held the bits in a register would be holding a
 * copy, and every operation on it would be atomic with respect to nothing.
 *
 * The write is an **ordinary store and not an atomic one**. Nothing can be sharing this location
 * yet: what a sharer holds is a borrow, and a borrow of a value that is still being constructed does
 * not exist. Making it atomic would cost an ordering edge to publish to nobody.
 *
 * The address is **bitcast** rather than projected, because there is no field to project. An
 * `Atomic(a)`'s Repr *is* its content's bytes - `computeAtomic` says so - so the two pointers are
 * one address named at two types, which is what a bitcast is.
 */
ModulePtr<Value> emitAtomic(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                            LocationId source, StringId name) {
    // A content outside §3.2's family has already been reported by `resolveAtomicType`, which leaves
    // the error type behind - so this is a call whose type failed rather than an emitter that was
    // handed the wrong one, and saying so twice would put "internal" in front of an ordinary user's
    // ordinary mistake.
    if(!type || resolver.global[type]->kind == Type::Error) return nullptr;

    if(resolver.global[type]->kind != Type::Atomic) {
        resolver.context.diagnostics.error("internal: atomic's result is not an atomic type"_v, source);
        return nullptr;
    }

    auto content = ((AtomicType*)resolver.global[type])->content;
    auto storage = resolver.allocate(type, source, name);
    auto local = ((InstAlloc*)resolver.local[storage])->local;

    auto address = resolver.addressOf(Place::inLocal(local), source);
    auto asContent = resolver.ref(resolver.emit<InstUnary>(
        source, StringId(), resolvePointerType(resolver.module, content), Value::Bitcast, address));

    resolver.assign(Place::atPointer(asContent), args[0], source);
    return storage;
}

/*
 * `intoValue(self)` - §3.1, and the one operation here that emits no atomic instruction.
 *
 * Owning the value proves there is no outstanding borrow and therefore no concurrent accessor, so
 * reading it is an ordinary load. That is what makes it the clean teardown path for a containing
 * structure, and the way a test inspects a private atomic after every worker has joined.
 */
ModulePtr<Value> emitIntoValue(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                               LocationId source, StringId name) {
    /*
     * The `->` the declaration writes on `self`, applied by hand - emitExchange in core.cpp does the
     * same thing for the same reason. expandIntrinsic matches arguments to parameters and applies
     * none of their conventions, so a `->` position that an emitter does not sink itself is not a
     * handover at all: the caller's storage is never marked moved out of, and `intoValue(c)` twice
     * reads the same live location twice and drops it twice.
     *
     * Sinking first, rather than reading the value and sinking afterwards for the record. Both leave
     * the operation correct, and this one costs an elidable copy where the other leaves an unused
     * InstMove - the copy is what an optimizer removes, and ownership is not something to owe.
     */
    auto owned = resolver.rootSink(resolver.sinkValue(args[0], source), source);
    if(!owned) return nullptr;

    // Through the address rather than out of the value, for the reason `atomic` writes through one:
    // an `Atomic(a)` is a memory type, so what a body holds of one is storage, and reading it at the
    // content's type is one pointer named two ways.
    auto address = resolver.addressOf(resolver.materialize(owned, source), source);
    auto asContent = resolver.ref(resolver.emit<InstUnary>(
        source, StringId(), resolvePointerType(resolver.module, type), Value::Bitcast, address));

    return resolver.load(Place::atPointer(asContent), source, name);
}

// One atomic instruction with its operands already in hand. Every emitter below ends in this.
ModulePtr<Value> emitAtomicInst(ExprResolver& resolver, AtomicKind kind, AtomicOrder order,
                                TypePtr type, Buffer<ModulePtr<Value>> operands,
                                LocationId source, StringId name) {
    auto instruction = resolver.create<InstAtomic>(source, name, type, kind, order);
    for(auto operand: operands) instruction->args.push(resolver.module.arena, operand);

    resolver.append(instruction);
    return isUnit(resolver.global, type) ? nullptr : resolver.ref(instruction);
}

ModulePtr<Value> emitLoad(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                          LocationId source, StringId name) {
    auto order = tryMaybe(orderArgument(resolver, args[1], { kLoadOrders, 3 }, "order"_v, source),
                          return nullptr);

    ModulePtr<Value> operands[] = { atomicAddress(resolver, args[0], source) };
    return emitAtomicInst(resolver, AtomicKind::Load, order, type, { operands, 1 }, source, name);
}

ModulePtr<Value> emitStore(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                           LocationId source, StringId name) {
    auto order = tryMaybe(orderArgument(resolver, args[2], { kStoreOrders, 3 }, "order"_v, source),
                          return nullptr);

    ModulePtr<Value> operands[] = { atomicAddress(resolver, args[0], source), args[1] };
    return emitAtomicInst(resolver, AtomicKind::Store, order, type, { operands, 2 }, source, name);
}

// The exchange and the five fetch operations, which are one shape: a location, a value, an order,
// and the value from before the update as the answer.
template<AtomicKind kind>
ModulePtr<Value> emitUpdate(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                            LocationId source, StringId name) {
    auto order = tryMaybe(orderArgument(resolver, args[2], { kUpdateOrders, 5 }, "order"_v, source),
                          return nullptr);

    ModulePtr<Value> operands[] = { atomicAddress(resolver, args[0], source), args[1] };
    return emitAtomicInst(resolver, kind, order, type, { operands, 2 }, source, name);
}

/*
 * `compareExchange` and its three siblings - §3.4 and §3.5.
 *
 * The instruction answers two things and the library answers one, so what is built here is the
 * branch that joins them: `ExchangeResult(a)` is `Exchanged` where the store happened and
 * `Unchanged(previous)` where it did not.
 *
 * The flag is read out of the instruction rather than reconstructed by comparing `previous` against
 * `expected`, and the weak form is why. It may fail spuriously - answering the value it was handed
 * and *not* having stored - and a caller that compared would read that as a win. See InstAtomicOk.
 */
ModulePtr<Value> emitCompare(ExprResolver& resolver, ModulePtr<Value> self, ModulePtr<Value> expected,
                             ModulePtr<Value> desired, AtomicOrder success, AtomicOrder failure,
                             bool weak, TypePtr type, LocationId source) {
    auto content = contentType(resolver, self);
    if(!content) {
        resolver.context.diagnostics.error("internal: a compare-exchange on something that is not an atomic"_v, source);
        return nullptr;
    }

    auto instruction = resolver.create<InstAtomic>(source, StringId(), content,
                                                   AtomicKind::Compare, success);
    instruction->failure = failure;
    instruction->weak = weak;
    instruction->args.push(resolver.module.arena, atomicAddress(resolver, self, source));
    instruction->args.push(resolver.module.arena, expected);
    instruction->args.push(resolver.module.arena, desired);
    resolver.append(instruction);

    auto previous = resolver.ref(instruction);
    auto exchanged = resolver.ref(resolver.emit<InstAtomicOk>(
        source, StringId(), resolver.module.scalar.bool_, previous));

    auto stored = resolver.addBlock();
    auto kept = resolver.addBlock();
    resolver.terminate(resolver.emit<InstJe>(source, StringId(), resolver.module.scalar.unit,
                                             exchanged, stored, kept));

    BranchArmList arms;

    resolver.current = stored;
    arms.push(BranchArm { resolver.current, resolver.makeConstructed(type, 0, nullptr, source), source });

    resolver.current = kept;
    arms.push(BranchArm { resolver.current, resolver.makeConstructed(type, 1, previous, source), source });

    return resolver.finishBranches(arms, source, true);
}

// The plain forms, whose failure order is §3.5's projection of the success order.
template<bool weak>
ModulePtr<Value> emitCompareExchange(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                     LocationId source, StringId) {
    auto success = tryMaybe(orderArgument(resolver, args[3], { kUpdateOrders, 5 }, "order"_v, source),
                            return nullptr);

    return emitCompare(resolver, args[0], args[1], args[2], success, failureOrderFor(success),
                       weak, type, source);
}

// And `Advanced`'s, which state both and are held to §3.5's table.
template<bool weak>
ModulePtr<Value> emitCompareExchangePair(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                         LocationId source, StringId) {
    auto success = tryMaybe(orderArgument(resolver, args[3], { kUpdateOrders, 5 }, "success order"_v, source),
                            return nullptr);
    auto failure = tryMaybe(orderArgument(resolver, args[4], { kLoadOrders, 3 }, "failure order"_v, source),
                            return nullptr);

    if(!isLegalFailureOrder(success, failure)) {
        resolver.context.diagnostics.error("the failure order of a compare-exchange may not be stronger than its success order - a failed comparison performs no write, so there is nothing for the stronger one to order"_v,
                                           source);
        return nullptr;
    }

    return emitCompare(resolver, args[0], args[1], args[2], success, failure, weak, type, source);
}

ModulePtr<Value> emitFence(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                           LocationId source, StringId name) {
    auto order = tryMaybe(orderArgument(resolver, args[0], { kFenceOrders, 4 }, "order"_v, source),
                          return nullptr);

    return emitAtomicInst(resolver, AtomicKind::Fence, order, type, {}, source, name);
}

ModulePtr<Value> emitSpinHint(ExprResolver& resolver, Buffer<ModulePtr<Value>>, TypePtr type,
                              LocationId source, StringId name) {
    // Any order at all: a spin hint establishes none, and the field is carried only so that every
    // atomic instruction reads alike. `Relaxed` is the one that says "no edge".
    return emitAtomicInst(resolver, AtomicKind::SpinHint, AtomicOrder::Relaxed, type, {}, source, name);
}

} // namespace

/*
 * `Native.atomicAt(address)` - §3.7, and the one unchecked entrance.
 *
 * Two instructions, and neither of them is an atomic one. The address arrives as `%U8` because the
 * caller has bytes rather than a typed pointer, so it is re-read at `%Atomic(a)` - which moves
 * nothing: `computeAtomic` makes an `Atomic(a)`'s Repr its content's bytes, so the two pointers are
 * one address named at two types. Then the memory that pointer names is borrowed.
 *
 * Which is `Native.borrow` with the type fixed, and that is the whole design of it. The unsafe
 * entrance *reuses* the semantics rather than defining a second set: everything the caller does with
 * the result goes through the same emitters as an atomic this program allocated, so there is no path
 * on which an externally-provided location and an owned one behave differently.
 *
 * Nothing here checks the four promises §3.7 lists, and nothing could: the address is a run-time
 * value, and whether every other accessor of those bytes uses a view of this width is a fact about
 * a program this compiler is not reading. `Native` is where the language says that.
 *
 * The place is `atPointer` for `emitBorrowAt`'s reason - the borrow is rooted where the pointer was
 * rooted, and the `return address` in the declaration is what carries that to the caller. Shared and
 * not exclusive, because every operation above takes a shared borrow: an atomic is the one thing many
 * holders may mutate at once, and handing out an exclusive one would say the opposite.
 */
ModulePtr<Value> emitAtomicAt(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                              LocationId source, StringId name) {
    // Silent on a failed type for `emitAtomic`'s reason: a view of a content outside the family has
    // already been reported where the type was resolved.
    if(!type || resolver.global[type]->kind == Type::Error) return nullptr;

    if(resolver.global[type]->kind != Type::Borrow) {
        resolver.context.diagnostics.error("internal: atomicAt's result is not a borrow"_v, source);
        return nullptr;
    }

    auto viewed = ((BorrowType*)resolver.global[type])->to;
    if(!viewed || resolver.global[viewed]->kind == Type::Error) return nullptr;

    if(resolver.global[viewed]->kind != Type::Atomic) {
        resolver.context.diagnostics.error("internal: atomicAt does not view an atomic"_v, source);
        return nullptr;
    }

    auto address = resolver.ref(resolver.emit<InstUnary>(
        source, StringId(), resolvePointerType(resolver.module, viewed), Value::Bitcast, args[0]));

    return resolver.ref(resolver.emit<InstBorrow>(
        source, name, type, Place::atPointer(address), false));
}

void definePreludeAtomic(Program& program, Module& atomic) {
    // On a JS build the module's one file is not read - see lib/Atomic/Atomic.native.yana - so there
    // is nothing declared for any of this to attach to. Returning rather than reporting, because an
    // absent native-only module is what a JS build is supposed to have.
    if(isJsMode(program.context.settings)) return;

    program.coreClasses.atomicValue = classNamed(atomic, "AtomicValue"_v);
    program.coreClasses.atomicInteger = classNamed(atomic, "AtomicInteger"_v);

    attachIntrinsic(atomic, "atomic"_v, emitAtomic);
    attachIntrinsic(atomic, "intoValue"_v, emitIntoValue);

    attachIntrinsic(atomic, "load"_v, emitLoad);
    attachIntrinsic(atomic, "store"_v, emitStore);
    attachIntrinsic(atomic, "exchange"_v, emitUpdate<AtomicKind::Exchange>);

    attachIntrinsic(atomic, "compareExchange"_v, emitCompareExchange<false>);
    attachIntrinsic(atomic, "compareExchangeWeak"_v, emitCompareExchange<true>);
    attachIntrinsic(atomic, "Advanced.compareExchange"_v, emitCompareExchangePair<false>);
    attachIntrinsic(atomic, "Advanced.compareExchangeWeak"_v, emitCompareExchangePair<true>);

    attachIntrinsic(atomic, "fetchAdd"_v, emitUpdate<AtomicKind::Add>);
    attachIntrinsic(atomic, "fetchSub"_v, emitUpdate<AtomicKind::Sub>);
    attachIntrinsic(atomic, "fetchAnd"_v, emitUpdate<AtomicKind::And>);
    attachIntrinsic(atomic, "fetchOr"_v, emitUpdate<AtomicKind::Or>);
    attachIntrinsic(atomic, "fetchXor"_v, emitUpdate<AtomicKind::Xor>);

    attachIntrinsic(atomic, "fence"_v, emitFence);
    attachIntrinsic(atomic, "spinHint"_v, emitSpinHint);

}

/*
 * §3.7's entrance, which is a declaration of **Native** and attaches here.
 *
 * The emitter belongs beside the operations it is a view for - it builds the same borrow every one
 * of them takes - and the declaration belongs where the language keeps what it cannot check. Those
 * are two different files and this is the seam between them: `lib/Native/Atomic.native.yana` writes
 * the signature and imports `Atomic` for the class that constrains it, and this attaches the body.
 *
 * Called from module.cpp only where the atomic module exists at all, since the signature naming
 * `Atomic(a)` cannot resolve without it.
 */
void definePreludeNativeAtomic(Program& program, Module& native) {
    if(isJsMode(program.context.settings)) return;
    attachIntrinsic(native, "atomicAt"_v, emitAtomicAt);
}
