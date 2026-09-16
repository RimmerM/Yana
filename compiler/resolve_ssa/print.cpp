#include "print.h"
#include "../resolve_mir/print.h"

namespace ssa {

namespace {

struct Printer {
    Net::Writer& writer;
    Context& context;
    mir::Program& program;
    DeclBase decl;
    SsaBase base;

    // The body being printed, for the questions a block cannot answer about itself - its region's
    // kind. Set by `function` before its blocks are walked and null outside one.
    Body* current = nullptr;

    // The declaration the body belongs to, for the one thing a body cannot answer about itself: which
    // requirement a slot-naming instruction means, which is its *signature's* context.
    mir::Function* owner = nullptr;

    void uint(U64 value) {
        writer.writeBytes(64, [&](Byte* buffer) { return show(value, (char*)buffer, 64); });
    }

    // A signed number, for the one instruction whose static field may be negative: a `map`'s case
    // values are `@value(n)`'s, and those are somebody else's numbers.
    void sint(I64 value) {
        writer.writeBytes(64, [&](Byte* buffer) { return show(value, (char*)buffer, 64); });
    }

    void name(StringId id) {
        auto text = context.findName(id);
        writer.writeString(stringView(text));
    }

    void valueRef(Value& value) {
        writer.writeString("%"_v);
        uint(value.id.index);
    }

    void use(Use used) {
        writer.writeString(useKindName(used.kind()));
        writer.writeString(" "_v);

        if(!used.isSet()) {
            writer.writeString("<none>"_v);
            return;
        }

        valueRef(*base[used.value()]);
    }

    /*
     * A value's declaration: its occurrence, then its convention.
     *
     * The occurrence and not the type alone, so that a shared window and its owner are two lines a
     * reader can tell apart - which is the whole reason the value carries one. `printOccurrence`
     * prints nothing extra for the default, so a golden of a milestone with no loans in it reads
     * exactly as it did when this printed a bare type.
     */
    void declaration(Value& value) {
        valueRef(value);
        writer.writeString(": "_v);
        mir::printOccurrence(writer, context, decl, value.occurrence);
        writer.writeString(" "_v);
        writer.writeString(conventionName(value.convention));

        /*
         * What a borrow depends on - `Value::loan`, printed where it says something.
         *
         * Only the storage set, and only when it is non-empty. A loan whose lender is the caller is
         * every borrowed parameter in the program and adds a word to each of them for a fact the
         * occurrence already states; what a reader cannot get from anywhere else is *which of this
         * body's* storage a borrow is rooted in, which is exactly the question a fixture about
         * borrowing across a call is asking.
         */
        if(value.convention != Convention::Guaranteed || !value.loan) return;

        auto& loan = *base[value.loan];
        if(!loan.storage.size()) return;

        writer.writeString(" from{"_v);
        auto first = true;
        for(auto held: loan.storage.contents(base)) {
            if(!first) writer.writeString(", "_v);
            valueRef(*base[held]);
            first = false;
        }

        writer.writeString("}"_v);
    }

