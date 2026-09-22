// Standalone JSIR verifier test driver - see compiler/resolve_ssa/js/verify.h.
//
// The verifier is the main reason the tier exists (Analysis-IR-JS.md summary item 8), and the
// fixture corpus asserts only that it *accepts* correct IR: every body there comes straight out of
// the conversion, which builds nothing the checks are about. A check that never fires reads exactly
// like a check that works, so §4.5 asks for a suite that deliberately builds malformed JSIR.
//
// That is this driver. It builds bodies by hand with `JsBuilder` - no program, no resolver, no
// fixture - and asserts on the sentence each mutation produces. Building by hand is what makes the
// cases reachable at all: the conversion cannot emit a number used as an object receiver, because
// the place it would need does not exist, and that is the point.
//
// What each case breaks, and why a fixture could not:
//
//  - a `Number` used as a property owner. The conversion reads the owner's representation before it
//    builds the place, so the only way to reach the check is to write the place down.
//  - a multi-part value used as one value without a materialization. `JsRepKind::Parts` is declared
//    and unproduced in this slice, so nothing in the corpus can carry one.
//  - a block argument and an edge operand at different representations. The conversion derives both
//    from one occurrence; a *pass* that rewrote one of them is what this guards against.
//  - a missing definition - an operand whose value nothing defines, which is the `null` §4 is about.
//  - a store whose value is not the place's content, which is the representation check from the
//    write side.
//  - a boxed global read as if it were not boxed. Which globals are boxed is one whole-program
//    decision two ends read (`JsTarget::boxedGlobals`), so the way it goes wrong is one of them
//    forgetting - and neither end can disagree with itself inside one fixture.
//  - caller and callee disagreeing about a frozen `JsAbi`, in both directions: an arity the plan
//    does not have, and an argument at the wrong representation. §3.7's whole argument is that two
//    predicates can quietly agree on the same wrong answer, so the plan is compared against rather
//    than recomputed.
//  - an operand whose definition does not dominate its use, which block ordering alone accepts.
//
// Every case builds a body that is *correct* first and asserts that it verifies, then breaks one
// thing and asserts the report - which is what says the sentence came from the mutation rather than
// from something the body had all along.
#include <Core.h>
#include "../compiler/resolve_ssa/js/build.h"
#include "../compiler/resolve_ssa/js/verify.h"
#include "../compiler/resolve_ssa/js/opt.h"

using namespace Tritium;
using namespace jsir;

static Size failures = 0;
static const char* currentTest = "";

static void check(bool condition, const char* what) {
    if(condition) return;

    println("Fail (%@): %@", StringView { currentTest, stringLength(currentTest) },
            StringView { what, stringLength(what) });
    failures++;
}

struct NoSource: SourceProvider {
    StringView getSource(StringId) override { return StringView(); }
    const Location* getNode(LocationId) override { return nullptr; }
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
 * One JSIR body under construction, with everything it needs and nothing it does not.
 *
 * No `mir::Program` contents and no SSA: the representation tables are built by the same functions
 * the conversion uses, and a body is whatever the case writes. That is the whole reason this driver
 * is short - `JsTarget` is the only thing a verifier needs, and it is constructible empty.
 */
struct JsirScratch {
    NoSource provider;
    QuietDiagnostics diagnostics { provider };
    Context context { diagnostics };
    mir::Program program;
    ssa::Converted converted;
    JsTarget target { context, program, converted };
    JsBody body;
    JsBuilder builder { target, body };

    JsRepId number() { return jsNumberRep(target, NumberDomain::Signed, 32); }

    // One function of no arguments answering a number, which is what every case below starts from.
    JsBlock* entry() {
        JsAbi abi;
        abi.result = JsPassing { JsPassingKind::One, number(), 1 };
        body.abi = target.addAbi(::move(abi));

        auto block = builder.addBlock(StringId());
        body.entry = block - builder.base;
        builder.switchTo(block);
        return block;
    }

    /*
     * `return value`, with the use counted.
     *
     * Twenty-seven cases end this way and the second line is the one worth not writing out:
     * `JsValue::uses` is a real invariant here - `codegen/js/opt.cpp` reads it to decide whether an
     * expression may be written at its single use - and a case that forgot it verified anyway, so
     * the mistake would have been a case asserting something slightly different from the case
     * beside it. Two of them did forget, which is how this helper was chosen.
     */
    JsInstReturn* returns(JsirPtr<JsValue> value) {
        auto ret = builder.add<JsInstReturn>(JsRepId(), kNullLocation);
        ret->value = value;
        if(value) builder.base[value]->uses++;
        return ret;
    }

    JsInstReturn* returns(JsValue* value) { return returns(value - builder.base); }

    /*
     * One callee's frozen plan, with the arity **derived** rather than written beside it.
     *
     * Eleven cases below build a `JsAbi` by hand, and that is the point: the verifier's subject is
     * a plan two ends disagree about, which no `.yana` program produces. What is not the point is
     * the arity - it is the sum of the positions plus the leading words the two `takes` flags add,
     * so a case that wrote the number out was restating something the plan already contains, and
     * could get it wrong without being about that.
     *
     * `JsTarget::addAbi` stays the only way one gets an id: `sealShared` has to run after the last
     * parameter is pushed, and these eleven sites are what found that out (J79 0.3).
     */
    JsAbiId calleeAbi(JsAbi&& abi) {
        abi.arity = U32(abi.takesGenEnv) + U32(abi.takesEnv);

        for(auto& passing: abi.parameters) {
            if(passing.kind != JsPassingKind::Omitted) abi.arity += passing.positions;
        }

        return target.addAbi(::move(abi));
    }

