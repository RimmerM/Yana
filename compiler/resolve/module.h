#pragma once

#include "block.h"
#include "class.h"
#include "test.h"
#include "../compiler/builtin.h"
#include "../lower/convention.h"

namespace ast {
struct Module;
struct Decl;
struct Expr;
}

struct Program;
struct ExprResolver;
struct Deferred;
struct ResolvedArg;
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
    StringId name {};
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
 * and whether - to run it. Each deferred position of `args` carries the promise instead of a value,
 * and the expansion forces one by calling ExprResolver::force in whichever block it built for it.
 *
 * A function with one of these still has a real body, generated the ordinary way, which is what a
 * call that cannot see through it reaches. That body forces its parameter by calling the thunk;
 * this one emits a branch. The two have to agree, so they are written next to each other.
 */
using DeferredIntrinsic = ModulePtr<Value> (*)(ExprResolver& resolver, Buffer<ResolvedArg> args,
                                               TypePtr type, LocationId source, StringId name);

struct Function {
    Function(Module* module, StringId name): module(module), name(name) {}

    Block* addBlock(Module& module, StringId name = StringId());
    Arg* addArg(Module& module, StringId name, TypePtr type, LocationId source);
    U32 addLocal(Module& module, TypePtr type, StringId name, ModulePtr<Value> value,
                 ast::BindType convention = ast::BindType::Borrow, bool borrowed = false,
                 bool closureEnv = false);

    Local localAt(ModuleBase base, U32 index) { return locals.get(base, index); }
    Size localCount() { return locals.size(); }

    Module* module;
    StringId name;
    LocationId source = kNullLocation;
    TypePtr returnType = nullptr;

    /*
     * The convention what a `lens fn` or an `iter fn` hands over is received under.
     *
     * A hand-over is a binding, so it has a capability and a convention exactly as a parameter does,
     * and both are written in result position: `-> ->T` is the transfer and `-> &T` the exclusive
     * borrow. resolveSignature folds them the way `bindingType` folds `&x: T` and `x: &T` into one
     * parameter, and this is where the folded answer waits for resolveLensSignature to put it on the
     * continuation's `value$`.
     *
     * On a Function rather than read back off the declaration because an *instance* member need not
     * repeat the result its class already fixed - it takes this from the class signature alongside
     * `returnType`, which is the only way the two can agree when the instance wrote nothing.
     */
    ast::BindType handoverBind = ast::BindType::Borrow;

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

    /*
     * Whether control can come back out of a call to this function.
     *
     * A *declaration* rather than an inference, and it has to be: `checkFailed` is `exitProcess(134)`
     * followed by a `return`, so nothing about its body says it does not come back - what says so is
     * the kernel, and the only place that can be written down is beside the function that names the
     * system call. `Core.checkFailed` is the one that carries it today; see `definePreludeContainers`
     * in core.cpp, where it is set for the same reason `checkCondition` is recorded there.
     *
     * One thing reads it - `endNonReturningBlocks` in opt/opt_branch.cpp, which ends the block such a
     * call stands in. That is where the whole of §10 item 2 of test/bench/findings.md is: the bounds
     * check's abort arm stops being a predecessor of the block below the check, so that block stops
     * being a join, and the second check of the same index against the same length is then a
     * redundancy the CSE and the loop passes can see.
     *
     * It is read *before* the inliner, and that is not an ordering detail: inlining the callee is
     * what makes the call stop existing, so a round that ran after it would find nothing to read. The
     * body is still copied in afterwards, behind the terminator this leaves - which is what keeps an
     * abort arm a system call rather than an ordinary one.
     */
    bool noReturn = false;

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
     * Set on an instance's implementation of a class `iter fn` or `lens fn` - see resolveInstance,
     * which is where such a signature is desugared.
     *
     * What it decides is one rule, and the rule exists because of who cannot check it. A call site
     * that selected the instance reaches this body and reads its summary, so a continuation this
     * body kept is reported there like any other retained argument. A call site that *deferred* the
     * dispatch - a `for` loop in a generic body - names the class signature, which has no body and
     * no summary, so it has to assume. It assumes the continuation is not retained, which is what
     * the declaration already promises ("it is called, not stored, and its extent is the call"), and
     * this flag is what makes the assumption true of every implementation rather than of the ones
     * some call site happened to look at. See checkContinuationExtent.
     */
    bool classContinuation = false;

    /*
     * Set for the body a class wrote for one of its own signatures - see resolveClassDefault.
     *
     * `instanceOf` is what every other pass asks "is this a class function", and a default is the
     * one that answers no: it is generic over the class's variables rather than an implementation
     * for one assignment of them, and it is specialized per instance the first time a call reaches
     * it. Which is a distinction with no consequence anywhere until a rule has to hold for
     * everything a *deferred* dispatch can land on - and a slot the instance left empty holds the
     * default, so it can land there. See checkClassBorrows.
     */
    bool classDefault = false;

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

    /*
     * Whether the declaration wrote `pub`.
     *
     * True for a function nothing declared - a lambda, a lifted continuation, an instance member,
     * a synthesized entry. None of those is reachable by name from another module in the first
     * place, so the marker on one would be a claim nothing reads: `Module::functions` is what an
     * import searches, and only a written top-level `fn` is entered in it.
     */
    bool exported = false;

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

    /*
     * `@convention(clobber)`, as written on the declaration - see readConventionAttribute.
     *
     * A `Maybe` rather than a value defaulted to `kDefaultCallType`, because "the author did not
     * say" and "the author wrote the name the default happens to have" are different facts about a
     * declaration even where they lower to the same number. Only the first may be changed by a
     * later pass deciding a function would be better entered another way.
     *
     * On the declaration for the reason `@platform` and the inline attributes are: what convention a
     * function is entered under has to be an answer two separate compilations reach without seeing
     * each other, and a caller in another module has only the declaration to read it off.
     */
    Maybe<LowerCallType> convention = Nothing();

