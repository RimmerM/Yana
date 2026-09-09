#include "print.h"
#include "../intrinsic/primitive.h"

namespace mir {

static void writeUInt(Net::Writer& writer, U64 value) {
    writer.writeBytes(64, [&](Byte* buffer) { return show(value, (char*)buffer, 64); });
}

static void writeInt(Net::Writer& writer, I64 value) {
    writer.writeBytes(64, [&](Byte* buffer) { return show(value, (char*)buffer, 64); });
}

static void writeFloat(Net::Writer& writer, F64 value) {
    writer.writeBytes(64, [&](Byte* buffer) { return show(value, (char*)buffer, 64); });
}

void printType(Net::Writer& writer, Context& context, DeclBase base, DeclPtr<Type> type) {
    if(!type) {
        writer.writeString("<none>"_v);
        return;
    }

    auto& value = *base[type];
    switch(value.kind) {
        case Type::Unit:
            writer.writeString("{}"_v);
            break;
        // The source's own name for each, from `primitive.def` - which is also what a diagnostic
        // prints, so a dump and a message name one type one way. Composing a float's out of a letter
        // and a width is the same mistake `primitiveIntName` exists to prevent one kind up.
        case Type::Int: {
            // A `@bits(n)` refinement is spelled the way it was written, on the type it refines -
            // `typeText` writes the same thing, for the reason stated above it.
            auto& integer = (IntType&)value;
            if(integer.canonical) {
                writer.writeString("@bits("_v);
                writeUInt(writer, integer.bits);
                writer.writeString(") "_v);
                printType(writer, context, base, integer.canonical);
                break;
            }

            writer.writeString(intTypeName(integer));
            break;
        }
        case Type::Float:
            writer.writeString(intrinsic::primitiveName(intrinsic::PrimitiveKind::Float,
                                                        ((FloatType&)value).bits));
            break;
        case Type::Pointer:
            writer.writeString("%"_v);
            printType(writer, context, base, ((PointerType&)value).to);
            break;
        /*
         * `Vec(F32)`, `Mask(F32, 2)` and `Atomic(I32)` - spelled by `typeText` rather than written
         * out again here.
         *
         * The one arm in this function that delegates, and the reason is the natural-count rule:
         * whether a count is printed depends on the target's register width, which is a decision
         * rather than a spelling, and a dump that made it separately from a diagnostic is two
         * answers to one question. Every other kind here is a spelling, which is why every other
         * kind is written out.
         */
        case Type::Vector:
        case Type::Atomic: {
            auto text = typeText(context, base, type);
            writer.writeString(StringView { text.text(), text.size() });
            break;
        }
        case Type::Variable:
            writer.writeString(stringView(context.findName(((VariableType&)value).name)));
            break;
        case Type::Const: {
            // The number alone. What it is a number *of* is on the parameter it fills, and a dump
            // that repeated the width at every argument would be describing the head at each use. A
            // count that is still an expression prints as the call it stands for.
            auto& number = (ConstType&)value;

            if(number.isDerived()) {
                writer.writeString("lanes("_v);
                printType(writer, context, base, number.lanesOf);
                writer.writeString(")"_v);
            } else {
                writeUInt(writer, number.value);
            }

            break;
        }
        case Type::Array: {
            // `[I32 *4]` - the same spelling `typeText` gives, for the same reason.
            auto& array = (ArrayType&)value;
            writer.writeString("["_v);
            printType(writer, context, base, array.element);
            writer.writeString(" *"_v);

            if(array.hasFixedLength()) writeUInt(writer, array.length);
            else printType(writer, context, base, array.count);

            writer.writeString("]"_v);
            break;
        }
        case Type::Fun: {
            // The shape, which is the whole of a structural type's spelling - `typeText` prints the
            // same thing for the same reason, so a dump and a message name one type one way.
            auto& fun = (FunType&)value;
            writer.writeString("("_v);
            auto first = true;

            for(auto param: fun.params.contents(base)) {
                if(!first) writer.writeString(", "_v);
                first = false;
                writer.writeString(conventionMark(param.occurrence.contract, Access::Shared));
                printType(writer, context, base, param.occurrence.type);
            }

            writer.writeString(") -> "_v);
            writer.writeString(conventionMark(fun.result.contract, Access::Owned));
            printType(writer, context, base, fun.result.type);
            break;
        }
        case Type::Record: {
            auto& record = (RecordType&)value;

            // A tuple, spelled as its fields - `typeText` prints the same thing for the same
            // reason, so a dump and a message name one type one way. There is no declaration for
            // the fields to be printed at instead, which is what makes this the whole spelling.
            if(record.tuple) {
                writer.writeString("{"_v);
                auto first = true;

                for(auto field: record.fields.contents(base)) {
                    if(!first) writer.writeString(", "_v);
                    first = false;

                    if(field.name != StringId()) {
                        writer.writeString(stringView(context.findName(field.name)));
                        writer.writeString(": "_v);
                    }

                    printType(writer, context, base, field.occurrence.type);
                }

                writer.writeString("}"_v);
                break;
            }

            // The declared name, and the arguments where it is an instantiation. A nominal type is
            // its declaration, so the fields are printed once where that declaration is - see
            // `Printer::record` - and what has to be here is whatever tells two instantiations of
            // one declaration apart.
            writer.writeString(stringView(context.findName(record.name)));

            if(!record.arguments.size()) break;

            writer.writeString("("_v);
            auto first = true;

            for(auto argument: record.arguments.contents(base)) {
                if(!first) writer.writeString(", "_v);
                first = false;
                printType(writer, context, base, argument);
            }

            writer.writeString(")"_v);
            break;
        }
        case Type::KindCount:
            break;
    }
}

/*
 * The contract, printed only where it is not the default.
 *
 * Owned, here, whole value is what a fresh result, a local and a plain field all are, and printing
 * it on every line would bury the occurrences that are actually interesting under the ones that are
 * not. A golden that says `I32` therefore means "owned here", and one that says `I32 [shared call]`
 * means a call-bounded loan - which is the distinction every ownership fixture is about.
 */
// One contract, in the words `accessName`/`originName` give it. Shared by the root and by every
// interior slot, because a slot now *is* a contract and a second spelling of one would be a second
// thing to keep in step with the enum.
static void printContract(Net::Writer& writer, Contract contract) {
    writer.writeString(accessName(contract.access()));
    writer.writeString(" "_v);
    writer.writeString(originName(contract.origin()));

    if(contract.origin() == Origin::Slot) {
        writer.writeString(" "_v);
        writeUInt(writer, contract.slot());
    }

    if(contract.form() != AccessForm::WholeValue) {
        writer.writeString(" "_v);
        writer.writeString(accessFormName(contract.form()));
    }
}

void printOccurrence(Net::Writer& writer, Context& context, DeclBase base, const Occurrence& occurrence) {
    printType(writer, context, base, occurrence.type);

    auto contract = occurrence.contract;
    auto isDefault = contract == ownedContract() && Shapes::isEmpty(occurrence.shape);
    if(isDefault) return;

    writer.writeString(" ["_v);
    printContract(writer, contract);

    // The interior shape, as its interned identity and the slots it holds. Printed because a golden
    // that showed `Pair(T, U)` for both `Pair(a'T, U)` and `Pair(T, a'U)` would be a golden that
    // could not have caught the two being confused.
    if(!Shapes::isEmpty(occurrence.shape)) {
        auto schema = base[occurrence.shape];
        writer.writeString(" shape"_v);
        writeUInt(writer, schema->id);

        for(auto slotPointer: schema->slots.contents(base)) {
            auto slot = base[slotPointer];
            writer.writeString(" ."_v);

            auto first = true;
            for(auto step: slot->path.contents(base)) {
                if(!first) writer.writeString("."_v);

                // A pointee is spelled rather than numbered - it has no index, and printing one
                // would read as a field of whatever came before it.
                if(PathStep::isPointee(step)) writer.writeString("*"_v);
                else writeUInt(writer, PathStep::fieldIndex(step));

                first = false;
            }

            writer.writeString("="_v);
            printContract(writer, slot->contract);
        }
    }

    writer.writeString("]"_v);
}

void printConstant(Net::Writer& writer, Context& context, DeclBase base,
                   const Constant& value) {
    switch(value.kind) {
        case Constant::None:
            writer.writeString("<none>"_v);
            break;
        case Constant::Unit:
            writer.writeString("{}"_v);
            break;
        case Constant::Enum: {
            // The constructor's own name, which is the library's word and not the compiler's -
            // `True` prints as `True` because `lib/Core` called it that.
            auto& record = *(RecordType*)base[value.type];
            writer.writeString(stringView(context.findName(
                record.cons.get(base, value.asConstructor()).name)));
            break;
        }
        case Constant::Int: {
            auto type = (IntType*)base[value.type];
            if(type->kind == Type::Int && type->isSigned) writeInt(writer, I64(value.asInt()));
            else writeUInt(writer, value.asInt());
            break;
        }
        case Constant::Float:
            writeFloat(writer, value.asFloat());
            break;
        case Constant::Text:
            // The text as it was written. Quoted rather than escaped, which is what every other
            // constant does here: this is a dump and not a source form.
            writer.writeString("\""_v);
            writer.writeString(stringView(context.findName(value.asText())));
            writer.writeString("\""_v);
            break;
        case Constant::Address:
            // The global's own name, which is what the relocation will name. `&` because that is
            // what the value is - the address of that storage and not what is in it.
            writer.writeString("&"_v);
            writer.writeString(stringView(context.findName(base[value.asGlobal()]->name)));
            break;
        case Constant::Aggregate: {
            // A fixed array, as the literal it reduced from - the bytes of a string literal are one.
            // A **vector** prints the same way and for the same reason: `0 :: Vec(I32, 4)` reduces to
            // the lane repeated, and what it is is a run of values with no constructor over them.
            if(base[value.type]->kind == Type::Array || base[value.type]->kind == Type::Vector) {
                auto& parts = *base[value.aggregate()];
                writer.writeString("["_v);
                for(U32 i = 0; i < parts.values.size(); i++) {
                    if(i) writer.writeString(", "_v);
                    printConstant(writer, context, base, parts.values.get(base, i));
                }
                writer.writeString("]"_v);
                break;
            }

            /*
             * The value as it would have been *written*, which is what a dump of a constant is for:
             * `Circle(7)` and `Point {x: 1, y: 2}` read back as the source that reduced to them.
             *
             * The three spellings are the three a construction has, told apart the same way
             * `constructionArguments` tells them apart - a constructor's own name where it has one,
             * the one-payload form where the single field carries that name, and a braced list
             * otherwise, with each field's label where it has one.
             */
            auto& record = *(RecordType*)base[value.type];
            auto& parts = *base[value.aggregate()];
            auto arm = record.cons.get(base, parts.constructor);

            if(arm.name != StringId()) {
                writer.writeString(stringView(context.findName(arm.name)));
            }

            if(!arm.count) break;

            auto payload = arm.count == 1 && record.field(base, arm.first).name == arm.name;

            writer.writeString(payload ? "("_v : " {"_v);

            for(U16 i = 0; i < arm.count; i++) {
                if(i) writer.writeString(", "_v);

                auto field = record.field(base, U16(arm.first + i));
                if(!payload && field.name != StringId()) {
                    writer.writeString(stringView(context.findName(field.name)));
                    writer.writeString(": "_v);
                }

                printConstant(writer, context, base, parts.values.get(base, i));
            }

            writer.writeString(payload ? ")"_v : "}"_v);
            break;
        }
    }
}

namespace {

struct Printer {
    Net::Writer& writer;
    Context& context;
    Program& program;
    DeclBase decl;
    MirBase mir;

