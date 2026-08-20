#include "build.h"

#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/IntrinsicsX86.h>
#include <llvm/IR/MDBuilder.h>

/*
 * One lower instruction at a time.
 *
 * Three things in here are longer than one builder call, and they are the three places where the
 * lower IR says something in bytes that LLVM says in types:
 *
 *   pointer arithmetic  `add ptr, int` is a byte offset, so it is a GEP over i8. Going through
 *                       ptrtoint would answer the same address and lose every alias fact with it.
 *   load and store      the access states a width in bytes and a result type, which is one
 *                       load of that width and one cast rather than a typed access.
 *   alloca              a fixed-size allocation is a frame object for the whole function wherever
 *                       it stands, so it belongs in the entry block; see genAlloca.
 *
 * Everything else is the instruction it looks like.
 */
namespace llvmgen {

// The value the IR names, or poison and a diagnostic. The lower validator has already checked that
// a definition dominates every use of it, so the failure is not something a correct pipeline can
// reach - but this backend has to be able to run over IR that has not been validated.
static llvm::Value* use(FunGen& f, LowerInst& inst, LowerPtr<LowerValue> value) {
    if(auto built = f.use(value)) return built;

    f.context.diagnostics.error("llvm: an instruction uses a value that was never defined"_v, inst.source);
    return llvm::PoisonValue::get(typeOf(f.gen, f.base[value]->type));
}

static llvm::StringRef nameOf(FunGen& f, LowerValue& value) {
    return nameOf(f.context, value.name);
}

/*
 * Vectors, and the two ways they meet the scalar code below.
 *
 * The first is that the float/integer question is asked of a lane rather than of a value: `isFloat`
 * answers no for an `f32x8` on purpose (see LowerType), so every site that has to treat a float
 * vector as a float names this instead. The second is the lane's *scalar form* - what a `vsplat`
 * takes and a `vlane` answers - which is wider than the lane for an 8- or 16-bit one and is an Int32
 * truth value for a mask.
 */
static bool isFloatLike(LowerType type) {
    return isFloat(type) || isFloatVector(type);
}

// The LLVM element type of a vector or a mask, which is not `typeOf(laneType())` for a mask: a mask
// is `<N x i1>` here and its lane type names the integer whose width it masks.
static llvm::Type* laneTypeOf(Gen& gen, LowerType type) {
    return llvm::cast<llvm::FixedVectorType>(typeOf(gen, type))->getElementType();
}

// A scalar narrowed into the lane it is going to occupy. A mask lane is a truth value, so anything
// that is not zero is one - a truncation would read the low bit and call 2 false.
static llvm::Value* intoLane(FunGen& f, llvm::Value* scalar, LowerType vector) {
    auto lane = laneTypeOf(f.gen, vector);
    if(scalar->getType() == lane) return scalar;

    if(lane->isIntegerTy(1)) {
        return f.builder.CreateICmpNE(scalar, llvm::ConstantInt::get(scalar->getType(), 0));
    }

    return f.builder.CreateTrunc(scalar, lane);
}

// And back out: an 8- or 16-bit lane arrives in a 32-bit register zero-extended, which is what
// `pextrb` and `pextrw` do on the machine, and a mask lane arrives as the 0 or 1 it means.
static llvm::Value* outOfLane(FunGen& f, llvm::Value* lane, LowerType scalar) {
    auto type = typeOf(f.gen, scalar);
    return lane->getType() == type ? lane : f.builder.CreateZExt(lane, type);
}

/*
 * Provided values.
 */

static void genImm(FunGen& f, LowerImm& inst) {
    auto type = typeOf(f.gen, inst.result.type);

    f.define(inst.result, isFloat(inst.result.type)
        ? (llvm::Constant*)llvm::ConstantFP::get(type, inst.f)
        : (llvm::Constant*)llvm::ConstantInt::get(type, inst.i));
}

static void genGlobalRef(FunGen& f, LowerInstGlobal& inst) {
    auto global = f.gen.globalOf(inst.target);

    if(!global) {
        f.context.diagnostics.error("llvm: reference to a global that is not in this module"_v, inst.source);
        global = nullptr;
    }

    f.define(inst.result, global ? (llvm::Value*)global
                                 : llvm::PoisonValue::get(typeOf(f.gen, LowerType::Pointer)));
}

static void genFunRef(FunGen& f, LowerInstFun& inst) {
    auto fun = f.gen.functionOf(inst.target);

    if(!fun) {
        f.context.diagnostics.error("llvm: reference to a function that is not in this module"_v, inst.source);
    }

    f.define(inst.result, fun ? (llvm::Value*)fun
                              : llvm::PoisonValue::get(typeOf(f.gen, LowerType::Pointer)));
}

/*
 * Conversions.
 */

static void genCast(FunGen& f, LowerInstCast& inst) {
    auto from = use(f, inst, inst.from);
    auto source = f.base[inst.from]->type;
    auto target = inst.result.type;
    auto type = typeOf(f.gen, target);
    llvm::Value* value;

    // Asked of the lane, so that a conversion between two vectors is the conversion between their
    // lanes: every builder call below takes a vector wherever it takes the scalar, and the lane
    // count the lower validator holds fixed across a `cast` is what makes that true.
    if(isFloatLike(source) && isFloatLike(target)) {
        value = f.builder.CreateFPCast(from, type, nameOf(f, inst.result));
    } else if(isFloatLike(source)) {
        /*
         * Saturating, per the ruling in `saturationRange`: out of range clamps to the nearest end
         * and NaN becomes zero.
         *
         * `fptosi` and `fptoui` are *poison* outside the range, which is the plain conversion and
         * not this one - LLVM has the saturating form as an intrinsic precisely because the plain
         * one cannot express it. Using the intrinsic rather than expanding the clamp here is also
         * what keeps this backend's answer identical to the local one without either having to know
         * how the other spells it: both are asked for the same three cases and both produce them.
         */
        auto id = inst.isSignedResult() ? llvm::Intrinsic::fptosi_sat : llvm::Intrinsic::fptoui_sat;
        llvm::Type* overloads[] = { type, from->getType() };

        value = f.builder.CreateIntrinsic(id, overloads, { from }, nullptr, nameOf(f, inst.result));
    } else if(isFloatLike(target)) {
        value = inst.isSignedSource() ? f.builder.CreateSIToFP(from, type, nameOf(f, inst.result))
                                      : f.builder.CreateUIToFP(from, type, nameOf(f, inst.result));
    } else {
        // Widening carries the sign bit up only for a source that has one; narrowing is a truncation
        // either way. CreateIntCast picks between the three, and answers the value unchanged when
        // the two types are already the same.
        value = f.builder.CreateIntCast(from, type, inst.isSignedSource(), nameOf(f, inst.result));
    }

    f.define(inst.result, value);
}

// How many bits a lower type occupies, which is the only thing a bitcast is about.
static U32 bitsOf(LowerType type) {
    return type.byteWidth() * 8;
}

static void genBitcast(FunGen& f, LowerInstUnary& inst) {
    auto from = use(f, inst, inst.from);
    auto source = f.base[inst.from]->type;
    auto target = inst.result.type;
    auto type = typeOf(f.gen, target);
    auto name = nameOf(f, inst.result);
    llvm::Value* value;

    if(isVectorLike(source) || isVectorLike(target)) {
        // Reinterpretation of one register: the lower validator has already checked that the two are
        // the same width, which is the whole of what a vector bitcast means - `i8x16` to `i32x4` is
        // one register read differently, and `i8x16` to `i32x8` names two and is refused there.
        value = f.builder.CreateBitCast(from, type, name);
    } else if(isPtr(source) && isPtr(target)) {
        // Both are the one opaque pointer type, so there is nothing left to cast.
        value = from;
    } else if(isPtr(source)) {
        value = f.builder.CreatePtrToInt(from, type, name);
    } else if(isPtr(target)) {
        value = f.builder.CreateIntToPtr(from, type, name);
    } else if(source == target) {
        value = from;
    } else {
        // Everything else goes through an integer of the source's width: a bitcast may only join
        // two types of one size, so a float that changes width has to be resized as an integer
        // between the two casts. That is what "without changing the bits, aside from truncation"
        // means when the widths differ.
        auto asInt = isFloat(source) ? f.builder.CreateBitCast(from, intTypeOfWidth(f.gen, bitsOf(source) / 8))
                                     : from;

        asInt = f.builder.CreateIntCast(asInt, intTypeOfWidth(f.gen, bitsOf(target) / 8), false);
        value = isFloat(target) ? f.builder.CreateBitCast(asInt, type, name) : asInt;
    }

    f.define(inst.result, value);
}

static void genUnary(FunGen& f, LowerInstUnary& inst) {
    auto from = use(f, inst, inst.from);
    auto name = nameOf(f, inst.result);
    llvm::Value* value;

    switch(inst.kind) {
        case LowerInst::Set:
            // A copy of a local is a name for the same value here. SSA has no locals to copy.
            value = from;
            break;
        case LowerInst::Neg:
            value = isFloatLike(inst.result.type) ? f.builder.CreateFNeg(from, name)
                                                  : f.builder.CreateNeg(from, name);
            break;
        // `llvm.bswap`, which LLVM lowers to `bswap` or `movbe` for itself. It was reached through
        // the intrinsic path until `Bswap` was a kind of its own; the name it is given here is the
        // same one, and what changed is that everything above this backend can now see through it.
        case LowerInst::Bswap:
            value = f.builder.CreateUnaryIntrinsic(llvm::Intrinsic::bswap, from, nullptr, name);
            break;
        case LowerInst::Sqrt:
            // One intrinsic for the scalar and the vector alike - `llvm.sqrt` is overloaded on its
            // operand type, so a `<4 x float>` is the same call with a different type argument.
            value = f.builder.CreateIntrinsic(llvm::Intrinsic::sqrt, { from->getType() }, { from },
                                              nullptr, name);
            break;

        /*
         * The four roundings, each of which LLVM names directly and overloads on the operand type
         * exactly as `llvm.sqrt` is overloaded - so a `<4 x double>` is the same call.
         *
         * `llvm.round` is the ties-**away**-from-zero one, which is the rule resolve/inst.def rules
         * on and is not `llvm.roundeven`. Picking the wrong one of that pair is invisible except at
         * an exact half, which is why the two are named here rather than reached through a table.
         */
        case LowerInst::Trunc:
        case LowerInst::Floor:
        case LowerInst::Ceil:
        case LowerInst::Round: {
            auto intrinsic = inst.kind == LowerInst::Trunc ? llvm::Intrinsic::trunc
                           : inst.kind == LowerInst::Floor ? llvm::Intrinsic::floor
                           : inst.kind == LowerInst::Ceil  ? llvm::Intrinsic::ceil
                                                           : llvm::Intrinsic::round;

            value = f.builder.CreateIntrinsic(intrinsic, { from->getType() }, { from }, nullptr, name);
            break;
        }

        /*
         * The magnitude, which is two intrinsics rather than one because LLVM spells the integer and
         * the floating-point cases separately.
         *
         * `llvm.fabs` clears the sign bit, which is exactly what `Value::Abs` was made a kind to
         * state (the sign of a NaN is unspecified - see resolve/inst.def). `llvm.abs` takes a second
         * argument saying whether the most negative integer is poison, and it is **false** here: the
         * lower IR's magnitude of `INT_MIN` is `INT_MIN`, the same wrap every other target gives,
         * and promising otherwise would let this be optimized on an assumption the language does not
         * make.
         */
        case LowerInst::Abs:
            if(isFloatLike(inst.result.type)) {
                value = f.builder.CreateIntrinsic(llvm::Intrinsic::fabs, { from->getType() }, { from },
                                                  nullptr, name);
            } else {
                value = f.builder.CreateIntrinsic(
                    llvm::Intrinsic::abs, { from->getType() },
                    { from, f.builder.getInt1(false) }, nullptr, name
                );
            }
            break;
        default:
            // `not` over a mask is the lane-wise negation of an `<N x i1>`, which is the same xor
            // against all ones an integer vector gets.
            value = f.builder.CreateNot(from, name);
            break;
    }

    f.define(inst.result, value);
}

/*
 * `a * b + c`, at most once rounded.
 *
 * `llvm.fma` is the intrinsic that *means* one rounding, so this is the one place the two backends
 * do not have to agree about a feature level: LLVM lowers it to the machine's fused instruction
 * where there is one and to a libm call or the two operations where there is not, and either way it
 * is the operation the IR asked for. The x64 backend makes the same choice one level lower, in
 * `expandFusedMultiplyAdd`.
 */
static void genFma(FunGen& f, LowerInstFma& inst) {
    auto a = use(f, inst, inst.a);
    auto b = use(f, inst, inst.b);
    auto c = use(f, inst, inst.c);

    f.define(inst.result, f.builder.CreateIntrinsic(llvm::Intrinsic::fma, { a->getType() },
                                                    { a, b, c }, nullptr, nameOf(f, inst.result)));
}

/*
 * Arithmetic.
 */

// `add ptr, int` and `sub ptr, int` are byte offsets - the lowering has spent every element type it
// had - so they are a GEP over i8 rather than an integer round trip. The index is widened as signed
// because a negative offset is what `sub` produces, and because that is what the x64 backend's
// `movsxd` does with a narrow one.
static llvm::Value* genPtrOffset(FunGen& f, llvm::Value* pointer, llvm::Value* offset, bool negate,
                                 const llvm::Twine& name)
{
    auto index = f.builder.CreateSExtOrTrunc(offset, llvm::Type::getInt64Ty(f.gen.llvm));
    if(negate) index = f.builder.CreateNeg(index);

    return f.builder.CreateGEP(llvm::Type::getInt8Ty(f.gen.llvm), pointer, index, name);
}

// A bitwise operation on an address, which LLVM has no pointer form of. The round trip through an
// integer is what the operation means: masking an address is arithmetic on its bits.
static llvm::Value* genPtrBits(FunGen& f, LowerInstBinary& inst, llvm::Value* lhs, llvm::Value* rhs,
                               const llvm::Twine& name)
{
    auto word = llvm::Type::getInt64Ty(f.gen.llvm);
    auto l = lhs->getType()->isPointerTy() ? f.builder.CreatePtrToInt(lhs, word) : f.builder.CreateZExtOrTrunc(lhs, word);
    auto r = rhs->getType()->isPointerTy() ? f.builder.CreatePtrToInt(rhs, word) : f.builder.CreateZExtOrTrunc(rhs, word);
    llvm::Value* value;

    switch(inst.kind) {
        case LowerInst::And: value = f.builder.CreateAnd(l, r); break;
        case LowerInst::Or:  value = f.builder.CreateOr(l, r); break;
        default:             value = f.builder.CreateXor(l, r); break;
    }

    return f.builder.CreateIntToPtr(value, typeOf(f.gen, inst.result.type), name);
}

static llvm::CmpInst::Predicate intPredicate(LowerCmp cmp) {
    switch(cmp) {
        case LowerCmp::eq:  return llvm::CmpInst::ICMP_EQ;
        case LowerCmp::neq: return llvm::CmpInst::ICMP_NE;
        case LowerCmp::gt:  return llvm::CmpInst::ICMP_UGT;
        case LowerCmp::ge:  return llvm::CmpInst::ICMP_UGE;
        case LowerCmp::lt:  return llvm::CmpInst::ICMP_ULT;
        case LowerCmp::le:  return llvm::CmpInst::ICMP_ULE;
        case LowerCmp::igt: return llvm::CmpInst::ICMP_SGT;
        case LowerCmp::ige: return llvm::CmpInst::ICMP_SGE;
        case LowerCmp::ilt: return llvm::CmpInst::ICMP_SLT;
        case LowerCmp::ile: return llvm::CmpInst::ICMP_SLE;

        // Float operands only, so neither has an integer reading and neither can reach here.
        // Answered rather than left to the fallthrough so that the switch stays exhaustive.
        case LowerCmp::uno: case LowerCmp::ord: break;
    }

    return llvm::CmpInst::ICMP_EQ;
}

/*
 * Ordered, so a comparison against a NaN is false - with the one exception every language makes.
 *
 * `!=` is the negation of `==` rather than a member of the ordered family, so it is `une` and not
 * `one`: a NaN is not equal to anything including itself, and `a != b` has to be true wherever
 * `a == b` is false. `one` made both of them false at once, and the JavaScript target's `!==` was
 * already true there - so the two backends answered differently for the same program.
 *
 * The signed forms are the same as the unsigned ones here: a float has one ordering, and the
 * distinction the IR carries is about integers.
 */
static llvm::CmpInst::Predicate floatPredicate(LowerCmp cmp) {
    switch(cmp) {
        case LowerCmp::eq:  return llvm::CmpInst::FCMP_OEQ;
        case LowerCmp::neq: return llvm::CmpInst::FCMP_UNE;
        case LowerCmp::uno: return llvm::CmpInst::FCMP_UNO;
        case LowerCmp::ord: return llvm::CmpInst::FCMP_ORD;
        case LowerCmp::gt:
        case LowerCmp::igt: return llvm::CmpInst::FCMP_OGT;
        case LowerCmp::ge:
        case LowerCmp::ige: return llvm::CmpInst::FCMP_OGE;
        case LowerCmp::lt:
        case LowerCmp::ilt: return llvm::CmpInst::FCMP_OLT;
        default:            return llvm::CmpInst::FCMP_OLE;
    }
}

static void genCmp(FunGen& f, LowerInstCmp& inst) {
    auto lhs = use(f, inst, inst.lhs);
    auto rhs = use(f, inst, inst.rhs);
    auto type = f.base[inst.lhs]->type;

    auto compared = isFloatLike(type) ? f.builder.CreateFCmp(floatPredicate(inst.getCmp()), lhs, rhs)
                                      : f.builder.CreateICmp(intPredicate(inst.getCmp()), lhs, rhs);

    // A comparison of vectors answers a mask, which is the `<N x i1>` the comparison already
    // produced - the one place where this backend's representation is LLVM's own rather than the
    // full-width lanes the Repr describes. Everything a mask reaches (`and`, `not`, `select`, a
    // reduction) is written against that, and LLVM's lowering collapses the pair.
    if(inst.result.type.isMask()) {
        compared->setName(nameOf(f, inst.result));
        f.define(inst.result, compared);
        return;
    }

    // The lower IR has no one-bit type: a comparison answers Int, which is 0 or 1.
    f.define(inst.result, f.builder.CreateZExt(compared, typeOf(f.gen, inst.result.type), nameOf(f, inst.result)));
}

/*
 * The three bit operations of `Core.Bits.bitsUpTo` and `Core.BitPermute`, which this backend has to
 * answer for whatever target the module names.
 *
 * `bitsUpTo` is written out and left to LLVM. The select and the mask below are the shape
 * InstCombine already knows: on a target with BMI2 it becomes `bzhi`, and on one without it becomes
 * the same four instructions the x64 backend's own expansion emits. Nothing is gained by naming an
 * x86 intrinsic here, and a target that is not x86 would then have nothing to select.
 *
 * The permutations are the opposite case, and it is why they get a helper. There is no target-
 * independent LLVM intrinsic for an arbitrary bit permutation, so a target that *has* the
 * instruction can only be reached by naming the x86 one - and a target that does not have it needs a
 * loop, which is a whole function rather than an expression.
 */

// The width, the mask type and the constants everything below needs.
struct BitOpWidth {
    llvm::IntegerType* type;
    U32 bits;
};

static BitOpWidth bitOpWidthOf(FunGen& f, LowerType type) {
    auto bits = U32(type == LowerType::Int64 ? 64 : 32);
    return BitOpWidth { llvm::IntegerType::get(f.gen.llvm, bits), bits };
}

// Whether this module's target has `pext` and `pdep` - which is x86-64 at v3 and nothing else. The
// level is the *build's* rather than the host's, exactly as `featuresOf` reads it when the target
// machine is created, so an `-arch arm64` build never reaches the x86 intrinsic.
static bool hasBitPermute(FunGen& f) {
    auto& settings = f.context.settings;
    return settings.arch == TargetArch::X64 && settings.extensions.level >= TargetExtensions::V3;
}

/*
 * The permutation written as a loop, in a function of its own, created once per module and width.
 *
 * A loop rather than the parallel-suffix network the x64 backend expands to, and the difference is
 * that this one may be *inlined and specialized* by the optimizer: a constant mask makes the trip
 * count constant and LLVM unrolls it into exactly the moves the mask calls for, which is better than
 * the network's fixed five rounds. What it cannot do is beat the instruction, which is why the
 * caller asks `hasBitPermute` first.
 *
 *     out = 0; bit = 1;
 *     while(mask) {
 *         low = mask & -mask;                             // the lowest set bit of the mask
 *         out |= gather ? ((value & low) ? bit : 0)       // pack it down to the next free position
 *                       : ((value & bit) ? low : 0);      // or spread the next bit out to it
 *         mask ^= low;
 *         bit <<= 1;
 *     }
 *
 * Internal and `alwaysinline`-free: it is left an ordinary internal function so that the inliner
 * decides, which is the right answer for a loop whose cost depends entirely on whether the mask
 * turned out to be a constant at the call site.
 */
static llvm::Function* bitPermuteHelper(FunGen& f, bool gather, BitOpWidth width) {
    // Named rather than formatted, and the four names written out: the helper is looked up by name
    // to find the one this module already has, so the name is an identity rather than a label.
    llvm::StringRef named = gather ? (width.bits == 64 ? "__yana_pext64" : "__yana_pext32")
                                   : (width.bits == 64 ? "__yana_pdep64" : "__yana_pdep32");

    if(auto existing = f.gen.module.getFunction(named)) return existing;

    llvm::Type* args[] = { width.type, width.type };
    auto signature = llvm::FunctionType::get(width.type, args, false);
    auto helper = llvm::Function::Create(signature, llvm::Function::InternalLinkage, named, f.gen.module);

    helper->addFnAttr(llvm::Attribute::NoUnwind);
    helper->addFnAttr(llvm::Attribute::WillReturn);

    auto entry = llvm::BasicBlock::Create(f.gen.llvm, "entry", helper);
    auto header = llvm::BasicBlock::Create(f.gen.llvm, "loop", helper);
    auto body = llvm::BasicBlock::Create(f.gen.llvm, "step", helper);
    auto exit = llvm::BasicBlock::Create(f.gen.llvm, "done", helper);

    llvm::IRBuilder<> b { entry };

    auto zero = llvm::ConstantInt::get(width.type, 0);
    auto one = llvm::ConstantInt::get(width.type, 1);
    auto value = helper->getArg(0);

    b.CreateBr(header);
    b.SetInsertPoint(header);

    auto mask = b.CreatePHI(width.type, 2);
    auto bit = b.CreatePHI(width.type, 2);
    auto out = b.CreatePHI(width.type, 2);

    mask->addIncoming(helper->getArg(1), entry);
    bit->addIncoming(one, entry);
    out->addIncoming(zero, entry);

    b.CreateCondBr(b.CreateICmpNE(mask, zero), body, exit);
    b.SetInsertPoint(body);

    auto low = b.CreateAnd(mask, b.CreateNeg(mask));
    auto tested = gather ? b.CreateAnd(value, low) : b.CreateAnd(value, bit);
    auto contributed = b.CreateSelect(b.CreateICmpNE(tested, zero), gather ? bit : low, zero);

    auto nextMask = b.CreateXor(mask, low);
    auto nextBit = b.CreateShl(bit, one);
    auto nextOut = b.CreateOr(out, contributed);

    mask->addIncoming(nextMask, body);
    bit->addIncoming(nextBit, body);
    out->addIncoming(nextOut, body);

    b.CreateBr(header);
    b.SetInsertPoint(exit);
    b.CreateRet(out);

    return helper;
}

static void genBitOperation(FunGen& f, LowerInstBinary& inst) {
    auto lhs = use(f, inst, inst.lhs);
    auto rhs = use(f, inst, inst.rhs);
    auto name = nameOf(f, inst.result);
    auto width = bitOpWidthOf(f, inst.result.type);

    if(inst.kind == LowerInst::BitsUpTo) {
        auto limit = llvm::ConstantInt::get(width.type, width.bits);
        auto small = f.builder.CreateICmpULT(rhs, limit);

        // The count masked to the width before the shift, for the reason the x64 expansion masks it:
        // the arm the select discards is still *computed*, and a shift by an out-of-range count is
        // poison in this IR rather than merely unread.
        auto masked = f.builder.CreateAnd(rhs, llvm::ConstantInt::get(width.type, width.bits - 1));
        auto one = llvm::ConstantInt::get(width.type, 1);
        auto mask = f.builder.CreateSub(f.builder.CreateShl(one, masked), one);

        f.define(inst.result, f.builder.CreateSelect(small, f.builder.CreateAnd(lhs, mask), lhs, name));
        return;
    }

    auto gather = inst.kind == LowerInst::GatherBits;

    if(hasBitPermute(f)) {
        auto which = gather
            ? (width.bits == 64 ? llvm::Intrinsic::x86_bmi_pext_64 : llvm::Intrinsic::x86_bmi_pext_32)
            : (width.bits == 64 ? llvm::Intrinsic::x86_bmi_pdep_64 : llvm::Intrinsic::x86_bmi_pdep_32);

        llvm::Value* args[] = { lhs, rhs };
        f.define(inst.result, f.builder.CreateIntrinsic(which, {}, args, nullptr, name));
        return;
    }

    llvm::Value* args[] = { lhs, rhs };
    f.define(inst.result, f.builder.CreateCall(bitPermuteHelper(f, gather, width), args, name));
}

static void genBinary(FunGen& f, LowerInstBinary& inst) {
    auto lhs = use(f, inst, inst.lhs);
    auto rhs = use(f, inst, inst.rhs);
    auto lhsType = f.base[inst.lhs]->type;
    auto rhsType = f.base[inst.rhs]->type;
    auto result = inst.result.type;
    auto name = nameOf(f, inst.result);
    auto isFloatOp = isFloatLike(result);
    llvm::Value* value;

    switch(inst.kind) {
        case LowerInst::Add:
            if(isPtr(result)) {
                value = isPtr(lhsType) ? genPtrOffset(f, lhs, rhs, false, name)
                                       : genPtrOffset(f, rhs, lhs, false, name);
            } else if(isFloatOp) {
                value = f.builder.CreateFAdd(lhs, rhs, name);
            } else {
                value = f.builder.CreateAdd(lhs, rhs, name);
            }
            break;

        case LowerInst::Sub:
            if(isPtr(lhsType) && isPtr(rhsType)) {
                auto difference = f.builder.CreatePtrDiff(llvm::Type::getInt8Ty(f.gen.llvm), lhs, rhs);
                value = f.builder.CreateIntCast(difference, typeOf(f.gen, result), true, name);
            } else if(isPtr(result)) {
                value = genPtrOffset(f, lhs, rhs, true, name);
            } else if(isFloatOp) {
                value = f.builder.CreateFSub(lhs, rhs, name);
            } else {
                value = f.builder.CreateSub(lhs, rhs, name);
            }
            break;

        // Multiplication answers the same bits whether its operands are signed or not, so the two
        // instructions the IR keeps apart - because the machine keeps them apart - are one here.
        case LowerInst::Mul:
        case LowerInst::IMul:
            value = isFloatOp ? f.builder.CreateFMul(lhs, rhs, name) : f.builder.CreateMul(lhs, rhs, name);
            break;

        case LowerInst::Div:
            value = isFloatOp ? f.builder.CreateFDiv(lhs, rhs, name) : f.builder.CreateUDiv(lhs, rhs, name);
            break;
        case LowerInst::IDiv:
            value = f.builder.CreateSDiv(lhs, rhs, name);
            break;
        case LowerInst::Rem:
            value = f.builder.CreateURem(lhs, rhs, name);
            break;
        case LowerInst::IRem:
            value = f.builder.CreateSRem(lhs, rhs, name);
            break;

        /*
         * The high half, spelled as the whole product taken at twice the width and cut down again.
         *
         * There is no `mulhi` in LLVM IR and no need for one: this is the shape the DAG combiner
         * recognizes, so it reaches the machine's own one-operand multiply on every target that has
         * one, and stays a real i128 multiply on the targets that do not.
         */
        case LowerInst::MulHi:
        case LowerInst::IMulHi: {
            auto isSigned = inst.kind == LowerInst::IMulHi;
            auto width = lhs->getType()->getScalarSizeInBits();
            llvm::Type* wide = llvm::IntegerType::get(f.gen.llvm, width * 2);

            // Per lane where the operands are a vector, which is what the double-width product has
            // to be taken in for the truncation below to be one lane's high half.
            if(auto vector = llvm::dyn_cast<llvm::FixedVectorType>(lhs->getType())) {
                wide = llvm::FixedVectorType::get(wide, vector->getNumElements());
            }

            auto extend = [&](llvm::Value* v) {
                return isSigned ? f.builder.CreateSExt(v, wide) : f.builder.CreateZExt(v, wide);
            };

            auto product = f.builder.CreateMul(extend(lhs), extend(rhs));
            auto high = f.builder.CreateLShr(product, llvm::ConstantInt::get(wide, width, false));
            value = f.builder.CreateTrunc(high, lhs->getType(), name);
            break;
        }

        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar:
        case LowerInst::Rol:
        case LowerInst::Ror: {
            // The IR lets the amount be either integer width; LLVM wants both operands alike. A
            // vector shifts either by a vector of counts or by one count in a general register that
            // every lane shares, and LLVM has only the first - so the shared count is splatted,
            // which is also the form the machine's immediate shift is selected back out of.
            llvm::Value* amount;

            if(auto vector = llvm::dyn_cast<llvm::FixedVectorType>(lhs->getType());
               vector && !rhs->getType()->isVectorTy())
            {
                amount = f.builder.CreateVectorSplat(vector->getElementCount(),
                                                     f.builder.CreateZExtOrTrunc(rhs, vector->getElementType()));
            } else {
                amount = f.builder.CreateZExtOrTrunc(rhs, lhs->getType());
            }

            /*
             * The rotations are the funnel shifts over a *repeated* operand, which is what
             * `llvm.fshl(x, x, c)` means and is how every LLVM front end spells a rotation - the
             * target then selects `rol` where it has one and the shift pair where it does not.
             *
             * The modulus comes free and is the same one this IR promises: `fshl` is defined to take
             * its count modulo the operand's width, so a rotation by the width is the identity here
             * exactly as it is on the machine.
             */
            if(inst.kind == LowerInst::Rol || inst.kind == LowerInst::Ror) {
                auto which = inst.kind == LowerInst::Rol ? llvm::Intrinsic::fshl : llvm::Intrinsic::fshr;
                llvm::Value* args[] = { lhs, lhs, amount };

                value = f.builder.CreateIntrinsic(which, { lhs->getType() }, args, nullptr, name);
                break;
            }

            if(inst.kind == LowerInst::Shl) value = f.builder.CreateShl(lhs, amount, name);
            else if(inst.kind == LowerInst::Shr) value = f.builder.CreateLShr(lhs, amount, name);
            else value = f.builder.CreateAShr(lhs, amount, name);
            break;
        }

        case LowerInst::And:
            if(isPtr(result)) value = genPtrBits(f, inst, lhs, rhs, name);
            else value = f.builder.CreateAnd(lhs, rhs, name);
            break;
        case LowerInst::Or:
            if(isPtr(result)) value = genPtrBits(f, inst, lhs, rhs, name);
            else value = f.builder.CreateOr(lhs, rhs, name);
            break;
        default:
            if(isPtr(result)) value = genPtrBits(f, inst, lhs, rhs, name);
            else value = f.builder.CreateXor(lhs, rhs, name);
            break;
    }

    f.define(inst.result, value);
}

/*
 * Conditions.
 *
 * A condition is an Int in the lower IR and an i1 in LLVM, and the only thing that produces one is a
 * comparison - so recovering the i1 the comparison already computed keeps the branch reading it
 * rather than re-testing the zero-extension of it. Anything else is a value the program computed,
 * and "the condition holds" means it is not zero, which is what `test r, r` says on the machine.
 */
static llvm::Value* genCondition(FunGen& f, llvm::Value* value) {
    if(auto zext = llvm::dyn_cast<llvm::ZExtInst>(value)) {
        if(zext->getSrcTy()->isIntegerTy(1)) return zext->getOperand(0);
    }

    return f.builder.CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0));
}

