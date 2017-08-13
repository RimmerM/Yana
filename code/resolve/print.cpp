#include "print.h"

void printValue(std::ostream& stream, Context& context, const Value* value) {
    if(value->kind == Value::Arg || value->kind == Value::Global || value->kind >= Value::FirstInst) {
        stream << '%';

        auto name = context.find(value->name);
        if(name.textLength > 0) {
            stream.write(name.text, name.textLength);
        } else if(value->kind >= Value::FirstInst) {
            stream << value->id;
        } else if(value->kind == Value::Arg) {
            stream << 'a';
            stream << ((Arg*)value)->index;
        } else {
            stream << "<unnamed>";
        }
    } else if(value->kind == Value::ConstInt) {
        stream << ((ConstInt*)value)->value;
    } else if(value->kind == Value::ConstFloat) {
        stream << ((ConstFloat*)value)->value;
    } else if(value->kind == Value::ConstString) {
        auto string = ((ConstString*)value);
        stream << '"';
        stream.write(string->value, string->length);
        stream << '"';
    }
}

void printType(std::ostream& stream, Context& context, const Type* type) {
    switch(type->kind) {
        case Type::Unit:
            stream << "void";
            break;
        case Type::Error:
            stream << "<type error>";
            break;
        case Type::Int:
            stream << 'i';
            stream << ((IntType*)type)->bits;
            break;
        case Type::Float:
            stream << 'f';
            stream << ((FloatType*)type)->bits;
            break;
        case Type::String:
            stream << 's';
            break;
        case Type::Ref:
            stream << '*';
            printType(stream, context, ((RefType*)type)->to);
            break;
        case Type::Array:
            stream << '[';
            printType(stream, context, ((ArrayType*)type)->content);
            stream << ']';
            break;
        case Type::Map:
            stream << '[';
            printType(stream, context, ((MapType*)type)->from);
            stream << " -> ";
            printType(stream, context, ((MapType*)type)->to);
            stream << ']';
            break;
        case Type::Record: {
            auto name = context.find(((RecordType*)type)->name);
            if(name.textLength > 0) {
                stream.write(name.text, name.textLength);
            }
            break;
        }
        case Type::Tup:
            stream << "<tup>";
            break;
        case Type::Gen:
            stream << "gen ";
            stream << ((GenType*)type)->index;
            break;
        case Type::Fun: {
            auto fun = (FunType*)type;
            stream << '(';
            for(Size i = 0; i < fun->argCount; i++) {
                printType(stream, context, fun->args[i].type);
                if(i < fun->argCount - 1) {
                    stream << ", ";
                }
            }
            stream << ") -> ";
            printType(stream, context, fun->result);
            break;
        }
        case Type::Alias: {
            printType(stream, context, ((AliasType*)type)->to);
            break;
        }
    }
}

void printBlockName(std::ostream& stream, const Block* block) {
    Size index = 0;
    for(Size i = 0; i < block->function->blocks.size(); i++) {
        if(block->function->blocks[i] == block) break;
        index++;
    }

    stream << '#';
    stream << index;
}

void printBlock(std::ostream& stream, Context& context, const Block* block) {
    printBlockName(stream, block);
    stream << ":\n";

    for(auto inst: block->instructions) {
        printInst(stream, context, inst);
    }
}

void printModule(std::ostream& stream, Context& context, const Module* module) {
    for(auto& fun: module->functions) {
        printFunction(stream, context, &fun);
        stream << '\n';
    }
}

void printFunction(std::ostream& stream, Context& context, const Function* fun) {
    stream << "fn ";
    auto name = context.find(fun->name);
    if(name.textLength > 0) {
        stream.write(name.text, name.textLength);
    } else {
        stream << "<unnamed>";
    }

    stream << '(';
    for(Size i = 0; i < fun->args.size(); i++) {
        printValue(stream, context, &fun->args[i]);
        stream << " : ";
        printType(stream, context, fun->args[i].type);
        if(i < fun->args.size() - 1) {
            stream << ", ";
        }
    }
    stream << ") -> ";
    printType(stream, context, fun->returnType);
    stream << ":\n";

    for(auto& block : fun->blocks) {
        printBlock(stream, context, block);
    }
}

