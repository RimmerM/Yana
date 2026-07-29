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

            auto& entry = g.writebacks[i];
            emitExpr(g, assign(g, entry.place, field(g, entry.box, g.boxField)));
            g.writebacks.remove(i);
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

    emitExpr(g, call(g, tableCell(g, descriptor, TypeDescLayout::kMoveInit),
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

void storeInto(Gen& g, const Place& place, TypePtr type, ModulePtr<Value> value) {
    auto& produced = *g.local[value];
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

    // Bool is a host `boolean`, so its three bitwise operations are the logical ones. `^` on two
    // booleans is inequality, which is what Core's `xor` on Bool means.
    if(isBool(g, type)) {
        switch(instruction.kind) {
            case Value::And: define(g, pointer, binary(g, BinaryOp::LogicalAnd, lhs, rhs)); return;
            case Value::Or: define(g, pointer, binary(g, BinaryOp::LogicalOr, lhs, rhs)); return;
            case Value::Xor: define(g, pointer, binary(g, BinaryOp::Ne, lhs, rhs)); return;
            default: break;
        }
    }

    auto simple = [&](BinaryOp op) {
        define(g, pointer, coerce(g, type, binary(g, op, lhs, rhs)));
    };

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

    auto argument = referenceTo(g, instruction.place);

    if(g.genEnv && isGeneric(g.global, type)) {
        // The teardown the caller's descriptor names, reached through the same cell read every
        // other erased operation uses.
        if(auto descriptor = genTypeDesc(g, type)) {
            emitExpr(g, call(g, tableCell(g, descriptor, TypeDescLayout::kDrop), argument));
        }

        return;
    }

    emitExpr(g, call(g, functionValue(g, instruction.drop, instruction.source), argument));
}

void genCall(Gen& g, ModulePtr<Value> pointer, InstCall& instruction) {
    Array<JsPtr<Expr>> args;
    for(auto arg: instruction.args.contents(g.local)) args.push(useValue(g, arg));

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

    for(auto arg: instruction.args.contents(g.local)) args.push(useValue(g, arg));
    define(g, pointer, callWith(g, callee, args));
}

// The environment assembled at the call site: the slots this caller knows concretely, and the ones
// it forwards out of its own.
JsPtr<Expr> genEnvTable(Gen& g, InstGenCall& instruction) {
    auto table = make<ArrayExpr>(g);

    // The schema word the layout puts in front of the slots. Emitted code never reads it; it is
    // here so that cell indices match the offsets the layout states.
    for(U32 i = 0; i < GenEnvLayout::kSlots / 4; i++) {
        table->values.push(g.file.arena, number(g, 0));
    }

    for(auto slot: instruction.fill.contents(g.local)) {
        auto value = slot.isForwarded()
            ? genWitness(g, slot.forwarded, slot.forwardedSupers)
            : globalValue(g, slot.constant);

        table->values.push(g.file.arena, value);
        table->values.push(g.file.arena, number(g, 0));
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
        callee = tableCell(g, witness, ClassWitnessLayout::methodsOffset(argCount) + 8 * instruction.index);
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
        auto value = useValue(g, arg);
        auto concrete = g.local[arg]->type;

        auto parameter = argIndex < function->args.size()
            ? g.local[function->args.get(g.local, argIndex)] : nullptr;

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
void genBorrow(Gen& g, ModulePtr<Value> value, Value& instruction, const Place& place) {
    auto type = placeType(g, place);
    auto projections = place.projections;

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
    auto target = placeExpr(g, place);
    auto box = declare(g, valueName(g, instruction), boxOf(g, target));

    g.values.add(U32(value), box);
    g.writebacks.push(Writeback { value, box, target });
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
        auto temporary = declare(g, generatedName(g, "swap"_v, instruction.id), a);
        emitExpr(g, assign(g, a, b));
        emitExpr(g, assign(g, b, temporary));
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
        define(g, value, placeExpr(g, exchange.place));
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
        case Value::LoadPlace:
            define(g, value, placeExpr(g, ((InstLoadPlace&)instruction).place));
            break;
        case Value::Init:
        case Value::Assign: {
            // One statement for both. Whatever the old value's drop needed was emitted as its own
            // InstDrop by the drop pass, so an assignment here is only the write.
            auto& init = (InstInit&)instruction;
            if(genFunValueWord(g, instruction, init)) break;

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
            define(g, value, coerce(g, instruction.type, unary(g, UnaryOp::Neg, from)));
            break;
        }
        case Value::Not: {
            auto operand = useValue(g, ((InstUnary&)instruction).from);

            if(isBool(g, instruction.type)) {
                define(g, value, unary(g, UnaryOp::Not, operand));
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
                auto offset = metric.metric == TypeMetricKind::Align ? TypeDescLayout::kAlign
                            : metric.metric == TypeMetricKind::Stride ? TypeDescLayout::kStride
                            : TypeDescLayout::kSize;
                define(g, value, tableCell(g, descriptor, offset));
                break;
            }

            auto& repr = g.program.repr.of(metric.of);
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
