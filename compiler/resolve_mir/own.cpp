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

        // Moving the record moves every member with it, so a member that may not travel pins the
        // record. The other direction does not hold and is not written: a relocatable member says
        // nothing about a record that is pinned for a reason of its own.
        into.relocatable = into.relocatable && inner.relocatable;

        // Recorded on the container and reconsidered below, once the instances are in - an authored
        // `Drop` is where a member's obligation can be discharged, and it is not known here yet.
        into.mustConsume = into.mustConsume || inner.mustConsume;

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
            record.ownership =
                Ownership { false, false, false, TeardownKind::Derived, TeardownKind::Derived };
            record.classified = true;
            return;
        }

        active.push(&record);

        Ownership result;

        /*
         * The record's own ownership schema - Analysis-IR.md §5.11, and the reason this fold runs
         * over the *declarations* rather than over the types.
         *
         * An external field is not a member: the record reaches it through one of its own origin
         * sets and does not own it, so it contributes nothing to the teardown and one slot to the
         * shape. What it does contribute to is copying - a shared external field may be duplicated,
         * because two shared loans of one object coexist, and a unique one may not, because a copy
         * would be a second exclusive capability (§2.1's `Editor`).
         */
        SmallArray<ExternalSlot, 8> slots;

        for(auto field: record.fields.contents(decl)) {
            if(!ownsField(field)) {
                if(field.contract.access() == Access::Unique) result.trivialCopy = false;

                ExternalSlot slot;
                slot.path.push(program.decl, field.index);
                slot.origin = field.contract.slot();
                slot.capability = field.contract.access();
                slots.push(slot);
                continue;
            }

            /*
             * An owned field whose *type* refers to storage somebody else keeps.
             *
             * `data Outer {inner: Reader}` where `Reader` has an external position: the outer record
             * inherits an origin requirement it does not name, and there is nowhere for a use site
             * to say what fills it. The spelling that would is a loan argument on the field's type -
             * `inner: Reader(a')`, which the parser has a kind for and `resolveType` does not read
             * yet - so this is refused by name rather than accepted with the slot silently dropped.
             */
            if(schemaOf(decl, field.type)) {
                report(record.source,
                       "%@ holds a %@, which refers to storage the record does not name"_v,
                       context.findName(record.name), typeText(context, decl, field.type));
                continue;
            }

            includeMember(field.type, result);
        }

        if(auto instance = program.findInstance(CoreClass::Reclaim, asBase<Type>(&record - decl))) {
            (void)instance;
            result.reclaim = TeardownKind::Authored;
        }

        if(auto instance = program.findInstance(CoreClass::Drop, asBase<Type>(&record - decl))) {
            (void)instance;
            result.drop = TeardownKind::Authored;
        }

        /*
         * A member's obligation, against the instance that could discharge it.
         *
         * `doc/spec/ownership.md` says where an obligation stops - "destructuring is where it stops:
         * the value ceases to exist and its fields take over" - and an authored `Drop` is that
         * destructuring, spelled by the author. Its parameter arrives owned, its fields are linear
         * inside the body, and a `@mustConsume` one that the body does not discharge is a diagnostic
         * *there*. So a record with an authored `Drop` answers for its members and does not pass the
         * obligation up.
         *
         * A **derived** teardown cannot: it releases each member and there is nothing to release, so
         * the obligation would be discharged by a walk that runs no code at all. That record is
         * `@mustConsume` too, and stays so until something authors an answer for it.
         */
        if(result.drop == TeardownKind::Authored) result.mustConsume = false;

        /*
         * Duplicating a value whose lifetime releases something would release it twice, and
         * duplicating one that owes a discharge would owe two.
         *
         * Stated once, here, rather than at each producer above, because it holds for the authored
         * cases exactly as it does for the derived ones - and a rule restated per producer is a rule
         * one producer eventually forgets.
         */
        if(result.needsTeardown() || result.mustConsume) result.trivialCopy = false;

        record.ownership = result;
        record.schema = program.shapes.intern(slots.pointer(), slots.size());
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
