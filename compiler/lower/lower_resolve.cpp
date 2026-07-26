#include "lower_resolve.h"
#include "lower_validate.h"
#include "lower_inst.h"
#include "lower_parser.h"

#define assertResultCount(results, count) \
    if((results).size() != (count)) { \
        resolve.diag.error("invalid result count for instruction"_v, ast.source); \
        return Nothing(); \
    }

#define assertArgCount(args, count) \
    if((args).size() != (count)) { \
        resolve.diag.error("invalid argument count for instruction"_v, ast.source); \
        return Nothing(); \
    }

#define assertOnlyTerminator() \
    if(block.outgoing[0] || block.outgoing[1]) { \
        resolve.diag.error("duplicate terminating instruction in block"_v, ast.source); \
        return Nothing(); \
    }

static Maybe<LowerValue*> findValue(LowerResolve& resolve, LowerBase base, LowerBlock& block, const LowerArgAst& ast, LocationId source) {
    auto module = base[block.fun]->module;

    switch(ast.kind) {
        case LowerArgAst::Imm: {
            // Since i and f are in a union in both, we can always pass the integer representation to get the correct result.
            auto v = new (resolve.moduleArena) LowerImm(0, ast.immType, ast.i);
            block.addInst(base, v);

            return Just(&v->result);
        }
        case LowerArgAst::Reg: {
            auto reg = resolve.knownLocals.getValue(ast.id);
            if(!reg) resolve.diag.error("unknown local %@"_v, source, resolve.context.findName(ast.id));

            return reg ? Just(base[reg.unwrap()]) : Nothing();
        }
        case LowerArgAst::Label: {
            auto targetFun = module->functions.getValue(ast.id);
            if(!targetFun) {
                resolve.diag.error("unknown function %@"_v, source, resolve.context.findName(ast.id));
                return Nothing();
            }

            auto load = new (resolve.moduleArena) LowerInstFun(0, targetFun.unwrap());
            block.addInst(base, load);

            return Just(&load->result);
        }
        case LowerArgAst::Global: {
            auto global = module->globals.getValue(ast.id);
            if(!global) {
                resolve.diag.error("unknown global %@"_v, source, resolve.context.findName(ast.id));
                return Nothing();
            }

            auto load = new (resolve.moduleArena) LowerInstGlobal(0, global.unwrap());
            block.addInst(base, load);

            return Just(&load->result);
        }
        case LowerArgAst::Inst: {
            auto target = ast.inst;

            if(!target->gen || target->gen->createdCount < 1) {
                resolve.diag.error("undefined value as input to instruction"_v, source);
                return Nothing();
            }

            return Just(target->gen->created().ptr);
        }
    }

    assertTrue("unsupported argument type in ast" == nullptr);
    return Nothing();
}

static Maybe<LowerBlock*> findBlock(LowerResolve& resolve, LowerBase base, LowerBlock& block, StringId name, LocationId source) {
    auto target = tryMaybe(base[block.fun]->blocks.contents(base).findWhere([&](auto block) { return base[block]->name == name; }), {
        resolve.diag.error("cannot find block named %@"_v, source, resolve.context.findName(name));
        return Nothing();
    });

    return Maybe<LowerBlock*>(base[target]);
}

static Maybe<LowerBlock*> findBlock(LowerResolve& resolve, LowerBase base, LowerBlock& block, const LowerArgAst& ast, LocationId source) {
    if(ast.kind == LowerArgAst::Label) {
        return findBlock(resolve, base, block, ast.id, source);
    } else {
        resolve.diag.error("expected a block label"_v, source);
    }

    return Nothing();
}

template<LowerInst::Kind kind>
InstResolver handleImplicit() {
    return [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 1);

        auto source = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());

        if(source->inst()->kind != kind) {
            resolve.diag.error("wrong immediate type to implicit load"_v, ast.source);
            return Nothing();
        }

        source->name = getResultName(ast.results[0]);
        return Just(source->inst());
    };
}

template<LowerInst::Kind kind>
InstResolver handleUnary() {
    return [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 1);

        auto source = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto result = ast.results[0];
        auto providedType = getResultType(result);
        auto type = providedType ? providedType.unwrap() : source->type;

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstUnary(kind, getResultName(result), type, source - base)));
    };
}

template<bool signedSource, bool signedResult>
InstResolver handleCast() {
    return [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 1);

        auto source = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto result = ast.results[0];
        auto providedType = getResultType(result);
        auto type = providedType ? providedType.unwrap() : source->type;

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstCast(getResultName(result), type, source - base, signedSource, signedResult)));
    };
}