    /*
     * `@x86_legacy_sse` - this function's vector instructions are encoded without a VEX prefix.
     *
     * One architecture, one instruction family, one file. It exists because the SHA extension has no
     * VEX spelling in the architecture, so a function holding one of those instructions crosses
     * between prefixed and unprefixed encodings every few instructions unless the whole body is
     * unprefixed - which costs 140x on the part it was measured on. See `legacyVectorEncodings` in
     * codegen/x64/target.h, where the measurement is, and Analysis-Warts.md §7 for why this is
     * declared rather than inferred by a pass over the call graph.
     *
     * Carried through to `LowerFunction::legacyVectors`, which is what the backend reads. Ignored by
     * every other backend: LLVM picks its own encodings and inserts its own `vzeroupper`, and no
     * other target has a prefix to leave off.
     */
    bool legacySse = false;

    // A class function's declared signature. It has arguments and a return type but no body and
    // never will: it exists so that selection has something to match against, and is the one
    // kind of Function that must not reach printing or lowering.
    bool signature = false;

    // What callers may assume about this function, computed by the ownership passes and read by
    // every call site of it. See FunctionSummary.
    FunctionSummary summary;

private:
    friend struct IrEditor;

    /*
     * Repointing a slot at the value that fills it, after `addLocal` made it.
     *
     * The pairing is two fields - `Local::value` here and `Value::slot` there - and a rewrite that
     * set one of them left the other answering with a value that no longer exists. That is why it
     * is a method rather than two assignments, and why it is private: it is the sixth of the
     * two-sided structures `IrEditor` exists to move together, and the only one that used to be
     * writable from outside it. `IrEditor::setLocalValue` is how a pass says this now.
     */
    void setLocalValue(ModuleBase base, U32 index, ModulePtr<Value> value) {
        auto slot = locals.get(base, index);
        slot.value = value;
        locals.set(base, index, slot);
        if(value) base[value]->slot = index;
    }
};

/*
 * The storage roots one instruction names, as the values whose use lists record them.
 *
 * A place rooted in a *local* is a use of the `Alloc` that gave the local its storage - see
 * `addPlaceUse` in resolve/edit.cpp, which is what makes "every access to this local" answerable by
 * walking one use list. That use has no operand slot holding it: the root is a local index, and the
 * Alloc is reached through the function's local table.
 *
 * Which is why this is separate from `mapOperands` rather than part of it. A *rewrite* must not
 * touch the root - pointing it somewhere else is not something a place can express - while a *use
 * count* must, or an erased instruction leaves a reader the Alloc still believes in. Erasing the
 * redundant store in opt_place.cpp is exactly that case.
 *
 * Together with `mapOperands`, this is the whole of what `IrEditor::append` records a use for, which is
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
    // A plain number.
    Int,

    /*
     * How wide a type is, as `metric` of `metricType()` - the table-slot form of a TypeMetric.
     *
     * The number itself is deliberately not here, for exactly the reason InstTypeMetric exists: a
     * descriptor's size is the *emitting target's* answer, and the two targets disagree about every
     * type. Resolve builds one descriptor per type and says which measurement each slot holds;
     * whichever backend materializes it fills in its own numbers. Writing native sizes at build time
     * made a descriptor a native artifact that the JS backend then read as though it described JS
     * values, which was the last thing in resolve that knew how wide anything was.
     *
     * This is the one kind whose payload is a region offset, and the one kind where that is safe:
     * the offset is the *question*, and no materializer emits it - each one answers it and emits the
     * answer. See the note on TableSlot.
     */
    Metric,

    /*
     * A metric and a constant sharing one cell: `metric(type) << kPackedMetricShift | extra`.
     *
     * Exists because the two halves of such a cell have different authors. A type's alignment is the
     * emitting target's answer, like any other metric; the flags beside it are resolve's, decided
     * from ownership. Neither can be written where the other is known, so the cell carries both and
     * whoever materializes it combines them - which is the same division Metric already makes, with
     * one more thing to say.
     *
     * One cell uses it, NativeTypeDesc::kFlags, and the shift is chosen for that use: the flags
     * occupy bits 0 to 7 and every bit above them was spare.
     */
    PackedMetric,

    // The address of a function or of another table, left for the materializer to name - on native
    // it becomes a LowerDataRelocation, and on JS the emitted name itself. Null where the slot is
    // deliberately empty, which for a lifecycle slot means "nothing to do", never "unavailable".
    Function,
    Global,
};

// Where a PackedMetric's measurement starts, and therefore how many low bits are left for the
// constant beside it. Here rather than in witness.h because it is the *cell's* encoding: whoever
// writes one and whoever reads one both need it, and only one of the two knows what the bits mean.
static constexpr U32 kPackedMetricShift = 8;

/*
 * One cell, as a kind and a single word.
 *
 * The payload is one U32 rather than a field per kind because no cell has ever held two of them:
 * a number, a metric's type, a function and a global are four readings of the same word, and
 * keeping four fields made every slot sixteen bytes to carry four. The accessors below are the
 * whole interface - nothing reads `payload` directly, so a reading applied to the wrong kind is a
 * missing case rather than a plausible-looking zero.
 *
 * **No surviving cell holds a region offset.** A type descriptor used to lead with the interned
 * type it described and a class witness with its class, both as raw offsets into the compiler's
 * arena, and both backends emitted those words verbatim - which made interning order observable in
 * the output and made a JS module carry a number like `52056` that nothing ever loaded. They were
 * there for a debug cross-check that was never built. Metric is the only kind left whose payload is
 * an offset, and it never reaches output: `writeTableWords` and `tableValue` both replace it with
 * their own measurement. Anything that wants identity in a table again should carry a
 * content-derived id, not a handle.
 */
struct TableSlot {
    TableCell kind = TableCell::Int;

