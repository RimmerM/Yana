#pragma once

#include "block.h"
#include "class.h"

namespace ast {
struct Module;
struct Decl;
struct Expr;
}

struct Program;
struct ExprResolver;
struct Deferred;
struct OwnershipResults;
struct AnalysisScratch;
struct LensYield;

/*
 * A named storage slot in a function.
 *
 * `convention` is how the name that introduced this slot accesses it - Design.md's borrow / `&` /
 * `->`. It is deliberately distinct from `owner`, which the analysis side table assigns: a
 * convention describes one access, while the owner identifies the object whose lifetime all of
 * those accesses constrain. `InstMove` transfers an owner, `InstBorrow` refers back to one, and
 * `InstCopy` creates a fresh one.
 */
struct Local {
    TypePtr type = nullptr;
    StringId name = 0;
    ModulePtr<Value> value = nullptr;
    ast::BindType convention = ast::BindType::Borrow;
    StorageClass storage = StorageClass::Stack;

    // Set for the slot behind a `&` parameter: the local names storage the caller owns, so it is
    // never allocated, never initialized here, and never dropped here.
    bool borrowed = false;

    /*
     * Set for the environment a closure captured into.
     *
     * This frame allocates it and the *function value* owns it: nothing here drops it and nothing
     * here hands the storage back, because the closure's own derived teardown does both through the
     * closure header its code word leads to. Where it is allocated is the ordinary decision - a
     * closure that outlives this frame takes its captures with it, and one that does not can have
     * them in the frame like anything else - and selectStorage records the answer in that header,
     * since the teardown that reads it is not in this frame at all.
     */
    bool closureEnv = false;

    /*
     * Set for the temporary a mutable borrow of a packed field was materialized into - Design.md's
     * tier 1, and see ExprResolver::borrowPlace.
     *
     * Recorded because the storage is *this frame's* while the field it stands for is not, so a
     * borrow of it may not outlive the frame. A callee that retains one, or a result rooted in one,
     * is Design.md's tier 2 and needs the field's property witness rather than a temporary; until
     * that exists the ownership passes report it, and this is what tells them which locals to
     * report about.
     *
     * Function::addLocal builds one of these positionally, so a field added in the middle silently
     * shifts every one after it. Everything below is set afterwards rather than by that call, which
     * is why it can be here at all.
     */
    bool materialized = false;

    /*
     * The local this one is a view of - Implementation-Containers.md §4's slice.
     *
     * A `{base, length}` descriptor built out of an array holds a raw pointer into that array's run,
     * so it names storage without naming a local: liveness sees the array read once, where the
     * descriptor was built, and would then be entitled to drop it before the call the descriptor was
     * built *for*. Recording which local it is a view of makes a use of the view a use of the
     * viewed, which is the one fact liveness was missing and the same one the borrow checker gets
     * from following the pointer out of the borrow (see lastUseOf).
     *
     * maxLimit for everything that is not a view, which is every local but these.
     */
    U32 viewOf = maxLimit<U32>;
};

/*
 * What one argument's summary says about what the callee does with it.
 *
 * `requirements` is what the callee demands of the caller's storage, which is how mutation demand
 * crosses a call without the caller re-inspecting the body: `fn bump(&counter: Int)` reports a
 * writable first argument, so every root reaching that position is writable at the caller too.
 *
 * `retained` is whether anything derived from the argument outlives the call - stored into a
 * global, written through a pointer the callee did not own, or handed to a further call that
 * retains it. A retained argument's loan cannot end at the call the way an ordinary borrow's does.
 */
struct ArgSummary {
    ReprRequirements requirements;
    bool retained = false;

    // The `return` marker, copied from the declaration so that a caller reads one structure rather
    // than two. Rejected on Sink arguments and on defaulted ones - see resolveSignature.
    bool returnRoot = false;

    /*
     * Whether anything in the body names this parameter's **own storage**, as opposed to reading
     * values out of it.
     *
     * `returnRoot` is one answer to two questions, and the difference is what this separates. A
     * `return` marker says the caller must keep the argument alive while the result lives, and it is
     * true of both of these:
     *
     *     fn slice(return self: Flat(a), ...) -> Flat(a)   -- copies self.items *out*
     *     fn whole(return self: Flat(a)) -> &Flat(a)       -- hands back self's storage
     *
     * For the first, a copy of the parameter is as good as the parameter: the pointer it hands back
     * is the same number and still names the caller's buffer. For the second it is not - the result
     * would name the copy, and the copy dies with the callee's frame.
     *
     * The distinction cannot be read off the provenance analysis, which deliberately conflates the
     * two: `computeProvenance` seeds a parameter's contents with the parameter itself, because
     * "reachable through" is the relation Design.md's return-root rule is stated over. So this is
     * the narrower question asked separately - see deriveSummary.
     *
     * Conservative: it is set where such a value exists at all, without checking that it reaches a
     * `ret`. What reads it is `opt_arg`, where the cost of over-reporting is one flattening declined.
     */
    bool namesStorage = false;

    bool operator==(const ArgSummary& other) const {
        return requirements == other.requirements && retained == other.retained &&
               returnRoot == other.returnRoot && namesStorage == other.namesStorage;
    }
};

/*
 * What a caller may know about a callee without looking at its body (Implementation-IR.md part 5).
 *
 * The three things it answers are the three a caller cannot derive for itself: what the callee does
 * to each argument, where a borrow in the result came from, and how long the result's storage has
 * to stay valid. Everything else a caller needs it can see.
 *
 * Root sets are bit masks over argument indices. That caps a return-root group at 64 arguments,
 * which is not a limit any signature reaches; a function with more of them simply cannot mark the
 * ones past it, and resolveSignature says so rather than silently dropping the marker.
 */
struct FunctionSummary {
    ModuleList<ArgSummary, false> args;

