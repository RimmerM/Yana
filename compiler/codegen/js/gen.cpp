#include "ast.h"
#include "../../compiler/context.h"
#include "../../resolve/module.h"

namespace js {

struct Loop {
    Loop* parent;
    Block* startBlock;
    Block* endBlock;
    U32 id;
};

struct Gen {
    Arena& mem;
    Context* context;
    U32 globalId;

    U32 localId = 0;
    Array<Stmt*>* block = nullptr;
    Loop* loop = nullptr;

    Id lengthId;
    Id assignOp;
    Id addOp, subOp, mulOp, divOp, remOp;
    Id shlOp, shrOp, sarOp, andOp, orOp, xorOp;
    Id eqOp, neqOp, gtOp, geOp, ltOp, leOp;
    Id negOp, notOp;
};

Stmt* genBlock(Gen* gen, Block* block);
Stmt* useFunction(Gen* gen, Function* fun);

Expr* useValue(Gen* gen, Value* value) {
    auto v = (Variable*)value->codegen;
    if(v) {
        v->refCount++;
        return new (gen->mem) VarExpr(v);
    }

    switch(value->kind) {
        case Value::ConstInt:
            return new (gen->mem) IntExpr(((ConstInt*)value)->value);
        case Value::ConstFloat:
            return new (gen->mem) FloatExpr(((ConstFloat*)value)->value);
        case Value::ConstString:
            return new (gen->mem) StringExpr(((ConstString*)value)->name);
    }

    // If this happens, some instruction is not generated correctly.
    return nullptr;
}

Variable* store(Gen* gen, Expr* value) {
    auto var = new (gen->mem) Variable;
    var->name = 0;
    var->localId = gen->localId++;
    var->refCount = 0;
    gen->block->push(new (gen->mem) DeclStmt(var, value));
    return var;
}

Variable* genTrunc(Gen* gen, InstTrunc* inst) {
    return store(gen, useValue(gen, inst->from));
}

Variable* genFTrunc(Gen* gen, InstFTrunc* inst) {
    return store(gen, useValue(gen, inst->from));
}

Variable* genZExt(Gen* gen, InstZExt* inst) {
    return store(gen, useValue(gen, inst->from));
}

Variable* genSExt(Gen* gen, InstSExt* inst) {
    return store(gen, useValue(gen, inst->from));
}

Variable* genFExt(Gen* gen, InstFExt* inst) {
    return store(gen, useValue(gen, inst->from));
}

Variable* genFToI(Gen* gen, InstFToI* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->orOp, useValue(gen, inst->from), new (gen->mem) IntExpr(0)));
}

Variable* genFToUI(Gen* gen, InstFToUI* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->orOp, useValue(gen, inst->from), new (gen->mem) IntExpr(0)));
}

Variable* genIToF(Gen* gen, InstIToF* inst) {
    return store(gen, useValue(gen, inst->from));
}

Variable* genUIToF(Gen* gen, InstUIToF* inst) {
    return store(gen, useValue(gen, inst->from));
}

Variable* genAdd(Gen* gen, InstAdd* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->addOp, useValue(gen, inst->lhs), useValue(gen, inst->rhs)));
}

Variable* genSub(Gen* gen, InstSub* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->subOp, useValue(gen, inst->lhs), useValue(gen, inst->rhs)));
}

Variable* genMul(Gen* gen, InstMul* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->mulOp, useValue(gen, inst->lhs), useValue(gen, inst->rhs)));
}

Variable* genDiv(Gen* gen, InstDiv* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->divOp, useValue(gen, inst->lhs), useValue(gen, inst->rhs)));
}

Variable* genIDiv(Gen* gen, InstIDiv* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->divOp, useValue(gen, inst->lhs), useValue(gen, inst->rhs)));
}

Variable* genRem(Gen* gen, InstRem* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->remOp, useValue(gen, inst->lhs), useValue(gen, inst->rhs)));
}