    void inst(Inst& value) {
        writer.writeString("  "_v);

        auto results = value.results.size();
        for(Size i = 0; i < results; i++) {
            if(i) writer.writeString(", "_v);
            declaration(*base[value.results.get(base, i)]);
        }

        if(results) writer.writeString(" = "_v);

        // Two kinds print the operation rather than the row's base name - see inst.def.
        switch(value.kind) {
            case Inst::Binary:
                writer.writeString(mir::binaryOpName(((InstBinary&)value).op));
                break;
            case Inst::Unary:
                writer.writeString(mir::unaryOpName(((InstUnary&)value).op));
                break;
            // The constructor, for `mir::NodeAggregate`'s reason: two arms may take the same
            // components, and which one this is decides the discriminant.
            case Inst::Aggregate: {
                writer.writeString("aggregate."_v);

                auto type = value.results.size() ? base[value.results.get(base, 0)]->type() : nullptr;
                auto index = ((InstAggregate&)value).constructor;

                if(type && decl[type]->kind == mir::Type::Record) {
                    auto& record = *(mir::RecordType*)decl[type];
                    if(index < record.cons.size()) {
                        name(record.cons.get(decl, index).name);
                        break;
                    }
                }

                writer.writeString(stringView(Tritium::format("%@", U64(index))));
                break;
            }
            // The tail-read guarantee, spent - `mir::NodeRead::overread`, and a golden about
            // `vectorPast` is about exactly this bit.
            case Inst::Load:
                writer.writeString(((InstLoad&)value).overread ? "load.past"_v : "load"_v);
                break;
            // Which operation, from op.def's atomic block - `instMnemonic` says `atomic` for
            // eleven of them, which is the base name a dump has no use for.
            case Inst::Atomic:
                writer.writeString(mir::atomicOpName(((InstAtomic&)value).op));
                break;
            default:
                writer.writeString(instMnemonic(value.kind));
                break;
        }

        // The fields that are not operands: what the instruction names statically. Written out per
        // kind rather than derived, because each is a different sort of name.
        switch(value.kind) {
            case Inst::Const:
                writer.writeString(" "_v);
                mir::printConstant(writer, context, decl, ((InstConst&)value).constant);
                break;
            case Inst::GlobalAddress:
                writer.writeString(" "_v);
                name(decl[((InstGlobalAddress&)value).global]->name);
                break;
            case Inst::FunValue:
                writer.writeString(" "_v);
                name(decl[((InstFunValue&)value).callee]->symbol);
                break;
            case Inst::Call: {
                auto& call = (InstCall&)value;

                // A call through a value names no declaration - the callee is the first use, which
                // the use walk above has already printed. A `Call` witness names none either: what
                // it dispatches to is a cell the caller filled, and the value is its first argument.
                if(call.through != mir::CallThrough::Value &&
                   call.through != mir::CallThrough::CallWitness) {
                    writer.writeString(" "_v);
                    name(decl[call.callee]->symbol);
                }

                /*
                 * Where the target came from, printed only where it is not the name.
                 *
                 * A direct call is the overwhelming majority and reads as it always did; a slot
                 * dispatch says which requirement it went through, because the name beside it is a
                 * *signature* rather than the body that runs and a golden that showed only the name
                 * could not tell the two apart. The environment is printed for the same reason - it
                 * is the whole of what the call hands over besides its arguments.
                 */
                if(call.through != mir::CallThrough::Declaration) {
                    writer.writeString(" ["_v);
                    writer.writeString(mir::callThroughName(call.through));
                    writer.writeString(" #"_v);
                    uint(call.requirement);

                    if(call.through == mir::CallThrough::ClassSlot) {
                        writer.writeString("."_v);
                        uint(call.member);
                    }

                    writer.writeString("]"_v);
                }

                if(call.environment) {
                    writer.writeString(" ["_v);
                    auto& filled = *decl[call.environment];

                    for(U32 i = 0; i < filled.arguments.size(); i++) {
                        if(i) writer.writeString(", "_v);
                        mir::printType(writer, context, decl, filled.arguments.get(decl, i));
                    }

                    writer.writeString("]"_v);
                }

                break;
            }
            case Inst::GenValue:
                writer.writeString(" "_v);
                mir::printType(writer, context, decl, ((InstGenValue&)value).variable);
                break;
            case Inst::Native:
                // Which operation, before the operands the shared walk prints.
                writer.writeString(" "_v);
                writer.writeString(mir::nativeOpName(((InstNative&)value).op));
                break;
            case Inst::VecLane:
            case Inst::VecWithLane:
                // The lane, which is a field rather than an operand - `mir::inst.def` is where the
                // reason a lane index is part of the instruction is written.
                writer.writeString(" #"_v);
                uint(((InstVecLane&)value).lane);
                break;
            case Inst::VecShuffle: {
                writer.writeString(" ["_v);
                auto first = true;

                for(auto lane: ((InstVecShuffle&)value).pattern.contents(base)) {
                    if(!first) writer.writeString(", "_v);
                    first = false;
                    uint(lane);
                }

                writer.writeString("]"_v);
                break;
            }
            case Inst::VecReduce:
                writer.writeString(" "_v);
                writer.writeString(mir::reduceOpName(((InstVecReduce&)value).reduce));
                break;
            case Inst::Atomic: {
                // The ordering, and the failure order and the strength for a compare-exchange -
                // `mir::print.cpp`'s arm, spelled the same way so the two dumps read alike.
                auto& atomic = (InstAtomic&)value;
                writer.writeString(" "_v);
                writer.writeString(mir::atomicOrderName(atomic.order));

                if(atomic.op == mir::AtomicOp::Compare) {
                    writer.writeString("/"_v);
                    writer.writeString(mir::atomicOrderName(atomic.failure));
                    if(atomic.weak) writer.writeString(" weak"_v);
                }

                break;
            }
            case Inst::Switch: {
                // The case values in edge order - the edge loop below prints the targets, and a
                // dump that showed only those could not say which number reaches which.
                writer.writeString(" ["_v);
                auto firstCase = true;

                for(Size i = 0; i < ((InstSwitch&)value).cases.size(); i++) {
                    if(!firstCase) writer.writeString(", "_v);
                    firstCase = false;

                    sint(((InstSwitch&)value).cases.pointer(base)[i].value);
                }

                writer.writeString("]"_v);
                break;
            }
            case Inst::Map: {
                // Every entry, and the fallback where the domain is open - the MIR printer's arm,
                // for the reason it gives: the table is the instruction.
                auto& map = (InstMap&)value;
                writer.writeString(" ["_v);
                auto firstEntry = true;

                for(auto entry: map.entries.contents(base)) {
                    if(!firstEntry) writer.writeString(", "_v);
                    firstEntry = false;

                    sint(entry.selector);
                    writer.writeString(" -> "_v);
                    mir::printConstant(writer, context, decl, entry.value);
                }

                writer.writeString("]"_v);

                if(!map.closed) {
                    writer.writeString(" else "_v);
                    mir::printConstant(writer, context, decl, map.fallback);
                }

                break;
            }
            case Inst::Metric: {
                // The measurement and the type, which are the whole of the instruction.
                auto& metric = (InstMetric&)value;
                writer.writeString(" "_v);
                writer.writeString(mir::typeMetricName(metric.metric));
                writer.writeString(" "_v);
                mir::printType(writer, context, decl, metric.of);
                break;
            }
            case Inst::RunSlots: {
                // The element, how many, and where the placement pass put them - the whole of the
                // instruction, since it has no uses for a reader to tell two apart by.
                auto& slots = (InstRunSlots&)value;
                writer.writeString(" "_v);
                mir::printType(writer, context, decl, slots.element);
                writer.writeString(" *"_v);
                uint(slots.capacity);
                writer.writeString(slots.placed ? " frame"_v : " heap"_v);
                break;
            }
            case Inst::FieldAddress: {
                auto& field = (InstFieldAddress&)value;

                // A projection through a property slot names the *requirement*, which is a field
                // name over a type parameter and prints as one - the subject has no field list to
                // read a name out of, and the requirement is what the author wrote.
                if(field.property != InstFieldAddress::kNoProperty) {
                    auto required = decl[owner->gen]->properties.get(decl, field.property);
                    writer.writeString(" ."_v);
                    name(required.field);
                    break;
                }

                // The discriminant has no name in the declaration, and the sigil the MIR place
                // printer uses is what a fixture reads to see that a match tested the tag.
                if(field.field == InstFieldAddress::kDiscriminant) {
                    writer.writeString(" .#"_v);
                    break;
                }

                auto record = (mir::RecordType*)decl[field.record];

                // The field's name, which is what the author wrote. An index is what the instruction
                // stores and is what a golden would have to be re-read after any reordering of the
                // declaration.
                writer.writeString(" ."_v);
                name(record->field(decl, field.field).name);
                break;
            }
            case Inst::Drop: {
                auto& dropped = (InstDrop&)value;
                writer.writeString(" "_v);
                mir::printOccurrence(writer, context, decl, dropped.held);

                // `InstDrop::boxesVisible`, printed only where it is false. A drop of storage this
                // frame owns is the ordinary case and saying so on every line would say nothing;
                // what a reader wants to see is the one that has to ask where the boxes came from.
                if(!dropped.boxesVisible) writer.writeString(" boxes borrowed"_v);
                break;
            }
            case Inst::Alloc: {
                auto& alloc = (InstAlloc&)value;
                writer.writeString(" "_v);
                writer.writeString(storageClassName(alloc.storage));
                writer.writeString(" "_v);
                mir::printOccurrence(writer, context, decl, alloc.held);
                break;
            }
            default:
                break;
        }

        forEachUse(base, value, [&](Use used) {
            writer.writeString(" "_v);
            use(used);
        });

        forEachEdge(base, value, [&](Edge& edge) {
            writer.writeString(" -> bb"_v);
            uint(base[edge.target]->index.index);

            if(edge.arguments.size()) {
                writer.writeString("("_v);
                auto first = true;
                for(auto used: edge.arguments.contents(base)) {
                    if(!first) writer.writeString(", "_v);
                    first = false;
                    use(used);
                }
                writer.writeString(")"_v);
            }
        });

        writer.writeString("\n"_v);
    }