template<LowerInst::Kind kind>
InstResolver handleBinary() {
    return [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 2);

        auto lhs = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto rhs = tryMaybe(findValue(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto result = ast.results[0];
        auto providedType = getResultType(result);
        auto type = providedType ? providedType.unwrap() : lhs->type;

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstBinary(getResultName(result), type, lhs - base, rhs - base, kind)));
    };
}

template<LowerCmp cmp>
InstResolver handleCmp() {
    return [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 2);

        auto lhs = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto rhs = tryMaybe(findValue(resolve, base, block, ast.args[1], ast.source), return Nothing());
        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstCmp(getResultName(ast.results[0]), lhs - base, rhs - base, cmp)));
    };
}

template<bool signExtend>
InstResolver handleLoad() {
    return [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 2);

        auto from = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto size = ast.args[1];

        if(!size.isInt()) {
            resolve.diag.error("expected byte count for load"_v, ast.source);
            size.i = 4;
        }

        auto result = ast.results[0];
        auto type = getResultType(result);

        if(!type) {
            resolve.diag.error("unknown result type for load"_v, ast.source);
            type = Just(LowerType::Int32);
        }

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstLoad(from - base, getResultName(result), type.unwrap(), size.i, signExtend)));
    };
}

static Maybe<LowerInst*> handleIntrinsic(LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) {
    // Which intrinsic this is comes from the name rather than from a resolver of its own, so that
    // adding one to the table in lower.cpp makes it parseable with no line here.
    auto found = findLowerIntrinsic(ast.inst);
    if(!found) {
        resolve.diag.error("unknown intrinsic"_v, ast.source);
        return Nothing();
    }

    auto& desc = lowerIntrinsicDesc(found.unwrap());
    assertResultCount(ast.results, desc.results);
    assertArgCount(ast.args, desc.args);

    auto embeddedSize = sizeof(LowerValue) * desc.results + sizeof(LowerPtr<LowerValue>) * desc.args;
    auto inst = (LowerInstIntrinsic*)resolve.moduleArena.alloc(sizeof(LowerInstIntrinsic) + embeddedSize);
    new (inst) LowerInstIntrinsic(found.unwrap(), desc.results, desc.args);

    for(Size i = 0; i < desc.args; i++) {
        auto a = tryMaybe(findValue(resolve, base, block, ast.args[i], ast.source), return Nothing());
        inst->used().ptr[i] = a - base;
    }

    // An undeclared result takes the type of the first operand, which is what a one-in-one-out
    // intrinsic almost always means; one with no operands to take it from is an Int.
    auto defaultType = desc.args > 0 ? base[inst->used().ptr[0]]->type : LowerType::Int32;
    Size created = 0;

    for(auto result: ast.results.contents()) {
        auto type = getResultType(result);
        new (inst->created().ptr + created++) LowerValue(inst, type ? type.unwrap() : defaultType, getResultName(result));
    }

    return Just(block.addInst(base, (LowerInst*)inst));
}

template<LowerCallType callType>
InstResolver handleCall() {
    return [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        if(ast.args.size() < 1) {
            resolve.diag.error("cannot call without a function argument"_v, ast.source);
            return Nothing();
        }

        LowerInst* inst;
        auto createdCount = ast.results.size();
        auto usedCount = ast.args.size();
        auto embeddedSize = sizeof(LowerValue) * createdCount + sizeof(LowerPtr<LowerValue>) * usedCount;

        inst = (LowerInstCall*)resolve.moduleArena.alloc(sizeof(LowerInstCall) + embeddedSize);
        new (inst) LowerInstCall(createdCount, usedCount, callType);

        for(Size i = 0; i < ast.args.size(); i++) {
            auto a = tryMaybe(findValue(resolve, base, block, ast.args[i], ast.source), return Nothing());
            inst->used().ptr[i] = a - base;
        }

        Size currentCreated = 0;

        for(auto result: ast.results.contents()) {
            auto type = getResultType(result);

            if(!type) {
                resolve.diag.error("unknown return type for call"_v, ast.source);
                type = Just(LowerType::Int32);
            }

            new (inst->created().ptr + currentCreated++) LowerValue(inst, type.unwrap(), getResultName(result));
        }

        return Just(block.addInst(base, inst));
    };
}