Variable* genIRem(Gen* gen, InstIRem* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->remOp, useValue(gen, inst->lhs), useValue(gen, inst->rhs)));
}

Variable* genFAdd(Gen* gen, InstFAdd* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->addOp, useValue(gen, inst->lhs), useValue(gen, inst->rhs)));
}

Variable* genFSub(Gen* gen, InstFSub* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->subOp, useValue(gen, inst->lhs), useValue(gen, inst->rhs)));
}

Variable* genFMul(Gen* gen, InstFMul* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->mulOp, useValue(gen, inst->lhs), useValue(gen, inst->rhs)));
}

Variable* genFDiv(Gen* gen, InstFDiv* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->divOp, useValue(gen, inst->lhs), useValue(gen, inst->rhs)));
}

Variable* genICmp(Gen* gen, InstICmp* inst) {
    Id op;
    switch(inst->cmp) {
        case ICmp::eq:
            op = gen->eqOp;
            break;
        case ICmp::neq:
            op = gen->neqOp;
            break;
        case ICmp::gt:
            op = gen->gtOp;
            break;
        case ICmp::ge:
            op = gen->geOp;
            break;
        case ICmp::lt:
            op = gen->ltOp;
            break;
        case ICmp::le:
            op = gen->leOp;
            break;
        case ICmp::igt:
            op = gen->gtOp;
            break;
        case ICmp::ige:
            op = gen->geOp;
            break;
        case ICmp::ilt:
            op = gen->ltOp;
            break;
        case ICmp::ile:
            op = gen->leOp;
            break;
    }
    return store(gen, new (gen->mem) InfixExpr(op, useValue(gen, inst->lhs), useValue(gen, inst->rhs)));
}

Variable* genFCmp(Gen* gen, InstFCmp* inst) {
    Id op;
    switch(inst->cmp) {
        case FCmp::eq:
            op = gen->eqOp;
            break;
        case FCmp::neq:
            op = gen->neqOp;
            break;
        case FCmp::gt:
            op = gen->gtOp;
            break;
        case FCmp::ge:
            op = gen->geOp;
            break;
        case FCmp::lt:
            op = gen->ltOp;
            break;
        case FCmp::le:
            op = gen->leOp;
            break;
    }
    return store(gen, new (gen->mem) InfixExpr(op, useValue(gen, inst->lhs), useValue(gen, inst->rhs)));
}

Variable* genShl(Gen* gen, InstShl* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->shlOp, useValue(gen, inst->arg), useValue(gen, inst->amount)));
}

Variable* genShr(Gen* gen, InstShr* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->shrOp, useValue(gen, inst->arg), useValue(gen, inst->amount)));
}

Variable* genSar(Gen* gen, InstSar* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->sarOp, useValue(gen, inst->arg), useValue(gen, inst->amount)));
}

Variable* genAnd(Gen* gen, InstAnd* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->andOp, useValue(gen, inst->lhs), useValue(gen, inst->rhs)));
}

Variable* genOr(Gen* gen, InstOr* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->orOp, useValue(gen, inst->lhs), useValue(gen, inst->rhs)));
}

Variable* genXor(Gen* gen, InstXor* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->xorOp, useValue(gen, inst->lhs), useValue(gen, inst->rhs)));
}

Variable* genAddRef(Gen* gen, InstAddRef* inst) {
    return store(gen, new (gen->mem) InfixExpr(gen->addOp, useValue(gen, inst->lhs), useValue(gen, inst->rhs)));
}

Variable* genRecord(Gen* gen, InstRecord* inst) {
    return nullptr;
}

Variable* genTup(Gen* gen, InstTup* inst) {
    return nullptr;
}

Variable* genAlloc(Gen* gen, InstAlloc* inst) {
    auto var = new (gen->mem) Variable;
    var->localId = gen->localId++;
    var->refCount = 0;
    var->name = inst->name;

    gen->block->push(new (gen->mem) DeclStmt(var, nullptr));
    return var;
}

