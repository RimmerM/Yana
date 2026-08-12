#include "elf.h"
#include "Net/Buffer.h"
#include <File.h>

/*
 * The file format, as the numbers it is.
 *
 * Written out by hand rather than through the host's <elf.h> for one reason: what is being produced
 * is a file for a *target*, and a compiler running on macOS or Windows has to produce exactly the
 * same bytes as one running on Linux. A header supplied by the host describes the host's idea of the
 * format, and taking sizes and constants from it would make the output depend on where the compiler
 * was built.
 *
 * Every field is written little-endian and at its stated width, which is what the class and data
 * encoding in e_ident promise a reader.
 */

static constexpr U8 kElfClass64 = 2;    // EI_CLASS
static constexpr U8 kElfData2Lsb = 1;   // EI_DATA
static constexpr U8 kElfVersion = 1;    // EI_VERSION, and e_version

static constexpr U16 kElfTypeExec = 2;  // ET_EXEC - a non-relocatable executable
static constexpr U16 kElfMachineX64 = 62;

static constexpr U16 kElfHeaderSize = 64;
static constexpr U16 kProgramHeaderSize = 56;
static constexpr U16 kSectionHeaderSize = 64;
static constexpr U16 kSymbolSize = 24;

static constexpr U32 kProgramTypeNull = 0;
static constexpr U32 kProgramTypeLoad = 1;

// The segment nothing is loaded from. Its permissions are the stack's, which is the only thing it
// says: without one the kernel falls back to whatever the architecture's default is, and on amd64
// that has historically meant an executable stack. This program does not need one.
static constexpr U32 kProgramTypeGnuStack = 0x6474e551;

static constexpr U32 kProgramFlagExecute = 1;
static constexpr U32 kProgramFlagWrite = 2;
static constexpr U32 kProgramFlagRead = 4;

static constexpr U32 kSectionTypeNull = 0;
static constexpr U32 kSectionTypeProgBits = 1;
static constexpr U32 kSectionTypeSymtab = 2;
static constexpr U32 kSectionTypeStrtab = 3;

static constexpr U64 kSectionFlagWrite = 1;
static constexpr U64 kSectionFlagAlloc = 2;
static constexpr U64 kSectionFlagExecute = 4;

static constexpr U8 kSymbolTypeObject = 1;
static constexpr U8 kSymbolTypeFunc = 2;
static constexpr U8 kSymbolBindLocal = 0;

// The section indexes, in the order they are written. The symbol table names two of them, and the
// header names the last, so the numbering is used rather than only implied.
static constexpr U16 kSectionText = 1;
static constexpr U16 kSectionData = 2;
static constexpr U16 kSectionSymtab = 3;
static constexpr U16 kSectionStrtab = 4;
static constexpr U16 kSectionStringNames = 5;
static constexpr U16 kSectionCount = 6;

// Three program headers: the two loaded segments and the stack permission. Fixed rather than
// computed, because elfCodeOffset() has to be answerable before anything has been generated - the
// image's absolute addresses are written against it.
static constexpr U16 kProgramHeaderCount = 3;

