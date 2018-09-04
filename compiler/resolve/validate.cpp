#include "validate.h"
#include "module.h"

static bool validateBaseValue(Diagnostics* diagnostics, Value* value) {
    // Make sure the uses list is correct.
    // False negatives are handled by calling validateOperand() in further instructions, so we just check for false positives here.
    for(const Use& use: value->uses) {
        auto found = false;
        auto user = use.user;
        for(Size i = 0; i < user->usedCount; i++) {
            if(user->usedValues[i] == value) {
                found = true;
                break;
            }
        }

        if(!found) {
            diagnostics->error("uses list for value is out of sync"_buffer, &value->source, noSource);
            return false;
        }
    }

    return validateType(diagnostics, value->type, &value->source, value->block->function);
}

static bool validateOperand(Diagnostics* diagnostics, Value* source, Value* op) {
    // Make sure the operand is actually still in the block.
    // Transformation passes could potentially remove instructions without updating all uses correctly.
    auto found = false;
    for(Value* inst: op->block->instructions) {
        if(inst == op) {
            found = true;
            break;
        }
    }

    if(!found) {
        diagnostics->error("instruction has dangling operand"_buffer, &source->source, noSource);
        return false;
    }

    // Make sure the operand uses list is correct.
    found = false;
    for(const Use& use: op->uses) {
        if(use.user == source) {
            found = true;
            break;
        }
    }

    if(!found) {
        diagnostics->error("uses list for operand is out of sync"_buffer, &op->source, noSource);
        return false;
    }

    return true;
}

bool validateArg(Diagnostics* diagnostics, Arg* arg) {
    if(!validateBaseValue(diagnostics, arg)) return false;

    Function* fun = arg->block->function;
    if(arg->index >= fun->args.size()) {
        diagnostics->error("argument index %@ is out of bounds for function %@"_buffer, &arg->source, noSource, arg->index, fun->name);
        return false;
    }

    if(fun->args[arg->index] != arg) {
        diagnostics->error("argument list for function %@ is out of sync"_buffer, &arg->source, noSource, fun->name);
        return false;
    }

    return true;
}

bool validateGlobal(Diagnostics* diagnostics, Global* global) {
    if(!validateBaseValue(diagnostics, global)) return false;

    if(global->ast || global->resolving) {
        diagnostics->error("global is not fully resolved"_buffer, &global->source, noSource);
        return false;
    }

    if(global->type->kind != Type::Ref) {
        diagnostics->error("global must be a reference type"_buffer, &global->source, noSource);
        return false;
    }

    return true;
}

bool validateInt(Diagnostics* diagnostics, ConstInt* i) {
    if(!validateBaseValue(diagnostics, i)) return false;

    if(i->type->kind != Type::Int) {
        diagnostics->error("integer constant must have integer type"_buffer, &i->source, noSource);
        return false;
    }

    auto type = (IntType*)i->type;
    auto max = U64(1) << type->bits;

    if(i->value > max) {
        diagnostics->error("integer constant out of range for target type. Constant value: %@, max value: %@"_buffer, &i->source, noSource, i->value, max);
        return false;
    }

    return true;
}

bool validateFloat(Diagnostics* diagnostics, ConstFloat* i) {
    if(!validateBaseValue(diagnostics, i)) return false;

    if(i->type->kind != Type::Float) {
        diagnostics->error("floating point constant must have float type"_buffer, &i->source, noSource);
        return false;
    }

    auto type = (FloatType*)i->type;
    auto max = U64(1) << type->bits;

    if(i->value > max) {
        diagnostics->error("integer constant out of range for target type. Constant value: %@, max value: %@"_buffer, &i->source, noSource, i->value, max);
        return false;
    }

    return true;
}

bool validateString(Diagnostics* diagnostics, ConstString* i) {
    if(!validateBaseValue(diagnostics, i)) return false;

    // TODO: Validate the string encoding.
    return true;
}

