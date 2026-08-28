#include "repr_print.h"

/*
 * The computed layout, as text a fixture can assert.
 *
 * Implementation-Repr.md's testing strategy asks for "a `test/resolver/Repr.*` family asserting the
 * computed Repr (size/align/niche/discriminant-folded-or-not) for a set of representative types on
 * both targets", and this is what makes that possible: every other stage's output is code, and a
 * layout decision is only visible in code once something reads a field. A niche found and not yet
 * used would otherwise be untestable, which is the same as untested.
 *
 * Printed per *target*, from that target's own table, so the same fixture run twice says both what
 * native chose and what JS chose - which is the property the two independent families exist for and
 * the one no single-target dump could show.
 */

namespace {

struct ReprPrint {
    Net::Writer& writer;
    Context& context;
    GlobalBase global;
    ReprTable& table;
};

void printNumber(ReprPrint& print, U64 value) {
    StringBuilder text;
    text.appendValue(value);
    print.writer.writeString(StringView { text.pointer(), text.size() });
}

void printTypeName(ReprPrint& print, TypePtr type) {
    StringBuilder text;
    describeType(print.context, print.global, type, text);
    print.writer.writeString(StringView { text.pointer(), text.size() });
}

/*
 * A niche as the patterns it leaves free rather than as the range it forbids.
 *
 * The free count is what a sum type actually asks for - "can you distinguish three constructors" -
 * so printing it is what makes a fixture assertion mean something. The range is printed too, since
 * *which* patterns are free is what decides whether `Nothing` comes out as zero.
 */
void printNiche(ReprPrint& print, const Niche& niche) {
    if(!niche.exists()) {
        print.writer.writeString("none"_v);
        return;
    }

    // One pattern and no word it is a pattern of, so there is no range and no offset to print - see
    // NicheKind::Absent.
    if(niche.isAbsent()) {
        print.writer.writeString("absent"_v);
        return;
    }

    print.writer.writeString("at "_v);
    printNumber(print, niche.offset);
    print.writer.writeString(" width "_v);
    printNumber(print, niche.bytes);
    print.writer.writeString(", valid "_v);
    printNumber(print, niche.validStart);
    print.writer.writeString(".."_v);
    printNumber(print, niche.validEnd);
    print.writer.writeString(", free "_v);
    printNumber(print, niche.freeBelow());
    print.writer.writeString(" below / "_v);
    printNumber(print, niche.freeAbove());
    print.writer.writeString(" above"_v);
}

void printRepr(ReprPrint& print, TypePtr type) {
    auto& repr = print.table.of(type);

    printTypeName(print, type);
    print.writer.writeString(": "_v);

    if(repr.opaque) {
        print.writer.writeString("opaque\n"_v);
        return;
    }

    print.writer.writeString("size "_v);
    printNumber(print, repr.size);
    print.writer.writeString(", align "_v);
    printNumber(print, repr.align);
    print.writer.writeString(", stride "_v);
    printNumber(print, repr.stride);

    switch(repr.discriminant) {
        case DiscriminantKind::None:
            break;
        case DiscriminantKind::Word:
            print.writer.writeString(", tag word at 0, payload at "_v);
            printNumber(print, repr.payloadOffset);
            break;
        case DiscriminantKind::Niche:
            print.writer.writeString(", tag folded into constructor "_v);
            printNumber(print, repr.encoding.payloadConstructor);

            // An absent niche has one pattern and no arithmetic over it, so there is no first pattern
            // and no direction to run in - the other constructor simply is the host's absent value.
            if(repr.encoding.niche.isAbsent()) {
                print.writer.writeString("'s absence"_v);
                break;
            }

            print.writer.writeString("'s niche from "_v);
            printNumber(print, repr.encoding.firstPattern);
            print.writer.writeString(repr.encoding.ascending ? " up"_v : " down"_v);
            break;
        case DiscriminantKind::Bits:
            print.writer.writeString(", tag at bits "_v);
            printNumber(print, repr.discriminantBitOffset);
            print.writer.writeString(".."_v);
            printNumber(print, repr.discriminantBitOffset + repr.discriminantBits - 1);
            print.writer.writeString(" of "_v);
            printNumber(print, repr.discriminantBytes);
            print.writer.writeString(" bytes, payload at 0"_v);
            break;
    }

    // Whether the whole of it is one integer, which for an aggregate is the interesting half of the
    // answer: it decides both whether a container may co-pack it and whether a `&` of it carries a
    // shift. Printed only where it is narrower than its storage, since that is the case that means
    // something - see isNarrowRepr.
    if(repr.scalarBits && repr.scalarBits < repr.size * 8) {
        print.writer.writeString(", scalar of "_v);
        printNumber(print, repr.scalarBits);
        print.writer.writeString(" bits"_v);
    }

    print.writer.writeString(", niche "_v);
    printNiche(print, repr.niche);
    print.writer.writeByte('\n');

    for(Size i = 0; i < repr.fields.size(); i++) {
        auto& field = repr.fields[i];

        print.writer.writeString("  ."_v);
        printNumber(print, i);
        print.writer.writeString(" at "_v);
        printNumber(print, field.offset);

        if(field.isPacked()) {
            print.writer.writeString(" bits "_v);
            printNumber(print, field.bitOffset);
            print.writer.writeString(".."_v);
            printNumber(print, U32(field.bitOffset) + field.bitWidth - 1);
            print.writer.writeString(" of "_v);
            printNumber(print, field.wordBytes);
            print.writer.writeString(" bytes"_v);
        }

        print.writer.writeString(": "_v);

        // The one thing about a field that its type does not say: the storage here is a pointer to
        // one of these rather than one of these. A dump that left it out would print a recursive
        // declaration as though it contained itself.
        if(field.boxed) print.writer.writeString("@box "_v);

        printTypeName(print, field.type);
        print.writer.writeByte('\n');
    }
}

/*
 * Which records a dump is about: the ones this program's code has a value of.
 *
 * The alternative - every record every module declares - is what this used to be, and it made the
 * golden files assertions about the *library* rather than about the fixture. `Core` declaring one
 * more record rewrote nine `.repr.expect` files that had never mentioned it, which is churn that
 * hides the one line a layout change actually moved.
 *
 * So an imported module contributes what something reached, on exactly the terms printGlobal and
 * printFunction already apply to the resolve dump - `used`, set by markProgramReachable. The root
 * module contributes its declarations whether or not anything reached them, because a fixture that
 * declares a record in order to assert its layout has no other way to ask, and asserting a layout
 * nothing consumes yet is what this dump exists for.
 */
struct ReprCollect {
    GlobalBase global;
    ModuleBase local;
    TypeList& records;
};

void collectType(ReprCollect& collect, TypePtr type) {
    if(!type || isGeneric(collect.global, type)) return;

    auto declared = collect.global[type];
    switch(declared->kind) {
        case Type::Ptr:
            collectType(collect, ((PtrType*)declared)->to);
            return;
        case Type::Borrow:
            collectType(collect, ((BorrowType*)declared)->to);
            return;
        case Type::Array:
            collectType(collect, ((ArrayType*)declared)->content);
            return;
        case Type::Vector:
            collectType(collect, ((VectorType*)declared)->content);
            return;
        case Type::Atomic:
            collectType(collect, ((AtomicType*)declared)->content);
            return;
        case Type::Tup:
            for(auto field: ((TupType*)declared)->fields.contents(collect.global)) {
                collectType(collect, field.type);
            }
            return;

        /*
         * A record is the thing being collected, and its own members are collected through it: a
         * record reached only as a field of another still appears, because whatever computed the
         * parent computed it too.
         *
         * Recorded before the members are walked, which is what terminates a recursive declaration -
         * the `@box` field that closes the cycle finds the record already here.
         */
        case Type::Record: {
            if(collect.records.containsValue(type)) return;
            collect.records.push(type);

            for(auto constructor: ((RecordType*)declared)->constructors.contents(collect.global)) {
                collectType(collect, constructor.content);
            }
            return;
        }
        default:
            return;
    }
}

// Every type the code of one function has a value of - its signature, its slots, and the type each
// instruction produces. A local rather than only the signature, because a record a body builds and
// never passes anywhere is still a record this program laid out.
void collectFunction(ReprCollect& collect, Function& function) {
    collectType(collect, function.returnType);

    for(auto arg: function.args.contents(collect.local)) {
        collectType(collect, collect.local[arg]->type);
    }

    for(Size i = 0; i < function.localCount(); i++) {
        collectType(collect, function.localAt(collect.local, i).type);
    }

    for(auto blockPointer: function.blocks.contents(collect.local)) {
        auto block = collect.local[blockPointer];

        for(auto phi: block->phis(collect.local)) {
            collectType(collect, collect.local[phi]->type);
        }

        for(auto instruction: block->instructions(collect.local)) {
            collectType(collect, collect.local[instruction]->type);
        }
    }
}

} // namespace

