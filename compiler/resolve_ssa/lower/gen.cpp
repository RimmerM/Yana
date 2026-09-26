#include "internal.h"
#include "../dominance.h"
#include "../../lower/lower_pipeline.h"

namespace ssa {

// The argument is in internal.h, beside the declaration.
U64 constantBits(DeclBase decl, const mir::Constant& constant) {
    switch(constant.kind) {
        case mir::Constant::None:
        case mir::Constant::Unit:
            return 0;
        case mir::Constant::Enum: {
            // The **stored** number for the constructor the constant names, which is where an index
            // becomes a tag: `RecordCon::value` is what `@value(n)` moved and the layer above holds
            // no opinion about it.
            auto& record = *(mir::RecordType*)decl[constant.type];
            return U64(record.cons.get(decl, constant.asConstructor()).value);
        }
        case mir::Constant::Int:
            return constant.bits;
        case mir::Constant::Aggregate:
            // Never reached: an aggregate is laid out field by field by `materialize`, which is what
            // a tree of values means, and the one caller that could hand one to a *register* -
            // `InstConst` in `inst.cpp` - places it before it gets here.
            return 0;
        case mir::Constant::Text:
            // Never reached either, and for a stronger reason: a text has no bytes of its own at
            // all. `Resolver::textValue` is what turns one into a value, above this tier, so the
            // only way one could arrive here is a constant this compiler never built.
            assertTrue(false);
            return 0;
        case mir::Constant::Address:
            // Never reached: an address has no bits until the image is placed, so `materialize`
            // writes a relocation for one and this - which answers a *number* - is not asked.
            assertTrue(false);
            return 0;
        case mir::Constant::Function:
            // A function entry address is also resolved by placement, never as an immediate.
            assertTrue(false);
            return 0;
        case mir::Constant::Float: {
            // The constant holds the `double` pattern whatever the declared width, because the width
            // is the target's answer and this is the first place that has it.
            if(decl[constant.type]->kind == mir::Type::Float &&
               ((mir::FloatType*)decl[constant.type])->bits == 32) {
                auto narrow = F32(constant.asFloat());
                U32 word;
                copy((Byte*)&narrow, (Byte*)&word, 4);
                return word;
            }

            return constant.bits;
        }
    }

    return 0;
}

static void writePackedBytes(Byte* at, U32 bytes, U64 value) {
    for(U32 i = 0; i < bytes; i++) at[i] = Byte((value >> (i * 8)) & 0xff);
}

/*
 * Merge a constant packed field without reading beyond its exact byte range. This mirrors the
 * instruction-level byte helper in `pack.cpp`; keeping the static and dynamic forms together makes
 * the unusual-width case impossible to fix in one path and forget in the other.
 */
static void mergePackedConstant(Byte* at, U32 wordBytes, U32 bitOffset, U32 bitWidth, U64 value) {
    if(!wordBytes) {
        auto span = (bitOffset + bitWidth + 7) / 8;
        auto fieldStart = bitOffset;
        auto fieldEnd = fieldStart + bitWidth;

        for(U32 byte = 0; byte < span; byte++) {
            auto byteStart = byte * 8;
            auto overlapStart = byteStart > fieldStart ? byteStart : fieldStart;
            auto overlapEnd = (byteStart + 8) < fieldEnd ? byteStart + 8 : fieldEnd;
            if(overlapStart >= overlapEnd) continue;

            auto width = overlapEnd - overlapStart;
            auto mask = maskOf(width);
            auto part = (value >> (overlapStart - fieldStart)) & mask;
            auto local = overlapStart - byteStart;
            auto byteMask = mask << local;
            at[byte] = Byte((U8(at[byte]) & ~byteMask) | (part << local));
        }

        return;
    }

    U64 word = 0;
    for(U32 byte = 0; byte < wordBytes; byte++) {
        word |= U64(U8(at[byte])) << (byte * 8);
    }

    auto mask = maskOf(bitWidth);
    word = (word & ~(mask << bitOffset)) | ((value & mask) << bitOffset);
    writePackedBytes(at, wordBytes, word);
}

/*
 * One constant's bytes, written where the manifestation says they go.
 *
 * The **one** place a constant becomes bytes, which is what `Constant::Aggregate` being a tree of
 * values rather than a blob is for: resolve says what the value is, and this walk asks the same
 * layout every other value of that type is laid out with - `reprOf` and `manifestNative`, and
 * nothing of its own. A second answer here would be a second record layout, and the way that fails
 * is silent.
 *
 * The destination arrives **zeroed**, so padding and the payload an arm does not use stay zero
 * without this having to say anything about either.
 */
void Lowering::materialize(const mir::Occurrence& occurrence, const mir::Constant& constant,
                           LowerGlobal* owner, Byte* into) {
    auto manifest = manifestNative(layout, occurrence);

    /*
     * The address of another global - zero bytes now, and a relocation saying who fills them.
     *
     * `anchorRelative` clear, because this is the *program's* own pointer type rather than a slot
     * in a compiler-built table: it is the target's full width and absolute, and stays unknown
     * until the image is mapped. `LowerDataRelocation` is where the pair is argued.
     *
     * A null owner is the scratch buffer a co-packed field is built in, and an address cannot be
     * there: a relocation names a byte offset inside a global's contents, and a pointer that shares
     * a word with its neighbours has no such offset to name. Nothing produces one - a `@bits(n)`
     * refinement is an integer - so this is an internal invariant rather than a diagnostic.
     */
    if(constant.kind == mir::Constant::Address || constant.kind == mir::Constant::Function) {
        assertTrue(owner != nullptr);

        LowerPtr<LowerGlobal> global = nullptr;
        LowerPtr<LowerFunction> function = nullptr;
        auto relocation = U32(owner->relocations.size());
        if(constant.kind == mir::Constant::Address) {
            auto found = globals.getValue(constant.asGlobal().offset);
            assertTrue(found.isJust());
            global = found.unwrap();
        } else {
            auto found = functions.getValue(constant.asFunction().offset);
            if(found) {
                function = found.unwrap();
            } else {
                pendingFunctionRelocations.push(PendingFunctionRelocation {
                    owner - lower, relocation, constant.asFunction().offset
                });
            }
        }

        owner->relocations.push(to.arena, LowerDataRelocation {
            U32(into - owner->initialContents.ptr), false, function, global
        });

        return;
    }

    /*
     * A **text**, at the `[CodeUnit *n]` its bytes are - written as the bytes.
     *
     * The one constant kind whose payload is longer than a word, and the reason it is a kind rather
     * than an aggregate of `n` numbers. Written as one it cost `n` interned `Constant`s per literal
     * and a walk over them here, which is what `test/bench/findings.md` §94 measured; written like
     * this the value *is* the interned name, and this loop is the whole of laying it out.
     *
     * Only at the byte array. A text at a `String` is the record and is built as one by the
     * resolver - `Resolver::stringConstant`, which spells the three fields the compiler has always
     * known - so what arrives here is either those bytes or an ordinary aggregate.
     */
    if(constant.kind == mir::Constant::Text) {
        /*
         * One byte per unit, asserted rather than assumed. A wider code unit would need a byte
         * order and this loop states none - `IntWidths::codeUnit` answers eight on every target
         * this stage compiles for, and the day one answers sixteen it is this arm that has to say
         * which end the low byte is at.
         */
        auto element = mir::owned(program.types.codeUnit);
        assertTrue(strideOf(manifestNative(layout, element)) == 1);

        auto text = context.findName(constant.asText());
        auto count = U32(text.size()) < manifest.size ? U32(text.size()) : manifest.size;

        for(U32 i = 0; i < count; i++) into[i] = Byte(text.text()[i]);
        return;
    }

    if(constant.kind != mir::Constant::Aggregate) {
        auto bits = constantBits(decl, constant);
        for(U32 i = 0; i < manifest.size; i++) into[i] = Byte((bits >> (i * 8)) & 0xff);
        return;
    }

    /*
     * A fixed array's elements, each at its stride - the bytes of a string literal are one.
     *
     * A **vector** is written by the same three lines, which is the whole of what a vector constant
     * is in storage: `0 :: Vec(I32, 4)` reduces to the lane repeated, and a vector's lanes are
     * packed at the lane's own stride with no padding between them and nothing over them. The two
     * are one arm rather than two because they are one statement - a run of values of one type laid
     * end to end - and a second copy is how the two would come to disagree about the stride.
     */
    auto run = decl[constant.type]->kind == mir::Type::Array
        ? ((mir::ArrayType*)decl[constant.type])->element
        : mir::laneTypeOf(decl, constant.type);

    if(run) {
        auto& parts = *decl[constant.aggregate()];
        auto element = mir::owned(run);
        auto stride = strideOf(manifestNative(layout, element));

        for(U32 i = 0; i < parts.values.size(); i++) {
            materialize(element, parts.values.get(decl, i), owner, into + i * stride);
        }

        return;
    }

    auto& record = *(mir::RecordType*)decl[constant.type];
    auto& parts = *decl[constant.aggregate()];
    auto arm = record.cons.get(decl, parts.constructor);
    auto repr = reprOf(layout, constant.type);

    /*
     * The discriminant, in whichever form this record spells it.
     *
     * A stored tag is the **stored** number and not the index the constant carries -
     * `RecordCon::value`, on the same terms `constantBits` reads it for an enum-layout record. A
     * folded one is a pattern for every constructor but the one that owns the payload, which writes
     * nothing: the payload's own bytes, written below, are what says which constructor this is.
     */
    switch(repr.discriminant) {
        case Discriminant::None:
            break;
        case Discriminant::Word:
            writePackedBytes(into + repr.discriminantOffset, repr.discriminantBytes,
                             U64(arm.value));
            break;
        case Discriminant::Bits:
            mergePackedConstant(into + repr.discriminantOffset, repr.discriminantBytes,
                                repr.discriminantBitOffset, repr.discriminantBits,
                                U64(arm.value));
            break;
        case Discriminant::Niche:
            if(parts.constructor != repr.encoding.payloadConstructor) {
                writePackedBytes(into + repr.encoding.niche.offset, repr.encoding.niche.bytes,
                                 repr.encoding.patternOf(parts.constructor));
            }
            break;
    }

    for(U16 i = 0; i < arm.count; i++) {
        auto index = U16(arm.first + i);
        auto field = record.field(decl, index);
        auto held = repr.fields.get(decl, index);
        auto value = parts.values.get(decl, i);

        /*
         * A co-packed field has no bytes of its own to be written into.
         *
         * Materialized into a buffer of its own width first and then merged into the word it shares,
         * which is the same read-modify-write `encodePacked` emits and is written this way rather
         * than as a number for the same reason that one is: the value may itself be an aggregate
         * this target scalarized, and the walk that turns one into bytes is this function.
         */
        if(held.isPacked()) {
            Byte bits[8] = {};
            materialize(field.occurrence, value, nullptr, bits);

            U64 held64 = 0;
            for(U32 b = 0; b < 8; b++) held64 |= U64(U8(bits[b])) << (b * 8);

            mergePackedConstant(into + held.offset, held.wordBytes, held.bitOffset, held.bitWidth,
                                held64);
            continue;
        }

        materialize(field.occurrence, value, owner, into + held.offset);
    }
}

/*
 * Where an aggregate constant reached inside a body lives.
 *
 * A record has no register form, so a constant of one is *storage* and something has to place it -
 * and this is the tier that may decide, because it is the first one that knows the target. Today it
 * is read-only data, interned per constant, and what a consumer gets is its address: a `let` of one
 * copies from it exactly as it copies from any other record, and nothing writes it.
 *
 * It is deliberately the whole of the decision rather than a fixed rule spread over the backends.
 * A constant that fits a register, or a pair of them, is cheaper as immediates than as a load from
 * data, and a JS target would rather have neither; both are refinements of *this* function, which
 * is why resolve hands down a tree of values and no placement at all.
 */
LowerPtr<LowerGlobal> Lowering::placeConstant(const mir::Constant& constant,
                                              const mir::Occurrence& occurrence) {
    auto key = (U64(constant.type.offset) << 32) | U64(U32(constant.bits));
    if(auto found = placedConstants.getValue(key)) return found.unwrap();

    auto manifest = manifestNative(layout, occurrence);
    auto name = erasedName(Tritium::format("constant$%@", U64(placedConstantCount++)));

    auto global = addTable(name, manifest.size);
    materialize(occurrence, constant, global, global->initialContents.ptr);

    auto handle = global - lower;
    placedConstants.add(key, handle);
    return handle;
}

void Lowering::declareGlobal(DeclPtr<mir::Global> pointer) {
    auto source = decl[pointer];
    auto target = new (to.arena) LowerGlobal(source->name);

    // What is *written*, rather than what may be assigned - the same distinction `LowerGlobal::mut`
    // makes: clear becomes LLVM's `constant`, which is a promise rather than a hint. `isWritten` is
    // where the two are told apart, because a `dynamic` global is assigned to by nothing and still
    // written once, by the entry sequence.
    target->mut = source->isWritten();

    auto manifest = manifestNative(layout, source->occurrence);
    target->initialContents = ByteBuffer((Byte*)to.arena.alloc(manifest.size), manifest.size);
    set(target->initialContents.ptr, manifest.size, 0);

    auto handle = target - lower;
    globals.add(pointer.offset, handle);
    to.globals.add(source->name, handle);
    to.globalOrder.push(handle);

    /*
     * The globals the **compiler** fills - `supplied.def`'s `BuiltinKind::Written`, which is the
     * three `commandLine*` cells the process entry stub stores argc, argv and envp into.
     *
     * Carried across rather than looked up by name: what a backend needs is the lowered global, the
     * name it happens to have is the library's, and this is the one place both halves are in hand.
     * A `Supplied` role is not one of these - its value went into the initializer the moment the
     * declaration existed (`bindSuppliedGlobal`), so there is nothing left for a backend to write.
     *
     * Only a reachable global gets here, and that is the intended answer rather than a gap: a
     * program that never asks about its arguments leaves the roles null and the stub emits nothing.
     */
    for(Size role = 0; role < intrinsic::kBuiltinCount; role++) {
        if(intrinsic::builtinDef(intrinsic::Builtin(role)).kind != intrinsic::BuiltinKind::Written) {
            continue;
        }

        if(program.roles.suppliedGlobal(intrinsic::Builtin(role)) == pointer) to.builtins[role] = handle;
    }
}

void Lowering::fillGlobal(DeclPtr<mir::Global> pointer) {
    auto source = decl[pointer];
    if(!source->initial.isSet()) return;

    auto target = lower[globals.getValue(pointer.offset).unwrap()];
    materialize(source->occurrence, source->initial, target, target->initialContents.ptr);
}

LowerValue* Lowering::mapped(SsaPtr<Value> value) {
    auto found = values.getValue(ssa[value]->id.index);
    return found ? lower[found.unwrap()] : nullptr;
}

/*
 * One edge's arguments, into the phis its target's parameters became.
 *
 * The alternative and the block it comes from are written as a pair, and the pair is what
 * `validateLowerModule` checks - so the order alternatives are filled in does not have to match the
 * order the target's `incoming` list was built in. That is worth not depending on: the incoming list
 * is built by `addInst` as terminators arrive, and this runs immediately before the terminator that
 * would extend it.
 */
void Lowering::supplyEdge(LowerBlock& from, Edge& edge) {
    auto target = ssa[edge.target];
    auto count = edge.arguments.size();

    for(U32 i = 0; i < count; i++) {
        auto used = edge.arguments.get(ssa, i);
        auto value = mapped(used.value());
        if(!value) continue;

        auto argument = ssa[target->argumentAt(ssa, i)];
        auto index = phiOf.getValue(argument->id.index);
        if(!index) continue;

        // An argument that crosses as its cells - `joinCells`: each one loaded here, out of the
        // storage this predecessor's value is in, into the phi it owns.
        if(auto split = joinSplits.getValue(argument->id.index)) {
            for(U32 k = 0; k < split.unwrap().count; k++) {
                auto part = loadCarried(from, joinParts[split.unwrap().start + k], value - lower);
                auto& pending = phis[index.unwrap() + k];
                assertTrue(pending.filled < pending.phi->usedCount);

                pending.phi->used()[pending.filled] = part;
                pending.phi->sources()[pending.filled] = &from - lower;
                pending.filled++;
            }

            continue;
        }

        // One alternative per incoming edge, and the phi was sized from the SSA predecessor count.
        // More arriving than that means the two disagree about the CFG, which is a conversion bug
        // rather than a program error - and writing past the allocation is what it would otherwise be.
        auto& pending = phis[index.unwrap()];
        assertTrue(pending.filled < pending.phi->usedCount);

        pending.phi->used()[pending.filled] = value - lower;
        pending.phi->sources()[pending.filled] = &from - lower;
        pending.filled++;
    }
}

/*
 * A declared range the lower IR has no room for, and the two kinds of type that state one.
 *
 * `LowerType` is a register class and nothing else, so `@bits(30) U64`, `U8` and `U64` all arrive
 * below this boundary as `Int64` or `Int32` and the distinction is gone. Reconstructing it later
 * means walking backwards to a load or a mask and cannot work for an argument or a phi at all, so
 * the useful half is preserved instead: an unsigned value of an n-bit type has every bit above n
 * clear, and `LowerValue::hintUnsignedWidth` is where the lower passes read it.
 *
 * **An unsigned integer answers its own width**, refinements included - `Repr::scalarBits` is that
 * number for every integer, which is `@bits(n)`'s whole appearance at this tier.
 *
 * **A payload-free sum answers the width its constructors need.** That one is the fact
 * `compiler/resolve` (the stage deleted in J173) found was worth the most: a `Bool` is
 * `data Bool = False | True`, one bit, and without this it reaches the lower passes as an `Int32`
 * nothing can tell from a counter - which is what `isBooleanValued` in `lower_fold.cpp`
 * reconstructs by walking the instruction graph, and that walk can only answer for a value some
 * instruction *computed* as a truth value.
 *
 * The shape differed between the stages in one place worth naming. `compiler/resolve`'s `EnumRange`
 * carried the logical bit count, and `mir::TagRange::bits` carries the **stored** width - 8, 16, 32
 * or 64 - so asking the tag range here would hint eight bits for a `Bool` and claim nothing. The
 * number this wants is the one the packer uses, `Repr::scalarBits`, which `layoutEnum` computes as
 * `bitsUpTo(highest)` for exactly that reason.
 *
 * A negative constructor value is declined, and so is a signed integer: such a value takes the whole
 * of its register, since its top bit is a sign rather than a bit that is clear.
 */
static Maybe<U32> normalFormBits(Layout layout, DeclPtr<mir::Type> type) {
    if(!type) return Nothing();

    auto& logical = *layout.base[type];

    if(logical.kind == mir::Type::Int) {
        auto& integer = (mir::IntType&)logical;
        if(integer.isSigned) return Nothing();

        auto bits = reprOf(layout, type).scalarBits;
        return bits ? Just(bits) : Nothing();
    }

    if(logical.kind != mir::Type::Record) return Nothing();

    auto& record = (mir::RecordType&)logical;
    if(record.layout != mir::RecordLayout::Enum || !record.cons.size()) return Nothing();
    if(mir::tagRangeOf(layout.base, record).isSigned) return Nothing();

    auto bits = reprOf(layout, type).scalarBits;
    return bits ? Just(bits) : Nothing();
}

/*
 * Every value this body mapped, hinted with what its type declared.
 *
 * One walk over the finished mapping rather than a hint at each of the forty sites that fill it -
 * `compiler/resolve`'s `hintUnsignedRanges` made the same choice for the same reason: the map is
 * what the walk has that the individual sites do not. Several SSA values may share one lower value;
 * `hintUnsignedWidth` keeps the narrowest, which is sound because each mapping is a claim that the
 * shared value is in that type's normal form.
 *
 * A value wider than its register, one in a vector lane and one that is an address are all left
 * alone: the hint is a statement about the high bits of an integer register and there are none to
 * make about any of those.
 */
void Lowering::hintDeclaredRanges(Body& body) {
    for(auto valuePointer: body.values.contents(ssa)) {
        auto value = ssa[valuePointer];

        auto bits = normalFormBits(layout, value->type());
        if(!bits) continue;

        auto found = values.getValue(value->id.index);
        if(!found) continue;

        auto lowered = lower[found.unwrap()];
        if(lowered->type.laneShift || lowered->type.mask) continue;

        auto registerBits = lowered->type.lane == LowerLane::Int32 ? 32u
                          : lowered->type.lane == LowerLane::Int64 ? 64u : 0u;
        if(!registerBits || bits.unwrap() >= registerBits) continue;

        lowered->hintUnsignedWidth(U8(bits.unwrap()));
    }
}

void Lowering::lowerBody(mir::Function& source, LowerFunction& target) {
    auto& body = *ssa[source.ssa];
    currentLowerFunction = &target;

    values.reset();
    blocks.reset();
    destinations.reset();
    phiOf.reset();
    joinSplits.reset();
    joinParts.clear();
    propertyAccesses.reset();
    virtualPlaces.reset();
    decodedNarrow.reset();
    phis.clear();

    /*
     * A lower block for every SSA block, the entry included.
     *
     * The lower IR's own entry block is a *stub*: `LowerFunction`'s constructor makes one, it holds
     * the argument references, nothing may jump to it, and `addBlock` gives it a jump to the first
     * real block. So the SSA entry maps to a new block rather than to that one - mapping it to the
     * stub leaves a function whose entry ends in a return, which is what `validateLowerModule`
     * reports as "entry block must end with unconditional jump".
     */
    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(ssa, BlockId(i));

        // A deleted block keeps its `BlockId` so that every side table keeps meaning what it did -
        // see `Block::deleted` - and nothing may enter it. Giving it a lower block would leave one
        // with no terminator, which is what `validateLowerModule` reports.
        if(block->deleted) continue;

        blocks.add(i, target.addBlock(lower, block->name) - lower);
    }

