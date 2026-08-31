// Standalone OSSA verifier test driver - see compiler/resolve_ssa/verify.h.
//
// The verifier's whole reason for existing is to be run *after a pass that rewrote something*: §12.1
// has it running after every ownership-sensitive optimizer pass in an assertion build. A fixture
// cannot reach that. Every body the fixture corpus produces comes straight out of the conversion,
// which emits instructions in the order it computes them - so the corpus asserts that the verifier
// accepts correct IR and says nothing about what it rejects, and a check that never fires reads
// exactly like a check that works.
//
// So this driver compiles one small program through the real pipeline, verifies it, then **breaks
// the IR by hand** the way a pass would break it, and asserts on the message. Each case restores
// what it changed and verifies again, which is what says the report came from the mutation rather
// than from something the program had all along.
//
// What the cases break, and why each is unreachable from a fixture:
//
//  - two adjacent instructions swapped, so a value is used above its own definition *inside one
//    block*. Block dominance cannot see it, because every block dominates itself.
//  - an operand pointed at its own instruction's result, which is the `>=` in the position test.
//  - a use list that names one user twice and another not at all, which is the multiplicity per
//    (value, instruction) pair: membership plus a row total accepts it.
//  - a block moved into a continuation region the edge into it does not enter, and a region whose
//    declared entry is not the block the edge into it targets. The nesting rule is what a region
//    *is*, and a graph after either reroute says the arm is an ordinary successor.
//  - a `drop` swapped with the call above it, so a borrow is used after its lender was released.
//    Both are dominated by the allocation, so every structural check passes.
//  - a destructive `load` hoisted above a use of a borrow of the slot it empties, and a whole-slot
//    `store` hoisted the same way. These are the *handover* and the *replacement*, and they are how
//    an owner actually leaves a frame: the loan names the allocation and the consuming operand names
//    the value that was loaded out of it, so a rule that watched only for a drop or a consume of the
//    lender itself watched for an event that a converted body almost never contains.
//  - a second consuming use given to a value of a type with an authored `Copy`. That type is not
//    linear - nothing runs when it dies - and it is not duplicable either, which is the cell
//    `Convention::Affine` exists for; the same mutation on the same body shape without the instance
//    is legal and has to stay legal, so the case runs both and asserts both answers.
//
// Two cases break nothing and are here for the same reason - they are about the *size* of a body
// rather than the shape of one, and a fixture for either would be thousands of lines and a golden to
// match. One is seventy simultaneous borrows, which is the only way to reach the multi-word rows of
// the bit tables the ownership analysis is built on. The other is 2800 readers, whose old origin
// state table would have been 2.76 GB - past what a region can address at all - and which asserts
// that against `LinearArena::kReserve` rather than against a number somebody picked.
//
// The last case is not a mutation at all: it sets a *classification* - `@mustConsume` on a record -
// between resolving and converting, because the attribute has no spelling yet and what is under test
// is what the tier boundary does with the answer rather than how the answer was reached.
#include <Core.h>
#include "../compiler/parse/parser.h"
#include "../compiler/resolve_ssa/pipeline.h"
#include "../compiler/resolve_ssa/verify.h"

using namespace Tritium;

static Size failures = 0;
static const char* currentTest = "";

static void check(bool condition, const char* what) {
    if(condition) return;

    println("Fail (%@): %@", StringView { currentTest, stringLength(currentTest) },
            StringView { what, stringLength(what) });
    failures++;
}

// The source is one string and there are no imports, so both halves of the provider are constants.
struct StringProvider: SourceProvider {
    StringView source;
    Context* context = nullptr;

    StringView getSource(StringId) override { return source; }

    const Location* getNode(LocationId id) override {
        return context ? context->getLocation(id) : nullptr;
    }
};

// Counted and kept, not printed: a driver that breaks the IR on purpose expects errors, and the
// text is what each case asserts on.
struct QuietDiagnostics: Diagnostics {
    using Diagnostics::Diagnostics;

    void message(Level level, StringView text, const Location* where) override {
        Diagnostics::message(level, text, where);
        if(level == ErrorLevel) format(recorded, String("%@\n"), text);
    }

    void reset() override {
        Diagnostics::reset();
        recorded.clear();
    }

    bool said(StringView fragment) {
        return findString(StringView { recorded.pointer(), recorded.size() }, fragment) != nullptr;
    }

    StringBuilder recorded;
};

/*
 * One compiled program, held open at the tier the cases mutate.
 *
 * The MIR region is deliberately *not* released - `Pipeline::releaseMir` is what a driver calls once
 * it is past the MIR dump, and the verifier reads declarations out of the MIR stage's `Program` at
 * every call. Releasing it here would make every case read freed bytes.
 */
struct Compiled {
    StringProvider provider;
    QuietDiagnostics diagnostics { provider };
    Context context { diagnostics };
    ssa::Pipeline pipeline { context };
    bool ready = false;

    /*
     * `between` runs on the resolved MIR, before it is converted.
     *
     * That seam is the only way to reach a classification this stage cannot yet be told to make.
     * `@mustConsume` and `@pinned` are attributes the parser does not read, so a fixture cannot
     * declare one - and the thing the conversion is being tested about is what it does with the
     * *classification*, not how the classification was arrived at. Setting the fact here asks the
     * question the attribute will ask, in the one place it is answered.
     */
    template<class F>
    Compiled(StringView source, F&& between) {
        provider.source = source;
        provider.context = &context;

        auto name = context.addUnqualifiedName("OssaTest", 8);
        Lexer lexer(context, diagnostics, source, name);
        Parser parser(context, lexer, name);
        auto ast = parser.parseModule();

        if(!pipeline.resolve(ast)) return;
        between(*pipeline.program);
        if(!pipeline.convert()) return;

        ready = diagnostics.errorCount() == 0;
    }

    explicit Compiled(StringView source): Compiled(source, [](mir::Program&) {}) {}

    mir::Program& program() { return *pipeline.program; }
    SsaBase base() { return *pipeline.converted.arena; }

    // The body of the first function that has one, which is what every case here mutates. Named
    // rather than searched for at each case so that a source with more than one function does not
    // quietly get a different body than the case meant.
    mir::Function* functionNamed(StringView wanted) {
        auto decl = *program().decl;

        for(auto modulePointer: program().modules) {
            for(auto functionPointer: decl[modulePointer]->functions.contents(decl)) {
                auto function = decl[functionPointer];
                if(!function->ssa) continue;

                if(stringView(context.findName(function->name)) == wanted) return function;
            }
        }

        return nullptr;
    }

