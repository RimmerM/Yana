#include "print.h"
#include "const.h"
#include "generic.h"
#include "place.h"
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
    if(place.root == PlaceRoot::Global) {
        // A global is written as its own name with no sigil, which is what distinguishes it on
        // sight from the locals and values that carry one.
        print.writer.writeString(print.context.findName(print.local[place.global]->name));
    } else if(place.root == PlaceRoot::Pointer || place.root == PlaceRoot::Borrow) {
        // `[%v3]` - the memory a pointer or a borrow names, as against the reference itself.
        auto& reference = *print.local[place.pointer];
        print.writer.writeByte('[');
        printValue(print, reference);
        print.writer.writeByte(']');
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
    }

    /*
     * The path, walked once by the walk everything shares - see resolve/place.h. What is printed is
     * the *name* of each step, which is the half a dump owns; the type each one arrives at is the
     * half every other consumer wanted too, and carrying it here as well is what it used to mean to
     * write this switch a fourteenth time.
     */
    walkPlace(*print.program.core, function, place, [&](const PlaceStep& step) {
        // A dump is asked about programs that did not resolve as well as ones that did, so a path
        // whose type ran out is printed as an unknown step rather than followed.
        if(step.broken) {
            print.writer.writeString(step.kind == ProjectionKind::Downcast ? "@?"_v : ".?"_v);
            return false;
        }

        switch(step.kind) {
            case ProjectionKind::Discriminant:
                print.writer.writeString(".discriminant"_v);
                break;

            case ProjectionKind::Property: {
                /*
                 * A constrained field, by the name the constraint gave it - `%p.name?`.
                 *
                 * The `?` is not decoration: this is the one projection whose position is not known,
                 * and a dump that printed it as `.name` would read as an ordinary field access and
                 * hide the only interesting thing about it. It disappears at specialization, so it
                 * appears in a generic body's dump and never in a specialization's.
                 */
                auto env = functionGen(print.global, function);
                auto& schema = genSchemaOf(*print.program.core, *env);

                print.writer.writeByte('.');

                for(auto slot: schema.slots.contents(print.global)) {
                    if(slot.kind != GenSlotKind::Property || slot.index != step.index) continue;

                    print.writer.writeString(print.context.findName(slot.name));
                }

                print.writer.writeByte('?');
                break;
            }

            case ProjectionKind::Downcast: {
                auto record = (RecordType*)print.global[step.owner];
                auto constructor = record->constructors.get(print.global, step.index);
                print.writer.writeByte('@');
                print.writer.writeString(print.context.findName(constructor.name));
                break;
            }

            case ProjectionKind::Field:
                print.writer.writeByte('.');

                // `%f.code` - a function value's three words are reached like any other aggregate's
                // fields, so they are printed like them rather than as offsets.
                if(print.global[step.owner]->kind == Type::Fun) {
                    print.writer.writeString(funValueFieldName(step.index));
                    break;
                }

                {
                    auto tuple = (TupType*)print.global[step.owner];
                    auto field = tuple->fields.get(print.global, step.index);

                    if(field.name) print.writer.writeString(print.context.findName(field.name));
                    else writeUInt(print.writer, step.index);
                }

                break;

            case ProjectionKind::Index:
                // `%xs[%i]` - one element of a `[T *n]`, or one element on from a reference
                // (Implementation-Containers.md §14.1), by the value that selects it rather than by
                // a position: the elements have no names and the index need not be constant.
                print.writer.writeByte('[');
                if(step.value) printValue(print, *print.local[step.value]);
                else print.writer.writeByte('?');
                print.writer.writeByte(']');
                break;

            case ProjectionKind::Deref:
                print.writer.writeString(".*"_v);
                break;

            case ProjectionKind::Unit:
                // `%h@Header.version:unit32` - the word the field was packed into, by the width that
                // says how much of the storage the access covers. The field it follows is kept in
                // the path rather than replaced by an offset, so a dump still says which word this
                // is.
                print.writer.writeString(":unit"_v);
                writeUInt(print.writer, step.index);
                break;
        }

        return true;
    });
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
            } else if(type->kind == Type::Int && ((IntType*)type)->isSigned && I64(constant) < 0) {
                // Constants are stored sign-extended, so a signed type's negative values are large
                // unsigned ones in the payload. Printing the bit pattern turns `- 4503599627370496`
                // in the source into 18442240474082181120 in the dump, which reads as a different
                // program rather than as the same one.
                print.writer.writeByte('-');
                writeUInt(print.writer, U64(-I64(constant)));
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
        case Value::ConstString: {
            // Quoted, and printed raw. A fixture's IR dump is read by a person, and a literal that
            // came out of the lexer's escape decoding is more useful shown as its content than
            // re-escaped into the form it was written in.
            auto text = print.context.findName(((ConstString&)value).text);
            print.writer.writeByte('"');
            print.writer.writeString(StringView { text.text(), text.size() });
            print.writer.writeByte('"');
            return;
        }
        default:
            print.writer.writeString("%v"_v);
            writeUInt(print.writer, value.id);
            return;
    }
}

