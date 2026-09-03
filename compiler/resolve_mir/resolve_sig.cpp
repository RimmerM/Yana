#include "resolve_internal.h"
#include "name.h"
#include "own.h"
#include "print.h"

namespace mir {

/*
 * The one sentence this stage says about the heap it has not got.
 *
 * Its own function rather than `unsupported`, because what an author needs here is the two
 * spellings that *do* work: "not implemented yet" alone points at nothing to write instead. One
 * function rather than the sentence twice, because two places reach it and they are the same
 * refusal - a signature position that *owns* a growable array, and an ascription that asks for one.
 * `StorageClass::Heap` is a rung `verifyBody` refuses outright, so either would be a position
 * nothing could ever fill.
 */
void Resolver::reportOwnedSequence(LocationId source) {
    context.diagnostics.error(
        "a position that owns a growable array needs an allocator, which this stage has not got - write a window `[T]` in a parameter, or a fixed `[T *n]`"_v,
        source);
}

/*
 * The sigils on a written type, peeled outermost first - see `WrittenSigils`.
 *
 * A loop rather than one test, so that `&'T` written as two nested nodes and `&a'T` written as one
 * reach the same answer: what the parser produces for a reference is a kind plus an optional group,
 * and the shape a reader wrote is not the shape the tree has.
 *
 * `access` is refined rather than answered, because the position's default is what it starts as and
 * a sigil only ever strengthens it: `&` makes it exclusive from either side, and `'` makes an owning
 * position borrow while leaving an already-borrowing one alone. `sunk` is the one thing that stops
 * the second of those, and the caller reports the contradiction rather than this - a `->` that also
 * ticks is a statement about the *position*, which this function does not have.
 */
bool Resolver::peelSigils(ast::Type& written, Access& access, bool sunk, WrittenSigils& into) {
    into.type = &written;
    into.tick = false;

    for(;;) {
        auto type = into.type;

        if(type->kind == ast::Type::Borrow) {
            access = Access::Unique;
        } else if(type->kind == ast::Type::Shared) {
            if(access == Access::Owned && !sunk) access = Access::Shared;
        } else {
            break;
        }

        if(type->ref.group) {
            if(into.tick && into.group != type->ref.group) {
                context.diagnostics.error("this names two origins, %@ and %@"_v, type->source,
                                          context.findName(into.group),
                                          context.findName(type->ref.group));
                return false;
            }

            into.group = type->ref.group;
        }

        into.tick = into.tick || type->kind == ast::Type::Shared || type->ref.group;
        into.type = parse[type->ref.to];
    }

    return true;
}

/*
 * A written occurrence - Analysis-Borrows-Revised.md §2.1's two tables, in one function.
 *
 * The access comes from either sigil and the origin from the tick, and the two are independent axes:
 *
 * | Written                | Access | Origin        |
 * | ---------------------- | ------ | ------------- |
 * | `x: T` (parameter)     | shared | call-bounded  |
 * | `T` (result, field)    | owned  | here          |
 * | `&x: T` / `x: &T`      | unique | call-bounded  |
 * | `->x: T`               | owned  | here          |
 * | `x: 'T` / `x: a'T`     | shared | slot          |
 * | `&x: a'T` / `x: &a'T`  | unique | slot          |
 *
 * `&x: T` and `x: &T` are one declaration, and deliberately: they were two in the old stage - same
 * effect, same ABI, different function types - so a function value written one way could not be
 * passed where the other was expected, with nothing behind the difference.
 *
 * What is refused is an unqualified `&` outside a parameter. "`&T` without an external-owner
 * qualifier is therefore not a stored reference spelling": a result or a field that holds an
 * exclusive loan has to say whose it is, because nothing at the use site can work it out.
 */
bool Resolver::resolveOccurrence(ast::Type& written, ast::BindType bind, Position position,
                                 SlotScope& slots, Occurrence& into) {
    // A parameter and a hand-over are both *binding* positions and default to a borrow; a result and
    // a field are owned. That is Analysis-Borrows-Revised.md §2.1's two tables plus §3.3's sentence
    // about a yield, which says the same thing about the same kind of position.
    auto binds = position == Position::Parameter || position == Position::Handover;
    auto access = binds ? Access::Shared : Access::Owned;
    auto sunk = false;

    switch(bind) {
        case ast::BindType::Borrow: break;
        case ast::BindType::Ref:    access = Access::Unique; break;
        case ast::BindType::Sink:   access = Access::Owned; sunk = true; break;
    }

    WrittenSigils sigils;
    if(!peelSigils(written, access, sunk, sigils)) return false;

    auto type = sigils.type;
    auto tick = sigils.tick;
    auto group = sigils.group;

    if(sunk && tick) {
        context.diagnostics.error(
            "`->` hands the value over and `'` says somebody else keeps it, so this asks for both"_v,
            written.source);
        return false;
    }

    /*
     * `[T]` in a *binding* position asks for a window - `doc/spec/collections.md`'s table, and
     * `AccessForm`'s own note.
     *
     * The **spelling** decides, and that is the one place in this function where it does: `xs: [T]`
     * is a shared element window and `xs: Array(T)` is shared access to the whole growable owner,
     * and they are the same logical type. That is exactly the distinction an access form exists to
     * carry - the shipping stage carries it as a *second type*, `sliceCapabilityType`'s sibling
     * record, and pays for it in `substituteType`, in `bindingType` and at every one of the sites
     * that has to ask "or a sibling of one".
     *
     * A `[T *n]` is never a window and needs no clause here: its count is in its type, so the two
     * words a descriptor would carry say nothing the type did not, and building one throws the
     * number away. It adapts to a window at a *call* instead, which is where the caller that wants
     * one asks for it.
     *
     * An owning position - a result, a field, `->xs` - keeps `WholeValue`, because what it owns is
     * the array and not a view of one.
     */
    auto window = type->kind == ast::Type::Arr && !type->arr.length && binds && !sunk;

    Contract contract;
    if(tick) {
        // A parameter and a field *publish* a slot; a result and a hand-over may only name one that
        // was published. The relationship has to be stated at both ends of the signature, which is
        // what makes `-> 'Object` on a signature with nothing to relate it to an error.
        auto slot = slots.find(group, position == Position::Parameter || position == Position::Field);
        if(slot == SlotScope::kNone) {
            /*
             * §2.2's `fn invalid(xs: [Object]) -> 'Object`.
             *
             * "An unmarked parameter is never silently promoted into a published slot because a
             * result happens to contain `'`." The signature has to state the relationship on both
             * ends, so a result naming an origin no parameter published is the error rather than
             * the instruction to go and find one.
             */
            if(group != StringId()) {
                context.diagnostics.error("no parameter publishes the origin %@"_v, written.source,
                                          context.findName(group));
            } else {
                context.diagnostics.error("no parameter publishes an origin for this to be in"_v,
                                          written.source);
            }

            return false;
        }

        contract = slotContract(access, slot,
                                window ? AccessForm::SequenceWindow : AccessForm::WholeValue);
    } else if(access == Access::Owned) {
        contract = ownedContract();
    } else if(binds) {
        contract = borrowedContract(access,
                                    window ? AccessForm::SequenceWindow : AccessForm::WholeValue);
    } else {
        // A result or a field that borrows and does not say from where. See the note above: there
        // is nothing at a use site that could work out whose storage it is.
        context.diagnostics.error(
            "this borrows and does not say from where - write `'` or a named origin on the type"_v,
            written.source);
        return false;
    }

    DeclPtr<Type> logical;
    DeclPtr<OwnershipSchema> shape;
    if(!resolveQualified(*type, position, slots, logical, shape)) return false;

    // The one gate on what a *value* may be of, and it is here because this is the one function
    // every parameter, result and field goes through. A count has no manifestation, so the position
    // that would have asked for one is the position that reports.
    if(!requireValueType(logical, written.source)) return false;

    /*
     * An **owned** growable array, which nothing in this stage can produce.
     *
     * There is no allocator here - `StorageClass::Heap` is a rung `verifyBody` refuses outright -
     * so a position that owns one is a position nothing could ever fill. Refused where it is
     * written, naming what is missing, rather than laid out as the two words the library happens to
     * declare and then failing at whatever tried to build one.
     *
     * A **window** is unaffected and is the whole point: it borrows storage somebody else owns, and
     * for the only donor this stage has - a `[T *n]` - that storage is a frame slot.
     */
    if(contract.isOwned() && isSequenceType(decl(), program, logical)) {
        reportOwnedSequence(written.source);
        return false;
    }

    into = Occurrence { logical, contract, shape };
    return true;
}

/*
 * The written type after its root sigils, and the interior positions it binds - `Reader(a')`.
 *
 * Analysis-Borrows-Revised.md §2.2's loan argument, which "does not occupy a parameter position or
 * count towards arity": `Reader(a')` **is** the type `Reader` - same interning, same layout, same
 * instances - and what the argument supplies is which of *this* scope's origin sets fills each of
 * the record's interior positions.
 *
 * That mapping is a renumbering and nothing more. `Reader`'s schema says where its external
 * positions sit, in the record's own slot scope; the shape built here has the same paths and the
 * same capabilities, with each slot number replaced by the one the argument named. Which is why the
 * two shapes intern into one table: they describe the same structure, read in two scopes.
 *
 * A use with no argument - a bare `x: Reader` - keeps a null shape and falls back on the record's
 * own schema through `shapeOf`. The two are not the same statement and `originSlotAt` is where the
 * difference is spent: an unbound interior position names no origin *here*, which is what makes
 * returning one a diagnostic rather than a slot number read in the wrong scope.
 */
bool Resolver::resolveQualified(ast::Type& written, Position position, SlotScope& slots,
                                DeclPtr<Type>& logical, DeclPtr<OwnershipSchema>& shape) {
    shape = nullptr;

    if(written.kind == ast::Type::Ptr) {
        return resolvePointee(written, position, &slots, logical, shape);
    }

    if(written.kind != ast::Type::App) {
        logical = resolveType(written);
        return logical != nullptr;
    }

    return resolveApplication(written, position, &slots, logical, shape);
}

/*
 * `%source'a` - the storage a pointer names, as one interior position.
 *
 * Analysis-Lens.md §12.2's requirement, at the one type whose interior has no field to name it. A
 * pointer's *own* qualification is the root `Contract` like anything else's - `x: %U8` is a copied
 * scalar - and what this adds is a statement about the pointee: `%source'a` says the address may
 * point into `source`'s storage, so those owners stay live until the pointer's last use.
 *
 * **The pointer is still an ordinary owned scalar.** Nothing here changes its representation, its
 * width or what may be done with it; a raw pointer is where the safe model stops by construction,
 * and the qualifier preserves *liveness* and nothing else. It proves no validity, no
 * initialization, no alignment and no exclusivity, which is the same sentence
 * `doc/spec/native.md` makes about `borrow`.
 *
 * The sigils are read by `peelSigils`, which is what a root position reads, so `%&source'a` and
 * `%source'a` differ in exactly the way `&source'T` and `source'T` do one level up. What is refused
 * is an *unqualified* borrow of the pointee - `%&a` - for the reason a result that borrows without
 * saying from where is refused: there is nothing at a use site that could work out whose storage it
 * is, and a pointer is the last place to start guessing.
 */
bool Resolver::resolvePointee(ast::Type& written, Position position, SlotScope* slots,
                              DeclPtr<Type>& logical, DeclPtr<OwnershipSchema>& shape) {
    shape = nullptr;
    logical = nullptr;

    // The pointee's own position is owned by default - it is not a binding, it is what the pointer
    // names - so a tick there is a statement and not the absence of one.
    auto access = Access::Owned;
    WrittenSigils sigils;
    if(!peelSigils(*parse[written.to], access, false, sigils)) return false;

    auto pointee = resolveType(*sigils.type);
    if(!pointee) return false;
    if(!requireValueType(pointee, sigils.type->source)) return false;

    logical = program.types.pointerType(pointee);

    if(!sigils.tick) return true;

    /*
     * A position with no schema scope has nothing to number an origin in - a pointee of a pointee, a
     * type argument of an application. Same refusal `resolveApplication` makes about a loan
     * argument, and for the same reason: the relationship would have no end to be stated at.
     */
    if(!slots) {
        context.diagnostics.error(
            "an origin can only be named where a signature or a field publishes one"_v,
            written.source);
        return false;
    }

    auto slot = slots->find(sigils.group,
                            position == Position::Parameter || position == Position::Field);
    if(slot == SlotScope::kNone) {
        if(sigils.group != StringId()) {
            context.diagnostics.error("no parameter publishes the origin %@"_v, written.source,
                                      context.findName(sigils.group));
        } else {
            context.diagnostics.error("no parameter publishes an origin for this to be in"_v,
                                      written.source);
        }

        return false;
    }

    ExternalSlot position_ {};
    position_.path.push(program.decl, PathStep::pointee());
    position_.contract = slotContract(access, slot, AccessForm::WholeValue);

    shape = program.shapes.intern(&position_, 1);
    return true;
}

/*
 * `a.field: b` - one structural field requirement, read into a `GenProperty`.
 *
 * The subject is written as a bare name rather than as a type, which is what the grammar has and
 * what the feature means: a requirement is about a *parameter* of this signature, so `Boxed(a).value`
 * is not a thing to write - a concrete subject has a field the resolver can simply look up, and a
 * requirement about one is a fact with nothing for a caller to prove.
 *
 * The subject may be introduced here, on `readClassRequirement`'s terms: `fn (a.size: I32) f(x: a)`
 * says `a` for the first time in the constraint list, and the numbering is the order the list is in.
 */
bool Resolver::readFieldRequirement(ast::Constraint constraint, DeclPtr<GenContext> gen,
                                    GenProperty& into) {
    GenScope generics(*this, gen);

    auto subject = genVariable(program, gen, constraint.field.typeName, constraint.source);
    if(!subject) {
        context.diagnostics.error("%@ is not a type this declaration binds"_v, constraint.source,
                                  context.findName(constraint.field.typeName));
        return false;
    }

    if(((VariableType*)decl()[subject])->isConst()) {
        context.diagnostics.error("%@ counts, so it has no fields"_v, constraint.source,
                                  context.findName(constraint.field.typeName));
        return false;
    }

    auto result = resolveType(*parse[constraint.field.type]);
    if(!result || !requireValueType(result, constraint.source)) return false;

    into.subject = subject;
    into.field = constraint.field.fieldName;
    into.result = result;
    into.source = constraint.source;
    return true;
}

/*
 * `f: (a) -> b` - one constrained callable, read into a `GenCallable`.
 *
 * The signature is stored decomposed - see `GenCallable` - and read here with the *signature's* own
 * occurrence rules deliberately not applied: what a requirement states is a shape to select a
 * function by, and the conventions each position carries are the selected function's own. A written
 * `&` or `->` inside one would be saying something about a callee this constraint does not declare.
 */
bool Resolver::readCallableRequirement(ast::Constraint constraint, DeclPtr<GenContext> gen,
                                       GenCallable& into) {
    GenScope generics(*this, gen);

    auto& written = *parse[constraint.fun.type];
    if(written.kind != ast::Type::Fun) {
        context.diagnostics.error("%@ is required to be callable, so what follows the colon is a function type"_v,
                                  constraint.source, context.findName(constraint.fun.name));
        return false;
    }

    auto& signature = *parse[written.fun];

    if(signature.kind != ast::FunKind::Plain) {
        unsupported(constraint.source, "a required lens or iterator"_v);
        return false;
    }

    into.name = constraint.fun.name;
    into.source = constraint.source;

    for(auto argument: signature.args.contents(parse)) {
        if(argument.bind != ast::BindType::Borrow) {
            unsupported(constraint.source, "a convention on a required callable's parameter"_v);
            return false;
        }

        auto type = resolveType(argument.type);
        if(!type || !requireValueType(type, constraint.source)) return false;

        into.args.push(program.decl, type);
    }

    into.result = resolveType(signature.ret);
    if(!into.result || !requireValueType(into.result, constraint.source)) return false;

    return true;
}

bool Resolver::resolveApplication(ast::Type& written, Position position, SlotScope* slots,
                                  DeclPtr<Type>& logical, DeclPtr<OwnershipSchema>& shape) {
    shape = nullptr;
    logical = nullptr;

    auto& application = *parse[written.app];

    if(application.base.kind != ast::Type::Con) {
        unsupported(application.base.source, "applying anything but a named type"_v);
        return false;
    }

    /*
     * A generic alias applied - `alias Bytes(a) = Array(a)` at `Bytes(I32)`.
     *
     * Answered before the record lookup and with the *target*, substituted: an alias is a spelling,
     * so what comes back is `Array(I32)` and nothing below this line ever learns that a second name
     * for it exists. An **origin** argument is refused there rather than here, because an alias has
     * no schema of its own for one to be numbered against - what publishes origins is the record the
     * target names, and it is reached through its own name.
     */
    if(auto alias = mir::findAlias(context, program, here(), application.base.name, written.source)) {
        SmallArray<DeclPtr<Type>, 4> arguments;

        for(auto argument: application.args.contents(parse)) {
            if(argument.kind == ast::Type::Loan) {
                context.diagnostics.error("%@ is an alias, and an origin is named on the type it stands for"_v,
                                          argument.source,
                                          context.findName(application.base.name));
                return false;
            }

            auto resolved = resolveTypeArgument(argument, decl()[alias]->gen, arguments.size());
            if(!resolved) return false;

            arguments.push(resolved);
        }

        logical = applyAlias(alias, arguments.pointer(), arguments.size(), written.source);
        return logical != nullptr;
    }

    auto found = mir::findRecord(context, program, here(), application.base.name, written.source);
    if(!found) {
        // A builtin name applied to something, or a name nothing declares. Both are "there is no
        // declaration with parameters here", which is what an application needs and neither has.
        context.diagnostics.error("%@ is not a type declaration this can be applied to"_v,
                                  application.base.source,
                                  context.findName(application.base.name));
        return false;
    }

    auto& declaration = *decl()[found];

    /*
     * Two argument lists in one, split by what was written - Analysis-Borrows-Revised.md §2.2.
     *
     * A loan argument "does not occupy a parameter position or count towards arity", so the two are
     * counted separately and checked against different things: the type arguments against the head's
     * parameters, and the origin arguments against the record's schema scope. They may be written in
     * any order, which is why this is one walk that sorts rather than two walks that skip.
     */
    SmallArray<DeclPtr<Type>, 4> arguments;
    SmallArray<U16, 4> binding;

    for(auto argument: application.args.contents(parse)) {
        if(argument.kind != ast::Type::Loan) {
            auto resolved = resolveTypeArgument(argument, declaration.gen, arguments.size());
            if(!resolved) return false;

            arguments.push(resolved);
            continue;
        }

        // A position with no schema scope - a pointee, a nested type argument - has nothing to
        // number an origin in, so the relationship it would state cannot be written down.
        if(!slots) {
            context.diagnostics.error(
                "an origin can only be named where a signature or a field publishes one"_v,
                argument.source);
            return false;
        }

        auto slot = slots->find(argument.name,
                                position == Position::Parameter || position == Position::Field);
        if(slot == SlotScope::kNone) {
            context.diagnostics.error("no parameter publishes the origin %@"_v, argument.source,
                                      context.findName(argument.name));
            return false;
        }

        binding.push(slot);
    }

    logical = instantiateRecord(context, program, found, arguments.pointer(), arguments.size(),
                                written.source);
    if(!logical) return false;

    /*
     * `data (Eq(a)) Set(a)` - what the head requires of the arguments this application supplied.
     *
     * Here rather than in `instantiateRecord`, and the difference is which question is being asked.
     * `instantiateRecord` also runs from `substituteType`, where the record is being *rebuilt* under
     * a specialization whose own call site already discharged everything; what has to be checked is
     * a head somebody **wrote**, which is exactly the applications this walk reaches. Discharging in
     * both places would report a specialization's arguments against a declaration nobody wrote at
     * that point in the source.
     */
    if(found && decl()[found]->gen) {
        Substitution applied { decl()[found]->gen, arguments.pointer(), arguments.size() };

        for(auto requirement: decl()[decl()[found]->gen]->classes.contents(decl())) {
            if(dischargeRequirement(requirement, applied, written.source)) continue;
            return false;
        }
    }

    if(!binding.size()) return true;

    /*
     * The origin arguments, as a renumbering of the record's own schema.
     *
     * `Reader`'s schema says where its external positions sit, in the record's own slot scope; the
     * shape built here has the same paths and the same capabilities, with each slot number replaced
     * by the one the argument named. Which is why the two intern into one table: they describe the
     * same structure, read in two scopes.
     *
     * Read off the *instantiation*, not the declaration, because substitution is what decides which
     * fields it has - a generic record's external positions are its own, and `Boxed(I32)`'s schema
     * is the fold over the fields `I32` produced.
     */
    auto& record = *(RecordType*)decl()[logical];

    if(binding.size() != record.originSlots) {
        context.diagnostics.error("%@ has %@ origin slots and this names %@"_v, written.source,
                                  context.findName(record.name), U32(record.originSlots),
                                  U32(binding.size()));
        return false;
    }

    // Nothing to renumber: a record with origin slots but no external position cannot be declared,
    // since a slot is introduced by the field that uses it.
    if(!record.schema) return true;

    SmallArray<ExternalSlot, 8> mapped;

    for(auto slotPointer: decl()[record.schema]->slots.contents(decl())) {
        auto& declared = *decl()[slotPointer];
        assertTrue(declared.contract.origin() == Origin::Slot);

        ExternalSlot bound;
        for(auto step: declared.path.contents(decl())) bound.path.push(program.decl, step);
        bound.contract = slotContract(declared.contract.access(), binding[declared.contract.slot()],
                                      declared.contract.form());

        mapped.push(bound);
    }

    shape = program.shapes.intern(mapped.pointer(), mapped.size());
    return true;
}

/*
 * A **callback position** - what a lens's continuation parameter and a `@lazy` parameter both are,
 * and the one place the contract behind them is chosen.
 *
 * A shared, call-bounded borrow of a function type, which is the whole of the safety argument for
 * both constructs. The three guarantees `doc/spec/lenses.md` states about a continuation - at most
 * once, during this activation, never kept - and the one `doc/spec/functions.md` states about a
 * thunk are what `Origin::CallBounded` already means to every pass in this stage:
 * `checkCarriedOrigins` refuses such a value at an owned position, so the callee cannot store it,
 * return it or hand it to anything that outlives the call. That is what licenses the *caller*
 * building the callback's environment on its own frame rather than on a heap this stage does not
 * have, and it is a property of the position rather than of which construct made one - which is why
 * it is settled here and not once per construct.
 */
static Occurrence callbackPosition(Program& program, const FunParam* params, Size count,
                                   const Occurrence& result) {
    auto type = program.types.funType(*program.decl, params, count, result);
    return Occurrence { type, borrowedContract(Access::Shared), nullptr };
}

/*
 * `@lazy` - `doc/spec/functions.md`'s Deferred arguments, at the declaration.
 *
 * The whole of the marker is a rewrite of the position's occurrence: what the callee holds is a
 * nullary thunk over the caller's frame, so the position becomes `callbackPosition`'s - the same one
 * `declareContinuation` gives a lens, and for the same reason. From here nothing else in the
 * resolver knows what `@lazy` is except the two places that have to - the call site, which builds
 * the thunk, and a read of the name, which calls it.
 *
 * The thunk **answers an owned value**, whatever convention the position was written with. A borrow
 * has to be a borrow *of* something, and what a thunk produces is a fresh value of the caller's
 * making with no place behind it - so `p: Point` under `@lazy` is not the shared borrow a strict
 * parameter of that type would be, and saying so here is what keeps the callee from believing it may
 * project storage somebody else owns.
 *
 * Three shapes are refused, each because the marker and the shape describe the same position
 * differently and one of them would have to be ignored:
 *
 *  - `&` and `->`, because what arrives is the thunk and not the value: there is nothing to borrow
 *    from and nothing to consume.
 *  - an external origin (`x: 'T`), because a published loan is a relationship to the *caller's*
 *    origin sets, and the thunk's result is made after the call started.
 *  - a `lens fn` or an `iter fn`, which resolve their arguments before lifting the rest of the
 *    block into the continuation - so the argument would run whether or not the callee forced it,
 *    which is the one thing the marker promises it does not do.
 */
bool Resolver::declareDeferred(Function& function, const ast::Arg& argument, Occurrence& occurrence) {
    if(function.lens) {
        context.diagnostics.error(
            "%@ resolves its arguments before it hands over, so a `@lazy` parameter here would be evaluated whether or not it was read - take a function and call it"_v,
            argument.source, function.iterator ? "an iterator"_v : "a lens"_v);
        return false;
    }

    if(argument.bind != ast::BindType::Borrow) {
        context.diagnostics.error(
            "a `@lazy` parameter receives a thunk rather than the value, so there is nothing here for `%@` to borrow from or consume"_v,
            argument.source, argument.bind == ast::BindType::Sink ? "->"_v : "&"_v);
        return false;
    }

    if(occurrence.contract.origin() == Origin::Slot) {
        context.diagnostics.error(
            "a `@lazy` parameter's value is made after the call started, so there is no caller origin for it to be published to"_v,
            argument.source);
        return false;
    }

    occurrence = callbackPosition(program, nullptr, 0, owned(occurrence.type));
    return true;
}

/*
 * `= expr` on a parameter - `doc/spec/functions.md`'s Default arguments.
 *
 * The constant itself is `declareDefault`'s, which a field default goes through too. What is here is
 * the four refusals, and each is about the **caller** rather than about the constant - which is what
 * makes this the right place for them and not the constant resolver:
 *
 *  - An `instance` member. What a call may leave out is read off the class signature, before an
 *    instance is selected, for the same reason strictness is - so a default written here is a
 *    constant nothing would ever pass.
 *  - `&`. The callee writes through the position, and a call that left it out would be writing into
 *    a temporary built from the constant, which nothing reads afterwards.
 *  - A published origin (`x: 'T`). The loan is a relationship to the *caller's* origin sets, and a
 *    call that omits the position has no root of its own to bind that slot to.
 *  - A position declared as a type variable, which `declareDefault` refuses for both callers.
 *
 * `->` is deliberately not among them, and the spec's list does not have it either: a consuming
 * position takes an owned value, and a constant built at the call site is exactly one. Neither is
 * `@lazy` - see the note at the promised type below.
 */
bool Resolver::declareArgDefault(const ast::Arg& argument, const Occurrence& occurrence,
                                 bool deferred, bool implementing, Constant& into) {
    auto& expr = *parse[argument.def];

    if(implementing) {
        context.diagnostics.error(
            "an instance may not write a default - a call reads what it may leave out off the class's signature, before an instance is selected"_v,
            argument.source);
        return false;
    }

    if(argument.bind == ast::BindType::Ref) {
        context.diagnostics.error(
            "%@ is written through, so a call that left it out would be handed a temporary nothing reads afterwards"_v,
            argument.source, context.findName(argument.name));
        return false;
    }

    if(occurrence.contract.origin() == Origin::Slot) {
        context.diagnostics.error(
            "%@ publishes an origin, so a call that left it out would have no root of its own to bind it to"_v,
            argument.source, context.findName(argument.name));
        return false;
    }

    /*
     * The type the constant has to be of, which for a `@lazy` position is not the one on the
     * occurrence: `declareDeferred` has already rewritten that into `() -> T`, and what the author
     * wrote - and what an omitted position has to produce - is the `T`.
     *
     * Read back off the thunk type rather than saved before the rewrite, because a position
     * *inherited* from a class signature arrives already rewritten and has no before to save. One
     * expression that is right for both is better than two that agree.
     *
     * The marker and a default get along, and the objection to them is about a construct this
     * language does not have. `doc/spec/functions.md` used to say the default "would be one more
     * expression the call site did not write and the callee may not run" - but a default is a
     * *constant*, so there is no expression, nothing to run and nothing to observe if the callee
     * never forces it. What a call that omits the position passes is `defaultThunk`'s nullary
     * function over that constant, built once per parameter.
     */
    auto promised = deferred ? ((FunType*)decl()[occurrence.type])->result.type : occurrence.type;

    into = declareDefault(expr, promised, argument.name, "parameter"_v, "call"_v);

    return into.isSet();
}

/*
 * The signature's parameters, in order.
 *
 * Answers false when the declaration is one this stage cannot represent, in which case the function
 * is dropped whole rather than left with a signature a call would be checked against. A partially
 * declared signature is worse than none: every call to it reports against positions the author did
 * not write.
 */
bool Resolver::declareArgs(Function& function, ast::Decl& declaration, SlotScope& slots,
                           Function* inherited, const Substitution& inheritedWith) {
    auto& written = declaration.fun;
    auto ok = true;

    for(auto argument: written.args.contents(parse)) {
        if(argument.caller) {
            unsupported(argument.source, "a `@caller` parameter"_v);
            ok = false;
            continue;
        }

        /*
         * `return x: T` - the older spelling of what `'` now says.
         *
         * Refused rather than translated. It is the same statement as `x: 'T` written in the name
         * area instead of the type, and the name area is where an argument *pattern* would go; one
         * spelling per concept is what keeps the two from having to be told apart later.
         */
        if(argument.returnRoot) {
            // A refusal and not an "unsupported yet": there is nothing here left to implement, and
            // saying there is would invite an author to wait for it.
            context.diagnostics.error(
                "a `return` parameter is the older spelling of `'` - write it on the type, as `x: 'T`"_v,
                argument.source);
            ok = false;
            continue;
        }

        Occurrence occurrence;

        // Whether this position is deferred, and whether the position itself came from the signature
        // this one implements - which is what decides whether the marker still has a rewrite to do.
        auto deferred = argument.lazy;
        auto inheritedType = false;

        /*
         * A position with no written type, taken from the signature this one inherits.
         *
         * The **whole occurrence** and not just the logical type: what the class declared is
         * `->value: a` or `x: a`, and an implementation that inherited only the type would silently
         * pick this walk's default convention for the position instead of the one every caller is
         * compiled against. That is the same three-part comparison `checkMemberSignature` makes, so
         * inheriting anything less would be inheriting a signature the check would then reject.
         */
        if(!argument.type) {
            if(!inherited || function.args.size() >= inherited->args.size()) {
                unsupported(argument.source, "an inferred parameter type"_v);
                ok = false;
                continue;
            }

            auto& position = *decl()[inherited->args.get(decl(), U32(function.args.size()))];

            occurrence = substituteOccurrence(context, program, position.occurrence, inheritedWith);
            inheritedType = true;

            /*
             * `@lazy` with it, because it is part of the position and not a second statement about
             * it: what was inherited is the thunk type the marker produced, and a parameter holding
             * one without the marker is a `() -> T` the body has to call by hand - which is not the
             * signature the class declared and not what its callers are compiled against.
             *
             * A *written* marker here says nothing new where it agrees, since the type it would
             * rewrite is the rewritten one already. Where it disagrees it is reported now rather
             * than through the signature comparison, which has two identical occurrences to compare
             * and nothing to show.
             */
            deferred = position.lazy;

            if(argument.lazy && !position.lazy) {
                context.diagnostics.error(
                    "%@ is not `@lazy` in the signature this implements, and a deferred position is not something an implementation may add"_v,
                    argument.source, context.findName(argument.name));
                ok = false;
                continue;
            }
        } else if(!resolveOccurrence(*parse[argument.type], argument.bind, Position::Parameter,
                                     slots, occurrence)) {
            ok = false;
            continue;
        }

        /*
         * Two parameters of one name.
         *
         * Refused rather than shadowed. `findLocal` walks the bindings backwards, so the second
         * would win every mention of the name in the body and the first would be a parameter the
         * callee cannot read at all - which is a silent wrong answer rather than a missing feature.
         */
        auto duplicate = false;
        for(auto existing: function.args.contents(decl())) {
            if(decl()[existing]->name != argument.name) continue;

            context.diagnostics.error("this function already has a parameter called %@"_v,
                                      argument.source, context.findName(argument.name));
            duplicate = true;
            break;
        }

        if(duplicate) {
            ok = false;
            continue;
        }

        // Only a marker over a *written* type rewrites the position: an inherited one arrives already
        // rewritten, and running the rewrite again would make a thunk answering a thunk.
        if(argument.lazy && !inheritedType && !declareDeferred(function, argument, occurrence)) {
            ok = false;
            continue;
        }

        // After the marker, because the marker is what decides whether this position may carry one
        // at all - and after the position is otherwise finished, so that what a default is checked
        // against is the type the callee actually holds.
        Constant def;
        if(argument.def && !declareArgDefault(argument, occurrence, deferred, inherited != nullptr,
                                              def)) {
            ok = false;
            continue;
        }

        auto declared = new (program.decl) Arg();
        declared->name = argument.name;
        declared->occurrence = occurrence;
        declared->source = argument.source;
        declared->index = U32(function.args.size());
        declared->lazy = deferred;
        declared->def = def;

        function.args.push(program.decl, declared - *program.decl);
    }

    return ok;
}

/*
 * `Proceed(exit) | Exit(reason)` - what a *skipping* lens's call actually answers with.
 *
 * Analysis-External-Iterators.md §2.2's three terminal outcomes, minus the two the exit signal
 * already distinguishes: `Completed` and `ConsumerLeft` are rows of the discriminant a continuation
 * returns, and `Failed` is the one that carries something the discriminant cannot. So the wrapper
 * appears exactly where the traversal declared a failure channel, and every lens without one keeps
 * the bare signal it has today - which is what makes this cost nothing where it says nothing.
 *
 * **The library's `Outcome(a, e)`, instantiated at the exit signal and the failure type** - Core's
 * own `data Outcome(a, e) = Proceed(a) | Exit(e)`, claimed through `known.def`'s `outcome` role.
 * It used to be a record synthesized here per skipping lens, with the two constructors spelled in
 * this file, on the argument that nothing outside this function constructed or matched it. That
 * argument held and was still no reason for the compiler to carry a second declaration of a type
 * the library declares: what the compiler needs is to name the record and number its
 * constructors, and the role gives it both. A call site reads `Function::result` and never builds
 * the type itself, and the instantiation is interned, so two skipping lenses at one failure type
 * answer one type - which the synthesized form could not say.
 *
 * `kOutcomeProceed` and `kOutcomeExit` are the constructor indices, and `checkDeclaredRoles` is
 * what holds the declaration to them: proceeding is the first case, one field each.
 */
DeclPtr<RecordType> Resolver::declareOutcome(Function&, DeclPtr<Type> failure, LocationId source) {
    DeclPtr<Type> arguments[2] = { program.types.exitCode(), failure };

    auto type = instantiateRecord(context, program, program.roles.record(intrinsic::Known::outcome),
                                  arguments, 2, source);
    if(!type) return nullptr;

    return DeclPtr<RecordType>((RecordType*)decl()[type] - *program.decl);
}

/*
 * The `yield` form's desugaring - Analysis-Lens.md §5.2, and Analysis-IR.md §11.4's W-A.
 *
 *      lens fn f(a) -> P        declares      fn ($r) f(a, body: (P) -> $r) -> $r
 *
 * One sentence, and everything else about milestone 7 is a consequence of it. A lens becomes an
 * ordinary function with one extra parameter: its body is resolved once and converted once, `yield`
 * is a call of that parameter, and the exactly-once rule is a walk over the callee's own blocks
 * rather than something a call site re-derives. Nothing below this function knows what a lens is
 * except the two places that have to - the call site, which splits its own block, and the
 * conversion, which decides how the continuation is passed.
 *
 * **`$r` is a variable and not the exit code it is currently always instantiated at.** What the
 * continuation answers with is a manifestation choice (§12.1's fifth item), so the call site picks
 * it; making it a *variable* is what keeps this declaration and milestone 6's explicit-callback form
 * one shape, and it is where the flat form's block value goes. A *skipping* lens is the one kind
 * that does not use it: `| E` fixes what the continuation answers, and the wrapper `declareOutcome`
 * builds is what the function answers instead - see `Function::continuationResult`.
 *
 * **The parameter is borrowed and call-bounded, which is the whole of the safety argument** - and
 * is `callbackPosition`'s, which a `@lazy` parameter is under too.
 */
bool Resolver::declareContinuation(Function& function, Function* inherited,
                                   const Substitution& inheritedWith, LocationId source) {
    /*
     * What the continuation answers, which is the one thing an iterator settles and a lens leaves
     * open - Analysis-Lens.md §7.3's "it fixes both types rather than letting the author declare
     * them".
     *
     * A lens answers whatever its continuation answered, so that is a variable its call site binds.
     * An iterator's continuation is a **loop body**, and what a loop body answers is the step signal
     * - `keep going`, `break`, `return` - which is a compiler ABI and not a type the author has or
     * could write. Fixing it here is what makes the iterator's own `yield` able to *test* the
     * answer, which is the whole of how a `break` stops an iterator.
     *
     * A **class member** fixes it for a third reason: its context is the class's and was closed when
     * the head was read, so there is nowhere for `$r` to be a variable of. That costs nothing today -
     * every call site binds `$r` to exactly this signal (see `handoverCall`) - and it is what makes a
     * member's continuation position a shape a witness table can hold, since the one thing such a
     * table cannot describe is a result the *caller* chose.
     */
    if(inherited) {
        /*
         * An **instance member**, which implements a protocol rather than stating one.
         *
         * Taken from the class whole rather than rebuilt from what the implementation wrote, and
         * that is not tidiness: `declareOutcome` synthesizes a record per skipping traversal, so two
         * derivations of one protocol would be two records with the same fields - one the call site
         * is typed against and one the callee returns. What the author *did* write is the hand-over
         * and the failure type, and `checkMemberSignature` compares those.
         */
        function.continuationResult = substituteOccurrence(context, program,
                                                           inherited->continuationResult,
                                                           inheritedWith);
        function.result = substituteOccurrence(context, program, inherited->result, inheritedWith);
    } else {
        /*
         * Fixed, for the three kinds that have something of their own to say about it, and a
         * variable for the plain module-level lens that has not.
         *
         * An iterator fixes it because its `yield` has to *test* the answer. A skipping lens fixes it
         * because its own result is the wrapper below, and a wrapper around a free variable would be
         * a type the call site has to pick a second argument for with nothing to pick it from - the
         * signal is what the continuation answers whatever `$r` was going to be. A class member
         * fixes it because it has no context to bind one in, which is what `genVariable` answering
         * null on a closed context says.
         */
        auto variable = function.iterator || function.failure
            ? nullptr
            : genVariable(program, function.gen, context.addUnqualifiedName("$r", 2), source);

        function.continuationResult = owned(variable ? variable : program.types.exitCode());

        /*
         * What the *function* answers, which is the continuation's answer only where nothing else
         * can happen to it - `Function::continuationResult`.
         *
         * A failure channel is the one thing that can, so a traversal with one answers the wrapper
         * and every other traversal answers the signal unchanged. This is the single place the two
         * are related, which is what keeps `finish`, the call site and the conversion from each
         * having to decide whether a lens is skipping.
         */
        if(function.failure) {
            function.result = owned(asBase<Type>(declareOutcome(function, function.failure, source)));
        } else {
            function.result = function.continuationResult;
        }
    }

    // The position the `yield` fills and the call site binds, declared exactly as written. What a
    // *loan of a trivially copyable type* physically is - a copy - is a question the conversion
    // answers, because a generic lens hands over an `a` and the signature has no way to know.
    FunParam parameter { function.handover };

    auto declared = new (program.decl) Arg();
    declared->name = context.addUnqualifiedName("$body", 5);
    declared->source = source;
    declared->index = U32(function.args.size());
    declared->occurrence = callbackPosition(program, &parameter, 1, function.continuationResult);

    function.continuationArg = U16(function.args.size());
    function.args.push(program.decl, declared - *program.decl);
    return true;
}

/*
 * The half of a `fn` declaration that is the same whether it stands in a module or in an instance.
 *
 * Answers null where the declaration is one this stage cannot represent. What it deliberately does
 * *not* do is register the name: a module function is reachable by name and an instance member is
 * reached by the compiler through its instance, and which of the two this is belongs to the caller.
 */
Function* Resolver::declareSignature(ast::Decl& declaration, StringId name,
                                     DeclPtr<GenContext> classContext, Function* inherited,
                                     Substitution inheritedWith) {
    auto& written = declaration.fun;

    /*
     * `lens fn` and `iter fn`, both in the `yield` form.
     *
     * One construct with one flag between them - Analysis-Lens.md §7.3 - so everything below is
     * written once: the hand-over position, the continuation parameter, the call site's region. The
     * explicit callback form (`body: (P) -> r`) needs `r` free in the signature and is milestone
     * 6's, which is where §12.4 puts it; a function-typed parameter is refused by `declareArgs` in
     * any case, so what reaches here is the sugar.
     */
    auto iterator = written.kind == ast::FunKind::Iter;
    auto lens = iterator || written.kind == ast::FunKind::Lens;

    if(lens && written.implicitReturn) {
        context.diagnostics.error(
            "%@ hands over with `yield`, so its body is a block rather than one expression"_v,
            declaration.source, iterator ? "an iterator"_v : "a lens"_v);
        return nullptr;
    }

    auto function = new (program.decl) Function(name, here());
    function->source = declaration.source;
    function->lens = lens;
    function->iterator = iterator;
    function->form = readGenForm(declaration);

    // `pub`. Here rather than at each caller because both of them - a plain `fn` and a class member -
    // register the name, and `Program::addFunctionName` reads this to decide which declaration a
    // module's name table holds. A member's own marker is overridden by its class head's answer in
    // `exportedSymbol`, which is where the rule that a `pub class` exports its signatures lives.
    function->exported = declaration.exported;

    /*
     * The emitted name, claimed here because this is the one place a written signature becomes a
     * `Function` - a plain `fn`, a class member and an instance member all arrive through it.
     *
     * A source name is no longer unique in a module: `fn negate(x)` and `fn negate(x, from)` are two
     * declarations of one name, and everything below the resolver keys a function by a single name.
     * So the symbol is claimed once, at the moment the declaration exists, rather than worked out
     * later by whoever needed one - which is where two answers would come from.
     */
    function->symbol = program.uniqueSymbol(here(), context, stringView(context.findName(name)));

    /*
     * The signature's generic context - `gen.h`.
     *
     * **Open**, and that is the difference from a `data` head: a function declares its type
     * variables by using them, so an unknown lowercase name in a parameter or a result introduces
     * one. The leading list is the explicit spelling of the same thing - `fn (a) id(->x: a)` - and
     * it exists so that a signature can name a variable no position mentions, which is what a
     * constraint over it needs. Both fill the same list, and the numbering is the order of first
     * occurrence across the constraints and then the signature.
     *
     * Created for every function whether or not it turns out to have variables. `isGeneric` is what
     * everything below asks, and an empty context answers it false - which keeps this walk from
     * having to decide up front something the signature has not said yet.
     */
    if(classContext) {
        // A class member's signature is generic in the *class's* parameter and binds nothing of its
        // own - the context is already closed, so a lowercase name the class did not declare is a
        // name nothing binds rather than a new variable.
        function->gen = classContext;
    } else {
        auto environment = new (program.decl) GenContext(true);
        function->gen = environment - *program.decl;
    }

    GenScope generics(*this, function->gen);

    for(auto constraint: written.constraints.contents(parse)) {
        /*
         * The five written forms, and what each one puts in the environment.
         *
         * `fn (a) id(->x: a)` declares a variable and nothing more, and `fn (n: I32) f()` declares
         * one that counts. The other three each state a requirement the caller proves and the body
         * spends: a class witness, a property witness, a function witness - Implementation-Generics.md
         * part 3's inventory, restricted to the entries this stage builds.
         *
         * One switch and not a chain of tests, because what differs between the arms is only which
         * list the requirement lands in: they are all read in this context's scope, they may all
         * introduce a variable by naming one, and they are all numbered by `freezeSchema` in the
         * order they were written.
         */
        if(constraint.kind == ast::Constraint::Any || constraint.kind == ast::Constraint::Const) {
            DeclPtr<Type> annotation;
            if(constraint.kind == ast::Constraint::Const) {
                annotation = resolveConstParameterType(*parse[constraint.constant.type]);
                if(!annotation) return nullptr;
            }

            auto name = constraint.kind == ast::Constraint::Const ? constraint.constant.name
                                                                 : constraint.name;

            if(findGenVariable(decl(), function->gen, name)) {
                context.diagnostics.error("%@ is already declared in this signature"_v,
                                          constraint.source, context.findName(name));
                return nullptr;
            }

            /*
             * A closed context is a class member's, whose variables are the class's - so an explicit
             * declaration here would be a parameter the class does not have and no caller supplies.
             */
            if(!genVariable(program, function->gen, name, constraint.source, annotation)) {
                context.diagnostics.error("%@ is a parameter of the class, so a member may not declare one"_v,
                                          constraint.source, context.findName(name));
                return nullptr;
            }

            continue;
        }

        /*
         * A class member's signature is generic in the *class's* context, which is shared with every
         * other member and with the class's own defaults - so a requirement written here would be
         * one the class did not state and one an instance was never asked to supply.
         */
        if(classContext) {
            unsupported(constraint.source, "a constraint on a class member"_v);
            return nullptr;
        }

        if(constraint.kind == ast::Constraint::Field) {
            GenProperty property;
            if(!readFieldRequirement(constraint, function->gen, property)) return nullptr;

            decl()[function->gen]->properties.push(program.decl, property);
            continue;
        }

        if(constraint.kind == ast::Constraint::Function) {
            GenCallable callable;
            if(!readCallableRequirement(constraint, function->gen, callable)) return nullptr;

            decl()[function->gen]->callables.push(program.decl, callable);
            continue;
        }

        if(!requireClassConstraint(constraint, "a constraint on a generic function"_v)) {
            return nullptr;
        }

        GenRequirement requirement;
        if(!readClassRequirement(constraint, function->gen, false, requirement)) return nullptr;

        decl()[function->gen]->classes.push(program.decl, requirement);
    }

    /*
     * The signature's own ownership schema scope - one per `fn`.
     *
     * The parameters are what publish a slot and the result is what may use one, so this is filled
     * by `declareArgs` and read by the result below, in that order. A scope per signature rather
     * than per module is §2.2's "signature-local": nothing outside can refer to a group, so the
     * names exist to match one occurrence against another within one declaration and to say which
     * one a diagnostic is about.
     */
    SlotScope slots;
    if(!declareArgs(*function, declaration, slots, inherited, inheritedWith)) return nullptr;

    /*
     * `-> ->T` and `-> &T` - the convention what leaves under, which only a hand-over has.
     *
     * The marker is a fact about what the *receiver* gets, so on a lens it is the third axis of
     * §3.3's binding position and is read like any other sigil. A plain function's result is owned
     * by the caller whatever is written, so a marker there says something the position cannot mean
     * and is refused rather than silently dropped - which would be right today and wrong the moment
     * the two spellings differ, with nothing to say when.
     */
    if(!lens && written.retBind != ast::BindType::Borrow) {
        unsupported(declaration.source, "a written result convention"_v);
        return nullptr;
    }

    /*
     * The result, or for a lens the hand-over.
     *
     * Absent is `{}` - a `:` body returns nothing and writes no result type at all - and an owned
     * occurrence, because a result the caller receives is theirs and there is nothing to borrow in
     * a unit. A written one goes through the same function every parameter did, with the scope the
     * parameters filled: `-> a'T` is a relationship to the storage bound to `a` at the call, and a
     * slot no parameter published is reported there rather than silently becoming a fresh one.
     */
    auto position = lens ? Position::Handover : Position::Result;
    auto& outgoing = lens ? function->handover : function->result;

    if(written.ret) {
        if(!resolveOccurrence(*parse[written.ret], written.retBind, position, slots, outgoing)) {
            return nullptr;
        }
    } else if(inherited) {
        /*
         * Inherited for the reason a parameter is - see `declareArgs`. An implementation that wrote
         * no result type is one the class already gave a result to, and `{}` here would be a
         * signature the member check then rejects for a mistake the author did not make.
         *
         * The **hand-over** for a lens, because that is the position `written.ret` would have
         * filled: what such a function *answers* is derived from the protocol and is inherited by
         * `declareContinuation` from the same place.
         */
        outgoing = substituteOccurrence(context, program,
                                        lens ? inherited->handover : inherited->result,
                                        inheritedWith);
    } else {
        outgoing = owned(program.types.unit);
    }

    function->originSlots = slots.count();

    /*
     * `<- R` - the resume channel, which this stage parses and does not implement.
     *
     * Analysis-External-Iterators.md §6.1 rejects it for *external* iteration's own reason, and for
     * an internal lens the reason it is not here yet is a different one: a continuation that answers
     * a value as well as the exit signal needs both to travel back, which is the same wrapper `| E`
     * introduces turned round - `Outcome(R, exit)` in place of the bare signal. Refused by name
     * rather than dropped, because a resume type silently ignored is a `yield` whose value is
     * garbage.
     */
    if(written.protocol.resume) {
        unsupported(parse[written.protocol.resume]->source, "a `<- R` resume value"_v);
        return nullptr;
    }

    /*
     * `| E` - the failure channel, and the whole of what makes a lens *skipping*.
     *
     * Read before `declareContinuation`, because what a traversal answers with depends on whether it
     * has one. A plain `fn` cannot reach here with a protocol - the parser only reads one after a
     * `lens`/`iter` - so what is checked is the case a written function *type* can produce.
     */
    if(written.protocol.failure) {
        if(!lens) {
            context.diagnostics.error(
                "only a `lens fn` or an `iter fn` can fail with `| E` - a plain function says so in its result type"_v,
                declaration.source);
            return nullptr;
        }

        function->failure = resolveType(*parse[written.protocol.failure]);
        if(!function->failure) return nullptr;

        /*
         * A failure type of the signature's own variables - refused, and this is the boundary of
         * what the wrapper is built for.
         *
         * `declareOutcome` makes a **concrete** record, so a failure type mentioning `a` would be a
         * generic record with no context to instantiate it through, and every call site would
         * project fields out of a shape it had not substituted. Making it generic is the ordinary
         * `instantiateRecord` path and is what this grows into; refusing it is what keeps the two
         * from disagreeing in the meantime.
         */
        if(!isConcreteType(decl(), function->failure)) {
            unsupported(parse[written.protocol.failure]->source,
                        "failing with a type the signature is generic in"_v);
            return nullptr;
        }
    } else if(inherited && inherited->failure) {
        // The channel with the result, and for the same reason: an implementation that wrote no
        // protocol is one the class already gave one to. A *written* one that disagrees is compared
        // rather than inherited, which is `checkMemberSignature`'s.
        function->failure = substituteType(context, program, inherited->failure, inheritedWith);
    }

    // The `yield` form's desugaring, which is what makes everything below this line unaware that a
    // lens is one - Analysis-IR.md §11.4's W-A.
    if(lens && !declareContinuation(*function, inherited, inheritedWith, declaration.source)) {
        return nullptr;
    }

    /*
     * The signature is what binds the variables, so nothing after it may introduce one.
     *
     * A body names types and does not declare them: `let x = 0 :: b` inside a body whose signature
     * never mentions `b` is a typo, and a context still open would turn it into a parameter no
     * caller can supply and no specialization can bind. The transition is here rather than at the
     * start of the body because this is where the signature ends, and a rule enforced at the far
     * end of the pipeline from where it is decided is one that grows exceptions.
     *
     * A class member's context is the class's and was closed when the head was read, so this is a
     * second close of a context that is already shut rather than a rule with an exception in it.
     */
    closeContext(decl(), function->gen);

    /*
     * The one fact that is settled at declaration time, and exactly one.
     *
     * `bodyKnown` is about the *declaration* - whether there is anything to analyse - and it is the
     * only summary field a declaration answers by itself. Every other fact stays `Unknown` until its
     * own pass exists, which is what `Fact` is for.
     */
    function->summary.bodyKnown = written.body != nullptr;

    /*
     * `@builtin(role)` - what the compiler implements this signature as, where it has no body.
     *
     * Here rather than in `declareFunction` because this is the one place a written signature
     * becomes a `Function`: a plain `fn`, a class member and an instance member all arrive through
     * it, and an intrinsic may be any of the three.
     *
     * Only the *binding* happens here. Whether the signature is the shape the emitter depends on is
     * `checkRoles`, after every declaration is in - a shape check run at this point would be reading
     * a signature whose relative terms may name a position not resolved yet.
     */
    if(auto role = bindRole(context, program, parse, declaration, Just(intrinsic::Entity::Function),
                            (function - *program.decl).offset)) {
        if(role.unwrap().kind == intrinsic::RoleKind::Op) {
            function->intrinsic = intrinsic::Op(role.unwrap().index);
            auto form = intrinsic::opDef(function->intrinsic).form;

            /*
             * A body as well as a `Node` role, which is two implementations of one function.
             *
             * Refused rather than resolved with one of them winning silently. The declaration says
             * the compiler supplies this, and a body beside it is either dead code or the author's
             * real intent - and nothing here can tell which.
             *
             * A `Flow` role is the other way round: the compiler lays the operator out where it is
             * written, and the body is what every other reach of the declaration runs - so a
             * declaration without one would be a function value with nothing behind it. `op.def`
             * is where the two forms are argued.
             */
            if(written.body && form == intrinsic::OpForm::Node) {
                context.diagnostics.error(
                    "%@ is implemented by the compiler, so this declaration has no body to write"_v,
                    declaration.source, intrinsic::opDef(function->intrinsic).role);
            }

            if(!written.body && form == intrinsic::OpForm::Flow) {
                context.diagnostics.error(
                    "%@ is expanded by the compiler where it is written as an operator, and carries the body every other use of it runs - this declaration has none"_v,
                    declaration.source, intrinsic::opDef(function->intrinsic).role);
            }
        }
    }

    current->functions.push(program.decl, function - *program.decl);
    return function;
}

void Resolver::declareFunction(ast::Decl& declaration) {
    auto function = declareSignature(declaration, declaration.fun.name);
    if(!function) {
        writtenFunctions.push(nullptr);
        return;
    }

    auto pointer = function - *program.decl;
    writtenFunctions.push(pointer);

    /*
     * One plain function per `(name, arity)` - `doc/spec/overloading.md`, and what
     * `Program::addFunctionName` keys by.
     *
     * The arity is part of the key, so `fn negate(x)` and `fn negate(x, from)` are two declarations
     * rather than a redeclaration; two of one name *and* one arity still are one, because selecting
     * between those would be overloading on types and a call would stop having exactly one callee
     * before its arguments are read.
     *
     * A defaulted position makes that key a range, so the count reported is the one `addFunctionName`
     * found taken and not this declaration's length: `fn f(a)` beside `fn f(a, b = 1)` collide at one
     * argument, which is a number neither signature has written anywhere.
     */
    auto conflict = U32(0);
    if(!program.addFunctionName(here(), pointer, &conflict)) {
        context.diagnostics.error("this module already declares a %@ taking %@ arguments"_v,
                                  declaration.source, context.findName(function->name),
                                  U64(conflict));
    }
}

} // namespace mir
