#pragma once

#include "module.h"

/*
 * Name resolution.
 *
 * Every lookup of a module-level name goes through search() below - types, constructors,
 * functions, class functions, classes, operator fixity and instances alike. There is one
 * traversal, so there is one answer to each of the questions the traversal decides:
 *
 *  - what a qualified name means (`Core.Maybe`, `Maybe.Just`, `Hmac.sha1`, `X.f` through an
 *    import alias);
 *  - which module a name is looked for in, and in what order;
 *  - what an import's include/exclude lists hide;
 *  - and what happens when two imports offer the same name.
 *
 * Getting those four answers in one place is the point: they are the rules that turn into
 * silent, hard-to-find inconsistencies when each kind of name grows its own copy.
 *
 * A name is a sequence of segments (`A.B.f` is three), and so is a *declaration's* name - a
 * declaration may be written under a namespace, which makes `Hmac.sha1` one name of two segments
 * rather than two things. Resolution walks, in order:
 *
 *  1. the module itself, matching the whole name against its own symbols, and - for a
 *     two-or-more segment name - the last segment against a symbol namespaced under the
 *     preceding one (a qualified record's constructors, `Maybe.Just`);
 *  2. every import that can be used unqualified, matching the whole name the same way;
 *  3. every import whose local name is a prefix of the name, matching the rest.
 *
 * Step 1 wins outright: a local definition shadows an import, which is what stops a new symbol
 * appearing in Core from silently capturing a name a user module already defines. Steps 2 and 3
 * have equal weight, so two hits there are ambiguous and reported as such rather than resolved
 * by import order - *between* imports. Within one they are ordered, and only in the one case where
 * both can apply: see search()'s fallback for an import whose own name is also a namespace of its
 * declarations.
 *
 * A namespace is a name and nothing else, which is why nothing here has a table of them. What a
 * prefix means - a module, a record, a class, a type's method namespace, or a namespace no
 * declaration but this one uses - is decided by which of the lookups answers, exactly as it always
 * was. registerNamespace holds the one rule that is not a lookup.
 */

/*
 * Whether one symbol is exported from the module that declared it - the `pub` half of visibility.
 *
 * One overload per thing search() can find, because `pub` is a property of the *declaration* and
 * each kind of declaration produced a different structure. They are here rather than inside each
 * typed wrapper for the reason the file comment gives: which modules a name is searched in, what an
 * import hides and what a declaration is willing to be named from elsewhere are the traversal's
 * business, and the wrappers below contribute only what is specific to their kind of symbol.
 *
 * `in` is the module the symbol was found in, which is what makes the arena and the type region
 * available - a ModulePtr is an offset and means nothing without one.
 *
 * Two things have no overload because they are never `pub` and are never hidden. An instance is
 * global, and doc/spec/modules.md says so; a fixity applies wherever its operator is in scope, so
 * OperatorFixity's is the one that answers true unconditionally.
 */
inline bool exportedSymbol(Module& in, TypePtr type) { return (*in.types)[type]->exported; }
inline bool exportedSymbol(Module& in, TypeAlias* alias) { return alias->exported; }
inline bool exportedSymbol(Module& in, GlobalPtr<TypeClass> typeClass) { return (*in.types)[typeClass]->exported; }
inline bool exportedSymbol(Module& in, ModulePtr<Function> function) { return (*in.arena)[function]->exported; }
inline bool exportedSymbol(Module& in, ModulePtr<Global> global_) { return (*in.arena)[global_]->exported; }
inline bool exportedSymbol(Module& in, OperatorFixity) { return true; }

// A constructor is exported with its record and never on its own: there is no way to export a type
// without its constructors, which doc/spec/modules.md records as an open question. `Maybe.Just` and
// a bare `Just` are therefore the same answer, which is what stops a qualified spelling being a way
// around the rule.
inline bool exportedSymbol(Module& in, ConstructorRef constructor) {
    auto global = *in.types;
    return constructor.record && ((Type*)global[constructor.record])->exported;
}

