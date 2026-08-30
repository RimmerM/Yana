#include "own.h"

namespace mir {

namespace {

struct Classifier {
    Context& context;
    Program& program;
    DeclBase decl;

    U32 errors = 0;

    // The records currently being folded, innermost last. A record reached while it is on this stack
    // contains itself, which is a layout with no size rather than an ownership answer.
    SmallArray<RecordType*, 16> active;

    /*
     * One member folded into its container's classification.
     *
     * The whole of "recurse into each field, then release this value's own storage", and the two
     * halves stay independent: a member with only a `Reclaim` must not give its container a `Drop`
     * and cost it region eligibility. That is the entire reason the halves are separate fields, so
     * folding them together here would give the split away at its first use.
     */
    void includeMember(DeclPtr<Type> member, Ownership& into) {
        auto inner = ownership(member);

        into.trivialCopy = into.trivialCopy && inner.trivialCopy;

        if(inner.reclaim != TeardownKind::None && into.reclaim == TeardownKind::None) {
            into.reclaim = TeardownKind::Derived;
        }

        if(inner.drop != TeardownKind::None && into.drop == TeardownKind::None) {
            into.drop = TeardownKind::Derived;
        }
    }

    /*
     * One record's answer: its members, then the instances the program wrote for it.
     *
     * The order matters and is the order the sentence is in. An authored instance is exactly the
     * statement "the structural answer is wrong for this type", so it is applied last and replaces
     * the half it is about - it does not run beside the derived walk, which would release each
     * member twice.
     */
    void classify(RecordType& record) {
        if(record.classified) return;

        for(auto& other: active) {
            if(other != &record) continue;

            report(record.source,
                   "%@ contains itself, and this stage has no `@box` to break the cycle with"_v,
                   context.findName(record.name));

            // Classified, so that a second reference to it reports nothing further and the fold that
            // asked for it gets an answer instead of recursing. The answer is the conservative one:
            // a type with no layout may not be copied and may not be assumed to release nothing.
            record.ownership = Ownership { false, TeardownKind::Derived, TeardownKind::Derived };
            record.classified = true;
            return;
        }

        active.push(&record);

        Ownership result;
        for(auto field: record.fields.contents(decl)) includeMember(field.type, result);

        if(auto instance = program.findInstance(CoreClass::Reclaim, asBase<Type>(&record - decl))) {
            (void)instance;
            result.reclaim = TeardownKind::Authored;
        }

        if(auto instance = program.findInstance(CoreClass::Drop, asBase<Type>(&record - decl))) {
            (void)instance;
            result.drop = TeardownKind::Authored;
        }

        /*
         * Duplicating a value whose lifetime releases something would release it twice.
         *
         * Stated once, here, rather than at each producer above, because it holds for the authored
         * cases exactly as it does for the derived ones - and a rule restated per producer is a rule
         * one producer eventually forgets.
         */
        if(result.needsTeardown()) result.trivialCopy = false;

        record.ownership = result;
        record.classified = true;
        active.pop();
    }

    // The answer for any type, with a record folded on demand so that a container reached before its
    // member still sees the member's settled answer rather than its default.
    Ownership ownership(DeclPtr<Type> type) {
        if(type && decl[type]->kind == Type::Record) classify(*(RecordType*)decl[type]);
        return ownershipOf(decl, type);
    }

    template<class... T>
    void report(LocationId source, StringView text, T&&... arguments) {
        errors++;
        context.diagnostics.error(text, source, forward<T>(arguments)...);
    }

    bool run() {
        for(auto modulePointer: program.modules) {
            auto module = decl[modulePointer];
            for(auto recordPointer: module->records.contents(decl)) classify(*decl[recordPointer]);
        }

        return errors == 0;
    }
};

} // namespace

bool classifyTypes(Context& context, Program& program) {
    Classifier classifier { context, program, *program.decl };
    return classifier.run();
}

} // namespace mir