    bool verify(mir::Function& function) {
        diagnostics.reset();
        return ssa::verifyBody(context, program(), base(), function);
    }
};

/*
 * A chain of three additions, so that the body has instructions whose results are used by the
 * instruction *after* them and which are not the terminator.
 *
 * That matters: swapping an instruction with a terminator would also put a terminator in the middle
 * of a block, and the case would then pass on the wrong message. The loop is here so that the
 * dominance walk has a back edge to get right while the cases are running - a verifier that reported
 * on legal IR would fail the "accepts it again" half of every case below.
 */
static const char* kSource =
    "fn chain(a: I32) -> I32:\n"
    "    let &total = 0 :: I32\n"
    "    let &i = 0 :: I32\n"
    "\n"
    "    while i < 3:\n"
    "        total = total + a + i + 1\n"
    "        i = i + 1\n"
    "\n"
    "    return total\n"
    "\n"
    "fn main() -> I32 = chain(2)\n";

/*
 * The two instructions of a block where the second names the first's result and neither is the
 * terminator - the pair whose order the position check is about.
 */
struct AdjacentPair {
    ssa::Block* block = nullptr;
    U32 position = 0;
    bool found = false;
};

static AdjacentPair findAdjacentUse(SsaBase base, ssa::Body& body) {
    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(base, BlockId(i));
        auto count = block->instructions.size();
        if(count < 3) continue;

        // `count - 1` is the terminator and is left where it is, so the pair has to end at
        // `count - 2` at the latest.
        for(U32 position = 0; position + 2 < count; position++) {
            auto definer = base[block->instructions.get(base, position)];
            auto user = base[block->instructions.get(base, position + 1)];

            auto names = false;
            for(auto resultPointer: definer->results.contents(base)) {
                ssa::forEachUse(base, *user, [&](ssa::Use used) {
                    if(used.isSet() && used.value() == resultPointer) names = true;
                });
            }

            if(names) return AdjacentPair { block, position, true };
        }
    }

    return AdjacentPair {};
}

static void swapInstructions(SsaBase base, ssa::Block& block, U32 position) {
    auto first = block.instructions.get(base, position);
    auto second = block.instructions.get(base, position + 1);
    block.instructions.set(base, position, second);
    block.instructions.set(base, position + 1, first);
}

/*
 * A value used above its own definition, in the block that defines it.
 *
 * This is what the check exists for and what block dominance alone cannot state. The verifier
 * recorded only which *block* a value was defined in and asked whether that block dominated the use;
 * a block dominates itself, so every ordering within one block passed. An optimizer that sank an
 * instruction one step too far, or hoisted a use one step too high, produced IR the verifier called
 * correct and the backend miscompiled.
 */
static void testUseAboveDefinition(Compiled& compiled) {
    currentTest = "use above its definition in one block";

    auto function = compiled.functionNamed("chain"_v);
    check(function != nullptr, "the fixture source did not produce a `chain` body");
    if(!function) return;

    auto base = compiled.base();
    auto& body = *base[function->ssa];

    check(compiled.verify(*function), "the unmodified body did not verify");

    auto pair = findAdjacentUse(base, body);
    check(pair.found, "no instruction in `chain` is used by the instruction after it");
    if(!pair.found) return;

    swapInstructions(base, *pair.block, pair.position);

    check(!compiled.verify(*function), "a use above its own definition was accepted");
    check(compiled.diagnostics.said("is defined below it in block"_v),
          "the report did not name the definition order");

    // Back the way it was, and accepted again - which is what says the message above came from the
    // swap and not from something this body had before it.
    swapInstructions(base, *pair.block, pair.position);
    check(compiled.verify(*function), "the restored body did not verify");
}

/*
 * A value used by the instruction that defines it.
 *
 * The same check with `>=` rather than `>`, and the reason it is written that way: an instruction
 * naming its own result is a cycle, not a definition, and a strict comparison would have called it
 * dominated. Reached by pointing an operand of the *defining* instruction at its own result, which
 * is the shape a rewrite that redirected a use to the wrong replacement produces.
 */
static void testSelfUse(Compiled& compiled) {
    currentTest = "an instruction naming its own result";

    auto function = compiled.functionNamed("chain"_v);
    if(!function) return;

    auto base = compiled.base();
    auto& body = *base[function->ssa];

    // Any instruction that both defines a value and names one: the self-reference is made out of the
    // two halves of one instruction, so which instruction it is does not matter.
    ssa::Inst* subject = nullptr;
    ssa::Use* first = nullptr;

    for(U32 i = 0; i < body.blocks.size() && !subject; i++) {
        auto block = body.block(base, BlockId(i));

        for(auto instPointer: block->instructions.contents(base)) {
            auto inst = base[instPointer];
            if(!inst->results.size()) continue;

            ssa::Use* found = nullptr;
            ssa::forEachUse(base, *inst, [&](ssa::Use& used) { if(!found) found = &used; });
            if(!found) continue;

            subject = inst;
            first = found;
            break;
        }
    }

    check(subject != nullptr, "no instruction in `chain` both defines and names a value");
    if(!subject) return;

    // The first operand, redirected from what it named to the instruction's own result. The use
    // lists are left alone deliberately: this is one broken invariant at a time, and the use-list
    // agreement check has cases of its own.
    auto saved = *first;
    *first = ssa::Use(subject->results.get(base, 0), saved.kind());

    check(!compiled.verify(*function), "an instruction naming its own result was accepted");
    check(compiled.diagnostics.said("is defined below it in block"_v),
          "the report did not name the definition order");

    *first = saved;
    check(compiled.verify(*function), "the restored body did not verify");
}

/*
 * A body with a guard, two borrows and the teardowns the analysis placed between them.
 *
 * `idOf(a) + idOf(b)` gives each guard a borrow and a drop, with the drop immediately after the call
 * that used the borrow - which is the pair the endpoint case moves past each other.
 */