// One candidate found by the traversal, before ambiguity is decided.
template<class T>
struct Found {
    T value {};
    Module* module = nullptr;
    bool found = false;

    explicit operator bool() const { return found; }
    T operator * () const { return value; }
};

// How much of a name a lookup consumed, so the traversal can tell `Maybe` from `Maybe.Just`.
struct NameRef {
    Identifier* identifier = nullptr;

    // The first segment that belongs to the symbol rather than to a module path.
    U32 start = 0;

    U32 segments() const { return identifier->segmentCount - start; }
    StringId segment(U32 index) const { return identifier->getHash(start + index); }

    // The leading segment of the symbol, which is the name an import's include and exclude lists
    // name: `hiding (Maybe)` hides `Maybe.Just`, and `hiding (Hmac)` hides all of that namespace.
    StringId single() const { return identifier->getHash(start); }

    /*
     * A run of the remaining segments as one id.
     *
     * A declaration's name may itself be several segments - `Hmac.sha1`, `String.reserve` - so a
     * lookup has to be able to ask for the whole of what is left rather than only for one segment
     * of it. `rest()` is that, and `range()` is what a symbol namespaced under another name needs:
     * `Maybe.Just` is a constructor whose record is `range(0, 1)` and whose own name is the last
     * segment.
     *
     * Hashed out of the text rather than interned, and that is the point of doing it this way:
     * `addIdentifier` keys a multi-segment name by the hash of its whole text and a single-segment
     * one by the hash of its only segment, which for one segment are the same bytes. So this
     * answers with the id the declaration was interned under while allocating nothing - and a name
     * lookup is the hottest path in the resolver, asked once per name and once per import of it.
     */
    StringId range(U32 from, U32 count) const {
        auto first = start + from;
        auto end = first + count;

        // One segment's id is stored already, and this is where the hot path goes: a single-segment
        // name is almost every lookup there is, and hashing its text again would put a pass over the
        // name in front of every one of them.
        if(count == 1) return identifier->getHash(first);

        // getSegmentOffset() reads `segments`, which a single-segment identifier does not have -
        // and the two edges are the whole of what it would be asked for here anyway.
        auto begin = first == 0 ? 0 : identifier->getSegmentOffset(first);

        // One before the next segment, which is the separator this run does not include.
        auto stop = end >= identifier->segmentCount ? identifier->textLength
                                                    : identifier->getSegmentOffset(end) - 1;

        return Context::nameHash(StringView { identifier->text + begin, stop - begin });
    }

    StringId rest() const { return range(0, segments()); }
};

/*
 * Runs `find` over the module and its visible imports, and reports ambiguity.
 *
 * `find` is called as find(Module&, NameRef) and returns a falsy T for "not here". It is
 * responsible only for matching a name against one module's own symbols; which modules it sees,
 * in what order, and what is done about two answers are this function's business.
 */
