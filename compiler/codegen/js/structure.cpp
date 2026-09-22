#include "jsir.h"
#include <cmath>
#include <cstring>
#include "../../resolve_ssa/js/name.h"
#include "../../resolve/type.h"

/*
 * JSIR -> structured JavaScript - Analysis-IR-JS.md §2.3's boundary, from below.
 *
 * Two jobs, and they are the two §2.3 puts on this side of the line.
 *
 * **Control flow becomes syntax.** JS has no `goto`, so a CFG has to be recovered as `if`,
 * `for(;;)` and labelled `break`/`continue`. The algorithm is `flow.cpp`'s, which is the shipping
 * backend's and is kept deliberately: a branch becomes an `if` whose arms are emitted up to the
 * branch's post-dominator, and a block with an edge back to it becomes a labelled `for(;;)` whose
 * body is emitted up to *its* follow. Two facts make that a small pass rather than a Relooper - the
 * resolver only produces reducible graphs, and the immediate post-dominator of a branch *is* the
 * block its arms join at. Reinventing it would have been a second answer to a question that has one.
 *
 * **Representation becomes spelling.** Every arithmetic spelling here is the one `codegen/js/inst.cpp`
 * and `type.cpp` already worked out - `Math.imul` for a 32-bit multiply, `$div` for a division the
 * host answers differently, `BigInt.asIntN` above 53 bits, `wide.cpp`'s helpers between 33 and 53.
 * They are copied rather than approximated, because the emitted arithmetic has to be at least as
 * compact and as fast as the pipeline this one replaces, and each of those spellings is a
 * measurement rather than a preference.
 *
 * What this file may **not** do is ask what a value is. Everything it switches on is a `JsRep`, a
 * `JsShape` or a `JsPlace` - never a `mir::Type`, which §8.1 rules out and which is the seam the
 * shipping backend does not have.
 */

namespace js {

using namespace jsir;

namespace {

/*
 * The labelled regions one block owns, and the two halves a loop header splits them into.
 *
 * One of these exists per call of `doTree`, which recurses through `emitBlocks` to the depth of the
 * dominator tree - so an `Array` here is `compiler/util/README.md`'s "a list whose lifetime is one
 * call", three of them per block that owns a region. Eight is that README's usual guess and is
 * comfortably above what a block owns in this corpus.
 */
using RegionList = SmallArray<U32, 8>;

struct Structurer {
    Structurer(Assembly& a, JsBody& body):
        a(a), body(body), target(a.target), ir(*a.target.arena) {}

    Assembly& a;
    JsBody& body;
    JsTarget& target;
    JsirBase ir;

    bool failed = false;

    /*
     * Set where the structured walk meets an edge whose label is not open, or a block it has already
     * written - which together are the one failure that is not a defect, and which a reducible graph
     * cannot produce. See `emitDispatch`. It is separate from `failed` because the answer to it is a
     * second attempt rather than a diagnostic.
     */
    bool unstructured = false;

    // How many blocks the entry reaches, which is what a reverse postorder number below says.
    U32 reachedCount = 0;

    /*
     * The module-level binding one thrown sentence is, interned per text.
     *
     * Written out at every site it would be 38 bytes per bounds check and there are 936 of them; the
     * name is four. See `Assembly::throwMessages`, and `assemble.cpp` for where the bindings go.
     */
    Name throwMessage(StringId text) {
        // A linear scan rather than a map: a program has a handful of distinct sentences in it - the
        // library's one check message and whatever `hostThrow` a body wrote - against hundreds of
        // sites, so the table is shorter than a hash of the text.
        for(Size i = 0; i < a.throwTexts.size(); i++) {
            if(a.throwTexts[i] == text) return a.throwOrder[i];
        }

        auto name = uniqueName(a, "$t"_v, false);
        a.throwOrder.push(name);
        a.throwTexts.push(text);
        return name;
    }

    void fail(StringView what, LocationId where) {
        failed = true;
        target.reported = true;
        a.context.diagnostics.error("internal: the JSIR emitter %@"_v, where, what);
    }

    const JsRep& rep(JsRepId id) { return target.rep(id); }


    // ---------------------------------------------------------------- constant maps

    /*
     * A `match` over literal constants, in the cheapest of five spellings (J88).
     *
     * §5.6 left which of the native shapes (`resolve_ssa/lower/map.cpp`) are worth having here to
     * measurement, and the measurement is pure-JS microbenchmarks - 39M calls, CPU time, eleven
     * interleaved rounds, a control column within 1%, and 35 ms the loop floor:
     *
     *   | arms                 | chain    | array `T[s]` | `switch` |
     *   | 3                    | 35 ms    | 35 ms        | 36 ms    |
     *   | 6 / 12 / 24 uniform  | ~188 ms  | ~35 ms       | ~187 ms  |
     *   | skewed 90% to arm 0  | 62-76 ms | 35 ms        | 63 ms    |
     *
     * So a table wins from six arms up and ties at three, and `switch` is the chain under another
     * name - the engine does not make it a jump table for a value this small. What is tried, in
     * the order of what each costs to run, which is `map.cpp`'s order:
     *
     * - **one literal**, where every arm (and an open map's fallback) answers the same;
     * - a **range test**, where two answers are all there are and the selectors of one of them are
     *   one run - of the domain when the map is closed, of the integers when it is open. A `Bool`
     *   property is the case `map.cpp` has it for, and the rule is not about `Bool` here;
     * - an **affine step** `s * k + b`, where every answer is an integer on one line through the
     *   selectors - closed, or open over a dense run with the run tested first;
     * - a **table**, a module-level array read as `T[s - lowest]`, from four arms up and where the
     *   selectors are dense enough (`kMapTableSlack`) that the row stays small. Interned by content
     *   and named `$m`, `$m$1` in the order the bodies ask. An open map reads `T[i] ?? fallback`
     *   where no arm is `null` - out of range is `undefined` - and tests the range where one is;
     * - otherwise the **chain** of `===` tests it always was.
     *
     * Only literal arms reach here: an arm that is an object is a `Select` chain built in
     * `convert.cpp` and never a table (J69 - a shared row of objects would let a write through one
     * answer change the next call's), and each of its object literals has one use, so the AST
     * inliner puts it in its arm and only the chosen one is built. An arm answering the same
     * record as the fallback is neither built nor tested (`convert.cpp`).
     *
     * next: `map.cpp`'s two remaining shapes - a `Bool` property as the bits of one immediate, and
     * a whole map packed into one - are table-free and would serve a sparse `Bool` map the table
     * refuses; and a table read at a selector only the AST optimizer finds constant (a local it
     * forwards) is left as `$m[2]` where the chain folded to the arm, which `Constant.yana` shows.
     */
    static constexpr I64 kMapTableSlack = 4;
    static constexpr I64 kMapTableLimit = 256;
    static constexpr U32 kMapTableArms = 4;

    static bool sameLiteral(const JsConstant& x, const JsConstant& y) {
        if(x.kind != y.kind) return false;

        switch(x.kind) {
            // Bits rather than `==`, for `map.cpp`'s reason: `-0` and `0` are different answers.
            case JsConstant::Number:
            case JsConstant::Bool:   return sameNumberBits(x.number, y.number) &&
                                            x.integral == y.integral;
            case JsConstant::BigInt: return x.bits == y.bits && x.isSigned == y.isSigned;
            case JsConstant::Text:   return x.text == y.text;
            default:                 return true;
        }
    }

    static bool integralNumber(const JsConstant& x, I64& into) {
        if(x.kind != JsConstant::Number || !x.integral) return false;
        if(!(x.number >= -9007199254740992.0 && x.number <= 9007199254740992.0)) return false;
        if(x.number != F64(I64(x.number))) return false;
        if(x.number == 0 && std::signbit(x.number)) return false;

        into = I64(x.number);
        return true;
    }

    JsPtr<Expr> integer(I64 value) {
        return number(a, F64(value), true);
    }

    // `lo <= s && s <= hi`, leaving out a bound the domain already guarantees.
    JsPtr<Expr> rangeTest(JsPtr<Expr> selector, I64 lo, I64 hi, bool lowKnown, bool highKnown) {
        if(lo == hi) return binary(a, BinaryOp::Eq, selector, integer(lo));

        auto low = lowKnown ? JsPtr<Expr>() : binary(a, BinaryOp::Ge, selector, integer(lo));
        auto high = highKnown ? JsPtr<Expr>() : binary(a, BinaryOp::Le, selector, integer(hi));

        if(!low) return high;
        if(!high) return low;
        return binary(a, BinaryOp::LogicalAnd, low, high);
    }

    Name mapTable(const JsConstant* cells, Size size) {
        for(U32 i = 0; i < a.mapNames.size(); i++) {
            auto start = a.mapStart[i];
            auto end = i + 1 < a.mapNames.size() ? a.mapStart[i + 1] : U32(a.mapCells.size());
            if(end - start != size) continue;

            auto same = true;
            for(U32 j = 0; same && j < size; j++) same = sameLiteral(a.mapCells[start + j], cells[j]);
            if(same) return a.mapNames[i];
        }

        auto name = uniqueName(a, "$m"_v, false);
        a.mapNames.push(name);
        a.mapStart.push(U32(a.mapCells.size()));
        for(Size j = 0; j < size; j++) a.mapCells.push(cells[j]);
        return name;
    }

    JsPtr<Expr> mapChain(JsInstMap& written, JsPtr<Expr> selector) {
        auto answer = mapLiteral(a, written.fallback);
        for(Size i = written.entries.size(); i > 0; i--) {
            auto entry = written.entries.get(ir, U32(i - 1));
            answer = ternary(a, binary(a, BinaryOp::Eq, selector, number(a, entry.from)),
                             mapLiteral(a, entry.to), answer);
        }

        return answer;
    }

    JsPtr<Expr> mapExpr(JsInstMap& written, JsPtr<Expr> selector) {
        auto closed = written.closed;
        auto& fallback = written.fallback;

        // Every arm, the closed map's last one included, with its selector as an integer.
        struct Arm { I64 key; JsConstant to; };
        SmallArray<Arm, 16> arms;

        auto keyed = rep(ir[written.selector]->rep).kind == JsRepKind::Number;
        auto addArm = [&](F64 from, const JsConstant& to) {
            JsConstant key;
            key.number = from;
            I64 value = 0;
            if(!integralNumber(key, value)) keyed = false;
            arms.push(Arm { value, to });
        };

        for(auto entry: written.entries.contents(ir)) addArm(entry.from, entry.to);
        if(closed) addArm(written.closedFrom, fallback);

        if(!keyed || arms.size() == 0) return mapChain(written, selector);

        // A selector the emitter already knows is the arm itself - which the chain got for free
        // from the AST optimizer folding `2 === 0 ? ...`, and a table read would not.
        auto definer = ir[written.selector]->definer;
        auto constant = definer && ir[definer]->kind == JsInst::Const &&
                        ((JsInstConst*)ir[definer])->constant.kind == JsConstant::Number;

        if(constant || a.base[selector]->kind == Expr::Number) {
            auto known = constant ? ((JsInstConst*)ir[definer])->constant.number
                                  : ((NumberExpr*)a.base[selector])->value;
            for(auto& arm: arms) {
                if(F64(arm.key) == known) return mapLiteral(a, arm.to);
            }

            if(!closed) return mapLiteral(a, fallback);
        }

        auto lowest = arms[0].key;
        auto highest = arms[0].key;
        for(auto& arm: arms) {
            if(arm.key < lowest) lowest = arm.key;
            if(arm.key > highest) highest = arm.key;
        }

        // Selectors are distinct, so a span equal to the count is every integer between the ends.
        auto count = I64(arms.size());
        auto dense = highest - lowest + 1 == count;

        // One literal.
        auto uniform = closed || sameLiteral(fallback, arms[0].to);
        for(auto& arm: arms) {
            if(!sameLiteral(arm.to, arms[0].to)) uniform = false;
        }

        if(uniform) return mapLiteral(a, arms[0].to);

        /*
         * Two answers, one of them on a run. Closed, the run is over the *domain* - a selector
         * between two of the arms is not a value the discriminant carries - so either answer may be
         * the one tested, and the bound at an end of the domain is left out. Open, the run has to be
         * the integers themselves, and the other answer is the fallback.
         */
        {
            auto tested = closed ? arms[0].to : fallback;
            JsConstant other;
            auto two = true;
            auto found = false;

            for(auto& arm: arms) {
                if(sameLiteral(arm.to, tested)) continue;
                if(!found) { other = arm.to; found = true; continue; }
                if(!sameLiteral(arm.to, other)) two = false;
            }

            if(two && found) {
                // `run` is the answer the test selects; `rest` is what it does not.
                auto attempt = [&](const JsConstant& run, const JsConstant& rest, JsPtr<Expr>& out) -> U32 {
                    I64 lo = 0, hi = 0, inRun = 0;
                    for(auto& arm: arms) {
                        if(!sameLiteral(arm.to, run)) continue;
                        if(!inRun || arm.key < lo) lo = arm.key;
                        if(!inRun || arm.key > hi) hi = arm.key;
                        inRun++;
                    }

                    if(!inRun) return 0;

                    if(closed) {
                        for(auto& arm: arms) {
                            if(arm.key >= lo && arm.key <= hi && !sameLiteral(arm.to, run)) return 0;
                        }
                    } else if(hi - lo + 1 != inRun) {
                        return 0;
                    }

                    auto lowKnown = closed && lo == lowest;
                    auto highKnown = closed && hi == highest;
                    out = ternary(a, rangeTest(selector, lo, hi, lowKnown, highKnown),
                                  mapLiteral(a, run), mapLiteral(a, rest));
                    return lo == hi ? 1 : U32(!lowKnown) + U32(!highKnown);
                };

                JsPtr<Expr> first, second;
                U32 firstCost = closed ? attempt(other, tested, first) : attempt(other, fallback, first);
                U32 secondCost = closed ? attempt(tested, other, second) : 0;

                // The chain's own orientation where the two cost the same - it tests the first
                // arm's answer - so that a two-arm map reads as it always did.
                if(secondCost && (!firstCost || secondCost <= firstCost)) return second;
                if(firstCost) return first;
            }
        }

        /*
         * `s * k + b`, solved from two arms and checked against every one, over a domain the arms
         * are the whole of - or over a dense run an open map tests first. A negative step is spelled
         * `b - s * |k|` so that a zero selector never makes `-0`.
         *
         * Checked in *doubles'* terms as well as in `I64`'s: every product `key * |k|`, the bias
         * and every answer must be exact integers of a double (J91), since that is what the host
         * computes with. `1000000008 -> 99999999` in steps of 99999999 is a product past 2^53, and
         * the host answered 100000000 for it; a table or a chain is exact.
         */
        if((closed || dense) && arms.size() >= 2) {
            SmallArray<I64, 16> values;
            auto numeric = true;
            for(auto& arm: arms) {
                I64 value = 0;
                if(!integralNumber(arm.to, value)) { numeric = false; break; }
                values.push(value);
            }

            auto runKey = arms[1].key - arms[0].key;
            auto runValue = numeric ? values[1] - values[0] : 0;

            if(numeric && runKey && !(runValue % runKey) && runValue) {
                auto stride = runValue / runKey;
                I64 first = 0, bias = 0;
                constexpr I64 kExact = I64(1) << 53;
                auto exact = [&](I64 x) { return x >= -kExact && x <= kExact; };
                auto fits = !__builtin_mul_overflow(arms[0].key, stride, &first) &&
                            !__builtin_sub_overflow(values[0], first, &bias) && exact(bias);
                for(Size i = 0; fits && i < arms.size(); i++) {
                    I64 term = 0, total = 0;
                    fits = !__builtin_mul_overflow(arms[i].key, stride, &term) &&
                           !__builtin_add_overflow(term, bias, &total) && total == values[i] &&
                           exact(term) && exact(total);
                }

                if(fits) {
                    auto magnitude = stride < 0 ? -stride : stride;
                    auto scaled = magnitude == 1 ? selector : binary(a, BinaryOp::Mul, selector, integer(magnitude));

                    JsPtr<Expr> step;
                    if(stride < 0)      step = binary(a, BinaryOp::Sub, integer(bias), scaled);
                    else if(bias > 0)   step = binary(a, BinaryOp::Add, scaled, integer(bias));
                    else if(bias < 0)   step = binary(a, BinaryOp::Sub, scaled, integer(-bias));
                    else                step = scaled;

                    if(closed) return step;
                    return ternary(a, rangeTest(selector, lowest, highest, false, false), step,
                                   mapLiteral(a, fallback));
                }
            }
        }

        /*
         * A table. Every cell a literal the host holds as itself - an `undefined` would read the same
         * as out of range - and the fallback too where the map is open, since it fills the holes.
         */
        auto span = highest - lowest + 1;
        auto tabled = arms.size() >= kMapTableArms && span <= kMapTableLimit &&
                      span <= kMapTableSlack * count;

        auto nullArm = false;
        auto literalKind = [](const JsConstant& x) {
            return x.kind == JsConstant::Number || x.kind == JsConstant::BigInt ||
                   x.kind == JsConstant::Text || x.kind == JsConstant::Null;
        };

        for(auto& arm: arms) {
            if(!literalKind(arm.to)) tabled = false;
            if(arm.to.kind == JsConstant::Null) nullArm = true;
        }

        if(!closed && !literalKind(fallback)) tabled = false;
        if(!tabled) return mapChain(written, selector);

        // A short run of padding in front is cheaper than a subtraction at every read.
        auto start = lowest > 0 && lowest <= 4 ? I64(0) : lowest;
        auto filler = closed ? arms[0].to : fallback;

        SmallArray<JsConstant, 32> cells;
        for(auto i = start; i <= highest; i++) cells.push(filler);
        for(auto& arm: arms) cells[Size(arm.key - start)] = arm.to;

        auto table = mapTable(&cells[0], cells.size());
        auto at = start == 0 ? selector
                : start > 0  ? binary(a, BinaryOp::Sub, selector, integer(start))
                             : binary(a, BinaryOp::Add, selector, integer(-start));
        auto read = elementAt(a, variable(a, table), at);

        /*
         * What the row holds, stated on the read as the narrowest integer type covering it - the
         * one fact the chain carried in its shape and a table read does not. Without it the AST
         * optimizer's range walk (`declaredRange`) knew nothing about `$m[s]`, and every `+` over
         * one grew a `| 0` that the chain's `1 ... 8` had made unnecessary.
         */
        {
            I64 low = 0, high = 0;
            auto integral = true;
            for(Size i = 0; integral && i < cells.size(); i++) {
                I64 value = 0;
                integral = integralNumber(cells[i], value);
                if(!i || value < low) low = value;
                if(!i || value > high) high = value;
            }

            if(integral) {
                U8 bits = 1;
                if(low >= 0) {
                    while(bits < 32 && (high >> bits)) bits++;
                    if(!(high >> bits)) a.base[read]->valueBits = bits;
                } else {
                    while(bits < 32 && (high >= (I64(1) << (bits - 1)) || low < -(I64(1) << (bits - 1)))) bits++;
                    if(high < (I64(1) << (bits - 1)) && low >= -(I64(1) << (bits - 1))) {
                        a.base[read]->valueBits = bits;
                        a.base[read]->valueSigned = true;
                    }
                }
            }
        }

        if(closed) return read;
        if(!nullArm) return binary(a, BinaryOp::Nullish, read, mapLiteral(a, fallback));

        return ternary(a, rangeTest(selector, lowest, highest, false, false), read,
                       mapLiteral(a, fallback));
    }

