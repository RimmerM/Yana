// Standalone IR-editor test driver - see compiler/resolve/edit.h.
//
// Every other driver in this directory is fixture-driven: it compiles source and compares what came
// out. That is the right shape for almost everything and it cannot reach what this covers, because
// the shapes here are ones **no source program produces**. A `je %c, X, X` - both arms of one branch
// at one block - is legal in the IR, is what `IrEditor`'s per-arm edge handling exists for, and was
// measured to occur in exactly none of the 242 fixtures. So the duplicate-edge cases were reasoned
// and unexercised, which is the state a bug lives in comfortably.
//
// The IR is therefore built here by hand, one function per case, and every step is followed by
// `verifyFunction` - the same check the pipeline runs, which counts the three places an edge is
// recorded rather than testing membership and so is precisely the thing that can see a duplicate
// edge half-removed. Each case also states its own expectations, because a verifier that is happy
// with a function does not say the *right* edge was the one that went.
#include <Core.h>
#include "../compiler/compiler/diagnostics.h"
#include "../compiler/resolve/module.h"
#include "../compiler/resolve/core.h"
#include "../compiler/resolve/edit.h"
#include "../compiler/resolve/builder.h"
#include "../compiler/resolve/verify.h"

using namespace Tritium;

static Size failures = 0;
static const char* currentTest = "";

static void check(bool condition, const char* what) {
    if(condition) return;

    println("Fail (%@): %@", StringView { currentTest, stringLength(currentTest) },
            StringView { what, stringLength(what) });
    failures++;
}

/*
 * One function with a branch whose two arms lead to the same block, and a phi in that block.
 *
 * The phi is the part worth building. A predecessor list can be repaired by a walk that happens to
 * count right; a phi cannot, because its alternatives are per *edge* - two edges from one block are
 * two alternatives, and every operation here has to move or remove exactly as many of them as it
 * moves predecessor entries. `verifyFunction` compares those two counts, which is what makes this a
 * test rather than a demonstration.
 *
 *      entry: %c = 1; je %c, join, join
 *      join:  %p = phi [entry: 10], [entry: 20]; ret %p
 */
struct Diamond {
    Block* entry = nullptr;
    Block* join = nullptr;
    InstPhi* phi = nullptr;
    ModulePtr<Value> cond = nullptr;
};

/*
 * The block `addFunction` already made, which is the one every case here has to build into.
 *
 * `Module::addFunction` creates the entry block itself, so a case that called `addBlock` for its own
 * entry left block 0 empty and terminator-less - and `hasBody` answers no to that, which makes
 * `verifyFunction` return true without looking at anything. Every `verifies` check in this file was
 * vacuous for exactly that reason, which is a thing a test driver is not allowed to be quiet about.
 */
static Block* entryBlock(Module& module, Function& function) {
    return (*module.arena)[function.blocks.get(*module.arena, 0)];
}

static Diamond buildDuplicateArms(Module& module, Function& function) {
    IrEditor editor(module, function);
    Diamond result;

    result.entry = entryBlock(module, function);
    result.join = function.addBlock(module);

    auto joinPointer = (ModulePtr<Block>)(result.join - *module.arena);

    result.cond = (ModulePtr<Value>)(addConstant<ConstInt>(module, function, *result.entry,
                                                           kNullLocation, module.scalar.bool_, 1)
                                     - *module.arena);

    auto ten = (ModulePtr<Value>)(addConstant<ConstInt>(module, function, *result.entry,
                                                        kNullLocation, module.scalar.int_, 10)
                                  - *module.arena);
    auto twenty = (ModulePtr<Value>)(addConstant<ConstInt>(module, function, *result.entry,
                                                           kNullLocation, module.scalar.int_, 20)
                                     - *module.arena);

    // The phi first and detached, because appending is what records its alternatives as uses and
    // they have to exist by then - the same order every pass that builds one works in.
    result.phi = createInst<InstPhi>(module, function, *result.join, kNullLocation, StringId(),
                                     module.scalar.int_);

    auto entryPointer = (ModulePtr<Block>)(result.entry - *module.arena);
    result.phi->inputs.push(module.arena, PhiInput { entryPointer, ten });
    result.phi->inputs.push(module.arena, PhiInput { entryPointer, twenty });
    editor.append(*result.join, (Inst*)result.phi);

    addInst<InstRet>(module, function, *result.join, kNullLocation, StringId(), module.scalar.unit,
                     (ModulePtr<Value>)(result.phi - *module.arena));

    // Both arms at the join, which is the whole point: two edges between one pair of blocks.
    addInst<InstJe>(module, function, *result.entry, kNullLocation, StringId(), module.scalar.unit,
                    result.cond, joinPointer, joinPointer);

    return result;
}