    /*
     * The entry block's parameters are the function's own, and they are already values.
     *
     * `LowerArg`s, made by the declaration walk in `run` and living in the lower function's stub
     * entry block. Not phis: nothing jumps to the SSA entry, so a phi there would be sized at zero
     * alternatives and answer nothing. This is the one block whose parameters come from outside the
     * function, which is exactly why it is the one block with a case of its own.
     *
     * The hidden result pointer, where there is one, is argument zero - so the declared parameters
     * start one later. That offset is the *only* thing about the convention this walk knows; what
     * decides whether there is one is the manifestation, asked once in `run`.
     */
    auto entryBlock = body.block(ssa, BlockId(0));
    auto place = returnPlaces.getValue((&source - decl).offset);
    auto given = environmentPlaces.getValue((&source - decl).offset);

    currentReturnPlace = place ? place.unwrap() : LowerPtr<LowerValue>(nullptr);

    /*
     * The environment this body reads its own layouts out of, and the numbering it reads it by.
     *
     * Both set together and both null for every ordinary function, which is what makes an erased
     * read fail loudly rather than index a schema that is not there - a slot number means nothing
     * except relative to one context, and the context is this body's own.
     */
    currentEnvironment = given ? given.unwrap() : LowerPtr<LowerValue>(nullptr);
    currentGen = given ? source.gen : DeclPtr<mir::GenContext>(nullptr);