    // Which measurement, for a Metric or PackedMetric cell. Ignored by every other kind.
    TypeMetricKind metric = TypeMetricKind::Size;

    // The constant a PackedMetric carries below its measurement. Occupies what would otherwise be
    // padding, which is why the cell that needed it cost nothing to allow.
    U16 extra = 0;

    // Read through the accessors below, never directly - see the note above.
    U32 payload = 0;

    static TableSlot intOf(U32 value) {
        return TableSlot { TableCell::Int, TypeMetricKind::Size, 0, value };
    }

    static TableSlot metricOf(TypePtr type, TypeMetricKind metric) {
        return TableSlot { TableCell::Metric, metric, 0, U32(type) };
    }

    static TableSlot packedMetricOf(TypePtr type, TypeMetricKind metric, U16 extra) {
        return TableSlot { TableCell::PackedMetric, metric, extra, U32(type) };
    }

    static TableSlot functionOf(ModulePtr<Function> function) {
        return TableSlot { TableCell::Function, TypeMetricKind::Size, 0, U32(function) };
    }

    static TableSlot globalOf(ModulePtr<Global> global) {
        return TableSlot { TableCell::Global, TypeMetricKind::Size, 0, U32(global) };
    }

    U32 value() const { return kind == TableCell::Int ? payload : 0; }

    TypePtr metricType() const {
        return kind == TableCell::Metric || kind == TableCell::PackedMetric ? TypePtr(payload) : nullptr;
    }

    ModulePtr<Function> function() const {
        return kind == TableCell::Function ? ModulePtr<Function>(payload) : nullptr;
    }

    ModulePtr<Global> global() const {
        return kind == TableCell::Global ? ModulePtr<Global>(payload) : nullptr;
    }
};

// Two bytes and a word, and it has to stay that way: a slot is the unit the arena holds tables in,
// and the four-field form this replaced was sixteen bytes to carry the same four.
static_assert(sizeof(TableSlot) == 8, "a table slot is a kind, a metric and one word");

// Whether a cell holds an address rather than a 32-bit word. The only thing about a slot that a
// layout has to know, which is what keeps every layout rule out of resolve.
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
    StringId field {};
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

    /*
     * The constant this starts at - see `ConstValue`, which is the whole of what may be written.
     *
     * A tree rather than bytes, for the reason that header gives: what a field's offset is and which
     * end of an address comes first are the emitting target's answers, so each target lays this out
     * with the same Repr it lays every other value of the type out with.
     *
     * Null for a global with no constant at all, which is one of two things: a compiler-built table
     * or blob, whose storage the two fields below are, or a `dynamic` global, whose value is
     * produced by the entry sequence and whose storage therefore starts at the zero of its type.
     */
    ModulePtr<ConstValue> initial = nullptr;

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

    /*
     * Set where this global's value is produced by code rather than written as a constant - a
     * root-module `let` whose initializer is an ordinary expression, run by the program's entry
     * sequence (Analysis-Initialization.md stage B).
     *
     * Four things read it. `initial` says nothing about one of these, so its storage starts at the
     * zero of its type on *both* targets rather than at whatever the target's uninitialized memory
     * happens to be - which is what makes a premature read read the same thing everywhere.
     * `globalValue` will not fold one, because there is no constant to fold to. The entry sequence
     * is where its declaring `Init` is emitted, which is what keeps the drop pass from pre-dropping
     * the zeroes it replaces. And `isWritten` below, which is the one an emitter asks.
     */
    bool dynamic = false;

    // Whether the *language* lets the program assign to this - `let &`.
    bool mut = false;

    /*
     * Whether anything writes this global's storage, which is what an emitter has to know and is not
     * the same question `mut` answers.
     *
     * A `dynamic` global is immutable to every expression that names it and is still written once, by
     * the entry sequence that produces its value. Emitting one as read-only data is not a missed
     * optimization but a wrong program: LLVM's `constant` is a promise, so every read of such a
     * global folded to the zero its storage was declared with and the store was dropped as dead. A
     * compiler-built table, a string literal's bytes and a constant `let` are the globals for which
     * the promise is true.
     */
    bool isWritten() const { return mut || dynamic; }

    // Whether the declaration wrote `pub`. False for every compiler-built global - a witness, a
    // descriptor, a string literal's bytes - which is right for the same reason it is on Function:
    // nothing in any source names one.
    bool exported = false;

    /*
     * A question about the target rather than storage - `@target(byteOrder)`, and so far only that.
     *
     * A constant whose value this stage does not know. Resolve serves every target at once, so the
     * byte order is no more answerable here than a `Size`'s width is, and the device is the one
     * `bitWidth` already uses for that: `globalValue` answers a read of this name with an
     * `InstTypeMetric` carrying `metric` below, and the target folds it to an immediate.
     *
     * A global rather than a nullary intrinsic because a constant is what it *is*: nothing can
     * write it, nothing can observe it changing, and a name that reads as a call invites a reader
     * to wonder whether the call is elided. It occupies nothing either - no read ever reaches
     * `used`, so no storage is emitted, which is the same thing an ordinary constant `let` does.
     *
     * The type is the compiler's, not the declaration's: what a target question answers in is
     * decided by the question. See targetConstants in module_decl.cpp for the table.
     */
    bool targetMetric = false;
    TypeMetricKind metric = TypeMetricKind::Size;

    bool used = false;
};

/*
 * One statement of a module's top level, in the order it was written.
 *
 * Collected by the declaration pass and run by the synthesized entry function - see
 * `resolveEntryBody`. Only the root module has any: a library module's top-level code would have to
 * run at some point in a program's startup, and defining that point is the cross-module half this
 * deliberately does not do (Analysis-Initialization.md stage C).
 *
 * `globals` is the `let` case - one entry per name the statement declares, in the order written, so
 * the entry sequence does not have to look a name back up to find what it declared. A null entry is
 * one the declaration pass rejected. An entry that is *not* `dynamic` is a constant, and there is
 * nothing to run for it at all.
 *
 * Both lists are in the module's arena rather than on the heap, on the same terms as `functionOrder`
 * beside them: this is per-module data that lives exactly as long as the module does, so it belongs
 * in the region that is released with it and not in an allocation with its own lifetime. It also
 * makes the struct trivially copyable, which is what lets it be an element of a list in turn.
 */