// How many alternatives one phi reads over edges from `from`, which is the count every operation
// here has to keep in step with the predecessor entries.
static Size alternativesFrom(ModuleBase base, InstPhi& phi, ModulePtr<Block> from) {
    Size count = 0;
    for(auto input: phi.inputs.contents(base)) count += input.block == from;
    return count;
}

static Size predecessorsFrom(ModuleBase base, Block& block, ModulePtr<Block> from) {
    Size count = 0;
    for(auto incoming: block.incoming(base)) count += incoming == from;
    return count;
}

/*
 * The shape as built, before anything edits it.
 *
 * Run first because every case below asserts against it, and a builder that produced something else
 * would make all of them vacuous - two arms at one block is exactly the sort of thing `addInst` could
 * quietly normalize away.
 */
static void testBuild(Module& module) {
    currentTest = "build";

    auto function = module.addFunction(module.context.addUnqualifiedName("build", 5), kNullLocation);
    auto built = buildDuplicateArms(module, *function);
    auto base = *module.arena;
    auto entryPointer = (ModulePtr<Block>)(built.entry - base);

    check(built.entry->successor(0) == built.entry->successor(1), "both arms lead to the join");
    check(predecessorsFrom(base, *built.join, entryPointer) == 2, "the join has two predecessors");
    check(alternativesFrom(base, *built.phi, entryPointer) == 2, "the phi has two alternatives");
    check(verifyFunction(module, *function, VerifyStage::Resolved, "as built"_v), "verifies");
}

/*
 * Both arms split, one at a time - the case `splitEdge`'s ordinal exists for.
 *
 * Splitting arm 0 must move *one* predecessor entry and *one* alternative, leaving the other arm's
 * records untouched and still naming the entry block. Splitting arm 1 then takes the other. The
 * version that took a successor rather than an ordinal moved one branch arm, both `outgoing` entries
 * and every matching predecessor entry, which is three different answers to one question - and the
 * check after the first split is the one that sees it.
 */
static void testSplitBothArms(Module& module) {
    currentTest = "split both arms";

    auto function = module.addFunction(module.context.addUnqualifiedName("split", 5), kNullLocation);
    auto built = buildDuplicateArms(module, *function);
    auto base = *module.arena;
    auto entryPointer = (ModulePtr<Block>)(built.entry - base);

    IrEditor editor(module, *function);

    auto first = editor.splitEdge(*built.entry, 0);
    auto firstPointer = (ModulePtr<Block>)(first - base);

    check(verifyFunction(module, *function, VerifyStage::Resolved, "after one split"_v),
          "verifies after splitting arm 0");
    check(built.entry->successor(0) == firstPointer, "arm 0 leaves through the split block");
    check(built.entry->successor(1) != firstPointer, "arm 1 is untouched");
    check(predecessorsFrom(base, *built.join, entryPointer) == 1, "one predecessor entry moved");
    check(alternativesFrom(base, *built.phi, entryPointer) == 1, "one alternative moved");
    check(alternativesFrom(base, *built.phi, firstPointer) == 1, "and it names the split block");

    auto second = editor.splitEdge(*built.entry, 1);
    auto secondPointer = (ModulePtr<Block>)(second - base);

    check(verifyFunction(module, *function, VerifyStage::Resolved, "after both splits"_v),
          "verifies after splitting arm 1");
    check(predecessorsFrom(base, *built.join, entryPointer) == 0, "the entry no longer reaches it");
    check(alternativesFrom(base, *built.phi, secondPointer) == 1, "the second alternative moved too");
    check(built.phi->inputs.size() == 2, "and the phi still has both");
}

