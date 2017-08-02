#include "print.h"

void printValue(std::ostream& stream, Context& context, const Value* value) {
    if(value->kind == Value::Arg || value->kind >= Value::FirstInst) {
        stream << '%';

        auto name = context.find(value->name);
        if(name.length > 0) {
            stream.write(name.name, name.length);
        } else if(value->kind >= Value::FirstInst) {
            auto block = ((Inst*)value)->block;
            auto blockIndex = block - block->function->blocks.pointer();
            Size index = 0;
            for(Size i = 0; i < block->instructions.size(); i++) {
                if(block->instructions[i] == value) break;
                index++;
            }

            stream << blockIndex;
            stream << '.';
            stream << index;
        } else {
            stream << "<unnamed>";
        }
    } else if(value->kind == Value::ConstInt) {
        stream << "i ";
        stream << ((ConstInt*)value)->value;
    } else if(value->kind == Value::ConstFloat) {
        stream << "f ";
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
            stream << '&';
            printType(stream, context, ((RefType*)type)->to);
            break;
        case Type::Ptr:
            stream << '*';
            printType(stream, context, ((PtrType*)type)->to);
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
            if(name.length) {
                stream.write(name.name, name.length);
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
    }
}

void printBlockName(std::ostream& stream, const Block* block) {
    auto index = block - block->function->blocks.pointer();
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
    if(name.length) {
        stream.write(name.name, name.length);
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
    stream << "):\n";

    for(auto& block : fun->blocks) {
        printBlock(stream, context, &block);
    }
}

void printInst(std::ostream& stream, Context& context, const Inst* inst) {
    stream << "  ";

    const char* name = "";
    switch(inst->kind) {
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

    printValue(stream, context, inst);
    stream << " = ";
    stream << name;

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
    }

    stream << " : ";
    printType(stream, context, inst->type);
    stream << '\n';
}