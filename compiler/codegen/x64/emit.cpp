#include "emit.h"
#include "../elf/elf.h"
#include "../entry.h"

/*
 * From a lowered module to a file that runs.
 *
 * The order here is the whole of the layout, and each step depends on the one before it:
 *
 *   1. every function, in module order, through the per-function pipeline
 *   2. the process entry point, which calls the module's own entry and then stops the process
 *   3. padding, so that the data begins on a page of its own
 *   4. every global
 *   5. the relocations between them, now that every symbol has an offset
 *   6. the absolute addresses inside constant data, now that the image has an address
 *
 * Steps 1 and 2 are before 4 because code and data must not interleave: they end up in segments with
 * different permissions, and a global between two functions would be a writable page inside the
 * executable one. Step 5 is after all four because a call may target a function not yet emitted when
 * it was written, and step 6 is last because it is the only one that needs to know where the image
 * will be mapped.
 */

/*
 * The command line, out of the stack the kernel handed over - Design-Test.md §11.2's F4.
 *
 * A fresh process is given its arguments *on the stack* and nowhere else: `[rsp]` is the count, the
 * vector begins at `rsp+8` and is terminated by a null, and the environment follows it. That layout
 * lasts exactly as long as the stack pointer does, so this runs before the entry stub touches it and
 * leaves the three globals holding what a program can go on to read at any point in its life.
 *
 *   mov rcx, [rsp]              argc
 *   lea rax, [rsp+8]            argv
 *   lea rax, [rsp+rcx*8+16]     envp - past the vector and its null terminator
 *
 * Which globals those are is `@builtin` - compiler/compiler/builtin.h - and not a name written here:
 * the library says which of its declarations fills each role, resolve records it and lowering hands
 * it over. Only the ones this program actually reaches are written, which is what keeps a binary
 * that never asks about its arguments exactly the size it was. The LLVM path takes the same three
 * facts off `main`'s parameters instead - see addNativeEntry.
 */

// `mov [rip+disp32], reg` for one of the two registers this stub stores from. The relocation is the
// ordinary global one, so the displacement is patched by the same pass that patches every other.
static void storeToGlobal(AsmModule& to, LowerGlobal* global, U8 modrm) {
    if(!global) return;

    to.buffer.writeByte(0x48); to.buffer.writeByte(0x89); to.buffer.writeByte(modrm);
    to.addRelocation(global);
}

static void genCommandLine(AsmModule& to, LowerBase base, LowerModule& module) {
    auto count = module.builtin(base, Builtin::commandLineCount);
    auto values = module.builtin(base, Builtin::commandLineValues);
    auto environment = module.builtin(base, Builtin::commandLineEnvironment);

    auto& out = to.buffer;

    /*
     * The count, where anything wants it - and the environment is one of those things, since where
     * the vector ends is what says where the environment begins.
     *
     * Each of the three is written only where this program reaches that global, and none of them
     * implies another: a program that asks only for `argv` gets two instructions and does not load a
     * count nothing will read.
     */
    if(count || environment) {
        out.writeByte(0x48); out.writeByte(0x8b);
        out.writeByte(0x0c); out.writeByte(0x24);             // mov rcx, [rsp]

        storeToGlobal(to, count, 0x0d);                       // mov [rip+d], rcx
    }

    if(values) {
        out.writeByte(0x48); out.writeByte(0x8d); out.writeByte(0x44);
        out.writeByte(0x24); out.writeByte(0x08);             // lea rax, [rsp+8]
        storeToGlobal(to, values, 0x05);                      // mov [rip+d], rax
    }

    if(environment) {
        out.writeByte(0x48); out.writeByte(0x8d); out.writeByte(0x44);
        out.writeByte(0xcc); out.writeByte(0x10);             // lea rax, [rsp+rcx*8+16]
        storeToGlobal(to, environment, 0x05);                 // mov [rip+d], rax
    }
}