/*
 * `je %c, X, X` folded to `jmp X` - `setTerminator`'s multiset rule at its least obvious.
 *
 * Two edges into the join and one afterwards, so exactly one predecessor entry and one alternative
 * go. Removing both and adding one back would leave the phi an alternative short; leaving both would
 * leave the join claiming a predecessor twice over one edge. Neither shows up in a dump.
 */
static void testFoldDuplicateBranch(Module& module) {
    currentTest = "fold duplicate branch";

    auto function = module.addFunction(module.context.addUnqualifiedName("fold", 4), kNullLocation);
    auto built = buildDuplicateArms(module, *function);
    auto base = *module.arena;
    auto entryPointer = (ModulePtr<Block>)(built.entry - base);
    auto joinPointer = (ModulePtr<Block>)(built.join - base);

    IrEditor editor(module, *function);
    auto jump = createInst<InstJmp>(module, *function, *built.entry, kNullLocation, StringId(),
                                    module.scalar.unit, joinPointer);
    editor.setTerminator(*built.entry, jump);

    check(verifyFunction(module, *function, VerifyStage::Resolved, "after folding"_v), "verifies");
    check(built.entry->successor(0) == joinPointer && !built.entry->successor(1), "one way out");
    check(predecessorsFrom(base, *built.join, entryPointer) == 1, "one predecessor entry left");
    check(alternativesFrom(base, *built.phi, entryPointer) == 1, "one alternative left");
}

// The same block's way out removed outright, which owes *both* edge records rather than one - the
// case a loop written as "remove the edge to the successor" gets half right.
static void testClearDuplicateTerminator(Module& module) {
    currentTest = "clear duplicate terminator";

    auto function = module.addFunction(module.context.addUnqualifiedName("clear", 5), kNullLocation);
    auto built = buildDuplicateArms(module, *function);
    auto base = *module.arena;
    auto entryPointer = (ModulePtr<Block>)(built.entry - base);

    IrEditor editor(module, *function);
    editor.clearTerminator(*built.entry);

    check(!built.entry->successor(0) && !built.entry->successor(1), "no way out");
    check(predecessorsFrom(base, *built.join, entryPointer) == 0, "both predecessor entries went");
    check(alternativesFrom(base, *built.phi, entryPointer) == 0, "both alternatives went");
    check(built.phi->inputs.size() == 0, "which is all of them");
}

/*
 * Both arms redirected at once, which is what splicing an empty block out does to its predecessors.
 *
 * The answer is what the caller loops on - `spliceEmptyBlock` empties a predecessor list one redirect
 * at a time and terminates only because each round removes an entry - so a doubled arm answering
 * "one" would leave that loop spinning on a list it never empties.
 */
static void testRedirectDuplicateArms(Module& module) {
    currentTest = "redirect duplicate arms";

    auto function = module.addFunction(module.context.addUnqualifiedName("redirect", 8), kNullLocation);
    auto built = buildDuplicateArms(module, *function);
    auto base = *module.arena;
    auto entryPointer = (ModulePtr<Block>)(built.entry - base);
    auto joinPointer = (ModulePtr<Block>)(built.join - base);

    // Somewhere else to send them: a block with no phis, so the redirect owes only the edge records.
    auto target = function->addBlock(module);
    auto targetPointer = (ModulePtr<Block>)(target - base);
    addInst<InstRet>(module, *function, *target, kNullLocation, StringId(), module.scalar.unit, nullptr);

    IrEditor editor(module, *function);
    auto moved = editor.redirectSuccessor(*built.entry, joinPointer, targetPointer);

    check(moved == 2, "both arms answered for");
    check(built.entry->successor(0) == targetPointer && built.entry->successor(1) == targetPointer,
          "both arms lead to the target");
    check(predecessorsFrom(base, *built.join, entryPointer) == 0, "the join lost both entries");
    check(alternativesFrom(base, *built.phi, entryPointer) == 0, "and the phi lost both");
    check(predecessorsFrom(base, *target, entryPointer) == 2, "the target gained both");
}

