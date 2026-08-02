#pragma once

#include "module.h"

/*
 * Name resolution.
 *
 * Every lookup of a module-level name goes through search() below - types, constructors,
 * functions, class functions, classes, operator fixity and instances alike. There is one
 * traversal, so there is one answer to each of the questions the traversal decides:
 *
 *  - what a qualified name means (`Core.Maybe`, `Maybe.Just`, `X.f` through an import alias);
 *  - which module a name is looked for in, and in what order;
 *  - what an import's include/exclude lists hide;
 *  - and what happens when two imports offer the same name.
 *
 * Getting those four answers in one place is the point: they are the rules that turn into
 * silent, hard-to-find inconsistencies when each kind of name grows its own copy.
 *
 * A name is a sequence of segments (`A.B.f` is three). Resolution walks, in order:
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
 * by import order.
 */

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

    // The whole remaining name as one id, valid only when a single segment is left. Multi-segment
    // symbol names are matched segment by segment instead.
    StringId single() const { return identifier->getHash(start); }
};

/*
 * Runs `find` over the module and its visible imports, and reports ambiguity.
 *
 * `find` is called as find(Module&, NameRef) and returns a falsy T for "not here". It is
 * responsible only for matching a name against one module's own symbols; which modules it sees,
 * in what order, and what is done about two answers are this function's business.
 */
template<class T, class Find>
Found<T> search(Context& context, Module& module, StringId name, LocationId source, Find&& find) {
    auto identifier = &context.find(name);
    Found<T> result;

    if(auto value = find(module, NameRef { identifier, 0 })) {
        return Found<T> { value, &module, true };
    }

    for(auto& import: module.imports) {
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
        if(!value) continue;

        // Visibility is checked on the leading segment of the symbol, which is the name the
        // import list names: `hiding (Maybe)` hides `Maybe.Just` along with the type, while
        // hiding a single constructor of a visible type is not something an import list can say.
        if(!Module::visible(import, reference.single())) continue;

        if(result.found && result.module != import.module) {
            context.diagnostics.error("ambiguous name %@ - it is visible through more than one import"_v,
                                      source, context.findName(name));
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
    StringId qualifier = 0;
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
    visit(VisibleModule { &module, nullptr, 0 });

    for(auto& import: module.imports) {
        if(!import.module) continue;

        visit(VisibleModule { import.module, &import, import.qualified ? import.localName : StringId(0) });
    }
}

TypePtr findType(Module& module, StringId name, LocationId source);
Maybe<TypeAlias*> findAlias(Module& module, StringId name, LocationId source);
Maybe<ConstructorRef> findConstructor(Module& module, StringId name, LocationId source);
ModulePtr<Function> findFunction(Module& module, StringId name, LocationId source);
ModulePtr<Global> findGlobal(Module& module, StringId name, LocationId source);
GlobalPtr<TypeClass> findClass(Module& module, StringId name, LocationId source);
Maybe<U8> findPrecedence(Module& module, StringId name);

// Every class function reachable under this name, across every visible module. A name may
// belong to more than one class, so selection has to see all of them and decide by type.
// The class functions one name refers to. Four inline: an overload set larger than that is a name
// several classes each declare, which is rare enough that its one allocation says nothing.
using ClassFunList = SmallArray<ClassFunRef, 4>;

void findClassFunctions(Module& module, StringId name, LocationId source, ClassFunList& target);

// Every instance of `typeClass` visible from this module, in declaration order.
void findInstances(Module& module, GlobalPtr<TypeClass> typeClass, Array<ModulePtr<ClassInstance>>& target);

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