static void genSelect(FunGen& f, LowerInstSelect& inst) {
    auto given = use(f, inst, inst.cmp);

    // A mask chooses per lane and is already the `<N x i1>` a lane-wise select consumes; an Int32
    // chooses the whole value and has the i1 recovered out of it.
    auto condition = f.base[inst.cmp]->type.isMask() ? given : genCondition(f, given);
    auto lhs = use(f, inst, inst.lhs);
    auto rhs = use(f, inst, inst.rhs);

    // `select` yields its first operand when the condition holds, matching the x64 form's tie.
    f.define(inst.result, f.builder.CreateSelect(condition, lhs, rhs, nameOf(f, inst.result)));
}

/*
 * The five kinds that only a vector is an operand or a result of.
 *
 * Four of them are the instruction they look like, which is what §6 of Implementation-Vector.md
 * predicted: LLVM has `insertelement`, `extractelement` and `shufflevector`, and they mean what the
 * lower IR's `vlane`, `vwithlane` and `vshuffle` mean. The reduction is the one that owes a decision,
 * and it is below.
 */

// A name is given after the fact rather than to the builder, which spells a splat as an insert and a
// shuffle and would otherwise name the pair `x.splatinsert` and `x.splat`. What the IR named is the
// vector, which is the second of the two.
static void nameResult(FunGen& f, llvm::Value* value, LowerValue& result) {
    if(auto name = nameOf(f, result); !name.empty()) value->setName(name);
}

