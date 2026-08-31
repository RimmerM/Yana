#include "resolve_internal.h"
#include "print.h"

namespace mir {

/*
 * Whether an instance head names a type *this module declares* - the orphan rule, at the placement
 * where it is a soundness rule rather than a convention.
 *
 * `Copy`, `Drop` and `Reclaim` each **subtract** a capability from what their head names, and
 * `doc/spec/ownership.md` places a removal in the module that declares what is being removed from:
 * a body compiled against the structural answer for somebody else's type may not have that answer
 * changed underneath it by a third module. So the head has to reach a record declared here -
 * `Copy(Array(a))` does through `Array`, `Copy(I32)` does not through anything, and an **open** head
 * reaches nothing at all and would take the capability from every type there is.
 *
 * A walk over the head rather than a test of its outermost constructor, because the rule is about
 * *reaching* a local declaration and an argument is as good as a head: `instance Drop(Pair(Guard))`
 * is a statement `Guard`'s module is entitled to make. For an ordinary class this is not asked at
 * all - nothing is being taken away, so what governs is coherence, which the instance table keeps.
 */
static bool namesLocalType(DeclBase base, DeclPtr<Module> module, DeclPtr<Type> type) {
    if(!type) return false;

    auto& written = *base[type];
    if(written.kind == Type::Pointer) {
        return namesLocalType(base, module, ((PointerType&)written).to);
    }

    if(written.kind != Type::Record) return false;

    auto& record = (RecordType&)written;
    if(record.module == module) return true;

    for(auto argument: record.arguments.contents(base)) {
        if(namesLocalType(base, module, argument)) return true;
    }

    return false;
}

bool Resolver::headNamesLocalType(const SmallArray<DeclPtr<Type>, 4>& arguments) {
    for(auto argument: arguments) {
        if(namesLocalType(decl(), here(), argument)) return true;
    }

    return false;
}

/*
 * Whether one written constraint is a class - the only kind every position reading one accepts.
 *
 * Reports where it is not, and the message is the **form** wherever the form is one of the three
 * that wait on a witness: `a.field: b` needs a property witness, `f: (a) -> b` a function witness
 * and `n: Int` a const slot. Which position it was written in makes no difference to any of the
 * three, so naming the position instead would be four messages about one missing piece.
 *
 * `elsewhere` is what a form outside that family is reported as, and that one *does* depend on the
 * position: an `Any` constraint introduces a variable where a signature reads it and means nothing
 * on a head, which is a placement mistake rather than something unbuilt.
 */
bool Resolver::requireClassConstraint(ast::Constraint constraint, StringView elsewhere) {
    if(constraint.kind == ast::Constraint::Class) return true;

    switch(constraint.kind) {
        case ast::Constraint::Field:
            unsupported(constraint.source, "a structural field requirement"_v);
            break;
        case ast::Constraint::Function:
            unsupported(constraint.source, "a constrained callable"_v);
            break;
        case ast::Constraint::Const:
            unsupported(constraint.source, "a const parameter"_v);
            break;
        default:
            unsupported(constraint.source, elsewhere);
            break;
    }

    return false;
}

/*
 * `Show(a)`, `Contiguous(c, a)` - one written class constraint, read into a requirement.
 *
 * The one reader, with three callers that state the same thing in three places: a signature's own
 * context, a class head's superclass list, and a parametric instance head's context. They differ in
 * what the arguments are *written in terms of* - a signature's own variables, a class's, an
 * instance's - which is the `gen` parameter and the whole of the difference. Three copies of this
 * loop is how a superclass comes to accept an arity a signature refuses.
 *
 * The arguments are resolved in `gen`'s scope and may therefore **introduce** a variable, which is
 * the order the numbering is in: `fn (Show(a)) present(x: a)` says `a` for the first time here.
 *
 * `allowConcrete` is false for a signature - a requirement naming no variable is a fact about the
 * program with nowhere for a caller to prove it - and true for an instance head, where
 * `instance (Eq(I32)) Eq(Pair)` is merely redundant rather than unprovable.
 */
bool Resolver::readClassRequirement(ast::Constraint constraint, DeclPtr<GenContext> gen,
                                    bool allowConcrete, GenRequirement& into) {
    auto klass = lookupClass(constraint.klass.name, constraint.source);
    if(!klass) return false;

    into.klass = klass;
    into.source = constraint.source;

    SmallArray<DeclPtr<Type>, 4> arguments;

    {
        GenScope generics(*this, gen);

        for(auto argument: constraint.klass.args.contents(parse)) {
            auto resolved = resolveTypeArgument(argument, decl()[klass]->gen, arguments.size());
            if(!resolved) return false;

            arguments.push(resolved);
        }
    }

    if(!checkArgumentKinds(context, decl(), decl()[klass]->gen, decl()[klass]->name,
                           arguments.pointer(), arguments.size(), constraint.source)) {
        return false;
    }

    // The class head's own defaults, on `instantiateRecord`'s terms: a written list shorter than the
    // arity is one the head finishes, and only a position with nothing to finish it is an arity
    // mistake.
    if(!fillDefaults(context, program, decl()[klass]->gen, arguments)) {
        context.diagnostics.error("%@ takes %@ arguments and this names %@"_v, constraint.source,
                                  context.findName(decl()[klass]->name),
                                  U64(decl()[klass]->arity(decl())),
                                  U64(constraint.klass.args.size()));
        return false;
    }

    auto concrete = true;
    for(auto argument: arguments) {
        concrete = concrete && isConcreteType(decl(), argument);
        into.args.push(program.decl, argument);
    }

    if(concrete && !allowConcrete) {
        // A requirement on concrete types is a fact about the program rather than about this
        // declaration, and there is nowhere for a caller to prove it: either the instance exists and
        // the constraint says nothing, or it does not and every call is wrong.
        context.diagnostics.error("a constraint names types this signature binds"_v,
                                  constraint.source);
        return false;
    }

    auto held = requirementArgs(decl(), into);
    if(satisfiesRequirement(context, program, gen, klass, held.data, held.count)) {
        context.diagnostics.error("this declaration already requires %@"_v, constraint.source,
                                  appliedClassText(context, decl(), klass, held.data, held.count));
        return false;
    }

    return true;
}

/*
 * `class Describe(a)`, `class Contiguous(c -> a)` - the head, and nothing under it.
 *
 * A shell pass for `declareTypes`' reason: a constraint in a signature names a class and an
 * `instance` head names one, so every class has to exist before either is read. What is read here is
 * the name, the parameters and the arrow; the members wait for `declareClassMembers`, because a
 * member's signature is an ordinary signature and may mention any type or class in the program.
 */
void Resolver::declareClass(ast::Decl& declaration) {
    auto& written = declaration.trait;
    writtenClasses.push(nullptr);

    if(!written.type.kind.size()) {
        context.diagnostics.error("a class declares at least one parameter"_v, declaration.source);
        return;
    }

    auto klass = new (program.decl) Class(written.type.name, here(), declaration.source);
    auto pointer = klass - *program.decl;

    auto environment = new (program.decl) GenContext(true);
    klass->gen = environment - *program.decl;

    for(auto parameter: written.type.kind.contents(parse)) {
        // `class Chunked(c, a, n: Int -> m)` - a const parameter of the head. Bound like a type one
        // and distinguished by its annotation, which is the whole of the difference: the arity, the
        // defaults, the dependency arrow and the instance key are one rule for both kinds.
        DeclPtr<Type> annotation;
        if(parameter.type) {
            annotation = resolveConstParameterType(*parse[parameter.type]);
            if(!annotation) return;
        }

        if(findGenVariable(decl(), klass->gen, parameter.name)) {
            context.diagnostics.error("%@ is already a parameter of this class"_v, parameter.source,
                                      context.findName(parameter.name));
            return;
        }

        genVariable(program, klass->gen, parameter.name, parameter.source, annotation);
    }

    closeContext(decl(), klass->gen);
    declareDefaults(written.type.kind, klass->gen);

    /*
     * `class Contiguous(c -> a)` - which parameters the others decide.
     *
     * The arrow's index as written, and the arity where nothing was written: `isDetermined` reads
     * "at or after this position", so a class with no dependency has every parameter deciding, which
     * is what keying the instance table on all of them means. Zero cannot be written - something has
     * to determine the first parameter - which is why the absent arrow needs no sentinel of its own.
     */
    auto arity = U16(decl()[klass->gen]->variables.size());
    klass->determined = written.type.determined ? written.type.determined : arity;

    if(klass->determined > arity) {
        context.diagnostics.error("this class determines more parameters than it declares"_v,
                                  declaration.source);
        return;
    }

    /*
     * The class requires *itself* of its own parameters, and that is not a tautology.
     *
     * A member's signature is generic in this context, so a body written inside the class - a
     * default - is a generic body whose environment has to say what it may dispatch on. Writing it
     * here means `satisfiesRequirement` answers for a default exactly as it answers for
     * `fn (Describe(a)) f(x: a)`, and the discharge rule is one rule rather than one rule with a
     * clause about class members in it.
     */
    GenRequirement own;
    own.klass = pointer;
    own.source = declaration.source;
    for(auto variable: decl()[klass->gen]->variables.contents(decl())) {
        own.args.push(program.decl, variable);
    }

    decl()[klass->gen]->classes.push(program.decl, own);

    if(!program.addClassName(here(), klass->name, pointer)) {
        context.diagnostics.error("%@ is already declared in this module"_v, declaration.source,
                                  context.findName(klass->name));
        return;
    }

    /*
     * One of the three the compiler itself reaches for, recognized by name.
     *
     * By name and not by anything the author wrote, because that is what the recognition *is*: this
     * stage does not build Core, so `class Drop(a)` written in a fixture and the one synthesized
     * below have to be the same class to the ownership classifier.
     *
     * **After** the name table has accepted it, which is the order and not an accident: a second
     * `class Copy(a)` in one module is refused there, and recognizing it first would leave
     * `program.core` naming a class that no `instance` can be attached to and no member was declared
     * for. Coherence for classes lives in that table, so nothing may record a class before it.
     */
    auto name = stringView(context.findName(klass->name));
    for(Size i = 0; i < Size(CoreClass::Count); i++) {
        if(name != coreClassName(CoreClass(i))) continue;

        // And with one parameter, which is the shape the classifier asks with. A three-parameter
        // `class Drop(a, b, c)` is a different class that happens to share a name, and treating it
        // as the compiler's own would have the ownership walk reading its head at an arity nobody
        // wrote.
        if(arity != 1) break;

        klass->core = CoreClass(i);
        program.core[i] = pointer;
        break;
    }

    program.classes.push(pointer);
    writtenClasses[writtenClasses.size() - 1] = pointer;
}

/*
 * The three classes the compiler reaches for, where the program declared none.
 *
 * Synthesized rather than special-cased, which is the whole point: `instance Drop(T)` is then
 * checked against a class declaration by the one rule every instance is checked by, and the
 * hand-written shape test per enumerator that used to do it is gone. Core declares these three, so
 * this is what stands in until this stage reads Core - at which point the loop above finds them and
 * this one makes nothing.
 *
 *   class Drop(a): fn drop(->value: a)
 *   class Reclaim(a): fn reclaim(->value: a)
 *   class Copy(a): fn copy(from: a) -> a
 *
 * Each half of `copy` is load-bearing in the opposite direction. The subject is **shared**: a copy
 * that consumed its argument would be a relocation with a call in front of it, and the caller still
 * holds the original at every site that duplicates one. The result is **owned** and of the same
 * type, because producing a valid independent value is what the class carries.
 */
void Resolver::synthesizeCoreClasses() {
    for(Size i = 0; i < Size(CoreClass::Count); i++) {
        if(program.core[i]) continue;

        auto which = CoreClass(i);
        auto name = coreClassName(which);
        // No module, and a null source. It belongs to no file - which is what `lookupClass` reads
        // to make the three visible from every module, and what the dump reads to leave them out of
        // a listing of what a file declared.
        auto klass = new (program.decl) Class(context.addUnqualifiedName(name.ptr, name.size()),
                                              nullptr, kNullLocation);
        auto pointer = klass - *program.decl;

        klass->core = which;

        auto environment = new (program.decl) GenContext(true);
        klass->gen = environment - *program.decl;

        auto variable = genVariable(program, klass->gen, context.addUnqualifiedName("a", 1),
                                    kNullLocation);
        closeContext(decl(), klass->gen);
        klass->determined = 1;

        auto memberName = which == CoreClass::Drop    ? "drop"_v
                        : which == CoreClass::Reclaim ? "reclaim"_v
                                                      : "copy"_v;

        // The member belongs to a module though its class does not, and the two are different
        // questions: a class is looked up by name, which is what belonging to none makes universal,
        // while a *function* is where its specializations are registered. None of these three has a
        // default, so there is nothing to specialize today - and a null here would be a crash
        // waiting for the first one rather than a fact about where the code goes.
        auto member = new (program.decl) Function(
            context.addUnqualifiedName(memberName.ptr, memberName.size()), here());

        member->gen = klass->gen;
        member->klass = pointer;
        member->classMember = 0;
        member->teardown = which != CoreClass::Copy;

        auto argument = new (program.decl) Arg();
        argument->name = context.addUnqualifiedName(which == CoreClass::Copy ? "from" : "value",
                                                    which == CoreClass::Copy ? 4 : 5);
        argument->occurrence = which == CoreClass::Copy ? borrowed(variable, Access::Shared)
                                                        : owned(variable);

        member->args.push(program.decl, argument - *program.decl);
        member->result = which == CoreClass::Copy ? owned(variable) : owned(program.types.unit);
        member->summary.bodyKnown = false;

        klass->members.push(program.decl, member - *program.decl);

        program.core[i] = pointer;
        program.classes.push(pointer);
    }
}

/*
 * A member of one of the three the compiler reaches for, by name - `copy`, and only `copy`.
 *
 * The same standing-in-for-Core that `lookupClass` does, from the other side: a synthesized class is
 * in no module's namespace, so its members are in no module's either, and `copy(x)` is a call source
 * writes. Core declares `class Copy(a)` and an ordinary import brings its member in; until this
 * stage reads Core, being visible everywhere is what stands in.
 *
 * A **teardown's** member is deliberately not here. `Drop` and `Reclaim` run because a value died,
 * so there is no call to write and finding `drop` by name would let source trigger a disposal the
 * ownership analysis is answerable for placing.
 */
DeclPtr<Function> Resolver::lookupCoreMember(StringId name, U32 arity) {
    for(Size i = 0; i < Size(CoreClass::Count); i++) {
        auto core = program.core[i];
        if(!core) continue;

        for(auto memberPointer: decl()[core]->members.contents(decl())) {
            auto& member = *decl()[memberPointer];
            if(member.teardown || member.name != name) continue;
            if(member.args.size() != arity) continue;

            return memberPointer;
        }
    }

    return nullptr;
}

DeclPtr<Class> Resolver::lookupClass(StringId name, LocationId source) {
    if(auto found = program.findClass(here(), name)) return found;

    /*
     * The three the compiler reaches for, which are visible from every module.
     *
     * A synthesized class is in no module's namespace, so the lookup above cannot find it - and it
     * has to be found, because `instance Drop(T)` is written in the module that declares `T` and
     * there may be several. Core declares these three and an ordinary import brings them in; until
     * this stage reads Core, being visible everywhere is what stands in for that.
     */
    for(Size i = 0; i < Size(CoreClass::Count); i++) {
        auto core = program.core[i];
        if(core && decl()[core]->name == name) return core;
    }

    context.diagnostics.error("%@ is not a class this module can see"_v, source,
                              context.findName(name));
    return nullptr;
}

/*
 * Which member of a class a name and an arity name - the same key a call uses.
 *
 * A class may declare `shift(x)` and `shift(x, by)`, so a name alone names two members: the key is
 * the pair, exactly as `Program::addFunctionName` states it for a module's own functions. Answers
 * the member count where there is none, and sets `named` where *some* member carries the name - the
 * difference between "no such member" and "no such arity", which are two mistakes and two messages.
 */
Size Resolver::classMemberIndex(Class& klass, StringId name, U32 arity, bool* named) {
    for(Size i = 0; i < klass.members.size(); i++) {
        auto& candidate = *decl()[klass.members.get(decl(), U32(i))];
        if(candidate.name != name) continue;

        if(named) *named = true;
        if(candidate.args.size() != arity) continue;

        return i;
    }

    return klass.members.size();
}

/*
 * `fn size(value: a) -> Int` inside a class - what an instance must supply.
 *
 * An ordinary signature, read in the *class's* generic context, which is the one decision here and
 * the one that makes the rest of the milestone small: a member is then a generic function, a call to
 * one binds the class's argument exactly as a generic call binds a callee's, and `callTarget` turns
 * the pair into a callee by selecting an instance where it would otherwise specialize.
 *
 * A body is the class **default** and is resolved with every other body, because it is one.
 */
void Resolver::declareClassMembers(ast::Decl& declaration, Size& next) {
    auto index = next++;
    if(index >= writtenClasses.size()) return;

    auto pointer = writtenClasses[index];
    if(!pointer) return;

    auto klass = decl()[pointer];
    auto& written = declaration.trait;

    // `class (Eq(a)) Ord(a)` - the superclasses, which is what the head's context list is here.
    for(auto constraint: written.constraints.contents(parse)) {
        if(!requireClassConstraint(constraint, "a class head constraint other than a superclass"_v)) {
            continue;
        }

        GenRequirement inherited;
        if(!readClassRequirement(constraint, klass->gen, false, inherited)) continue;

        if(inherited.klass == pointer) {
            context.diagnostics.error("%@ cannot be its own superclass"_v, constraint.source,
                                      context.findName(klass->name));
            continue;
        }

        /*
         * Written in the class's own parameters and nowhere else.
         *
         * `class (Eq(b)) Keyed(a, b)` is admitted and constrains the second parameter, which is what
         * a requirement rather than a bare class handle buys. What is not admitted is a superclass
         * mentioning a type this head does not bind: an instance would have no way to supply it and
         * `classProves` would substitute into a variable of somebody else's context.
         */
        auto foreign = false;
        for(auto argument: inherited.args.contents(decl())) {
            if(isConcreteType(decl(), argument)) continue;
            if(decl()[argument]->kind != Type::Variable) { foreign = true; break; }
            if(((VariableType*)decl()[argument])->owner != klass->gen) foreign = true;
        }

        if(foreign) {
            context.diagnostics.error("a superclass is written in the class's own parameters"_v,
                                      constraint.source);
            continue;
        }

        klass->superclasses.push(program.decl, inherited);

        // And as a requirement of the context, so that a default may call the superclass's members
        // on the same subject. It is the same fact from the two sides it is read from: an instance
        // has to supply it, and a body inside the class may rely on it.
        decl()[klass->gen]->classes.push(program.decl, inherited);
    }

    for(auto member: written.decls.contents(parse)) {
        if(member.kind != ast::Decl::Fun) {
            unsupported(member.source, "a declaration other than a function in a class"_v);
            continue;
        }

        /*
         * A `lens fn` member, which is the same refusal a lens call already makes rather than a
         * second one.
         *
         * A class member is a generic function over the class's context - that is the decision the
         * whole of this milestone rests on - and `resolveLensCall` refuses a lens and a generic in
         * one call for a reason written out there: flattening resolves the callee's body inside the
         * caller's walk, and there is nothing that swaps the *type* scope at a `yield` the way the
         * frame stack swaps the binding scope. So a lens member is refused here, where the shape is
         * visible, rather than at every call to one.
         */
        if(member.fun.kind != ast::FunKind::Plain) {
            unsupported(member.source, "a lens or iterator as a class member"_v);
            continue;
        }

        /*
         * The member's signature, in the class's context.
         *
         * `declareSignature` makes a context of its own, and the one this needs is the class's -
         * every member of `Describe(a)` is generic in the same `a`, and two contexts would make two
         * variables that nothing can bind together. `classContext` is what it reads.
         */
        auto function = declareSignature(member, member.fun.name, klass->gen);
        if(!function) continue;

        function->klass = pointer;
        function->classMember = U16(klass->members.size());

        /*
         * Registered by name, unless it is one of the compiler's own.
         *
         * A class member is called by name from source - `size(x)` finds it, sees the class, and
         * binds the argument - which is what makes `resolveCall` the one place a call is read. The
         * exception is a *teardown*: `Drop` and `Reclaim` run because a value died, so there is no
         * call to write and registering the member would put `drop` in the module namespace once per
         * class that has one.
         */
        auto teardown = klass->core == CoreClass::Drop || klass->core == CoreClass::Reclaim;
        if(!teardown && !program.addFunctionName(here(), function->name,
                                                 U32(function->args.size()),
                                                 function - *program.decl)) {
            context.diagnostics.error("%@ is already declared in this module"_v, member.source,
                                      context.findName(function->name));
            continue;
        }

        klass->members.push(program.decl, function - *program.decl);
    }

    if(!klass->members.size()) {
        context.diagnostics.error("%@ declares no members"_v, declaration.source,
                                  context.findName(klass->name));
    }
}

/*
 * `instance (Eq(a)) Eq(Opt(a))` - one head, and the members it supplies.
 *
 * The head is a **pattern** over the instance's own context rather than a type, which is the one
 * decision this function is arranged around: `a` is introduced by the position that first writes it,
 * the members are then ordinary generic functions over that context, and selecting one is
 * `matchType` producing its arguments. A ground head is the same structure with nothing to bind, so
 * there is one path here and not two.
 *
 * What the head still has to satisfy is checked in three places below, each for its own reason: the
 * orphan rule, because a removal has to be visible where what it removes from is declared; every
 * variable in a *deciding* position, because a head no call can decide is a declaration mistake
 * rather than a call's; and the superclasses, because this is where both halves are known.
 *
 * The member check is the class's signature, substituted. That is the whole of what used to be a
 * hand-written shape test per core class: `fn drop(->value: a)` with `a` at this head is what
 * `Drop`'s synthesized class already says, so the rule is written once and every class - the
 * compiler's three and the author's - is held to its own declaration.
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

    /*
     * The instance's own generic context - `instance (Eq(a)) Eq(Opt(a))`.
     *
     * Open while the head and its constraints are read, so that `a` is *introduced* by the position
     * that first writes it, exactly as a signature's variables are. What that buys is that a
     * parametric instance's members are ordinary generic functions over this context, and selecting
     * one is `matchType` producing its arguments - so nothing below this line needed a second
     * mechanism for a head that is a shape rather than a type.
     *
     * Made unconditionally and discarded below where the head bound nothing: a ground head with an
     * empty environment would make every reader ask two questions where there is one, which is the
     * rule `isGeneric` states.
     */
    auto environment = new (program.decl) GenContext(true);
    auto gen = environment - *program.decl;