LowerResolve::LowerResolve(Diagnostics& diag, Context& context, Region<LowerRegion>& moduleArena, Region<LowerParserRegion>& parserArena):
    diag(diag), context(context), moduleArena(moduleArena), parserArena(parserArena)
{
    /*
     * Note that we don't do any type checking yet in the initial resolve pass -
     * some instructions cannot be fully resolved until all blocks have been created,
     * so their type is unknown until then.
     */

    instructionSet.add(Context::nameHash("imm"_v), handleImplicit<LowerInst::Imm>());
    instructionSet.add(Context::nameHash("fun"_v), handleImplicit<LowerInst::Fun>());
    instructionSet.add(Context::nameHash("global"_v), handleImplicit<LowerInst::Global>());

    instructionSet.add(Context::nameHash("nop"_v), [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 0);
        assertArgCount(ast.args, 0);

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInst(LowerInst::Nop)));
    });

    instructionSet.add(Context::nameHash("cast"_v), handleCast<false, false>());
    instructionSet.add(Context::nameHash("sext"_v), handleCast<true, true>());
    instructionSet.add(Context::nameHash("ftoi"_v), handleCast<false, true>());
    instructionSet.add(Context::nameHash("itof"_v), handleCast<true, false>());
    instructionSet.add(Context::nameHash("bitcast"_v), handleUnary<LowerInst::Bitcast>());

    instructionSet.add(Context::nameHash("set"_v), handleUnary<LowerInst::Set>());
    instructionSet.add(Context::nameHash("neg"_v), handleUnary<LowerInst::Neg>());
    instructionSet.add(Context::nameHash("not"_v), handleUnary<LowerInst::Not>());

    instructionSet.add(Context::nameHash("add"_v), handleBinary<LowerInst::Add>());
    instructionSet.add(Context::nameHash("sub"_v), handleBinary<LowerInst::Sub>());
    instructionSet.add(Context::nameHash("mul"_v), handleBinary<LowerInst::Mul>());
    instructionSet.add(Context::nameHash("imul"_v), handleBinary<LowerInst::IMul>());
    instructionSet.add(Context::nameHash("div"_v), handleBinary<LowerInst::Div>());
    instructionSet.add(Context::nameHash("idiv"_v), handleBinary<LowerInst::IDiv>());
    instructionSet.add(Context::nameHash("rem"_v), handleBinary<LowerInst::Rem>());
    instructionSet.add(Context::nameHash("irem"_v), handleBinary<LowerInst::IRem>());
    instructionSet.add(Context::nameHash("shl"_v), handleBinary<LowerInst::Shl>());
    instructionSet.add(Context::nameHash("shr"_v), handleBinary<LowerInst::Shr>());
    instructionSet.add(Context::nameHash("sar"_v), handleBinary<LowerInst::Sar>());
    instructionSet.add(Context::nameHash("and"_v), handleBinary<LowerInst::And>());
    instructionSet.add(Context::nameHash("or"_v), handleBinary<LowerInst::Or>());
    instructionSet.add(Context::nameHash("xor"_v), handleBinary<LowerInst::Xor>());

    instructionSet.add(Context::nameHash("cmp_eq"_v), handleCmp<LowerCmp::eq>());
    instructionSet.add(Context::nameHash("cmp_neq"_v), handleCmp<LowerCmp::neq>());
    instructionSet.add(Context::nameHash("cmp_gt"_v), handleCmp<LowerCmp::gt>());
    instructionSet.add(Context::nameHash("cmp_ge"_v), handleCmp<LowerCmp::ge>());
    instructionSet.add(Context::nameHash("cmp_lt"_v), handleCmp<LowerCmp::lt>());
    instructionSet.add(Context::nameHash("cmp_le"_v), handleCmp<LowerCmp::le>());
    instructionSet.add(Context::nameHash("cmp_igt"_v), handleCmp<LowerCmp::igt>());
    instructionSet.add(Context::nameHash("cmp_ige"_v), handleCmp<LowerCmp::ige>());
    instructionSet.add(Context::nameHash("cmp_ilt"_v), handleCmp<LowerCmp::ilt>());
    instructionSet.add(Context::nameHash("cmp_ile"_v), handleCmp<LowerCmp::ile>());

    instructionSet.add(Context::nameHash("select"_v), [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 3);

        auto cmp = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto lhs = tryMaybe(findValue(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto rhs = tryMaybe(findValue(resolve, base, block, ast.args[2], ast.source), return Nothing());
        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstSelect(getResultName(ast.results[0]), lhs - base, rhs - base, cmp - base, lhs->type)));
    });

    instructionSet.add(Context::nameHash("alloca"_v), [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 1);

        auto size = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstAlloca(getResultName(ast.results[0]), size - base)));
    });

    instructionSet.add(Context::nameHash("load"_v), handleLoad<false>());
    instructionSet.add(Context::nameHash("loads"_v), handleLoad<true>());

    instructionSet.add(Context::nameHash("store"_v), [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 0);
        assertArgCount(ast.args, 3);

        auto to = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto from = tryMaybe(findValue(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto size = ast.args[2];

        if(!size.isInt()) {
            resolve.diag.error("expected byte count for store"_v, ast.source);
            size.i = 4;
        }

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstStore(to - base, from - base, size.i)));
    });

    instructionSet.add(Context::nameHash("copy"_v), [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 0);
        assertArgCount(ast.args, 3);

        auto to = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto from = tryMaybe(findValue(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto count = tryMaybe(findValue(resolve, base, block, ast.args[2], ast.source), return Nothing());

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstCopy(to - base, from - base, count - base)));
    });

    instructionSet.add(Context::nameHash("setpattern"_v), [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 0);
        assertArgCount(ast.args, 3);

        auto to = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto count = tryMaybe(findValue(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto pattern = tryMaybe(findValue(resolve, base, block, ast.args[2], ast.source), return Nothing());

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstSetPattern(to - base, count - base, pattern - base)));
    });

    // There is deliberately no `push`: which arguments travel on the stack is the calling
    // convention's answer, not the author's, and transformFunction derives the stores from it (see
    // insertStackArgs in codegen/x64/transform.cpp). A hand-written one could disagree with the
    // convention, and the callee would read its arguments from somewhere the caller never wrote.
    instructionSet.add(Context::nameHash("call"_v), handleCall<kDefaultCallType>());
    instructionSet.add(Context::nameHash("call_sysv"_v), handleCall<LowerCallType::Sysv>());
    instructionSet.add(Context::nameHash("call_win64"_v), handleCall<LowerCallType::Win64>());
    instructionSet.add(Context::nameHash("call_simple"_v), handleCall<LowerCallType::Simple>());
    instructionSet.add(Context::nameHash("call_complex"_v), handleCall<LowerCallType::Complex>());
    instructionSet.add(Context::nameHash("call_clobber"_v), handleCall<LowerCallType::Clobber>());
    instructionSet.add(Context::nameHash("syscall"_v), handleCall<LowerCallType::Syscall>());

    // Every intrinsic the IR names, without a line of its own: the table in lower.cpp is the one
    // statement of which exist, and this loop is what makes adding one there enough.
    for(Size i = 0; i < kLowerIntrinsicCount; i++) {
        instructionSet.add(Context::nameHash(lowerIntrinsicDesc(LowerIntrinsic(i)).name), handleIntrinsic);
    }

    instructionSet.add(Context::nameHash("je"_v), [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 0);
        assertArgCount(ast.args, 3);
        assertOnlyTerminator();

        auto cmp = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto lhs = tryMaybe(findBlock(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto rhs = tryMaybe(findBlock(resolve, base, block, ast.args[2], ast.source), return Nothing());

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstJe(cmp - base, lhs - base, rhs - base)));
    });

    instructionSet.add(Context::nameHash("jmp"_v), [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 0);
        assertArgCount(ast.args, 1);
        assertOnlyTerminator();

        auto lhs = tryMaybe(findBlock(resolve, base, block, ast.args[0], ast.source), return Nothing());
        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstJmp(lhs - base)));
    });

    instructionSet.add(Context::nameHash("ret"_v), [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 0);
        assertOnlyTerminator();

        auto inst = (LowerInstRet*)resolve.moduleArena.alloc(sizeof(LowerInstRet) + sizeof(LowerPtr<LowerValue>) * ast.args.size());
        new (inst) LowerInstRet;

        auto used = inst->used();

        for(auto arg: ast.args.contents()) {
            auto a = tryMaybe(findValue(resolve, base, block, arg, ast.source), return Nothing());
            used.ptr[inst->usedCount++] = a - base;
        }

        return Just(block.addInst(base, inst));
    });

    instructionSet.add(Context::nameHash("phi"_v), [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);

        // Phis can use locals from blocks that don't exist yet, so they are stored in a queue,
        // and arguments added once the rest of the function has been resolved.
        // For this reason, a result type needs to be provided explicitly.
        auto result = ast.results[0];
        auto type = getResultType(result);

        if(!type) {
            resolve.diag.error("unknown result type for phi"_v, ast.source);
            type = Just(LowerType::Int32);
        }

        // Allocate the node with enough space for all its arguments -
        // they need to be embedded into the same allocation.
        // resolveLowerPhi() will write the actual argument contents and set the used count;
        // otherwise we might accidentally read from the still uninitialized argument buffer.
        auto embeddedSize = (sizeof(LowerPtr<LowerValue>) + sizeof(LowerPtr<LowerBlock>)) * ast.args.size();
        auto phi = (LowerInst*)resolve.moduleArena.alloc(sizeof(LowerInstPhi) + embeddedSize);
        new (phi) LowerInstPhi(getResultName(result), type.unwrap());

        phi->block = &block - base;
        resolve.pending.push(resolve.parserArena, LowerPendingInst { phi - base, &ast - *resolve.parserArena });

        return Just(phi);
    });
}