template<class T, class Find>
Found<T> search(Context& context, Module& module, StringId name, LocationId source, Find&& find,
                bool report = true) {
    auto identifier = &context.find(name);
    Found<T> result;

    if(auto value = find(module, NameRef { identifier, 0 })) {
        return Found<T> { value, &module, true };
    }

    for(auto& import: module.imports) {
        // Only the imports the file being read wrote, plus the implicit ones every file has -
        // Analysis-Modules.md §2.1.2, and Module::activeFile for how the file is known here.
        if(!import.inScope(module.activeFile)) continue;

        NameRef reference { identifier, 0 };

        // A qualified name may address an import through its local name. The local name is
        // itself possibly multi-segment (`import A.B as X` vs `import A.B`), so this consumes
        // as many segments as the alias has.
        auto alias = &context.find(import.localName);
        auto matchedAlias = identifier->segmentCount > alias->segmentCount;

        if(matchedAlias) {
            for(U32 i = 0; i < alias->segmentCount; i++) {
                if(alias->getHash(i) != identifier->getHash(i)) {
                    matchedAlias = false;
                    break;
                }
            }
        }

        if(matchedAlias) {
            reference.start = alias->segmentCount;
        } else if(import.qualified) {
            // A qualified import contributes nothing to unqualified lookup.
            continue;
        }

        auto value = find(*import.module, reference);

        /*
         * The alias answered nothing, so the whole name is tried against the module as well.
         *
         * This is the case where the two steps meet: an import whose local name is also the
         * namespace of one of its own declarations. `import File` where module `File` declares
         * `File.open` offers that name both ways - `File` consumed as the module leaving `open`,
         * and `File.open` matched whole - and consuming the alias used to be *instead of* rather
         * than *before*, which made such a namespace unreachable rather than ambiguous.
         *
         * Before rather than beside, so nothing has to compare two values of an arbitrary T: an
         * author writing `File.open` means the module's `open` where there is one, and the module
         * decided to declare both if there are two. A `qualified` import still contributes only its
         * qualified spelling, which the alias match is.
         */
        if(!value && matchedAlias && !import.qualified) {
            reference.start = 0;
            value = find(*import.module, reference);
        }

        if(!value) continue;

        // Visibility is checked on the leading segment of the symbol, which is the name the
        // import list names: `hiding (Maybe)` hides `Maybe.Just` along with the type, while
        // hiding a single constructor of a visible type is not something an import list can say.
        if(!Module::visible(import, reference.single())) continue;

        /*
         * And the other half of visibility, which is the declaring module's rather than the
         * importing one's: an unmarked declaration is private and this is not a hit at all.
         *
         * Not a report. A private name is *not visible*, so a lookup that reaches one goes on to
         * the next import and ends up saying the name is not in scope - which is the same answer,
         * and the same diagnostic, as for a name nothing declares anywhere. Reporting "this exists
         * but you may not have it" would be an interface a private declaration does not have, and
         * it would make adding one to a module a breaking change for its importers.
         */
        if(!exportedSymbol(*import.module, value)) continue;

        if(result.found && result.module != import.module) {
            /*
             * `report` is false where the caller is asking whether a name exists at all rather than
             * deciding a call - see namedCallee, which probes a dot-call's name before it will
             * resolve the receiver. An ambiguity is not an answer to that question: if two are
             * visible and either is the kind being looked for, the answer is yes, and reporting here
             * would produce the diagnostic *before* the receiver has had its say about which one is
             * meant. That is the bug the dot-call precedence fix removed, and probing put it back.
             */
            if(report) {
                context.diagnostics.error("ambiguous name %@ - it is visible through more than one import"_v,
                                          source, context.findName(name));
            }

            return result;
        }

        result = Found<T> { value, import.module, true };
    }

    return result;
}

/*
 * One module a name written here could come from - the enumeration half of search().
 *
 * `import` is null for the module itself. `qualifier` is the local name a symbol from this module
 * has to be written under, and is zero exactly when it may be written on its own: a `qualified`
 * import contributes nothing to unqualified lookup, which is the same clause search() applies from
 * the other side.
 */
struct VisibleModule {
    Module* module = nullptr;
    const Import* import = nullptr;
    StringId qualifier {};
};

/*
 * Every module in scope here, in search()'s own order.
 *
 * search() answers "what does this name mean"; completion needs "what names are there", which is
 * this traversal with the match replaced by an enumeration - Implementation-Tooling.md §8.1. It
 * lives beside search() and shares its rules for the reason §1 gives for the semantic index: two
 * copies of Yana's visibility rules would agree on the easy cases and disagree on exactly the ones
 * a programmer opens an editor for.
 *
 * `visit` is called as visit(const VisibleModule&) once per module, and the caller enumerates
 * whichever of that module's tables it wants. The per-*name* half of visibility is not applied
 * here, because only the caller knows which names it is about to offer - `Module::visible(import,
 * name)` is that half and has to be asked of each one.
 */
template<class Visit>
void forEachVisible(Context& context, Module& module, Visit&& visit) {
    visit(VisibleModule { &module, nullptr, StringId() });

    for(auto& import: module.imports) {
        if(!import.module) continue;

        // The same filter search() applies - completion offers what a name lookup from here would
        // find, and this file's whole point is that those two answers are one traversal.
        if(!import.inScope(module.activeFile)) continue;

        visit(VisibleModule { import.module, &import, import.qualified ? import.localName : StringId(0) });
    }
}