    // ---------------------------------------------------------------- values

    JsPtr<Expr> valueOf(JsirPtr<JsValue> pointer, LocationId where) {
        if(!pointer) {
            fail("met a null operand"_v, where);
            return nullValue(a);
        }

        auto id = ir[pointer]->id.index;
        if(id < a.values.size() && a.values[id]) return a.values[id];

        fail("met a value with no expression"_v, where);
        return nullValue(a);
    }

    void define(JsInst& inst, JsPtr<Expr> value) {
        if(!inst.result) return;

        auto id = ir[inst.result]->id.index;
        while(a.values.size() <= id) a.values.push(nullptr);

        a.values[id] = value;
    }

    /*
     * A computed value, bound to a `var` and read by name from then on.
     *
     * Every instruction result goes through this rather than being inlined at its use, and the
     * one-use collapse is `opt.cpp`'s - which is where it belongs, because whether an expression may
     * move to its use is a question about the statements in between and this pass is writing them.
     * Doing it here would be the guess §5.4 warns about, made in the one order that cannot see the
     * answer.
     *
     * A literal is the exception and is registered as itself: duplicating one costs no evaluation
     * and a `var` holding one is a name the reader has to follow.
     */
    /*
     * Set while a **constant body** is being spelled - see `runConstant`, where a result is nested
     * into its one reader rather than bound to a `var` there is nowhere to declare.
     */
    bool inlineOnly = false;

    void defineBound(JsInst& inst, JsPtr<Expr> value) {
        if(!inst.result) return;
        if(inlineOnly) return define(inst, value);

        // The name a value already has, where the dispatch lowering is emitting its block a second
        // time - see `Assembly::valueNames`. A repeated `var` is a reassignment in JavaScript, which
        // is exactly what a re-entered block wants.
        auto id = ir[inst.result]->id.index;
        while(a.valueNames.size() <= id) a.valueNames.push(Name {});

        /*
         * A result whose one use is an edge copy into a block argument, computed straight into that
         * argument's variable - J90, and `jsir::JsCoalesced::into` is where the conditions are. The
         * value is registered under the argument's name in `arguments` as well, which is what makes
         * `emitEdge` see `p = p` and write nothing.
         */
        auto into = target.coalesced.intoOf(id);
        if(into != jsir::JsCoalesced::kNoClass && into < a.arguments.size() && a.arguments[into].text) {
            auto name = a.arguments[into];
            while(a.arguments.size() <= id) a.arguments.push(Name {});

            a.arguments[id] = name;
            emitExpr(a, assign(a, variable(a, name), value));
            define(inst, variable(a, name));
            return;
        }

        if(!a.valueNames[id].text) a.valueNames[id] = generatedName(a, "v"_v, id);
        define(inst, declare(a, a.valueNames[id], value));
    }

    // ---------------------------------------------------------------- places

    Name propertyOf(JsShapeId shape, U32 slot) {
        return Name { target.shape(shape).properties[slot].key };
    }

    JsPtr<Expr> placeExpr(JsPlaceId id, LocationId where) {
        auto& place = *body.place(ir, id);

        switch(place.kind) {
            case JsPlaceKind::Var:
                if(place.slot >= a.variables.size()) {
                    fail("met a variable place with no variable"_v, where);
                    return nullValue(a);
                }

                return variable(a, a.variables[place.slot]);

            case JsPlaceKind::Global: {
                auto found = a.globalNames.getValue(U32(place.global.offset));
                if(!found) {
                    fail("met a global with no name"_v, where);
                    return nullValue(a);
                }

                return variable(a, found.unwrap());
            }

            /*
             * `r.$o[r.$k]` - storage named by a descriptor.
             *
             * The one computed member access this tree can build, which is what `ast.h` reserves it
             * for: a reference is an `(object, key)` pair and reading one is an index. A boxed local
             * is the same shape with the key the literal `"$v"`, so one form serves a local, a field
             * and an element alike.
             */
            case JsPlaceKind::Cell: {
                auto reference = valueOf(place.owner, where);
                auto& shape = target.shape(place.shape);
                return elementAt(a, field(a, reference, Name { shape.properties[0].key }),
                                 field(a, reference, Name { shape.properties[1].key }));
            }

            case JsPlaceKind::Property:
                return field(a, valueOf(place.owner, where), propertyOf(place.shape, place.slot));

            case JsPlaceKind::Element:
                return elementAt(a, valueOf(place.owner, where), valueOf(place.index, where));

            case JsPlaceKind::Value:
                return valueOf(place.owner, where);

            // Not an expression of its own: a read decodes and a write read-modify-writes, and
            // both are `readPlace`'s and `writeInto`'s. Reaching here is a reference to one, which
            // the conversion refuses by name.
            case JsPlaceKind::BitRange:
                fail("met a bit range where one host expression was wanted"_v, where);
                return nullValue(a);
        }

        fail("met a place kind it has no arm for"_v, where);
        return nullValue(a);
    }

    /*
     * A read of a place, with the range the place's representation guarantees attached.
     *
     * `noteValueType`'s job from the other side: a property read and an element read have no shape
     * for the peephole to recover a range from, so every coercion around one survived. Here the
     * representation *is* the statement about the contents, so it is carried onto the expression and
     * `foldCoercion` can take the redundant `>>> 0` off a container's count.
     */
    JsPtr<Expr> readPlace(JsPlaceId id, LocationId where) {
        auto& place = *body.place(ir, id);

        // A range of a packed word: the word, decoded - `codegen/js/place.cpp`'s constant-range
        // form, which is the shipping backend's own spelling of the same bits.
        if(place.kind == JsPlaceKind::BitRange && place.bitScale) {
            return noteRange(decodeScaledRange(a, placeExpr(place.word, where),
                                               valueOf(place.bitScale, where), place.bitOffset,
                                               place.bitWidth, place.bitSigned, place.bitWhole),
                             place.content);
        }

        if(place.kind == JsPlaceKind::BitRange) {
            auto word = rep(body.place(ir, place.word)->content).bits;
            return noteRange(decodeBitRange(a, placeExpr(place.word, where), place.bitOffset,
                                            place.bitWidth, word, place.bitSigned),
                             place.content);
        }

        return noteRange(placeExpr(id, where), place.content);
    }

    /*
     * The range a representation guarantees, carried onto the expression - `Expr::valueBits`.
     *
     * Asked of a place's content and of a host operation's result, which are the two positions
     * whose expression has no shape for `noteValueType` in `opt.cpp` to recover a range from. A
     * host `.length` is the reason the second one exists: the *declared* type is a `Size` and what
     * the host promises is an unsigned 32-bit value, and this file used to claim that by matching
     * the member name - which is §4.4's "the name is never the recognizer" broken in the file whose
     * rule is that it switches on nothing but a `JsRep`. `convert.cpp` gives the result the
     * representation instead and this reads it like any other.
     */
    JsPtr<Expr> noteRange(JsPtr<Expr> value, JsRepId repId) {
        if(!value || !repId.valid()) return value;

        auto& content = rep(repId);
        if(content.kind != JsRepKind::Number) return value;
        if(content.domain == NumberDomain::Float32 || content.domain == NumberDomain::Float64) {
            return value;
        }

        a.base[value]->valueBits = content.domain == NumberDomain::Bool01 ? 1 : content.bits;
        a.base[value]->valueSigned = content.domain == NumberDomain::Signed;
        return value;
    }

    /*
     * A whole-value write into storage that *is* an object.
     *
     * The place cannot be rebound - a `&Point` parameter is the object itself, and assigning the
     * parameter would change nothing the caller can see - so the write is property by property, in
     * shape order. That is the same statement the native side makes by copying the bytes, and it is
     * why `JsPlaceKind::Value` is a kind rather than a flag on the store.
     */
    void writeInto(JsPlaceId id, JsPtr<Expr> value, LocationId where) {
        auto& place = *body.place(ir, id);

        /*
         * A range of a packed word is a read-modify-write of the word: `word = encode(word, v)`.
         * The word's place is spelled twice, which is safe because every operand a place names is
         * a bound value by now (`defineBound`) - reading it twice evaluates nothing twice.
         */
        if(place.kind == JsPlaceKind::BitRange && place.bitScale) {
            auto updated = encodeScaledRange(a, placeExpr(place.word, where),
                                             valueOf(place.bitScale, where), place.bitOffset,
                                             place.bitWidth, place.bitSigned, place.bitWhole, value);
            writeInto(place.word, updated, where);
            return;
        }

        if(place.kind == JsPlaceKind::BitRange) {
            auto word = rep(body.place(ir, place.word)->content).bits;
            auto updated = encodeBitRange(a, placeExpr(place.word, where), place.bitOffset,
                                          place.bitWidth, word, place.bitSigned, value);
            writeInto(place.word, updated, where);
            return;
        }

        if(place.kind != JsPlaceKind::Value) {
            emitExpr(a, assign(a, placeExpr(id, where), value));
            return;
        }

        auto& content = rep(place.content);
        auto destination = placeExpr(id, where);

        if(content.kind == JsRepKind::HostArray) {
            // `a.set(b)` for a typed row and a copy loop for a plain one would both be wrong here:
            // what this writes is the *elements*, and the only shape that says so without a count is
            // the host's own splice. Reported rather than guessed.
            fail("met a whole-array write through a borrow"_v, where);
            return;
        }

        if(content.kind != JsRepKind::GcObject) {
            fail("met a whole-value write into storage that is not an object"_v, where);
            return;
        }

        auto& shape = target.shape(content.shape);
        for(U32 i = 0; i < shape.properties.size(); i++) {
            auto key = Name { shape.properties[i].key };
            emitExpr(a, assign(a, field(a, destination, key), field(a, value, key)));
        }
    }

    // ---------------------------------------------------------------- arithmetic