bool validateTrunc(Diagnostics* diagnostics, InstTrunc* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    if(!validateOperand(diagnostics, i, i->from)) return false;

    // The types themselves have already been validated at this point.
    auto valid = i->from->type->kind == Type::Int && i->type->kind == Type::Int;
    if(valid) {
        auto from = (IntType*)i->from->type;
        auto to = (IntType*)i->type;
        valid = from->bits > to->bits;
    }

    if(!valid) {
        diagnostics->error("trunc instructions must truncate from an integer type to a smaller integer type"_buffer, &i->source, noSource);
        return false;
    }

    return true;
}

bool validateFTrunc(Diagnostics* diagnostics, InstFTrunc* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    if(!validateOperand(diagnostics, i, i->from)) return false;

    // The types themselves have already been validated at this point.
    auto valid = i->from->type->kind == Type::Float && i->type->kind == Type::Float;
    if(valid) {
        auto from = (FloatType*)i->from->type;
        auto to = (FloatType*)i->type;
        valid = from->bits > to->bits;
    }

    if(!valid) {
        diagnostics->error("ftrunc instructions must truncate from a floating point type to a smaller floating point type"_buffer, &i->source, noSource);
        return false;
    }

    return true;
}

static bool validateIntExt(Diagnostics* diagnostics, InstCast* i, const String& name) {
    if(!validateBaseValue(diagnostics, i)) return false;
    if(!validateOperand(diagnostics, i, i->from)) return false;

    // The types themselves have already been validated at this point.
    auto valid = i->from->type->kind == Type::Int && i->type->kind == Type::Int;
    if(valid) {
        auto from = (IntType*)i->from->type;
        auto to = (IntType*)i->type;
        valid = from->bits < to->bits;
    }

    if(!valid) {
        diagnostics->error("%@ instructions must widen from an integer type to a larger integer type"_buffer, &i->source, noSource, name);
        return false;
    }

    return true;
}

bool validateZExt(Diagnostics* diagnostics, InstZExt* i) {
    return validateIntExt(diagnostics, i, "zext");
}

bool validateSExt(Diagnostics* diagnostics, InstSExt* i) {
    return validateIntExt(diagnostics, i, "sext");
}

bool validateFExt(Diagnostics* diagnostics, InstFExt* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    if(!validateOperand(diagnostics, i, i->from)) return false;

    // The types themselves have already been validated at this point.
    auto valid = i->from->type->kind == Type::Float && i->type->kind == Type::Float;
    if(valid) {
        auto from = (FloatType*)i->from->type;
        auto to = (FloatType*)i->type;
        valid = from->bits < to->bits;
    }

    if(!valid) {
        diagnostics->error("fext instructions must widen from a floating point type to a larger floating point type"_buffer, &i->source, noSource);
        return false;
    }

    return true;
}

static bool validateFloatToInt(Diagnostics* diagnostics, InstCast* i, const String& name) {
    if(!validateBaseValue(diagnostics, i)) return false;
    if(!validateOperand(diagnostics, i, i->from)) return false;

    auto valid = i->from->type->kind == Type::Float && i->type->kind == Type::Int;
    if(!valid) {
        diagnostics->error("%@ instructions must convert from a floating point type to an integer type"_buffer, &i->source, noSource, name);
        return false;
    }

    return true;
}

bool validateFToI(Diagnostics* diagnostics, InstFToI* i) {
    return validateFloatToInt(diagnostics, i, "ftoi");
}

bool validateFToUI(Diagnostics* diagnostics, InstFToUI* i) {
    return validateFloatToInt(diagnostics, i, "ftoui");
}

static bool validateIntToFloat(Diagnostics* diagnostics, InstCast* i, const String& name) {
    if(!validateBaseValue(diagnostics, i)) return false;
    if(!validateOperand(diagnostics, i, i->from)) return false;

    auto valid = i->from->type->kind == Type::Int && i->type->kind == Type::Float;
    if(!valid) {
        diagnostics->error("%@ instructions must convert from an integer type to a floating point type"_buffer, &i->source, noSource, name);
        return false;
    }

    return true;
}

bool validateIToF(Diagnostics* diagnostics, InstIToF* i) {
    return validateIntToFloat(diagnostics, i, "itof");
}

