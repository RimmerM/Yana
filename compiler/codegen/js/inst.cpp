#include "build.h"

/*
 * Instructions.
 *
 * The shape of this file follows resolve/lower.cpp deliberately, because the two are siblings rather
 * than layers: one turns a place into an address and the other turns it into a property chain, and
 * everything above that point - drop insertion, ownership, generic contexts - has already happened
 * for both. Reading them side by side is the cheapest way to check that a rule lands the same way
 * twice, which is the whole argument for having a second target at all (Analysis-JS.md §4.2).
 */

namespace js {

namespace {

// Writing a computed value into a place, honouring a bit range where the place names one. Defined
// below, and forward-declared because the write-back flush above it is one of the three writers.
bool assignPlace(Gen& g, const Place& place, TypePtr type, JsPtr<Expr> value);

// Everything a value needs to be usable later: a name, a `var`, and an entry in the map. A unit
// result is a statement rather than a binding, since there is nothing to name.
void define(Gen& g, ModulePtr<Value> pointer, JsPtr<Expr> value) {
    auto& source = *g.local[pointer];

    if(isUnit(g.global, source.type)) {
        // Only what has an effect survives. A unit-typed instruction whose JS form is a value - a
        // fresh object for an allocation nobody reads - is nothing at all, and an object literal in
        // statement position parses as a block rather than as an expression anyway.
        auto kind = g.base[value]->kind;
        if(kind == Expr::Call || kind == Expr::Assign) emitExpr(g, value);
        return;
    }

    g.values.add(U32(pointer), declare(g, valueName(g, source), value));
}

/*
 * Putting a value where it was not - Design-Memory §4.1's two relocations.
 *
 * Which of them applies was settled in the resolver: a TrivialSink value *is* its bytes, and on
 * this target that means the reference moves and nothing else happens - the source is statically
 * dead, so there is nothing to invalidate and nothing to copy. Anything else relocates by the call
 * `InstMove::sink` names, and that call is an authored effect: a type whose `Sink` counts its own
 * relocations has to see every one of them, on every target.
 *
 * `value` is the resolve value being written rather than its type, and deliberately: only a *move*
 * relocates. Initializing storage from a copy or from a call result writes bytes that belong to
 * nobody else, and running a `Sink` there would empty a source that has none.
 */
/*
 * The relocation a body that cannot see the type performs - resolve/lower.cpp's erased half of
 * relocateWith, and the same three lines for the same reason.
 *
 * Which of the two relocations applies is exactly what the body has no way to decide: the resolver
 * left `sink` null because there was no concrete type to find one for. So the answer travels in the
 * descriptor the caller passed, and this is the cell read and the call that reads it.
 *
 * Unconditional, like the erased teardown: a TrivialSink type's `moveInit` is a real function that
 * copies its value rather than a null slot, so there is nothing to test before calling. On this
 * target that function is the property-by-property copy genBlockCopy emits, which is why relaxing
 * blockCopyShape to shapes that are not objects was the other half of closing this.
 */
bool erasedRelocate(Gen& g, JsPtr<Expr> target, JsPtr<Expr> source, TypePtr type) {
    if(!g.genEnv || !isGeneric(g.global, type)) return false;

    auto descriptor = genTypeDesc(g, type);
    if(!descriptor) return false;

    emitExpr(g, call(g, tableCell(g, descriptor, TypeDescFields::kMoveInit),
                     referenceTo(g, type, target), referenceTo(g, type, source)));
    return true;
}

/*
 * Whether a whole-value write into this place is this frame rebinding its own name for the value.
 *
 * It is, for a local this function allocated: the variable *is* the storage, so writing the whole of
 * it is an assignment to the variable. It is not for a `&` parameter or for a place rooted in a
 * borrow - those name storage somebody else owns, and reaching it means writing through the
 * reference rather than replacing it.
 *
 * Only asked where the shape is unknown. Where it is known, writing through the reference is the
 * property-by-property copy every other path here emits.
 */
bool rebindsOwnStorage(Gen& g, const Place& place) {
    auto projections = place.projections;
    if(place.root != PlaceRoot::Local || projections.isNotEmpty()) return false;
    if(place.local >= g.function->localCount()) return false;

    return !g.function->localAt(g.local, place.local).borrowed;
}

/*
 * Whether writing this place writes *through* a reference rather than rebinding a name.
 *
 * A place with a path always writes through something - `p.x = v` is a property assignment whichever
 * target it is on. A place with *no* path is the question: for a local it is the emitted `var`, and
 * assigning to it is what an assignment means; for a borrow, a raw pointer, or the slot behind a `&`
 * parameter, the emitted name holds someone else's storage and rebinding it changes nothing anybody
 * else can see.
 *
 * Only object-shaped values need the distinction, which is why it is asked next to `isJsObject`: a
 * scalar reached through a reference is the box or the triple, and both of those already name a slot.
 */
bool writesThroughReference(Gen& g, const Place& place) {
    auto projections = place.projections;
    if(projections.isNotEmpty()) return false;
    if(place.root == PlaceRoot::Borrow || place.root == PlaceRoot::Pointer) return true;
    if(place.root != PlaceRoot::Local || place.local >= g.function->localCount()) return false;

    return g.function->localAt(g.local, place.local).borrowed;
}

/*
 * A field of a type this body cannot see - Implementation-Generics.md part 5's `PropertyWitness`.
 *
 * The one projection this target cannot walk into a property chain, because there is no property to
 * name: where the field is was decided by whoever built the environment, and this body was emitted
 * once for all of them. So it is intercepted before the place walk, exactly as it is on native, and
 * the two loads and the call are the same three steps - a witness is an array here and a table of
 * addresses there, which is the whole of the difference.
 *
 * maxLimit when the place does not end in one, which is every place in a program with no field
 * constraint in it.
 */
U16 propertySlotOf(Gen& g, const Place& place) {
    auto projections = place.projections;
    auto count = projections.size();
    if(!count) return maxLimit<U16>;

    auto last = projections.get(g.local, count - 1);
    return last.kind == ProjectionKind::Property ? last.index : maxLimit<U16>;
}

/*
 * The owner a property is read out of, which is the same place with its last step left off.
 *
 * A whole local arrives as the variable holding it and a projected one as the chain, and either is
 * what the witness takes: its accessors were generated against a concrete owner, so what they expect
 * is a reference to one, which on this target is the object itself.
 */
JsPtr<Expr> propertyOwner(Gen& g, const Place& place) {
    auto projections = place.projections;
    return referenceTo(g, place, projections.size() - 1);
}

/*
 * Storage for one value of a type whose shape this body may not know.
 *
 * The accessors write through a reference, so what they are handed has to be one: a box for
 * something that is not an object here, and the object itself for something that is. A type variable
 * takes the box, because the erased boundary is where a `number` gets one anyway - see genGenCall.
 */
JsPtr<Expr> propertyStorage(Gen& g, TypePtr type, Name name) {
    auto known = type && !isGeneric(g.global, type) && isJsObject(g, type);
    return declare(g, name, known ? zeroValue(g, type) : boxOf(g, zeroValue(g, type)));
}

// What such storage holds, once the accessor has written into it.
JsPtr<Expr> propertyContents(Gen& g, TypePtr type, JsPtr<Expr> storage) {
    auto known = type && !isGeneric(g.global, type) && isJsObject(g, type);
    return known ? storage : field(g, storage, g.boxField);
}

/*
 * Writing a computed value into a place.
 *
 * The inverse of `placeExpr`, and separate from it for the reason part 1 names: a place into a
 * scalarized record is a bit range rather than a location, so it is an expression on the way out and
 * a read-modify-write of the whole binding on the way in. Every writer goes through here so that the
 * three of them cannot disagree about which places are locations.
 *
 * Nothing that reaches the bit path needs a drop or a sink: a value narrow enough to be a bit range
 * is trivially copyable by construction.
 */
bool assignPlace(Gen& g, const Place& place, TypePtr type, JsPtr<Expr> value) {
    PlaceBits bits;
    auto owner = placeOwner(g, place, bits);

    /*
     * A folded tag, which is a store of one pattern or nothing at all.
     *
     * The constructor is always a literal: a record is constructed by naming one, and
     * `place.discriminant = <computed>` is not something any front end can write. Asserted rather than
     * fallen back from, for the same reason the native lowering asserts it - a runtime encode would be
     * dead code that nothing could ever exercise or test.
     */
    if(bits.foldedTag) {
        auto& written = *g.base[value];
        assertTrue(written.kind == Expr::Number);

        encodeNicheTag(g, owner, bits.foldedTag, U64(((NumberExpr&)written).value));
        return true;
    }

    if(!bits.valid()) return false;

    emitExpr(g, assign(g, owner, encodeBits(g, owner, bits, type, value)));
    return true;
}

/*
 * Writing a field that is a property the host value already has - `arr.length`, see isHostProperty.
 *
 * Guarded rather than written straight, and the guard is not a heuristic: assigning an array's
 * `length` the number it already holds is a no-op by definition, so testing first can only skip a
 * store that would have done nothing. What makes it worth emitting is that the host's `length` is
 * not an ordinary property - the setter has to consider truncating, and it pays for that whether or
 * not the value changed.
 *
 * **Measured, and it is the difference between the elision paying and costing.** A build-and-read
 * loop over a 200k `Array(Node)` under Node 24: 160 ms with the count stored in a wrapper object,
 * 290 ms with the count elided and written straight, 115 ms with it elided and written through this.
 * The whole of the regression was one statement per `push`, since appending at the array's own
 * length has already moved it and the store that follows is writing a number that is already there.
 *
 * It is *this* rather than eliding the store in the container's source for one reason: the IR's
 * `length` field is what every pass above this one reads, so a `push` that stopped writing it would
 * leave a value the store-to-load forwarding could hand back after the array had grown past it. The
 * write stays in the IR and stops costing anything here, which keeps the two models the same.
 */
void storeHostProperty(Gen& g, JsPtr<Expr> target, JsPtr<Expr> value) {
    emit(g, make<IfStmt>(g, binary(g, BinaryOp::Ne, target, value), collect(g, [&] {
        emitExpr(g, assign(g, target, value));
    }), StmtList {}));
}

void storeInto(Gen& g, const Place& place, TypePtr type, ModulePtr<Value> value) {
    auto& produced = *g.local[value];

    /*
     * A whole function value written into storage this body holds as two variables.
     *
     * Two assignments rather than one, because there is no single slot to name: `placeExpr` answers
     * a whole-value place of that kind with a freshly built `{$c, $e}`, which is the right answer
     * for every *read* of one and is an object literal on the left of an assignment here. The
     * source is taken apart rather than built, so nothing is allocated on either side.
     *
     * Reached by a niche-folded `Maybe((Int) -> Int)` binding its payload, which is the shortest
     * shape that writes a function value into a local rather than constructing one there.
     */
    if(auto destination = destinationFunParts(g, place)) {
        auto parts = destination.unwrap();
        auto from = funPartsOf(g, value);

        emitExpr(g, assign(g, parts.code, from.code));
        emitExpr(g, assign(g, parts.env, from.env));
        return;
    }

    /*
     * A reference written into storage this body holds as that reference's parts, on exactly the
     * terms above: there is no single slot to name, so the source is taken apart rather than built
     * and nothing is allocated on either side.
     *
     * The source may be the `null` of an absent constructor rather than a reference - the `Nothing`
     * arm of the `Maybe(&v)` this exists for - and that arrives here as a write of the *folded tag*
     * instead, which `assignPlace` has already taken. What reaches this is a payload or a whole
     * value, both of which are a reference.
     */
    if(auto destination = destinationRefParts(g, place)) {
        auto parts = destination.unwrap();
        auto from = refPartsOf(g, value);

        emitExpr(g, assign(g, parts.owner, from.owner));

        /*
         * And the key and the shift, where both sides have them. A source holding nothing but the
         * absent constructor has neither - see Gen::flatRefTagOnly - so what it copies is the owner
         * alone, which is sound on the invariant the flattening rests on: the owner this write does
         * copy is `null`, and neither of the other two is ever read where the owner is null.
         */
        if(parts.key && from.key) emitExpr(g, assign(g, parts.key, from.key));
        if(parts.scale && from.scale) emitExpr(g, assign(g, parts.scale, from.scale));

        return;
    }

    if(assignPlace(g, place, type, useValue(g, value))) return;

    auto elided = false;
    auto target = placeTarget(g, place, elided);

    auto moved = produced.kind == Value::Move;
    ModulePtr<Function> sink = moved ? ((InstMove&)produced).sink : nullptr;

    if(!sink) {
        if(moved && erasedRelocate(g, target, useValue(g, value), type)) return;

        /*
         * The erased write that is not a relocation - Analysis-JS.md §3.4's remaining half, and
         * what `README.md` gap 4 used to report rather than emit.
         *
         * Native block-copies the descriptor's `size` bytes, which is a shallow copy of storage
         * whose shape it does not need. This target has no shallow copy of one whole value: a
         * nested aggregate is a separate object here and inline bytes there, so copying property by
         * property is the only form, and which properties there are is exactly what is unknown.
         *
         * So it is the descriptor that answers, exactly as it answers the relocation next door -
         * `copyInit` rather than `moveInit`, because this duplicates a value the source keeps.
         * What is in that slot is one generated function per concrete type, compiled by whichever
         * backend is emitting: a `memcpy` there, genBlockCopy's structural duplicate here.
         */
        if(g.genEnv && isGeneric(g.global, type) && !rebindsOwnStorage(g, place)) {
            if(auto descriptor = genTypeDesc(g, type)) {
                emitExpr(g, call(g, tableCell(g, descriptor, TypeDescFields::kCopyInit),
                                 referenceTo(g, type, target), referenceTo(g, type, useValue(g, value))));
                return;
            }

            g.context.diagnostics.error("the JS target cannot write a value of a type it has no descriptor for"_v,
                                        produced.source);
            return;
        }

        /*
         * A whole aggregate written through a reference is a copy *into* the storage the reference
         * names - which is what the native side's memcpy is, and what `v = other` here is not.
         *
         * The case is `xs[i] = value` where the element is a record: `getMut` hands back the element
         * object itself, because a reference to an object is the object on this target, and
         * rebinding the emitted name would leave the array holding the value that was there. It
         * cannot arise for a scalar, whose reference is the box or the triple - both of those name a
         * slot already - which is why this asks `isJsObject` first.
         *
         * Property by property, and whether each property is duplicated is `keepsLiveStorage`'s
         * question rather than a property of the case: where the value moved, the source is dead
         * afterwards and a nested aggregate has nobody left to alias with; where it is still
         * somebody's storage, this is `genBlockCopy` with the shape known and clones for the reason
         * that one does.
         */
        auto written = useValue(g, value);
        auto keeps = keepsLiveStorage(g, type, value);

        if(isJsObject(g, type) && writesThroughReference(g, place)) {
            auto source = g.base[written]->kind == Expr::Var
                ? written
                : declare(g, generatedName(g, "moved"_v, produced.id), written);

            eachProperty(g, type, [&](Name key, TypePtr member) {
                auto read = field(g, source, key);
                emitExpr(g, assign(g, field(g, target, key),
                                   keeps ? cloneValue(g, member, read, produced.source) : read));
            });

            return;
        }

        // And the whole value where the write is one assignment. `cloneValue` rather than the
        // property walk above, because the storage this names is the binding itself: there is no
        // object already there to write the properties of.
        if(keeps) written = cloneValue(g, type, written, produced.source);

        if(elided) {
            storeHostProperty(g, target, written);
            return;
        }

        emitExpr(g, assign(g, target, written));
        return;
    }

    emitExpr(g, call(g, functionValue(g, sink, produced.source),
                     referenceTo(g, type, target),
                     referenceTo(g, type, useValue(g, value))));
}

// The same, for the two instructions that are relocations by construction and carry the callee
// themselves rather than on a value that has to be recognized as a move first.
void relocateWith(Gen& g, JsPtr<Expr> target, JsPtr<Expr> source, TypePtr type,
                  ModulePtr<Function> sink, LocationId where) {
    if(!sink) {
        if(erasedRelocate(g, target, source, type)) return;

        emitExpr(g, assign(g, target, source));
        return;
    }

    emitExpr(g, call(g, functionValue(g, sink, where),
                     referenceTo(g, type, target), referenceTo(g, type, source)));
}

/*
 * A call to `$div` or `$rem`, naming the helper on its first use - see Gen::divideByZeroHelper.
 *
 * Pure, and that is not a formality: the whole point of the ruling these implement is that an
 * integer division computes a value and does nothing else, so the emitter is free to move one.
 */
static JsPtr<Expr> divisionHelperCall(Gen& g, Name& helper, StringView helperName,
                                      JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    if(!helper.text) helper = uniqueName(g, helperName, false);

    auto node = make<CallExpr>(g, variable(g, helper));
    node->args.push(g.file.arena, lhs);
    node->args.push(g.file.arena, rhs);
    node->pure = true;
    return asExpr(g, node);
}

static JsPtr<Expr> divideCall(Gen& g, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    return divisionHelperCall(g, g.divideByZeroHelper, "$div"_v, lhs, rhs);
}

static JsPtr<Expr> remainderCall(Gen& g, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    return divisionHelperCall(g, g.remainderByZeroHelper, "$rem"_v, lhs, rhs);
}

static JsPtr<Expr> bitOpCall(Gen& g, Value::Kind kind, TypePtr type, JsPtr<Expr> lhs, JsPtr<Expr> rhs);

void genBinary(Gen& g, ModulePtr<Value> pointer, InstBinary& instruction) {
    auto lhs = useValue(g, instruction.lhs);
    auto rhs = useValue(g, instruction.rhs);
    auto type = instruction.type;
    auto integer = intType(g, type);

    /*
     * Bool is 0 or 1, so its three bitwise operations are the bitwise ones and need no special case
     * at all beyond skipping the coercion below - the operands are already in range and the results
     * cannot leave it. This used to be `&&`/`||`/`!==` over host booleans; the operators here are
     * branchless, which is one of the reasons the number measured better.
     */
    if(isBool(g, type)) {
        switch(instruction.kind) {
            case Value::And: define(g, pointer, binary(g, BinaryOp::And, lhs, rhs)); return;
            case Value::Or: define(g, pointer, binary(g, BinaryOp::Or, lhs, rhs)); return;
            case Value::Xor: define(g, pointer, binary(g, BinaryOp::Xor, lhs, rhs)); return;
            default: break;
        }
    }

    auto simple = [&](BinaryOp op) {
        define(g, pointer, coerce(g, type, binary(g, op, lhs, rhs)));
    };

    /*
     * The 33-to-53-bit band, where the host has the representation but not the operators.
     *
     * Each of these goes to a helper rather than to an inline expansion, because the split form of
     * one bitwise operator is eight operations and a program using such a type uses it everywhere.
     * `opt.cpp` fuses chains of them back into one inline unpack-operate-pack, which is where the
     * measured win is - so the compact form is the default and the expansion is earned.
     *
     * Comparison, and only comparison, is missing from this list on purpose: two of these are
     * ordinary `number`s carrying their mathematical values, so `<` already means what it says.
     * That is the whole reason this representation is worth having.
     */
    if(isWideNumber(g, type)) {
        auto wide = [&](WideOp op) { define(g, pointer, wideCall(g, op, integer, lhs, rhs)); };

        switch(instruction.kind) {
            case Value::Add: wide(WideOp::Add); return;
            case Value::Sub: wide(WideOp::Sub); return;
            case Value::Mul: wide(WideOp::Mul); return;
            case Value::And: wide(WideOp::And); return;
            case Value::Or: wide(WideOp::Or); return;
            case Value::Xor: wide(WideOp::Xor); return;
            case Value::Shl: wide(WideOp::Shl); return;
            case Value::Shr: wide(WideOp::Shr); return;
            case Value::Sar: wide(WideOp::Sar); return;

            /*
             * Division and remainder need no helper and no coercion. Both operands are already in
             * range, `Math.trunc` is the truncation toward zero the language asks for, and a
             * quotient of two in-range values is in range - except for the one signed pair whose
             * quotient is the missing positive, which `Wrap` catches.
             */
            case Value::Div: {
                // Through `$div`, because a zero divisor is the one pair `Wrap` cannot rescue: the
                // quotient it is handed is an infinity, and `Infinity % 2^52` is NaN rather than the
                // 0 the language defines. See Gen::divideByZeroHelper.
                auto quotient = hostCall(g, "Math"_v, "trunc"_v, divideCall(g, lhs, rhs));
                define(g, pointer, integer->isSigned
                    ? wideCall(g, WideOp::Wrap, integer, quotient, nullptr) : quotient);
                return;
            }
            case Value::Rem:
                // And through `$rem`, whose zero arm is `a` - already in range, which is why this
                // band's remainder needs no coercion here any more than it did before.
                define(g, pointer, remainderCall(g, lhs, rhs));
                return;
            default: break;
        }
    }

    switch(instruction.kind) {
        case Value::Add: simple(BinaryOp::Add); return;
        case Value::Sub: simple(BinaryOp::Sub); return;
        case Value::Mul:
            /*
             * `Math.imul` is the only correct 32-bit multiply JS has: `a * b` is a double multiply,
             * and the low 32 bits of the true product are not the low 32 bits of the rounded one
             * once the product passes 2^53. §2.1 calls this out as the one coercion that is
             * unconditional.
             */
            if(isInt32Class(g, integer)) {
                define(g, pointer, coerce(g, type, hostCall(g, "Math"_v, "imul"_v, lhs, rhs)));
                return;
            }

            simple(BinaryOp::Mul);
            return;
        case Value::Div:
            // Integer division truncates toward zero, and the coercion the type needs anyway is what
            // does it: every integer form coerce() emits is a bitwise operator, which is defined on
            // the truncated value. BigInt division truncates on its own and a float must not, so
            // the same statement is right for all three.
            //
            // A `Long` divides as a `bigint`, and `0n` is the one divisor BigInt division *throws*
            // on rather than answering, so that width alone goes through `$div`. Every narrower
            // integer already produces the defined answer for nothing, the coercion above turning
            // the infinity `a / 0` yields into the 0 the language asks for.
            if(isInt64Class(g, integer)) {
                define(g, pointer, coerce(g, type, divideCall(g, lhs, rhs)));
                return;
            }

            simple(BinaryOp::Div);
            return;
        case Value::Rem:
            // The remainder gets no such reprieve at any width: `a % 0` is NaN for a number and a
            // throw for a `bigint`, and the answer is `a`. A float never reaches here - `%` is
            // `Integral`'s - but the test is written so that one would be left alone if it did.
            if(integer) {
                define(g, pointer, coerce(g, type, remainderCall(g, lhs, rhs)));
                return;
            }

            simple(BinaryOp::Rem);
            return;
        case Value::Shl: simple(BinaryOp::Shl); return;
        case Value::Shr:
            // A logical right shift of a `Long` has to go through the unsigned reading first:
            // `>>>` is not defined on BigInt at all.
            if(isInt64Class(g, integer)) {
                auto unsignedValue = hostCall(g, "BigInt"_v, "asUintN"_v, number(g, 64), lhs);
                define(g, pointer, coerce(g, type, binary(g, BinaryOp::Sar, unsignedValue, rhs)));
                return;
            }

            simple(BinaryOp::Shr);
            return;
        case Value::Sar: simple(BinaryOp::Sar); return;
        case Value::And: simple(BinaryOp::And); return;
        case Value::Or: simple(BinaryOp::Or); return;
        case Value::Xor: simple(BinaryOp::Xor); return;

        // The three BMI2 operations. Coerced like every other arithmetic result: the helper answers
        // the raw pattern at 32 or 64 bits and the type's normal form is applied here.
        case Value::BitsUpTo:
        case Value::GatherBits:
        case Value::ScatterBits:
            define(g, pointer, coerce(g, type, bitOpCall(g, instruction.kind, type, lhs, rhs)));
            return;

        default:
            g.context.diagnostics.error("internal error: unexpected binary instruction in JS codegen"_v,
                                        instruction.source);
            return;
    }
}

void genCast(Gen& g, ModulePtr<Value> pointer, InstUnary& instruction);

/*
 * The scratch pair a `Float`/`I32` bitcast goes through, and the two helpers that use it.
 *
 * A `Float` on this target is a `number` that `Math.fround` has made exactly representable as a
 * binary32, so its thirty-two bits are perfectly well defined and completely unreachable: no host
 * operator sees them. Two views over one `ArrayBuffer` is the only way, and one buffer per program
 * is enough, because the write and the read after it are the whole of the operation and nothing
 * runs between them.
 *
 * A helper *function* and not two statements at the call site, and that is a correctness matter
 * rather than a size one: the read of `$bci[0]` is not pure, so a binding holding one would be
 * eligible for the one-use inlining in opt.cpp and could be carried across the next bitcast's
 * write. An ordinary call is the form this backend already cannot move.
 */
static void ensureFloatBits(Gen& g, bool wide) {
    if(wide) {
        if(g.doubleBitsBuffer.text) return;

        g.doubleBitsBuffer = uniqueName(g, "$bcd"_v, false);
        g.doubleBitsInts = uniqueName(g, "$bcl"_v, false);
        return;
    }

    if(g.floatBitsBuffer.text) return;

    g.floatBitsBuffer = uniqueName(g, "$bcf"_v, false);
    g.floatBitsInts = uniqueName(g, "$bci"_v, false);
}

/*
 * A host array holding `elements`, in whichever of §14's two rows this element type belongs to.
 *
 * `[a, b, c]` for an element the host boxes, and `new Int32Array([a, b, c])` for one it does not -
 * which is the whole of the difference between the two rows, and the reason it is one function is
 * that both producers of a host array (the empty one and the literal) have to make the same choice.
 * `typedArrayFor` is the choice; nothing here decides anything.
 *
 * `type` is the array *reference*, so the element is its pointee - the same question the resolver
 * asked when it folded `hostFixedCapacity`, asked of the same type.
 */
// The same, asked of an array *reference* - the element is its pointee, which is the question the
// resolver asked when it folded `hostFixedCapacity`. The rule itself is at js scope below, because
// zeroValue and the constant builder in type.cpp are the other two readers of it.
static JsPtr<Expr> hostArrayOf(Gen& g, TypePtr type, JsPtr<Expr> elements) {
    return hostArrayForElement(g, pointeeType(g.global, type), elements);
}

static JsPtr<Expr> construct(Gen& g, StringView type, JsPtr<Expr> argument) {
    auto node = make<CallExpr>(g, variable(g, literalName(g, type)));
    node->args.push(g.file.arena, argument);
    node->construct = true;
    return asExpr(g, node);
}

// `toBits` says which direction - a float in and its bits out, or the reverse - and `wide` says at
// which width, the 64-bit pair answering in `bigint` where the 32-bit one answers in `number`.
static Name floatBitsHelper(Gen& g, bool toBits, bool wide) {
    auto& into = wide ? (toBits ? g.doubleToBitsHelper : g.bitsToDoubleHelper)
                      : (toBits ? g.floatToBitsHelper : g.bitsToFloatHelper);
    if(into.text) return into;

    ensureFloatBits(g, wide);
    into = uniqueName(g, wide ? (toBits ? "$bc$d2l"_v : "$bc$l2d"_v)
                              : (toBits ? "$bc$f2i"_v : "$bc$i2f"_v), false);
    return into;
}


/*
 * One of the four roundings, as an expression - the scalar path and one lane of the vector path.
 *
 * Three of them are the host function of the same name and are exact at every input: ECMA-262
 * specifies `Math.trunc`, `Math.floor` and `Math.ceil` outright rather than leaving them
 * implementation-approximated the way it leaves `log2` and `pow`, so there is no engine variation
 * to design around and no agreement to check against the native targets.
 *
 * `Round` is the exception and the helper is why - see Gen::roundAwayHelper. `Math.round` rounds a
 * half toward positive infinity and this language rounds one away from zero, so they differ at
 * every negative half and nowhere else.
 */
static JsPtr<Expr> roundingLane(Gen& g, Value::Kind kind, JsPtr<Expr> value) {
    if(kind == Value::Round) {
        if(!g.roundAwayHelper.text) g.roundAwayHelper = uniqueName(g, "$round"_v, false);

        auto node = make<CallExpr>(g, variable(g, g.roundAwayHelper));
        node->args.push(g.file.arena, value);
        node->pure = true;
        return asExpr(g, node);
    }

    auto name = kind == Value::Trunc ? "trunc"_v : kind == Value::Floor ? "floor"_v : "ceil"_v;
    return hostCall(g, "Math"_v, name, value);
}

/*
 * The byte reversal, which is a call to one of three helpers - see Gen::byteSwapHelpers.
 *
 * The width comes from the *type* rather than from the value's host representation, and the two do
 * not agree: a 16-bit swap and a 32-bit one are both `number` arithmetic here, and reversing four
 * bytes of a value that has two is a different answer rather than a slower one. The resolve verifier
 * has already refused every width that is not 16, 32 or 64, so there is no fourth case to answer.
 *
 * The helper answers the raw reversal and the caller coerces, which is what lets one function serve
 * both signednesses of a width: `>>> 0`, `| 0`, `& 0xffff` and `BigInt.asIntN` are the type's own
 * normal form, and applying it is the same statement every other arithmetic instruction here makes.
 */
static JsPtr<Expr> byteSwapCall(Gen& g, TypePtr type, JsPtr<Expr> value) {
    auto bits = heldBits(g, *(IntType*)g.global[type]);
    auto slot = bits == 16 ? 0 : bits == 32 ? 1 : 2;

    if(!g.byteSwapHelpers[slot].text) {
        auto name = bits == 16 ? "$swap16"_v : bits == 32 ? "$swap32"_v : "$swap64"_v;
        g.byteSwapHelpers[slot] = uniqueName(g, name, false);
    }

    auto node = make<CallExpr>(g, variable(g, g.byteSwapHelpers[slot]));
    node->args.push(g.file.arena, value);
    node->pure = true;
    return asExpr(g, node);
}

/*
 * The three bit counts, which are a call to one of five helpers - or, in the one case the host
 * answers directly, to `Math.clz32`.
 *
 * The width comes from the type and decides the domain as much as the arithmetic: 32 bits is a
 * `number` and 64 is a `bigint`, and the resolve verifier has already refused every other width, so
 * there is no third case. The answer is at the operand's own type, which is what `coerce` at the
 * call site puts back - a `U64`'s count is a `bigint` even though it is at most 64, because that is
 * what its type is here.
 *
 * `leadingZeros` at 32 bits is `Math.clz32`, whose argument goes through ToUint32 - so a negative
 * `I32` counts the bits of its two's complement pattern, which is the same value the machine
 * counts. That is the whole of the 32-bit case and is why it has no helper.
 */
static JsPtr<Expr> bitCountCall(Gen& g, Value::Kind kind, TypePtr type, JsPtr<Expr> value) {
    auto wide = heldBits(g, *(IntType*)g.global[type]) == 64;

    if(kind == Value::LeadingZeros && !wide) return hostCall(g, "Math"_v, "clz32"_v, value);

    auto want = [&](Value::Kind of, bool at) {
        auto slot = bitCountHelperSlot(of, at);
        if(g.bitCountHelpers[slot].text) return;

        auto name = of == Value::CountBits    ? (at ? "$popcount64"_v : "$popcount32"_v)
                  : of == Value::LeadingZeros ? "$clz64"_v
                                              : (at ? "$ctz64"_v : "$ctz32"_v);

        g.bitCountHelpers[slot] = uniqueName(g, name, false);
    };

    want(kind, wide);

    // The 64-bit population and trailing counts are written over their operand's two halves and
    // call the 32-bit helper on each, so asking for one asks for the other. The leading count needs
    // no such partner: its halves go to `Math.clz32`, which is the host's own.
    if(wide && kind != Value::LeadingZeros) want(kind, false);

    auto slot = bitCountHelperSlot(kind, wide);

    auto node = make<CallExpr>(g, variable(g, g.bitCountHelpers[slot]));
    node->args.push(g.file.arena, value);
    node->pure = true;
    return asExpr(g, node);
}

/*
 * A rotation, which is a call to one of ten helpers - see Gen::rotateHelpers.
 *
 * The width comes from the *type* and decides both the modulus and the host domain the body works
 * in. The helper answers the raw rotation at that width and the caller coerces, which is what lets
 * one function serve both signednesses - `>>> 0`, `| 0`, `& 0xffff` and `BigInt.asIntN` are the
 * type's own normal form, and applying it is the same statement every arithmetic instruction here
 * makes.
 *
 * Serves a lane as well as a scalar: `laneBinary` calls this with the *lane* type, and a lane of a
 * `Vec(U8)` is an eight-bit rotation for the same reason a `U8` is.
 */
static JsPtr<Expr> rotateCall(Gen& g, Value::Kind kind, TypePtr type, JsPtr<Expr> value, JsPtr<Expr> count) {
    auto integer = intType(g, type);
    auto bits = integer ? U32(heldBits(g, *(IntType*)g.global[canonicalType(g.global, type)])) : 0;
    auto slot = rotateHelperSlot(kind, bits);

    if(slot == kNoRotateHelper) {
        g.context.diagnostics.error("internal error: a rotation at a width with no instance"_v, LocationId());
        return value;
    }

    if(!g.rotateHelpers[slot].text) {
        char buffer[16];
        Size length = 0;
        buffer[length++] = '$';
        buffer[length++] = 'r';
        buffer[length++] = 'o';
        buffer[length++] = kind == Value::Rol ? 'l' : 'r';
        length += show(U64(bits), buffer + length, sizeof(buffer) - length);

        g.rotateHelpers[slot] = uniqueName(g, StringView { buffer, length }, false);
    }

    auto node = make<CallExpr>(g, variable(g, g.rotateHelpers[slot]));
    node->args.push(g.file.arena, value);
    node->args.push(g.file.arena, count);
    node->pure = true;
    return asExpr(g, node);
}

/*
 * The three BMI2 operations, each a call to one of six helpers - see Gen::bitOpHelpers.
 *
 * `rotateCall`'s shape at a smaller width set. `Core.Bits` is declared at 32 and 64 bits only, so
 * there are two domains rather than the rotations' three and no width with no slot: whichever of the
 * two the type is, the helper answers the raw operation there and the caller's coercion takes it to
 * the type's own normal form.
 */
static JsPtr<Expr> bitOpCall(Gen& g, Value::Kind kind, TypePtr type, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    auto integer = intType(g, type);
    auto bits = integer ? U32(heldBits(g, *(IntType*)g.global[canonicalType(g.global, type)])) : 32;
    auto wide = bits == 64;
    auto slot = bitOpHelperSlot(kind, wide);

    if(!g.bitOpHelpers[slot].text) {
        auto stem = kind == Value::BitsUpTo ? "$bzhi"_v : kind == Value::GatherBits ? "$pext"_v : "$pdep"_v;

        char buffer[16];
        Size length = 0;
        for(Size i = 0; i < stem.length; i++) buffer[length++] = stem.ptr[i];
        length += show(U64(wide ? 64 : 32), buffer + length, sizeof(buffer) - length);

        g.bitOpHelpers[slot] = uniqueName(g, StringView { buffer, length }, false);
    }

    auto node = make<CallExpr>(g, variable(g, g.bitOpHelpers[slot]));
    node->args.push(g.file.arena, lhs);
    node->args.push(g.file.arena, rhs);
    node->pure = true;
    return asExpr(g, node);
}

/*
 * One value's bits read as another type of the same width.
 *
 * Two shapes. Two integer types of one width are the same host value in two normal forms, so the
 * ordinary coercion is exactly the reinterpretation - `Int` to `U32` is `>>> 0`, which is what the
 * two's complement pattern of a negative number *is* read as unsigned. What is left is a float
 * meeting an integer, which no operator here expresses and the scratch typed-array pair does.
 *
 * **The sixty-four bit pair arrives only through a vector.** `defineBitcastLadder` still declines to
 * relate a scalar `Double` to a scalar `I64` on this target, and the scalar library has
 * `@platform(js)` bodies that need no such thing. A *vector* bitcast is a different question and is
 * not generated by that ladder at all - it is decided by byte width - so `Vec(F64)` to `Vec(Long)`
 * exists and `Real(Vec(F64))` is written on it. Hence two scratch pairs, chosen by the lane's
 * width: the wide one is a `Float64Array` over a `BigInt64Array` and answers a `bigint`.
 *
 * Its own function because a *vector* bitcast is this per lane - see genVecConvert, which is the
 * second caller and the reason the rule is written once.
 */
static JsPtr<Expr> bitcastLane(Gen& g, TypePtr from, TypePtr to, JsPtr<Expr> value) {
    auto fromFloat = isFloat(g.global, from);
    if(fromFloat == isFloat(g.global, to)) return coerce(g, to, value);

    // The width is read off whichever side is the float, the two being equal by construction: a
    // `Bitcast` relates two types of one size and nothing else.
    auto floatSide = fromFloat ? from : to;
    auto wide = ((FloatType*)g.global[floatSide])->width != FloatType::Float;
    auto helper = floatBitsHelper(g, fromFloat, wide);
    auto node = make<CallExpr>(g, variable(g, helper));
    node->args.push(g.file.arena, value);

    // Deliberately not `pure`: the buffer it writes is shared, so two of these may not be reordered
    // against each other.
    return coerce(g, to, asExpr(g, node));
}

// `bitcast(x)` - the same bits under another type. A reference reinterpreted is the same host
// object, and everything else is one lane's worth of the rule above.
void genBitcast(Gen& g, ModulePtr<Value> pointer, InstUnary& instruction) {
    auto from = g.local[instruction.from]->type;
    auto to = instruction.type;

    if(isPointer(g.global, from) || isPointer(g.global, to)) {
        // A pointer constant is the one case with anything to say - the IR has no pointer immediate,
        // so `null()` arrives here as a zero being reinterpreted. genCast argues the rest.
        genCast(g, pointer, instruction);
        return;
    }

    define(g, pointer, bitcastLane(g, from, to, useValue(g, instruction.from)));
}

/*
 * A float truncated into an integer, saturating - see `saturationRange`, which is where the rule is
 * argued and where the two bounds come from.
 *
 * `Math.min(Math.max(trunc(v), lo), hi)` and nothing else, which is short for two reasons that only
 * hold on a target whose numbers are doubles. Every bound of a type that fits in a `number` is
 * exactly representable, so clamping in the float domain lands on the integer the rule asks for
 * rather than near it; and `Math.min`/`Math.max` *propagate* NaN rather than ordering it, so NaN
 * reaches the coercion untouched and `NaN | 0` is the zero the rule asks for. Both are why `v` is
 * read once and no temporary is needed.
 */
static JsPtr<Expr> saturatingToNumber(Gen& g, TypePtr to, JsPtr<Expr> value) {
    auto truncated = hostCall(g, "Math"_v, "trunc"_v, value);

    auto integer = intType(g, to);
    if(!integer) return truncated;

    auto range = saturationRange(*integer);
    if(!range.highIsExact) return truncated;

    return hostCall(g, "Math"_v, "min"_v,
                    hostCall(g, "Math"_v, "max"_v, truncated, number(g, range.low, false)),
                    number(g, range.high, false));
}

/*
 * The same into a `bigint`, which needs a helper where the number case needed none.
 *
 * Neither shortcut above survives at sixty-four bits. `2^63 - 1` is not a double, so a clamp in the
 * float domain would answer `2^63 - 1024` for everything past the end; and `BigInt(NaN)` throws
 * rather than producing anything to coerce. So the three cases are written out, once per target
 * type, in a function - which is also what keeps `v` read once.
 */
static Name saturatingLongHelper(Gen& g, IntType& integer) {
    auto held = heldBits(g, integer);
    auto key = (U32(held) << 1) | U32(integer.isSigned);
    if(auto found = g.satHelpers.get(key)) return found.unwrap();

    char buffer[32];
    Size length = 0;
    buffer[length++] = '$';
    buffer[length++] = 's';
    buffer[length++] = 'a';
    buffer[length++] = 't';
    length += show(U64(held), buffer + length, sizeof(buffer) - length);
    buffer[length++] = integer.isSigned ? 'i' : 'u';

    auto name = uniqueName(g, StringView { buffer, length }, false);
    g.satHelpers.add(key, name);
    g.satHelperOrder.push(SatHelper { name, held, integer.isSigned });
    return name;
}

static JsPtr<Expr> saturatingToLong(Gen& g, TypePtr to, JsPtr<Expr> value) {
    auto integer = intType(g, to);
    if(!integer) return coerce(g, to, globalCall(g, "BigInt"_v, hostCall(g, "Math"_v, "trunc"_v, value)));

    auto node = make<CallExpr>(g, variable(g, saturatingLongHelper(g, *integer)));
    node->args.push(g.file.arena, value);
    node->pure = true;

    return asExpr(g, node);
}


void genCast(Gen& g, ModulePtr<Value> pointer, InstUnary& instruction) {
    auto from = g.local[instruction.from]->type;
    auto to = instruction.type;
    auto value = useValue(g, instruction.from);

    auto fromLong = isLong(g, from);
    auto toLong = isLong(g, to);
    auto fromBool = isBool(g, from);
    auto toBool = isBool(g, to);

    /*
     * Two integers this target holds identically, which is a conversion that converts nothing.
     *
     * `Int` to `Size` is the case that made this worth writing: a `Size` here is a signed 32-bit
     * index, so every subscript with an ordinary `Int` index widens into a type that is the same
     * `number` - and emitting the widening as `i | 0` put a coercion in front of every one of them.
     * Natively the same conversion is a real sign-extension, which is why this is the backend's
     * answer rather than a fold in `opt`: the two targets disagree about whether the instruction
     * does anything, and only one of them is entitled to remove it.
     *
     * Both `bits` and the sign have to match. Same width and different signedness is a
     * reinterpretation the program asked for, and `@bits` refinements differ in `bits`, so a
     * conversion to or from one never reaches here.
     */
    auto fromInt = intType(g, from);
    auto toInt = intType(g, to);

    if(fromInt && toInt && !fromBool && !toBool && !fromLong && !toLong &&
       fromInt->isSigned == toInt->isSigned && heldBits(g, *fromInt) == heldBits(g, *toInt)) {
        define(g, pointer, value);
        return;
    }

    // A conversion between two references moves nothing: both are the same object, and what changed
    // is only what the program says is behind it.
    if(isPointer(g.global, from) && isPointer(g.global, to)) {
        define(g, pointer, value);
        return;
    }

    // A `[T *n]` and a `%T` are one host array under two names - see expressibleInJs, which is where
    // the rule is argued. So this moves nothing either.
    if(isPointer(g.global, to) && from && g.global[from]->kind == Type::Array) {
        define(g, pointer, value);
        return;
    }

    /*
     * An address written as an integer, which is what a pointer constant is: the resolve IR has no
     * pointer immediate, so `null()` is a zero reinterpreted. Nothing else can be one, since
     * arithmetic that produces an address is not expressible here at all.
     */
    if(isPointer(g.global, to)) {
        auto& source = *g.local[instruction.from];
        auto bits = source.kind == Value::ConstInt ? ((ConstInt&)source).value : 0;

        define(g, pointer, bits ? number(g, F64(bits)) : nullValue(g));
        return;
    }

    if(fromBool && !toBool) {
        auto one = toLong ? bigInt(g, 1, true) : number(g, 1);
        auto zero = toLong ? bigInt(g, 0, true) : number(g, 0);
        define(g, pointer, ternary(g, value, one, zero));
        return;
    }

    // Through `boolNumber`, because a comparison is a host boolean and a `Bool` is the number 0 or 1
    // - see type.cpp, which is where that costs something and where it does not.
    if(toBool) {
        auto zero = fromLong ? bigInt(g, 0, true) : number(g, 0);
        define(g, pointer, boolNumber(g, binary(g, BinaryOp::Ne, value, zero)));
        return;
    }

    // `Long` is a `bigint` and everything else is a `number`, so crossing between them is a real
    // conversion rather than a widening (§2.1). Truncation toward zero happens on the number side,
    // because `BigInt()` rejects a non-integral double rather than rounding it.
    if(toLong && !fromLong) {
        // And a float source saturates, which on this side of the conversion is not optional in the
        // way it is elsewhere: `BigInt(NaN)` and `BigInt(Infinity)` *throw*, so the unsaturated form
        // was not merely disagreeing with native, it was a crash.
        if(isFloat(g.global, from)) {
            define(g, pointer, saturatingToLong(g, to, value));
            return;
        }

        /*
         * A literal crosses at compile time. `BigInt(0)` is `0n`, and writing the call out left a
         * constant the folds above this could not see through - which is the difference between
         * `Target.byteOrder == Big` costing nothing here and costing a `bigint` compare.
         *
         * An exact integer only, which is every number literal a cast from an integer type can
         * produce: `constantNumber` has already refused a fraction and an infinity, and `BigInt`
         * would have thrown on either.
         */
        F64 literal;
        if(constantNumber(g, value, literal) && isExactInteger(literal)) {
            define(g, pointer, coerce(g, to, bigInt(g, U64(I64(literal)), toInt && toInt->isSigned)));
            return;
        }

        // What the operand's type says about its range, which is the one thing the peephole cannot
        // recover here: an enum's tag arrives as a parameter or a call result. Without it the round
        // trip back out of the `bigint` domain - which is what every `valueOf` on this target is -
        // stays in the emitted text. See foldBigIntRoundTrip.
        noteScalarRange(g, value, from);

        define(g, pointer, coerce(g, to, globalCall(g, "BigInt"_v, value)));
        return;
    }

    if(fromLong && !toLong) {
        auto asNumber = globalCall(g, "Number"_v, value);
        define(g, pointer, isFloat(g.global, to) ? asNumber : coerce(g, to, asNumber));
        return;
    }

    if(isInteger(g.global, to) && isFloat(g.global, from)) {
        define(g, pointer, coerce(g, to, saturatingToNumber(g, to, value)));
        return;
    }

    /*
     * Integer to integer, both of them `number`s here.
     *
     * Widening into the 33-to-53-bit band is asked separately because most of it is free - the two
     * representations are the same host type, so only a negative value entering an unsigned type
     * is a real conversion - and the general coercion for that band is a remainder that would
     * otherwise be emitted on every widening.
     */
    if(isWideNumber(g, to)) {
        if(auto source = intType(g, from)) {
            define(g, pointer, wideFromNarrow(g, intType(g, to), source, value));
            return;
        }
    }

    define(g, pointer, coerce(g, to, value));
}

void genDrop(Gen& g, InstDrop& instruction) {
    /*
     * Three of the four things a teardown can do are nothing here (§3.3).
     *
     * `reclaim` releases the value's own storage and `releaseStorage` hands the allocation back,
     * and the host collector does both - which is exactly the carve-out Design-Memory §4 states for
     * this target. What is left is `drop`: an effect at last use, never elided on any target, and
     * the one place Yana-on-JS does something the JS it replaces cannot.
     */
    if(!instruction.drop) return;

    auto type = placeType(g, instruction.place);

    if(g.genEnv && isGeneric(g.global, type)) {
        // The teardown the caller's descriptor names, reached through the same cell read every
        // other erased operation uses. This one *is* written against a reference - the descriptor's
        // slot has one signature for every type - so the box is right here.
        if(auto descriptor = genTypeDesc(g, type)) {
            emitExpr(g, call(g, tableCell(g, descriptor, TypeDescFields::kDrop),
                             referenceTo(g, instruction.place)));
        }

        return;
    }

    /*
     * Which form the argument takes, asked of the *callee* rather than worked out from the type.
     *
     * It used to be worked out: a scalarized record went by value and everything else kept the box,
     * on the reading that "aggregates unboxed, scalars boxed" was what every teardown had been
     * compiled against. That was true only for as long as the two populations happened to line up.
     * An authored `Drop` declares `->value: T` and reads the value; derived glue declared a `%T` and
     * read through it; and the two agreed with one call site because a JS reference to an *object*
     * is that object. A type that is not an object - anything niche-folded - was the case where they
     * did not, and the box was allocated there to be read once and discarded.
     *
     * Now every teardown declares `->`, and the one thing left that genuinely takes an address is
     * the erased entry (teardownEntry), which a concrete site never names. So the question is the
     * parameter's declared type, which is a fact rather than a population.
     */
    auto callee = g.local[instruction.drop];
    auto takesAddress = callee->args.isNotEmpty() &&
                        isPointer(g.global, g.local[callee->args.get(g.local, 0)]->type);

    /*
     * A function value's teardown takes the two words rather than the value -
     * Implementation-JS-Closure.md part 3, and the one place flattening forces a convention change
     * rather than an optimization.
     *
     * The glue is generated per *type* and shared by every holder: one `drop$_Int_sub_gt_Int` for
     * every `(Int) -> Int` in the program. Once the two words can live as two variables of a frame
     * or two properties of an arbitrary record, there is no one value to hand it and no property
     * name it could agree with every holder about - so it is handed the words, which works wherever
     * they live because it never asks.
     *
     * It falls out of the argument convention rather than being a signature of its own: the glue
     * declares `->value: T` like any other teardown, `funIsFlattened` says a function value crosses
     * as two arguments, and both sides read that off the same declaration. Which is why the header
     * is still reached as `FunValueLayout::kHeader` inside the glue - the place walk answers it from
     * the parts, exactly as it answers `$o`/`$k` for a flattened reference parameter.
     */
    if(!takesAddress && isFunValue(g, placeType(g, instruction.place)) &&
       callee->args.isNotEmpty() && functionFlattensArgs(g, *callee) &&
       funIsFlattened(g, ((Arg*)g.local[callee->args.get(g.local, 0)])->type,
                      ((Arg*)g.local[callee->args.get(g.local, 0)])->convention)) {
        auto parts = funPartsOfPlace(g, instruction.place);

        emitExpr(g, call(g, functionValue(g, instruction.drop, instruction.source),
                         parts.code, parts.env));
        return;
    }

    emitExpr(g, call(g, functionValue(g, instruction.drop, instruction.source),
                     takesAddress ? referenceTo(g, instruction.place)
                                  : placeExpr(g, instruction.place)));
}

/*
 * One argument, as however many the convention says it occupies.
 *
 * A narrow reference goes as its parts and a function value as its two words - see refIsFlattened
 * and funIsFlattened. The decision is the *declared parameter* type rather than the argument's, and
 * the difference is not hypothetical: a concrete `&Bool` handed to a generic `&a` reaches a body
 * compiled against a type variable, which has no width to mask with and takes the object. Where the
 * two agree - every specialized call - reading the parameter costs a lookup and says the same thing.
 *
 * That is also the whole of why an erased boundary is never flat, for either form: a parameter
 * declared `a` is a `Type::Gen`, so neither predicate matches it and what crosses is the reference
 * the erased ABI already passes. One sentence covers every multi-part representation this target
 * has or later grows rather than one case each.
 */
void pushArg(Gen& g, Array<JsPtr<Expr>>& args, ModulePtr<Value> arg, bool flat, bool value,
             bool flatFun) {
    if(arg && flat) {
        auto parts = refPartsOf(g, arg);
        args.push(parts.owner);
        args.push(parts.key);

        // The third, whichever of the two it is - see refPartsOfExpr. Never both, so this is one
        // position rather than two, and the callee binds it as one parameter.
        if(parts.scale) args.push(parts.scale);
        if(parts.envKey) args.push(parts.envKey);
        return;
    }

    // The two words, in FunValueLayout's order. No object is built here and none is taken apart:
    // where the argument is a local the body holds flat, these *are* its two variables.
    if(arg && flatFun) {
        auto parts = funPartsOf(g, arg);
        args.push(parts.code);
        args.push(parts.env);
        return;
    }

    /*
     * A reference handed over where the callee declared a value - see callParameterTakesValue. What
     * travels is the box the callee reads the value back through, and the reference is where the
     * value comes from: `{$v: r.$o[r.$k]}`.
     *
     * A copy, and so it was before this target had the triple at all - the parameter is by value, so
     * nothing is written through the box and nothing has to be committed back. Removing it needs the
     * callee to take the reference, which is a convention change rather than a form choice.
     */
    if(arg && value) {
        auto type = g.local[arg]->type;
        if(type && g.global[type]->kind == Type::Borrow) {
            auto pointee = ((BorrowType*)g.global[type])->to;
            if(refIsTriple(g, pointee)) {
                auto parts = refPartsOf(g, arg);
                args.push(boxOf(g, elementAt(g, parts.owner, parts.key)));
                return;
            }
        }
    }

    args.push(useValue(g, arg));
}

void genCall(Gen& g, ModulePtr<Value> pointer, InstCall& instruction) {
    Array<JsPtr<Expr>> args;

    Size index = 0;
    for(auto arg: instruction.args.contents(g.local)) {
        if(!callParameterIsAbsent(g, instruction, index)) {
            pushArg(g, args, arg, callParameterIsFlatRef(g, instruction, index),
                    callParameterTakesValue(g, instruction, index),
                    callParameterIsFlatFun(g, instruction, index));
        }

        index++;
    }

    auto callee = functionValue(g, instruction.callee, instruction.source);
    define(g, pointer, callWith(g, callee, args));
}

void genCallDyn(Gen& g, ModulePtr<Value> pointer, InstCallDyn& instruction) {
    Array<JsPtr<Expr>> args;
    JsPtr<Expr> callee = nullptr;

    if(instruction.callable) {
        /*
         * An ordinary call of a function value, which is the two words: `f.$c(f.$e, ...)`, exactly
         * the entry native performs with the same two words in registers.
         *
         * The environment is passed whatever the value is. A lambda that captured nothing and the
         * thunk that makes a named function a value both hold `null` there and both ignore the
         * parameter, so there is one call shape rather than a test at each site - the same trade
         * `functionThunk` already makes for the arity.
         */
        auto parts = funPartsOf(g, instruction.callable);
        callee = parts.code;
        args.push(parts.env);
    } else {
        /*
         * A bare address, which is the compiler calling something it generated - a teardown reached
         * through a closure header or a descriptor slot. There is no function value and no
         * environment convention: whatever the caller pushed is the whole argument list.
         */
        callee = useValue(g, instruction.address);
    }

    Size index = 0;
    for(auto arg: instruction.args.contents(g.local)) {
        if(!callParameterIsAbsent(g, instruction, index)) {
            pushArg(g, args, arg, callParameterIsFlatRef(g, instruction, index),
                    callParameterTakesValue(g, instruction, index),
                    callParameterIsFlatFun(g, instruction, index));
        }

        index++;
    }

    define(g, pointer, callWith(g, callee, args));
}

// The environment assembled at the call site: the slots this caller knows concretely, and the ones
// it forwards out of its own.
JsPtr<Expr> genEnvTable(Gen& g, InstGenCall& instruction) {
    auto table = make<ArrayExpr>(g);

    /*
     * Straight into the slots, with nothing in front of them.
     *
     * There used to be a schema word here, filled with a literal zero because nothing ever wrote a
     * schema into it and nothing ever read one back out. A caller assembling a table still has to
     * agree with the interned ones it may be passed alongside, and both now start at slot 0.
     *
     * The values are the emitted bindings themselves rather than offsets from anything: an array
     * element holds a reference, so the anchor the native form measures from has no counterpart
     * here and none is needed - see repr/table.h.
     */
    for(auto slot: instruction.fill.contents(g.local)) {
        // A count slot holds the number - Implementation-Const-Generics.md §3.1 - which here is
        // simply a host number in an array element. Nothing to encode either way, which is the same
        // thing this whole table already has to say about addresses.
        auto value = slot.count
            ? (slot.isForwarded() ? genSlot(g, slot.forwarded) : number(g, F64(slot.value)))
            : (slot.isForwarded() ? genWitness(g, slot.forwarded, slot.forwardedSupers)
                                  : globalValue(g, slot.constant));

        table->values.push(g.file.arena, value);
    }

    return asExpr(g, table);
}

void genGenCall(Gen& g, ModulePtr<Value> pointer, InstGenCall& instruction) {
    Array<JsPtr<Expr>> args;
    JsPtr<Expr> callee = nullptr;

    if(instruction.typeClass) {
        // A deferred class dispatch: the witness out of an environment slot, the method out of the
        // witness. Two reads and no search, exactly as on native - and the callee is a concrete
        // thunk, so it needs no environment of its own.
        auto witness = genWitness(g, instruction.classSlot, instruction.classPath);
        callee = tableCell(g, witness, ClassWitnessFields::method(instruction.index));
    } else {
        callee = functionValue(g, instruction.callee, instruction.source);
        args.push(instruction.env ? globalValue(g, instruction.env) : genEnvTable(g, instruction));
    }

    /*
     * The concrete-to-erased boundary - Implementation-Generics.md part 8's "unknown-size values use
     * addresses", which on JS is "unknown-shape values arrive as a reference".
     *
     * A callee compiled against its own type variables cannot hold one of them in a variable and
     * hand it back, for the same reason native cannot return it in a register: it does not know what
     * it is. Native's answer is an address, and the JS answer is the box that stands in for one - so
     * a caller holding a `number` where the callee declared `a` wraps it, and unwraps whatever comes
     * back. A value that is already an object needs neither, which is most of them.
     *
     * A `&` parameter is a reference on both sides already, so it crosses unchanged. Boxing one
     * would hand the callee a reference to the reference, and every write through it would land in a
     * temporary the caller never reads.
     */
    auto function = g.local[instruction.callee];
    Size argIndex = 0;

    Array<JsPtr<Expr>> declared;
    for(auto arg: instruction.args.contents(g.local)) {
        auto concrete = g.local[arg]->type;

        auto parameter = argIndex < function->args.size()
            ? g.local[function->args.get(g.local, argIndex)] : nullptr;

        /*
         * Which positions exist is the *callee's* question, and the answer differs from the
         * concrete one here: a parameter declared as a type variable is a position in the erased
         * signature whatever this caller substituted, including `{}`. What travels for it is the
         * box a reference always is, holding nothing.
         */
        if(parameter && declaredArgIsAbsent(g, parameter->type, parameter->convention)) {
            argIndex++;
            continue;
        }

        if(isUnit(g.global, concrete)) {
            declared.push(boxOf(g, nullValue(g)));
            argIndex++;
            continue;
        }

        // A narrow reference the callee declared narrow goes flat here too. One it declared generic
        // does not, and that is the case this list has always been about: an erased body holds a
        // type variable and has neither the width to mask with nor the shift to apply.
        if(parameter && functionFlattensArgs(g, *function) &&
           refIsFlattened(g, parameter->type, parameter->convention)) {
            pushArg(g, declared, arg, true, false, false);
            argIndex++;
            continue;
        }

        // A function value the callee declared as one goes flat here too, and one it declared
        // generic does not - the same split, for the same reason and read off the same declaration.
        if(parameter && functionFlattensArgs(g, *function) &&
           funIsFlattened(g, parameter->type, parameter->convention)) {
            pushArg(g, declared, arg, false, false, true);
            argIndex++;
            continue;
        }

        /*
         * A mutable borrow crossing into a parameter the callee declared generic.
         *
         * It cannot cross as itself: what this caller holds is the `{$o, $k, $s}` triple, and every
         * use the callee can make of an `&a` - `moveInit`, `copyInit`, the erased teardown - reads
         * through its argument as a *slot*. So what goes over is the storage the triple names, which
         * is erasedStorageOf's job, and where there is no such slot this is `README.md` gap 1 and is
         * reported rather than written into the wrong object.
         */
        // What the argument refers to, which is what decides whether a slot exists to hand over. A
        // `&T` and a `%T` say it in the two different ways this target already tells apart.
        auto referenced = concrete && g.global[concrete]->kind == Type::Borrow
            ? ((BorrowType*)g.global[concrete])->to
            : pointeeType(g.global, concrete);

        if(parameter && parameter->isMutableBorrow() && isGeneric(g.global, parameter->type) &&
           arg && referenced && !isJsObject(g, referenced)) {
            auto borrowed = g.local[arg];
            Maybe<JsPtr<Expr>> storage;

            if(borrowed->kind == Value::Borrow) {
                storage = erasedStorageOf(g, ((InstBorrow*)borrowed)->place);
            } else if(borrowed->kind == Value::Address) {
                storage = erasedStorageOf(g, ((InstAddress*)borrowed)->place);
            }

            if(!storage) {
                g.context.diagnostics.error("the JS target cannot hand a mutable borrow of this storage to a parameter of a type it cannot see the shape of - it names a slot inside something else, and the erased ABI passes the slot itself (README.md gap 1)"_v,
                                            instruction.source);
            }

            declared.push(storage ? storage.unwrap() : useValue(g, arg));
            argIndex++;
            continue;
        }

        auto value = useValue(g, arg);

        if(parameter && !parameter->isMutableBorrow() && isGeneric(g.global, parameter->type) &&
           !isJsObject(g, concrete)) {
            value = boxOf(g, value);
        }

        declared.push(value);
        argIndex++;
    }

    /*
     * The result, decided by what the *callee* declared rather than by what this call substituted.
     *
     * A class function returning `a` hands its answer back through storage the caller supplies,
     * because the thunk that implements it was compiled with that storage as its first parameter.
     * A generic *function* is different and deliberately so: this backend emits both sides of that
     * call, so it returns its value the way every other JS function does and there is nothing
     * hidden to pass.
     */
    auto erasedResult = instruction.typeClass && isJsObject(g, function->returnType);
    auto directResult = !isJsObject(g, instruction.type) && !isUnit(g.global, instruction.type);
    JsPtr<Expr> place = nullptr;

    if(erasedResult) {
        // The storage the callee writes through, which is a box for anything that is not known to
        // be an object already - including a result whose type is one of *this* body's type
        // variables, since the erased convention is a reference the whole way down.
        auto boxed = directResult || isGeneric(g.global, instruction.type);
        auto storage = zeroValue(g, instruction.type);

        place = declare(g, generatedName(g, "out"_v, instruction.id),
                        boxed ? boxOf(g, storage) : storage);

        args.push(place);
    }

    for(auto value: declared) args.push(value);

    auto called = callWith(g, callee, args);

    if(!erasedResult) {
        // What comes back is whatever the callee holds, which for a result the callee declared
        // generic is the reference this caller has to read through.
        auto boxedResult = !instruction.typeClass && directResult &&
                           isJsObject(g, function->returnType);

        define(g, pointer, boxedResult ? field(g, called, g.boxField) : called);
        return;
    }

    emitExpr(g, called);
    if(!isUnit(g.global, instruction.type)) {
        g.values.add(U32(pointer), directResult ? field(g, place, g.boxField) : place);
    }
}

/*
 * A reference that names a slot - Design.md's tier 2, as this target spells it.
 *
 * Native carries an address plus the shift of the field within the word it names. There are no
 * addresses here, but a place already *is* an (object, property) pair, so the reference is that pair
 * reified plus the shift: `{$o: owner, $k: "field", $s: 1}`.
 *
 * The point of it is what it is not - a copy with a write-back. That was the box this target used to
 * make for the immutable half, and it only worked while the loan ended at the call that consumed it;
 * this one has no commit point at all, so a callee may keep it, return it through a `return`
 * parameter, or store it in a record that outlives the call, exactly as on native.
 *
 * A whole local is the same shape: prepareLocals boxed it, so the pair is that box and `$v`.
 *
 * Which references take this form is `refIsTriple` for a borrow and `addressIsTriple` for a raw
 * pointer - mutability is not a question in either, which is what removed the write-back.
 */
static bool genNarrowRef(Gen& g, ModulePtr<Value> value, Value& instruction, const Place& place,
                         TypePtr type, bool triple) {
    if(!triple) return false;

    auto narrow = isNarrowValue(g.global, type);
    auto projections = place.projections;
    auto count = projections.size();

    // Reborrowing one: it is already a triple of exactly this shape, in whichever form that one is
    // being carried. Passing the form along rather than the object is what keeps a chain of
    // re-borrows from materializing one at each link.
    auto reborrow = [&](ModulePtr<Value> from) {
        if(auto flat = g.flatRefs.get(U32(from))) {
            /*
             * Copied out before the table is written to, and it has to be.
             *
             * `HashMap::get` answers a reference *into* the table, and `add` may grow and rehash it -
             * so passing `flat.unwrap()` straight to `add` hands it a reference to storage `add` has
             * already released, and reading it again afterwards reads whatever the new table left
             * behind. The symptom was an object literal with three uninitialized property values and
             * a segfault in the peephole; the cause is not this file's rule about references at all,
             * and what made it appear was simply crossing a growth threshold.
             */
            auto parts = flat.unwrap();

            g.flatRefs.add(U32(value), parts);
            if(narrowRefNeedsObject(g, value)) define(g, value, materializeRef(g, parts));
        } else {
            define(g, value, useValue(g, from));
        }
    };

    /*
     * Whether the reference this place is rooted in is a triple already, which is what makes passing
     * it on rather than taking it apart correct. A `%a` whose pointee is not narrow is the box that
     * stands in for an address, and handing that on where a triple is wanted names the wrong thing.
     */
    auto rootIsTriple = [&](TypePtr referenced) {
        if(!referenced) return false;
        if(g.global[referenced]->kind == Type::Borrow) {
            return refIsTriple(g, ((BorrowType*)g.global[referenced])->to);
        }

        return addressIsTriple(g, pointeeType(g.global, referenced));
    };

    if(!count) {
        if((place.root == PlaceRoot::Borrow || place.root == PlaceRoot::Pointer) &&
           rootIsTriple(g.local[place.pointer]->type)) {
            reborrow(place.pointer);
            return true;
        }

        if(place.root == PlaceRoot::Local && place.local < g.function->localCount()) {
            auto slot = g.function->localAt(g.local, place.local);
            if(slot.borrowed && slot.convention == ast::BindType::Ref && refIsTriple(g, slot.type)) {
                reborrow(slot.value);
                return true;
            }
        }
    }

    /*
     * The pair, read off the walk's own output rather than re-derived from the projections.
     *
     * `placeOwner` already produces the expression a read of this place would evaluate - `s.flags`,
     * `xs[i]`, `box.$v`, `r.$o[r.$k]` - stopping at the word where the tail of the path is a bit
     * range, and the bits it reports say where inside that word the value sits. Every one of those
     * is `owner.key` or `owner[key]` in the tree, so taking the pair apart is reading two children
     * of one node.
     *
     * Doing it this way is the point rather than a shortcut: the reference has to name the slot the
     * *read* would have used, and a second walk that decided which projection was the last nameable
     * step could disagree with the first - which is what it did, for a payload reached through a
     * `Downcast` and for a whole boxed global. See Implementation-Simplification.md D, which is this
     * argument applied to the other twelve walks.
     */
    JsPtr<Expr> owner = nullptr;
    JsPtr<Expr> keyExpr = nullptr;
    JsPtr<Expr> envKeyExpr = nullptr;
    PlaceBits bits;

    // One access node taken apart into the pair it already is. `owner.key` and `owner[key]` are the
    // two shapes a read of a place ever has here, which is what makes a reference two children of
    // one node rather than a second walk that could disagree with the first.
    auto split = [&](JsPtr<Expr> access, JsPtr<Expr>& into) {
        if(!access) return false;

        if(g.base[access]->kind == Expr::Field) {
            auto& read = (FieldExpr&)*g.base[access];
            owner = read.object;
            into = asExpr(g, make<StringExpr>(g, read.field.text));
            return true;
        }

        if(g.base[access]->kind == Expr::Index) {
            auto& read = (IndexExpr&)*g.base[access];
            owner = read.array;
            into = read.index;
            return true;
        }

        return false;
    };

    PlaceBits walked;

    /*
     * A function value, whose two words are two properties of one owner - so the reference is that
     * owner and *both* keys.
     *
     * The words are taken from the place walk rather than from the projections, on the same terms
     * as the single-key case below: whichever form they are in - two properties of a record, two of
     * the pair object a local was kept in - each is `owner.key`, and they share the owner because
     * one walk produced both.
     */
    if(isFunValue(g, type)) {
        auto words = funPartsOfPlace(g, place);

        if(!split(words.code, keyExpr) || !split(words.env, envKeyExpr)) {
            g.context.diagnostics.error("the JS target cannot make a reference to a function value that is not reachable through an object"_v,
                                        instruction.source);
            return false;
        }
    }

    auto word = envKeyExpr ? JsPtr<Expr>(nullptr) : placeOwner(g, place, walked);

    if(envKeyExpr) {
        // Already taken apart above; the bit range is not a question a function value has.
    } else if(!walked.foldedTag && split(word, keyExpr)) {
        // Answered by the split.
    } else {
        /*
         * The last resort, and what makes this function total.
         *
         * Whether a reference is carried as a triple is decided from the *pointee type* by both
         * sides of a call, and neither can see the other - so a caller that cannot name the slot
         * still has to produce the parts. A box is a slot: `{$v: value}` with `$v` as the key names
         * it exactly.
         *
         * It is a *copy*, though: what it names is a cell this instruction created rather than the
         * storage the place does, so a write through it reaches nobody. The write-back that used to
         * follow is the form refIsTriple exists to remove - correct only while the loan ends at the
         * call that consumed it, which nothing here can know - so this is reported instead.
         *
         * What reaches it is a place whose read is not a property access at all: a bare `var`. Every
         * one of those is now storage something else boxed - prepareLocals for a local, genGlobal
         * for a global - so this is a gap rather than a case.
         */
        g.context.diagnostics.error("the JS target cannot make a reference to storage that is not reachable through an object - it needs the boxed storage of Implementation-Simplification.md B"_v,
                                    instruction.source);

        auto box = declare(g, valueName(g, instruction), boxOf(g, placeExpr(g, place)));
        owner = box;
        keyExpr = asExpr(g, make<StringExpr>(g, g.boxField.text));
        walked = PlaceBits {};
    }

    bits.offset = walked.offset;
    bits.scale = walked.scale;

    // Only where the pointee is narrow. A whole value occupies what it names, so there is no range
    // to record and `bits.valid()` has to stay false - see refIsTriple, which sends references to
    // wide values down this same path.
    if(narrow) bits.width = narrowWidth(g, type);

    /*
     * The three parts, as themselves where that is already safe and in a variable where it is not.
     *
     * A part may be read several times - each use of the reference reads all three - so anything with
     * an evaluation cost or an effect has to be named first. A variable or a literal has neither, and
     * naming those would replace `t.$v >>> 2` with three declarations and the same expression. The
     * key and the shift are literals in every case a record produces, and the owner is a variable
     * whenever the reference is to a whole local, which is most of them.
     */
    auto part = [&](StringView suffix, JsPtr<Expr> value) {
        auto kind = g.base[value]->kind;
        auto trivial = kind == Expr::Var || kind == Expr::Number || kind == Expr::String ||
                       kind == Expr::Bool || kind == Expr::Null;

        return trivial ? value : declare(g, partName(g, instruction, suffix), value);
    };

    RefParts parts;
    parts.owner = part("$o"_v, owner);
    parts.key = part("$k"_v, keyExpr);
    if(envKeyExpr) parts.envKey = part("$ke"_v, envKeyExpr);

    /*
     * The scale, on a target that has bit ranges in it at all. Where nothing is packed every narrow
     * value sits at offset zero of what it names, so the scale is provably one and is not
     * represented - the same elision native states about a full-width value, one level up. See
     * narrowRefCarriesScale.
     *
     * `2**offset` rather than the offset, because that is the number the callee's arithmetic wants:
     * it cannot know how wide the word it was handed is, so it divides rather than shifts, and
     * computing `2**s` from `s` at every access would be a `Math.pow` per read. Composition is a
     * multiply where it used to be an add, and both operands are constants at every site.
     */
    if(narrowRefCarriesScale(g) && !envKeyExpr) {
        auto scale = bits.scale ? bits.scale : number(g, 1);
        if(bits.offset) {
            auto step = number(g, powerOfTwo(bits.offset));
            scale = bits.scale ? binary(g, BinaryOp::Mul, scale, step) : step;
        }

        parts.scale = part("$s"_v, scale);
    }

    g.flatRefs.add(U32(value), parts);

    // And the object, only where something needs the reference to be one value - see
    // narrowRefNeedsObject. This is the allocation flattening exists to remove.
    if(narrowRefNeedsObject(g, value)) define(g, value, materializeRef(g, parts));

    return true;
}

/*
 * A borrow or an address, which is free except where what is named is not an object.
 *
 * Free for the reason §2.5 gives: the checker already proved nobody else holds the storage, so
 * handing over the object reference is what hand-written JS does anyway. A borrow of anything that
 * is not an object goes through genNarrowRef and names its slot; an address keeps the box that
 * stands in for one, which is what the erased ABI reads through - see refIsTriple.
 */
void genBorrow(Gen& g, ModulePtr<Value> value, Value& instruction, const Place& place, bool address) {
    auto type = placeType(g, place);
    auto triple = address ? addressIsTriple(g, type) : refIsTriple(g, type);

    if(genNarrowRef(g, value, instruction, place, type, triple)) return;

    /*
     * An object, or a path that ends in one: the reference *is* the object, so this is a name and
     * nothing else. A local prepareLocals boxed is the same statement about a scalar - the box was
     * made at the storage rather than here, so naming it is all an address of one is.
     *
     * An immutable borrow of a wide value is *not* elided into a snapshot here, and it was worth
     * trying: nothing may write through one, so a copy reads the same. What it needs is the callee's
     * agreement - a snapshot is not a reference, so both sides have to decide it from the
     * declaration, the way refIsFlattened is - rather than something a borrow can settle on its own.
     */
    if(isJsObject(g, type) || g.aliasBorrows.contains(U32(value))) {
        define(g, value, placeExpr(g, place));
        return;
    }

    auto projections = place.projections;
    if(projections.isEmpty()) {
        if(place.root == PlaceRoot::Local && place.local < g.function->localCount() &&
           place.local < g.boxed.size() && g.boxed[place.local]) {
            define(g, value, useValue(g, g.function->localAt(g.local, place.local).value));
            return;
        }

        if(place.root == PlaceRoot::Borrow || place.root == PlaceRoot::Pointer) {
            define(g, value, useValue(g, place.pointer));
            return;
        }
    }

    /*
     * An address of storage that is not already a box, which is the one shape left with nothing to
     * name: the box is made here and it is a *copy*, so a write through it reaches nobody.
     *
     * Reported rather than emitted, because the write-back that used to stand here was the form
     * refIsTriple exists to remove - it is only correct while the loan ends at the call that
     * consumed it, and nothing here can know that it does. Every address a program produces is of a
     * whole local, which prepareLocals boxed, so this is a gap rather than a case.
     */
    g.context.diagnostics.error("the JS target cannot take the address of storage that is not a whole local yet - it needs the boxed storage of Implementation-Simplification.md B"_v,
                                instruction.source);
    define(g, value, boxOf(g, placeExpr(g, place)));
}

/*
 * Three relocations through a temporary, and the temporary is not removable: neither place can be
 * written until both have been read. That is the cost `exchange` exists to avoid, and it is the same
 * cost on both targets.
 */
void genSwap(Gen& g, Value& instruction, InstSwap& swap) {
    auto a = placeExpr(g, swap.a);
    auto b = placeExpr(g, swap.b);

    if(!swap.sink) {
        /*
         * Both sides may be bit ranges of the *same* word - `swap(t.f.a, t.g.a)` is two bits of one
         * byte - so both reads are materialized before either write. With two locations one
         * temporary is enough and the second is left out, which is what keeps this the same emitted
         * shape it has always had on a target that packs nothing.
         */
        PlaceBits aBits, bBits;
        placeOwner(g, swap.a, aBits);
        placeOwner(g, swap.b, bBits);

        auto temporary = declare(g, generatedName(g, "swap"_v, instruction.id), a);
        auto other = aBits.valid() || bBits.valid()
            ? declare(g, generatedName(g, "swapped"_v, instruction.id), b)
            : b;

        if(!assignPlace(g, swap.a, swap.content, other)) emitExpr(g, assign(g, a, other));
        if(!assignPlace(g, swap.b, swap.content, temporary)) emitExpr(g, assign(g, b, temporary));
        return;
    }

    auto temporary = declare(g, generatedName(g, "swap"_v, instruction.id), zeroValue(g, swap.content));

    relocateWith(g, temporary, a, swap.content, swap.sink, instruction.source);
    relocateWith(g, a, b, swap.content, swap.sink, instruction.source);
    relocateWith(g, b, temporary, swap.content, swap.sink, instruction.source);
}

// Two relocations and no temporary: what is coming in is already a value rather than a place, so
// there is nothing to save it from.
void genExchange(Gen& g, ModulePtr<Value> value, Value& instruction, InstExchange& exchange) {
    if(!exchange.sink) {
        // The old value has to be read before the write, and where the place is a bit range the read
        // is an expression over the word the write is about to change - so it is materialized rather
        // than left to be evaluated at its use.
        PlaceBits bits;
        placeOwner(g, exchange.place, bits);

        auto previous = placeExpr(g, exchange.place);
        define(g, value, bits.valid()
            ? declare(g, valueName(g, instruction), previous)
            : previous);

        storeInto(g, exchange.place, instruction.type, exchange.value);
        return;
    }

    auto out = declare(g, valueName(g, instruction), zeroValue(g, instruction.type));
    g.values.add(U32(value), out);

    relocateWith(g, out, placeExpr(g, exchange.place), instruction.type, exchange.sink,
                 instruction.source);
    storeInto(g, exchange.place, instruction.type, exchange.value);
}

// Property by property, and a nested aggregate is duplicated rather than aliased: on native those
// bytes are *inline*, so the copy makes an independent value of them. The member `Sink` calls that
// follow this in the glue then run over storage that is this value's own, which is what they are
// there to fix up.
/*
 * The host - Implementation-Containers.md §14.1.
 *
 * Three operations and no host knowledge: which member is called and what it is called on both arrive
 * on the instruction, put there by a declaration in `Host` that says `.splice` in Yana. That is the property Analysis-JS.md §2.4 asks for when it rules host
 * knowledge out of codegen - a container's implementation names the host and the backend names
 * nothing.
 *
 * The receiver is `args[0]` for the two member forms. A `HostArray` and a `HostNew` have none: the
 * first is a literal and the second names a global constructor, which is the one place a host
 * *name* reaches the emitted text, and it reaches it from the same `method` field.
 */
// The plan, carried out. Nothing where the shape declines, and the caller then stores component by
// component - which is also what the allocation did, since the two read one plan.
bool buildWholeLocal(Gen& g, InstAggregate& aggregate) {
    auto& place = aggregate.place;
    if(place.root != PlaceRoot::Local) return false;
    if(place.local >= g.builtWhole.size() || !g.builtWhole[place.local]) return false;

    auto plan = wholeLocalPlan(g, aggregate);
    if(!plan.eligible) return false;

    emitExpr(g, assign(g, placeExpr(g, place), buildFromPlan(g, aggregate, plan)));
    return true;
}

/*
 * The elements of a literal - see InstAggregate.
 *
 * Two forms, and which one is emitted turns on a single question about the run's own value: an
 * aggregate written through a `hostarray` **is** that array's contents, because the instruction that
 * made it is right there and made it empty. So the whole thing is one literal, assigned to the name
 * the `hostarray` was bound to - and `foldInitialValue` in opt.cpp then collapses `var v = [];
 * v = [1, 2, 3];` into the declaration, on the same general rule that collapses a zeroed record.
 *
 * That is the shape §2.3 asks for and the one every engine specializes on. Writing the elements
 * afterwards instead walks the element-kind transitions a literal skips, which is what the peephole
 * over the emitted tree used to have to undo by matching a run of index writes.
 *
 * Anything else - a `[T *n]` whose storage already exists, an array reached through something other
 * than its own construction - is filled element by element, which is what it means for storage to
 * be there before the values are.
 */
void genAggregate(Gen& g, InstAggregate& aggregate) {
    auto whole = aggregate.place.projections.isEmpty();

    if(whole && aggregate.place.root == PlaceRoot::Pointer) {
        auto base = g.local[aggregate.place.pointer];

        if(base->kind == Value::Native && ((InstNative*)base)->op == NativeOp::HostArray) {
            auto elements = make<ArrayExpr>(g);

            eachAggregateComponent(g.local, aggregate,
                                   [&](const AggregateComponent& component, Size) {
                auto type = g.local[component.value]->type;

                elements->values.push(g.file.arena,
                                      keptValue(g, type, component.value,
                                                useValue(g, component.value), aggregate.source));
            });

            // Through the same choice the empty array made, and this is the second reader §14's
            // note means: a literal that wrote `[1, 2, 3]` over storage the other path had made a
            // `new Int32Array([])` is a plain host array in a variable the rest of the program grows
            // as a typed one, and nothing anywhere would have said so.
            emitExpr(g, assign(g, useValue(g, aggregate.place.pointer),
                               hostArrayOf(g, base->type, asExpr(g, elements))));
            return;
        }
    }

    if(buildWholeLocal(g, aggregate)) return;

    eachWrittenComponent(g.local, g.program.arena, aggregate,
                         [&](Place place, ModulePtr<Value> value, Size) {
        if(isUnit(g.global, g.local[value]->type)) return;
        storeInto(g, place, g.local[value]->type, value);
    });
}

void genHost(Gen& g, ModulePtr<Value> value, Value& instruction, InstNative& native) {
    auto args = native.args;
    auto member = [&]() -> Name {
        return propertyName(g, stringView(g.context.findName(native.method)));
    };

    auto argsFrom = [&](Size first) {
        Array<JsPtr<Expr>> list;
        for(Size i = first; i < args.size(); i++) list.push(useValue(g, args.get(g.local, i)));
        return list;
    };

    switch(native.op) {
        case NativeOp::HostCall: {
            auto receiver = useValue(g, args.get(g.local, 0));
            auto rest = argsFrom(1);

            define(g, value, callWith(g, field(g, receiver, member()), rest));
            break;
        }
        case NativeOp::HostField: {
            auto read = field(g, useValue(g, args.get(g.local, 0)), member());

            /*
             * The one host property whose *range* is specified, recorded on the node - see
             * FieldExpr::hostLength.
             *
             * `hostLength` and `hostStringLength` are the whole of `HostField` today, and both
             * answer a `uint32` by the host's own definition. That fact exists nowhere else: it is
             * not in the Yana type, which is `Size` and therefore `Int`, and the peephole cannot
             * recover it from a property read. Without it every `xs[i]`'s bounds check emitted
             * `xs.length >>> 0` for a coercion that can never do anything.
             *
             * Matched on the member `Host` declared rather than on a second enum, on the same terms
             * as `HostBinary`'s operator table below: the set of host members is closed - they are
             * attached by `attachIntrinsic` in resolve/host.cpp and nothing a program writes can add
             * one - so a member not in it is a missing line there rather than something reachable.
             */
            auto text = stringView(g.context.findName(native.method));
            if(text == "length"_v) asHostLength(g, read);

            define(g, value, read);
            break;
        }
        case NativeOp::HostArray: {
            auto elements = make<ArrayExpr>(g);
            for(auto arg: args.contents(g.local)) {
                elements->values.push(g.file.arena, useValue(g, arg));
            }

            define(g, value, hostArrayOf(g, native.type, asExpr(g, elements)));
            break;
        }
        case NativeOp::HostGrow: {
            /*
             * The typed row's growth. `a.constructor` is what makes one helper serve every element
             * type - the array knows which one it is, and asking it is cheaper than carrying the
             * name to a place that would then have to agree with `typedArrayFor` about it.
             */
            if(!g.growHelper.text) g.growHelper = uniqueName(g, "$grow"_v, false);

            auto call = make<CallExpr>(g, variable(g, g.growHelper));
            for(auto arg: args.contents(g.local)) call->args.push(g.file.arena, useValue(g, arg));

            define(g, value, asExpr(g, call));
            break;
        }
        /*
         * `$readU32(a, i, le)` - a word out of a byte array through the array's own `DataView`.
         *
         * The width comes off the accessor's name, which is what `method` carries and what the two
         * slots below are indexed by; the order is the last argument and is passed straight through.
         * See NativeOp::HostWordRead for why this is a helper and Gen::viewHelper for why the view
         * is cached on the array.
         */
        case NativeOp::HostWordRead:
        case NativeOp::HostWordWrite: {
            auto read = native.op == NativeOp::HostWordRead;
            auto accessor = stringView(g.context.findName(native.method));
            auto width = accessor == "getUint16"_v || accessor == "setUint16"_v ? 0
                       : accessor == "getUint32"_v || accessor == "setUint32"_v ? 1 : 2;
            auto& slot = read ? g.readWordHelper[width] : g.writeWordHelper[width];

            if(!slot.text) {
                static const StringView names[2][3] = {
                    { "$writeU16"_v, "$writeU32"_v, "$writeU64"_v },
                    { "$readU16"_v, "$readU32"_v, "$readU64"_v },
                };

                slot = uniqueName(g, names[read][width], false);
            }

            auto call = make<CallExpr>(g, variable(g, slot));
            for(auto arg: args.contents(g.local)) call->args.push(g.file.arena, useValue(g, arg));

            if(read) {
                define(g, value, asExpr(g, call));
            } else {
                emitExpr(g, asExpr(g, call));
            }

            break;
        }
        case NativeOp::HostBinary: {
            /*
             * `a <op> b`, where the operator is the host's own - see NativeOp::HostBinary.
             *
             * Matched on the *spelling* rather than on a second enum, because the spelling is already
             * what `method` carries for the two member arms above and duplicating it as a code here
             * would be two things to keep in step. The set is closed and small: `Host`'s declarations
             * are the only producers, so an operator that is not in this table is a missing line
             * there rather than something a program can reach.
             */
            static const struct { StringView text; BinaryOp op; } operators[] = {
                { "+"_v, BinaryOp::Add },
                { "==="_v, BinaryOp::Eq },
                { "!=="_v, BinaryOp::Ne },
                { "<"_v, BinaryOp::Lt },
                { "<="_v, BinaryOp::Le },
                { ">"_v, BinaryOp::Gt },
                { ">="_v, BinaryOp::Ge },
            };

            auto text = stringView(g.context.findName(native.method));
            auto left = useValue(g, args.get(g.local, 0));
            auto right = useValue(g, args.get(g.local, 1));

            for(auto& entry: operators) {
                if(entry.text.length != text.length) continue;
                if(compareMem(entry.text.ptr, text.ptr, text.length) != 0) continue;

                define(g, value, binary(g, entry.op, left, right));
                return;
            }

            g.context.diagnostics.error("internal error: unknown host operator in JS codegen"_v,
                                        instruction.source);
            break;
        }
        case NativeOp::HostGlobalCall: {
            // `Global.method(args...)`, with the whole dotted path in `method` - see
            // NativeOp::HostGlobalCall. Written out as a variable reference rather than looked up,
            // because a host global is not in any scope this backend tracks and needs no
            // disambiguation: it is the name the host already has.
            auto text = stringView(g.context.findName(native.method));
            auto arguments = argsFrom(0);
            define(g, value, callWith(g, variable(g, literalName(g, text)), arguments));
            break;
        }
        case NativeOp::HostThrow: {
            // A statement rather than an expression, which is the whole reason this is an operation
            // of its own - see NativeOp::HostThrow. It produces no value and nothing after it in
            // this block runs, which is already true of the resolve block it came from.
            //
            // The message is written here because the declaration that produces this cannot carry
            // one: a string literal in `Host` would need `Text`, which is built after it.
            auto message = make<StringExpr>(g, g.context.addUnqualifiedName("yana: a runtime check failed", 28));
            emit(g, make<ThrowStmt>(g, asExpr(g, message)));
            break;
        }

        default:
            break;
    }
}

void genBlockCopy(Gen& g, Value& instruction, InstNative& native) {
    auto shape = blockCopyShape(g, native);

    if(!shape) {
        g.context.diagnostics.error("`Native` memory operations have no meaning on the JS target"_v,
                                    instruction.source);
        return;
    }

    auto args = native.args;
    auto target = useValue(g, args.get(g.local, 0));
    auto source = useValue(g, args.get(g.local, 1));

    // A newtype *is* the value it wraps here, so what the bytes are is what the wrapped type is.
    for(TypePtr content = nullptr; isNewtype(g, shape, content) && content; shape = content) {}

    // Not an object, so both references are the box that stands in for one and the whole of the
    // copy is the property it holds.
    if(!isJsObject(g, shape)) {
        emitExpr(g, assign(g, field(g, target, g.boxField), field(g, source, g.boxField)));
        return;
    }

    /*
     * Property by property, and whether each one is duplicated is what `relocates` answers - for the
     * properties it is entitled to answer for, which is not all of them.
     *
     * A relocation's source is dead the moment the glue returns, so for a member the copy fully
     * moves there is nobody left to be a second name and the assignment alone is the whole copy.
     * `moveInit$Quad` was building a fresh `[Int *4]` per call to hand to a `from` that no longer
     * existed. A duplicate's source stays live and owns what it owned, which is every property of
     * `copyInit$` glue and the case this was written for.
     *
     * **A member that relocates by more than its bytes is not one of them**, and it is what makes
     * this a per-property question rather than a flag on the whole copy. The glue does not end here:
     * `moveInitFor` follows the copy with a `Sink` call per member whose bytes are not the whole
     * story, and that call needs the destination to be *distinct* storage - so the duplicate is
     * doing two jobs at once there, and the one that is easy to miss is that it is what creates the
     * destination at all. Aliasing them made `moveInit$Pair` call `sink(x, x)`, which this fixture's
     * sink happened to survive because it only assigns; one that clears its source, which is the
     * ordinary shape for a type owning a resource, would have cleared the value it had just moved.
     *
     * The predicate is `sinkMembers`' from the other side: it emits a call exactly where `sinkFor`
     * answers one, which is exactly where the type is not TrivialSink. Conservative in the safe
     * direction for a boxed field, which `sinkMembers` skips and this still duplicates.
     *
     * All of which exists on one target only. Native writes the same `memcpy` either way - the bytes
     * are the bytes - and the glue is resolve IR each backend compiles its own way, so the
     * distinction lives where the copy is built rather than blitted.
     */
    auto& module = *g.function->module;

    eachProperty(g, shape, [&](Name key, TypePtr member) {
        auto read = field(g, source, key);
        auto relocated = native.relocates && ownershipOf(module, member).trivialSink;

        emitExpr(g, assign(g, field(g, target, key),
                           relocated ? read : cloneValue(g, member, read, instruction.source)));
    });
}

} // namespace

void emitSaturationHelpers(Gen& g) {
    if(g.satHelperOrder.size() == 0) return;

    emit(g, make<CommentStmt>(g, internText(g,
        "float to integer saturates, and `BigInt` throws rather than clamping - see codegen/js/inst.cpp"_v)));

    for(Size i = 0; i < g.satHelperOrder.size(); i++) {
        auto helper = g.satHelperOrder[i];
        auto function = make<FunStmt>(g, helper.name);
        auto argument = literalName(g, "v"_v);
        auto value = variable(g, argument);
        function->args.push(g.file.arena, argument);

        IntType integer(helper.bits, IntType::widthFor(helper.bits), helper.isSigned);
        auto range = saturationRange(integer);

        auto low = integer.isSigned ? -(U64(1) << (helper.bits - 1)) : U64(0);
        auto high = integer.isSigned ? (U64(1) << (helper.bits - 1)) - 1 : ~U64(0) >> (64 - helper.bits);

        function->body = collect(g, [&] {
            // NaN first, since it answers neither comparison below and is the one input `BigInt`
            // refuses outright.
            auto isNaN = binary(g, BinaryOp::Ne, value, value);
            emit(g, make<IfStmt>(g, isNaN, collect(g, [&] {
                emit(g, make<ReturnStmt>(g, bigInt(g, 0, integer.isSigned)));
            }), StmtList {}));

            emit(g, make<IfStmt>(g, binary(g, BinaryOp::Le, value, number(g, range.low, false)),
                                 collect(g, [&] {
                emit(g, make<ReturnStmt>(g, bigInt(g, low, integer.isSigned)));
            }), StmtList {}));

            emit(g, make<IfStmt>(g, binary(g, BinaryOp::Ge, value, number(g, range.high, false)),
                                 collect(g, [&] {
                emit(g, make<ReturnStmt>(g, bigInt(g, high, integer.isSigned)));
            }), StmtList {}));

            emit(g, make<ReturnStmt>(g, globalCall(g, "BigInt"_v, hostCall(g, "Math"_v, "trunc"_v, value))));
        });

        emit(g, function);
    }
}

// One scratch pair and the up-to-two helpers over it, at one width. See `bitcastLane` for why the
// width picks between two pairs rather than one pair serving both.
static void emitFloatBitsPair(Gen& g, bool wide) {
    auto buffer = wide ? g.doubleBitsBuffer : g.floatBitsBuffer;
    if(!buffer.text) return;

    auto ints = wide ? g.doubleBitsInts : g.floatBitsInts;

    emit(g, make<CommentStmt>(g, internText(g, wide
        ? "a double's bits, which are a `bigint` here - see codegen/js/inst.cpp"_v
        : "a float's bits, which no host operator reaches - see codegen/js/inst.cpp"_v)));

    // The float array first and the integer view over its buffer second, which is the whole of what
    // makes the two names one storage location. `BigInt64Array` is the wide pair's integer side and
    // is what makes the value coming back a `bigint` rather than a truncated number.
    emit(g, make<DeclStmt>(g, buffer,
                           construct(g, wide ? "Float64Array"_v : "Float32Array"_v, number(g, 1)), true));
    emit(g, make<DeclStmt>(g, ints,
                           construct(g, wide ? "BigInt64Array"_v : "Int32Array"_v,
                                     field(g, variable(g, buffer), literalName(g, "buffer"_v))),
                           true));

    for(auto toBits: { true, false }) {
        auto name = wide ? (toBits ? g.doubleToBitsHelper : g.bitsToDoubleHelper)
                         : (toBits ? g.floatToBitsHelper : g.bitsToFloatHelper);
        if(!name.text) continue;

        auto written = toBits ? buffer : ints;
        auto read = toBits ? ints : buffer;

        auto function = make<FunStmt>(g, name);
        auto argument = literalName(g, "v"_v);
        auto value = variable(g, argument);
        function->args.push(g.file.arena, argument);

        function->body = collect(g, [&] {
            emitExpr(g, assign(g, index(g, variable(g, written), 0), value));
            emit(g, make<ReturnStmt>(g, index(g, variable(g, read), 0)));
        });

        emit(g, function);
    }
}

void emitFloatBitsHelpers(Gen& g) {
    emitFloatBitsPair(g, false);
    emitFloatBitsPair(g, true);
}

void emitRoundAwayHelper(Gen& g) {
    if(!g.roundAwayHelper.text) return;

    emit(g, make<CommentStmt>(g, internText(g,
        "`Math.round` ties toward +Infinity and this language ties away from zero - see codegen/js/inst.cpp"_v)));

    auto function = make<FunStmt>(g, g.roundAwayHelper);
    auto argument = literalName(g, "v"_v);
    auto value = variable(g, argument);
    function->args.push(g.file.arena, argument);

    /*
     * The negative arm is `-Math.round(-v)`, which is the reflection of ties-toward-+Infinity and so
     * is ties-toward-minus-infinity on the magnitude - away from zero, which is what is wanted.
     *
     * `v < 0` and not `v <= 0`: a negative zero takes the positive arm, where `Math.round(-0)` is
     * `-0` and the sign survives. Through the negative arm it would be `-Math.round(0)`, which is
     * `-0` as well - so this is a statement of intent rather than a correction, and the arm that
     * matters is the NaN one, which compares false and reaches `Math.round(NaN)`.
     */
    function->body = collect(g, [&] {
        emit(g, make<IfStmt>(g, binary(g, BinaryOp::Lt, value, number(g, 0, false)), collect(g, [&] {
            emit(g, make<ReturnStmt>(g, unary(g, UnaryOp::Neg,
                                              hostCall(g, "Math"_v, "round"_v,
                                                       unary(g, UnaryOp::Neg, value)))));
        }), StmtList {}));

        emit(g, make<ReturnStmt>(g, hostCall(g, "Math"_v, "round"_v, value)));
    });

    emit(g, function);
}

/*
 * The three byte reversals - see Gen::byteSwapHelpers.
 *
 *     function $swap16(v) { return ((v & 255) << 8) | ((v >> 8) & 255) }
 *     function $swap32(v) { return ((v & 255) << 24) | (((v >> 8) & 255) << 16) | ((v >> 8) & 65280) | ((v >> 24) & 255) }
 *     function $swap64(v) { return ((v & 255n) << 56n) | ... | ((v >> 56n) & 255n) }
 *
 * One shape for all three, and `>>` throughout rather than `>>>`. That is not a preference: `>>>`
 * does not exist for a `bigint` at all, and for the two narrow widths it would be the *wrong* half
 * of the pair anyway - what makes a shift-down safe here is the mask that follows it, which clears
 * the sign bits a negative `I16` or `I32` brings down whichever shift brought them.
 *
 * Each byte is masked to itself before it is placed, so no term ever holds a bit belonging to
 * another. What the emitted text costs is a few operators more than the compact idiom; what it buys
 * is that the same three lines are right for a signed operand, an unsigned one and a `bigint`.
 *
 * The answer is the raw reversal at the helper's width, and the caller's coercion is what takes it
 * back to the type's own normal form - which is what lets one helper serve both signednesses.
 */
/*
 * The five bit-count helpers - see Gen::bitCountHelpers.
 *
 *     function $popcount32(v) { var a = v - ((v >>> 1) & 0x55555555)
 *                               var b = (a & 0x33333333) + ((a >>> 2) & 0x33333333)
 *                               var c = (b + (b >>> 4)) & 0x0f0f0f0f
 *                               return Math.imul(c, 0x01010101) >>> 24 }
 *     function $ctz32(v)      { return v ? 31 - Math.clz32(v & -v) : 32 }
 *     function $popcount64(v) { return BigInt($popcount32(Number(v & 0xffffffffn))
 *                                           + $popcount32(Number((v >> 32n) & 0xffffffffn))) }
 *     function $clz64(v)      { var h = Number((v >> 32n) & 0xffffffffn)
 *                               return BigInt(h ? Math.clz32(h)
 *                                               : 32 + Math.clz32(Number(v & 0xffffffffn))) }
 *     function $ctz64(v)      { var l = Number(v & 0xffffffffn)
 *                               return BigInt(l ? $ctz32(l)
 *                                               : 32 + $ctz32(Number((v >> 32n) & 0xffffffffn))) }
 *
 * ## The 32-bit pair
 *
 * `$popcount32` is the five-step SWAR fold, and every operator in it is one JS has at exactly 32
 * bits: `>>>` and `&` are ToUint32 and ToInt32, so a negative `I32` folds as its two's complement
 * pattern with no coercion written anywhere, and `Math.imul` is the one exact 32-bit multiply the
 * host has - the ordinary `*` would round the byte-summing product, whose true value passes 2^53.
 * The final `>>> 24` is what makes the answer non-negative at every input.
 *
 * `$ctz32` is `Math.clz32` of the lowest set bit isolated, which is `v & -v` - the shortest thing
 * the host expresses, since it has no trailing-zero count of its own. The ternary is not the zero
 * guard it looks like: `31 - Math.clz32(0)` is -1, and the language's answer at zero is the width.
 *
 * ## The 64-bit three, and why they are written over halves
 *
 * A `bigint` has bitwise operators, so each of these *could* be a loop or a shift chain in the
 * `bigint` domain. They are not, because `bigint` arithmetic is heap arithmetic on every engine and
 * `Math.clz32` is a single machine instruction behind an intrinsic. Splitting once - two masks, two
 * `Number` conversions - and then working in the fast domain is a bounded number of cheap operations
 * where the loop is an unbounded number of expensive ones.
 *
 * `v & 0xffffffffn` is the low half for a negative operand as much as for a positive one, because a
 * `bigint`'s bitwise operators are defined on the two's complement representation extended
 * infinitely - so an `I64` needs no separate case, exactly as the byte reversal above needs none.
 *
 * The result is a `bigint` because the operand's type is 64 bits wide and that is what a value of it
 * is on this target. It is at most 64 either way; the conversion is about the type, not the range.
 */
void emitBitCountHelpers(Gen& g) {
    auto define = [&](Value::Kind kind, bool wide, void (*body)(Gen&, JsPtr<Expr>)) {
        auto& name = g.bitCountHelpers[bitCountHelperSlot(kind, wide)];
        if(!name.text) return;

        auto function = make<FunStmt>(g, name);
        auto argument = literalName(g, "v"_v);
        function->args.push(g.file.arena, argument);
        function->body = collect(g, [&] { body(g, variable(g, argument)); });

        emit(g, function);
    };

    // The one thing the three wide bodies share: a half of the operand, as a `number`.
    static auto half = [](Gen& g, JsPtr<Expr> value, bool high) {
        auto shifted = high ? binary(g, BinaryOp::Sar, value, bigInt(g, 32, false)) : value;
        return globalCall(g, "Number"_v,
                          binary(g, BinaryOp::And, shifted, bigInt(g, 0xffffffff, false)));
    };

    define(Value::CountBits, false, [](Gen& g, JsPtr<Expr> v) {
        auto step = [&](JsPtr<Expr> of, U32 shift, U32 mask, bool before) {
            auto down = binary(g, BinaryOp::Shr, of, number(g, F64(shift)));
            return before ? binary(g, BinaryOp::And, down, number(g, F64(mask)))
                          : binary(g, BinaryOp::Add, of, down);
        };

        auto a = declare(g, literalName(g, "a"_v),
                         binary(g, BinaryOp::Sub, v, step(v, 1, 0x55555555, true)));
        auto b = declare(g, literalName(g, "b"_v),
                         binary(g, BinaryOp::Add, binary(g, BinaryOp::And, a, number(g, F64(0x33333333))),
                                step(a, 2, 0x33333333, true)));
        auto c = declare(g, literalName(g, "c"_v),
                         binary(g, BinaryOp::And, step(b, 4, 0, false), number(g, F64(0x0f0f0f0f))));

        emit(g, make<ReturnStmt>(g, binary(g, BinaryOp::Shr,
                                           hostCall(g, "Math"_v, "imul"_v, c, number(g, F64(0x01010101))),
                                           number(g, 24))));
    });

    define(Value::TrailingZeros, false, [](Gen& g, JsPtr<Expr> v) {
        auto lowest = binary(g, BinaryOp::And, v, unary(g, UnaryOp::Neg, v));
        auto index = binary(g, BinaryOp::Sub, number(g, 31), hostCall(g, "Math"_v, "clz32"_v, lowest));

        emit(g, make<ReturnStmt>(g, ternary(g, v, index, number(g, 32))));
    });

    define(Value::CountBits, true, [](Gen& g, JsPtr<Expr> v) {
        auto count = [&](bool high) {
            auto node = make<CallExpr>(g, variable(g, g.bitCountHelpers[bitCountHelperSlot(Value::CountBits, false)]));
            node->args.push(g.file.arena, half(g, v, high));
            node->pure = true;
            return asExpr(g, node);
        };

        emit(g, make<ReturnStmt>(g, globalCall(g, "BigInt"_v,
                                               binary(g, BinaryOp::Add, count(false), count(true)))));
    });

    define(Value::LeadingZeros, true, [](Gen& g, JsPtr<Expr> v) {
        auto high = declare(g, literalName(g, "h"_v), half(g, v, true));
        auto below = binary(g, BinaryOp::Add, number(g, 32),
                            hostCall(g, "Math"_v, "clz32"_v, half(g, v, false)));

        emit(g, make<ReturnStmt>(g, globalCall(g, "BigInt"_v,
            ternary(g, high, hostCall(g, "Math"_v, "clz32"_v, high), below))));
    });

    define(Value::TrailingZeros, true, [](Gen& g, JsPtr<Expr> v) {
        auto scan = [&](JsPtr<Expr> of) {
            auto node = make<CallExpr>(g, variable(g, g.bitCountHelpers[bitCountHelperSlot(Value::TrailingZeros, false)]));
            node->args.push(g.file.arena, of);
            node->pure = true;
            return asExpr(g, node);
        };

        auto low = declare(g, literalName(g, "l"_v), half(g, v, false));
        auto above = binary(g, BinaryOp::Add, number(g, 32), scan(half(g, v, true)));

        emit(g, make<ReturnStmt>(g, globalCall(g, "BigInt"_v, ternary(g, low, scan(low), above))));
    });
}

/*
 * The ten rotations - see Gen::rotateHelpers.
 *
 *     function $rol32(v, c) { var n = c & 31;  return n ? (v << n) | (v >>> (32 - n)) : v }
 *     function $rol16(v, c) { var n = c & 15;  var x = v & 65535
 *                             return n ? (x << n) | (x >>> (16 - n)) : v }
 *     function $rol53(v, c) { var n = (c < 0 ? c + 9007199254740992 : c) % 53
 *                             return n ? $w53i$or($w53i$shl(v, n), $w53i$shr(v, 53 - n)) : v }
 *     function $rol64(v, c) { var n = BigInt.asUintN(64, c) % 64n
 *                             return n ? (v << n) | (BigInt.asUintN(64, v) >> (64n - n)) : v }
 *
 * ## The ternary is the whole argument, and it is not the zero case
 *
 * Every body guards on `n` and answers the operand unchanged for a zero count. That is not a
 * shortcut for a common case: it is what keeps the *other* arm's shift distances inside `[1, w-1]`,
 * where every host domain agrees about them. A count of zero would otherwise want a shift by the
 * full width, which is where the three domains stop agreeing - JS masks `>>>` to five bits, wide.cpp
 * would be asked for a distance it does not describe, and a `bigint` shift by 64 is the one that
 * *is* well defined. Guarding once is shorter than arguing three times.
 *
 * ## The modulus, and where the width is not a power of two
 *
 * Two of the five widths mask (`c & (w - 1)`), because a power of two's mask is its modulus and it
 * is the same operation the machine performs on its count register. `WideInt` is 53 bits and is the
 * one that cannot: the count is read as unsigned - which is what a negative `number` in the band
 * means as a bit pattern, and what `llvm.fshl` and the machine both do - and then reduced. The
 * native lowering does exactly this pair, with `maskToWidth` and a remainder.
 *
 * ## The three domains
 *
 * Below 33 bits the host operators are the operation and the mask is where the width lives: `v` may
 * be a negative `number` standing for a 16-bit pattern, so it is masked before either shift and the
 * caller's coercion signs the answer. At exactly 32 no mask is needed - the `int32` the operators
 * work in *is* the value. From 33 to 53 the operators do not exist and wide.cpp's helpers are what
 * a shift is; above that a value is a `bigint`, whose `>>` is arithmetic, so the right half reads
 * the unsigned form first for the reason the scalar `shr` does.
 */
/*
 * The six BMI2 helpers - see Gen::bitOpHelpers.
 *
 *     function $bzhi32(v, n) { return (n >>> 0) < 32 ? v & ((1 << n) - 1) : v }
 *     function $bzhi64(v, n) { var u = BigInt.asUintN(64, n)
 *                              return u < 64n ? v & ((1n << u) - 1n) : v }
 *     function $pext32(v, m) { var o = 0; var b = 1
 *                              $L: for(;;) { if(!m) break $L
 *                                            var l = m & -m
 *                                            o = o | ((v & l) ? b : 0)
 *                                            m = m ^ l; b = b << 1 }
 *                              return o }
 *     function $pdep32(v, m) { ... o = o | ((v & b) ? l : 0) ... }
 *
 * ## `bitsUpTo`, and why the guard is a ternary rather than a mask
 *
 * The operation saturates: a count at or above the width answers the value unchanged
 * (resolve/inst.def rules on it). JS masks a shift count to five bits, so `1 << 32` is 1 rather than
 * 0 and the mask arithmetic would answer zero where the language answers everything - which is
 * exactly the disagreement `bzhi`'s byte-wide count causes on the machine, arriving here through a
 * different door. The ternary is what makes the shift distance unreachable outside `[0, w-1]`, and
 * it is short-circuiting, so the out-of-range shift is never evaluated at all.
 *
 * `n >>> 0` is the unsigned reading, which is what the ruling says the count is: a negative `I32`
 * count is a large number and answers the value, not the value masked by a small one.
 *
 * ## The permutations, and the one line that keeps the loop finite
 *
 * Both loops walk the set bits of the mask lowest-first, and both terminate because `m ^= m & -m`
 * clears one bit per iteration. At 64 bits that is only true of the *unsigned* reading: a `bigint`'s
 * bitwise operators are defined on a two's complement representation extended infinitely, so a
 * negative mask has infinitely many set bits and the loop would never end. `BigInt.asUintN(64, m)`
 * before the loop is the whole of the fix, and it is the same conversion the 64-bit `shr` makes for
 * the same reason.
 *
 * A loop and not the parallel-suffix network the x64 backend expands to, because the trade is the
 * other way round here: there is no instruction to lose to, an engine's `bigint` operations are heap
 * arithmetic, and the loop runs once per set bit where the network runs five or six rounds whatever
 * the mask is.
 *
 * `var` inside the body rather than `let`, for the reason build.h gives beside `declare`: these are
 * function-scoped and re-entering the block re-assigns rather than re-declares.
 */
void emitBitOpHelpers(Gen& g) {
    auto define = [&](Value::Kind kind, bool wide, void (*body)(Gen&, bool, JsPtr<Expr>, JsPtr<Expr>)) {
        auto& name = g.bitOpHelpers[bitOpHelperSlot(kind, wide)];
        if(!name.text) return;

        auto function = make<FunStmt>(g, name);
        // The second parameter is a *count* for one of the three and a *mask* for the other two,
        // which is worth two letters in the emitted text: `$bzhi32(v, n)` and `$pext32(v, m)`.
        auto valueName = literalName(g, "v"_v);
        auto otherName = literalName(g, kind == Value::BitsUpTo ? "n"_v : "m"_v);
        function->args.push(g.file.arena, valueName);
        function->args.push(g.file.arena, otherName);

        function->body = collect(g, [&] {
            body(g, wide, variable(g, valueName), variable(g, otherName));
        });

        emit(g, function);
    };

    // A constant in whichever domain the width works in - a `number` below 33 bits and a `bigint`
    // above, which is the same split every arithmetic emitter here makes.
    static auto constant = [](Gen& g, bool wide, U64 n) {
        return wide ? bigInt(g, n, false) : number(g, F64(n));
    };

    static auto bitsUpTo = [](Gen& g, bool wide, JsPtr<Expr> value, JsPtr<Expr> count) {
        auto width = U64(wide ? 64 : 32);

        // The count as an unsigned number, which is what the operation's ruling reads it as.
        auto unsignedCount = wide
            ? declare(g, literalName(g, "u"_v),
                      hostCall(g, "BigInt"_v, "asUintN"_v, number(g, 64), count))
            : binary(g, BinaryOp::Shr, count, constant(g, false, 0));

        auto small = binary(g, BinaryOp::Lt, unsignedCount, constant(g, wide, width));
        auto bit = binary(g, BinaryOp::Shl, constant(g, wide, 1), unsignedCount);
        auto mask = binary(g, BinaryOp::Sub, bit, constant(g, wide, 1));

        emit(g, make<ReturnStmt>(g, ternary(g, small,
                                            binary(g, BinaryOp::And, value, mask), value)));
    };

    static auto permute = [](Gen& g, bool wide, bool gather, JsPtr<Expr> value, JsPtr<Expr> maskArg) {
        auto zero = constant(g, wide, 0);
        auto one = constant(g, wide, 1);

        // The mask read unsigned, without which a negative 64-bit one has infinitely many set bits
        // and the loop below never ends. Under a name of its own rather than over the parameter's,
        // which `var` would have allowed and which reads as a mistake.
        auto mask = wide
            ? declare(g, literalName(g, "u"_v),
                      hostCall(g, "BigInt"_v, "asUintN"_v, number(g, 64), maskArg))
            : maskArg;

        auto outName = literalName(g, "o"_v);
        auto bitName = literalName(g, "b"_v);
        auto out = declare(g, outName, zero);
        auto bit = declare(g, bitName, one);

        auto label = literalName(g, "$bp"_v);

        auto body = collect(g, [&] {
            auto done = collect(g, [&] { emit(g, make<BreakStmt>(g, label)); });
            emit(g, make<IfStmt>(g, unary(g, UnaryOp::Not, mask), done, StmtList {}));

            auto low = declare(g, literalName(g, "l"_v),
                               binary(g, BinaryOp::And, mask, unary(g, UnaryOp::Neg, mask)));

            // Which bit is tested and which is contributed is the whole of the difference between
            // the two directions: an extract asks the mask and answers the next low position, and a
            // deposit asks the next low position and answers the mask's.
            auto tested = binary(g, BinaryOp::And, value, gather ? low : bit);
            auto contributed = ternary(g, tested, gather ? bit : low, constant(g, wide, 0));

            emitExpr(g, assign(g, out, binary(g, BinaryOp::Or, out, contributed)));
            emitExpr(g, assign(g, mask, binary(g, BinaryOp::Xor, mask, low)));
            emitExpr(g, assign(g, bit, binary(g, BinaryOp::Shl, bit, constant(g, wide, 1))));
        });

        emit(g, make<LabelledStmt>(g, label, asStmt(g, make<ForeverStmt>(g, body))));
        emit(g, make<ReturnStmt>(g, out));
    };

    define(Value::BitsUpTo, false, [](Gen& g, bool wide, JsPtr<Expr> v, JsPtr<Expr> n) { bitsUpTo(g, wide, v, n); });
    define(Value::BitsUpTo, true, [](Gen& g, bool wide, JsPtr<Expr> v, JsPtr<Expr> n) { bitsUpTo(g, wide, v, n); });
    define(Value::GatherBits, false, [](Gen& g, bool wide, JsPtr<Expr> v, JsPtr<Expr> m) { permute(g, wide, true, v, m); });
    define(Value::GatherBits, true, [](Gen& g, bool wide, JsPtr<Expr> v, JsPtr<Expr> m) { permute(g, wide, true, v, m); });
    define(Value::ScatterBits, false, [](Gen& g, bool wide, JsPtr<Expr> v, JsPtr<Expr> m) { permute(g, wide, false, v, m); });
    define(Value::ScatterBits, true, [](Gen& g, bool wide, JsPtr<Expr> v, JsPtr<Expr> m) { permute(g, wide, false, v, m); });
}

void emitRotateHelpers(Gen& g) {
    auto define = [&](Value::Kind kind, U32 bits) {
        auto slot = rotateHelperSlot(kind, bits);
        auto& name = g.rotateHelpers[slot];
        if(!name.text) return;

        auto function = make<FunStmt>(g, name);
        auto valueName = literalName(g, "v"_v);
        auto countName = literalName(g, "c"_v);
        function->args.push(g.file.arena, valueName);
        function->args.push(g.file.arena, countName);

        auto left = kind == Value::Rol;
        auto wide = bits == 64;

        function->body = collect(g, [&] {
            auto value = variable(g, valueName);
            auto count = variable(g, countName);
            auto constant = [&](U64 n) { return wide ? bigInt(g, n, false) : number(g, F64(n)); };

            // The count, reduced. A power of two masks; 53 reads the count unsigned and divides.
            JsPtr<Expr> reduced;

            if(wide) {
                reduced = binary(g, BinaryOp::Rem,
                                 hostCall(g, "BigInt"_v, "asUintN"_v, number(g, 64), count),
                                 constant(64));
            } else if(bits == 53) {
                auto unsignedCount = ternary(g, binary(g, BinaryOp::Lt, count, number(g, 0)),
                                             binary(g, BinaryOp::Add, count, number(g, 9007199254740992.0)),
                                             count);

                reduced = binary(g, BinaryOp::Rem, unsignedCount, number(g, F64(bits)));
            } else {
                reduced = binary(g, BinaryOp::And, count, number(g, F64(bits - 1)));
            }

            auto n = declare(g, literalName(g, "n"_v), reduced);
            auto back = binary(g, BinaryOp::Sub, constant(bits), n);

            // The value's own bits, which below 32 are not all of the register's: a negative
            // `number` standing for a narrow pattern carries ones above its width, and a rotation
            // would bring them round into the answer where a shift only pushes them out.
            JsPtr<Expr> bitsOfValue = value;
            if(bits < 32) {
                bitsOfValue = declare(g, literalName(g, "x"_v),
                                      binary(g, BinaryOp::And, value, number(g, F64((U64(1) << bits) - 1))));
            }

            JsPtr<Expr> up, down;

            if(bits <= 32) {
                up = binary(g, BinaryOp::Shl, bitsOfValue, left ? n : back);
                down = binary(g, BinaryOp::Shr, bitsOfValue, left ? back : n);
            } else if(bits == 53) {
                up = wideCallAt(g, WideOp::Shl, bits, true, value, left ? n : back);
                down = wideCallAt(g, WideOp::Shr, bits, true, value, left ? back : n);
            } else {
                // `>>` on a `bigint` is arithmetic and there is no `>>>`, so the half travelling down
                // reads the unsigned form - the scalar `shr` does the same thing at the same width.
                auto unsignedValue = hostCall(g, "BigInt"_v, "asUintN"_v, number(g, 64), value);

                up = binary(g, BinaryOp::Shl, value, left ? n : back);
                down = binary(g, BinaryOp::Sar, unsignedValue, left ? back : n);
            }

            auto joined = bits == 53
                ? wideCallAt(g, WideOp::Or, bits, true, up, down)
                : binary(g, BinaryOp::Or, up, down);

            emit(g, make<ReturnStmt>(g, ternary(g, n, joined, value)));
        });

        emit(g, function);
    };

    static const U32 widths[] = { 8, 16, 32, 53, 64 };

    for(auto bits: widths) define(Value::Rol, bits);
    for(auto bits: widths) define(Value::Ror, bits);
}

void emitByteSwapHelpers(Gen& g) {
    auto define = [&](Size slot, U32 bits) {
        auto& name = g.byteSwapHelpers[slot];
        if(!name.text) return;

        auto function = make<FunStmt>(g, name);
        auto argument = literalName(g, "v"_v);
        auto value = variable(g, argument);
        function->args.push(g.file.arena, argument);

        auto wide = bits == 64;
        auto constant = [&](U64 n) { return wide ? bigInt(g, n, false) : number(g, F64(n)); };
        auto shift = [&](BinaryOp op, JsPtr<Expr> of, U32 by) {
            return by ? binary(g, op, of, constant(by)) : of;
        };

        JsPtr<Expr> result = nullptr;

        for(U32 i = 0; i < bits / 8; i++) {
            // Where this byte is and where the reversal puts it. A byte travelling up is masked
            // where it stands and shifted after; one travelling down is shifted first and masked in
            // its new place, so that neither the mask nor the shifted value is ever wider than the
            // type.
            auto from = i * 8;
            auto to = bits - 8 - from;

            auto term = to >= from
                ? shift(BinaryOp::Shl,
                        binary(g, BinaryOp::And, shift(BinaryOp::Sar, value, from), constant(0xff)), to)
                : binary(g, BinaryOp::And, shift(BinaryOp::Sar, value, from - to), constant(U64(0xff) << to));

            result = result ? binary(g, BinaryOp::Or, result, term) : term;
        }

        function->body = collect(g, [&] { emit(g, make<ReturnStmt>(g, result)); });
        emit(g, function);
    };

    define(0, 16);
    define(1, 32);
    define(2, 64);
}

/*
 * `function $div(a, b) { return b ? a / b : b }` and `function $rem(a, b) { return b ? a % b : a }`.
 *
 * The zero arm of the division returns the *divisor* rather than a written zero, which is what lets
 * one function serve a `number` and a `bigint` both: `0` and `0n` are each their own type's zero,
 * and the caller's coercion is happy with either. `b` as the condition rather than `b !== 0` for the
 * same reason - falsy is true of both zeros and of no other integer this backend produces.
 *
 * `a / b` is left untruncated deliberately. Every caller already applies the truncation its band
 * needs - `Math.trunc` in the 33-to-53-bit band, a bitwise coercion below it, and nothing at all for
 * a `bigint`, whose division truncates itself - so doing it here would be doing it twice.
 */
void emitDivisionHelpers(Gen& g) {
    auto define = [&](Name& name, BinaryOp op, bool zeroIsDividend) {
        if(!name.text) return;

        auto function = make<FunStmt>(g, name);
        auto left = literalName(g, "a"_v);
        auto right = literalName(g, "b"_v);
        function->args.push(g.file.arena, left);
        function->args.push(g.file.arena, right);

        auto a = variable(g, left);
        auto b = variable(g, right);

        function->body = collect(g, [&] {
            emit(g, make<ReturnStmt>(g, ternary(g, b, binary(g, op, a, b),
                                                zeroIsDividend ? a : b)));
        });

        emit(g, function);
    };

    if(g.divideByZeroHelper.text || g.remainderByZeroHelper.text) {
        emit(g, make<CommentStmt>(g, internText(g,
            "`x / 0` is 0 and `x % 0` is x - see doc/spec/types.md"_v)));
    }

    define(g.divideByZeroHelper, BinaryOp::Div, false);
    define(g.remainderByZeroHelper, BinaryOp::Rem, true);
}

/*
 * `function $grow(a, n) { var b = new a.constructor(n); b.set(a); return b; }`
 *
 * One helper for every element type, because the array carries its own constructor. `.set` is the
 * typed array's own bulk copy and is what the engine has a memcpy behind; writing the loop here
 * would be the same operation spelled so that it cannot be recognised.
 */
void emitGrowHelper(Gen& g) {
    if(!g.growHelper.text) return;

    emit(g, make<CommentStmt>(g, internText(g,
        "a typed array's growth - see Implementation-Containers.md, section 14"_v)));

    auto function = make<FunStmt>(g, g.growHelper);
    auto array = literalName(g, "a"_v);
    auto capacity = literalName(g, "n"_v);
    auto grown = literalName(g, "b"_v);

    function->args.push(g.file.arena, array);
    function->args.push(g.file.arena, capacity);

    function->body = collect(g, [&] {
        auto made = make<CallExpr>(g, field(g, variable(g, array), literalName(g, "constructor"_v)));
        made->args.push(g.file.arena, variable(g, capacity));
        made->construct = true;

        emit(g, make<DeclStmt>(g, grown, asExpr(g, made), true));

        auto copy = make<CallExpr>(g, field(g, variable(g, grown), literalName(g, "set"_v)));
        copy->args.push(g.file.arena, variable(g, array));
        emitExpr(g, asExpr(g, copy));

        emit(g, make<ReturnStmt>(g, variable(g, grown)));
    });

    emit(g, function);
}

/*
 * A host array holding `elements`, in whichever of §14's two rows this element type belongs to.
 *
 * `[a, b, c]` for an element the host boxes, and `new Int32Array([a, b, c])` for one it does not -
 * which is the whole of the difference between the two rows. It is one function because there are
 * three producers of a host array and they have to make the same choice: the storage of an
 * `Array(a)`, the zero of a `[T *n]`, and a written literal of either. `typedArrayFor` is the
 * choice; nothing here decides anything.
 *
 * The fixed array was the reader that disagreed, and it was silent: a `[U8 *64]` was a plain host
 * array of sixty-four boxed zeroes, where an `Array(U8)` of the same contents was a `Uint8Array`.
 * That cost eight bytes an element and, once the word transfers existed, meant a byte buffer the
 * host's own `DataView` could not be pointed at - see NativeOp::HostWordRead, which is what made the
 * disagreement visible.
 *
 * `pure` in the sense the flag means - reads nothing, writes nothing. A typed array's constructor
 * allocates, which no other expression can observe, and copies out of an argument whose own effects
 * the walk still visits. It is load-bearing rather than tidy: the empty array a literal is built
 * into is overwritten by the literal in the next statement, and `foldInitialValue` is what makes the
 * two one - but only for an initializer nothing has to be ordered against.
 */
JsPtr<Expr> hostArrayForElement(Gen& g, TypePtr element, JsPtr<Expr> elements) {
    auto typed = typedArrayFor(g.global, element);
    if(!typed.length) return elements;

    auto node = make<CallExpr>(g, variable(g, literalName(g, typed)));
    node->args.push(g.file.arena, elements);
    node->construct = true;
    node->pure = true;

    return asExpr(g, node);
}

/*
 * The word transfers, and the one thing that makes them worth having:
 *
 *     function $view(a) { return a.$dv || (a.$dv = new DataView(a.buffer, a.byteOffset)); }
 *     function $readU32(a, i, e) { return $view(a).getUint32(i, e); }
 *     function $writeU64(a, i, v, e) { $view(a).setBigUint64(i, v, e); }
 *
 * **The view is built once per array and kept on it.** That is not a tidy-up: `new DataView` at each
 * call measures 134 ms against the 9.7 ms of the shift chain these replace, so a form without a
 * cache is fifteen times *worse* than doing nothing. Nor is the cache free to site anywhere - a
 * one-entry memo on `.buffer` is 33 ms, and 352 ms once a loop alternates between two arrays, which
 * is what a digest reading a message and writing a pad does; a `WeakMap` is 27 ms on a write where
 * four stores are 4.6. The property is the only one of the four that is not slower than the shifts.
 *
 * It costs the array nothing measurable: an element sum over a `Uint8Array` carrying `$dv` is 8.4 ms
 * against 8.4 ms without it, and the shift chain over one is 9.8 against 9.7. The map transitions
 * once and stays monomorphic.
 *
 * **What it buys is the 64-bit pair.** At 32 bits the host and the shifts measure the same (9.7 both
 * ways on a read; the write is 2.3 against 4.6). At 64 bits a `U64` is a `bigint` here, so the
 * alternative to `getBigUint64` is composing one out of two 32-bit halves - 13.6 ms against 7.1 -
 * and the alternative to `setBigUint64` is eight stores and eight shifts in the `bigint` domain,
 * which is 52 ms against 7.1. All figures Node 24, 64 KB, best of repeated runs.
 *
 * `$view` is a call rather than the expression inlined into each of the four, for the reason the
 * user of this file will care about: it is the same speed - 9.8 ms against 9.7 - and it is one
 * function instead of four copies of the same three operations.
 */
void emitWordHelpers(Gen& g) {
    auto wanted = false;
    for(U32 i = 0; i < 3; i++) {
        wanted = wanted || g.readWordHelper[i].text || g.writeWordHelper[i].text;
    }

    if(!wanted) return;

    emit(g, make<CommentStmt>(g, internText(g,
        "the machine words of a binary format, through one DataView per array"_v)));

    g.viewHelper = uniqueName(g, "$view"_v, false);

    // `function $view(a) { return a.$dv || (a.$dv = new DataView(a.buffer, a.byteOffset)); }`
    {
        auto function = make<FunStmt>(g, g.viewHelper);
        auto array = literalName(g, "a"_v);
        function->args.push(g.file.arena, array);

        function->body = collect(g, [&] {
            auto slot = field(g, variable(g, array), literalName(g, "$dv"_v));

            auto made = make<CallExpr>(g, variable(g, literalName(g, "DataView"_v)));
            made->args.push(g.file.arena, field(g, variable(g, array), literalName(g, "buffer"_v)));
            made->args.push(g.file.arena, field(g, variable(g, array), literalName(g, "byteOffset"_v)));
            made->construct = true;

            auto stored = asExpr(g, make<AssignExpr>(g, slot, asExpr(g, made)));
            emit(g, make<ReturnStmt>(g, asExpr(g, make<BinaryExpr>(g, BinaryOp::LogicalOr, slot, stored))));
        });

        emit(g, function);
    }

    auto define = [&](Name name, StringView accessor, bool read) {
        if(!name.text) return;

        auto function = make<FunStmt>(g, name);
        auto array = literalName(g, "a"_v);
        auto index = literalName(g, "i"_v);
        auto amount = literalName(g, "v"_v);
        auto order = literalName(g, "e"_v);

        function->args.push(g.file.arena, array);
        function->args.push(g.file.arena, index);
        if(!read) function->args.push(g.file.arena, amount);
        function->args.push(g.file.arena, order);

        function->body = collect(g, [&] {
            auto view = make<CallExpr>(g, variable(g, g.viewHelper));
            view->args.push(g.file.arena, variable(g, array));

            auto call = make<CallExpr>(g, field(g, asExpr(g, view), literalName(g, accessor)));
            call->args.push(g.file.arena, variable(g, index));
            if(!read) call->args.push(g.file.arena, variable(g, amount));
            call->args.push(g.file.arena, variable(g, order));

            if(read) emit(g, make<ReturnStmt>(g, asExpr(g, call)));
            else emitExpr(g, asExpr(g, call));
        });

        emit(g, function);
    };

    define(g.readWordHelper[0], "getUint16"_v, true);
    define(g.readWordHelper[1], "getUint32"_v, true);
    define(g.readWordHelper[2], "getBigUint64"_v, true);
    define(g.writeWordHelper[0], "setUint16"_v, false);
    define(g.writeWordHelper[1], "setUint32"_v, false);
    define(g.writeWordHelper[2], "setBigUint64"_v, false);
}

// Declared in build.h, which is where the reasoning lives: two callers read this and they must
// not disagree. At js scope rather than in the anonymous namespace above because gen.cpp is the
// other one.
AggregateBuildPlan wholeLocalPlan(Gen& g, InstAggregate& aggregate) {
    AggregateBuildPlan plan;

    auto& place = aggregate.place;
    if(place.root != PlaceRoot::Local || place.projections.size() > 1) return plan;

    auto type = g.function->localAt(g.local, place.local).type;
    if(!type) return plan;

    /*
     * A `[T *n]`, which is a host array here - the same literal a growable one is built as, and the
     * elements are every slot the type has by construction: the resolver rejects a literal whose
     * length disagrees with the type's before this is reached.
     */
    if(g.global[type]->kind == Type::Array) {
        if(place.projections.isNotEmpty()) return plan;
        if(aggregate.components.size() != constValue(g.global, ((ArrayType*)g.global[type])->count)) return plan;

        auto stepped = true;
        eachAggregateComponent(g.local, aggregate, [&](const AggregateComponent& component, Size) {
            stepped = stepped && component.step.kind == ProjectionKind::Index;
        });

        if(!stepped) return plan;

        plan.kind = AggregateBuildPlan::Array;
        plan.type = type;
        plan.eligible = true;
        return plan;
    }

    if(!isJsObject(g, type)) return plan;

    /*
     * A newtype has no object of its own to build - it *is* the value it wraps (see isNewtype), so
     * its one component is the whole answer and the ordinary per-component write says exactly that:
     * the place walk elides the `Field` step and assigns the local.
     *
     * Declined here rather than handled, because the plan below assembles a literal out of the
     * *type's* properties by name - and a newtype's properties are the wrapped type's. `data Ring
     * {items: [Int]}` filled `Array(a)`'s `items` with the whole array and zero-filled its `length`,
     * which is a container of three elements reporting that it holds none. It read as if it worked
     * because the two field names agree, and it was invisible while `Array(a)` was a newtype itself
     * on this target: the wrapped type had no properties at all, so there was no literal to build.
     */
    if(TypePtr wrapped = nullptr; isNewtype(g, type, wrapped)) return plan;

    auto record = recordType(g, type);

    // The tuple whose fields a `Field` step names: the constructor's payload where the aggregate
    // steps through one, and otherwise whatever the place already arrives at.
    auto content = aggregate.constructor != maxLimit<U16> && record &&
                   aggregate.constructor < record->constructors.size()
        ? record->constructors.get(g.global, aggregate.constructor).content
        : placeType(g, place);

    /*
     * Which property each component fills, by name.
     *
     * Names rather than positions, because the two do not line up for a sum: the object carries
     * *every* constructor's properties - one hidden class for the type, which is what §2.3 buys -
     * and a construction supplies one constructor's. So the components are named here and the
     * literal is assembled by walking the type's own properties.
     */
    auto declined = false;

    eachAggregateComponent(g.local, aggregate, [&](const AggregateComponent& component, Size at) {
        if(declined) return;

        auto step = component.step;
        auto named = [&](Name key) { plan.filled.push(AggregateBuildPlan::Filled { key, at }); };

        if(step.kind == ProjectionKind::Discriminant) {
            named(g.tagField);
            return;
        }

        if(step.kind == ProjectionKind::Downcast) {
            /*
             * A payload written whole. It is one property only where `payloadIsOneProperty` says
             * so - a flattened tuple payload is several, and one value cannot fill several without
             * taking the record apart, which is a store per field and not this.
             */
            auto payload = record && step.index < record->constructors.size()
                ? record->constructors.get(g.global, step.index).content
                : nullptr;

            if(!payload || isUnit(g.global, payload) || !payloadIsOneProperty(g, payload)) {
                declined = true;
                return;
            }

            named(g.payloadField);
            return;
        }

        if(step.kind != ProjectionKind::Field || !content ||
           g.global[content]->kind != Type::Tup) {
            declined = true;
            return;
        }

        auto property = fieldProperty(g, content, step.index);
        if(!property.leader || property.fun) {
            declined = true;
            return;
        }

        named(property.name);
    });

    if(declined) return plan;

    /*
     * Every component has to name a property the type has. One that does not - a co-packed field is
     * the shape - would be dropped from the literal, which is a wrong value rather than a slow one.
     */
    Size found = 0;
    eachProperty(g, type, [&](Name key, TypePtr) {
        for(auto& filled: plan.filled) {
            if(filled.key.text == key.text) { found++; return; }
        }
    });

    if(found != plan.filled.size()) return plan;

    plan.kind = AggregateBuildPlan::Object;
    plan.type = type;
    plan.eligible = true;
    return plan;
}

JsPtr<Expr> buildFromPlan(Gen& g, InstAggregate& aggregate, const AggregateBuildPlan& plan) {
    // Each component on the terms `storeInto` writes one on: a literal is storage being filled, so
    // a component that is still somebody else's storage is duplicated into it. See keptValue.
    auto value = [&](Size at) {
        auto component = aggregate.components.get(g.local, at).value;
        auto type = g.local[component]->type;

        return keptValue(g, type, component, useValue(g, component), aggregate.source);
    };

    if(plan.kind == AggregateBuildPlan::Array) {
        auto elements = make<ArrayExpr>(g);
        for(Size i = 0; i < aggregate.components.size(); i++) {
            elements->values.push(g.file.arena, value(i));
        }

        // In §14's typed row where the element belongs to it, which is the same choice `zeroValue`
        // makes for the storage this literal is written over - see hostArrayForElement. The two
        // disagreeing is exactly the defect that rule exists to prevent.
        auto array = (ArrayType*)g.global[plan.type];
        return hostArrayForElement(g, array->content, asExpr(g, elements));
    }

    /*
     * The literal, in the type's own property order - which is the order every other value of the
     * type is built in, and the whole point of building one here rather than assigning properties.
     *
     * A property no component fills is one this construction does not reach: the tag of a record
     * that has none, and the payload of every constructor other than the one being built. Those are
     * the zeros that stay, and they are why `zeroValue` is not gone - see build.h.
     */
    auto object = make<ObjectExpr>(g);

    eachProperty(g, plan.type, [&](Name key, TypePtr member) {
        for(auto& filled: plan.filled) {
            if(filled.key.text != key.text) continue;

            object->properties.push(g.file.arena, Property { key, value(filled.at) });
            return;
        }

        object->properties.push(g.file.arena, Property { key, zeroValue(g, member) });
    });

    return asExpr(g, object);
}

JsPtr<Expr> functionValue(Gen& g, ModulePtr<Function> callee, LocationId where) {
    if(g.excluded.contains(U32(callee))) {
        g.context.diagnostics.error("%@ cannot be compiled for the JS target - it is `Native`, or it reaches something that is"_v,
                                    where, g.context.findName(g.local[callee]->name));
        return nullValue(g);
    }

    auto found = g.functionNames.get(U32(callee));
    if(!found) {
        g.context.diagnostics.error("internal error: no JS name for %@"_v, where,
                                    g.context.findName(g.local[callee]->name));
        return nullValue(g);
    }

    return variable(g, found.unwrap());
}

/*
 * Vectors - Implementation-Vector.md §7.
 *
 * A vector is `lanes` values here, so every operation over one is `lanes` operations and the array
 * form is what does *not* get built. That is the whole of this target's vector story, and it is why
 * these are written apart from the scalar emitters above rather than by threading a lane index
 * through them: what a lane needs is an expression, and every emitter above defines a value.
 *
 * The lane types are a strict subset of the scalar ones, which is what keeps the arithmetic here to
 * one function where `genBinary` is thirty. A lane is an integer or a float of 8, 16, 32 or 64 bits
 * (`resolveVectorType`), a 64-bit integer lane is refused outright on this target (Design-Vector
 * §7.3), and a `Bool` is not a lane at all - so the three cases `genBinary`'s length is about, the
 * `bigint`s, the 33-to-53-bit band and `Bool`'s bitwise reading, cannot arise.
 *
 * A mask is `lanes` host booleans, per §7. That is a different representation from the lane-width
 * all-ones the native Repr describes, in the same way and for the same reason LLVM's `<N x i1>` is:
 * a mask is only ever produced by a comparison, combined by `and`/`or`/`not` and consumed by a
 * select or a reduction, and a boolean is what each of those is cheapest over here.
 */

// Where a value's lanes are declared, so that a lane is a variable rather than a repeated
// expression. `x$0`, `x$1`, ... beside the `x$c`/`x$e` a function value's words already get.
/*
 * One lane converted from one lane type to another, as an expression.
 *
 * The scalar `genCast` is thirty cases because a scalar can be a `bigint`, a `Bool`, a pointer or a
 * value in the 33-to-53-bit band. A lane can be none of those, so what is left is the two: a float
 * narrowed into an integer saturates - the same `Math.trunc`-and-clamp the scalar path emits, from
 * the same function - and everything else is the coercion the target type needs anyway.
 */
static JsPtr<Expr> convertLane(Gen& g, TypePtr from, TypePtr to, JsPtr<Expr> value) {
    /*
     * A 64-bit integer lane is a `bigint` and every other lane is a `number`, so a conversion that
     * crosses between them is a real one rather than a coercion - which is `genCast`'s rule at
     * §2.1, asked here of a lane instead of a scalar and answered by the same three helpers.
     *
     * The float-to-bigint direction is the one that is not optional: `BigInt(NaN)` and
     * `BigInt(Infinity)` *throw*, so an unsaturated conversion is a crash rather than a
     * disagreement with the native targets. `saturatingToLong` is the scalar path's own helper.
     */
    auto fromLong = isLong(g, from);
    auto toLong = isLong(g, to);

    if(toLong && !fromLong) {
        if(isFloat(g.global, from)) return saturatingToLong(g, to, value);
        return coerce(g, to, globalCall(g, "BigInt"_v, value));
    }

    if(fromLong && !toLong) {
        auto asNumber = globalCall(g, "Number"_v, value);
        return isFloat(g.global, to) ? asNumber : coerce(g, to, asNumber);
    }

    if(isFloat(g.global, from) && !isFloat(g.global, to)) {
        return coerce(g, to, saturatingToNumber(g, to, value));
    }

    return coerce(g, to, value);
}

/*
 * Declares one value's lanes and records them.
 *
 * Every lane gets a `var` rather than being left as the expression that produced it, on exactly the
 * grounds `define` names one for a scalar: how many readers a lane has is a fact about the rest of
 * the function, and `opt.cpp` takes the name back where there is one reader. Leaving them unnamed
 * would duplicate the whole expression tree per use, which for a vector is the one thing that turns
 * a win into a loss.
 */
static void defineVec(Gen& g, ModulePtr<Value> pointer, VecParts parts) {
    auto& source = *g.local[pointer];

    for(U32 i = 0; i < parts.count; i++) {
        parts.lanes[i] = declare(g, laneName(g, source, i), parts.lanes[i]);
    }

    g.vecParts.add(U32(pointer), parts);
}

// The lane type of a vector or a mask, and how many lanes it has. A mask's lane type is the integer
// it masks, which nothing here computes with - what a mask lane holds is a host boolean.
static TypePtr laneType(Gen& g, TypePtr type) { return vectorLane(g.global, type); }
static U32 laneCount(Gen& g, TypePtr type) { return vectorLanes(g.global, type); }

static bool isMaskType(Gen& g, TypePtr type) {
    auto value = g.global[type];
    return value->kind == Type::Vector && ((VectorType*)value)->isMask;
}

// One lane of an arithmetic or bitwise operation. `coerce` is what narrows the result back into the
// lane's width, exactly as it does for a scalar of the same type - a `Vec(U8)` lane is a `number`
// masked to eight bits, and the mask is where the lane width lives on this target.
static JsPtr<Expr> laneBinary(Gen& g, Value::Kind kind, TypePtr type, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    auto integer = intType(g, type);

    auto simple = [&](BinaryOp op) { return coerce(g, type, binary(g, op, lhs, rhs)); };

    switch(kind) {
        case Value::Add: return simple(BinaryOp::Add);
        case Value::Sub: return simple(BinaryOp::Sub);
        case Value::Mul:
            // `Math.imul` for the one width where a double multiply answers different low bits -
            // §2.1's one unconditional coercion, and the same call the scalar path makes.
            if(isInt32Class(g, integer)) {
                return coerce(g, type, hostCall(g, "Math"_v, "imul"_v, lhs, rhs));
            }

            return simple(BinaryOp::Mul);
        /*
         * The two divisors the language answers for, lane by lane - the same rule the scalar path
         * applies and the same two helpers, split the same way. See Gen::divideByZeroHelper.
         *
         * A float lane is left alone at both: it divides to an IEEE infinity, which is the answer.
         * An integer lane narrower than 64 bits needs no help with the *quotient* either, the
         * coercion above turning that infinity into the 0 the rule asks for - `Infinity & 255` is 0
         * as surely as `Infinity | 0` is. A `Long` lane is a `bigint` and would throw, and no width
         * gets the remainder right on its own.
         *
         * No native backend reaches this: `unsupportedVectorReason` refuses a packed integer
         * division at every width and level, so a vector divides only where the lanes are already
         * ordinary variables.
         */
        case Value::Div:
            if(isInt64Class(g, integer)) {
                return coerce(g, type, divideCall(g, lhs, rhs));
            }

            return simple(BinaryOp::Div);
        case Value::Rem:
            if(integer) return coerce(g, type, remainderCall(g, lhs, rhs));

            return simple(BinaryOp::Rem);
        case Value::Shl: return simple(BinaryOp::Shl);
        // A rotation within the lane, which is the scalar helper at the lane's own width - see
        // `rotateCall`. Nothing native reaches this: `expandVectorRotate` rewrites a packed rotation
        // into shifts long before selection, so a vector rotates only where the lanes are already
        // ordinary variables.
        case Value::Rol:
        case Value::Ror: return coerce(g, type, rotateCall(g, kind, type, lhs, rhs));
        case Value::Shr:
            // A logical right shift of a 64-bit lane goes through the unsigned reading first, for
            // the reason the scalar path does it: `>>>` is not defined on BigInt at all, and a lane
            // of one is what `Vec(Long)` is made of.
            if(isLong(g, type)) {
                auto unsignedValue = hostCall(g, "BigInt"_v, "asUintN"_v, number(g, 64), lhs);
                return coerce(g, type, binary(g, BinaryOp::Sar, unsignedValue, rhs));
            }

            return simple(BinaryOp::Shr);
        case Value::Sar: return simple(BinaryOp::Sar);
        case Value::And: return simple(BinaryOp::And);
        case Value::Or:  return simple(BinaryOp::Or);
        default:         return simple(BinaryOp::Xor);
    }
}

// The same over a mask, whose lanes are booleans: `&`, `|` and `^` over two of those would answer
// numbers, so the logical operators are what the three bitwise kinds mean here.
static JsPtr<Expr> maskBinary(Gen& g, Value::Kind kind, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    switch(kind) {
        case Value::And: return binary(g, BinaryOp::LogicalAnd, lhs, rhs);
        case Value::Or:  return binary(g, BinaryOp::LogicalOr, lhs, rhs);
        default:         return binary(g, BinaryOp::Ne, lhs, rhs);   // xor of two booleans
    }
}

static void genVecBinary(Gen& g, ModulePtr<Value> pointer, InstBinary& instruction) {
    auto type = instruction.type;
    auto lanes = laneCount(g, type);
    auto lhs = vecPartsOf(g, instruction.lhs);
    auto rhs = vecPartsOf(g, instruction.rhs);
    auto parts = newVecParts(g, lanes);
    auto mask = isMaskType(g, type);
    auto element = laneType(g, type);

    for(U32 i = 0; i < lanes; i++) {
        parts.lanes[i] = mask ? maskBinary(g, instruction.kind, lhs.lanes[i], rhs.lanes[i])
                              : laneBinary(g, instruction.kind, element, lhs.lanes[i], rhs.lanes[i]);
    }

    defineVec(g, pointer, parts);
}

static void genVecUnary(Gen& g, ModulePtr<Value> pointer, InstUnary& instruction) {
    auto type = instruction.type;
    auto lanes = laneCount(g, type);
    auto from = vecPartsOf(g, instruction.from);
    auto parts = newVecParts(g, lanes);
    auto element = laneType(g, type);

    for(U32 i = 0; i < lanes; i++) {
        auto lane = from.lanes[i];

        if(isMaskType(g, type)) {
            // `not` is the only unary a mask takes, and over a boolean it is `!`.
            parts.lanes[i] = unary(g, UnaryOp::Not, lane);
        } else if(instruction.kind == Value::Neg) {
            parts.lanes[i] = coerce(g, element, unary(g, UnaryOp::Neg, lane));
        } else if(instruction.kind == Value::Sqrt) {
            parts.lanes[i] = coerce(g, element, hostCall(g, "Math"_v, "sqrt"_v, lane));
        } else if(instruction.kind == Value::Trunc || instruction.kind == Value::Floor
                  || instruction.kind == Value::Ceil || instruction.kind == Value::Round) {
            parts.lanes[i] = coerce(g, element, roundingLane(g, instruction.kind, lane));
        } else if(instruction.kind == Value::Abs) {
            /*
             * `Math.abs` is the magnitude at every lane kind this target has except one, and agrees
             * with what every native target does with one - `+0` for either zero, and a NaN whose
             * sign is not a thing JavaScript has an opinion about. See the `Abs` row in
             * resolve/inst.def.
             *
             * The exception is a 64-bit lane, which is a `bigint`: `Math.abs` coerces its argument
             * to a number and *throws* on one. The conditional is the magnitude there, and it needs
             * no zero case - a `bigint` has one zero and no sign on it.
             */
            if(isLong(g, element)) {
                parts.lanes[i] = coerce(g, element,
                    ternary(g, binary(g, BinaryOp::Lt, lane, bigInt(g, 0, true)),
                            unary(g, UnaryOp::Neg, lane), lane));
            } else {
                parts.lanes[i] = coerce(g, element, hostCall(g, "Math"_v, "abs"_v, lane));
            }
        } else {
            parts.lanes[i] = coerce(g, element, unary(g, UnaryOp::BitNot, lane));
        }
    }

    defineVec(g, pointer, parts);
}

/*
 * `a * b + c` per lane, at two roundings.
 *
 * There is no fused multiply-add on this target: `Math.fround` rounds and nothing multiplies and
 * adds without rounding in between. Design-Vector §3.3 makes that a permitted answer rather than a
 * gap - `fma` is permission to fuse, so a target with no fused instruction spends it as the two
 * operations - and it is the same answer the x64 backend gives below FMA3.
 *
 * The coercion is the lane's own, which for an `f32` lane is the `Math.fround` that makes it one:
 * without it the product would be computed at double precision and the sum would be too, which is
 * not two roundings of a float expression but none.
 */
static void genVecFma(Gen& g, ModulePtr<Value> pointer, InstFma& instruction) {
    auto type = instruction.type;
    auto lanes = laneCount(g, type);
    auto element = laneType(g, type);
    auto a = vecPartsOf(g, instruction.a);
    auto b = vecPartsOf(g, instruction.b);
    auto c = vecPartsOf(g, instruction.c);
    auto parts = newVecParts(g, lanes);

    for(U32 i = 0; i < lanes; i++) {
        auto product = coerce(g, element, binary(g, BinaryOp::Mul, a.lanes[i], b.lanes[i]));
        parts.lanes[i] = coerce(g, element, binary(g, BinaryOp::Add, product, c.lanes[i]));
    }

    defineVec(g, pointer, parts);
}

// A comparison of two vectors answers a mask, which here is `lanes` booleans - so this is the one
// place the operators are the host's own comparison rather than anything narrowed.
static void genVecCmp(Gen& g, ModulePtr<Value> pointer, InstCmp& instruction) {
    auto lanes = laneCount(g, instruction.type);
    auto lhs = vecPartsOf(g, instruction.lhs);
    auto rhs = vecPartsOf(g, instruction.rhs);
    auto parts = newVecParts(g, lanes);

    BinaryOp op;
    switch(instruction.cmp) {
        case CompareOp::Eq: op = BinaryOp::Eq; break;
        case CompareOp::Ne: op = BinaryOp::Ne; break;
        case CompareOp::Gt: op = BinaryOp::Gt; break;
        case CompareOp::Ge: op = BinaryOp::Ge; break;
        case CompareOp::Lt: op = BinaryOp::Lt; break;
        default: op = BinaryOp::Le; break;
    }

    for(U32 i = 0; i < lanes; i++) parts.lanes[i] = binary(g, op, lhs.lanes[i], rhs.lanes[i]);
    defineVec(g, pointer, parts);
}

// The lane-wise select: `lanes` ternaries over a mask's booleans, which is the same instruction the
// scalar select is with a condition per lane instead of one for the whole value.
static void genVecSelect(Gen& g, ModulePtr<Value> pointer, InstSelect& instruction) {
    auto lanes = laneCount(g, instruction.type);
    auto cond = vecPartsOf(g, instruction.cond);
    auto whenTrue = vecPartsOf(g, instruction.whenTrue);
    auto whenFalse = vecPartsOf(g, instruction.whenFalse);
    auto parts = newVecParts(g, lanes);

    for(U32 i = 0; i < lanes; i++) {
        parts.lanes[i] = ternary(g, cond.lanes[i], whenTrue.lanes[i], whenFalse.lanes[i]);
    }

    defineVec(g, pointer, parts);
}

/*
 * The five kinds that only a vector is an operand or a result of.
 *
 * Four of them are nothing at all once a vector is its lanes: a splat is one expression named
 * `lanes` times, a lane read is one of the parts, a lane write is the parts with one replaced, and a
 * shuffle is the parts permuted. None of them emits an operation - which is the clearest statement
 * of what this representation buys, since each is a real instruction on the machine.
 */
static void genVecSplat(Gen& g, ModulePtr<Value> pointer, InstVecSplat& instruction) {
    auto lanes = laneCount(g, instruction.type);
    auto from = useValue(g, instruction.from);
    auto parts = newVecParts(g, lanes);

    // The same expression in every lane. `defineVec` names each of them, so a source with an effect
    // or a cost is evaluated `lanes` times unless `opt.cpp` can see that it is a name - which is why
    // `lower_licm` hoisting a splat out of a loop is worth what §3.4 says it is.
    for(U32 i = 0; i < lanes; i++) parts.lanes[i] = from;
    defineVec(g, pointer, parts);
}

static void genVecLane(Gen& g, ModulePtr<Value> pointer, InstVecLane& instruction) {
    auto from = vecPartsOf(g, instruction.from);
    auto lane = instruction.lane < from.count ? instruction.lane : 0;

    if(instruction.kind == Value::VecLane) {
        define(g, pointer, from.lanes[lane]);
        return;
    }

    auto parts = newVecParts(g, from.count);
    for(U32 i = 0; i < from.count; i++) parts.lanes[i] = from.lanes[i];

    parts.lanes[lane] = useValue(g, instruction.value);
    defineVec(g, pointer, parts);
}

static void genVecShuffle(Gen& g, ModulePtr<Value> pointer, InstVecShuffle& instruction) {
    auto lanes = laneCount(g, instruction.type);
    auto left = vecPartsOf(g, instruction.left);
    auto right = vecPartsOf(g, instruction.right);
    auto parts = newVecParts(g, lanes);

    // A pattern entry names a lane of the two sources concatenated, so anything from `left.count` up
    // is a lane of the second - the same reading every backend gives it.
    for(U32 i = 0; i < lanes; i++) {
        auto entry = i < instruction.pattern.size() ? instruction.pattern[i] : 0;
        parts.lanes[i] = entry < left.count ? left.lanes[entry]
                       : (entry - left.count) < right.count ? right.lanes[entry - left.count]
                       : left.lanes[0];
    }

    defineVec(g, pointer, parts);
}

/*
 * The one that is not free: every lane combined into one scalar.
 *
 * An unrolled adjacent-pair tree - `(a0+a1) + (a2+a3)` for four lanes - and not a loop, and not a
 * left-to-right fold. The order is a stated language property (Design-Vector §4.5), so what this
 * emits has to be the same tree the other two backends emit or a float sum answers different bits on
 * different targets. Integer reductions are associative and could be written any way; they are
 * written this way because two shapes for one operation is how the two drift apart.
 */
static JsPtr<Expr> reduceLanes(Gen& g, ReduceOp reduce, TypePtr type, bool mask,
                               Buffer<JsPtr<Expr>> lanes, U32 from, U32 count) {
    if(count == 1) return lanes[from];

    auto half = count / 2;
    auto lhs = reduceLanes(g, reduce, type, mask, lanes, from, half);
    auto rhs = reduceLanes(g, reduce, type, mask, lanes, from + half, count - half);

    if(mask) {
        // `any` and `all` over booleans, and `count` as a sum of them - which is where the boolean
        // representation costs something, since `+` over two of them needs each read as a number.
        switch(reduce) {
            case ReduceOp::And: return binary(g, BinaryOp::LogicalAnd, lhs, rhs);
            case ReduceOp::Or:  return binary(g, BinaryOp::LogicalOr, lhs, rhs);
            default: break;
        }

        auto asNumber = [&](JsPtr<Expr> lane) { return ternary(g, lane, number(g, 1), number(g, 0)); };
        return binary(g, BinaryOp::Add, count == 2 ? asNumber(lhs) : lhs,
                                        count == 2 ? asNumber(rhs) : rhs);
    }

    /*
     * A 64-bit lane is a `bigint`, and `Math.min`/`Math.max` coerce their arguments to numbers and
     * *throw* on one - so the extremum is the comparison it always was, written out. The sum and
     * the product need no such arm: `+` and `*` are defined on two bigints and `coerce` narrows the
     * result the same way it does at every other lane width.
     *
     * No NaN question arises here, unlike the float lanes below: a `bigint` has no NaN, so the
     * comparison is total and either order of the operands answers the same thing.
     */
    if(isLong(g, type)) {
        auto keepLeft = binary(g, reduce == ReduceOp::Min ? BinaryOp::Lt : BinaryOp::Gt, lhs, rhs);

        switch(reduce) {
            case ReduceOp::Add: return coerce(g, type, binary(g, BinaryOp::Add, lhs, rhs));
            case ReduceOp::Mul: return coerce(g, type, binary(g, BinaryOp::Mul, lhs, rhs));
            case ReduceOp::Min:
            case ReduceOp::Max: return ternary(g, keepLeft, lhs, rhs);
            default: break;
        }
    }

    switch(reduce) {
        case ReduceOp::Add: return coerce(g, type, binary(g, BinaryOp::Add, lhs, rhs));
        case ReduceOp::Mul: return coerce(g, type, binary(g, BinaryOp::Mul, lhs, rhs));
        case ReduceOp::Min: return hostCall(g, "Math"_v, "min"_v, lhs, rhs);
        default:            return hostCall(g, "Math"_v, "max"_v, lhs, rhs);
    }
}

/*
 * The lowest set lane, which is the one mask consumer that is not a tree.
 *
 * A chain of conditionals built from the last lane back - `m0 ? 0 : m1 ? 1 : 2` for two lanes - so
 * the answer is the first index whose lane holds, and the lane count where none does. Every lane is
 * named once and in order, which is what makes this the same expression the tree would have been in
 * cost and a different one in shape.
 *
 * Nothing here is a bit scan, because there are no bits: a lane is a variable on this target, so
 * "which lane matched" is a question about a run of booleans and the conditional chain is what asks
 * it. The alternative - the `select(mask, iota, splat(lanes))` and minimum this kind replaced - is
 * the same chain with a `Math.min` per level and a number materialized per lane.
 */
static JsPtr<Expr> firstSetLane(Gen& g, Buffer<JsPtr<Expr>> lanes, U32 count) {
    auto result = number(g, count);
    for(auto i = count; i-- > 0;) result = ternary(g, lanes[i], number(g, i), result);

    return result;
}

static void genVecReduce(Gen& g, ModulePtr<Value> pointer, InstVecReduce& instruction) {
    auto source = g.local[instruction.from]->type;
    auto from = vecPartsOf(g, instruction.from);

    if(instruction.reduce == ReduceOp::FirstSet) {
        define(g, pointer, firstSetLane(g, from.contents(), from.count));
        return;
    }

    define(g, pointer, reduceLanes(g, instruction.reduce, instruction.type,
                                   isMaskType(g, source), from.contents(), 0, from.count));
}

/*
 * A conversion between two vectors is the conversion between their lanes, lane by lane - which is
 * what the lane count being preserved across a `Cast` is *for*.
 *
 * A `Bitcast` is the same statement one step down: where the two lanes are the same *width*, a
 * reinterpretation of the vector is a reinterpretation of each lane, and `genBitcast` above already
 * says what that is on this target - the ordinary coercion between two integers, and the scratch
 * typed-array pair where one side is a float. What has no reading here is a bitcast that changes the
 * lane width: `i8x16` to `i32x4` is one register read another way natively and is four numbers read
 * as sixteen here, and this target has no bits for that to mean anything about.
 *
 * The test is therefore the lane *stride* and not the lane type, which is what it used to be - so
 * `Vec(Int)` to `Vec(U32)`, two names for the same thirty-two bits, was refused with a diagnostic
 * about lane widths that are equal.
 */
static void genVecConvert(Gen& g, ModulePtr<Value> pointer, InstUnary& instruction) {
    auto to = instruction.type;
    auto source = g.local[instruction.from]->type;
    auto element = laneType(g, to);
    auto sourceElement = laneType(g, source);
    auto reinterprets = instruction.kind == Value::Bitcast;

    if(reinterprets && laneStride(g.global, sourceElement, g.repr.target.integers) !=
                       laneStride(g.global, element, g.repr.target.integers)) {
        g.context.diagnostics.error("a bitcast between vectors of different lane widths has no JavaScript form - a lane is a number here rather than a run of bits"_v,
                                    instruction.source);
        return;
    }

    auto lanes = laneCount(g, to);
    auto from = vecPartsOf(g, instruction.from);
    auto parts = newVecParts(g, lanes);

    for(U32 i = 0; i < lanes; i++) {
        if(i >= from.count) {
            parts.lanes[i] = zeroValue(g, element);
        } else if(reinterprets) {
            parts.lanes[i] = bitcastLane(g, sourceElement, element, from.lanes[i]);
        } else {
            parts.lanes[i] = convertLane(g, sourceElement, element, from.lanes[i]);
        }
    }

    defineVec(g, pointer, parts);
}

/*
 * Whether this instruction is one of the vector forms, and generating it if so.
 *
 * Asked before the scalar switch rather than as arms inside it, because the split is by *type* and
 * not by kind: `add` over two vectors and `add` over two integers are one instruction kind, and the
 * lane-count question is what tells them apart. A comparison asks about its operands rather than its
 * result, since its result is the mask.
 */
bool genVectorInst(Gen& g, ModulePtr<Value> pointer, Inst& instruction) {
    switch(instruction.kind) {
        case Value::VecSplat:
            genVecSplat(g, pointer, (InstVecSplat&)instruction);
            return true;
        case Value::VecLane:
        case Value::VecWithLane:
            genVecLane(g, pointer, (InstVecLane&)instruction);
            return true;
        case Value::VecShuffle:
            genVecShuffle(g, pointer, (InstVecShuffle&)instruction);
            return true;
        case Value::VecReduce:
            genVecReduce(g, pointer, (InstVecReduce&)instruction);
            return true;

        case Value::Cmp:
            if(!isVectorType(g.global, g.local[((InstCmp&)instruction).lhs]->type)) return false;
            genVecCmp(g, pointer, (InstCmp&)instruction);
            return true;

        default:
            break;
    }

    /*
     * A **mask** is one of these too, and asking `isVectorType` is what left it out.
     *
     * `Logic` over a mask is `and`, `or`, `xor` and `not` (Design-Vector §3.2), and the emitters
     * below have handled a mask operand since they were written - `maskBinary` and `genVecUnary`
     * both branch on it. What did not was the gate: `isVectorType` answers no for a mask by
     * definition, so combining two of them fell through to the scalar path and emitted a `&` over
     * two host booleans. Nothing produced a mask outside a comparison feeding a select until the
     * class existed, which is why it was invisible.
     */
    if(!vectorLanes(g.global, instruction.type)) return false;

    switch(instruction.kind) {
        case Value::Add: case Value::Sub: case Value::Mul: case Value::Div: case Value::Rem:
        case Value::Shl: case Value::Shr: case Value::Sar:
        case Value::Rol: case Value::Ror:
        case Value::And: case Value::Or: case Value::Xor:
            genVecBinary(g, pointer, (InstBinary&)instruction);
            return true;
        case Value::Neg:
        case Value::Not:
        case Value::Sqrt:
        case Value::Abs:
        case Value::Trunc:
        case Value::Floor:
        case Value::Ceil:
        case Value::Round:
            genVecUnary(g, pointer, (InstUnary&)instruction);
            return true;
        case Value::Fma:
            genVecFma(g, pointer, (InstFma&)instruction);
            return true;
        case Value::Select:
            genVecSelect(g, pointer, (InstSelect&)instruction);
            return true;
        case Value::Cast:
        case Value::Bitcast:
            genVecConvert(g, pointer, (InstUnary&)instruction);
            return true;
        default:
            return false;
    }
}

void genInstruction(Gen& g, ModulePtr<Inst> pointer) {
    auto& instruction = *g.local[pointer];
    auto value = (ModulePtr<Value>)pointer;

    // A vector is `lanes` values here, so every operation over one is `lanes` operations - which is
    // a different emitter rather than a case inside each of the ones below.
    if(genVectorInst(g, value, instruction)) return;

    switch(instruction.kind) {
        case Value::Alloc: {
            /*
             * Storage, which on this target is a value rather than a location. Where escape analysis
             * put it makes no difference: `StorageClass::Heap` and `StorageClass::Stack` are the
             * same object, because the host collector owns reclamation either way.
             */
            auto& allocation = (InstAlloc&)instruction;
            auto boxed = allocation.local < g.boxed.size() && g.boxed[allocation.local];

            /*
             * A run of slots has no JS form yet.
             *
             * Implementation-Containers.md §14 gives this target a host array or a `TypedArray`
             * rather than a run of storage, and reaching it needs the JS FFI its prerequisites name.
             * Reporting is what keeps that gap visible: silently emitting the one-object form here
             * would produce a container of capacity one that indexes off the end of itself.
             */
            if(allocation.extent) {
                g.context.diagnostics.error("a run of slots has no representation on the JS target yet - it needs the host array of Implementation-Containers.md §14"_v,
                                            instruction.source);
                break;
            }

            /*
             * Storage for a function value the body holds as its two words, which is two variables
             * and no object at all - see prepareFunLocals.
             *
             * Both start `null` rather than being left undeclared, and that is not caution: the two
             * `Init`s that follow assign them, and `var v$c = null; v$c = L;` is what the peephole's
             * "`var v = 0; v = x`" rewrite collapses into the declaration. Leaving them undeclared
             * would make the assignments implicit globals under a strict-mode script.
             */
            if(allocation.local < g.flatFuns.size() && g.flatFuns[allocation.local] && !boxed) {
                auto code = partName(g, instruction, "$c"_v);
                auto env = partName(g, instruction, "$e"_v);

                emit(g, make<DeclStmt>(g, code, nullValue(g), false));
                emit(g, make<DeclStmt>(g, env, nullValue(g), false));

                FunParts parts;
                parts.code = variable(g, code);
                parts.env = variable(g, env);

                g.funParts.add(U32(value), parts);
                break;
            }

            /*
             * Storage for a reference the body holds as its parts - see prepareRefLocals. Two or
             * three variables, and no object at any point in the local's life.
             *
             * The owner starts `null` for the same reason the two words above do, and here it also
             * *means* something: `null` is the absent niche, so a local a predecessor left as
             * `Nothing` reads as `Nothing` whether that arm wrote the tag or never ran. The key and
             * the shift start undeclared-but-declared, since nothing reads either while the owner
             * says there is no reference.
             */
            if(allocation.local < g.flatRefLocals.size() && g.flatRefLocals[allocation.local] && !boxed) {
                auto owner = partName(g, instruction, "$o"_v);
                emit(g, make<DeclStmt>(g, owner, nullValue(g), false));

                RefParts parts;
                parts.owner = variable(g, owner);

                /*
                 * The key and the shift only where a reference is ever written here - see
                 * Gen::flatRefTagOnly. A local holding nothing but the absent constructor has no
                 * value for either, and declaring them would leave two `var`s nothing assigns.
                 *
                 * Safe to leave out only because no flattened local is ever built back into an
                 * object: `prepareRefLocals`' second rule declines every use that would ask for one.
                 */
                if(!g.flatRefTagOnly[allocation.local]) {
                    auto key = partName(g, instruction, "$k"_v);
                    emit(g, make<DeclStmt>(g, key, JsPtr<Expr>(nullptr), false));
                    parts.key = variable(g, key);

                    if(narrowRefCarriesScale(g)) {
                        auto scale = partName(g, instruction, "$s"_v);
                        emit(g, make<DeclStmt>(g, scale, JsPtr<Expr>(nullptr), false));
                        parts.scale = variable(g, scale);
                    }
                }

                g.flatRefs.add(U32(value), parts);
                break;
            }

            /*
             * Storage an `InstAggregate` builds whole, which is declared holding nothing.
             *
             * `var v;` rather than a manufactured value of the type, because the aggregate assigns a
             * complete literal before anything reads the local and `foldInitialValue` in opt.cpp
             * then collapses the two into the declaration. The binding is still declared, which is
             * what keeps the assignment from being an implicit global under a strict-mode script;
             * what it holds until then is `undefined`, over a stretch the IR says nothing reads.
             *
             * This is what `zeroValue` was for at an allocation, and why removing it matters is not
             * the statement it saves: a fresh value has to be built out of the type's own shape, and
             * a type reached across an abstraction boundary has none this side of it to build from -
             * see InstAggregate. A boxed local still gets its box, since that is the storage rather
             * than a value of the type, and its one property is written the same way.
             */
            if(allocation.local < g.builtWhole.size() && g.builtWhole[allocation.local]) {
                define(g, value, boxed ? boxOf(g, nullValue(g)) : nullptr);
                break;
            }

            auto initial = freshStorage(g, instruction.type);
            if(boxed) initial = boxOf(g, initial);

            define(g, value, initial);
            break;
        }
        case Value::LoadPlace: {
            auto& loadInst = (InstLoadPlace&)instruction;

            // Read through the witness the caller passed, into storage this frame provides - the
            // same shape native uses, and for the same reason: the field's type is a variable, so
            // there is nothing to hand it back in.
            if(auto slot = propertySlotOf(g, loadInst.place); slot != maxLimit<U16>) {
                auto witness = genSlot(g, slot);
                auto out = propertyStorage(g, instruction.type, valueName(g, instruction));

                emitExpr(g, call(g, tableCell(g, witness, PropertyWitnessFields::kRead),
                                 propertyOwner(g, loadInst.place), out));
                g.values.add(U32(value), propertyContents(g, instruction.type, out));
                break;
            }

            /*
             * A whole function value read out of a place stays two words.
             *
             * Reading `s.run` is reading `s.run$c` and `s.run$e`, and joining them into an object
             * here would put back exactly the allocation part 2.2 removed from the record - one per
             * read rather than one per record, which is worse. So the load registers the two words
             * as this value's parts and binds nothing, and whatever reads it gets them: a call
             * enters with them, an argument passes them, and only a use that needs one value asks
             * `useValue` and builds the object there.
             */
            if(isFunValue(g, instruction.type)) {
                g.funParts.add(U32(value), funPartsOfPlace(g, loadInst.place));
                break;
            }

            /*
             * And a reference read out of a local held as its parts stays those parts, on the same
             * terms: joining them into an object here would put back the allocation the flattening
             * removed, one per read of the local rather than one per local.
             *
             * `match find(m, k): Just(v) -> …` is the shape - the load of the payload is what binds
             * `v`, and every use of `v` below it is a dereference, which wants the parts.
             */
            if(auto parts = refPartsOfPlace(g, loadInst.place)) {
                g.flatRefs.add(U32(value), parts.unwrap());
                break;
            }

            define(g, value, placeExpr(g, loadInst.place));
            break;
        }
        case Value::Init:
        case Value::Assign: {
            // One statement for both. Whatever the old value's drop needed was emitted as its own
            // InstDrop by the drop pass, so an assignment here is only the write.
            auto& init = (InstInit&)instruction;

            /*
             * Writing a value that carries nothing writes nothing - the same rule the native
             * lowering applies, and needed here for the same reason: the resolver skips the write
             * where it can see the type, but a specialization at `{}` clones instructions decided
             * before the substitution, so `init %local@Just, %carried` survives with `%carried`
             * holding nothing.
             *
             * It has to be skipped rather than emitted, because the place is not a property here.
             * A constructor whose content is unit contributes none (see eachProperty), so the
             * Downcast projection stays on the record itself - and writing `useValue`'s `null`
             * through that would replace the whole value with nothing rather than fill a field of
             * it. `fn wrap(x: a) -> Maybe(a) = Just(x)` at `wrap({})` is the shortest way to reach
             * it, with no lens anywhere in it.
             */
            if(isUnit(g.global, g.local[init.value]->type)) break;

            /*
             * Writing through the witness, which takes the replacement by reference for the same
             * reason the read hands one back by reference. `set` consumes what it is given and
             * releases what the field held, so this stages the value and lets the callee commit it.
             */
            if(auto slot = propertySlotOf(g, init.place); slot != maxLimit<U16>) {
                auto written = g.local[init.value]->type;
                auto witness = genSlot(g, slot);
                auto staging = declare(g, generatedName(g, "field"_v, instruction.id),
                                       isJsObject(g, written) && !isGeneric(g.global, written)
                                           ? useValue(g, init.value)
                                           : boxOf(g, useValue(g, init.value)));

                emitExpr(g, call(g, tableCell(g, witness, PropertyWitnessFields::kSet),
                                 propertyOwner(g, init.place), staging));
                break;
            }

            storeInto(g, init.place, g.local[init.value]->type, init.value);
            break;
        }
        /*
         * The elements of a literal, one write each - see InstAggregate.
         *
         * Deliberately the same text the per-element `Init`s produced, because this step is a
         * refactor and the fixtures are what says so: the literal this could be emitting instead is
         * the next change, and one that alters no output is one whose ownership and folding
         * behaviour can be compared against the build before it.
         */
        case Value::Aggregate:
            genAggregate(g, (InstAggregate&)instruction);
            break;
        case Value::Borrow:
            // Whether the reference names a slot or is the box that stands in for an address is
            // decided by which of the two a `&T` and a `%T` are - see refIsTriple. The borrow's
            // own mutability is not read at all, which is what removed the write-back.
            genBorrow(g, value, instruction, ((InstBorrow&)instruction).place, false);
            break;
        case Value::Address:
            genBorrow(g, value, instruction, ((InstAddress&)instruction).place, true);
            break;
        case Value::Move:
            // Nothing to emit beyond naming what moved: the object stays where it is, and what
            // changed is which name is allowed to reach it - which the resolver already proved.
            define(g, value, placeExpr(g, ((InstMove&)instruction).place));
            break;
        case Value::Swap:
            genSwap(g, instruction, (InstSwap&)instruction);
            break;
        case Value::Exchange:
            genExchange(g, value, instruction, (InstExchange&)instruction);
            break;
        case Value::Copy: {
            auto& copied = (InstCopy&)instruction;

            if(copied.copy) {
                define(g, value, call(g, functionValue(g, copied.copy, instruction.source),
                                      referenceTo(g, copied.place)));
                break;
            }

            define(g, value, cloneValue(g, instruction.type, placeExpr(g, copied.place), instruction.source));
            break;
        }
        case Value::Drop:
            genDrop(g, (InstDrop&)instruction);
            break;
        case Value::Cast:
            genCast(g, value, (InstUnary&)instruction);
            break;
        case Value::Bitcast:
            genBitcast(g, value, (InstUnary&)instruction);
            break;
        // The scalar square root and multiply-add, which are `Math.sqrt` and the two operations -
        // see genVecFma for why the second is not a gap.
        case Value::Sqrt:
            define(g, value, coerce(g, instruction.type,
                                    hostCall(g, "Math"_v, "sqrt"_v, useValue(g, ((InstUnary&)instruction).from))));
            break;
        // The scalar magnitude, which nothing produces today - `abs` is a vector intrinsic and the
        // resolver's verifier says so. It is here because the vector path above is written per lane
        // and this is the same call with the lane loop gone.
        case Value::Abs:
            define(g, value, coerce(g, instruction.type,
                                    hostCall(g, "Math"_v, "abs"_v, useValue(g, ((InstUnary&)instruction).from))));
            break;
        // The four roundings, which are the same call the vector path makes per lane.
        case Value::Trunc:
        case Value::Floor:
        case Value::Ceil:
        case Value::Round:
            define(g, value, coerce(g, instruction.type,
                                    roundingLane(g, instruction.kind,
                                                 useValue(g, ((InstUnary&)instruction).from))));
            break;
        case Value::Fma: {
            auto& fma = (InstFma&)instruction;
            auto product = coerce(g, instruction.type, binary(g, BinaryOp::Mul,
                                                              useValue(g, fma.a), useValue(g, fma.b)));

            define(g, value, coerce(g, instruction.type,
                                    binary(g, BinaryOp::Add, product, useValue(g, fma.c))));
            break;
        }
        case Value::Neg: {
            auto from = useValue(g, ((InstUnary&)instruction).from);

            // Negation of a wide value is `0 - x`, which is the subtract helper and its wrap: the
            // one value with no positive counterpart negates to itself, and `-x` alone would leave
            // it out of range.
            if(isWideNumber(g, instruction.type)) {
                define(g, value, wideCall(g, WideOp::Sub, intType(g, instruction.type),
                                          number(g, 0), from));
                break;
            }

            define(g, value, coerce(g, instruction.type, unary(g, UnaryOp::Neg, from)));
            break;
        }
        case Value::Not: {
            auto operand = useValue(g, ((InstUnary&)instruction).from);

            // `^ 1` rather than `!`, which is the same statement as the bitwise operators above: a
            // Bool is one bit, so flipping it is flipping that bit.
            if(isBool(g, instruction.type)) {
                define(g, value, binary(g, BinaryOp::Xor, operand, number(g, 1)));
            } else if(isWideNumber(g, instruction.type)) {
                // `~` is a 32-bit operator, so complementing a wider value has to complement both
                // halves and mask the top one back to the declared width.
                define(g, value, wideCall(g, WideOp::Not, intType(g, instruction.type),
                                          operand, nullptr));
            } else {
                define(g, value, coerce(g, instruction.type, unary(g, UnaryOp::BitNot, operand)));
            }

            break;
        }
        case Value::ByteSwap:
            define(g, value, coerce(g, instruction.type,
                                    byteSwapCall(g, instruction.type,
                                                 useValue(g, ((InstUnary&)instruction).from))));
            break;
        case Value::Rol:
        case Value::Ror:
            define(g, value, coerce(g, instruction.type,
                                    rotateCall(g, instruction.kind, instruction.type,
                                               useValue(g, ((InstBinary&)instruction).lhs),
                                               useValue(g, ((InstBinary&)instruction).rhs))));
            break;
        case Value::CountBits:
        case Value::LeadingZeros:
        case Value::TrailingZeros:
            define(g, value, coerce(g, instruction.type,
                                    bitCountCall(g, instruction.kind, instruction.type,
                                                 useValue(g, ((InstUnary&)instruction).from))));
            break;
        case Value::Add:
        case Value::Sub:
        case Value::Mul:
        case Value::Div:
        case Value::Rem:
        case Value::Shl:
        case Value::Shr:
        case Value::Sar:
        case Value::And:
        case Value::Or:
        case Value::Xor:
        // The three BMI2 operations, which are ordinary binaries here: `genBinary` sends each to one
        // of the six helpers. Not in `genVectorInst`'s list above, there being no lane-wise spelling
        // of any of them.
        case Value::BitsUpTo:
        case Value::GatherBits:
        case Value::ScatterBits:
            genBinary(g, value, (InstBinary&)instruction);
            break;
        case Value::Cmp: {
            auto& compare = (InstCmp&)instruction;
            auto lhs = useValue(g, compare.lhs);
            auto rhs = useValue(g, compare.rhs);

            /*
             * A comparison of a reference goes through `==` rather than `===`.
             *
             * Two objects compare the same either way - there is no coercion between them - so the
             * only case the two answers differ in is the one that matters: a property nothing
             * attached reads back as `undefined`, and `undefined == null` is exactly "there is no
             * reference here". That is what a closure with no environment is, and the shared
             * teardown tests for it against a null constant.
             */
            auto reference = isPointer(g.global, g.local[compare.lhs]->type) ||
                             isPointer(g.global, g.local[compare.rhs]->type);

            BinaryOp op;
            switch(compare.cmp) {
                case CompareOp::Eq: op = reference ? BinaryOp::LooseEq : BinaryOp::Eq; break;
                case CompareOp::Ne: op = reference ? BinaryOp::LooseNe : BinaryOp::Ne; break;
                case CompareOp::Gt: op = BinaryOp::Gt; break;
                case CompareOp::Ge: op = BinaryOp::Ge; break;
                case CompareOp::Lt: op = BinaryOp::Lt; break;
                default: op = BinaryOp::Le; break;
            }

            // The same widening genCast's `toBool` does, for the same reason: what `===` answers is
            // a host boolean and what a `Bool` is here is 0 or 1 - see boolNumber.
            define(g, value, boolNumber(g, binary(g, op, lhs, rhs)));
            break;
        }
        case Value::Select: {
            /*
             * The branch that stopped being one - `c ? a : b`.
             *
             * The condition is the same expression the `if` would have tested, so nothing has to be
             * done to make it a value: a `Bool` is the number 0 or 1 here, and both are what the
             * host's own truthiness already says they are.
             *
             * No `coerce`, deliberately. A select produces one of two values of its own type, and
             * both of them arrived through whatever already reduced them to that type's
             * representation - so there is nothing left to narrow, and a mask here would be one
             * `| 0` per conditional in the program saying so twice.
             */
            auto& select = (InstSelect&)instruction;
            define(g, value, ternary(g, useValue(g, select.cond), useValue(g, select.whenTrue),
                                     useValue(g, select.whenFalse)));
            break;
        }
        /*
         * The address in one table slot, which here is not an address at all.
         *
         * A JS table is an array and a slot holds the emitted binding itself, so this is `table[N]`
         * and there is nothing to decode - no width, no relocation, and no self-relative offset like
         * the one the native path adds back. `wordCount` is ignored for the same reason: it exists
         * to tell a byte offset where the words stop, and an array index has no bytes in it.
         *
         * That the two targets disagree this completely is exactly why the asker states the slot
         * instead of the access - see InstTableSlot.
         */
        case Value::TableSlot: {
            auto& read = (InstTableSlot&)instruction;
            define(g, value, tableCell(g, useValue(g, read.table), read.slot));
            break;
        }

        case Value::TypeMetric: {
            /*
             * How wide a type is *here*, which is not what the native target would have said.
             *
             * The point of the metric travelling in the IR rather than being folded during
             * resolution is this line: the JS family has its own answers, and a `sizeOf` compiled
             * for this target reports them. A generic body reads the descriptor slot instead, the
             * same way the native path does.
             */
            auto& metric = (InstTypeMetric&)instruction;

            // A count this body does not know is one cell of the environment and nothing else - see
            // genConstValue. It comes back as a host number, which is what a count is here.
            if(metric.metric == TypeMetricKind::Count) {
                if(auto value_ = genConstValue(g, metric.of)) {
                    define(g, value, value_);
                    break;
                }
            }

            if(auto descriptor = genTypeDesc(g, metric.of)) {
                // The alignment shares the flags cell and sits above them - see
                // TypeDescFields::kFlags. Same element, one shift.
                if(metric.metric == TypeMetricKind::Align) {
                    define(g, value, binary(g, BinaryOp::Shr,
                                            tableCell(g, descriptor, TypeDescFields::kFlags),
                                            number(g, F64(kPackedMetricShift))));
                    break;
                }

                auto slot = metric.metric == TypeMetricKind::Stride ? TypeDescFields::kStride
                                                                    : TypeDescFields::kSize;
                define(g, value, tableCell(g, descriptor, slot));
                break;
            }

            auto number_ = g.repr.metric(metric.of, metric.metric);

            /*
             * As a literal of the metric's own representation, which for an `I64` is a `bigint`.
             *
             * `sizeOf` answers an `I64`, so a `number` here is a value of the wrong host type - and
             * `coerce` does not rescue it: the reduction for a 64-bit type is `BigInt.asIntN`, which
             * throws on a `number` rather than converting one. It went unnoticed while nothing that
             * measured a type was compiled for this target; `FixedArray.yana`'s `sizeOf` is the first.
             */
            define(g, value, isLong(g, instruction.type)
                ? bigInt(g, U64(number_), true)
                : coerce(g, instruction.type, number(g, F64(number_))));
            break;
        }
        case Value::Symbol: {
            auto& symbol = (InstSymbol&)instruction;

            if(symbol.callee) {
                define(g, value, functionValue(g, symbol.callee, instruction.source));
            } else {
                define(g, value, globalValue(g, symbol.global));
            }

            break;
        }
        case Value::Call:
            genCall(g, value, (InstCall&)instruction);
            break;
        case Value::CallDyn:
            genCallDyn(g, value, (InstCallDyn&)instruction);
            break;
        case Value::GenCall:
            genGenCall(g, value, (InstGenCall&)instruction);
            break;
        case Value::Native: {
            auto& native = (InstNative&)instruction;

            if(isHostOp(native.op)) {
                genHost(g, value, instruction, native);
            } else {
                genBlockCopy(g, instruction, native);
            }

            break;
        }
        default:
            g.context.diagnostics.error("internal error: unexpected instruction in JS codegen"_v,
                                        instruction.source);
            break;
    }
}

} // namespace js
