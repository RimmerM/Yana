#include "complete.h"
#include "expr.h"
#include "name.h"

/*
 * What is in scope at the cursor, collected once - Implementation-Tooling.md §8.
 *
 * Everything below runs at the moment the resolver reaches the sentinel and never afterwards, which
 * is what makes it cheap enough to do all of it eagerly: the enclosing scope, the expected type and
 * the receiver are live right then, and keeping them past the capture would mean keeping the
 * resolver itself.
 */

bool isCursorSentinel(Context& context, StringId name) {
    return context.completion && name && name == cursorName(context);
}

bool wantsCompletion(Context& context) {
    return context.completion && !context.completion->captured;
}

/*
 * Ranking.
 *
 * A rank rather than a score: LSP clients sort by `sortText` and filter by the typed prefix, so
 * what the server owes them is a coarse, stable grouping, not a similarity measure they will
 * immediately re-order. Lower sorts first.
 */
enum Rank: U8 {
    // What this position asked for. §8.2's reason for the expected type being worth threading down
    // at all: `let x: Maybe(Int) = ` offers `Just` and `Nothing` above everything Core exports.
    RankExpected = 0,

    // The names the author wrote nearest to the cursor - this body's, then this module's.
    RankLocal = 1,
    RankModule = 2,

    // Everything reached through an import, which for any program at all is most of the list.
    RankImported = 3,
};

/*
 * Whether a candidate produces what the position asked for.
 *
 * A declaration and its instantiations are one answer here, because a constructor of `Maybe(a)` is
 * exactly what a `Maybe(Int)` position wants and comparing the two as types would say no. That is
 * the only widening: nothing here searches for a conversion, since offering a name whose type merely
 * *could* be converted is how a ranked list becomes an unranked one.
 */
static bool fitsExpected(GlobalBase global, TypePtr type, TypePtr expected) {
    if(!type || !expected) return false;

    auto candidate = canonicalType(global, type);
    auto wanted = canonicalType(global, expected);
    if(candidate == wanted) return true;

    if(global[candidate]->kind == Type::Record && global[wanted]->kind == Type::Record) {
        return ((RecordType*)global[candidate])->base(global) == ((RecordType*)global[wanted])->base(global);
    }

    return false;
}

// What a name produces where it is used, which is what `expected` is compared against. Null for the
// kinds that produce nothing - a type name, a class - and those simply never rank as expected.
static TypePtr resultTypeOf(Module& module, const Symbol& symbol) {
    auto global = *module.types;
    auto owner = symbol.module ? symbol.module : &module;
    auto local = *owner->arena;

    switch(symbol.kind) {
        case Symbol::Kind::Function:
            return local[ModulePtr<Function>(symbol.payload)]->returnType;
        case Symbol::Kind::ClassFun: {
            auto typeClass = global[GlobalPtr<TypeClass>(symbol.payload)];
            if(symbol.index >= typeClass->functions.size()) return nullptr;

            auto entry = typeClass->functions.get(global, symbol.index);
            return entry.fun ? local[entry.fun]->returnType : nullptr;
        }
        case Symbol::Kind::Global:
            return local[ModulePtr<Global>(symbol.payload)]->type;
        case Symbol::Kind::Constructor:
            // The record it builds, not its payload: `Just` is how a `Maybe` is written.
            return (Type*)global[GlobalPtr<RecordType>(symbol.payload)] - global;
        default:
            return nullptr;
    }
}

/*
 * Collecting.
 */

struct Collector {
    ExprResolver& resolver;
    CompletionRequest& request;
    GlobalBase global;

    // The names already offered, so that a binding shadowing an outer one - or an import offering
    // what this module also declares - is one item rather than two. Nearest first is what makes
    // this correct: whichever the *program* would mean is the one that reaches here first.
    Array<StringId> seen;