static void genVecSplat(FunGen& f, LowerInstVecSplat& inst) {
    auto type = inst.result.type;
    auto from = intoLane(f, use(f, inst, inst.from), type);
    auto splat = f.builder.CreateVectorSplat(type.lanes(), from);

    nameResult(f, splat, inst.result);
    f.define(inst.result, splat);
}

static void genVecLane(FunGen& f, LowerInstVecLane& inst) {
    auto from = use(f, inst, inst.from);
    auto source = f.base[inst.from]->type;
    auto index = llvm::ConstantInt::get(llvm::Type::getInt32Ty(f.gen.llvm), inst.getLane());

    if(inst.kind == LowerInst::VecWithLane) {
        auto value = intoLane(f, use(f, inst, inst.value), source);
        f.define(inst.result, f.builder.CreateInsertElement(from, value, index, nameOf(f, inst.result)));
        return;
    }

    auto lane = outOfLane(f, f.builder.CreateExtractElement(from, index), inst.result.type);

    nameResult(f, lane, inst.result);
    f.define(inst.result, lane);
}

static void genVecShuffle(FunGen& f, LowerInstVecShuffle& inst) {
    auto left = use(f, inst, inst.left);
    auto right = use(f, inst, inst.right);

    // One entry per lane of the *result*, each naming a lane of the two sources concatenated - which
    // is what a `shufflevector` mask means as well, so the pattern is copied across as it stands.
    llvm::SmallVector<int, 16> pattern;
    for(auto entry: inst.pattern()) pattern.push_back(int(entry));

    f.define(inst.result, f.builder.CreateShuffleVector(left, right, pattern, nameOf(f, inst.result)));
}