/*
 * The process entry point.
 *
 * Written as bytes rather than built as a LowerFunction and put through the pipeline, because it is
 * not a function: nothing calls it, it has no caller's convention to honour, it has no return
 * address on the stack to return to, and its last instruction is a system call that does not come
 * back. Eleven of those properties are exactly what the IR has no way to say, so saying it in the
 * IR would mean teaching every pass about a shape only this one thing has.
 *
 * What it does:
 *
 *   xor ebp, ebp        the outermost frame has no caller - a stack walk stops here
 *   <the command line>  read off the stack while it is still the kernel's - genCommandLine
 *   and rsp, -64        aligned to a vector, which is the tail-read guarantee as well - see below
 *   call <entry>        the program, under its own convention
 *   mov edi, eax        the status it answered, which the convention returns in rax
 *   mov eax, 231        __NR_exit_group
 *   syscall
 *   hlt                 unreachable; a fault is better than running into the data behind it
 *
 * The status is what the program answered, narrowed by the kernel to the low eight bits a process
 * can report - the same thing C says about `main`. A program that answers nothing exits zero, which
 * is why the status is cleared rather than read out of a register the entry never wrote.
 */
static U32 genProcessEntry(AsmModule& to, LowerBase base, LowerModule& module, LowerFunction& entry) {
    auto& out = to.buffer;

    // Aligned, and padded with a trapping byte rather than a zero: `add [rax], al` is what a run of
    // zeroes decodes to, and reaching padding should stop rather than corrupt memory.
    while(out.offset() & 15) out.writeByte(0xcc);
    auto start = U32(out.offset());

    out.writeByte(0x31); out.writeByte(0xed);                 // xor ebp, ebp

    // First, because it reads a layout that only exists while `rsp` is the one the kernel set.
    genCommandLine(to, base, module);

    /*
     * The stack, aligned to a vector - which is its half of the tail-read guarantee as well,
     * Implementation-Vector.md §8.2.
     *
     * A local may be read up to a vector's width past its end, and inside the call graph that costs
     * nothing: every frame has its caller's above it, so the bytes past the highest local are the
     * caller's own. The outermost frame is the one with no caller - and what sits above *it* is the
     * block the kernel wrote there before the process began: the count, the two vectors, the
     * auxiliary vector and the strings they point into, which is hundreds of mapped bytes and is
     * exactly the property the frames below inherit. So there is nothing to reserve here; rounding
     * `rsp` *down* is the whole of what the outermost frame needs.
     *
     * `kEntryStackAlignment` rather than the target's own vector width, and it is in
     * `codegen/entry.h` rather than here because the LLVM path's own entry writes the same number -
     * see the assembly in addNativeEntry. The argument for the value is on the constant.
     */
    out.writeByte(0x48); out.writeByte(0x83);
    out.writeByte(0xe4); out.writeByte(Byte(-I32(kEntryStackAlignment))); // and rsp, -64

    // The relocation writes the placeholder displacement itself, and resolveRelocations patches it
    // once the entry's offset is known - which it already is, but going through the same mechanism
    // as every other call is what keeps there being one rule for what a rel32 means.
    out.writeByte(0xe8);                                      // call rel32
    to.addRelocation(&entry);

    if(entry.returnTypes.size()) {
        out.writeByte(0x89); out.writeByte(0xc7);             // mov edi, eax
    } else {
        out.writeByte(0x31); out.writeByte(0xff);             // xor edi, edi
    }

    out.writeByte(0xb8);
    out.writeInt<LittleEndian>(kSysExitGroup);                // mov eax, 231
    out.writeByte(0x0f); out.writeByte(0x05);                 // syscall
    out.writeByte(0xf4);                                      // hlt

    return start;
}

/*
 * The module's own entry, checked for the two things this path cannot express.
 *
 * Reported here rather than asserted, because both are properties of the program being compiled: a
 * `main` taking arguments is something a user can write, and the entry sequence above has nothing to
 * pass it. The same two questions are asked by the LLVM path in addNativeEntry, and for the same
 * reasons - the answers have to agree, since it is the same program either way.
 */
static LowerFunction* findEntry(Context& context, LowerBase base, LowerModule& module) {
    if(!module.entry) {
        context.diagnostics.error("the program has no entry point - the module being compiled declares neither `main` nor any top-level statement"_v,
                                  nullptr);
        return nullptr;
    }

    auto found = module.functions.get(module.entry);
    if(!found) {
        context.diagnostics.error("amd64: the program has no entry point %@"_v, nullptr,
                                  context.findName(module.entry));
        return nullptr;
    }

    auto entry = base[found.unwrap()];

    if(entry->args.size()) {
        context.diagnostics.error("amd64: the entry point %@ cannot take arguments yet"_v, nullptr,
                                  context.findName(module.entry));
        return nullptr;
    }

    // One result, in a register the process exit status can be read out of. More than one is a
    // shape no `main` has, and a floating one is a status nothing could mean.
    if(entry->returnTypes.size() > 1 || (entry->returnTypes.size() == 1 &&
                                         !isIntLike((LowerType)entry->returnTypes.get(base, 0)))) {
        context.diagnostics.error("amd64: the entry point must return an integer"_v, nullptr);
        return nullptr;
    }

    return entry;
}

