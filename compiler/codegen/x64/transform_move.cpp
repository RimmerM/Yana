#include "transform_internal.h"

/*
 * `compress`, `expand` and `lookup` - `LowerInst::VecMove`, Analysis-Avx512.md §3.1 and §3.3.
 *
 * The three lane movements whose control is a value rather than a field of the instruction, and
 * the three that §3 admits as builtins because their `sse4`/`avx2` spelling is a table or a sequence
 * nothing could recognize. At `avx512` two of them are one instruction each and are left standing
 * for selection (`vpcompress*`, `vpexpand*`); a lookup is `vpermi2*` with the table as both of its
 * sources, which is the family `lowerTwoSourcePermutes` already built. Everything else is written
 * out here, as IR the rest of the pipeline lowers: shuffles with constant patterns, a `pshufb` or a
 * `vpermd` whose control comes out of a `.rodata` table, and - for the shape no permute serves, a
 * lookup whose table and indices differ in lane count other than a sixteen-byte table read at
 * thirty-two indices (`lookupDoubled`) - a walk over the lanes through a frame slot.
 *
 * ## Where it sits
 *
 * Above `chooseMaskDomain`, `poolVectorConstants` and the two permute lowerings, which are the three
 * passes that have to see what this builds: the constant splats become pooled loads, the byte
 * patterns become `pshufb`, and the comparisons this writes are masks like any other. Below
 * `lowerVectorReductions`, because the movemask this reads is the one instruction that pass leaves
 * standing - and at a word lane that is `pmovmskb`'s two bits a lane, which is why `laneBits` below
 * folds the pairs itself rather than reading a bit per lane that is not there.
 */

/*
 * A table of controls, built once per shape and per module and read at an index.
 *
 * Interned by *name* rather than by content, which is where it departs from `pooledBytes`: the
 * contents are a function of the shape the name spells, so the name is the whole key, and the
 * tables run to kilobytes where a pooled constant is a register's width - which is also why `fill`
 * writes them in place, and only the first time. Marked `pooled`, so a function whose instruction
 * was rewritten away leaves nothing behind.
 */
template<class Fill>
static LowerGlobal* moveTable(Context& ctx, LowerModule& module, const char* name, Size size,
                              U8 alignment, Fill&& fill) {
    Size length = 0;
    while(name[length]) length++;
    auto hash = Context::nameHash(name, length);

    auto entry = module.globals.add(hash);
    if(entry.existed) return (*module.arena)[*entry.value];

    // Copied for `pooledConstant`'s reason: `addUnqualifiedName` keeps the pointer it is handed.
    auto stored = (char*)module.arena.alloc(length);
    copyMem(name, stored, length);
    ctx.addUnqualifiedName(stored, length);

    auto global = new (module.arena) LowerGlobal(hash);
    auto contents = (U8*)module.arena.alloc(size);
    fill(contents);

    global->initialContents = { contents, size };
    global->pooled = true;
    global->alignment = alignment;
    *entry.value = global - *module.arena;
    module.globalOrder.push(global - *module.arena);

    return global;
}

namespace {

// The rewrite of one `VecMove`, with what every piece of it needs in hand.
struct MoveExpansion {
    Context& ctx;
    Expansion& e;

    LowerValue* global(LowerGlobal* table) {
        auto& module = *e.fun.module;
        return e.emit(new (e.fun.arena) LowerInstGlobal(StringId(), table - *module.arena));
    }

    // The same bits under another type, and nothing at all where the two are one type already.
    LowerValue* as(LowerType type, LowerValue* value) {
        return value->type == type ? value : e.reinterpret(type, value);
    }

    LowerValue* splat(LowerType type, U64 value) {
        return e.splat(type, e.integer(scalarFormOf(type), value));
    }

    // A vector constant with its bytes written out, which is a pooled load - the same thing
    // `poolVectorConstants` would have made of a `vsplat` chain, built directly.
    LowerValue* constant(LowerType type, const U8* bytes) {
        auto pooled = pooledBytes(ctx, *e.fun.module, bytes, type.byteWidth());
        return e.load(type, global(pooled), type.byteWidth());
    }