bool validateUIToF(Diagnostics* diagnostics, InstUIToF* i) {
    return validateIntToFloat(diagnostics, i, "uitof");
}

static bool validateIntArithmetic(Diagnostics* diagnostics, InstBinary* i, const String& name) {
    if(!validateBaseValue(diagnostics, i)) return false;
    if(!validateOperand(diagnostics, i, i->lhs)) return false;
    if(!validateOperand(diagnostics, i, i->rhs)) return false;

    auto valid = i->lhs->type->kind == Type::Int && i->rhs->type->kind == Type::Int && i->type->kind == Type::Int;
    if(valid) {
        auto lhs = (IntType*)i->lhs->type;
        auto rhs = (IntType*)i->rhs->type;
        auto to = (IntType*)i->type;
        valid = (lhs->bits == rhs->bits == to->bits) && (IntType::bitsForWidth(to->width) == to->bits);
    }

    if(!valid) {
        diagnostics->error("%@ instructions must have register-sized integer inputs and output of the same width"_buffer, &i->source, noSource, name);
        return false;
    }

    return true;
}

bool validateAdd(Diagnostics* diagnostics, InstAdd* i) {
    return validateIntArithmetic(diagnostics, i, "add");
}

bool validateSub(Diagnostics* diagnostics, InstSub* i) {
    return validateIntArithmetic(diagnostics, i, "sub");
}

bool validateMul(Diagnostics* diagnostics, InstMul* i) {
    return validateIntArithmetic(diagnostics, i, "mul");
}

bool validateDiv(Diagnostics* diagnostics, InstDiv* i) {
    return validateIntArithmetic(diagnostics, i, "div");
}

bool validateIDiv(Diagnostics* diagnostics, InstIDiv* i) {
    return validateIntArithmetic(diagnostics, i, "idiv");
}

bool validateRem(Diagnostics* diagnostics, InstRem* i) {
    return validateIntArithmetic(diagnostics, i, "rem");
}

bool validateIRem(Diagnostics* diagnostics, InstIRem* i) {
    return validateIntArithmetic(diagnostics, i, "irem");
}

static bool validateFloatArithmetic(Diagnostics* diagnostics, InstBinary* i, const String& name) {
    if(!validateBaseValue(diagnostics, i)) return false;
    if(!validateOperand(diagnostics, i, i->lhs)) return false;
    if(!validateOperand(diagnostics, i, i->rhs)) return false;

    auto valid = i->lhs->type->kind == Type::Float && i->rhs->type->kind == Type::Float && i->type->kind == Type::Float;
    if(valid) {
        auto lhs = (FloatType*)i->lhs->type;
        auto rhs = (FloatType*)i->rhs->type;
        auto to = (FloatType*)i->type;
        valid = lhs->bits == rhs->bits == to->bits;
    }

    if(!valid) {
        diagnostics->error("%@ instructions must have register-sized floating point inputs and output of the same width"_buffer, &i->source, noSource, name);
        return false;
    }

    return true;
}

bool validateFAdd(Diagnostics* diagnostics, InstFAdd* i) {
    return validateFloatArithmetic(diagnostics, i, "fadd");
}

bool validateFSub(Diagnostics* diagnostics, InstFSub* i) {
    return validateFloatArithmetic(diagnostics, i, "fsub");
}

bool validateFMul(Diagnostics* diagnostics, InstFMul* i) {
    return validateFloatArithmetic(diagnostics, i, "fmul");
}

bool validateFDiv(Diagnostics* diagnostics, InstFDiv* i) {
    return validateFloatArithmetic(diagnostics, i, "fdiv");
}

