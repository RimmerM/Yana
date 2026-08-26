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
            auto v = new (resolve.moduleArena) LowerImm(StringId(), ast.immType, ast.i);
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

            auto load = new (resolve.moduleArena) LowerInstFun(StringId(), targetFun.unwrap());
            block.addInst(base, load);

            return Just(&load->result);
        }
        case LowerArgAst::Global: {
            auto global = module->globals.getValue(ast.id);
            if(!global) {
                resolve.diag.error("unknown global %@"_v, source, resolve.context.findName(ast.id));
                return Nothing();
            }

            auto load = new (resolve.moduleArena) LowerInstGlobal(StringId(), global.unwrap());
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

// One successor of a conditional branch, together with what the source said about how likely it is.
struct LowerEdgeAst {
    LowerBlock* block;
    EdgeLikelihood likelihood;
};

// A branch target, written either as a bare block label or as `[block, weight]` - see
// EdgeLikelihood. The weight is relative to the branch's other edge and to nothing else, so a
// branch states both of them or neither; one on its own would be a ratio with nothing to be a ratio
// against, and the caller checks for that.
//
// The bracket form is the one the parser already accepts for a phi's `[block, value]`, which is why
// it needs no syntax of its own: the block arrives as the argument's `source` and the weight as the
// argument itself.
static Maybe<LowerEdgeAst> findEdge(LowerResolve& resolve, LowerBase base, LowerBlock& block, const LowerArgAst& ast, LocationId source) {
    if(!ast.source) {
        auto target = tryMaybe(findBlock(resolve, base, block, ast, source), return Nothing());
        return Just(LowerEdgeAst { target, EdgeLikelihood {} });
    }

    auto target = tryMaybe(findBlock(resolve, base, block, ast.source, source), return Nothing());

    if(!ast.isInt() || ast.i < 1 || ast.i > kMaxEdgeWeight) {
        resolve.diag.error("expected a positive branch weight within the supported range"_v, source);
        return Nothing();
    }

    // A weight written into the IR is a claim by whoever produced it. A static estimate is derived
    // rather than written down, and a measured one has no way in yet, so this is the one source the
    // text format can express.
    return Just(LowerEdgeAst { target, EdgeLikelihood { U32(ast.i), LikelihoodSource::FrontendHint } });
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

/*
 * Registering a resolver under the name its kind is printed as.
 *
 * The mnemonic comes from the kind's row rather than from a literal here, which is what makes a row
 * in inst.def the one place an instruction's name is written: the printer reads the same field, so a
 * name changed in one place cannot stop round-tripping through the other.
 *
 * `addKind` is the general form and takes whatever resolver the kind needs; `addUnary` and
 * `addBinary` are it with the shared handler filled in, which between them is most of the roster.
 * What is *not* registered through any of these is every name the text format spells differently
 * from the kind - a cast's four, a comparison's twelve, a load's three, the atomics' orders, the
 * ten reductions, the eight SHA operations, a call's seven conventions. Those are refinements of one
 * kind into several names, so there is no row to read them from and they are written out below.
 */
template<LowerInst::Kind kind>
void addKind(HashMap<StringId, InstResolver>& set, InstResolver resolver) {
    set.add(Context::nameHash(lowerInstTraits(kind).mnemonic), resolver);
}

template<LowerInst::Kind kind>
void addUnary(HashMap<StringId, InstResolver>& set) {
    addKind<kind>(set, handleUnary<kind>());
}

template<LowerInst::Kind kind>
void addBinary(HashMap<StringId, InstResolver>& set) {
    addKind<kind>(set, handleBinary<kind>());
}

/*
 * The SHA extension's two shapes, so that what the printer writes can be read back.
 *
 * Two resolvers rather than one because the arity differs: `sha256rnds2` names three vectors and the
 * other seven name two, which is the same split the kinds have.
 */
template<LowerSha op>
InstResolver handleSha() {
    return [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 2);

        auto lhs = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto rhs = tryMaybe(findValue(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto result = ast.results[0];
        auto providedType = getResultType(result);
        auto type = providedType ? providedType.unwrap() : lhs->type;

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstShaBinary(
            getResultName(result), type, lhs - base, rhs - base, op)));
    };
}

InstResolver handleSha256Rounds() {
    return [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 3);

        auto state = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto feed = tryMaybe(findValue(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto keys = tryMaybe(findValue(resolve, base, block, ast.args[2], ast.source), return Nothing());
        auto result = ast.results[0];
        auto providedType = getResultType(result);
        auto type = providedType ? providedType.unwrap() : state->type;

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstSha256Rounds(
            getResultName(result), type, state - base, feed - base, keys - base)));
    };
}

