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
 * file allocates**. Only the third owes anything when the value dies, so a drop at a site that
 * cannot name the lambda has to tell them apart.
 *
 * **The code word already says so, and it costs nothing to carry.** Which of the three a lambda
 * uses is a compile-time constant per lambda - one lambda expression is one allocation site, and
 * `resolve_mir` decides it from the position the lambda was written at - so the answer belongs with
 * the *code*, which is the one word that names the lambda, and not with each value made from it.
 * Four bytes of **prefix data** in front of the entry point hold it: `attachClosureHeader` writes
 * them and `funTeardown` reads them back at `[code - kSlotBytes]`. A closure costs no header bytes,
 * no header store and no tagging at the site that builds it, and nothing at all at the site that
 * calls it - `code` is called with `env` exactly as the pair holds it.
 *
 * **A zero cell is a lambda with nothing to release.** Otherwise the cell holds
 * `teardown - &image$base`, the way every other address cell in this stage does (`erase.cpp`, where
 * the encoding is argued), and that difference is never zero: the anchor is placed where no
 * function can be - after all the code on the x64 backend, in a data section or at address zero
 * under LLVM, and behind `.text.entry` in a raw image. So a drop of a lambda that owes nothing is a
 * load and a not-taken branch, which is what the alternative below got right and what
 * `compiler/resolve` (the stage deleted in J173) - always a callable slot, always an indirect call,
 * `teardown$none` where there was nothing to do - did not.
 *
 * **The environment word is still asked, first**, because the cell is the *lambda's* answer and a
 * value of that lambda may hold no environment at all: a lambda that captured nothing, a plain
 * function named as a value, and the empty pair a boxed iterator member holds before its region is
 * entered, which `convert.cpp`'s `stepStoreEmptyBox` builds out of the *same* shim a filled one
 * points at. So `funTeardown` asks two questions - see it, where the order is argued - and they are
 * `compiler/resolve`'s two with one test more.
 *
 * **What this replaced**, and why it is written down: the first form of D17 put the answer in the
 * *value* instead. The block was `[teardown][captures]` and the pair carried its address with bit
 * zero set, so a heap closure cost eight bytes of header - sixteen where a `Vec` capture raised the
 * record's alignment - plus a store and an `or` at every creation, and `funEnvironmentBias` put an
 * `add` in the callee's entry block to skip the header again. That is a cost per closure *instance*
 * for a fact that is constant per lambda. The trade it was making is real and is the reason a zero
 * cell is tested rather than called: a tag on the word is cheaper at a drop that finds nothing to do
 * than an unconditional call through a header would be. Answering both with one cell in front of
 * the code is what took the instance cost away without giving that back. `known.def`'s `isHeapAddress` was
 * the alternative to the tag and was ruled out before either: that role is `Presence::Optional` and
 * the *default* allocator does not fill it.
 *
 * "Immediately in front of the entry point" is a real constraint on the backend rather than a
 * convention - see `LowerFunction::prefix`, `AsmModule::startFunction`, and LLVM's prefix data,
 * which is the same thing under the same name.
 */

/*
 * The cell in front of a lifted function - D17, and one of the two ends of it.
 *
 * Interned on the *function*, because that is what it is in front of: one lambda is one placement
 * and one environment record, so the same four bytes answer every value ever made from it, however
 * many sites make one. `teardown` null is a frame-placed environment, which owes nothing, and the
 * cell is then zero. A heap-placed one always has one, whether or not its captures owe anything,
 * because the block has to be handed back.
 *
 * **Only a lambda with an environment has a cell**, which was `compiler/resolve`'s rule too. Every
 * code word a pair may hold comes through `emitFunValue`, which calls this before it stores the
 * word, and `funTeardown` reads `[code - kSlotBytes]` only past its null test - so a plain
 * function's thunk and a lambda that captured nothing, whose pairs always hold null, would carry
 * four bytes nothing reads. The one pair with a null word *and* a cell is the empty box, whose shim
 * is heap-placed. What that makes the null test is load-bearing twice: it is what tells an empty
 * box from a filled one, and it is what keeps the read off a function with nothing in front of it.
 */