bool validateICmp(Diagnostics* diagnostics, InstICmp* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    if(!validateOperand(diagnostics, i, i->lhs)) return false;
    if(!validateOperand(diagnostics, i, i->rhs)) return false;

    auto valid = i->lhs->type->kind == Type::Int && i->rhs->type->kind == Type::Int && i->type->kind == Type::Int;
    if(valid) {
        auto lhs = (IntType*)i->lhs->type;
        auto rhs = (IntType*)i->rhs->type;
        auto to = (IntType*)i->type;
        valid = lhs->bits == rhs->bits && IntType::bitsForWidth(to->width) == to->bits && to->width == IntType::Bool;
    }

    if(!valid) {
        diagnostics->error("icmp instructions must have register-sized integer inputs of the same width, as well as Bool output"_buffer, &i->source, noSource);
        return false;
    }

    return true;
}

bool validateFCmp(Diagnostics* diagnostics, InstFCmp* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    if(!validateOperand(diagnostics, i, i->lhs)) return false;
    if(!validateOperand(diagnostics, i, i->rhs)) return false;

    auto valid = i->lhs->type->kind == Type::Float && i->rhs->type->kind == Type::Float && i->type->kind == Type::Int;
    if(valid) {
        auto lhs = (FloatType*)i->lhs->type;
        auto rhs = (FloatType*)i->rhs->type;
        auto to = (IntType*)i->type;
        valid = lhs->bits == rhs->bits && to->width == IntType::Bool;
    }

    if(!valid) {
        diagnostics->error("fcmp instructions must have register-sized floating point inputs of the same width, as well as Bool output"_buffer, &i->source, noSource);
        return false;
    }

    return true;
}

static bool validateIntShift(Diagnostics* diagnostics, InstShift* i, const String& name) {
    if(!validateBaseValue(diagnostics, i)) return false;
    if(!validateOperand(diagnostics, i, i->arg)) return false;
    if(!validateOperand(diagnostics, i, i->amount)) return false;

    auto valid = i->arg->type->kind == Type::Int && i->amount->type->kind == Type::Int && i->type->kind == Type::Int;
    if(valid) {
        auto arg = (IntType*)i->arg->type;
        auto amount = (IntType*)i->amount->type;
        auto to = (IntType*)i->type;

        valid = arg->bits == to->bits && IntType::bitsForWidth(to->width) == to->bits;
        valid = valid && (IntType::bitsForWidth(amount->width) == amount->bits);
    }

    if(!valid) {
        diagnostics->error("%@ instructions must have register-sized integer source and output of the same width, as well as a register-sized integer amount"_buffer, &i->source, noSource, name);
        return false;
    }

    return true;
}

bool validateShl(Diagnostics* diagnostics, InstShl* i) {
    return validateIntShift(diagnostics, i, "shl");
}

bool validateShr(Diagnostics* diagnostics, InstShr* i) {
    return validateIntShift(diagnostics, i, "shr");
}

bool validateSar(Diagnostics* diagnostics, InstSar* i) {
    return validateIntShift(diagnostics, i, "sar");
}

bool validateAnd(Diagnostics* diagnostics, InstAnd* i) {
    return validateIntArithmetic(diagnostics, i, "and");
}

bool validateOr(Diagnostics* diagnostics, InstOr* i) {
    return validateIntArithmetic(diagnostics, i, "or");
}

bool validateXor(Diagnostics* diagnostics, InstXor* i) {
    return validateIntArithmetic(diagnostics, i, "xor");
}

bool validateAddRef(Diagnostics* diagnostics, InstAddRef* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    if(!validateOperand(diagnostics, i, i->lhs)) return false;
    if(!validateOperand(diagnostics, i, i->rhs)) return false;

    auto valid = i->lhs->type->kind == Type::Ref && i->type->kind == Type::Ref;
    if(valid) {
        auto from = (RefType*)i->lhs->type;
        auto to = (RefType*)i->type;
        valid = !from->isTraced && !to->isTraced && from->to == to->to;
    }

    if(!valid) {
        diagnostics->error("addref source and resulting type must be untraced references to the same type"_buffer, &i->source, noSource);
        return false;
    }

    valid = i->rhs->type->kind == Type::Int;
    if(valid) {
        auto arg = (IntType*)i->rhs->type;
        valid = IntType::bitsForWidth(arg->width) == arg->bits;
    }

    if(!valid) {
        diagnostics->error("addref addition operand must be an integer type of register size"_buffer, &i->source, noSource);
        return false;
    }

    return true;
}

