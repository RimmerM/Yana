#include "machine.h"
#include "x64_util.h"

/*
 * The intrinsic registry.
 *
 * An intrinsic is a machine operation the program named rather than one the lowering derived, and
 * this is the whole of what the backend knows about each one: a machine form like any other
 * instruction's, the features its encoding needs, what its operands and results have to be, and what
 * it does beyond them.
 *
 * What is worth reading here is what is *not* anywhere else. Each of the four below exercises a
 * different thing an intrinsic can demand of the allocator, and none of them appears by name in
 * placement, in legalization or in the encoder:
 *
 *   bswap   an ordinary destructive register operation, and the one existing target-specific
 *           instruction this path replaced.
 *   popcnt  reads one operand out of a frame slot where the allocator left it there, exactly as an
 *           `add` does, because it says so with the same constraint.
 *   cpuid   forces two operands into fixed registers and produces four results in fixed registers,
 *           one of which is callee-saved and so drags a save into the prologue.
 *   rdtscp  writes a register it does not name as a result at all, which is a clobber like a call's.
 *
 * Adding a fifth is a block below and a name in the IR's own table (lower.cpp). If it ever needs
 * more than that, the thing to change is the descriptor, not the pass that noticed it was missing.
 */

static IntrinsicOperandRule integerRule() {
    return IntrinsicOperandRule { IntrinsicOperandClass::Integer };
}

static IntrinsicOperandRule integer32Rule() {
    return IntrinsicOperandRule { IntrinsicOperandClass::Integer32 };
}

// One intrinsic being added: its form and its descriptor, which are written together because they
// are two halves of one row.
struct IntrinsicBuilder {
    MachineForm& form;
    IntrinsicDescriptor& desc;
};

void addIntrinsics(MachineTarget& target) {
    auto add = [&](LowerIntrinsic id, StringView formName, FeatureSet features) {
        auto& ir = lowerIntrinsicDesc(id);
        auto opcode = opcodeForIntrinsic(id);

        // An intrinsic's machine opcode is named after the intrinsic, so that a form table dump and
        // a verifier message say the thing the program wrote.
        target.opcodes[opcode].id = opcode;
        target.opcodes[opcode].name = ir.name;

        target.forms.push(MachineForm {});

        auto& form = target.forms[target.forms.size() - 1];
        form.id = MachineFormId(target.forms.size() - 1);
        form.opcode = opcode;
        form.name = formName;
        form.requiredFeatures = features;

        auto& desc = target.intrinsics[Size(id)];
        desc.id = id;
        desc.opcode = opcode;
        desc.form = form.id;
        desc.requiredFeatures = features;
        desc.defined = true;

        return IntrinsicBuilder { form, desc };
    };

    {
        // BSWAP r (0f c8+r) reverses the byte order of its register in place, so it is destructive
        // like the two-address ALU operations and needs nothing else said about it.
        auto b = add(LowerIntrinsic::Bswap, "bswap r"_v, kFeatureBaseline);
        b.form.uses.push(anyReg());
        b.form.defs.push(tiedDef(0));
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::OpcodeReg,
            .opcode = 0xc8, .escape = 0x0f,
            .rmField = useRef(0),
        };

        b.desc.operands.push(integerRule());
        b.desc.results.push(integerRule());
    }

    {
        // POPCNT r, r/m (f3 0f b8) counts the set bits of its operand into a register that need not
        // be the operand's. The r/m side is the ordinary memory alternative: a source the allocator
        // left in the frame is read there, with no reload, because this row says so in the same
        // words `add` does - and nothing in placement or legalization had to learn the name.
        auto b = add(LowerIntrinsic::Popcnt, "popcnt r, r/m"_v, kFeaturePopcnt);
        b.form.uses.push(regOrMem(MemoryAccessKind::Read));
        b.form.defs.push(def());
        b.form.flagsEffect = FlagsEffect::Def;
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::RegRm,
            .opcode = 0xb8, .escape = 0x0f, .prefix = 0xf3,
            .regField = defRef(0), .rmField = useRef(0),
            .width = OperationWidth::FromUse0,
        };

        b.desc.operands.push(integerRule());
        b.desc.results.push(integerRule());
    }

    {
        // CPUID (0f a2) reads the leaf in eax and the subleaf in ecx, and answers in all four of
        // eax, ebx, ecx and edx. Every one of those is a fixed register the allocator copies into
        // and out of - including ebx, which is callee-saved, so a function using this pays a push
        // and a pop for it without anything here saying so.
        auto b = add(LowerIntrinsic::Cpuid, "cpuid"_v, kFeatureBaseline);
        b.form.uses.push(fixedReg(IntRegister::rax));
        b.form.uses.push(fixedReg(IntRegister::rcx));
        b.form.defs.push(fixedDef(IntRegister::rax));
        b.form.defs.push(fixedDef(IntRegister::rbx));
        b.form.defs.push(fixedDef(IntRegister::rcx));
        b.form.defs.push(fixedDef(IntRegister::rdx));
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Opcode,
            .opcode = 0xa2, .escape = 0x0f,
            .width = OperationWidth::Fixed32,
        };

        b.desc.operands.push(integer32Rule());
        b.desc.operands.push(integer32Rule());
        for(Size i = 0; i < 4; i++) b.desc.results.push(integer32Rule());

        b.desc.effects.ordered = true; // a serializing instruction, which is half of what it is for
    }

    {
        // RDTSCP (0f 01 f9) reads the timestamp counter into edx:eax and the processor id into ecx.
        // Only the counter halves are results here, so ecx is a register the instruction writes
        // without naming - an implicit clobber, exactly like a call's, and the allocator keeps live
        // values out of it for exactly the same reason.
        auto b = add(LowerIntrinsic::Rdtscp, "rdtscp"_v, kFeatureRdtscp);
        b.form.defs.push(fixedDef(IntRegister::rax));
        b.form.defs.push(fixedDef(IntRegister::rdx));
        b.form.clobbers.add(gpr(IntRegister::rcx));
        b.form.encoding = EncodingDescriptor {
            .family = EncodingFamily::Opcode,
            .opcode = 0x01, .opcodeAlt = 0xf9, .escape = 0x0f,
            .width = OperationWidth::Fixed32,
        };

        b.desc.results.push(integer32Rule());
        b.desc.results.push(integer32Rule());
        b.desc.effects.ordered = true;
    }
}