    /*
     * A value brought back into its representation's domain - `codegen/js/type.cpp`'s `coerce`, read
     * off a `JsRep` instead of a `TypePtr`.
     *
     * `Int` is a wrapping 32-bit integer on both targets and JS has no 32-bit integer, so every
     * arithmetic result is reduced. Analysis-JS.md §2.1 recommends exactly this and notes that a
     * range analysis would elide most of them; `opt.cpp`'s `foldCoercion` is what takes back the
     * ones a range already says are no-ops, and §6.7 is the JSIR pass that will take the rest.
     */
    JsPtr<Expr> coerceTo(JsRepId id, JsPtr<Expr> value) {
        auto& to = rep(id);

        if(to.kind == JsRepKind::BigInt) {
            return hostCall(a, "BigInt"_v, to.domain == NumberDomain::Signed ? "asIntN"_v : "asUintN"_v,
                            number(a, to.bits), value);
        }

        if(to.kind != JsRepKind::Number) return value;

        switch(to.domain) {
            case NumberDomain::Bool01:
            case NumberDomain::Float64:
                return value;
            case NumberDomain::Float32:
                return hostCall(a, "Math"_v, "fround"_v, value);
            default:
                break;
        }

        auto isSigned = to.domain == NumberDomain::Signed;

        // The 33-to-53-bit band, where the host has the representation but not the operators.
        if(to.bits > 32) return wideCallAt(a, WideOp::Wrap, to.bits, isSigned, value, nullptr);

        if(to.bits >= 32) {
            return binary(a, isSigned ? BinaryOp::Or : BinaryOp::Shr, value, number(a, 0));
        }

        if(isSigned) {
            // Sign extension is what a narrow load does on native, and shifting up and back is how
            // JS spells it.
            auto shift = number(a, 32 - to.bits);
            return binary(a, BinaryOp::Sar, binary(a, BinaryOp::Shl, value, shift), shift);
        }

        return binary(a, BinaryOp::And, value, number(a, F64((U64(1) << to.bits) - 1)));
    }

    static Maybe<BinaryOp> comparisonOf(mir::BinaryOp op) {
        switch(op) {
            case mir::BinaryOp::Eq: return Just(BinaryOp::Eq);
            case mir::BinaryOp::Ne: return Just(BinaryOp::Ne);
            case mir::BinaryOp::Lt: return Just(BinaryOp::Lt);
            case mir::BinaryOp::Le: return Just(BinaryOp::Le);
            case mir::BinaryOp::Gt: return Just(BinaryOp::Gt);
            case mir::BinaryOp::Ge: return Just(BinaryOp::Ge);
            default: return Nothing();
        }
    }

    JsPtr<Expr> binaryExpr(JsInstBinary& inst) {
        auto where = inst.source;
        auto lhs = valueOf(inst.lhs, where);
        auto rhs = valueOf(inst.rhs, where);

        auto& to = rep(inst.result ? ir[inst.result]->rep : JsRepId());

        /*
         * A comparison answers the host's boolean and a `Bool` here is the number 0 or 1 - see
         * `boolNumber` in type.cpp, where the two forms coming apart is argued.
         *
         * A **mask lane** is the one destination that wants the boolean itself, which is the whole
         * of why `HostBool` is a representation: a comparison per lane under `boolNumber` would be
         * `lanes` ternaries in front of `lanes` selects that immediately test them again.
         */
        if(auto comparison = comparisonOf(inst.op)) {
            auto test = binary(a, comparison.unwrap(), lhs, rhs);
            return to.kind == JsRepKind::HostBool ? test : boolNumber(a, test);
        }

        /*
         * A mask lane's bitwise operators are the logical ones, because its lanes are booleans:
         * `&`, `|` and `^` over two of those would answer numbers, and `a !== b` is what the
         * exclusive or of two booleans is.
         */
        if(to.kind == JsRepKind::HostBool) {
            switch(inst.op) {
                case mir::BinaryOp::And: return binary(a, BinaryOp::LogicalAnd, lhs, rhs);
                case mir::BinaryOp::Or:  return binary(a, BinaryOp::LogicalOr, lhs, rhs);
                case mir::BinaryOp::Xor: return binary(a, BinaryOp::Ne, lhs, rhs);
                default: break;
            }
        }

        auto plain = [&](BinaryOp op) { return binary(a, op, lhs, rhs); };

        if(to.kind == JsRepKind::Number && to.domain == NumberDomain::Bool01) {
            switch(inst.op) {
                case mir::BinaryOp::And: return plain(BinaryOp::And);
                case mir::BinaryOp::Or:  return plain(BinaryOp::Or);
                case mir::BinaryOp::Xor: return plain(BinaryOp::Xor);
                default: break;
            }
        }

        if(to.isFloat()) {
            switch(inst.op) {
                case mir::BinaryOp::Add: return plain(BinaryOp::Add);
                case mir::BinaryOp::Sub: return plain(BinaryOp::Sub);
                case mir::BinaryOp::Mul: return plain(BinaryOp::Mul);
                case mir::BinaryOp::Div: return plain(BinaryOp::Div);
                case mir::BinaryOp::Rem: return plain(BinaryOp::Rem);
                default: break;
            }
        }

        /*
         * The 33-to-53-bit band, where the host has the representation but not the operators.
         *
         * Each goes to a helper rather than to an inline expansion, because the split form of one
         * bitwise operator is eight operations and a program using such a type uses it everywhere.
         * `opt.cpp` fuses chains of them back into one inline unpack-operate-pack, which is where
         * the measured win is. Comparison is absent on purpose: two of these are ordinary `number`s
         * carrying their mathematical values, so `<` already means what it says.
         */
        if(to.isWideNumber()) {
            auto isSigned = to.domain == NumberDomain::Signed;
            auto wide = [&](WideOp op) { return wideCallAt(a, op, to.bits, isSigned, lhs, rhs); };

            switch(inst.op) {
                case mir::BinaryOp::Add: return wide(WideOp::Add);
                case mir::BinaryOp::Sub: return wide(WideOp::Sub);
                case mir::BinaryOp::Mul: return wide(WideOp::Mul);
                case mir::BinaryOp::And: return wide(WideOp::And);
                case mir::BinaryOp::Or:  return wide(WideOp::Or);
                case mir::BinaryOp::Xor: return wide(WideOp::Xor);
                case mir::BinaryOp::Shl: return wide(WideOp::Shl);
                case mir::BinaryOp::Shr: return wide(WideOp::Shr);
                case mir::BinaryOp::Sar: return wide(WideOp::Sar);

                // Both operands are already in range and `Math.trunc` is the truncation toward zero
                // the language asks for, so the quotient needs no helper of its own - except for the
                // one signed pair whose quotient is the missing positive, which `Wrap` catches.
                case mir::BinaryOp::Div: {
                    auto quotient = hostCall(a, "Math"_v, "trunc"_v, divideCall(a, lhs, rhs));
                    return isSigned ? wideCallAt(a, WideOp::Wrap, to.bits, true, quotient, nullptr)
                                    : quotient;
                }
                case mir::BinaryOp::Rem:
                    return remainderCall(a, lhs, rhs);
                default: break;
            }
        }

        if(to.kind == JsRepKind::BigInt) {
            switch(inst.op) {
                case mir::BinaryOp::Add: return plain(BinaryOp::Add);
                case mir::BinaryOp::Sub: return plain(BinaryOp::Sub);
                case mir::BinaryOp::Mul: return plain(BinaryOp::Mul);

                // `0n` is the one divisor BigInt division *throws* on rather than answering, so this
                // width goes through `$div` - see `JsShared::divideByZeroHelper`.
                case mir::BinaryOp::Div: return divideCall(a, lhs, rhs);
                case mir::BinaryOp::Rem: return remainderCall(a, lhs, rhs);
                case mir::BinaryOp::And: return plain(BinaryOp::And);
                case mir::BinaryOp::Or:  return plain(BinaryOp::Or);
                case mir::BinaryOp::Xor: return plain(BinaryOp::Xor);
                case mir::BinaryOp::Shl: return plain(BinaryOp::Shl);
                case mir::BinaryOp::Sar: return plain(BinaryOp::Sar);

                // A logical right shift has to go through the unsigned reading first: `>>>` is not
                // defined on BigInt at all.
                case mir::BinaryOp::Shr:
                    return binary(a, BinaryOp::Sar,
                                  hostCall(a, "BigInt"_v, "asUintN"_v, number(a, 64), lhs), rhs);
                default: break;
            }
        }

        switch(inst.op) {
            case mir::BinaryOp::Add: return plain(BinaryOp::Add);
            case mir::BinaryOp::Sub: return plain(BinaryOp::Sub);

            /*
             * `Math.imul` is the only correct 32-bit multiply JS has: `a * b` is a double multiply,
             * and the low 32 bits of the true product are not the low 32 bits of the rounded one
             * once the product passes 2^53. §2.1 calls this out as the one unconditional coercion.
             */
            case mir::BinaryOp::Mul: return hostCall(a, "Math"_v, "imul"_v, lhs, rhs);

            // `a / 0` is an infinity and the `|0` the coercion emits anyway turns any infinity into
            // the 0 the language defines, so a narrow quotient needs no helper. The remainder gets
            // no such reprieve at any width: `a % 0` is NaN and the answer is `a`.
            case mir::BinaryOp::Div: return plain(BinaryOp::Div);
            case mir::BinaryOp::Rem: return remainderCall(a, lhs, rhs);

            case mir::BinaryOp::And: return plain(BinaryOp::And);
            case mir::BinaryOp::Or:  return plain(BinaryOp::Or);
            case mir::BinaryOp::Xor: return plain(BinaryOp::Xor);
            case mir::BinaryOp::Shl: return plain(BinaryOp::Shl);
            case mir::BinaryOp::Shr: return plain(BinaryOp::Shr);
            case mir::BinaryOp::Sar: return plain(BinaryOp::Sar);
            default: break;
        }

        fail("met a binary operation it has no spelling for"_v, where);
        return nullValue(a);
    }

    JsPtr<Expr> unaryExpr(JsInstUnary& inst) {
        auto where = inst.source;
        auto value = valueOf(inst.value, where);
        auto& to = rep(inst.result ? ir[inst.result]->rep : JsRepId());

        switch(inst.op) {
            case mir::UnaryOp::Neg:
                return unary(a, UnaryOp::Neg, value);

            case mir::UnaryOp::Not:
                // A mask lane is a host boolean and `not` is the only unary one takes, so its
                // complement is `!` - see `JsRepKind::HostBool`.
                if(to.kind == JsRepKind::HostBool) return unary(a, UnaryOp::Not, value);

                // A `Bool` is 0 or 1, so its complement is the flip of one bit rather than `~`,
                // which would answer -1 and -2.
                if(to.kind == JsRepKind::Number && to.domain == NumberDomain::Bool01) {
                    return binary(a, BinaryOp::Xor, value, number(a, 1));
                }

                /*
                 * The 33-to-53-bit band has no complement operator: `~` is a 32-bit operation, so
                 * `~34359738375` is `~7` and answers -8 rather than the value's own complement. The
                 * helper is the same one every other operation in the band goes to.
                 */
                if(to.isWideNumber()) {
                    return wideCallAt(a, WideOp::Not, to.bits, to.domain == NumberDomain::Signed,
                                      value, nullptr);
                }

                return unary(a, UnaryOp::BitNot, value);

            /*
             * `Math.abs` is the magnitude at every representation here except one: it coerces its
             * argument to a number and *throws* on a `bigint`. The conditional is the magnitude
             * there, and it needs no zero case - a `bigint` has one zero and no sign on it.
             */
            case mir::UnaryOp::Abs:
                if(to.kind == JsRepKind::BigInt) {
                    return ternary(a, binary(a, BinaryOp::Lt, value, bigInt(a, 0, to.isSigned())),
                                   unary(a, UnaryOp::Neg, value), value);
                }

                return hostCall(a, "Math"_v, "abs"_v, value);
            case mir::UnaryOp::Sqrt:  return hostCall(a, "Math"_v, "sqrt"_v, value);
            case mir::UnaryOp::Trunc: return hostCall(a, "Math"_v, "trunc"_v, value);
            case mir::UnaryOp::Floor: return hostCall(a, "Math"_v, "floor"_v, value);
            case mir::UnaryOp::Ceil:  return hostCall(a, "Math"_v, "ceil"_v, value);

            case mir::UnaryOp::Round: {
                // `Math.round` ties toward positive infinity and this language ties away from zero,
                // so the two disagree at every negative half - see `JsShared::roundAwayHelper`.
                if(!a.roundAwayHelper.text) a.roundAwayHelper = uniqueName(a, "$round"_v, false);
                return asPureCall(a, call(a, variable(a, a.roundAwayHelper), value));
            }

            default:
                fail("met a unary operation it has no spelling for"_v, where);
                return nullValue(a);
        }
    }

    /*
     * One representation crossing to another - the cast tower, read off two `JsRep`s.
     *
     * `codegen/js/inst.cpp`'s `genCast` step for step, with the type questions answered from the
     * representation instead. Four host domains meet here - the 0/1 boolean, the `number` below 33
     * bits, the `number` from 33 to 53 and the `bigint` above - and each crossing is a measurement
     * rather than a preference: `BigInt(NaN)` throws, `2^63 - 1` is not a double, and a widening
     * between two integers this target holds identically has to emit *nothing* or every subscript
     * with an ordinary index pays a coercion.
     */
    static SaturationRange saturationOf(const JsRep& rep) {
        auto bits = rep.bits >= 64 ? 64u : U32(rep.bits);

        if(rep.domain != NumberDomain::Signed) {
            auto high = bits >= 64 ? 18446744073709551616.0 : F64((U64(1) << bits) - 1);
            return SaturationRange { 0.0, high, bits <= 53 };
        }

        auto magnitude = F64(U64(1) << (bits - 1));
        return SaturationRange { -magnitude, bits >= 64 ? magnitude : magnitude - 1.0, bits <= 54 };
    }

    JsPtr<Expr> saturatingToNumber(const JsRep& to, JsPtr<Expr> value) {
        auto truncated = hostCall(a, "Math"_v, "trunc"_v, value);
        auto range = saturationOf(to);
        if(!range.highIsExact) return truncated;

        return hostCall(a, "Math"_v, "min"_v,
                        hostCall(a, "Math"_v, "max"_v, truncated, number(a, range.low, false)),
                        number(a, range.high, false));
    }