    // The declared group, and what resolving every return path actually found. The check is
    // `actual` is a subset of `declared`, plus `invalidRoot` being clear.
    U64 declaredRoots = 0;
    U64 actualRoots = 0;

    // Set when a returned borrow is rooted in something that can never be a member of any group -
    // a local, a global, or a sunk parameter. Kept apart from `actualRoots` so that the diagnostic
    // can say which of the two mistakes was made.
    bool invalidRoot = false;

    // Set when the result is or contains a borrow, and whether that borrow is a mutable one. A
    // mutable result has to be rooted in a `return &` member rather than in any member.
    bool returnsBorrow = false;
    bool mutableResult = false;

    StorageBound resultBound = StorageBound::Frame;

    // False until the fixpoint has visited this function once. A callee that has not been visited
    // contributes nothing, which is what makes the fixpoint start optimistic and climb.
    bool ready = false;

    // Set for a function whose body is not available to summarize - a class signature, an
    // unresolved callee, a call whose target is decided per specialization. Everything about an
    // opaque callee is the conservative answer.
    bool opaque = false;
};

// A function whose body the resolver generates at the call site instead of calling. The
// primitive operations are the only ones today: `+` on Int has a real Function with a real body
// so that it can be printed, lowered and taken the address of, but an ordinary call to it
// expands to the one instruction it contains rather than to a call the backend would have to
// inline later.
using Intrinsic = ModulePtr<Value> (*)(ExprResolver& resolver, Buffer<ModulePtr<Value>> args,
                                       TypePtr type, LocationId source, StringId name);

/*
 * The same, for a function whose signature has a `@lazy` parameter.
 *
 * The difference is the whole of what short-circuiting is: an ordinary intrinsic is handed values
 * that have already been computed, and this one is handed the argument itself and decides where -
 * and whether - to run it. `args` is null at each deferred position and `deferred` is set there,
 * and the expansion forces one by calling ExprResolver::force in whichever block it built for it.
 *
 * A function with one of these still has a real body, generated the ordinary way, which is what a
 * call that cannot see through it reaches. That body forces its parameter by calling the thunk;
 * this one emits a branch. The two have to agree, so they are written next to each other.
 */
using DeferredIntrinsic = ModulePtr<Value> (*)(ExprResolver& resolver, Buffer<ModulePtr<Value>> args,
                                               Buffer<Deferred> deferred, TypePtr type, LocationId source,
                                               StringId name);

struct Function {
    Function(Module* module, StringId name): module(module), name(name) {}

    Block* addBlock(Module& module, StringId name = 0);
    Arg* addArg(Module& module, StringId name, TypePtr type, LocationId source);
    U32 addLocal(Module& module, TypePtr type, StringId name, ModulePtr<Value> value,
                 ast::BindType convention = ast::BindType::Borrow, bool borrowed = false,
                 bool closureEnv = false);

    Local localAt(ModuleBase base, U32 index) { return locals.get(base, index); }
    Size localCount() { return locals.size(); }

    /*
     * Repointing a slot at the value that fills it, after `addLocal` made it.
     *
     * The pairing is two fields - `Local::value` here and `Value::slot` there - and a rewrite that
     * set one of them left the other answering with a value that no longer exists. That is what this
     * is for and the only reason it is a method: there are four rewrites that repoint a slot (the
     * two halves of specialization, the inliner splicing a result, and opt_arg giving a flattened
     * parameter storage), and each of them used to write the field directly.
     */
    void setLocalValue(ModuleBase base, U32 index, ModulePtr<Value> value) {
        auto slot = locals.get(base, index);
        slot.value = value;
        locals.set(base, index, slot);
        if(value) base[value]->slot = index;
    }

    Module* module;
    StringId name;
    LocationId source = kNullLocation;
    TypePtr returnType = nullptr;

    /*
     * Set for an `=` function that declared no result type, whose `returnType` the body decides.
     *
     * It starts null rather than unit because unit is an answer, and an answer here would be wrong
     * in the one way nothing downstream could detect: the body's value would be computed and then
     * dropped at the `ret`. The type arrives when the body is resolved, which for a caller that
     * needs it sooner is what requireReturnType() forces - see resolveFunctionBody().
     *
     * Only plain free functions ever set it. An instance member takes its result from the class
     * signature and a lens from its continuation, so in both the type is known without the body.
     */
    bool inferReturn = false;

    // What the returned-borrow rule needs, recorded by the signature pass so that an inferred
    // result can be checked against it once the body has produced one - see applyReturnRoots().
    bool returnRoots = false;
    bool returnRootWritten = false;
    bool returnRootsMutable = true;
    ModuleList<ModulePtr<Arg>, false> args;
    ModuleList<ModulePtr<Block>, false> blocks;
    ModuleList<Local, false> locals;
    ast::ParsePtr<ast::Decl> ast = nullptr;

    // Set when this function implements a class signature, for diagnostics and printing.
    GlobalPtr<TypeClass> instanceOf = nullptr;
    ModuleList<TypePtr, false> instanceArgs;

    /*
     * Whether this function *is* the end of its argument's life, so the frame does not owe it a
     * drop of its own.
     *
     * The three authored cases are recognizable by `instanceOf` - `Drop::drop` receives the value in
     * order to release it, `Reclaim::reclaim` the same, and `Sink::sink` empties its source into a
     * destination so what is left is not something to release. Derived teardown glue is the fourth
     * and is *anonymous*, so it has no class to be recognized by; it used to have no need of one
     * either, because its parameter was a `%T` and a raw pointer is outside the ownership model
     * entirely.
     *
     * It stopped being a `%T` when the storage handle turned out to be an allocation per drop site
     * on a managed target - see analyze_teardown.cpp. What the flag replaces is the property that
     * fell out of the pointer: without it, glue takes a `->` parameter the frame owns, the drop pass
     * inserts a drop of the value into the function whose whole job is to drop it, and the recursion
     * is at *run* time rather than at compile time - a fixture that hangs, not a diagnostic.
     */
    bool disposer = false;