template<LowerCmp cmp>
InstResolver handleCmp() {
    return [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 2);

        auto lhs = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto rhs = tryMaybe(findValue(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto result = ast.results[0];
        auto provided = getResultType(result);

        // A comparison of vectors answers a mask of their shape, which the text does not have to
        // state - there is only one mask it could be - and which it may state anyway.
        auto type = provided ? provided.unwrap()
            : isVectorLike(lhs->type) ? maskType(lhs->type.lane, lhs->type.lanes())
            : LowerType::Int32;

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstCmp(getResultName(result), lhs - base, rhs - base, cmp, type)));
    };
}

template<bool signExtend, bool overread>
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
            type = justType(LowerType::Int32);
        }

        auto load = new (resolve.moduleArena) LowerInstLoad(from - base, getResultName(result), type.unwrap(), size.i, signExtend);
        if(overread) load->setOverread();

        return Just(block.addInst(base, load));
    };
}

/*
 * The atomics - see LowerInst::FirstAtomic.
 *
 * An order arrives as a bare identifier, which the argument parser hands over as a `Label`. That is
 * the same argument kind a function name uses, so it deliberately does *not* go through
 * `findValue`: resolving `acquire` as a value would look for a function of that name and report an
 * unknown one. It is a field, read here exactly as a load's byte count is.
 *
 * The spellings are LLVM's rather than the library's - `seq_cst` and not `LoadSequential` - because
 * a `.lower` file is read beside the backend output it produces, and one vocabulary across the two
 * is worth more than agreement with a surface the file cannot mention anyway.
 */
static Maybe<LowerOrder> orderArgument(LowerResolve& resolve, LowerInstAst& ast, const LowerArgAst& arg) {
    if(arg.kind != LowerArgAst::Label) {
        resolve.diag.error("expected a memory order"_v, ast.source);
        return Nothing();
    }

    if(arg.id == Context::nameHash("relaxed"_v)) return Just(LowerOrder::Relaxed);
    if(arg.id == Context::nameHash("acquire"_v)) return Just(LowerOrder::Acquire);
    if(arg.id == Context::nameHash("release"_v)) return Just(LowerOrder::Release);
    if(arg.id == Context::nameHash("acq_rel"_v)) return Just(LowerOrder::AcquireRelease);
    if(arg.id == Context::nameHash("seq_cst"_v)) return Just(LowerOrder::Sequential);

    resolve.diag.error("unknown memory order %@; expected one of relaxed, acquire, release, acq_rel, seq_cst"_v,
                       ast.source, resolve.context.findName(arg.id));
    return Nothing();
}

// The byte count an atomic access carries, read exactly as a load's is. Held to a power of two here
// so that a malformed width cannot reach `makeMemoryFlags`, which encodes its logarithm; every
// further restriction - which widths a target can perform atomically at all - is the verifier's.
static U32 atomicWidthArgument(LowerResolve& resolve, LowerInstAst& ast, const LowerArgAst& arg) {
    if(!arg.isInt() || arg.i == 0 || (arg.i & (arg.i - 1)) != 0) {
        resolve.diag.error("expected a power-of-two byte count for an atomic access"_v, ast.source);
        return 4;
    }

    return U32(arg.i);
}

template<bool signExtend>
InstResolver handleAtomicLoad() {
    return [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 3);

        auto from = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto width = atomicWidthArgument(resolve, ast, ast.args[1]);
        auto order = tryMaybe(orderArgument(resolve, ast, ast.args[2]), return Nothing());

        auto result = ast.results[0];
        auto type = getResultType(result);

        if(!type) {
            resolve.diag.error("unknown result type for atomic load"_v, ast.source);
            type = justType(LowerType::Int32);
        }

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstAtomicLoad(
            from - base, getResultName(result), type.unwrap(), width, signExtend, order)));
    };
}

