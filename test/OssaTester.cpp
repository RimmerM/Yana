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
//  - a block moved into a continuation region the edge into it does not enter. The nesting rule is
//    what a region *is*, and a graph after such a reroute says the arm is an ordinary successor.
//  - a `drop` swapped with the call above it, so a borrow is used after its lender was released.
//    Both are dominated by the allocation, so every structural check passes.
//
// One case breaks nothing and is here for the same reason: a body with seventy simultaneous borrows,
// which is the only way to reach the multi-word rows of the bit tables the ownership analysis is
// built on. A fixture for it would be a two-hundred-line function and a golden to match.
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

// How many of a body's values carry the linear obligation. The whole of what the tier boundary
// decides about a type, counted rather than described.
static Size ownedValues(Compiled& compiled, StringView functionName) {
    auto function = compiled.functionNamed(functionName);
    if(!function) return 0;

    auto base = compiled.base();
    auto& body = *base[function->ssa];
    Size count = 0;

    for(U32 i = 0; i < body.values.size(); i++) {
        if(body.value(base, ValueId(i))->convention == ssa::Convention::Owned) count++;
    }

    return count;
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

int main() {
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
    testRegionNesting(compiled);

    Compiled borrowing { view(kLoanSource) };
    currentTest = "compiling the loan source";
    check(borrowing.ready, "the loan source did not compile through the ownership pipeline");
    if(borrowing.ready) testBorrowAfterRelease(borrowing);

    testMustConsumeIsLinear();
    testWideBitRows();

    if(failures) {
        println("%@ failures.", failures);
        return 1;
    }

    println("Pass.");
    return 0;
}