    void name(StringId id) {
        auto text = context.findName(id);
        writer.writeString(stringView(text));
    }

    /*
     * What a local is called in a dump.
     *
     * A temporary has no name - the resolver made it to give an owner a place to live in, and
     * claiming the author wrote a binding would be describing the source rather than the IR - so it
     * is called by its index. One function, because the declaration list and every place path have
     * to call it the same thing or a reader cannot pair them.
     */
    void localName(Local& local) {
        if(local.name) return name(local.name);

        writer.writeString("local"_v);
        writeUInt(writer, local.index.index);
    }

    /*
     * A place as its path.
     *
     * Written out rather than printed as its interned ID: an ID is what the representation stores
     * and a path is what a reader has to check, and a golden whose place column was a number would
     * change whenever an unrelated projection was interned earlier.
     *
     * `placeText` is the one spelling and a diagnostic uses the same one - see print.h. It builds a
     * `String` this immediately writes, which is a copy per place printed and is what one spelling
     * costs; a dump is not on any path where that has ever mattered.
     */
    void place(Body& body, Place& value) {
        auto text = placeText(context, decl, mir, body, value);
        writer.writeString(StringView { text.text(), text.size() });
    }

    void operand(Body& body, Operand value) {
        writer.writeString(useModeName(value.mode()));
        writer.writeString(" "_v);

        switch(value.source()) {
            case SourceKind::Result:
                writer.writeString("%"_v);
                writeUInt(writer, value.id());
                break;
            case SourceKind::Place:
                writer.writeString("("_v);
                place(body, *body.place(mir, value.placeId()));
                writer.writeString(")"_v);
                break;
            case SourceKind::Constant:
                printConstant(writer, context, decl, body.constant(mir, value.constantId()));
                break;
        }
    }

