#include "machine_internal.h"

/*
 * The rest of the machine: the flags, the stack, and memory.
 *
 * Select and alloca, the loads and stores, the in-place memory updates, the block operations, the
 * calls and the terminators - everything that is not arithmetic on a value already in a register.
 */

void MachineFormBuilder::registerMemoryAndControlForms() {
    /*
     * Select.
     */

    {
        // CMOVcc r, r/m: the destination doubles as a source, since it keeps its own value when the
        // condition does not hold. The flags were set by a comparison this select consumed.
        auto& form = add(FormSelectFlags, OpSelect, "cmovcc r, r"_v);
        form.uses.push(anyReg());
        form.uses.push(anyReg());
        form.uses.push(folded()); // the condition was consumed by the comparison that set the flags
        form.defs.push(tiedDef(0));
        form.flagsEffect = FlagsEffect::Use;

        // `select` yields its first operand when the condition holds and its second otherwise. The
        // tie has already put the first in the destination, so the move that remains is the second
        // one - which is why the condition the encoding carries is the negated one.
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Conditional,
            .opcode = 0x40, .escape = 0x0f,
            .regField = defRef(0), .rmField = useRef(1),
            .negateCondition = true,
        };
    }

    {
        // The condition arrived in a register instead, so it is tested first - and that test writes
        // the flags, which is why this form and the one above disagree about them.
        auto& form = add(FormSelectReg, OpSelect, "test r, r; cmovcc r, r"_v);
        form.uses.push(anyReg());
        form.uses.push(anyReg());
        form.uses.push(anyReg(ClassGpr32));
        form.defs.push(tiedDef(0));
        form.flagsEffect = FlagsEffect::UseDef;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Conditional,
            .opcode = 0x40, .escape = 0x0f,
            .regField = defRef(0), .rmField = useRef(1),
            .prelude = EncodingPrelude::TestLastUse,
            .negateCondition = true,
        };
    }

    /*
     * Floating-point select.
     *
     * There is no CMOVcc for a vector register, so the conditional move is a conditional *branch*
     * over an unconditional one: the tie has already put the first operand in the destination, so
     * what is left is to skip the copy of the second when the condition holds. Two instructions and
     * a forward jump of known length, against the shuffle-and-blend sequence the alternative would
     * be - which would need a mask in a third vector register and SSE4.1 besides.
     *
     * The copy is MOVAPS rather than MOVSS/MOVSD: it moves the whole register, so one form serves
     * both widths, and it needs no prefix to say which.
     */

    auto floatSelect = [&](MachineFormId id, StringView formName, RegisterClassId cls, bool testCondition) {
        auto& form = add(id, OpSelect, formName);
        form.uses.push(anyReg(cls));
        form.uses.push(anyReg(cls));

        // As for the integer select: a condition already in the flags was consumed by the
        // comparison that set them, and one still in a register is tested here.
        if(testCondition) form.uses.push(anyReg(ClassGpr32));
        else form.uses.push(folded());

        form.defs.push(tiedDef(0, cls));
        form.flagsEffect = testCondition ? FlagsEffect::UseDef : FlagsEffect::Use;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::FloatSelect,
            .opcode = 0x28, .escape = 0x0f,
            .regField = defRef(0), .rmField = useRef(1),
            .width = OperationWidth::Fixed32,
            .prelude = testCondition ? EncodingPrelude::TestLastUse : EncodingPrelude::None,
        };
    };

    floatSelect(FormSelectFloat32Flags, "jcc over; movaps xmm, xmm"_v, ClassFloat32, false);
    floatSelect(FormSelectFloat64Flags, "jcc over; movaps xmm, xmm"_v, ClassFloat64, false);
    floatSelect(FormSelectFloat32Reg, "test r, r; jcc over; movaps xmm, xmm"_v, ClassFloat32, true);
    floatSelect(FormSelectFloat64Reg, "test r, r; jcc over; movaps xmm, xmm"_v, ClassFloat64, true);

    /*
     * Stack allocation.
     *
     * A compile-time size becomes a frame object and one `lea`, which leaves the flags alone; a size
     * only known at run time has to round itself up and move the stack pointer, which does not. So
     * the two forms disagree about the flags, and OpAlloca says so.
     */

    {
        auto& form = add(FormAllocaFixed, OpAlloca, "lea r, [frame]"_v);
        form.uses.push(immediate(ImmediateWidth::Imm64));
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::AllocaFixed,
        };
    }

    {
        auto& form = add(FormAllocaDynamic, OpAlloca, "sub rsp, r"_v);
        form.uses.push(anyReg());
        form.defs.push(def());
        form.flagsEffect = FlagsEffect::Clobber;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::AllocaDynamic,
        };
    }

    /*
     * Memory access.
     *
     * One form per access width, because the width decides the bytes: a narrow load has to extend
     * into the whole destination register rather than merge with what it held, which is a different
     * opcode rather than a different operand size. A store only writes the bytes it names, so it
     * needs nothing but the right size - and, at one byte, the REX prefix that names spl/bpl/sil/dil
     * rather than ah/ch/dh/bh.
     */

    auto load = [&](MachineFormId id, StringView formName, U8 opcode, U8 escape, OperationWidth width) {
        auto& form = add(id, OpLoad, formName);
        form.uses.push(address());
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = opcode, .escape = escape,
            .regField = defRef(0),
            .width = width,
        };
    };

    // The narrow loads take their operand size from the *result*, since that is the register the
    // extension has to fill; the wider ones are the width they load.
    load(FormLoad8, "movzx r, byte [address]"_v, 0xb6, 0x0f, OperationWidth::FromResult);
    load(FormLoad8S, "movsx r, byte [address]"_v, 0xbe, 0x0f, OperationWidth::FromResult);
    load(FormLoad16, "movzx r, word [address]"_v, 0xb7, 0x0f, OperationWidth::FromResult);
    load(FormLoad16S, "movsx r, word [address]"_v, 0xbf, 0x0f, OperationWidth::FromResult);
    load(FormLoad32, "mov r32, [address]"_v, 0x8b, 0, OperationWidth::Fixed32);
    load(FormLoad32S, "movsxd r64, [address]"_v, 0x63, 0, OperationWidth::Fixed64);
    load(FormLoad64, "mov r64, [address]"_v, 0x8b, 0, OperationWidth::Fixed64);

    // The float loads say their width in the prefix like every other SSE form, so there is one per
    // width rather than one per (width, signedness): a float is never sign- or zero-extended by
    // being loaded, and a narrower one is a different type rather than a narrower access.
    auto floatLoad = [&](MachineFormId id, StringView formName, U8 prefix, RegisterClassId cls) {
        auto& form = add(id, OpLoad, formName);
        form.uses.push(address());
        form.defs.push(def(cls));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = 0x10, .escape = 0x0f, .prefix = prefix,
            .regField = defRef(0),
            .widthInPrefix = true,
        };
    };

    floatLoad(FormLoadF32, "movss xmm, [address]"_v, 0xf3, ClassFloat32);
    floatLoad(FormLoadF64, "movsd xmm, [address]"_v, 0xf2, ClassFloat64);

    auto store = [&](MachineFormId id, StringView formName, U8 opcode, OperationWidth width) {
        auto& form = add(id, OpStore, formName);
        form.uses.push(address());
        form.uses.push(anyReg());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = opcode,
            .regField = useRef(1),
            .width = width,
        };

        return &form;
    };

    store(FormStore8, "mov byte [address], r"_v, 0x88, OperationWidth::Fixed32)->encoding.byteRegField = true;
    store(FormStore16, "mov word [address], r"_v, 0x89, OperationWidth::Fixed32)->encoding.prefix = 0x66;
    store(FormStore32, "mov dword [address], r"_v, 0x89, OperationWidth::Fixed32);
    store(FormStore64, "mov qword [address], r"_v, 0x89, OperationWidth::Fixed64);

    /*
     * The same, with the value in the encoding rather than in a register.
     *
     * `mov [address], imm` has no second register at all, so ModRM.reg carries the opcode extension
     * /0 instead - which is the case `emitLoadStore` was already written for. What this removes is
     * not only the `mov $imm, r` above the store: it removes the operand, so the constant never
     * enters allocation and never competes for a register. `each` in test/bench/programs/Pipeline.yana
     * is the shape that makes the difference visible - two callee-saved registers were being pushed
     * and popped there to hold the constants 1 and 2 across nothing at all, because a store demanded
     * a register for them and every other register was taken.
     *
     * All four declare **Imm32** rather than a width of their own, and that is what makes the
     * selection below total. `canEmbedImm` in transform_peephole.cpp decides whether to embed a constant
     * before any form has been chosen, and it asks by opcode - so it answers for the widest form the
     * opcode has. A narrow form that declared a narrower immediate would be refusing values that
     * question had already accepted, and an operand that has been taken out of allocation has no
     * register left to fall back to. Truncating is not a compromise here: the store discards the
     * upper bytes whatever carries them, so writing the low ones is what the register form does too.
     * The 64-bit form is the only one the width genuinely constrains - its immediate is sign-extended
     * rather than truncated - and Imm32 is exactly that constraint.
     */
    auto storeImm = [&](MachineFormId id, StringView formName, U8 opcode, U8 immediateBytes,
                        U8 prefix, OperationWidth width)
    {
        auto& form = add(id, OpStore, formName);
        form.uses.push(address());
        form.uses.push(immediate(ImmediateWidth::Imm32));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = opcode,
            .prefix = prefix,
            .extension = 0,
            .immField = useRef(1),
            .width = width,
            .immediateBytes = immediateBytes,
        };
    };

    storeImm(FormStore8Imm, "mov byte [address], imm8"_v, 0xc6, 1, 0, OperationWidth::Fixed32);
    storeImm(FormStore16Imm, "mov word [address], imm16"_v, 0xc7, 2, 0x66, OperationWidth::Fixed32);
    storeImm(FormStore32Imm, "mov dword [address], imm32"_v, 0xc7, 4, 0, OperationWidth::Fixed32);
    storeImm(FormStore64Imm, "mov qword [address], imm32"_v, 0xc7, 4, 0, OperationWidth::Fixed64);

    // A store has no result to take its width from, so it states it - and states it as the width of
    // the value rather than of the address, which is what the prefix already says in bytes.
    auto floatStore = [&](MachineFormId id, StringView formName, U8 prefix, RegisterClassId cls,
                          OperationWidth width)
    {
        auto& form = add(id, OpStore, formName);
        form.uses.push(address());
        form.uses.push(anyReg(cls));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = 0x11, .escape = 0x0f, .prefix = prefix,
            .regField = useRef(1),
            .width = width,
            .widthInPrefix = true,
        };
    };

    floatStore(FormStoreF32, "movss [address], xmm"_v, 0xf3, ClassFloat32, OperationWidth::Fixed32);
    floatStore(FormStoreF64, "movsd [address], xmm"_v, 0xf2, ClassFloat64, OperationWidth::Fixed64);

    // The VEX transfers. Two operands rather than three: a `vmovss` that reads memory has no merge
    // source - it zeroes the rest of the register, which is what a load of a scalar wants - and a
    // store has no destination register to name.
    vexTwin(FormLoadF32Vex, FormLoadF32, "vmovss xmm, [address]"_v, false);
    vexTwin(FormLoadF64Vex, FormLoadF64, "vmovsd xmm, [address]"_v, false);
    vexTwin(FormStoreF32Vex, FormStoreF32, "vmovss [address], xmm"_v, false);
    vexTwin(FormStoreF64Vex, FormStoreF64, "vmovsd [address], xmm"_v, false);

    /*
     * `movbe` - the access that reverses the bytes on its way through, x86-64-v3.
     *
     * `0f 38 f0 /r` reads a location into a register byte-reversed and `0f 38 f1 /r` writes one the
     * same way; the width is the operand size, so the 16-bit rows are the 32-bit encoding under a
     * `66` prefix exactly as the ordinary `mov word` is.
     *
     * What these rows are for is the *register* they remove rather than the instruction: a value
     * read out of a binary format arrives already the right way round, with nothing holding the
     * unreversed one and no second instruction to reverse it. `selectByteSwapMemory` in
     * transform_address.cpp is what moves a `Load` and a `Bswap` onto one of these, and it is where
     * the feature is tested - a target below v3 keeps the two instructions it was given, which is
     * what `requiredFeatures` here would otherwise turn into an unencodable selection.
     *
     * **Two widths, not three.** The machine has a 16-bit `movbe` as well - the same encoding under a
     * `66` prefix - and there is no way to reach it from this IR: a 16-bit reversal is spent above
     * the lower IR, which has no scalar type narrower than `Int32` to carry one (see
     * `Value::ByteSwap` in resolve/inst.def). A row nobody can select is a row nothing tests, so the
     * width that has no producer has no form either, and adding one is part of carrying the narrow
     * reversal down rather than something already waiting for it.
     */
    auto movbe = [&](MachineFormId id, MachineOpcodeId opcode, StringView formName, U8 op,
                     U8 prefix, OperationWidth width, bool store)
    {
        auto& form = add(id, opcode, formName);
        form.uses.push(address());
        if(store) form.uses.push(anyReg());
        else form.defs.push(def());

        form.requiredFeatures = kFeatureMovbe;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = op, .escape = 0x0f, .prefix = prefix,
            .regField = store ? useRef(1) : defRef(0),
            .width = width,
        };

        form.encoding.opcodeMap = kOpcodeMap0F38;
    };

    movbe(FormMovbeLoad32, OpMovbeLoad, "movbe r32, [address]"_v, 0xf0, 0, OperationWidth::Fixed32, false);
    movbe(FormMovbeLoad64, OpMovbeLoad, "movbe r64, [address]"_v, 0xf0, 0, OperationWidth::Fixed64, false);

    movbe(FormMovbeStore32, OpMovbeStore, "movbe dword [address], r"_v, 0xf1, 0, OperationWidth::Fixed32, true);
    movbe(FormMovbeStore64, OpMovbeStore, "movbe qword [address], r"_v, 0xf1, 0, OperationWidth::Fixed64, true);

    /*
     * §45.2 The in-place memory updates - `add [rdi + rcx*4], edx`.
     *
     * Shaped exactly like the stores above and encoded by the same family: an `address()` operand,
     * the other operand in ModRM.reg or in the immediate, and *no result at all*. What the group-1
     * register forms already declare - a ReadWrite r/m operand with the result tied to it - is the
     * same operation reaching memory through a frame slot; this is the same operation reaching it
     * through an address the program computed, which is a location no operand can hold and so a form
     * rather than a location. `foldStoreUpdates` in transform_address.cpp is what moves an instruction here.
     *
     * The widths are the store's four. A narrower update is exact for all five of these operations
     * whatever the value's own width was, because each of them decides every bit of its result from
     * the bits of its operands at or below that position - so the low `w` bytes of a wide operation
     * are the `w`-byte operation, and the bytes above are the ones the store discarded anyway.
     *
     * The immediate forms exist at 32 and 64 bits, which is where the group-1 `imm8`/`imm32` pair
     * lives. Without them a fold would be a regression rather than a saving at the shape that wants
     * it most: `xs[i] += 1` reaches this with its constant already embeddable, and a register-only
     * form would put it back into a register and hold one across the loop for it. Which of the two
     * opcodes carries the value is decided by the value, in `emitLoadStore` - the same choice
     * `emitRmExtImm` makes for the register forms, made where an address is what it is written
     * around.
     */
    // Which operation this is, rather than where its forms start and what its opcode is: those two
    // are the same fact selection reads, and it is stated once - see StoreUpdateOp.
    auto storeUpdate = [&](LowerInst::Kind kind, const StringView (&names)[6],
                           U8 rmRegOp, U8 extension, bool logical)
    {
        auto& row = *storeUpdateOpFor(kind);
        auto first = row.firstForm;
        auto opcode = row.opcode;

        // The four widths, at the ids the list declares in order: byte, word, dword, qword.
        auto reg = [&](MachineFormId id, StringView formName, U8 op, U8 prefix,
                       OperationWidth width)
        {
            auto& form = add(id, opcode, formName);
            form.uses.push(address());
            form.uses.push(anyReg());
            form.flagsEffect = FlagsEffect::Def;
            form.resultInFlags = true;
            form.signInFlags = logical;
            form.encoding = EncodingDescriptor {
                .family = EncodingFamily::LoadStore,
                .opcode = op,
                .prefix = prefix,
                .regField = useRef(1),
                .width = width,
            };

            return &form;
        };

        reg(MachineFormId(first + 0), names[0], U8(rmRegOp - 1), 0,
            OperationWidth::Fixed32)->encoding.byteRegField = true;
        reg(MachineFormId(first + 1), names[1], rmRegOp, 0x66, OperationWidth::Fixed32);
        reg(MachineFormId(first + 2), names[2], rmRegOp, 0, OperationWidth::Fixed32);
        reg(MachineFormId(first + 3), names[3], rmRegOp, 0, OperationWidth::Fixed64);

        // And the two immediate forms. `0x83` carries a sign-extended byte and `0x81` four bytes;
        // both name the operation in the ModRM extension, the r/m field being the address.
        auto imm = [&](MachineFormId id, StringView formName, OperationWidth width) {
            auto& form = add(id, opcode, formName);
            form.uses.push(address());
            form.uses.push(immediate(ImmediateWidth::Imm8OrImm32));
            form.flagsEffect = FlagsEffect::Def;
            form.resultInFlags = true;
            form.signInFlags = logical;
            form.encoding = EncodingDescriptor {
                .family = EncodingFamily::LoadStore,
                .opcode = 0x83, .opcodeAlt = 0x81,
                .extension = extension,
                .immField = useRef(1),
                .width = width,
                .immediateBytes = 1,
            };
        };

        imm(MachineFormId(first + 4), names[4], OperationWidth::Fixed32);
        imm(MachineFormId(first + 5), names[5], OperationWidth::Fixed64);
    };

    // The `r/m, r` opcode of each operation, which is the register form's own primary (`add r/m, r`
    // is 0x01) - and the byte-width one is that opcode minus one, which is how the whole group-1
    // family is laid out. The extension is the same one the immediate register forms declare.
    static const StringView addNames[6] = {
        "add byte [address], r"_v, "add word [address], r"_v, "add dword [address], r"_v,
        "add qword [address], r"_v, "add dword [address], imm"_v, "add qword [address], imm"_v,
    };
    static const StringView subNames[6] = {
        "sub byte [address], r"_v, "sub word [address], r"_v, "sub dword [address], r"_v,
        "sub qword [address], r"_v, "sub dword [address], imm"_v, "sub qword [address], imm"_v,
    };
    static const StringView andNames[6] = {
        "and byte [address], r"_v, "and word [address], r"_v, "and dword [address], r"_v,
        "and qword [address], r"_v, "and dword [address], imm"_v, "and qword [address], imm"_v,
    };
    static const StringView orNames[6] = {
        "or byte [address], r"_v, "or word [address], r"_v, "or dword [address], r"_v,
        "or qword [address], r"_v, "or dword [address], imm"_v, "or qword [address], imm"_v,
    };
    static const StringView xorNames[6] = {
        "xor byte [address], r"_v, "xor word [address], r"_v, "xor dword [address], r"_v,
        "xor qword [address], r"_v, "xor dword [address], imm"_v, "xor qword [address], imm"_v,
    };

    storeUpdate(LowerInst::Add, addNames, 0x01, 0, false);
    storeUpdate(LowerInst::Sub, subNames, 0x29, 5, false);
    storeUpdate(LowerInst::And, andNames, 0x21, 4, true);
    storeUpdate(LowerInst::Or,  orNames,  0x09, 1, true);
    storeUpdate(LowerInst::Xor, xorNames, 0x31, 6, true);

    /*
     * Block operations.
     *
     * One encoding each, and the only one either of them has. A block operation short enough to be
     * worth straight-lining was written out as ordinary loads and stores long above here - see
     * `expandBlockOperations` - so what reaches selection is a count that is not a constant, or one
     * past the ceiling. Both are the string instruction, which demands fixed registers and consumes
     * them as it runs.
     */

    /*
     * The atomics - Analysis-Atomics.md §5.3.
     *
     * `xchg r, [m]` first, which is `0x87 /r` with the register in ModRM.reg and the location in
     * r/m - the store's shape with a result added. Two things about it are worth writing down.
     *
     * **The lock prefix is not written and is not missing.** `xchg` with a memory operand is locked
     * whether or not the byte is there, which is a property of the instruction rather than of this
     * encoding; every other locked form below states its prefix.
     *
     * **The result is tied to the value operand**, because that is what the instruction does: the
     * register it names ends up holding what the location held. So the allocator sees a destructive
     * two-address operation, and a caller whose value is still live afterwards pays the copy that
     * makes it one - which is the truth about the machine rather than a modelling choice.
     */

    auto exchange = [&](MachineFormId id, StringView formName, U8 opcode, U8 prefix, OperationWidth width) {
        auto& form = add(id, OpXchg, formName);
        form.uses.push(address());
        form.uses.push(anyReg());
        form.defs.push(tiedDef(1));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = opcode,
            .prefix = prefix,
            .regField = useRef(1),
            .width = width,
        };

        return &form;
    };

    exchange(FormXchg8, "xchg r8, byte [address]"_v, 0x86, 0,
             OperationWidth::Fixed32)->encoding.byteRegField = true;
    exchange(FormXchg16, "xchg r16, word [address]"_v, 0x87, 0x66, OperationWidth::Fixed32);
    exchange(FormXchg32, "xchg r32, dword [address]"_v, 0x87, 0, OperationWidth::Fixed32);
    exchange(FormXchg64, "xchg r64, qword [address]"_v, 0x87, 0, OperationWidth::Fixed64);

    /*
     * And `lock cmpxchg [m], r` - see OpCmpXchg, where the missing `setcc` is argued.
     *
     * rax is a fixed use *and* a fixed def, which is the shape a division already has: the value
     * compared has to be in it on the way in, and the value read is in it on the way out whichever
     * path the instruction took. The allocator therefore moves the expected value in and the
     * previous value out, and both moves are real - a compare-exchange in a retry loop pays them
     * once per attempt, which is what every implementation of one on this architecture pays.
     *
     * The flags are a **clobber** rather than a definition. `resultInFlags` would say that they
     * describe the result, and what they describe is something else: ZF is the exchange's success,
     * and the sign and carry flags come from a subtraction against a value the program never named.
     * The comparison `expandAtomics` writes is what a branch reads, and that comparison sets the
     * flags itself.
     */

    auto compareExchange = [&](MachineFormId id, StringView formName, U8 opcode, U8 prefix,
                               OperationWidth width)
    {
        auto& form = add(id, OpCmpXchg, formName);
        form.uses.push(address());
        form.uses.push(fixedReg(IntRegister::rax));
        form.uses.push(anyReg());

        auto previous = def();
        previous.kind = OperandConstraintKind::FixedRegister;
        previous.fixedReg = gpr(IntRegister::rax);
        form.defs.push(previous);

        /*
         * And the success flag, which occupies **nothing**.
         *
         * The IR instruction has two results and this machine instruction produces one. The second
         * is ZF, and `expandAtomics` has already written the comparison that stands in for it and
         * pointed every reader at that - so what is left here is a definition nothing reads and
         * nothing writes, which is exactly what `noDef` says.
         *
         * Saying it matters rather than being tidy. Left unstated, the operand fell past the end of
         * this list and was treated as unconstrained, so the allocator gave it a register - and the
         * register it gave was rax, which is where the value read is. `compareNarrow` in
         * test/x64/Atomic.lower is the case that caught it, and only under `build-assert`: the
         * machine verifier is what reports "operand p1 is read from rax, which holds o1".
         */
        form.defs.push(noDef());

        form.flagsEffect = FlagsEffect::Clobber;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = opcode, .escape = 0x0f, .prefix = prefix,
            .regField = useRef(2),
            .width = width,
            .lockPrefix = true,
        };

        return &form;
    };

    compareExchange(FormCmpXchg8, "lock cmpxchg byte [address], r8"_v, 0xb0, 0,
                    OperationWidth::Fixed32)->encoding.byteRegField = true;
    compareExchange(FormCmpXchg16, "lock cmpxchg word [address], r16"_v, 0xb1, 0x66, OperationWidth::Fixed32);
    compareExchange(FormCmpXchg32, "lock cmpxchg dword [address], r32"_v, 0xb1, 0, OperationWidth::Fixed32);
    compareExchange(FormCmpXchg64, "lock cmpxchg qword [address], r64"_v, 0xb1, 0, OperationWidth::Fixed64);

    /*
     * `lock xadd [m], r` - the fetch-and-add, and the one arithmetic update whose previous value
     * the architecture hands back. `0f c1 /r`, with the register in ModRM.reg exactly as the
     * exchange has it, and the same tied result for the same reason: the register carrying the
     * addend in is the register carrying the old value out.
     *
     * The lock prefix *is* written here, where `xchg`'s is not. `xchg` is locked implicitly and this
     * is not: without the byte it is a perfectly good non-atomic read-modify-write, which is exactly
     * the failure that would never show up in a single-threaded test.
     */
    auto exchangeAdd = [&](MachineFormId id, StringView formName, U8 opcode, U8 prefix,
                           OperationWidth width)
    {
        auto& form = add(id, OpXAdd, formName);
        form.uses.push(address());
        form.uses.push(anyReg());
        form.defs.push(tiedDef(1));
        form.flagsEffect = FlagsEffect::Clobber;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = opcode, .escape = 0x0f, .prefix = prefix,
            .regField = useRef(1),
            .width = width,
            .lockPrefix = true,
        };

        return &form;
    };

    exchangeAdd(FormXAdd8, "lock xadd byte [address], r8"_v, 0xc0, 0,
                OperationWidth::Fixed32)->encoding.byteRegField = true;
    exchangeAdd(FormXAdd16, "lock xadd word [address], r16"_v, 0xc1, 0x66, OperationWidth::Fixed32);
    exchangeAdd(FormXAdd32, "lock xadd dword [address], r32"_v, 0xc1, 0, OperationWidth::Fixed32);
    exchangeAdd(FormXAdd64, "lock xadd qword [address], r64"_v, 0xc1, 0, OperationWidth::Fixed64);

    /*
     * And the five locked in-place updates - see OpLockAdd.
     *
     * Shaped exactly like the `storeUpdate` block above and encoded by the same family, with two
     * differences that are both about what the operation *is* rather than about how it is written.
     *
     * **The result occupies nothing rather than not existing.** `X86StoreOp` has no result at all;
     * an atomic read-modify-write always has one, and these forms are taken precisely when nobody
     * reads it. So the def is stated as `noDef` and `expandAtomics` marks the value implicit - the
     * pair FormCmpXchg's flag already needed, and for the reason written there: an unstated def is
     * an unconstrained one, and the allocator gives an unconstrained def a register.
     *
     * **The flags are a clobber.** The arithmetic is the same arithmetic, but it is performed on a
     * location another thread may write immediately afterwards, so a comparison folded into one of
     * these would be answering a question about a value nobody holds.
     *
     * There are no immediate forms. A group-1 immediate encoding is available and correct, and what
     * is missing is a caller: an operand reaching here is `amount` or `mask` from the library's own
     * signature, and `selectStoreUpdates` - the pass that embeds a constant into the ordinary
     * in-place update - is written against `X86StoreOp` and never sees one of these.
     */
    auto lockUpdate = [&](MachineOpcodeId opcode, MachineFormId first, const StringView (&names)[4],
                          U8 rmRegOp)
    {
        auto row = [&](MachineFormId id, StringView formName, U8 op, U8 prefix,
                       OperationWidth width)
        {
            auto& form = add(id, opcode, formName);
            form.uses.push(address());
            form.uses.push(anyReg());
            form.defs.push(noDef());
            form.flagsEffect = FlagsEffect::Clobber;
            form.encoding = EncodingDescriptor {
                .family = EncodingFamily::LoadStore,
                .opcode = op,
                .prefix = prefix,
                .regField = useRef(1),
                .width = width,
                .lockPrefix = true,
            };

            return &form;
        };

        row(MachineFormId(first + 0), names[0], U8(rmRegOp - 1), 0,
            OperationWidth::Fixed32)->encoding.byteRegField = true;
        row(MachineFormId(first + 1), names[1], rmRegOp, 0x66, OperationWidth::Fixed32);
        row(MachineFormId(first + 2), names[2], rmRegOp, 0, OperationWidth::Fixed32);
        row(MachineFormId(first + 3), names[3], rmRegOp, 0, OperationWidth::Fixed64);
    };

    static const StringView lockAddNames[4] = {
        "lock add byte [address], r"_v, "lock add word [address], r"_v,
        "lock add dword [address], r"_v, "lock add qword [address], r"_v,
    };
    static const StringView lockSubNames[4] = {
        "lock sub byte [address], r"_v, "lock sub word [address], r"_v,
        "lock sub dword [address], r"_v, "lock sub qword [address], r"_v,
    };
    static const StringView lockAndNames[4] = {
        "lock and byte [address], r"_v, "lock and word [address], r"_v,
        "lock and dword [address], r"_v, "lock and qword [address], r"_v,
    };
    static const StringView lockOrNames[4] = {
        "lock or byte [address], r"_v, "lock or word [address], r"_v,
        "lock or dword [address], r"_v, "lock or qword [address], r"_v,
    };
    static const StringView lockXorNames[4] = {
        "lock xor byte [address], r"_v, "lock xor word [address], r"_v,
        "lock xor dword [address], r"_v, "lock xor qword [address], r"_v,
    };

    lockUpdate(OpLockAdd, FormLockAdd8, lockAddNames, 0x01);
    lockUpdate(OpLockSub, FormLockSub8, lockSubNames, 0x29);
    lockUpdate(OpLockAnd, FormLockAnd8, lockAndNames, 0x21);
    lockUpdate(OpLockOr,  FormLockOr8,  lockOrNames,  0x09);
    lockUpdate(OpLockXor, FormLockXor8, lockXorNames, 0x31);

    /*
     * The fence, at the two shapes it has on this architecture - see OpFence.
     *
     * `mfence` is group 15's `0f ae /6` in its no-operand spelling, which is the whole instruction:
     * no ModRM operand is encoded and the byte after the opcode is fixed. It is written here rather
     * than taken from the `MFence` intrinsic because a fence that the program wrote as an ordering
     * *and* one it wrote as `X86.mfence()` are two different statements, and only the first of them
     * has an order to read.
     *
     * The other form encodes nothing at all, which is the correct encoding of an acquire or release
     * fence on a machine that already orders everything they order. It is still an instruction - see
     * OpFence for what would be lost by deleting it.
     */
    {
        auto& form = add(FormMFence, OpFence, "mfence"_v);
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Opcode,
            .opcode = 0xae, .opcodeAlt = 0xf0, .escape = 0x0f,
            .width = OperationWidth::Fixed32,
        };
    }

    {
        auto& form = add(FormFenceNone, OpFence, "fence"_v);
        form.encoding = EncodingDescriptor { .family = EncodingFamily::None };
    }

    // PAUSE (f3 90) is `rep nop`: architecturally nothing, and a hint to the processor that the loop
    // around it is polling. It writes no register and reads none, so the whole of it is the prefix
    // and the opcode.
    {
        auto& form = add(FormSpinHint, OpSpinHint, "pause"_v);
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Opcode,
            .opcode = 0x90, .prefix = 0xf3,
            .width = OperationWidth::Fixed32,
        };
    }

    {
        auto& form = add(FormBlockCopyRep, OpBlockCopy, "rep movsb"_v);
        form.uses.push(fixedReg(IntRegister::rdi));
        form.uses.push(fixedReg(IntRegister::rsi));
        form.uses.push(fixedReg(IntRegister::rcx));

        // Consumed as it runs: rdi and rsi are left pointing past the copied region and rcx is
        // counted down to zero, so a value still live afterwards has to be copied somewhere safe
        // first instead of being read back advanced.
        form.clobbers.add(gpr(IntRegister::rdi));
        form.clobbers.add(gpr(IntRegister::rsi));
        form.clobbers.add(gpr(IntRegister::rcx));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::BlockCopyRep,
        };
    }

    {
        // Positional, matching the instruction's own operand order (to, count, pattern) rather than
        // the rdi/rax/rcx order `rep stosb` reads them in.
        auto& form = add(FormBlockSetRep, OpBlockSet, "rep stosb"_v);
        form.uses.push(fixedReg(IntRegister::rdi));
        form.uses.push(fixedReg(IntRegister::rcx));
        form.uses.push(fixedReg(IntRegister::rax));

        // rdi is advanced past the filled region and rcx counted down; rax is only read.
        form.clobbers.add(gpr(IntRegister::rdi));
        form.clobbers.add(gpr(IntRegister::rcx));
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::BlockSetRep,
        };
    }

    /*
     * Calls.
     *
     * Operand and result locations come from the selected calling convention rather than from a
     * table here: where an argument goes depends on how many of each bank came before it, which a
     * flat list cannot say. The clobber set comes from the same place.
     */

    auto call = [&](MachineFormId id, StringView formName, PseudoKind pseudo) {
        auto& form = add(id, OpCall, formName);
        form.conventionOperands = true;
        form.flagsEffect = FlagsEffect::Clobber;
        form.encoding = EncodingDescriptor { .family = EncodingFamily::Pseudo, .pseudo = pseudo };
    };

    call(FormCallDirect, "call rel32"_v, PseudoKind::CallDirect);
    call(FormCallIndirect, "call r/m"_v, PseudoKind::CallIndirect);
    call(FormSyscall, "syscall"_v, PseudoKind::Syscall);

    // The argument area is addressed through rsp, at the offset the convention assigned - an address
    // the legalizer resolves like any other, so this is an ordinary store.
    {
        auto& form = add(FormPushArgReg, OpPushArg, "mov [rsp + n], r"_v);
        form.uses.push(anyReg());
        form.defs.push(noDef()); // stands in for the argument in the call's operand list
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = 0x89,
            .regField = useRef(0),
            .width = OperationWidth::Fixed64,
        };
    }

    // A float argument is stored by the instruction that owns its bank, at its own width: the slot
    // is eight bytes wide whatever goes in it, and the callee reads back exactly the four or eight
    // the convention put there.
    auto floatPushArg = [&](MachineFormId id, StringView formName, U8 prefix, RegisterClassId cls,
                            OperationWidth width)
    {
        auto& form = add(id, OpPushArg, formName);
        form.uses.push(anyReg(cls));
        form.defs.push(noDef());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = 0x11, .escape = 0x0f, .prefix = prefix,
            .regField = useRef(0),
            .width = width,
            .widthInPrefix = true,
        };
    };

    floatPushArg(FormPushArgF32, "movss [rsp + n], xmm"_v, 0xf3, ClassFloat32, OperationWidth::Fixed32);
    floatPushArg(FormPushArgF64, "movsd [rsp + n], xmm"_v, 0xf2, ClassFloat64, OperationWidth::Fixed64);

    {
        // MOV r/m64, imm32 sign-extends, which is what a narrower constant occupying a full 8-byte
        // argument slot wants anyway.
        auto& form = add(FormPushArgImm, OpPushArg, "mov [rsp + n], imm"_v);
        form.uses.push(immediate(ImmediateWidth::Imm32));
        form.defs.push(noDef());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::LoadStore,
            .opcode = 0xc7, .extension = 0,
            .immField = useRef(0),
            .width = OperationWidth::Fixed64,
        };
    }

    /*
     * The remaining target operations.
     */

    {
        auto& form = add(FormLea, OpLea, "lea r, [address]"_v);
        form.defs.push(def());
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Lea,
            .opcode = 0x8d,
            .regField = defRef(0),
            .width = OperationWidth::Fixed64,
        };
    }

    /*
     * Terminators.
     *
     * All four are pseudos: which bytes a branch takes depends on which of its successors the block
     * order put next, and a return has to emit the epilogue the frame layout decided on.
     */

    add(FormJmp, OpJmp, "jmp rel32"_v).encoding = EncodingDescriptor {
        .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::Jump,
    };

    {
        auto& form = add(FormJccFlags, OpJcc, "jcc rel32"_v);
        form.uses.push(folded()); // the condition was consumed by the comparison that set the flags
        form.flagsEffect = FlagsEffect::Use;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::Branch,
        };
    }

    {
        // The same branch, where the comparison that set the flags was materialized as well and so
        // still holds a register - see §3.5.2.1 of the README. The condition is a real operand rather
        // than a folded one, because the value is genuinely live; it is simply not what the branch
        // reads. Two bytes cheaper than the form below, which re-derives the flags from it.
        auto& form = add(FormJccLive, OpJcc, "jcc rel32 (condition live)"_v);
        form.uses.push(anyReg(ClassGpr32));
        form.flagsEffect = FlagsEffect::Use;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::Branch,
        };
    }

    {
        auto& form = add(FormJccReg, OpJcc, "test r, r; jcc rel32"_v);
        form.uses.push(anyReg(ClassGpr32));
        form.flagsEffect = FlagsEffect::UseDef;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::Branch,
            .prelude = EncodingPrelude::TestLastUse,
        };
    }

    {
        // A return's operands are the function's results, placed by the result half of its own
        // convention. Nothing is live once the function has returned, so it clobbers nothing.
        auto& form = add(FormRet, OpRet, "ret"_v);
        form.conventionOperands = true;
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::Return,
        };
    }

    {
        /*
         * The one form in this table that emits no bytes, and the only one entitled to.
         *
         * Every other zero-byte case in the backend is an instruction that *became* nothing - a cast
         * whose extension was already done, a copy into the register the value was already in - and
         * each of those is a form that would have emitted something had the fold not applied. This
         * one has nothing to emit in the first place: the block it ends is one nothing arrives at
         * the end of, so there is no epilogue to run and no address to return to. It carries no
         * operands and no successors, which is why it needs neither convention nor clobbers.
         */
        auto& form = add(FormNoReturn, OpNoReturn, "noreturn"_v);
        form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Pseudo, .pseudo = PseudoKind::NoReturn,
        };
    }

    assertTrue(forms.size() == kMachineFormCount);
}