    // Entry `index` of a table whose entries are `1 << shift` bytes, read as `type`.
    LowerValue* entry(LowerGlobal* table, LowerValue* index, U32 shift, LowerType type) {
        auto wide = e.convert(LowerType::Int64, index, false, false);
        auto offset = e.binary(LowerInst::Shl, LowerType::Int64, wide, e.integer(LowerType::Int64, shift));
        auto address = e.binary(LowerInst::Add, LowerType::Pointer, global(table), offset);

        return e.load(type, address, type.byteWidth());
    }

    LowerValue* permuteInst(LowerType type, LowerValue* indices, LowerValue* from) {
        return e.emit(new (e.fun.arena) LowerInstX86Permute(StringId(), type, indices - e.base, from - e.base));
    }

    /*
     * The mask as a number with one bit per lane, lane `i` in bit `i`.
     *
     * The movemask is that everywhere but at a word lane, where `pmovmskb` answers a bit per byte and
     * so two per lane - `maskBitsPerLane`'s rule. The pairs are folded into one bit each: `pext`
     * against the even bits where BMI2 is claimed, and the four-step fold that is the same answer
     * everywhere else.
     */
    LowerValue* laneBits(LowerValue* mask) {
        auto bits = e.emit(new (e.fun.arena) LowerInstVecReduce(StringId(), LowerType::Int32,
                                                                 mask - e.base, LowerReduce::Bits));
        if(laneBytes(mask->type.lane) != 2) return bits;

        auto scalar = LowerType::Int32;
        auto even = mask->type.lanes() == 16 ? U64(0x55555555) : U64(0x5555);

        if(targetFeatures() & kFeatureBmi2) {
            return e.binary(LowerInst::GatherBits, scalar, bits, e.integer(scalar, even));
        }

        auto folded = e.binary(LowerInst::And, scalar, bits, e.integer(scalar, even));
        static const U64 kFolds[3][2] = { { 1, 0x33333333 }, { 2, 0x0f0f0f0f }, { 4, 0x00ff00ff } };

        for(auto& step: kFolds) {
            auto moved = e.binary(LowerInst::Shr, scalar, folded, e.integer(scalar, step[0]));
            folded = e.binary(LowerInst::And, scalar, e.binary(LowerInst::Or, scalar, folded, moved),
                              e.integer(scalar, step[1]));
        }

        if(mask->type.lanes() == 16) {
            auto moved = e.binary(LowerInst::Shr, scalar, folded, e.integer(scalar, 8));
            folded = e.binary(LowerInst::And, scalar, e.binary(LowerInst::Or, scalar, folded, moved),
                              e.integer(scalar, 0xffff));
        }

        return folded;
    }

    /*
     * A byte control for `pshufb` out of lane indices: byte `k` of lane `i` reads byte
     * `(indices[i] mod lanes) * width + k` of the table. The modulus is the `and` in front, the
     * scale is a shift at the lane's own width, and the spread is a constant byte shuffle - which
     * stays inside each 128-bit half, so it is one `pshufb` at either width.
     */
    LowerValue* byteControl(LowerValue* indices, U32 tableLanes) {
        auto type = indices->type;
        auto width = laneBytes(type.lane);
        auto wrapped = e.binary(LowerInst::And, type, indices, splat(type, tableLanes - 1));
        if(width == 1) return wrapped;

        auto shift = width == 2 ? 1 : width == 4 ? 2 : 3;
        auto scaled = e.binary(LowerInst::Shl, type, wrapped, splat(type, shift));

        auto bytes = vectorType(LowerLane::Int8, type.byteWidth());
        auto asBytes = as(bytes, scaled);
        auto spread = e.shuffle(bytes, asBytes, asBytes, [&](Size j) { return U8(j / width * width); });

        U8 steps[kMaxVectorBytes] = {};
        for(U32 j = 0; j < type.byteWidth(); j++) steps[j] = U8(j % width);

        return e.binary(LowerInst::Add, bytes, spread, constant(bytes, steps));
    }