static void resolveLowerInst(LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) {
    auto resolver = resolve.instructionSet.getValue(ast.inst);
    if(!resolver) {
        resolve.diag.error("unknown instruction %@"_v, ast.source, resolve.context.findName(ast.inst));
        return;
    }

    auto inst = tryMaybe(resolver.unwrap()(resolve, base, block, ast), return);
    inst->source = ast.source;
    ast.gen = inst;

    for(auto& created: inst->created()) {
        if(created.name) {
            auto result = resolve.knownLocals.add(created.name);
            if(result.existed) {
                resolve.diag.error("duplicate identifier %@"_v, ast.source, resolve.context.findName(created.name));
            } else {
                *result.value = &created - base;
            }
        }
    }
}

static void resolveLowerPhi(LowerResolve& resolve, LowerBase base, LowerInstPhi& phi, LowerInstAst& ast) {
    // Set the correct use count first, so we can get the correct memory offsets.
    phi.usedCount = ast.args.size();

    auto block = base[phi.block];
    auto used = phi.used().ptr;
    auto sources = phi.sources().ptr;
    Size index = 0;

    for(auto arg: ast.args.contents()) {
        if(!arg.source) {
            resolve.diag.error("missing block specifier in phi argument"_v, ast.source);
            return;
        }

        auto sourceBlock = tryMaybe(findBlock(resolve, base, *block, arg.source, ast.source), return);

        // Resolve the value within the source block.
        // This ensures that any implicit instructions (like loading a large constant)
        // are performed only when arriving from that block.
        auto a = tryMaybe(findValue(resolve, base, *sourceBlock, arg, ast.source), return);

        used[index] = a - base;
        sources[index] = sourceBlock - base;
        phi.result.type = a->type;
        index++;
    }

    block->addInst(base, &phi);
}

