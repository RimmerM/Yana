#include "own.h"
#include "select.h"
#include "gen.h"
#include "name.h"

namespace mir {

namespace {

/*
 * Automatic indirection - `doc/spec/repr.md`'s "where the cycle is cut", and `own.h` for why it runs
 * before the classification rather than beside it.
 *
 * A type whose layout is cyclic through *inline containment* cannot be laid out at all, so the
 * compiler breaks the cycle with an indirection and nothing in the source names it.
 *
 * ## What is not an edge
 *
 * A member whose storage is a *handle* has a size independent of what it names, so it breaks the
 * cycle before this walk sees it. Four of them:
 *
 *  - a **pointer** and a **function value**, whose kinds say so;
 *  - an **external field**, which holds a loan of somebody else's storage rather than a member -
 *    `ownsField`, the same question the classification asks one function down;
 *  - an **already-boxed field**, whether `@box` wrote it or a previous walk did. A programmer's
 *    `@box` is therefore the manual override on the cut below, by being read before it.
 *
 * ## Where the cut lands
 *
 * At the **back edge** - the first field whose type is a record already on the walk's stack - at
 * whatever depth it appears inside generic arguments. For
 *
 *     data Tree(a) = Branch {left: Maybe(Tree(a)), right: Maybe(Tree(a))} | Leaf(a)
 *
 * the reference to `Tree` inside `Tree`'s own body is that edge, and it is reached through
 * `Maybe(Tree)`'s payload rather than at `Branch.left`. Cutting there is what produces a one-word
 * child - the box is a non-null pointer, so `Nothing` folds into its null niche by the ordinary
 * search - where cutting at `Branch.left` would make an *absent* child a pointer to a heap-allocated
 * `Maybe`.
 *
 * This stage needs one flag for that where the old one needs two. A constructor's payload is an
 * ordinary field of the record's flat list here - `RecordCon` names a range of it - so the edge that
 * *is* a payload and the edge that is a named field are the same thing to mark, and
 * `Constructor::boxed` has no counterpart.
 *
 * ## Why the answer is uniform, and therefore not part of the type
 *
 * `Maybe(Tree)` has infinite inline size, so every `Maybe(Tree)` in the program - local, field,
 * argument, instantiation - must be a pointer to a `Tree`. There is no context that could want it
 * otherwise, so there is nothing for two contexts to disagree about, which is what makes it legal to
 * leave out of the type: type identity exists to keep two things that *differ* from being confused.
 *
 * A **tuple** is rewritten rather than mutated, because tuples are interned structurally and `{Tree}`
 * boxed has to be a different type from `{Tree}` unboxed. A **record** is mutated in place, because
 * it is nominal: `Maybe(Tree)` is one instantiation and the only thing that could observe the change
 * is `Maybe(Tree)` itself.
 */
struct CycleBreaker {
    Context& context;
    Program& program;
    DeclBase decl;

    U32 errors = 0;

    // The records currently being walked, innermost last, and the innermost declaration among them.
    // The second is the near end of whatever edge is cut below it - see `checkCrossFileCut`.
    SmallArray<DeclPtr<Type>, 16> stack;
    DeclPtr<Type> enclosing = nullptr;

    // Whether this type is one the walk is currently inside, which is the whole definition of a back
    // edge: reaching it again means laying it out needs its own size.
    bool onStack(DeclPtr<Type> type) {
        for(auto entry: stack) {
            if(entry == type) return true;
        }

        return false;
    }

    // The file a record *declaration* was written in, or none. None for an instantiation and for
    // anything the compiler made rather than parsed, which is the point rather than a shortcut: the
    // ordering the message below is about is the order of two `data` declarations, and an
    // instantiation exists only because one of them named it.
    StringId declaringFile(DeclPtr<Type> type) {
        if(!type || decl[type]->kind != Type::Record) return StringId();

        auto& record = *(RecordType*)decl[type];
        if(record.instanceOf || record.tuple) return StringId();
        if(record.source == kNullLocation) return StringId();

        auto location = context.getLocation(record.source);
        return location ? location->sourceModule : StringId();
    }

    /*
     * An inline containment cycle may be broken implicitly only within one file.
     *
     * Which edge of a mutually recursive pair becomes the indirection is decided by declaration
     * order, so laying `A` out first makes `B`'s reference to `A` the back edge. Within one file
     * that is defensible: the order is written down, the reader can see it, and `@box` overrides it.
     * Across files it is not - the order would be a path sort, and adding a file to a directory
     * could change the representation of an existing type.
     *
     * So this is a diagnostic rather than a cut, and it names both ends because either of them is a
     * place the programmer can answer it.
     */
    void checkCrossFileCut(DeclPtr<Type> owner, DeclPtr<Type> target, LocationId source) {
        auto near = declaringFile(owner);
        auto far = declaringFile(target);
        if(!near || !far || near == far) return;

        errors++;
        context.diagnostics.error(
            "%@ and %@ contain each other and are declared in different files, so which of them holds the pointer would be decided by the order their file names sort in - write `@box` on the field that should be the indirection"_v,
            source, typeText(context, decl, owner), typeText(context, decl, target));
    }