/*
 * The five vector instructions - Implementation-Vector.md §3.2, built here for the reason this
 * driver exists at all: they are shapes **no source program produces**.
 *
 * Stage 9's library is what will build them from a `Vec(a)` expression, and until it lands nothing
 * above the resolver can construct one - so the alternative to building them by hand is five kinds,
 * a verifier that has never seen them, a use list that has never recorded one and a translation to
 * the lower IR that has never run. That is the state a bug lives in comfortably, which is the
 * sentence at the top of this file.
 *
 * What each case asserts is the thing that would be silently wrong: that `IrEditor::append` records
 * the operands as uses (a kind missing from `addUse` compiles and leaks a reader), and that
 * `verifyFunction` accepts a well-formed one and rejects the malformed one beside it.
 *
 *      vectors: %v = vsplat %x; %l = vlane %v, 2; %w = vwithlane %v, %x, 1
 *               %s = vshuffle %v, %w, 0, 1, 2, 3; %r = vreduce_add %v; ret %l
 */
static void testVectorInstructions(Module& module) {
    currentTest = "vector instructions";

    auto& context = module.context;
    auto function = module.addFunction(context.addUnqualifiedName("vectors", 7), kNullLocation);
    auto block = entryBlock(module, *function);
    auto base = *module.arena;

    // Four lanes explicitly rather than the natural count, so that the pattern below is four entries
    // whatever this build's target vector width is.
    auto lane = module.scalar.float_;
    auto vector = resolveVectorType(module, lane, 4, false, kNullLocation);
    check(vectorLanes(*module.types, vector) == 4, "a four-lane vector");

    IrEditor editor(module, *function);

    auto scalar = (ModulePtr<Value>)(addConstant<ConstFloat>(module, *function, *block, kNullLocation,
                                                             lane, 1.5f) - base);

    auto splat = addInst<InstVecSplat>(module, *function, *block, kNullLocation, StringId(), vector,
                                       scalar);
    auto splatPointer = (ModulePtr<Value>)(splat - base);

    auto read = addInst<InstVecLane>(module, *function, *block, kNullLocation, StringId(), lane,
                                     splatPointer, 2);

    auto written = addInst<InstVecLane>(module, *function, *block, kNullLocation, StringId(), vector,
                                        splatPointer, 1, scalar);
    auto writtenPointer = (ModulePtr<Value>)(written - base);

    auto shuffle = createInst<InstVecShuffle>(module, *function, *block, kNullLocation, StringId(),
                                              vector, splatPointer, writtenPointer);
    for(U8 i = 0; i < 4; i++) shuffle->pattern.push(i);
    editor.append(*block, (Inst*)shuffle);

    addInst<InstVecReduce>(module, *function, *block, kNullLocation, StringId(), lane, splatPointer,
                           ReduceOp::Add);

    addInst<InstRet>(module, *function, *block, kNullLocation, StringId(), module.scalar.unit,
                     (ModulePtr<Value>)(read - base));

    /*
     * The splat is read four times - by the lane read, the lane write, the shuffle's left source and
     * the reduction - and every one of those is a kind `addUse` had to be told about. A kind it was
     * not told about compiles, records nothing, and leaves the dead-value pass entitled to delete
     * the instruction its reader still names.
     */
    check(base[splatPointer]->useCount() == 4, "every reader of the splat is recorded");
    check(base[scalar]->useCount() == 2, "and both readers of the scalar");

    check(verifyFunction(module, *function, VerifyStage::Resolved, "vectors"_v), "verifies");
}

/*
 * And the malformed ones, which is the half that says the rules above are checked rather than
 * written down. Each of these is one instruction that is wrong in one way.
 */