bool genX64Executable(Context& context, LowerModule& module, const String& path) {
    auto base = *module.arena;

    auto entry = findEntry(context, base, module);
    if(!entry) return false;

    // Built into the image's own symbol list as the emission goes, rather than gathered afterwards
    // out of the offset maps: a symbol's *extent* is only knowable at the moment its last byte is
    // written, and nothing recorded here records it.
    ElfImage image;
    AsmModule assembly;

    /*
     * One of each across the whole module: the allocator writes into them rather than building its
     * own, so allocating a module costs the largest function's storage instead of the sum of every
     * function's. The records each function's `registers` points into are consumed by genFunction
     * before the next one is allocated, so the arena holding them is emptied in step - see
     * RegScratch::resetRecords.
     */
    RegScratch scratch;
    FunctionRegs registers;
    MachineFunction machine;

    for(auto functionPointer: module.functionOrder) {
        auto function = base[functionPointer];

        machine.reset();
        transformFunction(context, base, *function, machine);

        scratch.resetRecords();
        allocateRegisters(context, base, *function, machine, scratch, registers);
        genFunction(context, base, assembly, *function, machine, registers);

        // After generating it, since that is when both ends are known. The recorded start is the
        // entry point rather than the first byte emitted: a function with prefix data is preceded by
        // it, and a debugger asking what is at the entry means the code.
        auto start = assembly.functionOffsets.getValue(function);
        assertTrue(start.isJust());

        image.symbols.push(ElfSymbol {
            .name = context.findName(function->name),
            .offset = start.unwrap(),
            .size = U32(assembly.buffer.offset()) - start.unwrap(),
            .function = true,
        });
    }

    // Before anything is laid out, because a function that could not be generated has left the
    // buffer holding whatever it managed to emit - and a file built out of that would be a program
    // rather than a diagnostic. transformFunction reports the cases it cannot build a frame for.
    if(context.diagnostics.errorCount()) return false;

    auto startOffset = genProcessEntry(assembly, base, module, *entry);
    image.symbols.push(ElfSymbol { .name = String("_start"), .offset = startOffset,
                                   .size = U32(assembly.buffer.offset()) - startOffset, .function = true });

    auto codeSize = U32(assembly.buffer.offset());

    /*
     * The gap between the two halves.
     *
     * Only when there is data to separate: padding a program with no globals out to a page boundary
     * would add a page to the file to describe nothing. The bytes are the same trap the alignment
     * padding above uses, since they are still inside the executable segment.
     */
    if(module.globalOrder.size()) {
        auto padding = elfDataPadding(codeSize);
        for(U32 i = 0; i < padding; i++) assembly.buffer.writeByte(0xcc);
    }

    auto dataOffset = U32(assembly.buffer.offset());

    for(auto globalPointer: module.globalOrder) {
        auto global = base[globalPointer];
        assembly.addGlobal(base, global);

        auto at = assembly.globalOffsets.getValue(global);
        assertTrue(at.isJust());

        image.symbols.push(ElfSymbol {
            .name = context.findName(global->name),
            .offset = at.unwrap(),
            .size = U32(global->initialContents.size()),
            .function = false,
        });
    }

    assembly.resolveRelocations(module.imageAnchor ? base[module.imageAnchor] : nullptr);

    // The last thing to touch the buffer: the addresses inside constant data - a witness table
    // holding a function pointer - are only knowable once the image has an address, and this image
    // has one because it is not position-independent. A dynamically linked program would emit these
    // as loader relocations instead; there is no loader here to emit them to.
    assembly.applyDataRelocations((Byte*)elfCodeAddress());

    image.bytes = assembly.buffer.buffer;
    image.size = assembly.buffer.offset();
    image.codeSize = codeSize;
    image.dataOffset = dataOffset;
    image.entryOffset = startOffset;

    return writeElfExecutable(context, image, context.settings.arch, path);
}
