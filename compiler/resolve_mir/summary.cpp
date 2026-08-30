#include "summary.h"

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
 * answer - `maySuspend`, which the fixpoint below reads - and that is the whole reason inst.def has
 * no `kMirSuspends` column: a kind-level bit could only say "every call" or "no call".
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
     * The functions the program can reach, in discovery order, and the set that says which.
     *
     * This is the whole call graph the fixpoint needs. There is deliberately no stored edge list:
     * the edges are the `Call` nodes, they are already in the bodies, and a second copy of them
     * would be a second thing to keep in step with the IR. What a stored graph would buy is
     * re-queueing only the callers of a function whose answer changed, and the round-based walk
     * below is within a constant factor of that on call graphs of this shape.
     */
    Array<DeclPtr<Function>> reached;
    HashMap<U32, bool> seen;

    Interprocedural(Context& context, Program& program):
        context(context), program(program), decl(*program.decl), mir(*program.mir) {}

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
            auto entry = seen.add(function.offset);
            if(entry.existed) return;

            *entry.value = true;
            reached.push(function);
        };

        if(program.entry) {
            root(program.entry);
        } else {
            for(auto modulePointer: program.modules) {
                auto module = decl[modulePointer];
                for(auto pointer: module->functions.contents(decl)) root(pointer);
                for(auto pointer: module->globals.contents(decl)) decl[pointer]->reachable = true;
            }
        }

        // Index-based rather than a separate worklist: `reached` *is* the worklist, and it is also
        // the answer, so the two cannot come to hold different sets.
        while(next < reached.size()) {
            auto function = decl[reached[next++]];
            function->reachable = true;
            if(!function->mir) continue;

            auto& body = *mir[function->mir];

            for(U32 i = 0; i < body.nodes.size(); i++) {
                auto node = body.node(mir, NodeId(i));
                if(node->kind == Node::Call) root(((NodeCall*)node)->callee);
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
             */
            if(body.elaboration) {
                auto& elaboration = *mir[body.elaboration];
                for(auto step: elaboration.drops.contents(mir)) {
                    reachTeardown(elaboration.path(mir, step.path)->type, root);
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
     * The functions that run when a value of this type dies.
     *
     * The same walk the conversion makes when it emits the teardown, asked for the names rather than
     * for the instructions - which is what a second walk here has to be, and is why both read
     * `ownershipOf` rather than each deciding what a derived teardown recurses into. A record cannot
     * contain itself (`classifyTypes` refuses one that does), so the recursion terminates without a
     * visited set.
     */
    template<class F>
    void reachTeardown(DeclPtr<Type> type, F&& root) {
        auto ownership = ownershipOf(decl, type);
        if(!ownership.needsTeardown()) return;

        auto authored = [&](CoreClass klass, TeardownKind kind) {
            if(kind != TeardownKind::Authored) return;

            auto instance = program.findInstance(klass, type);
            if(instance) root(decl[instance]->function);
        };

        authored(CoreClass::Drop, ownership.drop);
        authored(CoreClass::Reclaim, ownership.reclaim);

        // A derived half recurses into the members; an authored one replaces the walk rather than
        // running beside it, which is what makes this a test of each half rather than of the type.
        if(decl[type]->kind != Type::Record) return;
        if(ownership.drop == TeardownKind::Authored &&
           ownership.reclaim == TeardownKind::Authored) return;

        for(auto field: ((RecordType*)decl[type])->fields.contents(decl)) {
            reachTeardown(field.type, root);
        }
    }

    // Whether one body reaches a suspension point: one of its own, or a call to something that may.
    // `maySuspend` is the only reader of the published fact, here and everywhere else.
    bool bodySuspends(Function& function) {
        if(!function.mir) return false;

        auto& body = *mir[function.mir];
        for(U32 i = 0; i < body.nodes.size(); i++) {
            auto node = body.node(mir, NodeId(i));
            if(suspendsAt(*node) || maySuspend(decl, *node)) return true;
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
     * state is for: `Fact::get` treats an assumption as an answer and `isKnown` does not, so a round
     * reads its own provisional values through the same `maySuspend` every later pass will use, and
     * nothing may publish or cache one. A parallel table of provisional answers would have been a
     * second place the question is asked, and the two would answer differently for a callee that is
     * mid-fixpoint - which is exactly when it matters.
     *
     * Monotone, so it terminates: a function starts false, only ever becomes true, and one with no
     * body starts true because §3.5's rule for an unknown target is that it joins as may-suspend.
     */
    void computeSuspension() {
        for(auto pointer: reached) {
            auto function = decl[pointer];
            function->summary.suspension.set(!function->summary.bodyKnown, Knowledge::Assumed);
        }

        auto changed = true;
        while(changed) {
            changed = false;

            for(auto pointer: reached) {
                auto function = decl[pointer];
                if(function->summary.suspension.value) continue;
                if(!bodySuspends(*function)) continue;

                function->summary.suspension.set(true, Knowledge::Assumed);
                changed = true;
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

            function->summary.suspension.set(function->summary.suspension.value, Knowledge::Known);
        }
    }

    void run() {
        markReachable();
        computeSuspension();
    }
};

} // namespace

void computeSummaries(Context& context, Program& program) {
    Interprocedural pass(context, program);
    pass.run();
}

} // namespace mir
