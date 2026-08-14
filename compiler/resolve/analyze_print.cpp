#include "analyze_pass.h"

/*
 * Printing.
 *
 * The point of this file having a dump of its own is that liveness is the pass everything else
 * believes: a wrong range shows up as a drop in the wrong place, or as no drop at all, and neither
 * is obvious from the IR it produced. Printing the ranges makes the belief checkable.
 */

static void writeIndex(Net::Writer& writer, U32 value) {
    writer.writeBytes(32, [&](Byte* buffer) {
        return show(value, (char*)buffer, 32);
    });
}

// The three demand bits, in one place so that a local and an argument print alike.
static void printRequirements(Net::Writer& writer, const ReprRequirements& requirements) {
    switch(requirements.mutation) {
        case MutationDemand::ReadOnly: writer.writeString(" readonly"_v); break;
        case MutationDemand::Writable: writer.writeString(" writable"_v); break;
        case MutationDemand::Unknown: writer.writeString(" unknown"_v); break;
    }

    if(requirements.needsStableAddress) writer.writeString(" addressed"_v);
    if(requirements.mayResize) writer.writeString(" resizable"_v);
}

/*
 * The summary, printed first because it is what the rest of the program was analyzed against.
 *
 * A caller's diagnostics and a caller's storage decisions both follow from these lines, so a fixture
 * that asserts them is asserting the interface every call site was checked against rather than one
 * body's internals.
 */
static void printSummary(Net::Writer& writer, Context& context, ModuleBase base, Function& function) {
    auto& summary = function.summary;
    U16 index = 0;

    for(auto argPointer: function.args.contents(base)) {
        auto arg = base[argPointer];
        writer.writeString("  arg "_v);

        if(arg->name) writer.writeString(context.findName(arg->name));
        else writeIndex(writer, index);

        if(index < summary.args.size()) {
            auto entry = summary.args.get(base, index);
            printRequirements(writer, entry.requirements);

            if(entry.returnRoot) writer.writeString(" return"_v);
            if(entry.retained) writer.writeString(" retained"_v);
        }

        writer.writeByte('\n');
        index++;
    }

    switch(summary.resultBound) {
        case StorageBound::Frame: break;
        case StorageBound::Arguments: writer.writeString("  result arguments\n"_v); break;
        case StorageBound::Region: writer.writeString("  result region\n"_v); break;
        case StorageBound::Escapes: writer.writeString("  result escapes\n"_v); break;
    }
}

static void printFunctionOwnership(Net::Writer& writer, Context& context, Program& program,
                                   Function& function, OwnershipResult& result) {
    writer.writeString("fn "_v);
    writer.writeString(context.findName(function.name));
    writer.writeString(" {\n"_v);

    printSummary(writer, context, *program.arena, function);
    auto base = *program.arena;

    for(Size l = 0; l < result.locals.size(); l++) {
        auto& tracked = result.locals[l];
        writer.writeString("  %"_v);

        if(tracked.name) writer.writeString(context.findName(tracked.name));
        else {
            writer.writeString("local"_v);
            writeIndex(writer, U32(l));
        }

        writer.writeString(": "_v);
        writer.writeString(stringView(describeType(context, *program.types, tracked.type)));

        // Two ways for a slot not to be this frame's to release, and they are worth telling apart:
        // a borrowed one names storage the caller owns, while a closure's environment is storage
        // this frame allocated and the function value built out of it owns.
        if(!tracked.owned) {
            writer.writeString(function.localAt(base, U32(l)).closureEnv ? " closure"_v : " borrowed"_v);
        }

        if(tracked.droppable) writer.writeString(" droppable"_v);
        printRequirements(writer, tracked.requirements);

        // Only the allocations have a storage class to report, and only the non-default one is
        // worth a word: everything is frame-placed unless something proved it could not be.
        if(tracked.escapes) writer.writeString(" escapes"_v);
        if(tracked.storage == StorageClass::Heap) writer.writeString(" heap"_v);

        writer.writeString(" live"_v);
        auto ranges = result.rangesOf(l);

        if(!ranges.count) writer.writeString(" never"_v);

        for(Size i = 0; i < ranges.count; i++) {
            auto& range = result.rangeAt(ranges, i);
            writer.writeString(" ["_v);
            writeIndex(writer, range.from);
            writer.writeString(", "_v);
            writeIndex(writer, range.to);
            writer.writeByte(')');
        }

        writer.writeByte('\n');
    }

    writer.writeString("}\n"_v);
}

void printOwnership(Net::Writer& writer, Context& context, Program& program) {
    auto base = *program.arena;
    Size index = 0;

    if(!program.ownership) return;

    // The one thing this dump needs that a compilation does not compute - see
    // CompileSettings::ownershipRanges. A driver that prints ownership without having asked for the
    // ranges gets "live never" against every local, which reads as an answer rather than as an
    // omission, so it is asserted here rather than discovered in a golden file.
    assertTrue(context.settings.ownershipRanges);

    for(auto module: program.modules) {
        for(auto pointer: module->functionOrder.contents(base)) {
            auto found = program.ownership->functions.get(U32(pointer));
            if(!found) continue;

            auto function = base[pointer];
            if(!module->root && !function->used) continue;
            if(function->signature) continue;

            if(index++) writer.writeByte('\n');
            printFunctionOwnership(writer, context, program, *function, found.unwrap());
        }
    }
}