    /*
     * A byte table read at a byte control whose entries are all below the table's size.
     *
     * Sixteen bytes is `pshufb` and nothing else. Thirty-two is the case AVX2 makes a sequence of,
     * because `vpshufb` reads each 128-bit half of its table on its own: the table's low half is
     * placed in both halves of one register and its high half in both of another, each is read with
     * the control, and the control's bit 4 chooses between the two answers. `pshufb` ignores that
     * bit, so the same control serves both reads.
     */
    LowerValue* permuteBytes(LowerValue* table, LowerValue* control) {
        auto type = table->type;
        if(type.byteWidth() == 16) return permuteInst(type, control, table);

        auto quads = vectorType(LowerLane::Int64, 4);
        auto asQuads = as(quads, table);
        auto low = as(type, e.shuffle(quads, asQuads, asQuads, [](Size j) { return U8(j % 2); }));
        auto high = as(type, e.shuffle(quads, asQuads, asQuads, [](Size j) { return U8(2 + j % 2); }));

        auto fromLow = permuteInst(type, control, low);
        auto fromHigh = permuteInst(type, control, high);
        auto upper = e.emit(new (e.fun.arena) LowerInstCmp(StringId(), control - e.base,
                                                            splat(type, 15) - e.base, LowerCmp::igt,
                                                            maskType(type.lane, type.lanes())));

        return e.select(type, upper, fromHigh, fromLow);
    }

    /*
     * A table of one register read at indices of its own shape - `permute`, and every lookup that
     * reaches here.
     *
     * `vpermi2*` at `avx512`, with the table as both sources: the index's bit above the lane number
     * picks the source, and both are the table, so what is read is the index modulo the lane count
     * whatever that bit holds. `vpermd`/`vpermps` at a 256-bit doubleword lane, which reads the low
     * three bits of each index and nothing else. A 256-bit quadword lane is the same instruction
     * over each lane's two halves. Everything else is a byte control and `pshufb`.
     */
    LowerValue* permute(LowerValue* table, LowerValue* indices) {
        auto type = table->type;
        auto width = laneBytes(type.lane);
        auto wide = type.byteWidth() == 32;

        if(hasEvexRow(width == 1 ? EvexRow::BytePermute2 : EvexRow::Permute2)) {
            return e.emit(new (e.fun.arena) LowerInstX86Permute2(StringId(), type, indices - e.base,
                                                                  table - e.base, table - e.base));
        }

        if(wide && width == 4) return permuteInst(type, indices, table);

        if(wide && width == 8) {
            auto dwords = vectorType(LowerLane::Int32, 8);
            auto wrapped = e.binary(LowerInst::And, indices->type, indices, splat(indices->type, 3));
            auto doubled = as(dwords, e.binary(LowerInst::Shl, indices->type, wrapped, splat(indices->type, 1)));
            auto paired = e.shuffle(dwords, doubled, doubled, [](Size j) { return U8(j / 2 * 2); });

            static const U8 kHalves[32] = { 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
                                            0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0 };
            auto control = e.binary(LowerInst::Add, dwords, paired, constant(dwords, kHalves));

            return as(type, permuteInst(dwords, control, as(dwords, table)));
        }

        auto bytes = vectorType(LowerLane::Int8, type.byteWidth());
        auto control = byteControl(indices, type.lanes());

        return as(type, permuteBytes(as(bytes, table), control));
    }