    /*
     * What a node calls itself.
     *
     * Three kinds print something narrower than their inst.def row: an operation prints the
     * operation rather than "binary", and a write prints what it does to what was there. inst.def
     * says the mnemonic column is the base name and that this is where it is refined, so that the
     * roster stays one row per kind rather than one row per spelling.
     */
    void mnemonic(Node& value) {
        switch(value.kind) {
            case Node::Binary:
                writer.writeString(binaryOpName(((NodeBinary&)value).op));
                return;
            case Node::Unary:
                writer.writeString(unaryOpName(((NodeUnary&)value).op));
                return;
            case Node::Write:
                writer.writeString("write."_v);
                writer.writeString(destinationName(((NodeWrite&)value).mode));
                return;
            /*
             * The **constructor**, which is the one thing an operand list cannot show: two arms of a
             * sum may take the same number of components of the same types, and which one is being
             * built is what decides the discriminant. A record with one constructor prints its name
             * too, which is the type's - so `aggregate.Pair` reads as what it is rather than as a
             * special case for sums.
             */
            case Node::Aggregate: {
                writer.writeString("aggregate."_v);

                auto type = valueOccurrence(value).type;
                auto index = ((NodeAggregate&)value).constructor;

                // A fill states itself, because the operand list does not: one component at a
                // quantified count means every slot, and a dump that printed it as a one-element
                // array would be the one reading that is wrong.
                if(((NodeAggregate&)value).fill) {
                    writer.writeString("fill"_v);
                    return;
                }

                if(type && decl[type]->kind == Type::Record) {
                    auto& record = *(RecordType*)decl[type];
                    if(index < record.cons.size()) {
                        name(record.cons.get(decl, index).name);
                        return;
                    }
                }

                writeUInt(writer, index);
                return;
            }
            // Which operation, from op.def's atomic block - the base mnemonic is `atomic`, and a
            // dump that printed it would say the same word for eleven different instructions.
            case Node::Atomic:
                writer.writeString(atomicOpName(((NodeAtomic&)value).op));
                return;
            // The tail-read guarantee, spent. Printed because a golden about `vectorPast` is about
            // exactly this bit - the place, the mode and the type are `vectorAt`'s.
            case Node::Read:
                writer.writeString(((NodeRead&)value).overread ? "read.past"_v : "read"_v);
                return;
            // The two bits the loan checker's element exemption reads, because a dump that hid them
            // would show two `swap`s a reader could not tell apart from two ordinary borrows.
            case Node::Address:
                writer.writeString(((NodeAddress&)value).exchange ? "address.exchange"_v : "address"_v);
                return;
            case Node::Call: {
                auto& call = (NodeCall&)value;
                writer.writeString(call.subscript ? "call.subscript"_v
                                   : call.derivation ? "call.derivation"_v : "call"_v);
                return;
            }
            default:
                writer.writeString(nodeMnemonic(value.kind));
                return;
        }
    }

    void node(Body& body, Node& value) {
        writer.writeString("  "_v);

        if(hasResult(value.kind)) {
            writer.writeString("%"_v);
            writeUInt(writer, value.id.index);
            writer.writeString(": "_v);
            printOccurrence(writer, context, decl, valueOccurrence(value));
            writer.writeString(" = "_v);
        }

        mnemonic(value);

        // What the node names statically rather than through an operand: a constant's literal and a
        // call's callee. Written out per kind rather than derived, because an operand list is
        // generic and a static field is not - a kind that has one says so itself.
        switch(value.kind) {
            case Node::Const:
                writer.writeString(" "_v);
                printConstant(writer, context, decl, body.constant(mir, ((NodeConst&)value).constant));
                break;
            case Node::GenValue:
                // What the body says the number is - a parameter's name, or the expression a natural
                // lane count stands for. Which number it turns out to be is the specialization's,
                // and a dump of the generic body has no such answer.
                writer.writeString(" "_v);
                printType(writer, context, decl, ((NodeGenValue&)value).variable);
                break;
            case Node::FunValue:
                // The declaration a value of it points at. What stands behind it in the emitted
                // program is a thunk, which is a lowering answer and not something this tier has.
                writer.writeString(" "_v);
                name(decl[((NodeFunValue&)value).callee]->symbol);
                break;
            case Node::Call: {
                auto& call = (NodeCall&)value;

                // A call through a value names no declaration - the callee is the first operand,
                // which the operand walk below prints. Printing nothing here is what makes the two
                // forms distinguishable in a golden without a second word for the common one.
                if(call.callsValue()) break;

                callee(call.callee, call.typeArguments);
                break;
            }
            case Node::LensCall:
            case Node::LensTry: {
                auto& call = (NodeLensCall&)value;
                callee(call.callee, call.typeArguments);
                break;
            }
            case Node::Native:
                // Which operation, before the operands the shared walk prints - the column is the
                // only thing about one of these this tier knows.
                writer.writeString(" "_v);
                writer.writeString(nativeOpName(((NodeNative&)value).op));
                break;
            case Node::VecLane:
            case Node::VecWithLane:
                // The lane, which is a field rather than an operand - see inst.def, where the reason
                // a lane index is part of the instruction is written.
                writer.writeString(" #"_v);
                writeUInt(writer, ((NodeVecLane&)value).lane);
                break;
            case Node::VecShuffle: {
                // The pattern, one entry per lane of the result. Printed before the operands, which
                // is where every other static field of a node goes.
                writer.writeString(" ["_v);
                auto first = true;

                for(auto lane: ((NodeVecShuffle&)value).pattern.contents(mir)) {
                    if(!first) writer.writeString(", "_v);
                    first = false;
                    writeUInt(writer, lane);
                }

                writer.writeString("]"_v);
                break;
            }
            case Node::VecReduce:
                // Which fold - the mnemonic says `vec.reduce` for all seven, which a reader has no
                // use for.
                writer.writeString(" "_v);
                writer.writeString(reduceOpName(((NodeVecReduce&)value).reduce));
                break;
            case Node::Atomic: {
                /*
                 * The ordering, and the failure order and the strength where they say anything.
                 *
                 * The mnemonic already carries which operation it is, so what is left is what a
                 * reader of a concurrent program actually checks: which edge this establishes. The
                 * failure order is printed only for a compare-exchange, where it is a second answer
                 * rather than a copy of the first, and `weak` only where it is true - a golden that
                 * spelled "strong" at every one of them would bury the one line that is news.
                 */
                auto& atomic = (NodeAtomic&)value;
                writer.writeString(" "_v);
                writer.writeString(atomicOrderName(atomic.order));

                if(atomic.op == AtomicOp::Compare) {
                    writer.writeString("/"_v);
                    writer.writeString(atomicOrderName(atomic.failure));
                    if(atomic.weak) writer.writeString(" weak"_v);
                }

                break;
            }
            case Node::Switch: {
                // The case values, in edge order - the generic edge loop below prints the targets,
                // and a dump that showed only those could not say which number reaches which.
                writer.writeString(" ["_v);
                auto firstCase = true;

                for(Size i = 0; i < ((NodeSwitch&)value).cases.size(); i++) {
                    if(!firstCase) writer.writeString(", "_v);
                    firstCase = false;

                    writeInt(writer, ((NodeSwitch&)value).cases.pointer(mir)[i].value);
                }

                writer.writeString("]"_v);
                break;
            }
            case Node::Map: {
                /*
                 * Every entry, and the fallback where the domain is open.
                 *
                 * All of them rather than a count, on `VecShuffle`'s reasoning: the table *is* the
                 * node, and the recognition rule bounds how many there can be. A golden about a
                 * `match` over constants is a golden about which constant each arm answers.
                 */
                auto& map = (NodeMap&)value;
                writer.writeString(" ["_v);
                auto first = true;

                for(auto entry: map.entries.contents(mir)) {
                    if(!first) writer.writeString(", "_v);
                    first = false;

                    writeInt(writer, entry.selector);
                    writer.writeString(" -> "_v);
                    printConstant(writer, context, decl, entry.value);
                }

                writer.writeString("]"_v);

                if(!map.closed) {
                    writer.writeString(" else "_v);
                    printConstant(writer, context, decl, map.fallback);
                }

                break;
            }
            case Node::Metric: {
                // The measurement and the type, which are the whole of the node - it has no operands
                // at all, so printing nothing would leave a reader unable to tell two apart.
                auto& metric = (NodeMetric&)value;
                writer.writeString(" "_v);
                writer.writeString(typeMetricName(metric.metric));
                writer.writeString(" "_v);
                printType(writer, context, decl, metric.of);
                break;
            }
            case Node::RunSlots: {
                // The element and how many of them, for `Metric`'s reason: no operands, so a dump
                // that printed neither would show every run request as the same instruction.
                auto& slots = (NodeRunSlots&)value;
                writer.writeString(" "_v);
                printType(writer, context, decl, slots.element);
                writer.writeString(" *"_v);
                writeUInt(writer, slots.capacity);
                break;
            }
            default:
                break;
        }

        forEachOperand(mir, value, [&](Operand& item, OperandRole) {
            writer.writeString(" "_v);
            operand(body, item);
        });

        loans(body, value);

        forEachEdge(mir, value, [&](Edge& edge) {
            writer.writeString(" -> bb"_v);
            writeUInt(writer, mir[edge.target]->index.index);

            if(edge.arguments.size()) {
                writer.writeString("("_v);
                auto first = true;
                for(auto item: edge.arguments.contents(mir)) {
                    if(!first) writer.writeString(", "_v);
                    first = false;
                    operand(body, item);
                }
                writer.writeString(")"_v);
            }
        });

        writer.writeString("\n"_v);
    }

