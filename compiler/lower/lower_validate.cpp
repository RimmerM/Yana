#include "lower_validate.h"
#include "lower_inst.h"

static bool validateLowerArg(Diagnostics* diagnostics, LowerBase base, LowerBlock* entryPoint, LowerArg* arg, U32 index, const DominatorTree& dominators) {
    if(!validateLowerInst(diagnostics, base, entryPoint, arg, dominators)) return false;

    if(index != arg->getIndex()) {
        diagnostics->error("inconsistent indexes for argument"_v, arg->source);
        return false;
    }

    return true;
}

static bool validateImm(Diagnostics* diagnostics, LowerImm* inst) {
    if(isPtr(inst->result.type)) {
        diagnostics->error("cannot create immediate value of pointer type"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateGlobal(Diagnostics* diagnostics, LowerInstGlobal* inst) {
    if(!isPtr(inst->result.type)) {
        diagnostics->error("global references must be of pointer type"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateFun(Diagnostics* diagnostics, LowerInstFun* inst) {
    if(!isPtr(inst->result.type)) {
        diagnostics->error("function references must be of pointer type"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateCast(Diagnostics* diagnostics, LowerBase base, LowerInstCast* inst) {
    auto result = inst->result.type;
    auto source = base[inst->from]->type;
    auto valid = (isInt(result) || isFloat(result)) && (isInt(source) || isFloat(source));

    if(!valid) {
        diagnostics->error("incompatible cast types"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateBitcast(Diagnostics* diagnostics, LowerBase base, LowerInstUnary* inst) {
    auto result = inst->result.type;
    auto source = base[inst->from]->type;
    bool valid;

    if(isPtr(source)) {
        valid = isPtr(result) || isInt(result);
    } else if(isPtr(result)) {
        valid = isPtr(source) || isInt(source);
    } else {
        valid = (isInt(result) || isFloat(result)) && (isInt(source) || isFloat(source));
    }

    if(!valid) {
        diagnostics->error("incompatible cast types"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateUnary(Diagnostics* diagnostics, LowerBase base, LowerInstUnary* inst, bool allowFloat) {
    auto result = inst->result.type;
    auto source = base[inst->from]->type;

    bool valid = isInt(source);
    if(allowFloat && isFloat(source)) valid = true;

    if(!valid) {
        diagnostics->error("invalid type to unary operation"_v, inst->source);
        return false;
    }

    if(source != result) {
        diagnostics->error("inconsistent argument types to operation"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateSet(Diagnostics* diagnostics, LowerBase base, LowerInstUnary* inst) {
    if(base[inst->from]->type != inst->result.type) {
        diagnostics->error("inconsistent types in copy of local"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateRet(Diagnostics* diagnostics, LowerBase base, LowerInstRet* inst) {
    auto f = base[base[inst->block]->fun];
    if(inst->createdCount != 0 || inst->usedCount != f->returnTypes.size()) {
        diagnostics->error("incorrect number of values returned from function"_v, inst->source);
        return false;
    }

    // Make sure return points all return the correct types.
    auto used = inst->used();
    for(Size i = 0; i < used.length; i++) {
        auto use = base[used[i]];

        if(use->type != (LowerType)f->returnTypes.get(base, i)) {
            diagnostics->error("incorrect type returned from function"_v, inst->source);
            return false;
        }
    }

    return true;
}

static bool validateJmp(Diagnostics* diagnostics, LowerBase base, LowerInstJmp* inst) {
    auto b = base[inst->block];
    if(b->outgoing[1] != nullptr || b->outgoing[0] != inst->then) {
        diagnostics->error("incorrect block references from jump"_v, inst->source);
        return false;
    }

    if(!base[inst->then]->incoming.contents(base).containsValue(b - base)) {
        diagnostics->error("inconsistent references between blocks"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateJe(Diagnostics* diagnostics, LowerBase base, LowerInstJe* inst) {
    auto b = base[inst->block];
    if(inst->then == inst->otherwise) {
        diagnostics->error("same target block for all branches"_v, inst->source);
        return false;
    }

    if(b->outgoing[0] != inst->then || b->outgoing[1] != inst->otherwise) {
        diagnostics->error("incorrect block references from jump"_v, inst->source);
        return false;
    }

    if(!base[inst->then]->incoming.contents(base).containsValue(b - base) || !base[inst->otherwise]->incoming.contents(base).containsValue(b - base)) {
        diagnostics->error("inconsistent references between blocks"_v, inst->source);
        return false;
    }

    return true;
}

static bool validatePhi(Diagnostics* diagnostics, LowerBase base, LowerInstPhi* inst, const DominatorTree& dominators) {
    auto used = inst->used();
    auto blocks = inst->sources();

    if(used.length != blocks.length || used.length < 1) {
        diagnostics->error("inconsistent argument count to phi"_v, inst->source);
        return false;
    }

    if(used.length != base[inst->block]->incoming.size()) {
        diagnostics->error("phi must have an alternative for every incoming block"_v, inst->source);
        return false;
    }

    for(Size i = 0; i < used.length; i++) {
        auto value = base[used[i]];
        auto fromBlock = blocks[i];

        if(!base[inst->block]->incoming.contents(base).containsValue(fromBlock)) {
            diagnostics->error("incorrect source block for phi"_v, inst->source);
            return false;
        }

        if(value->type != base[used[0]]->type) {
            diagnostics->error("inconsistent types between phi alternatives"_v, inst->source);
            return false;
        }

        if(!base[value->inst()->block]->dominates(base[fromBlock], dominators)) {
            diagnostics->error("phi alternative doesn't dominate its source block"_v, inst->source);
            return false;
        }

        if(!value->uses.contents(base).containsValue((LowerInst*)inst - base)) {
            diagnostics->error("phi alternative is not in source uses list"_v, inst->source);
            return false;
        }
    }

    return true;
}

static bool validateCall(Diagnostics* diagnostics, LowerBase base, LowerInstCall* inst) {
    if(inst->usedCount < 1) {
        diagnostics->error("missing call target in call"_v, inst->source);
        return false;
    }

    auto used = inst->used();
    auto target = base[used.ptr[0]];

    if(target->type == LowerType::Int32) {
        // Syscall.
        return true;
    } else if(target->inst()->kind == LowerInst::Fun) {
        // Static call.
        auto f = base[((LowerInstFun*)target->inst())->target];

        if(inst->usedCount != f->args.size() + 1) {
            diagnostics->error("incorrect number of arguments to call"_v, inst->source);
            return false;
        }

        if(inst->createdCount != f->returnTypes.size()) {
            diagnostics->error("incorrect number of return values from call"_v, inst->source);
            return false;
        }

        for(Size i = 1; i < used.length; i++) {
            if(base[used[i]]->type != base[f->args.get(base, i - 1)]->result.type) {
                diagnostics->error("incorrect argument type to call"_v, inst->source);
                return false;
            }
        }

        auto created = inst->created();
        for(Size i = 0; i < created.length; i++) {
            if(created[i].type != (LowerType)f->returnTypes.get(base, i)) {
                diagnostics->error("incorrect return type from call"_v, inst->source);
                return false;
            }
        }
    } else if(!isPtr(target->type)) {
        diagnostics->error("call target must be a pointer"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateBinaryBase(Diagnostics* diagnostics, LowerInstBinary* inst) {
    if(inst->usedCount != 2 || inst->createdCount != 1) {
        diagnostics->error("incorrect arguments to operation"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateArith(Diagnostics* diagnostics, LowerBase base, LowerInstBinary* inst, bool allowFloat) {
    if(!validateBinaryBase(diagnostics, inst)) return false;

    auto l = base[inst->lhs]->type;
    auto r = base[inst->rhs]->type;
    auto valid = l == r && l == inst->result.type;

    if(allowFloat) {
        valid = valid && (isInt(l) || isFloat(l));
    } else {
        valid = valid && isInt(l);
    }

    if(!valid) {
        diagnostics->error("inconsistent argument types to operation"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateAdd(Diagnostics* diagnostics, LowerBase base, LowerInstBinary* inst) {
    if(!validateBinaryBase(diagnostics, inst)) return false;

    auto l = base[inst->lhs]->type;
    auto r = base[inst->rhs]->type;
    auto result = inst->result.type;
    bool valid;

    if(isPtr(l)) {
        valid = isPtr(result) && isInt(r);
    } else if(isPtr(r)) {
        valid = isPtr(result) && isInt(l);
    } else {
        valid = l == r && l == result && (isInt(l) || isFloat(l));
    }

    if(!valid) {
        diagnostics->error("inconsistent argument types to operation"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateSub(Diagnostics* diagnostics, LowerBase base, LowerInstBinary* inst) {
    if(!validateBinaryBase(diagnostics, inst)) return false;

    auto l = base[inst->lhs]->type;
    auto r = base[inst->rhs]->type;
    auto result = inst->result.type;
    bool valid;

    if(isPtr(l) && isPtr(r)) {
        valid = isInt(result);
    } else if(isPtr(l)) {
        valid = isPtr(result) && isInt(r);
    } else {
        valid = l == r && l == result && (isInt(l) || isFloat(l));
    }

    if(!valid) {
        diagnostics->error("inconsistent argument types to operation"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateBit(Diagnostics* diagnostics, LowerBase base, LowerInstBinary* inst) {
    if(!validateBinaryBase(diagnostics, inst)) return false;

    auto l = base[inst->lhs]->type;
    auto r = base[inst->rhs]->type;
    auto result = inst->result.type;
    bool valid;

    if(isPtr(l)) {
        valid = isPtr(result) && isInt(r);
    } else if(isPtr(r)) {
        valid = isPtr(result) && isInt(l);
    } else {
        valid = l == r && l == result && isInt(l) && isInt(r);
    }

    if(!valid) {
        diagnostics->error("inconsistent argument types to operation"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateShift(Diagnostics* diagnostics, LowerBase base, LowerInstBinary* inst) {
    if(!validateBinaryBase(diagnostics, inst)) return false;

    auto l = base[inst->lhs]->type;
    auto r = base[inst->rhs]->type;
    auto valid = isInt(l) && isInt(r) && l == inst->result.type;

    if(!valid) {
        diagnostics->error("inconsistent argument types to operation"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateCmp(Diagnostics* diagnostics, LowerBase base, LowerInstCmp* inst) {
    if(inst->usedCount != 2 || inst->createdCount != 1) {
        diagnostics->error("incorrect arguments to operation"_v, inst->source);
        return false;
    }

    if(inst->result.type != LowerType::Int32) {
        diagnostics->error("incorrect result type for comparison"_v, inst->source);
        return false;
    }

    // Operands can be:
    // - two of the same integer type.
    // - two of the same float type.
    // - two pointers.
    auto l = base[inst->lhs]->type;
    auto r = base[inst->rhs]->type;
    if(l != r || !(isInt(l) || isFloat(l) || l == LowerType::Pointer)) {
        diagnostics->error("inconsistent argument types to operation"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateSelect(Diagnostics* diagnostics, LowerBase base, LowerInstSelect* inst) {
    if(inst->usedCount != 3 || inst->createdCount != 1) {
        diagnostics->error("incorrect arguments to operation"_v, inst->source);
        return false;
    }

    if(base[inst->cmp]->type != LowerType::Int32) {
        diagnostics->error("incorrect type for comparison"_v, inst->source);
        return false;
    }

    if(!(isInt(base[inst->lhs]->type) || isFloat(base[inst->lhs]->type))) {
        diagnostics->error("inconsistent argument types to operation"_v, inst->source);
        return false;
    }

    if(base[inst->lhs]->type != base[inst->rhs]->type || base[inst->lhs]->type != inst->result.type) {
        diagnostics->error("inconsistent argument types to operation"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateAlloca(Diagnostics* diagnostics, LowerBase base, LowerInstAlloca* inst) {
    if(inst->usedCount != 1 || inst->createdCount != 1) {
        diagnostics->error("incorrect arguments to operation"_v, inst->source);
        return false;
    }

    if(inst->result.type != LowerType::Pointer) {
        diagnostics->error("incorrect result type for operation"_v, inst->source);
        return false;
    }

    if(!isInt(base[inst->byteCount]->type)) {
        diagnostics->error("incorrect type for allocation size"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateLoad(Diagnostics* diagnostics, LowerBase base, LowerInstLoad* inst) {
    if(inst->usedCount != 1 || inst->createdCount != 1) {
        diagnostics->error("incorrect arguments to operation"_v, inst->source);
        return false;
    }

    if(base[inst->from]->type != LowerType::Pointer) {
        diagnostics->error("load address must be a pointer"_v, inst->source);
        return false;
    }

    if(inst->isSigned() && !isInt(inst->result.type)) {
        diagnostics->error("cannot sign-extend non-integer type"_v, inst->source);
        return false;
    }

    if(inst->result.type == LowerType::Float32 && inst->getWidth() != 4) {
        diagnostics->error("incorrect load size for float"_v, inst->source);
        return false;
    }

    if(inst->result.type == LowerType::Float64 && inst->getWidth() != 8) {
        diagnostics->error("incorrect load size for double"_v, inst->source);
        return false;
    }

    if(!Math::isPowerOf2(inst->getWidth())) {
        diagnostics->error("incorrect load size"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateStore(Diagnostics* diagnostics, LowerBase base, LowerInstStore* inst) {
    if(inst->usedCount != 2 || inst->createdCount != 0) {
        diagnostics->error("incorrect arguments to operation"_v, inst->source);
        return false;
    }

    if(base[inst->to]->type != LowerType::Pointer) {
        diagnostics->error("store address must be a pointer"_v, inst->source);
        return false;
    }

    if(base[inst->value]->type == LowerType::Float32 && inst->getWidth() != 4) {
        diagnostics->error("incorrect store size for float"_v, inst->source);
        return false;
    }

    if(base[inst->value]->type == LowerType::Float64 && inst->getWidth() != 8) {
        diagnostics->error("incorrect load size for double"_v, inst->source);
        return false;
    }

    if(!Math::isPowerOf2(inst->getWidth())) {
        diagnostics->error("incorrect store size"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateCopy(Diagnostics* diagnostics, LowerBase base, LowerInstCopy* inst) {
    if(inst->usedCount != 3 || inst->createdCount != 0) {
        diagnostics->error("incorrect arguments to operation"_v, inst->source);
        return false;
    }

    if(base[inst->to]->type != LowerType::Pointer || base[inst->from]->type != LowerType::Pointer) {
        diagnostics->error("copy source and destination must be pointers"_v, inst->source);
        return false;
    }

    if(!isInt(base[inst->count]->type)) {
        diagnostics->error("copy count must be an integer"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateSetPattern(Diagnostics* diagnostics, LowerBase base, LowerInstSetPattern* inst) {
    if(inst->usedCount != 3 || inst->createdCount != 0) {
        diagnostics->error("incorrect arguments to operation"_v, inst->source);
        return false;
    }

    if(base[inst->to]->type != LowerType::Pointer) {
        diagnostics->error("pattern target must be a pointer"_v, inst->source);
        return false;
    }

    if(!isInt(base[inst->count]->type) || !isInt(base[inst->pattern]->type)) {
        diagnostics->error("pattern and count must be integers"_v, inst->source);
        return false;
    }

    return true;
}

bool validateLowerInst(Diagnostics* diagnostics, LowerBase base, LowerBlock* block, LowerInst* inst, const DominatorTree& dominators) {
    auto isPhi = inst->kind == LowerInst::Phi;

    if(inst->kind > LowerInst::LastInst) {
        diagnostics->error("instruction has unknown kind %@"_v, inst->source, (U32)inst->kind);
        return false;
    }

    if(base[inst->block] != block) {
        diagnostics->error("instruction has incorrect block back reference"_v, inst->source);
        return false;
    }

    auto used = inst->used();
    for(Size i = 0; i < used.length; i++) {
        auto c = base[used[i]];

        if(!c->uses.contents(base).containsValue(inst - base)) {
            diagnostics->error("inconsistencies in instruction use list"_v, inst->source);
            return false;
        }

        if(base[c->inst()->block]->fun != block->fun) {
            diagnostics->error("instruction uses value from wrong function"_v, inst->source);
            return false;
        }

        // Phi nodes need special validation, since they can take values from block that _don't_ dominate.
        // They can also reference themselves, as long as the block the instruction is in
        // dominates the source block for that reference.
        // This is done in validatePhi().
        if(!isPhi) {
            if(c->inst() == inst) {
                diagnostics->error("instruction cannot reference itself"_v, inst->source);
                return false;
            }

            if(!c->dominates(base, inst, dominators)) {
                diagnostics->error("instruction uses value that doesn't dominate it"_v, inst->source);
                return false;
            }
        }
    }

    auto created = inst->created();
    for(Size i = 0; i < created.length; i++) {
        auto c = created.ptr + i;

        if(c->inst() != inst || base[c->inst()->block] != block) {
            diagnostics->error("instruction creates invalid value"_v, inst->source);
            return false;
        }

        for(auto offset: c->uses.contents(base)) {
            auto use = base[offset];

            if(use == inst && !isPhi) {
                diagnostics->error("instruction cannot reference itself"_v, use->source);
                return false;
            }

            auto found = false;

            auto useUses = use->used();
            for(Size j = 0; j < useUses.length; j++) {
                if(base[useUses[j]] == c) {
                    found = true;
                    break;
                }
            }

            if(!found) {
                diagnostics->error("inconsistencies in instruction use list"_v, use->source);
                return false;
            }
        }
    }

    switch(inst->kind) {
        case LowerInst::Arg:
            // Already validated in the function itself.
            return true;
        case LowerInst::Global:
            return validateGlobal(diagnostics, (LowerInstGlobal*)inst);
        case LowerInst::Fun:
            return validateFun(diagnostics, (LowerInstFun*)inst);
        case LowerInst::Imm:
            return validateImm(diagnostics, (LowerImm*)inst);
        case LowerInst::Nop:
            return true;
        case LowerInst::Cast:
            return validateCast(diagnostics, base, (LowerInstCast*)inst);
        case LowerInst::Bitcast:
            return validateBitcast(diagnostics, base, (LowerInstUnary*)inst);
        case LowerInst::Set:
            return validateSet(diagnostics, base, (LowerInstUnary*)inst);
        case LowerInst::Neg:
            return validateUnary(diagnostics, base, (LowerInstUnary*)inst, true);
        case LowerInst::Not:
            return validateUnary(diagnostics, base, (LowerInstUnary*)inst, false);
        case LowerInst::Add:
            return validateAdd(diagnostics, base, (LowerInstBinary*)inst);
        case LowerInst::Sub:
            return validateSub(diagnostics, base, (LowerInstBinary*)inst);
        case LowerInst::Mul:
            return validateArith(diagnostics, base, (LowerInstBinary*)inst, true);
        case LowerInst::IMul:
            return validateArith(diagnostics, base, (LowerInstBinary*)inst, false);
        case LowerInst::Div:
            return validateArith(diagnostics, base, (LowerInstBinary*)inst, true);
        case LowerInst::IDiv:
        case LowerInst::Rem:
        case LowerInst::IRem:
            return validateArith(diagnostics, base, (LowerInstBinary*)inst, false);
        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar:
            return validateShift(diagnostics, base, (LowerInstBinary*)inst);
        case LowerInst::And:
        case LowerInst::Or:
        case LowerInst::Xor:
            return validateBit(diagnostics, base, (LowerInstBinary*)inst);
        case LowerInst::Cmp:
            return validateCmp(diagnostics, base, (LowerInstCmp*)inst);
        case LowerInst::Select:
            return validateSelect(diagnostics, base, (LowerInstSelect*)inst);
        case LowerInst::Alloca:
            return validateAlloca(diagnostics, base, (LowerInstAlloca*)inst);
        case LowerInst::Load:
            return validateLoad(diagnostics, base, (LowerInstLoad*)inst);
        case LowerInst::Store:
            return validateStore(diagnostics, base, (LowerInstStore*)inst);
        case LowerInst::Copy:
            return validateCopy(diagnostics, base, (LowerInstCopy*)inst);
        case LowerInst::SetPattern:
            return validateSetPattern(diagnostics, base, (LowerInstSetPattern*)inst);
        case LowerInst::Call:
            return validateCall(diagnostics, base, (LowerInstCall*)inst);
        case LowerInst::Je:
            return validateJe(diagnostics, base, (LowerInstJe*)inst);
        case LowerInst::Jmp:
            return validateJmp(diagnostics, base, (LowerInstJmp*)inst);
        case LowerInst::Ret:
            return validateRet(diagnostics, base, (LowerInstRet*)inst);
        case LowerInst::Phi:
            return validatePhi(diagnostics, base, (LowerInstPhi*)inst, dominators);
        case LowerInst::X86Address:
        case LowerInst::X86Lea:
        case LowerInst::X86Bswap:
        case LowerInst::X86Push:
        case LowerInst::X86Pop:
        case LowerInst::X86PushArg:
            diagnostics->error("platform-lowered instruction in block"_v, inst->source);
            return false;
    }

    return true;
}

bool validateLowerBlock(Diagnostics* diagnostics, LowerBase base, LowerFunction* function, LowerBlock* block, const DominatorTree& dominators) {
    if(block->ast) {
        diagnostics->error("block is incomplete"_v, block->source);
        return false;
    }

    if(base[block->fun] != function) {
        diagnostics->error("block %@ has incorrect function back reference"_v, block->source, block);
        return false;
    }

    if(!block->terminator || !isTerminator(base[block->terminator])) {
        diagnostics->error("block is missing terminating instruction"_v, block->source);
        return false;
    }

    for(auto offset: block->phis.contents(base)) {
        auto inst = base[offset];

        if(!isPhi(inst)) {
            diagnostics->error("non-phi in list of phi instructions"_v, inst->source);
            return false;
        }

        if(!validateLowerInst(diagnostics, base, block, inst, dominators)) return false;
    }

    for(auto offset: block->instructions.contents(base)) {
        auto inst = base[offset];

        if(isPhi(inst) || isTerminator(inst)) {
            diagnostics->error("special instruction in list of standard instructions"_v, inst->source);
            return false;
        }

        if(!validateLowerInst(diagnostics, base, block, inst, dominators)) return false;
    }

    if(!validateLowerInst(diagnostics, base, block, base[block->terminator], dominators)) return false;
    return true;
}

static bool validateLowerEntryBlock(Diagnostics* diagnostics, LowerBlock* block) {
    if(block->incoming.isNotEmpty()) {
        diagnostics->error("entry block cannot be jump target"_v, block->source);
        return false;
    }

    if(block->outgoing[0] == nullptr || block->outgoing[1] != nullptr) {
        diagnostics->error("entry block must end with unconditional jump"_v, block->source);
        return false;
    }

    return true;
}

bool validateLowerGlobal(Diagnostics* diagnostics, LowerBase base, LowerGlobal* global) {
    return true;
}

bool validateLowerFunction(Diagnostics* diagnostics, LowerBase base, LowerFunction* function) {
    if(function->blocks.isEmpty()) {
        diagnostics->error("function must have at least one block"_v, function->source);
        return false;
    }

    if(function->blocks.size() > maxLimit<BlockIndex>) {
        diagnostics->error("function cannot contain more than %@ blocks"_v, function->source, maxLimit<BlockIndex>);
        return false;
    }

    auto dominators = function->buildDominatorTree(base);
    auto entryPoint = base[function->blocks.get(base, 0)];

    for(Size i = 0; i < function->args.size(); i++) {
        if(!validateLowerArg(diagnostics, base, entryPoint, base[function->args.get(base, i)], i, dominators)) return false;
    }

    if(!validateLowerEntryBlock(diagnostics, entryPoint)) return false;

    for(auto block: function->blocks.contents(base)) {
        if(!validateLowerBlock(diagnostics, base, function, base[block], dominators)) return false;
    }

    return true;
}

bool validateLowerModule(Diagnostics* diagnostics, LowerModule* module) {
    auto base = *module->arena;

    for(auto g: module->globals) {
        if(!validateLowerGlobal(diagnostics, base, base[g])) return false;
    }

    for(auto f: module->functions) {
        if(!validateLowerFunction(diagnostics, base, base[f])) return false;
    }

    return true;
}