TypePtr findType(Module& module, StringId name, LocationId source);
Maybe<TypeAlias*> findAlias(Module& module, StringId name, LocationId source);
Maybe<ConstructorRef> findConstructor(Module& module, StringId name, LocationId source);

/*
 * A declaration written under a namespace - doc/spec/modules.md, "Namespaces".
 *
 * A declaration's name may be qualified, and the prefix is one of two things: a type this module
 * declares, in which case the declaration joins that type's namespace and a dot-call on a value of
 * it finds the name; or nothing at all, in which case the prefix is a plain namespace of the
 * module. **Nothing opens a namespace.** The name *is* the namespace, so which of the two a prefix
 * means never changes what the declaration is called or how it is looked up - it decides only
 * whether a dot-call reaches it, which is why there is no `namespace` declaration to write and no
 * collision to report between a namespace and anything else.
 *
 * What is reported is a prefix naming a type this module did *not* declare. That is the orphan
 * rule's reasoning rather than a spelling rule: if any module could add to `String`'s namespace
 * then `s.reserve(n)` would mean different things in two files, and "what can I do to a String"
 * would depend on what a file happened to import. Extending a foreign type is still available
 * through the plain dot-call form, which is import-scoped and honest about being local.
 *
 * A no-op for the single-segment name almost every declaration has.
 */
void registerNamespace(Module& module, StringId name, LocationId source);

/*
 * The name a function of this type's namespace was declared under, or none - `String.reserve` for
 * a receiver of type `String` and a written `reserve`.
 *
 * The answer is a name rather than a function so that the caller resolves it the ordinary way; see
 * Program::typeMethods for why that matters.
 */
StringId findTypeMethod(Module& module, TypePtr receiver, StringId name);
/*
 * `occurrence` is where the name was *written*, which is not always where it is looked up.
 *
 * The lookup location decides what is visible and is where an ambiguity between two imports is
 * reported, so every lookup has one. An occurrence is a name in the source, and a synthesized call
 * has none - it is `kNullLocation` there, which records nothing. The three-argument form is the
 * ordinary case, where the name is written at the place it is resolved from.
 */
// `report` is false for a probe that only asks whether the name exists - see search.
ModulePtr<Function> findFunction(Module& module, StringId name, LocationId source, LocationId occurrence,
                                 bool report = true);

inline ModulePtr<Function> findFunction(Module& module, StringId name, LocationId source) {
    return findFunction(module, name, source, source);
}
ModulePtr<Global> findGlobal(Module& module, StringId name, LocationId source);
GlobalPtr<TypeClass> findClass(Module& module, StringId name, LocationId source);
// The fixity an operator has here, falsy when nothing in scope declares one.
OperatorFixity findFixity(Module& module, StringId name);

// Every class function reachable under this name, across every visible module. A name may
// belong to more than one class, so selection has to see all of them and decide by type.
// The class functions one name refers to. Four inline: an overload set larger than that is a name
// several classes each declare, which is rare enough that its one allocation says nothing.
using ClassFunList = SmallArray<ClassFunRef, 4>;

void findClassFunctions(Module& module, StringId name, LocationId source, ClassFunList& target);

// Every instance of `typeClass` visible from this module, in declaration order.
void findInstances(Module& module, GlobalPtr<TypeClass> typeClass, Array<ModulePtr<ClassInstance>>& target);

// Records one resolved instance, in the module that declared it and in the program-wide index
// findInstances reads. Both, always: an instance in one and not the other is either invisible to
// dispatch or invisible to emission, and neither failure has anything to point at.
void registerInstance(Module& module, ModulePtr<ClassInstance> instance);

// One selected instance, and what selecting it bound its own type variables to. `args` is empty
// for a concrete head and has one entry per variable of a parametric one - which is what an
// implementation of that instance has to be specialized for before it can be called.
struct InstanceMatch {
    ModulePtr<ClassInstance> instance = nullptr;
    TypeList args;