    /*
     * And the same for the *name* half of a `name: value` pair, kept apart from it.
     *
     * A parameter called `count` and a local called `count` are two different things to write at an
     * argument position - `count: ` names the parameter and `count` passes the value - so one is not
     * a shadow of the other and offering only the first would hide whichever the author meant. Two
     * lists rather than one key, because the shadowing rule within each is unchanged.
     */
    Array<StringId> seenNaming;

    bool take(StringId name, bool naming) {
        if(!name) return false;

        auto& into = naming ? seenNaming : seen;

        for(auto existing: into) {
            if(existing == name) return false;
        }

        into.push(name);
        return true;
    }

    void add(const Symbol& symbol, TypePtr type, StringId qualifier, U8 rank, bool naming = false) {
        if(!take(symbol.name, naming)) return;

        CompletionItem item;
        item.symbol = symbol;
        item.type = type;
        item.qualifier = qualifier;
        item.naming = naming;
        item.rank = fitsExpected(global, type, request.expected) ? U8(RankExpected) : rank;

        request.items.push(item);
    }
};

/*
 * §8.1's second kind: the bindings of the enclosing body.
 *
 * The scope stack `ExprResolver` holds while resolving is exactly the answer, walked innermost
 * first - which is both the shadowing rule and the ranking. The `enclosing` chain is walked too,
 * because a name a lambda body does not bind may still belong to the body it was written in, and
 * naming one there is what makes it a capture: offering only what the innermost resolver has bound
 * would hide every name the author can still reach.
 */
static void collectBindings(Collector& into) {
    for(auto resolver = &into.resolver; resolver; resolver = resolver->enclosing) {
        auto& bindings = resolver->bindings;

        for(Size i = bindings.size(); i > 0; i--) {
            auto& binding = bindings[i - 1];

            // The sentinel can be a binding: `(na|me) -> ...` reads its parameter list out of an
            // expression, so a cursor in a lambda's parameter name becomes an argument called
            // `$cursor`. Offering it back would be the editor completing the word being typed with
            // itself.
            if(isCursorSentinel(resolver->context, binding.name)) continue;

            into.add(bindingSymbol(*resolver, binding), bindingType(*resolver, binding), 0, RankLocal);
        }
    }
}

/*
 * §8.1's first kind: everything a name written here could refer to.
 *
 * `forEachVisible` supplies which modules those are and how each has to be written, and
 * `Module::visible` the per-name half of the same rule - see resolve/name.h. Which tables hold what
 * is this function's only contribution.
 *
 * The order the tables are walked in is what decides a name two of them hold - `data Shape {..}`
 * declares both the type and its constructor - so it is fixed here rather than left to whichever is
 * asked first. The order *within* one table is a hash map's and means nothing, which is why the
 * items are sorted by name at the end rather than left in the order they were collected.
 */
static void collectVisible(Collector& into, Module& module) {
    auto& context = module.context;

    forEachVisible(context, module, [&](const VisibleModule& visible) {
        auto owner = visible.module;
        auto rank = visible.import ? U8(RankImported) : U8(RankModule);

        auto offer = [&](const Symbol& symbol) {
            if(visible.import && !Module::visible(*visible.import, symbol.name)) return;
            into.add(symbol, resultTypeOf(*owner, symbol), visible.qualifier, rank);
        };

        for(auto entry: owner->functions.entries()) {
            // A class function's signature is reached through its class rather than as a function
            // of the module, and an instance implementation has a synthesized name nothing can
            // write. Neither is a name to offer.
            auto function = (*owner->arena)[entry.value];
            if(function->signature || function->instanceOf) continue;

            offer(functionSymbol(*owner, entry.value));
        }

        for(auto entry: owner->globals.entries()) {
            offer(globalSymbol(*owner, entry.value));
        }

        /*
         * Constructors before types, which is the whole of what the order decides.
         *
         * `data Point {x: Int, y: Int}` declares both a type and a constructor called `Point`, and
         * one name is one item - so whichever table is walked first is the answer. The constructor
         * is the right one: every position that reaches the sentinel is an *expression* position
         * (§8.6), a type name is not a value there, and the difference is visible - a constructor
         * completes to `Point {x: , y: }` and a type completes to a name that does not resolve.
         *
         * A type whose name no constructor shares is unaffected and still offered, which is what
         * keeps `Shape` in the list beside `Circle` and `Square`.
         */
        for(auto entry: owner->constructors.entries()) {
            if(!entry.value.record) continue;
            offer(constructorSymbol(*owner, entry.value));
        }

        for(auto entry: owner->namedTypes.entries()) {
            offer(typeSymbol(*owner, entry.value, entry.key));
        }

        for(auto entry: owner->aliases.entries()) {
            offer(aliasSymbol(*owner, entry.value));
        }

        for(auto entry: owner->classes.entries()) {
            offer(classSymbol(*owner, entry.value));
        }

        for(auto& reference: owner->classFunctions) {
            offer(classFunSymbol(*owner, reference.typeClass, reference.index));
        }
    });
}

