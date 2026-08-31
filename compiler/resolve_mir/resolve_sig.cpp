#include "resolve_internal.h"
#include "print.h"

namespace mir {

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

    // The one gate on what a *value* may be of, and it is here because this is the one function
    // every parameter, result and field goes through. A count has no manifestation, so the position
    // that would have asked for one is the position that reports.
    if(!requireValueType(logical, written.source)) return false;

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

    return resolveApplication(written, position, &slots, logical, shape);
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
    if(auto alias = program.findAlias(here(), application.base.name)) {
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

    auto found = program.findRecord(here(), application.base.name);
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

        Occurrence occurrence;

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

            occurrence = substituteOccurrence(
                context, program,
                decl()[inherited->args.get(decl(), U32(function.args.size()))]->occurrence,
                inheritedWith);
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
Function* Resolver::declareSignature(ast::Decl& declaration, StringId name,
                                     DeclPtr<GenContext> classContext, Function* inherited,
                                     Substitution inheritedWith) {
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

    auto function = new (program.decl) Function(name, here());
    function->source = declaration.source;
    function->lens = lens;

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

    if(written.ret) {
        if(!resolveOccurrence(*parse[written.ret], written.retBind, position, slots,
                              function->result)) {
            return nullptr;
        }
    } else if(inherited) {
        // Inherited for the reason a parameter is - see `declareArgs`. An implementation that wrote
        // no result type is one the class already gave a result to, and `{}` here would be a
        // signature the member check then rejects for a mistake the author did not make.
        function->result = substituteOccurrence(context, program, inherited->result, inheritedWith);
    } else {
        function->result = owned(program.types.unit);
    }

    function->originSlots = slots.count();

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
     * One plain function per `(name, arity)` - `doc/spec/overloading.md`, and what
     * `Program::addFunctionName` keys by.
     *
     * The arity is part of the key, so `fn negate(x)` and `fn negate(x, from)` are two declarations
     * rather than a redeclaration; two of one name *and* one arity still are one, because selecting
     * between those would be overloading on types and a call would stop having exactly one callee
     * before its arguments are read.
     */
    if(!program.addFunctionName(here(), function->name, U32(function->args.size()), pointer)) {
        context.diagnostics.error("this module already declares a %@ taking %@ arguments"_v,
                                  declaration.source, context.findName(function->name),
                                  U64(function->args.size()));
    }
}

} // namespace mir
