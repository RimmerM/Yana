#include "specialize.h"
#include "own.h"
#include "select.h"
#include "print.h"

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
    function->teardown = written.teardown;
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
 * How deeply a type argument is nested - the one thing that can make specialization not terminate.
 *
 * A call chain that specializes at `a`, then at `Boxed(a)`, then at `Boxed(Boxed(a))` is polymorphic
 * recursion, and it has no fixed set of instantiations: each round is a strictly larger type, so a
 * worklist over argument lists never runs out. Nothing about the program is infinite - it is
 * specialization that cannot represent it - which is why this is a diagnostic about the choice of
 * lowering rather than about the program, and why the erased ABI is the answer that compiles it.
 *
 * A depth rather than a count of specializations, because the count is what a legitimately
 * generic-heavy program has a lot of and the depth is what only a diverging one grows.
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
    return specializeFunction(context, program, declared,
                              (DeclPtr<Type>*)found.arguments.pointer(), found.arguments.size(),
                              source);
}

DeclPtr<Function> teardownMember(Context& context, Program& program, CoreClass klass,
                                 DeclPtr<Type> type, LocationId source) {
    InstanceMatch found;
    if(!declaredInstanceFor(context, program, klass, type, &found)) return nullptr;

    return instanceMember(context, program, found, 0, source);
}

DeclPtr<Function> callTarget(Context& context, Program& program, Function& enclosing,
                             NodeCall& node) {
    auto base = *program.decl;
    auto mir = *program.mir;

    // A plain call. The overwhelmingly common case, and it allocates nothing and asks nothing.
    if(!node.typeArguments.size()) return node.callee;

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
    for(auto argument: node.typeArguments.contents(mir)) {
        arguments.push(substituteType(context, program, argument, with));
    }

    /*
     * Arguments still open after substitution, which hands the **generic** back.
     *
     * Nothing calls a generic - `markReachable` skips one wherever it appears - so returning it is
     * what ends the walk rather than a callee anything will emit. This is a *generic* body being
     * walked rather than one of its specializations, which happens only below a diagnostic that has
     * already stopped the compilation and left the generic reachable; a specialization is what a
     * bound signature is, so there is nothing to make here.
     */
    for(auto argument: arguments) {
        if(!isConcreteType(base, argument)) return node.callee;
    }

    /*
     * A class member, which is selected rather than specialized - `Class`.
     *
     * The same pair asked the same way: the node named the class's *declaration* of the member and
     * bound the class's arguments, and this turns the two into a callee. That it is the same
     * function is the point - reachability, the summaries and the conversion each ask once, so no
     * pass can come to a different answer about which body a call reaches.
     *
     * The instance is there: `dischargeRequirements` proved it at the call site, against the
     * arguments this substitution produces. Missing here is therefore a compiler error rather than a
     * program one, and it is reported as one instead of lowering a call to nothing.
     */
    auto& callee = *base[node.callee];
    if(callee.klass) {
        InstanceMatch found;
        if(!selectInstance(context, program, callee.klass, arguments.pointer(), arguments.size(),
                           node.source, false, found)) {
            context.diagnostics.error("internal: there is no instance of %@ at this call"_v,
                                      node.source,
                                      appliedClassText(context, base, callee.klass,
                                                       arguments.pointer(), arguments.size()));
            return node.callee;
        }

        return instanceMember(context, program, found, callee.classMember, node.source);
    }

    /*
     * A call chain that specializes at ever larger types - `a`, then `Boxed(a)`, then
     * `Boxed(Boxed(a))`. It has no fixed set of instantiations, so a worklist over argument lists
     * never runs out. Nothing about the program is infinite; it is specialization that cannot
     * represent it, which is why the message is about the lowering and why the erased ABI is the
     * answer that compiles it. It hands the generic back for the reason above.
     *
     * Below the class case rather than above it, because selection does not have the problem: the
     * instance table is finite, so a member - a default included - is chosen from a fixed set
     * however deeply the types nest.
     */
    static constexpr U32 kSpecializationDepth = 16;

    for(auto argument: arguments) {
        if(typeDepth(base, argument) <= kSpecializationDepth) continue;

        context.diagnostics.error(
            "%@ is called at a type built from its own, so specializing it does not terminate"_v,
            node.source, context.findName(base[node.callee]->name));
        return node.callee;
    }

    return specializeFunction(context, program, node.callee, arguments.pointer(), arguments.size(),
                              node.source);
}

} // namespace mir