/*
 * §8.1's third kind: what follows a `.`.
 *
 * The fields of the receiver, and only those. §8.1 also asks for every visible function whose first
 * parameter accepts the receiver - Yana's `x.f(y)` as `f(x, y)` - and that half is deliberately not
 * here: the resolver has no such rule (`resolveField` projects a field and nothing else), so every
 * name it added would complete to a program that does not compile. It belongs with the language
 * feature, not before it.
 *
 * A reference is followed one step, exactly as field selection follows one, so `p.` on a `&Point`
 * offers what `p.x` would have reached.
 */
// The named fields of one tuple, as items belonging to `owner`. Shared by the two positions that
// offer fields - after a `.`, and inside the braces of a construction - so that a field looks the
// same to the client whichever of them asked.
static void collectFields(Collector& into, Module& module, TypePtr owner, TypePtr content,
                          LocationId ownerSource, U8 rank) {
    auto global = *module.types;
    if(!content || global[content]->kind != Type::Tup) return;

    auto tuple = (TupType*)global[content];
    for(Size i = 0; i < tuple->fields.size(); i++) {
        auto field = tuple->fields.get(global, i);
        if(!field.name) continue;

        into.add(fieldSymbol(module, owner, U16(i), field.name, ownerSource), field.type, 0, rank);
    }
}

static void collectMembers(Collector& into, Module& module, TypePtr receiver) {
    auto global = *module.types;
    auto type = receiver;
    if(!type) return;

    if(isPointer(global, type)) type = ((PtrType*)global[type])->to;
    else if(isBorrow(global, type)) type = ((BorrowType*)global[type])->to;

    auto owner = type;
    auto ownerSource = kNullLocation;

    // A single-constructor record is what direct field selection reaches through, which is the same
    // condition projectField applies - a multi-constructor one has to be matched on first, so it has
    // no fields to offer under its own name.
    if(type && global[type]->kind == Type::Record) {
        auto record = (RecordType*)global[type];
        if(record->layout != RecordType::Single || record->constructors.size() == 0) return;

        ownerSource = global[record->base(global)]->source;
        type = record->constructors.get(global, 0).content;
    }

    collectFields(into, module, owner, type, ownerSource, RankLocal);
}

/*
 * Ordering.
 *
 * By rank, then by the name as written. The name rather than its StringId, which is a hash: the
 * order would look arbitrary in a fixture and change whenever an unrelated name did.
 *
 * An insertion sort, which is quadratic and fine here because the list is short and bounded by
 * something that does not grow with the file. What is in scope is one module plus its *direct*
 * imports, so a program that imports Core, Native and Collections offers about 130 names - measured
 * with `YanaLspTest sweep`, which reports the largest answer it saw for exactly this reason.
 */
static bool sortsBefore(Context& context, const CompletionItem& a, const CompletionItem& b) {
    if(a.rank != b.rank) return a.rank < b.rank;

    auto& left = context.find(a.symbol.name);
    auto& right = context.find(b.symbol.name);
    auto shared = left.textLength < right.textLength ? left.textLength : right.textLength;

    for(U32 i = 0; i < shared; i++) {
        if(left.text[i] != right.text[i]) return left.text[i] < right.text[i];
    }

    return left.textLength < right.textLength;
}

