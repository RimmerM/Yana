#include "resolve_internal.h"
#include "own.h"

namespace mir {

/*
 * External iterators - `fn f(...) -> iter(T)`, doc/inflight/Analysis-External-Iterators.md §2.1.
 *
 * A producer is an ordinary function with one parameter appended, and everything else is a call of
 * something `lib/Fiber` declares. Five sites, and each one is a sentence:
 *
 *  - **the declaration** appends `$iter: %IterBlock(T, {})` - the address of the producer's state,
 *    at the top of the mapping it runs on (`declareIterHandle`);
 *  - **a call** does not call. It captures the arguments in a tuple and hands that and an *entry* to
 *    `Iter.new`, which maps the stack and prepares a context that has not run (`constructIter`);
 *  - **the entry** is a function synthesized per producer and instantiation: it takes the tuple back
 *    out of the block, calls the producer with it, and ends the context (`iterEntry`);
 *  - **`yield e`** is `iterYield(block, e)`, whose answer says the consumer gave up on the producer -
 *    and the producer then returns, which is what runs its teardown (`resolveExternalYield`);
 *  - **`for x in ...`** asks `iterNext` and takes the value with `iterTake` (`resolveExternalFor`).
 *
 * A producer whose `yield` **lends** - `-> iter(T)` at a `T` with a teardown, §3.3 - is the same five
 * sentences through the other handle (`yieldLends`): a call hands over to `iterLentNew`, a `yield`
 * is `iterLend` with the value's *address*, and a `for` asks `iterLentNext` and borrows the value
 * with `iterCurrent`. That last one's signature roots the value in the handle, which is the whole of
 * §3.3's check: stepping or giving up the handle needs it whole, so neither can happen while the
 * value is still live.
 *
 * § What travels, and how
 *
 * A producer's parameters are the author's, so the tuple has to carry something every one of them
 * can be filled from on a stack the call site does not share - a closure's environment, built once
 * and handed over. Four ways in, decided by what the argument already is:
 *
 *  - **an owned value** travels as itself. At an owned parameter the producer consumes it; at a lent
 *    one it is a *temporary* the call was going to lend for one statement, so the handle keeps it
 *    and the entry lends it from there (`prepareArgument` leaves it owned for exactly this);
 *  - **a copy** made for the call travels as that copy;
 *  - **a view** travels as its view record - the two words, copied like any other value - and is
 *    restated as the view on the other side;
 *  - **any other loan** travels as its **address** and is borrowed again through it.
 *
 * **The handle borrows what it was lent**, as a closure borrows what it captured - §3.1 and §3.2.
 * A call of a producer is a call of `f$new` (`iterConstructor`), whose signature publishes every lent
 * parameter and binds the handle to it, so the lent locals stay alive and stay borrowed, shared or
 * exclusively, until the handle is dropped, and a handle over this frame's storage cannot leave it.
 *
 * § Why the producer sees `IterBlock(T, {})`
 *
 * How each argument travels is decided by the call, after the producer's signature is settled, so
 * the tuple's type is not something the signature can name. The producer never reads the tuple -
 * only the entry does, at the right type - so its parameter names the block at `{}`, and the
 * library keeps every field a producer reads ahead of the one whose layout depends on it
 * (`known.def`'s `iterBlock` row).
 */

/*
 * The walk's own state, saved around a body built in the middle of another - the entry and the
 * constructor are both synthesized while the caller's body is being resolved, which is
 * `resolveBody`'s situation under `requireResult` and is answered the same way.
 */
struct SynthesizedWalk {
    Resolver& r;
    Function* function;
    decltype(Resolver::held) held;
    decltype(Resolver::heldFloor) heldFloor;
    decltype(Resolver::loops) loops;
    Local* continuation;
    Local* yielded;
    bool guard;
    decltype(Resolver::handovers) handovers;
    decltype(Resolver::bindingFloor) bindingFloor;
    decltype(Resolver::lambdaFloor) lambdaFloor;
    U32 bindings;

    SynthesizedWalk(Resolver& r, Function& into):
        r(r), function(r.currentFunction), held(r.held), heldFloor(r.heldFloor), loops(r.loops),
        continuation(r.continuationLocal), yielded(r.yieldedLocal), guard(r.guardYields),
        handovers(r.handovers), bindingFloor(r.bindingFloor), lambdaFloor(r.lambdaFloor),
        bindings(U32(r.bindings.size())) {
        r.currentFunction = &into;
        r.held = nullptr;
        r.heldFloor = nullptr;
        r.loops = nullptr;
        r.continuationLocal = nullptr;
        r.yieldedLocal = nullptr;
        r.guardYields = false;
        r.handovers = 0;
        r.bindingFloor = bindings;
        r.lambdaFloor = U32(r.lambdaFrames.size());
    }