    void block(Block& value) {
        writer.writeString("bb"_v);
        uint(value.index.index);

        if(value.argumentCount()) {
            writer.writeString("("_v);
            auto first = true;
            for(auto argument: value.arguments(base)) {
                if(!first) writer.writeString(", "_v);
                first = false;
                declaration(*base[argument]);
            }
            writer.writeString(")"_v);
        }

        /*
         * The continuation region, printed only where it is not the body's own.
         *
         * A marker on every block would be a word on every line of every golden for a fact that is
         * the same everywhere. What a reader cannot get from the graph is which blocks the *builder*
         * called a cleanup - a diamond looks the same whichever its arms are - so that is what
         * appears, and a body with no conditional teardown reads exactly as it did.
         */
        auto region = current ? current->region(base, value.region) : nullptr;
        if(region && region->kind != Continuation::Body) {
            writer.writeString(" cleanup"_v);
        }

        writer.writeString(":\n"_v);
        for(auto instPointer: value.instructions(base)) inst(*base[instPointer]);
    }

    void function(mir::Function& value) {
        writer.writeString("fn "_v);
        name(value.symbol);
        writer.writeString("("_v);

        // The parameters as *declared*, not as the entry block's arguments. They are the same list -
        // the conversion makes one value per declaration - and this is the half that says what the
        // callee agreed to receive, which is what a call in another function is checked against.
        auto first = true;
        for(auto argumentPointer: value.args.contents(decl)) {
            auto argument = decl[argumentPointer];
            if(!first) writer.writeString(", "_v);
            first = false;

            name(argument->name);
            writer.writeString(": "_v);
            mir::printOccurrence(writer, context, decl, argument->occurrence);
        }

        writer.writeString(") -> "_v);
        mir::printOccurrence(writer, context, decl, value.result);

        if(!value.ssa) {
            writer.writeString(" = <no body>\n"_v);
            return;
        }

        writer.writeString(" {\n"_v);

        auto& body = *base[value.ssa];
        current = &body;
        owner = &value;
        // A deleted block keeps its table entry so that every `BlockId` keeps meaning what it did -
        // see `Block::deleted` - and printing one would be printing a block that is not there.
        auto printed = false;
        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto at = body.block(base, BlockId(i));
            if(at->deleted) continue;

            if(printed) writer.writeString("\n"_v);
            printed = true;
            block(*at);
        }