    /*
     * A table of one 128-bit register read at indices of twice its width - the hex digits, Base64's
     * offsets and every other sixteen-byte table a loop at the natural width reads at `avx2`.
     *
     * The table is placed in both halves (`vperm2f128` of itself, a shuffle whose result has twice
     * its sources' lanes), and then each half of the index vector reads its own copy: `vpshufb` never
     * crosses a half, which is the reason the 256-bit `permuteBytes` above needs two reads and a
     * select, and exactly why it needs one here. At `avx512` the doubled table goes to `permute` - a
     * `vpermi2*` over two copies of two copies reads the index modulo the table's own lanes as well.
     */
    LowerValue* lookupDoubled(LowerValue* table, LowerValue* indices) {
        auto type = table->type;
        auto lanes = type.lanes();
        auto doubledType = vectorType(type.lane, lanes * 2);
        auto doubled = e.shuffle(doubledType, table, table, [&](Size j) { return U8(j % lanes); });

        if(hasEvexRow(laneBytes(type.lane) == 1 ? EvexRow::BytePermute2 : EvexRow::Permute2)) {
            return permute(doubled, indices);
        }

        auto bytes = vectorType(LowerLane::Int8, 32);
        return as(doubledType, permuteInst(bytes, byteControl(indices, lanes), as(bytes, doubled)));
    }

    /*
     * A table of 128 or 256 bits read at 512-bit indices - the same sixteen-byte tables at `avx512`'s
     * natural width, where `pushHex`'s digits and `memberOf`'s bitmap are read at sixty-four bytes.
     *
     * The table is repeated until it fills a `zmm` - doubled into a `ymm` as above where it is one
     * register of 128 bits, and that joined with itself (`vinserti64x4`, FormVFullJoin) - and read by
     * one `vpermi2b` over two copies of the result, which takes each index modulo 128 and so modulo the
     * table's own lane count, the lookup's definition. The repetition depends on the table alone, so a
     * loop reading a constant table hoists it with the table.
     */
    LowerValue* lookupRepeated(LowerValue* table, LowerValue* indices) {
        auto type = table->type;
        auto lanes = type.lanes();

        if(type.byteWidth() == 16) {
            auto doubledType = vectorType(type.lane, lanes * 2);
            table = e.shuffle(doubledType, table, table, [&](Size j) { return U8(j % lanes); });
            type = doubledType;
            lanes = type.lanes();
        }

        auto fullType = vectorType(type.lane, lanes * 2);
        auto full = e.shuffle(fullType, table, table, [](Size j) { return U8(j); });
        return permute(full, indices);
    }

    /*
     * A frame slot of `size` bytes. A fixed allocation is a frame object wherever it stands (see
     * `collectFrameObjects`), so one inside a loop is one slot rather than one per iteration. Eight
     * bytes' alignment and no more: every access here is an unaligned one, and asking for sixteen
     * would realign the frame for nothing.
     */
    LowerValue* slot(U64 size) {
        auto count = e.integer(LowerType::Int64, size);
        return e.emit(new (e.fun.arena) LowerInstAlloca(StringId(), count - e.base, 8));
    }

    // `slot + index * width`, with the index an `Int64`.
    LowerValue* laneAddress(LowerValue* base, LowerValue* index, U32 width) {
        auto shift = width == 1 ? 0 : width == 2 ? 1 : width == 4 ? 2 : 3;
        auto offset = shift ? e.binary(LowerInst::Shl, LowerType::Int64, index, e.integer(LowerType::Int64, shift))
                            : index;
        return e.binary(LowerInst::Add, LowerType::Pointer, base, offset);
    }

    /*
     * A lookup through memory, a lane at a time - the shape no permute serves: a table whose lane
     * count is not its indices', unless it is one 128-bit register read at two (`lookupDoubled`). The
     * table is stored and each answer lane is read at its index - a handful of instructions a lane,
     * and correct at every shape.
     */
    LowerValue* lookupThroughMemory(LowerValue* table, LowerValue* indices, LowerType result) {
        auto width = laneBytes(table->type.lane);
        auto stored = slot(table->type.byteWidth());
        e.store(stored, table, table->type.byteWidth());

        auto scalar = scalarFormOf(result);
        auto wrap = U64(table->type.lanes() - 1);
        LowerValue* answer = nullptr;

        for(U32 i = 0; i < result.lanes(); i++) {
            auto index = e.lane(indices, U8(i));
            auto wide = index->type == LowerType::Int64 ? index : e.convert(LowerType::Int64, index, false, false);
            auto wrapped = e.binary(LowerInst::And, LowerType::Int64, wide, e.integer(LowerType::Int64, wrap));
            auto value = e.load(scalar, laneAddress(stored, wrapped, width), width);

            answer = i == 0 ? e.splat(result, value) : e.withLane(result, answer, U8(i), value);
        }

        return answer;
    }