template<LowerAtomicOp op>
InstResolver handleAtomicRmw() {
    return [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 4);

        auto to = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto value = tryMaybe(findValue(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto width = atomicWidthArgument(resolve, ast, ast.args[2]);
        auto order = tryMaybe(orderArgument(resolve, ast, ast.args[3]), return Nothing());

        auto result = ast.results[0];
        auto type = getResultType(result);

        // The previous value has the operand's type unless the text says otherwise - there is only
        // one type it could have, a read-modify-write answering what it read.
        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstAtomicRmw(
            to - base, value - base, getResultName(result),
            type ? type.unwrap() : base[value - base]->type, width, op, order)));
    };
}

/*
 * Compare-exchange, whose text form states both orders always.
 *
 * There is deliberately no one-order spelling that derives the second. §3.5's projection is a rule
 * about what a *caller* may leave unsaid, and both of the library forms - the plain one and
 * `Advanced`'s - arrive here with two orders already chosen; a text form that could also derive one
 * would be a third source of the same fact. It also means a dump round-trips and that reading the
 * failure path off one does not require applying a table in your head.
 */
template<bool weak>
InstResolver handleAtomicCas() {
    return [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 2);
        assertArgCount(ast.args, 6);

        auto to = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto expected = tryMaybe(findValue(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto desired = tryMaybe(findValue(resolve, base, block, ast.args[2], ast.source), return Nothing());
        auto width = atomicWidthArgument(resolve, ast, ast.args[3]);
        auto success = tryMaybe(orderArgument(resolve, ast, ast.args[4]), return Nothing());
        auto failure = tryMaybe(orderArgument(resolve, ast, ast.args[5]), return Nothing());

        auto previous = ast.results[0];
        auto exchanged = ast.results[1];
        auto previousType = getResultType(previous);
        auto exchangedType = getResultType(exchanged);

        auto inst = (LowerInstAtomicCas*)resolve.moduleArena.alloc(sizeof(LowerInstAtomicCas));
        new (inst) LowerInstAtomicCas(
            getResultName(previous), getResultName(exchanged),
            previousType ? previousType.unwrap() : base[expected - base]->type,
            to - base, expected - base, desired - base, width, weak, success, failure,
            exchangedType ? exchangedType.unwrap() : LowerType::Int32);

        return Just(block.addInst(base, inst));
    };
}

/*
 * The vector instructions.
 *
 * The lane index and the shuffle pattern are fields rather than operands, so they are read out of
 * the argument list here rather than resolved through findValue - which would turn each of them
 * into an `imm` instruction and a value the allocator has to place. That is the same thing a load's
 * byte count and an alloca's alignment already do, and it is why they read as trailing numbers.
 */
static Maybe<U8> laneArgument(LowerResolve& resolve, LowerInstAst& ast, const LowerArgAst& arg, U32 lanes) {
    if(!arg.isInt() || arg.i < 0 || U64(arg.i) >= lanes) {
        resolve.diag.error("expected a lane index within the vector"_v, ast.source);
        return Nothing();
    }

    return Just(U8(arg.i));
}

template<LowerReduce reduce>
InstResolver handleReduce() {
    return [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 1);

        auto from = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto result = ast.results[0];
        auto provided = getResultType(result);

        // A reduction of a vector answers in the lane's scalar form, which is the only type it could
        // answer in - so the text does not have to say so, and says so only where it differs.
        auto type = provided ? provided.unwrap() : scalarFormOf(from->type);

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstVecReduce(
            getResultName(result), type, from - base, reduce)));
    };
}

