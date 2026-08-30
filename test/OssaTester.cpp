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
// The mutation the first case makes is the one that used to pass: swapping two adjacent instructions
// so that a value is used above its own definition *inside one block*. Block dominance cannot see
// it, because every block dominates itself.
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

    explicit Compiled(StringView source) {
        provider.source = source;
        provider.context = &context;

        auto name = context.addUnqualifiedName("OssaTest", 8);
        Lexer lexer(context, diagnostics, source, name);
        Parser parser(context, lexer, name);
        auto ast = parser.parseModule();

        if(!pipeline.resolve(ast)) return;
        if(!pipeline.convert()) return;

        ready = diagnostics.errorCount() == 0;
    }

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

    if(failures) {
        println("%@ failures.", failures);
        return 1;
    }

    println("Pass.");
    return 0;
}