    /*
     * Set by `addAnonymousFunction`: this is reached through something other than its name.
     *
     * A class instance's implementation, a specialization, and every piece of compiler-built glue.
     * What they have in common is the property `markProgramReachable` reads it for - a reference
     * exists somewhere for the walk to find, so being unreached means nothing can ever run it, even
     * in the root module where a *named* declaration is a root of the walk rather than a finding.
     */
    bool anonymous = false;

    // Set when the function is generic: its type variables, and the class requirements its
    // signature declared or its body turned out to need. The body is resolved once against these
    // and specialized by cloning - see generic.h.
    GlobalPtr<GenEnv> gen = nullptr;
    ModuleList<ModulePtr<Function>, false> specializations;

    // Set on a specialization: the generic function it was cloned from, and for which types.
    ModulePtr<Function> specializationOf = nullptr;
    ModuleList<TypePtr, false> genericArgs;

    // Set on a lifted lambda that captured something: the static data emitted immediately in front
    // of this function's entry point, which is where its closures' teardown reads the environment
    // descriptor and the storage decision from. See ClosureHeaderLayout.
    ModulePtr<Global> closureHeader = nullptr;

    /*
     * Whether anything can still find that header at run time - see markClosureHeaders in
     * compiler/opt/opt_closure.cpp, which is the only thing that ever clears it.
     *
     * A header is reached through the code word and through nothing else, so a closure whose every
     * value is built, called and torn down where the compiler can see it has one that no path leads
     * to. `analyze_drop`'s `closureTeardown` is what makes that common rather than exotic: it
     * rewrites such a closure's drop to its environment's, by name, and the header it would have
     * gone through is then dead the moment it is written.
     *
     * True until proved otherwise, so a build that never runs the optimizer - `-no-opt`, and every
     * consumer that reaches a backend by another road - keeps emitting it.
     */
    bool closureHeaderRead = true;

    /*
     * Set when this function is a *code word*: a lifted lambda, or the thunk that makes a named
     * function into a function value.
     *
     * What it says is the convention FunValueLayout describes - the environment arrives as the
     * first parameter, whatever the signature says - and it is a property of the function rather
     * than something re-derived from where it came from, because a target may not spell that
     * convention the way the native ABI does. See codegen/js, where a code word *is* the closure
     * and the parameter is not passed at all.
     */
    bool takesEnv = false;

    /*
     * The generic function whose body lifted this one - a lens or `for` continuation written inside
     * a generic body.
     *
     * Such a lifted body names the enclosing function's type variables (a lens's continuation
     * returns one, which is what makes an `iter fn` generic without looking it), so it cannot be
     * emitted once and shared: it has to be specialized alongside whoever lifted it. `gen` is set to
     * the *enclosing* function's context rather than to one of its own, which is what makes that
     * possible - one binding list answers for both - and this records which function that context
     * belongs to, so cloning a body knows which of the symbols it walks past is one of these.
     *
     * Null for every other function, including a lifted body in a concrete one: that has no
     * variables in it and is shared by everyone.
     */
    ModulePtr<Function> liftedFrom = nullptr;

    /*
     * Design.md's Lens functions.
     *
     * `Lens` says the last parameter is the continuation and the result type is whatever that
     * continuation returns, so a call site may leave the argument out and have the rest of its own
     * block become it - see expr_lens.cpp. Nothing else about the function is special: it is an
     * ordinary generic function over the continuation's result type, and calling it with the
     * callback written out is an ordinary call.
     */
    ast::FunKind funKind = ast::FunKind::Plain;

    /*
     * Set for a lens written in the `yield` form, whose continuation parameter the signature
     * synthesized rather than the author writing it - `lens fn f(as) -> P` is
     * `lens fn f(as, body: (P) -> r) -> r`, with `yield e` a call of that parameter.
     *
     * What it decides in the body: `yield` is legal, falling off the end returns the value the
     * `yield` produced rather than the last statement's, and the one-yield-per-path rule is checked.
     */
    bool yieldForm = false;

    /*
     * Set for a *skipping* lens - Analysis-Lens.md §7.1, Design.md's Transparent and skipping
     * lenses. Its result type is not its continuation's, so the continuation runs at most once and
     * the call site has to say where the skip goes.
     *
     * Decided once, at the declaration, by the one comparison that tells the two apart, and recorded
     * rather than recomputed because what the flag really stands for is the `Try` instance the
     * signature was checked against. Never set together with `yieldForm`: the sugar has nowhere to
     * write a wrapper, so the ergonomic form is the transparent one.
     */
    bool skipping = false;

    Intrinsic intrinsic = nullptr;

    // Set instead of `intrinsic` when the signature has a `@lazy` parameter - see DeferredIntrinsic.
    DeferredIntrinsic deferredIntrinsic = nullptr;

    U32 valueCounter = 0;
    bool resolving = false;
    bool used = false;

    // Set when something calls this generic function through the erased ABI rather than through a
    // specialization. Only then does its body reach the backend: a function every call site
    // specialized has no machine code of its own, and emitting one nothing calls would be code with
    // no reason to exist rather than a fallback.
    bool genericallyUsed = false;

    // Set while this function is being cloned for one set of type arguments, so that a request to
    // clone it again for different ones is recognized as polymorphic recursion instead of
    // instantiating forever.
    bool instantiating = false;

    /*
     * `@inline` and `@noinline`, as written on the declaration - see readInlineAttribute.
     *
     * Two different kinds of thing, which is why they are two flags rather than one enum. `noInline`
     * is a *directive*: not inlining is always possible, so the optimizer honours it without
     * exception. `inlineHint` is a *hint*: it raises the budget compiler/opt weighs the callee
     * against, and a callee the pass declines structurally is still declined. Design.md says so
     * where the attributes are documented, because an attribute that silently means less than it
     * says is worse than one that does nothing.
     *
     * On the declaration rather than at the call site, on the same terms as `@platform` and the
     * argument flattening rule: a property read from the declaration is one two separate
     * compilations agree about without seeing each other.
     */
    bool inlineHint = false;
    bool noInline = false;

