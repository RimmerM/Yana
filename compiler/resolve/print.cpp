#include "print.h"

void printValue(Net::Writer& stream, Context& context, Module& module, const Value* value) {
    if(value->kind == Value::Arg || value->kind == Value::Global || value->kind >= Value::FirstInst) {
        stream.writeByte('%');

        auto name = context.find(value->name);
        if(name.textLength > 0) {
            stream.writeBytes((const Byte*)name.text, name.textLength);
        } else if(value->kind >= Value::FirstInst) {
            stream << value->id;
        } else if(value->kind == Value::Arg) {
            stream.writeByte('a');
            stream << ((Arg*)value)->index;
        } else {
            stream.writeString("<unnamed>"_v);
        }
    } else if(value->kind == Value::ConstInt) {
        stream << ((ConstInt*)value)->value;
        stream.writeString(": "_v);
        printType(stream, context, module, module.global[value->type]);
    } else if(value->kind == Value::ConstFloat) {
        stream << ((ConstFloat*)value)->value;
        stream.writeString(": ");
        printType(stream, context, module, module.global[value->type]);
    } else if(value->kind == Value::ConstString) {
        auto c = ((ConstString*)value);
        auto string = context.findName(c->value);

        stream.writeByte('"');
        stream.writeString(string);
        stream.writeByte('"');
    }
}

void printType(Net::Writer& stream, Context& context, Module& module, const Type* type) {
    switch(type->kind) {
        case Type::Unit:
            stream.writeString("void"_v);
            break;
        case Type::Error:
            stream.writeString("<type error>"_v);
            break;
        case Type::Int:
            stream.writeByte('i');
            stream << ((IntType*)type)->bits;
            break;
        case Type::Float:
            stream.writeByte('f');
            stream << ((FloatType*)type)->bits;
            break;
        case Type::String:
            stream.writeString("String"_v);
            break;
        case Type::Ptr:
            stream.writeByte('*');
            printType(stream, context, module, module.global[((PtrType*)type)->to]);
            break;
        case Type::Array:
            stream.writeByte('[');
            printType(stream, context, module, module.global[((ArrayType*)type)->content]);
            stream.writeByte(']');
            break;
        case Type::Map:
            stream.writeByte('[');
            printType(stream, context, module, module.global[((MapType*)type)->from]);
            stream.writeString(" -> "_v);
            printType(stream, context, module, module.global[((MapType*)type)->to]);
            stream.writeByte(']');
            break;
        case Type::Record: {
            auto record = (RecordType*)type;
            auto name = context.find(record->name);
            if(name.textLength > 0) {
                stream.writeBytes((const Byte*)name.text, name.textLength);
            }

            break;
        }
        case Type::Tup: {
            auto tup = (TupType*)type;
            stream.writeByte('{');
            for(Size i = 0; i < tup->count; i++) {
                printType(stream, context, tup->fields[i].type);
                if(i < tup->count - 1) {
                    stream.writeString(", "_v);
                }
            }
            stream.writeByte('}');
            break;
        }
        case Type::Fun: {
            auto fun = (FunType*)type;
            stream.writeByte('(');
            for(Size i = 0; i < fun->argCount; i++) {
                printType(stream, context, fun->args[i].type);
                if(i < fun->argCount - 1) {
                    stream.writeString(", "_v);
                }
            }
            stream.writeString(") -> "_v);
            printType(stream, context, module, module.global[fun->result]);
            break;
        }
        case Type::Alias: {
            printType(stream, context, module, module.global[((AliasType*)type)->to]);
            break;
        }
    }
}

void printBlockName(Net::Writer& stream, Module& module, Block& block) {
    Size index = 0;
    for(Size i = 0; i < block->function->blocks.size(); i++) {
        if(block->function->blocks[i] == block) break;
        index++;
    }

    stream.writeByte('#');
    stream << index;
}

void printBlock(Net::Writer& stream, Context& context, Module& module, Block& block) {
    printBlockName(stream, module, block);
    stream.writeString(":\n"_v);

    for(auto inst: block.phis.contents(module.local)) {
        printInst(stream, context, module, module.local[inst]);
    }

    for(auto inst: block.instructions.contents(module.local)) {
        printInst(stream, context, module, module.local[inst]);
    }

    if(block.terminator) {
        printInst(stream, context, module, module.local[block.terminator]);
    }
}

