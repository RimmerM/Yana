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
        printTypeName(print, field.type);
        print.writer.writeByte('\n');
    }
}

} // namespace

void printReprs(Net::Writer& writer, Context& context, Program& program, const ReprTarget& target) {
    ReprTable table(*program.types, target);
    ReprPrint print { writer, context, *program.types, table };
    auto global = *program.types;

    /*
     * Every concrete record the program contains, in declaration order.
     *
     * Deliberately not every interned type: the scalars have nothing interesting to say and a
     * generic declaration has no layout at all, so a dump of all of them would bury the two lines a
     * fixture is actually about. A record reached only as a field of another still appears, because
     * whatever computed the parent computed it.
     *
     * Collected and sorted rather than printed as the name table is walked, because that table is a
     * hash map: its iteration order is a function of the hashes, and a golden file ordered by it
     * would churn whenever an unrelated declaration changed a name. A TypePtr is a region offset, so
     * sorting by it is declaration order, which is the order the source is written in.
     */
    Array<TypePtr> records;

    auto collect = [&](TypePtr type) {
        if(!type || global[type]->kind != Type::Record || isGeneric(global, type)) return;
        if(!records.containsValue(type)) records.push(type);
    };

    for(auto module: program.modules) {
        for(auto entry: module->namedTypes.entries()) {
            collect(entry.value);

            // A generic declaration has no layout, but its instantiations do - and they are reached
            // through it rather than by a name of their own.
            auto declaration = entry.value;
            if(!declaration || global[declaration]->kind != Type::Record) continue;

            for(auto instance: ((RecordType*)global[declaration])->instances.contents(global)) {
                collect((Type*)global[instance] - global);
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