static void testVectorRejections(Module& module) {
    currentTest = "vector rejections";

    auto& context = module.context;
    auto base = *module.arena;
    auto lane = module.scalar.float_;
    auto vector = resolveVectorType(module, lane, 4, false, kNullLocation);
    auto narrow = resolveVectorType(module, lane, 2, false, kNullLocation);

    // The verifier reports through the module's diagnostics, and `main` treats a leftover finding as
    // a failure - so each rejection is counted here and the count is what is asserted, rather than
    // the errors being left to accumulate.
    auto before = context.diagnostics.errorCount();

    // The four findings below are printed as they are reported, because the diagnostics object is
    // the process's one and there is nothing to redirect it into. Saying so first is what keeps a
    // passing run from reading like a failing one.
    println("The four verifier findings below are the point of this case, and are expected:");

    auto reject = [&](const char* name, Size length, auto&& build) {
        auto function = module.addFunction(context.addUnqualifiedName(name, length), kNullLocation);
        auto block = entryBlock(module, *function);
        auto scalar = (ModulePtr<Value>)(addConstant<ConstFloat>(module, *function, *block,
                                                                 kNullLocation, lane, 1.5f) - base);
        auto splat = (ModulePtr<Value>)(addInst<InstVecSplat>(module, *function, *block, kNullLocation,
                                                              StringId(), vector, scalar) - base);

        build(*function, *block, scalar, splat);
        addInst<InstRet>(module, *function, *block, kNullLocation, StringId(), module.scalar.unit, nullptr);

        check(!verifyFunction(module, *function, VerifyStage::Resolved, "rejection"_v), name);
    };

    // A lane index past the end of the vector it names.
    reject("laneOutOfRange", 14, [&](Function& function, Block& block, ModulePtr<Value>, ModulePtr<Value> splat) {
        addInst<InstVecLane>(module, function, block, kNullLocation, StringId(), lane, splat, 7);
    });

    // A shuffle pattern with fewer entries than the result has lanes.
    reject("shortPattern", 12, [&](Function& function, Block& block, ModulePtr<Value>, ModulePtr<Value> splat) {
        IrEditor editor(module, function);
        auto shuffle = createInst<InstVecShuffle>(module, function, block, kNullLocation, StringId(),
                                                  vector, splat, splat);
        shuffle->pattern.push(0);
        editor.append(block, (Inst*)shuffle);
    });

    // A cast between two vectors of different lane counts, which is a shuffle and a cast rather than
    // a cast - §3.2's first consequence, and the one rule nothing else distinguishes.
    reject("laneCountCast", 13, [&](Function& function, Block& block, ModulePtr<Value>, ModulePtr<Value> splat) {
        addInst<InstUnary>(module, function, block, kNullLocation, StringId(), narrow, Value::Cast, splat);
    });

    // A reduction of something that is not a vector at all.
    reject("scalarReduce", 12, [&](Function& function, Block& block, ModulePtr<Value> scalar, ModulePtr<Value>) {
        addInst<InstVecReduce>(module, function, block, kNullLocation, StringId(), lane, scalar,
                               ReduceOp::Add);
    });

    check(context.diagnostics.errorCount() > before, "the verifier reported them");

    // `main` treats a leftover finding as a failure, which is what catches a verifier complaining
    // about a function no case asked about. These findings were asked for, so the count goes back to
    // where the run started rather than staying as four unattributed errors.
    context.diagnostics.reset();
}

// A source provider that answers nothing, because nothing here has a source: every function is
// built out of instructions rather than parsed, and every location is null. It exists so that a
// verifier finding still reaches `println` rather than a null dereference on the way to formatting.
struct NoSource: SourceProvider {
    StringView getSource(StringId module) override { return ""_v; }
    const Location* getNode(LocationId id) override { return nullptr; }
};

int main() {
    NoSource provider;
    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    Program program(context);
    defineCore(program);

    auto name = context.addQualifiedName("Edit", 4, 1);
    auto module = program.addModule(name, program.core->parse);

    testBuild(*module);
    testSplitBothArms(*module);
    testFoldDuplicateBranch(*module);
    testClearDuplicateTerminator(*module);
    testRedirectDuplicateArms(*module);
    testVectorInstructions(*module);
    testVectorRejections(*module);

    // The verifier reports through the module's diagnostics rather than by answering, so a finding
    // that `verifyFunction` returned false about is also an error here - and one it reported about a
    // function no case asked about would otherwise go unnoticed.
    if(context.diagnostics.errorCount()) {
        println("Fail: the verifier reported %@ findings", U32(context.diagnostics.errorCount()));
        failures++;
    }

    if(failures) {
        println("%@ failures", U64(failures));
        return 1;
    }

    println("Running test \"resolve/edit duplicate edges\"... Pass.");
    return 0;
}