    /*
     * The loan a read took, and what the value it produced is rooted in - §6.4 and §5.11.
     *
     * Two different facts and both are printed, because a fixture about borrowing is about the
     * relation between them. `loanN` is what this instruction created and is what the origin sets
     * below name; `roots{...}` is the environment's answer for the value, and it is printed only
     * where it says something the loan does not - a read whose only root is its own loan says
     * nothing, and printing it on every borrow in the program would bury the reborrows and the call
     * results that do.
     */
    void loans(Body& body, Node& value) {
        auto own = LoanId();
        if(value.kind == Node::Read) {
            own = ((NodeRead&)value).loan;
            if(own.valid()) {
                writer.writeString(" loan"_v);
                writeUInt(writer, own.index);
            }
        }

        if(!body.elaboration || !hasResult(value.kind)) return;

        /*
         * One group per origin *position*, and the position is in the name.
         *
         * `roots{..}` is what the value itself refers to and `roots.<i>{..}` is what its `i`th
         * interior slot does - so a record built from two borrowed guards dumps two groups, and a
         * golden that showed one union could not tell that apart from the collapsed answer it used
         * to be. Empty positions print nothing, which keeps every body that borrows once unchanged.
         */
        auto& elaboration = *mir[body.elaboration];
        auto positions = elaboration.originPositionCount(mir, value.id);

        for(U32 p = 0; p < positions; p++) {
            auto first = true;

            for(U32 l = 0; l < elaboration.loanCount; l++) {
                auto id = LoanId(l);
                if(!elaboration.rootedInPosition(mir, value.id, p, id)) continue;

                // The read's own loan is already printed as `loanN` above, and only ever roots the
                // value itself.
                if(p == 0 && id == own) continue;

                if(first) {
                    writer.writeString(" roots"_v);

                    if(p) {
                        writer.writeString("."_v);
                        writeUInt(writer, p - 1);
                    }

                    writer.writeString("{"_v);
                } else {
                    writer.writeString(", "_v);
                }

                writeUInt(writer, l);
                first = false;
            }

            if(!first) writer.writeString("}"_v);
        }
    }

    /*
     * What the ownership analysis placed at one position - §6.7, and the whole subject of milestone
     * four.
     *
     * A drop is not a node, deliberately: it is a side table the SSA conversion reads (§9.2), so a
     * body's node list says nothing about where its teardowns go. That makes this the only place the
     * analysis' own answer can be read at the level it was decided at - the SSA dump shows the
     * `drop` instructions the conversion emitted from it, which is one translation later and cannot
     * distinguish a placement the analysis got wrong from a conversion that misread it.
     *
     * A linear scan per position rather than a cursor. The step lists are sorted and a cursor would
     * be faster; a printer is not on any path where that has ever mattered, and a cursor here would
     * be a second reading of the sort order that `convert.cpp` already depends on.
     */
    void elaborated(Body& body, U32 index, U32 position, bool before) {
        if(!body.elaboration) return;
        auto& elaboration = *mir[body.elaboration];

        if(before) {
            for(auto& step: elaboration.drops.contents(mir)) {
                if(step.block != index || step.position != position) continue;

                // `drop.maybe` is a path that was owned on some incoming paths and not on others, and
                // is what the conversion emits a flag test for. Nothing in this language subset
                // reaches it - see the README - so a golden containing one is news.
                writer.writeString(step.conditional ? "  drop.maybe "_v : "  drop "_v);
                writer.writeString("("_v);

                // The step's own place, which a path-backed one and an overwrite of a place with no
                // path both carry - see `mir::DropStep`.
                place(body, *body.place(mir, step.place));
                writer.writeString(")\n"_v);
            }

            return;
        }

        // The flag writes, which record what the node just did - hence after it. `FlagStep::kEntry`
        // is not a position in any block and is printed with the locals instead, by `entryFlags`.
        for(auto& step: elaboration.flags.contents(mir)) {
            if(step.block != index || step.position != position) continue;

            writer.writeString("  flag ("_v);
            place(body, *body.place(mir, elaboration.path(mir, step.path)->place));
            writer.writeString(step.value ? ") = held\n"_v : ") = gone\n"_v);
        }
    }

    // The flags a function starts with: a parameter arrives owned and a local does not. Beside the
    // locals rather than in a block, because that is what they are - the state the frame begins in.
    void entryFlags(Body& body) {
        if(!body.elaboration) return;

        auto& elaboration = *mir[body.elaboration];
        for(auto& step: elaboration.flags.contents(mir)) {
            if(step.position != FlagStep::kEntry) continue;

            writer.writeString("  flag ("_v);
            place(body, *body.place(mir, elaboration.path(mir, step.path)->place));
            writer.writeString(step.value ? ") = held\n"_v : ") = gone\n"_v);
        }
    }

    /*
     * What a call names statically - the declaration and the arguments it is reached at.
     *
     * One function for `call` and `lens.call`, because a golden could not tell one call site from
     * another without the type arguments: the node names the *generic*, so `id(7)` and `id(True)`
     * are the same line, and which specialization each reaches is exactly what a fixture about
     * specialization is asserting.
     */
    void callee(DeclPtr<Function> function, MirList<DeclPtr<Type>>& typeArguments) {
        writer.writeString(" "_v);
        name(decl[function]->symbol);

        if(!typeArguments.size()) return;

        writer.writeString("("_v);
        auto first = true;

        for(auto argument: typeArguments.contents(mir)) {
            if(!first) writer.writeString(", "_v);
            first = false;
            printType(writer, context, decl, argument);
        }

        writer.writeString(")"_v);
    }

