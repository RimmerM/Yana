#include "block.h"
#include "module.h"

Value* Block::use(Value* value, Inst* user) {
    value->uses.push(Use{value, user});
    if(!value->blockUses.contains(this)) {
        value->blockUses.push(this);
    }
    return value;
}

Inst* Block::inst(Size size, Id name, Inst::Kind kind, Type* type) {
    auto inst = (Inst*)function->module->memory.alloc(size);
    inst->block = this;
    inst->name = name;
    inst->kind = kind;
    inst->type = type;
    inst->id = (U16)function->instCounter++;

    if(!complete) {
        instructions.push(inst);

        if(name) {
            namedValues[name] = inst;
        }

        if(isTerminating(kind)) {
            complete = true;
        }
    }

    return inst;
}

Value* Block::findValue(Id name) {
    auto n = namedValues.get(name);
    if(n) return *n;

    if(preceding) {
        return preceding->findValue(name);
    } else {
        for(Arg* arg: function->args) {
            if(arg->name == name) {
                return arg;
            }
        }
        return nullptr;
    }
}

Block* block(Function* fun, bool deferAdd) {
    auto block = new (fun->module->memory) Block;
    block->function = fun;
    block->id = fun->blockCounter++;

    if(!deferAdd) {
        fun->blocks.push(block);
    }

    return block;
}

void setName(Value* v, Id name) {
    v->name = name;
    if(name && v->block) {
        v->block->namedValues[name] = v;
    }
}