void resolveLowerBlock(LowerResolve& resolve, LowerBase moduleBase, RegionBase<LowerParserRegion> parserBase, LowerBlock& block) {
    auto ast = block.ast;
    if(!ast) return;

    // Reset the ast value here to prevent recursive calls.
    block.ast = nullptr;

    for(auto inst: parserBase[ast]->instructions.contents(parserBase)) {
        resolveLowerInst(resolve, moduleBase, block, *parserBase[inst]);
    }
}

bool resolveLowerModule(LowerResolve& resolve, LowerBase moduleBase, RegionBase<LowerParserRegion> parserBase, LowerModule& module) {
    auto errorCount = resolve.diag.errorCount();
    auto warningCount = resolve.diag.warningCount();

    for(auto offset: module.functions) {
        resolve.knownLocals.clear();
        resolve.pending.clear();

        auto f = moduleBase[offset];

        if(f->blocks.isEmpty()) {
            resolve.diag.error("function %@ contains no entry block."_v, f->source, resolve.context.findName(f->name));
            continue;
        }

        for(auto arg: f->args.contents(moduleBase)) {
            resolve.knownLocals.add(moduleBase[arg]->result.name, &moduleBase[arg]->result - moduleBase);
        }

        for(auto block: f->blocks.contents(moduleBase)) {
            resolveLowerBlock(resolve, moduleBase, parserBase, *moduleBase[block]);
        }

        for(auto pending: resolve.pending.contents(parserBase)) {
            auto inst = moduleBase[pending.inst];
            auto ast = parserBase[pending.ast];

            if(inst->kind == LowerInst::Phi) {
                assertTrue(pending.ast != nullptr);
                resolveLowerPhi(resolve, moduleBase, *(LowerInstPhi*)inst, *ast);
            } else {
                assertTrue("unknown pending instruction type" == nullptr);
            }
        }
    }

    module.errorCount += resolve.diag.errorCount() - errorCount;
    module.warningCount += resolve.diag.warningCount() - warningCount;
    if(module.errorCount > 0) return false;

    return validateLowerModule(&resolve.diag, &module);
}