static void sortItems(Context& context, Array<CompletionItem>& items) {
    for(U32 i = 1; i < items.size(); i++) {
        auto item = items[i];
        auto j = i;
        while(j > 0 && sortsBefore(context, item, items[j - 1])) {
            items[j] = items[j - 1];
            j--;
        }

        items[j] = item;
    }
}

/*
 * §8.1's fourth kind - the fields of what is being written.
 *
 * Not a special case of the third: after a `.` the receiver is a finished value and its fields are
 * the only thing that can follow, while inside `Square {si|` the author has not committed to
 * writing a field name at all - a positional constructor takes a value in the same place. So the
 * field names lead and everything in scope follows, and only the `name:` position (`namesOnly`)
 * drops the second half.
 *
 * The fields rank as `RankExpected` for the same reason the expected type does: they are what this
 * position asked for, and a field of the record being written is a better answer than a local that
 * merely shares its prefix.
 */
void captureConstructionCompletion(ExprResolver& resolver, TypePtr owner, TypePtr content, bool namesOnly) {
    auto& context = resolver.context;
    auto request = context.completion;
    if(!request || request->captured) return;

    auto global = resolver.global;

    request->captured = true;
    request->module = &resolver.module;
    request->function = &resolver.function - resolver.local;
    request->constructed = owner;
    request->construct = true;

    Collector collector { resolver, *request, global };

    auto ownerSource = kNullLocation;
    if(owner && global[owner]->kind == Type::Record) {
        ownerSource = global[((RecordType*)global[owner])->base(global)]->source;
    }

    collectFields(collector, resolver.module, owner ? owner : content, content, ownerSource, RankExpected);

    if(!namesOnly) {
        collectBindings(collector);
        collectVisible(collector, resolver.module);
    }

    sortItems(context, request->items);
    resolver.sawParseError = true;
}

/*
 * The parameters of one signature, as the `name:` half a call site may write.
 *
 * `Symbol::Kind::Arg` against the *callee*, which is what makes hovering the item and jumping from
 * it work without a second kind: it is the same symbol the parameter's own declaration records, so
 * an editor showing `argument mode: Mode` beside the item is showing what the item is.
 */
static void collectParameters(Collector& into, Module& module, ModulePtr<Function> signature) {
    if(!signature) return;

    auto local = *module.arena;
    auto declaration = local[signature];

    for(Size i = 0; i < declaration->args.size(); i++) {
        auto pointer = declaration->args.get(local, i);
        auto argument = local[pointer];
        if(!argument->name) continue;

        Symbol symbol;
        symbol.kind = Symbol::Kind::Arg;
        symbol.module = &module;
        symbol.function = signature;
        symbol.name = argument->name;
        symbol.definition = argument->source;
        symbol.payload = argument->index;

        into.add(symbol, argument->declaredType(), 0, RankExpected, true);
    }
}

void captureArgumentCompletion(ExprResolver& resolver, const OverloadSet& set, TypePtr expected,
                               bool namesOnly) {
    auto& context = resolver.context;
    auto request = context.completion;
    if(!request || request->captured) return;

    request->captured = true;
    request->module = &resolver.module;
    request->function = &resolver.function - resolver.local;
    request->expected = expected;

    Collector collector { resolver, *request, resolver.global };

    /*
     * Both halves of the overload set, and neither narrowed by whether it fits: which candidate
     * serves the call is decided by arguments that are, by definition, not written yet. A name that
     * only one of them declares is still a name this call might be about, and the Collector's
     * de-duplication is what keeps two candidates agreeing about one from offering it twice.
     */
    collectParameters(collector, resolver.module, set.direct);
    collectParameters(collector, resolver.module, set.mismatched);

    for(auto& candidate: set.candidates) {
        if(!candidate.typeClass) continue;

        auto entry = resolver.global[candidate.typeClass]->functions.get(resolver.global, candidate.index);
        collectParameters(collector, resolver.module, entry.fun);
    }

    // And everything a name in expression position would offer, because an argument that is not in
    // a name position may still be written positionally.
    if(!namesOnly) {
        collectBindings(collector);
        collectVisible(collector, resolver.module);
    }

    sortItems(context, request->items);
    resolver.sawParseError = true;
}

