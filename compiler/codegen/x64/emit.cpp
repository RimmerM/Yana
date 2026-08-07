#include "emit.h"
#include "../elf/elf.h"

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

// The system call that ends a process, and the register the local conventions return in - see
// kComplexResults in constraint.cpp. Both are amd64 Linux facts rather than choices.
static constexpr U32 kSysExitGroup = 231;

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
 *   and rsp, -16        the kernel hands over an aligned stack; this keeps it aligned if it did not
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
static U32 genProcessEntry(AsmModule& to, LowerFunction& entry) {
    auto& out = to.buffer;

    // Aligned, and padded with a trapping byte rather than a zero: `add [rax], al` is what a run of
    // zeroes decodes to, and reaching padding should stop rather than corrupt memory.
    while(out.offset() & 15) out.writeByte(0xcc);
    auto start = U32(out.offset());

    out.writeByte(0x31); out.writeByte(0xed);                 // xor ebp, ebp
    out.writeByte(0x48); out.writeByte(0x83);
    out.writeByte(0xe4); out.writeByte(0xf0);                 // and rsp, -16

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

    auto startOffset = genProcessEntry(assembly, *entry);
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
