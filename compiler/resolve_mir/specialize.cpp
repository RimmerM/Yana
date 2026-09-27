#include "specialize.h"
#include "name.h"
#include "own.h"
#include "select.h"
#include "print.h"
#include "analyze.h"

namespace mir {

Function& bodyOwnerOf(DeclBase base, Function& function) {
    return function.specializes ? *base[function.specializes] : function;
}

Substitution substitutionOf(DeclBase base, Function& function) {
    if(!function.specializes) return Substitution {};

    auto arguments = function.typeArguments.contents(base);
    return Substitution { base[function.specializes]->gen, arguments.pointerAt(0),
                          arguments.size() };
}

DeclPtr<Function> funValueTarget(Context& context, Program& program, Function& enclosing,
                                 DeclPtr<Function> callee, LocationId source) {
    auto base = *program.decl;
    if(!callee || !base[callee]->gen) return callee;

    auto with = substitutionOf(base, enclosing);
    if(with.isEmpty() || with.owner != base[callee]->gen) return callee;

    auto made = specializeFunction(context, program, callee, with.arguments, with.count, source);
    return made ? made : callee;
}

/*
 * The removals this argument list has to pass - `Disposal` in `decl.h`, and waypoint G4's check.
 *
 * A **removal** takes a capability away and supplies nothing in its place, so unlike every other
 * question a generic body asks about its parameters there is no conservative answer that lets the
 * body be compiled once: `@explicitDrop` withholds the implicit teardown, and a body that assumed it
 * had none could not release anything at all. `doc/spec/ownership.md` draws the line there - "a
 * question with a universal fallback is answered once in the body, and a question with no fallback
 * has to be asked at the instantiation" - and this is the instantiation.
 *
 * Run **once per specialization** rather than per call, which is what interning gives: the question
 * is about the argument list, and ten calls at the same types are one answer. What that costs is
 * that the location is the first call to reach these types, which is deterministic in a program and
 * is the point the fix belongs at anyway.
 *
 * The generic's own module is what the `@local` half compares against, and not the caller's: the
 * disposal is written in the generic's body and runs there, so the module that has to have
 * permission is the one the body is in. That is `bodyOwnerOf` read at declaration time, and it is
 * why the check is here rather than at the call.
 */
static void checkDisposals(Context& context, Program& program, Function& generic,
                           const Substitution& with, LocationId source) {
    auto base = *program.decl;

    for(auto disposal: generic.disposals.contents(base)) {
        auto type = substituteType(context, program, disposal.type, with);
        if(!type) continue;

        if(disposal.implicit && needsExplicitDisposal(base, type)) {
            context.diagnostics.error(
                "%@ may not be disposed of implicitly, and %@ chose the point where the one it holds ends"_v,
                source, typeText(context, base, type), context.findName(generic.name));
        }

        if(!disposalPermitted(context, program, type, generic.module)) {
            context.diagnostics.error(
                "%@ has a `@local` teardown, and %@ disposes of one - it may only be disposed of in the module that declared it"_v,
                source, typeText(context, base, type), context.findName(generic.name));
        }
    }
}

static U32 typeDepth(DeclBase base, DeclPtr<Type> type);

/*
 * Whether making this specialization is a step of a chain that will not end - polymorphic
 * recursion, seen as **growth along the chain of discoveries** rather than as an absolute depth.
 *
 * `f(a)` calling `f(Boxed(a))` specializes at `a`, then at `Boxed(a)`, then at `Boxed(Boxed(a))`:
 * no fixed set of instantiations, and a worklist over argument lists never runs out. What tells that
 * chain from an ordinary one is not how deep the type is - a one-off `id` call at a twenty-deep
 * record is finite and was refused by the old bound of sixteen as "built from its own" - but that
 * the specialization being walked was itself made because of one of this generic with a *shallower*
 * argument list, and that one because of a shallower one still. `Program::specializationParents`
 * is that chain, and this counts the ancestors of the same generic that are strictly shallower than
 * the argument list asked for now. A diverging chain adds one at every step; a finite program
 * reaches a few at most, since each is a level of type growth through one generic that has to end
 * somewhere - an instance selecting a different body at the deeper type is the usual way.
 *
 * Indirect recursion is the same chain: `f(a)` through `g(Boxed(a))` back to `f(Boxed(Boxed(a)))`
 * has `g` between two `f`s, and the count is over the `f`s alone.
 *
 * A diagnostic about the choice of lowering rather than about the program - nothing about it is
 * infinite, it is specialization that cannot represent it - which is why the erased ABI is the
 * answer that compiles it and why `@generic` on the callee is the fix rather than a smaller program.
 * The resource budget is a separate number, `kSpecializationsPerGeneric`, and is about the program.
 */
static constexpr U32 kGrowthSteps = 4;

static U32 argumentsDepth(DeclBase base, const DeclPtr<Type>* args, Size count) {
    U32 deepest = 0;
    for(Size i = 0; i < count; i++) deepest = max(deepest, typeDepth(base, args[i]));
    return deepest;
}

static bool growthPermitted(Context& context, Program& program, DeclPtr<Function> generic,
                            DeclPtr<Type>* args, Size count, LocationId source) {
    auto base = *program.decl;
    auto depth = argumentsDepth(base, args, count);
    U32 growth = 0;

    for(auto at = program.discoveringFrom; at;) {
        auto& ancestor = *base[at];

        // A plain function ends the chain: it was not made because of anything.
        if(!ancestor.specializes) break;

        if(ancestor.specializes == generic) {
            auto own = argumentsDepth(base, ancestor.typeArguments.pointer(base),
                                      ancestor.typeArguments.size());
            if(own < depth) growth++;
        }

        auto parent = program.specializationParents.getValue(U32(at.offset));
        if(!parent) break;
        at = DeclPtr<Function>(parent.unwrap());
    }

    if(growth < kGrowthSteps) return true;

    context.diagnostics.error(
        "%@ is called at a type built from its own, so specializing it does not terminate - it has to be compiled once, with `@generic`"_v,
        source, context.findName(base[generic]->name));
    return false;
}

/*
 * One specialization, made once per argument list.
 *
 * The signature is substituted and the *body is not copied*: `mir` is the generic's own handle, so
 * this declaration and the generic run the same nodes, the same places and the same elaboration. The
 * only thing that differs is what the types under them are read as, which is `substitutionOf`.
 *
 * Registered in the generic's own module rather than the caller's, so that two callers at the same
 * types share one function and a specialization is emitted where its declaration is - the same
 * placement rule `instantiateRecord` follows, and for the same reason.
 */
DeclPtr<Function> specializeFunction(Context& context, Program& program, DeclPtr<Function> generic,
                                     DeclPtr<Type>* args, Size count, LocationId source) {
    auto base = *program.decl;
    auto& written = *base[generic];

    assertTrue(written.gen && base[written.gen]->variables.size() == count);

    // A specialization is what a *bound* signature is. One holding a variable would be a second
    // generic under another name, with an environment nothing filled and a layout nothing has - so
    // the rule the header states is checked where the only producer is.
    for(Size i = 0; i < count; i++) assertTrue(isConcreteType(base, args[i]));

    auto slot = program.internSpecialization(generic, args, count);
    if(*slot) return *slot;

    // Before the budget and before the name: a chain that does not terminate is refused on what it
    // is rather than on what it has cost so far.
    if(!growthPermitted(context, program, generic, args, count, source)) return nullptr;

    // A *new* body, which is the only thing a budget bounds - see `specializationPermitted`. Counted
    // here rather than at the call because this is where one is made, and several calls at one
    // argument list reach the line above instead.
    written.specializations++;

    /*
     * `id(I32)` - a name of its own, and it has to be one.
     *
     * Everything below the resolver keys a function by its name: the lowered module holds one map
     * from `StringId` to function, and the backend's symbol table holds one entry per name. Two
     * specializations of `id` sharing the generic's name would be one symbol with two bodies, which
     * is not a diagnostic anywhere - the second simply replaces the first.
     *
     * The bytes are copied into the declaration arena before they are interned, because **an
     * interned name borrows its text**: handing `addUnqualifiedName` a formatted temporary leaves
     * the name table pointing into a buffer this function is about to return past.
     */
    auto name = stringView(context.findName(written.symbol));
    auto text = String(name.ptr, name.size()) + String("(");
    for(Size i = 0; i < count; i++) {
        if(i) text = text + String(", ");
        text = text + typeText(context, base, args[i]);
    }

    text = text + String(")");

    // Through `uniqueSymbol` for the reason the generic's own name went through it: two class
    // members of one name specialized at one type would mangle alike, and the mangling is what the
    // symbol table keys on. It is built from the generic's **symbol** rather than its source name,
    // so the disambiguation the generic already got is carried rather than redone.
    auto symbol = program.uniqueSymbol(written.module, context,
                                       StringView(text.text(), text.size()));

    auto function = new (program.decl) Function(symbol, written.module);
    auto pointer = function - base;

    function->source = written.source;
    function->specializes = generic;

    // Where in the chain of discoveries this one sits - see `Program::discoveringFrom`.
    if(program.discoveringFrom) {
        auto parent = program.specializationParents.add(U32(pointer.offset));
        *parent.value = U32(program.discoveringFrom.offset);
    }

    // Carried because it is what a *caller* has to know to call this at all - a lens hands over to a
    // continuation and a plain function returns. Always false today, since a lens and a generic in
    // one declaration are refused by name; set anyway, because the flag being dropped is not a thing
    // the milestone that lifts that refusal would think to look for. `checkSpecialization` holds the
    // two to agreeing.
    function->lens = written.lens;
    function->alwaysInline = written.alwaysInline;
    function->inlineHint = written.inlineHint;

    // The frequency the generic declared, which substitution does not change: how often a body runs
    // is a statement about the call sites and not about the types they are at.
    function->cold = written.cold;
    function->hot = written.hot;
    function->noInline = written.noInline;
    function->teardown = written.teardown;
    function->authoredReclaim = written.authoredReclaim;

    // And whether it *is* a view's derivation, on the same terms: `Contiguous(Array(a), a).elements`
    // specialized at `a := I32` is still the member that makes a window out of a container, and the
    // SSA conversion reads the flag to fold one applied to a window it already has.
    function->derivation = written.derivation;
    function->originSlots = written.originSlots;
    function->summary.bodyKnown = written.summary.bodyKnown;

    for(Size i = 0; i < count; i++) function->typeArguments.push(program.decl, args[i]);

    /*
     * Interned before the signature is substituted, on `Types::instantiate`'s terms.
     *
     * A recursive generic - `fn f(->x: a) -> a = f(x)` - reaches its own specialization while this
     * one is still being built, and finding the handle already there is what makes that an ordinary
     * self-call rather than a walk that does not terminate.
     */
    *slot = pointer;

    Substitution with { written.gen, args, count };

    for(auto argumentPointer: written.args.contents(base)) {
        auto& declared = *base[argumentPointer];

        auto substituted = new (program.decl) Arg();
        substituted->name = declared.name;
        substituted->source = declared.source;
        substituted->index = declared.index;
        substituted->occurrence = substituteOccurrence(context, program, declared.occurrence, with);

        function->args.push(program.decl, substituted - base);
    }

    function->result = substituteOccurrence(context, program, written.result, with);

    /*
     * A lifted lambda's environment, which a lambda written in a generic body has (C3): argument
     * zero's type substituted with the rest, and where the storage goes copied, since the placement
     * was decided from the position the lambda was written at and no argument list changes that.
     */
    function->environment = substituteType(context, program, written.environment, with);
    function->environmentStorage = written.environmentStorage;

    // What this argument list has to pass - see above. After the signature rather than before it, so
    // that a diagnostic here leaves a complete declaration behind for the passes that walk on.
    checkDisposals(context, program, written, with, source);

    /*
     * The generic's body, shared rather than copied - see the header.
     *
     * `mir` is read through `bodyOwnerOf` everywhere it matters, and is set here as well so that the
     * passes which only ask "does this function have a body" - reachability, conversion's own filter
     * - keep working unchanged. `Program::releaseMir` clears both, and clearing a handle twice is
     * what it already does for a function reached from two modules.
     */
    function->mir = written.mir;

    base[written.module]->functions.push(program.decl, pointer);
    return pointer;
}

/*
 * How deeply a type argument is nested - what `growthPermitted` compares along the chain of
 * discoveries, since a diverging chain is one whose argument lists get strictly deeper.
 *
 * A depth rather than a count of specializations, because the count is what a legitimately
 * generic-heavy program has a lot of and the depth is what only a diverging one grows - and a
 * *comparison* of depths rather than a bound on one, because a deep type on its own is a finite
 * program that merely nests.
 */
static U32 typeDepth(DeclBase base, DeclPtr<Type> type) {
    if(!type) return 1;

    // A pointer nests exactly as an instantiation does - `%a`, `%%a`, `%%%a` is the same chain of
    // strictly larger types with a different constructor - so the walk descends through both. Only
    // reachable once something can take an address, which is milestone 7's; written here anyway,
    // because a depth that answered one for a whole family is a bound that silently is not one.
    switch(base[type]->kind) {
        case Type::Pointer:
            return 1 + typeDepth(base, ((PointerType*)base[type])->to);
        case Type::Record: {
            U32 deepest = 0;
            for(auto argument: ((RecordType*)base[type])->arguments.contents(base)) {
                deepest = max(deepest, typeDepth(base, argument));
            }

            return 1 + deepest;
        }
        // `[[a *2] *2]` nests the same way a pointer does, and a bound that answered one for the
        // whole family would not be a bound.
        case Type::Array:
            return 1 + typeDepth(base, ((ArrayType*)base[type])->element);

        // A lane and an atomic's content, on the same reading. A vector's lane cannot in fact nest -
        // a lane is a machine width - but a bound that answered one for a constructor because
        // today's rules happen to keep it shallow is a bound resting on a rule stated elsewhere.
        case Type::Vector:
            return 1 + typeDepth(base, ((VectorType*)base[type])->lane);

        case Type::Atomic:
            return 1 + typeDepth(base, ((AtomicType*)base[type])->content);
        default:
            return 1;
    }
}

/*
 * One member of one instance - what it wrote, or the class's default made concrete.
 *
 * Two sources and one shape. A **written** member of a parametric instance is a generic function
 * over the *instance's* own context, so what runs for `Eq(Array(I32))` is that member specialized at
 * what the head bound; a written member of a ground head has nothing to bind and is itself. A
 * **default** is a generic body over the *class's* context, so what runs is the default specialized
 * at the class's arguments.
 *
 * The two differ only in which context is substituted and with what, which is why they are one
 * function: a default and a hand-written implementation are the same kind of thing by the time
 * anything calls one - one function, one signature, one body, chosen at the point of use.
 *
 * Nothing is cached on the `Instance`, and that is the change a parametric head forces: one instance
 * serves every argument list its head matches, so a member slot on it could only have held the first
 * of them. `specializeFunction` interns per argument list, which is the same table doing the same
 * job at the granularity the answer actually has.
 */
DeclPtr<Function> instanceMember(Context& context, Program& program, const InstanceMatch& found,
                                 U16 member, LocationId source) {
    auto base = *program.decl;
    auto& instance = *base[found.instance];

    if(auto written = instance.members.get(base, member)) {
        if(!instance.gen) return written;

        return specializeFunction(context, program, written,
                                  (DeclPtr<Type>*)found.bindings.pointer(), found.bindings.size(),
                                  source);
    }

    auto declared = base[instance.klass]->members.get(base, member);

    /*
     * A slot the instance left empty whose class declaration has **no default** - which for a written
     * instance is already reported (`resolveInstance` refuses one that leaves a bodiless member out)
     * and for a *generated* one is a gap in the compiler.
     *
     * Reported here rather than where the instance is built, because the two are different
     * statements: the compiler generating three of a class's four members is a fact about the
     * compiler, and only a call of the fourth is a program that cannot be compiled. Nothing is in
     * that state today - `Enum`'s `nameOf` and `fromName` were, and are generated since 2026-09-06 -
     * so this is the guard a partial `providesMember` row would otherwise be found by.
     *
     * The specialization is still handed back, so the caller carries on and reports whatever else it
     * finds rather than cascading off a null; the compilation has an error and stops at the next
     * checkpoint. Without this a call reached a `Function` with no body at all, which every pass
     * below silently skips - and what came out was a call to a symbol nothing emits.
     */
    if(!base[declared]->summary.bodyKnown && instance.provided != intrinsic::Provided::Count) {
        context.diagnostics.error("the compiler does not generate %@ for %@ yet"_v, source,
                                  context.findName(base[declared]->name),
                                  appliedClassText(context, base, instance.klass,
                                                   found.arguments.pointer(),
                                                   found.arguments.size()));
    }

    return specializeFunction(context, program, declared,
                              (DeclPtr<Type>*)found.arguments.pointer(), found.arguments.size(),
                              source);
}

DeclPtr<Function> teardownMember(Context& context, Program& program, intrinsic::Known klass,
                                 DeclPtr<Type> type, LocationId source) {
    InstanceMatch found;
    if(!declaredInstanceFor(context, program, klass, type, &found)) return nullptr;

    return instanceMember(context, program, found, 0, source);
}

DeclPtr<Function> viewMember(Context& context, Program& program, DeclPtr<Type> type, Access access,
                             LocationId source) {
    auto base = *program.decl;
    auto row = viewRowOf(base, program, type);
    if(!row) return nullptr;

    auto adaptor = access == Access::Unique ? row->uniqueAdaptor : row->sharedAdaptor;
    auto member = access == Access::Unique ? row->uniqueMember : row->sharedMember;
    if(adaptor == intrinsic::Known::Count) return nullptr;

    InstanceMatch found;
    if(!declaredInstanceFor(context, program, adaptor, type, &found)) return nullptr;

    return instanceMember(context, program, found, member, source);
}

bool matchCallable(Context& context, Program& program, DeclPtr<Module> from,
                   GenCallable required, const Substitution& with, CallableWitness& into) {
    auto base = *program.decl;

    /*
     * Substituted first and *then* looked up, which is the whole of what makes this a witness rather
     * than an ordinary call: the body was typed against a promise about `a`, and what answers it is
     * whatever `grow` means at the type `a` turned out to be.
     */
    into.concrete = true;

    for(auto argument: required.args.contents(base)) {
        auto supplied = substituteType(context, program, argument, with);
        if(!supplied) return false;

        into.concrete = into.concrete && isConcreteType(base, supplied);
        into.args.push(supplied);
    }

    into.result = substituteType(context, program, required.result, with);
    if(!into.result) return false;

    into.concrete = into.concrete && isConcreteType(base, into.result);
    if(!into.concrete) return true;

    /*
     * By name and arity, which is how a call site selects an overload - so a requirement is a class
     * of one member whose dispatch key is a name, and nothing here invents a second way to find a
     * callee.
     *
     * Looked up **where the requirement was written**, not where the instantiation is: the body was
     * typed against a promise its own signature made, so what discharges it is what that signature's
     * module and file could see. `FileScope` is what puts the position back, since this runs long
     * after the pass that read the declaration walked past it.
     */
    FileScope scope(base, context, from, required.source);
    into.found = mir::findFunction(context, program, from, required.name, U32(into.args.size()),
                                   required.source, false);
    if(!into.found) return true;

    /*
     * The callee's declared signature against the one the requirement asked for.
     *
     * `matchType` where the callee is generic, so `fn grow(v: a) -> a` answers a requirement for
     * `(Vessel) -> Vessel`; the same walk binds nothing where it is not, since a context with no
     * variables matches by nominal identity throughout. One walk for both, because a generic
     * declaration matched at a concrete signature *is* the pattern match, and a second comparison for
     * the concrete case is how the two come to disagree about a nested position.
     */
    auto& declared = *base[into.found];
    into.generic = isGeneric(base, declared.gen);

    auto owner = into.generic ? declared.gen : DeclPtr<GenContext>(nullptr);
    for(Size i = 0; into.generic && i < base[owner]->variables.size(); i++) into.bindings.push(nullptr);

    into.agrees = true;
    for(Size i = 0; i < into.args.size(); i++) {
        auto position = base[declared.args.get(base, U32(i))]->occurrence.type;
        into.agrees = into.agrees &&
                      matchType(base, program.types, owner, into.bindings, position, into.args[i]);
    }

    into.agrees = into.agrees
        && matchType(base, program.types, owner, into.bindings, declared.result.type, into.result);

    // A position the match never reached leaves a null, which is a signature that agrees where it was
    // compared and says nothing about a variable the requirement does not mention.
    for(auto binding: into.bindings) {
        into.agrees = into.agrees && binding && isConcreteType(base, binding);
    }

    return true;
}

/*
 * `grow(x)` typed against `grow: (a) -> a`, at one argument list - `NodeCall::genCallable`.
 *
 * `dischargeCallable` asked exactly this at the call that supplied the argument list, so what is
 * left here is to spend the answer: a generic witness is specialized at the bindings the match
 * produced, and a concrete one is itself. A failure is therefore a compiler error rather than a
 * program one, and it is reported as one instead of lowering a call to the wrong function.
 *
 * In an **erased** body there is no argument list to spend, and the same requirement is answered the
 * other way: the function slot the schema numbers for it holds whatever the caller put there. Which
 * of the two happens is the enclosing body's form and nothing else, which is the whole of what a
 * form decides.
 */
static void callableTarget(Context& context, Program& program, Function& enclosing,
                           const CallRequest& node, CallSite& into) {
    auto base = *program.decl;

    auto& generic = bodyOwnerOf(base, enclosing);
    auto required = base[generic.gen]->callables.get(base, node.genCallable);

    if(enclosing.form == GenForm::Erased) {
        into.through = CallThrough::FunctionSlot;
        into.requirement = node.genCallable;
        return;
    }

    CallableWitness witness;
    if(!matchCallable(context, program, generic.module, required,
                      substitutionOf(base, enclosing), witness)) {
        return;
    }

    // Still open after substituting, which leaves the callee as it was for the reason `callTarget`
    // does below: this is the *generic* body being walked rather than one of its specializations,
    // and there is nothing to select against a variable.
    if(!witness.concrete) return;

    if(!witness.found || !witness.agrees) {
        context.diagnostics.error("internal: nothing answers the required %@ at this call"_v,
                                  node.source, context.findName(required.name));
        return;
    }

    if(!witness.generic) {
        into.target = witness.found;
        return;
    }

    into.target = specializeFunction(context, program, witness.found, witness.bindings.pointer(),
                                     witness.bindings.size(), node.source);
}

/*
 * Which declared class requirement of `gen` promises `klass(args)`, and **where in its witness** the
 * members are - the slot a member call in an erased body dispatches through, plus the offset.
 *
 * A direct match is offset zero. A match through a **superclass** is the rest, and it works because
 * a witness table is flattened: `class (Eq(a)) Ord(a)` puts `Eq`'s members after `Ord`'s in one
 * table, so the promise the discharge already admitted has somewhere to be read from. That is what
 * `freezeSchema` means by superclasses not being slots of their own - they are not slots, they are
 * ranges - and it is why this is `classWitnessPath` rather than `satisfiesRequirement`: the latter
 * answers yes without saying which superclass of which requirement it took, and two requirements can
 * both prove one class.
 *
 * The **declared** requirements only, in schema order, because that is what a caller fills.
 */
static bool classRequirementSlot(Context& context, Program& program, DeclPtr<GenContext> gen,
                                 DeclPtr<Class> klass, const DeclPtr<Type>* args, Size count,
                                 U16& into, U16& offset) {
    if(!gen) return false;

    auto base = *program.decl;
    auto& owner = *base[gen];

    for(U32 i = 0; i < owner.classes.size(); i++) {
        auto requirement = owner.classes.get(base, i);
        auto held = requirementArgs(base, requirement);

        U16 found = 0;
        if(!classWitnessPath(context, program, requirement.klass, held.data, held.count, klass,
                             args, count, found)) {
            continue;
        }

        into = U16(i);
        offset = found;
        return true;
    }

    return false;
}

/*
 * One class witness, **flattened**: the class's own members, then each superclass's, recursively.
 *
 * The layout `classWitnessCount` states, filled. A superclass is discharged here rather than left to
 * a second slot because the caller holds `Ord(T)` and the callee may call `same` - the promise is
 * already proved by the instance declaration, so what was missing was somewhere to put the answer.
 *
 * `instanceMember` is what fills a member the instance left out with the class's default, and it is
 * the same function `callTarget` reaches under specialization. That it is the same one is the point:
 * an erased call and a specialized call to one class member run one body, so nothing about which
 * form a caller took can change what a program does.
 */
static bool fillClassWitness(Context& context, Program& program, DeclPtr<Class> klass,
                             const DeclPtr<Type>* args, Size count, LocationId source,
                             GenWitness& into) {
    auto base = *program.decl;

    /*
     * `Call(f, S)` - the one witness whose contents no instance supplies, because the class is
     * structural and declares no members (§4).
     *
     * What goes in the single cell is decided from the subject exactly as `structuralAnswer` decides
     * whether the class holds at all: a **site type** names the code it is called through, so the
     * cell is its lifted function; a **function type** is a pair, so the code is a word of the value
     * and the cell is a shim the lowering writes over it - there is no declaration to name, and the
     * null is what says so. `witnessFor` is where the table is built, and it keys on the type for
     * that reason: two `Fun` subjects both leave the cell unnamed here and are two different shims.
     */
    if(count == 2 && args[0] && base[klass]->role == intrinsic::Known::call) {
        into.type = args[0];

        /*
         * A **site type**'s lifted function is a declaration and goes in the list, which is also the
         * only thing that keeps it: in a body compiled once nothing names the lambda, so this is the
         * edge the reachability walk follows to convert it. A **function type** names none - the
         * code is a word of the value - so the list stays empty and the cell is a shim.
         *
         * Empty rather than a null entry, because every reader of a witness's function list roots,
         * holds or counts what is in it, and a null would be a declaration pointer at offset zero.
         */
        if(base[args[0]]->kind == Type::Record) {
            auto& record = *(RecordType*)base[args[0]];
            if(record.lambda && record.lambdaShape) {
                into.functions.push(program.decl, record.lambda);
            }
        }

        return true;
    }

    InstanceMatch found;
    if(!selectInstance(context, program, klass, (DeclPtr<Type>*)args, count, source, true, found)) {
        return false;
    }

    auto members = base[klass]->members.size();
    for(U16 member = 0; member < members; member++) {
        auto reached = instanceMember(context, program, found, member, source);
        if(!reached) return false;

        into.functions.push(program.decl, reached);
    }

    /*
     * And the **"make an object"** entry of every member written `iter fn` - §6.4, and the region
     * `classWitnessCount` puts after the declared members.
     *
     * A member declared `iter fn` cannot be called through a witness at all: what a call to one
     * produces is a state record minted per instance, and a body compiled once has no substitution
     * to mint it under. What it can be handed is the erased form, so what goes in the cell is a
     * function over *this instance's* record that builds one and hands back the pair.
     *
     * Through `Program::IterObjects` because the record is minted from the instance's own body,
     * which is the resolver's to read - see the hook, where the split is argued. A program that
     * declares no `iter fn` member reaches none of this and its tables are what they were.
     */
    for(U16 member = 0; member < members; member++) {
        if(!base[base[klass]->members.get(base, member)]->iterObject) continue;

        auto reached = instanceMember(context, program, found, member, source);
        if(!reached) return false;

        if(!program.iterObjects) {
            context.diagnostics.error(
                "internal: a witness of %@ needs an object of %@ and nothing can make one here"_v,
                source, appliedClassText(context, base, klass, (DeclPtr<Type>*)args, count),
                context.findName(base[reached]->name));
            return false;
        }

        auto make = program.iterObjects->objectMaker(
            reached, base[base[klass]->members.get(base, member)]->iterObject, source);
        if(!make) return false;

        into.functions.push(program.decl, make);
    }

    // The superclasses, at what this class's head says they are applied to - `classProves`'
    // substitution, made here for the same reason it is made there.
    Substitution with { base[klass]->gen, (DeclPtr<Type>*)args, count };

    for(auto super: base[klass]->superclasses.contents(base)) {
        SmallArray<DeclPtr<Type>, 4> arguments;
        for(auto argument: super.args.contents(base)) {
            arguments.push(substituteType(context, program, argument, with));
        }

        if(!fillClassWitness(context, program, super.klass, arguments.pointer(), arguments.size(),
                             source, into)) {
            return false;
        }
    }

    return true;
}

/*
 * The environment one call to an erased generic hands over - `GenEnvironment`.
 *
 * Every requirement of the frozen schema discharged against one argument list, in schema order. The
 * work is exactly what `specializeFunction` does *implicitly* by substituting a body: an instance is
 * selected, a field is found, a callable is matched. What an erased form changes is only where the
 * answer goes - into data the caller passes, rather than into types the callee was compiled at - so
 * this walk and the substitution have to agree, and they do because both ask the same three
 * functions.
 *
 * Interned, so the slots are filled once per argument list however many calls reach them. The slot
 * is claimed before the walk runs for `internSlot`'s own reason: a requirement whose discharge
 * reaches this generic again finds a handle rather than recursing.
 */
DeclPtr<GenEnvironment> environmentFor(Context& context, Program& program, DeclPtr<Function> generic,
                                       DeclPtr<Type>* args, Size count, LocationId source) {
    auto base = *program.decl;
    auto& written = *base[generic];

    assertTrue(written.gen && base[written.gen]->variables.size() == count);
    for(Size i = 0; i < count; i++) assertTrue(isConcreteType(base, args[i]));

    auto slot = program.internEnvironment(generic, args, count);
    if(*slot) return *slot;

    auto environment = new (program.decl) GenEnvironment();
    *slot = environment - base;

    environment->generic = generic;
    for(Size i = 0; i < count; i++) environment->arguments.push(program.decl, args[i]);

    // Over the environment's own copy of the arguments, not the caller's buffer: a `Substitution`
    // holds a pointer, and this one outlives every caller because the list is in the arena the
    // declaration is.
    Substitution with { written.gen, environment->arguments.pointer(base), count };

    auto schema = base[written.gen]->schema;
    assertTrue(schema); // `freezeSchemas` runs before anything asks for a numbering - see gen.h.

    auto failed = false;
    auto& numbering = *base[schema];

    for(U32 index = 0; index < numbering.slots.size(); index++) {
        auto row = numbering.slots.get(base, index);

        GenWitness witness;
        witness.kind = row.kind;

        switch(row.kind) {
            case GenSlotKind::Type: {
                witness.type = substituteType(context, program, row.type, with);
                failed = failed || !witness.type || !isConcreteType(base, witness.type);

                /*
                 * How the values this slot describes were allocated - `GenWitness::storage`, and the
                 * one thing here that is not a function of the argument list.
                 *
                 * `Frame` because it is the only rung anything selects: `StorageClass::Heap` has no
                 * allocator behind it and `Region` has no witness, and `verifyBody` refuses an
                 * object on either. What makes it a field rather than a constant in `erase.cpp` is
                 * that the descriptor's teardown is chosen from it, so the day a call site can
                 * allocate an argument differently the answer is already carried where it is read.
                 *
                 * `internEnvironment` keys on (generic, arguments) and this is the assertion that
                 * lets it: two sites at one argument list that allocated differently want two
                 * environments, so the key has to grow the moment this stops being constant.
                 */
                witness.storage = StorageClass::Frame;
                break;
            }

            /*
             * A const parameter's value, which the argument list already carries as a `ConstType`.
             *
             * Read off the substituted variable rather than off the argument at its index, because
             * which index a const parameter is at is the numbering's question and this walk is the
             * numbering being read - see `GenSchema`. The two agree; asking the schema is what keeps
             * them from having to.
             */
            case GenSlotKind::Const: {
                auto bound = substituteType(context, program, row.type, with);
                if(!bound || base[bound]->kind != Type::Const) { failed = true; break; }

                // A count that is still an expression is not a number, so the witness has nothing to
                // put in the slot - the same failure a variable is, said for the second state a
                // `ConstType` has.
                auto& number = *(ConstType*)base[bound];
                if(number.isDerived()) { failed = true; break; }

                witness.constant = number.value;
                break;
            }

            /*
             * A class witness - one function per member of the class, in the class's order.
             *
             * `instanceMember` is what fills a member the instance left out with the class's
             * default, and it is the same function `callTarget` reaches under specialization. That
             * it is the same one is the point: an erased call and a specialized call to one class
             * member run one body, so nothing about which form a caller took can change what a
             * program does.
             */
            case GenSlotKind::Class: {
                SmallArray<DeclPtr<Type>, 4> arguments;
                for(auto argument: row.args.contents(base)) {
                    arguments.push(substituteType(context, program, argument, with));
                }

                failed = failed || !fillClassWitness(context, program, row.klass,
                                                     arguments.pointer(), arguments.size(), source,
                                                     witness);
                break;
            }

            /*
             * A property witness - which field of the substituted subject, by the name the
             * requirement stated.
             *
             * The *index* and not the offset: an offset is a layout, and a layout is the target's
             * answer. Carrying the index is what keeps `fieldOffsetNative` the one place a field's
             * position is computed, which is the rule that walk exists to state.
             */
            case GenSlotKind::Property: {
                witness.type = substituteType(context, program, row.type, with);
                if(!witness.type || base[witness.type]->kind != Type::Record) { failed = true; break; }

                auto& record = *(RecordType*)base[witness.type];
                auto found = false;

                for(auto field: record.fields.contents(base)) {
                    if(field.name != row.name) continue;

                    witness.field = field.index;
                    found = true;
                    break;
                }

                if(!found) {
                    context.diagnostics.error(
                        "%@ has no field called %@, and the signature requires one"_v, source,
                        typeText(context, base, witness.type), context.findName(row.name));
                    failed = true;
                }

                break;
            }

            /*
             * A function witness - `matchCallable`, the same walk the call site read to report.
             *
             * Asked again rather than remembered, for the reason the header gives: the discharge and
             * the target have to agree about *which* function answers, and one walk asked twice
             * cannot disagree with itself.
             */
            case GenSlotKind::Function: {
                auto required = base[written.gen]->callables.get(base,
                                                                  index - numbering.functionStart);
                CallableWitness answer;

                if(!matchCallable(context, program, written.module, required, with, answer) ||
                   !answer.concrete || !answer.found || !answer.agrees) {
                    context.diagnostics.error(
                        "nothing answers the required %@ at this call"_v, source,
                        context.findName(required.name));
                    failed = true;
                    break;
                }

                auto reached = answer.generic
                    ? specializeFunction(context, program, answer.found, answer.bindings.pointer(),
                                         answer.bindings.size(), source)
                    : answer.found;

                if(!reached) { failed = true; break; }
                witness.functions.push(program.decl, reached);
                break;
            }

            case GenSlotKind::Count:
                break;
        }

        environment->slots.push(program.decl, witness);
    }

    return failed ? nullptr : *slot;
}

/*
 * The specialization budget - Implementation-Generics.md §10's "per-module and global budgets to
 * prevent unbounded code growth", and its cost model's fifth input.
 *
 * **Per generic**, which is the doc's own fifth input - "the same function specialized at N distinct
 * contexts costs N bodies" - and is what a container method reached at every element type in a
 * program does.
 *
 * The program-wide half of §10's "per-module and global budgets" is **not here**, and the reason is
 * a measurement rather than an omission. The scale-free way to write it is a growth *ratio* - the
 * instructions specialization adds against the program's own - and measured over this corpus that
 * ratio is 9.1x on the smallest fixture and at most 0.73x on every other, because the denominator
 * collapses on a small program while the numerator does not. A bound shaped like that fires on
 * exactly the programs it should leave alone. The alternative, a flat count, needs a number chosen
 * against a real program rather than against fixtures whose largest total is six. Both want a
 * program bigger than a fixture to calibrate on, so the term waits for one.
 *
 * Asked only where a specialization would be **new**, which the intern probe answers: ten calls at
 * one argument list are one body and the budget is about bodies. The probe creates the slot and
 * leaves it null, which is what "no such specialization yet" already means, so asking costs nothing
 * a later `specializeFunction` does not spend anyway.
 *
 * Asked only at a **call**, and this is the limit worth stating. `instanceMember` and
 * `teardownMember` also specialize, and neither may be declined: a teardown selected during the
 * analysis has no second form to fall back to and a null there reads as "this type has nothing to
 * run", which would be a value that quietly leaks rather than a program that fails to compile.
 * Design.md puts the choice "per instantiation" at a call site, and that is exactly where a legal
 * alternative exists.
 *
 * The number is far above anything a real program reaches, and deliberately so, because **a crossed
 * budget is a diagnostic rather than a fallback**. What a budget is *for* is declining the marginal
 * specialization and emitting the erased form instead, and that form now exists - but the form is
 * chosen by `selectGenForms`, before any specialization is made, and how many argument lists a
 * generic is reached at is what *this* walk discovers. So the two are one round apart: turning the
 * decline into a fallback is deciding the form after a first specialization round, which is the term
 * the G6 note named and the pass this file does not have. Until then the number sits where it cannot
 * fire on a program that did nothing unusual, and the message names `@generic` as the answer.
 *
 * **128, measured - K1.** It was 32, chosen when the largest program was a fixture reaching six, and
 * `test/base` - the whole library's `@test` suite compiled as one program - is the program the
 * paragraph above was waiting for: it reached exactly 32 at `releaseRun`, once per heap array
 * element type anywhere in the library, and J193 and J194 each had to write a test around the
 * thirty-third. `releaseRun` no longer multiplies (it takes its run untyped); what is left at the top
 * is `values` and `checkEqual` at 28 and `growArray`, `reserve`, `resize` and `isNull` at 25, each
 * once per element or compared type, which is the ordinary shape of a library that grows - the
 * largest bench program, the lexer, peaks at 7. So 128 is four times the one real program's peak.
 * Not uncapped: the growth-chain bound (`growthPermitted`) ends a polymorphic recursion on its own,
 * but a generated program can still reach a generic at a combinatorial number of lists, and this is
 * what says so by name instead of by the size of the binary.
 *
 * **What was not done, and is the better answer where it applies.** Most of those bodies do not
 * depend on the element type beyond a pointer - `releaseRun` read two fields of a `Run(a)` whose
 * layout is the same at every `a`, and `isNull` is a pointer test - so their specializations are one
 * body under several names. Sharing them needs a proof that a variable is used only as the pointee
 * of `%` and never sized, stepped over or loaded through, over every node and every callee that
 * sees it; a wrong answer there is a pointer stepped by one byte instead of by an element, silently.
 * The library change was the safe half of that and this is the other; the proof waits for a slice
 * that can afford one.
 */
static constexpr U32 kSpecializationsPerGeneric = 128;

static bool specializationPermitted(Context& context, Program& program, DeclPtr<Function> callee,
                                    DeclPtr<Type>* args, Size count, LocationId source) {
    auto base = *program.decl;

    if(*program.internSpecialization(callee, args, count)) return true;

    auto& generic = *base[callee];

    /*
     * Declined, and reported rather than fallen back from - see the caller.
     *
     * The fallback is the erased form, and it is not available *here*: the form was decided before
     * this walk began, and an erased body is only buildable because `selectGenForms` checked that
     * one is. Revising the form at this point would name a body nothing had checked. What the
     * message does instead is name the attribute that decides it, which is an answer the author can
     * write and could not before G5.
     */
    if(generic.specializations >= kSpecializationsPerGeneric) {
        context.diagnostics.error(
            "%@ is specialized at more than %@ argument lists - it has to be compiled once, with `@generic`"_v,
            source, context.findName(generic.name), U64(kSpecializationsPerGeneric));
        return false;
    }

    return true;
}

/*
 * A call of one of an `openSite` record's generated functions, in a specialization of the body that
 * built the object - P16. The node names the open record's member; what runs is the same member of
 * the record minted at this body's arguments, which `substituteType` answers. Matched by kind, since
 * every one of them is a handle on the record or a member of its one `Iterate` instance.
 *
 * Left alone where the substitution does not close the record, which is the generic body itself.
 */
static void openSiteMember(Context& context, Program& program, Function& enclosing,
                           Function& named, LocationId source, CallSite& into) {
    auto base = *program.decl;
    auto concrete = substituteType(context, program, named.generatedRecord,
                                   substitutionOf(base, enclosing));
    if(concrete == named.generatedRecord || base[concrete]->kind != Type::Record) return;

    auto& record = *(RecordType*)base[concrete];
    DeclPtr<Function> target = nullptr;

    switch(named.generated) {
        case Function::Generated::IterNew: target = record.producerNew; break;
        case Function::Generated::IterStep: target = record.producerStep; break;
        case Function::Generated::IterWalk: target = record.producerWalk; break;
        case Function::Generated::IterItem: target = record.producerItem; break;
        case Function::Generated::IterTake: target = record.producerTake; break;
        case Function::Generated::IterFailure: target = record.producerFailure; break;
        case Function::Generated::IterDrop:
            target = teardownMember(context, program, intrinsic::Known::drop, concrete, source);
            break;
        case Function::Generated::IterNext:
        case Function::Generated::IterResume:
            for(auto instance: base[record.module]->instances.contents(base)) {
                auto& made = *base[instance];
                if(!made.args.size() || made.args.get(base, 0) != concrete) continue;

                for(auto member: made.members.contents(base)) {
                    if(member && base[member]->generated == named.generated) target = member;
                }
            }
            break;
        default:
            break;
    }

    if(!target) {
        context.diagnostics.error(
            "internal: %@ has no counterpart in the object this specialization builds"_v, source,
            context.findName(named.name));
        return;
    }

    into.target = target;
}

void callTarget(Context& context, Program& program, Function& enclosing, CallRequest node,
                CallSite& into) {
    auto base = *program.decl;
    auto mir = *program.mir;

    into.target = node.callee;
    into.through = CallThrough::Declaration;
    into.environment = nullptr;
    into.siteCall = false;

    /*
     * A call through a function **value** names no declaration and reaches nothing from here.
     *
     * What runs is whatever the pair the body is holding points at, and whoever *made* that pair is
     * what reached it - a `fun.value` node names its declaration and the reachability walk follows
     * that edge. Drawing one here would be drawing it from a node with no callee to name.
     *
     * Unless the value is a **site type**, which is the case that decision existed to leave open -
     * Design-Iterator-Objects.md §4. There the code *is* named, by the type, so this is an ordinary
     * direct call whose first argument the node holds separately; and the edge the paragraph above
     * says cannot be drawn is drawn here, which is what keeps a lambda nothing built a pair for
     * reachable. Answered under the enclosing substitution, because in a generic body the target's
     * type is that body's own variable and which of the two forms the call is, is the
     * specialization's answer rather than the node's.
     */
    if(node.callsValue) {
        auto target = node.targetType;

        if(target) {
            target = substituteType(context, program, target, substitutionOf(base, enclosing));
        }

        if(target && base[target]->kind == Type::Record) {
            auto& record = *(RecordType*)base[target];

            if(record.lambda && record.lambdaShape) {
                into.target = record.lambda;
                into.through = CallThrough::Declaration;
                into.siteCall = true;
                return;
            }
        }

        /*
         * A value the **`Call` constraint** typed, in a body compiled once - §4's remaining half.
         *
         * The type is still a variable here, because a substitution that closed it would have taken
         * one of the two arms above; what says how to call it is the constraint, and what a caller
         * filled for that constraint is one synthesized cell. So this is a witness dispatch with no
         * member list behind it: `callWitnessRequirement` finds which declared requirement the cell
         * belongs to, `fillClassWitness` puts the code in it, and the value travels as argument zero
         * (`CallThrough::CallWitness`).
         *
         * Below the site-type arm rather than beside it, because the two are the same question asked
         * of a closed type and an open one: a specialization takes the arm above with the code named
         * by the type, and the erased form takes this one with the code named by the witness.
         */
        if(enclosing.form == GenForm::Erased && target && base[target]->kind == Type::Variable) {
            U16 requirement = 0;
            if(callWitnessRequirement(program, bodyOwnerOf(base, enclosing).gen, target, requirement)) {
                into.through = CallThrough::CallWitness;
                into.requirement = requirement;
                into.member = 0;
                return;
            }
        }

        into.through = CallThrough::Value;
        return;
    }

    /*
     * Whether this body is a generic compiled **once** - `GenForm::Erased`, and the one thing about
     * the enclosing declaration this function reads besides its substitution.
     *
     * `form` is set on the generic and left `Auto` on a specialization, so the test is the whole
     * question: a specialization has an argument list and every answer below comes out of it, and an
     * erased body has none and reads them out of the environment it was handed.
     */
    auto erased = enclosing.form == GenForm::Erased;

    // A call the *requirement* typed rather than the callee's own signature - see above. Before the
    // type-argument test, because such a call has none: what it was written against is a promise
    // about the enclosing signature's variables, not an argument list of the callee's.
    if(node.genCallable != NodeCall::kNoCallable) {
        return callableTarget(context, program, enclosing, node, into);
    }

    // A plain call. The overwhelmingly common case, and it allocates nothing and asks nothing -
    // unless it names a member of an object a generic body built at its own variables (P16).
    if(!node.typeArguments->size()) {
        auto& named = *base[node.callee];
        if(named.generated != Function::Generated::None && named.generatedRecord &&
           base[named.generatedRecord]->kind == Type::Record &&
           ((RecordType*)base[named.generatedRecord])->openSite) {
            openSiteMember(context, program, enclosing, named, node.source, into);
        }

        return;
    }

    /*
     * The node's arguments are written in the *enclosing declaration's* terms, so they are read
     * under whatever substitution that declaration is being compiled with.
     *
     * In an ordinary body that substitution is empty and the arguments are already concrete. In a
     * generic body they mention the enclosing signature's variables, and it is the specialization of
     * the *caller* that decides them - which is exactly why the node could not have named a callee.
     */
    auto with = substitutionOf(base, enclosing);

    SmallArray<DeclPtr<Type>, 4> arguments;
    for(auto argument: node.typeArguments->contents(mir)) {
        arguments.push(substituteType(context, program, argument, with));
    }

    auto& callee = *base[node.callee];

    /*
     * A class member, which is selected rather than specialized - `Class`.
     *
     * The same pair asked the same way: the node named the class's *declaration* of the member and
     * bound the class's arguments, and this turns the two into a callee. That it is the same
     * function is the point - reachability, the summaries and the conversion each ask once, so no
     * pass can come to a different answer about which body a call reaches.
     *
     * In an erased body the arguments are the body's own variables, so there is nothing to select
     * against: what runs is whatever the caller put in the class requirement's witness, and this
     * finds which requirement that is. Above the concreteness test below because a member call is
     * the one that has an answer *without* a bound argument list, which is the whole of what a
     * witness is.
     */
    if(callee.klass && erased) {
        /*
         * The requirement whose witness answers it, where this signature declared one.
         *
         * Not found means the call binds concrete arguments - a member call at a named type inside an
         * erased body - and that is selected below exactly as it is in a specialization. Which of the
         * two a call is, is `erasableCall`'s question too, and the two ask it the same way: a body
         * that could not find a slot *and* names a variable was made a specialization instead, so
         * arriving here with neither is impossible rather than unusual.
         */
        U16 offset = 0;
        if(classRequirementSlot(context, program, enclosing.gen, callee.klass, arguments.pointer(),
                                arguments.size(), into.requirement, offset)) {
            into.through = CallThrough::ClassSlot;

            // The **flat** index into the requirement's witness, which is the member's own where the
            // requirement is the class and the superclass's range plus it where it is not. One
            // number and not a path, because the table is flattened - `classRequirementSlot`.
            into.member = U16(offset + callee.classMember);

            /*
             * Or the member's **"make an object"** entry, which is a slot of its own in the region
             * after the declared members - Design-Iterator-Objects.md §6.4, `classWitnessCount`.
             *
             * This is the whole of what a body compiled once does differently about an object of a
             * class member: the call is the ordinary class dispatch, and what it reaches is the cell
             * that makes one rather than the cell that is the producer - which has no signature,
             * because what a call to it produces is a record minted per instance.
             */
            if(callee.objectEntry) {
                into.member = U16(offset + base[callee.klass]->members.size() +
                                  classObjectIndex(base, callee.klass, callee.classMember));
            }

            return;
        }
    }

    /*
     * Arguments still open after substitution.
     *
     * In an ordinary walk this hands the **generic** back: nothing calls one - `markReachable` skips
     * one wherever it appears - so returning it ends the walk rather than naming a callee anything
     * will emit. That happens only below a diagnostic that has already stopped the compilation.
     *
     * In an erased body it is the forwarding `erasableCall` refuses, so a body that reaches here
     * with one was chosen to be compiled once over a call that says it cannot be.
     */
    for(auto argument: arguments) {
        if(isConcreteType(base, argument)) continue;
        if(!erased) return;

        context.diagnostics.error(
            "internal: a body chosen to be compiled once calls %@ at its own type parameters"_v,
            node.source, context.findName(callee.name));
        return;
    }

    if(callee.klass) {
        /*
         * The instance is there: `dischargeRequirements` proved it at the call site, against the
         * arguments this substitution produces. Missing here is therefore a compiler error rather
         * than a program one, and it is reported as one instead of lowering a call to nothing.
         */
        InstanceMatch found;
        if(!selectInstance(context, program, callee.klass, arguments.pointer(), arguments.size(),
                           node.source, false, found)) {
            context.diagnostics.error("internal: there is no instance of %@ at this call"_v,
                                      node.source,
                                      appliedClassText(context, base, callee.klass,
                                                       arguments.pointer(), arguments.size()));
            return;
        }

        auto reached = instanceMember(context, program, found, callee.classMember, node.source);

        /*
         * And the **object** of that member where this is the entry that makes one - §6.4, the same
         * cell a witness holds, reached directly because the instance is decided here.
         *
         * A body specialized at a concrete subject calls the instance's `f$obj` and gets the erased
         * pair, which is the same value the witness hands a body compiled once. That the two forms
         * answer one value is the point: a `@specialize` on the declaration is advice about code
         * size, and `iter/Witness.yana` asserts it changes no answer.
         */
        if(reached && callee.objectEntry) {
            /*
             * Made once and kept on the member (`Function::iterMaker`), because this is asked twice
             * from two tiers: the reachability walk reaches every call in a reachable body, and the
             * SSA conversion asks again - by which point `resolveProgram` has returned and there is
             * nothing left to make one with. The first ask is what the second reads.
             */
            if(base[reached]->iterMaker) {
                reached = base[reached]->iterMaker;
            } else if(program.iterObjects) {
                reached = program.iterObjects->objectMaker(
                    reached, DeclPtr<Function>(&callee - base), node.source);
            } else {
                context.diagnostics.error(
                    "internal: an object of %@ is needed and nothing can make one here"_v,
                    node.source, context.findName(base[reached]->name));
                return;
            }
        }

        into.target = reached;
        return;
    }

    /*
     * A callee compiled **once**, which is a direct call that hands over an environment as well as
     * its arguments - `GenEnvironment`.
     *
     * Before the two bounds below, because neither is about it: a body that exists once does not
     * grow with the argument lists that reach it, so neither the nesting bound nor the budget has
     * anything to decline. That is the whole of what the erased form buys, and it is why the budget
     * revising a form is the *only* order these three can be asked in.
     */
    if(callee.form == GenForm::Erased) {
        into.environment = environmentFor(context, program, node.callee, arguments.pointer(),
                                          arguments.size(), node.source);
        return;
    }

    /*
     * A call chain that specializes at ever larger types - `a`, then `Boxed(a)`, then
     * `Boxed(Boxed(a))` - is refused inside `specializeFunction`, by `growthPermitted`, because it
     * is a fact about the chain of discoveries rather than about this call: the class-member and
     * environment paths make specializations too, and a parametric instance whose member calls
     * itself at a deeper type diverges exactly as a plain generic does.
     *
     * What the program may still afford - see above. Last of the three reasons a call may decline to
     * specialize, after the arguments being open and the nesting being unbounded, because it is the
     * only one of them that is about the program rather than about this call: a budget refuses a
     * specialization that is perfectly well formed.
     *
     * Still a **diagnostic** now that the erased form exists, and the reason is the one the G6 note
     * already named: the form is decided before any specialization is made, and how many argument
     * lists a generic is reached at is discovered by this walk. Turning the decline into a fallback
     * would give one generic two compiled forms at once - the bodies already specialized, and an
     * erased one for the rest - and an erased body is only buildable if `selectGenForms` checked it
     * was, which it did not, because at that point this generic was not going to be one. So what
     * changes here is the message: `@generic` is now an answer the author can write.
     */
    if(!specializationPermitted(context, program, node.callee, arguments.pointer(),
                                arguments.size(), node.source)) {
        return;
    }

    into.target = specializeFunction(context, program, node.callee, arguments.pointer(),
                                     arguments.size(), node.source);
}

/*
 * Whether one node would have to read a generic environment - one instruction's half of the
 * measurement below.
 *
 * Four things a substitution supplies and an erased body has to load, in the order they cost: a
 * *named* requirement is one slot and one indirection, and a value **whose type is not concrete** is
 * every one of its size, its copy and its teardown coming out of a type descriptor. The last is the
 * majority and is what makes this a measurement rather than a count of the three written forms - a
 * body that names no requirement at all can still be almost entirely descriptor work.
 *
 * One answer per node rather than per operand, because what is being compared is a *density* over
 * the instructions: a node that touches the environment twice still lowers into one instruction's
 * worth of ordinary work, and counting the touches would make a wide node look like a hot loop.
 */
static bool readsEnvironment(DeclBase decl, MirBase mir, Body& body, Node& node) {
    if(node.kind == Node::GenValue) return true;

    if(node.kind == Node::Call) {
        auto& call = (NodeCall&)node;

        // A function slot - the call was typed against a promise rather than against a declaration.
        if(call.genCallable != NodeCall::kNoCallable) return true;

        // A callee whose arguments are still open in this body's own terms: a class member reached
        // through a witness, or a generic reached without one. Both are a slot and an indirect call.
        for(auto argument: call.typeArguments.contents(mir)) {
            if(!isConcreteType(decl, argument)) return true;
        }
    }

    if(hasResult(node.kind) && !isConcreteType(decl, valueType(node))) return true;

    auto found = false;

    visitNode(node, [&](auto& concrete) {
        concrete.eachOperand(mir, [&](Operand operand, OperandRole) {
            if(found || operand.source() != SourceKind::Place) return;

            /*
             * The whole projection chain, because a step can be concrete over one that is not:
             * `x.size` is an `I32` reached through a place whose type is a variable, so a test on
             * the leaf alone would read the one shape this feature exists for as costing nothing.
             */
            for(auto place = body.place(mir, PlaceId(operand.id())); place;
                place = place->parent ? mir[place->parent] : nullptr) {
                if(place->kind == Place::Property) { found = true; return; }
                if(!isConcreteType(decl, place->occurrence.type)) { found = true; return; }
            }
        });
    });

    return found;
}

/*
 * How much of one body would have to read an environment, out of how much body there is - `GenCost`,
 * which is where the answer is published and where the shape of it is argued.
 *
 * The **teardowns count**, and they are not nodes: `analyzeProgram` records them as an elaboration
 * beside the graph, and a drop of a value whose type is a variable is a descriptor load and an
 * indirect call - the single most expensive thing an erased body does. Measuring before they were
 * placed would have systematically read exactly the generics that own something as cheap to erase.
 */
static GenCost measureGenCost(Program& program, Body& body) {
    auto decl = *program.decl;
    auto mir = *program.mir;

    U32 reads = 0;
    U32 total = 0;

    for(auto nodePointer: body.nodes.contents(mir)) {
        total++;
        if(readsEnvironment(decl, mir, body, *mir[nodePointer])) reads++;
    }

    if(body.elaboration) {
        auto& elaboration = *mir[body.elaboration];

        for(auto step: elaboration.drops.contents(mir)) {
            total++;

            // Off the step's place rather than its path, because an overwrite of a global, a borrow
            // or an element has no path - see `mir::DropStep`.
            if(!isConcreteType(decl, body.place(mir, step.place)->occurrence.type)) reads++;
        }
    }

    return GenCost::measured(reads, total);
}

/*
 * Whether a value of this type has a representation an erased body can build - one type's half of
 * the check below.
 *
 * A **variable** does: a descriptor slot carries its size, its alignment and what runs when it dies,
 * which is everything the code needs and the reason the schema numbers one per variable. What does
 * *not* is a record with a field at a type parameter - `Boxed(a)` - because what a body does with a
 * record is project fields out of it, and a field's offset is a layout: the caller would have to
 * hand over a descriptor holding a table of offsets, which is a second layout algorithm beside the
 * one `fieldOffsetNative` already is.
 *
 * So the line is drawn where the *numbering* draws it - `GenSlotKind::Type` is one slot per declared
 * variable, and a descriptor answers a size, an alignment and a teardown and no question about
 * structure. A body that wants one named field of a subject it cannot see writes the requirement
 * down instead, and gets `GenSlotKind::Property`'s accessor pair rather than a layout.
 */
static bool erasableType(DeclBase base, DeclPtr<Type> type) {
    if(!type) return true;

    auto kind = base[type]->kind;

    // A type parameter is exactly what a descriptor slot is for, and a count is not a value.
    if(kind == Type::Variable || kind == Type::Const) return true;

    /*
     * A pointer is a word whatever it addresses, and a **function value** is two words whatever it
     * was made from - the uniformity `lower_fun.cpp` is built on, read here as a layout. What is
     * reached *through* either is an occurrence of its own and is checked where it appears, which is
     * also what stops this walk from following a record that holds a pointer to itself forever.
     */
    if(kind != Type::Record) return true;

    /*
     * A record over its **fields** rather than over its arguments, and the difference is a whole
     * feature: `Buf(n)` mentions a parameter and has a perfectly ordinary layout, because `n` is a
     * *count* and no field's type depends on it. What has no layout is a field whose type is a type
     * parameter - `Boxed(a)` - and that is what is looked for.
     *
     * The fields are the instantiation's own, already substituted by `instantiateRecord`, so this is
     * the layout question asked of the thing the layout would be computed from.
     */
    for(auto field: ((RecordType*)base[type])->fields.contents(base)) {
        auto held = field.occurrence.type;
        if(base[held]->kind == Type::Variable || !erasableType(base, held)) return false;
    }

    return true;
}

/*
 * Whether one call has a shape an erased body can make - the other half of the check below.
 *
 * Two it cannot, and both are limits of the *environment* rather than of the program: a class member
 * reached through a **superclass** of a requirement, whose slot holds the subclass's witness and has
 * no path to the superclass's members; and a generic called at this body's **own** type parameters,
 * which would need an environment built out of loads from this one rather than a table in the image.
 *
 * Asked here - where the form is chosen - and not only at `callTarget`, which is where the call
 * would have to be built. That was the arrangement this replaces and it had a hole with a diagnostic
 * in it: a body nobody wrote an attribute on could be *measured* erased, reach `callTarget`, and be
 * told the compiler does not implement it. An automatic choice must never produce a message, because
 * the specialized form is a correct answer to the same question - so the shapes are refused where
 * the choice is made, and a measured body falls back in silence.
 *
 * The arguments are read as written. In `callTarget` they are substituted first because that walk
 * reaches a body through a *specialization*; here the subject is the generic itself - a
 * specialization never reaches this function, see `selectGenForms` - so the substitution is empty
 * and the node's own arguments are the answer.
 *
 * Over a `CallRequest` rather than over a `NodeCall`, because a **lens call** is a call and asks the
 * same question: `callTarget` resolves both kinds through one request, so a shape declined here for
 * one of them and not the other is the hole this function exists to close - a body whose lens call
 * cannot be built erased, chosen erased, and told at the conversion that the compiler does not
 * implement it.
 */
static bool erasableCall(Context& context, Program& program, Function& function, Body& body,
                         Node& node, bool report) {
    auto decl = *program.decl;
    auto mir = *program.mir;

    auto request = isLensCall(node.kind) ? callRequestOf((NodeLensCall&)node)
                                         : callRequestOf((NodeCall&)node);

    // A function slot. The callee is a promise about this signature's own variables, which is the
    // one call an erased body makes without knowing anything about the callee at all.
    if(request.genCallable != NodeCall::kNoCallable) return true;

    /*
     * A call through a **function value**, whose signature has to be concrete - Analysis-IR.md's G7e.
     *
     * The *pair* is two words in every body, which is what made a function value erasable at all and
     * is why one may be held, copied and passed here with no descriptor. Calling one is a different
     * question: the code word was compiled against the signature the value was **made** at, so how
     * its result is handed back and how its arguments arrive were decided there, from concrete
     * manifestations. A body compiled once that saw the result as a type parameter would call it
     * with a hidden result pointer the callee has no parameter for.
     *
     * So `(I32) -> r` inside an erased body is refused and the call site specializes instead, which
     * is always available - `run(tried, 5)` compiles, it is just compiled per result type. That is
     * exactly the line `compiler/resolve` drew before J173 deleted it (`genericBodyLowerable`
     * declined "a call through a function value at a generic signature"), and lifting it needs the
     * callback ABI to state a result convention that both ends can read off the *value* rather than
     * off its type.
     */
    if(request.callsValue) {
        // A lens call never reaches here - a lens is reached by name, and `lensCallee` is what says
        // so - which is why the target below is read off the row that has one.
        auto target = ((NodeCall&)node).target;
        DeclPtr<Type> signature;

        if(target.source() == SourceKind::Result && target.id() < body.nodes.size()) {
            auto producer = body.node(mir, NodeId(target.id()));
            if(hasResult(producer->kind)) signature = valueOccurrence(*producer).type;
        } else if(target.source() == SourceKind::Place && target.id() < body.places.size()) {
            signature = body.place(mir, PlaceId(target.id()))->occurrence.type;
        }

        if(isConcreteType(decl, signature)) return true;

        /*
         * A call the **`Call` constraint** typed, which is the one shape an open signature does not
         * stop - Design-Iterator-Objects.md §4, built 2026-09-16.
         *
         * The paragraph above is about a signature nothing decided; here the shape *is* decided, and
         * `Call(f, (I32) -> I32)` states it in full - which is exactly what the erased ABI needs and
         * a bare `(I32) -> r` does not have. What was missing was the other half, the witness: a
         * zero-member class has an empty table, so there was no cell for the code. There is one now,
         * synthesized rather than declared - `CallThrough::CallWitness` - and it holds a site type's
         * lifted function or a shim over the pair, with the value's storage as argument zero.
         *
         * A requirement of *this* signature and not of some enclosing one, which is the whole of the
         * test: what a caller fills is a slot of this context's numbering, so a shape read off a
         * variable this declaration does not bind has no cell anywhere.
         */
        if(signature && decl[signature]->kind == Type::Variable) {
            U16 requirement = 0;
            if(callWitnessRequirement(program, body.gen, signature, requirement)) return true;
        }

        if(report) {
            context.diagnostics.error(
                "the compiler does not implement a generic body compiled once calling a %@ yet - the value's own signature says how its result is handed back, and this one is not decided here"_v,
                node.source, typeText(context, decl, signature));
        }

        return false;
    }

    // A plain call - the overwhelmingly common case, and one that binds nothing.
    if(!request.typeArguments->size()) return true;

    SmallArray<DeclPtr<Type>, 4> arguments;
    for(auto argument: request.typeArguments->contents(mir)) arguments.push(argument);

    auto& callee = *decl[request.callee];

    if(callee.klass) {
        U16 requirement = 0;
        U16 offset = 0;
        if(classRequirementSlot(context, program, function.gen, callee.klass, arguments.pointer(),
                                arguments.size(), requirement, offset)) {
            return true;
        }

        /*
         * Nothing this signature declared proves the class, directly or through a superclass.
         *
         * `dischargeRequirements` admitted the call, so what is left is a promise that came from
         * somewhere the environment does not carry - a concrete instance the substitution would have
         * found. That is a call at *this body's own* type parameters read one construct in, and it
         * falls to the same message the loop below gives.
         */
    }

    for(auto argument: arguments) {
        if(isConcreteType(decl, argument)) continue;

        if(report) {
            context.diagnostics.error(
                "the compiler does not implement a generic body compiled once calling %@ at its own type parameters yet"_v,
                node.source, context.findName(callee.name));
        }

        return false;
    }

    return true;
}

/*
 * Whether this body could be compiled once - the check a chosen erased form is held to.
 *
 * Every position a *representation* is asked for: the declared signature, the locals, the value each
 * node produces, the places it projects through and the paths the teardowns name. What it is looking
 * for is the one shape the environment cannot express - a type that mentions a variable and is not
 * one - and it reports the first, by name, at the position that wrote it. Every *call* as well, for
 * the two shapes `erasableCall` names.
 *
 * Run here rather than at the conversion because the *form* is what it qualifies: a body that cannot
 * be erased is not an error, it is a body that has to be specialized, and the only pass in a
 * position to say which is the one that chooses. What made this necessary rather than tidy is that
 * the failure downstream is a manifestation with no answer - a pointer with no size - which lowers
 * silently and copies the wrong number of bytes.
 *
 * `report` is whether an attribute asked for this form. A written `@generic` that cannot be honoured
 * is an error the author has to see; a measured one is a preference, and the specialized form is a
 * correct answer to the same question - so the fallback is silent and the walk is the same walk.
 */
static bool checkErasable(Context& context, Program& program, Function& function, Body& body,
                          bool report) {
    auto decl = *program.decl;
    auto mir = *program.mir;

    auto refuse = [&](DeclPtr<Type> type, LocationId source) {
        if(!report) return false;

        context.diagnostics.error(
            "%@ is compiled once, so every value in it is reached through a type descriptor - and %@ has a field at a type it does not know"_v,
            source == kNullLocation ? function.source : source,
            context.findName(function.name), typeText(context, decl, type));
        return false;
    };

    for(auto argumentPointer: function.args.contents(decl)) {
        auto& declared = *decl[argumentPointer];
        if(!erasableType(decl, declared.occurrence.type)) {
            return refuse(declared.occurrence.type, declared.source);
        }
    }

    if(!erasableType(decl, function.result.type)) return refuse(function.result.type, function.source);

    for(U32 i = 0; i < body.locals.size(); i++) {
        auto local = body.local(mir, LocalId(i));
        if(!erasableType(decl, local->occurrence.type)) {
            return refuse(local->occurrence.type, local->source);
        }
    }

    for(U32 i = 0; i < body.places.size(); i++) {
        auto place = body.place(mir, PlaceId(i));
        if(!erasableType(decl, place->occurrence.type)) {
            return refuse(place->occurrence.type, function.source);
        }
    }

    for(auto nodePointer: body.nodes.contents(mir)) {
        auto& node = *mir[nodePointer];
        auto calls = node.kind == Node::Call || isLensCall(node.kind);

        if(calls && !erasableCall(context, program, function, body, node, report)) return false;

        if(!hasResult(node.kind)) continue;
        if(!erasableType(decl, valueType(node))) return refuse(valueType(node), node.source);
    }

    /*
     * And no **continuation** that reads the environment - K8, J219's "names requirement 0 of
     * runsAgree.cont".
     *
     * A `for` over an `iter fn` or a lens call's block is outlined by the conversion into a function
     * of its own (`agree.cont`), and argument zero of that function is its captures record - the one
     * slot the generic environment would need as well, which is the lambda's refusal
     * (`resolve_lambda.cpp`) met again one tier down. So a class call, a generic call or a value at an
     * open type inside one has no environment to be read through, and the verifier caught the class
     * call naming a requirement of a signature that states none. Nothing refused it before because
     * the form is measured: a body specialized at one argument list is never erased, which is why
     * J219's one-instance reduction did not reproduce it and two instances did.
     *
     * Declining the form is always available and is the answer the lambda gets - the body is
     * specialized. Threading the environment into the continuation's record is the alternative, and
     * waits on the same change the lambda's does.
     */
    auto inContinuation = [&](ContinuationId id) {
        for(auto at = id; at.valid(); at = body.region(mir, at)->parent) {
            if(body.region(mir, at)->kind == Continuation::Handover) return true;
        }
        return false;
    };

    for(auto blockPointer: body.blocks.contents(mir)) {
        auto& block = *mir[blockPointer];
        if(!inContinuation(block.region)) continue;

        for(auto nodePointer: block.nodes.contents(mir)) {
            auto& node = *mir[nodePointer];
            auto open = hasResult(node.kind) && !isConcreteType(decl, valueType(node));

            if(!open && (node.kind == Node::Call || isLensCall(node.kind))) {
                auto request = isLensCall(node.kind) ? callRequestOf((NodeLensCall&)node)
                                                     : callRequestOf((NodeCall&)node);
                open = request.genCallable != NodeCall::kNoCallable;

                for(auto argument: request.typeArguments->contents(mir)) {
                    if(!isConcreteType(decl, argument)) open = true;
                }
            }

            if(!open) continue;

            if(report) {
                context.diagnostics.error(
                    "%@ is compiled once, and this is inside a `for` or lens block that is outlined into a function of its own, which cannot reach the generic environment yet - remove `@generic` so the function is specialized"_v,
                    node.source, context.findName(function.name));
            }

            return false;
        }
    }

    return true;
}

/*
 * The automatic answer - what a signature that wrote neither attribute gets.
 *
 * One eighth of a body, which is where the judgement is written down; `GenCost` is where the
 * measurement is argued and what makes this one comparison rather than a rule.
 *
 * Deliberately not weighted by how often a site runs. Loop depth is a property of the body and could
 * be had; call frequency is not, and is the term that would actually matter. Half a weighting reads
 * as a cost model and is not one, so what is measured is the honest share.
 *
 * Deliberately not weighted per *kind* of environment work either, although the kinds do not cost
 * the same - a drop of an open value is a descriptor load and an indirect call, a `gen.value` is one
 * load. One answer per operation is what makes this a share of the body rather than a count of
 * touches, and a node that reads the environment twice still lowers into one node's worth of
 * ordinary work around it.
 *
 * Nothing about the *number of instantiations* is here, and cannot be: it is discovered by the
 * specializing walk, which is downstream of this decision. That is one of the two terms a later pass
 * could add, by deciding the form after a first specialization round rather than before it; the
 * other is the interprocedural one `GenCost::join` describes.
 */
static constexpr U8 kErasureShare = GenCost::kWhole / 8;

static bool erasureFits(GenCost cost) {
    return cost.share <= kErasureShare;
}

/*
 * The third term - J145, J94 §6.2 G: a body too small to be worth erasing.
 *
 * An erased call hands over an environment address beside the arguments and names a body the
 * inliner may not copy (`inline.cpp`: its slots are numbered against its own schema), so a body no
 * larger than that call's own setup costs more erased than copied - once per call site, where a
 * specialized copy is inlined away. `Structural.yana`'s `copyable(x: a, n: Int) = n` is the shape:
 * its environment is read by nothing, so `erasureFits` said yes, and every site then paid a call and
 * a placed environment for a function that returns its second argument.
 *
 * Counted in MIR nodes and elaboration drops, the same units `measureGenCost` counts `total` in. Four
 * is a read, a return and room for one operation between them. An `exported` generic stays erased
 * whatever its size, for the reason `erasureSavesCopies` gives.
 *
 * **And only where the specializing walk will not refuse it** - J148 (`reachedUnderCap`). A generic
 * the form decision forces to `Specialized` is held to `kSpecializationsPerGeneric` like any other,
 * and that refusal is an error rather than a fallback (see `specializationPermitted`), so the floor
 * as J145 built it turned `second(x: a, n: I32) = n` called at 40 record types - erased and correct
 * before - into "specialized at more than 32 argument lists" (`generic/ErasureFloorCap.yana`, at
 * 130 lists since K1 raised the cap to 128). Exempting a floor-chosen copy from the cap was the other
 * way to it and was not taken: when J148 wrote this the cap was the only thing that ended a
 * polymorphic recursion, and a four-node body can be one (`fn (a) f(x: a) = f({x})`). The growth
 * bound (`growthPermitted`) ends that one now, but a copy per list is still a copy per list.
 */
static constexpr U32 kErasureFloor = 4;

static bool erasureTooSmall(Program& program, Body& body) {
    auto mir = *program.mir;

    auto total = Size(body.nodes.size());
    if(body.elaboration) total += mir[body.elaboration]->drops.size();
    return total <= kErasureFloor;
}

/*
 * Whether this declaration is reached by **selection** rather than by a call naming it.
 *
 * A class member and an instance member are both: what runs is chosen from the instance table, and
 * `instanceMember` makes a specialization of it at the bindings the match produced. So the form of
 * one is not a free choice - an erased body reached by selection would be a body the selection had
 * no argument list to make, and the witness table that names it wants a concrete callee behind a
 * thunk. They are compiled per argument list, and an attribute asking otherwise is reported.
 */
static bool selectedMember(DeclBase base, Program& program, DeclPtr<Function> pointer) {
    if(base[pointer]->klass) return true;

    for(auto modulePointer: program.modules) {
        for(auto instancePointer: base[modulePointer]->instances.contents(base)) {
            for(auto member: base[instancePointer]->members.contents(base)) {
                if(member == pointer) return true;
            }
        }
    }

    return false;
}

/*
 * How many **distinct concrete argument lists** name each generic, counted before the walk that
 * would discover them - the one term `kErasureShare`'s note says this decision cannot have.
 *
 * It cannot have the exact number, and this is not it. What the specializing walk answers is how
 * many lists the *reachable* program calls at, and that walk runs after every form is settled. What
 * this answers is a **syntactic upper-bounded lower bound**: every call node in every body, whether
 * the code around it is reachable or not, with anything the count cannot read written down as
 * `many`. So a generic this says is reached at one list is reached at one list or at none, and a
 * generic it says nothing about is left to the measurement alone.
 *
 * That one direction is the whole of what it is for. Erasure trades an indirect ABI and a body full
 * of descriptor work against **the copies it saves**, and at one argument list it saves none: the
 * specialized body is the same body with its types filled in, it replaces the erased one entirely,
 * and it is a body the SSA inliner may clone - `opt/inline.cpp`'s `clonableDeclaration` refuses an
 * erased generic outright, because a copy of one would read a slot of an environment the copy cannot
 * bring with it. `test/resolve/WideCall.yana` is the shape: `wide` binds two variables, uses neither
 * for anything but passing, is called twice at `[Bool, {}]` and nowhere else, and cost the caller a
 * thirty-four argument call where the specialized body folds to a constant.
 *
 * Four things are `many` rather than a list, and each is a way a body is reached that names no
 * arguments here:
 *
 *  - a **function value** of the generic, which names the declaration and hands it to whoever calls
 *    the pair. There is no list at the site and the callee is whatever the value points at;
 *  - a call the **requirement** typed - `NodeCall::genCallable` - which `callableTarget` resolves
 *    against the environment the enclosing body was handed rather than against an argument list;
 *  - a call whose written arguments are **not concrete**, which is every call inside another
 *    generic's body that passes its own variables along. The list such a call reaches is one the
 *    enclosing substitution decides, and there is one per specialization of the enclosing body;
 *  - a **second, different** list, which is the case the count is looking for the absence of.
 *
 * A generic reached at **no** list at all is left alone rather than specialized, and the reason is
 * that "no call names it here" is not "nothing calls it": a `pub` generic in a `-mode lib` build is
 * called by a compilation that has not happened yet, and the erased form is the one a caller outside
 * this program can reach. `exported` is checked for the same reason one step earlier.
 */
struct GenericReach {
    /*
     * The distinct lists seen so far, as the written lists themselves: a written argument list is
     * short and lives in the MIR arena, which nothing rewrites before the specializing walk, and
     * the arena pointers in it are interned, so `DeclPtr<Type>` equality is the identity test -
     * `internSpecialization` re-checks a hashed key against exactly these offsets.
     *
     * Up to one past `kSpecializationsPerGeneric` - J148. The first two answer `erasureSavesCopies`
     * (one list or several); the rest are what lets `erasureTooSmall` specialize only a generic the
     * specializing walk cannot then refuse for being reached at too many.
     *
     * Held as a chain through `ArgumentLists::seen` rather than as a row here - K1. The row was a
     * fixed array of `kMaxLists` pointers, because this lives in a `HashMap` whose slots are raw
     * storage the caller fills and a member owning a buffer would be one nothing here destroys; that
     * was 264 bytes a generic at a cap of 32 and would be 1 KiB at 128, in every slot of a table
     * holding every generic any call names, for a count that almost always stops at one or two.
     */
    static constexpr Size kMaxVariables = 8;
    static constexpr Size kMaxLists = kSpecializationsPerGeneric + 1;
    static constexpr U32 kNoList = ~U32(0);

