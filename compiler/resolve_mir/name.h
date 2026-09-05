#pragma once

#include "decl.h"

namespace mir {

/*
 * Name resolution across modules.
 *
 * Every lookup of a module-level name goes through `search` below - types, aliases, constructors,
 * functions, classes and operator fixity alike. There is one traversal, so there is one answer to
 * each of the questions the traversal decides:
 *
 *  - what a qualified name means (`Core.Maybe`, `Maybe.Just`, `Hmac.sha1`, `X.f` through an alias);
 *  - which module a name is looked for in, and in what order;
 *  - what an import's include/exclude lists hide;
 *  - and what happens when two imports offer the same name.
 *
 * Getting those four answers in one place is the point: they are the rules that turn into silent,
 * hard-to-find inconsistencies when each kind of name grows its own copy.
 *
 * A name is a sequence of segments (`A.B.f` is three), and so is a *declaration's* name - a
 * declaration may be written under a namespace, which makes `Hmac.sha1` one name of two segments
 * rather than two things. Resolution walks, in order:
 *
 *  1. the module itself, matching the whole name against its own symbols, and - for a two-or-more
 *     segment name - the last segment against a symbol namespaced under the preceding ones
 *     (a record's constructors, `Maybe.Just`);
 *  2. every import that can be used unqualified, matching the whole name the same way;
 *  3. every import whose local name is a prefix of the name, matching the rest.
 *
 * Step 1 wins outright: a local definition shadows an import, which is what stops a new symbol
 * appearing in Core from silently capturing a name a user module already defines. Steps 2 and 3 have
 * equal weight, so two hits there are ambiguous and reported as such rather than resolved by import
 * order - *between* imports. Within one they are ordered, and only in the one case where both can
 * apply: see the fallback in `search` for an import whose own name is also a namespace of its
 * declarations.
 *
 * **A class function is the one name that does not shadow**, and `findOverloads` is where that is
 * argued: an overload set is R1's `(name, arity)` set and the members of it are gathered from every
 * reading, because `doc/spec/overloading.md`'s R5 is that a plain function *joins* a set rather than
 * replacing it. The shadowing above is still what picks the set's one plain function. That is why
 * `visitReadings` exists beside `search` rather than inside it.
 *
 * A namespace is a name and nothing else, which is why nothing here has a table of them. What a
 * prefix means - a module, a record, or a namespace no declaration but this one uses - is decided by
 * which of the lookups answers.
 */

/*
 * Whether one symbol is exported from the module that declared it - the `pub` half of visibility.
 *
 * One overload per thing `search` can find, because `pub` is a property of the *declaration* and
 * each kind of declaration produced a different structure. They are here rather than beside each
 * declaration for the reason the file comment gives: which modules a name is searched in, what an
 * import hides and what a declaration is willing to be named from elsewhere are the traversal's
 * business, and the typed wrappers below contribute only what is specific to their kind of symbol.
 *
 * Two things have no overload because they are never `pub` and are never hidden. An instance is
 * global, and `doc/spec/modules.md` says so; a fixity applies wherever its operator is in scope, so
 * `Fixity`'s is the one that answers true unconditionally.
 */
inline bool exportedSymbol(DeclBase base, DeclPtr<Function> function) {
    auto& declared = *base[function];

    // A class member's answer is the class head's - `pub class` exports the signatures it declares,
    // and there is nothing for a second marker on a member to mean. Written here rather than copied
    // onto the member, so that a head marked after the fact cannot leave a member disagreeing.
    return declared.klass ? base[declared.klass]->exported : declared.exported;
}

inline bool exportedSymbol(DeclBase base, DeclPtr<Global> global) { return base[global]->exported; }
inline bool exportedSymbol(DeclBase base, DeclPtr<Alias> alias) { return base[alias]->exported; }
inline bool exportedSymbol(DeclBase base, DeclPtr<Class> klass) { return base[klass]->exported; }

/*
 * What one module declared an operator's grouping to be, and whether it declared one at all.
 *
 * `declared` rather than a sentinel precedence, because zero is a precedence a program may write and
 * "nothing in scope says" is a different answer from "as loosely as possible". It is what carries
 * "not here" through the traversal, which is what makes fixity a lookup like any other.
 */
struct Fixity {
    OpProperties properties {};
    bool declared = false;