struct TopLevelStmt {
    ast::ParsePtr<ast::Decl> decl = nullptr;
    ModuleList<ModulePtr<Global>, false> globals;
};

/*
 * An operator's declared fixity.
 *
 * Precedence and associativity are one fact and not two. They are declared in one line, they are
 * looked up together, and precedence climbing needs both at each rung - so a lookup that answers
 * only half of it is a lookup whose caller has to guess the rest, which is exactly how `infixr`
 * came to be parsed, recorded and then silently dropped between the parser and the resolver.
 *
 * `declared` is what makes the whole thing falsy for an operator that has no fixity, which is what
 * search() reads to mean "not in this module". Precedence 0 is a real precedence - it is where Core
 * puts the compound assignments - so the absence cannot be spelled as a value.
 */
struct OperatorFixity {
    U8 precedence = 0;
    bool right = false;
    bool declared = false;

    explicit operator bool() const { return declared; }
};

// One module made visible in another. `include`/`exclude` are the parsed symbol lists; an empty
// `include` means everything the module exports.
struct Import {
    Module* module = nullptr;
    StringId localName {};
    Array<StringId> include;
    Array<StringId> exclude;

    /*
     * The file of the importing module that wrote this, as an index into `Module::files` -
     * Analysis-Modules.md §2.1.2. An import is written in a file and is in scope for that file, so
     * a name one file imported is not visible to its siblings.
     *
     * `kEveryFile` for the implicit import of Core, which no file wrote and every file has.
     */
    U16 file = 0;

    bool qualified = false;

    static constexpr U16 kEveryFile = maxLimit<U16>;

    // Whether this import is in scope in `from` - see Module::activeFile.
    bool inScope(U16 from) const { return file == kEveryFile || file == from; }
};

struct Module {
    Module(Program& program, StringId name);

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

    /*
     * Every AST in the compilation shares one region - Context::parseRegion - so this is the same
     * base for every module, and a declaration of any file of this module is addressable through it.
     * That is what makes a module of several files cost nothing at the eighty-odd sites that
     * dereference a `ParsePtr` through here.
     */
    ast::ParseBase parse;

    /*
     * The files this module is made of, in path order - Analysis-Modules.md §2.1.
     *
     * Usually one. A grouped module has several, and the only thing that changes for the passes is
     * that each of them runs over every file before the next one starts: within a module there are
     * no imports and no exports, so two files of it are two halves of one declaration list.
     *
     * `SmallArray` for the reason `ast::ModuleGroup::files` is one, and with the same bound: this is
     * that list copied onto the resolved module, so the two should not disagree about what an
     * ordinary module holds. A `Module` is heap-allocated and never moved, so the address rule costs
     * nothing here; `fileOf` answers an index and every other reader iterates.
     */
    SmallArray<ast::Module*, 8> files;

    /*
     * What this module can see, with each entry tagged by the file that wrote it - see Import::file.
     * One array rather than one per file because the traversal in search() is over all of them at
     * once: two candidates found through two imports are ambiguous whichever files they came from,
     * and the implicit import of Core belongs to every file.
     */
    Array<Import> imports;

    /*
     * Which file of this module is being read - Analysis-Modules.md §2.1.2.
     *
     * An import is scoped to the file that wrote it, so every name lookup has to know which file it
     * is a lookup *from*. The alternative is threading that through `resolveType` and the forty-odd
     * `find*` call sites and everything that reaches them, for a fact that is in practice a
     * well-nested dynamic extent: a declaration pass reads one file at a time, and a body belongs to
     * the file its declaration was written in.
     *
     * So it is set at the few points that begin such an extent and never anywhere else. Every one of
     * them goes through `FileScope`, which restores the previous value - resolution nests, because
     * instantiating a generic resolves a body in the middle of another one.
     *
     * A module of one file never changes it, which is every module the compiler builds itself.
     */
    U16 activeFile = 0;

    /*
     * Which file a location is in, as an index into `files`.
     *
     * The declaration's own record of where it was written is the ground truth for which file it
     * belongs to, so nothing here carries a second copy of that fact to fall out of step with it.
     * Linear, and called once per declaration or body rather than once per name.
     *
     * File 0 for a location in no file of this module, which is what a generated declaration has -
     * Core's own instances carry no source at all. Those are in modules of one file, where 0 is the
     * only answer there is.
     */
    U16 fileOf(LocationId source);

    HashMap<StringId, TypePtr> namedTypes;
    HashMap<StringId, TypeAlias> aliases;
    HashMap<StringId, ConstructorRef> constructors;
    HashMap<StringId, ModulePtr<Function>> functions;
    // How many string literals this module has emitted a global for, which is what makes each of
    // their names unique - see ExprResolver::resolveString.
    U32 stringLiteralCount = 0;

    // And how many constructor-map tables it has built, for the same reason and by the same rule -
    // see emitConstructorMap. Numbered per module, because `LowerModule::globals` is one map over
    // the whole program keyed by name and two modules would otherwise collide on `map$0`.
    U32 constructorMapCount = 0;

    HashMap<StringId, ModulePtr<Global>> globals;
    HashMap<StringId, GlobalPtr<TypeClass>> classes;
    HashMap<StringId, OperatorFixity> operatorFixity;

    // Class functions and instances are scanned rather than hashed: a name may belong to several
    // classes, and an instance is found by class and argument types rather than by name. Both
    // lists are small enough that the linear scan is not worth avoiding.
    Array<ClassFunRef> classFunctions;
    Array<ModulePtr<ClassInstance>> instances;