    /*
     * Which continuation region a block is in, printed only where it is not the body's own.
     *
     * The whole point of the structure is that the CFG cannot say it - a diamond is a diamond
     * whatever its arms mean - so a dump that showed only the graph would show a lens call as a
     * chain of jumps. What is printed is what the region *states* and nothing derived: its kind,
     * what a hand-over declared it hands over, and where it rejoins.
     */
    void region(Body& body, Block& value) {
        auto region = body.region(mir, value.region);
        if(!region || region->kind == Continuation::Body) return;

        writer.writeString(" handover"_v);

        if(region->kind == Continuation::Handover) {
            writer.writeString("("_v);
            printOccurrence(writer, context, decl, region->handover);
            writer.writeString(")"_v);
        }

        // How many times it runs per entry to its parent, where that is not once. The one value that
        // changes what an analysis walks rather than only what it may assume, so a golden that did
        // not show it could not tell a lens call from a loop.
        if(region->multiplicity == Continuation::AtMostOnce) writer.writeString(" at.most.once"_v);
        if(region->multiplicity == Continuation::Many) writer.writeString(" many"_v);

        // Once per region rather than once per block: the entry is where the region is stated, and
        // repeating it on every block of a long frame would bury what is different about them.
        if(region->entry != value.index || !region->destination.valid()) return;

        writer.writeString(" -> bb"_v);
        writeUInt(writer, region->destination.index);
    }

    void block(Body& body, Block& value) {
        writer.writeString("bb"_v);
        writeUInt(writer, value.index.index);

        if(value.arguments.size()) {
            writer.writeString("("_v);
            auto first = true;
            for(auto argument: value.arguments.contents(mir)) {
                if(!first) writer.writeString(", "_v);
                first = false;

                auto node = mir[argument];
                writer.writeString("%"_v);
                writeUInt(writer, node->id.index);
                writer.writeString(": "_v);
                printOccurrence(writer, context, decl, valueOccurrence(*node));
            }
            writer.writeString(")"_v);
        }

        region(body, value);
        writer.writeString(":\n"_v);

        auto position = 0u;
        for(auto nodePointer: value.nodes.contents(mir)) {
            elaborated(body, value.index.index, position, true);
            node(body, *mir[nodePointer]);
            elaborated(body, value.index.index, position, false);
            position++;
        }
    }

    void function(Function& value) {
        if(value.lens) writer.writeString("lens "_v);
        writer.writeString("fn "_v);

        // The **symbol** and not the source name, because a module can hold two functions of one
        // name at two arities and a dump that printed the name would show two identical headers.
        // What the symbol adds is exactly a suffix where that happened and nothing anywhere else.
        name(value.symbol);
        writer.writeString("("_v);

        auto first = true;
        for(auto argumentPointer: value.args.contents(decl)) {
            auto argument = decl[argumentPointer];
            if(!first) writer.writeString(", "_v);
            first = false;

            // The marker, which the occurrence beside it does not say: `declareDeferred` rewrote
            // the position into `() -> T`, and a `() -> T` a *caller* writes is an ordinary
            // parameter holding a function. What the two spellings differ in is who builds the
            // thunk, which is exactly what a dump has to show.
            if(argument->lazy) writer.writeString("@lazy "_v);

            // `@caller`, for the same reason: it says the position may be left out, which is a fact
            // about every call rather than a note on the declaration. Which fill it is is what the
            // spelling says - a source fill names the parameter it is the text of.
            if(argument->caller.fill == CallerFill::Source) {
                writer.writeString("@caller(source: "_v);
                name(decl[value.args.get(decl, argument->caller.source)]->name);
                writer.writeString(") "_v);
            } else if(argument->caller.fill == CallerFill::Site) {
                writer.writeString("@caller "_v);
            }

            name(argument->name);
            writer.writeString(": "_v);
            printOccurrence(writer, context, decl, argument->occurrence);

            // The default, which is part of what calls the signature accepts and not a note about
            // it: `writtenArity` alone no longer says which counts reach this declaration. On a
            // deferred position it is the constant of what the thunk *promises*, which is why it
            // reads as a `T` beside a `() -> T` - see `Arg::defThunk`.
            if(argument->def.isSet()) {
                writer.writeString(" = "_v);
                printConstant(writer, context, decl, argument->def);
            }
        }

        writer.writeString(") -> "_v);
        printOccurrence(writer, context, decl, value.result);

        // Only where it is *known*: an unknown suspension answer and a known "no" are different
        // facts, and a dump that printed nothing for both would be hiding the one that matters.
        if(value.summary.suspension().isKnown() && value.summary.suspension().value) {
            writer.writeString(" suspends"_v);
        }

        /*
         * A specialization has no body of its own and is not missing one - `specialize.h`.
         *
         * Its `mir` handle is the generic's, so printing "the body" here would print the generic's
         * nodes a second time under substituted parameter names, which is a dump claiming there are
         * two bodies where the whole design says there is one. What a reader wants instead is the
         * pair: which generic, and at what.
         *
         * That makes the golden say the thing worth asserting. The specializations a program built
         * are listed with their argument lists, the generic body is printed once above them, and a
         * fixture about drop placement can read the drops on that one body and know they are the
         * drops of every specialization - because there is nowhere else for one to be.
         */
        if(value.specializes) {
            writer.writeString(" = <"_v);
            name(decl[value.specializes]->name);
            writer.writeString(" at "_v);

            auto first = true;
            for(auto argument: value.typeArguments.contents(decl)) {
                if(!first) writer.writeString(", "_v);
                first = false;
                printType(writer, context, decl, argument);
            }

            writer.writeString(">\n"_v);
            return;
        }

        // A lens has no body of its own and is not missing one: it is resolved into every call
        // site, so what a dump can show about it is its signature and where its code went.
        if(!value.mir) {
            writer.writeString(" = <no body>\n"_v);
            return;
        }

        writer.writeString(" {\n"_v);

        /*
         * The canonical numbering, one line per slot - `GenSchema`.
         *
         * Printed because it is the only thing a fixture can assert about the one property nothing
         * at run time can check for itself: that a caller's slot 3 and a callee's slot 3 are the
         * same slot. The numbers are what emitted code will load, so a golden that showed the
         * variables and not their positions would be asserting the easy half.
         *
         * Ahead of the locals for the reason the locals are ahead of the blocks: it is what the body
         * is written against, and a reader looking for what `a` is should not have to find its first
         * use to learn that it is this declaration's first parameter.
         */
        if(isGeneric(decl, value.gen)) {
            /*
             * Which form this body is compiled in, ahead of the numbering it would read - `GenForm`.
             *
             * Printed because it is a decision and not a derivation: the measurement behind it is
             * over the finished body *after* the teardowns are placed, so nothing a reader of the
             * dump can see would let them recompute it. What a fixture asserts here is the policy,
             * which is the half of milestone 6 that has to be right before an ABI is built on it.
             */
            writer.writeString("  form "_v);
            writer.writeString(genFormName(value.form));
            writer.writeString("\n"_v);

            auto& schema = *decl[decl[value.gen]->schema];

            for(U32 i = 0; i < schema.slots.size(); i++) {
                auto slot = schema.slots.get(decl, i);
                writer.writeString("  slot #"_v);
                writeUInt(writer, i);
                writer.writeString(" "_v);
                writer.writeString(genSlotKindName(slot.kind));
                writer.writeString(" "_v);

                /*
                 * Each kind printed as what a *caller* would have to build to fill it, which is what
                 * a schema is for. Not one field printed five ways: a class witness is for a class
                 * applied to an argument list, a property witness is a field of a subject, and a
                 * function witness is a name at a signature - and a caller looking at a slot has to
                 * see the whole of what it is being asked for.
                 */
                if(slot.kind == GenSlotKind::Class) {
                    name(decl[slot.klass]->name);
                    writer.writeString("("_v);

                    for(U32 j = 0; j < slot.args.size(); j++) {
                        if(j) writer.writeString(", "_v);
                        printType(writer, context, decl, slot.args.get(decl, j));
                    }

                    writer.writeString(")"_v);
                } else if(slot.kind == GenSlotKind::Property) {
                    printType(writer, context, decl, slot.type);
                    writer.writeString("."_v);
                    name(slot.name);
                } else if(slot.kind == GenSlotKind::Function) {
                    name(slot.name);
                    writer.writeString(": ("_v);

                    for(U32 j = 0; j < slot.args.size(); j++) {
                        if(j) writer.writeString(", "_v);
                        printType(writer, context, decl, slot.args.get(decl, j));
                    }

                    writer.writeString(") -> "_v);
                    printType(writer, context, decl, slot.type);
                } else {
                    printType(writer, context, decl, slot.type);
                }

                writer.writeString("\n"_v);
            }
        }

        auto& body = *mir[value.mir];

        /*
         * The storage the function declares, before the blocks that use it.
         *
         * A local appears inside a place otherwise, where the path prints its name and nothing says
         * what is in it or whether it may be written - and those are the two facts a fixture about
         * initialization is asserting. One line each, ahead of the code, is where a reader looks.
         */
        auto declaredLocals = 0u;

        /*
         * How many locals share each object lineage - `Object`, printed only where it says something.
         *
         * One id per local on every line would bury the fixtures' subject under a number that is
         * different for every local in almost every body. What a reader cannot get from anywhere
         * else is which *names* turned out to be one object, so that is what is printed: a lineage
         * two or more locals are part of, which is exactly what a move between them produces.
         */
        SmallArray<U32, 16> lineageUsers;
        for(U32 i = 0; i < body.objects.size(); i++) lineageUsers.push(0);

        for(U32 i = 0; i < body.locals.size(); i++) {
            auto root = body.objectRoot(mir, body.local(mir, LocalId(i))->object);
            if(root.valid()) lineageUsers[root.index]++;
        }

        for(U32 i = 0; i < body.locals.size(); i++) {
            auto local = body.local(mir, LocalId(i));

            // A parameter is a local and is not listed here: the signature line above already says
            // its name, its occurrence and its position, and a second line would be the same
            // declaration printed twice. What this list is for is the storage a *body* introduced.
            if(local->isParameter()) continue;

            declaredLocals++;
            writer.writeString(local->writable ? "  local & "_v : "  local "_v);
            localName(*local);
            writer.writeString(": "_v);
            printOccurrence(writer, context, decl, local->occurrence);

            // Printed only where it is not the frame, so that the goldens of a milestone with no
            // escaping local stay about ownership rather than about placement.
            auto storage = storageOf(mir, body, *local);
            if(storage != StorageClass::Frame) {
                writer.writeString(" "_v);
                writer.writeString(storageClassName(storage));
            }

            auto root = body.objectRoot(mir, local->object);
            if(root.valid() && lineageUsers[root.index] > 1) {
                writer.writeString(" object"_v);
                writeUInt(writer, root.index);
            }

            writer.writeString("\n"_v);
        }

        entryFlags(body);
        if(declaredLocals) writer.writeString("\n"_v);

        for(U32 i = 0; i < body.blocks.size(); i++) {
            if(i) writer.writeString("\n"_v);
            block(body, *body.block(mir, BlockId(i)));
        }

        writer.writeString("}\n"_v);
    }

