#include "internal.h"

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
 *
 * ## Where the environment is, and how a drop finds out - D17
 *
 * The second word is one of three things and a holder of the pair may not ask which: null, the
 * address of a **local of the frame that built the closure**, or the address of a **heap block this
 * file allocates**. A block starts with its teardown - the function that releases the captures and
 * hands the block back - and a frame environment has none, so a drop has to tell them apart without
 * reading anything.
 *
 * **The low bit of the word says so.** A heap block's address is stored with bit zero set; a frame
 * address and null have it clear. The alternative was `known.def`'s `isHeapAddress`, and what ruled
 * it out is not a benchmark: that role is `Presence::Optional` and the *default* allocator does not
 * fill it, so a closure teardown built on it would be unwritable in the build everything uses. The
 * tag also wins on both costs. A drop of a value the compiler can see the lambda of costs nothing at
 * all, because it is not emitted - `mir::ownershipOfOccurrence` answers "nothing to release" for a
 * value carrying `Shapes::closure`, which every frame-placed closure does. And the **call** pays one
 * instruction in the callee and none at the site: `funEnvironmentBias` is added to the environment
 * parameter of a heap-placed lambda once, in its entry block, which is a constant the backend folds
 * into the displacement of the first capture it reads. `code` is called with `env` exactly as the
 * pair holds it.
 */

/*
 * How far past a block's start its environment record is, and what the pair's word carries.
 *
 * `kClosureEnvTag` is the bit; the header is one pointer at offset zero and the record follows it,
 * aligned to whatever the record wants - a `Vec` capture asks for sixteen and a block that answered
 * eight would be a misaligned load. Both ends of both numbers are in this file: `emitFunValue`
 * writes them, `closureBlockTeardown` reads the record back out and `funEnvironmentBias` is what the
 * lambda's own entry block adds.
 */
static constexpr U64 kClosureEnvTag = 1;

U32 Lowering::closureHeaderBytes(DeclPtr<mir::Type> environment) {
    auto manifest = manifestNative(layout, mir::owned(environment));
    auto align = manifest.align > layout.target.pointerAlign ? manifest.align
                                                             : layout.target.pointerAlign;

    return align;
}

U32 Lowering::funEnvironmentBias(DeclPtr<mir::Function> lambda) {
    if(!decl[lambda]->environment ||
       decl[lambda]->environmentStorage == mir::StorageClass::Frame) {
        return 0;
    }

    return closureHeaderBytes(decl[lambda]->environment) - U32(kClosureEnvTag);
}

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

    auto result = manifestNative(layout, callee.result);
    auto calleePlace = returnPlaces.getValue(concrete.offset);

    /*
     * The thunk's own signature is the function type's, so it takes a pointer wherever the result
     * is in memory - including where the callee returns that result as its parts, which the thunk
     * then writes through the pointer. Its parameters are the declared manifestations for the same
     * reason, and a parameter the callee takes apart is loaded out of the address it arrives at.
     */
    auto shape = callShapeOf(concrete);
    auto splitResult = shape && shape->splitsResult();

    LowerPtr<LowerValue> resultPlace = nullptr;
    if(result.inMemory) resultPlace = &function->addArg(lower, StringId(), LowerType::Pointer)->result - lower;

    SmallArray<LowerPtr<LowerValue>, 8> incoming;
    for(auto argument: callee.args.contents(decl)) {
        auto manifest = manifestNative(layout, decl[argument]->occurrence);
        incoming.push(&function->addArg(lower, StringId(), manifest.type)->result - lower);
    }

    auto returns = !result.inMemory && decl[callee.result.type]->kind != mir::Type::Unit;
    if(returns) function->returnTypes.push(to.arena, result.type);

    auto block = function->addBlock(lower, StringId());

    SmallArray<LowerPtr<LowerValue>, 8> passed;
    for(U32 i = 0; i < incoming.size(); i++) passCarried(*block, shape, i, incoming[i], passed);

    auto calleeValue = block->addInst(lower, new (to.arena) LowerInstFun(callee.symbol,
                                                                        target.unwrap()));

    auto hidden = calleePlace ? 1u : 0u;
    auto convention = lower[target.unwrap()]->callType;
    auto created = returns ? 1u : splitResult ? shape->resultCount : 0u;

    auto called = call(lower, to, *block, created, passed.size() + 1 + hidden, convention,
                       [&](LowerInstCall* built) {
        if(returns) new (built->created().ptr) LowerValue(built, result.type, StringId());
        for(U32 i = 0; splitResult && i < created; i++) {
            new (built->created().ptr + i) LowerValue(built, resultPart(*shape, i).type, StringId());
        }

        built->used()[0] = ((LowerInstSingle*)calleeValue)->created().ptr - lower;

        Size index = 1;

        // The caller's storage handed straight on. A thunk over a plain function adapts nothing
        // about the result: both ends agree about the type, so they agree about how it comes back.
        if(hidden) built->used()[index++] = resultPlace;
        for(Size i = 0; i < passed.size(); i++) built->used()[index++] = passed[i];
    });

    if(splitResult) storeReturnedParts(*block, *shape, called, resultPlace);

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
                                                            U64(layout.target.funSize())));

    auto storage = block.addInst(lower, new (to.arena) LowerInstAlloca(
        StringId(), ((LowerInstSingle*)size)->created().ptr - lower, layout.target.pointerAlign));

    auto address = ((LowerInstSingle*)storage)->created().ptr - lower;

    auto word = block.addInst(lower, new (to.arena) LowerInstFun(lower[code]->name, code));
    block.addInst(lower, new (to.arena) LowerInstStore(
        address, ((LowerInstSingle*)word)->created().ptr - lower, layout.target.pointerSize));

    /*
     * The environment, or the null a value that captured nothing holds.
     *
     * Written either way rather than left to whatever the frame held, because a call through the
     * pair reads it unconditionally: what makes a plain function callable across this shape is that
     * the word is *there* and the thunk ignores it, not that anything checks it first.
     *
     * A **heap-placed** one is built here rather than passed in: what the MIR handed over is the
     * environment record as a value of this frame, and the block that outlives the frame is storage
     * the lowering asks for - the same arrangement `@box` has, one tier down from where the
     * ownership was decided.
     */
    LowerPtr<LowerValue> held = environment;

    if(held && decl[callee]->environmentStorage != mir::StorageClass::Frame) {
        held = emitClosureBlock(block, decl[callee]->environment, held, source);
        if(!held) return nullptr;
    }

    storeWord(block, address, layout.target.funEnvOffset(),
              held ? held : noEnvironment(block));
    return address;
}