/*
 * A reduction, in the order Design-Vector §4.5 makes a stated language property.
 *
 * Integer and mask reductions are the LLVM intrinsic: they are associative, so the order the target
 * picks cannot be observed, and the intrinsic is what its own lowering knows how to select.
 *
 * A float sum or product is not associative and the order *is* observable, so it is built here as
 * the same adjacent-pair tree the other two backends build - `(a0+a1) + (a2+a3)` for four lanes -
 * rather than as `llvm.vector.reduce.fadd`, which is strictly sequential without `reassoc` and
 * unspecified with it. Neither of those is the answer this language states, and a checksum that
 * differs between two backends of one compiler is the bug report §13 predicts.
 */
static llvm::Value* genReduceTree(FunGen& f, llvm::Value* value, bool isMul) {
    auto lanes = llvm::cast<llvm::FixedVectorType>(value->getType())->getNumElements();

    while(lanes > 1) {
        llvm::SmallVector<int, 16> even, odd;

        for(unsigned i = 0; i < lanes; i += 2) {
            even.push_back(int(i));
            odd.push_back(int(i + 1));
        }

        auto lhs = f.builder.CreateShuffleVector(value, even);
        auto rhs = f.builder.CreateShuffleVector(value, odd);

        value = isMul ? f.builder.CreateFMul(lhs, rhs) : f.builder.CreateFAdd(lhs, rhs);
        lanes /= 2;
    }

    return f.builder.CreateExtractElement(value, uint64_t(0));
}

