#include "summary.h"
#include "specialize.h"

namespace mir {

namespace {

/*
 * Whether one node is a suspension point in its own right.
 *
 * An exhaustive switch and not a `return false`, for the reason `hasTeardown` is written the same
 * way: the answer is empty today and the first kind that changes it is exactly the kind that would
 * otherwise be added without anyone deciding. `yield` is milestone 9 and is what turns a row here
 * into a `true`.
 *
 * A *call* is deliberately false here. Whether calling something suspends is the callee's published
 * answer - `maySuspend`, which every later pass reads - and that is the whole reason inst.def has no
 * `kMirSuspends` column: a kind-level bit could only say "every call" or "no call". The fixpoint
 * below does not ask it either: a call is an *edge*, and the propagation walks the edges.
 */
static bool suspendsAt(Node& node) {
    switch(node.kind) {
        case Node::Argument:
        case Node::Const:
        case Node::Read:
        case Node::Binary:
        case Node::Unary:
        case Node::Aggregate:
        case Node::Call:
        case Node::Write:
        case Node::Return:
        case Node::Jump:
        case Node::Yield:
        case Node::Branch:
        case Node::Unreachable:
            return false;
        case Node::KindCount:
            break;
    }

    // Not a fallback. A kind this has no answer for is one that was added without deciding whether
    // it can suspend, and the safe reading of an undecided suspension point is that it is one.
    assertTrue(false);
    return true;
}

struct Interprocedural {
    Context& context;
    Program& program;
    DeclBase decl;
    MirBase mir;

    /*
     * The functions the program can reach, in discovery order, and the index that says which.
     *
     * `indexOf` maps a function to its position here, which is what makes the call graph below a
     * pair of dense arrays rather than a map of handles. One table and not two: the set and the
     * numbering are the same question, so a membership test that could disagree with a numbering
     * does not exist.
     */
    Array<DeclPtr<Function>> reached;
    HashMap<U32, U32> indexOf;

    /*
     * The call graph, collected once while the bodies are walked - and it **is** stored, which
     * reverses an earlier decision here.
     *
     * The argument for not storing it was that the edges are the `Call` nodes and a copy is a second
     * thing to keep in step. What that cost is a fixpoint that re-scanned every reachable body on
     * every round: one callee changing its answer late re-reads the whole program, so a call chain
     * of length n costs n rounds over n bodies. That is quadratic in the size of the program, in a
     * pass every compilation runs, for a saving of one array.
     *
     * The "second thing to keep in step" objection does not apply, because this is not a cache: it
     * is built by the walk that establishes reachability, in the same pass, from bodies nothing is
     * allowed to rewrite while summaries are being computed, and it is discarded when the pass ends.
     *
     * `edges` is (caller << 32 | callee) in discovery order; `callerStart`/`callerList` is that
     * relation reversed, in CSR, which is what a worklist over "who has to be reconsidered when this
     * function's answer changes" reads.
     */
    Array<U64> edges;
    Array<U32> callerStart;
    Array<U32> callerList;

    Interprocedural(Context& context, Program& program):
        context(context), program(program), decl(*program.decl), mir(*program.mir) {}

    static U64 edgeOf(U32 caller, U32 callee) { return (U64(caller) << 32) | callee; }

