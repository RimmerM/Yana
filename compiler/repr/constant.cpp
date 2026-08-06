#include "constant.h"

/*
 * Bytes at the target's order, at any width.
 *
 * Written by hand rather than through a `BufferWriter` because a constant's leaves are of every
 * width a type can be - one, two, four and eight bytes, and whatever a packed word turned out to be
 * - and the two directions have to be exact inverses: a packed field is a read-modify-write of a
 * word this same function wrote.
 */
static void writeBytes(const ReprTarget& target, ByteBuffer bytes, U32 offset, U32 width, U64 value) {
    for(U32 i = 0; i < width; i++) {
        auto at = target.byteOrder == LittleEndian ? offset + i : offset + width - 1 - i;
        if(at >= bytes.length) continue;

        bytes.ptr[at] = Byte((value >> (8 * i)) & 0xff);
    }
}

static U64 readBytes(const ReprTarget& target, ByteBuffer bytes, U32 offset, U32 width) {
    U64 value = 0;

    for(U32 i = 0; i < width; i++) {
        auto at = target.byteOrder == LittleEndian ? offset + i : offset + width - 1 - i;
        if(at >= bytes.length) continue;

        value |= U64(U8(bytes.ptr[at])) << (8 * i);
    }

    return value;
}

// One bit range of a word, preserving every bit outside it - the static form of `encodePackedField`,
// and the reason two co-packed fields can be written independently here as well as there.
static void writePacked(const ReprTarget& target, ByteBuffer bytes, U32 offset, U32 wordBytes,
                        U32 bitOffset, U32 bitWidth, U64 value) {
    auto mask = bitWidth >= 64 ? ~U64(0) : ((U64(1) << bitWidth) - 1);
    auto word = readBytes(target, bytes, offset, wordBytes);

    word = (word & ~(mask << bitOffset)) | ((value & mask) << bitOffset);
    writeBytes(target, bytes, offset, wordBytes, word);
}

namespace {

struct ConstantWriter {
    ReprTable& repr;
    ModuleBase local;
    GlobalBase global;
    Array<ConstRelocation>& relocations;

    bool write(ModulePtr<ConstValue> constant, ByteBuffer bytes, U32 offset);

    // The fields of one content tuple, against that tuple's own Repr - which is the list a `Field`
    // projection reads its offset from, so a constant lands where a store to the same field would.
    bool writeFields(TypePtr tuple, ModuleList<ModulePtr<ConstValue>, false>& children, ByteBuffer bytes, U32 offset);