    /*
     * Every context of this module whose head wrote a generic-parameter default.
     *
     * A default is resolved on demand - see `resolveGenDefaults` - because it may name a type
     * declared further down. But "on demand" is not a place a *diagnostic* may live: a `pub` type
     * nothing in its own module applies would have its default checked in whichever importer first
     * used it, or in none at all. So this list is walked once at the end of declaration resolution,
     * and every default is spent whether or not anybody asked. Whichever came first wins; the flag
     * on the context makes the second a no-op.
     */
    Array<GlobalPtr<GenEnv>> defaultedContexts;

    ModuleList<ModulePtr<Function>, false> functionOrder;
    ModuleList<ModulePtr<Global>, false> globalOrder;

    // The module the program was asked to compile. Its functions are emitted whether or not
    // anything calls them; every other module contributes only what is reached.
    bool root = false;

    /*
     * A module of a package this compilation is not, which that package's manifest does not export -
     * see ProjectFile::exports.
     *
     * Decided where the module is found, because that is the only place that knows *which* package
     * answered, and read at the import that named it, because that is where a person can act on it.
     * False for everything when the library draws no boundary, and false for every module of the
     * package being compiled - `base` sees all of `base`.
     */
    bool packagePrivate = false;

    /// Whether this module's files are the standard library package's - ast::ModuleGroup::library.
    /// The importer's half of the export check: a package's own modules see all of it.
    bool fromLibrary = false;

    // The statements this module's top level runs, in source order. Non-empty only for the root
    // module - see TopLevelStmt.
    ModuleList<TopLevelStmt, false> topLevel;

    /*
     * The `let`s a `.test.yana` file of this module declares - Design-Test.md §3.1, and §11.2's F5.
     *
     * A second list rather than entries in the one above, because the two are different things that
     * happen to share a shape. `topLevel` is *the program's start*: it is one file's, it may hold
     * arbitrary statements, and its order is the order the author wrote. This is a set of
     * initializers, one group per test file, run before the cases by `resolveTestEntry` and existing
     * only in a build where that entry exists at all.
     *
     * Grouped by file because the declaration passes run over `files` one at a time, so the pushes
     * arrive that way; `fileOf` recovers which is which rather than a field here repeating it.
     */
    ModuleList<TopLevelStmt, false> testTopLevel;

    /*
     * How far this module's declarations have got.
     *
     * What it used to be for was rejecting a cycle: a module was interned before its declarations
     * were resolved, so an import reaching one already on the stack found a Module that existed and
     * was empty. `Resolving` was how that was told apart from a finished one.
     *
     * It now separates the prelude from the program - Analysis-Modules.md §2.2. Core and Native are
     * Resolved before the program-wide passes start, so this is what makes them drop out of the
     * walk; everything else moves from Unresolved to Resolving to Resolved together, because that is
     * what "each pass over every module" means.
     */
    enum class DeclState : U8 {
        Unresolved, /// Interned, with nothing resolved into it yet.
        Resolving,  /// Its declarations are being resolved further down the current import chain.
        Resolved,   /// Every declaration exists, so its signatures can be depended on.
    };

    DeclState declState = DeclState::Unresolved;
};

/*
 * The extent over which one file of a module is the file being read - see Module::activeFile.
 *
 * Restores rather than clears, because these nest: resolving a body may instantiate a generic whose
 * body is in another file, or another module.
 */
struct FileScope {
    FileScope(Module& module, U16 file): module(module), previous(module.activeFile) {
        module.activeFile = file;
    }

    FileScope(Module& module, LocationId source): FileScope(module, module.fileOf(source)) {}
    FileScope(const FileScope&) = delete;

    ~FileScope() { module.activeFile = previous; }

    Module& module;
    U16 previous;
};

// Supplies the parsed source of an imported module. The resolver asks for a module the first
// time an `import` names it and never twice.
//
// A group of files rather than a file - Analysis-Modules.md §2.1. Whoever answers this is what knows
// where the files are, so it is also what decides which of them the module is made of.
struct ModuleProvider {
    virtual ~ModuleProvider() = default;
    virtual ast::ModuleGroup* getModule(StringId name) = 0;
};

struct Program {
    explicit Program(Context& context, Size typeMemory = 4 * 1024 * 1024, Size irMemory = 16 * 1024 * 1024);
    ~Program();

    // The module's files. There is no base to pass: every AST in the compilation is in
    // Context::parseRegion, so a module's `parse` is that and could not be anything else.
    Module* addModule(ast::ModuleGroup& group);

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

    // `Vec(a, n)` and `Mask(a)`, interned on the lane type, the lane count and the mask flag - the
    // same shape the fixed array above is interned in, and for the same reason.
    GlobalList<GlobalPtr<VectorType>> vectorTypes;

    // `Atomic(a)`, interned on its content alone - Analysis-Atomics.md §3.1. The same shape the two
    // above are interned in, and there is nothing else to key on: an atomic's width and alignment
    // are its content's, and the ordering is a property of each operation rather than of the
    // location.
    GlobalList<GlobalPtr<AtomicType>> atomicTypes;

    // The numbers written in the count positions above - Implementation-Const-Generics.md §2.1.
    // Interned on the value and the type it is a value of, so that the count in `[Int *4]` and the
    // one in `Vec(Float, 4)` are one TypePtr and a count position compares by pointer.
    GlobalList<GlobalPtr<ConstType>> constTypes;