Variable* genLoad(Gen* gen, InstLoad* inst) {
    auto value = useValue(gen, inst->from);
    return store(gen, value);
}

Variable* genStore(Gen* gen, InstStore* inst) {
    auto to = useValue(gen, inst->to);
    auto value = useValue(gen, inst->value);
    auto assign = new (gen->mem) AssignExpr(gen->assignOp, to, value);
    gen->block->push(new (gen->mem) ExprStmt(assign));
    return nullptr;
}

Variable* genGetField(Gen* gen, InstGetField* inst) {
    return nullptr;
}

Variable* genStringLength(Gen* gen, InstStringLength* inst) {
    return store(gen, new (gen->mem) FieldExpr(useValue(gen, inst->from), new (gen->mem) StringExpr(gen->lengthId)));
}

Variable* genStringData(Gen* gen, InstStringData* inst) {
    return store(gen, useValue(gen, inst->from));
}

Variable* genCall(Gen* gen, InstCall* inst) {
    return nullptr;
}

Variable* genCallDyn(Gen* gen, InstCallDyn* inst) {
    return nullptr;
}

Variable* genJe(Gen* gen, InstJe* inst) {
    auto success = inst->then;
    auto fail = inst->otherwise;

    if(success->loop || fail->loop) {

        auto cond = new (gen->mem) IfStmt(useValue(gen, inst->cond), nullptr, nullptr);

    }

    if(success->succeeding == fail->succeeding) {

    }

    return nullptr;
}

Variable* genJmp(Gen* gen, InstJmp* inst) {
    if(inst->to->incoming.size() <= 1) {
        auto block = genBlock(gen, inst->to);
        if(block->type == Stmt::Block) {
            auto list = (BlockStmt*)block;
            for(U32 i = 0; i < list->count; i++) {
                gen->block->push(list->stmts[i]);
            }
        } else {
            gen->block->push(block);
        }
    } else if(inst->to->loop) {
        // As long as the branches between blocks follow the requirements,
        // the loop start will always be resolved before any jumps to it.
        assertTrue(gen->loop != nullptr);

        // If this jumps back into the current loop, we don't have to do anything
        // as the corresponding while/for will just go to the next iteration.
        // If this jumps back into a nested loop, we have to insert a continue to its label.
        if(inst->to == gen->loop->startBlock) {
            // Do nothing - the language semantics will loop automatically.
        } else {
            auto loop = gen->loop->parent;
            while(loop) {
                if(inst->to == loop->startBlock) break;
                loop = loop->parent;
            }

            // This has to exist in valid source for the same reason as above.
            assertTrue(loop != nullptr);

            loop->startBlock->codegen;
        }
    } else {
        gen->context->diagnostics.error("internal error: unsupported jump type"_buffer, nullptr, noSource);
    }

    return nullptr;
}

Variable* genRet(Gen* gen, InstRet* inst) {
    auto value = inst->value ? useValue(gen, inst->value) : nullptr;
    gen->block->push(new (gen->mem) ReturnStmt(value));
    return nullptr;
}

Variable* genPhi(Gen* gen, InstPhi* inst) {
    return nullptr;
}