    explicit operator bool() const { return instance != nullptr; }
};

/*
 * The instance of `typeClass` that serves these types, or none.
 *
 * A concrete head is selected by equality and a parametric one by matching, which is the whole of
 * the difference: `Ord(%U8)` is chosen because it is written for `%U8`, and `Ord(Ptr(a))` because
 * `Ptr(a)` matches it. A parametric head must additionally prove its own constraints for what the
 * match bound - `instance (Eq(a)) Eq(Maybe(a))` serves `Maybe(Int)` exactly when something serves
 * `Eq(Int)` - and the proof is recursive, bounded only because a class hierarchy is finite.
 *
 * A requirement whose types are not concrete yet is accepted rather than proved. Nothing commits
 * to an instance in that state: a call whose class arguments are still type variables is deferred
 * to the instantiation that makes them concrete, and that instantiation asks this again.
 *
 * When two instances match, the more specific one wins - the one whose head the other's matches
 * and not the reverse - so a hand-written `Eq(%U8)` beats the blanket `Eq(Ptr(a))` rather than
 * being ambiguous with it.
 *
 * A null entry in `args` is a position the caller does not constrain: it matches any head, and what
 * the selected instance put there is read back out of `forTypes` under the match's own bindings.
 * That is how a class keyed on fewer parameters than it has is asked - `Try(m, a, e)`, where `m`
 * decides the other two - and it is safe because an instance whose own variables the match left
 * open is still rejected, so an unconstrained position can only hold what a constrained one already
 * determined.
 */
InstanceMatch matchInstance(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args);

// matchInstance for callers that only need to know whether the program implements this class here.
ModulePtr<ClassInstance> findInstance(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args);

// Whether `pattern`'s head is at least as general as `other`'s: the head one instance is written
// for matches the head the other is. Two instances general in each other's terms are the same
// instance written twice, which is what makes this the duplicate test as well as the tiebreak.
bool instanceCovers(Module& module, ClassInstance& pattern, ClassInstance& other);

/*
 * Fills the class arguments a functional dependency determines, in place.
 *
 * `class Contiguous(c -> a)` says one `c` has one `a`, so a caller that bound only `c` asks the
 * instance table with a hole where `a` goes and reads back what the head that matched put there.
 * That is matchInstance's existing null-argument protocol, aimed by the declaration rather than by
 * a hand-written caller - which is what `Try`'s lens lowering used to do on its own.
 *
 * A determined position that is *already* bound is passed through rather than re-asked, so an
 * ascription that agrees with the dependency selects the same instance and one that disagrees
 * selects none and reports itself as a missing instance.
 *
 * Answers nothing for a class with no dependency, for a determining position that is unbound or is
 * still a type variable, and where no instance applies. The first two are not failures: a generic
 * position is a body's own variable, which the enclosing signature's constraints answer instead.
 *
 * `bindGeneric` lifts that last restriction, for a caller asking what an instance *looks like*
 * rather than which one to call - see the definition.
 */
InstanceMatch resolveDetermined(Module& module, GlobalPtr<TypeClass> typeClass, TypeList& args,
                                bool bindGeneric = false);

/*
 * Whether two instances break their class's functional dependency: their determining positions can
 * describe the same types, and their determined ones then disagree.
 *
 * This is what a declared dependency buys, and matchInstance's hole-filling is unsound without it.
 * Two heads that disagree about a determined position are not duplicates and neither covers the
 * other, so nothing else rejects them - and a selection with a hole would then answer with
 * whichever of the two was declared first.
 *
 * Checked one way per ordering, because that is the only overlap `matchType` can see: a pair that
 * overlaps without either head covering the other - `C(Pair(Int, a) -> x)` and `C(Pair(b, Bool) ->
 * y)` - needs the unifier the resolver does not have, and is the same coherence gap
 * matchInstanceAt's tiebreak already documents.
 */
bool breaksDependency(Module& module, ClassInstance& pattern, ClassInstance& other);