static const char* kLoanSource =
    "let &trace = 0 :: I32\n"
    "\n"
    "data Guard {id: I32}\n"
    "\n"
    "instance Drop(Guard):\n"
    "    fn drop(->value: Guard):\n"
    "        trace = trace + 1\n"
    "\n"
    "fn guard(id: I32) -> Guard = Guard {id: id}\n"
    "\n"
    "fn idOf(g: Guard) -> I32 = g.id\n"
    "\n"
    "fn borrows() -> I32:\n"
    "    let a = guard(1)\n"
    "    let b = guard(2)\n"
    "    return idOf(a) + idOf(b)\n"
    "\n"
    "fn main() -> I32 = borrows()\n";

/*
 * A type with nothing to run at the end of its life - the shape `@explicitDrop` is for.
 *
 * No `Drop` instance, no member that has one, so its teardown is `None` and the question "is it
 * linear" is answered by the attribute alone. That combination is the whole reason the two questions
 * are separate fields, and it is the one a rule keyed off the teardown gets wrong.
 *
 * `relay` is what the assertion reads and is **byte for byte the same in both sources**: it receives
 * the token under `->`, hands it straight back out, and therefore disposes of nothing whether or not
 * the type is linear. The two `main`s differ, and they have to - a linear value has to be disposed of
 * somewhere and `->x` on a trivially copyable one is refused - so the function that is compared is
 * deliberately not the one that changed.
 */
static const char* kTokenSource =
    "data Token {n: I32}\n"
    "\n"
    "fn make(n: I32) -> Token = Token {n: n}\n"
    "\n"
    "fn count(t: Token) -> I32 = t.n\n"
    "\n"
    "fn relay(->t: Token) -> Token = t\n"
    "\n"
    "fn main() -> I32:\n"
    "    let t = relay(make(3))\n"
    "    return count(t)\n";

static const char* kLinearTokenSource =
    "@explicitDrop\n"
    "data Token {n: I32}\n"
    "\n"
    "fn make(n: I32) -> Token = Token {n: n}\n"
    "\n"
    "fn count(t: Token) -> I32 = t.n\n"
    "\n"
    "fn relay(->t: Token) -> Token = t\n"
    "\n"
    "fn main() -> I32:\n"
    "    let t = relay(make(3))\n"
    "    let n = count(t)\n"
    "    ->t\n"
    "    return n\n";

static StringView view(const char* text) { return StringView { text, stringLength(text) }; }

// A `Drop` whose immediately preceding instruction uses a borrow of the storage it releases - the
// pair the endpoint rule is about, and the only pair swapping which produces the state it names.
static AdjacentPair findDropAfterBorrow(SsaBase base, ssa::Body& body) {
    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(base, BlockId(i));

        for(U32 position = 1; position < block->instructions.size(); position++) {
            auto inst = base[block->instructions.get(base, position)];
            if(inst->kind != ssa::Inst::Drop) continue;

            SsaPtr<ssa::Value> released = nullptr;
            ssa::forEachUse(base, *inst, [&](ssa::Use used) {
                if(used.isSet() && !released) released = used.value();
            });

            if(!released) continue;

            auto user = base[block->instructions.get(base, position - 1)];
            auto borrows = false;

            ssa::forEachUse(base, *user, [&](ssa::Use used) {
                if(!used.isSet()) return;

                auto value = base[used.value()];
                if(value->convention != ssa::Convention::Guaranteed || !value->loan) return;

                for(auto held: base[value->loan]->storage.contents(base)) {
                    if(held == released) borrows = true;
                }
            });

            if(borrows) return AdjacentPair { block, position - 1, true };
        }
    }

    return AdjacentPair {};
}

/*
 * A borrow used after the storage it depends on was released.
 *
 * The rule an OSSA-local loan identity exists for, and the one dominance cannot state: the `drop`
 * and the use are both dominated by the allocation, so every structural check passes and the program
 * reads storage it has already handed back. Nothing a conversion emits reaches it - the MIR analysis
 * puts a teardown at the last use of what it releases - which is exactly why it is the verifier's
 * job: what breaks it is a *pass* moving one of the two, and swapping them here is that pass.
 */
static void testBorrowAfterRelease(Compiled& compiled) {
    currentTest = "a borrow used after its lender was released";

    auto function = compiled.functionNamed("borrows"_v);
    check(function != nullptr, "the loan source did not produce a `borrows` body");
    if(!function) return;

    auto base = compiled.base();
    auto& body = *base[function->ssa];

    check(compiled.verify(*function), "the unmodified body did not verify");

    auto pair = findDropAfterBorrow(base, body);
    check(pair.found, "no drop in `borrows` follows a use of a borrow of what it releases");
    if(!pair.found) return;

    swapInstructions(base, *pair.block, pair.position);

    check(!compiled.verify(*function), "a borrow used after its lender was released was accepted");
    check(compiled.diagnostics.said("whose loan has already ended"_v),
          "the report did not name the loan");

    swapInstructions(base, *pair.block, pair.position);
    check(compiled.verify(*function), "the restored body did not verify");
}

// How many of a body's values carry one convention. The whole of what the tier boundary decides
// about a type, counted rather than described.
static Size conventionCount(Compiled& compiled, StringView functionName, ssa::Convention wanted) {
    auto function = compiled.functionNamed(functionName);
    if(!function) return 0;

    auto base = compiled.base();
    auto& body = *base[function->ssa];
    Size count = 0;

    for(U32 i = 0; i < body.values.size(); i++) {
        if(body.value(base, ValueId(i))->convention == wanted) count++;
    }

    return count;
}

static Size ownedValues(Compiled& compiled, StringView functionName) {
    return conventionCount(compiled, functionName, ssa::Convention::Owned);
}

/*
 * A type that must be disposed of explicitly is linear, even though it has no teardown.
 *
 * `conventionFor` used to ask `hasTeardown`. That is a different question which happens to have the
 * same answer for every type that does not use `@explicitDrop`, and the type it is wrong about is
 * the one the obligation is *for*: a linear token with nothing to run has no teardown at all, so
 * keying the linear rule off the teardown turned exactly the values that must be consumed exactly
 * once into `Trivial` ones an optimizer may duplicate and discard.
 *
 * Two compilations, and what is compared is `relay`, which the two sources spell identically. This
 * used to reach in and set the classification by hand, because nothing in the language set it;
 * writing the attribute is better than fabricating what it produces, since a fabrication cannot
 * drift from the real fold and this can.
 *
 * It also used to assert the *verifier's* report for a linear value reaching a return unconsumed,
 * by compiling a program that leaked one. That is no longer reachable from source and should not be:
 * `analyze_drop.cpp` now refuses the leak in MIR, with a message about the program rather than an
 * `internal:` one, so the verifier's rule is back to being the guard against a later pass losing an
 * owned value that it was written as.
 */
