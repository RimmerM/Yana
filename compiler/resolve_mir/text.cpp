#include "resolve_internal.h"

namespace mir {

/*
 * `"abc"` as a **constant** - the three records a literal string is, spelled by the compiler.
 *
 * **This used to run the library's own `stringLiteral`** and take whatever came back. That was
 * defensible while the compiler claimed not to know a `String`'s shape, and it stopped being
 * defensible once `resolve_ssa/opt/place.cpp` began reading `Run`'s three fields by position: the
 * layout was in two places, one of which could disagree silently, and the interpreted one was the
 * expensive one - a body run per distinct literal, plus a `teardownRunsNothing` beside it, plus an
 * aggregate per byte for the bytes it addressed. J46 §3.5 is the argument and
 * `test/bench/findings.md` §94 is the measurement.
 *
 * What a literal string is, written out:
 *
 *  - the **bytes**, a constant global of `[CodeUnit *n]` whose initializer is the text itself;
 *  - a `Run(CodeUnit)` of that address, `n` slots, and `runBorrowed` - because a literal's bytes
 *    are the image's and nobody hands them back, which is the same flag a frame-placed run gets and
 *    for the same reason;
 *  - an `Array(CodeUnit)` of that run at length `n`;
 *  - a `String` over that array.
 *
 * `runBorrowed` is what makes the teardown nothing, and it is nothing **by construction** rather
 * than by running `Reclaim` to find out - which is what `markInertConstant` is told below.
 *
 * On JavaScript none of that exists and the constant is the text itself - see the arm at the top,
 * which is the one place the two targets' literals differ.
 */
Constant Resolver::stringConstant(StringId written, LocationId source) {
    auto declared = program.roles.record(intrinsic::Known::string);
    if(!declared) {
        reportMissingRole(intrinsic::Known::string, "a string literal"_v, source);
        return Constant();
    }

    /*
     * The **second arm** `Resolver::textValue`'s comment predicted - "the day a target's string has
     * no static form is the day this grows a second arm again, and it will be *this* function that
     * grows it". That day is a JavaScript build.
     *
     * A host string has no run, no bytes of its own and no address: what a literal *is* there is the
     * text, handed to the engine, and everything the native arm below builds - a `[CodeUnit *n]`
     * global, a `Run` of borrowed slots, an `Array` over it, a relocation into the image - describes
     * storage this target does not have. So the constant stays a `Constant::Text` at the `string`
     * record's own type, which is exactly what that kind was written to mean: `constant.h` says "a
     * native build wants a run in static storage and a descriptor pointing into it, JS wants the
     * host's own string", and the kind exists so that the answer is the *target's* rather than this
     * tier's. JSIR is what turns one into a host string literal.
     *
     * No `markInertConstant`: that records that a native literal's teardown is nothing *by
     * construction* because its run is borrowed. Here the record owes no teardown at all - the
     * collector owns every host string, which is why `lib/` declares no `Reclaim(String)` for this
     * target - so `isLinear` answers false and there is nothing to record.
     */
    if(isJsMode(context.settings)) return textConstant(DeclPtr<Type>(declared.offset), written);

    auto& record = *(RecordType*)decl()[declared];
    if(record.fields.size() != 1) {
        context.diagnostics.error(
            "this library's `String` is not the one record a string literal is built out of"_v,
            source);
        return Constant();
    }

    auto unit = program.types.codeUnit;
    auto shaped = arrayShape(*this, unit, source);
    if(!shaped) return Constant();

    auto& shape = shaped.unwrap();
    auto count = U64(context.findName(written).size());
    auto bytes = stringBytesGlobal(written, source);

    Constant runParts[3];
    runParts[RunShape::kItems] = addressConstant(program.types.pointerType(unit), bytes);
    runParts[RunShape::kCapacity] = intConstant(shape.capacity, count);
    runParts[RunShape::kOwnsHeap] = intConstant(shape.flag, RunShape::kBorrowed);

    auto run = program.aggregateConstant(shape.run, 0, runParts, 3);

    Constant arrayParts[2];
    arrayParts[RunShape::kRun] = run;
    arrayParts[RunShape::kLength] = intConstant(shape.length, count);

    auto array = program.aggregateConstant(shape.array, 0, arrayParts, 2);

    /*
     * And the string over it. `record.field(decl(), 0)`'s type rather than `shape.array`, because
     * what the field says is what the aggregate is checked against - a library whose `String` holds
     * something other than this array is one this declines to build a literal for.
     */
    auto held = record.field(decl(), 0).occurrence.type;
    if(!held || !sameType(held, shape.array)) {
        context.diagnostics.error(
            "this library's `String` does not hold an array of this target's code units"_v, source);
        return Constant();
    }

    auto value = program.aggregateConstant(DeclPtr<Type>(declared.offset), 0, &array, 1);

    /*
     * The teardown is nothing, recorded rather than discovered.
     *
     * `Reclaim(String)` reaches `releaseRun`, which is a test of the flag above - and the flag is
     * `runBorrowed`, so the test fails and the call does nothing. Asked by running the bodies it
     * was one interpretation per distinct literal; known from the flag it is this line. What it
     * buys is that a slot holding a literal owes no teardown call at all.
     */
    if(isLinear(decl(), value.type)) program.markInertConstant(value);

    return value;
}

/*
 * Every text leaf of a value, built - the deferral applied to an aggregate.
 *
 * A walk rather than a special case at the array, because a text may sit at any depth: `data Row
 * {name: String}` in a fixed array is two levels down and is the same statement. The aggregate is
 * re-interned through `aggregateConstant` where a leaf moved, which keeps `Constant`'s equality the
 * comparison of three words it is everywhere else.
 */
Constant Resolver::textsResolved(const Constant& constant, LocationId source) {
    if(constant.kind == Constant::Text) return stringConstant(constant.asText(), source);
    if(constant.kind != Constant::Aggregate) return constant;

    auto& parts = *decl()[constant.aggregate()];
    SmallArray<Constant, 8> values;
    auto moved = false;

    for(auto value: parts.values.contents(decl())) {
        auto built = holdsText(decl(), value) ? textsResolved(value, source) : value;
        if(!built.isSet()) return Constant();

        moved = moved || !(built == value);
        values.push(built);
    }

    if(!moved) return constant;

    return program.aggregateConstant(constant.type, parts.constructor, values.pointer(),
                                     values.size());
}

} // namespace mir