    explicit operator bool() const { return declared; }
};

// A fixity is never `pub` and never private: an operator's grouping applies wherever the operator is
// in scope, so an import's include/exclude lists are the only thing that hides one.
inline bool exportedSymbol(DeclBase, Fixity) { return true; }

// A record's own marker, read through the *declaration*: an instantiation is made where it was first
// written rather than by a declaration pass, so nothing wrote `pub` on `Boxed(I32)`.
bool exportedSymbol(DeclBase base, DeclPtr<RecordType> record);

// A constructor is exported with its record and never on its own: there is no way to export a type
// without its constructors, which `doc/spec/modules.md` records as an open question. `Maybe.Just`
// and a bare `Just` are therefore the same answer, which is what stops a qualified spelling being a
// way around the rule.
inline bool exportedSymbol(DeclBase base, ConstructorRef constructor) {
    return constructor.record && exportedSymbol(base, constructor.record);
}

/*
 * Where a name is being looked up *from*, for the length of a scope.
 *
 * An import is scoped to the file that wrote it, so a lookup made on a declaration's behalf after
 * its own pass has finished - discharging a callable requirement at an instantiation, say - has to
 * put the position back before it asks. Restoring rather than clearing, because those walks nest.
 *
 * The declaration's `source` is the ground truth for which file it belongs to, so nothing carries a
 * second copy of that fact. The resolver's own passes set the position through `ModuleScope` while
 * they walk; this is for everything that arrives afterwards.
 */
struct FileScope {
    FileScope(DeclBase base, Context& context, DeclPtr<Module> module, LocationId source):
        module(base[module]), saved(base[module]->activeFile) {
        this->module->activeFile = this->module->fileOf(base, context, source);
    }

    ~FileScope() { module->activeFile = saved; }

    Module* module;
    U16 saved;
};

// One candidate found by the traversal, before ambiguity is decided.
template<class T>
struct Found {
    T value {};
    DeclPtr<Module> module = nullptr;
    bool found = false;