static llvm::Intrinsic::ID reduceIntrinsic(LowerReduce reduce, bool isFloat) {
    switch(reduce) {
        case LowerReduce::Add:  return llvm::Intrinsic::vector_reduce_add;
        case LowerReduce::Mul:  return llvm::Intrinsic::vector_reduce_mul;
        case LowerReduce::Min:  return isFloat ? llvm::Intrinsic::vector_reduce_fmin
                                               : llvm::Intrinsic::vector_reduce_umin;
        case LowerReduce::IMin: return llvm::Intrinsic::vector_reduce_smin;
        case LowerReduce::Max:  return isFloat ? llvm::Intrinsic::vector_reduce_fmax
                                               : llvm::Intrinsic::vector_reduce_umax;
        case LowerReduce::IMax: return llvm::Intrinsic::vector_reduce_smax;
        case LowerReduce::And:  return llvm::Intrinsic::vector_reduce_and;
        case LowerReduce::Or:   return llvm::Intrinsic::vector_reduce_or;

        // Not an intrinsic here either: `genVecReduce` answers it above with the bitcast that is its
        // whole expansion, exactly as it answers `FirstSet` - so this table is never asked.
        case LowerReduce::Bits:
            assertTrue("a movemask reduction has no reduce intrinsic" == nullptr);
            break;

        // Not an intrinsic on this side either: `genVecReduce` answers it above with the bitcast and
        // the trailing-zero count that are its whole expansion, and never reaches this table.
        case LowerReduce::FirstSet:
            assertTrue("a first-set reduction has no reduce intrinsic" == nullptr);
            break;
    }

    return llvm::Intrinsic::vector_reduce_add;
}

