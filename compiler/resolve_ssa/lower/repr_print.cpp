#include "repr_print.h"
#include "manifest.h"

namespace ssa {

namespace {

struct ReprPrint {
    Net::Writer& writer;
    Context& context;
    Layout layout;
};

void printNumber(ReprPrint& print, U64 value) {
    StringBuilder text;
    text.appendValue(value);
    print.writer.writeString(StringView { text.pointer(), text.size() });
}

void printTypeName(ReprPrint& print, DeclPtr<mir::Type> type) {
    auto text = typeText(print.context, print.layout.base, type);
    print.writer.writeString(stringView(text));
}

/*
 * A niche as the patterns it leaves free rather than as the range it forbids.
 *
 * The free count is what a sum type actually asks for - "can you tell three constructors apart" - so
 * printing it is what makes an assertion mean something. The range is printed too, since *which*
 * patterns are free is what decides whether `Nothing` comes out as zero.
 */
void printNiche(ReprPrint& print, const Niche& niche) {
    if(!niche.exists()) {
        print.writer.writeString("none"_v);
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

void printRepr(ReprPrint& print, DeclPtr<mir::Type> type) {
    auto repr = reprOf(print.layout, type);

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

    /*
     * How the constructor is told apart, in the shipping stage's exact words so that the two dumps
     * are comparable line for line - which is the whole reason this format is not this stage's own.
     */
    switch(repr.discriminant) {
        case Discriminant::None:
            break;
        case Discriminant::Word:
            print.writer.writeString(", tag word at "_v);
            printNumber(print, repr.discriminantOffset);
            print.writer.writeString(", payload at "_v);
            printNumber(print, repr.payloadOffset);
            break;
        case Discriminant::Bits:
            print.writer.writeString(", tag at bits "_v);
            printNumber(print, repr.discriminantBitOffset);
            print.writer.writeString(".."_v);
            printNumber(print, repr.discriminantBitOffset + repr.discriminantBits - 1);
            break;
        case Discriminant::Niche:
            print.writer.writeString(", tag folded into constructor "_v);
            printNumber(print, repr.encoding.payloadConstructor);
            print.writer.writeString("'s niche from "_v);
            printNumber(print, repr.encoding.firstPattern);
            print.writer.writeString(repr.encoding.ascending ? " up"_v : " down"_v);
            break;
    }

    // Whether the whole of it is one integer, which for an aggregate is the interesting half of the
    // answer: it decides both whether a container may co-pack it and whether a `&` of it carries a
    // shift. Printed only where it is narrower than its storage, since that is the case that means
    // something.
    if(repr.isNarrow()) {
        print.writer.writeString(", scalar of "_v);
        printNumber(print, repr.scalarBits);
        print.writer.writeString(" bits"_v);
    }

    print.writer.writeString(", niche "_v);
    printNiche(print, repr.niche);
    print.writer.writeByte('\n');

    for(Size i = 0; i < repr.fields.size(); i++) {
        auto field = repr.fields.get(print.layout.base, U32(i));

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
            if(!field.isBytewise()) {
                printNumber(print, field.wordBytes);
                print.writer.writeString(" bytes"_v);
            } else {
                print.writer.writeString("byte-wise"_v);
            }
        }

        print.writer.writeString(": "_v);

        /*
         * The field's **occurrence**, sigil and all - `'Int` where the declaration borrowed.
         *
         * Written here rather than by `typeText` because the two stages keep the fact in different
         * places: the shipping one has a `BorrowType` and prints it as part of the type, and this
         * one keeps access on the contract. Spelling it the same way is what leaves the two dumps
         * differing only where a *layout* differs.
         */
        auto access = field.occurrence.contract.access();
        if(access == mir::Access::Unique) print.writer.writeString("&"_v);
        else if(access == mir::Access::Shared) print.writer.writeString("'"_v);

        printTypeName(print, field.occurrence.type);
        print.writer.writeByte('\n');
    }
}

/*
 * Which records a dump is about: the ones the fixture's own module has a value of.
 *
 * The alternative - every record every module declares - makes the golden an assertion about the
 * *prelude*: `Run`, `Array`, `TypeTraits` and every instantiation a library body made for itself,
 * none of which the fixture mentions and all of which would rewrite the file whenever the prelude
 * changed. So the seeds are the root module's records and its functions' types, and a record of
 * another module appears exactly when something here reaches it - which is what puts `Bool` in the
 * dump and leaves `Run(a)` out.
 *
 * The same rule the shipping stage's `printReprs` applies, and deliberately: these two dumps exist
 * to be diffed, so what they are *about* has to be the same question as well as the same format.
 *
 * A generic declaration and an instantiation at a variable have no layout - their fields are the
 * variables - so neither is collected. `isConcreteType` is the one test for both.
 */
struct ReprCollect {
    Layout layout;
    Array<DeclPtr<mir::RecordType>>& records;
};

void collectType(ReprCollect& collect, DeclPtr<mir::Type> type) {
    auto base = collect.layout.base;
    if(!type || !mir::isConcreteType(base, type)) return;

    auto declared = base[type];
    switch(declared->kind) {
        case mir::Type::Pointer:
            collectType(collect, ((mir::PointerType*)declared)->to);
            return;
        case mir::Type::Array:
            collectType(collect, ((mir::ArrayType*)declared)->element);
            return;
        case mir::Type::Vector:
            collectType(collect, ((mir::VectorType*)declared)->lane);
            return;
        case mir::Type::Atomic:
            collectType(collect, ((mir::AtomicType*)declared)->content);
            return;

        /*
         * A record is the thing being collected, and its own members are collected through it: a
         * record reached only as a field of another still appears, because whatever computed the
         * parent computed it too.
         *
         * Recorded before the members are walked, which is what terminates a recursive declaration.
         */
        case mir::Type::Record: {
            auto pointer = DeclPtr<mir::RecordType>(type.offset);
            for(auto held: collect.records) if(held == pointer) return;

            collect.records.push(pointer);

            for(auto field: ((mir::RecordType*)declared)->fields.contents(base)) {
                collectType(collect, field.occurrence.type);
            }
            return;
        }
        default:
            return;
    }
}

} // namespace

void printReprs(Net::Writer& writer, Context& context, mir::Program& program) {
    auto layout = layoutFor(context, program);
    ReprPrint print { writer, context, layout };
    auto base = layout.base;

    Array<DeclPtr<mir::RecordType>> records;
    ReprCollect collect { layout, records };

    /*
     * The root module's own declarations, reached or not, and everything they lead to.
     *
     * Reached or not, because a fixture that declares a record purely to assert its layout has no
     * other way to ask - and asserting a layout nothing consumes yet is what this dump exists for.
     */
    auto& root = *base[program.root()];

    for(auto record: root.records.contents(base)) {
        if(record) collectType(collect, asBase<mir::Type>(record));
    }

    // And the types the module's own code has a value of - a record built by a body and passed
    // nowhere is still a record this program laid out.
    for(auto pointer: root.functions.contents(base)) {
        auto& function = *base[pointer];
        collectType(collect, function.result.type);

        for(auto argument: function.args.contents(base)) {
            collectType(collect, base[argument]->occurrence.type);
        }
    }

    /*
     * Declaration order, which is the region offset: the seeds arrive in reachability order, and a
     * golden ordered by that would churn whenever an unrelated declaration moved. Insertion sort,
     * because a fixture has a handful of records.
     */
    for(Size i = 1; i < records.size(); i++) {
        auto value = records[i];
        Size j = i;

        while(j && records[j - 1].offset > value.offset) {
            records[j] = records[j - 1];
            j--;
        }

        records[j] = value;
    }

    Size index = 0;
    for(auto record: records) {
        if(index++) writer.writeByte('\n');
        printRepr(print, asBase<mir::Type>(record));
    }
}

} // namespace ssa