    JsPtr<Expr> convertExpr(JsInstConvert& inst) {
        auto where = inst.source;
        auto value = valueOf(inst.value, where);

        auto fromId = ir[inst.value]->rep;
        auto toId = ir[inst.result]->rep;
        auto& from = rep(fromId);
        auto& to = rep(toId);

        if(fromId == toId) return value;

        /*
         * A **host boolean** crossing to or from a number, which is the one pair `genCast`'s tower
         * never had: a mask lane read as the number it stands for - which is what a mask's `count`
         * reduction sums - and a number read as a lane, which nothing produces today but which the
         * tower answers rather than leaving to a later arm to get wrong.
         */
        if(from.kind == JsRepKind::HostBool) {
            if(to.kind == JsRepKind::BigInt) {
                return ternary(a, value, bigInt(a, 1, to.isSigned()), bigInt(a, 0, to.isSigned()));
            }

            return coerceTo(toId, boolNumber(a, value));
        }

        if(to.kind == JsRepKind::HostBool) {
            auto zero = from.kind == JsRepKind::BigInt ? bigInt(a, 0, from.isSigned()) : number(a, 0);
            return binary(a, BinaryOp::Ne, value, zero);
        }

        auto fromBool = from.kind == JsRepKind::Number && from.domain == NumberDomain::Bool01;
        auto toBool = to.kind == JsRepKind::Number && to.domain == NumberDomain::Bool01;
        auto fromLong = from.kind == JsRepKind::BigInt;
        auto toLong = to.kind == JsRepKind::BigInt;

        /*
         * Two integers this target holds identically, which is a conversion that converts nothing.
         *
         * `Int` to `Size` is the case that made this worth writing: a `Size` here is a signed 32-bit
         * index, so emitting the widening as `i | 0` put a coercion in front of every subscript.
         * Natively the same conversion is a real sign extension, which is why this is the backend's
         * answer rather than a fold one tier up - the two targets disagree about whether the
         * instruction does anything, and only one of them is entitled to remove it.
         */
        if(from.kind == JsRepKind::Number && to.kind == JsRepKind::Number && !from.isFloat() &&
           !to.isFloat() && !fromBool && !toBool && from.domain == to.domain &&
           from.bits == to.bits) {
            return value;
        }

        // Two host references are one value under two names - a `%T`, a `[T *n]`, an erased handle.
        if(!from.isPrimitive() && !to.isPrimitive()) return value;
        if(to.kind == JsRepKind::HostArray || to.kind == JsRepKind::Erased) return value;

        // A `Bool` is already the number 0 or 1, so widening one costs nothing outside the `bigint`
        // domain - which is the one place the two representations genuinely differ.
        if(fromBool && !toBool) {
            if(toLong) return ternary(a, value, bigInt(a, 1, to.isSigned()), bigInt(a, 0, to.isSigned()));
            return coerceTo(toId, value);
        }

        // Through `boolNumber`, because a comparison is a host boolean and a `Bool` is the number.
        if(toBool) {
            auto zero = fromLong ? bigInt(a, 0, from.isSigned()) : number(a, 0);
            return boolNumber(a, binary(a, BinaryOp::Ne, value, zero));
        }

        /*
         * `bigint` and `number` are two domains, so crossing between them is a real conversion
         * rather than a widening (§2.1). A float source saturates on the way in, and that is not
         * optional in the way it is elsewhere: `BigInt(NaN)` and `BigInt(Infinity)` *throw*.
         */
        if(toLong && !fromLong) {
            if(from.isFloat()) {
                auto helper = saturatingLongHelperAt(a, to.bits, to.domain == NumberDomain::Signed);
                auto node = make<CallExpr>(a, variable(a, helper));
                node->args.push(a.file.arena, value);
                node->pure = true;
                return asExpr(a, node);
            }

            // A literal crosses at compile time: `BigInt(0)` is `0n`, and writing the call out left
            // a constant the folds could not see through.
            F64 literal;
            if(constantNumber(a, value, literal) && isExactInteger(literal)) {
                return coerceTo(toId, bigInt(a, U64(I64(literal)), to.domain == NumberDomain::Signed));
            }

            return coerceTo(toId, globalCall(a, "BigInt"_v, value));
        }

        if(fromLong && !toLong) {
            if(to.isFloat()) return globalCall(a, "Number"_v, value);

            // Reduced in the `bigint` domain first, because `Number()` of a value outside the safe
            // range loses low bits and the coercion after it would then be reducing the wrong number.
            auto reduced = hostCall(a, "BigInt"_v,
                                    to.domain == NumberDomain::Signed ? "asIntN"_v : "asUintN"_v,
                                    number(a, to.bits), value);

            return coerceTo(toId, globalCall(a, "Number"_v, reduced));
        }

        if(!to.isFloat() && from.isFloat()) return coerceTo(toId, saturatingToNumber(to, value));

        /*
         * Widening into the 33-to-53-bit band, which is mostly free: the two representations are the
         * same host type, so only a negative value entering an unsigned type is a real conversion -
         * and the general coercion for that band is a remainder this would otherwise emit on every
         * widening. `wideFromNarrow`'s rule, read off the two reps.
         */
        if(to.isWideNumber() && from.kind == JsRepKind::Number && !from.isFloat()) {
            auto toSigned = to.domain == NumberDomain::Signed;
            auto fromSigned = from.domain == NumberDomain::Signed;

            if(from.bits < to.bits && (toSigned || !fromSigned)) return value;
            return wideCallAt(a, WideOp::Wrap, to.bits, toSigned, value, nullptr);
        }

        return coerceTo(toId, value);
    }

    // ---------------------------------------------------------------- host

    JsPtr<Expr> hostExpr(JsInstHost& inst) {
        auto where = inst.source;
        auto count = inst.arguments.size();

        auto argument = [&](U32 index) {
            return valueOf(inst.arguments.get(ir, index), where);
        };

        auto member = [&]() { return propertyName(a, stringView(a.context.findName(inst.member))); };

        switch(inst.op) {
            case mir::HostOp::Field: {
                if(count < 1) break;
                return field(a, argument(0), member());
            }

            case mir::HostOp::Call: {
                if(count < 1) break;

                auto node = make<CallExpr>(a, field(a, argument(0), member()));
                for(U32 i = 1; i < count; i++) node->args.push(a.file.arena, argument(i));
                return asExpr(a, node);
            }

            case mir::HostOp::Global: {
                /*
                 * A call on the host's global scope, whose whole dotted path is the member name.
                 *
                 * Split here rather than in the library, because what the tree can express is a
                 * field chain: `Date.now` is `field(variable("Date"), "now")` and a path this tree
                 * cannot build is one the emitted source could not have either.
                 */
                auto text = stringView(a.context.findName(inst.member));
                JsPtr<Expr> callee = nullptr;
                Size start = 0;

                for(Size i = 0; i <= text.length; i++) {
                    if(i != text.length && text.ptr[i] != '.') continue;

                    auto part = StringView { text.ptr + start, U32(i - start) };
                    callee = callee ? field(a, callee, literalName(a, part))
                                    : variable(a, literalName(a, part));
                    start = i + 1;
                }

                if(!callee) break;

                auto node = make<CallExpr>(a, callee);
                for(U32 i = 0; i < count; i++) node->args.push(a.file.arena, argument(i));
                return asExpr(a, node);
            }

            case mir::HostOp::Binary: {
                if(count < 2) break;

                auto text = stringView(a.context.findName(inst.member));
                auto op = BinaryOp::Add;

                if(text == "+"_v) op = BinaryOp::Add;
                else if(text == "-"_v) op = BinaryOp::Sub;
                else if(text == "*"_v) op = BinaryOp::Mul;
                else if(text == "/"_v) op = BinaryOp::Div;
                else if(text == "%"_v) op = BinaryOp::Rem;
                else if(text == "<"_v) op = BinaryOp::Lt;
                else if(text == "<="_v) op = BinaryOp::Le;
                else if(text == ">"_v) op = BinaryOp::Gt;
                else if(text == ">="_v) op = BinaryOp::Ge;
                else if(text == "==="_v || text == "=="_v) op = BinaryOp::Eq;
                else if(text == "!=="_v || text == "!="_v) op = BinaryOp::Ne;
                else {
                    fail("met a host binary operator it has no spelling for"_v, where);
                    return nullValue(a);
                }

                auto result = binary(a, op, argument(0), argument(1));

                // A host comparison answers the host's boolean, and a `Bool` here is the number.
                auto& to = rep(inst.result ? ir[inst.result]->rep : JsRepId());
                if(to.kind == JsRepKind::Number && to.domain == NumberDomain::Bool01) {
                    return boolNumber(a, result);
                }

                return result;
            }

            /*
             * `[]`, or the elements a literal was built with.
             *
             * `hostArray()` is declared with no arguments - an empty host array is what `emptyArray`
             * is - and `resolveArrayLiteral` builds the *same row* with the elements as operands,
             * because a literal is one node rather than a call plus n pushes. So the arity here is
             * the literal's and the typed row the element chooses is the result's own
             * representation.
             */
            /*
             * Whether this element's array has a capacity to manage - a compile-time constant with
             * an argument, and the argument is what carries the question.
             *
             * An intrinsic is handed its result type rather than its type arguments, so the element
             * is read off the array reference it is asked about - which here is the operand's own
             * representation. `typedArrayFor` is the rule, and the two rows of
             * Implementation-Containers.md §14 are what it decides between.
             */
            case mir::HostOp::FixedCapacity: {
                if(count < 1) break;

                return number(a, rep(ir[inst.arguments.get(ir, 0)]->rep).hasFixedCapacity() ? 1 : 0);
            }

            // `$grow(a, n)` - a typed row is fixed-length, so growing one is a fresh row of the same
            // constructor with the old contents copied in. See `emitGrowHelper`.
            case mir::HostOp::Grow: {
                if(count < 2) break;

                if(!a.growHelper.text) a.growHelper = uniqueName(a, "$grow"_v, false);

                auto node = make<CallExpr>(a, variable(a, a.growHelper));
                node->args.push(a.file.arena, argument(0));
                node->args.push(a.file.arena, argument(1));
                return asExpr(a, node);
            }

            /*
             * `$readU32(a, i, le)` - a machine word out of a byte row through the row's own
             * `DataView`, and `$writeU32(a, i, v, le)` the other way.
             *
             * The shipping backend's helpers, reached through `JsShared`'s own slots rather than
             * restated here: what the helper *is* - one cached view per array, and why a fresh
             * `DataView` per call is fifteen times worse than the shift chain it replaces - is
             * `emitWordHelpers`' measurement, and two backends emitting two spellings of one
             * `getBigUint64` is how they come to disagree about a binary format. The width comes off
             * the accessor's name, which is what `member` carries, exactly as it does there.
             */
            case mir::HostOp::ViewRead:
            case mir::HostOp::ViewWrite: {
                auto read = inst.op == mir::HostOp::ViewRead;
                if(count < (read ? 3u : 4u)) break;

                auto accessor = stringView(a.context.findName(inst.member));
                auto width = accessor == "getUint16"_v || accessor == "setUint16"_v ? 0
                           : accessor == "getUint32"_v || accessor == "setUint32"_v ? 1 : 2;
                auto& slot = read ? a.readWordHelper[width] : a.writeWordHelper[width];

                if(!slot.text) {
                    static const StringView names[2][3] = {
                        { "$writeU16"_v, "$writeU32"_v, "$writeU64"_v },
                        { "$readU16"_v, "$readU32"_v, "$readU64"_v },
                    };

                    slot = uniqueName(a, names[read][width], false);
                }

                auto node = make<CallExpr>(a, variable(a, slot));
                for(U32 i = 0; i < count; i++) node->args.push(a.file.arena, argument(i));
                return asExpr(a, node);
            }

            case mir::HostOp::Array: {
                auto elements = make<ArrayExpr>(a);
                for(U32 i = 0; i < count; i++) elements->values.push(a.file.arena, argument(i));

                auto& to = rep(inst.result ? ir[inst.result]->rep : JsRepId());
                if(to.kind != JsRepKind::HostArray || to.typedArray == TypedArray::Plain) {
                    return asExpr(a, elements);
                }

                auto node = make<CallExpr>(a,
                    variable(a, literalName(a, typedArrayName(to.typedArray))));
                node->args.push(a.file.arena, asExpr(a, elements));
                node->construct = true;
                return asExpr(a, node);
            }

            default:
                break;
        }

        target.notYet(mir::hostOpName(inst.op), where);
        failed = true;
        return nullValue(a);
    }

    // ---------------------------------------------------------------- objects

    /*
     * An object literal with **every** property of the shape present, in the shape's order.
     *
     * §2.3 makes construction order this target's field offsets, so a literal that skips a property
     * or writes two in the other order is a second hidden class for one record - which is a cost
     * rather than an error and therefore invisible. Two sites build one: an `Object` instruction and
     * a `Materialize` into a `GcObject` or a `Cell`, which is what makes a flat reference come out
     * as the same descriptor `borrow.describe` builds.
     */
    template<class Values>
    JsPtr<Expr> shapedObject(JsShapeId shapeId, Values& values, LocationId where) {
        auto& shape = target.shape(shapeId);
        auto object = make<ObjectExpr>(a);

        for(U32 i = 0; i < shape.properties.size(); i++) {
            object->properties.push(a.file.arena, Property {
                Name { shape.properties[i].key },
                valueOf(values.get(ir, i), where)
            });
        }

        return asExpr(a, object);
    }

    JsPtr<Expr> objectExpr(JsInstObject& inst) {
        return shapedObject(inst.shape, inst.values, inst.source);
    }

    /*
     * A host row out of its elements - the literal, or the typed constructor the element asks for.
     *
     * Shared by the two rows this tier builds: an ordinary `array` instruction and a vector's
     * materialization, which are the same statement made about a value that has a length and a value
     * that has lanes. §14's typed row is where the element has one - a `[U8 *64]` is a `Uint8Array`,
     * which is what lets a word transfer reach its buffer and is what the row was for.
     */
    JsPtr<Expr> rowOf(const JsRep& to, JsirList<JsirPtr<JsValue>, false>& values, LocationId where) {
        auto elements = make<ArrayExpr>(a);
        for(U32 i = 0; i < values.size(); i++) {
            elements->values.push(a.file.arena, valueOf(values.get(ir, i), where));
        }

        if(to.kind != JsRepKind::HostArray || to.typedArray == TypedArray::Plain) {
            return asExpr(a, elements);
        }

        auto node = make<CallExpr>(a, variable(a, literalName(a, typedArrayName(to.typedArray))));
        node->args.push(a.file.arena, asExpr(a, elements));
        node->construct = true;
        return asExpr(a, node);
    }

    JsPtr<Expr> arrayExpr(JsInstArray& inst) {
        return rowOf(rep(ir[inst.result]->rep), inst.values, inst.source);
    }

    /*
     * A structural duplicate - the one ownership operation that costs anything here (§2.5).
     *
     * Property by property rather than a call into a runtime cloner: the shape is known and an
     * object literal listing every property in construction order is both the fastest form and the
     * one that keeps the type's hidden class.
     */
    JsPtr<Expr> cloneExpr(JsInstClone& inst) {
        auto where = inst.source;
        auto source = valueOf(inst.from, where);
        auto& from = rep(ir[inst.from]->rep);

        if(from.kind == JsRepKind::HostArray) {
            return asPureCall(a, call(a, field(a, source, literalName(a, "slice"_v))));
        }

        if(from.kind != JsRepKind::GcObject && from.kind != JsRepKind::Nullable) return source;

        return cloneOf(ir[inst.from]->rep, source, 0);
    }