Variable* genInstValue(Gen* gen, Inst* inst) {
    switch(inst->kind) {
        case Inst::InstTrunc:
            return genTrunc(gen, (InstTrunc*)inst);
        case Inst::InstFTrunc:
            return genFTrunc(gen, (InstFTrunc*)inst);
        case Inst::InstZExt:
            return genZExt(gen, (InstZExt*)inst);
        case Inst::InstSExt:
            return genSExt(gen, (InstSExt*)inst);
        case Inst::InstFExt:
            return genFExt(gen, (InstFExt*)inst);
        case Inst::InstFToI:
            return genFToI(gen, (InstFToI*)inst);
        case Inst::InstFToUI:
            return genFToUI(gen, (InstFToUI*)inst);
        case Inst::InstIToF:
            return genIToF(gen, (InstIToF*)inst);
        case Inst::InstUIToF:
            return genUIToF(gen, (InstUIToF*)inst);
        case Inst::InstAdd:
            return genAdd(gen, (InstAdd*)inst);
        case Inst::InstSub:
            return genSub(gen, (InstSub*)inst);
        case Inst::InstMul:
            return genMul(gen, (InstMul*)inst);
        case Inst::InstDiv:
            return genDiv(gen, (InstDiv*)inst);
        case Inst::InstIDiv:
            return genIDiv(gen, (InstIDiv*)inst);
        case Inst::InstRem:
            return genRem(gen, (InstRem*)inst);
        case Inst::InstIRem:
            return genIRem(gen, (InstIRem*)inst);
        case Inst::InstFAdd:
            return genFAdd(gen, (InstFAdd*)inst);
        case Inst::InstFSub:
            return genFSub(gen, (InstFSub*)inst);
        case Inst::InstFMul:
            return genFMul(gen, (InstFMul*)inst);
        case Inst::InstFDiv:
            return genFDiv(gen, (InstFDiv*)inst);
        case Inst::InstICmp:
            return genICmp(gen, (InstICmp*)inst);
        case Inst::InstFCmp:
            return genFCmp(gen, (InstFCmp*)inst);
        case Inst::InstShl:
            return genShl(gen, (InstShl*)inst);
        case Inst::InstShr:
            return genShr(gen, (InstShr*)inst);
        case Inst::InstSar:
            return genSar(gen, (InstSar*)inst);
        case Inst::InstAnd:
            return genAnd(gen, (InstAnd*)inst);
        case Inst::InstOr:
            return genOr(gen, (InstOr*)inst);
        case Inst::InstXor:
            return genXor(gen, (InstXor*)inst);
        case Inst::InstAddRef:
            return genAddRef(gen, (InstAddRef*)inst);
        case Inst::InstRecord:
            return genRecord(gen, (InstRecord*)inst);
        case Inst::InstTup:
            return genTup(gen, (InstTup*)inst);
        case Inst::InstAlloc:
            return genAlloc(gen, (InstAlloc*)inst);
        case Inst::InstLoad:
            return genLoad(gen, (InstLoad*)inst);
        case Inst::InstStore:
            return genStore(gen, (InstStore*)inst);
        case Inst::InstGetField:
            return genGetField(gen, (InstGetField*)inst);
        case Inst::InstStringLength:
            return genStringLength(gen, (InstStringLength*)inst);
        case Inst::InstStringData:
            return genStringData(gen, (InstStringData*)inst);
        case Inst::InstCall:
            return genCall(gen, (InstCall*)inst);
        case Inst::InstCallDyn:
            return genCallDyn(gen, (InstCallDyn*)inst);
        case Inst::InstJe:
            return genJe(gen, (InstJe*)inst);
        case Inst::InstJmp:
            return genJmp(gen, (InstJmp*)inst);
        case Inst::InstRet:
            return genRet(gen, (InstRet*)inst);
        case Inst::InstPhi:
            return genPhi(gen, (InstPhi*)inst);
    }
    return nullptr;
}

Variable* genInst(Gen* gen, Inst* inst) {
    auto value = genInstValue(gen, inst);
    inst->codegen = value;
    return value;
}

Stmt* genBlock(Gen* gen, Block* block) {
    auto fun = (FunStmt*)block->function->codegen;

    Loop loop;
    if(block->loop) {
        loop.parent = gen->loop;
        loop.startBlock = block;
        loop.endBlock = block->succeeding;

        gen->loop = &loop;
    }

    Array<Stmt*> statements;
    auto prevBlock = gen->block;
    auto prevId = gen->localId;

    gen->localId = fun->idCounter;
    gen->block = &statements;

    for(auto inst: block->instructions) {
        genInst(gen, inst);
    }

    fun->idCounter = gen->localId;
    gen->localId = prevId;
    gen->block = prevBlock;

    if(block->loop) {
        gen->loop = loop.parent;
    }

    if(statements.size() == 1) {
        return statements[0];
    } else {
        auto s = (Stmt**)gen->mem.alloc(sizeof(Stmt*) * statements.size());
        for(U32 i = 0; i < statements.size(); i++) {
            s[i] = statements[i];
        }
        return new (gen->mem) BlockStmt(s, statements.size());
    }
}

