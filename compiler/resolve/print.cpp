#include "print.h"
#include "generic.h"
#include "witness.h"

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

static void printValue(ResolvePrint& print, Value& value);

static void printPlace(ResolvePrint& print, Function& function, const Place& place) {
    auto type = print.program.scalar.error;

    if(place.root == PlaceRoot::Global) {
        // A global is written as its own name with no sigil, which is what distinguishes it on
        // sight from the locals and values that carry one.
        auto global_ = print.local[place.global];
        print.writer.writeString(print.context.findName(global_->name));
        type = global_->type;
    } else if(place.root == PlaceRoot::Pointer || place.root == PlaceRoot::Borrow) {
        // `[%v3]` - the memory a pointer or a borrow names, as against the reference itself.
        auto& reference = *print.local[place.pointer];
        print.writer.writeByte('[');
        printValue(print, reference);
        print.writer.writeByte(']');

        type = place.root == PlaceRoot::Borrow
            ? ((BorrowType*)print.global[reference.type])->to
            : pointeeType(print.global, reference.type);
    } else {
        auto known = place.local < function.localCount();
        auto root = known ? function.localAt(print.local, place.local) : Local {};
        print.writer.writeByte('%');

        if(root.name) {
            print.writer.writeString(print.context.findName(root.name));
        } else {
            print.writer.writeString("local"_v);
            writeUInt(print.writer, place.local);
        }

        if(known) type = root.type;
    }

    auto projections = place.projections;

    for(auto projection: projections.contents(print.local)) {
        if(projection.kind == ProjectionKind::Discriminant) {
            print.writer.writeString(".discriminant"_v);
            type = print.program.scalar.int_;
        } else if(projection.kind == ProjectionKind::Property) {
            /*
             * A constrained field, by the name the constraint gave it - `%p.name?`.
             *
             * The `?` is not decoration: this is the one projection whose position is not known, and
             * a dump that printed it as `.name` would read as an ordinary field access and hide the
             * only interesting thing about it. It disappears at specialization, so it appears in a
             * generic body's dump and never in a specialization's.
             */
            auto env = functionGen(print.global, function);
            auto& schema = genSchemaOf(*print.program.core, *env);

            print.writer.writeByte('.');

            for(auto slot: schema.slots.contents(print.global)) {
                if(slot.kind != GenSlotKind::Property || slot.index != projection.index) continue;

                print.writer.writeString(print.context.findName(slot.name));
                type = slot.result;
            }

            print.writer.writeByte('?');
        } else if(projection.kind == ProjectionKind::Downcast) {
            auto record = (RecordType*)print.global[type];
            print.writer.writeByte('@');
            print.writer.writeString(print.context.findName(record->constructors.get(print.global, projection.index).name));
            type = record->constructors.get(print.global, projection.index).content;
        } else if(projection.kind == ProjectionKind::Field && print.global[type]->kind == Type::Fun) {
            // `%f.code` - a function value's three words are reached like any other aggregate's
            // fields, so they are printed like them rather than as offsets.
            print.writer.writeByte('.');
            print.writer.writeString(funValueFieldName(projection.index));
            type = funValueFieldType(*print.program.core, projection.index);
        } else if(projection.kind == ProjectionKind::Field) {
            auto tuple = (TupType*)print.global[type];
            auto field = tuple->fields.get(print.global, projection.index);
            print.writer.writeByte('.');

            if(field.name) print.writer.writeString(print.context.findName(field.name));
            else writeUInt(print.writer, projection.index);
            type = field.type;
        } else if(projection.kind == ProjectionKind::Deref) {
            print.writer.writeString(".*"_v);
            type = pointeeType(print.global, type);
        } else {
            print.writer.writeString("[...]"_v);
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
        case Value::Assign: return "assign"_v;
        case Value::Borrow: return ((InstBorrow&)value).mut ? "borrow_mut"_v : "borrow"_v;
        case Value::Move: return "move"_v;
        case Value::Swap: return "swap"_v;
        case Value::Exchange: return "exchange"_v;
        case Value::Copy: return "copy"_v;
        case Value::Drop:
            // Named after what it actually runs. A teardown with an authored half on either side is
            // opaque to region placement, so telling the two apart in the dump is telling apart the
            // two things placement decides between.
            if(((InstDrop&)value).dropKind == TeardownKind::Authored ||
               ((InstDrop&)value).reclaimKind == TeardownKind::Authored) {
                return "drop"_v;
            }

            return "drop_derived"_v;
        case Value::Address: return "addressof"_v;
        case Value::TypeMetric:
            switch(((InstTypeMetric&)value).metric) {
                case TypeMetricKind::Size: return "sizeof"_v;
                case TypeMetricKind::Align: return "alignof"_v;
                case TypeMetricKind::Stride: return "strideof"_v;
            }

            return "sizeof"_v;
        case Value::Native:
            switch(((InstNative&)value).op) {
                case NativeOp::CopyMemory: return "copymemory"_v;
                case NativeOp::SetMemory: return "setmemory"_v;
                case NativeOp::Syscall: return "syscall"_v;
            }
            break;
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
        case Value::Symbol: return "symbol"_v;
        case Value::Call: return "call"_v;
        case Value::CallDyn: return "calldyn"_v;
        case Value::GenCall: return "gencall"_v;
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
            // Where the storage came from, when it is not the frame. Silence means the ordinary
            // case, so that only a decision worth reading takes up room in a fixture.
            if(((InstAlloc&)inst).storage == StorageClass::Heap) {
                print.writer.writeString(((InstAlloc&)inst).releasedHere ? " heap"_v : " heap escaping"_v);
            }

            break;
        case Value::LoadPlace:
            print.writer.writeByte(' ');
            printPlace(print, *function, ((InstLoadPlace&)inst).place);
            break;
        case Value::Init:
        case Value::Assign: {
            auto& init = (InstInit&)inst;
            print.writer.writeByte(' ');
            printPlace(print, *function, init.place);
            print.writer.writeString(", "_v);
            printValue(print, *print.local[init.value]);
            break;
        }
        case Value::Borrow:
            print.writer.writeByte(' ');
            printPlace(print, *function, ((InstBorrow&)inst).place);
            break;
        case Value::Move: {
            auto& moved = (InstMove&)inst;
            print.writer.writeByte(' ');
            printPlace(print, *function, moved.place);

            // `move %x via Sink(T).sink` - which of the two relocations this is, since a bitwise
            // move and a call are very different things to read past.
            if(moved.sink) {
                print.writer.writeString(" via "_v);
                print.writer.writeString(print.context.findName(print.local[moved.sink]->name));
            }

            break;
        }
        case Value::Swap: {
            auto& swap = (InstSwap&)inst;
            print.writer.writeByte(' ');
            printPlace(print, *function, swap.a);
            print.writer.writeString(", "_v);
            printPlace(print, *function, swap.b);

            if(swap.sink) {
                print.writer.writeString(" via "_v);
                print.writer.writeString(print.context.findName(print.local[swap.sink]->name));
            }

            break;
        }
        case Value::Exchange: {
            auto& exchange = (InstExchange&)inst;
            print.writer.writeByte(' ');
            printPlace(print, *function, exchange.place);
            print.writer.writeString(", "_v);
            printValue(print, *print.local[exchange.value]);

            if(exchange.sink) {
                print.writer.writeString(" via "_v);
                print.writer.writeString(print.context.findName(print.local[exchange.sink]->name));
            }

            break;
        }
        case Value::Copy: {
            auto& copied = (InstCopy&)inst;
            print.writer.writeByte(' ');
            printPlace(print, *function, copied.place);

            if(copied.copy) {
                print.writer.writeString(" via "_v);
                print.writer.writeString(print.context.findName(print.local[copied.copy]->name));
            }

            break;
        }
        case Value::Drop: {
            auto& dropped = (InstDrop&)inst;
            print.writer.writeByte(' ');
            printPlace(print, *function, dropped.place);

            // The two halves are printed separately because they are elided separately: a region
            // reset discharges the reclaim and leaves the drop, and a dump that merged them would
            // not show which one survived.
            if(dropped.drop) {
                print.writer.writeString(" via "_v);
                print.writer.writeString(print.context.findName(print.local[dropped.drop]->name));
            }

            if(dropped.reclaim) {
                print.writer.writeString(" reclaim "_v);
                print.writer.writeString(print.context.findName(print.local[dropped.reclaim]->name));
            }

            // The last part of the reclaim half: handing back storage this frame owns. Printed
            // because a teardown of a type with nothing to run is otherwise indistinguishable from
            // no teardown at all.
            if(dropped.releaseStorage) print.writer.writeString(" release"_v);

            // `if %flag2` - the drop is conditional, which is a fact about the control flow that
            // reached here rather than about the type, so it is worth seeing at the drop.
            if(dropped.flag != maxLimit<U32>) {
                print.writer.writeString(" if "_v);
                printPlace(print, *function, Place::inLocal(dropped.flag));
            }

            break;
        }
        case Value::Address:
            print.writer.writeByte(' ');
            printPlace(print, *function, ((InstAddress&)inst).place);
            break;
        case Value::TypeMetric:
            // The type, not a number. What the number is depends on the target, and printing it
            // here would make every fixture that measures anything an assertion about the machine
            // this compiler happened to be built for.
            print.writer.writeByte(' ');
            printType(print, ((InstTypeMetric&)inst).of);
            break;
        case Value::Native: {
            auto& native = (InstNative&)inst;
            Size index = 0;

            for(auto arg: native.args.contents(print.local)) {
                print.writer.writeString(index++ ? ", "_v : " "_v);
                printValue(print, *print.local[arg]);
            }

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
        case Value::Symbol: {
            auto& symbol = (InstSymbol&)inst;
            print.writer.writeByte(' ');
            print.writer.writeString(print.context.findName(
                symbol.callee ? print.local[symbol.callee]->name : print.local[symbol.global]->name));

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
        case Value::CallDyn: {
            // The function value, or the bare address a compiler-internal call names instead.
            // The environment is not printed: it is not an argument, it is the word every callable
            // is reached through, and a dump that mixed it in would misnumber the real ones.
            auto& call = (InstCallDyn&)inst;
            print.writer.writeByte(' ');
            printValue(print, *print.local[call.callable ? call.callable : call.address]);

            for(auto arg: call.args.contents(print.local)) {
                print.writer.writeString(", "_v);
                printValue(print, *print.local[arg]);
            }

            break;
        }
        case Value::GenCall: {
            auto& call = (InstGenCall&)inst;

            // Printed because it is what distinguishes the two forms an InstGenCall can be in: one
            // waiting for a specialization to decide it, and one that has already been given
            // everything it needs and will be emitted as a call.
            if(call.env) {
                print.writer.writeString(" env "_v);
                print.writer.writeString(print.context.findName(print.local[call.env]->name));
            }

            print.writer.writeByte(' ');

            // `Ord(a).<=` for a class dispatch, `swap(a, b)` for a generic function: in both
            // cases what is printed is what specialization will substitute into.
            if(call.typeClass) {
                auto typeClass = print.global[call.typeClass];
                print.writer.writeString(print.context.findName(typeClass->name));
                print.writer.writeByte('(');
            } else {
                print.writer.writeString(print.context.findName(print.local[call.callee]->name));
                print.writer.writeByte('(');
            }

            Size index = 0;
            for(auto typeArg: call.typeArgs.contents(print.local)) {
                if(index++) print.writer.writeString(", "_v);
                printType(print, typeArg);
            }

            print.writer.writeByte(')');

            if(call.typeClass) {
                print.writer.writeByte('.');
                print.writer.writeString(print.context.findName(print.local[call.callee]->name));
            }

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

// A generic function's context, written the way source writes it: `(Ord(a)) fn max(...)`. The
// list includes the requirements the body turned out to need as well as the ones the signature
// declared, because they are the same thing by the time it is printed.
static void printGenEnv(ResolvePrint& print, GenEnv& env) {
    if(env.classes.isEmpty() && env.properties.isEmpty() && env.functions.isEmpty()) return;

    print.writer.writeByte('(');
    Size index = 0;

    for(auto constraint: env.classes.contents(print.global)) {
        if(index++) print.writer.writeString(", "_v);

        print.writer.writeString(print.context.findName(
            constraint.typeClass ? print.global[constraint.typeClass]->name : constraint.name));

        print.writer.writeByte('(');
        Size argIndex = 0;

        for(auto arg: constraint.args.contents(print.global)) {
            if(argIndex++) print.writer.writeString(", "_v);
            printType(print, arg);
        }

        print.writer.writeByte(')');
    }

    for(auto constraint: env.properties.contents(print.global)) {
        if(index++) print.writer.writeString(", "_v);
        printType(print, constraint.owner);
        print.writer.writeByte('.');
        print.writer.writeString(print.context.findName(constraint.field));
        print.writer.writeString(": "_v);
        printType(print, constraint.result);
    }

    for(auto constraint: env.functions.contents(print.global)) {
        if(index++) print.writer.writeString(", "_v);
        print.writer.writeString(print.context.findName(constraint.name));
        print.writer.writeString(": "_v);
        printType(print, constraint.signature);
    }

    print.writer.writeString(") "_v);
}

/*
 * The canonical slot numbering.
 *
 * Printed because it is what emitted code *loads*: a caller writes slot 3 and a callee reads slot 3,
 * and nothing else in the output would show that the two agree. A fixture asserting these numbers is
 * asserting the one property the erased ABI cannot check for itself.
 */
static void printGenSchema(ResolvePrint& print, Module& module, GenEnv& env) {
    auto& schema = genSchemaOf(module, env);
    if(schema.slots.isEmpty()) return;

    print.writer.writeString("  schema {\n"_v);

    for(auto slot: schema.slots.contents(print.global)) {
        print.writer.writeString("    "_v);
        print.writer.writeByte('#');
        writeUInt(print.writer, slot.index);
        print.writer.writeByte(' ');

        switch(slot.kind) {
            case GenSlotKind::Type:
                print.writer.writeString("type "_v);
                printType(print, slot.type);
                break;
            case GenSlotKind::Class:
                print.writer.writeString("class "_v);
                print.writer.writeString(print.context.findName(print.global[slot.typeClass]->name));
                print.writer.writeByte('(');

                {
                    Size argIndex = 0;
                    for(auto arg: slot.args.contents(print.global)) {
                        if(argIndex++) print.writer.writeString(", "_v);
                        printType(print, arg);
                    }
                }

                print.writer.writeByte(')');
                break;
            case GenSlotKind::Property:
                print.writer.writeString("property "_v);
                printType(print, slot.type);
                print.writer.writeByte('.');
                print.writer.writeString(print.context.findName(slot.name));
                print.writer.writeString(": "_v);
                printType(print, slot.result);
                break;
            case GenSlotKind::Function:
                print.writer.writeString("function "_v);
                print.writer.writeString(print.context.findName(slot.name));
                print.writer.writeString(": "_v);
                printType(print, slot.type);
                break;
        }

        print.writer.writeByte('\n');
    }

    print.writer.writeString("  }\n\n"_v);
}

static void printFunction(ResolvePrint& print, Function& function) {
    if(function.gen) printGenEnv(print, *print.global[function.gen]);

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

    if(function.gen && function.module) printGenSchema(print, *function.module, *print.global[function.gen]);

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
// `let &heapNext: %U8 = 0` - the declaration as written, since a global has no body to print.
/*
 * A compiler-built constant table - a TypeDesc, a runtime environment.
 *
 * Printed as its scalar words plus the symbol each of its addresses names, because the bytes on
 * their own say nothing and the addresses are not known until the module is placed. This is the
 * only way a fixture can assert that slot N of an environment holds the descriptor it should.
 */
static void printTable(ResolvePrint& print, Global& global_) {
    print.writer.writeString("table "_v);
    print.writer.writeString(print.context.findName(global_.name));

    // Where a table goes is part of what it is, for the one kind that does not go with the data:
    // a closure header is these bytes at this function's entry point, and a fixture that could not
    // see which function would not be asserting anything about it.
    if(global_.prefixOf) {
        print.writer.writeString(" before "_v);
        print.writer.writeString(print.context.findName(print.local[global_.prefixOf]->name));
    }

    print.writer.writeString(" {\n"_v);

    /*
     * One line per slot, numbered by slot.
     *
     * Not by byte offset, which is what this used to print: a table has no byte offsets until some
     * backend gives it one, and a resolve dump that showed native ones would be asserting a layout
     * this stage does not choose. Numbering by slot is also what makes the dump say the same thing
     * for both targets, which is the property the whole arrangement exists for.
     */
    Size index = 0;

    for(auto slot: global_.table.contents(print.local)) {
        print.writer.writeString("  ."_v);
        writeUInt(print.writer, index++);
        print.writer.writeByte(' ');

        switch(slot.kind) {
            case TableCell::Int:
                writeUInt(print.writer, slot.value);
                break;

            case TableCell::Type:
                printType(print, TypePtr(slot.value));
                break;

            // The measurement, not a number: this stage has no idea what the answer is, and a dump
            // that guessed one would be asserting a layout nothing here chose.
            case TableCell::Metric:
                switch(slot.metric) {
                    case TypeMetricKind::Size: print.writer.writeString("sizeof "_v); break;
                    case TypeMetricKind::Align: print.writer.writeString("alignof "_v); break;
                    case TypeMetricKind::Stride: print.writer.writeString("strideof "_v); break;
                }

                printType(print, TypePtr(slot.value));
                break;

            case TableCell::Class:
                print.writer.writeString(print.context.findName(
                    print.global[GlobalPtr<TypeClass>(slot.value)]->name));
                break;

            // An address, by the name of what it names. A null one is the deliberately empty slot -
            // "nothing to do" - and says so rather than printing a zero that looks like a number.
            case TableCell::Function:
                if(slot.function) {
                    print.writer.writeString(print.context.findName(print.local[slot.function]->name));
                } else {
                    print.writer.writeString("null"_v);
                }

                break;

            case TableCell::Global:
                if(slot.global) {
                    print.writer.writeString(print.context.findName(print.local[slot.global]->name));
                } else {
                    print.writer.writeString("null"_v);
                }

                break;
        }

        print.writer.writeByte('\n');
    }

    print.writer.writeString("}\n"_v);
}

static void printGlobal(ResolvePrint& print, Global& global_) {
    if(global_.isTable) {
        printTable(print, global_);
        return;
    }

    print.writer.writeString(global_.mut ? "let &"_v : "let "_v);
    print.writer.writeString(print.context.findName(global_.name));
    print.writer.writeString(": "_v);
    printType(print, global_.type);
    print.writer.writeString(" = "_v);
    writeUInt(print.writer, global_.initial);
    print.writer.writeByte('\n');
}

void printProgram(Net::Writer& writer, Context& context, Program& program) {
    ResolvePrint print { writer, context, program, *program.types, *program.arena };
    Size index = 0;

    for(auto module: program.modules) {
        for(auto global_: module->globalOrder.contents(print.local)) {
            if(!module->root && !print.local[global_]->used) continue;
            if(index++) writer.writeByte('\n');
            printGlobal(print, *print.local[global_]);
        }
    }

    for(auto module: program.modules) {
        for(auto function: module->functionOrder.contents(print.local)) {
            if(print.local[function]->signature) continue;
            if(!module->root && !print.local[function]->used) continue;
            if(index++) writer.writeByte('\n');
            printFunction(print, *print.local[function]);
        }
    }
}