    /*
     * `instance (n: I32) Sized(Buf(n))` - the parameters the context list *declares*, before the
     * head that uses them.
     *
     * Split from the class requirements below, and the split is a dependency rather than a
     * preference: a const parameter's annotation is what says the head's `n` is a count, and the
     * head is read first. A requirement is the other way round - `Eq(a)` is about a variable the
     * head has already introduced - so it stays where it is, after the head, where the class it
     * names has something to be applied to.
     */
    for(auto constraint: written.constraints.contents(parse)) {
        if(constraint.kind != ast::Constraint::Any &&
           constraint.kind != ast::Constraint::Const) {
            continue;
        }

        DeclPtr<Type> annotation;
        if(constraint.kind == ast::Constraint::Const) {
            annotation = resolveConstParameterType(*parse[constraint.constant.type]);
            if(!annotation) return;
        }

        auto name = constraint.kind == ast::Constraint::Const ? constraint.constant.name
                                                             : constraint.name;

        if(findGenVariable(decl(), gen, name)) {
            context.diagnostics.error("%@ is already declared in this head"_v, constraint.source,
                                      context.findName(name));
            return;
        }

        genVariable(program, gen, name, constraint.source, annotation);
    }

    if(written.type.kind != ast::Type::App) {
        unsupported(declaration.source, "this instance head"_v);
        return;
    }