    // A class function's declared signature. It has arguments and a return type but no body and
    // never will: it exists so that selection has something to match against, and is the one
    // kind of Function that must not reach printing or lowering.
    bool signature = false;

    // What callers may assume about this function, computed by the ownership passes and read by
    // every call site of it. See FunctionSummary.
    FunctionSummary summary;
};

/*
 * The storage roots one instruction names, as the values whose use lists record them.
 *
 * A place rooted in a *local* is a use of the `Alloc` that gave the local its storage - see
 * `addPlaceUse` in resolve/block.cpp, which is what makes "every access to this local" answerable by
 * walking one use list. That use has no operand slot holding it: the root is a local index, and the
 * Alloc is reached through the function's local table.
 *
 * Which is why this is separate from `mapOperands` rather than part of it. A *rewrite* must not
 * touch the root - pointing it somewhere else is not something a place can express - while a *use
 * count* must, or an erased instruction leaves a reader the Alloc still believes in. Erasing the
 * redundant store in opt_place.cpp is exactly that case.
 *
 * Together with `mapOperands`, this is the whole of what `Block::add` records a use for, which is
 * the statement `verifyFunction` checks a use list against.
 */
template<class F>
inline void eachPlaceRootValue(ModuleBase base, Function& function, const Value& instruction, F&& f) {
    eachPlace(instruction, [&](const Place& place) {
        if(place.root != PlaceRoot::Local) return;
        if(place.local >= function.localCount()) return;

        if(auto storage = function.localAt(base, place.local).value) f(storage);
    });
}

/*
 * What one cell of a compiler-built table holds.
 *
 * A table is built as a list of these and nothing else - no bytes, no offsets, no byte order. That
 * is the whole of what makes it target-neutral: which *position* a slot ends up at is a question
 * only the reader can answer, and the two readers disagree completely. Native wants a byte offset
 * into a blob whose addresses are eight bytes wide in the target's endianness; JS wants an element
 * of an array, where an address is a name and has no width at all. Writing native bytes here and
 * letting JS read them back at native offsets - which is what this used to do - made every JS table
 * a padded transcription of an x64 memory image, and made "add a target" mean "reinterpret somebody
 * else's bytes".
 *
 * So resolve states the slots, `repr/table.h` turns them into native offsets and relocations, and
 * `codegen/js` turns them into array elements. The numberings in witness.h are what emitted code is
 * compiled against, and they are the only thing the two materializations share.
 */
enum class TableCell: U8 {
    // A plain number, in `value`.
    Int,

    // An interned type or class, as its region offset. Distinguished from Int because a dump can
    // then name what one refers to instead of printing a region offset, which would make every
    // fixture holding a descriptor churn whenever an unrelated declaration moved.
    Type,
    Class,

    /*
     * How wide a type is, as `metric` of the type in `value` - the table-slot form of a TypeMetric.
     *
     * The number itself is deliberately not here, for exactly the reason InstTypeMetric exists: a
     * descriptor's size is the *emitting target's* answer, and the two targets disagree about every
     * type. Resolve builds one descriptor per type and says which measurement each slot holds;
     * whichever backend materializes it fills in its own numbers. Writing native sizes at build time
     * made a descriptor a native artifact that the JS backend then read as though it described JS
     * values, which was the last thing in resolve that knew how wide anything was.
     */
    Metric,

    // The address of a function or of another table, left for the materializer to name - on native
    // it becomes a LowerDataRelocation, and on JS the emitted name itself. Null where the slot is
    // deliberately empty, which for a lifecycle slot means "nothing to do", never "unavailable".
    Function,
    Global,
};

struct TableSlot {
    TableCell kind = TableCell::Int;

    // Which measurement, for a Metric cell. Ignored by every other kind.
    TypeMetricKind metric = TypeMetricKind::Size;

    // The number, or the interned type or class, as its region offset.
    U32 value = 0;

    // At most one of these, and only for the matching kind.
    ModulePtr<Function> function = nullptr;
    ModulePtr<Global> global = nullptr;
};

// Whether a cell holds a target address rather than a 32-bit word. The only thing about a slot that
// a layout has to know, which is what keeps every layout rule out of resolve.
inline bool isAddressCell(TableCell kind) {
    return kind == TableCell::Function || kind == TableCell::Global;
}

/*
 * One interned runtime generic environment.
 *
 * Keyed by the callee and its type arguments, which is what decides every slot: the descriptors are
 * derived from the arguments, and the witnesses from the arguments plus the instances visible where
 * the environment is built. A linear scan is right here for the same reason it is for instances -
 * one generic function called at three argument lists has three of these, not three thousand.
 */
struct InternedEnv {
    ModulePtr<Function> callee;
    TypeList args;
    ModulePtr<Global> env;
};

// One interned class method table, keyed by the class and the types it was selected at - which is
// what decides both the instance and every thunk in it. See classWitnessFor.
struct InternedWitness {
    GlobalPtr<TypeClass> typeClass;
    TypeList args;
    ModulePtr<Global> witness;
};

// One interned field accessor pair, keyed by the owner and the field name - which together decide
// both accessors and the two descriptors beside them. See propertyWitnessFor.
struct InternedProperty {
    TypePtr owner = nullptr;
    StringId field = 0;
    ModulePtr<Global> witness = nullptr;
};

/*
 * A module-level storage slot.
 *
 * A global is storage nothing owns and every function can reach, which is why the language keeps
 * it to what a runtime genuinely needs: the Native heap's bump pointer has to survive between two
 * calls to allocateHeap, and there is nowhere else for it to live.
 *
 * Its initializer is a constant rather than an expression, because there is no program point at
 * which module-level code would run. A scalar's bytes are its value; anything larger starts as
 * zeroes, which is what a pointer table or a control block wants anyway.
 *
 * `contents` is the exception, and it exists for the tables the *compiler* writes rather than the
 * program: a TypeDesc, a class witness, a property witness. Those are constants of a shape no
 * source type describes and they contain addresses, so they carry their own bytes and their own
 * relocations instead of being described by `type` and `initial`.
 */
