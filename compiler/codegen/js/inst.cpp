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
         * Property by property and shallow, and not `cloneValue`: this is a *move*, so the source is
         * dead afterwards and a nested aggregate has nobody left to alias with. `genBlockCopy` clones
         * because both of its sides stay live.
         */
        auto written = useValue(g, value);

        if(isJsObject(g, type) && writesThroughReference(g, place)) {
            auto source = g.base[written]->kind == Expr::Var
                ? written
                : declare(g, generatedName(g, "moved"_v, produced.id), written);

            eachProperty(g, type, [&](Name key, TypePtr) {
                emitExpr(g, assign(g, field(g, target, key), field(g, source, key)));
            });

            return;
        }

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
                auto quotient = hostCall(g, "Math"_v, "trunc"_v, binary(g, BinaryOp::Div, lhs, rhs));
                define(g, pointer, integer->isSigned
                    ? wideCall(g, WideOp::Wrap, integer, quotient, nullptr) : quotient);
                return;
            }
            case Value::Rem:
                define(g, pointer, binary(g, BinaryOp::Rem, lhs, rhs));
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
            if(integer && integer->width == IntType::Int) {
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
            simple(BinaryOp::Div);
            return;
        case Value::Rem: simple(BinaryOp::Rem); return;
        case Value::Shl: simple(BinaryOp::Shl); return;
        case Value::Shr:
            // A logical right shift of a `Long` has to go through the unsigned reading first:
            // `>>>` is not defined on BigInt at all.
            if(integer && integer->width == IntType::Long) {
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
static void ensureFloatBits(Gen& g) {
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
static JsPtr<Expr> hostArrayOf(Gen& g, TypePtr type, JsPtr<Expr> elements) {
    auto typed = typedArrayFor(g.global, pointeeType(g.global, type));
    if(!typed.length) return elements;

    auto node = make<CallExpr>(g, variable(g, literalName(g, typed)));
    node->args.push(g.file.arena, elements);
    node->construct = true;

    /*
     * `pure` in the sense the flag means - reads nothing, writes nothing. A typed array's
     * constructor allocates, which no other expression can observe, and copies out of an argument
     * whose own effects the walk still visits.
     *
     * It is load-bearing rather than tidy. The empty array a literal is built into is overwritten by
     * the literal in the next statement, and `foldInitialValue` is what makes the two one - but only
     * for an initializer nothing has to be ordered against. Without this, every array literal of a
     * numeric element allocated a `new Int32Array([])` that nothing ever read.
     */
    node->pure = true;

    return asExpr(g, node);
}

static JsPtr<Expr> construct(Gen& g, StringView type, JsPtr<Expr> argument) {
    auto node = make<CallExpr>(g, variable(g, literalName(g, type)));
    node->args.push(g.file.arena, argument);
    node->construct = true;
    return asExpr(g, node);
}

// `toBits` says which direction: a float in and its bits out, or the reverse.
static Name floatBitsHelper(Gen& g, bool toBits) {
    auto& into = toBits ? g.floatToBitsHelper : g.bitsToFloatHelper;
    if(into.text) return into;

    ensureFloatBits(g);
    into = uniqueName(g, toBits ? "$bc$f2i"_v : "$bc$i2f"_v, false);
    return into;
}


/*
 * One value's bits read as another type of the same width.
 *
 * Two shapes. Two integer types of one width are the same host value in two normal forms, so the
 * ordinary coercion is exactly the reinterpretation - `Int` to `U32` is `>>> 0`, which is what the
 * two's complement pattern of a negative number *is* read as unsigned. What is left is a float
 * meeting an integer, which no operator here expresses and the scratch typed-array pair does.
 *
 * The sixty-four bit float pairs never arrive: `defineBitcastLadder` declines to generate them on
 * this target, because a `bigint` going through a `DataView` costs more than any program would have
 * reached for a bitcast to save.
 *
 * Its own function because a *vector* bitcast is this per lane - see genVecConvert, which is the
 * second caller and the reason the rule is written once.
 */
static JsPtr<Expr> bitcastLane(Gen& g, TypePtr from, TypePtr to, JsPtr<Expr> value) {
    auto fromFloat = isFloat(g.global, from);
    if(fromFloat == isFloat(g.global, to)) return coerce(g, to, value);

    auto helper = floatBitsHelper(g, fromFloat);
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
    auto key = (U32(integer.bits) << 1) | U32(integer.isSigned);
    if(auto found = g.satHelpers.get(key)) return found.unwrap();

    char buffer[32];
    Size length = 0;
    buffer[length++] = '$';
    buffer[length++] = 's';
    buffer[length++] = 'a';
    buffer[length++] = 't';
    length += show(U64(integer.bits), buffer + length, sizeof(buffer) - length);
    buffer[length++] = integer.isSigned ? 'i' : 'u';

    auto name = uniqueName(g, StringView { buffer, length }, false);
    g.satHelpers.add(key, name);
    g.satHelperOrder.push(SatHelper { name, U16(integer.bits), integer.isSigned });
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

    if(toBool) {
        define(g, pointer, binary(g, BinaryOp::Ne, value, fromLong ? bigInt(g, 0, true) : number(g, 0)));
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
                elements->values.push(g.file.arena, useValue(g, component.value));
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

    eachProperty(g, shape, [&](Name key, TypePtr member) {
        emitExpr(g, assign(g, field(g, target, key),
                           cloneValue(g, member, field(g, source, key), instruction.source)));
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

void emitFloatBitsHelpers(Gen& g) {
    if(!g.floatBitsBuffer.text) return;

    emit(g, make<CommentStmt>(g, internText(g,
        "a float's bits, which no host operator reaches - see codegen/js/inst.cpp"_v)));

    // `Float32Array` first and the integer view over its buffer second, which is the whole of what
    // makes the two names one storage location.
    emit(g, make<DeclStmt>(g, g.floatBitsBuffer, construct(g, "Float32Array"_v, number(g, 1)), true));
    emit(g, make<DeclStmt>(g, g.floatBitsInts,
                           construct(g, "Int32Array"_v,
                                     field(g, variable(g, g.floatBitsBuffer), literalName(g, "buffer"_v))),
                           true));

    for(auto toBits: { true, false }) {
        auto name = toBits ? g.floatToBitsHelper : g.bitsToFloatHelper;
        if(!name.text) continue;

        auto written = toBits ? g.floatBitsBuffer : g.floatBitsInts;
        auto read = toBits ? g.floatBitsInts : g.floatBitsBuffer;

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
    auto value = [&](Size at) {
        return useValue(g, aggregate.components.get(g.local, at).value);
    };

    if(plan.kind == AggregateBuildPlan::Array) {
        auto elements = make<ArrayExpr>(g);
        for(Size i = 0; i < aggregate.components.size(); i++) {
            elements->values.push(g.file.arena, value(i));
        }

        return asExpr(g, elements);
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
            if(integer && integer->width == IntType::Int) {
                return coerce(g, type, hostCall(g, "Math"_v, "imul"_v, lhs, rhs));
            }

            return simple(BinaryOp::Mul);
        case Value::Div: return simple(BinaryOp::Div);
        case Value::Rem: return simple(BinaryOp::Rem);
        case Value::Shl: return simple(BinaryOp::Shl);
        case Value::Shr: return simple(BinaryOp::Shr);
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

    switch(reduce) {
        case ReduceOp::Add: return coerce(g, type, binary(g, BinaryOp::Add, lhs, rhs));
        case ReduceOp::Mul: return coerce(g, type, binary(g, BinaryOp::Mul, lhs, rhs));
        case ReduceOp::Min: return hostCall(g, "Math"_v, "min"_v, lhs, rhs);
        default:            return hostCall(g, "Math"_v, "max"_v, lhs, rhs);
    }
}

static void genVecReduce(Gen& g, ModulePtr<Value> pointer, InstVecReduce& instruction) {
    auto source = g.local[instruction.from]->type;
    auto from = vecPartsOf(g, instruction.from);

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

    if(reinterprets && laneStride(g.global, sourceElement) != laneStride(g.global, element)) {
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
        case Value::And: case Value::Or: case Value::Xor:
            genVecBinary(g, pointer, (InstBinary&)instruction);
            return true;
        case Value::Neg:
        case Value::Not:
        case Value::Sqrt:
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

            define(g, value, binary(g, op, lhs, rhs));
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