    bool verify() {
        diagnostics.reset();
        target.reported = false;
        return verifyBody(target, body);
    }
};

// A record shape with one numeric property, for the cases that need an object to read out of.
static JsShapeId oneProperty(JsirScratch& s) {
    JsShape shape;
    shape.kind = JsShapeKind::Object;
    shape.properties.push(JsProperty {
        s.context.addUnqualifiedName("x", 1), s.number(), JsPropertyRole::Field, 0
    });

    auto id = JsShapeId(s.target.shapes.size());
    s.target.shapes.push(::move(shape));
    return id;
}

static JsRepId objectRep(JsirScratch& s, JsShapeId shape) {
    JsRep rep;
    rep.kind = JsRepKind::GcObject;
    rep.shape = shape;

    auto id = JsRepId(s.target.reps.size());
    s.target.reps.push(rep);
    return id;
}

static void testNumberAsObjectReceiver() {
    currentTest = "a number used as a property owner";

    JsirScratch s;
    auto shape = oneProperty(s);
    auto object = objectRep(s, shape);

    s.entry();
    auto held = s.builder.add<JsInstObject>(object, kNullLocation, shape);
    auto zero = s.builder.number(0, s.number(), kNullLocation);
    held->values.push(s.target.arena, zero - s.builder.base);
    zero->uses++;

    auto value = s.builder.base[held->result];
    auto read = s.builder.load(s.builder.addProperty(value, shape, 0), kNullLocation);

    auto ret = s.returns(read);

    check(s.verify(), "the correct body should verify");

    // The owner is now the number the property holds rather than the object holding it - which is
    // §4.3's "a field access cannot target `Number`".
    auto broken = s.builder.addProperty(read, shape, 0);
    s.builder.add<JsInstLoad>(s.number(), kNullLocation, broken);

    check(!s.verify(), "a number used as a property owner should be rejected");
    check(s.diagnostics.said("not a host object"_v), "the report should name the owner");
}

static void testPartsUsedAsOneValue() {
    currentTest = "a multi-part value used as one value";

    JsirScratch s;
    s.entry();

    JsRep parts;
    parts.kind = JsRepKind::Parts;
    parts.partCount = 2;
    auto partsRep = JsRepId(s.target.reps.size());
    s.target.reps.push(parts);

    auto held = s.builder.add<JsInstConst>(partsRep, kNullLocation, JsConstant {});
    auto ret = s.returns(held->result);

    check(!s.verify(), "parts used as one value should be rejected");
    check(s.diagnostics.said("without a materialization"_v), "the report should name the conversion");
}

static void testEdgeRepresentationMismatch() {
    currentTest = "an edge operand at the wrong representation";

    JsirScratch s;
    auto entry = s.entry();
    auto join = s.builder.addBlock(StringId());

    auto argument = s.builder.addArgument(join, s.number());
    auto supplied = s.builder.number(1, s.number(), kNullLocation);

    auto jump = s.builder.add<JsInstJump>(JsRepId(), kNullLocation);
    s.builder.connect(jump->to, entry, join);
    s.builder.edgeArgument(jump->to, supplied);

    s.builder.switchTo(join);
    auto ret = s.returns(argument);

    check(s.verify(), "the correct body should verify");

    // The block argument now claims a representation the edge does not supply. This is what catches
    // a loop-carried value whose representation changed on one edge, before structuring turns the
    // argument into a `var` that could hold any JavaScript value at all.
    argument->rep = jsNumberRep(s.target, NumberDomain::Float64, 64);

    check(!s.verify(), "an edge operand at the wrong representation should be rejected");
    check(s.diagnostics.said("not the block argument's"_v), "the report should name the edge");
}

static void testMissingDefinition() {
    currentTest = "an operand with no definition";

    JsirScratch s;
    s.entry();

    auto value = s.builder.addValue(s.number());
    auto ret = s.returns(value);

    // A value with neither a definer nor a block is the `null` §4 is about: nothing computes it, and
    // a tier that let it through would emit a name for it and read `undefined`.
    check(!s.verify(), "an operand with no definition should be rejected");
    check(s.diagnostics.said("no definition"_v), "the report should say the definition is missing");
}

static void testStoreRepresentationMismatch() {
    currentTest = "a store at the wrong representation";

    JsirScratch s;
    s.entry();

    auto place = s.builder.addVariable(s.number());
    auto value = s.builder.number(7, s.number(), kNullLocation);
    s.builder.store(place, value, kNullLocation);

    auto read = s.builder.load(place, kNullLocation);
    auto ret = s.returns(read);

    check(s.verify(), "the correct body should verify");

    // The variable now holds a float and the store still writes an integer - a representation the
    // place's content says it cannot hold.
    s.builder.place(place).content = jsNumberRep(s.target, NumberDomain::Float64, 64);

    check(!s.verify(), "a store at the wrong representation should be rejected");
    check(s.diagnostics.said("not the place's content"_v), "the report should name the place");
}

/*
 * A **boxed global** read as if it were not boxed - J67's whole-program decision, from the one end
 * that can catch a disagreement about it.
 *
 * `planBoxedGlobals` decides in one place and every `GlobalAddress` reads that decision, so the way
 * this goes wrong is one of the two forgetting: the binding holds `{$v: 0}` and a reader names the
 * number directly, or the other way round. Neither is reachable from a fixture, because the two ends
 * are the same `if` - and that is exactly the shape §3.7 argues has to be a verifier property rather
 * than two predicates.
 */
static void testBoxedGlobalReadUnboxed() {
    currentTest = "a boxed global whose reader does not know it is boxed";

    JsirScratch s;
    s.entry();

    auto content = s.number();
    auto boxShape = jsBoxShape(s.target, content);
    auto slot = s.builder.addGlobal(DeclPtr<mir::Global>(U32(16)), jsBoxRep(s.target, content));
    auto box = s.builder.load(slot, kNullLocation);
    auto inside = s.builder.addProperty(box, boxShape, 0);

    s.builder.store(inside, s.builder.number(7, content, kNullLocation), kNullLocation);

    auto read = s.builder.load(inside, kNullLocation);
    auto ret = s.returns(read);

    check(s.verify(), "the correct body should verify");

    // The binding now holds the number itself and the property place still reads `$v` out of it.
    s.builder.place(slot).content = content;

    check(!s.verify(), "a property read out of an unboxed global should be rejected");
    check(s.diagnostics.said("not the place's content"_v),
          "the report should name the load that read the box out of it");
}

static void testCallDisagreesWithAbi() {
    currentTest = "a call that disagrees with its callee's convention";

    JsirScratch s;
    s.entry();

    // A plan taking one number and answering one.
    JsAbi callee;
    callee.parameters.push(JsPassing { JsPassingKind::One, s.number(), 1 });
    callee.result = JsPassing { JsPassingKind::One, s.number(), 1 };

    auto abi = s.calleeAbi(::move(callee));

    auto argument = s.builder.number(3, s.number(), kNullLocation);
    auto call = s.builder.add<JsInstCall>(s.number(), kNullLocation,
                                          DeclPtr<mir::Function>(nullptr), abi);
    call->arguments.push(s.target.arena, argument - s.builder.base);
    argument->uses++;

    auto ret = s.returns(call->result);

    check(s.verify(), "the correct call should verify");

    // One argument too many, which is the failure that shifts every argument after it.
    auto second = s.builder.number(4, s.number(), kNullLocation);
    call->arguments.push(s.target.arena, second - s.builder.base);
    second->uses++;

    check(!s.verify(), "a call with the wrong arity should be rejected");
    check(s.diagnostics.said("arity disagrees"_v), "the report should name the arity");

    // And the same call at the right arity and the wrong representation.
    call->arguments.remove(s.builder.base, 1);
    argument->rep = jsNumberRep(s.target, NumberDomain::Float64, 64);

    check(!s.verify(), "a call argument at the wrong representation should be rejected");
    check(s.diagnostics.said("disagrees with its callee's convention"_v),
          "the report should name the convention");
}

static void testDefinitionDoesNotDominate() {
    currentTest = "an operand whose definition does not dominate its use";

    JsirScratch s;
    auto entry = s.entry();
    auto other = s.builder.addBlock(StringId());
    auto join = s.builder.addBlock(StringId());

    auto condition = s.builder.number(1, jsNumberRep(s.target, NumberDomain::Bool01, 1),
                                      kNullLocation);
    auto branch = s.builder.add<JsInstBranch>(JsRepId(), kNullLocation, condition - s.builder.base);
    s.builder.connect(branch->whenTrue, entry, other);
    s.builder.connect(branch->whenFalse, entry, join);

    s.builder.switchTo(other);
    auto inArm = s.builder.number(9, s.number(), kNullLocation);
    auto jump = s.builder.add<JsInstJump>(JsRepId(), kNullLocation);
    s.builder.connect(jump->to, other, join);

    s.builder.switchTo(join);
    auto zero = s.builder.number(0, s.number(), kNullLocation);
    auto ret = s.returns(zero);

    check(s.verify(), "the correct body should verify");

    // The join now reads a value only one arm defines. Block index order accepts it; dominance does
    // not, which is the whole reason the verifier computes one.
    ret->value = inArm - s.builder.base;
    inArm->uses++;

    check(!s.verify(), "a use its definition does not dominate should be rejected");
    check(s.diagnostics.said("does not dominate"_v), "the report should name dominance");
}

/*
 * A function pair used as one value where the plan asks for the pair's *contents* - J66's family.
 *
 * `jsAbiOfType` freezes a function type's convention, and the two ends of an indirect call have to
 * agree about it the way a direct call's do. A `CallValue` whose argument is at the wrong
 * representation is the same failure the direct check catches, asked of the plan the *signature*
 * yields rather than of the one a declaration does.
 */
static void testCallValueDisagreesWithSignature() {
    currentTest = "a call through a value that disagrees with its signature";

    JsirScratch s;
    s.entry();

    JsAbi signature;
    signature.takesEnv = true;
    signature.parameters.push(JsPassing { JsPassingKind::One, s.number(), 1 });
    signature.result = JsPassing { JsPassingKind::One, s.number(), 1 };

    auto abi = s.calleeAbi(::move(signature));

    auto shape = jsFunPairShape(s.target);
    auto erased = s.target.shape(shape).properties[0].rep;

    auto code = s.builder.add<JsInstConst>(erased, kNullLocation, JsConstant {});
    auto environment = s.builder.add<JsInstConst>(erased, kNullLocation, JsConstant {});
    auto argument = s.builder.number(3, s.number(), kNullLocation);

    auto call = s.builder.add<JsInstCallValue>(s.number(), kNullLocation, code->result,
                                               environment->result, abi);
    call->arguments.push(s.target.arena, argument - s.builder.base);
    argument->uses++;

    auto ret = s.returns(call->result);

    check(s.verify(), "the correct call through a value should verify");

    // The pair's convention is one signature both ends read, so an argument at another
    // representation is the disagreement §3.7 exists to make a report rather than a wrong answer.
    argument->rep = jsNumberRep(s.target, NumberDomain::Float64, 64);

    check(!s.verify(), "a call through a value at the wrong representation should be rejected");
    check(s.diagnostics.said("disagrees with its callee's convention"_v),
          "the report should name the convention");
}

/*
 * A `return` that is not this function's own plan - J66's family, and the one the corpus found.
 *
 * `loans/Origins.yana` returned the `{$o, $k}` descriptor where its signature declares the number
 * the descriptor names, and nothing reported: the caller is checked against the callee's plan and
 * the callee was checked against nothing. This is the other half.
 */
static void testReturnDisagreesWithOwnAbi() {
    currentTest = "a return that is not its own plan's";

    JsirScratch s;
    s.entry();

    auto value = s.builder.number(7, s.number(), kNullLocation);
    auto ret = s.returns(value);

    check(s.verify(), "the correct body should verify");

    // The same body answering the cell rather than its contents, which is exactly what a place read
    // as a value used to produce.
    auto cell = s.builder.add<JsInstConst>(jsCellRep(s.target, s.number()), kNullLocation,
                                           JsConstant {});
    ret->value = cell->result;
    s.builder.base[cell->result]->uses++;

    check(!s.verify(), "a return at the wrong representation should be rejected");
    check(s.diagnostics.said("not its own plan's"_v), "the report should name the plan");
}

/*
 * A property place whose shape is the compiler's own, over an owner that is not one of its objects.
 *
 * The `Host` shape kind is the one relaxation of §4.3's most valuable check - `Array(Handle)` is the
 * bare host row and `length` is a property that row keeps itself - and a relaxation that admitted
 * everything would be the check gone. So the exception is the *shape's kind*: an ordinary `Object`
 * shape over a host array is still refused.
 */
static void testHostShapeDoesNotExcuseAnObjectShape() {
    currentTest = "an object shape read out of a host array";

    JsirScratch s;
    auto shape = oneProperty(s);
    auto row = jsHostArrayRep(s.target, s.number());

    s.entry();
    auto array = s.builder.add<JsInstArray>(row, kNullLocation);
    auto owner = s.builder.base[array->result];

    auto read = s.builder.load(s.builder.addProperty(owner, shape, 0), kNullLocation);
    auto ret = s.returns(read);

    check(!s.verify(), "an object shape over a host array should be rejected");
    check(s.diagnostics.said("not a host object"_v), "the report should name the owner");

    // The same place under a `Host` shape is the transparent record's `@host` field and is accepted,
    // which is what says the kind is doing the work rather than the owner's representation.
    s.target.shapes[shape.index].kind = JsShapeKind::Host;
    check(s.verify(), "a host shape over a host array should verify");
}

/*
 * The four checks §3.6 and §3.8 ask for once `Parts` is a value form somebody produces.
 *
 * Each builds the correct body first, which for a multi-part value means a real interned `Parts`
 * representation with a materialized form - a two-lane vector, which is the smallest thing this
 * tier now produces - and then breaks one statement about it.
 */
static JsRepId twoLanes(JsirScratch& s, bool materialized = true) {
    JsRepId lanes[] = { s.number(), s.number() };
    return jsPartsRep(s.target, Buffer<JsRepId> { lanes, 2 },
                      materialized ? jsHostArrayRep(s.target, s.number(), 2) : JsRepId());
}

static void testMaterializeWithoutASingleValueForm() {
    currentTest = "a materialization of parts that have no single-value form";

    JsirScratch s;
    s.entry();

    // The honest half first: a representation *with* a form materializes and splits back.
    auto parts = twoLanes(s);
    auto a = s.builder.number(1, s.number(), kNullLocation);
    auto b = s.builder.number(2, s.number(), kNullLocation);
    JsValue* both[] = { a, b };

    auto row = s.builder.materialize(parts, Buffer<JsValue*> { both, 2 }, kNullLocation);
    check(row != nullptr, "a materialization with a form should be built");

    auto first = s.builder.splitPart(row, parts, 0, kNullLocation);
    auto ret = s.returns(first);

    check(s.verify(), "the correct body should verify");

    // And the mutation: the same parts with their form taken away, which is what a representation
    // family that has none looks like from the verifier's side.
    s.target.reps[parts.index].content = JsRepId();

    check(!s.verify(), "a materialization of formless parts should be rejected");
    check(s.diagnostics.said("no single-value form"_v), "the report should name the form");
}

static void testMaterializationSuppliesEveryPart() {
    currentTest = "a materialization that does not supply every part";

    JsirScratch s;
    s.entry();

    auto parts = twoLanes(s);
    auto a = s.builder.number(1, s.number(), kNullLocation);
    auto b = s.builder.number(2, s.number(), kNullLocation);
    JsValue* both[] = { a, b };

    auto row = s.builder.materialize(parts, Buffer<JsValue*> { both, 2 }, kNullLocation);

    // The split before the `return`, not inside its argument: the builder appends as it is called,
    // so an operand computed in the terminator's own expression lands after it.
    auto second = s.builder.splitPart(row, parts, 1, kNullLocation);
    auto ret = s.returns(second);

    check(s.verify(), "the correct body should verify");

    // A third lane, which the representation does not have - the same check a dropped one trips,
    // and the one this list can be brought to from the outside.
    auto node = (JsInstMaterialize*)s.builder.base[row->definer];
    node->values.push(s.target.arena, a - s.builder.base);
    a->uses++;

    check(!s.verify(), "a materialization that does not supply every part should be rejected");
    check(s.diagnostics.said("every part"_v), "the report should name the parts");
}

static void testSplitOfTheWrongValue() {
    currentTest = "a split of something that is not its parts' single-value form";

    JsirScratch s;
    s.entry();

    auto parts = twoLanes(s);
    auto a = s.builder.number(1, s.number(), kNullLocation);
    auto b = s.builder.number(2, s.number(), kNullLocation);
    JsValue* both[] = { a, b };

    auto row = s.builder.materialize(parts, Buffer<JsValue*> { both, 2 }, kNullLocation);
    auto split = s.builder.splitPart(row, parts, 0, kNullLocation);
    auto ret = s.returns(split);

    check(s.verify(), "the correct body should verify");

    // The split now reads a lane out of a lane - a plain number, which has no parts to name.
    auto node = (JsInstSplit*)s.builder.base[split->definer];
    node->from = a - s.builder.base;

    check(!s.verify(), "a split of a scalar should be rejected");
    check(s.diagnostics.said("single-value form"_v), "the report should name the form");

    /*
     * And the second mutation, which is the shape J78 met rather than a scalar: the materialized
     * form of a **different** multi-part family.
     *
     * Both are legal host values of legal representations, so nothing about either one is wrong on
     * its own and only the identity of the representation separates them - which is exactly why a
     * `represent` that relabelled a window's view object as the `%Array(a)` row went unreported
     * until the split that read the window back out of it. A scalar trips the same check for a
     * reason a pass cannot produce; this is the one it can.
     */
    JsRepId lanes[] = { s.number(), s.number() };
    auto shape = oneProperty(s);
    auto elsewhere = jsPartsRep(s.target, Buffer<JsRepId> { lanes, 2 }, objectRep(s, shape));
    auto foreign = s.builder.materialize(elsewhere, Buffer<JsValue*> { both, 2 }, kNullLocation);

    s.diagnostics.reset();

    // Re-fetched, because appending the materialization above may have moved the instruction
    // region out from under the pointer taken before it.
    ((JsInstSplit*)s.builder.base[split->definer])->from = foreign - s.builder.base;
    foreign->uses++;

    check(!s.verify(), "a split of another family's materialized form should be rejected");
    check(s.diagnostics.said("single-value form"_v), "the report should name the form");
}

/*
 * A multi-part value carried across an edge - one logical argument, two target arguments, and one
 * lane dropped on one predecessor.
 *
 * §3.8's rule is that "the verifier requires all predecessor operands to match the block arguments
 * exactly", and this is the shape it exists for: an accumulator carried around a vector loop, where
 * the two edges into the header have to agree lane for lane. The existing edge case asserts the
 * representation; this one asserts the **arity**, which is the failure a multi-part value adds.
 */
static void testEdgeDropsOneLane() {
    currentTest = "an edge that supplies one lane too few";

    JsirScratch s;
    auto entry = s.entry();
    auto join = s.builder.addBlock(StringId());

    auto parts = twoLanes(s);
    auto first = s.builder.addArgument(join, s.number());
    auto second = s.builder.addArgument(join, s.number());

    auto a = s.builder.number(1, s.number(), kNullLocation);
    auto b = s.builder.number(2, s.number(), kNullLocation);

    auto jump = s.builder.add<JsInstJump>(JsRepId(), kNullLocation);
    s.builder.connect(jump->to, entry, join);
    s.builder.edgeArgument(jump->to, a);
    s.builder.edgeArgument(jump->to, b);

    s.builder.switchTo(join);
    JsValue* both[] = { first, second };
    auto row = s.builder.materialize(parts, Buffer<JsValue*> { both, 2 }, kNullLocation);
    auto taken = s.builder.splitPart(row, parts, 0, kNullLocation);
    auto ret = s.returns(taken);

    check(s.verify(), "the correct body should verify");

    // A third lane on the edge, against the block's two - which is the arity failure a multi-part
    // argument adds and the one a pass that rewrote one end would produce.
    s.builder.edgeArgument(jump->to, a);

    check(!s.verify(), "an edge whose arity is not its target's should be rejected");
    check(s.diagnostics.said("arity disagrees"_v), "the report should name the arity");
}

/*
 * A multi-part parameter passed as one value - the `Parts` half of §3.7's call check.
 *
 * The plan says two positions and the call supplies one, which is exactly the shape an emitter that
 * materialized where it should have flattened produces. The arity check is what catches it, and the
 * per-part representation check below it is what catches the lanes arriving in the wrong order.
 */
static void testCallFlattensItsParts() {
    currentTest = "a call that does not flatten a multi-part parameter";

    JsirScratch s;
    s.entry();

    auto parts = twoLanes(s);

    JsAbi callee;
    callee.parameters.push(JsPassing { JsPassingKind::Parts, parts, 2 });
    callee.result = JsPassing { JsPassingKind::One, s.number(), 1 };

    auto abi = s.calleeAbi(::move(callee));

    auto a = s.builder.number(1, s.number(), kNullLocation);
    auto b = s.builder.number(2, s.number(), kNullLocation);

    auto call = s.builder.add<JsInstCall>(s.number(), kNullLocation, nullptr, abi);
    call->arguments.push(s.target.arena, a - s.builder.base);
    call->arguments.push(s.target.arena, b - s.builder.base);
    a->uses++;
    b->uses++;

    auto ret = s.returns(call->result);

    check(s.verify(), "the correct body should verify");

    // Lane zero replaced by the row the two would have materialized into - the arity still agrees,
    // so what catches it is the per-part representation check and nothing else would.
    JsValue* both[] = { a, b };
    auto row = s.builder.materialize(parts, Buffer<JsValue*> { both, 2 }, kNullLocation);
    call->arguments.set(s.builder.base, 0, row - s.builder.base);
    row->uses++;

    check(!s.verify(), "a call passing a part as one value should be rejected");
    check(s.diagnostics.said("disagrees with its callee's convention"_v),
          "the report should name the convention");
}


/*
 * A **flat reference** passed as the descriptor - J71's half of the same check.
 *
 * `(owner, key)` is two positions and the `{$o, $k}` object is one, so an emitter that materialized
 * where the plan says flat supplies one argument too few. The per-part check cannot catch it - both
 * parts are `Erased` and an erased part accepts any host value, which is the honest statement about
 * an owner that may be any object and a key that may be a string or a number - so the **arity** is
 * the whole of what stands over this, and this case is what says so.
 */
static void testFlatReferenceIsTwoPositions() {
    currentTest = "a flat reference passed as one descriptor";

    JsirScratch s;
    s.entry();

    auto flat = jsCellPartsRep(s.target, s.number());

    JsAbi callee;
    callee.parameters.push(JsPassing { JsPassingKind::Parts, flat, 2 });
    callee.result = JsPassing { JsPassingKind::One, s.number(), 1 };

    auto abi = s.calleeAbi(::move(callee));

    auto shape = oneProperty(s);
    auto owner = s.builder.add<JsInstObject>(objectRep(s, shape), kNullLocation, shape);
    auto zero = s.builder.number(0, s.number(), kNullLocation);
    owner->values.push(s.target.arena, zero - s.builder.base);
    zero->uses++;

    JsConstant text;
    text.kind = JsConstant::Text;
    text.text = s.context.addUnqualifiedName("x", 1);
    auto key = s.builder.constant(text, jsErasedRep(s.target), kNullLocation);

    auto call = s.builder.add<JsInstCall>(s.number(), kNullLocation, nullptr, abi);
    call->arguments.push(s.target.arena, owner->result);
    call->arguments.push(s.target.arena, key - s.builder.base);
    s.builder.base[owner->result]->uses++;
    key->uses++;

    auto ret = s.returns(call->result);

    check(s.verify(), "the correct body should verify");

    // The descriptor the two would have materialized into, in the one position an emitter that
    // did not read the plan would put it.
    JsValue* both[] = { s.builder.base[owner->result], key };
    auto descriptor = s.builder.materialize(flat, Buffer<JsValue*> { both, 2 }, kNullLocation);
    call->arguments.remove(s.builder.base, 1);
    call->arguments.set(s.builder.base, 0, descriptor - s.builder.base);
    descriptor->uses++;

    check(!s.verify(), "a flat reference passed as one value should be rejected");
    check(s.diagnostics.said("arity disagrees"_v), "the report should name the arity");
}

/*
 * The erased ABI's three mutations - §4.5 asked for these before the family's direct path is relied
 * on, and each one is a shape a pass that rewrote one end of the boundary would produce.
 */

/*
 * A call to a body compiled once that hands over no table, and one to an ordinary body that hands
 * over one.
 *
 * Both halves, because the failure is symmetric and only the pair says so: a caller that forgot the
 * table shifts every argument after it by one, and a caller that supplied one to a callee with no
 * parameter for it does the same in the other direction. Neither is visible downstream as anything
 * but a wrong number.
 */
static void testErasedCallCarriesItsEnvironment() {
    currentTest = "a call that disagrees about the generic environment";

    JsirScratch s;
    s.entry();

    JsAbi callee;
    callee.parameters.push(JsPassing { JsPassingKind::One, s.number(), 1 });
    callee.result = JsPassing { JsPassingKind::One, s.number(), 1 };
    callee.takesGenEnv = true;

    auto abi = s.calleeAbi(::move(callee));

    auto table = s.builder.add<JsInstGenTable>(jsGenTableRep(s.target), kNullLocation, nullptr);
    auto argument = s.builder.number(1, s.number(), kNullLocation);

    auto call = s.builder.add<JsInstCall>(s.number(), kNullLocation, nullptr, abi);
    call->genEnv = table->result;
    s.builder.base[table->result]->uses++;
    call->arguments.push(s.target.arena, argument - s.builder.base);
    argument->uses++;

    auto ret = s.returns(call->result);

    check(s.verify(), "the correct body should verify");

    call->genEnv = nullptr;
    s.builder.base[table->result]->uses--;

    check(!s.verify(), "a call to a body compiled once with no table should be rejected");
    check(s.diagnostics.said("generic environment"_v), "the report should name the environment");
}

/*
 * A table that is not one - the cell reader's assumption, stated where it can be checked.
 *
 * Every slot read in an erased body is an element of a host row, so a first argument that is not one
 * is a body indexing a number. The representation is the only thing that says so, since the value
 * arrives as an ordinary argument.
 */
static void testGenericEnvironmentIsATable() {
    currentTest = "a generic environment that is not a table";

    JsirScratch s;
    s.entry();

    JsAbi callee;
    callee.result = JsPassing { JsPassingKind::One, s.number(), 1 };
    callee.takesGenEnv = true;

    auto abi = s.calleeAbi(::move(callee));

    auto table = s.builder.add<JsInstGenTable>(jsGenTableRep(s.target), kNullLocation, nullptr);

    auto call = s.builder.add<JsInstCall>(s.number(), kNullLocation, nullptr, abi);
    call->genEnv = table->result;
    s.builder.base[table->result]->uses++;

    auto ret = s.returns(call->result);

    check(s.verify(), "the correct body should verify");

    // A number in the table's position, which is what a pass that forwarded the wrong operand leaves.
    auto wrong = s.builder.number(7, s.number(), kNullLocation);
    call->genEnv = wrong - s.builder.base;
    wrong->uses++;

    check(!s.verify(), "a generic environment that is not a table should be rejected");
    check(s.diagnostics.said("not a table"_v), "the report should say what it is not");
}

/*
 * A concrete argument at a position the callee did not erase.
 *
 * This is the half the `erased` flag on `JsPassing` had to buy back. The waiver used to be "either
 * side happens to be an `Erased` representation", which let a genuinely mismatched pair through
 * whenever one of them was a type variable's; keyed on the *declaration* instead, a position the
 * signature wrote concretely is held to its representation exactly.
 */
static void testConcretePositionIsStillChecked() {
    currentTest = "a concrete parameter of an erased callee is still checked";

    JsirScratch s;
    s.entry();

    auto text = jsStringRep(s.target);

    JsAbi callee;
    callee.parameters.push(JsPassing { JsPassingKind::One, s.number(), 1 });
    callee.parameters.push(JsPassing { JsPassingKind::One, text, 1 });
    callee.parameters[0].erased = true;
    callee.result = JsPassing { JsPassingKind::One, s.number(), 1 };
    callee.takesGenEnv = true;

    auto abi = s.calleeAbi(::move(callee));

    auto table = s.builder.add<JsInstGenTable>(jsGenTableRep(s.target), kNullLocation, nullptr);

    JsConstant empty;
    empty.kind = JsConstant::Text;
    empty.text = s.context.addUnqualifiedName("", 0);

    auto erased = s.builder.number(1, text, kNullLocation);
    auto concrete = s.builder.constant(empty, text, kNullLocation);

    auto call = s.builder.add<JsInstCall>(s.number(), kNullLocation, nullptr, abi);
    call->genEnv = table->result;
    s.builder.base[table->result]->uses++;
    call->arguments.push(s.target.arena, erased - s.builder.base);
    call->arguments.push(s.target.arena, concrete - s.builder.base);
    erased->uses++;
    concrete->uses++;

    auto ret = s.returns(call->result);

    // The erased position takes a string where the plan says a number, and is admitted: what the
    // callee sees through one is its caller's representation, which no id comparison could state.
    check(s.verify(), "an erased position takes whatever the caller substituted");

    // The concrete one takes a number where the plan says a string, and is not.
    auto wrong = s.builder.number(5, s.number(), kNullLocation);
    call->arguments.set(s.builder.base, 1, wrong - s.builder.base);
    wrong->uses++;

    check(!s.verify(), "a concrete position at the wrong representation should be rejected");
    check(s.diagnostics.said("disagrees with its callee's convention"_v),
          "the report should name the convention");
}

/*
 * An **erased `Cell`** position supplied the value rather than a descriptor - J70.
 *
 * The waiver `JsPassing::erased` grants is about the *content* of a position and not about its
 * convention: a body compiled once reads a `Cell` argument as `x.$o[x.$k]` whatever is in it, so a
 * caller that handed the host object itself - which is what a `Value` place answers when it is asked
 * for its own reference - hands over a descriptor the callee indexes into and finds nothing at. That
 * was a `undefined[undefined]` at run time rather than a report, and `generic/Property.yana` is the
 * fixture it arrived in.
 */
static void testErasedCellStillWantsADescriptor() {
    currentTest = "an erased Cell position still wants a descriptor";

    JsirScratch s;
    s.entry();

    auto object = jsRefShape(s.target);
    auto cell = jsCellRep(s.target, s.number());

    JsAbi callee;
    callee.parameters.push(JsPassing { JsPassingKind::Cell, s.number(), 1 });
    callee.parameters[0].erased = true;
    callee.result = JsPassing { JsPassingKind::One, s.number(), 1 };

    auto abi = s.calleeAbi(::move(callee));

    JsConstant key;
    key.kind = JsConstant::Text;
    key.text = s.context.addUnqualifiedName("$v", 2);

    auto owner = s.builder.add<JsInstObject>(cell, kNullLocation, object);
    auto held = s.builder.number(1, s.number(), kNullLocation);
    auto name = s.builder.constant(key, s.target.shape(object).properties[1].rep, kNullLocation);
    owner->values.push(s.target.arena, held - s.builder.base);
    owner->values.push(s.target.arena, name - s.builder.base);
    held->uses++;
    name->uses++;

    auto call = s.builder.add<JsInstCall>(s.number(), kNullLocation, nullptr, abi);
    call->arguments.push(s.target.arena, owner->result);
    s.builder.base[owner->result]->uses++;

    auto ret = s.returns(call->result);

    // A real descriptor at an erased `Cell` position is admitted whatever its content says, because
    // the content is the half the substitution decides.
    check(s.verify(), "a descriptor should satisfy an erased Cell position");

    // The value itself is not, however plausible it looks beside a concrete `&Point`.
    auto bare = s.builder.number(7, s.number(), kNullLocation);
    call->arguments.set(s.builder.base, 0, bare - s.builder.base);
    bare->uses++;

    check(!s.verify(), "a bare value at an erased Cell position should be rejected");
    check(s.diagnostics.said("disagrees with its callee's convention"_v),
          "the report should name the convention");
}


/*
 * §6.3's copy-to-alias elision, from the one side no fixture can reach: the **callee's** half of the
 * decision.
 *
 * What makes a duplicate removable at a call is `JsPassing::shared` - the declaration saying the
 * position is a loan the callee may only read. No `.yana` program can vary that without varying
 * everything else about itself, so the mutation is done here: the same body is optimized twice, once
 * against a callee whose parameter is shared and once against the same callee with the bit cleared,
 * and the clone has to survive the second. A rewrite that read "is this an argument" rather than "is
 * this a *shared* argument" passes the first check and fails this one.
 *
 * The body is the shape the measurement found: a record in a `Var`, read out and duplicated for a
 * call. `Var` is what the demand pass gives a local whose address never escaped, which is the
 * elision's second condition.
 */
static void testCopyToAliasNeedsASharedPosition() {
    currentTest = "a copy handed to a position the callee may write";

    for(U32 shared = 0; shared < 2; shared++) {
        JsirScratch s;
        s.entry();

        auto shape = oneProperty(s);
        auto record = objectRep(s, shape);

        JsAbi callee;
        callee.parameters.push(JsPassing { JsPassingKind::One, record, 1, false, shared != 0 });
        callee.result = JsPassing { JsPassingKind::One, s.number(), 1 };

        auto abi = s.calleeAbi(::move(callee));

        auto built = s.builder.add<JsInstObject>(record, kNullLocation, shape);
        auto zero = s.builder.number(0, s.number(), kNullLocation);
        built->values.push(s.target.arena, zero - s.builder.base);
        zero->uses++;

        auto slot = s.builder.addVariable(record);
        s.builder.store(slot, s.builder.base[built->result], kNullLocation);

        auto held = s.builder.load(slot, kNullLocation);
        auto copy = s.builder.add<JsInstClone>(record, kNullLocation, held - s.builder.base);
        held->uses++;

        auto call = s.builder.add<JsInstCall>(s.number(), kNullLocation, nullptr, abi);
        call->arguments.push(s.target.arena, copy->result);
        s.builder.base[copy->result]->uses++;

        auto ret = s.returns(call->result);

        check(s.verify(), "the correct body should verify");
        check(optimizeBody(s.target, s.body), "the optimizer should not report");
        check(s.verify(), "the optimized body should verify");

        auto aliased = s.target.optimizer.aliased == 1;
        if(shared) {
            check(aliased, "a copy at a shared position should become an alias");
        } else {
            check(!aliased, "a copy at a position the callee may write should stay a copy");
        }

        check(s.target.optimizer.clones == 1, "the clone should have been counted once");
    }
}

/*
 * J73's `adoptInitialStores`, and the two conditions no `.yana` program varies on its own.
 *
 * The rewrite takes the storage an escaping local *is* from the first value written into it rather
 * than building it at its zero and overwriting it, and what makes that sound is an ordering fact and
 * a sharing fact. Neither can be turned off from the language: the conversion emits the zero and the
 * store together, so a corpus fixture that had a use between them would have to be a body the
 * conversion does not produce, and a stored value with a second name is a body `opt_alias`'s own
 * conditions would already have changed. So both are asserted here, as the same body built twice.
 */
static void testAdoptionNeedsTheStoreFirst() {
    currentTest = "an initial store with the zero named in front of it";

    for(U32 between = 0; between < 2; between++) {
        JsirScratch s;
        s.entry();

        auto shape = oneProperty(s);
        auto record = objectRep(s, shape);

        auto zero = s.builder.number(0, s.number(), kNullLocation);
        auto built = s.builder.add<JsInstObject>(record, kNullLocation, shape);
        built->values.push(s.target.arena, zero - s.builder.base);
        zero->uses++;

        auto storage = s.builder.base[built->result];
        auto place = s.builder.addValuePlace(storage, record);

        // The use that has to refuse the rewrite: a read of the storage before anything filled it.
        if(between) {
            auto early = s.builder.addProperty(storage, shape, 0);
            s.builder.load(early, kNullLocation);
        }

        auto one = s.builder.number(1, s.number(), kNullLocation);
        auto filled = s.builder.add<JsInstObject>(record, kNullLocation, shape);
        filled->values.push(s.target.arena, one - s.builder.base);
        one->uses++;

        s.builder.store(place, s.builder.base[filled->result], kNullLocation);

        auto read = s.builder.load(s.builder.addProperty(storage, shape, 0), kNullLocation);
        auto ret = s.returns(read);

        check(s.verify(), "the correct body should verify");
        check(optimizeBody(s.target, s.body), "the optimizer should not report");
        check(s.verify(), "the optimized body should verify");

        if(between) {
            check(s.target.optimizer.adopted == 0,
                  "a zero something reads before the store should keep its own object");
        } else {
            check(s.target.optimizer.adopted == 1,
                  "a zero nothing names before the store should be taken from it");
        }
    }
}

static void testAdoptionNeedsTheStoredValueUnshared() {
    currentTest = "an initial store whose value has a second name";

    for(U32 shared = 0; shared < 2; shared++) {
        JsirScratch s;
        s.entry();

        auto shape = oneProperty(s);
        auto record = objectRep(s, shape);

        JsAbi callee;
        callee.parameters.push(JsPassing { JsPassingKind::One, record, 1, false, true });
        callee.result = JsPassing { JsPassingKind::One, s.number(), 1 };

        auto abi = s.calleeAbi(::move(callee));

        auto zero = s.builder.number(0, s.number(), kNullLocation);
        auto built = s.builder.add<JsInstObject>(record, kNullLocation, shape);
        built->values.push(s.target.arena, zero - s.builder.base);
        zero->uses++;

        auto storage = s.builder.base[built->result];
        auto place = s.builder.addValuePlace(storage, record);

        auto one = s.builder.number(1, s.number(), kNullLocation);
        auto filled = s.builder.add<JsInstObject>(record, kNullLocation, shape);
        filled->values.push(s.target.arena, one - s.builder.base);
        one->uses++;

        // The second name: the same object handed to a callee as well as stored.
        if(shared) {
            auto call = s.builder.add<JsInstCall>(s.number(), kNullLocation, nullptr, abi);
            call->arguments.push(s.target.arena, filled->result);
            s.builder.base[filled->result]->uses++;
        }

        s.builder.store(place, s.builder.base[filled->result], kNullLocation);

        auto read = s.builder.load(s.builder.addProperty(storage, shape, 0), kNullLocation);
        auto ret = s.returns(read);

        check(s.verify(), "the correct body should verify");
        check(optimizeBody(s.target, s.body), "the optimizer should not report");
        check(s.verify(), "the optimized body should verify");

        if(shared) {
            check(s.target.optimizer.adopted == 0,
                  "a stored value somebody else also names should not become the storage");
        } else {
            check(s.target.optimizer.adopted == 1,
                  "a stored value nothing else names should become the storage");
        }
    }
}

/*
 * The three rules a **niche-folded** representation adds - §3.2's `Nullable`, from the end that can
 * catch a disagreement about it.
 *
 * None of them is reachable from a `.yana` program, for `JsPassing::shared`'s reason: `jsRep` is
 * what decides a fold and what builds the `null`, so a fixture cannot have one without the other.
 * What each case does is build the body a correct fold produces, assert that it verifies, and then
 * move exactly the one thing the rule is about.
 */
static JsRepId nullableRep(JsirScratch& s, JsRepId payload) {
    JsRep rep;
    rep.kind = JsRepKind::Nullable;
    rep.content = payload;
    rep.tag = jsNumberRep(s.target, NumberDomain::Unsigned, 8);
    rep.absentTag = 0;
    rep.presentTag = 1;

    auto id = JsRepId(s.target.reps.size());
    s.target.reps.push(rep);
    return id;
}

static void testNullIsNotAValueOfANumber() {
    currentTest = "a null at a representation with no absent value";

    JsirScratch s;
    s.entry();

    auto folded = nullableRep(s, objectRep(s, oneProperty(s)));

    JsConstant absent;
    absent.kind = JsConstant::Null;
    auto value = s.builder.constant(absent, folded, kNullLocation);

    auto ret = s.returns(value);

    // The plan says the function answers a number, so the `return` is checked against that too; the
    // fold is what this case is about, so the plan is moved with the value.
    s.target.abis[s.body.abi.index].result = JsPassing { JsPassingKind::One, folded, 1 };
    check(s.verify(), "a null at a niche-folded representation should verify");

    /*
     * The same `null` at the payload's *number*, which is what a fold taken over a representation
     * that has no spare pattern produces - `Analysis-IR-JS.md` §1.2's shipping-backend defect, where
     * the value reads back wrong everywhere instead of throwing where it was written.
     */
    auto number = s.number();
    value->rep = number;
    s.target.abis[s.body.abi.index].result = JsPassing { JsPassingKind::One, number, 1 };

    check(!s.verify(), "a null at a number should be rejected");
    check(s.diagnostics.said("no absent value"_v), "the report should say there is no absent value");
}

static void testNicheDoesNotNest() {
    currentTest = "a niche fold over a payload that is already nullable";

    JsirScratch s;
    s.entry();

    auto inner = nullableRep(s, objectRep(s, oneProperty(s)));
    auto outer = nullableRep(s, inner);

    JsConstant absent;
    absent.kind = JsConstant::Null;
    auto value = s.builder.constant(absent, inner, kNullLocation);

    auto ret = s.returns(value);
    s.target.abis[s.body.abi.index].result = JsPassing { JsPassingKind::One, inner, 1 };

    check(s.verify(), "one fold over an object should verify");

    // `Maybe(Maybe(T))`: one `null` standing for two values, which is the nesting §3.2 forbids and
    // the reason the outer of the two keeps its `$tag`.
    value->rep = outer;
    s.target.abis[s.body.abi.index].result = JsPassing { JsPassingKind::One, outer, 1 };

    check(!s.verify(), "a fold over a nullable payload should be rejected");
    check(s.diagnostics.said("already absent-valued"_v), "the report should name the nesting");
}

static void testNichePayloadPlaceNamesItsFold() {
    currentTest = "a niche payload place whose property is not that fold";

    JsirScratch s;
    s.entry();

    auto payload = objectRep(s, oneProperty(s));
    auto folded = nullableRep(s, payload);

    // A record with one property holding the folded value, and a place over that property read at
    // the payload's representation - which is what a projection into `Just` is.
    JsShape holder;
    holder.kind = JsShapeKind::Object;
    holder.properties.push(JsProperty {
        s.context.addUnqualifiedName("m", 1), folded, JsPropertyRole::Field, 0
    });

    auto holderShape = JsShapeId(s.target.shapes.size());
    s.target.shapes.push(::move(holder));
    auto holderRep = objectRep(s, holderShape);

    auto object = s.builder.add<JsInstObject>(holderRep, kNullLocation, holderShape);
    auto field = s.builder.constant(JsConstant(), folded, kNullLocation);
    object->values.push(s.target.arena, field - s.builder.base);
    field->uses++;

    auto owner = s.builder.base[object->result];
    auto whole = s.builder.addProperty(owner, holderShape, 0);
    auto inner = s.builder.addNichePayload(whole, payload);
    auto read = s.builder.load(inner, kNullLocation);

    auto ret = s.returns(read);
    s.target.abis[s.body.abi.index].result = JsPassing { JsPassingKind::One, payload, 1 };

    check(s.verify(), "a payload place over its own fold should verify");

    // The licence is for the payload of *this* property's fold and for nothing else: a place that
    // reads some other representation out of the same property is a value built one way and read
    // the other, which is the check the flag would otherwise switch off.
    s.builder.place(inner).content = s.number();
    read->rep = s.number();
    s.target.abis[s.body.abi.index].result = JsPassing { JsPassingKind::One, s.number(), 1 };

    check(!s.verify(), "a payload place at a foreign representation should be rejected");
    check(s.diagnostics.said("not that fold"_v), "the report should name the fold");
}

int main() {
    /*
     * **In definition order**, and the list is the file read top to bottom.
     *
     * The order used to be the order cases were added in, which is neither the file's nor anything
     * else: a reader comparing the two had to check twenty-seven names by eye to see that none had
     * been left out. Keeping the two the same makes an omission a diff rather than an absence.
     */
    testNumberAsObjectReceiver();
    testPartsUsedAsOneValue();
    testEdgeRepresentationMismatch();
    testMissingDefinition();
    testStoreRepresentationMismatch();
    testBoxedGlobalReadUnboxed();
    testCallDisagreesWithAbi();
    testDefinitionDoesNotDominate();
    testCallValueDisagreesWithSignature();
    testReturnDisagreesWithOwnAbi();
    testHostShapeDoesNotExcuseAnObjectShape();
    testMaterializeWithoutASingleValueForm();
    testMaterializationSuppliesEveryPart();
    testSplitOfTheWrongValue();
    testEdgeDropsOneLane();
    testCallFlattensItsParts();
    testFlatReferenceIsTwoPositions();
    testErasedCallCarriesItsEnvironment();
    testGenericEnvironmentIsATable();
    testConcretePositionIsStillChecked();
    testErasedCellStillWantsADescriptor();
    testCopyToAliasNeedsASharedPosition();
    testAdoptionNeedsTheStoreFirst();
    testAdoptionNeedsTheStoredValueUnshared();
    testNullIsNotAValueOfANumber();
    testNicheDoesNotNest();
    testNichePayloadPlaceNamesItsFold();

    if(failures) {
        println("%@ failures.", failures);
        return 1;
    }

    println("Pass.");
    return 0;
}
