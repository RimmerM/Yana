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

/*
 * What a value's type may be at all, before anything is asked about the instruction holding it.
 *
 * Two rules, and both are about the lane types that exist only as lanes. An 8- or 16-bit *scalar*
 * arrives in a 32-bit register and states its width on the access that reads it, which is the rule
 * every pass here is written against; a vector of pointers is not something this IR can express, and
 * a mask of one lane is a Bool, which is an Int32. Each of the three would otherwise be a type that
 * looks legal and that no backend has a register class for.
 */
static bool validateValueType(Diagnostics* diagnostics, LowerType type, LocationId source) {
    if(!isVectorLike(type)) {
        if(type.lane == LowerLane::Int8 || type.lane == LowerLane::Int16) {
            diagnostics->error("an 8- or 16-bit value can only be a vector lane"_v, source);
            return false;
        }

        return true;
    }

    if(type.lane == LowerLane::Pointer) {
        diagnostics->error("a vector of pointers has no representation"_v, source);
        return false;
    }

    if(type.laneShift == 0) {
        diagnostics->error("a mask of one lane is a Bool"_v, source);
        return false;
    }

    // The widest register any described target has. A wider type would have no home, and the
    // arithmetic that computes a lane count from a byte width would silently produce it.
    if(type.byteWidth() > 64) {
        diagnostics->error("a vector wider than 64 bytes has no register"_v, source);
        return false;
    }

    return true;
}

