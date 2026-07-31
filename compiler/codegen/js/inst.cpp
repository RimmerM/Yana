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
 * The write-back half of a borrow that had to be boxed at the borrow rather than at its storage.
 *
 * Run after the instruction that consumed the borrow, which is the point the loan ends: the callee
 * has finished writing through the box and the field it stood in for has to be told.
 */
void flushWritebacks(Gen& g, Value& instruction) {
    if(g.writebacks.isEmpty()) return;

    ModuleList<ModulePtr<Value>, false>* args = nullptr;
    switch(instruction.kind) {
        case Value::Call: args = &((InstCall&)instruction).args; break;
        case Value::CallDyn: args = &((InstCallDyn&)instruction).args; break;
        case Value::GenCall: args = &((InstGenCall&)instruction).args; break;
        default: return;
    }

    for(auto arg: args->contents(g.local)) {
        for(Size i = 0; i < g.writebacks.size(); i++) {
            if(g.writebacks[i].borrow != arg) continue;

            auto entry = g.writebacks[i];
            g.writebacks.remove(i);

            auto committed = field(g, entry.box, g.boxField);
            if(!assignPlace(g, entry.target, entry.type, committed)) {
                emitExpr(g, assign(g, placeExpr(g, entry.target), committed));
            }
            break;
        }
    }
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

void storeInto(Gen& g, const Place& place, TypePtr type, ModulePtr<Value> value) {
    auto& produced = *g.local[value];

    if(assignPlace(g, place, type, useValue(g, value))) return;

    auto target = placeExpr(g, place);

    auto moved = produced.kind == Value::Move;
    ModulePtr<Function> sink = moved ? ((InstMove&)produced).sink : nullptr;

    if(!sink) {
        if(moved && erasedRelocate(g, target, useValue(g, value), type)) return;

        /*
         * The erased write that is not a relocation - Analysis-JS.md §3.4's remaining half.
         *
         * Native block-copies the descriptor's `size` bytes, which is a shallow copy of storage
         * whose shape it does not need. This target has no shallow copy of one whole value: a
         * nested aggregate is a separate object here and inline bytes there, so copying property by
         * property is the only form, and which properties there are is exactly what is unknown. The
         * descriptor has no operation that answers it, and inventing one that runs `moveInit`
         * instead would relocate a value native does not relocate.
         */
        if(g.genEnv && isGeneric(g.global, type) && !rebindsOwnStorage(g, place)) {
            g.context.diagnostics.error("the JS target cannot write a value of a type it cannot see the shape of into storage this function does not own - a descriptor operation for it is the target split Analysis-JS.md §3.4 asks for"_v,
                                        produced.source);
            return;
        }

        emitExpr(g, assign(g, target, useValue(g, value)));
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
        if(isFloat(g.global, from)) value = hostCall(g, "Math"_v, "trunc"_v, value);

        define(g, pointer, coerce(g, to, globalCall(g, "BigInt"_v, value)));
        return;
    }

    if(fromLong && !toLong) {
        auto asNumber = globalCall(g, "Number"_v, value);
        define(g, pointer, isFloat(g.global, to) ? asNumber : coerce(g, to, asNumber));
        return;
    }

    if(isInteger(g.global, to) && isFloat(g.global, from)) {
        define(g, pointer, coerce(g, to, hostCall(g, "Math"_v, "trunc"_v, value)));
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

    if(instruction.flag != maxLimit<U32>) {
        g.context.diagnostics.error("the JS target does not implement conditional teardown yet"_v,
                                    instruction.source);
        return;
    }

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
     * A scalarized record goes by value; everything else keeps the box it always had.
     *
     * `referenceTo` boxes whatever is not an object, and for as long as every aggregate here *was*
     * one that meant "aggregates unboxed, scalars boxed" - which is what every teardown was compiled
     * against. Scalarization moves aggregates from one side of that to the other, and the callee did
     * not move with them: it reads the by-value parameter it declares, so a box would make it read
     * `{$v: 3} & 1` and get zero. Keeping the box for everything else leaves the teardowns that
     * genuinely take a reference - a closure's, `Sink`'s - exactly as they were.
     */
    auto scalarized = type && isMemoryType(g.global, type) &&
                      g.global[type]->kind != Type::Fun && g.repr.of(type).scalarBits != 0;

    emitExpr(g, call(g, functionValue(g, instruction.drop, instruction.source),
                     scalarized ? placeExpr(g, instruction.place)
                                : referenceTo(g, instruction.place)));
}

/*
 * One argument, as however many the convention says it occupies.
 *
 * A narrow reference goes as its parts - see refIsFlattened. The decision is the *declared parameter*
 * type rather than the argument's, and the difference is not hypothetical: a concrete `&Bool` handed
 * to a generic `&a` reaches a body compiled against a type variable, which has no width to mask with
 * and takes the object. Where the two agree - every specialized call - reading the parameter costs a
 * lookup and says the same thing.
 */
void pushArg(Gen& g, Array<JsPtr<Expr>>& args, ModulePtr<Value> arg, bool flat) {
    if(arg && flat) {
        auto parts = refPartsOf(g, arg);
        args.push(parts.owner);
        args.push(parts.key);
        if(parts.scale) args.push(parts.scale);
        return;
    }

    args.push(useValue(g, arg));
}

void genCall(Gen& g, ModulePtr<Value> pointer, InstCall& instruction) {
    Array<JsPtr<Expr>> args;

    Size index = 0;
    for(auto arg: instruction.args.contents(g.local)) {
        if(!callParameterIsAbsent(g, instruction, index)) {
            pushArg(g, args, arg, callParameterIsFlatRef(g, instruction, index));
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
         * An ordinary call. A function value is a host function here, so there is no code word to
         * load and no environment to pass: whatever a capturing lambda closed over, it closed over
         * when it was built, and a non-capturing one and a plain function referenced by name have
         * nothing to close over at all. All three are one shape, which is what the `{code, env}`
         * pair bought on native and what the host gives for nothing.
         */
        callee = useValue(g, instruction.callable);
    } else {
        callee = useValue(g, instruction.address);
    }

    Size index = 0;
    for(auto arg: instruction.args.contents(g.local)) {
        if(!callParameterIsAbsent(g, instruction, index)) {
            pushArg(g, args, arg, callParameterIsFlatRef(g, instruction, index));
        }

        index++;
    }

    define(g, pointer, callWith(g, callee, args));
}

// The environment assembled at the call site: the slots this caller knows concretely, and the ones
// it forwards out of its own.
JsPtr<Expr> genEnvTable(Gen& g, InstGenCall& instruction) {
    auto table = make<ArrayExpr>(g);

    // The schema word in front of the slots. Emitted code never reads it; it is here because the
    // numbering says slot N is at N + kSlots, and a caller assembling a table has to agree with the
    // interned ones it may be passed alongside.
    for(U16 i = 0; i < GenEnvFields::kWordCount; i++) {
        table->values.push(g.file.arena, number(g, 0));
    }

    for(auto slot: instruction.fill.contents(g.local)) {
        auto value = slot.isForwarded()
            ? genWitness(g, slot.forwarded, slot.forwardedSupers)
            : globalValue(g, slot.constant);

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
        auto argCount = U16(g.global[g.global[instruction.typeClass]->gen]->types.size());
        callee = tableCell(g, witness, ClassWitnessFields::method(argCount, instruction.index));
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
        if(parameter && functionFlattensRefs(g, *function) &&
           refIsFlattened(g, parameter->type, parameter->convention)) {
            pushArg(g, declared, arg, true);
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
 * Building a function value - Analysis-JS.md §3.2, answered with a host closure.
 *
 * `makeFunValue` writes the two words FunValueLayout describes, in that order, into storage it
 * allocated for exactly that. On this target those two words are one thing: a function value *is* a
 * host function, so the code word is the value and the environment is what the closure closed over
 * rather than a second field to load at every call.
 *
 * Which means the value cannot be built until both words are known, so the code word is remembered
 * and the environment is what emits. Nothing is guessed - the pattern is one function's output and
 * anything else falls through to the ordinary write, which for a whole function value is an
 * assignment of the reference and correct.
 *
 * The environment is supplied by calling the lambda's *factory*, which is what genFunction emits in
 * place of a capturing lambda: `L$make(env)` returns a closure over `env`, so every closure of one
 * lambda is a separate function object over a separate environment, which is what a value the
 * program can hold has to be.
 */
bool genFunValueWord(Gen& g, Value& instruction, InstInit& init) {
    auto projections = init.place.projections;
    if(init.place.root != PlaceRoot::Local || projections.size() != 1) return false;

    auto projection = projections.get(g.local, 0);
    if(projection.kind != ProjectionKind::Field) return false;

    auto base = init.place;
    base.projections.clear();

    auto type = placeType(g, base);
    if(!type || g.global[type]->kind != Type::Fun) return false;

    if(projection.index == FunValueLayout::kCode) {
        auto& source = *g.local[init.value];
        if(source.kind != Value::Symbol || !((InstSymbol&)source).callee) return false;

        g.pendingCode.add(init.place.local, U32(((InstSymbol&)source).callee));
        return true;
    }

    if(projection.index != FunValueLayout::kEnv) return false;

    auto found = g.pendingCode.get(init.place.local);
    if(!found) return false;

    auto callee = ModulePtr<Function>(found.unwrap());
    auto code = functionValue(g, callee, instruction.source);

    // A lambda that captured nothing, and the thunk that makes a named function a value, are the
    // function itself: this target does not pass the environment, so there is nothing to bind.
    auto value = g.local[callee]->closureHeader ? call(g, code, useValue(g, init.value)) : code;

    emitExpr(g, assign(g, placeExpr(g, base), value));
    return true;
}

/*
 * A borrow or an address, which is free except where what is named is not an object.
 *
 * Free for the reason §2.5 gives: the checker already proved nobody else holds the storage, so
 * handing over the object reference is what hand-written JS does anyway. A whole local that is not
 * an object is *stored* boxed - see prepareLocals - so the box is already there and this is still
 * free; a field is not, so one is made here and written back after the call that consumes it.
 */
/*
 * A reference to a narrow value - Design.md's tier 2, as this target spells it.
 *
 * Native carries an address plus the shift of the field within the word it names. There are no
 * addresses here, but a place already *is* an (object, property) pair, so the reference is that pair
 * reified and needs no shift: `{$o: owner, $k: "field"}`.
 *
 * The point of it is what it is not - a copy with a write-back. Those two are the box `genBorrow`
 * makes below, and they only work while the loan ends at the call that consumed them; this one has
 * no commit point at all, so a callee may keep it, return it through a `return` parameter, or store
 * it in a record that outlives the call, exactly as on native.
 *
 * A whole local is the same shape: prepareLocals boxed it, so the pair is that box and `$v`.
 */
static bool genNarrowRef(Gen& g, ModulePtr<Value> value, Value& instruction, const Place& place,
                         TypePtr type) {
    if(!isNarrowJsValue(g, type)) return false;

    auto projections = place.projections;
    auto count = projections.size();

    // Reborrowing one: it is already a triple of exactly this shape, in whichever form that one is
    // being carried. Passing the form along rather than the object is what keeps a chain of
    // re-borrows from materializing one at each link.
    auto reborrow = [&](ModulePtr<Value> from) {
        if(auto flat = g.flatRefs.get(U32(from))) {
            g.flatRefs.add(U32(value), flat.unwrap());
            if(narrowRefNeedsObject(g, value)) {
                define(g, value, materializeRef(g, flat.unwrap()));
            }
        } else {
            define(g, value, useValue(g, from));
        }
    };

    if(!count) {
        if(place.root == PlaceRoot::Borrow || place.root == PlaceRoot::Pointer) {
            reborrow(place.pointer);
            return true;
        }

        if(place.root == PlaceRoot::Local && place.local < g.function->localCount()) {
            auto slot = g.function->localAt(g.local, place.local);
            if(slot.borrowed) {
                reborrow(slot.value);
                return true;
            }
        }
    }

    JsPtr<Expr> owner = nullptr;
    JsPtr<Expr> keyExpr = nullptr;
    PlaceBits bits;

    // The word the reference names, when nothing on the path is an object: whatever the root is.
    auto rootWord = [&]() -> bool {
        if(place.root == PlaceRoot::Borrow || place.root == PlaceRoot::Pointer) {
            // Reborrowing through a reference: it already names the word, and the shift this body
            // walked adds to the one it arrived with. That addition is what makes `&f.optionA` inside
            // a callee reach the caller's bit rather than bit zero of something.
            auto parts = refPartsOf(g, place.pointer);
            owner = parts.owner;
            keyExpr = parts.key;
            return true;
        }

        if(place.root != PlaceRoot::Local || place.local >= g.function->localCount()) return false;

        auto slot = g.function->localAt(g.local, place.local);
        if(slot.borrowed && isNarrowJsValue(g, slot.type)) {
            auto parts = refPartsOf(g, slot.value);
            owner = parts.owner;
            keyExpr = parts.key;
            return true;
        }

        // A local, which prepareLocals boxed precisely so that a reference to it has something to
        // name - so the *box* is the owner. `placeExpr` would give its contents.
        if(place.local >= g.boxed.size() || !g.boxed[place.local]) return false;

        owner = useValue(g, slot.value);
        keyExpr = asExpr(g, make<StringExpr>(g, g.boxField.text));
        return true;
    };

    if(!count) {
        if(!rootWord()) return false;
    } else {
        auto last = projections.get(g.local, count - 1);
        if(last.kind != ProjectionKind::Field) return false;

        /*
         * The split point: the longest prefix of the path whose value is still an object, because
         * that is the last thing with a property to name. Everything past it is inside one `number`
         * and contributes a bit offset rather than a property.
         *
         * `&s.flags.a`, where `Settings` is an object and `flags` a scalarized `Flags`, is the case
         * that makes this a search rather than "the last projection": the reference has to be
         * `{$o: s, $k: "flags", $s: 0}`, so both remaining steps are offsets. Splitting at the last
         * projection would name a property of a number.
         */
        Size walkedTo = count;
        while(walkedTo > 0 && !isJsObject(g, placeType(g, place, walkedTo - 1))) walkedTo--;

        if(walkedTo > 0) {
            auto atObject = walkedTo - 1;
            auto at = projections.get(g.local, atObject);
            if(at.kind != ProjectionKind::Field) return false;

            auto ownerType = placeType(g, place, atObject);
            if(!ownerType || g.global[ownerType]->kind != Type::Tup) return false;

            // The property the walk would have projected, which for a co-packed field is the word it
            // shares rather than a name of its own - and then the shift below is what says which bits
            // of that word the reference means.
            owner = placeExpr(g, place, atObject);
            keyExpr = asExpr(g, make<StringExpr>(g, fieldProperty(g, ownerType, at.index).name.text));
        } else if(!rootWord()) {
            return false;
        }

        // Everything from the word onwards, as bit offsets within it. `placeOwner` over the whole
        // path accumulates exactly these, since a step into an object contributes none.
        PlaceBits walked;
        placeOwner(g, place, walked);
        bits.offset = walked.offset;
        bits.scale = walked.scale;
        bits.width = narrowWidth(g, placeType(g, place));
    }

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

        return trivial ? value : declare(g, refPartName(g, instruction, suffix), value);
    };

    RefParts parts;
    parts.owner = part("$o"_v, owner);
    parts.key = part("$k"_v, keyExpr);

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
    if(narrowRefCarriesScale(g)) {
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

void genBorrow(Gen& g, ModulePtr<Value> value, Value& instruction, const Place& place) {
    auto type = placeType(g, place);
    auto projections = place.projections;

    if(genNarrowRef(g, value, instruction, place, type)) return;

    // An object, or a path that ends in one: the reference *is* the object, so this is a name and
    // nothing else. A borrow prepareLocals proved never leaves this function is the same statement
    // about a scalar - the emitted name of the storage, with no box between.
    if(isJsObject(g, type) || g.aliasBorrows.contains(U32(value))) {
        define(g, value, placeExpr(g, place));
        return;
    }

    /*
     * A scalar named as a whole. Where that is a local, prepareLocals already stored it boxed, so
     * the reference is the box and this instruction still emits nothing - which is what keeps
     * §3.3's "a borrow costs nothing" true for the `&counter: Int` that appears in ordinary
     * signatures. Re-borrowing something that is already a reference is the same statement.
     */
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

    // A scalar reached through a path - `&p.x`, or a module-level global. Nothing behind that
    // storage is a reference already, so one is made here and written back once the loan ends.
    auto box = declare(g, valueName(g, instruction), boxOf(g, placeExpr(g, place)));

    g.values.add(U32(value), box);
    g.writebacks.push(Writeback { value, box, place, type });
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

void genInstruction(Gen& g, ModulePtr<Inst> pointer) {
    auto& instruction = *g.local[pointer];
    auto value = (ModulePtr<Value>)pointer;

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
             * A function value has no zero worth writing: it is a host function, and the closure
             * that fills the slot is built by the environment word rather than by this - see
             * genFunValueWord. The binding still has to exist, because the two writes that follow
             * name it.
             */
            if(instruction.type && g.global[instruction.type]->kind == Type::Fun && !boxed) {
                auto name = valueName(g, instruction);
                emit(g, make<DeclStmt>(g, name, JsPtr<Expr>(nullptr), false));
                g.values.add(U32(value), variable(g, name));
                break;
            }

            auto initial = zeroValue(g, instruction.type);
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

            define(g, value, placeExpr(g, loadInst.place));
            break;
        }
        case Value::Init:
        case Value::Assign: {
            // One statement for both. Whatever the old value's drop needed was emitted as its own
            // InstDrop by the drop pass, so an assignment here is only the write.
            auto& init = (InstInit&)instruction;
            if(genFunValueWord(g, instruction, init)) break;

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
        case Value::Borrow:
            genBorrow(g, value, instruction, ((InstBorrow&)instruction).place);
            break;
        case Value::Address:
            genBorrow(g, value, instruction, ((InstAddress&)instruction).place);
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

            if(auto descriptor = genTypeDesc(g, metric.of)) {
                auto slot = metric.metric == TypeMetricKind::Align ? TypeDescFields::kAlign
                          : metric.metric == TypeMetricKind::Stride ? TypeDescFields::kStride
                          : TypeDescFields::kSize;
                define(g, value, tableCell(g, descriptor, slot));
                break;
            }

            auto& repr = g.repr.of(metric.of);
            auto number_ = metric.metric == TypeMetricKind::Align ? repr.align
                         : metric.metric == TypeMetricKind::Stride ? repr.stride
                         : repr.size;

            define(g, value, coerce(g, instruction.type, number(g, F64(number_))));
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
        case Value::Native:
            genBlockCopy(g, instruction, (InstNative&)instruction);
            break;
        default:
            g.context.diagnostics.error("internal error: unexpected instruction in JS codegen"_v,
                                        instruction.source);
            break;
    }

    flushWritebacks(g, instruction);
}

} // namespace js