struct Global {
    Global(Module* module, StringId name): module(module), name(name) {}

    Module* module;
    StringId name;
    TypePtr type = nullptr;
    LocationId source = kNullLocation;

    // The scalar constant this starts at, in the bit pattern of its own type. Aggregates leave it
    // zero and are emitted as zeroed storage of their Repr's size.
    U64 initial = 0;

    /*
     * Set for a compiler-built constant table. When present it is the whole of the global's
     * storage: `type` and `initial` say nothing about one.
     *
     * Slots rather than bytes, and that is the point - see TableCell. What each slot means is
     * decided here; where it lands, how wide it is and which end of it comes first are decided by
     * whichever backend is about to emit the table, because those are the only three questions whose
     * answers differ between them.
     */
    ModuleList<TableSlot, false> table;
    bool isTable = false;

    /*
     * Set by `addAnonymousGlobal`: nothing in the source can name this.
     *
     * Every compiler-built table - a witness, a type descriptor, a closure header. Read by
     * `markProgramReachable` on the same terms as `Function::anonymous`: such a table is reached
     * through a reference the walk can find, so being unreached means nothing can ever read it, even
     * in the root module where a *declared* global is a root of the walk rather than a finding.
     */
    bool anonymous = false;

    /*
     * The bytes of a string literal - Implementation-String.md part 9, and the second exception to
     * "an initializer is a constant".
     *
     * A blob rather than a table, because there is nothing in it to describe: a table's slots exist
     * so that a backend can decide how wide an address is and which end of it comes first, and a
     * literal's bytes are already the encoding the target asked for. Resolve encodes the decoded
     * scalar sequence into the target's native unit - UTF-8 here, since a JS build never makes one
     * of these at all - and what a backend does with them is copy them.
     *
     * Non-empty is the flag, in the same way `isTable` is one for the other exception. When it is
     * set it is the whole of the global's storage, and `type` and `initial` say nothing about it.
     */
    ByteBuffer literalBytes;

    /*
     * Set when this table is not module-level storage at all, but the bytes immediately in front of
     * a function's entry point - a closure header. It is still a global in every other respect: it
     * has a name, it holds relocations, and a table naming it names it by that name.
     *
     * What changes is where the bytes go, and that only the code generator can honour: the header
     * has to be at a fixed negative offset from the entry point, so it is emitted with the function
     * rather than into the module's data.
     */
    ModulePtr<Function> prefixOf = nullptr;

    bool mut = false;
    bool used = false;
};

// One module made visible in another. `include`/`exclude` are the parsed symbol lists; an empty
// `include` means everything the module exports.
struct Import {
    Module* module = nullptr;
    StringId localName = 0;
    Array<StringId> include;
    Array<StringId> exclude;
    bool qualified = false;
};

struct Module {
    Module(Program& program, StringId name, ast::ParseBase parse);

    Function* addFunction(StringId name, LocationId source);
    Global* addGlobal(StringId name, LocationId source);
    Block* entry(Function& function);

    // True when `name` may be looked up in this module from outside it, per one import's
    // include/exclude lists. Symbol visibility is checked here so that every lookup path
    // applies the same rule.
    static bool visible(const Import& import, StringId name);

    Program& program;
    Context& context;

    // Both regions belong to the program rather than to one module: a type resolved in Core has
    // to be the same TypePtr everywhere, and a call from a user module to a Core function has to
    // name the same ModulePtr<Function> its own calls do.
    Region<GlobalRegion>& types;
    Region<ModuleRegion>& arena;
    ScalarTypes& scalar;
    CoreClasses& coreClasses;

    StringId name;
    ast::ParseBase parse;
    Array<Import> imports;

    HashMap<StringId, TypePtr> namedTypes;
    HashMap<StringId, TypeAlias> aliases;
    HashMap<StringId, ConstructorRef> constructors;
    HashMap<StringId, ModulePtr<Function>> functions;
    // How many string literals this module has emitted a global for, which is what makes each of
    // their names unique - see ExprResolver::resolveString.
    U32 stringLiteralCount = 0;

    HashMap<StringId, ModulePtr<Global>> globals;
    HashMap<StringId, GlobalPtr<TypeClass>> classes;
    HashMap<StringId, U8> operatorPrecedence;

    // Class functions and instances are scanned rather than hashed: a name may belong to several
    // classes, and an instance is found by class and argument types rather than by name. Both
    // lists are small enough that the linear scan is not worth avoiding.
    Array<ClassFunRef> classFunctions;
    Array<ModulePtr<ClassInstance>> instances;

    ModuleList<ModulePtr<Function>, false> functionOrder;
    ModuleList<ModulePtr<Global>, false> globalOrder;

    // The module the program was asked to compile. Its functions are emitted whether or not
    // anything calls them; every other module contributes only what is reached.
    bool root = false;

    /*
     * How far this module's declarations have got.
     *
     * A module is interned before its declarations are resolved, so an import that reaches one
     * already on the stack finds a Module that exists and is empty - and the signatures resolved
     * against it would see whichever of its declarations happened to come first. That is not a
     * partial answer, it is an order-dependent one: the same two files compile to different
     * programs depending on which was named to the compiler.
     *
     * So the state is recorded rather than inferred from whether the module is present, and
     * resolveImports rejects an import of a module in Resolving.
     */
    enum class DeclState : U8 {
        Unresolved, /// Interned, with nothing resolved into it yet.
        Resolving,  /// Its declarations are being resolved further down the current import chain.
        Resolved,   /// Every declaration exists, so its signatures can be depended on.
    };

