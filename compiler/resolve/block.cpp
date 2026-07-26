#include "block.h"
#include "module.h"

static void addUse(Module& module, ModulePtr<Value> value, Inst* user) {
    if(!value) return;
    auto base = *module.arena;
    base[value]->uses.push(module.arena, user - base);
}

// A place is used through the local it is rooted in, so that the value the storage came from -
// an alloc, or an argument - sees every read and write of any part of it.
static void addPlaceUse(Module& module, const Place& place, Inst* user) {
    auto base = *module.arena;
    auto function = base[user->block] ? base[base[user->block]->function] : nullptr;
    if(!function || place.local >= function->localCount()) return;

    addUse(module, function->localAt(base, place.local).value, user);
}

Inst* Block::add(Module& module, Inst* inst) {
    auto base = *module.arena;
    auto pointer = inst - base;

    assertTrue(inst->block == this - base);

    if(inst->kind == Value::Phi) {
        phis.push(module.arena, (InstPhi*)inst - base);
        for(auto input: ((InstPhi*)inst)->inputs.contents(base)) addUse(module, input.value, inst);
    } else if(isTerminator(*inst)) {
        assertTrue(terminator == nullptr);
        terminator = pointer;

        if(inst->kind == Value::Je) {
            auto branch = (InstJe*)inst;
            outgoing[0] = branch->thenBlock;
            outgoing[1] = branch->elseBlock;
            base[branch->thenBlock]->incoming.push(module.arena, this - base);
            base[branch->elseBlock]->incoming.push(module.arena, this - base);
            addUse(module, branch->cond, inst);
        } else if(inst->kind == Value::Jmp) {
            auto jump = (InstJmp*)inst;
            outgoing[0] = jump->target;
            base[jump->target]->incoming.push(module.arena, this - base);
        } else {
            addUse(module, ((InstRet*)inst)->value, inst);
        }
    } else {
        instructions.push(module.arena, pointer);

        switch(inst->kind) {
            case Value::LoadPlace:
                addPlaceUse(module, ((InstLoadPlace*)inst)->place, inst);
                break;
            case Value::Init: {
                auto init = (InstInit*)inst;
                addPlaceUse(module, init->place, inst);
                addUse(module, init->value, inst);
                break;
            }
            case Value::Cast:
            case Value::Neg:
            case Value::Not:
                addUse(module, ((InstUnary*)inst)->from, inst);
                break;
            case Value::Add:
            case Value::Sub:
            case Value::Mul:
            case Value::Div:
            case Value::Rem:
            case Value::Shl:
            case Value::Shr:
            case Value::Sar:
            case Value::And:
            case Value::Or:
            case Value::Xor:
            case Value::Cmp: {
                auto binary = (InstBinary*)inst;
                addUse(module, binary->lhs, inst);
                addUse(module, binary->rhs, inst);
                break;
            }
            case Value::Call:
                for(auto arg: ((InstCall*)inst)->args.contents(base)) addUse(module, arg, inst);
                break;
            case Value::GenCall:
                for(auto arg: ((InstGenCall*)inst)->args.contents(base)) addUse(module, arg, inst);
                break;
            default:
                break;
        }
    }

    return inst;
}