    // The environment, then the hidden result pointer, then the declared parameters - see `run`,
    // where the order is decided and the reason is written.
    auto leading = (given ? 1u : 0u) + (place ? 1u : 0u);

    /*
     * A closure's environment is a declared parameter that was emitted *first*, so it is the one
     * position where the two orders differ - see `run`. Everything after it is the ordinary
     * mapping, because the leading count includes it either way.
     */
    auto closure = source.environment != nullptr;

    auto entryLower = lower[blocks.getValue(0).unwrap()];
    currentEntry = entryLower;

    /*
     * A parameter the shape takes apart arrives as its parts, and the body still reads it through
     * an address - so the entry gives it a slot and writes the parts back. That slot is what
     * `splitAggregateSlots` and `promoteStackSlots` take apart again, into the registers the parts
     * arrived in, wherever the body only reads fields.
     */
    auto shape = callShapeOf(&source - decl);
    auto index = leading + (closure ? 1u : 0u);
    currentShape = shape;

    for(U32 i = 0; i < entryBlock->argumentCount(); i++) {
        auto argument = ssa[entryBlock->argumentAt(ssa, i)];
        auto carried = shape && i < shape->argCount ? shapeArgs[shape->firstArg + i] : ShapeArg {};

        if(carried.count) {
            auto storage = frameStorage(*entryLower, argument->occurrence.contract.isOwned()
                ? argument->occurrence : mir::owned(argument->occurrence.type));

            for(U32 k = 0; k < carried.count; k++) {
                auto part = &lower[target.args.get(lower, index + k)]->result - lower;
                storeCarried(*entryLower, shapeParts[carried.start + k], part, storage);
            }

            values.add(argument->id.index, storage);
            index += carried.count;
            continue;
        }

        auto position = closure && i == 0 ? 0u : index++;
        auto declared = lower[target.args.get(lower, position)];

        /*
         * The environment arrives as the record, wherever it was placed - D17. A heap-placed one
         * used to arrive tagged and biased past a header, which was an `add` in this block and a
         * header word in every block allocated; both went when the teardown moved in front of the
         * code, so a frame environment and a heap environment are now the same parameter.
         */
        values.add(argument->id.index, &declared->result - lower);
    }