    auto& head = *parse[written.type.app];
    if(head.base.kind != ast::Type::Con) {
        unsupported(declaration.source, "this instance head"_v);
        return;
    }

    auto klass = lookupClass(head.base.name, declaration.source);
    if(!klass) return;

    auto& declared = *decl()[klass];

    SmallArray<DeclPtr<Type>, 4> arguments;

    {
        GenScope generics(*this, gen);

        for(auto argument: head.args.contents(parse)) {
            auto resolved = resolveTypeArgument(argument, declared.gen, arguments.size());
            if(!resolved) return;

            arguments.push(resolved);
        }
    }

    if(!checkArgumentKinds(context, decl(), declared.gen, declared.name, arguments.pointer(),
                           arguments.size(), declaration.source)) {
        return;
    }

    if(!fillDefaults(context, program, declared.gen, arguments)) {
        context.diagnostics.error("%@ takes %@ arguments and this head names %@"_v,
                                  declaration.source, context.findName(declared.name),
                                  U64(declared.arity(decl())), U64(head.args.size()));
        return;
    }

    /*
     * A **nominal** head in every deciding position, and for the three that subtract a capability it
     * is a soundness rule rather than a milestone restriction.
     *
     * `Copy`, `Drop` and `Reclaim` all *take* `TrivialCopy` from their subject, so the head decides
     * whose capability is being taken away. The orphan rule places `instance Copy(MyType)` in
     * `MyType`'s own module, which is where `doc/spec/ownership.md`'s "removing a capability" asks a
     * removal to be visible; an **open** head - `instance (TrivialCopy(a)) Copy(a)` - names no type
     * at all and would take the capability from every type there is, including ones already compiled
     * against the derived answer.
     *
     * **So an open head for one of the three is refused here**, and that is stricter than
     * `declaredInstanceFor` needs it to be: that function already declines to read an open head when
     * it classifies, so admitting one would have made the blanket *selectable* without ever letting
     * it subtract anything. What the strictness costs is `lib/Core`'s own
     * `instance (TrivialCopy(a)) Copy(a)`, which is how `Copy(I32)` is discharged there and cannot
     * be written here - so a generic body requiring `Copy(a)` cannot be instantiated at a primitive
     * yet. Which of the two readings is right is a question about the language rather than about
     * this check, and it is open.
     *
     * An ordinary class is admitted with an open head, because nothing about it is a removal: what
     * an open head costs there is specificity, and `select.h` spends it by trying the keyed table
     * first and admitting at most one such head per class.
     */
    if(declared.core != CoreClass::Count && !headNamesLocalType(arguments)) {
        context.diagnostics.error("%@ may only be given to a type this module declares"_v,
                                  declaration.source, context.findName(declared.name));
        return;
    }