Stmt* genFunction(Gen* gen, Function* fun) {
    auto argCount = fun->args.size();
    auto args = (Variable*)gen->mem.alloc(argCount * sizeof(Variable));

    auto f = new (gen->mem) FunStmt(fun->name, gen->globalId++, args, argCount, nullptr);
    fun->codegen = f;

    for(U32 i = 0; i < argCount; i++) {
        args[i].name = fun->args[i]->name;
        args[i].refCount = 0;
        args[i].localId = f->idCounter++;
        fun->args[i]->codegen = args + i;
    }

    f->body = genBlock(gen, fun->blocks[0]);
    return f;
}

Stmt* useFunction(Gen* gen, Function* fun) {
    auto v = (Stmt*)fun->codegen;
    if(v) return v;

    return genFunction(gen, fun);
}

Stmt* genGlobal(Gen* gen, Global* global) {
    auto var = new (gen->mem) Variable;
    var->name = global->name;
    var->localId = gen->globalId++;
    var->refCount = 0;

    auto decl = new (gen->mem) DeclStmt(var, nullptr);
    global->codegen = decl;
    return decl;
}

static void addOps(Context* context, Gen& gen) {
    gen.lengthId = context->addUnqualifiedName("length", 6);
    gen.assignOp = context->addUnqualifiedName("=", 1);

    gen.addOp = context->addUnqualifiedName("+", 1);
    gen.subOp = context->addUnqualifiedName("-", 1);
    gen.mulOp = context->addUnqualifiedName("*", 1);
    gen.divOp = context->addUnqualifiedName("/", 1);
    gen.remOp = context->addUnqualifiedName("%", 1);

    gen.shlOp = context->addUnqualifiedName("<<", 2);
    gen.shrOp = context->addUnqualifiedName(">>", 2);
    gen.sarOp = context->addUnqualifiedName(">>>", 3);
    gen.andOp = context->addUnqualifiedName("&", 1);
    gen.orOp = context->addUnqualifiedName("|", 1);
    gen.xorOp = context->addUnqualifiedName("^", 1);

    gen.eqOp = context->addUnqualifiedName("===", 3);
    gen.neqOp = context->addUnqualifiedName("!==", 3);
    gen.gtOp = context->addUnqualifiedName(">", 1);
    gen.geOp = context->addUnqualifiedName(">=", 2);
    gen.ltOp = context->addUnqualifiedName("<", 1);
    gen.leOp = context->addUnqualifiedName("<=", 2);

    gen.negOp = context->addUnqualifiedName("-", 1);
    gen.notOp = context->addUnqualifiedName("!", 1);
}

File* genModule(Context* context, Module* module) {
    Gen gen{context->exprArena, context, 0};
    addOps(context, gen);

    auto file = new (gen.mem) File;
    for(auto& global: module->globals) {
        file->statements.push(genGlobal(&gen, &global));
    }

    for(auto& fun: module->functions) {
        // Functions can be lazily generated depending on their usage order, so we only generate if needed.
        file->statements.push(useFunction(&gen, &fun));
    }

    for(const InstanceMap& map: module->classInstances) {
        for(U32 i = 0; i < map.instances.size(); i++) {
            ClassInstance* instance = map.instances[i];
            auto instances = instance->instances;
            auto funCount = instance->typeClass->gen.funCount;

            for(U32 j = 0; j < funCount; j++) {
                file->statements.push(useFunction(&gen, instances[j]));
            }
        }
    }

    return file;
}

} // namespace js