    /*
     * A constant reduced to the bits of its own storage, for the one position that needs a number
     * rather than a placement: a *packed* field, whose word belongs to its neighbours as well.
     *
     * Written into a scratch buffer and read back rather than special-cased, so that a whole record
     * the target scalarized reaches this by the same path a `Bool` does. An address can never be
     * here - nothing narrow is a pointer - and a relocation appearing in the scratch is what says so.
     */
    bool packedBits(ModulePtr<ConstValue> constant, U64& into);
};

bool ConstantWriter::packedBits(ModulePtr<ConstValue> constant, U64& into) {
    if(!constant) {
        into = 0;
        return true;
    }

    auto size = repr.of(local[constant]->type).size;
    if(size == 0 || size > sizeof(U64)) return false;

    Byte buffer[sizeof(U64)] = {};
    ByteBuffer scratch(buffer, size);

    Array<ConstRelocation> none;
    ConstantWriter inner { repr, local, global, none };

    if(!inner.write(constant, scratch, 0) || none.size()) return false;

    into = readBytes(repr.target, scratch, 0, size);
    return true;
}

bool ConstantWriter::writeFields(TypePtr tuple, ModuleList<ModulePtr<ConstValue>, false>& children,
                                 ByteBuffer bytes, U32 offset) {
    auto& layout = repr.of(tuple);
    auto items = children.contents(local);

    for(Size i = 0; i < items.size(); i++) {
        if(i >= layout.fields.size()) return false;

        auto& field = layout.fields[i];

        // The field holds an owning pointer to storage rather than the storage - see the header.
        // There is nothing for one to point at in a constant, and writing the pointer as a zero
        // would produce a value the program then dereferences.
        if(field.boxed) return false;

        // A unit field occupies nothing, so there is nothing to write - the same silence `write`
        // keeps for a unit place.
        if(!items[i]) continue;

        if(field.isPacked()) {
            U64 bits = 0;
            if(!packedBits(items[i], bits)) return false;

            writePacked(repr.target, bytes, offset + field.offset, field.wordBytes,
                        field.bitOffset, field.bitWidth, bits);
            continue;
        }

        if(!write(items[i], bytes, offset + field.offset)) return false;
    }

    return true;
}

bool ConstantWriter::write(ModulePtr<ConstValue> constant, ByteBuffer bytes, U32 offset) {
    if(!constant) return true;

    auto& value = *local[constant];
    auto children = value.children.contents(local);

    switch(value.kind) {
        case ConstKind::Scalar:
            writeBytes(repr.target, bytes, offset, repr.of(value.type).size, value.bits);
            return true;

        case ConstKind::Address:
            // Zeroes, plus a note of what belongs there. Nothing has an address until the module is
            // placed, which is the same reason a table's Function and Global cells are left empty.
            relocations.push(ConstRelocation { offset, value.global });
            return true;

        case ConstKind::String:
            // The static form the evaluator built underneath the text - the two words a native
            // string is. A target that has none put nothing there, and cannot be emitting bytes.
            if(children.size() != 1) return false;
            return write(children[0], bytes, offset);

        case ConstKind::Aggregate: {
            auto declared = global[value.type];

            if(declared->kind == Type::Array) {
                // `n` elements at the stride an `Index` projection steps by, which is where the
                // constant part of that projection's offset comes from - see lowerPlace.
                auto stride = repr.of(((ArrayType*)declared)->content).stride;

                for(Size i = 0; i < children.size(); i++) {
                    if(!write(children[i], bytes, offset + U32(i * stride))) return false;
                }

                return true;
            }

            // A `String`'s representation is the record describing its bytes, at the same offset -
            // see computeString, which copies that record's size and alignment unchanged.
            if(declared->kind == Type::String) {
                if(children.size() != 1) return false;
                return write(children[0], bytes, offset);
            }

            if(declared->kind != Type::Tup) return false;
            return writeFields(value.type, value.children, bytes, offset);
        }

        case ConstKind::Construct: {
            auto declared = global[value.type];
            if(declared->kind != Type::Record) return false;

            auto& record = *(RecordType*)declared;
            if(value.index >= record.constructors.size()) return false;

            auto constructor = record.constructors.get(global, value.index);
            auto& layout = repr.of(value.type);

            /*
             * The discriminant, in whichever of the four forms this target chose - and each one is
             * the static half of the encoder beside it in lower_pack.cpp, which is what makes a
             * constructed constant and a constructed value the same bytes.
             */
            switch(layout.discriminant) {
                case DiscriminantKind::None:
                    break;
                case DiscriminantKind::Word:
                    // At offset zero, which is where a `Discriminant` projection lands: that step
                    // adds nothing to the path's offset.
                    writeBytes(repr.target, bytes, offset, layout.discriminantBytes, value.index);
                    break;
                case DiscriminantKind::Bits:
                    writePacked(repr.target, bytes, offset, layout.discriminantBytes,
                                layout.discriminantBitOffset, layout.discriminantBits, value.index);
                    break;
                case DiscriminantKind::Niche:
                    // The payload constructor writes nothing: it *is* the payload's own bits, and
                    // being inside the valid range is what identifies it - see encodeNicheTag.
                    if(value.index != layout.encoding.payloadConstructor) {
                        writeBytes(repr.target, bytes, offset + layout.encoding.niche.offset,
                                   layout.encoding.niche.bytes,
                                   layout.encoding.patternOf(U16(value.index)));
                    }
                    break;
            }

            // A payload reached through an indirection has nowhere to point - see the header, and
            // `constantHasStaticForm`, which refuses one where it can be named.
            if(constructor.boxed) return false;

            auto content = constructor.content;
            if(!content || isUnit(global, content)) return true;

            auto payload = offset + layout.payloadOffset;

            // A tuple content is the constructor's fields; anything else is one payload carried
            // whole, which is the same split `resolveConstruct` makes.
            if(global[content]->kind == Type::Tup) return writeFields(content, value.children, bytes, payload);

            if(children.size() != 1) return false;
            return write(children[0], bytes, payload);
        }
    }

    return false;
}

} // namespace

bool materializeConstant(ReprTable& repr, ModuleBase local, ModulePtr<ConstValue> constant,
                         ByteBuffer bytes, Array<ConstRelocation>& relocations) {
    ConstantWriter writer { repr, local, repr.global, relocations };
    return writer.write(constant, bytes, 0);
}