    for(auto constraint: written.constraints.contents(parse)) {
        // Already spent by the declaring pass above, which is the half of this list that binds
        // rather than requires.
        if(constraint.kind == ast::Constraint::Any ||
           constraint.kind == ast::Constraint::Const) {
            continue;
        }

        if(!requireClassConstraint(constraint, "an instance head constraint other than a class"_v)) {
            return;
        }

        GenRequirement requirement;
        if(!readClassRequirement(constraint, gen, true, requirement)) return;

        decl()[gen]->classes.push(program.decl, requirement);
    }

    closeContext(decl(), gen);

    /*
     * Every variable the head binds has to appear in a *deciding* position.
     *
     * `instance Contiguous(Array(a), a)` binds `a` where the key is built and is selectable;
     * `instance Digest(Crc32, n)` would bind `n` only where the dependency says the head answers,
     * so nothing a call supplies could ever decide it and the member could not be specialized.
     * Checked here rather than discovered as a failed match, since a head that can never be selected
     * is a declaration mistake and not a call's.
     */
    if(decl()[gen]->variables.size()) {
        SmallArray<bool, 4> decided;
        for(Size i = 0; i < decl()[gen]->variables.size(); i++) decided.push(false);

        for(Size i = 0; i < arguments.size(); i++) {
            if(declared.isDetermined(U16(i))) continue;
            markVariables(decl(), gen, arguments[i], decided);
        }

        for(Size i = 0; i < decided.size(); i++) {
            if(decided[i]) continue;

            context.diagnostics.error(
                "%@ is bound where %@ answers rather than where it is asked, so no call can decide it"_v,
                declaration.source,
                typeText(context, decl(), decl()[gen]->variables.get(decl(), i)),
                context.findName(declared.name));
            return;
        }
    }

