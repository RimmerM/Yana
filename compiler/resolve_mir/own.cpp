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
        into.explicitDrop = into.explicitDrop || inner.explicitDrop;

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
         * An authored `Copy` subtracts `TrivialCopy` - `doc/spec/ownership.md`'s "an authored `Copy`
         * subtracts `TrivialCopy`".
         *
         * Read here rather than left to the fold below, because there is nothing in the member graph
         * for the fold to see: this is a type all of whose members *are* trivial and whose author
         * said the structural answer is wrong anyway. Without it the instance is dead - every
         * default and `->` position keeps duplicating the bytes, the body runs only where somebody
         * wrote `copy(x)` by hand, and whatever invariant it maintains is broken by copies the
         * author cannot see.
         *
         * Not folded into the `needsTeardown()` line below, and the difference matters: a type with
         * an authored `Copy` and no teardown is tracked without being linear. Its uses are moves and
         * a use after one is a diagnostic, and nothing runs when it dies - which is what
         * `isTracked`'s `|| !trivialCopy` term is for, and the first type to actually reach it.
         */
        if(auto instance = program.findInstance(CoreClass::Copy, asBase<Type>(&record - decl))) {
            (void)instance;
            result.trivialCopy = false;
        }

        /*
         * The containment rule, in one line - `doc/spec/ownership.md`'s "a derived teardown may not
         * dispose of an `@explicitDrop` member".
         *
         * A **derived** teardown recursing into a linear member *is* the implicit disposal the
         * attribute forbids, so the fold above propagated the obligation to the container and the
         * container stays linear: `Array(Connection)` is `@explicitDrop`, its holder is, and the
         * question travels up until something answers it. That is propagation rather than a second
         * structural rule, and it is where every previous design's virality came from.
         *
         * An **authored** `Drop` is the answer, and it is the whole stopping rule. Its body runs on
         * the whole value, its subject's members are the body's to dispose of, and one it neither
         * disposes of nor moves out is a diagnostic *there* - `analyze_drop.cpp` places that check
         * at the point the subject dies. So a `Pool` holding connections is an ordinary droppable
         * value and nothing above it is coloured.
         */
        if(result.drop == TeardownKind::Authored) result.explicitDrop = false;

        /*
         * The declaration's own statement, applied after everything the fold worked out - and this
         * order is the whole reason `writtenExplicitDrop` is a separate field.
         *
         * `@explicitDrop data Guard` with `instance Drop(Guard)` is the ordinary shape of the
         * feature, and the line above would clear exactly it: the type's *own* authored `Drop` is
         * what discharges a *member's* obligation and says nothing about the type's. Reading the
         * attribute last is what keeps "I answer for what I hold" and "nobody may dispose of me
         * unasked" as the two independent statements they are.
         *
         * It subtracts nothing from either teardown half. `Reclaim` stays exactly as derived as it
         * was, so a linear value's storage is still region-eligible and no author ever writes
         * "release my own bytes"; what the attribute withholds is the permission to run the teardown
         * at a point the compiler chose, which is not a fact about the type's members at all.
         */
        if(record.writtenExplicitDrop) result.explicitDrop = true;

        /*
         * Duplicating a value whose lifetime releases something would release it twice, and
         * duplicating one that owes a disposal would owe two.
         *
         * Stated once, here, rather than at each producer above, because it holds for the authored
         * cases exactly as it does for the derived ones - and a rule restated per producer is a rule
         * one producer eventually forgets.
         */
        if(result.needsTeardown() || result.explicitDrop) result.trivialCopy = false;

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