    /*
     * What the program can reach, from its roots.
     *
     * The roots are the entry where the program has one and every declaration where it does not,
     * which is the program/library split Analysis-IR.md §11.4 names: a library's roots are its
     * exports, and until `pub` is implemented every declaration is exported. Stating it this way
     * means the day exports arrive changes this list and nothing else.
     */
    void markReachable() {
        Size next = 0;

        auto root = [&](DeclPtr<Function> function) {
            auto entry = indexOf.add(function.offset);
            if(!entry.existed) {
                *entry.value = U32(reached.size());
                reached.push(function);
            }

            return *entry.value;
        };

        if(program.entry) {
            root(program.entry);
        } else {
            for(auto modulePointer: program.modules) {
                auto module = decl[modulePointer];

                /*
                 * A generic declaration is never a root, whatever a library exports.
                 *
                 * There is no code for one: its parameters are unbound, so it has no layout, no ABI
                 * and nothing to lower. What a library exports of it is the *body*, for a consumer
                 * to specialize; the erased ABI is what gives it a second, callable form, and that
                 * is a later waypoint with a real answer rather than this one silently lowering a
                 * type nothing can manifest.
                 */
                for(auto pointer: module->functions.contents(decl)) {
                    if(isGeneric(decl, decl[pointer]->gen)) continue;
                    root(pointer);
                }

                for(auto pointer: module->globals.contents(decl)) decl[pointer]->reachable = true;
            }
        }

        // Index-based rather than a separate worklist: `reached` *is* the worklist, and it is also
        // the answer, so the two cannot come to hold different sets.
        while(next < reached.size()) {
            auto caller = U32(next);
            auto function = decl[reached[next++]];
            function->reachable = true;
            if(!function->mir) continue;

            auto& body = *mir[function->mir];

            for(U32 i = 0; i < body.nodes.size(); i++) {
                auto node = body.node(mir, NodeId(i));
                if(node->kind != Node::Call) continue;

                /*
                 * `callTarget` and not the node's own callee - `specialize.h`.
                 *
                 * A call to a generic names the generic, and which *specialization* it reaches
                 * depends on the substitution this body is being read under. That is the walk that
                 * makes specializations exist at all: reachability is what discovers which argument
                 * lists a program actually calls at, so asking here is what creates them, and no
                 * separate instantiation pass has to agree with this one about the answer.
                 */
                auto target = callTarget(context, program, *function, *(NodeCall*)node);

                // A generic is never reached, on the same terms it is never a root: it has no
                // layout, no ABI and nothing to lower. `callTarget` answers with one where it cannot
                // make a specialization, which is only below a diagnostic - and walking its body
                // then would be walking one whose types no argument list has bound.
                if(isGeneric(decl, decl[target]->gen)) continue;

                edges.push(edgeOf(caller, root(target)));
            }

            /*
             * The teardowns this body places - `Elaboration::drops`.
             *
             * An `instance Drop(T)` is reached by an *object dying*, not by a call: nothing in any
             * body names it, so a walk that only followed `Call` nodes would leave its function
             * unconverted and the drop the conversion emits with nothing to call. That is why this
             * pass runs after `analyzeProgram` rather than before it - the drop list is what says
             * which teardowns the program can reach, and it does not exist until the drops are
             * placed.
             *
             * They are **call graph edges**, not merely roots. A teardown that suspends suspends the
             * body that placed it just as surely as one it wrote a call to, and a graph that held
             * only the `Call` nodes would answer no. That the old fixpoint agreed here was luck: it
             * asked `maySuspend` per node, so it never saw a drop either.
             */
            if(body.elaboration) {
                auto& elaboration = *mir[body.elaboration];
                auto with = substitutionOf(decl, *function);

                for(auto step: elaboration.drops.contents(mir)) {
                    // The path's type is the *generic* body's, so which teardown actually runs is a
                    // question about this specialization. Substituted here for the same reason the
                    // conversion substitutes before it emits one: the placement is the generic
                    // body's answer and the code is this function's.
                    auto held = substituteType(context, program,
                                               elaboration.path(mir, step.path)->type, with);

                    reachTeardown(held, [&](DeclPtr<Function> target) {
                        edges.push(edgeOf(caller, root(target)));
                    });
                }
            }

            /*
             * The globals this body names, from the place table.
             *
             * Over the places rather than over the nodes, because a global is reached through a
             * *place* whatever the node that names it does with it - a read, a write, an address
             * taken - and a walk over nodes would have to enumerate which kinds those are.
             */
            for(U32 i = 0; i < body.places.size(); i++) {
                auto place = body.place(mir, PlaceId(i));
                if(place->kind == Place::Global) {
                    decl[DeclPtr<Global>(place->payload)]->reachable = true;
                }
            }
        }
    }

    /*
     * The call graph reversed, in compressed sparse row.
     *
     * Counting sort into one array rather than a list per function: a per-callee `Array` is a heap
     * allocation per function in a program's whole call graph, for a relation that is read once.
     */
    void buildCallers() {
        auto count = U32(reached.size());

        callerStart.clear();
        for(U32 i = 0; i <= count; i++) callerStart.push(0);
        for(auto edge: edges) callerStart[U32(edge) + 1]++;
        for(U32 i = 0; i < count; i++) callerStart[i + 1] += callerStart[i];

        callerList.clear();
        for(Size i = 0; i < edges.size(); i++) callerList.push(0);

        // A second copy of the starts, consumed as the fill advances. The prefix sums have to
        // survive it: they are the row bounds every reader uses.
        SmallArray<U32, 64> cursor;
        for(U32 i = 0; i < count; i++) cursor.push(callerStart[i]);

        for(auto edge: edges) callerList[cursor[U32(edge)]++] = U32(edge >> 32);
    }