bool validateRecord(Diagnostics* diagnostics, InstRecord* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateTup(Diagnostics* diagnostics, InstTup* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateFun(Diagnostics* diagnostics, InstFun* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateAlloc(Diagnostics* diagnostics, InstAlloc* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateAllocArray(Diagnostics* diagnostics, InstAllocArray* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateLoad(Diagnostics* diagnostics, InstLoad* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateLoadField(Diagnostics* diagnostics, InstLoadField* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateLoadArray(Diagnostics* diagnostics, InstLoadArray* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateStore(Diagnostics* diagnostics, InstStore* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateStoreField(Diagnostics* diagnostics, InstStoreField* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateStoreArray(Diagnostics* diagnostics, InstStoreArray* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateGetField(Diagnostics* diagnostics, InstGetField* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateUpdateField(Diagnostics* diagnostics, InstUpdateField* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateArrayLength(Diagnostics* diagnostics, InstArrayLength* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateArrayCopy(Diagnostics* diagnostics, InstArrayCopy* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateArraySlice(Diagnostics* diagnostics, InstArraySlice* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateStringLength(Diagnostics* diagnostics, InstStringLength* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateStringData(Diagnostics* diagnostics, InstStringData* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateCall(Diagnostics* diagnostics, InstCall* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateCallDyn(Diagnostics* diagnostics, InstCallDyn* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateCallForeign(Diagnostics* diagnostics, InstCallForeign* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateJe(Diagnostics* diagnostics, InstJe* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateJmp(Diagnostics* diagnostics, InstJmp* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateRet(Diagnostics* diagnostics, InstRet* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validatePhi(Diagnostics* diagnostics, InstPhi* i) {
    if(!validateBaseValue(diagnostics, i)) return false;
    return true;
}

bool validateValue(Diagnostics* diagnostics, Value* value) {
    switch(value->kind) {
        case Inst::Arg:
            return validateArg(diagnostics, (Arg*)value);
        case Inst::Global:
            return validateGlobal(diagnostics, (Global*)value);
        case Inst::ConstInt:
            return validateInt(diagnostics, (ConstInt*)value);
        case Inst::ConstFloat:
            return validateFloat(diagnostics, (ConstFloat*)value);
        case Inst::ConstString:
            return validateString(diagnostics, (ConstString*)value);
        case Inst::InstNop:
            return true;
        case Inst::InstTrunc:
            return validateTrunc(diagnostics, (InstTrunc*)value);
        case Inst::InstFTrunc:
            return validateFTrunc(diagnostics, (InstFTrunc*)value);
        case Inst::InstZExt:
            return validateZExt(diagnostics, (InstZExt*)value);
        case Inst::InstSExt:
            return validateSExt(diagnostics, (InstSExt*)value);
        case Inst::InstFExt:
            return validateFExt(diagnostics, (InstFExt*)value);
        case Inst::InstFToI:
            return validateFToI(diagnostics, (InstFToI*)value);
        case Inst::InstFToUI:
            return validateFToUI(diagnostics, (InstFToUI*)value);
        case Inst::InstIToF:
            return validateIToF(diagnostics, (InstIToF*)value);
        case Inst::InstUIToF:
            return validateUIToF(diagnostics, (InstUIToF*)value);
        case Inst::InstAdd:
            return validateAdd(diagnostics, (InstAdd*)value);
        case Inst::InstSub:
            return validateSub(diagnostics, (InstSub*)value);
        case Inst::InstMul:
            return validateMul(diagnostics, (InstMul*)value);
        case Inst::InstDiv:
            return validateDiv(diagnostics, (InstDiv*)value);
        case Inst::InstIDiv:
            return validateIDiv(diagnostics, (InstIDiv*)value);
        case Inst::InstRem:
            return validateRem(diagnostics, (InstRem*)value);
        case Inst::InstIRem:
            return validateIRem(diagnostics, (InstIRem*)value);
        case Inst::InstFAdd:
            return validateFAdd(diagnostics, (InstFAdd*)value);
        case Inst::InstFSub:
            return validateFSub(diagnostics, (InstFSub*)value);
        case Inst::InstFMul:
            return validateFMul(diagnostics, (InstFMul*)value);
        case Inst::InstFDiv:
            return validateFDiv(diagnostics, (InstFDiv*)value);
        case Inst::InstICmp:
            return validateICmp(diagnostics, (InstICmp*)value);
        case Inst::InstFCmp:
            return validateFCmp(diagnostics, (InstFCmp*)value);
        case Inst::InstShl:
            return validateShl(diagnostics, (InstShl*)value);
        case Inst::InstShr:
            return validateShr(diagnostics, (InstShr*)value);
        case Inst::InstSar:
            return validateSar(diagnostics, (InstSar*)value);
        case Inst::InstAnd:
            return validateAnd(diagnostics, (InstAnd*)value);
        case Inst::InstOr:
            return validateOr(diagnostics, (InstOr*)value);
        case Inst::InstXor:
            return validateXor(diagnostics, (InstXor*)value);
        case Inst::InstAddRef:
            return validateAddRef(diagnostics, (InstAddRef*)value);
        case Inst::InstRecord:
            return validateRecord(diagnostics, (InstRecord*)value);
        case Inst::InstTup:
            return validateTup(diagnostics, (InstTup*)value);
        case Inst::InstFun:
            return validateFun(diagnostics, (InstFun*)value);
        case Inst::InstAlloc:
            return validateAlloc(diagnostics, (InstAlloc*)value);
        case Inst::InstAllocArray:
            return validateAllocArray(diagnostics, (InstAllocArray*)value);
        case Inst::InstLoad:
            return validateLoad(diagnostics, (InstLoad*)value);
        case Inst::InstLoadField:
            return validateLoadField(diagnostics, (InstLoadField*)value);
        case Inst::InstLoadArray:
            return validateLoadArray(diagnostics, (InstLoadArray*)value);
        case Inst::InstStore:
            return validateStore(diagnostics, (InstStore*)value);
        case Inst::InstStoreField:
            return validateStoreField(diagnostics, (InstStoreField*)value);
        case Inst::InstStoreArray:
            return validateStoreArray(diagnostics, (InstStoreArray*)value);
        case Inst::InstGetField:
            return validateGetField(diagnostics, (InstGetField*)value);
        case Inst::InstUpdateField:
            return validateUpdateField(diagnostics, (InstUpdateField*)value);
        case Inst::InstArrayLength:
            return validateArrayLength(diagnostics, (InstArrayLength*)value);
        case Inst::InstArrayCopy:
            return validateArrayCopy(diagnostics, (InstArrayCopy*)value);
        case Inst::InstArraySlice:
            return validateArraySlice(diagnostics, (InstArraySlice*)value);
        case Inst::InstStringLength:
            return validateStringLength(diagnostics, (InstStringLength*)value);
        case Inst::InstStringData:
            return validateStringData(diagnostics, (InstStringData*)value);
        case Inst::InstCall:
            return validateCall(diagnostics, (InstCall*)value);
        case Inst::InstCallDyn:
            return validateCallDyn(diagnostics, (InstCallDyn*)value);
        case Inst::InstCallForeign:
            return validateCallForeign(diagnostics, (InstCallForeign*)value);
        case Inst::InstJe:
            return validateJe(diagnostics, (InstJe*)value);
        case Inst::InstJmp:
            return validateJmp(diagnostics, (InstJmp*)value);
        case Inst::InstRet:
            return validateRet(diagnostics, (InstRet*)value);
        case Inst::InstPhi:
            return validatePhi(diagnostics, (InstPhi*)value);
    }

    return false;
}

bool validateBlock(Diagnostics* diagnostics, Block* block) {
    if(!block->complete) {
        diagnostics->error("block is incomplete"_buffer, &block->function->source, noSource);
        return false;
    }

    for(Value* inst: block->instructions) {
        if(!validateValue(diagnostics, inst)) return false;
    }

    // TODO: Validate control flow trackers.
    // TODO: Validate control flow flags.
    return true;
}

bool validateFunction(Diagnostics* diagnostics, Function* function) {
    if(function->ast || function->resolving) {
        diagnostics->error("function is not fully resolved"_buffer, &function->source, noSource);
        return false;
    }

    for(Arg* arg: function->args) {
        if(!validateArg(diagnostics, arg)) return false;
    }

    if(!validateType(diagnostics, function->returnType, &function->source, function)) return false;

    for(Block* block: function->blocks) {
        if(!validateBlock(diagnostics, block)) return false;
    }

    // TODO: Validate return point types.
    // TODO: Validate generic environment.
    return true;
}

bool validateModule(Diagnostics* diagnostics, Module* module) {
    for(auto type: module->types) {
        if(!validateType(diagnostics, type, nullptr, nullptr)) return false;
    }

    for(Function& function: module->functions) {
        if(!validateFunction(diagnostics, &function)) return false;
    }

    // TODO: Everything else.
    return true;
}

static bool validateGenType(Diagnostics* diagnostics, GenType* type, Node* source, Function* fun) {
    auto env = type->env;
    if(env->kind != GenEnv::Function || env->container != fun) {
        diagnostics->error("generic type is used outside of its environment"_buffer, source, noSource);
        return false;
    }

    if(type->index >= env->typeCount) {
        diagnostics->error("generic type out of bounds in its environment"_buffer, source, noSource);
        return false;
    }

    if(env->types[type->index] != type) {
        diagnostics->error("generic environment type list is out of sync"_buffer, source, noSource);
        return false;
    }

    auto argCount = type->argCount.from(0);
    if(argCount > 0) {
        for(U32 i = 0; i < argCount; i++) {
            if(!validateType(diagnostics, type->args[i], source, fun)) return false;
        }
    }

    return true;
}

static bool validateIntType(Diagnostics* diagnostics, IntType* type, Node* source, Function* fun) {
    if(type->width >= IntType::KindCount) {
        diagnostics->error("invalid register width '%@' for integer"_buffer, source, noSource, (int)type->width);
        return false;
    }

    if(type->bits > IntType::bitsForWidth(type->width)) {
        diagnostics->error("integer type is too large for its register width"_buffer, source, noSource);
        return false;
    }

    if(type->bits < 1) {
        diagnostics->error("integer type is too small; it must contain at least 1 bit"_buffer, source, noSource);
        return false;
    }

    return true;
}

static bool validateFloatType(Diagnostics* diagnostics, FloatType* type, Node* source, Function* fun) {
    if(type->width >= FloatType::KindCount) {
        diagnostics->error("invalid register width '%@' for floating point"_buffer, source, noSource, (int)type->width);
        return false;
    }

    if(type->bits != FloatType::bitsForWidth(type->width)) {
        diagnostics->error("floating point size must be the same as its register width"_buffer, source, noSource);
        return false;
    }

    return true;
}

static bool validateRefType(Diagnostics* diagnostics, RefType* type, Node* source, Function* fun) {
    return validateType(diagnostics, type->to, source, fun);
}

static bool validateFunType(Diagnostics* diagnostics, FunType* type, Node* source, Function* fun) {
    for(U32 i = 0; i < type->argCount; i++) {
        auto arg = type->args + i;

        if(arg->index != i) {
            diagnostics->error("function argument list is out of sync"_buffer, source, noSource);
            return false;
        }

        if(!validateType(diagnostics, arg->type, source, fun)) return false;
    }

    return validateType(diagnostics, type->result, source, fun);
}

static bool validateArrayType(Diagnostics* diagnostics, ArrayType* type, Node* source, Function* fun) {
    return validateType(diagnostics, type->content, source, fun);
}

static bool validateMapType(Diagnostics* diagnostics, MapType* type, Node* source, Function* fun) {
    if(!validateType(diagnostics, type->from, source, fun)) return false;
    return validateType(diagnostics, type->to, source, fun);
}

static bool validateTupType(Diagnostics* diagnostics, TupType* type, Node* source, Function* fun) {
    for(U32 i = 0; i < type->count; i++) {
        auto field = type->fields + i;

        if(field->container != type || field->index != i) {
            diagnostics->error("tuple field list is out of sync"_buffer, source, noSource);
            return false;
        }

        if((field->name != 0) != type->named) {
            diagnostics->error("tuple field naming mode is different from the container naming mode"_buffer, source, noSource);
            return false;
        }

        if(!validateType(diagnostics, field->type, source, fun)) return false;
    }

    return true;
}

static bool validateRecordType(Diagnostics* diagnostics, RecordType* type, Node* source, Function* fun) {
    if(type->ast) {
        diagnostics->error("record type is not fully resolved"_buffer, source, noSource);
        return false;
    }

    if(type->argCount) {
        diagnostics->error("record type is incomplete due to having remaining type arguments"_buffer, source, noSource);
        return false;
    }

    if(type->kind == RecordType::Enum) {
        for(U32 i = 0; i < type->conCount; i++) {
            if(type->cons[i].content) {
                diagnostics->error("record type is marked as Enum kind, but constructor %@ contains data"_buffer, source, noSource, i);
                return false;
            }
        }
    } else if(type->kind == RecordType::Single) {
        if(type->conCount != 1) {
            diagnostics->error("record type is marked as Single kind, but number of constructors is %@"_buffer, source, noSource, type->conCount);
            return false;
        }
    } else if(type->kind == RecordType::Multi) {
        if(type->conCount >= 1) {
            diagnostics->error("record type is marked as Multi kind, but number of constructors is %@"_buffer, source, noSource, type->conCount);
            return false;
        }
    } else {
        diagnostics->error("record type has unrecognized kind '%@'"_buffer, source, noSource, (int)type->kind);
        return false;
    }

    for(U32 i = 0; i < type->conCount; i++) {
        auto con = type->cons + i;

        if(con->parent != type || con->index != i) {
            diagnostics->error("record constructor list is out of sync"_buffer, source, noSource);
            return false;
        }

        if(!validateType(diagnostics, con->content, source, fun)) return false;
    }

    return true;
}

static bool validateAliasType(Diagnostics* diagnostics, AliasType* type, Node* source, Function* fun) {
    if(type->ast) {
        diagnostics->error("alias type is not fully resolved"_buffer, source, noSource);
        return false;
    }

    if(type->argCount) {
        diagnostics->error("alias type is incomplete due to having remaining type arguments"_buffer, source, noSource);
        return false;
    }

    return validateType(diagnostics, type->to, source, fun);
}

bool validateType(Diagnostics* diagnostics, Type* type, Node* source, Function* fun) {
    switch(type->kind) {
        case Type::Error:
            diagnostics->error("error types can never be used in valid code"_buffer, source, noSource);
            return false;
        case Type::Unit:
            // Unit types don't contain any type-specific data.
            return true;
        case Type::Gen:
            return validateGenType(diagnostics, (GenType*)type, source, fun);
        case Type::Int:
            return validateIntType(diagnostics, (IntType*)type, source, fun);
        case Type::Float:
            return validateFloatType(diagnostics, (FloatType*)type, source, fun);
        case Type::String:
            // String types don't contain any type-specific data.
            return true;
        case Type::Ref:
            return validateRefType(diagnostics, (RefType*)type, source, fun);
        case Type::Fun:
            return validateFunType(diagnostics, (FunType*)type, source, fun);
        case Type::Array:
            return validateArrayType(diagnostics, (ArrayType*)type, source, fun);
        case Type::Map:
            return validateMapType(diagnostics, (MapType*)type, source, fun);
        case Type::Tup:
            return validateTupType(diagnostics, (TupType*)type, source, fun);
        case Type::Record:
            return validateRecordType(diagnostics, (RecordType*)type, source, fun);
        case Type::Alias:
            return validateAliasType(diagnostics, (AliasType*)type, source, fun);
    }

    return true;
}