void printGlobal(Net::Writer& stream, Context& context, Module& module, const Global* global) {
    stream.writeString("global "_v);
    auto name = context.find(global->name);
    if(name.textLength > 0) {
        stream.writeByte('%');
        stream.writeBytes((const Byte*)name.text, name.textLength);
    } else {
        stream.writeString("<unnamed>"_v);
    }

    stream.writeString(": "_v);
    printType(stream, context, module, module.global[global->type]);
    stream.writeByte('\n');
}

void printModule(Net::Writer& stream, Context& context, Module& module) {
    for(auto& global: module.globals) {
        printGlobal(stream, context, module, &global);
    }

    stream.writeByte('\n');

    for(auto& fun: module.functions) {
        printFunction(stream, context, module, &fun);
        stream.writeByte('\n');
    }
}

void printFunction(Net::Writer& stream, Context& context, Module& module, const Function* fun, StringId forceName) {
    stream.writeString("fn "_v);
    auto name = context.find(forceName ? forceName : fun->name);
    if(name.textLength > 0) {
        stream.writeBytes((const Byte*)name.text, name.textLength);
    } else {
        stream.writeString("<unnamed>"_v);
    }

    stream.writeByte('(');
    for(Size i = 0; i < fun->args.size(); i++) {
        printValue(stream, context, fun->args[i]);
        stream << ": ";
        printType(stream, context, fun->args[i]->type);
        if(i < fun->args.size() - 1) {
            stream << ", ";
        }
    }
    stream.writeString(") -> "_v);
    printType(stream, context, module, module.global[fun->returnType]);
    stream.writeString(" {\n"_v);

    for(auto& block : fun->blocks) {
        printBlock(stream, context, block);
    }

    stream.writeString("}\n"_v);
}