    /*
     * The superclasses, checked here because this is where both halves are known.
     *
     * `instance Ord(T)` needs `instance Eq(T)`, and requiring it at the *instance* is what lets a
     * generic body that requires `Ord(a)` call `Eq(a)`'s members without a second proof - see
     * `satisfiesRequirement`. Declaration order decides which instance is visible, and a superclass
     * instance written below this one is reported rather than silently accepted, because the whole
     * value of the check is that it is one lookup.
     *
     * A **parametric** head proves its superclasses from its own context rather than from the table:
     * `instance (Eq(a)) Eq(Array(a))` cannot look up `Eq(a)`, and what stands in its place is the
     * requirement it declared. That is the same two answers a call site is discharged with, which is
     * why both are asked here in the same order.
     */
    for(auto super: declared.superclasses.contents(decl())) {
        SmallArray<DeclPtr<Type>, 4> needed;

        {
            Substitution applied { declared.gen, arguments.pointer(), arguments.size() };
            for(auto argument: super.args.contents(decl())) {
                needed.push(substituteType(context, program, argument, applied));
            }
        }

        if(satisfiesRequirement(context, program, gen, super.klass, needed.pointer(),
                                needed.size())) {
            continue;
        }

        InstanceMatch found;
        if(selectInstance(context, program, super.klass, needed.pointer(), needed.size(),
                          declaration.source, false, found)) {
            continue;
        }

        context.diagnostics.error("%@ requires %@, and there is no instance of it"_v,
                                  declaration.source, context.findName(declared.name),
                                  appliedClassText(context, decl(), super.klass, needed.pointer(),
                                                   needed.size()));
        return;
    }