    /*
     * The functions that run when a value of this type dies.
     *
     * The same walk the conversion makes when it emits the teardown, asked for the names rather than
     * for the instructions - which is what a second walk here has to be, and is why both read
     * `ownershipOf` for the halves and `tearsDownField` for the children rather than each deciding
     * what a derived teardown recurses into. A record cannot contain itself (`classifyTypes` refuses
     * one that does), so the recursion terminates without a visited set.
     */
    template<class F>
    void reachTeardown(DeclPtr<Type> type, F&& root) {
        auto ownership = ownershipOf(decl, type);
        if(!ownership.needsTeardown()) return;

        auto authored = [&](CoreClass klass, TeardownKind kind) {
            if(kind != TeardownKind::Authored) return;

            // The *member*, through `teardownMember`, and not the instance's declaration: a
            // parametric head supplies a generic body and what runs is its specialization. Asking
            // the shared question here is what makes the function the reachability keeps the one
            // the lowering later names.
            if(auto member = teardownMember(context, program, klass, type, kNullLocation)) {
                root(member);
            }
        };

        authored(CoreClass::Drop, ownership.drop);
        authored(CoreClass::Reclaim, ownership.reclaim);

        // A derived half recurses into the members; an authored one replaces the walk rather than
        // running beside it, which is what makes this a test of each half rather than of the type.
        if(decl[type]->kind != Type::Record) return;
        if(ownership.drop == TeardownKind::Authored &&
           ownership.reclaim == TeardownKind::Authored) return;

        for(auto field: ((RecordType*)decl[type])->fields.contents(decl)) {
            if(!tearsDownField(decl, field)) continue;
            reachTeardown(field.occurrence.type, root);
        }
    }

    /*
     * Whether one body suspends **in its own right** - a node that is a suspension point.
     *
     * Deliberately not "or calls something that may": that half is the call graph, and asking it
     * here is what made the fixpoint a rescan. One walk per function, before the propagation, and
     * never again.
     */
    bool bodySuspendsItself(Function& function) {
        if(!function.mir) return false;

        auto& body = *mir[function.mir];
        for(U32 i = 0; i < body.nodes.size(); i++) {
            if(suspendsAt(*body.node(mir, NodeId(i)))) return true;
        }

        return false;
    }

    /*
     * Whether calling a function may suspend the enclosing fiber - Analysis-Sharing.md §3.4.
     *
     * Inferred and published rather than written, because the across-suspend exclusivity rule needs
     * the answer on native whether or not JavaScript needs it to choose a lowering, and a marker in
     * the signature would be a difference in what the programmer writes between two targets.
     *
     * The fixpoint runs **in the summaries themselves**, at `Knowledge::Assumed`, which is what that
     * state is for: `Fact::get` treats an assumption as an answer and `isKnown` does not, so nothing
     * may publish or cache a provisional value. A parallel table of provisional answers would have
     * been a second place the question is asked, and the two would answer differently for a callee
     * that is mid-fixpoint - which is exactly when it matters.
     *
     * The *schedule* is a worklist over the reversed call graph rather than rounds over every body:
     * a function whose answer becomes true is the only reason a caller has to be looked at again, so
     * that is what is queued. Monotone, so it terminates and each function is queued at most once: a
     * function starts false, only ever becomes true, and one with no body starts true because §3.5's
     * rule for an unknown target is that it joins as may-suspend.
     */
    void computeSuspension() {
        Array<U32> work;
        Size next = 0;

        for(U32 i = 0; i < reached.size(); i++) {
            auto function = decl[reached[i]];
            auto initial = !function->summary.bodyKnown || bodySuspendsItself(*function);

            function->summary.setSuspension(initial, Knowledge::Assumed);
            if(initial) work.push(i);
        }

        while(next < work.size()) {
            auto index = work[next++];

            for(auto slot = callerStart[index]; slot < callerStart[index + 1]; slot++) {
                auto caller = decl[reached[callerList[slot]]];
                if(caller->summary.suspension().value) continue;

                caller->summary.setSuspension(true, Knowledge::Assumed);
                work.push(callerList[slot]);
            }
        }

        /*
         * Published, which is a different statement from computed.
         *
         * A function whose body is available has a settled answer and publishes `Known`. One without
         * a body has an *assumption* - "may suspend", made by this pass on no evidence - and
         * publishing it as `Known` would let a module image carry a fact nobody established.
         */
        for(auto pointer: reached) {
            auto function = decl[pointer];
            if(!function->summary.bodyKnown) continue;

            function->summary.setSuspension(function->summary.suspension().value, Knowledge::Known);
        }
    }

    void run() {
        markReachable();
        buildCallers();
        computeSuspension();
    }
};

} // namespace

void computeSummaries(Context& context, Program& program) {
    Interprocedural pass(context, program);
    pass.run();
}

} // namespace mir