/*
 * Validation.
 */

bool validateIntrinsics(const MachineTarget& target) {
    auto ok = true;

    for(Size i = 0; i < kLowerIntrinsicCount; i++) {
        auto id = LowerIntrinsic(i);
        auto& desc = target.intrinsics[i];
        auto& ir = lowerIntrinsicDesc(id);

        auto fail = [&](StringView what) {
            ok = false;
            logError("intrinsic \"%@\": %@", ir.name, what);
        };

        // Every intrinsic the IR can write down has to be one this target can select, or a program
        // using it would reach the encoder with no description of what to emit.
        if(!desc.defined) {
            fail("has no descriptor for this target"_v);
            continue;
        }

        if(desc.id != id || desc.opcode != opcodeForIntrinsic(id)) {
            fail("is registered under another intrinsic's opcode"_v);
            continue;
        }

        auto& form = target.form(desc.form);
        if(form.opcode != desc.opcode) fail("names a form belonging to another opcode"_v);
        if(form.requiredFeatures != desc.requiredFeatures) fail("and its form disagree about the features they need"_v);

        // A rule per operand and per result, so that the type check below covers all of them rather
        // than however many happened to be written out.
        if(desc.operands.size() != ir.args) fail("does not state a rule for every operand"_v);
        if(desc.results.size() != ir.results) fail("does not state a rule for every result"_v);

        // The form has to be able to hold everything the IR gives the intrinsic. A form saying less
        // than the instruction does would leave operands unconstrained by accident rather than on
        // purpose, which for a privileged instruction is the difference between working code and a
        // fault.
        if(form.uses.size() != ir.args) fail("and its form disagree about how many operands it has"_v);
        if(form.defs.size() != ir.results) fail("and its form disagree about how many results it has"_v);
    }

    return ok;
}

// Whether the values an intrinsic was given match the rules its descriptor states. Asked where the
// form is selected, which is the last point at which a wrong operand is still a compile error rather
// than a wrong instruction.
bool checkIntrinsicOperands(LowerBase base, const IntrinsicDescriptor& desc, LowerInstIntrinsic* inst) {
    auto check = [&](const IntrinsicOperandRule& rule, LowerType type, LowerValue* value) {
        switch(rule.kind) {
            case IntrinsicOperandClass::Integer:
                return isIntLike(type);
            case IntrinsicOperandClass::Integer32:
                return type == LowerType::Int32;
            case IntrinsicOperandClass::Integer64:
                return type == LowerType::Int64 || type == LowerType::Pointer;

            case IntrinsicOperandClass::Immediate: {
                if(!value || value->inst()->kind != LowerInst::Imm) return false;

                auto imm = ((LowerImm*)value->inst())->i;
                return imm >= rule.minImmediate && imm <= rule.maxImmediate;
            }
        }

        return false;
    };

    auto used = inst->used();
    auto created = inst->created();

    for(Size i = 0; i < desc.operands.size() && i < used.size(); i++) {
        auto value = base[used[i]];
        if(!check(desc.operands[i], value->type, value)) return false;
    }

    for(Size i = 0; i < desc.results.size() && i < created.size(); i++) {
        if(!check(desc.results[i], created[i].type, nullptr)) return false;
    }

    return true;
}