    /*
     * The duplicate itself, property by property and **recursively**.
     *
     * Natively a trivial copy is a block copy of the bytes, so every nested aggregate is duplicated
     * with it; here a property holding an object would be shared, and the copy would alias
     * everything below its first level. `Array(Int)` is where that shows - `{items, length}` whose
     * `items` is the row - and the one-level form made a copy of a container share its elements.
     *
     * A sum copies every property of the flattened union rather than the live arm's, for
     * `zeroOf`'s reason: which constructor a value holds is a run-time fact and the shape is not, so
     * copying all of them keeps this one object literal instead of a switch over the tag.
     */
    JsPtr<Expr> cloneOf(JsRepId id, JsPtr<Expr> source, U32 depth) {
        auto& from = rep(id);
        if(depth > 16) return source;

        /*
         * A **niche-folded** value is not an object and may still hold one - `Maybe(Person)` is a
         * `Person` or a `null` - so it is asked before the shortcut below rather than falling into
         * it. Duplicating one is duplicating the payload where there is one, and the test for that
         * is the same comparison the tag decode is.
         *
         * The source is written twice, which is what the shipping backend's `cloneValue` does at the
         * same shape and on the same terms: every JSIR value is bound to a `var` here and `opt.cpp`
         * moves an expression to its use only where it reaches *every* occurrence of the name.
         */
        if(from.kind == JsRepKind::Nullable) {
            if(!rep(from.content).isObjectLike()) return source;

            return ternary(a, binary(a, BinaryOp::Eq, source, nullValue(a)), nullValue(a),
                           cloneOf(from.content, source, depth + 1));
        }

        /*
         * A row of values is `slice()`; a row of *objects* is not.
         *
         * `slice()` is the shallow copy every engine has and is the whole answer for the numeric
         * rows this type is nearly always over. Where an element is an object the copy has to reach
         * it - a shared element is a copy that aliases, which is what `ErasedCopy` measures - and the
         * count is part of the type, so the elements are written out and cloned one by one. A row
         * whose length this target does not know and whose elements are objects has no copy this
         * tree can spell at all, since there is no lambda here to map with.
         */
        if(from.kind == JsRepKind::HostArray) {
            auto& element = rep(from.content);
            if(element.kind != JsRepKind::GcObject) {
                return asPureCall(a, call(a, field(a, source, literalName(a, "slice"_v))));
            }

            if(!from.length) {
                fail("met a copy of a host row of objects whose length it does not know"_v,
                     kNullLocation);
                return source;
            }

            auto elements = make<ArrayExpr>(a);
            for(U32 i = 0; i < from.length; i++) {
                elements->values.push(a.file.arena,
                                      cloneOf(from.content, index(a, source, i), depth + 1));
            }

            return asExpr(a, elements);
        }

        if(from.kind != JsRepKind::GcObject) return source;

        auto& shape = target.shape(from.shape);
        auto object = make<ObjectExpr>(a);

        for(auto& property: shape.properties) {
            auto key = Name { property.key };
            object->properties.push(a.file.arena, Property {
                key, cloneOf(property.rep, field(a, source, key), depth + 1)
            });
        }

        return asExpr(a, object);
    }

    // ---------------------------------------------------------------- calls

    JsPtr<Expr> calleeName(DeclPtr<mir::Function> callee, LocationId where) {
        auto found = a.functionNames.getValue(U32(callee.offset));
        if(!found) {
            fail("met a call to a function with no name"_v, where);
            return nullValue(a);
        }

        return variable(a, found.unwrap());
    }

    JsPtr<Expr> callExpr(JsInstCall& inst) {
        auto where = inst.source;
        auto node = make<CallExpr>(a, calleeName(inst.callee, where));

        // The generic environment first, where the callee is a body compiled once - the erased
        // ABI's one position, and `JsAbi::takesGenEnv` is what put it on the node.
        if(inst.genEnv) node->args.push(a.file.arena, valueOf(inst.genEnv, where));

        for(U32 i = 0; i < inst.arguments.size(); i++) {
            node->args.push(a.file.arena, valueOf(inst.arguments.get(ir, i), where));
        }

        return asExpr(a, node);
    }

    JsPtr<Expr> callValueExpr(JsInstCallValue& inst) {
        auto where = inst.source;
        auto node = make<CallExpr>(a, valueOf(inst.code, where));

        // The environment first where the value called is a pair - see `JsInstCallValue`, where the
        // one convention is argued, and `funCodeExpr`, which is what makes a plain function fit it.
        // A witness cell is not a pair and leads with nothing.
        if(inst.environment) node->args.push(a.file.arena, valueOf(inst.environment, where));

        for(U32 i = 0; i < inst.arguments.size(); i++) {
            node->args.push(a.file.arena, valueOf(inst.arguments.get(ir, i), where));
        }

        return asExpr(a, node);
    }

    JsPtr<Expr> funCodeExpr(JsInstFunCode& inst) {
        auto abi = jsAbiOf(target, inst.callee, inst.source);

        /*
         * A body compiled once taken as a *value* - which needs a thunk that supplies the table out
         * of its own captures, since a pair carries one word and that word is the closure's.
         *
         * Reported rather than shimmed: the shim below would drop the leading argument, and what
         * the callee would then read as its table is its first declared parameter.
         */
        if(target.abi(abi).takesGenEnv) {
            target.notYet("a function value of a generic body compiled once"_v, inst.source);
            failed = true;
            return nullValue(a);
        }

        // A callee that already begins with an environment *is* the code word; one that does not
        // gets the shim `assemble.cpp` emits, so that every call through a pair is one shape.
        if(target.abi(abi).takesEnv) return calleeName(inst.callee, inst.source);

        auto key = U32(inst.callee.offset);
        if(auto found = a.thunkNames.getValue(key)) return variable(a, found.unwrap());

        char buffer[256];
        auto text = stringView(a.context.findName(target.nameOf(inst.callee)));
        auto length = text.length < sizeof(buffer) - 8 ? text.length : sizeof(buffer) - 8;
        copy(text.ptr, buffer, length);
        copy("$fn", buffer + length, 3);

        auto name = uniqueName(a, StringView { buffer, U32(length + 3) }, false);
        a.thunkNames.add(key, name);
        a.thunkOrder.push(inst.callee);
        return variable(a, name);
    }

    /*
     * The name of one generic environment, reserved on first ask - `Assembly::genTableNames`.
     *
     * Keyed on the `mir::GenEnvironment` handle, which is already interned on (generic, argument
     * list): ten calls at one argument list are one table in the file, which is the whole reason
     * this is a name rather than a row built at the site.
     */
    Name genTableName(JsInstGenTable& inst) {
        auto key = U32(inst.environment.offset);
        if(auto found = a.genTableNames.getValue(key)) return found.unwrap();

        auto name = mintPrefixed(a, "$ge$"_v,
                                 stringView(a.context.findName(target.genericName(inst.environment))));
        a.genTableNames.add(key, name);
        a.genTableOrder.push(inst.environment);
        return name;
    }

    // ---------------------------------------------------------------- helpers

    /*
     * One of the runtime families, by tag - `JsHelperOp`, and the newer half of §2.3's translation.
     *
     * The name comes out of `JsShared`'s slot table and nothing here spells one, which is §4.4's
     * rule: a helper is recognized by the tag on the node and never by what it is called. What this
     * does decide is the *width*, which is the value's representation - the operand's for a count or
     * a swap, and the float side's for a reinterpretation, the two sides of a bitcast being equal by
     * construction.
     */
    JsPtr<Expr> helperExpr(JsInstHelper& inst) {
        auto where = inst.source;

        auto operandRep = inst.arguments.size()
            ? ir[inst.arguments.get(ir, 0)]->rep : JsRepId();
        auto& from = rep(operandRep);

        auto resultRep = inst.result ? ir[inst.result]->rep : JsRepId();

        Name name;
        switch(inst.op) {
            case JsHelperOp::FloatToBits:
                name = floatBitsHelperAt(a, true, from.bits == 64);
                break;

            // The width is the *float* side's, which here is the result: the operand is the integer
            // the bits are arriving from, and a `Bitcast` relates two types of one size.
            case JsHelperOp::BitsToFloat:
                name = floatBitsHelperAt(a, false, rep(resultRep).bits == 64);
                break;

            case JsHelperOp::ByteSwap:
                name = byteSwapHelperAt(a, from.bits);
                break;

            case JsHelperOp::CountBits:
            case JsHelperOp::LeadingZeros:
            case JsHelperOp::TrailingZeros: {
                auto family = inst.op == JsHelperOp::CountBits    ? BitFamily::CountBits
                            : inst.op == JsHelperOp::LeadingZeros ? BitFamily::LeadingZeros
                                                                  : BitFamily::TrailingZeros;

                name = bitCountHelperAt(a, family, from.bits == 64);

                // The one the host answers itself - `Math.clz32`, whose argument goes through
                // ToUint32, so a negative `I32` counts the bits the machine counts.
                if(!name.text) {
                    return hostCall(a, "Math"_v, "clz32"_v, valueOf(inst.arguments.get(ir, 0), where));
                }

                break;
            }

            case JsHelperOp::Rol:
            case JsHelperOp::Ror: {
                name = rotateHelperAt(a, inst.op == JsHelperOp::Rol ? BitFamily::Rol : BitFamily::Ror,
                                      from.bits);
                if(!name.text) {
                    fail("met a rotation at a width with no helper"_v, where);
                    return nullValue(a);
                }

                break;
            }

            case JsHelperOp::BitsUpTo:
            case JsHelperOp::GatherBits:
            case JsHelperOp::ScatterBits: {
                auto family = inst.op == JsHelperOp::BitsUpTo   ? BitFamily::BitsUpTo
                            : inst.op == JsHelperOp::GatherBits ? BitFamily::GatherBits
                                                                : BitFamily::ScatterBits;

                name = bitOpHelperAt(a, family, from.bits == 64);
                break;
            }

            case JsHelperOp::FunDrop:
                name = funDropHelperAt(a);
                break;
        }

        auto node = make<CallExpr>(a, variable(a, name));
        for(U32 i = 0; i < inst.arguments.size(); i++) {
            node->args.push(a.file.arena, valueOf(inst.arguments.get(ir, i), where));
        }

        // The purity is the *tag's* - `effectsOf`, where the float pair's shared buffer is why two
        // of those may not be reordered against each other.
        node->pure = isPure(inst);
        return asExpr(a, node);
    }

    // ---------------------------------------------------------------- instructions

    void emitInst(JsInst& inst) {
        auto where = inst.source;

        switch(inst.kind) {
            case JsInst::Const: {
                auto& written = (JsInstConst&)inst;
                auto& to = rep(ir[inst.result]->rep);

                switch(written.constant.kind) {
                    case JsConstant::Number:
                        define(inst, number(a, written.constant.number, written.constant.integral));
                        return;
                    case JsConstant::BigInt:
                        define(inst, bigInt(a, written.constant.bits, written.constant.isSigned));
                        return;
                    case JsConstant::Text:
                        define(inst, asExpr(a, make<StringExpr>(a, written.constant.text)));
                        return;
                    case JsConstant::Null:
                        define(inst, nullValue(a));
                        return;
                    case JsConstant::Undefined:
                        define(inst, asExpr(a, make<UndefinedExpr>(a)));
                        return;
                    case JsConstant::Bool:
                        define(inst, boolean(a, written.constant.number != 0));
                        return;
                }

                (void)to;
                return;
            }

            case JsInst::FunCode:
                define(inst, funCodeExpr((JsInstFunCode&)inst));
                return;

            case JsInst::GenTable:
                define(inst, variable(a, genTableName((JsInstGenTable&)inst)));
                return;

            case JsInst::Alias:
                define(inst, valueOf(((JsInstAlias&)inst).from, where));
                return;

            case JsInst::Object:
                defineBound(inst, objectExpr((JsInstObject&)inst));
                return;

            case JsInst::Array:
                defineBound(inst, arrayExpr((JsInstArray&)inst));
                return;

            case JsInst::Clone:
                defineBound(inst, cloneExpr((JsInstClone&)inst));
                return;

            case JsInst::Pack: {
                auto& written = (JsInstPack&)inst;
                auto& row = target.packings[written.packing];

                SmallArray<JsPtr<Expr>, 8> values;
                SmallArray<U32, 8> offsets, widths;
                for(U32 i = 0; i < row.fields.size(); i++) {
                    values.push(valueOf(written.values.get(ir, i), where));
                    offsets.push(row.fields[i].offset);
                    widths.push(row.fields[i].width);
                }

                defineBound(inst, noteRange(packBitRanges(a, held(values), held(offsets), held(widths), row.span),
                                            row.word));
                return;
            }

            case JsInst::Load:
                defineBound(inst, readPlace(((JsInstLoad&)inst).place, where));
                return;

            case JsInst::Store: {
                auto& written = (JsInstStore&)inst;
                writeInto(written.place, valueOf(written.value, where), where);
                return;
            }

            case JsInst::Binary:
                defineBound(inst, binaryExpr((JsInstBinary&)inst));
                return;

            case JsInst::Unary:
                defineBound(inst, unaryExpr((JsInstUnary&)inst));
                return;

            case JsInst::Coerce: {
                auto& written = (JsInstCoerce&)inst;
                defineBound(inst, coerceTo(ir[inst.result]->rep, valueOf(written.value, where)));
                return;
            }

            case JsInst::Convert:
                defineBound(inst, convertExpr((JsInstConvert&)inst));
                return;

            case JsInst::Select: {
                auto& written = (JsInstSelect&)inst;
                defineBound(inst, ternary(a, valueOf(written.condition, where),
                                          valueOf(written.whenTrue, where),
                                          valueOf(written.whenFalse, where)));
                return;
            }

            /*
             * A `match` over constants - `mapExpr` chooses the spelling.
             */
            case JsInst::Map: {
                auto& written = (JsInstMap&)inst;
                defineBound(inst, mapExpr(written, valueOf(written.selector, where)));
                return;
            }

            case JsInst::Call: {
                auto value = callExpr((JsInstCall&)inst);
                if(inst.result) defineBound(inst, value);
                else emitExpr(a, value);
                return;
            }

            case JsInst::CallValue: {
                auto value = callValueExpr((JsInstCallValue&)inst);
                if(inst.result) defineBound(inst, value);
                else emitExpr(a, value);
                return;
            }

            case JsInst::Host: {
                /*
                 * `throw message` is a statement rather than an expression, which is why it is
                 * handled here and not in `hostExpr`: JavaScript has no `abort`, and the one thing
                 * every host agrees on is that an exception nobody catches ends the program with the
                 * value it carried reported. What follows it in the block is dead code the
                 * structurer never reaches - see `JsInst::Throw`, where the same is argued for the
                 * decision not to make it a terminator.
                 */
                auto& host = (JsInstHost&)inst;
                if(host.op == mir::HostOp::Throw) {
                    // The message is the *member*, not an operand - `hostHasMember` includes this
                    // row, and it is what `hostThrow("...")`'s literal became at expansion. So the
                    // text is spent at compile time and leaves nothing in the program, which is the
                    // whole of what the literal parameter bought (`lib/Native/Host.js.yana`).
                    emit(a, make<ThrowStmt>(a, variable(a, throwMessage(host.member))));
                    return;
                }

                auto value = hostExpr(host);
                if(!inst.result) { emitExpr(a, value); return; }

                // `hostRange` is the host's own guarantee where it is narrower than the
                // representation - see `JsInstHost::hostRange`, which is where the one row that
                // has one is named.
                defineBound(inst, host.hostRange ? asHostLength(a, value) : value);
                return;
            }

            case JsInst::Helper: {
                auto value = helperExpr((JsInstHelper&)inst);
                if(inst.result) defineBound(inst, value);
                else emitExpr(a, value);
                return;
            }

            case JsInst::TypeDrop: {
                auto& written = (JsInstTypeDrop&)inst;
                emitExpr(a, call(a, variable(a, typeDropNameFor(a, written.type, written.drop,
                                                                written.reclaim, written.source)),
                                 valueOf(written.value, where)));
                return;
            }

            /*
             * The two forms a multi-part value takes where one value is the only thing that fits:
             * an object of the parts' shape, or a row of them.
             *
             * Which one it is comes off the *materialized representation* the node names rather than
             * off the parts themselves, which is what lets a function pair and a vector be one
             * opcode: the pair's form is a two-property object whose keys the shape gives and a
             * vector's is `lanes` elements in order, typed where the lane has a typed row.
             */
            case JsInst::Materialize: {
                auto& written = (JsInstMaterialize&)inst;
                auto& to = rep(ir[inst.result]->rep);

                // A `Cell` is an object of the `{$o, $k}` shape, which is what makes a flat
                // reference materialize the same descriptor `borrow.describe` builds - one
                // spelling for a reference whatever produced it.
                /*
                 * A **three-part** reference - `(owner, key, scale)` - is the one `Cell` that is not
                 * its parts in a shape: the scale has no property to go in, so the one value is
                 * `$bitCell`'s accessor box (J84, `codegen/js/place.cpp`).
                 */
                if(to.kind == JsRepKind::Cell && written.values.size() == 3) {
                    auto& content = rep(to.content);
                    auto width = content.domain == NumberDomain::Bool01 ? 1u : U32(content.bits);
                    defineBound(inst, bitCellCall(a, valueOf(written.values.get(ir, 0), where),
                                                  valueOf(written.values.get(ir, 1), where),
                                                  valueOf(written.values.get(ir, 2), where), width,
                                                  content.domain == NumberDomain::Signed));
                    return;
                }

                if(to.kind == JsRepKind::GcObject || to.kind == JsRepKind::Cell) {
                    defineBound(inst, shapedObject(to.shape, written.values, where));
                    return;
                }

                if(to.kind != JsRepKind::HostArray) {
                    fail("met a materialization into a form it cannot build"_v, where);
                    return;
                }

                defineBound(inst, rowOf(to, written.values, where));
                return;
            }

            case JsInst::Split: {
                auto& written = (JsInstSplit&)inst;
                auto& from = rep(ir[written.from]->rep);

                if(from.kind == JsRepKind::GcObject || from.kind == JsRepKind::Cell) {
                    defineBound(inst, field(a, valueOf(written.from, where),
                                            propertyOf(from.shape, written.part)));
                    return;
                }

                if(from.kind != JsRepKind::HostArray) {
                    fail("met a split of something that is not a materialized value"_v, where);
                    return;
                }

                defineBound(inst, index(a, valueOf(written.from, where), written.part));
                return;
            }

            // `Math.min` and `Math.max` - see `JsMathOp`, where the pair being its own row rather
            // than a `Host` node is argued.
            case JsInst::Math: {
                auto& written = (JsInstMath&)inst;
                defineBound(inst, hostCall(a, "Math"_v,
                                           written.op == JsMathOp::Min ? "min"_v : "max"_v,
                                           valueOf(written.lhs, where), valueOf(written.rhs, where)));
                return;
            }

            case JsInst::Throw:
                emit(a, make<ThrowStmt>(a, valueOf(((JsInstThrow&)inst).value, where)));
                return;

            case JsInst::Jump:
            case JsInst::Branch:
            case JsInst::Switch:
            case JsInst::Return:
            case JsInst::Unreachable:
            case JsInst::KindCount:
                // Terminators are the structurer's, which is the one caller that knows where control
                // is going and what the enclosing constructs are.
                return;
        }
    }