static U64 alignUp(U64 value, U64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

U32 elfCodeOffset() {
    return U32(alignUp(kElfHeaderSize + U64(kProgramHeaderCount) * kProgramHeaderSize, 16));
}

U32 elfDataPadding(U32 codeSize) {
    auto end = elfCodeOffset() + U64(codeSize);
    return U32(alignUp(end, kElfPageSize) - end);
}

static void writeIdent(Net::BufferWriter& out) {
    out.writeByte(0x7f);
    out.writeByte('E');
    out.writeByte('L');
    out.writeByte('F');
    out.writeByte(kElfClass64);
    out.writeByte(kElfData2Lsb);
    out.writeByte(kElfVersion);

    // EI_OSABI and EI_ABIVERSION, then the padding to sixteen bytes. Zero is System V and the base
    // ABI, which is what a program making raw system calls is written against.
    for(Size i = 7; i < 16; i++) out.writeByte(0);
}

static void writeProgramHeader(Net::BufferWriter& out, U32 type, U32 flags, U64 offset, U64 address,
                               U64 fileSize, U64 memorySize, U64 alignment) {
    out.writeInt<LittleEndian>(type);
    out.writeInt<LittleEndian>(flags);
    out.writeLong<LittleEndian>(offset);
    out.writeLong<LittleEndian>(address);

    // p_paddr, which has no meaning for a hosted program and is conventionally the virtual address.
    out.writeLong<LittleEndian>(address);

    out.writeLong<LittleEndian>(fileSize);
    out.writeLong<LittleEndian>(memorySize);
    out.writeLong<LittleEndian>(alignment);
}

static void writeSectionHeader(Net::BufferWriter& out, U32 name, U32 type, U64 flags, U64 address,
                               U64 offset, U64 size, U32 link, U32 info, U64 alignment, U64 entrySize) {
    out.writeInt<LittleEndian>(name);
    out.writeInt<LittleEndian>(type);
    out.writeLong<LittleEndian>(flags);
    out.writeLong<LittleEndian>(address);
    out.writeLong<LittleEndian>(offset);
    out.writeLong<LittleEndian>(size);
    out.writeInt<LittleEndian>(link);
    out.writeInt<LittleEndian>(info);
    out.writeLong<LittleEndian>(alignment);
    out.writeLong<LittleEndian>(entrySize);
}

static void writeSymbol(Net::BufferWriter& out, U32 name, U8 info, U16 section, U64 value, U64 size) {
    out.writeInt<LittleEndian>(name);
    out.writeByte(info);
    out.writeByte(0); // st_other: default visibility
    out.writeShort<LittleEndian>(section);
    out.writeLong<LittleEndian>(value);
    out.writeLong<LittleEndian>(size);
}

// Appends a name to a string table and answers the offset a header refers to it by. A table always
// opens with the empty string, so offset 0 is "no name" and never a real one.
static U32 addString(Net::BufferWriter& table, StringView name) {
    auto offset = U32(table.offset());
    table.writeBytes((const Byte*)name.ptr, name.length);
    table.writeByte(0);
    return offset;
}

static Maybe<U16> machineFor(TargetArch arch) {
    switch(arch) {
        case TargetArch::X64: return Just(U16(kElfMachineX64));

        // The others have no backend that produces an image, so a number here would describe a file
        // nothing can fill in. Reported by the caller rather than guessed at.
        case TargetArch::X86:
        case TargetArch::ARM:
        case TargetArch::ARM64:
        default:
            return Nothing();
    }
}

bool writeElfExecutable(Context& context, const ElfImage& image, TargetArch arch, const String& path) {
    auto machine = machineFor(arch);
    if(!machine) {
        context.diagnostics.error("elf: %@ executables cannot be generated by the local backend"_v,
                                  nullptr, archName(arch));
        return false;
    }

    auto codeOffset = elfCodeOffset();

    /*
     * What the image promised.
     *
     * Checked unconditionally rather than asserted, even though none of it is influenced by the
     * program being compiled: what a wrong answer produces here is a *file* - one the kernel maps
     * with the wrong protections on the wrong pages, and whose symptom is a fault somewhere inside
     * the program rather than anything pointing at the layout. A debug-only check would be a check
     * that is absent from every build anyone runs.
     */
    if(image.dataOffset > image.size || image.codeSize > image.dataOffset ||
       (image.dataOffset != image.size && (codeOffset + image.dataOffset) % kElfPageSize != 0)) {
        context.diagnostics.error("elf: the generated image is not laid out for a segmented file - code %@, data at %@ of %@"_v,
                                  nullptr, U64(image.codeSize), U64(image.dataOffset), U64(image.size));
        return false;
    }

    auto dataSize = U64(image.size) - image.dataOffset;
    auto dataOffset = U64(codeOffset) + image.dataOffset;
    auto textEnd = U64(codeOffset) + image.codeSize;

    /*
     * The tables that name things, built before anything is written because their sizes decide where
     * everything after the image goes.
     *
     * `.strtab` opens with the empty string so that a symbol with no name refers to offset 0, and
     * `.shstrtab` does the same for the null section header - which is the one section that is
     * required to exist and required to be empty.
     */
    Net::BufferWriter names(256);
    addString(names, ""_v);
    auto nameText = addString(names, ".text"_v);
    auto nameData = addString(names, ".data"_v);
    auto nameSymtab = addString(names, ".symtab"_v);
    auto nameStrtab = addString(names, ".strtab"_v);
    auto nameStringNames = addString(names, ".shstrtab"_v);

    Net::BufferWriter strings(1024);
    addString(strings, ""_v);

    Net::BufferWriter symbols(1024 + image.symbols.size() * kSymbolSize);
    writeSymbol(symbols, 0, 0, 0, 0, 0); // the reserved null symbol every table opens with

    for(auto& symbol: image.symbols) {
        auto name = addString(strings, stringView(symbol.name));
        auto type = symbol.function ? kSymbolTypeFunc : kSymbolTypeObject;
        auto section = symbol.function ? kSectionText : kSectionData;

        writeSymbol(symbols, name, U8(kSymbolBindLocal << 4 | type), section,
                    elfCodeAddress() + symbol.offset, symbol.size);
    }

    // Where everything past the loaded image goes. The symbol table is eight-byte aligned because
    // its entries hold 64-bit fields, and the section header table for the same reason.
    auto tablesOffset = alignUp(U64(codeOffset) + image.size, 8);
    auto symtabOffset = tablesOffset;
    auto strtabOffset = symtabOffset + symbols.offset();
    auto shstrtabOffset = strtabOffset + strings.offset();
    auto sectionsOffset = alignUp(shstrtabOffset + names.offset(), 8);
    auto fileSize = sectionsOffset + U64(kSectionCount) * kSectionHeaderSize;

    Net::BufferWriter out { Size(fileSize) };

    /*
     * The header.
     */
    writeIdent(out);
    out.writeShort<LittleEndian>(kElfTypeExec);
    out.writeShort<LittleEndian>(machine.unwrap());
    out.writeInt<LittleEndian>(kElfVersion);
    out.writeLong<LittleEndian>(elfCodeAddress() + image.entryOffset);
    out.writeLong<LittleEndian>(kElfHeaderSize);  // e_phoff: the program headers follow the header
    out.writeLong<LittleEndian>(sectionsOffset);
    out.writeInt<LittleEndian>(0);                // e_flags: none are defined for amd64
    out.writeShort<LittleEndian>(kElfHeaderSize);
    out.writeShort<LittleEndian>(kProgramHeaderSize);
    out.writeShort<LittleEndian>(kProgramHeaderCount);
    out.writeShort<LittleEndian>(kSectionHeaderSize);
    out.writeShort<LittleEndian>(kSectionCount);
    out.writeShort<LittleEndian>(kSectionStringNames);

    /*
     * The segments.
     *
     * The first one starts at file offset zero and so maps the headers along with the code, which is
     * what makes every file offset in the image equal to its address minus kElfLoadAddress. That is
     * not a convenience: the rip-relative displacements inside the code were resolved over the image
     * as one run of bytes, so the two halves have to keep the distance they were assembled at, and
     * the only way to give them different protections is to have the data begin on a page of its own.
     *
     * A program with no data at all gets a header that loads nothing rather than one fewer header,
     * because the count is what fixes elfCodeOffset() - and that is what the image's absolute
     * addresses were computed against, before anything was known about how much data there would be.
     */
    writeProgramHeader(out, kProgramTypeLoad, kProgramFlagRead | kProgramFlagExecute,
                       0, kElfLoadAddress, textEnd, textEnd, kElfPageSize);

    if(dataSize) {
        /*
         * The data segment is mapped a vector's width longer than the file holds - the static half
         * of the tail-read guarantee, Implementation-Vector.md §8.3.
         *
         * A global is storage the language allocated, so a vector loop over one may read up to a
         * register past its end. Inside the segment that is free, since something else follows; the
         * case this closes is a global that ends at the segment's last mapped byte, which is rare
         * and faults silently when it happens. It costs nothing in the file: the memory size of a
         * segment may exceed its file size, and the kernel zero-fills the difference, which is what
         * makes a `.bss` free as well.
         */
        writeProgramHeader(out, kProgramTypeLoad, kProgramFlagRead | kProgramFlagWrite,
                           dataOffset, kElfLoadAddress + dataOffset, dataSize,
                           dataSize + kMaxVectorBytes, kElfPageSize);
    } else {
        writeProgramHeader(out, kProgramTypeNull, 0, 0, 0, 0, 0, 0);
    }

    writeProgramHeader(out, kProgramTypeGnuStack, kProgramFlagRead | kProgramFlagWrite,
                       0, 0, 0, 0, 16);

    /*
     * The image.
     *
     * Padded up to elfCodeOffset() first - the program headers are not a multiple of sixteen bytes,
     * and the image's own alignment is relative to its first byte.
     */
    while(out.offset() < codeOffset) out.writeByte(0);
    out.writeBytes(image.bytes, image.size);

    /*
     * The tables, and the sections that describe them.
     *
     * None of this is loaded: an executable that has already had every address written into it needs
     * no symbol table to run. It is here so that a disassembler can name what it is looking at - see
     * ElfSymbol on why every entry is local.
     */
    while(out.offset() < symtabOffset) out.writeByte(0);
    out.writeBytes(symbols.buffer, symbols.offset());
    out.writeBytes(strings.buffer, strings.offset());
    out.writeBytes(names.buffer, names.offset());

    while(out.offset() < sectionsOffset) out.writeByte(0);

    writeSectionHeader(out, 0, kSectionTypeNull, 0, 0, 0, 0, 0, 0, 0, 0);

    writeSectionHeader(out, nameText, kSectionTypeProgBits, kSectionFlagAlloc | kSectionFlagExecute,
                       elfCodeAddress(), codeOffset, image.codeSize, 0, 0, 16, 0);

    writeSectionHeader(out, nameData, kSectionTypeProgBits, kSectionFlagAlloc | kSectionFlagWrite,
                       dataSize ? kElfLoadAddress + dataOffset : 0, dataOffset, dataSize, 0, 0, 16, 0);

    // sh_link is the string table the symbol names are in, and sh_info is the index of the first
    // non-local symbol - which is every symbol here, since all of them are local, so it is the count.
    writeSectionHeader(out, nameSymtab, kSectionTypeSymtab, 0, 0, symtabOffset, symbols.offset(),
                       kSectionStrtab, U32(image.symbols.size() + 1), 8, kSymbolSize);

    writeSectionHeader(out, nameStrtab, kSectionTypeStrtab, 0, 0, strtabOffset, strings.offset(),
                       0, 0, 1, 0);

    writeSectionHeader(out, nameStringNames, kSectionTypeStrtab, 0, 0, shstrtabOffset, names.offset(),
                       0, 0, 1, 0);

    assertTrue(out.offset() == fileSize); // the layout above and the writing below disagree

    // Created executable, which is the one thing about this file that is not in its contents. The
    // mode is applied when the file is created, so a rebuild over an executable keeps it.
    auto opened = File::openFile(path, writeAccess(), File::CreateAlways, File::Executable);
    if(opened.isErr()) {
        context.diagnostics.error("cannot write %@: %@"_v, nullptr, path,
                                  describeError(opened.unwrapErr()));
        return false;
    }

    // Looped, because a write may be short: the failure that would otherwise produce is a truncated
    // executable that was reported as having been written, and an ELF file missing its tail is a
    // file the kernel will happily start and then fault in.
    auto file = opened.moveUnwrapOk();

    for(Size at = 0; at < out.offset();) {
        auto written = file.write({ out.buffer + at, out.offset() - at });

        if(written.isErr()) {
            context.diagnostics.error("cannot write %@: %@"_v, nullptr, path,
                                      describeError(written.unwrapErr()));
            return false;
        }

        if(!written.unwrapOk()) {
            context.diagnostics.error("cannot write %@: the file stopped accepting data after %@ of %@ bytes"_v,
                                      nullptr, path, U64(at), U64(out.offset()));
            return false;
        }

        at += written.unwrapOk();
    }

    return true;
}