static void genVecReduce(FunGen& f, LowerInstVecReduce& inst) {
    auto from = use(f, inst, inst.from);
    auto source = f.base[inst.from]->type;
    auto reduce = inst.getReduce();

    if(isFloatVector(source) && (reduce == LowerReduce::Add || reduce == LowerReduce::Mul)) {
        auto tree = genReduceTree(f, from, reduce == LowerReduce::Mul);

        nameResult(f, tree, inst.result);
        f.define(inst.result, tree);
        return;
    }

    /*
     * The lowest set lane, which is the mask read as a *number* rather than reduced.
     *
     * `<N x i1>` bitcast to an `iN` is the movemask this backend never has to name: lane `i` lands
     * in bit `i` on every little-endian target LLVM has, and the pattern is one it selects
     * `pmovmskb` for outright. The sentinel bit one past the last lane is what gives "nothing is
     * set" the lane count as its answer, and it is also what makes the operand non-zero - which the
     * `cttz` below needs, since the IR's own kind says a zero operand is undefined.
     *
     * **The sentinel is spent only where there is room for it**, which is the same rule the x64
     * expansion follows and for the same reason: a 32-lane mask fills the `i32` it was bitcast into,
     * and bit 32 is not a bit that word has. Where none fits there is nothing to add - `cttz` told
     * that a zero operand is *not* poison answers the operand's width, and the width is the lane
     * count exactly. That is one `tzcnt` on a target that has one and a scan plus a correction on a
     * target that does not, which is LLVM's choice to make rather than ours.
     */
    /*
     * The mask as a number, which is what `Native.bits` names and what `FirstSet` below scans.
     *
     * The same bitcast, without the trailing-zero count on top: `<N x i1>` to an `iN` is the
     * movemask this backend never has to name, and lane `i` lands in bit `i` on every little-endian
     * target LLVM has. Nothing above the lane count is set, which is the kind's own promise, so the
     * zero extension into the `i32` the result is stated at is all that follows it.
     */
    if(reduce == LowerReduce::Bits) {
        auto bits = f.builder.CreateBitCast(from, llvm::IntegerType::get(f.gen.llvm, source.lanes()));
        auto value = f.builder.CreateZExt(bits, typeOf(f.gen, inst.result.type));

        nameResult(f, value, inst.result);
        f.define(inst.result, value);
        return;
    }

    if(reduce == LowerReduce::FirstSet) {
        auto lanes = source.lanes();
        auto width = lanes <= 32 ? 32 : 64;
        auto counted = llvm::IntegerType::get(f.gen.llvm, width);
        auto bits = f.builder.CreateBitCast(from, llvm::IntegerType::get(f.gen.llvm, lanes));
        auto wide = f.builder.CreateZExt(bits, counted);

        auto fits = lanes < U32(width);
        auto marked = fits ? f.builder.CreateOr(wide, llvm::ConstantInt::get(counted, U64(1) << lanes))
                           : wide;

        llvm::Value* args[] = { marked, fits ? f.builder.getTrue() : f.builder.getFalse() };
        auto first = f.builder.CreateIntrinsic(llvm::Intrinsic::cttz, { counted }, args);

        // Into the `i32` the kind states, which is a truncation only where the count had to be
        // taken at a width the sentinel fit in.
        auto value = f.builder.CreateZExtOrTrunc(first, typeOf(f.gen, inst.result.type));

        nameResult(f, value, inst.result);
        f.define(inst.result, value);
        return;
    }

    // How many lanes of a mask are set is a sum of ones rather than a reduction of truth values, so
    // the `<N x i1>` is widened into the result's own width first - `and` and `or` are the two that
    // reduce the mask as it stands.
    if(source.isMask() && reduce == LowerReduce::Add) {
        auto lanes = llvm::FixedVectorType::get(typeOf(f.gen, inst.result.type), source.lanes());
        auto widened = f.builder.CreateZExt(from, lanes);

        f.define(inst.result, f.builder.CreateIntrinsic(llvm::Intrinsic::vector_reduce_add,
                                                        { lanes }, { widened }, nullptr,
                                                        nameOf(f, inst.result)));
        return;
    }

    auto id = reduceIntrinsic(reduce, isFloatVector(source));
    auto reduced = f.builder.CreateIntrinsic(id, { from->getType() }, { from });

    // An 8- or 16-bit lane's reduction is that lane's width, and a mask's is an i1, so both are
    // widened into the scalar form the result states - the same rule `vlane` answers by.
    auto value = outOfLane(f, reduced, inst.result.type);

    nameResult(f, value, inst.result);
    f.define(inst.result, value);
}