    auto instance = new (program.decl) Instance();
    instance->klass = klass;
    instance->source = declaration.source;
    instance->local = local;
    instance->module = here();

    for(auto argument: arguments) instance->args.push(program.decl, argument);

    // A ground head binds nothing, and its members are ordinary functions rather than generics over
    // an empty environment - `isGeneric`'s rule, applied where the context is made rather than at
    // every reader of one.
    instance->gen = decl()[gen]->variables.size() ? gen : nullptr;

    auto pointer = instance - *program.decl;

    for(Size i = 0; i < declared.members.size(); i++) instance->members.push(program.decl, nullptr);

    /*
     * What a member's declared signature is checked against - the class's own, at this head.
     *
     * The head and not a type: `instance (Eq(a)) Eq(Opt(a))` supplies `eq(x: Opt(a), y: Opt(a))`,
     * and substituting the class's parameter by `Opt(a)` is what turns the class's declaration into
     * exactly that. The result is written in the *instance's* variables, which is what a member of a
     * parametric head is generic in.
     */
    Substitution with { declared.gen, arguments.pointer(), arguments.size() };

    // Whether any written member was rejected. A member reported for its own mistake is not a member
    // the instance *left out*, and saying so would be a second message about the first one.
    auto rejected = false;

    for(auto member: written.decls.contents(parse)) {
        if(member.kind != ast::Decl::Fun) {
            unsupported(member.source, "a declaration other than a function in an instance"_v);
            continue;
        }

        /*
         * A plain function, and this is the one place that has to say so.
         *
         * A `lens` has no standalone body - it is flattened into the call that opened it - so a
         * member declared as one is a member nothing can call through the table. Refused here rather
         * than left to the flattening, which would never reach it.
         */
        if(member.fun.kind != ast::FunKind::Plain) {
            context.diagnostics.error(
                "an instance member is a plain function: a lens is flattened into the call that opened it and has no body to dispatch to"_v,
                member.source);
            rejected = true;
            continue;
        }

        /*
         * Which member of the class this is - by name **and arity**, the same key a call uses.
         *
         * A class may declare `shift(x)` and `shift(x, by)`, so a name alone names two members and
         * an instance writing both would have matched the first twice. It is the same rule
         * `Program::addFunctionName` states, read at the member list instead of at the module's.
         */
        auto written = U32(member.fun.args.size());
        auto named = false;
        auto index = classMemberIndex(declared, member.fun.name, written, &named);

        if(index == declared.members.size()) {
            if(named) {
                context.diagnostics.error("%@ declares no %@ taking %@ arguments"_v, member.source,
                                          context.findName(declared.name),
                                          context.findName(member.fun.name), U64(written));
            } else {
                context.diagnostics.error("%@ declares no member called %@"_v, member.source,
                                          context.findName(declared.name),
                                          context.findName(member.fun.name));
            }

            rejected = true;
            continue;
        }

        if(instance->members.get(decl(), U32(index))) {
            context.diagnostics.error("%@ is given twice"_v, member.source,
                                      context.findName(member.fun.name));
            rejected = true;
            continue;
        }

        /*
         * A distinct name for the dump and for the backend's symbol table.
         *
         * The member is not reachable by this name from source - a call finds the *class's*
         * declaration and selection turns it into this one - so what it has to be is unique and
         * readable. The bytes are copied into the declaration arena first, because **an interned
         * name borrows its text**: handing `addUnqualifiedName` a formatted temporary leaves the
         * name table pointing into a buffer that is freed when this function returns.
         */
        auto text = Tritium::format("%@.%@", typeText(context, decl(), arguments[0]),
                                    context.findName(member.fun.name));

        auto stored = (char*)program.decl.alloc(text.size());
        copy(text.text(), stored, text.size());

        /*
         * Declared in the **instance's** context, which is what makes a parametric member work.
         *
         * `instance (Eq(a)) Eq(Opt(a))` writes `fn eq(x: Opt(a), y: Opt(a)) -> Bool` and every `a`
         * in it is the head's, so the member is a generic function over the instance and what runs
         * for `Eq(Opt(I32))` is its specialization. Passing the context in is the same thing
         * `declareClassMembers` does with the class's, and for the same reason: two contexts would
         * make two variables nothing can bind together.
         *
         * A member of a **ground** head gets null and makes a context of its own, which then has to
         * stay empty - an instance member binds no type the head did not.
         */
        auto function = declareSignature(member, context.addUnqualifiedName(stored, text.size()),
                                         instance->gen,
                                         decl()[declared.members.get(decl(), U32(index))], with);
        if(!function) { rejected = true; continue; }

        if(!instance->gen && isGeneric(decl(), function->gen)) {
            context.diagnostics.error("an instance member binds no type of its own"_v, member.source);
            rejected = true;
            continue;
        }

        if(!checkMemberSignature(*function, *decl()[declared.members.get(decl(), U32(index))], with,
                                 member.source)) {
            rejected = true;
            continue;
        }

        // Not a teardown unless the class is one: `copy` borrows its subject and returns a second
        // value, so the drop elaboration has no reason to reach it, and the flag that tells it not
        // to place one for the subject would be wrong.
        function->teardown = declared.core == CoreClass::Drop ||
                             declared.core == CoreClass::Reclaim;

        instance->members.set(decl(), U32(index), function - *program.decl);
    }