    /*
     * A tuple, with whichever of its fields turned out to be back edges boxed. Answers the interned
     * tuple to use in place of this one, which is this one where nothing changed.
     *
     * A tuple cannot be its own back edge - reaching one means a record on the way round, and the
     * record is where the cut belongs - but the guard keeps the recursion bounded regardless.
     */
    DeclPtr<Type> breakTuple(RecordType& tuple, LocationId source) {
        auto self = asBase<Type>(DeclPtr<RecordType>(&tuple - decl));
        if(onStack(self)) return self;

        stack.push(self);

        SmallArray<RecordField, 8> fields;
        auto changed = false;

        for(auto field: tuple.fields.contents(decl)) {
            auto updated = field;

            if(field.boxed || !ownsField(field)) {
                // Already a handle. Either way this edge is not part of any cycle.
            } else if(onStack(field.occurrence.type)) {
                checkCrossFileCut(enclosing, field.occurrence.type, source);
                updated.boxed = true;
                changed = true;
                program.usesBoxes = true;
            } else {
                auto rewritten = breakCycles(field.occurrence.type, source);
                if(rewritten != field.occurrence.type) {
                    updated.occurrence.type = rewritten;
                    changed = true;
                }
            }

            fields.push(updated);
        }

        stack.pop();
        if(!changed) return self;

        // Through the constructor rather than through the intern table, because a tuple made here is
        // a tuple like any other: it needs its constructor, its layout and its schema, and the one
        // walk that gives a tuple those is `mir::tupleType`.
        return tupleType(context, program, fields.pointer(), fields.size(), tuple.source);
    }

    void breakRecord(RecordType& record, LocationId source) {
        auto self = asBase<Type>(DeclPtr<RecordType>(&record - decl));
        if(record.layoutBroken || onStack(self)) return;

        /*
         * Never a tuple, and this is where that is held to rather than left to the reader.
         *
         * A tuple is interned on its field list, so boxing a field of one in place would leave
         * `{Tree}` boxed sitting in the slot keyed as `{Tree}` unboxed - and the next lookup of the
         * unboxed spelling would get a pointer where it asked for a value. `breakTuple` rebuilds
         * instead, and `breakCycles` is what routes each kind to its own walk.
         */
        assertTrue(!record.tuple);

        stack.push(self);

        auto outer = enclosing;
        enclosing = self;

        for(U16 i = 0; i < U16(record.fields.size()); i++) {
            auto field = record.fields.get(decl, i);
            if(field.boxed || !ownsField(field)) continue;

            if(onStack(field.occurrence.type)) {
                checkCrossFileCut(self, field.occurrence.type, source);
                field.boxed = true;
                record.fields.set(decl, i, field);
                program.usesBoxes = true;
                continue;
            }

            auto rewritten = breakCycles(field.occurrence.type, source);
            if(rewritten == field.occurrence.type) continue;

            field.occurrence.type = rewritten;
            record.fields.set(decl, i, field);
        }

        stack.pop();
        enclosing = outer;
        record.layoutBroken = true;
    }

    DeclPtr<Type> breakCycles(DeclPtr<Type> type, LocationId source) {
        if(!type) return type;

        auto value = decl[type];

        /*
         * Reaching a value through one of these costs a load rather than containment, so what is on
         * the other side cannot make this layout infinite. The three the *kind* answers; the other
         * two - an external field and an already-boxed one - are answered by the field, above.
         */
        if(value->kind == Type::Pointer || value->kind == Type::Fun) return type;

        if(value->kind == Type::Record) {
            auto& record = *(RecordType*)value;
            if(record.tuple) return breakTuple(record, source);

            breakRecord(record, source);
            return type;
        }

        /*
         * A fixed array is walked through and never rewritten.
         *
         * `[T *n]` has no edge to put a pointer on. The elements are `n` of the type at a stride and
         * the run's address is computed from the owner rather than stored, so boxing the element
         * would make `[Tree *4]` four pointers - a different type from the one that was written, and
         * silently.
         *
         * So this only descends, and a cycle that reaches back through one is left to the "contains
         * itself" report in `classify`. That is the right split: `data T {kids: [T *4]}` has no
         * finite size *whatever* the compiler does to it, so the honest answer is a diagnostic rather
         * than a representation the program did not ask for.
         */
        if(value->kind == Type::Array) {
            breakCycles(((ArrayType*)value)->element, source);
            return type;
        }

        return type;
    }