    /*
     * The tables, one per shape and direction.
     *
     * A 128-bit entry is a `pshufb` control, sixteen bytes: a compress entry names, for each result
     * lane below the count, the bytes of the source lane it takes, and `0x80` - which `pshufb` answers
     * zero for - past it; an expand entry names the source lane each set lane takes and `0x80` at
     * the lanes the mask clears, so the zeros `expand` owes are the instruction's own. A 256-bit entry
     * is a `vpermd` index vector, thirty-two bytes, and has no zero to name - an expand's clear lanes
     * are selected away after it.
     *
     * Indexed by the mask's lanes as a number, so there are `1 << lanes` entries: 64 bytes at two
     * quadword lanes, 4 KB at eight word lanes, 8 KB at eight doubleword lanes of a `ymm`.
     */
    LowerGlobal* laneTable(bool compress, U32 lanes, U32 width, U32 entryBytes) {
        char name[48];
        formatName(name, compress, lanes, width, entryBytes);

        auto entries = Size(1) << lanes;

        return moveTable(ctx, *e.fun.module, name, entries * entryBytes, U8(entryBytes), [&](U8* bytes) {
            fillLaneTable(bytes, compress, lanes, width, entryBytes);
        });
    }

    static void fillLaneTable(U8* bytes, bool compress, U32 lanes, U32 width, U32 entryBytes) {
        auto entries = Size(1) << lanes;

        for(Size mask = 0; mask < entries; mask++) {
            auto cell = bytes + mask * entryBytes;
            auto dword = entryBytes == 32;

            for(U32 k = 0; k < entryBytes; k++) cell[k] = dword ? 0 : 0x80;

            // Which source lane each result lane reads, or `lanes` for none.
            U32 read[16];
            for(U32 i = 0; i < lanes; i++) read[i] = lanes;

            U32 count = 0;
            for(U32 i = 0; i < lanes; i++) {
                if(!((mask >> i) & 1)) continue;
                if(compress) read[count] = i;
                else read[i] = count;
                count++;
            }

            for(U32 i = 0; i < lanes; i++) {
                if(read[i] == lanes) continue;

                if(dword) {
                    // Doubleword indices, two per quadword lane.
                    auto per = width / 4;
                    for(U32 k = 0; k < per; k++) cell[(i * per + k) * 4] = U8(read[i] * per + k);
                } else {
                    for(U32 k = 0; k < width; k++) cell[i * width + k] = U8(read[i] * width + k);
                }
            }
        }
    }

    static void formatName(char* out, bool compress, U32 lanes, U32 width, U32 entryBytes) {
        auto text = compress ? "$vcompress$" : "$vexpand$";
        Size at = 0;
        while(*text) out[at++] = *text++;

        auto number = [&](U32 value) {
            char digits[8];
            Size count = 0;
            do { digits[count++] = char('0' + value % 10); value /= 10; } while(value);
            while(count) out[at++] = digits[--count];
        };

        number(lanes);
        out[at++] = 'x';
        number(width * 8);
        out[at++] = '$';
        number(entryBytes * 8);
        out[at] = 0;
    }