    /*
     * A member the instance left out, which is the class's **default** where there is one.
     *
     * Filled with the class's own declaration rather than with a specialization of it, because there
     * is no body yet: bodies are resolved after every declaration is in, and a specialization made
     * here would share a null. `instanceMember` is what turns it into the specialization, at the
     * point something actually calls it.
     */
    for(Size i = 0; !rejected && i < declared.members.size(); i++) {
        if(instance->members.get(decl(), U32(i))) continue;

        auto& missing = *decl()[declared.members.get(decl(), U32(i))];
        if(missing.summary.bodyKnown) continue;

        context.diagnostics.error("this instance of %@ leaves out %@"_v, declaration.source,
                                  context.findName(declared.name), context.findName(missing.name));
        return;
    }

    if(rejected) return;

    if(local && declared.core != CoreClass::Drop && declared.core != CoreClass::Reclaim) {
        // `@local` restricts who may *trigger* a member, and only a teardown is triggered by a value
        // dying rather than by a call the caller wrote. Restricting anything else would be an
        // ordinary export question about a function, which is the module system's and not this
        // attribute's.
        context.diagnostics.error("`@%@` restricts who may run a teardown, and %@ is not one"_v,
                                  declaration.source, attributeName(DeclAttribute::Local),
                                  context.findName(declared.name));
        return;
    }

    module.instances.push(program.decl, pointer);
    writtenInstances[writtenInstances.size() - 1] = pointer;

    if(!program.addInstance(decl(), pointer)) {
        // Two heads of one *shape*, which is what the key is - see `Program::addInstance`. The
        // message names the head rather than the key, since a key is not a thing anyone wrote.
        context.diagnostics.error("%@ already has an instance of this shape"_v, declaration.source,
                                  appliedClassText(context, decl(), klass, arguments.pointer(),
                                                   arguments.size()));
    }
}

/*
 * One written member against the one the class declared, at this instance's type.
 *
 * The class's signature substituted is what every caller is checked against, so an implementation
 * that differs anywhere is a function the dispatch would call with arguments it does not accept.
 * All three halves of the occurrence are compared, and each is a difference the other two do not
 * show. The *type* is what a caller passes; the *contract* is how - `fn drop(value: T)` where the
 * class says `->value: a` would receive a loan of storage that is about to stop existing; and the
 * *shape* is which of the subject's interior positions the member reaches, which is what a caller
 * has to fill in and what an implementation reaching one more of than the class declared would read
 * with nothing published for it.
 */