void Lowering::attachClosureHeader(LowerPtr<LowerFunction> code, LowerPtr<LowerFunction> teardown) {
    auto target = lower[code];

    // Built once. A second call with the same answer would be harmless and a second call with a
    // different one is a contradiction about a lambda's placement, which `assertTrue` is the right
    // shape for - see the interning note above.
    if(target->prefix) {
        assertTrue((teardown != nullptr) == (lower[target->prefix]->relocations.size() != 0));
        return;
    }

    auto global = new (to.arena) LowerGlobal(erasedName(
        String("header(") + context.findName(target->name) + String(")")));

    global->mut = false;
    global->initialContents = ByteBuffer((Byte*)to.arena.alloc(kSlotBytes), kSlotBytes);
    set(global->initialContents.ptr, kSlotBytes, 0);

    /*
     * Not in `to.globalOrder`, which is the whole of what prefix data is: a global in that list is
     * emitted wherever the module's data goes, and these bytes have exactly one place they may be.
     * `LowerFunction::prefix` is how they are reached, and the printer prints them there.
     */
    if(teardown) addPointer(*global, 0, teardown, nullptr);

    target->prefix = global - lower;
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

    /*
     * What this closure owes when it dies, decided here and spent on the code rather than on the
     * value - D17.
     *
     * A heap-placed environment owes its captures and its block and a frame-placed one owes
     * nothing, and gets a zero cell; a plain function gets no cell at all - see
     * `attachClosureHeader`. Asked of the **declaration** and not of this site,
     * because the cell is the code's and the code is shared: `stepStoreEmptyBox` builds a pair out
     * of a heap-placed shim with a null environment word, so a site with nothing to release is not
     * a lambda with nothing to release. Which is why the word is tested too - `funTeardown`.
     */
    auto heapPlaced = decl[callee]->environment &&
                      decl[callee]->environmentStorage != mir::StorageClass::Frame;

    LowerPtr<LowerFunction> teardown = nullptr;
    if(heapPlaced) {
        teardown = closureBlockTeardown(decl[callee]->environment, source);
        if(!teardown) return nullptr;
    }

    if(decl[callee]->environment) attachClosureHeader(code, teardown);

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

    if(held && heapPlaced) {
        held = emitClosureBlock(block, decl[callee]->environment, held, source);
        if(!held) return nullptr;
    }

    storeWord(block, address, layout.target.funEnvOffset(),
              held ? held : noEnvironment(block));
    return address;
}

/*
 * The heap block a closure that leaves its frame carries - the captures, and nothing in front of
 * them.
 *
 * One call and one copy. The block *is* the environment record: what a drop needs to know about it
 * is in front of the lambda's code rather than at the head of the block, so there is no header to
 * reserve, no header to store and no tag to put on the address - D17, above. `environment` is the
 * record the MIR built on the frame and relocated into this value, so the copy below is that
 * relocation - a move of bytes, which is what a relocation is here.
 */
LowerPtr<LowerValue> Lowering::emitClosureBlock(LowerBlock& block, DeclPtr<mir::Type> environment,
                                                LowerPtr<LowerValue> from, LocationId source) {
    auto callee = roleFunction(intrinsic::Known::allocateHeap, source);
    if(!callee) return nullptr;

    auto manifest = manifestNative(layout, mir::owned(environment));

    auto name = block.addInst(lower, new (to.arena) LowerInstFun(StringId(), callee));

    auto allocated = call(lower, to, block, 1, 2, lower[callee]->callType,
                          [&](LowerInstCall* built) {
        new (built->created().ptr) LowerValue(built, LowerType::Pointer, StringId());
        built->used()[0] = ((LowerInstSingle*)name)->created().ptr - lower;
        built->used()[1] = immediate(block, LowerType::Int64, U64(strideOf(manifest)));
    });

    auto address = allocated->created().ptr - lower;

    /*
     * The captures, moved in - **not** zeroed first, which is why this is not a fourth statement
     * of `createBox`'s rule (`inst.cpp`'s note, and the README's "all three allocation sites").
     *
     * The rule is for a block whose fields are written one at a time, where whatever `allocateHeap`
     * handed back is still readable between them. What happens here is a single whole-range copy of
     * `manifest.size` bytes, padding included: every byte of the environment comes from `from`, so a
     * zero-fill ahead of it would write bytes the copy immediately overwrites. It did, and the
     * `hasPaddedWord` guard in front of it made that look deliberate.
     */
    block.addInst(lower, new (to.arena) LowerInstCopy(
        address, from, immediate(block, LowerType::Int64, U64(manifest.size))));

    return address;
}