static void testMustConsumeIsLinear() {
    currentTest = "a type with an obligation and no teardown";

    Compiled plain { view(kTokenSource) };
    check(plain.ready, "the token source did not compile");

    Compiled linear { view(kLinearTokenSource) };
    check(linear.ready, "the linear token source did not compile");
    if(!plain.ready || !linear.ready) return;

    check(ownedValues(plain, "relay"_v) == 0,
          "a type with neither a teardown nor an obligation produced an owned value");
    check(ownedValues(linear, "relay"_v) > 0,
          "a value that must be disposed of explicitly was converted as trivial");

    // The body that only *borrows* the type is untouched, which is what says the difference came
    // from the obligation and not from the second compilation differing some other way.
    check(ownedValues(plain, "count"_v) == ownedValues(linear, "count"_v),
          "the obligation changed a body that only borrows the type");
}

/*
 * A use list that names one user twice and another not at all.
 *
 * The reviewer's shape, and the reason the check counts per pair rather than per row: `A` and `B`
 * each name a value once, and a use list holding `A` twice and `B` never satisfies both halves of
 * the weaker statement - `A` is stamped, `B` is never asked about, and the total is two either way.
 * That is exactly what a rewrite which redirected an operand and updated the wrong list produces, so
 * it is not a hypothetical shape; it is the one the relation is maintained by an API to prevent.
 *
 * Built by hand for the usual reason: the editor is what keeps the two directions in step, so
 * nothing that goes through it can reach this.
 */
static void testUseListMultiplicity(Compiled& compiled) {
    currentTest = "a use list that names one user twice and another not at all";

    auto function = compiled.functionNamed("chain"_v);
    if(!function) return;

    auto& arena = compiled.pipeline.converted.arena;
    auto base = compiled.base();
    auto& body = *base[function->ssa];

    check(compiled.verify(*function), "the unmodified body did not verify");

    // A value with two *distinct* users, which is what the miscount needs: one to double and one to
    // lose.
    ssa::Value* subject = nullptr;
    SsaPtr<ssa::Inst> kept = nullptr;
    Size dropped = 0;

    for(U32 i = 0; i < body.values.size() && !subject; i++) {
        auto value = body.value(base, ValueId(i));
        if(value->uses.size() < 2) continue;

        auto first = value->uses.get(base, 0);

        for(Size k = 1; k < value->uses.size(); k++) {
            if(value->uses.get(base, k) == first) continue;

            subject = value;
            kept = first;
            dropped = k;
            break;
        }
    }

    check(subject != nullptr, "no value in `chain` has two distinct users");
    if(!subject) return;

    auto lost = subject->uses.get(base, dropped);
    subject->uses.remove(base, dropped);
    subject->uses.push(arena, kept);

    check(!compiled.verify(*function), "a use list with the wrong multiplicity was accepted");
    check(compiled.diagnostics.said("more often than it names it"_v),
          "the report did not name the multiplicity");

    // The row total is unchanged by the mutation, which is the whole point - so restoring it is a
    // matter of putting the lost entry back and dropping the duplicate. Order within a use list is
    // not a fact the verifier checks, so it does not have to go back where it came from.
    subject->uses.remove(base, subject->uses.size() - 1);
    subject->uses.push(arena, lost);

    check(compiled.verify(*function), "the restored body did not verify");
}

/*
 * A block in a continuation region that the edge into it does not enter legally.
 *
 * The nesting rule is what a continuation region *is*: entered from its parent, left to an ancestor.
 * Nothing in a converted body breaks it - the conversion opens the one nested region it builds
 * around the block that is the arm - so, exactly as with the position check, the only way to assert
 * what the rule rejects is to build the state by hand.
 *
 * Two nested regions and a block put in the inner one, so that the edge into it comes from a block
 * that is neither the inner region's parent nor inside it. That is the shape a pass that rerouted an
 * edge past a cleanup's entry produces, and it is the reason the region is recorded rather than read
 * back off the graph: after such a reroute the graph says the arm is an ordinary successor.
 */
static void testRegionNesting(Compiled& compiled) {
    currentTest = "an edge into a region it does not enter";

    auto function = compiled.functionNamed("chain"_v);
    if(!function) return;

    auto& arena = compiled.pipeline.converted.arena;
    auto base = compiled.base();
    auto& body = *base[function->ssa];

    check(compiled.verify(*function), "the unmodified body did not verify");

    // A block with a predecessor, which is what makes the edge into it the thing under test.
    ssa::Block* nested = nullptr;
    for(U32 i = 0; i < body.blocks.size() && !nested; i++) {
        auto block = body.block(base, BlockId(i));
        if(block->incoming.size()) nested = block;
    }

    check(nested != nullptr, "no block in `chain` has a predecessor");
    if(!nested) return;

    auto outer = ContinuationId(U32(body.regions.size()));
    auto first = new (arena) ssa::Continuation(outer, ssa::Continuation::Cleanup, ContinuationId(0));
    body.regions.push(arena, first - base);

    auto inner = ContinuationId(U32(body.regions.size()));
    auto second = new (arena) ssa::Continuation(inner, ssa::Continuation::Cleanup, outer);
    body.regions.push(arena, second - base);

    auto saved = nested->region;
    nested->region = inner;

    check(!compiled.verify(*function), "an edge that skips a region's entry was accepted");
    check(compiled.diagnostics.said("neither encloses it nor is entered from it"_v),
          "the report did not name the nesting rule");

    nested->region = saved;
    check(compiled.verify(*function), "the restored body did not verify");
}

/*
 * The two sources of `Convention::Affine`, which differ by one declaration.
 *
 * `Counter` has nothing to run when it dies and may not be duplicated by copying its bytes, because
 * the author wrote what duplicating one *is*. Those are `mir::Ownership`'s two independent questions
 * answering no and no, and the four-convention fold had nowhere to put that pair: `Trivial` says
 * freely duplicable, which is the one thing the instance denies, and `Owned` demands a consumption
 * the value does not owe.
 *
 * `sink` is what the case reads and is byte for byte the same in both, which is the same discipline
 * `testMustConsumeIsLinear` follows: the function that is compared is deliberately not the one that
 * changed.
 */