    /*
     * The Core names `Vec` and `Mask` resolve by.
     *
     * These are the two type constructors that are not declarations: there is no `data Vec(a)` for
     * a lookup to find, because what one *is* is decided by the target rather than by a body. So
     * `resolveApp` compares the written name against these before it looks a type up, which is
     * Implementation-Vector.md §1.4's "named type applications resolved by the Core name rather than
     * by new grammar" - no parser change, and no record to instantiate.
     *
     * They are looked for **after** the ordinary lookup rather than instead of it, so a declaration
     * shadows them exactly as a local `Maybe` shadows Core's. That is not politeness: `Mask` is a
     * name programs already use - two fixtures in this tree declare `data Mask {bits: Int}` - and a
     * builtin that won the lookup would be a reserved word the language never announced.
     *
     * Both are interned by `definePreludeTypes` and are null until it has run.
     */
    StringId vecTypeName {};
    StringId maskTypeName {};

    /*
     * And `Atomic`, on exactly the same terms - Analysis-Atomics.md §3.1.
     *
     * A third constructor with no declaration behind it, for a different reason than the two above:
     * what an `Atomic(Int)` *is* is perfectly writable, and what cannot be written is that it is not
     * `TrivialCopy` while its content is. `resolveApp` compares against this after the ordinary
     * lookup, so a program that declares its own `Atomic` shadows it.
     *
     * Interned by `definePreludeTypes` and null until it has run. Native only: the `Atomic` module
     * declares every operation `@platform(native)`, so a JS build resolves the type and finds
     * nothing that operates on it - see §5.4.
     */
    StringId atomicTypeName {};

    // The parameter list it is applied through - `(a)`. Built beside `vectorGen` and for its
    // reason: the arity rule and the message an arity mistake gives are the general ones.
    GlobalPtr<GenEnv> atomicGen = nullptr;

    /*
     * The parameter list both of them are applied through - `(a, n: Int = 0)`.
     *
     * What a declaration would have carried, carried without one. It exists so that the arity rule,
     * the count position and the default are the *general* ones rather than a hand-written check
     * beside `resolveVectorApp`: which argument is a number is read off `GenKind::Const` exactly as
     * a `data A(width: Int)` is, and `Vec(Float)` is an application that omitted its second argument
     * exactly as `Pair(Int)` would be.
     *
     * `0` is the default because zero is already the natural form written down - `resolveVectorType`
     * treats a null count and a zero one as one question, "ask the target" - so this names a
     * sentinel that was already there and already writable rather than introducing one.
     *
     * One list for both constructors, because a mask takes the same two arguments for the same
     * reasons; `isMask` is decided by which name was written and is not a parameter.
     */
    GlobalPtr<GenEnv> vectorGen = nullptr;

    // Instantiations created before the declaration they came from had been read, waiting for
    // their constructor contents. Drained by completePendingInstances().
    Array<GlobalPtr<RecordType>> pendingInstances;

    // The synthesized derived teardown glue, interned per type and per half. Registered before its
    // body is built, so a type reachable from itself terminates instead of generating glue forever.
    HashMap<U32, ModulePtr<Function>> dropGlue;
    HashMap<U32, ModulePtr<Function>> reclaimGlue;

    // The merged walk - one InstDrop per member naming that member's merged teardown, which is what
    // a drop site actually calls. See teardownBothFor, and Teardown::Both for why the halves are not
    // asked for separately there.
    HashMap<U32, ModulePtr<Function>> teardownGlue;

    // The same glue for a function type with the header test left out, for the drop sites that can
    // prove it - see funTeardownKnownHeader and devirtualizeClosureDrop. Only the function types
    // some site proved it for are in here, and only the one half this target asks for.
    HashMap<U32, ModulePtr<Function>> teardownGlueKnown;

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
    HashMap<U32, ModulePtr<Function>> teardownEntry;

    // The wrapper that runs one half after the other, keyed by type - see teardownBothFor. Only for
    // a type whose two halves are two genuinely different *authored* answers; a type whose halves
    // are both derived gets one merged walk instead, and everything else has a single half to name.
    HashMap<U32, ModulePtr<Function>> teardownBoth;

    HashMap<U32, ModulePtr<Function>> moveInitGlue;
    HashMap<U32, ModulePtr<Function>> copyInitGlue;

    // The reclaim a closure whose environment is heap-placed runs, keyed by the environment type -
    // see closureReleaseFor. Only the environment types that turned out to need one are in here.
    HashMap<U32, ModulePtr<Function>> closureRelease;

    /*
     * The thunk a `@lazy` parameter's **constant** default is wrapped in - see makeThunk.
     *
     * Interned per default, because there is exactly one right answer per default and building it
     * per call site is one anonymous function per *assertion*: `check(x)` defaults its `@lazy`
     * message, so a fixture with twenty-one checks in it carried twenty-one identical functions
     * returning `""`. Nothing distinguishes them - a constant thunk captures nothing, names nothing
     * of the caller's, and at a concrete type is the same body for every specialization, which is
     * the same argument that lets one be built inside a generic body at all.
     *
     * Keyed by the constant, which is unique program-wide: the module arena belongs to the program,
     * so a `ModulePtr` from one module means the same thing read from another. The function lands in
     * whichever module reached the default first and every later call site names it there, exactly
     * as a call from a user module to a Core function already does.
     */
    HashMap<U32, ModulePtr<Function>> constantThunks;

    // The teardown a type with nothing to run gets, so that a descriptor's lifecycle slots are
    // always callable - see emptyTeardown.
    ModulePtr<Function> emptyTeardown = nullptr;

    /*
     * The symbol every table slot is measured from - see imageAnchor, and TableCell.
     *
     * A label and nothing else: it occupies no storage and is never read, and what a reader wants is
     * its *address*, which on x64 is one `lea r, [rip + global]` and no memory traffic at all. Null
     * where no table was built, and on a target that has no addresses to measure.
     */
    ModulePtr<Global> imageAnchor = nullptr;

    /*
     * What the program declared for each of the compiler's own roles - `compiler/compiler/builtin.h`.
     *
     * Indexed by `Builtin`, and null for every role no declaration claimed, which is the ordinary
     * case: a program that never asks about its command line declares none of them, and one built
     * for a target the role has no meaning on cannot. Filled by `readBuiltinAttribute` as the
     * declarations are read, and carried into the lower module beside `imageAnchor` - see
     * lowerProgram, which is the last point at which both halves of a global are in hand.
     */
    ModulePtr<Global> builtins[kBuiltinCount] = {};