    /*
     * A phi per block parameter, detached and sized from the SSA predecessor count.
     *
     * Before any instruction, because the block's own code reads the parameter and every
     * predecessor's terminator writes an alternative into it. The count comes from the SSA block's
     * `incoming`, which `Editor::seal` filled: the lower block's own incoming list is built by the
     * terminators this walk is about to add, so it is empty right now and asking it would size every
     * phi at zero.
     */
    SmallArray<CarriedPart, 16> cells;

    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(ssa, BlockId(i));
        if(i == 0 || !block->argumentCount()) continue;

        auto into = lower[blocks.getValue(i).unwrap()];
        auto incoming = block->predecessorCount();

        for(auto argumentPointer: block->arguments(ssa)) {
            auto argument = ssa[argumentPointer];
            auto manifest = manifestOf(*argument);

            auto makePhi = [&](LowerType type) {
                auto storage = allocTrailing<LowerInstPhi, LowerPtr<LowerValue>, LowerPtr<LowerBlock>>(
                    to.arena, sizeof(LowerPtr<LowerValue>) * incoming + sizeof(LowerPtr<LowerBlock>) * incoming);

                auto phi = new (storage) LowerInstPhi(StringId(), type);
                assertTrue(incoming <= LowerInst::kMaxUsed);
                phi->usedCount = U16(incoming);

                // Zeroed rather than left as the arena found it: an alternative that is still null
                // is what says this phi is not answerable yet, and reading whatever the arena last
                // held there is a wild pointer. `lower_promote.cpp` records that having happened.
                for(Size u = 0; u < incoming; u++) phi->used()[u] = nullptr;

                phis.push(PendingPhi { phi, into - lower, 0 });
                return &((LowerInstSingle*)phi)->result - lower;
            };

            /*
             * A value held in memory, crossing as its cells - `joinCells`, which is where the
             * argument is. A phi per cell, and one slot the join stores them into: the value this
             * block reads is that slot, exactly as an entry parameter carried in parts is.
             *
             * Stored whole rather than zeroed first: every cell is written, a co-packed word
             * included, and its spare bits are the ones the predecessor's own storage held.
             */
            if(joinCells(*argument, cells)) {
                phiOf.add(argument->id.index, U32(phis.size()));
                joinSplits.add(argument->id.index, JoinSplit { U32(joinParts.size()), U32(cells.size()) });

                auto owned = argument->occurrence.contract.isOwned() ? argument->occurrence
                                                                     : mir::owned(argument->occurrence.type);
                auto slot = frameStorage(*into, owned, true);

                for(auto& part: cells) {
                    joinParts.push(part);
                    storeCarried(*into, part, makePhi(part.type), slot);
                }

                values.add(argument->id.index, slot);
                continue;
            }

            phiOf.add(argument->id.index, U32(phis.size()));
            values.add(argument->id.index, makePhi(manifest.type));
        }
    }

    /*
     * The instructions, over the blocks in **reverse postorder** rather than in table order.
     *
     * A `BlockId` is handed out at creation and `Body::blocks` is indexed by it, so the table order
     * is the order blocks were made and nothing keeps that in step with the graph: a pass that
     * appends a block and points an existing branch at it leaves a block whose values are read by a
     * block earlier in the table. This walk maps each SSA value as it goes, so in table order that
     * is a lookup with nothing in it. `compiler/resolve` restored the invariant with a pass of its
     * own (`reorderBlocksInRpo`, which renumbered the blocks); taking the order here instead
     * removes it from every pass author's burden rather than restating it.
     *
     * Reverse postorder is what makes the lookup answerable: a definition dominates its uses, and
     * dominance implies coming earlier in reverse postorder. A back edge is the exception and is why
     * the phis above are made before any of this and attached after all of it.
     *
     * Then the blocks the entry cannot reach, in table order, rather than skipped. They have a lower
     * block already - the creation loop above made one per SSA block - and a lower block with no
     * terminator is what `validateLowerModule` reports; an edge out of one also feeds a phi that was
     * sized from the SSA predecessor count and owes an alternative per edge. So they are lowered,
     * last, where nothing they define can be wanted: a reachable block cannot read a value defined
     * in one, since its definition would not dominate the use.
     */
    Array<U32> order;
    reversePostorder(ssa, body, order);

    auto lowerBlockAt = [&](U32 i) {
        auto block = body.block(ssa, BlockId(i));
        auto into = lower[blocks.getValue(i).unwrap()];

        for(auto instPointer: block->instructions(ssa)) {
            lowerInst(*into, *ssa[instPointer]);
        }
    };

    IndexSet reached;
    reached.reset(body.blocks.size());

    for(auto i: order) {
        reached.set(i, true);
        lowerBlockAt(i);
    }

    for(U32 i = 0; i < body.blocks.size(); i++) {
        if(!reached[i] && !body.block(ssa, BlockId(i))->deleted) lowerBlockAt(i);
    }

    // Attached last, which is what registers their alternatives as uses - see `PendingPhi`. A phi
    // that did not receive one alternative per edge is a lowering bug rather than a program error,
    // and `validateLowerModule` is where it stops.
    for(auto& pending: phis) lower[pending.block]->addInst(lower, pending.phi);

    // And the ranges the types stated and `LowerType` has nowhere to keep, over the mapping this
    // walk has just finished filling - see `hintDeclaredRanges`, which is where the argument is.
    hintDeclaredRanges(body);
}