static const char* kAuthoredCopySource =
    "data Counter {id: I32}\n"
    "\n"
    "instance Copy(Counter):\n"
    "    fn copy(from: Counter) -> Counter = Counter {id: from.id}\n"
    "\n"
    "fn sink(->c: Counter) -> I32 = c.id\n"
    "\n"
    "fn main() -> I32 = sink(Counter {id: 3})\n";

static const char* kTrivialCopySource =
    "data Counter {id: I32}\n"
    "\n"
    "fn sink(->c: Counter) -> I32 = c.id\n"
    "\n"
    "fn main() -> I32 = sink(Counter {id: 3})\n";

/*
 * A borrow and the two ways the storage behind it actually stops holding what was borrowed.
 *
 * `handover` loads the owner out of its slot and consumes the *loaded value*; `replaced` writes a
 * different object over the whole slot. Neither is a drop of the lender and neither is a consuming
 * use of it, which is what the endpoint rule used to watch for - so both of them are program shapes
 * a converted body is full of and the verifier had nothing to say about.
 *
 * `note` returns nothing on purpose: the replacement case sinks it past the store, and an
 * instruction with no result can be moved without leaving a use above its own definition.
 */
static const char* kEndpointSource =
    "let &trace = 0 :: I32\n"
    "\n"
    "data Counter {id: I32}\n"
    "\n"
    "instance Copy(Counter):\n"
    "    fn copy(from: Counter) -> Counter = Counter {id: from.id}\n"
    "\n"
    "fn make(n: I32) -> Counter = Counter {id: n}\n"
    "\n"
    "fn peek(c: Counter) -> I32 = c.id\n"
    "\n"
    "fn note(c: Counter):\n"
    "    trace = trace + c.id\n"
    "\n"
    "fn sink(->c: Counter) -> I32 = c.id\n"
    "\n"
    "fn handover(->g: Counter) -> I32 = peek(g) + sink(g)\n"
    "\n"
    "fn replaced() -> I32:\n"
    "    let &c = Counter {id: 2}\n"
    "    note(c)\n"
    "    c = make(7)\n"
    "    return c.id\n"
    "\n"
    "fn main() -> I32 = replaced() + handover(make(3))\n";

/*
 * The storage a borrowed value's loan names, where it names exactly one thing.
 *
 * Null for the caller's loan - whose lender is outside the body - and for a may-set with more than
 * one member, neither of which the sources here build. A finder that guessed at either would be
 * asserting about a body it did not produce.
 */
static SsaPtr<ssa::Value> soleLender(SsaBase base, ssa::Value& value) {
    if(value.convention != ssa::Convention::Guaranteed || !value.loan) return nullptr;

    auto loan = base[value.loan];
    if(loan->storage.size() != 1) return nullptr;

    return loan->storage.get(base, 0);
}

static bool usesBorrowOf(SsaBase base, ssa::Inst& inst, SsaPtr<ssa::Value> storage) {
    auto found = false;

    ssa::forEachUse(base, inst, [&](ssa::Use used) {
        if(!used.isSet()) return;
        if(soleLender(base, *base[used.value()]) == storage) found = true;
    });

    return found;
}

/*
 * One instruction moved to another position in its block, by adjacent swaps.
 *
 * A move rather than a swap, because the pair whose order the endpoint rule is about is not always
 * adjacent - and what a pass does is sink a use past a store or hoist a load above one rather than
 * exchange two neighbours. The caller is what keeps the move to one broken invariant: the
 * instruction it moves has no results, so nothing between the two positions can be left naming a
 * value defined below it.
 */
static void moveInstruction(SsaBase base, ssa::Block& block, U32 from, U32 to) {
    while(from < to) { swapInstructions(base, block, from); from++; }
    while(from > to) { swapInstructions(base, block, from - 1); from--; }
}

/*
 * An instruction's entry moved from one value's use list to another's.
 *
 * A redirected operand with the use lists left alone is two broken invariants, and the second of
 * them makes the *accepted* half of a paired case unassertable - a body the verifier should accept
 * would be reported for the bookkeeping instead. So the reverse relation moves with the operand.
 */
template<class A>
static void moveUseEntry(SsaBase base, A& arena, ssa::Inst& user, ssa::Value& from, ssa::Value& to) {
    for(Size i = 0; i < from.uses.size(); i++) {
        if(from.uses.get(base, i) != &user - base) continue;

        from.uses.remove(base, i);
        break;
    }

    to.uses.push(arena, &user - base);
}

/*
 * A use of a borrow immediately above the destructive `load` that empties the slot it borrows.
 *
 * The handover, as the conversion writes it: the owner is loaded out and the *loaded value* is what
 * the callee consumes, so the loan names the allocation and no operand of the consuming instruction
 * names it. Swapping the two is a pass hoisting the load one step, and it is the whole of what the
 * endpoint rule has to notice.
 */
static AdjacentPair findBorrowBeforeHandover(SsaBase base, ssa::Body& body) {
    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(base, BlockId(i));

        for(U32 position = 0; position + 1 < block->instructions.size(); position++) {
            auto next = base[block->instructions.get(base, position + 1)];
            if(next->kind != ssa::Inst::Load || !next->results.size()) continue;
            if(!ssa::isTracked(base[next->results.get(base, 0)]->convention)) continue;

            auto address = ((ssa::InstLoad*)next)->address;
            if(!address.isSet()) continue;

            auto inst = base[block->instructions.get(base, position)];
            if(usesBorrowOf(base, *inst, address.value())) {
                return AdjacentPair { block, position, true };
            }
        }
    }

    return AdjacentPair {};
}

// Where an instruction has to end up, and where it started - the replacement case's mutation, which
// is a move rather than a swap.
struct MovePair {
    ssa::Block* block = nullptr;
    U32 from = 0;
    U32 to = 0;
    bool found = false;
};

/*
 * A result-less use of a borrow, and the whole-slot `store` further down that overwrites what it
 * borrows.
 *
 * Result-less so that the move is safe: sinking an instruction that defines nothing past anything at
 * all leaves every other definition where it was. `Store` is skipped as a candidate for the same
 * reason the address is checked rather than assumed - the initializing store to a slot is a store to
 * the same address, and it happens before any loan on it exists.
 */