    // ------------------------------------------------------- block-argument coalescing

    // ---------------------------------------------------------------- control flow

    U32 blockCount() { return body.blocks.size(); }

    JsBlock* blockAt(U32 index) { return body.block(ir, JsBlockId(index)); }

    // The CFG facts, computed once per body and shared with the verifier and the optimizer -
    // `jsir::JsCfg`, where the three walks this replaced are named. Held as a pointer because the
    // structurer is constructed before `prepareCfg` runs.
    const JsCfg* flow = nullptr;

    void prepareCfg() {
        auto count = blockCount();

        flow = &jsCfgOf(target, body);
        reachedCount = flow->reachedCount;

        /*
         * The two reasons a block owns a label, and they are the whole of the structuring's input.
         *
         * **Two ways forward into it.** Such a block is where two arms meet, and it is written once
         * in the scope of its immediate dominator with each arm leaving to it - which is sound for
         * any reducible graph, because a forward edge runs up the reverse postorder and the label of
         * anything later than the block it is written in is still open.
         *
         * **A loop leaves to it.** A `while` whose test is its header has exactly one edge to the
         * block behind it, so the count above says nothing; what says it is that the block is a
         * child of a loop header and is not in that header's loop. Such a block shares the loop's
         * own label wherever it is the innermost one, which is what keeps the `for(;;) { ... break
         * L; }` shape `recoverLoops` reads back into a `while`.
         */
        a.labelled.reset(count);

        for(U32 i = 0; i < count; i++) {
            if(flow->reachable[i] && flow->forwardPreds[i] > 1) a.labelled.set(i, true);
            if(!flow->loopHeader[i]) continue;

            for(U32 j = 0; j < count; j++) {
                if(flow->idom[j] == i && flow->reachable[j] && !flow->loops[i][j]) {
                    a.labelled.set(j, true);
                }
            }
        }

        a.emitted.reset(count);
        a.exits.clear();
    }

    /*
     * The label a region opens, found innermost first.
     *
     * Two kinds under one search, because a loop header and the block behind the loop may share one
     * label: `continue L` is the header's entry and `break L` is the exit's, and which of the two an
     * edge wants is decided by whether it retreats, not by which construct the label is on.
     */
    Name labelFor(U32 block, bool loop) {
        for(Size i = a.exits.size(); i > 0; i--) {
            auto& exit = a.exits[i - 1];
            if(exit.block == block && exit.loop == loop) return exit.label;
        }

        return Name {};
    }

    // The regions one block owns, outermost first - its dominator-tree children that carry a label.
    void labelChildren(U32 block, RegionList& into) {
        for(U32 i = 0; i < blockCount(); i++) {
            if(i == block || flow->idom[i] != block) continue;
            if(!flow->reachable[i] || !a.labelled[i]) continue;

            into.push(i);
        }

        // Latest region outermost, so that a branch out of an earlier one finds a label still open -
        // see `computeOrder`, which is where the order comes from and why it is not post-dominance.
        for(Size i = 1; i < into.size(); i++) {
            for(Size j = i; j > 0; j--) {
                if(flow->rpo[into[j - 1]] >= flow->rpo[into[j]]) break;

                auto swap = into[j - 1];
                into[j - 1] = into[j];
                into[j] = swap;
            }
        }
    }

    /*
     * Whether control can fall out of the end of a list.
     *
     * `opt.cpp` has the general form of this question and it is deliberately not shared: this one is
     * asked of a list the structurer has just built out of its own constructs, where a conservative
     * "yes" costs one `else` that could have been hoisted and nothing else. What that buys is that a
     * labelled region and a loop need no answer here at all - the emitter never asks about one in a
     * position where the answer decides anything.
     */
    bool completesHere(StmtList& list) {
        if(list.isEmpty()) return true;
        return completesHere(a.base[list.get(a.base, list.size() - 1)]);
    }

    bool completesHere(Stmt* stmt) {
        switch(stmt->kind) {
            case Stmt::Return:
            case Stmt::Throw:
            case Stmt::Break:
            case Stmt::Continue:
                return false;

            case Stmt::If: {
                auto& branch = *(IfStmt*)stmt;
                if(branch.otherwise.isEmpty()) return true;

                return completesHere(branch.then) || completesHere(branch.otherwise);
            }

            case Stmt::Block:
                return completesHere(((BlockStmt*)stmt)->body);

            default:
                return true;
        }
    }

    /*
     * `break L` removed from every tail position of the region it leaves.
     *
     * The dominator-tree rule gives every join a region, and most joins are the bottom of an `if`
     * whose arms both simply arrive there - so the region reads `L: { if (c) { A; break L } else {
     * B; break L } }` where `if (c) { A } else { B }` says the same thing. Leaving a region by
     * falling out of its end and leaving it by naming it arrive at the same statement, so a `break`
     * in tail position is removable whether or not the label survives; `regionIsNamed` below is then
     * what says the region itself can go.
     *
     * Tail position is more than the last statement, for `trimTailReturns`' reason in `opt.cpp`: a
     * trailing `if` leaves both arms in one, a trailing block leaves its body in one, and removing a
     * statement puts whatever preceded it in one. A **loop** body is deliberately not tail position -
     * falling off the end of one goes round again.
     */
    void stripTailBreaks(StmtList& list, Name label) {
        if(list.isEmpty()) return;

        auto last = a.base[list.get(a.base, list.size() - 1)];

        if(last->kind == Stmt::Break && ((BreakStmt*)last)->label.text == label.text) {
            list.remove(a.base, list.size() - 1);
            stripTailBreaks(list, label);
            return;
        }

        if(last->kind == Stmt::If) {
            auto& branch = *(IfStmt*)last;
            stripTailBreaks(branch.then, label);
            stripTailBreaks(branch.otherwise, label);
            return;
        }

        if(last->kind == Stmt::Block) {
            stripTailBreaks(((BlockStmt*)last)->body, label);
            return;
        }

        if(last->kind == Stmt::Labelled) {
            auto content = a.base[((LabelledStmt*)last)->content];
            if(content->kind == Stmt::Block) stripTailBreaks(((BlockStmt*)content)->body, label);
        }
    }

    bool regionIsNamed(StmtList& list, Name label) {
        for(auto pointer: list.contents(a.base)) {
            if(regionIsNamed(a.base[pointer], label)) return true;
        }

        return false;
    }

    bool regionIsNamed(Stmt* stmt, Name label) {
        switch(stmt->kind) {
            case Stmt::Break:
                return ((BreakStmt*)stmt)->label.text == label.text;

            case Stmt::Block:
                return regionIsNamed(((BlockStmt*)stmt)->body, label);

            case Stmt::If: {
                auto& branch = *(IfStmt*)stmt;
                return regionIsNamed(branch.then, label) || regionIsNamed(branch.otherwise, label);
            }

            case Stmt::Loop:
                return regionIsNamed(((LoopStmt*)stmt)->body, label);

            case Stmt::Labelled:
                return regionIsNamed(a.base[((LabelledStmt*)stmt)->content], label);

            default:
                return false;
        }
    }

    // Whether a list's **last** statement is `break L` - the one position from which leaving the
    // region and falling out of the arm are the same arrival, which is what makes the fold below a
    // rewrite of spelling rather than of control.
    bool endsWithBreak(StmtList& list, Name label) {
        if(list.isEmpty()) return false;

        auto last = a.base[list.get(a.base, list.size() - 1)];
        return last->kind == Stmt::Break && ((BreakStmt*)last)->label.text == label.text;
    }

    /*
     * `L: { if (c) { A; break L } REST }` written as the `if/else` it is - and `L: { if (c) break L;
     * REST }` as `if (!c) { REST }`, which is the same rule with an empty `A`.
     *
     * A branch one of whose arms is the join itself is where this comes from, and it is half of
     * every `match` arm in the corpus: `emitBranch` hoists the arm that leaves, which is right where
     * the arm is a `throw` and wrong where it is the one `break` that made the region survive at
     * all. Putting the remainder back under the test removes the region *and* a level of nesting -
     * `comparisons` in `test/base` is 1,198 bytes of the empty-`A` shape alone.
     *
     * The **non-empty** arm is J76's half and is the larger one: the region and its `break` are two
     * whole lines per join, and `Base.js` carried 1,928 labelled regions against the shipping
     * backend's 252. What makes it sound is where the `break` is: in tail position of the arm,
     * leaving the region *is* falling out of the arm, so what is left of the arm and what is left of
     * the region are the two sides of one test, and every `break L` still standing in either half
     * names the end of the same `if`.
     *
     * Nothing else is asked, and the two questions the first version asked are worth writing down
     * because both were wrong. Refusing where the **remainder** still names the label - on the
     * grounds that a `match` is a *chain* of guards under one region and folding the first of them
     * puts every later arm one level deeper - turns the rule off almost entirely: the arms that end
     * in a bare `break L` are exactly the chain's, and the shape is worth **-34,447** bytes over
     * `Base.js` with the refusal out and **0** with it in. `resolve/Niche.yana`, a five-arm `match`,
     * is the one fixture that pays for it: 1,363 bytes against 1,339, and `resolve/Switch.yana`,
     * a nine-arm one, 4,075 against 3,808.
     *
     * `if (c) {}` rather than a negation written here, for `emitBranch`'s reason: `flipEmptyThen` in
     * `opt.cpp` inverts through `negatedCondition`, which has the ranges.
     */
    void foldRegionGuard(StmtList& inner, Name label) {
        if(inner.size() < 2) return;

        auto guardIsThen = false;
        {
            auto first = a.base[inner.get(a.base, 0)];
            if(first->kind != Stmt::If) return;

            auto& branch = *(IfStmt*)first;
            guardIsThen = branch.otherwise.isEmpty() && endsWithBreak(branch.then, label);

            auto guardIsElse = branch.then.isEmpty() && endsWithBreak(branch.otherwise, label);
            if(!guardIsThen && !guardIsElse) return;
        }

        // The remainder collected before the node is read back, for the reason `foldDecidedIf` in
        // `opt.cpp` states: a push allocates from the region `a.base` points into, so a raw node
        // pointer taken across one may be into a buffer that has moved.
        StmtList rest;
        for(Size i = 1; i < inner.size(); i++) rest.push(a.file.arena, inner.get(a.base, i));

        auto& branch = *(IfStmt*)a.base[inner.get(a.base, 0)];
        auto& arm = guardIsThen ? branch.then : branch.otherwise;

        arm.remove(a.base, arm.size() - 1);

        if(guardIsThen) branch.otherwise = rest;
        else            branch.then = rest;

        while(inner.size() > 1) inner.remove(a.base, inner.size() - 1);
    }