    /*
     * A byte vector of sixteen lanes, compressed or expanded a half at a time.
     *
     * Its mask is sixteen bits, so a table of whole controls would have 65536 entries; two tables of
     * 256 read one half each. What joins them is how many lanes the low half keeps, `k`.
     *
     * A compress takes the low half's control as it stands - its tail is zero bytes here rather than
     * `0x80`, so the `or` below leaves the high half's bytes alone - and the high half's control, whose
     * entries already name bytes eight and up, moved up by `k` bytes with a `pshufb` out of a third,
     * nine-entry table. The high half's own tail is `0x80`, so the answer's tail is zero.
     *
     * An expand's low half needs nothing, and its high half reads source lanes `k` and up: its entries
     * sit in bytes eight and up already, so `k` is added to those bytes and to no others. A clear
     * lane's `0x80` plus at most eight is still a byte with its top bit set, and still reads zero.
     */
    LowerGlobal* halfTable(bool compress, bool high) {
        const char* names[2][2] = { { "$vexpand$8x8$low", "$vexpand$8x8$high" },
                                    { "$vcompress$8x8$low", "$vcompress$8x8$high" } };

        return moveTable(ctx, *e.fun.module, names[compress][high], 256 * 16, 16, [&](U8* bytes) {
            fillHalfTable(bytes, compress, high);
        });
    }

    static void fillHalfTable(U8* bytes, bool compress, bool high) {
        for(Size mask = 0; mask < 256; mask++) {
            auto cell = bytes + mask * 16;
            U32 count = 0;

            if(compress) {
                for(U32 k = 0; k < 16; k++) cell[k] = high ? 0x80 : 0x00;

                for(U32 i = 0; i < 8; i++) {
                    if((mask >> i) & 1) cell[count++] = U8(high ? i + 8 : i);
                }
            } else {
                for(U32 k = 0; k < 16; k++) cell[k] = 0;
                auto first = high ? 8u : 0u;

                for(U32 i = 0; i < 8; i++) {
                    cell[first + i] = (mask >> i) & 1 ? U8(count++) : 0x80;
                }
            }
        }
    }

    LowerGlobal* shiftTable() {
        return moveTable(ctx, *e.fun.module, "$vcompress$shift", 9 * 16, 16, [](U8* bytes) {
            for(U32 k = 0; k <= 8; k++) {
                for(U32 j = 0; j < 16; j++) bytes[k * 16 + j] = j >= k ? U8(j - k) : 0x80;
            }
        });
    }