static MovePair findBorrowBeforeReplacement(SsaBase base, ssa::Body& body) {
    for(U32 b = 0; b < body.blocks.size(); b++) {
        auto block = body.block(base, BlockId(b));
        auto count = block->instructions.size();

        for(U32 j = 0; j < count; j++) {
            auto store = base[block->instructions.get(base, j)];
            if(store->kind != ssa::Inst::Store) continue;

            auto address = ((ssa::InstStore*)store)->address;
            if(!address.isSet()) continue;

            for(U32 i = 0; i < j; i++) {
                auto inst = base[block->instructions.get(base, i)];
                if(inst->results.size() || inst->kind == ssa::Inst::Store) continue;
                if(!usesBorrowOf(base, *inst, address.value())) continue;

                return MovePair { block, i, j, true };
            }
        }
    }

    return MovePair {};
}

/*
 * A borrow used after the owner it borrows was handed away, and after it was replaced.
 *
 * The pair the endpoint rule missed. A loan records the *allocation* as its lender, and an owner
 * leaves a frame by being loaded out of that allocation and consumed as a value - so the recorded
 * lender and the consuming operand are two different values with nothing relating them, and the rule
 * that watched for a drop or a consume of the lender itself was watching for an event a converted
 * body hardly ever contains. The replacement is the same gap seen from the other side: a store over
 * the whole slot puts a different object where the borrowed one was.
 */
static void testBorrowAcrossHandover(Compiled& compiled) {
    currentTest = "a borrow used after its owner was handed away";

    auto function = compiled.functionNamed("handover"_v);
    check(function != nullptr, "the endpoint source did not produce a `handover` body");
    if(!function) return;

    auto base = compiled.base();
    auto& body = *base[function->ssa];

    check(compiled.verify(*function), "the unmodified body did not verify");

    auto pair = findBorrowBeforeHandover(base, body);
    check(pair.found, "no use of a borrow in `handover` precedes the load that empties its slot");
    if(!pair.found) return;

    swapInstructions(base, *pair.block, pair.position);

    check(!compiled.verify(*function), "a borrow used after the handover was accepted");
    check(compiled.diagnostics.said("whose loan has already ended"_v),
          "the report did not name the loan");

    swapInstructions(base, *pair.block, pair.position);
    check(compiled.verify(*function), "the restored body did not verify");
}

static void testBorrowAcrossReplacement(Compiled& compiled) {
    currentTest = "a borrow used after its storage was replaced";

    auto function = compiled.functionNamed("replaced"_v);
    check(function != nullptr, "the endpoint source did not produce a `replaced` body");
    if(!function) return;

    auto base = compiled.base();
    auto& body = *base[function->ssa];

    check(compiled.verify(*function), "the unmodified body did not verify");

    auto pair = findBorrowBeforeReplacement(base, body);
    check(pair.found, "no use of a borrow in `replaced` precedes the store that overwrites its slot");
    if(!pair.found) return;

    moveInstruction(base, *pair.block, pair.from, pair.to);

    check(!compiled.verify(*function), "a borrow used after the replacement was accepted");
    check(compiled.diagnostics.said("whose loan has already ended"_v),
          "the report did not name the loan");

    moveInstruction(base, *pair.block, pair.to, pair.from);
    check(compiled.verify(*function), "the restored body did not verify");
}

/*
 * A value of a type whose duplication is the author's `Copy`, given a second consuming use.
 *
 * `Counter` is tracked and not linear: nothing runs when it dies, so it may be abandoned, and a
 * second owner is a call rather than the bytes. The four-convention fold sent it to `Trivial`, whose
 * contract is *freely duplicable*, and the one rule the instance exists to state was therefore the
 * one rule this tier did not carry - an optimizer could give the value a second use and the verifier
 * would accept it.
 *
 * Both halves are asserted, on two bodies that are the same instructions: the duplication is
 * reported for the type with the instance and accepted for the one without, which is what says the
 * check follows the classification rather than the shape. The use lists move with the operand so
 * that the accepted half is accepted for the right reason.
 */
/*
 * The mutation both halves of the case run: a second consuming use of `sink`'s parameter, given to
 * the `return` at the end of its body.
 *
 * A free function rather than a lambda inside the case, because the two compilations must not be
 * open at the same time - see the note in `main` about the reserved address range.
 */
static void duplicateParameter(Compiled& compiled, bool reject) {
    auto function = compiled.functionNamed("sink"_v);
    check(function != nullptr, "the source did not produce a `sink` body");
    if(!function) return;

    auto& arena = compiled.pipeline.converted.arena;
    auto base = compiled.base();
    auto& body = *base[function->ssa];

    check(compiled.verify(*function), "the unmodified body did not verify");

    auto entry = base[body.entry];
    check(entry->arguments.size() != 0, "`sink` has no parameter to duplicate");
    if(!entry->arguments.size() || !entry->instructions.size()) return;

    auto parameter = base[entry->arguments.get(base, 0)];
    auto last = base[entry->instructions.get(base, entry->instructions.size() - 1)];

    check(last->kind == ssa::Inst::Return, "`sink` does not end in a return");
    if(last->kind != ssa::Inst::Return) return;

    auto& values = ((ssa::InstReturn*)last)->values;
    check(values.size() != 0, "`sink` returns nothing to redirect");
    if(!values.size()) return;

    // The returned value, redirected to the parameter - which the store above it has already
    // consumed. For the trivially copyable type that is a second read of a value nothing counts.
    auto saved = values.get(base, 0);
    values.set(base, 0, ssa::Use(parameter - base, saved.kind()));
    moveUseEntry(base, arena, *last, *base[saved.value()], *parameter);

    if(reject) {
        check(!compiled.verify(*function), "a second owner of an authored-copy value was accepted");
        check(compiled.diagnostics.said("which was already consumed"_v),
              "the report did not name the duplication");
    } else {
        check(compiled.verify(*function), "a second use of a trivially copyable value was reported");
    }

    values.set(base, 0, saved);
    moveUseEntry(base, arena, *last, *parameter, *base[saved.value()]);
    check(compiled.verify(*function), "the restored body did not verify");
}