    U32 first = kNoList;
    U32 distinct = 0;

    // Reached in a way that names no list here - the four `many`s above but the second list. A
    // context binding more variables than `kMaxVariables` is read the same way.
    bool unreadable = false;

    bool seen() const { return distinct > 0; }
    bool many() const { return unreadable || distinct > 1; }

    // Whether every specialization the walk can make of it fits under the cap.
    bool underCap() const { return !unreadable && distinct <= kSpecializationsPerGeneric; }
};

// One list a generic was seen at, and the next one the same generic was - a chain per generic
// through one array for the whole count. A plain `Array` rather than a `SmallArray`: there is one of
// these per `selectGenForms` and it is pointed into by index from every row of the table.
struct SeenList {
    MirList<DeclPtr<Type>>* list;
    U32 next;
};

struct ArgumentLists {
    HashMap<U32, GenericReach> reach;
    Array<SeenList> seen;
};

/*
 * One call's contribution, and the only thing that reads a node here.
 *
 * The arguments are taken **as written** and never substituted. A concrete type is its own image
 * under every substitution, so a list this accepts is the list the call reaches whichever body it is
 * read inside; and a list holding a variable is refused rather than resolved, which is the third
 * `many` above. That is what keeps this a walk over nodes rather than a second specializer.
 */
// The row for one generic, made on first sight. `HashMap::add` hands back raw storage, so a fresh
// slot is filled here rather than left holding whatever the bucket did - `internSpecialization` says
// the same thing about its own.
static GenericReach& reachFor(ArgumentLists& lists, DeclPtr<Function> callee) {
    auto entry = lists.reach.add(callee.offset);
    if(!entry.existed) *entry.value = GenericReach {};

    return *entry.value;
}

static void noteArgumentList(DeclBase base, ArgumentLists& lists,
                             DeclPtr<Function> callee, MirList<DeclPtr<Type>>* arguments,
                             MirBase mir) {
    auto& reach = reachFor(lists, callee);
    if(reach.unreadable || reach.distinct >= GenericReach::kMaxLists) return;

    if(!arguments || !arguments->size() ||
       arguments->size() > GenericReach::kMaxVariables) {
        reach.unreadable = true;
        return;
    }

    auto written = arguments->contents(mir);

    for(auto type: written) {
        if(!isConcreteType(base, type)) { reach.unreadable = true; return; }
    }

    for(auto known = reach.first; known != GenericReach::kNoList; known = lists.seen[known].next) {
        auto seen = lists.seen[known].list->contents(mir);
        if(seen.size() != written.size()) continue;

        auto same = true;
        for(Size i = 0; i < written.size() && same; i++) same = seen[i] == written[i];
        if(same) return;
    }

    // `reach` is a slot of the table and `lists.seen` a different container, so pushing here moves
    // nothing `reach` points into.
    lists.seen.push(SeenList { arguments, reach.first });
    reach.first = U32(lists.seen.size() - 1);
    reach.distinct++;
}

static void markMany(ArgumentLists& lists, DeclPtr<Function> callee) {
    reachFor(lists, callee).unreadable = true;
}

// Every body in the program, once. `module->functions` is the whole list - `verifyProgram` walks the
// same one and it is what a specialization is later filed into - and at this point it holds only
// what was written, since nothing has specialized anything yet.
static void countArgumentLists(Program& program, ArgumentLists& lists) {
    auto base = *program.decl;
    auto mir = *program.mir;

    for(auto modulePointer: program.modules) {
        for(auto functionPointer: base[modulePointer]->functions.contents(base)) {
            auto& function = *base[functionPointer];
            if(!function.mir) continue;

            auto& body = *mir[function.mir];

            for(U32 i = 0; i < body.nodes.size(); i++) {
                auto node = body.node(mir, NodeId(i));

                if(node->kind == Node::FunValue) {
                    auto callee = ((NodeFunValue*)node)->callee;
                    if(callee && isGeneric(base, base[callee]->gen)) markMany(lists, callee);
                    continue;
                }

                CallRequest request;
                if(node->kind == Node::Call) request = callRequestOf(*(NodeCall*)node);
                else if(isLensCall(node->kind)) request = callRequestOf(*(NodeLensCall*)node);
                else continue;

                // A call through a value names no declaration at all, so there is nothing here to
                // count against - what it runs was reached by whoever built the pair, above.
                if(request.callsValue || !request.callee) continue;
                if(!isGeneric(base, base[request.callee]->gen)) continue;

                if(request.genCallable != NodeCall::kNoCallable) {
                    markMany(lists, request.callee);
                    continue;
                }

                noteArgumentList(base, lists, request.callee, request.typeArguments, mir);
            }
        }
    }
}

/*
 * Whether erasure would still be **saving** anything - the count above, read at the one decision.
 *
 * True is "leave the measurement alone", so everything this does not know about answers true: a
 * generic with no entry is one no call node named, which is the library case the struct's note
 * argues, and `many` is the ordinary generic erasure exists for.
 */
// Whether every argument list the generic can be specialized at is one written here, and there are
// no more of them than the cap - so `erasureTooSmall` may choose the specialized form (J148). No
// entry is no call node, and so no specialization at all.
static bool reachedUnderCap(ArgumentLists& lists, DeclPtr<Function> pointer) {
    auto reach = lists.reach.get(pointer.offset);
    return reach.isNothing() || reach.unwrap().underCap();
}

static bool erasureSavesCopies(ArgumentLists& lists, DeclPtr<Function> pointer) {
    auto reach = lists.reach.get(pointer.offset);
    if(reach.isNothing()) return true;

    auto& counted = reach.unwrap();
    return counted.many() || !counted.seen();
}

void selectGenForms(Context& context, Program& program) {
    auto decl = *program.decl;
    auto mir = *program.mir;

    // Counted first and read below, because the answer is about the *program* and the decision is
    // made one declaration at a time - the same reason this pass exists rather than the form being
    // derived where it is wanted.
    ArgumentLists argumentLists;
    countArgumentLists(program, argumentLists);

    for(auto modulePointer: program.modules) {
        for(auto functionPointer: decl[modulePointer]->functions.contents(decl)) {
            auto& function = *decl[functionPointer];

            /*
             * A specialization shares the generic's body, and the generic holds the decision - see
             * `Function::form`. Skipped rather than answered, so one body has one form.
             *
             * Its cost is nonetheless settled and is zero: what a specialization *is* is that body
             * read under an argument list, so every read the measurement counts has an answer in the
             * substitution and none of them survives into code. Stated rather than measured, for the
             * same reason a plain function's is below.
             */
            if(function.specializes) {
                function.summary.setGenCost(GenCost { 0 }, Knowledge::Known);
                continue;
            }

            if(!isGeneric(decl, function.gen)) {
                if(function.form != GenForm::Auto) {
                    context.diagnostics.error(
                        "choosing how a generic body is compiled says nothing about %@, which binds no type parameters"_v,
                        function.source, context.findName(function.name));
                }

                // Not left `Auto`: a declaration with no environment is compiled one way, and a
                // reader asking a plain function its form should get the answer rather than "the
                // question does not apply" and have to know which. Its cost is zero for the reason
                // it has one form - there is no environment to do work on - and saying so costs a
                // store where measuring it would be a walk over every body in the program.
                function.form = GenForm::Specialized;
                function.summary.setGenCost(GenCost { 0 }, Knowledge::Known);
                continue;
            }

            /*
             * The measurement, published before anything reads it - `Summary::genCost`.
             *
             * Every generic with a body, and not only the ones whose form is still open: it is a
             * fact about the body rather than a step of the decision below. A `@specialize` that
             * overrode a high cost, and a class member whose form was never a choice, are exactly
             * the cases a later reader - the inliner, a report on why a program is the size it is -
             * wants the number for. A generic with no body publishes nothing, which is what
             * `Knowledge::Unknown` is for.
             */
            if(function.mir) {
                function.summary.setGenCost(measureGenCost(program, *mir[function.mir]),
                                            Knowledge::Known);
            }

            /*
             * The four kinds of generic whose form is not a choice, each for its own reason.
             *
             * A **selected member** is reached from the instance table and specialized there. A
             * **teardown** is called by the compiler at a fixed signature, which is the one place a
             * body's ABI is not the caller's to decide. A **lens** has no standalone body at all -
             * it is resolved into its caller - so there is nothing to compile once. A **lifted
             * lambda** of a generic body (C3) holds its captures in argument zero, which is where an
             * erased body's environment would have to go.
             *
             * Answered silently, and there is nothing to report: neither attribute has a placement
             * inside a `class` or an `instance` - the parser refuses one there - and a teardown and
             * a lens are refused with a generic context by name. So no author can have asked for the
             * form these are given, and a message would be about a measurement nobody ran.
             */
            if(selectedMember(decl, program, functionPointer) || function.teardown ||
               function.lens || function.environment || !function.mir) {
                function.form = GenForm::Specialized;
                continue;
            }

            auto written = function.form != GenForm::Auto;

            /*
             * The measured answer, the second term J52 gave it - see `GenericReach` - and the third
             * J145 gave it, `erasureTooSmall`.
             *
             * All three have to say yes. `erasureFits` is about what the erased body would *cost*, and
             * `erasureSavesCopies` is about what it would *save*: a generic this compilation reaches
             * at exactly one argument list is compiled once either way, so erasing it buys nothing
             * and gives up a concrete ABI, a body the folder can see through and a body the inliner
             * may clone.
             *
             * `exported` is left erased whatever the count says, for the reason the struct's note
             * gives: a caller outside this compilation names no argument list here, and the form a
             * `pub` generic is compiled in is the one that caller has to be able to reach.
             */
            if(!written) {
                auto fits = erasureFits(function.summary.genCost().value);
                auto saves = function.exported ||
                             erasureSavesCopies(argumentLists, functionPointer);
                auto worth = function.exported || !erasureTooSmall(program, *mir[function.mir]) ||
                             !reachedUnderCap(argumentLists, functionPointer);

                function.form = fits && saves && worth ? GenForm::Erased : GenForm::Specialized;
            }

            /*
             * A chosen erased form, held to the one thing an environment cannot express.
             *
             * The measurement above is about *cost* and this is about *possibility*, and they are
             * asked in that order for the reason the budget is asked after both: a body that has to
             * be specialized is specialized whatever it would have cost, and a body nobody could
             * erase is not a body whose density is worth a message.
             *
             * A written `@generic` that cannot be honoured is an error; a measured one falls back,
             * because nothing asked for it and the specialized form is a correct answer to the same
             * question.
             */
            if(function.form != GenForm::Erased) continue;

            if(!checkErasable(context, program, function, *mir[function.mir], written)) {
                function.form = GenForm::Specialized;
                continue;
            }
        }
    }
}

} // namespace mir