static bool validateImm(Diagnostics* diagnostics, LowerImm* inst) {
    if(isPtr(inst->result.type)) {
        diagnostics->error("cannot create immediate value of pointer type"_v, inst->source);
        return false;
    }

    // A vector constant is `lanes` numbers rather than one, and an immediate holds one word. What
    // produces one is a load of a pooled constant, which is an ordinary load of ordinary data.
    if(isVectorLike(inst->result.type)) {
        diagnostics->error("a vector constant is not an immediate"_v, inst->source);
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

    /*
     * A conversion between two vectors preserves the lane count, and this is where that is said.
     *
     * It has to be said, because nothing else distinguishes the two things it could mean: a `cast`
     * from `f32x8` to `f64x8` is a widening of every lane into a register twice as wide, and one to
     * `f64x4` is the *low half* widened - which is `unpackLow` and a cast, two instructions with
     * different operands. Design-Vector §3.4 makes the first one what a conversion is; the second
     * is spelled as the shuffle it is.
     */
    if(isVectorLike(result) || isVectorLike(source)) {
        if(result.laneShift != source.laneShift || result.isMask() || source.isMask()) {
            diagnostics->error("a conversion between vectors must preserve the lane count"_v, inst->source);
            return false;
        }

        return true;
    }

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

    /*
     * A bitcast is about bits, so a vector one is legal exactly where the two are the same width -
     * `i8x16` to `i32x4` reinterprets one register, and `i8x16` to `i32x8` names two.
     *
     * A **mask** is legal only against another mask of the *same shape*, which is stricter than the
     * width rule beside it. Since a mask stopped normalizing its element (Design-Vector §2.4),
     * `Mask(Float)` and `Mask(Int)` are two types over one register and this is what relates them -
     * so refusing every mask, which is what this did, refused the operation that replaced the
     * normalization. What it may not do is change the lane shape: an `m8x16` and an `m32x4` are one
     * register and sixteen truth values against four, so reinterpreting one as the other is a
     * repacking and not a renaming. `laneShift` is the whole of the test, the width being equal by
     * construction once the shift and the count are.
     */
    if(isVectorLike(source) || isVectorLike(result)) {
        if(source.isMask() != result.isMask()) {
            diagnostics->error("incompatible cast types"_v, inst->source);
            return false;
        }

        if(source.isMask() && source.laneShift != result.laneShift) {
            diagnostics->error("a bitcast between masks must preserve the lane shape"_v, inst->source);
            return false;
        }

        if(source.byteWidth() != result.byteWidth()) {
            diagnostics->error("incompatible cast types"_v, inst->source);
            return false;
        }

        return true;
    }

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

    // `allowFloat` is what separates the two callers, and it now separates more than it says:
    // `neg` takes anything arithmetic, and `not` takes anything whose lanes are bits - which is
    // where a mask comes in, and where a float does not.
    bool valid = allowFloat
        ? (isInt(source) || isFloat(source) || isIntVector(source) || isFloatVector(source))
        : (isInt(source) || isIntVector(source) || source.isMask());

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

/*
 * A byte reversal, whose one rule beyond agreeing with its operand is that there is a whole number
 * of bytes to reverse.
 *
 * `Int32` or `Int64`, which is every scalar integer this IR has - the point being that a *narrower*
 * one does not exist here, so nothing below this can be handed a 16-bit swap by mistake. A vector is
 * refused for the reason the resolve verifier refuses one: reversing every lane is a shuffle against
 * a pattern, not this instruction at a wider type.
 */
static bool validateBswap(Diagnostics* diagnostics, LowerBase base, LowerInstUnary* inst) {
    auto result = inst->result.type;

    if(result != LowerType::Int32 && result != LowerType::Int64) {
        diagnostics->error("a byte reversal is defined on 32- and 64-bit integers only"_v, inst->source);
        return false;
    }

    if(base[inst->from]->type != result) {
        diagnostics->error("inconsistent argument types to operation"_v, inst->source);
        return false;
    }

    return true;
}

/*
 * A square root, and the three-operand multiply-add.
 *
 * Both are floats and nothing else, at any width and any lane count: a square root of an integer is
 * a question about rounding no machine answers, and a fused multiply-add of two is the ordinary two
 * instructions with nothing fused about them.
 */
static bool validateFloatOnly(Diagnostics* diagnostics, LowerType type, LowerInst* inst) {
    if(isFloat(type) || isFloatVector(type)) return true;

    diagnostics->error("this operation is defined on floating-point values only"_v, inst->source);
    return false;
}

static bool validateSqrt(Diagnostics* diagnostics, LowerBase base, LowerInstUnary* inst) {
    if(!validateFloatOnly(diagnostics, inst->result.type, inst)) return false;

    if(base[inst->from]->type != inst->result.type) {
        diagnostics->error("inconsistent argument types to operation"_v, inst->source);
        return false;
    }

    return true;
}

/*
 * The magnitude, whose rule is what is left of the resolver's after the language types are gone.
 *
 * A float or an integer, scalar or vector, and the operand's type is the result's. The *signedness*
 * of an integer lane cannot be checked here and is not this validator's to check: a lane kind has
 * forgotten it by now, and `verifyFunction` has already refused an unsigned one - see the row in
 * resolve/inst.def.
 */
static bool validateAbs(Diagnostics* diagnostics, LowerBase base, LowerInstUnary* inst) {
    auto type = inst->result.type;

    if(!isFloat(type) && !isFloatVector(type) && !isIntLike(type) && !isIntVector(type)) {
        diagnostics->error("the magnitude is defined on numbers and vectors of them only"_v, inst->source);
        return false;
    }

    if(base[inst->from]->type != type) {
        diagnostics->error("inconsistent argument types to operation"_v, inst->source);
        return false;
    }

    return true;
}

static bool validateFma(Diagnostics* diagnostics, LowerBase base, LowerInstFma* inst) {
    if(!validateFloatOnly(diagnostics, inst->result.type, inst)) return false;

    if(inst->usedCount != 3) {
        diagnostics->error("incorrect arguments to operation"_v, inst->source);
        return false;
    }

    // All three and the result are one type. There is no widening here and no mixed precision: what
    // the operation promises is a single rounding of one expression, which says nothing useful if
    // the operands had to be converted to reach each other first.
    for(auto offset: inst->used()) {
        if(base[offset]->type != inst->result.type) {
            diagnostics->error("inconsistent argument types to operation"_v, inst->source);
            return false;
        }
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

    // A weight is a ratio against the sibling edge, so both edges state one or neither does - an
    // edge weighted alone would be a ratio with nothing to compare against. The frequency analysis
    // divides by the pair's total, which is also why neither may be zero.
    for(auto& likelihood: inst->likelihood) {
        auto stated = likelihood.source != LikelihoodSource::Unknown;

        if(stated != inst->hasLikelihood()) {
            diagnostics->error("a branch states an edge weight for both of its edges or for neither"_v, inst->source);
            return false;
        }

        if(likelihood.weight < 1 || likelihood.weight > kMaxEdgeWeight) {
            diagnostics->error("branch edge weight out of range"_v, inst->source);
            return false;
        }
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

// An intrinsic's meaning is the target's, so what can be checked without one is its shape: that it
// names an intrinsic that exists, and that as many values go in and come out as that intrinsic has.
// Whether the target can actually select it is the form table's answer, not this one's.
static bool validateIntrinsic(Diagnostics* diagnostics, LowerBase base, LowerInstIntrinsic* inst) {
    if(Size(inst->getIntrinsic()) >= kLowerIntrinsicCount) {
        diagnostics->error("unknown intrinsic"_v, inst->source);
        return false;
    }

    auto& desc = lowerIntrinsicDesc(inst->getIntrinsic());

    if(inst->usedCount != desc.args || inst->createdCount != desc.results) {
        diagnostics->error("incorrect number of operands or results for intrinsic"_v, inst->source);
        return false;
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

    // A syscall has no callee to check: operand zero is the call number rather than an address,
    // and the kernel's signature is not something the IR knows. Asked of the call's own
    // convention rather than of the operand's type, so that a number of either width is one.
    if(inst->getCallType() == LowerCallType::Syscall) {
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

    // A vector is accepted wherever the scalar of its lane type would be, which is most of the
    // operation set and is what §3.1 of Implementation-Vector.md means by reuse: `add` over two
    // vectors is an `add`. Integer division and remainder are included even though no machine here
    // has one - the backend expands them lane by lane (Design-Vector §3.1), which is a lowering
    // question rather than a typing one.
    if(allowFloat) {
        valid = valid && (isInt(l) || isFloat(l) || isIntVector(l) || isFloatVector(l));
    } else {
        valid = valid && (isInt(l) || isIntVector(l));
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
        valid = l == r && l == result && (isInt(l) || isFloat(l) || isIntVector(l) || isFloatVector(l));
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
        valid = l == r && l == result && (isInt(l) || isFloat(l) || isIntVector(l) || isFloatVector(l));
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

    // A mask is one of the two things `and`, `or` and `xor` take beyond an integer, and it is how
    // two masks are combined: nothing else operates on one, since what a lane holds is a truth value.
    // A float *vector* is left out for the same reason a scalar float is - bitwise arithmetic on one
    // goes through a bitcast, so that what the bits are is stated rather than assumed.
    if(isPtr(l)) {
        valid = isPtr(result) && isInt(r);
    } else if(isPtr(r)) {
        valid = isPtr(result) && isInt(l);
    } else {
        valid = l == r && l == result && (isInt(l) || isIntVector(l) || l.isMask());
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

    // A vector shifts either by a vector of counts, one per lane, or by one count in a general
    // register that every lane shares - both of which the machine has, and both of which the
    // resolve IR produces (`x << 3` over a vector is the second).
    auto valid = (isInt(l) || isIntVector(l)) && (r == l || isInt(r)) && l == inst->result.type;

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

    auto l = base[inst->lhs]->type;
    auto r = base[inst->rhs]->type;

    // A comparison of two vectors answers a mask of the same shape rather than a Bool, which is the
    // one typing rule that makes the existing instruction serve (§3.1): `InstCmp` already carries
    // its condition and states its result type as a field, so nothing else about it changes.
    if(isVectorLike(l) || isVectorLike(r)) {
        if(l != r || !(isIntVector(l) || isFloatVector(l))) {
            diagnostics->error("inconsistent argument types to operation"_v, inst->source);
            return false;
        }

        if(!inst->result.type.isMask() || !sameLaneShape(inst->result.type, l)) {
            diagnostics->error("a comparison of vectors answers a mask of the same shape"_v, inst->source);
            return false;
        }

        return true;
    }

    if(inst->result.type != LowerType::Int32) {
        diagnostics->error("incorrect result type for comparison"_v, inst->source);
        return false;
    }

    // Operands can be:
    // - two of the same integer type.
    // - two of the same float type.
    // - two pointers.
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

    auto cmp = base[inst->cmp]->type;
    auto lhs = base[inst->lhs]->type;

    // The lane-wise select, which is the same instruction with a wider condition: a mask chooses
    // per lane where an Int32 chooses the whole value. The mask has to have the shape of what is
    // being selected, or it would be choosing between lanes that are not there.
    if(isVectorLike(lhs)) {
        if(!cmp.isMask() || !sameLaneShape(cmp, lhs)) {
            diagnostics->error("a select over vectors needs a mask of the same shape"_v, inst->source);
            return false;
        }
    } else if(cmp != LowerType::Int32) {
        diagnostics->error("incorrect type for comparison"_v, inst->source);
        return false;
    }

    // Anything that lives in a register, which here is every lower type there is: an integer or a
    // pointer is a `cmov`, a float is the branch-over-a-move expansion, and all three are one
    // `select` on LLVM and one ternary on JS. What decides whether a *resolve* value may become one
    // is `selectableType` in opt/opt_select.cpp, and it is stricter than this on purpose - a memory
    // type also lowers to `Pointer`, and its value is the address of storage rather than a value.
    if(!(isIntLike(lhs) || isFloat(lhs) || isVectorLike(lhs))) {
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

    // The frame lays objects out by rounding up to this, and rounds a run-time allocation's size by
    // it, so anything that is not a power of two would produce an address that satisfies nothing.
    if(inst->alignment == 0 || (inst->alignment & (inst->alignment - 1)) != 0) {
        diagnostics->error("allocation alignment is not a power of two"_v, inst->source);
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

    /*
     * A vector is loaded whole - there is no narrower access to widen from.
     *
     * **A mask included, which this used to refuse.** The refusal said "a mask is not held in memory
     * at all on a target where it is a k-register", and three things were wrong with it. `Repr`
     * already gives a mask a full layout (`computeVector` never asks `isMask`), and §2 of
     * Implementation-Vector.md already ruled that the memory form *is* the vector form at every
     * feature level, precisely so that `Maybe(Mask(Float))` means one thing everywhere - so this
     * denied that a memory form exists when the layout above it had already chosen one. There was no
     * matching check on the store side, so the IR let a mask be written and refused to read it back,
     * which makes a `Mask` local writable and unreadable rather than rejected. And the k-register it
     * defers to does not exist: `maskRegisterCountFor` answers zero unconditionally, so every mask
     * on every target this compiles for is already a vector in a vector register.
     *
     * If k-registers do land, reloading one becomes `vpmovm2d` and the question is which form a
     * *spill* takes - which is a register-allocation decision, and is where §2 put it.
     */
    if(isVectorLike(inst->result.type)) {
        if(inst->getWidth() != inst->result.type.byteWidth()) {
            diagnostics->error("a vector load reads the whole vector"_v, inst->source);
            return false;
        }
    } else if(inst->isOverread()) {
        // The flag says a *vector* load reads past the end of what it names. On a scalar it would be
        // an exemption from bounds reasoning that buys nothing and that the verifier above this IR
        // has no rule for.
        diagnostics->error("only a vector load can overread"_v, inst->source);
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

/*
 * The five that only a vector can be an operand or a result of.
 *
 * Between them they state what the lane index and the shuffle pattern mean, which nothing else can:
 * they are fields rather than operands, so no type rule reaches them and no pass can be relied on to
 * have checked one. A lane index past the end of the vector is the shape of the mistake, and it is
 * one that would read a neighbouring register or fold into a wrong encoding rather than fault.
 */
static bool validateVectorInst(Diagnostics* diagnostics, LowerBase base, LowerInst* inst) {
    if(inst->createdCount != 1) {
        diagnostics->error("incorrect arguments to operation"_v, inst->source);
        return false;
    }

    switch(inst->kind) {
        case LowerInst::VecSplat: {
            auto splat = (LowerInstVecSplat*)inst;
            auto result = splat->result.type;

            if(inst->usedCount != 1) {
                diagnostics->error("incorrect arguments to operation"_v, inst->source);
                return false;
            }

            if(!isVectorLike(result)) {
                diagnostics->error("vsplat produces a vector"_v, inst->source);
                return false;
            }

            if(base[splat->from]->type != scalarFormOf(result)) {
                diagnostics->error("vsplat takes the lane's scalar form"_v, inst->source);
                return false;
            }

            return true;
        }

        case LowerInst::VecLane:
        case LowerInst::VecWithLane: {
            auto lane = (LowerInstVecLane*)inst;
            auto source = base[lane->from]->type;
            auto isWrite = inst->kind == LowerInst::VecWithLane;

            if(inst->usedCount != (isWrite ? 2u : 1u)) {
                diagnostics->error("incorrect arguments to operation"_v, inst->source);
                return false;
            }

            if(!isVectorLike(source)) {
                diagnostics->error("a lane can only be taken from a vector"_v, inst->source);
                return false;
            }

            if(lane->getLane() >= source.lanes()) {
                diagnostics->error("lane index past the end of the vector"_v, inst->source);
                return false;
            }

            if(isWrite) {
                if(lane->result.type != source || base[lane->value]->type != scalarFormOf(source)) {
                    diagnostics->error("vwithlane writes the lane's scalar form into its own vector"_v, inst->source);
                    return false;
                }
            } else if(lane->result.type != scalarFormOf(source)) {
                diagnostics->error("vlane produces the lane's scalar form"_v, inst->source);
                return false;
            }

            return true;
        }

        case LowerInst::VecShuffle: {
            auto shuffle = (LowerInstVecShuffle*)inst;
            auto left = base[shuffle->left]->type;
            auto right = base[shuffle->right]->type;
            auto result = shuffle->result.type;

            if(inst->usedCount != 2) {
                diagnostics->error("incorrect arguments to operation"_v, inst->source);
                return false;
            }

            if(!isVectorLike(left) || left != right) {
                diagnostics->error("a shuffle selects from two vectors of one type"_v, inst->source);
                return false;
            }

            // The lane *type* is what a shuffle moves around, so the result may have a different
            // number of lanes than the sources - which is what makes `packLanes` and the two
            // `unpack`s shuffles rather than instructions of their own - but not a different lane.
            if(result.lane != left.lane || result.isMask() != left.isMask()) {
                diagnostics->error("a shuffle cannot change the lane type"_v, inst->source);
                return false;
            }

            for(auto entry: shuffle->pattern()) {
                if(entry >= left.lanes() * 2) {
                    diagnostics->error("shuffle pattern names a lane neither source has"_v, inst->source);
                    return false;
                }
            }

            return true;
        }

        case LowerInst::VecReduce: {
            auto reduce = (LowerInstVecReduce*)inst;
            auto source = base[reduce->from]->type;

            if(inst->usedCount != 1) {
                diagnostics->error("incorrect arguments to operation"_v, inst->source);
                return false;
            }

            if(!isVectorLike(source)) {
                diagnostics->error("a reduction combines the lanes of a vector"_v, inst->source);
                return false;
            }

            /*
             * The two kinds whose result is not the lane's scalar form, and they are the two that
             * read a mask as a *number* rather than combining its lanes: `Bits` answers the lanes as
             * bits and `FirstSet` answers the index of the lowest set one. Neither is a value a lane
             * holds - thirty-two `i8` lanes answer up to 32 either way - so both state the integer
             * they produce, and everything else states the ordinary rule.
             */
            auto readsMaskAsNumber = reduce->getReduce() == LowerReduce::Bits
                                  || reduce->getReduce() == LowerReduce::FirstSet;

            if(readsMaskAsNumber) {
                if(!source.isMask()) {
                    diagnostics->error("only a mask reduces to its bits or to its first set lane"_v, inst->source);
                    return false;
                }

                if(reduce->result.type != LowerType::Int32) {
                    diagnostics->error("a reduction to bits or to a lane index produces an i32"_v, inst->source);
                    return false;
                }

                return true;
            }

            if(reduce->result.type != scalarFormOf(source)) {
                diagnostics->error("a reduction produces the lane's scalar form"_v, inst->source);
                return false;
            }

            // A mask holds truth values, so the three that mean something over one are `and` (all),
            // `or` (any) and `add` (how many). The rest are arithmetic over lanes a mask does not
            // have. `Bits` and `FirstSet` are the other two and have answered above, since their
            // result type differs.
            if(source.isMask()) {
                auto reduction = reduce->getReduce();
                auto valid = reduction == LowerReduce::And
                    || reduction == LowerReduce::Or
                    || reduction == LowerReduce::Add;

                if(!valid) {
                    diagnostics->error("a mask reduces with and, or or add"_v, inst->source);
                    return false;
                }
            } else if(isFloatVector(source)) {
                auto reduction = reduce->getReduce();

                if(reduction == LowerReduce::And || reduction == LowerReduce::Or) {
                    diagnostics->error("a bitwise reduction of a float vector goes through a bitcast"_v, inst->source);
                    return false;
                }

                if(reduction == LowerReduce::IMin || reduction == LowerReduce::IMax) {
                    diagnostics->error("a float vector reduces with min and max, not their signed forms"_v, inst->source);
                    return false;
                }
            }

            return true;
        }

        default:
            assertTrue("not a vector instruction" == nullptr);
            return false;
    }
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

        if(!validateValueType(diagnostics, c->type, inst->source)) return false;

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
        // Nothing to check: no operands to type, no result to name, no fields to be out of range.
        case LowerInst::VZeroUpper:
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
        case LowerInst::Bswap:
            return validateBswap(diagnostics, base, (LowerInstUnary*)inst);
        case LowerInst::Sqrt:
            return validateSqrt(diagnostics, base, (LowerInstUnary*)inst);
        case LowerInst::Trunc:
        case LowerInst::Floor:
        case LowerInst::Ceil:
        case LowerInst::Round:
            return validateSqrt(diagnostics, base, (LowerInstUnary*)inst);
        case LowerInst::Abs:
            return validateAbs(diagnostics, base, (LowerInstUnary*)inst);
        case LowerInst::Fma:
            return validateFma(diagnostics, base, (LowerInstFma*)inst);
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
        case LowerInst::MulHi:
        case LowerInst::IMulHi:
            return validateArith(diagnostics, base, (LowerInstBinary*)inst, false);
        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar:
        case LowerInst::Rol:
        case LowerInst::Ror:
            return validateShift(diagnostics, base, (LowerInstBinary*)inst);
        case LowerInst::And:
        case LowerInst::Or:
        case LowerInst::Xor:
        // The three BMI2 operations, held to the bit operations' rule: two integer operands of the
        // instruction's own type. The *width* restriction - 32 and 64 bits only - is the resolve
        // verifier's, which is the last stage at which a language type is still visible.
        case LowerInst::BitsUpTo:
        case LowerInst::GatherBits:
        case LowerInst::ScatterBits:
            return validateBit(diagnostics, base, (LowerInstBinary*)inst);
        case LowerInst::Cmp:
            return validateCmp(diagnostics, base, (LowerInstCmp*)inst);
        case LowerInst::Select:
            return validateSelect(diagnostics, base, (LowerInstSelect*)inst);
        case LowerInst::VecSplat:
        case LowerInst::VecLane:
        case LowerInst::VecWithLane:
        case LowerInst::VecShuffle:
        case LowerInst::VecReduce:
            return validateVectorInst(diagnostics, base, inst);
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

        // Nothing to hold to the signature, which is the difference from a `ret`: this one does not
        // return, so there is no result for `validateRet`'s rule to be about.
        case LowerInst::Unreachable:
            return true;
        case LowerInst::Phi:
            return validatePhi(diagnostics, base, (LowerInstPhi*)inst, dominators);
        case LowerInst::Intrinsic:
            return validateIntrinsic(diagnostics, base, (LowerInstIntrinsic*)inst);
        case LowerInst::X86Address:
        case LowerInst::X86Lea:
        case LowerInst::X86PushArg:
        case LowerInst::X86MinMax:
        case LowerInst::X86MulWide:
        case LowerInst::X86Sext:
        case LowerInst::X86MaskAnd:
        case LowerInst::X86Permute:
        case LowerInst::X86StoreOp:
        case LowerInst::X86MovbeLoad:
        case LowerInst::X86MovbeStore:
        case LowerInst::X86AndNot:
        case LowerInst::X86LowBit:
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

/*
 * A store into a global that says nothing writes it.
 *
 * `mut` clear is a promise: it becomes LLVM's `constant`, and this backend folds a load of such a
 * global into its reader and rematerializes it rather than spilling. A module that breaks the
 * promise does not fail loudly - the reads keep answering the initializer and the store is dropped
 * as dead - so it is worth one walk to say so.
 *
 * The *direct* case only, which is a store whose target is the global's own address. A store through
 * a pointer the program computed is an escape question rather than a check, and this is a validator.
 * That is enough for what it exists to catch: a hand-written `.lower` fixture that stores into a
 * global and forgot to write `mut` in front of it, which is exactly what every fixture did while the
 * parser had no syntax for it.
 */
static bool validateGlobalWrites(Diagnostics* diagnostics, LowerBase base, LowerFunction* function) {
    for(auto b: function->blocks.contents(base)) {
        for(auto i: base[b]->instructions.contents(base)) {
            auto inst = base[i];
            if(inst->kind != LowerInst::Store) continue;

            auto target = base[((LowerInstStore*)inst)->to];
            if(target->inst()->kind != LowerInst::Global) continue;

            auto global = base[((LowerInstGlobal*)target->inst())->target];
            if(!global->mut) {
                diagnostics->error("store into a global that is not declared mut"_v, inst->source);
                return false;
            }
        }
    }

    return true;
}

bool validateLowerModule(Diagnostics* diagnostics, LowerModule* module) {
    auto base = *module->arena;

    for(auto g: module->globalOrder) {
        if(!validateLowerGlobal(diagnostics, base, base[g])) return false;
    }

    for(auto f: module->functionOrder) {
        if(!validateLowerFunction(diagnostics, base, base[f])) return false;
        if(!validateGlobalWrites(diagnostics, base, base[f])) return false;
    }

    return true;
}