/*
 * The heap block a closure that leaves its frame carries - its teardown, then the captures.
 *
 * Three stores and a call, and the last of them is the tag: the address goes into the pair with bit
 * zero set, which is the whole of how a drop at a site that cannot name this lambda finds the
 * teardown. `environment` is the record the MIR built on the frame and relocated into this value, so
 * the copy below is that relocation - a move of bytes, which is what a relocation is here.
 */
LowerPtr<LowerValue> Lowering::emitClosureBlock(LowerBlock& block, DeclPtr<mir::Type> environment,
                                                LowerPtr<LowerValue> from, LocationId source) {
    auto callee = roleFunction(intrinsic::Known::allocateHeap, source);
    if(!callee) return nullptr;

    auto manifest = manifestNative(layout, mir::owned(environment));
    auto header = closureHeaderBytes(environment);
    auto bytes = header + strideOf(manifest);

    auto name = block.addInst(lower, new (to.arena) LowerInstFun(StringId(), callee));

    auto allocated = call(lower, to, block, 1, 2, lower[callee]->callType,
                          [&](LowerInstCall* built) {
        new (built->created().ptr) LowerValue(built, LowerType::Pointer, StringId());
        built->used()[0] = ((LowerInstSingle*)name)->created().ptr - lower;
        built->used()[1] = immediate(block, LowerType::Int64, U64(bytes));
    });

    auto address = allocated->created().ptr - lower;

    // The teardown, at offset zero - the one word a holder of the pair can reach without knowing
    // what was captured, which is the whole reason the block has a header at all.
    auto teardown = closureBlockTeardown(environment, source);
    auto pointer = block.addInst(lower, new (to.arena) LowerInstFun(lower[teardown]->name,
                                                                    teardown));

    block.addInst(lower, new (to.arena) LowerInstStore(
        address, ((LowerInstSingle*)pointer)->created().ptr - lower, layout.target.pointerSize));

    /*
     * And the captures, moved in - **not** zeroed first, which is why this is not a fourth statement
     * of `createBox`'s rule (`inst.cpp`'s note, and the README's "all three allocation sites").
     *
     * The rule is for a block whose fields are written one at a time, where whatever `allocateHeap`
     * handed back is still readable between them. What happens here is a single whole-range copy of
     * `manifest.size` bytes, padding included: every byte of the environment comes from `from`, so a
     * zero-fill ahead of it would write bytes the copy immediately overwrites. It did, and the
     * `hasPaddedWord` guard in front of it made that look deliberate.
     */
    auto record = offsetAddress(block, address, header);

    block.addInst(lower, new (to.arena) LowerInstCopy(
        record, from, immediate(block, LowerType::Int64, U64(manifest.size))));

    auto tagged = block.addInst(lower, new (to.arena) LowerInstBinary(
        StringId(), LowerType::Pointer, address, immediate(block, LowerType::Int64, kClosureEnvTag),
        LowerInst::Or));

    return ((LowerInstSingle*)tagged)->created().ptr - lower;
}