/*
 * What a heap-placed environment owes when the value holding it dies - what the cell in front of
 * the lambda names.
 *
 * Interned on the environment record, because there is exactly one lambda per record and one block
 * shape per lambda. It takes the environment word exactly as the pair holds it, which is the block
 * itself: nothing is in front of the record, so there is no offset here for a drop and a creation
 * to disagree about.
 *
 * `teardownHalves` is what runs the captures, asked for both halves and for no release of its own -
 * the storage under the record is this function's to hand back, since this is what a heap-placed
 * environment owes and the record itself does not know it is on the heap.
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

        call(lower, to, *block, 0, 2, lower[callee]->callType, [&](LowerInstCall* built) {
            built->used()[0] = ((LowerInstSingle*)name)->created().ptr - lower;
            built->used()[1] = address;
        });
    }

    emitFreeHeap(*block, address, source);
    emitReturn(*block, nullptr, 0);
    return handle;
}

/*
 * A function value's teardown, in one place - the environment word, the cell in front of the code,
 * and the call behind it.
 *
 * One function for the whole program rather than one per function type, because nothing about it is
 * the type's: the pair is two words at the same two offsets whatever it was made from. So a drop of
 * a `Fun` is one call, and every `Fun` in the program shares the callee.
 *
 * **Two questions and not one**, and they are different questions. Whether this *value* holds an
 * environment at all is the second word: null is a lambda that captured nothing, a plain function
 * named as a value, and the empty pair a boxed iterator member holds before its region is entered -
 * `convert.cpp`'s `stepStoreEmptyBox`, which builds it out of the same shim a filled one points at,
 * so the code word cannot be asked. Whether *that lambda* owes anything is the cell in front of the
 * code, and it is what tells a frame-placed environment from a heap-placed one.
 *
 * The null test comes first, and it has to: only a lambda with an environment has a cell
 * (`attachClosureHeader`), so the order is what keeps the read off a plain function's thunk. It is
 * also the cheaper of the two - one load against the cell's two dependent ones, the code word and
 * then the bytes in front of it. What the split costs over `compiler/resolve`'s form is nothing:
 * that one tested null as well and then called through the header unconditionally, so a frame
 * closure paid an indirect call to a teardown that did nothing where this pays a not-taken branch.
 *
 * The teardown is called across `kDefaultCallType` for `emitErasedTeardown`'s reason - an indirect
 * call has no callee to read a convention off, so there is one convention on this side of the
 * boundary rather than a lookup that could find a different one.
 */
LowerPtr<LowerFunction> Lowering::funTeardown(LocationId source) {
    if(funRelease) return funRelease;

    auto function = to.addFunction(erasedName(String("teardown(fun)")));
    function->teardown = true;
    auto argument = function->addArg(lower, StringId(), LowerType::Pointer);

    funRelease = function - lower;

    auto entry = function->addBlock(lower, StringId());
    auto header = function->addBlock(lower, StringId());
    auto owned = function->addBlock(lower, StringId());
    auto done = function->addBlock(lower, StringId());

    auto pair = &argument->result - lower;

    /*
     * The environment word as a number for the test and as an address for the call - two loads of
     * one slot, and the second is inside the arm that needs it.
     *
     * This tier types a pointer, so a value cannot be both; and reading it as an address on the
     * common path would put a load in front of a branch that leaves. What the split costs is one
     * load on the path that is about to make an indirect call anyway, and the optimizer forwards
     * the second out of the first in every golden here.
     */
    auto word = wordAt(*entry, pair, layout.target.funEnvOffset(), LowerType::Int64,
                       layout.target.pointerSize);

    auto empty = entry->addInst(lower, new (to.arena) LowerInstCmp(
        StringId(), word, immediate(*entry, LowerType::Int64, 0), LowerCmp::eq));

    entry->addInst(lower, new (to.arena) LowerInstJe(
        ((LowerInstSingle*)empty)->created().ptr - lower, done - lower, header - lower));

    /*
     * The cell, read the way every other address cell in this stage is read - four signed bytes,
     * sign-extended - except that it is at a negative offset from a function rather than a positive
     * one from a table, so the address is stated here instead of through `cellAddress`.
     */
    auto code = wordAt(*header, pair, layout.target.funCodeOffset(), LowerType::Pointer,
                       layout.target.pointerSize);

    auto site = header->addInst(lower, new (to.arena) LowerInstBinary(
        StringId(), LowerType::Pointer, code, immediate(*header, LowerType::Int64, U64(kSlotBytes)),
        LowerInst::Sub));

    auto loaded = load(lower, to, *header, lower[((LowerInstSingle*)site)->created().ptr - lower],
                       kSlotBytes, true, LowerType::Int32, StringId());

    auto slot = cast<true, true>(lower, to, *header,
                                 lower[loaded->created().ptr - lower], LowerType::Int64,
                                 StringId())->created().ptr - lower;

    auto zero = header->addInst(lower, new (to.arena) LowerInstCmp(
        StringId(), slot, immediate(*header, LowerType::Int64, 0), LowerCmp::eq));

    header->addInst(lower, new (to.arena) LowerInstJe(
        ((LowerInstSingle*)zero)->created().ptr - lower, done - lower, owned - lower));

    // The teardown's address, the offset added back onto the anchor - `cellPointer`'s last step,
    // spelled out here because the test above needs the offset before it becomes an address.
    auto anchor = imageAnchor();
    auto base = owned->addInst(lower, new (to.arena) LowerInstGlobal(lower[anchor]->name, anchor));

    auto teardown = binary<LowerInst::Add>(
        lower, to, *owned, lower[slot],
        lower[((LowerInstSingle*)base)->created().ptr - lower], LowerType::Pointer,
        StringId())->created().ptr - lower;

    // The environment exactly as the pair holds it, which is the record - see `emitClosureBlock`,
    // where there stopped being anything in front of it.
    auto held = wordAt(*owned, pair, layout.target.funEnvOffset(), LowerType::Pointer,
                       layout.target.pointerSize);

    call(lower, to, *owned, 0, 2, kDefaultCallType, [&](LowerInstCall* built) {
        built->used()[0] = teardown;
        built->used()[1] = held;
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
