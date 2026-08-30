#include "print.h"

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
        case Type::Bool:
            writer.writeString("Bool"_v);
            break;
        case Type::Int: {
            auto& integer = (IntType&)value;
            writer.writeString(integer.isSigned ? "I"_v : "U"_v);
            writeUInt(writer, integer.bits);
            break;
        }
        case Type::Float:
            writer.writeString("F"_v);
            writeUInt(writer, ((FloatType&)value).bits);
            break;
        case Type::Pointer:
            writer.writeString("%"_v);
            printType(writer, context, base, ((PointerType&)value).to);
            break;
        case Type::Record:
            // The declared name alone. A nominal type is its declaration, and the fields are printed
            // once where that declaration is - see `Printer::record`.
            writer.writeString(stringView(context.findName(((RecordType&)value).name)));
            break;
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
void printOccurrence(Net::Writer& writer, Context& context, DeclBase base, const Occurrence& occurrence) {
    printType(writer, context, base, occurrence.type);

    auto contract = occurrence.contract;
    auto isDefault = contract == ownedContract() && Shapes::isEmpty(occurrence.shape);
    if(isDefault) return;

    writer.writeString(" ["_v);
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
                writeUInt(writer, step);
                first = false;
            }

            writer.writeString("="_v);
            writer.writeString(accessName(slot->capability));
            writeUInt(writer, slot->origin);
        }
    }

    writer.writeString("]"_v);
}

void printConstant(Net::Writer& writer, DeclBase base, const Constant& value) {
    switch(value.kind) {
        case Constant::None:
            writer.writeString("<none>"_v);
            break;
        case Constant::Unit:
            writer.writeString("{}"_v);
            break;
        case Constant::Bool:
            writer.writeString(value.asBool() ? "True"_v : "False"_v);
            break;
        case Constant::Int: {
            auto type = (IntType*)base[value.type];
            if(type->kind == Type::Int && type->isSigned) writeInt(writer, I64(value.asInt()));
            else writeUInt(writer, value.asInt());
            break;
        }
        case Constant::Float:
            writeFloat(writer, value.asFloat());
            break;
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
                printConstant(writer, decl, body.constant(mir, value.constantId()));
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
                printConstant(writer, decl, body.constant(mir, ((NodeConst&)value).constant));
                break;
            case Node::Call:
                writer.writeString(" "_v);
                name(decl[((NodeCall&)value).callee]->name);
                break;
            default:
                break;
        }

        forEachOperand(mir, value, [&](Operand& item, OperandRole) {
            writer.writeString(" "_v);
            operand(body, item);
        });

        forEachEdge(value, [&](Edge& edge) {
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
                place(body, *body.place(mir, elaboration.path(mir, step.path)->place));
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
        writer.writeString("fn "_v);
        name(value.name);
        writer.writeString("("_v);

        auto first = true;
        for(auto argumentPointer: value.args.contents(decl)) {
            auto argument = decl[argumentPointer];
            if(!first) writer.writeString(", "_v);
            first = false;

            name(argument->name);
            writer.writeString(": "_v);
            printOccurrence(writer, context, decl, argument->occurrence);
        }

        writer.writeString(") -> "_v);
        printOccurrence(writer, context, decl, value.result);

        // Only where it is *known*: an unknown suspension answer and a known "no" are different
        // facts, and a dump that printed nothing for both would be hiding the one that matters.
        if(value.summary.suspension.isKnown() && value.summary.suspension.value) {
            writer.writeString(" suspends"_v);
        }

        if(!value.mir) {
            writer.writeString(" = <no body>\n"_v);
            return;
        }

        writer.writeString(" {\n"_v);

        auto& body = *mir[value.mir];

        /*
         * The storage the function declares, before the blocks that use it.
         *
         * A local appears inside a place otherwise, where the path prints its name and nothing says
         * what is in it or whether it may be written - and those are the two facts a fixture about
         * initialization is asserting. One line each, ahead of the code, is where a reader looks.
         */
        auto declaredLocals = 0u;

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
            if(local->storage != StorageClass::Frame) {
                writer.writeString(" "_v);
                writer.writeString(storageClassName(local->storage));
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
        writer.writeString(" {"_v);

        auto first = true;
        for(auto field: value.fields.contents(decl)) {
            if(!first) writer.writeString(", "_v);
            first = false;

            name(field.name);
            writer.writeString(": "_v);
            printType(writer, context, decl, field.type);
        }

        writer.writeString("}"_v);

        auto teardown = [&](StringView what, TeardownKind kind) {
            if(kind == TeardownKind::None) return;

            writer.writeString(" "_v);
            writer.writeString(what);
            writer.writeString(kind == TeardownKind::Authored ? " authored"_v : " derived"_v);
        };

        teardown("drop"_v, value.ownership.drop);
        teardown("reclaim"_v, value.ownership.reclaim);
        writer.writeString("\n"_v);
    }

    void global(Global& value) {
        writer.writeString(value.writable ? "let & "_v : "let "_v);
        name(value.name);
        writer.writeString(": "_v);
        printOccurrence(writer, context, decl, value.occurrence);

        if(value.initial.isSet()) {
            writer.writeString(" = "_v);
            printConstant(writer, decl, value.initial);
        }

        writer.writeString("\n"_v);
    }

    void run() {
        for(auto modulePointer: program.modules) {
            auto module = decl[modulePointer];

            writer.writeString("module "_v);
            name(module->name);
            writer.writeString("\n"_v);

            for(auto recordPointer: module->records.contents(decl)) {
                writer.writeString("\n"_v);
                record(*decl[recordPointer]);
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

        // The field's *name*, which is what the author wrote. An index is what the representation
        // stores and is what a golden would have to be re-read after any reordering of a
        // declaration; the name is stable and is the thing a message and a fixture are both about.
        case Place::Field: {
            auto record = (RecordType*)base[mir[value.parent]->occurrence.type];
            return Tritium::format("%@.%@", text,
                                   stringView(context.findName(record->field(base, U16(value.payload)).name)));
        }
        case Place::Index:
            return Tritium::format("%@[%%%@]", text, U64(value.payload));
        case Place::Deref:
            return Tritium::format("%@.*", text);
        case Place::KindCount:
            break;
    }

    return text;
}

} // namespace mir