/*
 * A value of a type whose duplication is the author's `Copy`, given a second consuming use.
 *
 * `Counter` is tracked and not linear: nothing runs when it dies, so it may be abandoned, and a
 * second owner is a call rather than the bytes. The four-convention fold sent it to `Trivial`, whose
 * contract is *freely duplicable*, and the one rule the instance exists to state was therefore the
 * one rule this tier did not carry - an optimizer could give the value a second use and the verifier
 * would accept it.
 *
 * Both halves are asserted, on two bodies that are the same instructions: the duplication is
 * reported for the type with the instance and accepted for the one without, which is what says the
 * check follows the classification rather than the shape. The use lists move with the operand so
 * that the accepted half is accepted for the right reason.
 */
static void testAuthoredCopyIsAffine() {
    currentTest = "a type with an authored `Copy` and no teardown";

    Size affine = 0;
    Size owned = 0;
    Size trivialAffine = 0;

    {
        Compiled authored { view(kAuthoredCopySource) };
        check(authored.ready, "the authored-copy source did not compile");
        if(!authored.ready) return;

        affine = conventionCount(authored, "sink"_v, ssa::Convention::Affine);
        owned = conventionCount(authored, "sink"_v, ssa::Convention::Owned);
        duplicateParameter(authored, true);
    }

    {
        Compiled trivial { view(kTrivialCopySource) };
        check(trivial.ready, "the trivial-copy source did not compile");
        if(!trivial.ready) return;

        trivialAffine = conventionCount(trivial, "sink"_v, ssa::Convention::Affine);
        duplicateParameter(trivial, false);
    }

    check(affine > 0, "a type whose duplication is the author's `Copy` produced no affine value");
    check(owned == 0, "a type with nothing to run was made to owe a consumption");
    check(trivialAffine == 0, "a trivially copyable type produced an affine value");
}

/*
 * An edge from a parent region into a child at a block that is not the child's entry.
 *
 * The half of the nesting rule that was missing. "Entered from its parent" without a designated
 * entry accepts an edge into *any* block of the region, so a pass that threads a branch past the
 * flag test a cleanup arm begins with produces a graph in which the arm is an ordinary successor -
 * and the whole reason the region is recorded rather than read back off the CFG is that afterwards
 * nothing can tell the two apart.
 *
 * The case asserts both directions on the same region: entered at its own entry it verifies, and the
 * entry moved by one block it does not. That pairing is what says the rule is about the entry rather
 * than about the region having been built by hand.
 */
static void testRegionEntry(Compiled& compiled) {
    currentTest = "an edge into a region at a block that is not its entry";

    auto function = compiled.functionNamed("chain"_v);
    if(!function) return;

    auto& arena = compiled.pipeline.converted.arena;
    auto base = compiled.base();
    auto& body = *base[function->ssa];

    check(compiled.verify(*function), "the unmodified body did not verify");

    // A block with a predecessor - the edge under test - and any other block to move the entry to.
    ssa::Block* nested = nullptr;
    ssa::Block* other = nullptr;

    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(base, BlockId(i));

        if(!nested) { if(block->incoming.size()) nested = block; continue; }
        if(!other) other = block;
    }

    check(nested && other, "`chain` has too few blocks for the entry case");
    if(!nested || !other) return;

    auto id = ContinuationId(U32(body.regions.size()));
    auto region = new (arena) ssa::Continuation(id, ssa::Continuation::Cleanup, nested->region);
    region->entry = nested->index;
    body.regions.push(arena, region - base);

    auto saved = nested->region;
    nested->region = id;

    check(compiled.verify(*function), "a region entered at its own entry was reported");

    region->entry = other->index;

    check(!compiled.verify(*function), "an edge into a region past its entry was accepted");
    check(compiled.diagnostics.said("which is not its entry"_v),
          "the report did not name the entry");

    region->entry = nested->index;
    nested->region = saved;
    check(compiled.verify(*function), "the restored body did not verify");
}

/*
 * A body wider than one word of loans and one word of move paths.
 *
 * The origin environment, the loan extents and move-path liveness are bit tables whose rows are
 * `(count + 63) / 64` words wide - see `BitMatrix`. Every fixture in the corpus fits in one word of
 * each, so the multi-word paths through those three walks are reachable only from a program no
 * golden could carry: seventy borrows in one body is a two-hundred-line function and a MIR dump to
 * match, for a property that is about the width and not about the shape.
 *
 * So it is generated. What it asserts is that the body really did exceed a word on both axes - a
 * check that silently fell back to sixty-four would be a check that never fires - and that it
 * resolves, converts and verifies.
 */
static void testWideBitRows() {
    currentTest = "a body wider than one word of loans";

    constexpr U32 kCount = 70;

    StringBuilder text;
    format(text, String("let &trace = 0 :: I32\n"));
    format(text, String("data Guard {id: I32}\n"));
    format(text, String("instance Drop(Guard):\n"));
    format(text, String("    fn drop(->value: Guard):\n"));
    format(text, String("        trace = trace + 1\n"));
    format(text, String("fn guard(id: I32) -> Guard = Guard {id: id}\n"));
    format(text, String("fn peek(g: Guard) -> I32 = g.id\n"));
    format(text, String("fn main() -> I32:\n"));

    for(U32 i = 0; i < kCount; i++) format(text, String("    let &g%@ = guard(%@)\n"), i, i);

    // The borrows are taken together and used together, so that every one of them is live at the
    // same point - which is what makes the loan row wide rather than merely long.
    for(U32 i = 0; i < kCount; i++) format(text, String("    let d%@ = g%@\n"), i, i);

    format(text, String("    let &total = 0 :: I32\n"));
    for(U32 i = 0; i < kCount; i++) format(text, String("    total = total + peek(d%@)\n"), i);
    format(text, String("    return total\n"));

    Compiled wide { StringView { text.pointer(), text.size() } };
    check(wide.ready, "a body with seventy simultaneous borrows did not compile");
    if(!wide.ready) {
        println("%@", StringView { wide.diagnostics.recorded.pointer(),
                                   wide.diagnostics.recorded.size() });
        return;
    }

    auto function = wide.functionNamed("main"_v);
    check(function != nullptr, "the generated body was not resolved");
    if(!function || !function->mir) return;

    auto mir = *wide.program().mir;
    auto& body = *mir[function->mir];

    check(body.loans.size() > 64, "the generated body did not exceed one word of loans");

    auto elaboration = mir[body.elaboration];
    check(elaboration && elaboration->paths.size() > 64,
          "the generated body did not exceed one word of move paths");

    check(wide.verify(*function), "the wide body did not verify");
}

