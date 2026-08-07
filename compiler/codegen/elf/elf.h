#pragma once

#include "../../compiler/context.h"
#include "../../compiler/settings.h"

/*
 * The ELF64 container.
 *
 * What a code generator produces is a run of bytes and a set of offsets into it; what an operating
 * system will start is a file that says where those bytes go and which of them may be executed.
 * This is the second half of that, and it knows nothing about the first: it takes a finished image
 * - code, then data, then the offsets of the symbols inside both - and writes the file a Linux
 * kernel will map. Nothing here reads the IR, the target registers or a machine instruction.
 *
 * It is a directory of its own rather than part of codegen/x64 because the format is not the
 * architecture's: the same writer produces an executable for any backend that can hand over a flat
 * image, and the only architecture-shaped thing in the file is one number in the header. Mach-O and
 * PE, when they arrive, are files beside this one consuming the same ElfImage-shaped description.
 *
 * **What it does not do.** There is no dynamic linking, no interpreter, no relocation section and
 * no import table. That is not a simplification - it is what this target *is*: the runtime is
 * written in Yana over raw system calls (see Native and Native.Linux in resolve/native.cpp), so a
 * finished program references nothing outside its own image and there is nothing left for a loader
 * to resolve. Every address a program needed to know was already written into it, which is the
 * whole reason a static non-PIE executable is enough here and not merely convenient.
 */

/*
 * Where the file is mapped, and what that makes the address of its first byte.
 *
 * A fixed load address rather than a chosen one, because the image is not position-independent: the
 * addresses inside constant data - a witness table holding a function pointer - are written into the
 * file as absolute 64-bit words by whoever built the image, and a word that has already been written
 * cannot also be a relocation the loader applies. `elfCodeAddress()` is what those words were
 * computed against, so an image built for one load address may not be written at another.
 *
 * 0x400000 is the traditional amd64 ET_EXEC base: high enough to leave the first page unmapped, so a
 * null dereference still faults, and low enough to stay in the 32-bit address range that a rel32
 * call and a `mov eax, imm32` address can reach.
 */
static constexpr U64 kElfLoadAddress = 0x400000;

// The mapping granularity the segments are laid out against. Stated rather than asked of the host,
// because this describes the file being *written* - a compiler running on a machine with 16 KiB
// pages still produces a file for a kernel with 4 KiB ones.
static constexpr U32 kElfPageSize = 4096;

// The file offset the image's first byte lands at: the ELF header and the program headers, rounded
// up so that an image whose own offsets assume 16-byte alignment keeps it. Every offset inside an
// image is this much less than the file offset it ends up at, and exactly this much less than the
// address, which is what lets a rel32 computed over the image stay correct in the file.
U32 elfCodeOffset();

// The address the image's first byte is mapped at. Whoever fills in an image's absolute addresses
// has to use this - see kElfLoadAddress.
inline U64 elfCodeAddress() { return kElfLoadAddress + elfCodeOffset(); }

/*
 * The bytes of padding an image must place between its code and its data.
 *
 * The two halves get separate segments so that nothing writable is executable and nothing
 * executable is writable, and a segment's protection applies to whole pages - so the data has to
 * begin on a page of its own. The padding is inserted by the image's builder rather than here
 * because it lands *inside* the image: every offset the builder recorded, and every relative
 * displacement it has already resolved, is an offset into that one run of bytes.
 *
 * `codeSize` is the image size at the point the code ends. Zero when the data would begin on a page
 * boundary anyway.
 */
U32 elfDataPadding(U32 codeSize);

/*
 * One name in the emitted symbol table.
 *
 * Nothing links against these and nothing can call them: this target has no stable ABI and no
 * foreign exports, so an executable's symbols name no export surface. They exist so that a
 * disassembler or a debugger can say `Native.allocateHeap` where it would otherwise print a bare
 * address, which is why every one of them is emitted as STB_LOCAL - a local symbol is a statement
 * about what is at an address and not a claim that anything may reach it. Exported symbols are a
 * different table entirely (`.dynsym`, reached through `PT_DYNAMIC`) and belong to the shared
 * library mode, which is not implemented.
 */
struct ElfSymbol {
    String name;

    // Offset into the image, not a file offset and not an address.
    U32 offset = 0;

    // Bytes the symbol covers, or 0 where the builder does not know. A debugger uses this to decide
    // whether an address is *inside* a function or merely after it.
    U32 size = 0;

    // Whether this names code or data, which is the section it is placed in as well as its type.
    bool function = false;
};

/*
 * A finished program, as the bytes and the offsets into them.
 *
 * One buffer rather than two, because the two halves are not independent: the code addresses its
 * globals with rip-relative displacements computed over this exact layout, so code and data have to
 * keep the distance they were assembled at. The segments are cut out of it at `dataOffset`, which
 * is why that offset has to be one `elfDataPadding` produced.
 */
struct ElfImage {
    const Byte* bytes = nullptr;
    Size size = 0;

    // Where the code ends. Not the same as `dataOffset`: the padding between them belongs to
    // neither, and calling it code would put bytes in `.text` that are not instructions.
    U32 codeSize = 0;

    // Where the data begins, which must be a page boundary once `elfCodeOffset()` is added - see
    // elfDataPadding. Equal to `size` for a program with no data at all.
    U32 dataOffset = 0;

    // The image offset of the process entry point, which is `_start` rather than the program's own
    // entry: the kernel jumps here with no return address on the stack and no way for the function
    // it reaches to return anywhere.
    U32 entryOffset = 0;

    Array<ElfSymbol> symbols;
};

// Writes `image` to `path` as an executable ELF64 file, reporting through `context.diagnostics` and
// returning false if it could not. `arch` selects the machine the header names; only X64 is
// supported, and anything else is reported rather than guessed at.
bool writeElfExecutable(Context& context, const ElfImage& image, TargetArch arch, const String& path);
