#include "build.h"

#include <llvm/IR/InlineAsm.h>
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

    if(isFloat(source) && isFloat(target)) {
        value = f.builder.CreateFPCast(from, type, nameOf(f, inst.result));
    } else if(isFloat(source)) {
        value = inst.isSignedResult() ? f.builder.CreateFPToSI(from, type, nameOf(f, inst.result))
                                      : f.builder.CreateFPToUI(from, type, nameOf(f, inst.result));
    } else if(isFloat(target)) {
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
    switch(type) {
        case LowerType::Int32:
        case LowerType::Float32:
            return 32;
        default:
            return 64;
    }
}

static void genBitcast(FunGen& f, LowerInstUnary& inst) {
    auto from = use(f, inst, inst.from);
    auto source = f.base[inst.from]->type;
    auto target = inst.result.type;
    auto type = typeOf(f.gen, target);
    auto name = nameOf(f, inst.result);
    llvm::Value* value;

    if(isPtr(source) && isPtr(target)) {
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
            value = isFloat(inst.result.type) ? f.builder.CreateFNeg(from, name)
                                              : f.builder.CreateNeg(from, name);
            break;
        default:
            value = f.builder.CreateNot(from, name);
            break;
    }

    f.define(inst.result, value);
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
    }

    return llvm::CmpInst::ICMP_EQ;
}

// Ordered, so a comparison against a NaN is false whichever way it is written. The signed forms are
// the same as the unsigned ones here: a float has one ordering, and the distinction the IR carries
// is about integers.
static llvm::CmpInst::Predicate floatPredicate(LowerCmp cmp) {
    switch(cmp) {
        case LowerCmp::eq:  return llvm::CmpInst::FCMP_OEQ;
        case LowerCmp::neq: return llvm::CmpInst::FCMP_ONE;
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

    auto compared = isFloat(type) ? f.builder.CreateFCmp(floatPredicate(inst.getCmp()), lhs, rhs)
                                  : f.builder.CreateICmp(intPredicate(inst.getCmp()), lhs, rhs);

    // The lower IR has no one-bit type: a comparison answers Int, which is 0 or 1.
    f.define(inst.result, f.builder.CreateZExt(compared, typeOf(f.gen, inst.result.type), nameOf(f, inst.result)));
}

static void genBinary(FunGen& f, LowerInstBinary& inst) {
    auto lhs = use(f, inst, inst.lhs);
    auto rhs = use(f, inst, inst.rhs);
    auto lhsType = f.base[inst.lhs]->type;
    auto rhsType = f.base[inst.rhs]->type;
    auto result = inst.result.type;
    auto name = nameOf(f, inst.result);
    auto isFloatOp = isFloat(result);
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

        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar: {
            // The IR lets the amount be either integer width; LLVM wants both operands alike.
            auto amount = f.builder.CreateZExtOrTrunc(rhs, lhs->getType());

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
    auto condition = genCondition(f, use(f, inst, inst.cmp));
    auto lhs = use(f, inst, inst.lhs);
    auto rhs = use(f, inst, inst.rhs);

    // `select` yields its first operand when the condition holds, matching the x64 form's tie.
    f.define(inst.result, f.builder.CreateSelect(condition, lhs, rhs, nameOf(f, inst.result)));
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

    if(isFloat(result) || (isPtr(result) && width == 8)) {
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

    if(!isFloat(type)) {
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
            genUnary(f, (LowerInstUnary&)inst);
            break;
        case LowerInst::Add:
        case LowerInst::Sub:
        case LowerInst::Mul:
        case LowerInst::IMul:
        case LowerInst::Div:
        case LowerInst::IDiv:
        case LowerInst::Rem:
        case LowerInst::IRem:
        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar:
        case LowerInst::And:
        case LowerInst::Or:
        case LowerInst::Xor:
            genBinary(f, (LowerInstBinary&)inst);
            break;
        case LowerInst::Cmp:
            genCmp(f, (LowerInstCmp&)inst);
            break;
        case LowerInst::Select:
            genSelect(f, (LowerInstSelect&)inst);
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
        case LowerInst::Phi:
            // Created empty before any block was built, and filled once every block exists.
            break;
        case LowerInst::X86Address:
        case LowerInst::X86Lea:
        case LowerInst::X86PushArg:
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