void captureUpdateCompletion(ExprResolver& resolver, TypePtr receiver) {
    captureCompletion(resolver, nullptr, receiver, true);

    // Set after the capture rather than passed into it, because it says nothing about *what* was
    // collected - the items are the receiver's members either way - and everything about what
    // choosing one has to type.
    if(resolver.context.completion) resolver.context.completion->construct = true;
}

/*
 * §8.1's fifth kind - the constructors a pattern could name.
 *
 * The visibility walk is `collectVisible`'s, restricted to one table. That restriction is the whole
 * of the difference and it is the point: everything else in scope is a *value*, and a pattern
 * position holds a constructor, a literal, or a name being introduced. Offering a function there
 * would complete to something that cannot be written.
 *
 * `expected` is the pivot rather than a conversion target, so `fitsExpected` ranks the constructors
 * of the record being matched above every other visible one - which is the same mechanism, and the
 * same rank, that puts `Nothing` first at a `Maybe(Int)`.
 */
void capturePatternCompletion(ExprResolver& resolver, TypePtr pivot) {
    auto& context = resolver.context;
    auto request = context.completion;
    if(!request || request->captured) return;

    request->captured = true;
    request->module = &resolver.module;
    request->function = &resolver.function - resolver.local;
    request->expected = pivot;
    request->pattern = true;

    Collector collector { resolver, *request, resolver.global };

    forEachVisible(context, resolver.module, [&](const VisibleModule& visible) {
        auto owner = visible.module;
        auto rank = visible.import ? U8(RankImported) : U8(RankModule);

        for(auto entry: owner->constructors.entries()) {
            if(!entry.value.record) continue;

            auto symbol = constructorSymbol(*owner, entry.value);
            if(visible.import && !Module::visible(*visible.import, symbol.name)) continue;

            collector.add(symbol, resultTypeOf(*owner, symbol), visible.qualifier, rank);
        }
    });

    sortItems(context, request->items);
    resolver.sawParseError = true;
}

void captureCompletion(ExprResolver& resolver, TypePtr expected, TypePtr receiver, bool member) {
    auto& context = resolver.context;
    auto request = context.completion;

    // One answer per compile. A second sentinel cannot exist - the parser emits one - so this is
    // about the same sentinel being reached twice, which a body resolved once per specialization
    // would do.
    if(!request || request->captured) return;

    request->captured = true;
    request->module = &resolver.module;
    request->function = &resolver.function - resolver.local;
    request->expected = expected;
    request->receiver = receiver;
    request->member = member;

    Collector collector { resolver, *request, resolver.global };

    if(member) {
        collectMembers(collector, resolver.module, receiver);
    } else {
        collectBindings(collector);
        collectVisible(collector, resolver.module);
    }

    sortItems(context, request->items);

    /*
     * The body carries on, and the sentinel is an expression that produced nothing.
     *
     * §8.2 proposes a "done" flag here instead, unwound through the resolver's early returns - which
     * would be `current = nullptr`, the state a `return` leaves behind. That is wrong, and the way
     * it is wrong is instructive: `resolve()` checks `current` on the way *in*, but a caller that
     * has already started an instruction does not check it again between resolving its operands and
     * emitting. A sentinel inside an argument therefore left `resolveCall` emitting into a block
     * that was not there.
     *
     * Producing nothing needs no such flag, because it is what an unresolvable name already does -
     * the best-trodden path in the resolver, since it is the one every misspelling takes. The
     * capture above is already complete, so there is nothing left to stop for.
     *
     * `sawParseError` because a body with the cursor in it does not return a value for the same
     * reason a half-typed one does not, and saying so would be a diagnostic about the author's
     * caret.
     */
    resolver.sawParseError = true;
}