bool Resolver::checkMemberSignature(Function& written, Function& expected, const Substitution& with,
                                    LocationId source) {
    auto ok = written.args.size() == expected.args.size();

    for(Size i = 0; ok && i < expected.args.size(); i++) {
        auto& supplied = *decl()[written.args.get(decl(), U32(i))];
        auto declared = substituteOccurrence(context, program,
                                             decl()[expected.args.get(decl(), U32(i))]->occurrence,
                                             with);

        ok = supplied.occurrence == declared;
    }

    if(ok) {
        auto declared = substituteOccurrence(context, program, expected.result, with);
        ok = written.result == declared;
    }

    if(!ok) {
        // The signature the class asks for, spelled out. A message that only said the two disagreed
        // would leave the author to work out *which* of the type, the binding and the result moved -
        // and the binding is the half a type comparison does not show.
        context.diagnostics.error("%@ must be written `%@`"_v, source,
                                  context.findName(expected.name), signatureText(expected, with));
    }

    return ok;
}

/*
 * One signature as source would spell it - `fn copy(from: Consuming) -> Consuming`.
 *
 * For a diagnostic and for nothing else, which is why it is here rather than beside `typeText`: what
 * it renders is a *declaration*, and the only reader that wants one is a message telling an author
 * what to write.
 */
String Resolver::signatureText(Function& function, const Substitution& with) {
    auto named = [&](StringId name) {
        auto view = stringView(context.findName(name));
        return String(view.ptr, view.size());
    };

    auto text = String("fn ") + named(function.name) + String("(");

    for(Size i = 0; i < function.args.size(); i++) {
        auto& argument = *decl()[function.args.get(decl(), U32(i))];
        auto occurrence = substituteOccurrence(context, program, argument.occurrence, with);

        if(i) text = text + String(", ");

        // The binding prefix, which is the half a type name does not carry: an owned parameter takes
        // the value and a unique one writes through it, and neither is what an unqualified name says.
        if(occurrence.contract.isOwned()) text = text + String("->");
        else if(occurrence.contract.access() == Access::Unique) text = text + String("&");

        text = text + named(argument.name) + String(": ") +
               typeText(context, decl(), occurrence.type);
    }

    auto result = substituteOccurrence(context, program, function.result, with);
    return text + String(") -> ") + typeText(context, decl(), result.type);
}

/*
 * Whether the defaults of one class bottom out - `doc/spec/classes.md`'s ranks, as a depth-first
 * walk rather than as a number.
 *
 * A default is written in terms of the class's other members, which is the whole point of one: `!=`
 * is `!(lhs == rhs)`. What that costs is the possibility of a circle - `one` written as `two` and
 * `two` written as `one` - and an instance supplying neither then has two functions that call each
 * other and nothing underneath. The compiler accepts the declaration, selection finds a body at
 * every step, and the program recurses until the stack ends.
 *
 * A **signature with no default is where every chain stops**, because an instance has to write it.
 * So the question is whether the graph of "this default calls that member" is acyclic once those are
 * removed, and a three-colour walk answers it: an edge to a member already on the stack is the
 * circle. The current compiler computes a rank per member and compares; the two answer the same
 * question, and this one has nothing to store on the declaration afterwards.
 *
 * A **self-call is the one-member case** of the same circle, and it is refused with the rest. What
 * it would otherwise be is a default that recurses with a base case - which is a real shape and is
 * writable as an ordinary function the default calls, rather than as a member an instance may
 * replace half way down.
 *
 * Asked of the class rather than of an instance, and that is not a simplification: what is wrong is
 * the declaration, and an instance that happens to supply one of the two would leave the mistake in
 * the class for the next instance to find.
 */
static bool rankDefault(Context& context, Program& program, DeclPtr<Class> klassPointer, U32 index,
                        SmallArray<U8, 8>& state) {
    auto base = *program.decl;
    auto mir = *program.mir;
    auto& klass = *base[klassPointer];

    if(state[index]) return true;

    auto& member = *base[klass.members.get(base, index)];

    // No default, so nothing calls through it: an instance writes this one. Settled rather than
    // walked, which is what makes the graph the one the rule is about.
    if(!member.mir) { state[index] = 2; return true; }

    state[index] = 1;
    auto ok = true;
    auto& body = *mir[member.mir];

    for(U32 i = 0; i < body.nodes.size() && ok; i++) {
        auto node = body.node(mir, NodeId(i));
        if(node->kind != Node::Call) continue;

        // A call through a function value names no declaration, so it names no class member and
        // cannot be a step in a default's cycle - see `NodeCall::callsValue`.
        if(((NodeCall*)node)->callsValue()) continue;

        auto& callee = *base[((NodeCall*)node)->callee];
        if(callee.klass != klassPointer) continue;

        // Reported by the frame that finds the back edge and by no other, so a circle of three is
        // one diagnostic naming the step that closes it rather than three naming each other.
        if(state[callee.classMember] == 1) {
            if(callee.classMember == index) {
                context.diagnostics.error(
                    "the default for %@ calls itself, so an instance that does not write %@ has nothing to run"_v,
                    member.source, context.findName(member.name), context.findName(member.name));
            } else {
                context.diagnostics.error(
                    "the default for %@ calls %@, whose own default leads back to it - an instance writing neither would have nothing to run, so one of the two has to be left for every instance"_v,
                    member.source, context.findName(member.name), context.findName(callee.name));
            }

            ok = false;
            break;
        }

        ok = rankDefault(context, program, klassPointer, callee.classMember, state);
    }

    state[index] = 2;
    return ok;
}

void Resolver::checkClassDefaults() {
    for(auto klassPointer: program.classes) {
        auto& klass = *decl()[klassPointer];

        SmallArray<U8, 8> state;
        for(Size i = 0; i < klass.members.size(); i++) state.push(0);

        for(U32 i = 0; i < klass.members.size(); i++) {
            if(!rankDefault(context, program, klassPointer, i, state)) break;
        }
    }
}

} // namespace mir