/*
 * A body whose old origin table would not have fit in a region.
 *
 * `nodeLocals` was `runs x origin-carrying locals x ceil(loans / 64)` words, and all three terms grow
 * with the program: a `let` over a record with an external field is a row, a local and a loan at
 * once. `test/bench/ownership-scaling.py` shape A is that product measured, and §72 of
 * `test/bench/findings.md` is where it was found - 640 of them written four times each is 819 MB for
 * a body of seven thousand lines, and the growth is cubic in the locals.
 *
 * So the bound this asserts is not a number somebody picked. It is `LinearArena::kReserve`, the 2 GB
 * a `RegionPtr`'s `U32` offset can address: a table past it is one the representation could not have
 * held whatever machine it ran on. The body below is 2800 readers, whose old product is 2801 rows x
 * 2800 locals x 44 words - 2.76 GB, a third again past the ceiling - and which resolves in a third
 * of a second and about 60 MB now.
 *
 * The counterfactual is recomputed here rather than published by the analysis, because it is the
 * size of a table that no longer exists - there is nothing left to ask. What the walk mirrors is
 * `buildLocalRows` and `changesLocalOrigins` as they stood: a block starts a run and a write to a
 * whole origin-carrying local ends one.
 */
static void testOriginStateBeyondARegion() {
    currentTest = "a body whose old origin table would not have fit in a region";

    constexpr U32 kCount = 2800;

    StringBuilder text;
    format(text, String("data Cell {v: I32}\n"));
    format(text, String("data Reader {held: r'Cell}\n"));
    format(text, String("fn main() -> I32:\n"));

    for(U32 i = 0; i < kCount; i++) format(text, String("    let &c%@ = Cell {v: %@}\n"), i, i);

    // Each of these is the three factors at once: a local that carries origins, a loan of the cell,
    // and a write to a whole owned local - which is what starts a row.
    for(U32 i = 0; i < kCount; i++) format(text, String("    let &r%@ = Reader {held: c%@}\n"), i, i);

    format(text, String("    return 0\n"));

    Compiled big { StringView { text.pointer(), text.size() } };
    check(big.ready, "a body with 2800 readers did not compile");
    if(!big.ready) {
        println("%@", StringView { big.diagnostics.recorded.pointer(),
                                   big.diagnostics.recorded.size() });
        return;
    }

    auto function = big.functionNamed("main"_v);
    check(function != nullptr, "the generated body was not resolved");
    if(!function || !function->mir) return;

    auto decl = *big.program().decl;
    auto mir = *big.program().mir;
    auto& body = *mir[function->mir];

    Size loanWords = (body.loans.size() + 63) / 64;

    Size originLocals = 0;
    for(U32 i = 0; i < body.locals.size(); i++) {
        if(mir::carriesOrigins(decl, body.local(mir, LocalId(i))->occurrence)) originLocals++;
    }

    // `changesLocalOrigins`, as it stood: a `Write` whose destination is a whole local that carries
    // origins. Only whether a local carries them, so the answer does not depend on the state.
    auto startsRow = [&](mir::Node& node) {
        if(node.kind != mir::Node::Write) return false;

        auto& write = (mir::NodeWrite&)node;
        if(write.destination.source() != mir::SourceKind::Place) return false;

        auto place = body.place(mir, write.destination.placeId());
        if(place->parent || place->kind != mir::Place::Local) return false;

        return mir::carriesOrigins(decl, body.local(mir, LocalId(place->payload))->occurrence);
    };

    Size rows = 0;
    for(U32 b = 0; b < body.blocks.size(); b++) {
        auto block = body.block(mir, BlockId(b));
        auto open = false;

        for(auto nodePointer: block->nodes.contents(mir)) {
            if(!open) { rows++; open = true; }
            if(startsRow(*mir[nodePointer])) open = false;
        }
    }

    check(loanWords > 1 && originLocals > 1 && rows > 1,
          "the generated body did not grow all three factors of the old table");

    auto wouldHaveBeen = rows * originLocals * loanWords * sizeof(U64);

    check(wouldHaveBeen > LinearArena::kReserve,
          "the generated body's old origin table would have fitted in a region after all");

    check(big.verify(*function), "the body did not verify");
}

/*
 * One source at a time, and each in a scope of its own.
 *
 * Not tidiness: every `Compiled` holds a pipeline whose arenas **reserve** a large address range and
 * commit into it, and `test/run-tests.sh` runs the drivers under `ulimit -v`. Two open at once is
 * what the file used to hold and is within the budget; three is not, and what it produces is not an
 * allocation failure with a message but the reservation failing inside a driver that then reports
 * nothing at all. So each source is opened, used and closed.
 */
int main() {
    {
        Compiled compiled { StringView { kSource, stringLength(kSource) } };

        currentTest = "compiling the driver's own source";
        check(compiled.ready, "the driver's source did not compile through the ownership pipeline");
        if(!compiled.ready) {
            println("%@", StringView { compiled.diagnostics.recorded.pointer(),
                                       compiled.diagnostics.recorded.size() });
            return 1;
        }

        testUseAboveDefinition(compiled);
        testSelfUse(compiled);
        testUseListMultiplicity(compiled);
        testRegionEntry(compiled);
        testRegionNesting(compiled);
    }

    {
        Compiled borrowing { view(kLoanSource) };
        currentTest = "compiling the loan source";
        check(borrowing.ready, "the loan source did not compile through the ownership pipeline");
        if(borrowing.ready) testBorrowAfterRelease(borrowing);
    }

    {
        Compiled endpoints { view(kEndpointSource) };
        currentTest = "compiling the endpoint source";
        check(endpoints.ready, "the endpoint source did not compile through the ownership pipeline");
        if(!endpoints.ready) {
            println("%@", StringView { endpoints.diagnostics.recorded.pointer(),
                                       endpoints.diagnostics.recorded.size() });
        } else {
            testBorrowAcrossHandover(endpoints);
            testBorrowAcrossReplacement(endpoints);
        }
    }

    testAuthoredCopyIsAffine();
    testMustConsumeIsLinear();
    testWideBitRows();
    testOriginStateBeyondARegion();

    if(failures) {
        println("%@ failures.", failures);
        return 1;
    }

    println("Pass.");
    return 0;
}