/*
 * What a heap-placed environment owes when the value holding it dies - the block's header word.
 *
 * Interned on the environment record, because there is exactly one lambda per record and one block
 * shape per lambda. It takes the **block**, not the record: the untagged word is what the `Fun`
 * teardown has, and skipping the header here rather than at the drop keeps the offset in the one
 * file that knows it.
 *
 * `teardownHalves` is what runs the captures, asked for both halves and for no release of its own -
 * the block is not the record's storage, it is the record's storage plus a header, so the free below
 * is this function's rather than something delegated.
 */
LowerPtr<LowerFunction> Lowering::closureBlockTeardown(DeclPtr<mir::Type> environment,
                                                       LocationId source) {
    if(auto found = closureTeardowns.getValue(environment.offset)) return found.unwrap();

    auto function = to.addFunction(erasedName(
        String("closure(") + mir::typeText(context, decl, environment) + String(")")));

    auto argument = function->addArg(lower, StringId(), LowerType::Pointer);
    auto handle = function - lower;
    closureTeardowns.add(environment.offset, handle);

    auto block = function->addBlock(lower, StringId());
    auto address = &argument->result - lower;

    if(mir::hasTeardown(decl, environment)) {
        auto callee = teardownHalves(environment, true, true, false, source, BoxRelease::Owned);
        auto name = block->addInst(lower, new (to.arena) LowerInstFun(StringId(), callee));

        auto record = offsetAddress(*block, address, closureHeaderBytes(environment));

        call(lower, to, *block, 0, 2, lower[callee]->callType, [&](LowerInstCall* built) {
            built->used()[0] = ((LowerInstSingle*)name)->created().ptr - lower;
            built->used()[1] = record;
        });
    }

    emitFreeHeap(*block, address, source);
    emitReturn(*block, nullptr, 0);
    return handle;
}

/*
 * A function value's teardown, in one place - the tag test, and the header behind it.
 *
 * One function for the whole program rather than one per function type, because nothing about it is
 * the type's: the pair is two words at the same two offsets whatever it was made from, and what
 * decides whether there is anything to run is a bit in the second of them. So a drop of a `Fun` is
 * one call, and every `Fun` in the program shares the callee.
 *
 * The header is called across `kDefaultCallType` for `emitErasedTeardown`'s reason - an indirect
 * call has no callee to read a convention off, so there is one convention on this side of the
 * boundary rather than a lookup that could find a different one.
 */
LowerPtr<LowerFunction> Lowering::funTeardown(LocationId source) {
    if(funRelease) return funRelease;

    auto function = to.addFunction(erasedName(String("teardown(fun)")));
    auto argument = function->addArg(lower, StringId(), LowerType::Pointer);

    funRelease = function - lower;

    auto entry = function->addBlock(lower, StringId());
    auto owned = function->addBlock(lower, StringId());
    auto done = function->addBlock(lower, StringId());

    auto pair = &argument->result - lower;

    /*
     * The word as a number for the test and as an address for the arithmetic - two loads of one
     * slot, and the second is inside the arm that needs it.
     *
     * This tier types a pointer, so a value cannot be both; and reading it as an address on the
     * common path would put a load in front of a branch that almost always leaves. What the split
     * costs is one load on the path that is about to make an indirect call anyway.
     */
    auto word = wordAt(*entry, pair, layout.target.funEnvOffset(), LowerType::Int64,
                       layout.target.pointerSize);

    auto bit = entry->addInst(lower, new (to.arena) LowerInstBinary(
        StringId(), LowerType::Int64, word, immediate(*entry, LowerType::Int64, kClosureEnvTag),
        LowerInst::And));

    auto zero = entry->addInst(lower, new (to.arena) LowerInstCmp(
        StringId(), ((LowerInstSingle*)bit)->created().ptr - lower,
        immediate(*entry, LowerType::Int64, 0), LowerCmp::eq));

    entry->addInst(lower, new (to.arena) LowerInstJe(
        ((LowerInstSingle*)zero)->created().ptr - lower, done - lower, owned - lower));

    // The block, which is the word with the tag taken off - a subtraction and not a mask, because
    // the bit is known to be set on this edge and one form is what the backend folds into the load.
    auto held = wordAt(*owned, pair, layout.target.funEnvOffset(), LowerType::Pointer,
                       layout.target.pointerSize);

    auto block = owned->addInst(lower, new (to.arena) LowerInstBinary(
        StringId(), LowerType::Pointer, held, immediate(*owned, LowerType::Int64, kClosureEnvTag),
        LowerInst::Sub));

    auto address = ((LowerInstSingle*)block)->created().ptr - lower;
    auto header = wordAt(*owned, address, 0, LowerType::Pointer, layout.target.pointerSize);

    call(lower, to, *owned, 0, 2, kDefaultCallType, [&](LowerInstCall* built) {
        built->used()[0] = header;
        built->used()[1] = address;
    });

    owned->addInst(lower, new (to.arena) LowerInstJmp(done - lower));
    emitReturn(*done, nullptr, 0);

    (void)source;
    return funRelease;
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

    block.addInst(lower, new (to.arena) LowerInstStore(at, value, layout.target.pointerSize));
}

} // namespace ssa