        writer.writeString("}\n"_v);
    }

    void run() {
        for(auto modulePointer: program.modules) {
            auto module = decl[modulePointer];

            // The program's own modules, and not the library's - `resolve_mir/print.cpp`'s `run`
            // makes the argument, and the two dumps have to agree about what a fixture is showing.
            if(module->fromLibrary) continue;

            writer.writeString("module "_v);
            name(module->name);
            writer.writeString("\n"_v);

            for(auto globalPointer: module->globals.contents(decl)) {
                auto global = decl[globalPointer];
                writer.writeString("\n"_v);
                writer.writeString(global->writable ? "let & "_v : "let "_v);
                name(global->name);
                writer.writeString(": "_v);
                mir::printType(writer, context, decl, global->occurrence.type);

                if(global->initial.isSet()) {
                    writer.writeString(" = "_v);
                    mir::printConstant(writer, context, decl, global->initial);
                }

                writer.writeString("\n"_v);
            }

            /*
             * The **iterator site types** this module minted, and what keeps each member alive -
             * Design-Iterator-Objects.md §7 stage 3's inspection output.
             *
             * Here rather than in the MIR dump because it is a fact about the *transform*: the MIR
             * dump prints the record as an ordinary `data`, which is what it is, and this is the
             * question an author has about one - "why is this in my record". A member with no
             * `yield`s listed is the index, the slot or a captured argument, which live for the
             * object's whole life; one with a list is a payload member, and the list is the
             * hand-overs it has to survive.
             *
             * `#` is a value the producer keeps across a hand-over that no name in its body reaches -
             * a counted `for`'s own bound, and the like. Two names on one line are **slot sharing**:
             * members whose live ranges never meet share a field, and the `across` list is then the
             * union of theirs - which is what says why one field survives two hand-overs that no
             * single local of the producer does.
             *
             * `$held` is the one member after the payload and carries no `across` list: it is not
             * something the producer keeps but the record's own note about the slot, so it is read
             * out of `heldField` rather than out of `payloadYields`.
             */
            for(auto recordPointer: module->records.contents(decl)) {
                auto record = decl[recordPointer];
                if(!record->producer) continue;

                // `lens` where the producer is one, which is the same distinction the record's own
                // name makes (`.Lens` against `.Iter`): both are site types and only one of them is
                // something a consumer ever steps.
                writer.writeString(record->lensSite ? "\nlens "_v : "\niter "_v);
                name(record->name);
                writer.writeString(" of "_v);
                name(decl[record->producer]->name);
                writer.writeString(" {\n"_v);

                for(auto field: record->fields.contents(decl)) {
                    writer.writeString("  "_v);
                    if(field.name == StringId()) writer.writeString("#"_v);
                    else name(field.name);

                    writer.writeString(": "_v);
                    mir::printType(writer, context, decl, field.occurrence.type);

                    /*
                     * And **how** the member is held, where the type alone does not say it: three
                     * kinds of field are a pointer and a reader asking "why is this in my record"
                     * is asking which of them this is - and a fourth word, `nested`, which is the
                     * one payload member the producer's body does not name at all: a region the
                     * producer runs over part of its own body, whose whole state the outer record
                     * keeps. `for x in g(...)` and `withLock(m):` are both that, and which of the
                     * two it is is the member's own type - `g.Iter` against `withLock.Lens`.
                     */
                    if(field.index >= 2 && field.index < record->payloadFirst) {
                        auto kind = record->captureKinds.get(decl, field.index - 2);
                        if(kind == mir::RecordType::kCaptureAddress) writer.writeString(" lent"_v);
                        else if(kind == 2) writer.writeString(" window"_v);
                    }

                    /*
                     * The payload proper - which is every field past `payloadFirst` except the three
                     * that come after it and are not members: `$held`, and delegation's `$down` and
                     * `$inner`, which are the record's own notes about its slot and its chain rather
                     * than anything the producer keeps across a hand-over.
                     */
                    if(field.index >= record->payloadFirst && field.index != record->heldField &&
                       field.index != record->downField && field.index != record->innerField) {
                        // Every entry in this field, which is one unless they share - see above.
                        for(U32 j = 0; j < record->payloadLocals.size(); j++) {
                            if(record->payloadFields.get(decl, j) != field.index) continue;

                            auto member = record->payloadLocals.get(decl, j);
                            if(member & mir::RecordType::kHeapMember) writer.writeString(" placed"_v);
                            if(member & mir::RecordType::kBorrowMember) writer.writeString(" borrowed"_v);
                            if(member & mir::RecordType::kNestMember) {
                                writer.writeString(" nested"_v);

                                // And whether the child is **boxed** - §6.3's recursion, where the
                                // member holds the erased form rather than the child's own record.
                                // The field's type says it, and the word is here so that the
                                // inspection reads as the record does rather than as the type list.
                                if(decl[field.occurrence.type]->kind == mir::Type::Iter) {
                                    writer.writeString(" boxed"_v);
                                }

                                /*
                                 * And whether the level **delegates** - §6.3's `yield from`, where
                                 * the member is the link rather than the bare pair and the consumer
                                 * steps the child directly. Always boxed, so the two words read as
                                 * one statement about the same member.
                                 */
                                if(record->linkType &&
                                   field.occurrence.type == record->linkType) {
                                    writer.writeString(" boxed delegated"_v);
                                }
                            }
                        }

                        writer.writeString(" across"_v);

                        auto listed = false;

                        for(U32 y = 0; y < record->handovers; y++) {
                            auto keeps = false;

                            for(U32 j = 0; j < record->payloadLocals.size() && !keeps; j++) {
                                if(record->payloadFields.get(decl, j) != field.index) continue;
                                keeps = record->keepsAcross(decl, j, y);
                            }

                            if(!keeps) continue;

                            writer.writeString(" "_v);
                            uint(y);
                            listed = true;
                        }

                        /*
                         * An **empty** list, which one kind of member has: a `@heap`-placed local is
                         * a member because of where it lives and not because anything reads it after
                         * a hand-over, so a producer that only touches it before the first `yield`
                         * keeps it across none of them. Said rather than left as a bare `across`,
                         * which reads as a list the printer failed to write.
                         */
                        if(!listed) writer.writeString(" none"_v);
                    }

                    writer.writeString("\n"_v);
                }

                writer.writeString("}\n"_v);
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

void printProgram(Net::Writer& writer, Context& context, mir::Program& program, Converted& converted) {
    Printer printer { writer, context, program, *program.decl, *converted.arena };
    printer.run();
}

} // namespace ssa