    // The instances of TrivialCopy and TrivialSink the compiler answers structurally, interned per
    // (class, type). See structuralInstance in name.cpp.
    HashMap<U64, ModulePtr<ClassInstance>> structuralInstances;

    /*
     * The functions declared under a type's namespace - `String.reserve` for `String` - keyed by
     * (declaring type, last segment) and holding the whole name the declaration was written under.
     *
     * Program-wide rather than per module because a type has exactly one declaring module and
     * therefore one namespace, which is the rule registerNamespace enforces: `x.f(y)` must not mean
     * different things in two files. What it holds is a *name* rather than a function, so that the
     * dot-call resolves it through findFunction like any other written name - `pub`, the import
     * lists and `hiding` then decide exactly as they do for the qualified spelling, and none of
     * that has to be restated here. See findTypeMethod.
     */
    HashMap<U64, StringId> typeMethods;

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
     * Whether every module's declarations have been read - see resolveProgram, which is the only
     * thing that sets it.
     *
     * `ownershipOf` caches its answer on the type, and that answer is the one classification a
     * *later* declaration can change: writing `instance Drop(T)` or `instance Reclaim(T)` is exactly
     * the statement that T's structural answer was wrong. So a cached answer is only sound once no
     * further instance can appear, and until then the classification is recomputed rather than
     * remembered.
     *
     * The window was real rather than hypothetical, and it is worth keeping the case that made it
     * so: the prelude used to be six modules, each resolving its own bodies inside its define step
     * because the next one needed it finished, so a body was resolved before the modules above it
     * had declared anything. `instance Reclaim(String)` was in `Text` and the first thing to ask a
     * `String` for its ownership was `NativeText`'s `stringLiteral`, two steps earlier: the answer
     * "nothing to release" was cached there and every string temporary in every program leaked for
     * it. The prelude is two modules resolved together now (Analysis-Modules.md §2.4), which closes
     * that particular window and does not change the rule - a program's own modules still declare
     * instances after the prelude's bodies exist.
     */
    bool declarationsComplete = false;

    // The working state of ownershipOf's fixpoint over recursive types - see OwnershipSolve, which
    // says why it is here and not on the Type. Held across queries so that the window above, in
    // which no answer may be remembered, does not reallocate a map per query.
    OwnershipSolve ownershipSolve;

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
     * Every class instance in the program, grouped by the class it implements.
     *
     * findInstances used to answer this by walking every module's own `instances` list and filtering
     * it by class, which is O(instances in the whole program) at every instance lookup - and there
     * is one lookup per class-dispatched call in every body resolved. It was the compiler's single
     * hottest function by a wide margin.
     *
     * A mirror of the module lists rather than a replacement for them: a module still owns its own
     * instances, because emission walks a module and coherence is not what decides which module an
     * instance belongs to. registerInstance() writes both, and is the only way either is written.
     *
     * The rows are inline, on the ordinary terms of compiler/util/README.md: there is one per class
     * in the program and most classes have a handful of instances, so a plain Array here would trade
     * the lookup this exists to save for an allocation per class. A SmallArray is safe as a hash map
     * value - the inline buffer is not pointed at from inside the object, so a rehash relocates one
     * correctly.
     */
    HashMap<U32, SmallArray<ModulePtr<ClassInstance>, 8>> instancesByClass;

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
     * The same, with the site's module, line and column as three more arguments - the declaration
     * `-check-locations` selects. Null in a build whose `Core` does not declare one, which is what
     * keeps the flag from being a hard dependency on a library version.
     */
    ModulePtr<Function> checkConditionAt = nullptr;

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

    /*
     * `String.reserve`, and the name `++=` - the two a format written as the right operand of `++=`
     * needs on top of those, Design-Test.md §11.1's P2.
     *
     * A format expression already has a sink and a fresh `String` is merely the case where nothing
     * else supplied one, so `out ++= "a{x}b"` hands it the string that exists: the summed extent
     * becomes a reservation on that string rather than an allocation, and the appends run against
     * the same buffer.
     *
     * The name rather than the function, because what is recognized is the *shape* of the written
     * expression and not a call to be redirected - see resolvePrecedence, which never emits one.
     * Null in a build whose prelude has no `++=`, where the shape is simply not recognized and the
     * ordinary function runs.
     */
    ModulePtr<Function> reserveString = nullptr;
    StringId appendAssign = StringId();

    /*
     * `-`, so that `-1` can be recognized as a written negative number rather than resolved as a
     * negation applied to a positive one.
     *
     * The name and not the function, for `appendAssign`'s reason: what is recognized is the shape of
     * what was written, and resolvePrefix never emits a call for it. A build whose prelude has no
     * `-` simply does not recognize the shape.
     */
    StringId negate = StringId();

    Module* core = nullptr;

    /*
     * The tests of this compilation, and the two `Test` declarations the entry is built out of -
     * Design-Test.md §11.2's F1. Empty in every build without `-test`.
     *
     * A struct of its own, in resolve/test.h, because it is the whole of what the compiler knows
     * about tests and none of it is anything else's business: what fills it is `readTestAttribute`
     * and what reads it is `resolveTestEntry`.
     */
    TestRegistry tests;

    // Core's `Outcome(a, e)`, and which of its two constructors is which. Looked up once rather
    // than by name at each use for the reason the classes above are: the exit signal a continuation
    // reports back with is compiler-emitted, so nothing in the source it is emitted into names it.
    GlobalPtr<RecordType> outcomeType = nullptr;
    U16 outcomeProceed = 0;
    U16 outcomeExit = 1;