/*
 * What one instruction is called in the dump.
 *
 * The name of a *kind* is inst.def's, which is what this used to spell out one `return` per opcode.
 * What is left is the five that refine it from their own fields, and each of them refines it for the
 * same reason: the distinction is one a reader of the dump is meant to be able to make, and the
 * fields it comes out of are not printed anywhere else.
 */
static StringView instructionName(Value& value, GlobalBase global) {
    switch(value.kind) {
        case Value::Borrow: return ((InstBorrow&)value).mut ? "borrow_mut"_v : "borrow"_v;
        case Value::Drop:
            // Named after what it actually runs. A teardown with an authored half on either side is
            // opaque to region placement, so telling the two apart in the dump is telling apart the
            // two things placement decides between.
            if(((InstDrop&)value).dropKind == TeardownKind::Authored ||
               ((InstDrop&)value).reclaimKind == TeardownKind::Authored) {
                return "drop"_v;
            }

            return "drop_derived"_v;
        case Value::TypeMetric:
            switch(((InstTypeMetric&)value).metric) {
                case TypeMetricKind::Size: return "sizeof"_v;
                case TypeMetricKind::Align: return "alignof"_v;
                case TypeMetricKind::Stride: return "strideof"_v;
            }
            break;
        case Value::Native:
            switch(((InstNative&)value).op) {
                case NativeOp::CopyMemory: return "copymemory"_v;
                case NativeOp::SetMemory: return "setmemory"_v;
                case NativeOp::Syscall: return "syscall"_v;
                case NativeOp::HostCall: return "hostcall"_v;
                case NativeOp::HostField: return "hostfield"_v;
                case NativeOp::HostArray: return "hostarray"_v;
                case NativeOp::HostBinary: return "hostbinary"_v;
                case NativeOp::HostGlobalCall: return "hostglobalcall"_v;
                case NativeOp::HostThrow: return "hostthrow"_v;
            }
            break;
        case Value::Cmp:
            switch(((InstCmp&)value).cmp) {
                case CompareOp::Eq: return "cmp_eq"_v;
                case CompareOp::Ne: return "cmp_ne"_v;
                case CompareOp::Gt: return "cmp_gt"_v;
                case CompareOp::Ge: return "cmp_ge"_v;
                case CompareOp::Lt: return "cmp_lt"_v;
                case CompareOp::Le: return "cmp_le"_v;
            }
            break;
        default:
            break;
    }

    return instructionMnemonic(value.kind);
}

static void printBlockRef(ResolvePrint& print, Block& block) {
    print.writer.writeString("b"_v);
    writeUInt(print.writer, block.index);
}