    DeclState declState = DeclState::Unresolved;
};

// Supplies the parsed source of an imported module. The resolver asks for a module the first
// time an `import` names it and never twice.
struct ModuleProvider {
    virtual ~ModuleProvider() = default;
    virtual ast::Module* getModule(StringId name) = 0;
};

struct Program {
    explicit Program(Context& context, Size typeMemory = 4 * 1024 * 1024, Size irMemory = 16 * 1024 * 1024);
    ~Program();

    Module* addModule(StringId name, ast::ParseBase parse);
    Module* findModule(StringId name);

    Context& context;
    Region<GlobalRegion> types;
    Region<ModuleRegion> arena;
    ScalarTypes scalar;
    CoreClasses coreClasses;

    // Numbers the literal variables of the whole program, so that two `?n` in one diagnostic are
    // never the same name for different literals.
    U32 literalCounter = 0;

    Array<Module*> modules;
    GlobalList<GlobalPtr<TupType>> tupleTypes;
    GlobalList<GlobalPtr<PtrType>> pointerTypes;

    // The `@bits(n)` refinements, interned per unrefined type and width so that the `Id` two modules
    // write is one TypePtr - which is what keeps sameType() pointer equality for them too.
    GlobalList<GlobalPtr<IntType>> refinedIntTypes;
    GlobalList<GlobalPtr<BorrowType>> borrowTypes;
    GlobalList<GlobalPtr<FunType>> funTypes;

    // `[T *n]`, interned on the element type and the length together - Implementation-Containers.md
    // §6. Nothing here is per-module: two modules writing `[Int *4]` name one type, which is what a
    // signature agreeing across a module boundary needs.
    GlobalList<GlobalPtr<ArrayType>> fixedArrayTypes;

    // Instantiations created before the declaration they came from had been read, waiting for
    // their constructor contents. Drained by completePendingInstances().
    Array<GlobalPtr<RecordType>> pendingInstances;

    // The synthesized derived teardown glue, interned per type and per half. Registered before its
    // body is built, so a type reachable from itself terminates instead of generating glue forever.
    HashMap<U32, ModulePtr<Function>> dropGlue;
    HashMap<U32, ModulePtr<Function>> reclaimGlue;

    // The same glue for a function type with the header test left out, for the drop sites that can
    // prove it - see funTeardownKnownHeader and devirtualizeClosureDrop. Only the function types
    // some site proved it for are in here.
    HashMap<U32, ModulePtr<Function>> dropGlueKnown;
    HashMap<U32, ModulePtr<Function>> reclaimGlueKnown;

    /*
     * The *erased* entry point of a teardown half, interned the same way - see teardownEntryFor.
     *
     * A teardown's own signature takes its subject by `->`, which is what the concrete drop sites
     * that know the type call. A descriptor slot cannot: erased code holds storage and a slot has
     * one signature for every type that might fill it, so what a slot holds is a `%T` entry that
     * drops through the address it was handed.
     *
     * Only the types that actually reach a descriptor get one, which is why this is keyed the same
     * way and filled from typeDescFor rather than beside the glue. A program with no erased generics
     * has none of these at all.
     */
    HashMap<U32, ModulePtr<Function>> dropEntry;
    HashMap<U32, ModulePtr<Function>> reclaimEntry;

    HashMap<U32, ModulePtr<Function>> moveInitGlue;
    HashMap<U32, ModulePtr<Function>> copyInitGlue;

    // The reclaim a closure whose environment is heap-placed runs, keyed by the environment type -
    // see closureReleaseFor. Only the environment types that turned out to need one are in here.
    HashMap<U32, ModulePtr<Function>> closureRelease;

    // The teardown a type with nothing to run gets, so that a descriptor's lifecycle slots are
    // always callable - see emptyTeardown.
    ModulePtr<Function> emptyTeardown = nullptr;

    // The instances of TrivialCopy and TrivialSink the compiler answers structurally, interned per
    // (class, type). See structuralInstance in name.cpp.
    HashMap<U64, ModulePtr<ClassInstance>> structuralInstances;

    // The runtime half of the generic model, interned per type - see witness.h. A TypeDesc is
    // built the first time something generic needs to know about a type it cannot see.
    HashMap<U32, ModulePtr<Global>> typeDescs;

    // The runtime environments, interned per callee and type argument list - see genEnvFor.
    Array<InternedEnv> genEnvs;

    // The class method tables, interned per class and argument list - see classWitnessFor.
    Array<InternedWitness> classWitnesses;

    // The field accessor tables, interned per owner and field name - see propertyWitnessFor.
    Array<InternedProperty> propertyWitnesses;

    // The glue that lets a plain function be a function value, interned per function: one word of
    // adapter that drops the environment every callable is handed. See expr_fun.cpp.
    HashMap<U32, ModulePtr<Function>> functionThunks;

    // Numbers the lifted lambda bodies of the whole program, so that two of them are never printed
    // or linked under one name.
    U32 lambdaCounter = 0;

    /*
     * Every name some signature in the program declares a `@lazy` parameter for.
     *
     * A call site has to know which of its arguments to leave unevaluated *before* it resolves any
     * of them, which is before it knows which overload it is calling - so the question is asked of
     * the name rather than of the callee, at every call in the program. This is what keeps that
     * from costing an overload-set walk each time: the set has three names in it for a program
     * that only uses Core's, so the answer for everything else is one hash lookup.
     *
     * Registered by resolveSignature, which is the one route both a plain function and a class
     * signature take.
     */
    HashSet<StringId> lazyNames;

    /*
     * Whether a concrete generic call site becomes a specialization or an erased call.
     *
     * Both forms are first-class outputs (Design.md's "Generic and specialized code"), and the
     * honest test that they agree is compiling the same program each way and comparing what it
     * does - which is what `Generic` is for. `Always` is the default because a specialization is
     * faster wherever it is available; nothing about correctness depends on the choice.
     */
    enum class Specialization: U8 {
        Always,
        Generic,
    };