static void addVectorInstructions(HashMap<StringId, InstResolver>& instructionSet) {
    addKind<LowerInst::VecSplat>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 1);

        auto from = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto result = ast.results[0];
        auto type = getResultType(result);

        // The one vector instruction whose result type cannot be derived from its operand: how many
        // lanes a scalar is splatted into is exactly what the instruction says and the scalar does
        // not.
        if(!type) {
            resolve.diag.error("unknown result type for vsplat"_v, ast.source);
            type = justType(LowerType::Int32);
        }

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstVecSplat(
            getResultName(result), type.unwrap(), from - base)));
    });

    addKind<LowerInst::VecLane>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 2);

        auto from = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto lane = tryMaybe(laneArgument(resolve, ast, ast.args[1], from->type.lanes()), return Nothing());
        auto result = ast.results[0];
        auto provided = getResultType(result);
        auto type = provided ? provided.unwrap() : scalarFormOf(from->type);

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstVecLane(
            getResultName(result), type, from - base, lane)));
    });

    addKind<LowerInst::VecWithLane>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 3);

        auto from = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto value = tryMaybe(findValue(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto lane = tryMaybe(laneArgument(resolve, ast, ast.args[2], from->type.lanes()), return Nothing());
        auto result = ast.results[0];
        auto provided = getResultType(result);
        auto type = provided ? provided.unwrap() : from->type;

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstVecLane(
            getResultName(result), type, from - base, lane, value - base)));
    });

    addKind<LowerInst::VecShuffle>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);

        if(ast.args.size() < 3) {
            resolve.diag.error("expected two vectors and a lane pattern for vshuffle"_v, ast.source);
            return Nothing();
        }

        auto left = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto right = tryMaybe(findValue(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto result = ast.results[0];
        auto provided = getResultType(result);

        // The pattern has one entry per lane of the result, so writing a different number of them
        // than the result has lanes is a mistake rather than a shorthand. Where the result type is
        // not stated it is the sources' - a shuffle that changes the lane count has to say so.
        auto type = provided ? provided.unwrap() : left->type;
        auto pattern = ast.args.size() - 2;

        if(pattern != type.lanes()) {
            resolve.diag.error("a vshuffle pattern needs one entry per lane of its result"_v, ast.source);
            return Nothing();
        }

        auto inst = (LowerInstVecShuffle*)resolve.moduleArena.alloc(
            sizeof(LowerInstVecShuffle) + LowerInstVecShuffle::patternBytes(type));
        new (inst) LowerInstVecShuffle(getResultName(result), type, left - base, right - base);

        // A pattern entry names a lane of the two sources concatenated, so it runs to twice the
        // source's lane count rather than to the result's.
        auto sourceLanes = left->type.lanes() * 2;

        for(Size i = 0; i < pattern; i++) {
            auto lane = tryMaybe(laneArgument(resolve, ast, ast.args[i + 2], sourceLanes), return Nothing());
            inst->pattern()[i] = lane;
        }

        return Just(block.addInst(base, (LowerInst*)inst));
    });

    instructionSet.add(Context::nameHash("vreduce_add"_v), handleReduce<LowerReduce::Add>());
    instructionSet.add(Context::nameHash("vreduce_mul"_v), handleReduce<LowerReduce::Mul>());
    instructionSet.add(Context::nameHash("vreduce_min"_v), handleReduce<LowerReduce::Min>());
    instructionSet.add(Context::nameHash("vreduce_imin"_v), handleReduce<LowerReduce::IMin>());
    instructionSet.add(Context::nameHash("vreduce_max"_v), handleReduce<LowerReduce::Max>());
    instructionSet.add(Context::nameHash("vreduce_imax"_v), handleReduce<LowerReduce::IMax>());
    instructionSet.add(Context::nameHash("vreduce_and"_v), handleReduce<LowerReduce::And>());
    instructionSet.add(Context::nameHash("vreduce_bits"_v), handleReduce<LowerReduce::Bits>());
    instructionSet.add(Context::nameHash("vreduce_first"_v), handleReduce<LowerReduce::FirstSet>());
    instructionSet.add(Context::nameHash("vreduce_or"_v), handleReduce<LowerReduce::Or>());
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
                type = justType(LowerType::Int32);
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

    addKind<LowerInst::Imm>(instructionSet, handleImplicit<LowerInst::Imm>());
    addKind<LowerInst::Fun>(instructionSet, handleImplicit<LowerInst::Fun>());
    addKind<LowerInst::Global>(instructionSet, handleImplicit<LowerInst::Global>());

    addKind<LowerInst::Nop>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 0);
        assertArgCount(ast.args, 0);

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInst(LowerInst::Nop)));
    });

    // `vzeroupper` - `nop`'s shape, and here so that a `.lower` fixture can round-trip one: the
    // printer emits the name, so the parser has to accept it or a golden could not be regenerated.
    addKind<LowerInst::VZeroUpper>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 0);
        assertArgCount(ast.args, 0);

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstVZeroUpper()));
    });

    instructionSet.add(Context::nameHash("cast"_v), handleCast<false, false>());
    instructionSet.add(Context::nameHash("sext"_v), handleCast<true, true>());
    instructionSet.add(Context::nameHash("ftoi"_v), handleCast<false, true>());
    instructionSet.add(Context::nameHash("itof"_v), handleCast<true, false>());
    addUnary<LowerInst::Bitcast>(instructionSet);

    addUnary<LowerInst::Set>(instructionSet);
    addUnary<LowerInst::Neg>(instructionSet);
    addUnary<LowerInst::Not>(instructionSet);
    addUnary<LowerInst::Bswap>(instructionSet);
    addUnary<LowerInst::Sqrt>(instructionSet);
    addUnary<LowerInst::Abs>(instructionSet);
    addUnary<LowerInst::Trunc>(instructionSet);
    addUnary<LowerInst::Floor>(instructionSet);
    addUnary<LowerInst::Ceil>(instructionSet);
    addUnary<LowerInst::Round>(instructionSet);

    // The one three-operand arithmetic instruction, so the one that cannot borrow a handler. Its
    // result type is the operands' - all three and the result are one type, which validateFma
    // checks - so the text does not have to state it and states it only where it differs.
    addKind<LowerInst::Fma>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 3);

        auto a = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto b = tryMaybe(findValue(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto c = tryMaybe(findValue(resolve, base, block, ast.args[2], ast.source), return Nothing());

        auto result = ast.results[0];
        auto provided = getResultType(result);
        auto type = provided ? provided.unwrap() : a->type;

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstFma(
            getResultName(result), type, a - base, b - base, c - base)));
    });

    addBinary<LowerInst::Add>(instructionSet);
    addBinary<LowerInst::Sub>(instructionSet);
    addBinary<LowerInst::Mul>(instructionSet);
    addBinary<LowerInst::IMul>(instructionSet);
    addBinary<LowerInst::Div>(instructionSet);
    addBinary<LowerInst::IDiv>(instructionSet);
    addBinary<LowerInst::Rem>(instructionSet);
    addBinary<LowerInst::IRem>(instructionSet);
    addBinary<LowerInst::MulHi>(instructionSet);
    addBinary<LowerInst::IMulHi>(instructionSet);
    addBinary<LowerInst::Shl>(instructionSet);
    addBinary<LowerInst::Shr>(instructionSet);
    addBinary<LowerInst::Sar>(instructionSet);
    addBinary<LowerInst::Rol>(instructionSet);
    addBinary<LowerInst::Ror>(instructionSet);
    addBinary<LowerInst::And>(instructionSet);
    addBinary<LowerInst::Or>(instructionSet);
    addBinary<LowerInst::Xor>(instructionSet);
    addBinary<LowerInst::BitsUpTo>(instructionSet);
    addBinary<LowerInst::GatherBits>(instructionSet);
    addBinary<LowerInst::ScatterBits>(instructionSet);
    addBinary<LowerInst::Crc32>(instructionSet);

    // The SHA extension, under the names the printer writes - see nameOfLowerSha.
    instructionSet.add(Context::nameHash("sha1msg1"_v), handleSha<LowerSha::Sha1Msg1>());
    instructionSet.add(Context::nameHash("sha1msg2"_v), handleSha<LowerSha::Sha1Msg2>());
    instructionSet.add(Context::nameHash("sha1nexte"_v), handleSha<LowerSha::Sha1NextE>());
    instructionSet.add(Context::nameHash("sha1rnds4_0"_v), handleSha<LowerSha::Sha1Rounds0>());
    instructionSet.add(Context::nameHash("sha1rnds4_1"_v), handleSha<LowerSha::Sha1Rounds1>());
    instructionSet.add(Context::nameHash("sha1rnds4_2"_v), handleSha<LowerSha::Sha1Rounds2>());
    instructionSet.add(Context::nameHash("sha1rnds4_3"_v), handleSha<LowerSha::Sha1Rounds3>());
    instructionSet.add(Context::nameHash("sha256msg1"_v), handleSha<LowerSha::Sha256Msg1>());
    instructionSet.add(Context::nameHash("sha256msg2"_v), handleSha<LowerSha::Sha256Msg2>());
    instructionSet.add(Context::nameHash("sha256rnds2"_v), handleSha256Rounds());

    instructionSet.add(Context::nameHash("cmp_eq"_v), handleCmp<LowerCmp::eq>());
    instructionSet.add(Context::nameHash("cmp_neq"_v), handleCmp<LowerCmp::neq>());
    instructionSet.add(Context::nameHash("cmp_uno"_v), handleCmp<LowerCmp::uno>());
    instructionSet.add(Context::nameHash("cmp_ord"_v), handleCmp<LowerCmp::ord>());
    instructionSet.add(Context::nameHash("cmp_gt"_v), handleCmp<LowerCmp::gt>());
    instructionSet.add(Context::nameHash("cmp_ge"_v), handleCmp<LowerCmp::ge>());
    instructionSet.add(Context::nameHash("cmp_lt"_v), handleCmp<LowerCmp::lt>());
    instructionSet.add(Context::nameHash("cmp_le"_v), handleCmp<LowerCmp::le>());
    instructionSet.add(Context::nameHash("cmp_igt"_v), handleCmp<LowerCmp::igt>());
    instructionSet.add(Context::nameHash("cmp_ige"_v), handleCmp<LowerCmp::ige>());
    instructionSet.add(Context::nameHash("cmp_ilt"_v), handleCmp<LowerCmp::ilt>());
    instructionSet.add(Context::nameHash("cmp_ile"_v), handleCmp<LowerCmp::ile>());

    addKind<LowerInst::Select>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);
        assertArgCount(ast.args, 3);

        auto cmp = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto lhs = tryMaybe(findValue(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto rhs = tryMaybe(findValue(resolve, base, block, ast.args[2], ast.source), return Nothing());
        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstSelect(getResultName(ast.results[0]), lhs - base, rhs - base, cmp - base, lhs->type)));
    });

    // `alloca %bytes, alignment`. The alignment is optional in the text format and defaults to 8,
    // which is what a scalar or a pointer needs - anything wanting more says so.
    addKind<LowerInst::Alloca>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);

        if(ast.args.size() != 1 && ast.args.size() != 2) {
            resolve.diag.error("invalid argument count for instruction"_v, ast.source);
            return Nothing();
        }

        U32 alignment = 8;

        if(ast.args.size() == 2) {
            auto arg = ast.args[1];
            auto valid = arg.isInt() && arg.i > 0 && (arg.i & (arg.i - 1)) == 0;

            if(!valid) {
                resolve.diag.error("expected a power-of-two alignment for alloca"_v, ast.source);
            } else {
                alignment = U32(arg.i);
            }
        }

        auto size = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstAlloca(
            getResultName(ast.results[0]), size - base, alignment)));
    });

    addVectorInstructions(instructionSet);

    addKind<LowerInst::Load>(instructionSet, handleLoad<false, false>());
    instructionSet.add(Context::nameHash("loads"_v), handleLoad<true, false>());

    // A load that deliberately reads past the end of what it names - see isOverreadLoad. Unsigned
    // only: it is what a vector loop's last iteration does, and a vector load has no sign.
    instructionSet.add(Context::nameHash("loadx"_v), handleLoad<false, true>());

    addKind<LowerInst::Store>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
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

    /*
     * The atomics. `atomic_load %p, 4, acquire`, `atomic_store %p, %v, 4, release`,
     * `atomic_add %p, %v, 4, relaxed`, `%old, %ok = atomic_cas %p, %e, %d, 4, acq_rel`.
     *
     * The width and the order are trailing fields on every one of them, in that order, which is the
     * shape a load and an alloca already read: operands first, then what the instruction knows
     * about itself. A fence has no operands at all and is therefore just its order.
     */
    addKind<LowerInst::AtomicLoad>(instructionSet, handleAtomicLoad<false>());
    instructionSet.add(Context::nameHash("atomic_loads"_v), handleAtomicLoad<true>());

    addKind<LowerInst::AtomicStore>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 0);
        assertArgCount(ast.args, 4);

        auto to = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto value = tryMaybe(findValue(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto width = atomicWidthArgument(resolve, ast, ast.args[2]);
        auto order = tryMaybe(orderArgument(resolve, ast, ast.args[3]), return Nothing());

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstAtomicStore(
            to - base, value - base, width, order)));
    });

    instructionSet.add(Context::nameHash("atomic_xchg"_v), handleAtomicRmw<LowerAtomicOp::Exchange>());
    instructionSet.add(Context::nameHash("atomic_add"_v), handleAtomicRmw<LowerAtomicOp::Add>());
    instructionSet.add(Context::nameHash("atomic_sub"_v), handleAtomicRmw<LowerAtomicOp::Sub>());
    instructionSet.add(Context::nameHash("atomic_and"_v), handleAtomicRmw<LowerAtomicOp::And>());
    instructionSet.add(Context::nameHash("atomic_or"_v), handleAtomicRmw<LowerAtomicOp::Or>());
    instructionSet.add(Context::nameHash("atomic_xor"_v), handleAtomicRmw<LowerAtomicOp::Xor>());

    instructionSet.add(Context::nameHash("atomic_cas"_v), handleAtomicCas<false>());
    instructionSet.add(Context::nameHash("atomic_cas_weak"_v), handleAtomicCas<true>());

    addKind<LowerInst::Fence>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 0);
        assertArgCount(ast.args, 1);

        auto order = tryMaybe(orderArgument(resolve, ast, ast.args[0]), return Nothing());
        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstFence(order)));
    });

    addKind<LowerInst::SpinHint>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 0);
        assertArgCount(ast.args, 0);

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstSpinHint()));
    });

    addKind<LowerInst::Copy>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 0);
        assertArgCount(ast.args, 3);

        auto to = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto from = tryMaybe(findValue(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto count = tryMaybe(findValue(resolve, base, block, ast.args[2], ast.source), return Nothing());

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstCopy(to - base, from - base, count - base)));
    });

    addKind<LowerInst::SetPattern>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
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
    addKind<LowerInst::Call>(instructionSet, handleCall<kDefaultCallType>());
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

    addKind<LowerInst::Je>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 0);
        assertArgCount(ast.args, 3);
        assertOnlyTerminator();

        auto cmp = tryMaybe(findValue(resolve, base, block, ast.args[0], ast.source), return Nothing());
        auto lhs = tryMaybe(findEdge(resolve, base, block, ast.args[1], ast.source), return Nothing());
        auto rhs = tryMaybe(findEdge(resolve, base, block, ast.args[2], ast.source), return Nothing());

        // A weight is relative to the other edge, so one without the other says nothing.
        auto stated = lhs.likelihood.source != LikelihoodSource::Unknown;
        if(stated != (rhs.likelihood.source != LikelihoodSource::Unknown)) {
            resolve.diag.error("a branch states a weight for both of its edges or for neither"_v, ast.source);
            return Nothing();
        }

        auto je = new (resolve.moduleArena) LowerInstJe(cmp - base, lhs.block - base, rhs.block - base);
        je->likelihood[0] = lhs.likelihood;
        je->likelihood[1] = rhs.likelihood;

        return Just(block.addInst(base, je));
    });

    addKind<LowerInst::Jmp>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 0);
        assertArgCount(ast.args, 1);
        assertOnlyTerminator();

        auto lhs = tryMaybe(findBlock(resolve, base, block, ast.args[0], ast.source), return Nothing());
        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstJmp(lhs - base)));
    });

    addKind<LowerInst::Ret>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
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

    // The end of a block control never leaves. No operands, since there is nothing to return and
    // nowhere to go - see LowerInstUnreachable.
    addKind<LowerInst::Unreachable>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 0);
        assertArgCount(ast.args, 0);
        assertOnlyTerminator();

        return Just(block.addInst(base, new (resolve.moduleArena) LowerInstUnreachable()));
    });

    addKind<LowerInst::Phi>(instructionSet, [](LowerResolve& resolve, LowerBase base, LowerBlock& block, LowerInstAst& ast) -> Maybe<LowerInst*> {
        assertResultCount(ast.results, 1);

        // Phis can use locals from blocks that don't exist yet, so they are stored in a queue,
        // and arguments added once the rest of the function has been resolved.
        // For this reason, a result type needs to be provided explicitly.
        auto result = ast.results[0];
        auto type = getResultType(result);

        if(!type) {
            resolve.diag.error("unknown result type for phi"_v, ast.source);
            type = justType(LowerType::Int32);
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

    for(auto offset: module.functionOrder) {
        resolve.knownLocals.reset();
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