    LowerValue* bytesByHalves(bool compress, LowerValue* vector, LowerValue* bits) {
        auto type = vector->type;
        auto scalar = LowerType::Int32;

        auto low = e.binary(LowerInst::And, scalar, bits, e.integer(scalar, 0xff));
        auto high = e.binary(LowerInst::Shr, scalar, bits, e.integer(scalar, 8));
        auto kept = e.intrinsic(LowerIntrinsic::Popcnt, scalar, low);

        auto fromLow = entry(halfTable(compress, false), low, 4, type);
        auto fromHigh = entry(halfTable(compress, true), high, 4, type);
        LowerValue* control = nullptr;

        if(compress) {
            auto moved = permuteInst(type, entry(shiftTable(), kept, 4, type), fromHigh);
            control = e.binary(LowerInst::Or, type, fromLow, moved);
        } else {
            static const U8 kUpper[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
            auto offset = e.binary(LowerInst::And, type, e.splat(type, kept), constant(type, kUpper));
            control = e.binary(LowerInst::Add, type, e.binary(LowerInst::Or, type, fromLow, fromHigh), offset);
        }

        return permuteInst(type, control, vector);
    }

    // A 128-bit vector moved by its mask's lanes as a number, `bits` - by halves at a byte lane, and
    // by one table otherwise.
    LowerValue* moveRegister(bool compress, LowerValue* vector, LowerValue* bits) {
        auto type = vector->type;
        auto byteType = vectorType(LowerLane::Int8, 16);

        if(laneBytes(type.lane) == 1) return as(type, bytesByHalves(compress, as(byteType, vector), bits));

        auto control = entry(laneTable(compress, type.lanes(), laneBytes(type.lane), 16), bits, 4, byteType);
        return as(type, permuteInst(byteType, control, as(byteType, vector)));
    }

    /*
     * A 256-bit byte or word vector, a 128-bit half at a time - the shapes whose mask is too many
     * bits for any table.
     *
     * Each half is moved by the 128-bit route, its mask being its half of the mask's bits, and what
     * joins them is how many lanes the low half keeps, `k`. A compress stores the low half's answer and
     * then the high half's `k` lanes further on, over its unspecified tail, and reads the register
     * back; an expand reads its high half's source out of the vector stored whole, starting at lane
     * `k`. One slot, two stores and a load, against a store and a load per lane.
     */
    LowerValue* moveByHalves(bool compress, LowerValue* vector, LowerValue* bits) {
        auto type = vector->type;
        auto width = laneBytes(type.lane);
        auto half = type.lanes() / 2;
        auto halfType = vectorType(type.lane, half);
        auto scalar = LowerType::Int32;

        auto pick = [&](LowerValue* from, U32 first) {
            return e.shuffle(halfType, from, from, [&](Size j) { return U8(first + j); });
        };

        auto lowBits = e.binary(LowerInst::And, scalar, bits, e.integer(scalar, (U64(1) << half) - 1));
        auto highBits = e.binary(LowerInst::Shr, scalar, bits, e.integer(scalar, half));
        auto kept = e.intrinsic(LowerIntrinsic::Popcnt, scalar, lowBits);
        auto offset = e.convert(LowerType::Int64, kept, false, false);
        auto stored = slot(type.byteWidth());

        if(compress) {
            e.store(stored, moveRegister(true, pick(vector, 0), lowBits), 16);
            e.store(laneAddress(stored, offset, width), moveRegister(true, pick(vector, half), highBits), 16);
            return e.load(type, stored, type.byteWidth());
        }

        e.store(stored, vector, type.byteWidth());
        auto highSource = e.load(halfType, laneAddress(stored, offset, width), 16);

        auto lowAnswer = moveRegister(false, pick(vector, 0), lowBits);
        auto highAnswer = moveRegister(false, highSource, highBits);
        return e.shuffle(type, lowAnswer, highAnswer, [](Size j) { return U8(j); });
    }

    // A compress or an expand of any lane below `avx512`: one table read by the mask's lanes as a
    // number at 128 bits (by halves at a byte lane), a 128-bit half at a time at a 256-bit byte or
    // word lane, and a `vpermd` table at a 256-bit doubleword or quadword lane.
    LowerValue* maskedMove(bool compress, LowerValue* vector, LowerValue* mask) {
        auto type = vector->type;
        auto width = laneBytes(type.lane);

        if(type.byteWidth() == 16) return moveRegister(compress, vector, laneBits(mask));
        if(width <= 2) return moveByHalves(compress, vector, laneBits(mask));

        auto dwords = vectorType(width == 4 && isFloatVector(type) ? LowerLane::Float32 : LowerLane::Int32, 8);
        auto indices = entry(laneTable(compress, type.lanes(), width, 32), laneBits(mask), 5,
                             vectorType(LowerLane::Int32, 8));
        auto moved = as(type, permuteInst(dwords, indices, as(dwords, vector)));

        return compress ? moved : e.select(type, mask, moved, splat(type, 0));
    }
};

} // namespace

void lowerVectorMoves(Context& ctx, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(inst->kind != LowerInst::VecMove) continue;

            auto move = (LowerInstVecMove*)inst;
            auto kind = move->getMove();

            // The instruction itself, at the level that has one - see `selectPackedForm`.
            if(kind != LowerLaneMove::Lookup && hasEvexRow(EvexRow::Compress)) continue;

            Expansion e { base, fun, block, i };
            MoveExpansion m { ctx, e };

            auto source = base[move->source];
            auto control = base[move->control];
            LowerValue* answer = nullptr;

            // The indices are the table's lane width (`validateVectorInst`), so only the counts differ.
            if(kind == LowerLaneMove::Lookup) {
                if(source->type.lanes() == control->type.lanes()) {
                    answer = m.permute(source, control);
                } else if(isFullVector(control->type) && source->type.byteWidth() < 64
                          && hasEvexRow(laneBytes(source->type.lane) == 1 ? EvexRow::BytePermute2 : EvexRow::Permute2)) {
                    answer = m.lookupRepeated(source, control);
                } else if(source->type.byteWidth() == 16 && control->type.byteWidth() == 32) {
                    answer = m.lookupDoubled(source, control);
                } else {
                    answer = m.lookupThroughMemory(source, control, move->result.type);
                }
            } else {
                answer = m.maskedMove(kind == LowerLaneMove::Compress, source, control);
            }

            replaceAllUses(base, &move->result, answer);
            removeInst(base, inst);

            // Everything the rewrite inserted stands above where the move was, and none of it is a
            // move, so the walk resumes at what followed it.
            i = e.at - 1;
        }
    }
}

