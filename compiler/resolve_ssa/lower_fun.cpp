#include "lower_internal.h"

namespace ssa {

/*
 * Function values - Analysis-IR.md §11.4's G7, and every number the representation turns on.
 *
 * A function value is **two words**: a code address and the environment its captures live in. The
 * pair is one type however it was made - a plain function named as a value, a lambda that captured
 * nothing, a lambda that captured five things - and what makes that possible is that `code` always
 * takes the environment as its first argument. A call site therefore never has to know which of the
 * three it is holding, which is the whole of the design: it is what lets a function type be a type
 * rather than a family of them.
 *
 * This is a file for `erase.cpp`'s reason. The offsets are in `manifest.h` because the layout is a
 * manifestation, and both ends of every *other* number here - the thunk's argument order, what a
 * null environment means - are functions below.
 *
 * A plain function goes through a **thunk**, and that is the one cost the uniformity has. `fn
 * add(a: I32, b: I32)` declares its own convention and a value of `(I32, I32) -> I32` is called
 * across the one above, so something has to absorb the environment argument. It is a forwarding
 * call and nothing else - see `funThunkFor`, which is `erase.cpp`'s `thunkFor` without the erased
 * positions to adapt, because a function type's positions are concrete by construction.
 *
 * A **lifted lambda** needs no thunk at all, and that is the same sentence read from the other end:
 * it was built with the environment as its first parameter, so it already has the shape a function
 * value is called across. The thunk exists to give a declaration that shape, not to give the pair
 * one.
 */

/*
 * A plain function, as the code word of a function value.
 *
 * Interned on the declaration, so a name used as a value in ten places is one thunk. The environment
 * parameter is first and unused, exactly as it is in a witness thunk and for the same sentence:
 * everything reached through a pointer is called the same way, and a shape that varied with whether
 * the callee happened to need one could not be called indirectly at all.
 */
LowerPtr<LowerFunction> Lowering::funThunkFor(DeclPtr<mir::Function> concrete, LocationId source) {
    if(auto found = funThunks.getValue(concrete.offset)) return found.unwrap();

    auto target = functions.getValue(concrete.offset);
    if(!target) {
        context.diagnostics.error("internal: a function value names a function that was not lowered"_v,
                                  source);
        return nullptr;
    }

    auto& callee = *decl[concrete];
    auto function = to.addFunction(
        erasedName(String("funvalue(") + context.findName(callee.symbol) + String(")")));

    auto handle = function - lower;
    funThunks.add(concrete.offset, handle);

    // The environment, first and unused - see the note above.
    function->addArg(lower, StringId(), LowerType::Pointer);

    auto result = manifestNative(decl, callee.result);
    auto calleePlace = returnPlaces.getValue(concrete.offset);

    LowerPtr<LowerValue> resultPlace = nullptr;
    if(calleePlace) resultPlace = &function->addArg(lower, StringId(), LowerType::Pointer)->result - lower;

    SmallArray<LowerPtr<LowerValue>, 8> passed;
    for(auto argument: callee.args.contents(decl)) {
        auto manifest = manifestNative(decl, decl[argument]->occurrence);
        passed.push(&function->addArg(lower, StringId(), manifest.type)->result - lower);
    }

    auto returns = !result.inMemory && decl[callee.result.type]->kind != mir::Type::Unit;
    if(returns) function->returnTypes.push(to.arena, result.type);

    auto block = function->addBlock(lower, StringId());
    auto calleeValue = block->addInst(lower, new (to.arena) LowerInstFun(callee.symbol,
                                                                        target.unwrap()));

    auto hidden = calleePlace ? 1u : 0u;
    auto convention = lower[target.unwrap()]->callType;

    auto called = call(lower, to, *block, returns ? 1u : 0u, passed.size() + 1 + hidden, convention,
                       [&](LowerInstCall* built) {
        if(returns) new (built->created().ptr) LowerValue(built, result.type, StringId());

        built->used()[0] = ((LowerInstSingle*)calleeValue)->created().ptr - lower;

        Size index = 1;

        // The caller's storage handed straight on. A thunk over a plain function adapts nothing
        // about the result: both ends agree about the type, so they agree about how it comes back.
        if(hidden) built->used()[index++] = resultPlace;
        for(Size i = 0; i < passed.size(); i++) built->used()[index++] = passed[i];
    });

    auto returned = returns ? called->created().ptr - lower : LowerPtr<LowerValue>(nullptr);
    emitReturn(*block, &returned, returns ? 1u : 0u);

    return handle;
}

/*
 * The pair, built into storage this frame supplies.
 *
 * A function value is address-only, so what a `fun.value` produces is where the two words are rather
 * than the two words themselves - which is exactly what an aggregate produces, and for the same
 * reason: the shared IR has no pair.
 */
LowerPtr<LowerValue> Lowering::emitFunValue(LowerBlock& block, DeclPtr<mir::Function> callee,
                                            LowerPtr<LowerValue> environment, LocationId source) {
    /*
     * Which code the pair points at, which is the one place the two kinds differ.
     *
     * A lifted lambda already takes an environment first, so the pair points at the lambda itself; a
     * declaration does not, so it points at a thunk that absorbs one. Everything after this line is
     * the same for both, which is what "a function type is a type rather than a family of them"
     * means in code.
     */
    LowerPtr<LowerFunction> code = nullptr;

    if(decl[callee]->environment) {
        auto found = functions.getValue(callee.offset);
        if(!found) {
            context.diagnostics.error(
                "internal: a function value names a function that was not lowered"_v, source);
            return nullptr;
        }

        code = found.unwrap();
    } else {
        code = funThunkFor(callee, source);
    }

    if(!code) return nullptr;

    // The byte count as a value rather than a field, which is what `LowerInstAlloca` takes - see
    // `frameStorage`, which allocates every other kind of storage this stage makes.
    auto size = block.addInst(lower, new (to.arena) LowerImm(StringId(), LowerType::Int64,
                                                            U64(kFunSize)));

    auto storage = block.addInst(lower, new (to.arena) LowerInstAlloca(
        StringId(), ((LowerInstSingle*)size)->created().ptr - lower, kPointerAlign));

    auto address = ((LowerInstSingle*)storage)->created().ptr - lower;

    auto word = block.addInst(lower, new (to.arena) LowerInstFun(lower[code]->name, code));
    block.addInst(lower, new (to.arena) LowerInstStore(
        address, ((LowerInstSingle*)word)->created().ptr - lower, kPointerSize));

    /*
     * The environment, or the null a value that captured nothing holds.
     *
     * Written either way rather than left to whatever the frame held, because a call through the
     * pair reads it unconditionally: what makes a plain function callable across this shape is that
     * the word is *there* and the thunk ignores it, not that anything checks it first.
     */
    storeWord(block, address, kFunEnvOffset, environment ? environment : noEnvironment(block));
    return address;
}

// One word written into a table this frame is building. The offset arithmetic is `wordAt`'s read
// half, spelled once so that a write and a read of the same slot cannot disagree about where it is.
void Lowering::storeWord(LowerBlock& block, LowerPtr<LowerValue> address, U32 offset,
                         LowerPtr<LowerValue> value) {
    auto at = address;
    if(offset) {
        auto constant = block.addInst(lower, new (to.arena) LowerImm(
            StringId(), LowerType::Int64, U64(offset)));

        at = ((LowerInstSingle*)block.addInst(lower, new (to.arena) LowerInstBinary(
            StringId(), LowerType::Pointer, at,
            ((LowerInstSingle*)constant)->created().ptr - lower, LowerInst::Add)))->created().ptr - lower;
    }

    block.addInst(lower, new (to.arena) LowerInstStore(at, value, kPointerSize));
}

} // namespace ssa