    Specialization specialization = Specialization::Always;

    /*
     * Whether the optimizer has run over this program - see compiler/opt.
     *
     * One target consumes one resolved program, because `@platform` selects declarations during
     * resolution and a JS build and a native build therefore do not share one. The optimizer
     * rewrites the program in place against the target that asked for it, so this says out loud
     * that a second target's request would be answered with the first target's IR, rather than
     * leaving the two to be the same by luck.
     */
    bool optimized = false;

    /*
     * Whether any function value in this program can carry a teardown at all - see
     * markClosureHeaders, which is the only thing that ever clears it.
     *
     * A teardown is found through the closure header a code word leads to, and only a *lambda* has
     * one: the thunk that makes a plain function into a function value carries a null environment
     * and no header, by construction. So a program whose every function value is a thunk - or whose
     * every lambda captured nothing droppable, or had its header proved unreachable - has nothing
     * for any `(a) -> b` teardown to find, anywhere, and the generic one is a call that tests a
     * property no function in the program has.
     *
     * Whole-program, and therefore not a question resolve may ask: `ownershipOf` runs while bodies
     * are still being resolved, and a lambda declared later would change an answer already given.
     * Here it is asked once, after every body exists and nothing more can be added.
     *
     * True until proved otherwise, so a build that never runs the optimizer keeps the generic form.
     */
    bool funValuesCarryTeardown = true;

    // What the ownership passes found, per function, kept for printing rather than for any later
    // stage - see analyze.h. Held behind a pointer because analyze.h is written against this
    // header rather than the other way round.
    Ptr<OwnershipResults> ownership;

    /*
     * The buffers those passes work in, which belong here rather than to one function's run.
     *
     * Every set and every row they use is reached from this and sized to the largest function seen
     * so far, so the five hundredth function analyzed allocates nothing. Held as a raw pointer with
     * an explicit teardown because what it contains is private to the passes; see analyze.h.
     */
    AnalysisScratch* analysisScratch = nullptr;

    /*
     * The lists resolution builds and throws away, kept for the length of the compilation.
     *
     * All three are borrowed by scope rather than declared where they are used, because all three
     * are built once per lookup and there are tens of thousands of lookups: the candidates every
     * instance match collects, the argument lists a constraint is substituted into, and the text an
     * instance method's name is assembled in. Pools rather than single buffers because instance
     * matching recurses while proving a head's own constraints. See Scratch.
     */
    ScratchPool<Array<ModulePtr<ClassInstance>>> instanceCandidates;
    ScratchPool<TypeList> typeLists;
    ScratchPool<ValueList> valueLists;
    ScratchPool<StringBuilder> names;

    /*
     * The runtime operations the compiler emits calls to on its own.
     *
     * These are ordinary Native functions - nothing about them is special except that a program can
     * end up calling them without having written the call, so the compiler has to be able to find
     * them by something other than name resolution at a call site. Heap-placed storage is the whole
     * of the list today: escape analysis decides an allocation cannot live on the frame, and the
     * allocation and its release then have to come from somewhere.
     */
    ModulePtr<Function> allocateHeap = nullptr;
    ModulePtr<Function> freeHeap = nullptr;

    // Native's `releaseRun` - the placement switch of Implementation-Containers.md §2. Recorded for
    // one reason: it is storage release, so an authored `Reclaim` is allowed to call it, and the
    // shape check has to be able to recognize it without matching on a name.
    ModulePtr<Function> releaseRun = nullptr;

    /*
     * Collections' `checkCondition` - `if failed then checkFailed()`, as one call.
     *
     * Recorded for the reason `allocateHeap` is, and doubly so: nothing in any program writes the
     * call, and the two things that emit one - a subscript's bounds test and a `@bits` narrowing -
     * are in different stages and must reach the same function.
     *
     * The *condition* is computed by whoever emits the check and the branch is inside this function,
     * which is what keeps a check from splitting the block it is emitted into - see
     * ExprResolver::emitCheck. Null when the checks are off, which is what makes them cost nothing.
     */
    ModulePtr<Function> checkCondition = nullptr;

    /*
     * Native's `stringLiteral` - the two words that describe a constant's bytes.
     *
     * A string literal is emitted by the resolver, which has a global's address and a byte count and
     * no call site to resolve a name through, so this is here for the reason `allocateHeap` is. Null
     * on JS, where a literal is a host string constant and there is nothing to construct.
     */
    ModulePtr<Function> stringLiteral = nullptr;

    // Collections' `newStringOfCapacity`, `pushString` and `formatBound` - the three a format
    // expression is assembled from. Here for the reason `stringLiteral` is: `"a{x}b"` is resolved
    // by the compiler, and there is no written call for name resolution to start from.
    ModulePtr<Function> newString = nullptr;
    ModulePtr<Function> pushString = nullptr;
    ModulePtr<Function> formatBound = nullptr;

    Module* core = nullptr;

    // Core's `Outcome(a, e)`, and which of its two constructors is which. Looked up once rather
    // than by name at each use for the reason the classes above are: the exit signal a continuation
    // reports back with is compiler-emitted, so nothing in the source it is emitted into names it.
    GlobalPtr<RecordType> outcomeType = nullptr;
    U16 outcomeProceed = 0;
    U16 outcomeExit = 1;

    // Where the array lives, and the generic declaration `[a]` resolves to. Both are null until
    // defineCollections has run, which is what keeps Core and Native - built before it - from
    // being handed an implicit import of a module that does not exist yet.
    Module* collections = nullptr;
    GlobalPtr<RecordType> arrayType = nullptr;

    // Native itself. Its *names* are private to whoever imports it, and its *instances* are not -
    // see findInstances. A module that never wrote `import Native` still ends up owning a `Run(a)`
    // the moment it writes an array literal, and what reclaiming one means cannot depend on whether
    // the module that has to do it happened to name the module the type came from.
    Module* native = nullptr;