/*
 * A shuffle whose result is a different width from its sources - which the lower IR permits, and
 * which selection reads against the *result's* lane count (`packedShuffleChoice` says so).
 *
 * That misreading is harmless in exactly two shapes and wrong in every other: a widening shuffle that
 * reads only its left source's lanes, whose numbering is the same either way, and a narrowing one
 * that reads only the lanes below the result's count. So `shuffle(v, [16 .. 31])` over a 32-byte `v`
 * was the *low* half, and joining two 128-bit vectors into one 256-bit register read the first twice -
 * silently, at `avx2`, in both resolvers. Found by `moveByHalves`, which builds both.
 *
 * Everything else goes through one frame slot: both sources stored end to end, and the answer loaded
 * back - one load where the pattern is a run of consecutive lanes (a half, a join), and a load per lane
 * otherwise. The pack and unpack steps `expandLaneCountConversions` builds are selected whole and are
 * left alone.
 */
void lowerWidthChangingShuffles(Context& ctx, LowerBase base, LowerFunction& fun) {
    for(auto offset: fun.blocks.contents(base)) {
        auto block = base[offset];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto inst = base[block->instructions.get(base, i)];
            if(!isWidthChangingShuffle(base, inst)) continue;

            auto shuffle = (LowerInstVecShuffle*)inst;
            auto type = shuffle->result.type;
            auto left = base[shuffle->left];
            auto right = base[shuffle->right];
            auto sources = left->type;

            // A shape selection reads correctly stays, where it has a form to read it into.
            auto pattern = shuffle->pattern();
            auto readable = type.lanes() > sources.lanes() ? sources.lanes() : type.lanes();
            auto harmless = true;
            for(auto entry: pattern) {
                if(entry >= readable) harmless = false;
            }

            // And the halves and joins of a 512-bit register, which `packedShuffleChoice` reads by
            // their sources' width and so reads correctly whatever their pattern says - see
            // FormVFullLow.
            if(isFullHalvesShuffle(base, inst)) continue;
            if(harmless && packedShuffleChoice(base, inst)) continue;

            Expansion e { base, fun, block, i };
            MoveExpansion m { ctx, e };

            auto width = laneBytes(type.lane);
            auto stored = m.slot(sources.byteWidth() * 2);
            e.store(stored, left, sources.byteWidth());
            e.store(e.binary(LowerInst::Add, LowerType::Pointer, stored,
                             e.integer(LowerType::Int64, sources.byteWidth())), right, sources.byteWidth());

            auto run = true;
            for(Size j = 1; j < pattern.size(); j++) {
                if(pattern[j] != pattern[0] + j) run = false;
            }

            LowerValue* answer = nullptr;

            if(run) {
                answer = e.load(type, m.laneAddress(stored, e.integer(LowerType::Int64, pattern[0]), width),
                                type.byteWidth());
            } else {
                auto scalar = scalarFormOf(type);

                for(Size j = 0; j < pattern.size(); j++) {
                    auto at = m.laneAddress(stored, e.integer(LowerType::Int64, pattern[j]), width);
                    auto value = e.load(scalar, at, width);
                    answer = j == 0 ? e.splat(type, value) : e.withLane(type, answer, U8(j), value);
                }
            }

            replaceAllUses(base, &shuffle->result, answer);
            removeInst(base, inst);
            i = e.at - 1;
        }
    }
}