    /*
     * A `data` declaration: its fields, and what it owes.
     *
     * The teardown line is printed only where there is one, on the same terms as a contract: a
     * fixture about drops is asserting that a record acquired a *derived* teardown from a member, or
     * an *authored* one from an instance, and printing "none" for every other record would bury the
     * two lines that say so.
     */
    void record(RecordType& value) {
        writer.writeString("data "_v);
        name(value.name);

        /*
         * The head's parameters, or an instantiation's arguments.
         *
         * One line for both, because the two are the same list read from opposite ends and a dump
         * that printed only one of them could not show the pair the whole feature is about: `data
         * Boxed(a)` beside `data Boxed(I32)` is one declaration and one instantiation of it, and
         * which fields the second has is exactly what substitution decided.
         */
        auto arguments = value.gen ? decl[value.gen]->variables : value.arguments;
        if(arguments.size()) {
            writer.writeString("("_v);
            auto firstArgument = true;

            for(auto argument: arguments.contents(decl)) {
                if(!firstArgument) writer.writeString(", "_v);
                firstArgument = false;
                printType(writer, context, decl, argument);
            }

            writer.writeString(")"_v);
        }

        auto fieldRange = [&](U16 from, U16 to) {
            writer.writeString(" {"_v);

            auto first = true;
            for(U16 i = from; i < to; i++) {
                auto field = value.fields.get(decl, i);
                if(!first) writer.writeString(", "_v);
                first = false;

                name(field.name);
                writer.writeString(": "_v);

                // The declaration's contract, spelled the way the author wrote it: `'T` and `&'T`
                // say the record refers to the field rather than owning it, which is the one thing
                // about a field that changes what the record *is*.
                auto contract = field.occurrence.contract;
                if(contract.origin() == Origin::Slot) {
                    if(contract.access() == Access::Unique) writer.writeString("&"_v);
                    writeUInt(writer, contract.slot());
                    writer.writeString("'"_v);
                }

                /*
                 * `@box`, as the author wrote it - and as the compiler wrote it, which is the same
                 * word for the same thing. `breakLayoutCycles` is the second writer, and its cut is
                 * exactly what a reader of a dump most wants to see: whether `data List = Nil | Cons
                 * {tail: List}` came out with the pointer where they expected it.
                 *
                 * The flag rather than `indirectField`, because a dump is about what the declaration
                 * says. A `@box ()` field is written and is not an indirection, and printing it as
                 * an ordinary field would make the two spellings one line.
                 */
                if(field.boxed) writer.writeString("@box "_v);

                printType(writer, context, decl, field.occurrence.type);

                // The default, for the reason a parameter's is printed: it is what a construction
                // that omits the field builds, so a dump without it describes a different type.
                if(field.def.isSet()) {
                    writer.writeString(" = "_v);
                    printConstant(writer, context, decl, field.def);
                }
            }

            writer.writeString("}"_v);
        };

        /*
         * One constructor is printed as its fields and several as the source spelling.
         *
         * `data Pair {x: I32}` against `data Outcome(a, e) = Proceed {Proceed: a} | Exit {Exit: e}`,
         * which is what the declaration means rather than what it was typed as: a one-payload
         * constructor's field carries the constructor's own name, and a dump that hid that would hide
         * which field index a projection is about.
         *
         * The braced form is the case where the sole constructor is *called* after the type, and
         * only then may the name be left out: `data Pair = Other {x: I32}` declares one constructor
         * whose name is the only way to build the type, so a dump printing it as `data Pair {x: I32}`
         * would be naming something no program may write.
         */
        auto sole = value.layout == RecordLayout::Single && value.cons.size() == 1
                        ? Just(value.cons.get(decl, 0)) : Nothing();

        if(value.layout == RecordLayout::Single && (!sole || sole.unwrap().name == value.name)) {
            fieldRange(0, U16(value.fields.size()));
        } else {
            writer.writeString(" ="_v);

            auto firstCon = true;
            for(auto constructor: value.cons.contents(decl)) {
                writer.writeString(firstCon ? " "_v : " | "_v);
                firstCon = false;

                name(constructor.name);

                // `@value(n)`, and only where one was written - see `RecordCon::pinned`. What a
                // reader needs from a dump is which numbers the *declaration* fixed, since an
                // unpinned constructor's number is its position and the list already says that.
                if(constructor.pinned) {
                    writer.writeString(" = "_v);
                    writeInt(writer, constructor.value);
                }

                if(constructor.count) {
                    fieldRange(constructor.first, U16(constructor.first + constructor.count));
                }
            }
        }

        /*
         * Which declaration an instantiation came from.
         *
         * Without it two lines can be spelled identically and mean different types: `data Boxed(a)`
         * is the declaration, and `data Boxed(a)` is also `Boxed` applied to some *function's* `a`,
         * which is what a generic body that holds one produces. They differ in what substituting
         * does to them, which is exactly the difference a golden about specialization is about.
         */
        if(value.instanceOf) {
            writer.writeString(" of "_v);
            name(decl[value.instanceOf]->name);
        }

        // The interned shape, where there is one. Printed as its identity and its positions, so that
        // a fixture can assert that two records with the same interior structure are one shape and
        // that `Pair(a'T, U)` and `Pair(T, a'U)` are not - which is what interning exists for.
        if(!Shapes::isEmpty(value.schema)) {
            auto schema = decl[value.schema];
            writer.writeString(" shape"_v);
            writeUInt(writer, schema->id);
        }

        auto teardown = [&](StringView what, TeardownKind kind) {
            if(kind == TeardownKind::None) return;

            writer.writeString(" "_v);
            writer.writeString(what);
            writer.writeString(kind == TeardownKind::Authored ? " authored"_v : " derived"_v);
        };

        teardown("drop"_v, value.ownership.drop);
        teardown("reclaim"_v, value.ownership.reclaim);

        /*
         * The folded answer rather than the written attribute, and the difference is the whole of
         * the containment rule: a record that merely *holds* a linear member prints this without
         * having said it, and one whose authored `Drop` answered for its members does not print it
         * even where a member's would have propagated. A fixture asserting where the obligation
         * stops has to read the fold, since the attribute is what it was asked and not what it
         * concluded.
         */
        if(value.ownership.explicitDrop) writer.writeString(" explicit"_v);

        writer.writeString("\n"_v);
    }

