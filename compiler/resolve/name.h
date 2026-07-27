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

TypePtr findType(Module& module, StringId name, LocationId source);
Maybe<TypeAlias*> findAlias(Module& module, StringId name, LocationId source);
Maybe<ConstructorRef> findConstructor(Module& module, StringId name, LocationId source);
ModulePtr<Function> findFunction(Module& module, StringId name, LocationId source);
ModulePtr<Global> findGlobal(Module& module, StringId name, LocationId source);
GlobalPtr<TypeClass> findClass(Module& module, StringId name, LocationId source);
Maybe<U8> findPrecedence(Module& module, StringId name);

// Every class function reachable under this name, across every visible module. A name may
// belong to more than one class, so selection has to see all of them and decide by type.
void findClassFunctions(Module& module, StringId name, LocationId source, Array<ClassFunRef>& target);

// Every instance of `typeClass` visible from this module, in declaration order.
void findInstances(Module& module, GlobalPtr<TypeClass> typeClass, Array<ModulePtr<ClassInstance>>& target);

// The one instance of `typeClass` for exactly these types, or null. Selection is by argument
// types alone, so this is the single answer to "does this program implement this class here".
ModulePtr<ClassInstance> findInstance(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args);