    explicit operator bool() const { return found; }
    T operator * () const { return value; }
};

/*
 * The traversal itself - every reading of a name the rules admit, in the order they admit them.
 *
 * `find` is called as find(DeclPtr<Module>, NameRef) and returns a falsy T for "not here". It is
 * responsible only for matching a name against one module's own symbols; which modules it sees, in
 * what order, and which readings of a qualified name are tried against each are this function's
 * business. `visit` is called as visit(T, DeclPtr<Module>) with every reading that answered and
 * says whether to go on.
 *
 * Split out of `search` below so that the two policies a caller can want - **the first reading
 * wins** and **every reading joins** - are two visitors over one walk rather than two walks. There
 * is exactly one caller of each today and the split is still worth making: module order, alias
 * consumption and both halves of visibility are what go wrong silently when they are written twice,
 * and they are what this function is.
 */
template<class T, class Find, class Visit>
void visitReadings(Context& context, Program& program, DeclPtr<Module> from, StringId name,
                   Find&& find, Visit&& visit) {
    auto base = *program.decl;
    auto identifier = &context.find(name);

    if(auto value = find(from, NameRef { identifier, 0 })) {
        if(!visit(value, from)) return;
    }

    auto& module = *base[from];

    for(auto& import: module.imports.contents(base)) {
        if(!import.module) continue;

        // Only the imports the file being read wrote, plus the implicit ones every file has -
        // Analysis-Modules.md §2.1.2, and `Module::activeFile` for how the file is known here.
        if(!import.inScope(module.activeFile)) continue;

        /*
         * One reading of the name against this import, and whether it is a hit *here*.
         *
         * Both halves of visibility are inside, so that a reading which finds something the author
         * may not name is "not here" rather than an answer - which is what lets the two readings
         * below be tried in order without the first one masking the second.
         */
        auto reading = [&](U32 start) -> T {
            NameRef reference { identifier, start };
            auto value = find(import.module, reference);
            if(!value) return T {};

            // The importing side, checked on the leading segment of the symbol, which is the name an
            // import list names: `hiding (Maybe)` hides `Maybe.Just` along with the type.
            if(!import.visible(base, reference.single())) return T {};

            /*
             * And the declaring side: an unmarked declaration is private and this is not a hit.
             *
             * Not a report. A private name is *not visible*, so a lookup that reaches one goes on to
             * the next import and ends up saying the name is not in scope - which is the same
             * answer, and the same diagnostic, as for a name nothing declares anywhere. Reporting
             * "this exists but you may not have it" would be an interface a private declaration does
             * not have, and it would make adding one to a module a breaking change for its
             * importers.
             */
            if(!exportedSymbol(base, value)) return T {};

            return value;
        };

        // A qualified name may address an import through its local name. The local name is itself
        // possibly multi-segment (`import A.B as X` vs `import A.B`), so this consumes as many
        // segments as the alias has.
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

        T value {};

        // The alias consumed, which is the only reading a `qualified` import contributes.
        if(matchedAlias) value = reading(alias->segmentCount);

        /*
         * And the whole name, where the alias offered nothing.
         *
         * This is the case where steps 2 and 3 of the file comment meet: an import whose local name
         * is also the namespace of one of its own declarations. `import File` where module `File`
         * declares `File.open` offers that name both ways - `File` consumed as the module leaving
         * `open`, and `File.open` matched whole - and consuming the alias *instead of* rather than
         * *before* would make such a namespace unreachable rather than ambiguous.
         *
         * Before rather than beside, so nothing has to compare two values of an arbitrary T: an
         * author writing `File.open` means the module's `open` where there is one, and the module
         * decided to declare both if there are two.
         */
        if(!value && !import.qualified) value = reading(0);

        if(!value) continue;

        if(!visit(value, import.module)) return;
    }
}

/*
 * The first reading that answers, and ambiguity where two imports answer - what every lookup but
 * one asks for.
 *
 * Step 1 wins outright, which is the file comment's rule: a local definition shadows an import.
 * Steps 2 and 3 have equal weight, so two of them are reported rather than ordered.
 *
 * `report` is false where the caller is asking whether a name exists at all rather than deciding a
 * use of it. An ambiguity is not an answer to that question.
 */
template<class T, class Find>
Found<T> search(Context& context, Program& program, DeclPtr<Module> from, StringId name,
                LocationId source, Find&& find, bool report = true) {
    Found<T> result;

    visitReadings<T>(context, program, from, name, find, [&](T value, DeclPtr<Module> in) {
        if(in == from) {
            result = Found<T> { value, from, true };
            return false;
        }

        if(result.found && result.module != in) {
            if(report) {
                context.diagnostics.error("ambiguous name %@ - it is visible through more than one import"_v,
                                          source, context.findName(name));
            }

            return false;
        }

        result = Found<T> { value, in, true };
        return true;
    });

    return result;
}

/*
 * The typed lookups - what a use site calls.
 *
 * Each contributes only the rule that is specific to its kind of symbol, and leaves module order,
 * qualified prefixes, visibility and ambiguity to the traversal above. `from` is the module the name
 * was written in; `source` is where, which is what an ambiguity is reported against.
 */
DeclPtr<RecordType> findRecord(Context& context, Program& program, DeclPtr<Module> from,
                               StringId name, LocationId source);
DeclPtr<Alias> findAlias(Context& context, Program& program, DeclPtr<Module> from, StringId name,
                         LocationId source);
ConstructorRef findConstructor(Context& context, Program& program, DeclPtr<Module> from,
                               StringId name, LocationId source);
DeclPtr<Class> findClass(Context& context, Program& program, DeclPtr<Module> from, StringId name,
                         LocationId source);
DeclPtr<Global> findGlobal(Context& context, Program& program, DeclPtr<Module> from, StringId name,
                           LocationId source);

/*
 * The overload set a name and arity reach from here - `OverloadSet`, and R1 in
 * `doc/spec/overloading.md`.
 *
 * What a *call* asks, since selecting between candidates is what a call does. `report` is false for
 * a probe that only asks whether the name exists - see `search`.
 */
void findOverloads(Context& context, Program& program, DeclPtr<Module> from, StringId name,
                   U32 arity, LocationId source, OverloadSet& into, bool report = true);

// The one function of that name and arity, where the set has exactly one - `Program::findFunction`,
// where "and also null where it has several" is argued.
DeclPtr<Function> findFunction(Context& context, Program& program, DeclPtr<Module> from,
                               StringId name, U32 arity, LocationId source, bool report = true);

// Whether any module in scope declares a function of that name, whatever its arity - for a message
// that can tell "no such function" from "not at this arity". Two questions with two answers, since
// the second is the one an author who wrote the wrong number of arguments has to read.
bool hasFunctionName(Context& context, Program& program, DeclPtr<Module> from, StringId name);

/*
 * The fixity an operator has here, and the fallback where nothing in scope declares one.
 *
 * An `infixl`/`infixr` declaration is looked up through the imports like any other name, so an
 * operator declared in Core has Core's precedence wherever Core is imported. What is *not* a
 * declaration is the builtin table this stage still stands Core's fixities in for - see
 * `Resolver::findOperator`, which is the one place the two are put together.
 */
Fixity findFixity(Context& context, Program& program, DeclPtr<Module> from, StringId name);

} // namespace mir