    void global(Global& value) {
        writer.writeString(value.writable ? "let & "_v : "let "_v);
        name(value.name);
        writer.writeString(": "_v);
        printOccurrence(writer, context, decl, value.occurrence);

        if(value.initial.isSet()) {
            writer.writeString(" = "_v);
            printConstant(writer, context, decl, value.initial);
        }

        writer.writeString("\n"_v);
    }

    /*
     * `class (Eq(a)) Ord(a): fn compare, fn max` - the head, the superclasses and the member names.
     *
     * The names and not the signatures, for the reason the members are not listed as declarations:
     * each member's signature is printed where the member is, and what this line adds is *which*
     * declarations belong to the class and in what order - which is the order an instance's member
     * list is in and the order a witness would be built in.
     */
    // `Eq(a)`, `Contiguous(c, a)` - one requirement as its class applied to its arguments. Shared
    // by the superclass list and an instance head's context, which are the same structure.
    void applied(GenRequirement requirement) {
        name(decl[requirement.klass]->name);
        writer.writeString("("_v);

        for(U32 i = 0; i < requirement.args.size(); i++) {
            if(i) writer.writeString(", "_v);
            printType(writer, context, decl, requirement.args.get(decl, i));
        }

        writer.writeString(")"_v);
    }

    void klass(Class& value) {
        writer.writeString("class "_v);

        if(value.superclasses.size()) {
            writer.writeString("("_v);
            for(U32 i = 0; i < value.superclasses.size(); i++) {
                if(i) writer.writeString(", "_v);
                applied(value.superclasses.get(decl, i));
            }

            writer.writeString(") "_v);
        }

        name(value.name);
        writer.writeString("("_v);

        auto arity = U32(decl[value.gen]->variables.size());
        for(U32 i = 0; i < arity; i++) {
            // The arrow, where the parameters after it are the ones the ones before decide. Printed
            // as it was written rather than as an index, because what a reader is checking is which
            // side of it a parameter is on.
            if(i) writer.writeString(i == value.determined ? " -> "_v : ", "_v);
            printType(writer, context, decl, decl[value.gen]->variables.get(decl, i));
        }

        writer.writeString(")"_v);

        for(U32 i = 0; i < value.members.size(); i++) {
            writer.writeString(i ? ", "_v : ": "_v);
            name(decl[value.members.get(decl, i)]->name);
        }

        writer.writeString("\n"_v);
    }

    /*
     * `instance Describe(Metres): Metres.size, Describe.twice(Metres)` - which function each member
     * resolved to.
     *
     * The *resolved* member and not the written one, which is what a reader of a dispatch is looking
     * for: an instance that left a member out gets the class's default specialized at its type, and
     * a listing that only showed what the source wrote would leave a call with no visible target.
     * Null where the default has not been reached - nothing calls it, so nothing made it.
     */
    void instance(Instance& value) {
        auto& declared = *decl[value.klass];

        writer.writeString("instance "_v);

        // The head's own context, which is what a parametric instance is selected through.
        if(value.gen && decl[value.gen]->classes.size()) {
            writer.writeString("("_v);
            for(U32 i = 0; i < decl[value.gen]->classes.size(); i++) {
                if(i) writer.writeString(", "_v);
                applied(decl[value.gen]->classes.get(decl, i));
            }

            writer.writeString(") "_v);
        }

        name(declared.name);
        writer.writeString("("_v);

        for(U32 i = 0; i < value.args.size(); i++) {
            if(i) writer.writeString(", "_v);
            printType(writer, context, decl, value.args.get(decl, i));
        }

        writer.writeString(")"_v);

        for(U32 i = 0; i < value.members.size(); i++) {
            writer.writeString(i ? ", "_v : ": "_v);

            auto member = value.members.get(decl, i);
            if(member) { name(decl[member]->name); continue; }

            /*
             * A member the instance inherits, named as the class's own declaration of it.
             *
             * What was printed here was the *specialization* the default had been turned into, which
             * stopped being one answer when a head became a pattern: one instance serves every
             * argument list its head matches, so there is a specialization per list and no single
             * one for the declaration to name. `Class.member` is the fact that survives - which
             * member this instance supplies and which it inherits - and the specializations
             * themselves are printed where every other function is.
             */
            writer.writeString(stringView(context.findName(declared.name)));
            writer.writeString("."_v);
            name(decl[declared.members.get(decl, i)]->name);
        }

        if(value.local) writer.writeString(" local"_v);
        writer.writeString("\n"_v);
    }