    // What the generic declaration `[a]` resolves to. Null until the prelude's container hook has
    // run, which is once every file of Core has been through the declaration passes.
    GlobalPtr<RecordType> arrayType = nullptr;

    // And the map, which `[k: v]` and `[K: V]` resolve to - Implementation-Map.md §7. Recorded on
    // the same terms as `arrayType` and for the same reason: the spelling is grammar and what it
    // means is a library record, so the literal needs a pointer to the declaration rather than a
    // name to look up. Null in a build whose Core declared no `Map`, which is what the literal's
    // diagnostic reports.
    GlobalPtr<RecordType> mapType = nullptr;

    // Native itself. Its *names* are private to whoever imports it, and its *instances* are not -
    // see findInstances. A module that never wrote `import Native` still ends up owning a `Run(a)`
    // the moment it writes an array literal, and what reclaiming one means cannot depend on whether
    // the module that has to do it happened to name the module the type came from.
    Module* native = nullptr;

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

    /*
     * Where a finished program starts - Analysis-Initialization.md stage B.
     *
     * The root module's top-level statements are the body of one synthesized function, and `main`,
     * where it is declared, is called at the end of it; the result of that function is the program's
     * status. A root module with no top-level statements has nothing to synthesize, so this is
     * `main` itself and every existing program is unchanged by the rule.
     *
     * Null where the root module declares neither, which is a library rather than a program: the JS
     * file then ends with no call and the native path reports that there is no entry point, which is
     * the same answer both gave before there was a name for the question.
     */
    ModulePtr<Function> entry = nullptr;

    // Core and Native are parsed from `lib/`, not from the module map, so the program owns those
    // ASTs for as long as anything can still resolve against them - and the groups laid over them,
    // since `lib/` has no module map to hold those either.
    Array<ast::Module*> embeddedAsts;
    Array<ast::ModuleGroup*> embeddedGroups;
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
/*
 * Which functions and globals a finished program can arrive at, recomputed from its roots.
 *
 * `excluded`, where it is given, is the set of functions a backend has decided it cannot emit -
 * every one of them by `ModulePtr<Function>` as a `U32`. An edge into one is not an edge: its body
 * is not walked, so a function or a global that only it could reach stops being reachable.
 *
 * That is what lets a target drop the other target's runtime without naming any of it. The JS
 * backend cannot express a syscall, so it cannot have the heap allocator; the size-class arithmetic
 * beside the allocator is perfectly expressible and is reached from nowhere else, and asking this
 * again with the allocator removed is the whole of how it goes away.
 */
void markProgramReachable(Program& program, const HashSet<U32>* excluded = nullptr);

/*
 * `testRoots` names modules that are part of the compilation without anything importing them -
 * Design-Test.md §3.4, and tests are the only thing that needs it.
 *
 * A program is otherwise exactly what its root reaches, and that stays the rule: a test module is
 * not reached from the program it tests, because the dependency runs the other way. Adding them
 * here rather than teaching the discovery walk about test roots keeps "what is in this program" one
 * question with one answer, asked of a list the driver builds.
 *
 * Empty in every build without `-test`, and even under `-test` it is only the modules that actually
 * declare a `@test` - see `moduleDeclaresTests`, which is what keeps a test build from being "every
 * file the compiler was pointed at, resolved". Everything a listed module imports is reached from
 * it by the ordinary walk.
 */
Ptr<Program> resolveProgram(Context& context, ast::ModuleGroup& root, ModuleProvider* provider = nullptr,
                            Program::Specialization specialization = Program::Specialization::Always,
                            Buffer<ast::ModuleGroup*> testRoots = {});

// A root of one file, which is what every driver that resolves a source string rather than a source
// tree has. The group is built here so that nothing else has to.
Ptr<Program> resolveProgram(Context& context, ast::Module& root, ModuleProvider* provider = nullptr,
                            Program::Specialization specialization = Program::Specialization::Always);

/*
 * The prelude: Core and Native, resolved together - Analysis-Modules.md §2.4.
 *
 * The two are a cycle - Core is written over raw pointers and the heap, Native is written over
 * `Int` and the classes - so they go through the same phase-ordered passes the program-wide walk
 * uses, over both modules at once. What is different is that they are also *assembled*: the
 * primitives before the passes and the generated instances after them come from the compiler
 * rather than from a file, so this is that pass sequence with a hook at each end.
 *
 * Both are Resolved when it returns, which is what makes them drop out of the program-wide walk.
 */
void definePrelude(Program& program);

/*
 * Declaration resolution for the whole program at once - Analysis-Modules.md §2.2 and §1.4.
 *
 * Ten passes, each run over every module before the next one starts, which is what makes a cycle in
 * the import graph mean something: no pass asks "has this other module finished", they ask "does
 * this name exist yet", and after pass 3 the answer is yes for every declaration in the program.
 * Declaration resolution used to be triggered by an import and run depth-first, which is the only
 * reason the graph had to be acyclic.
 */
void resolveProgramDecls(Program& program, ModuleProvider* provider);

// One of the two modules the compiler builds itself, laid over files of `lib/` the program already
// owns - see the definition.
Module* addEmbeddedModule(Program& program, ast::ModuleGroup& group);

bool resolveModuleBodies(Module& module);

/*
 * Builds the program's entry point out of the root module's top level, and records it on the
 * program - see Program::entry.
 *
 * Before any other body, and that is a dependency rather than a preference: a dynamically
 * initialized global has no type until its initializer has been resolved, and every other body in
 * the program may name one.
 */
void resolveProgramEntry(Program& program);

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

// Makes every module one `import` names visible in this one. Only links: every module of the
// program has been discovered and interned before it runs.
void resolveImports(Module& module);

// Checks each instance against its class's superclasses and resolves the module's `default`
// declarations. Both need every instance of the module to exist, so this runs after them - which
// for Core means after the generated instances, not after its source.
void checkModuleClasses(Module& module);

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