void printInst(std::ostream& stream, Context& context, const Inst* inst) {
    stream << "  ";

    const char* name = "";
    switch(inst->kind) {
        case Inst::InstNop:
            name = "nop";
            break;
        case Inst::InstTrunc:
            name = "trunc";
            break;
        case Inst::InstFTrunc:
            name = "truncf";
            break;
        case Inst::InstZExt:
            name = "zext";
            break;
        case Inst::InstSExt:
            name = "sext";
            break;
        case Inst::InstFExt:
            name = "fext";
            break;
        case Inst::InstFToI:
            name = "ftoi";
            break;
        case Inst::InstFToUI:
            name = "ftoui";
            break;
        case Inst::InstIToF:
            name = "itof";
            break;
        case Inst::InstUIToF:
            name = "uitof";
            break;
        case Inst::InstAdd:
            name = "add";
            break;
        case Inst::InstSub:
            name = "sub";
            break;
        case Inst::InstMul:
            name = "mul";
            break;
        case Inst::InstDiv:
            name = "div";
            break;
        case Inst::InstIDiv:
            name = "idiv";
            break;
        case Inst::InstRem:
            name = "rem";
            break;
        case Inst::InstIRem:
            name = "irem";
            break;
        case Inst::InstFAdd:
            name = "fadd";
            break;
        case Inst::InstFSub:
            name = "fsub";
            break;
        case Inst::InstFMul:
            name = "fmul";
            break;
        case Inst::InstFDiv:
            name = "fdiv";
            break;
        case Inst::InstICmp:
            name = "icmp";
            break;
        case Inst::InstFCmp:
            name = "fcmp";
            break;
        case Inst::InstShl:
            name = "shl";
            break;
        case Inst::InstShr:
            name = "shr";
            break;
        case Inst::InstSar:
            name = "sar";
            break;
        case Inst::InstAnd:
            name = "and";
            break;
        case Inst::InstOr:
            name = "or";
            break;
        case Inst::InstXor:
            name = "xor";
            break;
        case Inst::InstJe:
            name = "je";
            break;
        case Inst::InstRecord:
            name = "record";
            break;
        case Inst::InstTup:
            name = "tup";
            break;
        case Inst::InstFun:
            name = "fun";
            break;
        case Inst::InstAlloc:
            name = "alloc";
            break;
        case Inst::InstAllocArray:
            name = "allocarray";
            break;
        case Inst::InstLoad:
            name = "load";
            break;
        case Inst::InstLoadField:
            name = "loadfield";
            break;
        case Inst::InstLoadGlobal:
            name = "loadglobal";
            break;
        case Inst::InstLoadArray:
            name = "loadarray";
            break;
        case Inst::InstStore:
            name = "store";
            break;
        case Inst::InstStoreField:
            name = "storefield";
            break;
        case Inst::InstStoreGlobal:
            name = "storeglobal";
            break;
        case Inst::InstStoreArray:
            name =  "storearray";
            break;
        case Inst::InstGetField:
            name = "getfield";
            break;
        case Inst::InstUpdateField:
            name = "updatefield";
            break;
        case Inst::InstArrayLength:
            name = "arraylength";
            break;
        case Inst::InstArrayCopy:
            name = "arraycopy";
            break;
        case Inst::InstArraySlice:
            name = "arrayslice";
            break;
        case Inst::InstCall:
            name = "call";
            break;
        case Inst::InstCallGen:
            name = "call gen";
            break;
        case Inst::InstCallDyn:
            name = "call dyn";
            break;
        case Inst::InstCallDynGen:
            name = "call dyn gen";
            break;
        case Inst::InstCallForeign:
            name = "call foreign";
            break;
        case Inst::InstJmp:
            name = "jmp";
            break;
        case Inst::InstRet:
            name = "ret";
            break;
        case Inst::InstPhi:
            name = "phi";
            break;
    }

    if(inst->type->kind != Type::Unit && inst->kind != Inst::InstRet) {
        printValue(stream, context, inst);
        stream << " = ";
    }

    stream << name;
    stream << ' ';

    switch(inst->kind) {
        case Inst::InstAlloc:
            if(((InstAlloc*)inst)->mut) stream << "<mut>";
            break;
        case Inst::InstCall:
        case Inst::InstCallGen: {
            auto fun = context.find(((InstCall*)inst)->fun->name);
            if(fun.textLength > 0) {
                stream.write(fun.text, fun.textLength);
            } else {
                stream << "<unnamed>";
            }

            if(inst->usedCount > 0) {
                stream << ", ";
            }
            break;
        }
        case Inst::InstCallForeign: {
            auto fun = context.find(((InstCallForeign*)inst)->fun->name);
            if(fun.textLength > 0) {
                stream.write(fun.text, fun.textLength);
            } else {
                stream << "<unnamed>";
            }

            if(inst->usedCount > 0) {
                stream << ", ";
            }
            break;
        }
    }

    for(Size i = 0; i < inst->usedCount; i++) {
        printValue(stream, context, inst->usedValues[i]);
        if(i < inst->usedCount - 1) {
            stream << ", ";
        }
    }

    switch(inst->kind) {
        case Inst::InstJe:
            stream << ", ";
            printBlockName(stream, ((const InstJe*)inst)->then);
            stream << ", ";
            printBlockName(stream, ((const InstJe*)inst)->otherwise);
            break;
        case Inst::InstJmp:
            printBlockName(stream, ((const InstJmp*)inst)->to);
            break;
        case Inst::InstGetField: {
            auto get = (InstGetField*)inst;
            for(Size i = 0; i < get->chainLength; i++) {
                stream << ", ";
                stream << get->indexChain[i];
            }
            break;
        }
        case Inst::InstLoadField: {
            auto get = (InstLoadField*)inst;
            for(Size i = 0; i < get->chainLength; i++) {
                stream << ", ";
                stream << get->indexChain[i];
            }
            break;
        }
    }

    stream << " : ";
    printType(stream, context, inst->type);
    stream << '\n';
}