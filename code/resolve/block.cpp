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
