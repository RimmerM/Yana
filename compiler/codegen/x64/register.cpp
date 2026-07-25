#include "gen.h"
#include "x64_util.h"

inline RegClass classForType(LowerType type) {
    if(isIntLike(type)) {
        return GenReg;
    } else {
        return XmmReg;
    }
}

static LowerInst* split(LowerInst* inst, const DominatorTree& dominators) {
    return nullptr;
}

struct AllocationHeader {
    U8 moveCount;

};

struct Allocation {
    // For every instruction:
    //  - Perform any moves to get values into place.
    //  - Set the instruction inputs.
    //  - Set the instruction outputs.
    LowerList<Byte> changes;
};

struct RegisterMapping {
    // Contains one entry for every block in the function.
    EmbedList<Allocation, false> blocks;
};

struct Occupation {
    // The value currently in this register.
    LowerPtr<LowerValue> value = nullptr;

    // The number of remaining uses for the value within this block, if liveOut == false.
    // Otherwise, set to 0.
    U16 usesRemaining = 0;

    // If set, this value is still live at the end of the current block.
    bool liveOut = false;
};

struct AllocatorInst {
    using EncodedMove = U32;

    Array<EncodedMove> moves;
    Array<RegId> uses;
    Array<RegId> creates;
};

struct Allocator {
    Ptr<Liveness> live;

    HashMap<LowerPtr<LowerValue>, RegId> mapping;
    Array<Occupation> registers[kRegClassCount];

    const U16 available[kRegClassCount] = {
        16,                             // General registers.
        16,                             // Xmm registers.
        getRegIndex(maxLimit<RegId>),   // Maximum number of stack slots we can represent.
    };

    U16 inUse[kRegClassCount] = { 0, 0, 0 };

    AllocatorInst inst;
    Net::BufferWriter ops { 2048 };

    void onBlock(LowerBase base, LowerBlock* block) {
        auto set = live->getBlock(block);

        // Check that the pre-calculated live-in was actually correct.
        Size totalUsed = 0;
        for(auto i: inUse) totalUsed += i;
        assertTrue(set->liveIn.count(set->valueCount) == totalUsed);

        // For all the currently live values that die in this block, update the occupation state.
        set->liveIn.iterate(set->valueCount, [&](LiveId liveId) {
            // If the value is both live-in and live-out, we can ignore it.
            // If it is only live-in, it will be destroyed in this block, so we calculate the use count.
            if(set->liveOut.get(set->valueCount, liveId)) return;

            auto value = live->getValue(liveId);
            U16 uses = 0;

            for(auto use: value->uses.contents(base)) {
                if(base[base[use]->block] == block) uses++;
            }

            auto reg = tryMaybe(mapping.getValue(value - base), { assertTrue("invalid register state" == nullptr); return; });
            auto& o = registers[getRegClass(reg)][getRegIndex(reg)];

            assertTrue(o.value == value - base);
            o.liveOut = false;
            o.usesRemaining = uses;
        });

        // Store the starting op for the block.
        block->ops = ops.offset();
    }

    void onCreate(LowerBase base, LowerValue* v, RegId reg) {
        // If no register was provided, try to find a free one.
        if(reg == kInvalidReg) {
            reg = allocate(base, v);
        }

        // Get lifetime information for the value.
        auto c = getRegClass(reg);
        auto block = v->inst()->block;
        auto set = live->getBlock(base[block]);
        auto liveId = v->liveId();
        auto isLive = liveId == kNullLive ? false : set->liveOut.get(set->valueCount, liveId);

        // If the value is not live-out, we calculate the total number of uses within this block.
        // That way, we can easily know when the value has died.
        U16 uses = 0;
        if(!isLive) {
            for(auto use: v->uses.contents(base)) {
                if(base[use]->block == block) uses++;
            }
        }

        occupy(reg, { v - base, uses, isLive });
        mapping.add(v - base, reg);
        inUse[c]++;
        inst.creates.push(reg);
    }

    void onDestroy(LowerBase base, LowerValue* v) {
        auto reg = tryMaybe(mapping.getValue(v - base), { assertTrue("invalid register state" == nullptr); return; });
        auto c = getRegClass(reg);
        auto& o = registers[c][getRegIndex(reg)];

        assertTrue(!o.liveOut && o.usesRemaining == 0);
        assertTrue(inUse[c] > 0);

        o = Occupation {};
        mapping.remove(v - base);
        inUse[c]--;
    }

    void onMove(LowerBase base, LowerValue* v, RegId target) {
        auto current = tryMaybe(mapping.getValue(v - base), { assertTrue("invalid register state" == nullptr); return; });
        auto& o = registers[getRegClass(current)][getRegIndex(current)];

        occupy(target, o);
        o = Occupation {};

        mapping.add(v - base, target);
        inUse[getRegClass(target)]++;

        assertTrue(inUse[getRegClass(current)] > 0);
        inUse[getRegClass(current)]--;
        inst.moves.push((U32(current) << 16) | target);
    }