    /*
     * A region, with the statements that cannot leave it lifted out of it and its label dropped
     * where nothing names it.
     *
     * The lift is `L: { A; B }` -> `A; L: { B }` wherever nothing in `A` names `L`, and it is not
     * cosmetic: `foldInitialValue` in `opt.cpp` joins the `var` a function declares to the write
     * that fills it by searching *forward through one list*, and a region between the two is where
     * that search stops. `readsPresets` is the measurement - its whole body is one region, because a
     * chain of refutable `let`s shares one failure exit, and without the lift its forty slots stayed
     * declared at the top and assigned inside: 4,176 bytes on one function.
     */
    void emitRegion(Name label, StmtList& inner) {
        while(inner.isNotEmpty() && !regionIsNamed(a.base[inner.get(a.base, 0)], label)) {
            a.body->push(a.file.arena, inner.get(a.base, 0));
            inner.remove(a.base, 0);
        }

        foldRegionGuard(inner, label);
        stripTailBreaks(inner, label);

        if(regionIsNamed(inner, label)) {
            emit(a, make<LabelledStmt>(a, label, asStmt(a, make<BlockStmt>(a, inner))));
            return;
        }

        for(auto statement: inner.contents(a.base)) a.body->push(a.file.arena, statement);
    }

    /*
     * The block arguments one edge owes, as assignments.
     *
     * This is the one place a block argument stops being an argument - §2.3's "block arguments have
     * not become mutable `var`s and edge assignments" holds right up to here. The assignment belongs
     * on the edge rather than at the join, which is exactly what makes it disappear when the two
     * arms of an `if` are emitted in place.
     */
    void emitEdge(JsEdge& edge, LocationId where) {
        if(!edge.target) return;

        /*
         * The copies are a **parallel** assignment and JavaScript has only sequential ones - J90.
         * `jump -> block1(%2, %1, %15)`, a loop swapping two variables, was written
         * `p1 = p2; p2 = p1;` until then, which hands both the old `p2`. The coalescer's sequential
         * rule keeps two *coalesced* arguments from meeting that hazard, and nothing kept two
         * plain ones from it. So the copies are ordered: one is written once no other pending copy
         * reads what it overwrites, which leaves the source order wherever that order was already
         * right, and a cycle is broken by saving one destination in a `var` first - the swap's
         * `var t = p1; p1 = p2; p2 = t;`. A source that is neither a name nor a literal is saved
         * before anything is assigned, since which names it reads is not asked here.
         */
        struct Copy {
            Name to;
            JsPtr<Expr> from;
        };

        SmallArray<Copy, 8> pending;

        auto to = ir[edge.target];
        for(U32 i = 0; i < edge.arguments.size(); i++) {
            auto argument = ir[to->argumentAt(ir, i)];
            auto id = argument->id.index;
            if(id >= a.arguments.size() || !a.arguments[id].text) continue;

            /*
             * `p = p` is what a coalesced copy becomes, and it is written as nothing - which is the
             * whole of what the coalescing buys. Compared as *names* rather than as values,
             * because that is what the assignment would say: two values in one class are one
             * variable, and a class's members are exactly the values whose live ranges do not meet.
             */
            auto supplied = edge.arguments.get(ir, i);
            auto sourceId = supplied ? ir[supplied]->id.index : maxLimit<U32>;

            if(sourceId < a.arguments.size() && a.arguments[sourceId].text == a.arguments[id].text) {
                continue;
            }

            pending.push(Copy { a.arguments[id], valueOf(supplied, where) });
        }

        auto isLiteral = [&](JsPtr<Expr> expr) {
            switch(a.base[expr]->kind) {
                case Expr::Number: case Expr::BigInt: case Expr::String: case Expr::Bool:
                case Expr::Null: case Expr::Undefined:
                    return true;
                default:
                    return false;
            }
        };

        auto reads = [&](JsPtr<Expr> expr, Name name) {
            auto node = a.base[expr];
            return node->kind == Expr::Var && ((VarExpr*)node)->name.text == name.text;
        };

        if(pending.size() > 1) {
            for(auto& copy: pending) {
                auto kind = a.base[copy.from]->kind;
                if(kind == Expr::Var || isLiteral(copy.from)) continue;
                copy.from = declare(a, uniqueName(a, "t"_v, true), copy.from);
            }
        }

        while(pending.size() > 0) {
            auto chosen = pending.size();
            for(Size i = 0; i < pending.size() && chosen == pending.size(); i++) {
                auto free = true;
                for(Size j = 0; j < pending.size(); j++) {
                    if(j != i && reads(pending[j].from, pending[i].to)) free = false;
                }

                if(free) chosen = i;
            }

            if(chosen == pending.size()) {
                // A cycle: every pending destination is some other copy's source.
                auto saved = pending[0].to;
                auto temporary = declare(a, uniqueName(a, "t"_v, true), variable(a, saved));
                for(Size j = 1; j < pending.size(); j++) {
                    if(reads(pending[j].from, saved)) pending[j].from = temporary;
                }

                continue;
            }

            emitExpr(a, assign(a, variable(a, pending[chosen].to), pending[chosen].from));
            pending.remove(chosen);
        }
    }

    void doTree(U32 block);
    void emitBlocks(U32 owner, RegionList& kids, Size at);
    void emitLoop(U32 header, RegionList& outside, Size at, RegionList& inside);

    /*
     * One edge, as the three things an edge can be.
     *
     * A retreating edge is the back edge of the loop whose header it names, so it is that loop's
     * `continue`. A forward edge into a block that owns a label is that label's `break`. Everything
     * else is a block with one way in that this one dominates, so it is written here.
     *
     * A label that is not open is the whole of how an irreducible graph is discovered: in a
     * reducible one every forward edge runs up the reverse postorder, so the region of anything it
     * can name is still open - see `run`, where the discovery is taken.
     */
    void doEdge(JsEdge& edge, U32 from, LocationId where) {
        emitEdge(edge, where);
        if(!edge.target) return;

        auto to = ir[edge.target]->index.index;

        if(flow->dominates(to, from)) {
            auto label = labelFor(to, true);
            if(!label.text) { unstructured = true; return; }

            emit(a, make<ContinueStmt>(a, label));
            return;
        }

        if(a.labelled[to]) {
            auto label = labelFor(to, false);
            if(!label.text) { unstructured = true; return; }

            emit(a, make<BreakStmt>(a, label));
            return;
        }

        doTree(to);
    }

    /*
     * An `if` whose arms are the two edges - with an arm control never leaves hoisted out from under
     * it.
     *
     * `if (c) { throw } else { <the rest of the body> }` is what a bounds check would otherwise be,
     * and there are nine hundred of them in the library: every one of them would put the whole
     * remainder of its function one level deeper. Hoisting is sound exactly when the arm that stays
     * cannot fall out of itself, which is what `completesHere` answers, and it is also what leaves a
     * loop's test as the `if (c) { ... }` with the exit behind it that `recoverLoops` reads back
     * into a `while`.
     *
     * The `if (c) {} else { ... }` form is deliberate in the second case rather than a negation
     * written here: `flipEmptyThen` in `opt.cpp` inverts it through `negatedCondition`, which knows
     * the ranges and turns `!(a < b)` into `a >= b`, and a `!` written here would arrive there as a
     * condition it has to see through instead.
     */
    void emitBranch(JsPtr<Expr> condition, StmtList then, StmtList otherwise) {
        if(otherwise.isNotEmpty() && !completesHere(then)) {
            emit(a, make<IfStmt>(a, condition, then, StmtList {}));
            for(auto statement: otherwise.contents(a.base)) a.body->push(a.file.arena, statement);
            return;
        }

        if(then.isNotEmpty() && otherwise.isNotEmpty() && !completesHere(otherwise)) {
            emit(a, make<IfStmt>(a, condition, StmtList {}, otherwise));
            for(auto statement: then.contents(a.base)) a.body->push(a.file.arena, statement);
            return;
        }

        emit(a, make<IfStmt>(a, condition, then, otherwise));
    }

    /*
     * One block's terminator, with three things left to the caller.
     *
     * The structured walk and the state machine below it both end a block, and both used to say the
     * whole of this switch - two copies of five arms, of which two agreed exactly, two differed in
     * one call and one differed in a statement. What they actually disagree about is three things,
     * and each of them is a decision rather than an accident:
     *
     *  - **what an edge is.** A structured edge is `doEdge`, which is a `break`, a `continue` or the
     *    target written inline; a dispatched one is an assignment to the state variable and a
     *    `continue` around the loop;
     *  - **what an `if` is.** The structured walk hoists an arm control never leaves out from under
     *    it (`emitBranch`, above), which is what keeps nine hundred bounds checks from putting the
     *    rest of their function one level deeper. The dispatch writes the plain `IfStmt`: both of
     *    its arms end in a `continue`, so neither completes and there is nothing to hoist;
     *  - **what falling out of the block means.** An `Unreachable` writes nothing at all in the
     *    structured form, because whatever put control there already ended the program; in the
     *    dispatch it has to `break` the loop, since an arm that writes nothing falls out of the
     *    chain and spins.
     *
     * Everything else - the `return`, the equality chain a `Switch` becomes, the arm that reports -
     * is one statement of it now.
     */
    template<class Edge, class Branch, class FallOut>
    void emitTerminator(JsInst& terminator, Edge&& edge, Branch&& branch, FallOut&& fallOut) {
        auto where = terminator.source;

        switch(terminator.kind) {
            case JsInst::Return: {
                auto& written = (JsInstReturn&)terminator;
                emit(a, make<ReturnStmt>(a, written.value ? valueOf(written.value, where)
                                                          : JsPtr<Expr>(nullptr)));
                return;
            }

            case JsInst::Unreachable:
                fallOut();
                return;

            case JsInst::Jump:
                edge(((JsInstJump&)terminator).to, where);
                return;

            case JsInst::Branch: {
                auto& written = (JsInstBranch&)terminator;
                auto condition = valueOf(written.condition, where);

                auto then = collect(a, [&] { edge(written.whenTrue, where); });
                auto otherwise = collect(a, [&] { edge(written.whenFalse, where); });

                branch(condition, then, otherwise);
                return;
            }

            /*
             * A multi-way branch, as nested `if`s over the selector.
             *
             * The same structuring an `if` gets, generalized to N of them, which is what keeps one
             * implementation and one set of exit shapes. The host's own `switch` is the obvious
             * alternative and is deliberately not taken yet: it needs a case whose body falls out
             * through a `break`, which is a fourth way to leave a construct and a third thing
             * `labelFor` would have to know about.
             *
             * next: measure `switch` against this chain the way §5.6 asks, and take it if the arm
             * counts in the corpus pay for the extra exit shape.
             */
            case JsInst::Switch: {
                auto& written = (JsInstSwitch&)terminator;
                auto selector = valueOf(written.selector, where);

                auto chain = collect(a, [&] { edge(written.fallback, where); });
                auto items = written.cases.pointer(ir);

                for(Size i = written.cases.size(); i > 0; i--) {
                    auto& entry = items[i - 1];
                    auto test = binary(a, BinaryOp::Eq, selector, number(a, entry.value));
                    auto arm = collect(a, [&] { edge(entry.edge, where); });

                    chain = collect(a, [&] { branch(test, arm, chain); });
                }

                for(auto statement: chain.contents(a.base)) a.body->push(a.file.arena, statement);
                return;
            }

            default:
                fail("met a terminator it has no arm for"_v, where);
                return;
        }
    }

    // One block's instructions and its terminator, with the regions it owns already open.
    void nodeBody(U32 block) {
        if(a.emitted[block]) { unstructured = true; return; }
        a.emitted.set(block, true);

        for(auto instPointer: blockAt(block)->instructions(ir)) emitInst(*ir[instPointer]);

        auto terminator = blockAt(block)->terminator(ir);
        if(!terminator) return;

        emitTerminator(*terminator,
                       [&](JsEdge& to, LocationId where) { doEdge(to, block, where); },
                       [&](JsPtr<Expr> condition, StmtList then, StmtList otherwise) {
                           emitBranch(condition, then, otherwise);
                       },
                       [] {});
    }

    /*
     * The graph as a state machine, which is what an irreducible one is emitted as.
     *
     * The walk above covers every **reducible** graph and nothing else covers the rest:
     * `inside$step` in `test/resolve/iter/LensRegion.yana` is a resume `switch` whose arms enter a
     * cycle at three different blocks, so the cycle has three entries and no loop header - the
     * definition of irreducible. Refusing it would make the one construct the rewrite produces that
     * the shipping stage does not a target gap forever, so the answer is the standard one: every
     * block becomes an arm of `for(;;)`, the state variable names which arm runs next, and an edge
     * is an assignment plus a `continue`.
     *
     * **How much this is now asked of is the measurement that matters.** Before J73 the walk above
     * was a chain of special cases and six of `test/base`'s functions fell through to here - 86,333
     * bytes of `Base.js`, four of them the five largest per-function gaps against the shipping
     * backend - while **every CFG in that program is reducible**. So none of those six was this
     * lowering's business at all. What is left here is the genuinely irreducible remainder, which in
     * the whole corpus is the bodies of one fixture.
     *
     * **It is the whole function rather than the irreducible region**, and the measurement is why.
     * Confining it means giving the region's exits somewhere to go, which is a second dispatch after
     * the loop over the exit targets; on the one body in the corpus that needs this, the strongly
     * connected component with the three entries is 20 of the function's 30 blocks, so the confined
     * form would pay that second dispatch to keep a third of the body structured. If a later
     * measurement finds a body where the region is small, the region is `sccOf` away and the exit
     * dispatch is this same chain - but a confinement nothing measures is a second code path to get
     * wrong. `doc/progress/2026-09-21-J67-jsir-coverage.md` §1 has the numbers.
     *
     * The arm chain is a **binary search** over the state rather than a run of `===` tests, because
     * an iterator's step is hot code and the linear chain the `Switch` terminator writes is a
     * reasonable answer for three arms and not for thirty. Each dispatch costs ceil(log2 n)
     * comparisons.
     */
    void emitDispatch(U32 entry) {
        auto count = blockCount();
        if(!count) return;

        auto state = generatedName(a, "b"_v, a.labelCounter);
        auto label = generatedName(a, "L"_v, a.labelCounter++);

        emit(a, make<DeclStmt>(a, state, number(a, entry), false));

        // `state = <block>; continue L` - one edge, with whatever the block arguments owe in front.
        auto goTo = [&](JsEdge& edge, LocationId where) {
            emitEdge(edge, where);

            auto target = edge.target ? ir[edge.target]->index.index : kNoBlock;
            emitExpr(a, assign(a, variable(a, state), number(a, target)));
            emit(a, make<ContinueStmt>(a, label));
        };

        Array<StmtList> arms;
        for(U32 i = 0; i < count; i++) {
            arms.push(collect(a, [&] {
                for(auto instPointer: blockAt(i)->instructions(ir)) emitInst(*ir[instPointer]);

                auto terminator = blockAt(i)->terminator(ir);

                // An arm that writes nothing falls out of the chain and around the loop, so both
                // "no terminator" and an `Unreachable` say `break L` here rather than spinning -
                // which is the third thing `emitTerminator` leaves to its caller.
                auto fallOut = [&] { emit(a, make<BreakStmt>(a, label)); };
                if(!terminator) { fallOut(); return; }

                emitTerminator(*terminator, goTo,
                               [&](JsPtr<Expr> condition, StmtList then, StmtList otherwise) {
                                   emit(a, make<IfStmt>(a, condition, then, otherwise));
                               },
                               fallOut);
            }));
        }

        auto loopBody = collect(a, [&] {
            auto chain = dispatchChain(arms, state, 0, count);
            for(auto statement: chain.contents(a.base)) a.body->push(a.file.arena, statement);
        });

        emit(a, make<LabelledStmt>(a, label, asStmt(a, make<LoopStmt>(a, loopBody))));
    }

