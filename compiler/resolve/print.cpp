#include "print.h"

struct ResolvePrint {
    Net::Writer& writer;
    Context& context;
    Program& program;
    GlobalBase global;
    ModuleBase local;
};

static void writeUInt(Net::Writer& writer, U64 value) {
    writer.writeBytes(64, [&](Byte* buffer) {
        return show(value, (char*)buffer, 64);
    });
}

static void writeFloat(Net::Writer& writer, F64 value) {
    writer.writeBytes(64, [&](Byte* buffer) {
        return show(value, (char*)buffer, 64);
    });
}

static void printType(ResolvePrint& print, TypePtr pointer) {
    auto text = describeType(print.context, print.global, pointer);
    print.writer.writeString(stringView(text));
}

static void printPlace(ResolvePrint& print, Function& function, const Place& place) {
    auto known = place.local < function.localCount();
    auto root = known ? function.localAt(print.local, place.local) : Local {};
    print.writer.writeByte('%');

    if(root.name) {
        print.writer.writeString(print.context.findName(root.name));
    } else {
        print.writer.writeString("local"_v);
        writeUInt(print.writer, place.local);
    }

    auto type = known ? root.type : print.program.scalar.error;
    auto projections = place.projections;

    for(auto projection: projections.contents(print.local)) {
        if(projection.kind == ProjectionKind::Discriminant) {
            print.writer.writeString(".discriminant"_v);
            type = print.program.scalar.int_;
        } else if(projection.kind == ProjectionKind::Downcast) {
            auto record = (RecordType*)print.global[type];
            print.writer.writeByte('@');
            print.writer.writeString(print.context.findName(record->constructors.get(print.global, projection.index).name));
            type = record->constructors.get(print.global, projection.index).content;
        } else if(projection.kind == ProjectionKind::Field) {
            auto tuple = (TupType*)print.global[type];
            auto field = tuple->fields.get(print.global, projection.index);
            print.writer.writeByte('.');

            if(field.name) print.writer.writeString(print.context.findName(field.name));
            else writeUInt(print.writer, projection.index);
            type = field.type;
        } else {
            print.writer.writeString(projection.kind == ProjectionKind::Deref ? ".*"_v : "[...]"_v);
        }
    }
}

static void printValue(ResolvePrint& print, Value& value) {
    if(value.name) {
        print.writer.writeByte('%');
        print.writer.writeString(print.context.findName(value.name));
        return;
    }

    switch(value.kind) {
        case Value::ConstInt: {
            // A payload-free record is represented by its constructor index, so printing the
            // index back as the constructor's name is what keeps `True` readable as `True`.
            auto constant = ((ConstInt&)value).value;
            auto type = print.global[value.type];

            if(type->kind == Type::Record && constant < ((RecordType*)type)->constructors.size()) {
                auto constructor = ((RecordType*)type)->constructors.get(print.global, Size(constant));
                print.writer.writeString(print.context.findName(constructor.name));
            } else {
                writeUInt(print.writer, constant);
            }

            return;
        }
        case Value::ConstFloat:
            writeFloat(print.writer, ((ConstFloat&)value).value);
            return;
        case Value::ConstDouble:
            writeFloat(print.writer, ((ConstDouble&)value).value);
            return;
        default:
            print.writer.writeString("%v"_v);
            writeUInt(print.writer, value.id);
            return;
    }
}

static StringView instructionName(Value& value, GlobalBase global) {
    switch(value.kind) {
        case Value::Alloc: return "alloc"_v;
        case Value::LoadPlace: return "load"_v;
        case Value::Init: return "init"_v;
        case Value::Cast: return "cast"_v;
        case Value::Neg: return "neg"_v;
        case Value::Not: return "not"_v;
        case Value::Add: return "add"_v;
        case Value::Sub: return "sub"_v;
        case Value::Mul: return "mul"_v;
        case Value::Div: return "div"_v;
        case Value::Rem: return "rem"_v;
        case Value::Shl: return "shl"_v;
        case Value::Shr: return "shr"_v;
        case Value::Sar: return "sar"_v;
        case Value::And: return "and"_v;
        case Value::Or: return "or"_v;
        case Value::Xor: return "xor"_v;
        case Value::Cmp: {
            switch(((InstCmp&)value).cmp) {
                case CompareOp::Eq: return "cmp_eq"_v;
                case CompareOp::Ne: return "cmp_ne"_v;
                case CompareOp::Gt: return "cmp_gt"_v;
                case CompareOp::Ge: return "cmp_ge"_v;
                case CompareOp::Lt: return "cmp_lt"_v;
                case CompareOp::Le: return "cmp_le"_v;
            }
            break;
        }
        case Value::Call: return "call"_v;
        case Value::Je: return "je"_v;
        case Value::Jmp: return "jmp"_v;
        case Value::Ret: return "ret"_v;
        case Value::Phi: return "phi"_v;
        default: break;
    }
    return "<invalid>"_v;
}

static void printBlockRef(ResolvePrint& print, Block& block) {
    print.writer.writeString("b"_v);
    writeUInt(print.writer, block.index);
}