/*
 * Memory.
 *
 * Nothing here promises more alignment than one byte. The lower IR states an alignment for an
 * allocation and nothing for an access, so an access is only known to be aligned by whatever laid
 * the object out - and under-promising costs nothing on the targets this backend has (an unaligned
 * scalar move is the same instruction) where over-promising would be undefined behaviour. When the
 * IR carries an alignment on Load and Store, it belongs here.
 */
static const llvm::Align kUnknownAlign = llvm::Align(1);

static void genAlloca(FunGen& f, LowerInstAlloca& inst) {
    auto count = use(f, inst, inst.byteCount);
    auto byte = llvm::Type::getInt8Ty(f.gen.llvm);
    llvm::AllocaInst* value;

    if(llvm::isa<llvm::ConstantInt>(count)) {
        // A fixed-size allocation is a frame object for the whole function wherever it was written -
        // which is the same test collectFrameObjects makes in the x64 backend - so it goes in the
        // entry block, where LLVM gives it that meaning too. Left where it stands, one inside a loop
        // would allocate again on every iteration.
        llvm::IRBuilder<> entry(f.entry, f.entry->getFirstInsertionPt());
        value = entry.CreateAlloca(byte, count, nameOf(f, inst.result));
    } else {
        value = f.builder.CreateAlloca(byte, count, nameOf(f, inst.result));
    }

    value->setAlignment(llvm::Align(inst.alignment));
    f.define(inst.result, value);
}

static void genLoad(FunGen& f, LowerInstLoad& inst) {
    auto from = use(f, inst, inst.from);
    auto result = inst.result.type;
    auto type = typeOf(f.gen, result);
    auto width = inst.getWidth();
    auto name = nameOf(f, inst.result);
    llvm::Value* value;

    // A vector is loaded whole - there is no narrower access to widen from, and the validator says
    // so. The overread flag needs nothing here: LLVM has no way to be told that a load deliberately
    // reads past the end of the object it names, and needs none, because the load it is given names
    // the whole width it reads and no object smaller than that is derived from anything here.
    if(isVectorLike(result) || isFloat(result) || (isPtr(result) && width == 8)) {
        value = f.builder.CreateAlignedLoad(type, from, kUnknownAlign, name);
    } else {
        auto loaded = f.builder.CreateAlignedLoad(intTypeOfWidth(f.gen, width), from, kUnknownAlign);

        // A narrow load is widened into its result the way the access says it should be: `loads`
        // carries the sign bit up, `load` fills with zeroes.
        value = isPtr(result) ? f.builder.CreateIntToPtr(loaded, type, name)
                              : f.builder.CreateIntCast(loaded, type, inst.isSigned(), name);
    }

    f.define(inst.result, value);
}

static void genStore(FunGen& f, LowerInstStore& inst) {
    auto to = use(f, inst, inst.to);
    auto value = use(f, inst, inst.value);
    auto type = f.base[inst.value]->type;
    auto width = inst.getWidth();

    if(!isFloat(type) && !isVectorLike(type)) {
        auto word = intTypeOfWidth(f.gen, width);

        if(isPtr(type)) {
            if(width != 8) value = f.builder.CreatePtrToInt(value, word);
        } else {
            value = f.builder.CreateIntCast(value, word, false);
        }
    }

    f.builder.CreateAlignedStore(value, to, kUnknownAlign);
}

static void genCopy(FunGen& f, LowerInstCopy& inst) {
    f.builder.CreateMemCpy(use(f, inst, inst.to), kUnknownAlign,
                           use(f, inst, inst.from), kUnknownAlign,
                           use(f, inst, inst.count));
}

static void genSetPattern(FunGen& f, LowerInstSetPattern& inst) {
    auto pattern = f.builder.CreateIntCast(use(f, inst, inst.pattern), llvm::Type::getInt8Ty(f.gen.llvm), false);
    f.builder.CreateMemSet(use(f, inst, inst.to), pattern, use(f, inst, inst.count), kUnknownAlign);
}

/*
 * Calls.
 */

void defineResults(FunGen& f, LowerInst& inst, llvm::Value* call) {
    auto created = inst.created();

    if(created.length == 1) {
        if(auto name = nameOf(f, created[0]); !name.empty()) call->setName(name);
        f.define(created[0], call);
    } else if(created.length > 1) {
        // Several results are one anonymous struct, which is the only aggregate this backend
        // produces - see resultTypeOf.
        for(Size i = 0; i < created.length; i++) {
            f.define(created[i], f.builder.CreateExtractValue(call, i, nameOf(f, created[i])));
        }
    }
}