    void run() {
        for(auto modulePointer: program.modules) {
            auto module = decl[modulePointer];

            /*
             * The program's own modules, and not the library's.
             *
             * The same filter the class listing below already applies for the same stated reason -
             * what a fixture wants to see is what its own source said. Core arrives in every
             * compilation through the implicit import (`linkImports`), so without this every dump in
             * the corpus would open with the whole of `lib/Core` and would churn whenever the
             * library did. A program *compiling* the library is not the library's consumer and is
             * not filtered: its root is one of these modules, and `fromLibrary` is false there.
             */
            if(module->fromLibrary) continue;

            writer.writeString("module "_v);
            name(module->name);
            writer.writeString("\n"_v);

            for(auto recordPointer: module->records.contents(decl)) {
                auto& value = *decl[recordPointer];

                /*
                 * The declarations, and the instantiations a value of the program actually has.
                 *
                 * A generic body names `Boxed(a)` for its *own* `a`, so a program with three
                 * generic functions over `Boxed` has three instantiations that are different types
                 * and print identically - a variable belongs to the declaration that bound it, and
                 * a name alone cannot say which. What they contribute to a dump is nothing a reader
                 * can use: their fields are the declaration's with a renaming, and the occurrences
                 * inside each body already show them at every position that has one.
                 *
                 * So what is listed is the declaration - `data Boxed(a)`, once - and every
                 * instantiation something can hold a value of, which is what a layout, an ownership
                 * answer and a teardown are questions about.
                 */
                if(value.instanceOf && !isConcreteType(decl, asBase<Type>(recordPointer))) continue;

                writer.writeString("\n"_v);
                record(value);
            }

            /*
             * The classes this file declared, and the instances of them.
             *
             * This file's, which is the filter: `Program::classes` is program-wide, so the three
             * the compiler reaches for would otherwise appear at the top of every dump in the
             * corpus saying what the reader already knows. What a fixture wants to see is what its
             * own source said.
             *
             * The members are not listed here. Each one is a `fn` in the module's own function list,
             * printed there with its signature and its slots, and a second copy under the class
             * would be the same declaration with two spellings to keep in step.
             */
            for(auto klassPointer: program.classes) {
                auto& value = *decl[klassPointer];
                if(value.module != modulePointer) continue;

                writer.writeString("\n"_v);
                klass(value);
            }

            for(auto instancePointer: module->instances.contents(decl)) {
                writer.writeString("\n"_v);
                instance(*decl[instancePointer]);
            }

            for(auto globalPointer: module->globals.contents(decl)) {
                writer.writeString("\n"_v);
                global(*decl[globalPointer]);
            }

            for(auto functionPointer: module->functions.contents(decl)) {
                writer.writeString("\n"_v);
                function(*decl[functionPointer]);
            }
        }

        if(program.entry) {
            writer.writeString("\nentry "_v);
            name(decl[program.entry]->name);
            writer.writeString("\n"_v);
        }
    }
};

} // namespace

void printProgram(Net::Writer& writer, Context& context, Program& program) {
    Printer printer { writer, context, program, *program.decl, *program.mir };
    printer.run();
}

void printFunction(Net::Writer& writer, Context& context, Program& program, Function& function) {
    Printer printer { writer, context, program, *program.decl, *program.mir };
    printer.function(function);
}


String appliedClassText(Context& context, DeclBase base, DeclPtr<Class> klass,
                        const DeclPtr<Type>* args, Size count) {
    if(!klass) return String("?");

    String text = Tritium::format("%@(", stringView(context.findName(base[klass]->name)));

    for(Size i = 0; i < count; i++) {
        // A position selection has not decided, which a functional dependency leaves open until the
        // head answers it. Printed as what it is, so a message about a failed selection says which
        // half of the application was the question.
        auto written = args[i] ? typeText(context, base, args[i]) : String("?");
        text = Tritium::format("%@%@%@", text, i ? ", "_v : ""_v, written);
    }

    return Tritium::format("%@)", text);
}

/*
 * A place as text - print.h says what it is for.
 *
 * The recursion is over the *parent chain*, so a projection reads left to right the way it is
 * written. A local with no name is `localN`, matching the declaration list in a dump, because a
 * reader who sees one in a message and one in the IR has to be able to pair them.
 */
String placeText(Context& context, DeclBase base, MirBase mir, Body& body, const Place& value) {
    String text = value.parent ? placeText(context, base, mir, body, *mir[value.parent]) : String();

    auto named = [&](StringId id) {
        auto view = stringView(context.findName(id));
        return Tritium::format("%@%@", text, view);
    };

    switch(value.kind) {
        case Place::Local: {
            auto local = body.local(mir, LocalId(value.payload));
            if(local->name) return named(local->name);
            return Tritium::format("%@local%@", text, U64(local->index.index));
        }
        case Place::Global:
            return Tritium::format("%@global %@", text,
                                   stringView(context.findName(base[DeclPtr<Global>(value.payload)]->name)));
        case Place::Loan:
            return Tritium::format("%@loan%@", text, U64(value.payload));
        case Place::Address:
            return Tritium::format("%@[%%%@]", text, U64(value.payload));

        /*
         * The field's *name*, which is what the author wrote. An index is what the representation
         * stores and is what a golden would have to be re-read after any reordering of a
         * declaration; the name is stable and is the thing a message and a fixture are both about.
         *
         * An **anonymous** field has no name to be stable, so its position is what it is called -
         * `t.0`, which is also the only spelling the source has for it. The two cases are the same
         * split `positionalField` makes and are the whole of how a field is named.
         */
        case Place::Field: {
            auto record = (RecordType*)base[mir[value.parent]->occurrence.type];
            auto field = record->field(base, U16(value.payload));

            if(field.name == StringId()) return Tritium::format("%@.%@", text, U64(value.payload));
            return Tritium::format("%@.%@", text, stringView(context.findName(field.name)));
        }
        /*
         * The requirement's field name, which is again what the author wrote - and here it is the
         * *only* thing there is: the parent's type is a variable, so there is no field list to index
         * into and no index to print. The payload is the requirement's position, which the dump
         * shows above the body as a slot.
         */
        case Place::Property: {
            if(!body.gen || value.payload >= base[body.gen]->properties.size()) {
                return Tritium::format("%@.?%@", text, U64(value.payload));
            }

            auto property = base[body.gen]->properties.get(base, U32(value.payload));
            return Tritium::format("%@.%@", text, stringView(context.findName(property.field)));
        }
        /*
         * `x.#` - the discriminant, which is the one step with no name to print because the source
         * cannot write it. The sigil is what a fixture reads to see that a match tested the tag
         * rather than a field.
         */
        case Place::Discriminant:
            return Tritium::format("%@.#", text);
        /*
         * `xs[2]` where the index is a literal, and `xs[...]` where it is not.
         *
         * The **number** where there is one, because a message about two overlapping element borrows
         * is unreadable without it: "`xs[%%6]` conflicts with `xs[%%4]`" names two nodes a reader
         * cannot see, and the author wrote two numbers. Where the index is computed there is nothing
         * to name - a node ID is the dump's vocabulary and not a program's - so the ellipsis says
         * "an element" and the caret says which one.
         */
        case Place::Index: {
            auto index = body.node(mir, NodeId(value.payload));

            if(index && index->kind == Node::Const) {
                auto held = body.constant(mir, ((NodeConst*)index)->constant);
                if(held.kind == Constant::Int) {
                    return Tritium::format("%@[%@]", text, held.asInt());
                }
            }

            return Tritium::format("%@[...]", text);
        }
        /*
         * Nothing of its own, deliberately.
         *
         * The one thing that makes a `Deref` is reaching an external record field, and the step is
         * an implementation of what the author wrote rather than something they wrote: `r.held` is
         * the object, and `r.held.*` would be the compiler describing its own indirection in a
         * message about the program. A raw-pointer dereference, when there is one, is a source
         * spelling and gets one here.
         */
        case Place::Deref:
            return text;
        case Place::KindCount:
            break;
    }

    return text;
}

} // namespace mir