void printReprs(Net::Writer& writer, Context& context, Program& program, const ReprTarget& target) {
    ReprTable table(*program.types, target);
    ReprPrint print { writer, context, *program.types, table };
    auto global = *program.types;

    /*
     * Every concrete record this program has a value of, in declaration order - see ReprCollect for
     * which those are.
     *
     * Deliberately not every interned type: the scalars have nothing interesting to say and a
     * generic declaration has no layout at all, so a dump of all of them would bury the two lines a
     * fixture is actually about.
     *
     * Collected and sorted rather than printed as each list is walked, because the seeds arrive in
     * reachability order and the root module's are found through a hash map: its iteration order is
     * a function of the hashes, and a golden file ordered by it would churn whenever an unrelated
     * declaration changed a name. A TypePtr is a region offset, so sorting by it is declaration
     * order, which is the order the source is written in.
     */
    TypeList records;
    ReprCollect collect { global, *program.arena, records };

    for(auto module: program.modules) {
        for(auto globalPointer: module->globalOrder.contents(collect.local)) {
            auto& value = *collect.local[globalPointer];
            if(!module->root && !value.used) continue;
            collectType(collect, value.type);
        }

        for(auto functionPointer: module->functionOrder.contents(collect.local)) {
            auto& function = *collect.local[functionPointer];

            // A class signature has no body and no concrete types to have a layout - its arguments
            // are the class's variables. Skipped for the same reason printProgram skips it.
            if(function.signature) continue;
            if(!module->root && !function.used) continue;

            collectFunction(collect, function);
        }

        /*
         * The root module's declarations, reached or not - see ReprCollect.
         *
         * A generic declaration has no layout, but its instantiations do, and they are reached
         * through it rather than by a name of their own. An imported generic's instances are not
         * walked here: the ones this program has a value of arrived through the code above, and the
         * rest are instantiations the library made for itself.
         */
        if(!module->root) continue;

        for(auto entry: module->namedTypes.entries()) {
            collectType(collect, entry.value);

            auto declaration = entry.value;
            if(!declaration || global[declaration]->kind != Type::Record) continue;

            for(auto instance: ((RecordType*)global[declaration])->instances.contents(global)) {
                collectType(collect, (Type*)global[instance] - global);
            }
        }
    }

    // Insertion sort, because a fixture has a handful of records and the library has no sort. The
    // key is the region offset, which is the order the declarations were read in.
    for(Size i = 1; i < records.size(); i++) {
        auto value = records[i];
        Size j = i;

        while(j && U32(records[j - 1]) > U32(value)) {
            records[j] = records[j - 1];
            j--;
        }

        records[j] = value;
    }

    Size index = 0;
    for(auto type: records) {
        if(index++) writer.writeByte('\n');
        printRepr(print, type);
    }
}