    // If this use also consumes the value, returns the register that was freed by it.
    // Otherwise, returns kInvalidReg.
    RegId onUse(LowerBase base, LowerValue* v) {
        auto reg = tryMaybe(mapping.getValue(v - base), { assertTrue("invalid register state" == nullptr); return kInvalidReg; });
        auto c = getRegClass(reg);
        auto& o = registers[c][getRegIndex(reg)];
        bool consumes = false;

        if(!o.liveOut) {
            assertTrue(o.usesRemaining > 0);
            consumes = --o.usesRemaining == 0;
        }

        if(consumes) {
            onDestroy(base, v);
        }

        inst.uses.push(reg);
        return consumes ? reg : kInvalidReg;
    }

    void onInst(LowerBase base, LowerInst* i) {
        assertTrue(inst.uses.size() <= 255);
        assertTrue(inst.creates.size() <= 255);
        assertTrue(inst.moves.size() <= 255);

        ops.writeByte(inst.moves.size());
        for(auto u: inst.moves) {
            ops.writeInt<kByteOrder>(u);
        }

        ops.writeByte(inst.uses.size());
        for(auto u: inst.uses) {
            ops.writeShort<kByteOrder>(u);
        }

        ops.writeByte(inst.creates.size());
        for(auto u: inst.creates) {
            ops.writeShort<kByteOrder>(u);
        }

        inst.uses.clear();
        inst.creates.clear();
        inst.moves.clear();
    }

    void occupy(RegId reg, Occupation o) {
        auto index = getRegIndex(reg);
        auto c = getRegClass(reg);

        auto& occupation = registers[c];
        while(occupation.size() <= index) occupation.push();

        assertTrue(occupation[index].value == nullptr);
        occupation[index] = o;
    }

    RegId allocate(LowerBase base, LowerValue* value) {
        auto typeClass = classForType(value->type);
        auto classCount = available[typeClass];
        auto& classRegs = registers[typeClass];

        if(inUse[typeClass] >= classCount) return kInvalidReg;

        for(Size i = 0; i < classCount; i++) {
            if(classRegs.size() > i && classRegs[i].value == nullptr) {
                return makeRegId(typeClass, i);
            }
        }

        if(classRegs.size() < classCount) {
            return makeRegId(typeClass, classRegs.size());
        }

        return kInvalidReg;
    }

    bool consumes(LowerBase base, LowerValue* v) {
        auto reg = tryMaybe(mapping.getValue(v - base), { assertTrue("invalid register state" == nullptr); return false; });
        auto& o = registers[getRegClass(reg)][getRegIndex(reg)];

        return !o.liveOut && o.usesRemaining <= 1;
    }
};

static void assignArgs(LowerBase base, LowerFunction& fun, Allocator& allocator, const InstConstraints& call) {
    auto args = fun.args.contents(base);
    U32 index[kRegClassCount];

    for(auto offset: args) {
        auto arg = base[offset];
        auto targetClass = classForType(arg->result.type);
        auto classIndex = index[targetClass];

        if(classIndex >= kMaxRegInputs || call.constraints[targetClass].args[classIndex] == kInvalidReg) {
            allocator.onCreate(base, &arg->result, makeRegId(StackReg, index[StackReg]));
            index[StackReg]++;
        } else {
            allocator.onCreate(base, &arg->result, call.constraints[targetClass].args[classIndex]);
            index[targetClass]++;
        }
    }
}

void allocateRegisters(Context& ctx, LowerBase base, LowerFunction& fun) {
    Constraints constraints;
    Allocator a { .live = fun.buildLiveness(base) };

    assignArgs(base, fun, a, constraints.getCall(fun.callType));

    for(auto b: fun.blocks.contents(base)) {
        auto block = base[b];
        a.onBlock(base, block);

        for(auto i: block->instructions.contents(base)) {
            auto inst = base[i];
            auto c = constraints.getConstraints(base, inst);

            if(c) {

            }

            // Keep track of the first register freed by using the arguments, and try it reuse it for the result.
            // We only keep track of one register, because basically every instruction
            // that returns multiple values is also constrained in some way.
            RegId freed = kInvalidReg;

            for(auto use: inst->used()) {
                if(auto r = a.onUse(base, base[use]); r != kInvalidReg && freed == kInvalidReg) {
                    freed = r;
                }
            }

            for(auto& v: inst->created()) {
                if(c && c->constraints[classForType(v.type)].args)
                a.onCreate(base, &v, freed);
            }

            a.onInst(base, inst);
        }
    }
}