static void printInstruction(ResolvePrint& print, Inst& inst) {
    print.writer.writeString("    "_v);

    // Whether there is a name to print on the left. The kind decides whether there is a result at
    // all - a store, a drop and the three terminators define nothing - and the type decides whether
    // one that exists is worth naming, since a call may still answer unit.
    auto produces = producesValue(inst) && !isUnit(print.global, inst.type);
    if(produces) {
        printValue(print, inst);
        print.writer.writeString(" = "_v);
    }

    print.writer.writeString(instructionName(inst, print.global));
    auto function = print.local[print.local[inst.block]->function];

    switch(inst.kind) {
        case Value::Alloc:
            // How many, when it is a run rather than one object - see InstAlloc::extent.
            if(auto extent = ((InstAlloc&)inst).extent) {
                print.writer.writeString(" *"_v);
                printValue(print, *print.local[extent]);
            }

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
        /*
         * One `place = value` per component, rather than the base place and a list of values.
         *
         * The values alone do not say what the instruction *is*: fields 0 and 2 print the same as
         * fields 0 and 1, and a sum's tag and payload print the same as two tuple fields. So a
         * golden fixture could not have caught a corrupted component mapping, which is most of what
         * there is to get wrong here - `constructor` and the steps are the whole of the shape.
         *
         * Printed through `aggregateElement` for the same reason every other consumer goes through
         * it: what a fixture shows has to be the place the passes reasoned about. That builds a
         * path in the module arena, which is the one allocation this file makes - a dump is not on
         * any hot path, and printing a *different* path than the passes walk is the failure this is
         * here to prevent.
         */
        case Value::Aggregate: {
            auto& aggregate = (InstAggregate&)inst;
            auto first = true;

            eachWrittenComponent(print.local, print.program.core->arena, aggregate,
                                 [&](Place place, ModulePtr<Value> value, Size) {
                print.writer.writeString(first ? " "_v : ", "_v);
                first = false;

                printPlace(print, *function, place);
                print.writer.writeString(" = "_v);
                printValue(print, *print.local[value]);
            });

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

            // The host member, where there is one. It is what the operation *is* - two `hostcall`s
            // differing only in their method are two different operations - so it prints in front of
            // the arguments rather than as one of them.
            if(native.method) {
                print.writer.writeByte(' ');
                print.writer.writeString(print.context.findName(native.method));
            }

            for(auto arg: native.args.contents(print.local)) {
                print.writer.writeString(index++ ? ", "_v : " "_v);
                printValue(print, *print.local[arg]);
            }

            break;
        }
        case Value::Cast:
        case Value::Bitcast:
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
        case Value::Select: {
            auto& select = (InstSelect&)inst;
            print.writer.writeByte(' ');
            printValue(print, *print.local[select.cond]);
            print.writer.writeString(" ? "_v);
            printValue(print, *print.local[select.whenTrue]);
            print.writer.writeString(" : "_v);
            printValue(print, *print.local[select.whenFalse]);
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

        for(auto phi: block->phis(print.local)) {
            printInstruction(print, *print.local[phi]);
        }

        for(auto instruction: block->instructions(print.local)) {
            printInstruction(print, *print.local[instruction]);
        }
        if(block->terminator()) printInstruction(print, *print.local[block->terminator()]);
    }

    print.writer.writeString("}\n"_v);
}

// Only the root module is printed in full. An imported module contributes the functions
// something actually reached: Core declares a few hundred instance implementations, and all but
// the handful a program calls are dead weight in a fixture.
// `let &heapNext: %U8 = 0` - the declaration as written, since a global has no body to print.
/*
 * A source-level constant - see ConstValue.
 *
 * As the value rather than as bytes, which is what it is: an aggregate prints as its components in
 * braces, a fixed array in brackets, a constructor by name, and an address as the symbol it names.
 * A dump that printed a layout would be asserting one this stage does not choose, which is the same
 * rule `printTable` states for the compiler's own constants.
 */
static void printConstant(ResolvePrint& print, ModulePtr<ConstValue> constant) {
    if(!constant) {
        print.writer.writeString("<none>"_v);
        return;
    }

    auto& value = *print.local[constant];

    auto components = [&](char open, char close) {
        print.writer.writeByte(open);

        auto first = true;
        for(auto child: value.children.contents(print.local)) {
            if(!first) print.writer.writeString(", "_v);
            first = false;
            printConstant(print, child);
        }

        print.writer.writeByte(close);
    };

    switch(value.kind) {
        case ConstKind::Scalar:
            writeUInt(print.writer, value.bits);
            return;
        case ConstKind::Aggregate:
            if(print.global[value.type]->kind == Type::Array) components('[', ']');
            else components('{', '}');
            return;
        case ConstKind::Construct: {
            auto declared = print.global[value.type];
            if(declared->kind == Type::Record && value.index < ((RecordType*)declared)->constructors.size()) {
                auto name = ((RecordType*)declared)->constructors.get(print.global, Size(value.index)).name;
                print.writer.writeString(print.context.findName(name));
            }

            if(value.children.size()) components('(', ')');
            return;
        }
        case ConstKind::Address:
            print.writer.writeByte('&');
            print.writer.writeString(print.context.findName(print.local[value.global]->name));
            return;
        case ConstKind::String:
            // The text and the static form both, because they are two different assertions: what the
            // string is, and which bytes and which run the target will write for it.
            print.writer.writeByte('"');
            print.writer.writeString(print.context.findName(value.text));
            print.writer.writeByte('"');

            if(value.children.size()) {
                print.writer.writeByte(' ');
                components('{', '}');
            }

            return;
    }
}

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
                writeUInt(print.writer, slot.value());
                break;

            // The measurement and the constant beside it, as `alignof T | 131` - the two halves
            // printed as the two halves rather than combined, since combining them needs the answer
            // this stage does not have.
            case TableCell::PackedMetric:
            case TableCell::Metric:
                switch(slot.metric) {
                    case TypeMetricKind::Size: print.writer.writeString("sizeof "_v); break;
                    case TypeMetricKind::Align: print.writer.writeString("alignof "_v); break;
                    case TypeMetricKind::Stride: print.writer.writeString("strideof "_v); break;
                }

                printType(print, slot.metricType());

                if(slot.kind == TableCell::PackedMetric) {
                    print.writer.writeString(" | "_v);
                    writeUInt(print.writer, slot.extra);
                }

                break;

            // An address, by the name of what it names. A null one is the deliberately empty slot -
            // "nothing to do" - and says so rather than printing a zero that looks like a number.
            case TableCell::Function:
                if(auto function = slot.function()) {
                    print.writer.writeString(print.context.findName(print.local[function]->name));
                } else {
                    print.writer.writeString("null"_v);
                }

                break;

            case TableCell::Global:
                if(auto global_ = slot.global()) {
                    print.writer.writeString(print.context.findName(print.local[global_]->name));
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

    /*
     * A dynamically initialized global has no constant to print - what it holds is written by the
     * entry sequence, which is where the initializer is. Printing `initial` for one would print the
     * zero its storage starts at as though it were the declaration, which is the one thing a reader
     * of this dump would take it for.
     *
     * A string literal's blob has none either, and its bytes are the whole of it - see
     * Global::literalBytes. Printed as the bytes rather than as nothing, since which literal a
     * relocation points at is exactly what a fixture holding a constant string wants to assert.
     */
    if(global_.dynamic) {
        print.writer.writeString("<startup>"_v);
    } else if(global_.literalBytes.length) {
        print.writer.writeByte('"');
        print.writer.writeString(StringView((const char*)global_.literalBytes.ptr,
                                            global_.literalBytes.length));
        print.writer.writeByte('"');
    } else {
        printConstant(print, global_.initial);
    }

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