static void genCall(FunGen& f, LowerInstCall& inst) {
    if(inst.getCallType() == LowerCallType::Syscall) {
        defineResults(f, inst, genSyscall(f, inst));
        return;
    }

    auto used = inst.used();
    auto created = inst.created();

    llvm::SmallVector<llvm::Value*, 8> args;
    for(Size i = 1; i < used.length; i++) {
        args.push_back(use(f, inst, used[i]));
    }

    auto target = f.base[used[0]];
    llvm::CallInst* call;

    if(target->inst()->kind == LowerInst::Fun) {
        auto callee = f.gen.functionOf(((LowerInstFun*)target->inst())->target);

        if(!callee) {
            f.context.diagnostics.error("llvm: call to a function that is not in this module"_v, inst.source);
            return;
        }

        call = f.builder.CreateCall(callee, args);
    } else {
        // An indirect call has no declaration to take a signature from, so the signature is the one
        // the call site states: the types of the values it passes and of the ones it takes back.
        llvm::SmallVector<llvm::Type*, 8> argTypes;
        llvm::SmallVector<llvm::Type*, 2> resultTypes;

        for(Size i = 1; i < used.length; i++) argTypes.push_back(typeOf(f.gen, f.base[used[i]]->type));
        for(Size i = 0; i < created.length; i++) resultTypes.push_back(typeOf(f.gen, created[i].type));

        call = f.builder.CreateCall(signatureOf(f.gen, argTypes, resultTypes), use(f, inst, used[0]), args);
    }

    call->setCallingConv(conventionOf(inst.getCallType()));
    defineResults(f, inst, call);
}

/*
 * Control flow.
 */

static void genJe(FunGen& f, LowerInstJe& inst) {
    auto condition = genCondition(f, use(f, inst, inst.cond));
    auto branch = f.builder.CreateCondBr(condition, f.block(inst.then), f.block(inst.otherwise));

    // What the IR knows about the branch and the CFG does not - see EdgeLikelihood. A branch that
    // states nothing is left alone rather than given a neutral pair, so that LLVM's own estimate
    // still applies where the frontend had no opinion.
    if(inst.hasLikelihood()) {
        llvm::MDBuilder metadata(f.gen.llvm);
        branch->setMetadata(llvm::LLVMContext::MD_prof,
                            metadata.createBranchWeights(inst.likelihood[0].weight, inst.likelihood[1].weight));
    }
}

static void genRet(FunGen& f, LowerInstRet& inst) {
    auto used = inst.used();

    if(used.length == 0) {
        f.builder.CreateRetVoid();
    } else if(used.length == 1) {
        f.builder.CreateRet(use(f, inst, used[0]));
    } else {
        llvm::Value* aggregate = llvm::PoisonValue::get(f.target.getReturnType());

        for(Size i = 0; i < used.length; i++) {
            aggregate = f.builder.CreateInsertValue(aggregate, use(f, inst, used[i]), i);
        }

        f.builder.CreateRet(aggregate);
    }
}

void genInst(FunGen& f, LowerInst& inst) {
    switch(inst.kind) {
        case LowerInst::Arg:
            // Defined by the function itself, from the LLVM argument of the same position.
            break;
        case LowerInst::Nop:
            break;
        case LowerInst::Imm:
            genImm(f, (LowerImm&)inst);
            break;
        case LowerInst::Global:
            genGlobalRef(f, (LowerInstGlobal&)inst);
            break;
        case LowerInst::Fun:
            genFunRef(f, (LowerInstFun&)inst);
            break;
        case LowerInst::Cast:
            genCast(f, (LowerInstCast&)inst);
            break;
        case LowerInst::Bitcast:
            genBitcast(f, (LowerInstUnary&)inst);
            break;
        case LowerInst::Set:
        case LowerInst::Neg:
        case LowerInst::Not:
        case LowerInst::Bswap:
        case LowerInst::Sqrt:
        case LowerInst::Abs:
        case LowerInst::Trunc:
        case LowerInst::Floor:
        case LowerInst::Ceil:
        case LowerInst::Round:
            genUnary(f, (LowerInstUnary&)inst);
            break;
        case LowerInst::Fma:
            genFma(f, (LowerInstFma&)inst);
            break;
        case LowerInst::Add:
        case LowerInst::Sub:
        case LowerInst::Mul:
        case LowerInst::IMul:
        case LowerInst::Div:
        case LowerInst::IDiv:
        case LowerInst::Rem:
        case LowerInst::IRem:
        case LowerInst::MulHi:
        case LowerInst::IMulHi:
        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar:
        case LowerInst::Rol:
        case LowerInst::Ror:
        case LowerInst::And:
        case LowerInst::Or:
        case LowerInst::Xor:
            genBinary(f, (LowerInstBinary&)inst);
            break;
        case LowerInst::BitsUpTo:
        case LowerInst::GatherBits:
        case LowerInst::ScatterBits:
            genBitOperation(f, (LowerInstBinary&)inst);
            break;
        case LowerInst::Cmp:
            genCmp(f, (LowerInstCmp&)inst);
            break;
        case LowerInst::Select:
            genSelect(f, (LowerInstSelect&)inst);
            break;
        case LowerInst::VecSplat:
            genVecSplat(f, (LowerInstVecSplat&)inst);
            break;
        case LowerInst::VecLane:
        case LowerInst::VecWithLane:
            genVecLane(f, (LowerInstVecLane&)inst);
            break;
        case LowerInst::VecShuffle:
            genVecShuffle(f, (LowerInstVecShuffle&)inst);
            break;
        case LowerInst::VecReduce:
            genVecReduce(f, (LowerInstVecReduce&)inst);
            break;
        case LowerInst::Alloca:
            genAlloca(f, (LowerInstAlloca&)inst);
            break;
        case LowerInst::Load:
            genLoad(f, (LowerInstLoad&)inst);
            break;
        case LowerInst::Store:
            genStore(f, (LowerInstStore&)inst);
            break;
        case LowerInst::Copy:
            genCopy(f, (LowerInstCopy&)inst);
            break;
        case LowerInst::SetPattern:
            genSetPattern(f, (LowerInstSetPattern&)inst);
            break;
        case LowerInst::Call:
            genCall(f, (LowerInstCall&)inst);
            break;
        case LowerInst::Intrinsic:
            genIntrinsic(f, (LowerInstIntrinsic&)inst);
            break;
        case LowerInst::Je:
            genJe(f, (LowerInstJe&)inst);
            break;
        case LowerInst::Jmp:
            f.builder.CreateBr(f.block(((LowerInstJmp&)inst).then));
            break;
        case LowerInst::Ret:
            genRet(f, (LowerInstRet&)inst);
            break;
        case LowerInst::Unreachable:
            f.builder.CreateUnreachable();
            break;
        case LowerInst::Phi:
            // Created empty before any block was built, and filled once every block exists.
            break;
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
            // Created by the x64 backend's own transforms, which run on a copy of the IR this
            // backend never sees. Reaching one means two targets were run over one module.
            f.context.diagnostics.error("llvm: a target-lowered instruction reached the LLVM backend"_v, inst.source);
            break;
        default:
            f.context.diagnostics.error("llvm: unsupported instruction"_v, inst.source);
            break;
    }
}

} // namespace llvmgen