void Lowering::run() {
    /*
     * Globals first: a function's first instruction may take the address of one, and the module
     * resolves that by name.
     *
     * **Declared before any is filled**, which is what lets an initializer name another global: the
     * bytes of a string literal are a global of their own, minted while the initializer that points
     * at them was being resolved, so it comes *after* its referrer in declaration order. One pass
     * would have had to materialize a relocation naming a global that did not exist yet.
     */
    for(auto modulePointer: program.modules) {
        auto module = decl[modulePointer];
        for(auto globalPointer: module->globals.contents(decl)) {
            if(decl[globalPointer]->reachable) declareGlobal(globalPointer);
        }
    }

    for(auto modulePointer: program.modules) {
        auto module = decl[modulePointer];
        for(auto globalPointer: module->globals.contents(decl)) {
            if(decl[globalPointer]->reachable) fillGlobal(globalPointer);
        }
    }

    for(auto modulePointer: program.modules) {
        auto module = decl[modulePointer];

        for(auto functionPointer: module->functions.contents(decl)) {
            auto function = decl[functionPointer];
            if(!function->reachable || !function->ssa) continue;

            // The symbol, which is what one name per emitted function means - see `Function::symbol`.
            auto target = to.addFunction(function->symbol);
            target->source = function->source;

            /*
             * The two declaration-level backend facts this stage carries - `@x86_legacy_sse` and
             * `@convention`, and `DeclAttribute::LegacySse` for why a resolver carries one at all.
             * The x64 backend refuses a SHA-extension body without the first, so
             * `lib/Digest/ShaBlock.sha.avx2.yana` does not compile at all if that line is missing.
             *
             * A convention left unwritten leaves `callType` at the backend's own default rather than
             * writing one, which is what `Function::convention` being a `Maybe` is for.
             * `foreignBoundary` is derived rather than declared: it is what a *non-executable* build
             * of a function entered under a foreign ABI is, and nothing in source declares one yet.
             */
            target->legacyVectors = function->legacySse;
            target->teardown = function->teardown;

            if(function->convention) {
                target->callType = function->convention.unwrap();
                target->foreignBoundary = !isExecutableMode(context.settings.mode) &&
                                          conventionIsForeignAbi(target->callType);
            } else if(releasesUnderSimple(functionPointer)) {
                target->callType = LowerCallType::Simple;
            }

            auto result = manifestNative(layout, function->result);

            // Which records this signature takes and gives back as their fields - `convention.cpp`.
            // Before any parameter is declared, because the answer decides how many there are.
            planCallShape(functionPointer);
            auto shape = callShapeOf(functionPointer);

            /*
             * The generic environment, before everything - Analysis-IR.md §11.4's G5.
             *
             * First rather than after the hidden result pointer, because everything after it may
             * depend on it: the storage that pointer names is sized from a descriptor slot wherever
             * the result is a type parameter, so a caller reads the environment to build the rest of
             * the call. An order where the dependent argument came first would be legal and would
             * read backwards.
             */
            if(mir::isGeneric(decl, function->gen) && function->form == mir::GenForm::Erased) {
                auto given = target->addArg(lower, StringId(), LowerType::Pointer);
                environmentPlaces.add(functionPointer.offset, &given->result - lower);
            }

            /*
             * A **lifted lambda's** captures, in the same leading position and for the same
             * sentence - `Function::environment`.
             *
             * It is a declared parameter of the MIR function rather than a hidden one, because the
             * body reaches its captures by *projecting* it and a projection needs a place. What it
             * is not is a parameter in the declared order: argument zero is "what the callee needs
             * that its arguments do not say", the two producers of a call agree that it comes first,
             * and a closure whose result came back through storage would otherwise put the pointer
             * that describes the call ahead of the one that answers it.
             */
            auto closure = function->environment != nullptr;

            if(closure) {
                auto first = decl[function->args.get(decl, 0)];
                target->addArg(lower, first->name, manifestNative(layout, first->occurrence).type);
            }

            /*
             * The hidden result pointer, before every declared parameter.
             *
             * A value with no register form comes back through storage the caller supplies, so the
             * caller says where. It is a *leading* argument because that is what the shared IR's
             * other producer does, and two producers that disagreed about the position would build
             * calls no callee could read.
             */
            if(result.inMemory && !(shape && shape->splitsResult())) {
                auto place = target->addArg(lower, StringId(), LowerType::Pointer);
                returnPlaces.add(functionPointer.offset, &place->result - lower);
            }

            // The parameters, before any body is lowered: a call in one function names the arguments
            // of another, so every signature has to exist before the first call is built. Their
            // manifestation is the declared occurrence's, which is what makes a shared window and
            // its owner two different signatures for one logical type.
            //
            // A parameter the shape takes apart is one argument per part, in the order the parts
            // were laid out - which is also the order every call site loads them in.
            for(U32 i = closure ? 1 : 0; i < function->args.size(); i++) {
                auto argument = decl[function->args.get(decl, i)];
                auto carried = shape ? shapeArgs[shape->firstArg + i] : ShapeArg {};

                if(!carried.count) {
                    target->addArg(lower, argument->name,
                                   manifestNative(layout, argument->occurrence).type);
                    continue;
                }

                for(U32 k = 0; k < carried.count; k++) {
                    target->addArg(lower, StringId(), shapeParts[carried.start + k].type);
                }
            }

            // A `{}` and an address-only result both return nothing: the first has no bytes and the
            // second was written into the caller's storage before the `ret`. A result the shape
            // takes apart returns its parts.
            if(shape && shape->splitsResult()) {
                for(U32 k = 0; k < shape->resultCount; k++) {
                    target->returnTypes.push(to.arena, resultPart(*shape, k).type);
                }
            } else if(decl[function->result.type]->kind != mir::Type::Unit && !result.inMemory) {
                target->returnTypes.push(to.arena, result.type);
            }

            if(functionPointer == program.entry) to.entry = target->name;

            functions.add(functionPointer.offset, target - lower);
        }
    }

    for(auto& pending: pendingFunctionRelocations) {
        auto found = functions.getValue(pending.function);
        assertTrue(found);
        auto relocation = lower[pending.global]->relocations.get(lower, pending.relocation);
        relocation.function = found.unwrap();
        lower[pending.global]->relocations.set(lower, pending.relocation, relocation);
    }
    pendingFunctionRelocations.clear();

    /*
     * Every function the loop above declared, which is every function that has a `mir::Function`
     * behind it - so everything appended past this point is one this stage wrote for itself, and the
     * tail pass at the end is what it is for.
     */
    auto declaredCount = to.functionOrder.size();

    countRecordDrops();

    for(auto modulePointer: program.modules) {
        auto module = decl[modulePointer];

        for(auto functionPointer: module->functions.contents(decl)) {
            auto function = decl[functionPointer];
            if(!function->reachable || !function->ssa) continue;

            auto& target = *lower[functions.getValue(functionPointer.offset).unwrap()];
            auto errorsBefore = context.diagnostics.errorCount();
            lowerBody(*function, target);

            /*
             * And the run of passes over the lower IR every producer of it owes its output - see
             * `lower/lower_pipeline.h`, which is where the order between them is argued and where
             * it is shared from. This stage used to call one of them, `makeDivisionTotal`, and say
             * so; the rest were written out at the bottom of `compiler/resolve`'s driver (the stage
             * deleted in J173) and were the whole of the code-quality gap between the two.
             *
             * Gated on `-no-ir-opt` inside the pipeline, division totalization excepted: that one is
             * a correctness pass and not an optimization, since `doc/spec/types.md` makes integer
             * division total and neither native machine does.
             *
             * Per function and immediately behind its body, rather than a walk of its own after the
             * loop, because there is nothing in between the two that could care: what it reads is
             * the function's own blocks.
             */
            // Not over a body lowering refused part of - `checkAddressable` stops a function
            // mid-block, and the passes assume one that was finished. The program is rejected
            // either way; this keeps the rejection a diagnostic rather than a fold over half a body.
            // Asked of *this* body's errors: a count over the whole context also switched the
            // passes off for every body after an unrelated earlier error (J148).
            if(context.diagnostics.errorCount() != errorsBefore) continue;

            auto place = returnPlaces.getValue(functionPointer.offset);
            optimizeLowerFunction(lower, to, target,
                                  place ? place.unwrap() : LowerPtr<LowerValue>(nullptr),
                                  context.settings.optimizeIr);
        }
    }

    /*
     * And the functions this stage **wrote** - every teardown, every box release, every witness
     * accessor and every thunk - which the loop above cannot reach because they do not exist until a
     * body asks for one.
     *
     * They were the one kind of function in the module that never met `lower/lower_pipeline.h`, and
     * what that cost is visible in one line: `teardown(Maybe(Tree))` opened with
     * `load; sub 1; cmp_le; select 1, 0; cmp_eq 1` where `total`, reading the same niche of the same
     * type two functions above it, opened with `cmp_neq %p, 0`. Same layout, same decoder
     * (`decodeFolded`), and the difference was entirely that one of the two had been folded and the
     * other had not - `narrowComparison` turns the range test into the inequality and
     * `isBooleanValued` takes the materialization off it, and neither had ever run here.
     * `doc/progress/2026-09-08-J33-tree-family.md` recorded this as "two decoders"; there is one, and
     * this is what was missing under it.
     *
     * A tail pass rather than a call beside each `addFunction`, because a generated body may generate
     * another - a sum's teardown asks for its fields' - and optimizing one mid-emission would be
     * running a pipeline over a module a later emission still appends to. Every declared function
     * was added before the first body was lowered, so `declaredCount` is exactly the boundary.
     *
     * No return place: a generated body returns nothing or returns a scalar, and the hidden result
     * pointer is a fact about a *declared* signature.
     */
    for(Size i = declaredCount; i < to.functionOrder.size(); i++) {
        optimizeLowerFunction(lower, to, *lower[to.functionOrder[i]], LowerPtr<LowerValue>(nullptr),
                              context.settings.optimizeIr);
    }

    // And what rewrites a callee and its callers together, which needs both loops above finished.
    // Not behind a body that reported, for the reason the first loop gives.
    if(context.diagnostics.errorCount() == 0) optimizeLowerModule(lower, to, context.settings.optimizeIr);
}

Ptr<LowerModule> lowerProgram(Context& context, mir::Program& program, Converted& converted) {
    auto result = Ptr<LowerModule>(new LowerModule(8 * 1024 * 1024));

    // `-mode llvm` is LLVM's reading whatever `-backend` says, since that is what reads the dump;
    // `-mode lower` is the backend's, so the dump shows what that backend is handed. The calling
    // convention reads this field too rather than the setting (J182), so the two cannot disagree -
    // and a driver that runs one backend has to set `settings.backend` to it, since the setting's
    // default is LLVM (see test/ResolveTester.cpp's `FixtureFront`).
    result->consumer = context.settings.backend == NativeBackend::Local && context.settings.mode != CompileMode::Llvm
        ? LowerConsumer::Local : LowerConsumer::Llvm;

    Lowering lowering {
        context, program, converted, *result,
        *program.decl, *converted.arena, *result->arena,
        layoutFor(context, program)
    };

    lowering.run();
    return result;
}

} // namespace ssa
