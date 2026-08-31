#include "resolve_internal.h"

namespace mir {

/*
 * One declaration's attributes, read into the flags the stage has fields for.
 *
 * Two of them, and both are [removals](../../doc/spec/ownership.md#removing-a-capability): what a
 * removal takes away has to be closed and visible at the declaration, which is why they are
 * attributes on a `data` or an `instance` rather than instances of their own. An unrecognized one is
 * reported rather than ignored, because a silently dropped `@explicitDrop` is a linear type that is
 * not linear and nothing downstream would ever say so.
 *
 * A null destination is a *placement* error rather than an unknown name, and the two are reported
 * differently: `@local` on an `instance` and on a `data` mean the same thing and both have a field
 * here, while `@explicitDrop instance Drop(T)` is a statement about a type written on something that
 * is not one. An attribute this stage has no field for at all is neither, and is `unsupported`.
 */
void Resolver::readAttributes(ast::Decl& declaration, bool* explicitDrop, bool* local, StringView on) {
    for(auto attribute: declaration.attributes.contents(parse)) {
        auto which = findAttribute(stringView(context.findName(attribute.name)));
        if(which == DeclAttribute::Count) {
            unsupported(attribute.source, "this attribute"_v);
            continue;
        }

        // The one statement of which attribute lands where, and a switch rather than a conditional
        // so that a new enumerator is a compiler error here instead of an attribute that parses,
        // resolves and is quietly dropped on the floor.
        bool* target = nullptr;
        switch(which) {
            case DeclAttribute::ExplicitDrop: target = explicitDrop; break;
            case DeclAttribute::Local:        target = local; break;
            case DeclAttribute::Count:        break;
        }

        if(!target) {
            context.diagnostics.error("`@%@` has no meaning on %@"_v, attribute.source,
                                      attributeName(which), on);
            continue;
        }

        if(attribute.args.isNotEmpty()) {
            context.diagnostics.error("`@%@` takes no arguments"_v, attribute.source,
                                      attributeName(which));
            continue;
        }

        *target = true;
    }
}

/*
 * Pass one, part one: the record *names*, before any field type is resolved.
 *
 * Two sub-passes rather than one, because a `data` may hold a record declared below it and the field
 * type of the first would then resolve against a name table that does not have the second in it yet.
 * A shell is a name and an identity, which is all a nominal type needs to be referred to; the fields
 * arrive in `declareFields`.
 */
void Resolver::declareTypes(ast::Module& file) {
    for(auto declaration: file.decls.contents(parse)) {
        if(declaration.kind != ast::Decl::Data) continue;

        auto& written = declaration.data;
        auto push = [&](DeclPtr<RecordType> record) { writtenRecords.push(record); };

        if(written.constraints.size() || written.type.kind.size()) {
            unsupported(declaration.source, "a generic data declaration"_v);
            push(nullptr);
            continue;
        }

        /*
         * One constructor, whose name is the type's - `data Pair {x: I32, y: I32}`.
         *
         * Several constructors are an enumeration: the value carries a discriminant, a projection
         * needs a downcast that a pattern proves, and the ownership schema differs per arm. That is
         * a representation with three parts this milestone does not have, and refusing it by name is
         * more useful than a record whose second constructor was silently dropped.
         */
        if(written.cons.size() != 1) {
            unsupported(declaration.source, "a data declaration with several constructors"_v);
            push(nullptr);
            continue;
        }

        auto constructor = written.cons.get(parse, 0);
        if(constructor.name != written.type.name) {
            unsupported(constructor.source, "a constructor whose name differs from its type"_v);
            push(nullptr);
            continue;
        }

        if(!constructor.content || parse[constructor.content]->kind != ast::Type::Tup) {
            unsupported(constructor.source, "a constructor without named fields"_v);
            push(nullptr);
            continue;
        }

        auto record = program.types.recordType(written.type.name, here(), declaration.source);
        auto pointer = DeclPtr<RecordType>(record - *program.decl);

        // Read on the shell rather than in `declareFields`, because `classifyTypes` runs over the
        // records and not over the written declarations - a record whose fields were rejected still
        // has to carry what its declaration said about it.
        readAttributes(declaration, &record->writtenExplicitDrop, &record->local, "a type"_v);

        module.records.push(program.decl, pointer);
        push(pointer);

        if(!program.addRecordName(here(), record->name, pointer)) {
            context.diagnostics.error("%@ is already declared in this module"_v,
                                      declaration.source, context.findName(record->name));
        }
    }
}

/*
 * The fields of one record, once every record name exists.
 *
 * `next` indexes the *written* declarations including the rejected ones, for the reason
 * `writtenRecords` exists: the two walks have to advance in step or a later declaration is given an
 * earlier one's fields.
 */
void Resolver::declareFields(ast::Decl& declaration, Size& next) {
    if(next >= writtenRecords.size()) return;

    auto pointer = writtenRecords[next++];
    if(!pointer) return;

    auto record = decl()[pointer];

    // The shell exists, so `declareTypes` already established that there is exactly one constructor
    // and that its content is a written tuple - this is the same declaration read a second time
    // rather than a second set of conditions to keep in step.
    auto constructor = declaration.data.cons.get(parse, 0);
    auto& written = *parse[constructor.content];

    // The record's own ownership schema scope - §5.11. A `data` declaration is a scope exactly as a
    // signature is, and for the same reason: `data Editor {input: &'String, at: Size}` publishes one
    // origin, and what fills it is decided where an `Editor` is constructed.
    SlotScope slots;

    for(auto field: written.tup.fields.contents(parse)) {
        if(!field.name) {
            unsupported(record->source, "a positional field"_v);
            continue;
        }

        if(field.def) {
            unsupported(record->source, "a field default"_v);
            continue;
        }

        Occurrence occurrence;
        if(!resolveOccurrence(field.type, ast::BindType::Borrow, Position::Field, slots, occurrence)) {
            continue;
        }

        // A second field of one name would make `p.x` name whichever the lookup reached first, which
        // is a silent wrong answer rather than a missing feature.
        auto duplicate = false;
        for(auto existing: record->fields.contents(decl())) {
            if(existing.name != field.name) continue;

            context.diagnostics.error("%@ already has a field called %@"_v, record->source,
                                      context.findName(record->name), context.findName(field.name));
            duplicate = true;
            break;
        }

        if(duplicate) continue;

        RecordField declared;
        declared.name = field.name;
        declared.occurrence = occurrence;
        declared.source = record->source;
        declared.index = U16(record->fields.size());

        record->fields.push(program.decl, declared);
    }

    record->originSlots = slots.count();
    if(record->fields.size()) return;

    // A record with no fields has no layout question and no ownership question, and every value of
    // it is the same value - which is `{}`, spelled by a declaration. Refused rather than admitted
    // as a second unit type.
    unsupported(record->source, "a record with no fields"_v);
}

void Resolver::declareGlobals(ast::Decl& declaration) {
    if(declaration.stmt.kind != ast::Expr::Decl) {
        unsupported(declaration.source, "a module-level statement"_v);
        return;
    }

    for(auto binding: declaration.stmt.decl.contents(parse)) {
        if(binding.pat.kind != ast::Pat::Var) {
            context.diagnostics.error("a global must be declared as a single name"_v, binding.pat.source);
            writtenGlobals.push(nullptr);
            continue;
        }

        if(!binding.content) {
            context.diagnostics.error("a global requires an initializer"_v, binding.pat.source);
            writtenGlobals.push(nullptr);
            continue;
        }

        auto global = new (program.decl) Global(binding.pat.var, owned(nullptr));
        global->source = binding.pat.source;
        global->writable = binding.bind == ast::BindType::Ref;

        auto pointer = global - *program.decl;
        module.globals.push(program.decl, pointer);
        writtenGlobals.push(pointer);

        if(!program.addGlobalName(here(), global->name, pointer)) {
            context.diagnostics.error("%@ is already declared in this module"_v,
                                      binding.pat.source, context.findName(global->name));
        }
    }
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

    /*
     * The sigils on the type, peeled outermost first.
     *
     * A loop rather than one test, so that `&'T` written as two nested nodes and `&a'T` written as
     * one reach the same answer: what the parser produces for a reference is a kind plus an optional
     * group, and the shape a reader wrote is not the shape the tree has.
     */
    auto type = &written;
    auto tick = false;

    // Braced, and it matters: `StringId`'s default constructor is trivial so that a token's union
    // can hold one, which makes `StringId x;` indeterminate rather than zero - see its declaration.
    StringId group {};

    for(;;) {
        if(type->kind == ast::Type::Borrow) {
            access = Access::Unique;
        } else if(type->kind == ast::Type::Shared) {
            if(access == Access::Owned && !sunk) access = Access::Shared;
        } else {
            break;
        }

        if(type->ref.group) {
            if(tick && group != type->ref.group) {
                context.diagnostics.error("this names two origins, %@ and %@"_v, type->source,
                                          context.findName(group), context.findName(type->ref.group));
                return false;
            }

            group = type->ref.group;
        }

        tick = tick || type->kind == ast::Type::Shared || type->ref.group;
        type = parse[type->ref.to];
    }

    if(sunk && tick) {
        context.diagnostics.error(
            "`->` hands the value over and `'` says somebody else keeps it, so this asks for both"_v,
            written.source);
        return false;
    }

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

        contract = slotContract(access, slot);
    } else if(access == Access::Owned) {
        contract = ownedContract();
    } else if(binds) {
        contract = borrowedContract(access);
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

    if(written.kind != ast::Type::App) {
        logical = resolveType(written);
        return logical != nullptr;
    }

    auto& application = *parse[written.app];
    logical = resolveType(application.base);
    if(!logical) return false;

    if(decl()[logical]->kind != Type::Record) {
        context.diagnostics.error("%@ has no origin slots for an argument to fill"_v, written.source,
                                  typeText(context, decl(), logical));
        return false;
    }

    auto& record = *(RecordType*)decl()[logical];

    // One slot per argument, in the order the record introduced them. A type argument here is a
    // generic application, which is a different feature and is reported as one.
    SmallArray<U16, 4> binding;

    for(auto argument: application.args.contents(parse)) {
        if(argument.kind != ast::Type::Loan) {
            unsupported(argument.source, "a type argument"_v);
            return false;
        }

        auto slot = slots.find(argument.name,
                               position == Position::Parameter || position == Position::Field);
        if(slot == SlotScope::kNone) {
            context.diagnostics.error("no parameter publishes the origin %@"_v, argument.source,
                                      context.findName(argument.name));
            return false;
        }

        binding.push(slot);
    }

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
 * The signature's parameters, in order.
 *
 * Answers false when the declaration is one this stage cannot represent, in which case the function
 * is dropped whole rather than left with a signature a call would be checked against. A partially
 * declared signature is worse than none: every call to it reports against positions the author did
 * not write.
 */
bool Resolver::declareArgs(Function& function, ast::Decl& declaration, SlotScope& slots) {
    auto& written = declaration.fun;
    auto ok = true;

    for(auto argument: written.args.contents(parse)) {
        if(argument.def) {
            unsupported(argument.source, "a parameter default"_v);
            ok = false;
            continue;
        }

        if(argument.lazy) {
            unsupported(argument.source, "a `@lazy` parameter"_v);
            ok = false;
            continue;
        }

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

        if(!argument.type) {
            unsupported(argument.source, "an inferred parameter type"_v);
            ok = false;
            continue;
        }

        Occurrence occurrence;
        if(!resolveOccurrence(*parse[argument.type], argument.bind, Position::Parameter, slots,
                              occurrence)) {
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

        auto declared = new (program.decl) Arg();
        declared->name = argument.name;
        declared->occurrence = occurrence;
        declared->source = argument.source;
        declared->index = U32(function.args.size());

        function.args.push(program.decl, declared - *program.decl);
    }

    return ok;
}

/*
 * The half of a `fn` declaration that is the same whether it stands in a module or in an instance.
 *
 * Answers null where the declaration is one this stage cannot represent. What it deliberately does
 * *not* do is register the name: a module function is reachable by name and an instance member is
 * reached by the compiler through its instance, and which of the two this is belongs to the caller.
 */
Function* Resolver::declareSignature(ast::Decl& declaration, StringId name) {
    auto& written = declaration.fun;

    /*
     * `lens fn` in the `yield` form, and nothing else yet.
     *
     * An `iter fn` is the same construct with `Many` multiplicity and two more exits - `break` and
     * `continue` - and it is the next waypoint rather than a different mechanism. The explicit
     * callback form (`body: (P) -> r`) needs `r` free in the signature and is milestone 6's, which
     * is where §12.4 puts it; a function-typed parameter is refused by `declareArgs` in any case, so
     * what reaches here is the sugar.
     */
    auto lens = written.kind == ast::FunKind::Lens;

    if(written.kind == ast::FunKind::Iter) {
        unsupported(declaration.source, "iterator functions"_v);
        return nullptr;
    }

    if(lens && written.implicitReturn) {
        context.diagnostics.error(
            "a lens hands over with `yield`, so its body is a block rather than one expression"_v,
            declaration.source);
        return nullptr;
    }

    if(written.constraints.size()) {
        unsupported(declaration.source, "generic functions"_v);
        return nullptr;
    }

    auto function = new (program.decl) Function(name, here());
    function->source = declaration.source;
    function->lens = lens;

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
    if(!declareArgs(*function, declaration, slots)) return nullptr;

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

    if(written.ret) {
        if(!resolveOccurrence(*parse[written.ret], written.retBind, position, slots,
                              function->result)) {
            return nullptr;
        }
    } else {
        function->result = owned(program.types.unit);
    }

    function->originSlots = slots.count();

    /*
     * The one fact that is settled at declaration time, and exactly one.
     *
     * `bodyKnown` is about the *declaration* - whether there is anything to analyse - and it is the
     * only summary field a declaration answers by itself. Every other fact stays `Unknown` until its
     * own pass exists, which is what `Fact` is for.
     */
    function->summary.bodyKnown = written.body != nullptr;

    // A lens's body is not resolved here and not resolved once: it is resolved into each call site.
    // Recorded now because pass one sees every declaration before any body is walked, so a lens
    // called above where it is written is found the same as one called below it.
    if(lens && written.body) {
        lensBodies.push(LensBody { function - *program.decl, parse[written.body] });
    }

    module.functions.push(program.decl, function - *program.decl);
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
     * One declaration per name, which is this milestone's stand-in for an overload set.
     *
     * doc/spec/overloading.md keys a set by `(name, arity)` and admits one plain function per key
     * beside any number of class functions, and selecting among those is milestone 6. Until then a
     * second declaration would be one the name table silently kept the first of - a function nothing
     * could call and no message about it.
     */
    if(!program.addFunctionName(here(), function->name, pointer)) {
        unsupported(declaration.source, "two functions of one name"_v);
    }
}

/*
 * `instance Drop(T)`, `instance Reclaim(T)` and `instance Copy(T)` - the milestone-4 slice of
 * instance selection.
 *
 * Three classes, all builtin (see `CoreClass`), all with one member, and all heads concrete. What
 * that leaves is a lookup rather than a solve, which is deliberate: the *shape* here is already
 * §11.4's sixth milestone - selection over logical types with no ownership contract involved - and
 * what that milestone adds is a parametric head and a context to discharge, above this table rather
 * than beside it.
 *
 * The member is an ordinary function with an ordinary body and is deliberately **not** registered by
 * name. Nothing calls it from source: a teardown runs because an object died, and the call is the
 * one the drop elaboration emits. Registering it would also make `instance Drop(A)` and
 * `instance Drop(B)` two declarations of `drop` in one module. `Copy`'s member is unregistered for
 * the weaker reason that nothing calls it *yet* - `copy(x)` is a class call this stage cannot
 * resolve - and it is declared anyway so that the signature is checked where it is written rather
 * than at the milestone that finally dispatches to it.
 */
void Resolver::declareInstance(ast::Decl& declaration) {
    auto& written = declaration.instance;
    writtenInstances.push(nullptr);

    /*
     * `@local instance Drop(T)` - `doc/spec/ownership.md`'s "who may dispose".
     *
     * Read before anything is checked, so that the attribute on an instance this stage refuses for
     * another reason is still reported for its own. Coherence is untouched by it: the instance stays
     * unique and stays selected program-wide, and what is restricted is permission to *trigger* it -
     * which is why this works where hiding the instance would not. Hiding one would make selection
     * depend on who is looking.
     */
    auto local = false;
    readAttributes(declaration, nullptr, &local, "an instance"_v);

    if(written.constraints.size()) {
        unsupported(declaration.source, "an instance with a context"_v);
        return;
    }

    if(written.type.kind != ast::Type::App) {
        unsupported(declaration.source, "this instance head"_v);
        return;
    }

    auto& head = *parse[written.type.app];
    if(head.base.kind != ast::Type::Con || head.args.size() != 1) {
        unsupported(declaration.source, "this instance head"_v);
        return;
    }

    auto className = stringView(context.findName(head.base.name));
    auto klass = CoreClass::Count;
    if(className == "Drop"_v) klass = CoreClass::Drop;
    else if(className == "Reclaim"_v) klass = CoreClass::Reclaim;
    else if(className == "Copy"_v) klass = CoreClass::Copy;

    if(klass == CoreClass::Count) {
        unsupported(declaration.source, "a class other than Drop, Reclaim and Copy"_v);
        return;
    }

    auto argument = head.args.get(parse, 0);
    auto subject = resolveType(argument);
    if(!subject) return;

    /*
     * A nominal head, and for `Copy` that is a soundness rule rather than a milestone restriction.
     *
     * All three of these instances *subtract* `TrivialCopy` from their subject, so the head decides
     * whose capability is being taken away. The orphan rule places `instance Copy(MyType)` in
     * `MyType`'s own module, which is where `doc/spec/ownership.md`'s "removing a capability" asks a
     * removal to be visible; a structural head - `Copy(Maybe(MyType))`, `Copy([MyType])` - is
     * orphan-legal by what is *inside* it and would take the capability from a type somebody else
     * declared and already compiled generic bodies against. Refused rather than honoured.
     */
    if(decl()[subject]->kind != Type::Record) {
        context.diagnostics.error("%@ may only be given to a type this module declares"_v,
                                  declaration.source, coreClassName(klass));
        return;
    }

    if(written.decls.size() != 1) {
        context.diagnostics.error("an instance of %@ declares exactly one function"_v,
                                  declaration.source, coreClassName(klass));
        return;
    }

    auto member = written.decls.get(parse, 0);
    if(member.kind != ast::Decl::Fun) {
        unsupported(declaration.source, "a declaration other than a function in an instance"_v);
        return;
    }

    /*
     * A plain function, and this is the one place that has to say so.
     *
     * A member of one of these classes is called by the *compiler* - a teardown runs because an
     * object died - so there is no call site to write a continuation at and nothing for a `yield` to
     * hand over to. Refused here rather than left to the flattening, which would never reach it: a
     * lens has no standalone body, so an accepted one would be a teardown with nothing to call.
     */
    if(member.fun.kind != ast::FunKind::Plain) {
        context.diagnostics.error("%@ is called when an object dies, so its member is a plain function"_v,
                                  member.source, coreClassName(klass));
        return;
    }

    /*
     * The member's own name, which is the class's spelled in lower case in Core and is checked here
     * rather than accepted: `instance Drop(T): fn release(...)` declares nothing the compiler will
     * ever call, and accepting it would be a teardown that silently never runs.
     */
    auto expected = klass == CoreClass::Drop    ? "drop"_v
                  : klass == CoreClass::Reclaim ? "reclaim"_v
                                                : "copy"_v;
    if(stringView(context.findName(member.fun.name)) != expected) {
        context.diagnostics.error("an instance of %@ declares %@"_v, member.source,
                                  coreClassName(klass), expected);
        return;
    }

    /*
     * A distinct name for the dump and for the backend's symbol table.
     *
     * The member is not reachable by this name from source - nothing looks one up - so what it has
     * to be is unique and readable. The bytes are copied into the declaration arena first, because
     * **an interned name borrows its text**: handing `addUnqualifiedName` a formatted temporary
     * leaves the name table pointing into a buffer that is freed when this function returns, and
     * what prints afterwards is whatever the allocator put there next.
     */
    auto text = Tritium::format("%@.%@", context.findName(((RecordType*)decl()[subject])->name),
                                expected);

    auto stored = (char*)program.decl.alloc(text.size());
    copy(text.text(), stored, text.size());

    auto name = context.addUnqualifiedName(stored, text.size());

    auto function = declareSignature(member, name);
    if(!function) return;

    /*
     * The signature the class declares, checked against the one that was written.
     *
     * `fn drop(->value: a) -> {}` with `a` at the instance's own type. Checked rather than assumed
     * because everything downstream calls it with exactly that: the drop elaboration hands over the
     * dying object under `Take`, and a member declared to borrow it would receive a loan of storage
     * that is about to stop existing.
     *
     * `fn copy(from: a) -> a` is the other shape, and each half of it is load-bearing in the
     * opposite direction. The subject is **shared**: a `copy` that consumed its argument would be a
     * relocation with a call in front of it, and the caller still holds the original at every site
     * that duplicates one. The result is **owned** and of the same type: producing "a valid
     * independent value" is the contract the class carries, and a borrowed result would hand back a
     * second name for the storage that was to be duplicated.
     */
    auto ok = function->args.size() == 1;

    if(ok) {
        auto& argument = *decl()[function->args.get(decl(), 0)];
        ok = sameType(argument.occurrence.type, subject);

        if(klass == CoreClass::Copy) {
            ok = ok && argument.occurrence.contract.access() == Access::Shared &&
                 sameType(function->result.type, subject) && function->result.contract.isOwned();
        } else {
            ok = ok && argument.occurrence.contract.isOwned() &&
                 decl()[function->result.type]->kind == Type::Unit;
        }
    }

    if(!ok) {
        if(klass == CoreClass::Copy) {
            context.diagnostics.error("copy for %@ must be written `fn copy(from: %@) -> %@`"_v,
                                      member.source, typeText(context, decl(), subject),
                                      typeText(context, decl(), subject),
                                      typeText(context, decl(), subject));
        } else {
            context.diagnostics.error("%@ for %@ must be written `fn %@(->value: %@) -> {}`"_v,
                                      member.source, expected,
                                      typeText(context, decl(), subject), expected,
                                      typeText(context, decl(), subject));
        }

        return;
    }

    // Not a teardown: `copy` borrows its subject and returns a second value, so the drop elaboration
    // has no reason to reach it and the flag that tells it not to place one for the subject would be
    // wrong here - the caller's original outlives the call.
    function->teardown = klass != CoreClass::Copy;

    if(local && klass == CoreClass::Copy) {
        // `@local` restricts who may *trigger* the member, and a `copy` is triggered by a call the
        // caller writes rather than by a value dying. Restricting it would be an ordinary export
        // question about a function, which is the module system's and not this attribute's.
        context.diagnostics.error("`@%@` restricts who may run a teardown, and %@ is not one"_v,
                                  declaration.source, attributeName(DeclAttribute::Local),
                                  coreClassName(CoreClass::Copy));
        return;
    }

    auto instance = new (program.decl) Instance();
    instance->klass = klass;
    instance->type = subject;
    instance->function = function - *program.decl;
    instance->source = declaration.source;
    instance->local = local;
    instance->module = here();

    auto pointer = instance - *program.decl;
    module.instances.push(program.decl, pointer);
    writtenInstances[writtenInstances.size() - 1] = pointer;

    if(!program.addInstance(pointer)) {
        context.diagnostics.error("%@ already has an instance of %@"_v, declaration.source,
                                  typeText(context, decl(), subject), coreClassName(klass));
    }
}

/*
 * Every record's fields, and nothing else.
 *
 * A pass of its own, ahead of `declare`, because `foldSchemas` runs between the two: a signature may
 * write `x: Reader(a')`, which binds `Reader`'s interior positions, and under one interleaved walk a
 * function declared above a `data` in the same file was resolved before that record had fields at
 * all. Splitting is what makes the precondition a phase order rather than a source order.
 */
void Resolver::declareRecordFields(ast::Module& file) {
    Size record = 0;

    for(auto declaration: file.decls.contents(parse)) {
        if(declaration.kind == ast::Decl::Data) declareFields(declaration, record);
    }
}

void Resolver::declare(ast::Module& file) {
    for(auto declaration: file.decls.contents(parse)) {
        switch(declaration.kind) {
            case ast::Decl::Stmt:
                declareGlobals(declaration);
                break;
            case ast::Decl::Fun:
                declareFunction(declaration);
                break;
            case ast::Decl::Data:
                break;
            case ast::Decl::Instance:
                declareInstance(declaration);
                break;
            case ast::Decl::Error:
                break;
            default:
                unsupported(declaration.source, "this declaration"_v);
                break;
        }
    }
}

} // namespace mir