    StmtList dispatchChain(Array<StmtList>& arms, Name state, U32 lo, U32 hi) {
        if(hi - lo == 1) return arms[lo];

        auto mid = lo + (hi - lo) / 2;
        auto test = binary(a, BinaryOp::Lt, variable(a, state), number(a, mid));

        auto then = dispatchChain(arms, state, lo, mid);
        auto otherwise = dispatchChain(arms, state, mid, hi);

        StmtList wrapped;
        wrapped.push(a.file.arena, asStmt(a, make<IfStmt>(a, test, then, otherwise)));
        return wrapped;
    }

    /*
     * One constant body as the expression a `var` is initialized with - `convertConstant`'s other
     * end, and the reason this file has no second constant walk.
     *
     * A constant body is one block of value-producing instructions ending in a `return`, and it is a
     * *tree*: every result has exactly one reader, which is its parent. So the ordinary `var` per
     * result would be a run of bindings nothing else names, and `inlineOnly` turns `defineBound`
     * into `define` - the expression is registered where it is built and nested into its parent.
     * That is what makes the emitted text the nested literal a global initializer has always been
     * rather than a preamble the file has nowhere to put.
     */
    JsPtr<Expr> runConstant() {
        a.values.clear();
        a.valueNames.clear();
        a.variables.clear();
        a.arguments.clear();
        for(U32 i = 0; i < body.values.size(); i++) a.values.push(nullptr);

        inlineOnly = true;

        auto block = body.entry ? ir[body.entry] : nullptr;
        if(!block) {
            fail("met a constant with no body"_v, kNullLocation);
            return nullValue(a);
        }

        JsPtr<Expr> answer = nullValue(a);
        for(auto pointer: block->instructions(ir)) {
            auto& inst = *ir[pointer];

            if(inst.kind == JsInst::Return) {
                auto& written = (JsInstReturn&)inst;
                if(written.value) answer = valueOf(written.value, inst.source);
                break;
            }

            emitInst(inst);
        }

        return failed ? nullValue(a) : answer;
    }

    StmtList run(JsList<Name, false>& parameters) {
        /*
         * The `var`s this function needs, declared before anything reads one.
         *
         * Two kinds and no others: a `Var` place's storage, and a block argument. `var` rather than
         * `let` for the reason `DeclStmt` states - a JSIR value is defined once and used wherever its
         * definition dominates, which is not the same as "wherever it is lexically enclosed" once a
         * CFG has been structured.
         */
        // `reserve` before each fill, for `JsConvertScratch::reset`'s reason: `clear` keeps the
        // buffer and `push` still climbs to the known count one doubling at a time. `valueNames` is
        // filled on demand by `defineBound` rather than here, which is why it is reserved and not
        // filled.
        a.values.clear();
        a.valueNames.clear();
        a.variables.clear();
        a.arguments.clear();

        a.values.reserve(U32(body.values.size()));
        a.valueNames.reserve(U32(body.values.size()));
        a.variables.reserve(U32(body.variables.size()));
        a.arguments.reserve(U32(body.values.size()));

        for(U32 i = 0; i < body.values.size(); i++) a.values.push(nullptr);
        for(U32 i = 0; i < body.values.size(); i++) a.arguments.push(Name {});

        a.variableNamed.clear();
        for(U32 i = 0; i < body.variables.size(); i++) a.variableNamed.push(0);

        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = blockAt(i);
            if(block->deleted) continue;

            for(auto pointer: block->instructions(ir)) {
                auto& inst = *ir[pointer];
                auto placeId = inst.kind == JsInst::Load ? ((JsInstLoad&)inst).place
                             : inst.kind == JsInst::Store ? ((JsInstStore&)inst).place : JsPlaceId();
                if(!placeId.valid()) continue;

                auto place = body.place(ir, placeId);
                if(place->kind == JsPlaceKind::BitRange && place->word.valid()) place = body.place(ir, place->word);
                if(place->kind == JsPlaceKind::Var && place->slot < a.variableNamed.size()) a.variableNamed[place->slot] = 1;
            }
        }

        auto statements = collect(a, [&] {
            for(U32 i = 0; i < body.variables.size(); i++) {
                auto name = generatedName(a, "s"_v, i);
                a.variables.push(name);
                if(a.variableNamed[i]) emit(a, make<DeclStmt>(a, name, nullptr, false));
            }

            /*
             * The entry block's arguments are the emitted parameters, named after what the
             * declaration called them so that the source still reads as the function it is.
             *
             * An `Omitted` parameter has no position at all, which is why the walk is over the ABI
             * rather than over the declared arguments: the two disagree exactly where a unit
             * parameter was left out, and a name bound at the wrong index is a body reading the
             * argument beside the one it meant.
             */
            auto& abi = target.abi(body.abi);
            U32 position = 0;

            /*
             * The generic environment stands for no declared argument, so it is named before the
             * walk and the walk starts one position in - the same offset `convert.cpp` applied when
             * it put the table there, read from the other end.
             */
            if(abi.takesGenEnv && body.parameters.size()) {
                auto value = ir[body.parameters.get(ir, position++)];
                auto name = uniqueName(a, "$g"_v, true);
                parameters.push(a.file.arena, name);
                a.values[value->id.index] = variable(a, name);
            }

            for(U32 i = 0; i < abi.parameters.size(); i++) {
                if(abi.parameters[i].kind == JsPassingKind::Omitted) continue;
                if(position >= body.parameters.size()) break;

                auto written = target.argumentName(body.function, i);

                auto text = written ? stringView(a.context.findName(written)) : StringView {};

                /*
                 * A multi-part parameter is `positions` emitted parameters, named `v$0`, `v$1`, ...
                 *
                 * The suffix rather than four unrelated generated names, because what a reader of
                 * the emitted file needs is to see that the four are one argument - which is the
                 * same thing `laneName` buys the shipping backend, here read off the plan instead of
                 * off a vector type.
                 */
                auto count = abi.parameters[i].positions;
                for(U16 part = 0; part < count && position < body.parameters.size(); part++) {
                    auto value = ir[body.parameters.get(ir, position++)];

                    auto name = Name {};
                    if(count > 1) {
                        char suffix[32];
                        Size length = 0;

                        for(Size c = 0; c < text.length && length + 12 < sizeof(suffix); c++) {
                            suffix[length++] = text.ptr[c];
                        }

                        if(!length) suffix[length++] = 'a';
                        suffix[length++] = '$';
                        length += show(U64(part), suffix + length, sizeof(suffix) - length);
                        name = uniqueName(a, StringView { suffix, U32(length) }, true);
                    } else {
                        name = text.length ? uniqueName(a, text, true) : generatedName(a, "a"_v, i);
                    }

                    parameters.push(a.file.arena, name);
                    a.values[value->id.index] = variable(a, name);
                }
            }

            /*
             * Every other block's argument becomes a `var` the edges assign - one per coalescing
             * class rather than one per argument, which is §6.8.
             *
             * Which arguments share a class is `jsir::jsCoalesceArguments`'s answer and the
             * liveness behind it is that file's; what is left here is the name, which is the half
             * that genuinely belongs where the variables are.
             */
            auto entry = ir[body.entry];
            auto& classes = jsCoalesceArguments(target, body);

            a.classNames.clear();
            for(U32 i = 0; i < body.values.size(); i++) a.classNames.push(Name {});

            for(U32 i = 0; i < body.blocks.size(); i++) {
                auto block = blockAt(i);
                if(block == entry || block->deleted) continue;

                for(U32 j = 0; j < block->argumentCount(); j++) {
                    auto argument = ir[block->argumentAt(ir, j)];
                    auto id = argument->id.index;

                    auto root = classes.rootOf(id);
                    if(root == JsCoalesced::kNoClass) root = id;

                    if(!a.classNames[root].text) {
                        a.classNames[root] = generatedName(a, "p"_v, root);
                        emit(a, make<DeclStmt>(a, a.classNames[root], nullptr, false));
                    }

                    a.arguments[id] = a.classNames[root];
                    a.values[id] = variable(a, a.classNames[root]);
                }
            }

            prepareCfg();

            auto structured = collect(a, [&] { doTree(entry->index.index); });

            if(unstructured && !failed) {
                a.emitted.reset(blockCount());
                a.exits.clear();
                emitDispatch(entry->index.index);
                return;
            }

            for(auto statement: structured.contents(a.base)) a.body->push(a.file.arena, statement);
        });

        return failed ? StmtList {} : statements;
    }
};

/*
 * Control flow as the dominator tree - J73, and it replaces a walk that was a chain of special cases.
 *
 * The rule is one sentence: **a block with more than one way forward into it owns a labelled region
 * in the scope of its immediate dominator, and every edge into it is that label's `break`.** A loop
 * header owns a `for(;;)` instead and a retreating edge is that loop's `continue`. Everything else
 * has one way in and is dominated by the block that branches to it, so it is written there.
 *
 * That covers **every reducible graph** and it is worth saying why, because the walk it replaces
 * covered most of them and discovered the rest by meeting a block it had already written. A forward
 * edge in a reducible graph runs up the reverse postorder, and the regions open at any point are
 * exactly those of the blocks later in that order than whatever is being written - so the label an
 * edge names is always one of them. The regions under one block are therefore opened latest-first,
 * which is `labelChildren`'s sort and is the same order `ownedMerges` used to sort its joins in.
 *
 * What went with it was `loopFollow`, `ownsMerge`, `nestedMerge` and the whole post-dominance
 * fixpoint: the immediate post-dominator was what the old walk used to decide where an `if`'s arms
 * stopped, and it is not a fact about the graph that any of these constructs needs. The measurement
 * is in `doc/progress/2026-09-22-J73-jsir-structure-niches.md` - six functions of `test/base` took
 * the state-machine fallback and every CFG in that program is reducible, so all six were the walk
 * failing rather than the graph having no spelling.
 *
 * Two refinements over the textbook form, and each is a measured cost rather than a tidiness:
 *
 *  - **a loop shares its label with the block behind it.** The textbook form writes the loop inside
 *    that block's region, so `while (c) { ... }` comes out as `B1: { L2: for(;;) { ... break B1; } }`
 *    and `recoverLoops` in `opt.cpp`, which reads `for(;;) { if (c) { ... } break L; }` back into a
 *    `while`, matches none of it. Here the innermost region a loop header owns *outside* its own
 *    natural loop is written as the loop's own label, so leaving the loop and arriving at that block
 *    are one `break`;
 *  - **the regions a loop header owns inside its loop are opened inside the loop.** The textbook
 *    form opens every region a node owns before the loop, which would put a join that belongs to the
 *    loop body behind the loop. `naturalLoop` is what tells the two apart.
 */
void Structurer::doTree(U32 block) {
    if(a.emitted[block]) { unstructured = true; return; }

    RegionList kids;
    labelChildren(block, kids);

    if(!flow->loopHeader[block]) {
        emitBlocks(block, kids, 0);
        return;
    }

    RegionList inside;
    RegionList outside;
    for(auto kid: kids) {
        if(flow->loops[block][kid]) inside.push(kid);
        else outside.push(kid);
    }

    emitLoop(block, outside, 0, inside);
}

// The regions a block owns, from the outermost in, with the block's own body innermost.
void Structurer::emitBlocks(U32 owner, RegionList& kids, Size at) {
    if(unstructured) return;

    if(at == kids.size()) {
        nodeBody(owner);
        return;
    }

    auto kid = kids[at];
    auto label = generatedName(a, "B"_v, a.labelCounter++);

    a.exits.push(Exit { kid, label, false });
    auto inner = collect(a, [&] { emitBlocks(owner, kids, at + 1); });
    a.exits.pop();

    emitRegion(label, inner);
    doTree(kid);
}

/*
 * A loop header: the regions behind the loop from the outermost in, then the loop itself.
 *
 * The innermost of those regions is written as the loop's own label rather than as a block around
 * it - two `Exit` entries for one name, which is the shape the old walk also had and the reason
 * `labelFor` asks which kind it wants. Everything the header owns *inside* the loop is opened
 * within the loop body, where the blocks it labels belong.
 */
void Structurer::emitLoop(U32 header, RegionList& outside, Size at, RegionList& inside) {
    if(unstructured) return;

    if(at + 1 < outside.size()) {
        auto kid = outside[at];
        auto label = generatedName(a, "B"_v, a.labelCounter++);

        a.exits.push(Exit { kid, label, false });
        auto inner = collect(a, [&] { emitLoop(header, outside, at + 1, inside); });
        a.exits.pop();

        emitRegion(label, inner);
        doTree(kid);
        return;
    }

    auto shared = at < outside.size() ? outside[at] : kNoBlock;
    auto label = generatedName(a, "L"_v, a.labelCounter++);

    a.exits.push(Exit { header, label, true });
    if(shared != kNoBlock) a.exits.push(Exit { shared, label, false });

    // The `break` behind the body, and only where control can reach it: an unconditional one at the
    // end of a body that never falls out would tell `completesNormally` in `opt.cpp` that the loop
    // has a way out when it has none.
    auto loopBody = collect(a, [&] {
        emitBlocks(header, inside, 0);
        if(shared != kNoBlock && completesHere(*a.body)) emit(a, make<BreakStmt>(a, label));
    });

    if(shared != kNoBlock) a.exits.pop();
    a.exits.pop();

    emit(a, make<LabelledStmt>(a, label, asStmt(a, make<LoopStmt>(a, loopBody))));
    if(shared != kNoBlock) doTree(shared);
}

} // namespace

StmtList structureBody(Assembly& a, JsBody& body, JsList<Name, false>& parameters) {
    Structurer structurer(a, body);
    return structurer.run(parameters);
}

JsPtr<Expr> mapLiteral(Assembly& a, const JsConstant& value) {
    switch(value.kind) {
        case JsConstant::Number: return number(a, value.number, value.integral);
        case JsConstant::BigInt: return bigInt(a, value.bits, value.isSigned);
        case JsConstant::Text:   return asExpr(a, make<StringExpr>(a, value.text));
        case JsConstant::Null:   return nullValue(a);
        default:                 return asExpr(a, make<UndefinedExpr>(a));
    }
}

JsPtr<Expr> structureConstant(Assembly& a, JsBody& body) {
    Structurer structurer(a, body);
    return structurer.runConstant();
}

} // namespace js
