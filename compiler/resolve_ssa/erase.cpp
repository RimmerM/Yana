#include "lower_internal.h"
#include "../resolve_mir/specialize.h"

namespace ssa {

namespace {

/*
 * The erased ABI - Analysis-IR.md §11.4's G5, in the one file that knows it.
 *
 * A generic body compiled **once** cannot read a layout out of its types, because its types are
 * parameters. What it reads instead is an environment: one machine word per slot of the caller's
 * `mir::GenSchema`, holding the answers a substitution would otherwise have supplied. Every number
 * in that arrangement is here - the word size, the descriptor's three fields, where the environment
 * sits in the argument list - because both ends of it are here too: the caller that fills a table
 * and the body that loads out of one are two functions in this file rather than two files that have
 * to agree.
 *
 * What is deliberately *not* here is any question about which slot means what. That is the
 * numbering's, `freezeSchema` is where it is decided, and this file indexes it.
 */
constexpr U32 kSlotBytes = 8;

/*
 * What a `Type` slot points at - the identity of one type, as bytes.
 *
 * Three words and no more, because three is what a body compiled once actually asks: how much
 * storage a value needs, what it is aligned to, and what runs when it dies. Everything a descriptor
 * could also carry - a name, a field table, a copy function - is either asked by no body this
 * milestone compiles or is derivable, and a field nothing reads is a description checked against
 * nothing.
 *
 * `teardown` is **never null**, and that is what makes a drop in an erased body one indirect call
 * rather than a load, a test and a branch. A type with nothing to run points at the module's one
 * empty function: one shared body is smaller than a null check at every drop site, and it is also
 * the answer that needs no second rule about what a descriptor slot may hold.
 */
constexpr U32 kDescriptorSize = 0;
constexpr U32 kDescriptorAlign = 8;
constexpr U32 kDescriptorTeardown = 16;
constexpr U32 kDescriptorBytes = 24;

/*
 * What a dynamically sized allocation is aligned to.
 *
 * A constant, because `LowerInstAlloca` takes its alignment as one: the byte count is a value there
 * and the alignment is a field, which is the shared IR's own shape and the right one - an alignment
 * is a property of what the memory holds rather than of how much of it there is. Over-aligning is
 * always sound, and this is the widest alignment any type in the language asks for, so what it costs
 * is a few bytes of frame and what it buys is not having to give the shared IR a computed alignment
 * for the sake of one producer.
 */
constexpr U32 kDynamicAlign = 16;

/*
 * Whether the declaration wrote a type parameter at this position.
 *
 * The rule the whole adaptation turns on, and it is about the *declaration* rather than about any
 * argument list: what makes a position erased is that the class or the requirement wrote a variable
 * there, so every instance answers it the same way and one witness table holds one signature.
 */
bool erasedPosition(DeclBase decl, const mir::Occurrence& declared) {
    return !mir::isConcreteType(decl, declared.type);
}

} // namespace

StringId Lowering::erasedName(const String& text) {
    /*
     * The bytes are copied into the **declaration** arena before they are interned, because an
     * interned name borrows its text and a formatted `String` is gone when this returns.
     * `Program::uniqueSymbol` states the rule; this is the same rule one region over, and the
     * declaration arena rather than the lower module's because the name table outlives the module.
     */
    auto stored = (char*)program.decl.alloc(text.size());
    copy(text.text(), stored, text.size());
    return context.addUnqualifiedName(stored, text.size());
}

LowerGlobal* Lowering::addTable(StringId name, U32 bytes) {
    auto global = new (to.arena) LowerGlobal(name);
    global->mut = false;
    global->initialContents = ByteBuffer((Byte*)to.arena.alloc(bytes), bytes);
    set(global->initialContents.ptr, bytes, 0);

    auto handle = global - lower;
    to.globals.add(name, handle);
    to.globalOrder.push(handle);
    return global;
}

void Lowering::writeWord(LowerGlobal& table, U32 offset, U64 value) {
    for(U32 i = 0; i < 8; i++) {
        table.initialContents.ptr[offset + i] = Byte((value >> (i * 8)) & 0xff);
    }
}

/*
 * One address inside a compiler-built table, written when the image is placed.
 *
 * Absolute and pointer-wide rather than anchor-relative, which is the other shape
 * `LowerDataRelocation` carries: what is stored here is read as a pointer by emitted code - an
 * indirect call's target, a descriptor a body loads a size out of - and an anchor-relative slot is
 * four bytes the code would have to add the anchor back to at every use.
 */
void Lowering::addPointer(LowerGlobal& table, U32 offset, LowerPtr<LowerFunction> function,
                          LowerPtr<LowerGlobal> global) {
    LowerDataRelocation relocation;
    relocation.offset = offset;
    relocation.anchorRelative = false;
    relocation.function = function;
    relocation.global = global;
    table.relocations.push(to.arena, relocation);
}

/*
 * A `ret` carrying these values.
 *
 * Both places this stage returns from - a body's own `Return` and the entry thunks below - arrive
 * with the values already in an array, which is the one shape `lower_builder.h`'s `ret` does not
 * take, so this is that call with the loop that fills it.
 */
void Lowering::emitReturn(LowerBlock& block, const LowerPtr<LowerValue>* values, Size count) {
    ret(lower, to, block, count, [=](LowerInstRet* inst) {
        for(Size i = 0; i < count; i++) inst->used()[i] = values[i];
    });
}

LowerPtr<LowerFunction> Lowering::emptyTeardown() {
    if(trivialTeardown) return trivialTeardown;

    auto function = to.addFunction(erasedName(String("teardown()")));
    function->addArg(lower, StringId(), LowerType::Pointer);

    auto block = function->addBlock(lower, StringId());
    emitReturn(*block, nullptr, 0);

    trivialTeardown = function - lower;
    return trivialTeardown;
}

/*
 * What runs when a value of `type` dies, as a function taking its address.
 *
 * `emitTeardown` is the walk and this is somewhere to put it: the derived recursion over the members
 * and the authored instance calls are exactly what a concrete `drop` lowers to, so a descriptor's
 * teardown is that expansion with a `ret` after it. Nothing about it is specific to erasure, which
 * is the point - an erased body and a specialized one release a `Guard` by running the same
 * instructions, and one of them reaches them through a pointer.
 */
LowerPtr<LowerFunction> Lowering::teardownFor(DeclPtr<mir::Type> type, LocationId source) {
    if(!mir::ownershipOf(decl, type).needsTeardown()) return emptyTeardown();

    if(auto found = teardowns.getValue(type.offset)) return found.unwrap();

    auto function = to.addFunction(
        erasedName(String("teardown(") + mir::typeText(context, decl, type) + String(")")));

    auto argument = function->addArg(lower, StringId(), LowerType::Pointer);
    auto handle = function - lower;

    // Recorded before the walk, because a type whose teardown reaches its own type - a record
    // holding a pointer to one - would otherwise build a second function for it and recurse.
    teardowns.add(type.offset, handle);

    auto block = function->addBlock(lower, StringId());
    emitTeardown(*block, &argument->result - lower, type, true, true, source);
    emitReturn(*block, nullptr, 0);

    return handle;
}

/*
 * The descriptor for one concrete type.
 *
 * Interned on the type, which is what makes it an identity rather than a copy: two environments that
 * both describe `I32` name one table, so a body comparing two descriptors would be comparing the
 * types - which is what `Any` will need and is free here.
 */
LowerPtr<LowerGlobal> Lowering::descriptorFor(DeclPtr<mir::Type> type, LocationId source) {
    if(auto found = descriptors.getValue(type.offset)) return found.unwrap();

    auto manifest = manifestNative(decl, mir::owned(type));
    auto table = addTable(
        erasedName(String("descriptor(") + mir::typeText(context, decl, type) + String(")")),
        kDescriptorBytes);

    auto handle = table - lower;
    descriptors.add(type.offset, handle);

    writeWord(*table, kDescriptorSize, manifest.size);
    writeWord(*table, kDescriptorAlign, manifest.align ? manifest.align : 1);
    addPointer(*table, kDescriptorTeardown, teardownFor(type, source), nullptr);

    return handle;
}

/*
 * One entry thunk: the erased signature of a member, adapting to the concrete function behind it.
 *
 * The piece G5's note called the one with no precedent here - a function whose body no source wrote.
 * It is built in the **lower IR** and not in MIR, and that is a decision rather than a shortcut: a
 * thunk has no ownership content at all. It moves each argument from the form the table promises
 * into the form the callee declares and hands the result back the other way; there is no value whose
 * life it decides, no place it borrows and no teardown it could place. Built in MIR it would be a
 * body the loan analysis, the drop elaboration and the SSA verifier each had to have an opinion
 * about, over storage named by a raw address - which is `Place::Address`, the construct that exists
 * precisely so that nothing has to produce one.
 *
 * What that costs is that a thunk does not appear in the MIR or SSA dumps, and that is the right end
 * of the trade: what a golden should assert about a thunk is its calling convention, and a calling
 * convention is exactly what those two tiers deliberately do not carry.
 */
LowerPtr<LowerFunction> Lowering::thunkFor(U64 promise, DeclPtr<mir::Function> concrete,
                                           const mir::Occurrence* declared, Size count,
                                           const mir::Occurrence& declaredResult,
                                           LocationId source) {
    /*
     * A thunk is **the pair** - which signature is promised and which function stands behind it -
     * and the key is built here rather than at the two call sites for the reason the pair is the
     * identity: a caller that keyed on the promise alone gave every instance of a class one thunk,
     * so the second instance's witness table pointed at the first instance's member. That is
     * invisible in a dump - the table is a relocation - and invisible in a run wherever the two
     * types happen to share a layout, which two records of one `I32` do.
     */
    auto key = mir::InternKey().mix(promise).mix(concrete.offset).hash;
    if(auto found = thunks.getValue(key)) return found.unwrap();

    auto target = functions.getValue(concrete.offset);
    if(!target) {
        context.diagnostics.error("internal: a witness names a function that was not lowered"_v,
                                  source);
        return nullptr;
    }

    auto& callee = *decl[concrete];
    assertTrue(callee.args.size() == count);

    auto function = to.addFunction(
        erasedName(String("thunk(") + context.findName(callee.symbol) + String(")")));

    auto handle = function - lower;
    thunks.add(key, handle);

    /*
     * The environment, first and unused.
     *
     * A witness for a concrete instance answers every requirement its own head had at selection
     * time, so there is nothing left for a caller to supply. The parameter is there because the
     * *signature* is what a table promises: everything reached through a slot is called the same
     * way, and a shape that varied with whether the callee happened to need one could not be called
     * indirectly at all.
     */
    function->addArg(lower, StringId(), LowerType::Pointer);

    auto erasedResult = manifestNative(decl, declaredResult);
    auto concreteResult = manifestNative(decl, callee.result);

    LowerPtr<LowerValue> resultPlace = nullptr;
    if(erasedResult.inMemory) {
        resultPlace = &function->addArg(lower, StringId(), LowerType::Pointer)->result - lower;
    }

    SmallArray<LowerPtr<LowerValue>, 8> incoming;
    for(Size i = 0; i < count; i++) {
        incoming.push(&function->addArg(lower, StringId(),
                                        manifestNative(decl, declared[i]).type)->result - lower);
    }

    auto returns = !erasedResult.inMemory && decl[declaredResult.type]->kind != mir::Type::Unit;
    if(returns) function->returnTypes.push(to.arena, erasedResult.type);

    auto block = function->addBlock(lower, StringId());

    /*
     * Each argument, from the promised form into the declared one.
     *
     * Exactly one direction ever needs an instruction: a position the declaration erased arrives as
     * an address, and the callee wants a *value* wherever the type it was bound to has a register
     * form. Everything else passes through - an address-only bound type is already what the callee
     * takes, and a position that was never erased was never adapted.
     */
    SmallArray<LowerPtr<LowerValue>, 8> passed;
    for(Size i = 0; i < count; i++) {
        auto& bound = decl[callee.args.get(decl, U32(i))]->occurrence;

        if(!erasedPosition(decl, declared[i]) || arrivesAsAddress(decl, bound)) {
            passed.push(incoming[i]);
            continue;
        }

        auto manifest = manifestNative(decl, bound);
        auto& logical = *decl[bound.type];
        auto isSigned = logical.kind == mir::Type::Int && ((mir::IntType&)logical).isSigned;

        auto loaded = block->addInst(lower, new (to.arena) LowerInstLoad(
            incoming[i], StringId(), manifest.type, manifest.size, isSigned));

        passed.push(((LowerInstSingle*)loaded)->created().ptr - lower);
    }

    auto calleeValue = block->addInst(lower, new (to.arena) LowerInstFun(callee.symbol,
                                                                        target.unwrap()));

    auto calleePlace = returnPlaces.getValue(concrete.offset);
    auto hidden = calleePlace ? 1u : 0u;
    auto created = concreteResult.inMemory || decl[callee.result.type]->kind == mir::Type::Unit
        ? 0u : 1u;

    auto convention = lower[target.unwrap()]->callType;
    auto result = call(lower, to, *block, created, passed.size() + 1 + hidden, convention,
                       [&](LowerInstCall* built) {
        for(Size i = 0; i < created; i++) {
            new (built->created().ptr + i) LowerValue(built, concreteResult.type, StringId());
        }

        built->used()[0] = ((LowerInstSingle*)calleeValue)->created().ptr - lower;

        Size index = 1;

        // The caller's storage handed straight on, where both sides return through one. The thunk
        // has nothing to copy: what the callee writes is already where this function's caller
        // wanted it.
        if(hidden) built->used()[index++] = resultPlace;
        for(Size i = 0; i < passed.size(); i++) built->used()[index++] = passed[i];
    });

    // A register result where the promise was storage - the one place the two ends genuinely differ
    // about the result, and the store is the whole of the adaptation.
    if(created && erasedResult.inMemory) {
        block->addInst(lower, new (to.arena) LowerInstStore(
            resultPlace, result->created().ptr - lower, concreteResult.size));
    }

    auto returned = returns ? result->created().ptr - lower : LowerPtr<LowerValue>(nullptr);
    emitReturn(*block, &returned, returns ? 1u : 0u);

    return handle;
}

/*
 * The witness table for one class requirement - one function pointer per member, in the class's
 * order.
 *
 * A table of its own rather than the members inlined into the environment, because a slot is one
 * word: the environment says *which* instance answers a requirement and the table says what that
 * instance does. That is also the shape a superclass will extend - a witness holding the ones it
 * implies, which `freezeSchema` says is why superclasses are not slots of their own.
 */
LowerPtr<LowerGlobal> Lowering::witnessFor(DeclPtr<mir::Class> klass, mir::GenWitness witness,
                                           LocationId source) {
    auto& declaration = *decl[klass];
    auto count = U32(witness.functions.size());

    /*
     * Interned on the pair the table *is* - the class and the members answering it - for the reason
     * `descriptorFor` is: two environments that name the same instance should name one table, so
     * that a witness pointer is an identity and not a copy. Two generics both requiring
     * `Describe(a)` at `Vessel` is the ordinary case and it is one table.
     */
    auto key = mir::InternKey().mix(klass.offset);
    for(U32 i = 0; i < count; i++) key.mix(witness.functions.get(decl, i).offset);
    if(auto found = witnesses.getValue(key.hash)) return found.unwrap();

    auto text = String("witness(") + context.findName(declaration.name) + String("(");
    for(U32 i = 0; i < witness.functions.size(); i++) {
        if(i) text = text + String(", ");
        text = text + context.findName(decl[witness.functions.get(decl, i)]->symbol);
    }

    auto table = addTable(erasedName(text + String("))")), count * kSlotBytes);
    auto handle = table - lower;
    witnesses.add(key.hash, handle);

    for(U32 member = 0; member < count; member++) {
        auto concrete = witness.functions.get(decl, member);
        auto declared = declaration.members.get(decl, member);

        SmallArray<mir::Occurrence, 4> positions;
        for(auto argument: decl[declared]->args.contents(decl)) {
            positions.push(decl[argument]->occurrence);
        }

        auto promise = (U64(klass.offset) << 32) | U64(member);
        auto thunk = thunkFor(promise, concrete, positions.pointer(), positions.size(),
                              decl[declared]->result, source);
        if(!thunk) continue;

        addPointer(*table, member * kSlotBytes, thunk, nullptr);
    }

    return handle;
}

/*
 * One filled environment, as a table in the image.
 *
 * Static because every slot of it is: this milestone builds an environment only where the argument
 * list at the call is concrete, so each answer is a type known at compile time, a number, or the
 * address of another table. A call inside an erased body that would have to *forward* slots out of
 * its own environment is refused in `callTarget` rather than reaching here with nothing to write.
 */
LowerPtr<LowerGlobal> Lowering::environmentFor(DeclPtr<mir::GenEnvironment> environment,
                                               LocationId source) {
    if(auto found = environments.getValue(environment.offset)) return found.unwrap();

    auto& filled = *decl[environment];
    auto& generic = *decl[filled.generic];

    auto text = String("environment(") + context.findName(generic.symbol) + String("(");
    for(U32 i = 0; i < filled.arguments.size(); i++) {
        if(i) text = text + String(", ");
        text = text + mir::typeText(context, decl, filled.arguments.get(decl, i));
    }

    auto table = addTable(erasedName(text + String("))")),
                          U32(filled.slots.size()) * kSlotBytes);

    auto handle = table - lower;
    environments.add(environment.offset, handle);

    auto numbering = decl[decl[generic.gen]->schema];

    for(U32 index = 0; index < filled.slots.size(); index++) {
        auto witness = filled.slots.get(decl, index);
        auto offset = index * kSlotBytes;

        switch(witness.kind) {
            case mir::GenSlotKind::Type:
                addPointer(*table, offset, nullptr, descriptorFor(witness.type, source));
                break;
            case mir::GenSlotKind::Const:
                writeWord(*table, offset, witness.constant);
                break;
            case mir::GenSlotKind::Class:
                addPointer(*table, offset, nullptr,
                           witnessFor(numbering->slots.get(decl, index).klass, witness, source));
                break;

            // The **offset**, computed here and nowhere else: `fieldOffsetNative` is the one walk
            // that turns a field index into a position, and what the erased form changes is only
            // that the number travels as data rather than as an immediate.
            case mir::GenSlotKind::Property:
                writeWord(*table, offset,
                          fieldOffsetNative(decl, witness.type, witness.field));
                break;

            case mir::GenSlotKind::Function: {
                auto required = decl[generic.gen]->callables.get(
                    decl, index - numbering->functionStart);

                SmallArray<mir::Occurrence, 4> positions;
                for(auto argument: required.args.contents(decl)) {
                    positions.push(mir::Occurrence {
                        argument, mir::borrowedContract(mir::Access::Shared), nullptr });
                }

                auto promise = (U64(generic.gen.offset) << 32) | U64(index);
                auto thunk = thunkFor(promise, witness.functions.get(decl, 0), positions.pointer(),
                                      positions.size(), mir::owned(required.result), source);
                if(thunk) addPointer(*table, offset, thunk, nullptr);
                break;
            }

            case mir::GenSlotKind::Count:
                break;
        }
    }

    return handle;
}

/*
 * Which slot of the body being lowered answers one thing about it.
 *
 * The numbering read, and the one place it is: `freezeSchema` decided the order and this indexes it.
 * A variable is found by identity rather than by counting, because the two fixed-width groups are
 * interleaved in the *parameter list* and separated in the numbering - so counting positions in the
 * declaration would be re-deriving the split the schema exists to have already made.
 */
U32 Lowering::variableSlot(DeclPtr<mir::Type> variable) {
    auto numbering = decl[decl[currentGen]->schema];

    for(U32 index = 0; index < numbering->slots.size(); index++) {
        auto slot = numbering->slots.get(decl, index);
        if(slot.type == variable) return index;
    }

    assertTrue(false); // a body naming a variable its own context does not bind
    return 0;
}

U32 Lowering::requirementSlot(mir::GenSlotKind kind, U16 requirement) {
    auto numbering = decl[decl[currentGen]->schema];

    switch(kind) {
        case mir::GenSlotKind::Class:    return numbering->classStart + requirement;
        case mir::GenSlotKind::Property: return numbering->propertyStart + requirement;
        case mir::GenSlotKind::Function: return numbering->functionStart + requirement;
        default: break;
    }

    assertTrue(false);
    return 0;
}

/*
 * One slot out of the environment this body was handed.
 *
 * Every erased read is this plus an offset, which is what makes the layout above one decision rather
 * than one per reader. `type` and `width` are the *access*, not the slot: a slot is a machine word,
 * and a const parameter that counts in `I32` reads the low four bytes of one - which is
 * little-endian and is the same assumption the constant writer above makes.
 */
LowerPtr<LowerValue> Lowering::slotValue(LowerBlock& block, U32 slot, LowerType type, U32 width) {
    assertTrue(currentEnvironment); // asked of a body that was not given one

    auto address = currentEnvironment;
    if(slot) {
        auto offset = block.addInst(lower, new (to.arena) LowerImm(
            StringId(), LowerType::Int64, U64(slot * kSlotBytes)));

        address = ((LowerInstSingle*)block.addInst(lower, new (to.arena) LowerInstBinary(
            StringId(), LowerType::Pointer, address,
            ((LowerInstSingle*)offset)->created().ptr - lower, LowerInst::Add)))->created().ptr - lower;
    }

    auto loaded = block.addInst(lower, new (to.arena) LowerInstLoad(
        address, StringId(), type, width, false));

    return ((LowerInstSingle*)loaded)->created().ptr - lower;
}

/*
 * One word inside a compiler-built table, at a fixed offset.
 *
 * The one arithmetic every reader of one shares: a descriptor's size and its teardown, and a witness
 * table's member. Deliberately not three functions named after the three tables - what differs
 * between them is which constant is passed, and three copies of an `add` and a `load` is how one of
 * them comes to be written at the wrong width.
 */
LowerPtr<LowerValue> Lowering::wordAt(LowerBlock& block, LowerPtr<LowerValue> table, U32 offset,
                                      LowerType type) {
    auto address = table;
    if(offset) {
        auto constant = block.addInst(lower, new (to.arena) LowerImm(
            StringId(), LowerType::Int64, U64(offset)));

        address = ((LowerInstSingle*)block.addInst(lower, new (to.arena) LowerInstBinary(
            StringId(), LowerType::Pointer, address,
            ((LowerInstSingle*)constant)->created().ptr - lower, LowerInst::Add)))->created().ptr - lower;
    }

    auto loaded = block.addInst(lower, new (to.arena) LowerInstLoad(
        address, StringId(), type, 8, false));

    return ((LowerInstSingle*)loaded)->created().ptr - lower;
}

// One member of a witness table, at the position the class declares it in - see `witnessFor`, which
// fills it in that order. The multiplication is here rather than at the call for the reason every
// other constant in this file is: `kSlotBytes` has one reader.
LowerPtr<LowerValue> Lowering::witnessMember(LowerBlock& block, LowerPtr<LowerValue> table,
                                             U16 member) {
    return wordAt(block, table, member * kSlotBytes, LowerType::Pointer);
}

/*
 * The environment a function reached through a slot is handed - **none**.
 *
 * A witness behind a slot is a concrete instance's, so every requirement its own head had was
 * discharged when the instance was selected and there is nothing left for a caller to supply. The
 * parameter is still in the signature, because what a slot promises is one calling convention and a
 * shape that varied with whether the callee happened to need one could not be called through a
 * pointer at all.
 *
 * As an integer zero rather than a null pointer, which the shared IR has no immediate for -
 * `validateFunction` refuses one, on the grounds that an address is never a literal.
 */
LowerPtr<LowerValue> Lowering::noEnvironment(LowerBlock& block) {
    auto zero = block.addInst(lower, new (to.arena) LowerImm(StringId(), LowerType::Int64, U64(0)));
    return ((LowerInstSingle*)zero)->created().ptr - lower;
}

/*
 * How many bytes one value occupies, as a value.
 *
 * The whole of what `Manifestation::dynamic` costs a consumer, in one function: a literal where the
 * layout is known and a load out of a descriptor where it is not. Every place that used to read
 * `manifest.size` asks this instead, so the two cases cannot come apart - which is the failure the
 * flag exists to make impossible, since a `size` of zero read from a dynamic manifestation allocates
 * one byte and copies none.
 */
LowerPtr<LowerValue> Lowering::sizeValue(LowerBlock& block, const mir::Occurrence& occurrence) {
    auto manifest = manifestNative(decl, occurrence);

    if(!manifest.dynamic) {
        auto size = block.addInst(lower, new (to.arena) LowerImm(
            StringId(), LowerType::Int64, U64(manifest.size)));

        return ((LowerInstSingle*)size)->created().ptr - lower;
    }

    auto descriptor = slotValue(block, variableSlot(occurrence.type));
    return wordAt(block, descriptor, kDescriptorSize, LowerType::Int64);
}

// Storage for a value whose size is not a number - one `alloca` over `sizeValue`. The alignment is
// the constant above and the reason is written there.
LowerPtr<LowerValue> Lowering::dynamicStorage(LowerBlock& block, const mir::Occurrence& occurrence) {
    auto size = sizeValue(block, occurrence);
    auto value = block.addInst(lower, new (to.arena) LowerInstAlloca(StringId(), size,
                                                                    kDynamicAlign));

    return ((LowerInstSingle*)value)->created().ptr - lower;
}

/*
 * A teardown at a type the body does not know - the indirect half of `emitTeardown`.
 *
 * One load of the descriptor, one load of its teardown, one indirect call. No test, because the
 * pointer is never null - see `kDescriptorTeardown`.
 */
void Lowering::emitErasedTeardown(LowerBlock& block, LowerPtr<LowerValue> address,
                                  DeclPtr<mir::Type> variable) {
    auto descriptor = slotValue(block, variableSlot(variable));
    auto teardown = wordAt(block, descriptor, kDescriptorTeardown, LowerType::Pointer);

    /*
     * The default convention, which every function this lowering emits declares - the teardown
     * behind a descriptor slot included, since it is one of them. An indirect call has no callee to
     * read one off, so what makes the two ends agree is that there is one convention on this side of
     * the boundary rather than a lookup that could find a different one.
     */
    call(lower, to, block, 0, 2, kDefaultCallType, [&](LowerInstCall* built) {
        built->used()[0] = teardown;
        built->used()[1] = address;
    });
}

} // namespace ssa