    void run() {
        for(auto modulePointer: program.modules) {
            auto module = decl[modulePointer];

            for(auto recordPointer: module->records.contents(decl)) {
                auto& record = *decl[recordPointer];

                /*
                 * A **generic declaration** has no layout to be cyclic: `List(a)` is a shape rather
                 * than a type, and the indirection belongs to `List(Int)`, which is where the walk
                 * finds it. Marked broken so that a member walk reaching one stops rather than
                 * cutting an edge on a declaration nothing has a value of.
                 */
                if(isGeneric(decl, record.gen)) {
                    record.layoutBroken = true;
                    continue;
                }

                /*
                 * The roots are the **written declarations**, in declaration order, and an
                 * instantiation is reached through whichever of them names it.
                 *
                 * That is what makes the cut land where `doc/spec/repr.md` says it does. Walking
                 * from `Tree` descends into `Maybe(Tree)` and finds `Tree` on the stack at the
                 * payload, which is a one-word child; walking from `Maybe(Tree)` as a root would
                 * descend into `Tree` and cut at `Branch.left` instead, making an *absent* child a
                 * pointer to a heap-allocated `Maybe`. Both are finite layouts and only one is the
                 * one the language promises.
                 *
                 * Order is therefore load-bearing, and deliberately so: laying `A` out first makes
                 * `B`'s reference to `A` the back edge, reordering two declarations does change
                 * which of them holds the pointer, and `@box` is how a programmer asks for the other
                 * cut. `checkCrossFileCut` is what stops that order being one nobody can see.
                 */
                if(record.instanceOf || record.tuple) continue;

                breakRecord(record, record.source);
            }
        }

        /*
         * Then the **instantiations no declaration reached**, in a second sweep.
         *
         * `Maybe(Tree)` is reached through `Tree`'s field and is cut by the walk above. `List(a)` in
         * `fn depth(xs: List(a)) -> Int` is not reached by anything: it exists because a *signature*
         * named it, and a signature is not a member of any type. Left uncut it is a record that
         * contains itself, which is the diagnostic rather than the list the program wrote - and
         * `data List(a)` is the canonical recursive declaration, so this is not a corner.
         *
         * **After** the declarations rather than beside them, and the order is the whole reason this
         * is a second loop. Rooting `Maybe(Tree)` before `Tree` would descend into `Tree` and cut at
         * `Branch.left`, making an absent child a pointer to a heap-allocated `Maybe`; by the time
         * this runs, everything a declaration reaches is already `layoutBroken` and stops here at its
         * first line. So what this sweep cuts is exactly what nothing else could have.
         *
         * The shipping stage arranges the same thing differently - `completeInstance` breaks each
         * instantiation as it is filled - and pays for it with a walk that has to notice it ran while
         * a declaration was still incomplete. Here every declaration is in before either loop runs,
         * which is what `resolveProgram` places this pass after.
         */
        for(auto modulePointer: program.modules) {
            auto module = decl[modulePointer];

            for(auto recordPointer: module->records.contents(decl)) {
                auto& record = *decl[recordPointer];
                if(record.layoutBroken || record.tuple || !record.instanceOf) continue;
                if(isGeneric(decl, record.gen)) continue;

                breakRecord(record, record.source);
            }
        }

        /*
         * The anonymous records - `Program::tuples` - are deliberately **not** roots.
         *
         * A tuple has no name, so nothing can refer to one except by containing it, and a cycle
         * therefore always runs through a record: `{Tree}` is reachable only from whatever field
         * wrote it. That field's walk is what rewrites it, and rewriting one reached as a root would
         * intern a corrected tuple nothing holds - the correction going where the reference is not.
         */
    }
};

/*
 * Whether a declared `Copy` instance's answer is its **payload's** rather than its own.
 *
 * The question is about the instance and not about the arguments: a head that binds variables and
 * whose own context requires `Copy` of one of them says "this carrier copies exactly as far as what
 * it holds does", which is the structural answer restated - and where the payload does not copy, the
 * member fold has already said so. A head that binds nothing, and one that promises `Copy` at every
 * payload, are both the author overriding that fold, and those subtract.
 *
 * `instance.klass` and not the role, because this is asked of one class and the recursion it is
 * about is the class asking for itself.
 */
static bool inheritedCopy(DeclBase base, Instance& instance) {
    if(!instance.gen) return false;

    auto& gen = *base[instance.gen];
    if(gen.variables.size() == 0) return false;

    for(auto requirement: gen.classes.contents(base)) {
        if(requirement.klass != instance.klass) continue;

        for(auto argument: requirement.args.contents(base)) {
            if(argument && base[argument]->kind == Type::Variable) return true;
        }
    }

    return false;
}

struct Classifier {
    Context& context;
    Program& program;
    DeclBase decl;