static void printInstruction(ResolvePrint& print, Inst& inst) {
    print.writer.writeString("    "_v);

    auto produces = !isTerminator(inst) && !isUnit(print.global, inst.type);
    if(produces) {
        printValue(print, inst);
        print.writer.writeString(" = "_v);
    }

    print.writer.writeString(instructionName(inst, print.global));
    auto function = print.local[print.local[inst.block]->function];

    switch(inst.kind) {
        case Value::Alloc:
            break;
        case Value::LoadPlace:
            print.writer.writeByte(' ');
            printPlace(print, *function, ((InstLoadPlace&)inst).place);
            break;
        case Value::Init: {
            auto& init = (InstInit&)inst;
            print.writer.writeByte(' ');
            printPlace(print, *function, init.place);
            print.writer.writeString(", "_v);
            printValue(print, *print.local[init.value]);
            break;
        }
        case Value::Cast:
        case Value::Neg:
        case Value::Not: {
            print.writer.writeByte(' ');
            printValue(print, *print.local[((InstUnary&)inst).from]);
            break;
        }
        case Value::Add:
        case Value::Sub:
        case Value::Mul:
        case Value::Div:
        case Value::Rem:
        case Value::Shl:
        case Value::Shr:
        case Value::Sar:
        case Value::And:
        case Value::Or:
        case Value::Xor:
        case Value::Cmp: {
            auto& binary = (InstBinary&)inst;
            print.writer.writeByte(' ');
            printValue(print, *print.local[binary.lhs]);
            print.writer.writeString(", "_v);
            printValue(print, *print.local[binary.rhs]);
            break;
        }
        case Value::Call: {
            auto& call = (InstCall&)inst;
            print.writer.writeByte(' ');
            print.writer.writeString(print.context.findName(print.local[call.callee]->name));

            for(auto arg: call.args.contents(print.local)) {
                print.writer.writeString(", "_v);
                printValue(print, *print.local[arg]);
            }

            break;
        }
        case Value::Je: {
            auto& branch = (InstJe&)inst;
            print.writer.writeByte(' ');
            printValue(print, *print.local[branch.cond]);
            print.writer.writeString(", "_v);
            printBlockRef(print, *print.local[branch.thenBlock]);
            print.writer.writeString(", "_v);
            printBlockRef(print, *print.local[branch.elseBlock]);
            break;
        }
        case Value::Jmp:
            print.writer.writeByte(' ');
            printBlockRef(print, *print.local[((InstJmp&)inst).target]);
            break;
        case Value::Ret: {
            auto value = ((InstRet&)inst).value;
            if(value) {
                print.writer.writeByte(' ');
                printValue(print, *print.local[value]);
            }

            break;
        }
        case Value::Phi: {
            auto& phi = (InstPhi&)inst;
            Size index = 0;
            for(auto input: phi.inputs.contents(print.local)) {
                print.writer.writeString(index++ ? ", ["_v : " ["_v);
                printBlockRef(print, *print.local[input.block]);
                print.writer.writeString(", "_v);
                printValue(print, *print.local[input.value]);
                print.writer.writeByte(']');
            }

            break;
        }
        default:
            break;
    }

    if(produces) {
        print.writer.writeString(" : "_v);
        printType(print, inst.type);
    }

    print.writer.writeByte('\n');
}

static void printFunction(ResolvePrint& print, Function& function) {
    print.writer.writeString("fn "_v);
    print.writer.writeString(print.context.findName(function.name));
    print.writer.writeByte('(');

    Size index = 0;
    for(auto argPointer: function.args.contents(print.local)) {
        auto arg = print.local[argPointer];
        if(index++) print.writer.writeString(", "_v);
        printValue(print, *arg);
        print.writer.writeString(": "_v);
        printType(print, arg->type);
    }

    print.writer.writeString(") -> "_v);
    printType(print, function.returnType);
    print.writer.writeString(" {\n"_v);

    index = 0;
    for(auto blockPointer: function.blocks.contents(print.local)) {
        if(index++) print.writer.writeByte('\n');
        auto block = print.local[blockPointer];
        print.writer.writeString("  "_v);
        printBlockRef(print, *block);
        print.writer.writeString(":\n"_v);

        for(auto phi: block->phis.contents(print.local)) {
            printInstruction(print, *print.local[phi]);
        }

        for(auto instruction: block->instructions.contents(print.local)) {
            printInstruction(print, *print.local[instruction]);
        }
        if(block->terminator) printInstruction(print, *print.local[block->terminator]);
    }

    print.writer.writeString("}\n"_v);
}

// Only the root module is printed in full. An imported module contributes the functions
// something actually reached: Core declares a few hundred instance implementations, and all but
// the handful a program calls are dead weight in a fixture.
void printProgram(Net::Writer& writer, Context& context, Program& program) {
    ResolvePrint print { writer, context, program, *program.types, *program.arena };
    Size index = 0;

    for(auto module: program.modules) {
        for(auto function: module->functionOrder.contents(print.local)) {
            if(print.local[function]->signature) continue;
            if(!module->root && !print.local[function]->used) continue;
            if(index++) writer.writeByte('\n');
            printFunction(print, *print.local[function]);
        }
    }
}