    ~SynthesizedWalk() {
        r.bindings.resize(bindings);
        r.bindingFloor = bindingFloor;
        r.lambdaFloor = lambdaFloor;
        r.currentFunction = function;
        r.held = held;
        r.heldFloor = heldFloor;
        r.loops = loops;
        r.continuationLocal = continuation;
        r.yieldedLocal = yielded;
        r.guardYields = guard;
        r.handovers = handovers;
    }
};

/*
 * Whether a producer's `yield` **lends** what it yields rather than handing it over - §3.3.
 *
 * A borrowing convention at a type that is not trivially copyable: a trivially copyable value is
 * copied into the slot, which no program can tell from the loan, and costs the consumer nothing.
 *
 * Decided on the *declared* hand-over and never on a substituted one, because the producer's body is
 * resolved once, generically, and whether its `yield` lends is decided there. A call at `T = Size`
 * of a producer generic in `T` therefore gets the lending handle too, which is what that body does.
 */
static bool yieldLends(DeclBase base, Function& producer) {
    return !producer.handover.contract.isOwned() && !isTriviallyCopyable(base, producer.handover.type);
}

/*
 * The parameter a producer's body reaches its block through - `Function::continuationArg`, shared
 * with the lens's continuation so that `writtenArity` and every call site's arity check go on not
 * counting it.
 *
 * A pointer and not a borrow of the block, for the reason `Native.Fiber`'s `suspendOn` takes one:
 * the address arrives from `stack.init`, which is not a borrow any frame on this stack took. Under
 * the plain convention, as `suspendOn`'s is, so that the entry's function type is the one
 * `newFiber`'s position states.
 */
bool Resolver::declareIterHandle(Function& function, LocationId source) {
    auto block = program.roles.record(intrinsic::Known::iterBlock);
    if(!block) {
        reportMissingRole(intrinsic::Known::iterBlock, "an external iterator"_v, source);
        return false;
    }

    // A deferred argument is a thunk over the caller's frame, and the caller's frame is not the
    // stack the producer runs on - so it would be a call into storage that is not there.
    for(auto argumentPointer: function.args.contents(decl())) {
        auto& argument = *decl()[argumentPointer];

        if(argument.lazy) {
            unsupported(argument.source, "a `@lazy` parameter of an external iterator"_v);
            return false;
        }
    }

    DeclPtr<Type> arguments[2] = { function.handover.type, program.types.unit };
    auto state = instantiateRecord(context, program, block, arguments, 2, source);
    if(!state) return false;

    auto declared = new (program.decl) Arg();
    declared->name = context.addUnqualifiedName("$iter", 5);
    declared->source = source;
    declared->index = U32(function.args.size());
    declared->occurrence = borrowed(program.types.pointerType(state), Access::Shared);

    function.continuationArg = U16(function.args.size());
    function.args.push(program.decl, declared - *program.decl);

    // What the *body* answers. A caller never sees it - a call builds a handle instead - which is
    // what keeps `return` inside a producer the unit return it reads as.
    function.result = owned(program.types.unit);
    return true;
}

/*
 * `f(a, b)` where `f` answers `iter(T)` - a handle, with the producer prepared and not run.
 *
 * Reached from `finishCall` with the arguments already prepared against the declared parameters, so
 * what each one *is* - moved, copied or lent - has been decided by the ordinary rules, and this reads
 * the answer off the value rather than deciding it again.
 *
 * **Concrete only.** The entry is one body per instantiation, and a call inside a generic function
 * has no instantiation to build one for; a function value cannot carry type arguments, so there is
 * no way to hand a generic entry over either.
 */
Node* Resolver::constructIter(Builder& builder, Function& callee, SmallArray<Node*, 8>& arguments,
                              TypeArguments& typeArguments, LocationId source) {
    typeArguments.fillDetermined(source);

    if(typeArguments.reportUndecided(source, "argument"_v,
                                     Tritium::format("call to %@", context.findName(callee.name)))) {
        return nullptr;
    }

    if(!dischargeRequirements(callee, typeArguments, source)) return nullptr;

    auto with = typeArguments.with();

    for(Size i = 0; i < with.count; i++) {
        if(with.arguments[i] && isConcreteType(decl(), with.arguments[i])) continue;

        unsupported(source, "calling an external iterator from inside a generic function"_v);
        return nullptr;
    }

    SmallArray<RecordField, 8> fields;
    SmallArray<IterCapture, 8> kinds;
    SmallArray<DeclPtr<OwnershipSchema>, 8> shapes;

    for(U32 i = 0; i < arguments.size(); i++) {
        auto value = arguments[i];
        if(!value) return nullptr;

        auto& parameter = *decl()[callee.args.get(decl(), i)];
        auto& arrived = valueOccurrence(*value);

        // A value that refers to storage inside it - a closure, a `'T` result - travels like any
        // other; what it refers to is the handle's to keep alive, which `iterConstructor` publishes.
        RecordField field;
        field.source = parameter.source;

        if(parameter.occurrence.contract.isOwned() || arrived.contract.isOwned()) {
            field.occurrence = owned(valueType(*value));
            kinds.push(IterCapture::Value);
        } else if(arrived.contract.form() == AccessForm::View) {
            // A view is already a value - two words that name somebody else's storage - so its
            // record is what is stored; the loan it stands for is the constructor's to carry.
            field.occurrence = owned(viewRecordOf(context, program, arrived));
            kinds.push(IterCapture::View);
        } else {
            field.occurrence = owned(program.types.pointerType(valueType(*value)));
            kinds.push(IterCapture::Address);
        }

        fields.push(field);
        shapes.push(arrived.shape);
    }

    // `{}` where there was nothing to capture, rather than a tuple of no fields - the same type an
    // empty literal is, and what `IterBlock.begin` then hands back.
    DeclPtr<Type> captured = program.types.unit;

    if(fields.size()) {
        captured = tupleType(context, program, fields.pointer(), fields.size(), source);
        if(!captured) return nullptr;
    }

    auto entry = iterEntry(callee, with, captured, source);
    if(!entry) return nullptr;

    auto constructor = iterConstructor(callee, with, captured, entry, kinds, shapes, source);
    if(!constructor) return nullptr;

    /*
     * An ordinary call, and that is the whole of the borrowing: `iterConstructor`'s signature
     * publishes every lent position at one origin slot and binds that slot in the handle it
     * returns, so the loan analysis roots the handle in each argument's loan by the rule it applies
     * to any `'T` result. The lent locals stay borrowed - shared or exclusively, as each parameter
     * asked - for as long as the handle is live, which is what a closure's environment does.
     */
    Substitution none;
    return builder.addCall(source, *decl()[constructor], arguments.pointer(), arguments.size(), none);
}

/*
 * `f$new` - what a call of a producer calls, per entry.
 *
 *      fn f$new(a: A, b: s'B, c: s'C) -> Iter(T, {A, BView, %C})    -- `block` in `s`
 *          Iter.new({a, viewRecord(b), addressOf(c)}, f$entry)
 *
 * A function rather than nodes at the call site for one reason: a **signature** is what can say that
 * the handle refers to what it was lent. Every lent parameter publishes slot 0 - one slot, because the
 * handle has one position to bind it at, what its `block` pointer reaches - and the result binds it
 * there. Owned and copied parameters publish nothing.
 *
 * The access the slot is bound at is exclusive where any lent parameter is. Every argument's own loan
 * keeps the access it was taken at either way; this is only what a reborrow *through* the handle
 * could claim, and nothing reborrows through it.
 */
DeclPtr<Function> Resolver::iterConstructor(Function& callee, const Substitution& with,
                                            DeclPtr<Type> captured, DeclPtr<Function> entry,
                                            const SmallArray<IterCapture, 8>& kinds,
                                            const SmallArray<DeclPtr<OwnershipSchema>, 8>& shapes,
                                            LocationId source) {
    /*
     * The rest of the key: every argument's interior positions as one shape over the tuple, each
     * path prefixed with the argument's index. Interned, so two calls carrying the same compare as
     * one handle - and a closure and a plain function at one position do not.
     */
    SmallArray<ExternalSlot, 8> prefixed;

    for(U32 i = 0; i < shapes.size(); i++) {
        if(!shapes[i]) continue;

        auto& schema = *decl()[shapes[i]];
        for(U32 j = 0; j < schema.slots.size(); j++) {
            auto& slot = *decl()[schema.slots.get(decl(), j)];

            ExternalSlot position;
            position.path.push(program.decl, PathStep::field(U16(i)));
            for(Size step = 0; step < slot.path.size(); step++) {
                position.path.push(program.decl, slot.path.get(decl(), step));
            }

            position.contract = slot.contract;
            prefixed.push(position);
        }
    }

    DeclPtr<OwnershipSchema> carried = nullptr;
    if(prefixed.size()) carried = program.shapes.intern(prefixed.pointer(), prefixed.size());

    // The constructor already built for this entry and what this call carries - or else a row of the
    // entry's that has none yet, or else a copy of one, since every constructor it has publishes
    // something else.
    auto none = U32(iterEntries.size());
    auto sibling = none;
    auto known = none;

    for(U32 i = 0; i < none; i++) {
        auto& row = iterEntries[i];
        if(row.entry != entry) continue;

        if(!row.constructor) known = known == none ? i : known;
        else if(row.carried == carried) return row.constructor;

        sibling = i;
    }

    if(known == none) {
        assertTrue(sibling != none);

        auto row = iterEntries[sibling];
        row.constructor = nullptr;
        iterEntries.push(row);
        known = none;
    }

    auto yielded = substituteType(context, program, callee.handover.type, with);

    // A producer that lends what it yields is stepped through the other handle - `yieldLends`.
    auto lending = yieldLends(decl(), callee);
    auto stateRole = lending ? intrinsic::Known::iterLentState : intrinsic::Known::iterState;
    auto newRole = lending ? intrinsic::Known::iterLentNew : intrinsic::Known::iterNew;

    auto state = program.roles.record(stateRole);
    if(!state) {
        reportMissingRole(stateRole, "an external iterator"_v, source);
        return nullptr;
    }

    DeclPtr<Type> handleArguments[2] = { yielded, captured };
    auto handle = instantiateRecord(context, program, state, handleArguments, 2, source);
    if(!handle) return nullptr;

    auto module = callee.module;
    auto text = Tritium::format("%@$new", context.findName(callee.symbol));
    auto symbol = program.uniqueSymbol(module, context, StringView(text.text(), text.size()));

    auto constructor = new (program.decl) Function(symbol, module);
    constructor->source = callee.source;
    constructor->summary.bodyKnown = true;

    auto lends = false;
    auto access = Access::Shared;

    /*
     * A parameter's own interior positions - a closure's environment, a `'T` field - each renumbered
     * to slot 0, so that what the argument refers to *inside* it is bound in the handle as well:
     * the handle keeps the value, so it keeps what the value keeps. At the producer those positions
     * are borrowed for the call, which is exactly what a handle outliving the call cannot promise.
     *
     * A function value with no environment names no storage, so it publishes nothing.
     */
    auto handleShape = [&](DeclPtr<OwnershipSchema> shape) -> DeclPtr<OwnershipSchema> {
        if(!shape || shape == program.shapes.noEnvironment()) return nullptr;

        auto& schema = *decl()[shape];
        SmallArray<ExternalSlot, 8> slots;

        for(U32 j = 0; j < schema.slots.size(); j++) {
            auto& slot = *decl()[schema.slots.get(decl(), j)];

            ExternalSlot renumbered;
            renumbered.path = slot.path;
            renumbered.contract = slotContract(slot.contract.access(), 0, slot.contract.form());
            slots.push(renumbered);

            if(slot.contract.access() == Access::Unique) access = Access::Unique;
        }

        lends = true;
        return program.shapes.intern(slots.pointer(), slots.size());
    };

    for(U32 i = 0; i < kinds.size(); i++) {
        auto& parameter = *decl()[callee.args.get(decl(), i)];
        auto substituted = substituteOccurrence(context, program, parameter.occurrence, with);
        auto contract = substitutedContract(decl(), program, parameter.occurrence.type, substituted.type,
                                            substituted.contract);

        auto declared = new (program.decl) Arg();
        declared->name = parameter.name;
        declared->source = parameter.source;
        declared->index = i;

        auto inside = handleShape(shapes[i]);

        if(kinds[i] == IterCapture::Value) {
            auto& record = *(RecordType*)decl()[captured];
            declared->occurrence = Occurrence { record.field(decl(), U16(i)).occurrence.type,
                                                ownedContract(), inside };
        } else {
            declared->occurrence = Occurrence { substituted.type,
                                                slotContract(contract.access(), 0, contract.form()),
                                                inside };
            lends = true;
            if(contract.access() == Access::Unique) access = Access::Unique;
        }

        constructor->args.push(program.decl, declared - *program.decl);
    }

    DeclPtr<OwnershipSchema> shape = nullptr;

    if(lends) {
        ExternalSlot slot;
        // At the pointee and not at the field: the block pointer is the handle's own member, and what
        // it reaches is storage the lent loans keep - `%s'U8`'s provenance, which changes no layout,
        // rather than a qualifier that would take the member away and refuse `&self: Iter(y, a)`.
        slot.path.push(program.decl, PathStep::field(0));
        slot.path.push(program.decl, PathStep::pointee());
        slot.contract = slotContract(access, 0);
        shape = program.shapes.intern(&slot, 1);
    }

    constructor->result = Occurrence { handle, ownedContract(), shape };
    constructor->originSlots = lends ? 1 : 0;

    auto pointer = DeclPtr<Function>(constructor - *program.decl);
    decl()[module]->functions.push(program.decl, pointer);
    iterEntries[known].constructor = pointer;
    iterEntries[known].carried = carried;

    auto ok = true;

    {
        SynthesizedWalk walk(*this, *constructor);
        ErrorMark mark(context);
        Builder body(context, program, *constructor);

        SmallArray<Local*, 8> locals;
        for(U32 i = 0; i < kinds.size(); i++) {
            locals.push(body.addParameter(*decl()[constructor->args.get(decl(), i)]));
        }

        SmallArray<Node*, 8> values;

        for(U32 i = 0; ok && i < kinds.size(); i++) {
            auto& declared = *decl()[constructor->args.get(decl(), i)];
            auto value = readPlace(body, source, *body.localPlace(*locals[i]), requestOf(declared.occurrence));

            if(value && kinds[i] == IterCapture::View) {
                value = body.addViewRecord(source, *value);
            } else if(value && kinds[i] == IterCapture::Address) {
                auto& record = *(RecordType*)decl()[captured];
                value = body.addAddressOf(source, *value, record.field(decl(), U16(i)).occurrence);
            }

            if(!value) ok = false;
            else values.push(value);
        }

        Node* tuple = nullptr;
        if(ok) {
            tuple = values.size() ? body.addAggregate(source, captured, 0, values.pointer(), values.size())
                                  : body.addConst(source, unitConstant(program.types.unit));
            ok = tuple != nullptr;
        }

        Node* made = nullptr;
        if(ok) {
            auto entryValue = body.addFunValue(source, entry, funTypeOf(context, program, entry), nullptr);
            Node* operands[2] = { tuple, entryValue };
            made = entryValue ? callKnown(body, newRole, operands, 2, source) : nullptr;
            ok = made != nullptr;
        }

        if(ok) body.addReturn(source, *made);
        else if(!body.isTerminated()) body.addUnreachable(source);

        finish(body, *constructor, false, source, mark);
        body.seal();
    }

    constructor->bodyState = Function::BodyState::Read;
    return ok ? pointer : nullptr;
}

/*
 * The function a producer's stack starts at - `stack.init`'s entry, one per producer, per tuple of
 * what a call captured, per instantiation.
 *
 *      fn f$entry($block: %IterBlock(T, {})):
 *          let captured = IterBlock.begin($block)    -- :: {A, %B, ...}
 *          f(captured.0, borrow(captured.1), ..., $block)
 *          IterBlock.finish($block)
 *
 * Built as MIR directly rather than as source, because what it does to each field is the one thing a
 * source body could not say: move it, copy it, or borrow through the address it arrived as, decided
 * per field by what the call site did. A field whose type is the parameter's own is a value; one
 * whose type is a pointer to it arrived by address.
 *
 * `IterBlock.finish` does not come back - a context has no frame below its entry to return into - so the
 * `return` the body ends in is for the verifier and is never reached.
 */
DeclPtr<Function> Resolver::iterEntry(Function& callee, const Substitution& with,
                                      DeclPtr<Type> captured, LocationId source) {
    // The instantiation as one interned handle, so that the cache below compares three pointers.
    SmallArray<RecordField, 4> instantiation;

    for(Size i = 0; i < with.count; i++) {
        RecordField field;
        field.source = source;
        field.occurrence = owned(with.arguments[i]);
        instantiation.push(field);
    }

    DeclPtr<Type> arguments = program.types.unit;
    if(instantiation.size()) {
        arguments = tupleType(context, program, instantiation.pointer(), instantiation.size(), source);
        if(!arguments) return nullptr;
    }

    auto calleePointer = DeclPtr<Function>(&callee - *program.decl);

    for(auto& known: iterEntries) {
        if(known.callee == calleePointer && known.captured == captured &&
           known.arguments == arguments) {
            return known.entry;
        }
    }

    auto yielded = substituteType(context, program, callee.handover.type, with);

    DeclPtr<Type> blockArguments[2] = { yielded, program.types.unit };
    auto state = instantiateRecord(context, program, program.roles.record(intrinsic::Known::iterBlock),
                                   blockArguments, 2, source);
    if(!state) return nullptr;

    auto module = callee.module;
    auto text = Tritium::format("%@$entry", context.findName(callee.symbol));
    auto symbol = program.uniqueSymbol(module, context, StringView(text.text(), text.size()));

    auto entry = new (program.decl) Function(symbol, module);
    entry->source = callee.source;
    entry->result = owned(program.types.unit);
    entry->summary.bodyKnown = true;

    auto handle = new (program.decl) Arg();
    handle->name = context.addUnqualifiedName("$block", 6);
    handle->source = source;
    handle->index = 0;
    // The plain convention, which is what `newFiber`'s `(%a) -> {}` position states - a pointer is
    // copied whichever way it is passed, and the position is what the value's type has to match.
    handle->occurrence = borrowed(program.types.pointerType(state), Access::Shared);
    entry->args.push(program.decl, handle - *program.decl);

    auto pointer = DeclPtr<Function>(entry - *program.decl);
    decl()[module]->functions.push(program.decl, pointer);

    // Recorded before the body, so that a producer constructing itself reaches this entry rather
    // than building a second one.
    iterEntries.push(IterEntry { calleePointer, captured, arguments, pointer });

    SynthesizedWalk walk(*this, *entry);

    auto ok = true;

    {
        ErrorMark mark(context);
        Builder body(context, program, *entry);

        auto blockLocal = body.addParameter(*handle);
        auto blockPlace = body.localPlace(*blockLocal);

        Node* begin[1] = { readPlace(body, source, *blockPlace, inspectRequest()) };
        auto taken = begin[0] ? callKnown(body, intrinsic::Known::iterBegin, begin, 1, source, captured)
                              : nullptr;

        if(!taken) ok = false;

        SmallArray<Node*, 8> passed;

        if(ok) {
            auto tuple = body.addLocal(StringId(), valueOccurrence(*taken), source, false);
            auto tuplePlace = body.localPlace(*tuple);
            body.addWrite(source, *tuplePlace, *taken, Destination::Init);

            for(U32 i = 0; ok && i < callee.writtenArity(); i++) {
                auto& parameter = *decl()[callee.args.get(decl(), i)];
                auto substituted = substituteOccurrence(context, program, parameter.occurrence, with);

                // The position as the call prepared its argument for it - `prepareArgument`'s own
                // restatement, so that a generic parameter at a type with a view is that view here too.
                auto declared = Occurrence { substituted.type,
                                             substitutedContract(decl(), program, parameter.occurrence.type,
                                                                 substituted.type, substituted.contract),
                                             substituted.shape };

                auto& record = *(RecordType*)decl()[captured];
                auto fieldType = record.field(decl(), U16(i)).occurrence.type;
                auto fieldPlace = body.fieldPlace(*tuplePlace, U16(i));

                auto viewed = declared.contract.form() == AccessForm::View;
                Node* value = nullptr;

                if(viewed && viewRecordOf(context, program, declared) == fieldType) {
                    // A captured view: the record, restated as the view it was.
                    auto copy = readPlace(body, source, *fieldPlace, inspectRequest());
                    if(copy) value = body.addRepresent(source, *copy, declared);
                } else if(sameType(fieldType, declared.type)) {
                    /*
                     * The value itself - moved or copied into an owned position, and lent from the
                     * tuple into a lent one. A lent one is an owned temporary the call handed over
                     * rather than dangle, and it is read as the whole value and then adapted, since a
                     * view of it is built by the member that derives one and not by a read.
                     */
                    auto whole = declared;
                    if(viewed) {
                        whole.contract = Contract { declared.contract.access(), declared.contract.origin(),
                                                    AccessForm::WholeValue, declared.contract.slot() };
                    }

                    value = readPlace(body, source, *fieldPlace, requestOf(whole));
                    if(value && viewed) value = adaptToView(body, *value, declared, source);
                } else {
                    auto op = declared.contract.access() == Access::Unique ? intrinsic::Op::pointerBorrowMut
                                                                           : intrinsic::Op::pointerBorrow;
                    auto& claim = program.roles.op[Size(op)];

                    if(!claim.filled()) {
                        reportMissingRole(op, "an external iterator's lent argument"_v, source);
                        ok = false;
                        break;
                    }

                    Node* address[1] = { readPlace(body, source, *fieldPlace, inspectRequest()) };
                    if(address[0]) {
                        value = callFunction(body, *decl()[DeclPtr<Function>(claim.declaration)],
                                             address, 1, source);
                    }
                }

                if(!value) ok = false;
                else passed.push(value);
            }
        }

        if(ok) {
            auto own = readPlace(body, source, *blockPlace, inspectRequest());
            if(own) passed.push(own);

            ok = own && body.addCall(source, callee, passed.pointer(), passed.size(), with);
        }

        if(ok) {
            Node* finishing[1] = { readPlace(body, source, *blockPlace, inspectRequest()) };
            ok = finishing[0] && callKnown(body, intrinsic::Known::iterFinish, finishing, 1, source);
        }

        if(!body.isTerminated()) body.addReturnUnit(source);

        finish(body, *entry, true, source, mark);
        body.seal();
    }

    entry->bodyState = Function::BodyState::Read;
    return ok ? pointer : nullptr;
}

/*
 * `yield e` inside a producer - the value handed across, and the one place a producer learns its
 * consumer is gone.
 *
 * `iterYield` answers whether the consumer resumed it to *cancel* rather than to ask for more - its
 * handle was dropped while this was suspended, §3.5. What the producer owes then is exactly what an
 * early `return` owes, so that is what it does: the teardown of every live local, loops and lens
 * regions included, is the path a `return` already has. There is nothing for a departure to resume
 * and no signal to carry, which is why this shares none of `resolveYield`'s bookkeeping.
 *
 * **A lent value is lent by address** (§3.3, `yieldLends`). The `yield` borrows the value where it
 * is - holding a temporary in a local first, as an internal `yield` does - and hands the consumer its
 * address, which stays valid for exactly as long as this frame is suspended at the call below. That
 * is the producer's half, and all it needs: nothing here runs while the consumer holds the value.
 * What stops the consumer holding it past that point is on the consumer's side - `LentIter.current`
 * borrows the handle, and resuming the producer needs the handle whole.
 *
 * A trivially copyable value is copied into the slot even under a lending convention, which no
 * program can tell from the loan.
 */
void Resolver::resolveExternalYield(Builder& builder, ast::Expr& expr, Function& function) {
    auto& handover = function.handover;
    auto lends = yieldLends(decl(), function);

    Node* value = nullptr;

    if(expr.yield && lends) {
        // The whole value, shared - never the view a plain `T` position would take, since what is
        // handed across is an address and a view record is not the value it describes.
        auto wanted = requestOf(borrowed(handover.type, Access::Shared));

        value = resolveValue(builder, *parse[expr.yield], handover.type, wanted);
        if(!value) return;

        value = coerce(builder, *value, handover.type, expr.source, "a lent value"_v, Access::Shared);
        if(!value) return;

        value = holdBorrowed(builder, *value, wanted, handover.type, expr.source);
        if(!value) return;

        if(valueOccurrence(*value).contract.form() == AccessForm::View) {
            context.diagnostics.error(
                "this is a view of %@ rather than the value, so there is nothing here to lend by address - yield the value it views, or write `-> iter(->T)` to hand one over"_v,
                expr.source, typeText(context, decl(), handover.type));
            return;
        }

        value = builder.addAddressOf(expr.source, *value, owned(program.types.pointerType(handover.type)));
        if(!value) return;
    } else if(expr.yield) {
        value = resolveValue(builder, *parse[expr.yield], handover.type, requestOf(ownedContract()));
        if(!value) return;

        value = coerce(builder, *value, handover.type, expr.source, "a handed-over value"_v,
                       Access::Owned);
        if(!value) return;
    } else if(decl()[handover.type]->kind == Type::Unit) {
        value = builder.addConst(expr.source, unitConstant(program.types.unit));
    } else {
        context.diagnostics.error("%@ hands over %@, so this `yield` needs a value"_v, expr.source,
                                  context.findName(function.name),
                                  typeText(context, decl(), handover.type));
        return;
    }

    auto block = readPlace(builder, expr.source, *builder.localPlace(*continuationLocal),
                           inspectRequest());
    if(!block) return;

    Node* operands[2] = { block, value };
    auto cancelled = callKnown(builder, lends ? intrinsic::Known::iterLend : intrinsic::Known::iterYield,
                               operands, 2, expr.source);
    if(!cancelled) return;

    auto leaving = builder.addBlock(StringId());
    auto next = builder.addBlock(StringId());

    builder.addBranch(expr.source, builder.resultOf(*cancelled, UseMode::Inspect), *leaving, *next,
                      nullptr, 0);

    builder.setBlock(*leaving);
    builder.addReturnUnit(expr.source);

    builder.setBlock(*next);
}

/*
 * `for x in f(...)` where `f` answers `iter(T)`, and `for x in handle` over a local holding one.
 *
 *      [let &$h = f(...)]
 *    header:  if !iterNext($h) -> exit
 *      let x = iterTake($h)
 *      ...body...            continue -> header, break -> exit
 *      jump header
 *    exit:
 *
 * An ordinary loop in the consumer's own region - the producer's frame is on another stack, so
 * nothing here is a continuation and `break` and `continue` are the edges `while` gives them.
 *
 * **What leaving does differs between the two spellings, and on purpose.** A handle the loop built
 * is a local of this statement, so any way out of the loop drops it and the drop cancels the
 * producer. A handle the author holds is theirs: leaving the loop leaves it suspended where it was,
 * and a second `for` over it carries on from there - which is what stepping by hand means.
 *
 * Answers false, having emitted nothing, where the source is neither.
 */
bool Resolver::resolveExternalFor(Builder& builder, ast::Expr& expr, ast::ForExpr& loop) {
    auto source = expr.source;
    auto state = program.roles.record(intrinsic::Known::iterState);
    auto lentState = program.roles.record(intrinsic::Known::iterLentState);

    auto callee = namedCallee(loop.from);
    auto constructs = callee && callee->externalIterator;

    // Which handle, and so whether the loop variable is the value or a loan of it: the producer's
    // convention where the loop builds the handle, and the handle's own type where it is held.
    auto lending = constructs && yieldLends(decl(), *callee.function);

    Local* held = nullptr;

    if(!constructs) {
        if(loop.from.kind != ast::Expr::Var) return false;

        held = findLocal(loop.from.var);
        if(!held) return false;

        auto record = recordDeclarationOf(decl(), held->occurrence.type);
        lending = record && record == lentState;
        if(!record || (record != state && !lending)) return false;
    }

    if(loop.alts.size()) {
        context.diagnostics.error(
            "an external iterator cannot fail, so there is nothing for an alternative to handle"_v,
            loop.pat.source);
        return true;
    }

    if(constructs) {
        auto value = resolveValue(builder, loop.from, nullptr, requestOf(ownedContract()));
        if(!value) return true;

        held = builder.addLocal(StringId(), valueOccurrence(*value), source, true);
        builder.addWrite(source, *builder.localPlace(*held), *value, Destination::Init);
    }

    auto place = builder.localPlace(*held);

    Carry carry(*this, source);

    auto header = builder.addBlock(StringId());
    auto exit = builder.addBlock(StringId());

    carry.declare(builder, *header);
    carry.declare(builder, *exit);

    SmallArray<Operand, 8> entry;
    carry.edge(builder, entry);
    builder.addJump(source, *header, entry.pointer(), entry.size());

    builder.setBlock(*header);
    carry.arrive(builder, *header, 0);

    auto scope = bindings.size();

    auto step = [&]() -> bool {
        Node* self[1] = { readPlace(builder, source, *place, bindingRequest(Access::Unique)) };
        if(!self[0]) return false;

        auto more = callKnown(builder, lending ? intrinsic::Known::iterLentNext : intrinsic::Known::iterNext,
                              self, 1, source);
        if(!more) return false;

        failIfNot(builder, *more, *exit, carry, source);

        /*
         * The value, taken out of the slot - or, from a lending producer, borrowed from the handle.
         * Shared, and rooted in the handle by `LentIter.current`'s signature, so the loop variable
         * keeps the handle borrowed for as long as the body uses it: the next pass's advance needs
         * the handle whole and is refused while the value is still live.
         */
        auto access = lending ? Access::Shared : Access::Unique;
        Node* again[1] = { readPlace(builder, source, *place, bindingRequest(access)) };
        if(!again[0]) return false;

        auto item = callKnown(builder, lending ? intrinsic::Known::iterCurrent : intrinsic::Known::iterTake,
                              again, 1, source);
        if(!item) return false;

        auto named = loop.pat.kind == ast::Pat::Var;
        auto bound = builder.addLocal(named ? loop.pat.var : StringId(), valueOccurrence(*item),
                                      loop.pat.source, false);
        builder.addWrite(loop.pat.source, *builder.localPlace(*bound), *item, Destination::Init);

        if(named) {
            bindLocal(loop.pat.var, bound);
        } else if(loop.pat.kind != ast::Pat::Any &&
                  !bindIrrefutable(builder, loop.pat, *builder.localPlace(*bound), "a `for` binding"_v)) {
            return false;
        }

        return true;
    };

    if(!step()) {
        bindings.resize(U32(scope));
        if(!builder.isTerminated()) builder.addUnreachable(source);
        builder.setBlock(*exit);
        carry.arrive(builder, *exit, 0);
        return true;
    }

    {
        Loop frame(*this, &carry, *exit, *header, builder.region(), false);
        resolveStatement(builder, loop.body);
    }

    bindings.resize(U32(scope));

    if(!builder.isTerminated()) {
        SmallArray<Operand, 8> back;
        carry.edge(builder, back);
        builder.addJump(source, *header, back.pointer(), back.size());
    }

    builder.setBlock(*exit);
    carry.arrive(builder, *exit, 0);
    return true;
}

} // namespace mir