    U32 errors = 0;

    // The records currently being folded, innermost last. A record reached while it is on this stack
    // contains itself, which is a layout with no size rather than an ownership answer - unless the
    // edge that reached it is a box, which is what `includeBoxedMember` stops at.
    SmallArray<RecordType*, 16> active;

    /*
     * Whether any answer this walk produced was computed with a member of it left out - see
     * `includeBoxedMember`, which is the one place it is set.
     *
     * A record folded while another is on the stack answers without that other's contribution and is
     * then remembered, so mutual recursion through two boxes can settle low: `A` holding a boxed `B`
     * holding a boxed `A` folds `B` first, without `A`'s drop, and caches it. `run` re-folds until
     * nothing changes rather than ordering the walk, because there is no order that fixes it - each
     * of the two needs the other's answer.
     */
    bool cyclic = false;

    /*
     * One member folded into its container's classification.
     *
     * The whole of "recurse into each field, then release this value's own storage", and the two
     * halves stay independent: a member with only a `Reclaim` must not give its container a `Drop`
     * and cost it region eligibility. That is the entire reason the halves are separate fields, so
     * folding them together here would give the split away at its first use.
     */
    /*
     * A **boxed** member - `RecordField::boxed`, and `doc/spec/repr.md`'s table.
     *
     * The three answers all change, and each of them is about the box rather than about what is in
     * it:
     *
     *  - **`TrivialCopy` is lost.** A bitwise duplicate would copy the pointer and leave two owners
     *    of one allocation. `Copy` is still writable by hand - allocate a new box, copy the target -
     *    so what boxing does is demote `TrivialCopy` to `Copy` rather than remove copying, which is
     *    the one non-transparent consequence of `@box` and the reason it is an API change.
     *  - **Relocation is preserved, and sometimes gained.** Moving the owner moves a pointer and the
     *    target keeps its address, so a self-referential type - which is not relocatable inline
     *    because it has no inline form at all - becomes relocatable by boxing the edge. That is why
     *    the target's own answer is deliberately *not* folded in here.
     *  - **`Reclaim` is always derived**, because the box has to be handed back even where its
     *    target releases nothing. `Drop` follows the target: a box around something with no effect
     *    at last use has none either.
     *
     * The **cycle** is what makes the last of those conditional. Asking a member already on the walk
     * for its drop is asking the question this walk is answering, so a self-referential edge
     * contributes nothing and the outer walk's own members are what decide. That is the least fixed
     * point and it is the right one: a drop can only enter a cycle through a member outside it or
     * through an authored instance, and both are seen by the walk that owns them. `run` is where the
     * mutual case is closed - a declaration classified while another was still on the stack answered
     * without it, and is folded again until nothing changes.
     */
    void includeBoxedMember(DeclPtr<Type> member, Ownership& into) {
        into.trivialCopy = false;
        into.reclaim = TeardownKind::Derived;

        if(onActiveStack(member)) {
            cyclic = true;
            return;
        }

        auto inner = ownership(member);
        into.explicitDrop = into.explicitDrop || inner.explicitDrop;

        if(inner.drop != TeardownKind::None && into.drop == TeardownKind::None) {
            into.drop = TeardownKind::Derived;
        }
    }

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

