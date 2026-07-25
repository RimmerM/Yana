#include "block.h"
#include "module.h"

Value* Block::use(Module* module, Value* value, Inst* user) {
    value->uses.push(module->memory, user - module->local);
    return value;
}

Value* Block::inst(Module* module, Size size, StringId name, Inst::Kind kind, Type* type) {
    auto inst = (Value*)module->memory.alloc(size);
    new (inst) Value(kind, this - module->local, type - module->global);

    inst->name = name;
    inst->id = (U16)module->local[function]->instCounter++;

    if(isTerminator(kind)) {
        assertTrue(terminator == nullptr);
        terminator = (Inst*)inst - module->local;
    } else if(isPhi(kind)) {
        phis.push(module->memory, (InstPhi*)inst - module->local);
    } else {
        instructions.push(module->memory, (Inst*)inst - module->local);
    }

    if(name) {
        namedValues[name] = inst;
    }

    return inst;
}

Value* Block::findValue(Module* module, StringId name) {
    auto n = namedValues.get(name);
    if(n) return n.unwrap();

    if(preceding) {
        return module->local[preceding]->findValue(module, name);
    } else {
        auto fun = module->local[function];

        for(auto arg: fun->args.contents(module->local)) {
            auto a = module->local[arg];
            if(a->name == name) {
                return a;
            }
        }
        return nullptr;
    }
}

Block* block(Module* module, Function* fun, bool deferAdd) {
    auto block = new (module->memory) Block(fun - module->local, fun->blockCounter++);

    if(!deferAdd) {
        fun->blocks.push(module->memory, block - module->local);
    }

    return block;
}

void setName(Module* module, Value* v, StringId name) {
    v->name = name;
    if(name && v->block) {
        module->local[v->block]->namedValues[name] = v;
    }
}
