#include "lower_internal.h"
#include "../resolve_mir/specialize.h"

namespace ssa {

namespace {

/*
 * The erased ABI - Analysis-IR.md §11.4's G5, in the one file that knows it.
 *
 * A generic body compiled **once** cannot read a layout out of its types, because its types are
 * parameters. What it reads instead is an environment: one four-byte cell per slot of the caller's
 * `mir::GenSchema`, holding the answers a substitution would otherwise have supplied. Every number
 * in that arrangement is here - the cell size, the descriptor's two of them, where the environment
 * sits in the argument list - because both ends of it are here too: the caller that fills a table
 * and the body that loads out of one are two functions in this file rather than two files that have
 * to agree.
 *
 * What is deliberately *not* here is any question about which slot means what. That is the
 * numbering's, `freezeSchema` is where it is decided, and this file indexes it.
 */
constexpr U32 kSlotBytes = 4;

/*
 * A slot that holds an address holds `target - &image$base` as a **signed 32-bit** number.
 *
 * Four bytes rather than a pointer, which is what halves every table here, and anchor-relative
 * rather than absolute or self-relative - `repr/table.h` is where the three encodings are argued and
 * the short version is that both alternatives fail a case this one does not. Absolute is right for
 * an ELF image whose load address is fixed low and silently wrong for the JIT, which maps wherever
 * `mmap` chooses, and it cannot be written until the image is placed. Self-relative needs no anchor
 * and serves every table that is a *constant* - but not one built on the frame, which is what an
 * erased body forwarding its slots to another one has to build, and a frame is tens of terabytes
 * from the image. A callee reads either kind the same way, so one encoding has to serve both.
 *
 * What it costs at the read is `lea` + `add`: the anchor's address is one instruction with no
 * memory traffic, and it is pure and loop-invariant, so a body reading six slots computes it once.
 * What it buys besides the bytes is that `target - &anchor` is known when the image is *assembled* -
 * so these slots never reach `applyDataRelocations`, and identical bytes serve an ELF executable at
 * a fixed address and a JIT buffer anywhere.
 */

/*
 * What a `Type` slot points at - the identity of one type, as bytes. **Two cells, eight bytes.**
 *
 * Three questions, in two cells: how much storage a value needs, what it is aligned to, and what
 * runs when it dies. The first two share a cell because neither needs a whole one - a size is 24
 * bits and an alignment is 8 - and the pair is read together or not at all wherever a *stride* is
 * wanted. Everything a descriptor could also carry - a name, a field table, a copy function - is
 * either asked by no body this milestone compiles or is derivable, and a field nothing reads is a
 * description checked against nothing.
 *
 * **No stride cell**, which is the one place this differs from the old stage's four. A stride is
 * `alignUp(size, align)`, so the cell above answers it in three instructions that are pure,
 * loop-invariant and hoisted out of any loop that indexes with it - and the alternative is a whole
 * cell per descriptor carrying a number derivable from the one beside it. The day a target or a
 * `@repr` chooses a stride that is *not* `alignUp(size, align)`, this becomes a third cell and the
 * derivation becomes a load; until then it would be a number that cannot disagree with its inputs.
 *
 * `teardown` is **never null**, and that is what makes a drop in an erased body one indirect call
 * rather than a load, a test and a branch. A type with nothing to run points at the module's one
 * empty function: one shared body is smaller than a null check at every drop site, and it is also
 * the answer that needs no second rule about what a descriptor slot may hold.
 */
constexpr U32 kDescriptorMetrics = 0;
constexpr U32 kDescriptorTeardown = 4;
constexpr U32 kDescriptorBytes = 8;

/*
 * How the size and the alignment share one cell: the size low, the alignment in the top byte.
 *
 * The alignment is stored **as itself** rather than as a log, so reading it is one shift and no
 * table - `NativeTypeDesc::kFlags` makes the same choice for the same reason. Eight bits is every
 * alignment any target asks for, the widest today being a 64-byte vector.
 *
 * A size of 16MB is the cap that buys it, and it is checked rather than assumed - see
 * `descriptorFor`. It bounds one *value* of a type used as an erased type argument, which is a very
 * different number from the largest object a program may allocate: a `[U8 *n]` past it is refused
 * with a message rather than truncated into a wrong layout.
 */
/*
 * How the size and the alignment share one cell: the size low, the alignment in the top byte.
 *
 * The alignment is stored **as itself** rather than as a log, so reading it is one shift and no
 * table - `NativeTypeDesc::kFlags` makes the same choice for the same reason. Eight bits is every
 * alignment any target asks for, the widest today being a 64-byte vector.
 *
 * A size of 16MB is the cap that buys it, and it is checked rather than assumed - see
 * `descriptorFor`. It bounds one *value* of a type used as an erased type argument, which is a very
 * different number from the largest object a program may allocate: a `[U8 *n]` past it is refused
 * with a message rather than truncated into a wrong layout.
 */
constexpr U32 kAlignShift = 24;
constexpr U32 kMaxDescriptorSize = (1u << kAlignShift) - 1;
constexpr U32 kMaxDescriptorAlign = 0xff;

/*
 * What a `Property` slot holds - **one cell**, in one of three shapes.
 *
 * Which shape is decided by the *declared result type of the requirement*, and by nothing about the
 * subject: `a.size: I32` is a promise the signature made, so both the caller filling the slot and the
 * body reading it know the field's width and its signedness before either has seen the other. That is
 * what makes this an ABI rather than a per-site choice, and it is the same argument `isNarrowRepr`
 * makes in `repr/repr.h` about which shape a `&T` takes.
 *
 *  - **`Offset`** - the field's byte position, and the body emits an `add` and a load or store of the
 *    field's own width. Every field whose type fills its own storage. This is the shape a property
 *    slot should be in as often as possible: it puts nothing between a body compiled once and the two
 *    instructions a hand-written access compiles to.
 *
 *  - **`Narrow`** - the position with the field's **bit shift** above it, for a scalar that does not
 *    fill its storage and may therefore have been co-packed into a neighbour's. `Bool` is one - one
 *    bit in a byte - and so is every `@bits(n)` the standard library spells. The body loads the unit,
 *    shifts and masks on the way out, and reads-modifies-writes on the way in; the *width* of all of
 *    that is the declared type's, so only the two positions travel. Both halves in one cell for the
 *    reason `kDescriptorMetrics` puts two there: neither needs a whole one.
 *
 *  - **`Witness`** - a pointer to a `{read, set}` pair, for a field whose type the body **does not
 *    know**: `fn (a.field: b) f(x: a)` promises that `a` has a `field` of some `b` the caller
 *    decides, so there is no width to load with and no mask to apply. A number cannot answer that,
 *    and neither can any number: what the caller has to hand over is the access itself.
 *
 * The two that are not a plain offset are read the same way in the body - materialize into a
 * temporary, use it, commit it back - which is Design.md's tier 1 and the shape a generic field
 * access has to take whenever the access is not one instruction.
 */
constexpr U32 kPropertyShiftShift = 24;
constexpr U32 kMaxPropertyOffset = (1u << kPropertyShiftShift) - 1;

// The `{read, set}` pair a `Witness` slot points at - two cells, in the order the old stage's
// `PropertyWitnessFields` numbers them.
constexpr U32 kPropertyRead = 0;
constexpr U32 kPropertySet = kSlotBytes;
constexpr U32 kPropertyBytes = 2 * kSlotBytes;

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

/*
 * A number in a compiler-built table, `bytes` wide, in the **target's** byte order.
 *
 * The one place this stage turns a number into bytes, which is what makes the order a property of
 * the compilation rather than of whichever site did the writing. It used to be a four-byte
 * little-endian loop, and the reader opposite it used to compensate by loading the low bytes of a
 * wider group - two assumptions that were correct together and neither of which said so.
 *
 * `bytes` is a whole number of cells. That it can exceed one is what a const parameter counting in
 * `I64` needs, and the *reader* now loads the group at its own width rather than a prefix of it, so
 * nothing below depends on which end the low byte is at - see `numberSlot`.
 */
void Lowering::writeNumber(LowerGlobal& table, U32 offset, U32 bytes, U64 value) {
    for(U32 i = 0; i < bytes; i++) {
        auto at = target.byteOrder == LittleEndian ? offset + i : offset + bytes - 1 - i;
        table.initialContents.ptr[at] = Byte((value >> (i * 8)) & 0xff);
    }
}

// One four-byte cell, which is every slot that is not a count. See `writeNumber`, which is this
// with the width spelled out.
void Lowering::writeCell(LowerGlobal& table, U32 offset, U32 value) {
    writeNumber(table, offset, kSlotBytes, value);
}

/*
 * How many bytes one slot of a numbering occupies.
 *
 * Every slot is one cell except a **count**, which is as many as the type it counts in needs -
 * `data Buf(n: I64)` is a legal declaration and a four-byte cell cannot hold what it counts. Rounded
 * up to a whole number of cells so that the slot after it still begins on one, which keeps every
 * address cell in the table four-byte aligned without a rule about which kinds may follow which.
 *
 * The one place a slot's width is decided. `slotOffset` sums it and `environmentFor` writes at what
 * that sums to, so a caller's idea of where slot three begins and a callee's cannot differ.
 */
static U32 slotBytes(DeclBase base, const mir::GenSlot& slot) {
    if(slot.kind != mir::GenSlotKind::Const) return kSlotBytes;

    auto counted = ((mir::VariableType*)base[slot.type])->constType;
    auto bits = counted ? ((mir::IntType*)base[counted])->bits : 32;
    auto bytes = (bits + 7) / 8;

    return (bytes + kSlotBytes - 1) / kSlotBytes * kSlotBytes;
}

// Where one slot of a numbering begins, in bytes. A walk rather than a stored table for
// `layoutRecord`'s reason: the offsets *are* the widths in front of them, and a second answer is one
// that can disagree with the first.
static U32 slotOffset(DeclBase base, mir::GenSchema& numbering, U32 index) {
    U32 offset = 0;
    for(U32 i = 0; i < index; i++) offset += slotBytes(base, numbering.slots.get(base, i));
    return offset;
}

// What one filled environment measures. The same walk one step further, so that the table a caller
// allocates is exactly the table a callee indexes.
static U32 schemaBytes(DeclBase base, mir::GenSchema& numbering) {
    return slotOffset(base, numbering, U32(numbering.slots.size()));
}

/*
 * The symbol every address cell is measured from - one per program, and zero bytes wide.
 *
 * A name for a position rather than storage: nothing is ever stored at it and nothing loads from
 * it, so what a reader wants is its *address*, which is one `lea` off the instruction pointer.
 *
 * Built on demand, and the demand is deliberately from **both** ends - `addPointer` writes a slot
 * and `cellPointer` reads one, and both come through here. The old stage records getting that wrong
 * once, by keying creation off the table *builder*: a reader in a program that happened to build no
 * table then measured from nothing. Here the two ends cannot disagree, because there is one
 * accessor and it creates.
 */
LowerPtr<LowerGlobal> Lowering::imageAnchor() {
    if(to.imageAnchor) return to.imageAnchor;

    auto global = new (to.arena) LowerGlobal(erasedName(String("image$base")));
    global->mut = false;

    auto handle = global - lower;
    to.globals.add(global->name, handle);
    to.globalOrder.push(handle);
    to.imageAnchor = handle;
    return handle;
}

/*
 * One address inside a compiler-built table.
 *
 * **Anchor-relative**, so the cell is four bytes rather than a pointer and is final when the image
 * is assembled - see `kSlotBytes`, where the encoding is argued. `LowerDataRelocation` carries both
 * shapes; the absolute one is for a pointer a *source* constant holds, which is the program's own
 * pointer type and cannot be written until the image is mapped.
 */
void Lowering::addPointer(LowerGlobal& table, U32 offset, LowerPtr<LowerFunction> function,
                          LowerPtr<LowerGlobal> global) {
    imageAnchor();

    LowerDataRelocation relocation;
    relocation.offset = offset;
    relocation.anchorRelative = true;
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
 * What runs when a value of `type` **that was allocated this way** dies, as a function taking its
 * address.
 *
 * `emitTeardown` is the walk and this is somewhere to put it: the derived recursion over the members
 * and the authored instance calls are exactly what a concrete `drop` lowers to, so a descriptor's
 * teardown is that expansion with a `ret` after it. Nothing about it is specific to erasure, which
 * is the point - an erased body and a specialized one release a `Guard` by running the same
 * instructions, and one of them reaches them through a pointer.
 *
 * **The placement is a parameter because the two halves are not both the storage's.** §6.7 splits a
 * teardown into the effect at last use and the release of what the value occupied, and which of
 * those an erased body may run is decided by how the value got its storage rather than by its type:
 *
 *  - `Frame`  - both. The address is a slot somebody else owns, so what runs is the value's own
 *    obligations and nothing about the memory under it.
 *  - `Region` - the **drop half alone**. A region discharges the reclaim half of everything inside
 *    it in bulk, so running it again per value releases every member twice. This is the case the old
 *    stage argued through at `NativeTypeDesc::kTeardown` and concluded "does not want a second slot;
 *    it wants a second descriptor" - which is what keying on the pair is.
 *  - `Heap`   - both, and then the storage itself, because a boxed value is one whose address the
 *    body is the last holder of.
 *
 * A body compiled once can therefore be handed the same type twice under two descriptors and do the
 * right thing at each, without a flag to test or a branch to take: the decision was made where it is
 * known, which is where the value was allocated.
 */
LowerPtr<LowerFunction> Lowering::teardownFor(DeclPtr<mir::Type> type, mir::StorageClass storage,
                                              LocationId source) {
    auto ownership = mir::ownershipOf(decl, type);

    // Which halves this placement leaves for the reader to run - see above. A region's reclaim is
    // already spent, and a heap value's storage is a third thing that is not either half.
    auto runDrop = ownership.drop != mir::TeardownKind::None;
    auto runReclaim = storage != mir::StorageClass::Region &&
                      ownership.reclaim != mir::TeardownKind::None;

    return teardownHalves(type, runDrop, runReclaim, storage == mir::StorageClass::Heap, source);
}

/*
 * The same function asked for by **which halves it runs** rather than by a placement.
 *
 * Two callers and one key. A descriptor asks by placement, because that is what it knows and the
 * mapping from one to the other is the note above. `emitTeardown` asks by halves, because it is in
 * the middle of a walk that has already worked out which of them a member is owed - an authored
 * `Drop` on the enclosing record replaces the derived drop and leaves the reclaim, and a member's
 * function has to be the one that runs exactly what is left. Encoding that as a placement would have
 * been a second meaning for `Region`, which is a real answer about real storage.
 */
LowerPtr<LowerFunction> Lowering::teardownHalves(DeclPtr<mir::Type> type, bool runDrop,
                                                 bool runReclaim, bool release, LocationId source) {
    // Nothing to run *and* nothing to hand back is the empty function, which is what makes a drop in
    // an erased body one indirect call rather than a load, a test and a branch.
    if(!runDrop && !runReclaim && !release) return emptyTeardown();

    auto key = mir::InternKey().mix(type.offset).mix(U64(runDrop))
                               .mix(U64(runReclaim)).mix(U64(release)).hash;
    if(auto found = teardowns.getValue(key)) return found.unwrap();

    /*
     * Named by what it **leaves out**, so that a dump of a program with two of them for one type says
     * which is which rather than showing the same line twice.
     *
     * The omission and not the inclusion, because a type that has no reclaim half at all is not
     * having one suppressed: `teardown(Guard)` runs everything `Guard` owes, and only a name that
     * says otherwise would be worth reading.
     */
    auto ownership = mir::ownershipOf(decl, type);
    auto text = String("teardown(") + mir::typeText(context, decl, type);
    if(!runDrop && ownership.drop != mir::TeardownKind::None) text = text + String(" no drop");
    if(!runReclaim && ownership.reclaim != mir::TeardownKind::None) {
        text = text + String(" no reclaim");
    }

    if(release) text = text + String(" boxed");

    auto function = to.addFunction(erasedName(text + String(")")));

    auto argument = function->addArg(lower, StringId(), LowerType::Pointer);
    auto handle = function - lower;

    // Recorded before the walk, because a type whose teardown reaches its own type - a record
    // holding a pointer to one - would otherwise build a second function for it and recurse.
    teardowns.add(key, handle);

    auto block = function->addBlock(lower, StringId());

    /*
     * A **sum** is the one type whose derived walk branches, so this is where its branch goes: a
     * function with blocks of its own. `emitTeardown` reaches this function by calling it rather
     * than by expanding it, which is what keeps that walk free of control flow - see the note there.
     */
    if(mir::isSumRecord(decl, type) &&
       mir::ownershipOf(decl, type).drop == mir::TeardownKind::Derived) {
        block = emitSumTeardown(*function, *block, &argument->result - lower, type, runDrop,
                                runReclaim, source);
    } else {
        emitTeardown(*block, &argument->result - lower, type, runDrop, runReclaim, source);
    }

    /*
     * The storage, after the obligations - the one half this stage cannot emit.
     *
     * A boxed value's address names memory an allocator handed out, and there is no allocator here:
     * `StorageClass::Heap` is a rung nothing selects and `verifyBody` refuses an object on it. So
     * reaching this is a placement having become selectable without the release that pairs with it,
     * which is a leak rather than a wrong answer and is exactly the kind of gap a silent `return`
     * would hide until a program grew.
     */
    if(release) {
        context.diagnostics.error(
            "internal: a value of %@ is handed to generic code on the heap, and this stage has no allocator to hand its storage back to"_v,
            source, mir::typeText(context, decl, type));
    }

    emitReturn(*block, nullptr, 0);
    return handle;
}

/*
 * The descriptor for one concrete type, as values of it were **allocated**.
 *
 * Interned on that pair, which is what makes it an identity rather than a copy: two environments
 * that both describe a frame-placed `I32` name one table, so a body comparing two descriptors would
 * be comparing the types - which is what `Any` will need and is free here.
 *
 * Two descriptors for one type is not a redundancy, and `teardownFor` is where the reason is
 * written: the size and the alignment are the type's, and what runs when a value dies is the type's
 * *and* the storage's. Only the second cell differs between them, which is the cheapest way this
 * encoding could carry the distinction - the alternative is a flag in the first cell that every drop
 * site tests, which is the branch a descriptor exists to have already taken.
 */
LowerPtr<LowerGlobal> Lowering::descriptorFor(DeclPtr<mir::Type> type, mir::StorageClass storage,
                                              LocationId source) {
    auto key = mir::InternKey().mix(type.offset).mix(U64(storage)).hash;
    if(auto found = descriptors.getValue(key)) return found.unwrap();

    auto manifest = manifestNative(decl, mir::owned(type));

    auto text = String("descriptor(") + mir::typeText(context, decl, type);
    if(storage != mir::StorageClass::Frame) {
        auto placed = mir::storageClassName(storage);
        text = text + String(" in ") + String(placed.ptr, placed.length);
    }

    auto table = addTable(erasedName(text + String(")")), kDescriptorBytes);

    auto handle = table - lower;
    descriptors.add(key, handle);

    /*
     * The two measurements in one cell - see `kAlignShift`.
     *
     * Checked rather than masked. A size or an alignment past what the cell holds is a *layout* this
     * encoding cannot describe, and truncating either produces a program that allocates the wrong
     * number of bytes with nothing to show for it; the old stage refuses a count it cannot put in a
     * cell the same way and with the same shape of message.
     */
    auto align = manifest.align ? manifest.align : 1;

    if(manifest.size > kMaxDescriptorSize || align > kMaxDescriptorAlign) {
        context.diagnostics.error(
            "%@ cannot be passed to generic code - one value of it is %@ bytes aligned to %@, and a type descriptor describes up to %@ aligned to %@"_v,
            source, mir::typeText(context, decl, type), U64(manifest.size), U64(align),
            U64(kMaxDescriptorSize), U64(kMaxDescriptorAlign));
    }

    writeCell(*table, kDescriptorMetrics, manifest.size | (align << kAlignShift));
    addPointer(*table, kDescriptorTeardown, teardownFor(type, storage, source), nullptr);

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

        auto loaded = block->addInst(lower, new (to.arena) LowerInstLoad(
            incoming[i], StringId(), manifest.type, manifest.size, manifest.isSigned));

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
 * Static because every slot of it is - which is also why nothing here writes an address cell at *run
 * time*. The day an erased body forwards its own slots into a nested call, the environment for that
 * call is built on the **frame**, and filling an address cell there is the inverse of `cellPointer`:
 * `address - &image$base`, narrowed to 32 bits. That the anchor makes a frame-built table and a
 * constant one measure from the same place is the whole reason the encoding is anchor-relative
 * rather than self-relative - see `kSlotBytes`.
 *
 * Static because every slot of it is: this milestone builds an environment only where the argument
 * list at the call is concrete, so each answer is a type known at compile time, a number, or the
 * address of another table. A call inside an erased body that would have to *forward* slots out of
 * its own environment is refused in `callTarget` rather than reaching here with nothing to write.
 */
/*
 * The `{read, set}` pair for one field of one concrete subject - `PropertyForm::Witness`.
 *
 * Interned on the pair, because that is what the pair *is*: two requirements naming one field of one
 * type are one witness, exactly as two environments naming one instance are one class witness.
 *
 * Built in the **lower IR** and not in MIR, on `thunkFor`'s terms and for its reason: an accessor has
 * no ownership content of its own to decide. It moves bytes between storage its caller named and
 * storage its subject holds, and who owns what was settled by the signature that declared the
 * requirement.
 *
 * **Both halves take the `simple` convention**, and that is the whole reason a call here is
 * affordable. `simple` passes in rdi/rsi/rdx and clobbers those plus rax and xmm0-2, leaving every
 * other allocatable register callee-saved - see `codegen/x64/constraint.cpp`, where the tables are.
 * A `complex` callee takes eight argument registers and clobbers nine plus all sixteen vector ones,
 * so a call through one forces the *caller* to spill everything it was holding; a body that reads
 * three fields of an erased subject would spill three times over what is a load and a store. Two
 * pointers and no result is exactly what `simple` was cut for, and an accessor's body - a `copy`, or
 * a shift and a mask - uses nothing beyond the argument registers it was given.
 */
LowerPtr<LowerGlobal> Lowering::propertyWitnessFor(DeclPtr<mir::Type> subject, U16 field,
                                                   LocationId source) {
    auto key = mir::InternKey().mix(subject.offset).mix(U64(field)).hash;
    if(auto found = properties.getValue(key)) return found.unwrap();

    auto& record = *(mir::RecordType*)decl[subject];
    auto declared = record.fields.get(decl, field);
    auto held = manifestNative(decl, mir::owned(declared.occurrence.type));

    auto name = String(mir::typeText(context, decl, subject)) + String(".") +
                String(context.findName(declared.name));

    auto table = addTable(erasedName(String("property(") + name + String(")")), kPropertyBytes);
    auto handle = table - lower;
    properties.add(key, handle);

    /*
     * A field the reader would have to duplicate rather than move.
     *
     * `read` hands a *second* value back to a caller that keeps neither the subject nor a borrow of
     * it, so a field whose type owns something would be duplicated by a byte copy - the one thing a
     * copy of a non-trivial value may not be. What answers it is the scoped form the old stage
     * describes at `PropertyWitnessFields` - a borrow of the field handed to a callback - and that
     * needs the CPS lowering neither stage has. Refused by name rather than mis-copied.
     */
    if(!mir::isTriviallyCopyable(decl, declared.occurrence.type)) {
        context.diagnostics.error(
            "the ownership resolver does not implement reading %@ through a field requirement of an unknown type yet - %@ is not trivially copyable, so a generic body would need a borrow of it rather than a copy"_v,
            source, String(name), mir::typeText(context, decl, declared.occurrence.type));
        return handle;
    }

    auto offset = fieldOffsetNative(decl, subject, field);

    /*
     * `read(owner, out)` and `set(owner, value)`, both by address.
     *
     * A block copy either way, which is what an inline field of a concrete subject is. The *shape* is
     * what a witness buys rather than these two bodies: this is the slot form a field whose type the
     * body cannot see falls into, so what varies behind it is the subject's layout and not the
     * caller's knowledge - and a packed subject's pair is this function with a shift and a mask in it.
     */
    auto reader = to.addFunction(erasedName(String("read(") + name + String(")")));
    reader->callType = LowerCallType::Simple;
    {
        auto owner = &reader->addArg(lower, StringId(), LowerType::Pointer)->result - lower;
        auto out = &reader->addArg(lower, StringId(), LowerType::Pointer)->result - lower;

        auto block = reader->addBlock(lower, StringId());
        auto size = block->addInst(lower, new (to.arena) LowerImm(
            StringId(), LowerType::Int64, U64(held.size)));

        block->addInst(lower, new (to.arena) LowerInstCopy(
            out, cellAddress(*block, owner, offset),
            ((LowerInstSingle*)size)->created().ptr - lower));

        emitReturn(*block, nullptr, 0);
    }

    /*
     * The field's current contents die first and the new ones are installed after, which is the order
     * every assignment in the language runs in - a release that happened second would run an effect
     * over bytes that had already been overwritten.
     */
    auto writer = to.addFunction(erasedName(String("set(") + name + String(")")));
    writer->callType = LowerCallType::Simple;
    {
        auto owner = &writer->addArg(lower, StringId(), LowerType::Pointer)->result - lower;
        auto value = &writer->addArg(lower, StringId(), LowerType::Pointer)->result - lower;

        auto block = writer->addBlock(lower, StringId());
        auto place = cellAddress(*block, owner, offset);

        if(mir::tearsDownField(decl, declared)) {
            emitTeardown(*block, place, declared.occurrence.type, true, true, source);
        }

        auto size = block->addInst(lower, new (to.arena) LowerImm(
            StringId(), LowerType::Int64, U64(held.size)));

        block->addInst(lower, new (to.arena) LowerInstCopy(
            place, value, ((LowerInstSingle*)size)->created().ptr - lower));

        emitReturn(*block, nullptr, 0);
    }

    addPointer(*table, kPropertyRead, reader - lower, nullptr);
    addPointer(*table, kPropertySet, writer - lower, nullptr);
    return handle;
}

/*
 * One property slot, filled - the writer's half of `PropertyForm`.
 *
 * The **bit shift is zero at every site today** and is written anyway, because what has to be right
 * is the encoding rather than the number: nothing here co-packs a field, so a `Bool` gets a byte of
 * its own, and the day `packBits` places two of them in one the only thing that changes is what this
 * function computes. A reader that had assumed the whole cell was an offset would be the thing that
 * had to change instead, in a way no fixture would have caught.
 */
void Lowering::writePropertySlot(LowerGlobal& table, U32 offset, DeclPtr<mir::Type> subject,
                                 U16 field, PropertyForm form, LocationId source) {
    if(form == PropertyForm::Witness) {
        addPointer(table, offset, nullptr, propertyWitnessFor(subject, field, source));
        return;
    }

    auto position = fieldOffsetNative(decl, subject, field);
    if(position > kMaxPropertyOffset) {
        context.diagnostics.error(
            "%@ cannot be passed to generic code - its %@ field is %@ bytes in, and a property slot reaches %@"_v,
            source, mir::typeText(context, decl, subject),
            context.findName(((mir::RecordType*)decl[subject])->fields.get(decl, field).name),
            U64(position), U64(kMaxPropertyOffset));
    }

    // The shift, which `packBits` will one day answer with something other than zero. A narrow field
    // that has not been packed starts at bit zero of the unit it was given.
    U32 shift = 0;
    writeCell(table, offset, position | (shift << kPropertyShiftShift));
}

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

    auto numbering = decl[decl[generic.gen]->schema];
    auto table = addTable(erasedName(text + String("))")), schemaBytes(decl, *numbering));

    auto handle = table - lower;
    environments.add(environment.offset, handle);

    for(U32 index = 0; index < filled.slots.size(); index++) {
        auto witness = filled.slots.get(decl, index);
        auto offset = slotOffset(decl, *numbering, index);

        switch(witness.kind) {
            case mir::GenSlotKind::Type:
                addPointer(*table, offset, nullptr,
                           descriptorFor(witness.type, witness.storage, source));
                break;
            /*
             * The number itself, in as many cells as the type it counts in needs.
             *
             * There is no width limit here and there should not be one: a count is bounded by the
             * parameter's own declared type, which `resolveTypeArgument` already checked the literal
             * against, so a slot sized to that type can hold every value that reached it. The rule
             * this replaces refused anything past 32 bits - which made `data Buf(n: I64)` a
             * declaration the language accepted and the erased form could not carry.
             */
            case mir::GenSlotKind::Const:
                writeNumber(*table, offset, slotBytes(decl, numbering->slots.get(decl, index)),
                            witness.constant);
                break;
            case mir::GenSlotKind::Class:
                addPointer(*table, offset, nullptr,
                           witnessFor(numbering->slots.get(decl, index).klass, witness, source));
                break;

            /*
             * Where the field is, or how to reach it - `PropertyForm`, decided by what the
             * requirement declared its type to be and therefore by what the *body* already knows.
             *
             * `fieldOffsetNative` is the one walk that turns a field index into a position, and what
             * the erased form changes for the two number shapes is only that it travels as data
             * rather than as an immediate.
             */
            case mir::GenSlotKind::Property: {
                auto required = decl[generic.gen]->properties.get(
                    decl, index - numbering->propertyStart);

                writePropertySlot(*table, offset, witness.type, witness.field,
                                  propertyFormOf(decl, required.result), source);
                break;
            }

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
 * Where one cell of a table begins - the one arithmetic every reader of one shares.
 *
 * Deliberately not a function per table. What differs between a descriptor's metrics, a witness
 * table's member and an environment's slot is which constant is passed, and three copies of an
 * `add` is how one of them comes to be written at the wrong width.
 */
LowerPtr<LowerValue> Lowering::cellAddress(LowerBlock& block, LowerPtr<LowerValue> table,
                                           U32 offset) {
    if(!offset) return table;

    auto constant = block.addInst(lower, new (to.arena) LowerImm(
        StringId(), LowerType::Int64, U64(offset)));

    return ((LowerInstSingle*)block.addInst(lower, new (to.arena) LowerInstBinary(
        StringId(), LowerType::Pointer, table,
        ((LowerInstSingle*)constant)->created().ptr - lower, LowerInst::Add)))->created().ptr - lower;
}

/*
 * A **plain word** at an offset, widened to the access.
 *
 * `type` and `width` are the *access*, not the thing read: a const parameter counting in `I32` reads
 * the low four bytes of its cell, and one counting in `I8` reads the low one. That is little-endian,
 * and it is the same assumption `writeCell` writes a cell under.
 *
 * Used for two things that are not the same thing, which is why the width is an argument rather than
 * a constant here: a table cell holding a number, and one of the two pointer-wide words of a
 * function value (`lower_fun.cpp`). The cell that holds an *address* is `cellPointer` and is not
 * this - it is four bytes whatever the target's pointer is.
 */
LowerPtr<LowerValue> Lowering::wordAt(LowerBlock& block, LowerPtr<LowerValue> table, U32 offset,
                                      LowerType type, U32 width) {
    auto loaded = block.addInst(lower, new (to.arena) LowerInstLoad(
        cellAddress(block, table, offset), StringId(), type, width, false));

    return ((LowerInstSingle*)loaded)->created().ptr - lower;
}

/*
 * A cell holding an **address**: four signed bytes, sign-extended, added to the image anchor.
 *
 * Every address a table carries comes back through here - an environment's descriptor or witness, a
 * witness table's member, a descriptor's teardown - because the encoding is not something four call
 * sites should each know.
 *
 * *Signed*, because a slot may name something in front of the anchor as easily as behind it. The
 * anchor's own address is an instruction rather than a load, so it is pure: a body reading several
 * cells computes it once however many times this is called.
 */
LowerPtr<LowerValue> Lowering::cellPointer(LowerBlock& block, LowerPtr<LowerValue> table,
                                           U32 offset) {
    auto address = cellAddress(block, table, offset);

    auto loaded = load(lower, to, block, lower[address], kSlotBytes, true, LowerType::Int32,
                       StringId());

    auto widened = cast<true, true>(lower, to, block, lower[loaded->created().ptr - lower],
                                    LowerType::Int64, StringId());

    auto anchor = imageAnchor();
    auto base = block.addInst(lower, new (to.arena) LowerInstGlobal(lower[anchor]->name, anchor));

    auto sum = binary<LowerInst::Add>(lower, to, block, lower[widened->created().ptr - lower],
                                      lower[((LowerInstSingle*)base)->created().ptr - lower],
                                      LowerType::Pointer, StringId());

    return sum->created().ptr - lower;
}

/*
 * One slot of the environment this body was handed, holding a **number**, which is a count and
 * nothing else - every other kind of slot holds an address.
 *
 * Read at the width the *slot* is, never at the width the access wants, and that one rule is the
 * whole of what makes it right on any target. A load narrower than the slot asks the machine for
 * whichever end of it holds the low byte, which is a byte-order assumption dressed as an offset; a
 * load wider than the slot reads the slot beside it, which is what an `I64` count did to a
 * four-byte cell. Loading exactly the slot and letting the load's own zero extension widen it is
 * neither, and it is why nothing here mentions `target.byteOrder`: the machine's own load applies
 * the order `writeNumber` wrote under.
 */
LowerPtr<LowerValue> Lowering::numberSlot(LowerBlock& block, U32 slot, LowerType type) {
    assertTrue(currentEnvironment); // asked of a body that was not given one

    auto numbering = decl[decl[currentGen]->schema];
    auto bytes = slotBytes(decl, numbering->slots.get(decl, slot));

    // A slot wider than the access would be a truncating load, and no numbering produces one: a
    // count's slot is sized from the very type its value manifests at.
    assertTrue(bytes <= kSlotBytes || type == LowerType::Int64);

    return wordAt(block, currentEnvironment, slotOffset(decl, *numbering, slot), type, bytes);
}

// One slot of that environment holding an address - a type's descriptor, a class witness, a property
// witness, or the function a callable requirement was satisfied with.
LowerPtr<LowerValue> Lowering::slotPointer(LowerBlock& block, U32 slot) {
    assertTrue(currentEnvironment); // asked of a body that was not given one

    auto numbering = decl[decl[currentGen]->schema];
    return cellPointer(block, currentEnvironment, slotOffset(decl, *numbering, slot));
}

/*
 * One field requirement, reached - the three shapes of `PropertyForm`, read.
 *
 * Answers the **address the body should use**, which for the plain case is the field itself and for
 * the other two is a temporary this frame supplies. That difference is the whole of why the two are
 * recorded: a temporary holds a *copy* of the field, so a write into it has to be committed back,
 * and `commitProperty` is what does that.
 *
 * `held` is the field's occurrence as the requirement declared it, and it is what sizes the
 * temporary and what every mask below is measured against. Not the subject's layout: the body knows
 * what the field's type is because the signature wrote it, and knows nothing about where it sits,
 * which is exactly the shape of the feature.
 */
LowerPtr<LowerValue> Lowering::readProperty(LowerBlock& block, LowerPtr<LowerValue> owner, U32 slot,
                                            const mir::Occurrence& held, PropertyForm form) {
    auto cell = numberSlot(block, slot, LowerType::Int64);

    // The plain case: the slot is a byte offset and the field *is* at that address. Nothing is
    // copied, nothing is committed, and what follows is the `add` a concrete projection produces.
    if(form == PropertyForm::Offset) {
        auto sum = block.addInst(lower, new (to.arena) LowerInstBinary(
            StringId(), LowerType::Pointer, owner, cell, LowerInst::Add));

        return ((LowerInstSingle*)sum)->created().ptr - lower;
    }

    auto temporary = frameStorage(block, held);

    /*
     * A field whose type the body does not know, through the witness's reader.
     *
     * `simple`, which both ends state because neither can read the other's: the call is indirect
     * through a cell, so the convention is part of what the slot promises rather than something a
     * call site looks up. See `propertyWitnessFor`, where the choice is argued.
     */
    if(form == PropertyForm::Witness) {
        auto table = slotPointer(block, slot);
        auto reader = cellPointer(block, table, kPropertyRead);

        call(lower, to, block, 0, 3, LowerCallType::Simple, [&](LowerInstCall* built) {
            built->used()[0] = reader;
            built->used()[1] = owner;
            built->used()[2] = temporary;
        });

        return temporary;
    }

    /*
     * A **narrow** field: load the unit it sits in, shift its bits down, and mask off its
     * neighbours'.
     *
     * The unit is the field type's own storage - a `Bool` owns one bit of a byte, so a byte is
     * loaded - and the mask is that type's value bits. Both are constants here, because the body has
     * the declared type; the only thing that comes out of the slot is *where*, which is the byte
     * offset and the shift the two halves of the cell carry.
     */
    auto access = narrowAccess(block, owner, cell, held);

    block.addInst(lower, new (to.arena) LowerInstStore(temporary, access.value, access.unitBytes));
    return temporary;
}

/*
 * The unit a narrow field sits in, and the field's bits taken out of it.
 *
 * Shared by the read and the write because the write needs every step of the read before it can put
 * anything back - the address, the loaded unit, the shift and the mask are the same four values, and
 * computing them twice is how the two halves come to disagree about which bits belong to whom.
 */
Lowering::NarrowAccess Lowering::narrowAccess(LowerBlock& block, LowerPtr<LowerValue> owner,
                                              LowerPtr<LowerValue> cell,
                                              const mir::Occurrence& held) {
    auto manifest = manifestNative(decl, held);
    auto bits = narrowBitsOf(decl, held.type);

    NarrowAccess access;
    access.unitBytes = manifest.size;
    access.type = manifest.type;

    // The two halves of the cell. The offset is the low three bytes and the shift is the top one -
    // `kPropertyShiftShift`, which is the same split `kDescriptorMetrics` makes for the same reason.
    auto offsetMask = block.addInst(lower, new (to.arena) LowerImm(
        StringId(), LowerType::Int64, U64(kMaxPropertyOffset)));

    auto shiftBy = block.addInst(lower, new (to.arena) LowerImm(
        StringId(), LowerType::Int64, U64(kPropertyShiftShift)));

    auto offset = binary<LowerInst::And>(lower, to, block, lower[cell],
                                         lower[((LowerInstSingle*)offsetMask)->created().ptr - lower],
                                         LowerType::Int64, StringId())->created().ptr - lower;

    access.shift = binary<LowerInst::Shr>(lower, to, block, lower[cell],
                                          lower[((LowerInstSingle*)shiftBy)->created().ptr - lower],
                                          LowerType::Int64, StringId())->created().ptr - lower;

    auto sum = block.addInst(lower, new (to.arena) LowerInstBinary(
        StringId(), LowerType::Pointer, owner, offset, LowerInst::Add));

    access.address = ((LowerInstSingle*)sum)->created().ptr - lower;

    auto loaded = block.addInst(lower, new (to.arena) LowerInstLoad(
        access.address, StringId(), access.type, access.unitBytes, false));

    access.unit = ((LowerInstSingle*)loaded)->created().ptr - lower;

    // The field's own bits, as a constant of the *declared* type - the one number here the slot does
    // not carry, because the requirement already said what the field is.
    auto maskValue = bits >= 64 ? ~U64(0) : ((U64(1) << bits) - 1);
    auto mask = block.addInst(lower, new (to.arena) LowerImm(StringId(), access.type, maskValue));
    access.mask = ((LowerInstSingle*)mask)->created().ptr - lower;

    auto shifted = binary<LowerInst::Shr>(lower, to, block, lower[access.unit], lower[access.shift],
                                          access.type, StringId())->created().ptr - lower;

    access.value = binary<LowerInst::And>(lower, to, block, lower[shifted], lower[access.mask],
                                          access.type, StringId())->created().ptr - lower;

    return access;
}

/*
 * The commit half - what a write through a field requirement runs once the temporary holds the new
 * value.
 *
 * A `Witness` slot hands it to `set`, which releases what the field held; a `Narrow` one does the
 * read-modify-write itself, because the bits around the field belong to its neighbours and a plain
 * store would take them with it. An `Offset` slot never reaches here: the body wrote through the
 * field's own address and there is nothing to put back.
 */
void Lowering::commitProperty(LowerBlock& block, const PropertyAccess& access) {
    if(access.form == PropertyForm::Witness) {
        auto table = slotPointer(block, access.slot);
        auto writer = cellPointer(block, table, kPropertySet);

        call(lower, to, block, 0, 3, LowerCallType::Simple, [&](LowerInstCall* built) {
            built->used()[0] = writer;
            built->used()[1] = access.owner;
            built->used()[2] = access.temporary;
        });

        return;
    }

    auto cell = numberSlot(block, access.slot, LowerType::Int64);
    auto narrow = narrowAccess(block, access.owner, cell, access.held);

    auto written = block.addInst(lower, new (to.arena) LowerInstLoad(
        access.temporary, StringId(), narrow.type, narrow.unitBytes, false));

    auto value = ((LowerInstSingle*)written)->created().ptr - lower;

    // The new bits in place, and the unit with the field's own cleared out of it. `and` with the
    // complement rather than a second constant, so that the one mask this reads is the one the read
    // half computed from the same declared type.
    auto masked = binary<LowerInst::And>(lower, to, block, lower[value], lower[narrow.mask],
                                         narrow.type, StringId())->created().ptr - lower;

    auto placed = binary<LowerInst::Shl>(lower, to, block, lower[masked], lower[narrow.shift],
                                         narrow.type, StringId())->created().ptr - lower;

    auto hole = binary<LowerInst::Shl>(lower, to, block, lower[narrow.mask], lower[narrow.shift],
                                       narrow.type, StringId())->created().ptr - lower;

    auto keep = block.addInst(lower, new (to.arena) LowerInstUnary(
        LowerInst::Not, StringId(), narrow.type, hole));

    auto kept = binary<LowerInst::And>(lower, to, block, lower[narrow.unit],
                                       lower[((LowerInstSingle*)keep)->created().ptr - lower],
                                       narrow.type, StringId())->created().ptr - lower;

    auto merged = binary<LowerInst::Or>(lower, to, block, lower[kept], lower[placed],
                                        narrow.type, StringId())->created().ptr - lower;

    block.addInst(lower, new (to.arena) LowerInstStore(narrow.address, merged, narrow.unitBytes));
}

// One member of a witness table, at the position the class declares it in - see `witnessFor`, which
// fills it in that order. The multiplication is here rather than at the call site, so that a member
// index and a byte offset never travel under the same name.
LowerPtr<LowerValue> Lowering::witnessMember(LowerBlock& block, LowerPtr<LowerValue> table,
                                             U16 member) {
    return cellPointer(block, table, member * kSlotBytes);
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

    /*
     * The low 24 bits of the metrics cell - see `kAlignShift`. One load and one `and`, both pure, so
     * a body that sizes the same parameter twice pays for it once.
     */
    auto descriptor = slotPointer(block, variableSlot(occurrence.type));
    auto metrics = wordAt(block, descriptor, kDescriptorMetrics, LowerType::Int64, kSlotBytes);

    auto mask = block.addInst(lower, new (to.arena) LowerImm(
        StringId(), LowerType::Int64, U64(kMaxDescriptorSize)));

    auto size = binary<LowerInst::And>(lower, to, block, lower[metrics],
                                       lower[((LowerInstSingle*)mask)->created().ptr - lower],
                                       LowerType::Int64, StringId());

    return size->created().ptr - lower;
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
    auto descriptor = slotPointer(block, variableSlot(variable));
    auto teardown = cellPointer(block, descriptor, kDescriptorTeardown);

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