            /*
             * What is left after `breakLayoutCycles` has run, which is a narrower thing than it was.
             *
             * Every containment cycle with an edge a pointer can be put on was cut before this walk
             * started, so reaching this now means a cycle whose every edge is one that cannot be:
             * through a fixed array, whose elements are the type at a stride and have no slot to
             * hold an address, or across two files, where the cut was refused rather than made
             * because the order deciding it is a file-name sort. Both have already been reported by
             * name where they can be; this is the backstop that stops the fold recursing.
             */
            report(record.source,
                   "%@ contains itself through an edge that cannot hold a pointer, so it has no finite size - a fixed array of a type inside that type is the usual way to write one"_v,
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

        for(auto field: record.fields.contents(decl)) {
            /*
             * An external field is not a member: the record reaches it through one of its own origin
             * sets and does not own it, so it contributes nothing to the teardown. What it does
             * contribute to is copying - a shared external field may be duplicated, because two
             * shared loans of one object coexist, and a unique one may not, because a copy would be
             * a second exclusive capability (§2.1's `Editor`). Where it sits is `foldSchemas`', and
             * has been settled before this pass runs.
             */
            if(!ownsField(field)) {
                if(field.occurrence.contract.access() == Access::Unique) result.trivialCopy = false;
                continue;
            }

            // `indirectField` rather than the flag: a box around a unit is not an indirection, and
            // a record whose only boxed field is one has nothing to hand back.
            if(indirectField(decl, field)) {
                includeBoxedMember(field.occurrence.type, result);
                continue;
            }

            includeMember(field.occurrence.type, result);
        }

        if(auto instance = declaredInstanceFor(context, program, intrinsic::Known::reclaim, asBase<Type>(&record - decl))) {
            (void)instance;
            result.reclaim = TeardownKind::Authored;
        }

        if(auto instance = declaredInstanceFor(context, program, intrinsic::Known::drop, asBase<Type>(&record - decl))) {
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
         *
         * **At exactly this head, and a parametric one over the carrier is not that** -
         * `doc/spec/ownership.md`'s TrivialCopy row, refined 2026-09-07. `instance (Copy(a))
         * Copy(Maybe(a))` says nothing about `Maybe(Int)`: what it subtracts is its payload's, and
         * where the payload is trivial the `Copy(a)` it asks for is the *blanket* instance, which
         * subtracts from nothing. `lib/Core/Core.yana` says so beside the three carriers - "the two
         * are candidates only where the payload is trivial *and* the carrier is not, which cannot
         * happen" - and the shipping stage has always agreed. Read literally, this line disagreed:
         * `let b = x` over a `Maybe(Int)` became a move, `node(n)?.leaf?.value` a use after one.
         *
         * So a conditional parametric head defers to the fold above, which already has the answer:
         * `Maybe(String)` is non-trivial because its member is, and no subtraction is needed to say
         * it. What still subtracts here is the head that is a statement about *this* type - a
         * concrete one (`instance Copy(String)`), and a parametric one whose context does **not**
         * make it conditional, since that one claims `Copy` at every payload including the trivial
         * ones and is therefore an author overriding the structural answer.
         */
        if(auto instance = declaredInstanceFor(context, program, intrinsic::Known::copy,
                                               asBase<Type>(&record - decl))) {
            if(!inheritedCopy(decl, *decl[instance])) result.trivialCopy = false;
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
        record.classified = true;
        active.pop();
    }

    // Whether this type is a record the fold is currently inside - the same question the "contains
    // itself" report asks, asked of a member before the recursion rather than after it.
    bool onActiveStack(DeclPtr<Type> type) {
        if(!type || decl[type]->kind != Type::Record) return false;

        auto record = (RecordType*)decl[type];
        for(auto other: active) {
            if(other == record) return true;
        }

        return false;
    }

    // The answer for any type, with a record folded on demand so that a container reached before its
    // member still sees the member's settled answer rather than its default.
    Ownership ownership(DeclPtr<Type> type) {
        if(type && decl[type]->kind == Type::Record) classify(*(RecordType*)decl[type]);

        /*
         * Through a **fixed array** as well, which is the one member kind whose own answer is folded
         * rather than stored.
         *
         * `ownershipOf` computes `[T *n]`'s from `T`'s, and it has no stack to notice that `T` is the
         * record this walk is already inside. That is the one containment cycle `breakLayoutCycles`
         * leaves standing - the elements are the type at a stride and there is no slot an
         * indirection could go in - so descending here is what turns it into the report below rather
         * than into a layout the next walk recurses on forever.
         */
        if(type && decl[type]->kind == Type::Array) ownership(((ArrayType*)decl[type])->element);

        return ownershipOf(decl, type);
    }

    template<class... T>
    void report(LocationId source, StringView text, T&&... arguments) {
        errors++;
        context.diagnostics.error(text, source, forward<T>(arguments)...);
    }

    // Whether two answers for one record differ - the fixpoint's whole test. Written out rather than
    // an `operator==` on `Ownership`, because equality on that type would be a comparison anything
    // could reach and this is the one question that wants it.
    static bool differs(const Ownership& a, const Ownership& b) {
        return a.trivialCopy != b.trivialCopy || a.relocatable != b.relocatable ||
               a.explicitDrop != b.explicitDrop || a.reclaim != b.reclaim || a.drop != b.drop;
    }

    // Every record in the program, in one order, for the two walks below to share. Not a cached list
    // - classifying one record can make another - so it is rebuilt each round, which is what makes
    // the fixpoint see a type the previous round created.
    template<class F> void eachRecord(F&& body) {
        for(auto modulePointer: program.modules) {
            auto module = decl[modulePointer];
            for(auto recordPointer: module->records.contents(decl)) body(*decl[recordPointer]);
        }

        // The anonymous records, which no module declares - `Program::tuples`, where that is argued.
        // Walked by index because classifying one can make another: substituting through a tuple's
        // fields interns the substituted tuple.
        for(Size i = 0; i < program.tuples.size(); i++) body(*decl[program.tuples[i]]);
    }

    bool run() {
        eachRecord([&](RecordType& record) { classify(record); });

        /*
         * The second half, and only where a boxed edge closed a cycle - `includeBoxedMember`.
         *
         * A record folded while another was on the stack answered without that other's contribution,
         * so `data A {b: @box B}` and `data B {a: @box A}` can both come out owing nothing at last
         * use when one of them holds a `Guard`. Re-folding until nothing moves is the least fixed
         * point, and it is reached in a bounded number of rounds because every field of every answer
         * moves in one direction only: `trivialCopy` and `relocatable` fall, the two teardown halves
         * rise from `None`, and `explicitDrop` rises.
         *
         * Nothing is paid for a program with no cycle through a box, which is nearly every program:
         * the flag is set by exactly one line and stays false otherwise.
         */
        if(!cyclic) return errors == 0;

        Size records = 0;
        eachRecord([&](RecordType&) { records++; });

        // Five monotone fields per record, so a round that moves nothing is the answer and a program
        // cannot need more rounds than it has moves left to make.
        auto rounds = records * 5 + 1;

        for(Size round = 0; round < rounds; round++) {
            auto moved = false;

            eachRecord([&](RecordType& record) {
                if(!record.classified) return;

                auto before = record.ownership;
                record.classified = false;
                classify(record);
                if(differs(before, record.ownership)) moved = true;
            });

            if(!moved) return errors == 0;
        }

        // Not a bound on program size: each round moves at least one of five monotone fields on at
        // least one record, so a program that needs more rounds than it has records has a fold that
        // is not monotone - which is a compiler bug rather than a large program.
        assertTrue(false);
        return errors == 0;
    }
};

} // namespace

// The argument is in constant.h, beside the kind.
bool holdsText(DeclBase base, const Constant& constant) {
    if(constant.kind == Constant::Text) return true;
    if(constant.kind != Constant::Aggregate) return false;

    for(auto value: base[constant.aggregate()]->values.contents(base)) {
        if(holdsText(base, value)) return true;
    }

    return false;
}

// The argument is in own.h, beside the declaration.
bool constantNeedsAllocation(DeclBase base, const Constant& constant) {
    if(constant.kind != Constant::Aggregate) return false;
    if(!constant.type) return false;

    auto& parts = *base[constant.aggregate()];

    // A fixed array's elements are the type repeated, so any one of them answering is the array
    // answering. There is no boxed *element*: `[T *n]` has no edge to put a pointer on, which is
    // `breakCycles`' own rule one file up.
    if(base[constant.type]->kind == Type::Array) {
        for(auto value: parts.values.contents(base)) {
            if(constantNeedsAllocation(base, value)) return true;
        }

        return false;
    }

    if(base[constant.type]->kind != Type::Record) return false;

    auto& record = *(RecordType*)base[constant.type];
    if(parts.constructor >= record.cons.size()) return false;

    // The constructor's own field range, in order, which is exactly what `values` fills - see
    // `AggregateConstant`. A field the constant did not provide took its default, and a default is a
    // constant too, so this walk sees every value the aggregate is made of.
    auto arm = record.cons.get(base, parts.constructor);
    auto count = min(Size(arm.count), parts.values.size());

    for(Size i = 0; i < count; i++) {
        auto field = record.field(base, U16(arm.first + i));
        if(indirectField(base, field)) return true;
        if(constantNeedsAllocation(base, parts.values.get(base, i))) return true;
    }

    return false;
}

/*
 * The argument is in own.h, beside the declaration; `reaching` is the whole of what is written here.
 *
 * A record can contain itself through a box - `breakLayoutCycles` is what gives
 * `data List = Nil | Cons {tail: List}` a layout - so the walk needs somewhere to notice it has been
 * at a type before. What the guard keeps is that a type already on the stack is *already being
 * asked*: it reached here through a field of itself, and every box under it is a box the frame below
 * this one will find.
 *
 * This used to be a depth cut at sixteen answering `true` past it. That terminated, because a
 * containment cycle has a boxed edge and `indirectField` stops at the first one long before the
 * bound - but it also answered `true` for a legitimately deep, acyclic, box-free chain of records,
 * which turns `Program::asksBoxProvenance` on program-wide for a program with no box in it.
 * `summary.cpp`'s `reachTeardown` walks the same `tearsDownField` structure with the same stack, so
 * a reader meeting one of them now finds the other written the same way.
 */
static bool reachesBoxUnder(DeclBase base, DeclPtr<Type> type, SmallArray<DeclPtr<Type>, 16>& reaching) {
    if(!type) return true;

    switch(base[type]->kind) {
        case Type::Variable:
            return true;

        // An array's slots are its element repeated, so a box in one is a box in the array.
        case Type::Array:
            return reachesBoxUnder(base, ((ArrayType*)base[type])->element, reaching);

        case Type::Record: break;
        default: return false;
    }

    for(auto entry: reaching) {
        if(entry == type) return false;
    }

    reaching.push(type);
    PopOnExit<DeclPtr<Type>> leaving { reaching };

    auto& record = *(RecordType*)base[type];

    // `tearsDownField` and not the field list, because what this answers is a question about the
    // *teardown*: a field the record does not own, or one whose type owes nothing, is a field the
    // derived walk never descends into and so never releases a box through.
    for(auto field: record.fields.contents(base)) {
        if(!tearsDownField(base, field)) continue;
        if(indirectField(base, field)) return true;
        if(reachesBoxUnder(base, field.occurrence.type, reaching)) return true;
    }

    return false;
}

bool reachesBox(DeclBase base, DeclPtr<Type> type) {
    SmallArray<DeclPtr<Type>, 16> reaching;
    return reachesBoxUnder(base, type, reaching);
}

bool breakLayoutCycles(Context& context, Program& program) {
    CycleBreaker breaker { context, program, *program.decl };
    breaker.run();
    return breaker.errors == 0;
}

void breakRecordLayout(Context& context, Program& program, RecordType& record) {
    CycleBreaker breaker { context, program, *program.decl };
    /*
     * A **tuple** made after the pass cannot need a cut, and this is where that is checked rather
     * than assumed.
     *
     * Every field type it was built out of already exists, so each of them has been through the walk
     * - a declaration by the pass, an instantiation by the call above this one - and a type that has
     * been cut is one this walk stops at. So the rebuild `breakTuple` would perform has nothing to
     * rebuild, and an answer that is not this same tuple means a type reached here uncut, which is
     * an ordering bug rather than a tuple that needed boxing.
     */
    if(record.tuple) {
        auto answer = breaker.breakTuple(record, record.source);
        assertTrue(answer == asBase<Type>(DeclPtr<RecordType>(&record - *program.decl)));
        return;
    }

    breaker.breakRecord(record, record.source);
}

bool classifyTypes(Context& context, Program& program) {
    Classifier classifier { context, program, *program.decl };
    auto ok = classifier.run();

    // From here on a record made by a type application classifies itself - see `classifyRecord`.
    // Set whether or not the pass reported, because what it records is that every declaration has
    // been seen, which a diagnostic does not change.
    program.typesClassified = true;
    return ok;
}

bool classifyRecord(Context& context, Program& program, RecordType& record) {
    Classifier classifier { context, program, *program.decl };
    classifier.classify(record);
    return classifier.errors == 0;
}

bool disposalPermitted(Context& context, Program& program, DeclPtr<Type> type,
                       DeclPtr<Module> from) {
    auto base = *program.decl;

    auto instance = declaredInstanceFor(context, program, intrinsic::Known::drop, type);
    if(!instance) return true;

    // The *instance's* module rather than the type's, and they can differ: the orphan rule admits an
    // instance in the module of any type in the head, and what `@local` is written on is the
    // instance.
    auto& declared = *base[instance];
    return !declared.local || declared.module == from;
}

bool foldRecordSchema(Context& context, Program& program, RecordType& record) {
    auto decl = *program.decl;
    U32 errors = 0;

    // One slot per external field, at the path that reaches it. In field order, which is the
    // order `Analysis::slotAt` and `selectPositions` compare paths in.
    SmallArray<ExternalSlot, 8> slots;

    for(auto field: record.fields.contents(decl)) {
        if(!ownsField(field)) {
            ExternalSlot slot;
            slot.path.push(program.decl, PathStep::field(field.index));
            slot.contract = field.occurrence.contract;
            slots.push(slot);
            continue;
        }

        /*
         * An owned field whose *occurrence* names an interior position - `at: %buf'U8`.
         *
         * The field is an owned scalar and its root contract says so; what the tick qualifies is one
         * position *inside* it, which is exactly what `Occurrence::shape` carries. So the record
         * inherits those positions at the field's own path, one step deeper - `[Field(i), Pointee]`
         * for a pointer, and whatever the occurrence said for anything else.
         *
         * Prepending rather than re-deriving, because the occurrence already resolved the origin in
         * *this record's* slot scope: `declareFields` reads a field through `resolveOccurrence` with
         * the record's scope, so the slot numbers are already the ones the schema is written in.
         *
         * Distinct from the refusal below, which is about a field whose **type** carries a schema
         * the record never named. Here the record did name it, on the field, in its own scope.
         */
        if(auto interior = field.occurrence.shape) {
            for(auto slotPointer: decl[interior]->slots.contents(decl)) {
                auto& inner = *decl[slotPointer];

                ExternalSlot slot;
                slot.path.push(program.decl, PathStep::field(field.index));
                for(auto step: inner.path.contents(decl)) slot.path.push(program.decl, step);

                slot.contract = inner.contract;
                slots.push(slot);
            }

            continue;
        }

        /*
         * An owned field whose *type* refers to storage somebody else keeps.
         *
         * `data Outer {inner: Reader}` where `Reader` has an external position: the outer record
         * inherits an origin requirement it does not name, and there is nowhere for a use site to
         * say what fills it. The spelling that would is a loan argument on the field's type -
         * `inner: Reader(a')` - which a *signature* reads now and a field does not: the record's own
         * schema would gain `Reader`'s slots at a deeper path, so the fold would have to run on
         * demand over a graph this pass walks flat. Refused by name rather than accepted with the
         * slot silently dropped.
         */
        if(schemaOf(decl, field.occurrence.type)) {
            errors++;
            context.diagnostics.error(
                "%@ holds a %@, which refers to storage the record does not name"_v,
                record.source, context.findName(record.name),
                typeText(context, decl, field.occurrence.type));
        }
    }

    record.schema = program.shapes.intern(slots.pointer(), slots.size());
    return errors == 0;
}

/*
 * What one qualified occurrence's **surviving bytes** still owe - see `checkQualifiedShapes`, where
 * the rule is.
 *
 * Three sources, and the first two are why this is not a fold over the surviving fields alone. An
 * **authored** teardown takes the whole value, so it would run over the borrowed position however
 * few fields are left; and a type that is not a record has no fields for a shape to name, so a shape
 * over one is a shape the walk cannot subtract from. What is left is the ordinary case: a field the
 * shape did not remove, which owes exactly what it owes.
 *
 * **A declared `Copy` is not one of the three, and that division is the rule rather than an
 * omission.** A teardown is something the compiler inserts, so an occurrence that would owe a
 * partial one has no site to blame and has to be refused where it is written. A duplication is
 * something a use site *asks for*: the bytes of a qualified value duplicate correctly - that is
 * what the field walk below establishes - and what an authored `copy` would duplicate instead is
 * the storage behind the borrow, at the logical type, because a borrow handed to a bare variable
 * binds what it points at. So it is refused at the call that asks, by `checkCarriedOrigins`, which
 * is one rule covering every position such a value can reach rather than a second reading of this
 * one. Counting it here cost `Map.find`, `findMut` and `pairs` for a `Copy(Entry(k, v))` nothing on
 * those paths ever calls.
 */
static bool residualObligation(Context& context, Program& program,
                               const Occurrence& occurrence) {
    auto decl = *program.decl;
    auto type = occurrence.type;
    if(decl[type]->kind != Type::Record) return true;

    auto& record = *(RecordType*)decl[type];

    // The type's own teardown, which replaces the derived walk rather than running beside it - so a
    // record that has one is a record whose members this rule cannot reason about.
    if(record.ownership.explicitDrop) return true;
    if(declaredInstanceFor(context, program, intrinsic::Known::drop, type)) return true;
    if(declaredInstanceFor(context, program, intrinsic::Known::reclaim, type)) return true;

    for(auto field: record.fields.contents(decl)) {
        if(fieldPosition(decl, occurrence.shape, field.index)) continue;

        auto ownership = ownershipOf(decl, field.occurrence.type);
        if(ownsField(field) && (ownership.needsTeardown() || !ownership.trivialCopy)) return true;
    }

    return false;
}

static bool checkQualifiedOccurrence(Context& context, Program& program,
                                     const Occurrence& occurrence, LocationId source) {
    auto decl = *program.decl;
    if(!occurrence.type) return true;
    if(!qualifiesInterior(decl, occurrence)) return true;
    if(!residualObligation(context, program, occurrence)) return true;

    context.diagnostics.error(
        "a qualified type argument may only be written where nothing is left to release or duplicate, and %@ still owes one - qualify the whole value instead"_v,
        source, typeText(context, decl, occurrence.type));
    return false;
}

bool checkQualifiedShapes(Context& context, Program& program) {
    auto decl = *program.decl;
    auto ok = true;

    for(auto modulePointer: program.modules) {
        auto module = decl[modulePointer];

        for(auto functionPointer: module->functions.contents(decl)) {
            auto& function = *decl[functionPointer];

            for(auto argumentPointer: function.args.contents(decl)) {
                auto& argument = *decl[argumentPointer];
                ok = checkQualifiedOccurrence(context, program, argument.occurrence,
                                              argument.source) && ok;
            }

            ok = checkQualifiedOccurrence(context, program, function.result, function.source) && ok;
            ok = checkQualifiedOccurrence(context, program, function.handover, function.source) && ok;
        }
    }

    return ok;
}

bool foldSchemas(Context& context, Program& program) {
    auto decl = *program.decl;
    auto ok = true;

    for(auto modulePointer: program.modules) {
        auto module = decl[modulePointer];

        for(auto recordPointer: module->records.contents(decl)) {
            ok = foldRecordSchema(context, program, *decl[recordPointer]) && ok;
        }
    }

    return ok;
}

} // namespace mir