void printInst(Net::Writer& stream, Context& context, Module& module, const Inst* inst) {
    stream.writeString("  "_v);

    StringView name;
    switch(inst->kind) {
        case Inst::InstNop:
            name = "nop"_v;
            break;
        case Inst::InstTrunc:
            name = "trunc"_v;
            break;
        case Inst::InstFTrunc:
            name = "truncf"_v;
            break;
        case Inst::InstZExt:
            name = "zext"_v;
            break;
        case Inst::InstSExt:
            name = "sext"_v;
            break;
        case Inst::InstFExt:
            name = "fext"_v;
            break;
        case Inst::InstFToI:
            name = "ftoi"_v;
            break;
        case Inst::InstFToUI:
            name = "ftoui"_v;
            break;
        case Inst::InstIToF:
            name = "itof"_v;
            break;
        case Inst::InstUIToF:
            name = "uitof"_v;
            break;
        case Inst::InstAdd:
            name = "add"_v;
            break;
        case Inst::InstSub:
            name = "sub"_v;
            break;
        case Inst::InstMul:
            name = "mul"_v;
            break;
        case Inst::InstDiv:
            name = "div"_v;
            break;
        case Inst::InstIDiv:
            name = "idiv"_v;
            break;
        case Inst::InstRem:
            name = "rem"_v;
            break;
        case Inst::InstIRem:
            name = "irem"_v;
            break;
        case Inst::InstFAdd:
            name = "fadd"_v;
            break;
        case Inst::InstFSub:
            name = "fsub"_v;
            break;
        case Inst::InstFMul:
            name = "fmul"_v;
            break;
        case Inst::InstFDiv:
            name = "fdiv"_v;
            break;
        case Inst::InstICmp:
            name = "icmp"_v;
            break;
        case Inst::InstFCmp:
            name = "fcmp"_v;
            break;
        case Inst::InstShl:
            name = "shl"_v;
            break;
        case Inst::InstShr:
            name = "shr"_v;
            break;
        case Inst::InstSar:
            name = "sar"_v;
            break;
        case Inst::InstAnd:
            name = "and"_v;
            break;
        case Inst::InstOr:
            name = "or"_v;
            break;
        case Inst::InstXor:
            name = "xor"_v;
            break;
        case Inst::InstAddPtr:
            name = "addptr"_v;
            break;
        case Inst::InstJe:
            name = "je"_v;
            break;
        case Inst::InstRecord:
            name = "record"_v;
            break;
        case Inst::InstTup:
            name = "tup"_v;
            break;
        case Inst::InstFun:
            name = "fun"_v;
            break;
        case Inst::InstAlloc:
            name = "alloc"_v;
            break;
        case Inst::InstAllocArray:
            name = "allocarray"_v;
            break;
        case Inst::InstLoad:
            name = "load"_v;
            break;
        case Inst::InstLoadField:
            name = "loadfield"_v;
            break;
        case Inst::InstLoadArray:
            name = "loadarray"_v;
            break;
        case Inst::InstStore:
            name = "store"_v;
            break;
        case Inst::InstStoreField:
            name = "storefield"_v;
            break;
        case Inst::InstStoreArray:
            name = "storearray"_v;
            break;
        case Inst::InstGetField:
            name = "getfield"_v;
            break;
        case Inst::InstUpdateField:
            name = "updatefield"_v;
            break;
        case Inst::InstArrayLength:
            name = "arraylength"_v;
            break;
        case Inst::InstArrayCopy:
            name = "arraycopy"_v;
            break;
        case Inst::InstArraySlice:
            name = "arrayslice"_v;
            break;
        case Inst::InstStringLength:
            name = "stringlength"_v;
            break;
        case Inst::InstStringData:
            name = "stringdata"_v;
            break;
        case Inst::InstCall:
            name = "call"_v;
            break;
        case Inst::InstCallDyn:
            name = "call dyn"_v;
            break;
        case Inst::InstCallForeign:
            name = "call foreign"_v;
            break;
        case Inst::InstJmp:
            name = "jmp"_v;
            break;
        case Inst::InstRet:
            name = "ret"_v;
            break;
        case Inst::InstPhi:
            name = "phi"_v;
            break;
    }

    if(module.global[inst->type]->kind != Type::Unit && inst->kind != Inst::InstRet) {
        printValue(stream, context, inst);
        stream.writeString(" = "_v);
    }

    stream.writeString(name);

    if(inst->kind == Inst::InstAlloc) {
        if(((InstAlloc*)inst)->mut) stream.writeString("<mut>"_v);
    }

    stream.writeByte('(');

    if(inst->kind == Inst::InstCall) {
        auto fun = context.find(((InstCall*)inst)->fun->name);
        if(fun.textLength > 0) {
            stream.writeBytes((const Byte*)fun.text, fun.textLength);
        } else {
            stream.writeString("<unnamed>"_v);
        }

        if(inst->usedCount > 0) {
            stream << ", ";
        }
    } else if(inst->kind == Inst::InstCallForeign) {
        auto fun = context.find(((InstCallForeign*)inst)->fun->name);
        if(fun.textLength > 0) {
            stream.write(fun.text, fun.textLength);
        } else {
            stream << "<unnamed>";
        }

        if(inst->usedCount > 0) {
            stream << ", ";
        }
    } else if(inst->kind == Inst::InstCallDyn) {
        if(((InstCallDyn*)inst)->isIntrinsic) {
            stream << "<intrinsic> ";
        }
    } else if(inst->kind == Inst::InstRecord) {
        auto record = (InstRecord*)inst;
        auto con = context.findName(record->con->name);
        stream.write(con.text(), con.size());

        if(inst->usedCount > 0) {
            stream << ", ";
        }
    }

    if(inst->kind == Inst::InstPhi) {
        auto phi = (InstPhi*)inst;
        for(U32 i = 0; i < phi->altCount; i++) {
            stream << '[';
            printValue(stream, context, phi->alts[i].value);
            stream << ", ";
            printBlockName(stream, phi->alts[i].fromBlock);
            stream << ']';

            if(i < phi->altCount - 1) {
                stream << ", ";
            }
        }
    } else if(inst->kind == Inst::InstJe) {
        auto je = (InstJe*)inst;
        stream << '[';
        printValue(stream, context, je->cond);
        stream << ", ";
        printBlockName(stream, je->then);
        stream << ", ";
        printBlockName(stream, je->otherwise);
        stream << ']';
    } else {
        for(U32 i = 0; i < inst->usedCount; i++) {
            printValue(stream, context, inst->usedValues[i]);
            if(i < inst->usedCount - 1) {
                stream << ", ";
            }
        }
    }

    if(inst->kind == Inst::InstJmp) {
        printBlockName(stream, ((const InstJmp*)inst)->to);
    } else if(inst->kind == Inst::InstGetField) {
        auto get = (InstGetField*)inst;
        for(Size i = 0; i < get->chainLength; i++) {
            stream << ", ";
            stream << get->indexChain[i];
        }
    } else if(inst->kind == Inst::InstLoadField) {
        auto get = (InstLoadField*)inst;
        for(Size i = 0; i < get->chainLength; i++) {
            stream << ", ";
            stream << get->indexChain[i];
        }
    }

    stream.writeString("): "_v);
    printType(stream, context, module.global[inst->type]);
    stream.writeByte('\n');
}