    // `Host` - the JS half's intrinsics (Implementation-Containers.md §14.1). Empty on a native
    // build, since every declaration in it is `@platform(js)`. Recorded so that Collections can be
    // handed an import of it without naming a module that may have read nothing.
    Module* host = nullptr;

    /*
     * The two halves `String` was split into - Implementation-Simplification.md §17.
     *
     * `nativeText` holds the reinterpretations that say what a native string is *made of* and is
     * **not** implicitly imported, on the same terms as Native and for the same reason: forging a
     * `String` out of unvalidated bytes should take an import that says so. Empty on JS, where a
     * string is the host string and has no run to hand out.
     *
     * `text` holds the algorithms over them, and *is* implicitly imported, because a string literal
     * is grammar and what `print` and `Show` mean has to be reachable without being asked for.
     *
     * Two modules rather than one because the reinterpretation names `Array(U8)`, so it has to sit
     * above Collections - and the algorithms use it, so they have to sit above that. Null until
     * their define functions have run, which is what keeps everything built before them from being
     * handed an implicit import of a module that does not exist yet.
     */
    Module* nativeText = nullptr;
    Module* text = nullptr;

    // Native's `Run(a)` - the allocation primitive every container is built on
    // (Implementation-Containers.md §2). Recorded for the same reason the array is: `newRun` is an
    // intrinsic and an array literal builds a run directly, so both need the declaration without
    // going through name resolution in whichever module happened to write the literal.
    GlobalPtr<RecordType> runType = nullptr;

    // Native's `Flat(a)` - what a borrow of `[a]` is (Implementation-Containers.md §4). Recorded
    // because the resolver produces one wherever a signature writes `[T]` in a binding position,
    // which is a decision about the *convention* and therefore cannot be made by resolveType alone.
    GlobalPtr<RecordType> sliceType = nullptr;

    Module* root = nullptr;

    // Core and Native are parsed from source embedded in the compiler, so the program owns those
    // ASTs for as long as anything can still resolve against them.
    Array<ast::Module*> embeddedAsts;
};

// Resolves `root` and everything it imports, with Core built and implicitly imported first.
// `specialization` decides what a concrete generic call site becomes; both answers have to produce
// the same observable behaviour, which is what the fixtures compare.
/*
 * Which functions and tables the program can still reach from its root, recomputed.
 *
 * Run once at the end of resolution and again at the end of compiler/opt, because that stage is
 * where a reference stops existing: inlining a callee removes the `Call` that named it, and dead
 * value elimination can remove the `Symbol` that held one. Without a second walk the body is still
 * emitted, so inlining would be pure growth - a copy of the callee at the call site *and* the callee.
 *
 * Idempotent by construction: every `used` flag is reset from the module's root-ness before the walk
 * starts, so this answers the same question about whatever the program is now rather than adding to
 * what it answered before.
 */
void markProgramReachable(Program& program);

Ptr<Program> resolveProgram(Context& context, ast::Module& root, ModuleProvider* provider = nullptr,
                            Program::Specialization specialization = Program::Specialization::Always);

// Resolves the declarations of one already-registered module. Exposed because Core and Native are
// assembled from both parsed source and directly generated definitions. `importsResolved` is for
// a module that had to import something before its own declarations could be built - Native
// generates class instances, and the classes are Core's.
void resolveModuleDecls(Module& module, ast::Module& ast, ModuleProvider* provider, bool importsResolved = false);
bool resolveModuleBodies(Module& module);

// The returned-borrow rule, applied once the result type is known - immediately for a written
// result, a pass later for an inferred one.
void applyReturnRoots(Module& module, Function& function, LocationId source);

/*
 * The result type of `function`, resolving its body first if that is what decides it.
 *
 * Every read of `Function::returnType` that may reach a caller-visible `=` function goes through
 * here, because an inferred result is null until the body runs and a null would otherwise reach
 * type construction as though it were a type.
 *
 * Recursion is the case with no answer: a function whose result type is what its own body produces
 * has to have that body resolved to know it, and that body is the one asking. It is reported and
 * broken with unit rather than left to recurse.
 */
TypePtr requireReturnType(Module& module, Function& function, LocationId source);

// Makes every module one `import` names visible in this one, resolving each the first time it is
// named. Exposed for the same reason resolveModuleDecls is.
void resolveImports(Module& module, ast::Module& ast, ModuleProvider* provider);

// Checks each instance against its class's superclasses and resolves the module's `default`
// declarations. Both need every instance of the module to exist, so this runs after them - which
// for Core means after the generated instances, not after its source.
void checkModuleClasses(Module& module, ast::Module& ast);

// Resolves one function's body if it has not been resolved yet. Exposed because instantiating a
// generic function needs its body, which may belong to a module whose bodies have not been
// reached in program order.
bool resolveFunctionBody(Module& module, Function& function);

/*
 * The lens half of a signature - Design.md's two declaration forms, reduced to one.
 *
 * Called from resolveSignature once the written arguments exist, because which form the declaration
 * is in is a question about the last of them. It either accepts the signature as it stands (the
 * explicit-callback form) or adds the continuation parameter the `yield` form left out; a shape
 * this version does not implement is reported here and the function reverts to an ordinary one, so
 * that nothing downstream has to re-check what a lens is.
 */
void resolveLensSignature(Module& module, Function& function, GenEnv* env, ast::Decl& decl);

// That a `yield`-form lens hands over exactly once on every path that does not diverge, checked
// over the resolved body - see expr_lens.cpp.
void checkLensYields(Module& module, Function& function, Buffer<LensYield> yields, LocationId source);

// The printed name of one instance implementation: `Num(Int).+`. Instances are not addressable by
// name in source, but every function reaching the backend needs a unique one - both the ones
// resolved from source and the ones Core generates.
StringId instanceFunctionName(Module& module, TypeClass& typeClass, Buffer<TypePtr> args, StringId method);
