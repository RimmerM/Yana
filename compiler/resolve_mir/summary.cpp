#include "summary.h"
#include "specialize.h"

namespace mir {

namespace {

/*
 * Whether one node is a suspension point in its own right.
 *
 * An exhaustive switch and not a `return false`, for the reason `hasTeardown` is written the same
 * way: the answer is empty today and the first kind that changes it is exactly the kind that would
 * otherwise be added without anyone deciding. A `yield` across a **stack switch** - a fiber's, which
 * is what `lib/Fiber` keeps the switch for - is what turns a row here into a `true`; an iterator
 * object's step is an ordinary call and is not one, and neither is `lens.call`, because a lens call
 * is a call - what
 * suspends is what the callee does, which is `maySuspend` on it and is what the edge walk reads.
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
        case Node::GenValue:
        case Node::Read:
        case Node::Binary:
        case Node::Unary:
        case Node::Aggregate:

        case Node::FunValue:

        // The address of storage in hand, and a measurement of a type. Neither reaches anything that
        // could run: a metric is an immediate or a load out of a descriptor the caller supplied.
        case Node::Address:
        case Node::Metric:

        // A request for slots. What answers it is an allocation - the frame's, or the allocator's,
        // and neither is a Yana function that could hand control over. The library's `runSlots` is
        // a call and this is what replaced it, which is the one difference worth noting here.
        case Node::RunSlots:

        // A machine operation runs and comes back: the kernel is not a Yana function, so nothing
        // reachable through one can hand over.
        case Node::Native:

        // And a host operation, for the same reason one target further out: what runs is the
        // engine's, so it may do anything except hand control to a Yana continuation - which is
        // what suspending is. `hostThrow` leaves without coming back, and that is not a suspension
        // either: nothing resumes.
        case Node::Host:

        // An atomic operation is an instruction and a memory ordering. It may make a thread wait for
        // a cache line; it may not hand control to a Yana function, which is what suspending is.
        case Node::Atomic:
        case Node::AtomicOk:

        // A branch-free choice and the vector operations: arithmetic in a register, with nothing in
        // any of them to hand control over to.
        case Node::Fma:
        case Node::Select:
        case Node::Map:
        case Node::VecSplat:
        case Node::VecLane:
        case Node::VecWithLane:
        case Node::VecShuffle:
        case Node::VecReduce:
        case Node::VecIota:
        case Node::Call:
        case Node::Write:
        case Node::Return:
        case Node::Jump:
        case Node::Break:
        case Node::Resume:
        case Node::LensCall:
        case Node::LensTry:
        case Node::Branch:
        case Node::Switch:
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
    /*
     * The **invocation** relation - caller to a body that *runs* inside the caller's activation -
     * and nothing else. This is what suspension propagates over, and it is a different relation from
     * reachability, which is what `reached` and `root` keep: a body that merely *constructs* a
     * function value, stores it, returns it or passes it on reaches the declaration it names and
     * does not run it, so the declaration is code the program needs and not an effect the
     * constructor has. One relation used to serve both, drawn from the construction site because
     * that is where a declaration is named, and every function that built a suspending value would
     * have been a suspending function.
     *
     * So an edge here is one of: a direct call; a teardown the body placed; a call through a value,
     * attributed to every function whose value flows to that function *type* (`flowsTo`, which is
     * Analysis-Sharing.md §3.5's flow set at the granularity of the type); and, for a call handing
     * an environment to a body compiled once, the witnesses in the slots that body actually reads
     * (`usedSlots`) rather than every slot it was handed.
     */
    Array<U64> edges;
    Array<U32> callerStart;
    Array<U32> callerList;

    /*
     * Which functions a value of one function type can be - the declaration named by every
     * `fun.value` built at that type, keyed by the interned type. Whole-program, which is the stated
     * compilation model; a value that arrives as a parameter is of a type some construction site in
     * the program built, or of one nothing built, and a type nothing built is a call that runs
     * nothing this program has.
     */
    HashMap<U32, SmallArray<DeclPtr<Function>, 2>> flowsTo;

    // A call through a value, held until every construction site has been seen - which is when the
    // worklist has drained, since a `fun.value` is found by walking bodies.
    struct ValueCall {
        U32 caller;
        U32 type;
    };

    Array<ValueCall> valueCalls;

    // A call handing an environment over, held for the same reason: which of its slots the erased
    // body reads is found by walking that body, and the body may be behind this call in the walk.
    struct EnvironmentUse {
        U32 caller;
        DeclPtr<GenEnvironment> environment;
    };

    Array<EnvironmentUse> environmentUses;

    /*
     * The same call edges again, with the **node** on them - what the per-argument facts read.
     *
     * `edges` is the invocation relation and nothing else, which is all suspension, placement and
     * divergence need; retention, mutation and consumption are per *position*, so they need the call
     * the edge came from. A second array rather than a wider `edges`, because the suspension
     * worklist reads `edges` once per round over the whole program and paying two words per teardown
     * edge for a field three passes read would be paying it there.
     *
     * Recorded during the reachability walk rather than by a second one: which function a call
     * reaches is `callTarget`'s answer, and that walk is also what *makes* the specialization, so a
     * second traversal asking again would either duplicate the discovery or have to trust it.
     */
    struct CallEdge {
        U32 caller;
        U32 callee;
        NodeId node;
    };

    Array<CallEdge> callEdges;

    /*
     * Which callers made a call this pass cannot attribute to a position - an indirect call, a
     * witness slot, a generic with no specialization.
     *
     * One flag per caller and not per call: what it means is that the *body* passed arguments
     * somewhere unattributable, so every per-argument fact for it starts at the top of its lattice.
     * That is the same fallback `bodyKnown == false` gets one level out, and it is stated here
     * because the reachability walk is where the unattributable call is recognized.
     */
    Array<U8> opaque;

    void markOpaque(U32 caller) {
        while(opaque.size() <= caller) opaque.push(0);
        opaque[caller] = 1;
    }

    bool isOpaque(U32 caller) const { return caller < opaque.size() && opaque[caller] != 0; }

    /*
     * Per body compiled once, the slots of its environment it reads - one bit per slot of its frozen
     * schema: the class and callable slots its calls go through, the type slots whose descriptor
     * teardown a drop of a value of that variable runs. Filled while the erased body is walked, read
     * once the walk is over.
     */
    HashMap<U32, SmallArray<U64, 2>> usedSlots;

    Interprocedural(Context& context, Program& program):
        context(context), program(program), decl(*program.decl), mir(*program.mir) {}

    static U64 edgeOf(U32 caller, U32 callee) { return (U64(caller) << 32) | callee; }

    /*
     * The globals a reachable global's *initializer* names - `Constant::Address`.
     *
     * The walk above finds a global through a **place** in a body, which is every way a program can
     * reach one. This is the one way a global reaches another with no body between them: the bytes
     * of a string literal are a global of their own and the string's own constant points at them,
     * so a reader that stopped at the string would leave the bytes unemitted and the relocation
     * naming nothing.
     *
     * A worklist rather than one sweep, because an address is a leaf of an arbitrary tree and the
     * global it names may have an initializer of its own. Nothing builds a cycle - a constant is
     * finished before the global that names it exists - and the `reachable` flag is what bounds the
     * walk in any case.
     */
    /*
     * Every global an **operand-less** constant names, marked.
     *
     * A constant carried by a node is payload rather than an operand, so the place walk that finds
     * every other global a body reaches cannot see one: a `map`'s entries and a string literal's
     * value are both trees with an `Address` leaf in them, and neither is a place. Marking is the
     * whole of what is needed here - `reachGlobalAddresses` runs after this drain and follows
     * whatever the marked global's own initializer names.
     */
    void reachConstantAddresses(const Constant& constant) {
        if(constant.kind == Constant::Address) {
            decl[constant.asGlobal()]->reachable = true;
            return;
        }

        if(constant.kind != Constant::Aggregate) return;

        for(auto value: decl[constant.aggregate()]->values.contents(decl)) {
            reachConstantAddresses(value);
        }
    }

    void reachGlobalAddresses() {
        Array<DeclPtr<Global>> pending;

        auto reach = [&](const Constant& constant, auto&& again) -> void {
            if(constant.kind == Constant::Address) {
                auto named = constant.asGlobal();
                if(decl[named]->reachable) return;

                decl[named]->reachable = true;
                pending.push(named);
                return;
            }

            if(constant.kind != Constant::Aggregate) return;

            for(auto value: decl[constant.aggregate()]->values.contents(decl)) again(value, again);
        };

        for(auto modulePointer: program.modules) {
            for(auto pointer: decl[modulePointer]->globals.contents(decl)) {
                if(decl[pointer]->reachable) reach(decl[pointer]->initial, reach);
            }
        }

        while(pending.size()) {
            auto pointer = pending[pending.size() - 1];
            pending.pop();
            reach(decl[pointer]->initial, reach);
        }
    }

    /*
     * What the program can reach, from its roots.
     *
     * The roots are the entry where the program has one, and **the root module's** declarations
     * where it does not - which is the program/library split Analysis-IR.md §11.4 names. An imported
     * module contributes only what is reached from here, whatever it exports: a library that is
     * being *published* would root at its `pub` declarations instead, and that is a compilation this
     * stage has no mode for yet.
     *
     * The root module and not every module, which stopped being the same statement the day imports
     * arrived: rooting at every one would emit the whole of `lib/` into a dump of one file.
     */
    void markReachable() {
        Size next = 0;

        /*
         * Two questions rather than one: **walk this body's edges**, and **emit this function**.
         *
         * They used to be the same question and one caller needs them apart. An iterator object's
         * step has no body here and calls whatever the producer calls, so the producer's body is
         * what the edge walk has to read - and a producer nothing else calls is not itself in the
         * image, since the object form is the only one anything runs. Rooting it for its edges *and*
         * for itself left its continuation-passing form in the image with nothing calling it.
         *
         * `reachable` is set here rather than at the drain, so that a body already queued for its
         * edges becomes emitted the moment a real call reaches it.
         */
        auto mark = [&](DeclPtr<Function> function, bool emit) {
            auto entry = indexOf.add(function.offset);
            if(!entry.existed) {
                *entry.value = U32(reached.size());
                reached.push(function);
            }

            if(emit) decl[function]->reachable = true;
            return *entry.value;
        };

        auto root = [&](DeclPtr<Function> function) { return mark(function, true); };
        auto rootBody = [&](DeclPtr<Function> function) { return mark(function, false); };

        auto reachConstantFunctions = [&](const Constant& constant, auto&& again) -> void {
            if(constant.kind == Constant::Function) {
                root(constant.asFunction());
                return;
            }

            if(constant.kind != Constant::Aggregate) return;
            for(auto value: decl[constant.aggregate()]->values.contents(decl)) again(value, again);
        };

        /*
         * The allocator, where the program has a `@box` anywhere - `Program::usesBoxes` - or asks
         * for a run's slots anywhere, which is `Program::usesRuns`.
         *
         * A root and not an edge, and that is the honest shape of it: the calls these end up in are
         * emitted by the *lowering* - `box.cpp`'s construction and release - so there is no node in
         * any body for an edge to be drawn from. Design-Allocator.md §18.5's binding is what makes
         * them nameable at all; this is where they become reachable.
         *
         * `isHeapAddress` joins them because a build whose provider answers it releases boxes
         * through it, and a build whose provider does not has an unfilled role and roots nothing.
         * Which of the two a program is in is `storesBoxProvenance`, and it is decided before this
         * walk runs.
         */
        auto allocatorRooted = false;

        auto rootAllocator = [&] {
            if(allocatorRooted) return;
            if(!program.usesBoxes && !program.usesRuns && !program.usesHeapPlacement) return;
            allocatorRooted = true;

            intrinsic::Known allocator[3] = { intrinsic::Known::allocateHeap,
                                              intrinsic::Known::freeHeap,
                                              intrinsic::Known::isHeapAddress };

            for(auto role: allocator) {
                if(auto declared = program.roles.function(role)) root(declared);
            }
        };

        rootAllocator();

        /*
         * And the located check pair under `-check-locations`, for the allocator's reason exactly:
         * the call is built by the *lowering* (`Lowering::locatedCheck`), so no node in any body
         * names either declaration and neither this walk nor the SSA one could find an edge to it.
         */
        if(context.settings.checkLocations) {
            intrinsic::Known located[2] = { intrinsic::Known::checkFailedAt,
                                            intrinsic::Known::checkConditionAt };

            for(auto role: located) {
                if(auto declared = program.roles.function(role)) root(declared);
            }
        }

        if(program.entry) root(program.entry);

        /*
         * A compile that answers questions roots every function it could be asked about.
         *
         * An editor hovers over a function nothing calls yet, and `yana explain` names one; an answer
         * that depended on whether `main` happened to reach it would be an answer about the call
         * graph rather than about the function. Nothing is emitted from either compile, so what this
         * costs is the summaries of the program's own functions and nothing else - the library is
         * still reached only through them. A generic is not a root here for the reason it is not one
         * below.
         */
        if(context.index || context.settings.explaining()) {
            for(auto module: program.modules) {
                if(decl[module]->fromLibrary) continue;

                for(auto pointer: decl[module]->functions.contents(decl)) {
                    if(isGeneric(decl, decl[pointer]->gen)) continue;
                    root(pointer);
                }
            }
        }

        if(!program.entry) {
            auto module = decl[program.root()];

            /*
             * A generic declaration is never a root, whatever a library exports.
             *
             * There is no code for one: its parameters are unbound, so it has no layout, no ABI and
             * nothing to lower. What a library exports of it is the *body*, for a consumer to
             * specialize; the erased ABI is what gives it a second, callable form, and that is a
             * later waypoint with a real answer rather than this one silently lowering a type
             * nothing can manifest.
             */
            for(auto pointer: module->functions.contents(decl)) {
                if(isGeneric(decl, decl[pointer]->gen)) continue;
                root(pointer);
            }

            for(auto pointer: module->globals.contents(decl)) decl[pointer]->reachable = true;
        }

        // Index-based rather than a separate worklist: `reached` *is* the worklist, and it is also
        // the answer, so the two cannot come to hold different sets.
        while(true) {
            /*
             * Asked again on every turn, because this walk is also the walk that **specializes**: a
             * call discovered here makes a specialization, which substitutes a type, which can
             * instantiate a recursive one - and `breakLayoutCycles` boxing that instantiation's back
             * edge is the first moment the program is known to need the heap at all.
             *
             * So `usesBoxes` can turn true after the roots were taken. Rooting from inside the loop
             * is what lets the functions it names be drained like any other, rather than being
             * appended after the drain has finished and never lowered - which is the ordering
             * `roleFunction` reports as an internal error.
             */
            rootAllocator();

            /*
             * And a function a reachable global's initializer names - a code pointer in a `let`, a
             * record or a fixed array - asked only once the queue is empty, because only then is
             * the set of reachable globals settled for the bodies drained so far.
             *
             * Inside the drain rather than after it, which is where it was: a function rooted there
             * was marked emitted and never walked, so whatever it called and every global it named
             * stayed unreached - a `@convention(x86_interrupt)` handler reached only through its
             * slot in a table wrote a global the image did not have. Settled when a pass over the
             * globals adds no function, which it must eventually, since `root` adds each once.
             */
            if(next >= reached.size()) {
                program.discoveringFrom = nullptr;
                reachGlobalAddresses();

                for(auto modulePointer: program.modules) {
                    for(auto pointer: decl[modulePointer]->globals.contents(decl)) {
                        if(!decl[pointer]->reachable) continue;
                        reachConstantFunctions(decl[pointer]->initial, reachConstantFunctions);
                    }
                }
            }

            if(next >= reached.size()) break;

            auto caller = U32(next);
            program.discoveringFrom = reached[next];
            auto function = decl[reached[next++]];

            /*
             * A body this stage did not build - an iterator object's step, whose blocks are the
             * *producer's* and are converted a second time (`Function::generated`).
             *
             * There is no MIR here for the edge walk below to read, and what the step calls is
             * whatever the producer calls - so the producer's body is walked. **For its edges
             * only**: what runs is the step, and a producer nothing else calls has no
             * continuation-passing form in the image. A `for x in f(...)` anywhere is an ordinary
             * call and marks it emitted through `root`, so a producer used both ways is still one
             * body of each.
             */
            if(function->generatedFrom) rootBody(function->generatedFrom);

            // And the **step**, which is the one of the four such bodies the other three call. The
            // edge is in a body this stage did not build, so there is nothing for the walk below to
            // read it out of.
            /*
             * The **erased** steppers name no record at all: what they carry is the `iter(T)` they
             * step, and what they call is the pair. So the block below is for the concrete four,
             * and the kind test is what separates them - `Function::generatedRecord` holds a site
             * type there and a protocol here.
             */
            if(function->generatedRecord && decl[function->generatedRecord]->kind == Type::Record) {
                auto& site = *(RecordType*)decl[function->generatedRecord];
                if(site.producerStep) root(site.producerStep);

                /*
                 * And the **trampoline** where the record has one - §6.3. Every generated body over
                 * a delegating record calls it instead of the step, and the call is the conversion's
                 * rather than anything a walk here can read out of MIR.
                 */
                if(site.producerWalk) root(site.producerWalk);

                /*
                 * And what the **"make an object" entry** calls - §6.4, `Generated::IterObject`.
                 *
                 * Three edges and none of them is in any body: the constructor it calls, the heap
                 * shim its `fun.value` names, and what the **block** owes - whose header is this
                 * record's own `Drop`, written by the lowering. The last is the same argument the
                 * boxed nest below makes about the same pair, made here because this one is built
                 * by a generated body rather than by a step.
                 */
                if(function->generated == Function::Generated::IterObject) {
                    if(site.producerNew) root(site.producerNew);
                    if(site.producerBox) root(site.producerBox);

                    reachTeardown(function->generatedRecord,
                                  [&](DeclPtr<Function> target) { root(target); });
                }

                /*
                 * And the check `f$next` makes of a **resuming** object's index - the one call in a
                 * generated body that is not one of the four, and the one the reachability walk
                 * could not otherwise see. `emitIterStepper` is where it is emitted and says why.
                 */
                if(function->generated == Function::Generated::IterNext && site.producer) {
                    auto& declaring = bodyOwnerOf(decl, *decl[site.producer]);

                    if(declaring.resume.type && decl[declaring.resume.type]->kind != Type::Unit) {
                        if(auto guard = program.roles.function(intrinsic::Known::checkCondition)) {
                            root(guard);
                        }
                    }
                }

                /*
                 * And a **nested traversal**'s four - §3.2's first step, `RecordType::kNestMember`.
                 *
                 * The outer producer's step builds the child, steps it, reads its item out and
                 * cancels it, and every one of those calls is emitted by the *conversion*: there is
                 * no node in any body here for the edge walk to read them out of, exactly as there
                 * is none for the step the record names. So the member's own type is what they are
                 * rooted from.
                 */
                if(function->generated == Function::Generated::IterStep ||
                   function->generated == Function::Generated::IterWalk) {
                    U32 nest = 0;

                    for(U32 j = 0; j < site.payloadLocals.size(); j++) {
                        auto member = site.payloadLocals.get(decl, j);
                        if(!(member & RecordType::kNestMember)) continue;

                        auto held = site.field(decl, site.payloadFields.get(decl, j)).occurrence.type;

                        /*
                         * A **boxed** child - §6.3's recursion, where the member is the protocol
                         * `iter(Y)` and the record behind it is named by `RecordType::nestBoxed`.
                         *
                         * The shim is what the pair's code word is, so it is rooted here for the
                         * reason the other four are: the outer step builds the box and steps it, and
                         * there is no node in any body for the edge walk to read that out of. The
                         * child's own constructor and step are rooted through the shim's record,
                         * which is the same four calls one indirection over.
                         */
                        auto shim = nest < site.nestBoxed.size() ? site.nestBoxed.get(decl, nest)
                                                                 : DeclPtr<Function>(nullptr);
                        nest++;

                        // A **delegating** member is the link rather than the bare pair, so what
                        // names the child is the link's first field - `RecordType::linkType`.
                        auto delegates = held == site.linkType;

                        if(delegates) {
                            held = ((RecordType*)decl[held])->field(decl, 0).occurrence.type;
                        }

                        if(decl[held]->kind != Type::Record) {
                            if(!shim) continue;

                            root(shim);
                            held = decl[shim]->generatedRecord;
                            if(!held || decl[held]->kind != Type::Record) continue;
                        }

                        auto& child = *(RecordType*)decl[held];

                        /*
                         * And what the **box** owes - §6.3, and the one place a boxed level's own
                         * teardown can be found.
                         *
                         * A level is ended by dropping the pair, whose block header is the child
                         * record's `Drop` - and that header is a function the *lowering* writes, so
                         * there is no node anywhere for the edge walk to read the edge out of. It is
                         * the same argument `fun.value` makes about a heap-placed environment's
                         * captures, made here because this pair is built by the conversion.
                         */
                        if(shim) reachTeardown(held, [&](DeclPtr<Function> target) { root(target); });

                        // And the **ordinary** shim beside the raw one where the member delegates:
                        // the departure path builds its box with that one, and the call is the
                        // conversion's - see `RecordType::producerBox`.
                        if(delegates && child.producerBox) root(child.producerBox);

                        if(child.producerNew) root(child.producerNew);
                        if(child.producerStep) root(child.producerStep);

                        // And its **trampoline** where it has one: a region over a delegating child
                        // steps it through that rather than through its step, whether the member
                        // holds the child inline or in a box - see `convertStepNest`.
                        if(child.producerWalk) root(child.producerWalk);
                        if(child.producerItem) root(child.producerItem);
                        if(child.producerTake) root(child.producerTake);

                        /*
                         * And the window a nested hand-over may be adapted through - `chunks` hands
                         * over `[a]`, which the region's entry receives as the descriptor. The same
                         * derivation `Iterate`'s members are rooted for, one boundary over.
                         */
                        auto item = child.field(decl, 1).occurrence.type;
                        if(decl[item]->kind == Type::Pointer) item = ((PointerType*)decl[item])->to;

                        if(hasViewFor(decl, program, item, Access::Shared)) {
                            if(auto member = viewMember(context, program, item, Access::Shared,
                                                        kNullLocation)) {
                                root(member);
                            }
                        }
                    }
                }

            }

            /*
             * The **view derivation** `Iterate`'s members need where the item has one -
             * `hasViewFor`, the same question `rootViewDerivations` asks of a specialization.
             *
             * A borrowed `String` item is that string's window, so `f$next` builds one out of the
             * slot; the member it calls is a body the resolver never named, and neither this
             * function's own MIR (there is none) nor the conversion (which may not mint one) can
             * keep it. Shared only, which is the access `Maybe(source'item)` declares.
             *
             * All four steppers, the erased pair included: what differs between them is where the
             * item comes from, and this is about what its type asks for on the way out.
             */
            auto stepper = function->generated == Function::Generated::IterNext ||
                           function->generated == Function::Generated::IterResume ||
                           function->generated == Function::Generated::IterErasedNext ||
                           function->generated == Function::Generated::IterErasedResume;

            if(stepper && function->result.type) {
                auto& answered = *(RecordType*)decl[function->result.type];

                for(auto held: answered.fields.contents(decl)) {
                    if(!hasViewFor(decl, program, held.occurrence.type, Access::Shared)) continue;
                    if(auto member = viewMember(context, program, held.occurrence.type,
                                                Access::Shared, kNullLocation)) {
                        root(member);
                    }
                }
            }

            if(!function->mir) continue;

            rootViewDerivations(*function, [&](DeclPtr<Function> target) { root(target); });

            auto& body = *mir[function->mir];

            for(U32 i = 0; i < body.nodes.size(); i++) {
                auto node = body.node(mir, NodeId(i));

                /*
                 * A function **value** reaches the declaration it names - G7 - and does not run it.
                 *
                 * Reachability and not an edge: what the value does is name code the program needs,
                 * and whoever *calls* it is the activation the callee runs inside. Which function
                 * that is is what a walk over `Call` nodes cannot see - the call names no
                 * declaration - so the value is recorded here against the function type it was
                 * built at, and a call through a value of that type draws its edges to everything
                 * recorded under it, once every construction site has been walked. That is §3.5's
                 * flow set by type, and it is what keeps a body that merely builds, stores or
                 * returns a suspending value from being a suspending body.
                 */
                /*
                 * The globals a **constant inside a body** names - a `map`'s string table, and a
                 * string literal's own bytes. Both are `reachConstantAddresses`, above, and the
                 * reason neither is found by the place walk below is written there.
                 */
                if(node->kind == Node::Const) {
                    auto constant = body.constant(mir, ((NodeConst*)node)->constant);
                    reachConstantAddresses(constant);
                    reachConstantFunctions(constant, reachConstantFunctions);
                }

                if(node->kind == Node::Map) {
                    auto& map = *(NodeMap*)node;

                    for(auto entry: map.entries.contents(mir)) {
                        reachConstantAddresses(entry.value);
                        reachConstantFunctions(entry.value, reachConstantFunctions);
                    }

                    if(map.fallback.isSet()) {
                        reachConstantAddresses(map.fallback);
                        reachConstantFunctions(map.fallback, reachConstantFunctions);
                    }
                    continue;
                }

                if(node->kind == Node::FunValue) {
                    auto callee = ((NodeFunValue*)node)->callee;
                    root(callee);

                    /*
                     * And what a **heap-placed** environment owes - D17.
                     *
                     * Nothing in any body names it: the captures are released by the teardown in
                     * front of the block, which the lowering writes and which is reached through the
                     * value's second word rather than by a `drop` anything can walk. So this is the
                     * one place a capture's `instance Drop(T)` can be found, and it is the same
                     * argument `reachEnvironment` makes about a descriptor slot.
                     */
                    auto& lifted = *decl[callee];
                    if(lifted.environment && lifted.environmentStorage != StorageClass::Frame) {
                        reachTeardown(lifted.environment,
                                      [&](DeclPtr<Function> target) { root(target); });
                    }

                    flowsTo[U32(valueOccurrence(*node).type.offset)].push(callee);
                    continue;
                }

                /*
                 * A `lens.call` reaches its callee by exactly the same rules a `call` does - see
                 * `CallRequest`, which is what makes that one function rather than two that agree.
                 * That it is a terminator changes what the *caller* does with control and nothing
                 * about which body runs.
                 */
                CallRequest request;
                if(node->kind == Node::Call) request = callRequestOf(*(NodeCall*)node);
                else if(isLensCall(node->kind)) request = callRequestOf(*(NodeLensCall*)node);
                else continue;

                /*
                 * `callTarget` and not the node's own callee - `specialize.h`.
                 *
                 * A call to a generic names the generic, and which *specialization* it reaches
                 * depends on the substitution this body is being read under. That is the walk that
                 * makes specializations exist at all: reachability is what discovers which argument
                 * lists a program actually calls at, so asking here is what creates them, and no
                 * separate instantiation pass has to agree with this one about the answer.
                 */
                CallSite site;
                callTarget(context, program, *function, request, site);

                /*
                 * A call through a value runs whatever flows to the value's type - resolved after
                 * the walk, when every construction site is known. The operand is a result or a
                 * place, and either carries the occurrence the type is read off.
                 *
                 * Unless the value is a **site type**, whose code the type names: there is one
                 * callee, `callTarget` answered it, and the edge below is the ordinary direct one.
                 * That edge is the only thing keeping a lambda nothing built a pair for in the
                 * program at all, which is why it is drawn here rather than left to the flow set.
                 */
                if(request.callsValue && !site.siteCall &&
                   site.through != CallThrough::CallWitness) {
                    auto& call = *(NodeCall*)node;
                    valueCalls.push(ValueCall { caller, U32(occurrenceOf(body, call.target).type.offset) });
                    markOpaque(caller);
                    continue;
                }

                /*
                 * A target loaded out of *this* body's environment reaches nothing from here.
                 *
                 * What runs is whatever the caller put in the slot, and the caller is what reached
                 * it - through the environment it built, below. So the edge exists and is a
                 * different one; drawing a second one from here would name the class's declaration
                 * of the member, which is a signature with no body. What *this* body records is
                 * that it reads the slot, which is the half of the answer the caller cannot see.
                 */
                if(site.through == CallThrough::ClassSlot || site.through == CallThrough::FunctionSlot ||
                   site.through == CallThrough::CallWitness) {
                    auto kind = site.through == CallThrough::FunctionSlot ? GenSlotKind::Function
                                                                         : GenSlotKind::Class;
                    markSlot(*function, requirementSlotOf(*function, kind, site.requirement));
                }

                if(site.through != CallThrough::Declaration) {
                    markOpaque(caller);
                    continue;
                }

                /*
                 * The environment a call to a generic compiled **once** hands over.
                 *
                 * Every function in it is reached by this call and by nothing else: an instance
                 * member named only by a witness table is called through a pointer, so a walk over
                 * `Call` nodes alone would leave it unconverted and the table with nothing to point
                 * at. Edges rather than roots, on the same terms the teardowns below are - a
                 * witness that suspends suspends the body that called through it.
                 */
                if(site.environment) {
                    reachEnvironment(site.environment, root);
                    environmentUses.push(EnvironmentUse { caller, site.environment });
                }

                /*
                 * A body compiled once calling another at its own open types forwards slots of the
                 * environment it was handed, and which ones is a question about the callee's
                 * numbering against this body's that the lowering answers and this walk does not.
                 * Every slot counts as read, which is the conservative end and the old answer.
                 */
                if(site.target && function->form == GenForm::Erased && !site.environment &&
                   decl[site.target]->form == GenForm::Erased) {
                    markAllSlots(*function);
                }

                /*
                 * A generic that is *not* compiled once is never reached, on the same terms it is
                 * never a root: it has no layout, no ABI and nothing to lower. `callTarget` answers
                 * with one where it cannot make a specialization, which is only below a diagnostic.
                 *
                 * One that **is** compiled once is an ordinary reachable function whose body is
                 * walked under the empty substitution, which is what an erased form means.
                 */
                if(!site.target) { markOpaque(caller); continue; }
                if(isGeneric(decl, decl[site.target]->gen) &&
                   decl[site.target]->form != GenForm::Erased) {
                    markOpaque(caller);
                    continue;
                }

                auto target = root(site.target);
                edges.push(edgeOf(caller, target));

                // And the same edge with the *node* on it - see `CallEdge`. Recorded here rather
                // than by a second walk because `site.target` is what the second walk would have to
                // re-derive, and `callTarget` is the walk that makes specializations exist.
                callEdges.push(CallEdge { caller, target, NodeId(i) });
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
                    // The place's type is the *generic* body's, so which teardown actually runs is a
                    // question about this specialization. Substituted here for the same reason the
                    // conversion substitutes before it emits one: the placement is the generic
                    // body's answer and the code is this function's. Read off the place rather than
                    // off a path, since an overwrite of a global, a borrow or an element has none.
                    auto held = substituteType(context, program,
                                               body.place(mir, step.place)->occurrence.type, with);

                    reachTeardown(held, [&](DeclPtr<Function> target) {
                        edges.push(edgeOf(caller, root(target)));
                    });

                    // In a body compiled once the type is still a variable, and what runs is the
                    // teardown in that variable's descriptor slot - which is a slot this body reads.
                    if(function->form == GenForm::Erased) {
                        eachVariable(held, [&](DeclPtr<Type> variable) {
                            if(((VariableType*)decl[variable])->owner != function->gen) return;
                            markSlot(*function, variableSlotOf(*function, variable));
                        });
                    }
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

        // The last pass of the drain above settled the globals, and every function their
        // initializers name was rooted and walked there.
        program.discoveringFrom = nullptr;

        /*
         * The edges that needed the whole walk - see `edges`. Everything they name was rooted as it
         * was met, so `root` answers an index here and adds nothing after the drain.
         */
        for(auto& call: valueCalls) {
            auto targets = flowsTo.get(call.type);
            if(!targets) continue;

            for(auto callee: targets.unwrap()) edges.push(edgeOf(call.caller, root(callee)));
        }

        for(auto& use: environmentUses) {
            auto& environment = *decl[use.environment];
            auto used = usedSlots.get(U32(environment.generic.offset));

            // No body walked, or one that reads no slot: the direct edge to the erased body is
            // what carries its own answer, and a body without one already answers may-suspend.
            if(!used) continue;

            auto& bits = used.unwrap();
            for(U32 i = 0; i < environment.slots.size(); i++) {
                if(i / 64 >= bits.size() || !((bits[i / 64] >> (i % 64)) & 1)) continue;

                auto witness = environment.slots.get(decl, i);
                for(auto member: witness.functions.contents(decl)) {
                    edges.push(edgeOf(use.caller, root(member)));
                }

                if(witness.kind != GenSlotKind::Type) continue;

                reachTeardown(witness.type, [&](DeclPtr<Function> target) {
                    edges.push(edgeOf(use.caller, root(target)));
                });
            }
        }
    }

    // The occurrence an operand carries - a result's or a place's. A constant carries none that a
    // call could go through.
    const Occurrence& occurrenceOf(Body& body, Operand operand) {
        if(operand.source() == SourceKind::Place) return body.place(mir, operand.placeId())->occurrence;
        return valueOccurrence(*body.node(mir, operand.nodeId()));
    }

    /*
     * The slots of a body compiled once - `usedSlots`, and the numbering it is read against.
     *
     * The numbering is the schema's, read the way `Lowering::variableSlot` and `requirementSlot`
     * read it, because the environment a caller hands over is laid out in that order and the bit
     * this sets has to name the slot the caller fills.
     */
    SmallArray<U64, 2>& slotsOf(Function& generic) {
        auto& bits = usedSlots[U32((&generic - decl).offset)];
        if(!bits.size()) {
            auto& numbering = *decl[decl[generic.gen]->schema];
            for(Size i = 0; i < (numbering.slots.size() + 63) / 64; i++) bits.push(0);
        }

        return bits;
    }

    void markSlot(Function& generic, U32 slot) {
        auto& bits = slotsOf(generic);
        if(slot / 64 < bits.size()) bits[slot / 64] |= U64(1) << (slot % 64);
    }

    void markAllSlots(Function& generic) {
        for(auto& word: slotsOf(generic)) word = ~U64(0);
    }

    U32 requirementSlotOf(Function& generic, GenSlotKind kind, U16 requirement) {
        auto& numbering = *decl[decl[generic.gen]->schema];
        return (kind == GenSlotKind::Class ? numbering.classStart : numbering.functionStart)
             + requirement;
    }

    U32 variableSlotOf(Function& generic, DeclPtr<Type> variable) {
        auto& numbering = *decl[decl[generic.gen]->schema];
        for(U32 index = 0; index < numbering.slots.size(); index++) {
            if(numbering.slots.get(decl, index).type == variable) return index;
        }

        return ~U32(0);
    }

    // Every variable a type mentions, wherever it is nested - the walk `isConcreteType` makes,
    // asked to name what it finds rather than whether it found anything.
    template<class F>
    void eachVariable(DeclPtr<Type> type, F&& f) {
        if(!type) return;

        switch(decl[type]->kind) {
            case Type::Variable:
                f(type);
                return;
            case Type::Pointer:
                return eachVariable(((PointerType*)decl[type])->to, f);
            case Type::Array:
                return eachVariable(((ArrayType*)decl[type])->element, f);
            case Type::Vector:
                return eachVariable(((VectorType*)decl[type])->lane, f);
            case Type::Atomic:
                return eachVariable(((AtomicType*)decl[type])->content, f);
            case Type::Record:
                for(auto argument: ((RecordType*)decl[type])->arguments.contents(decl)) {
                    eachVariable(argument, f);
                }
                return;
            case Type::Fun: {
                auto& fun = *(FunType*)decl[type];
                for(auto param: fun.params.contents(decl)) eachVariable(param.occurrence.type, f);
                eachVariable(fun.result.type, f);
                return;
            }
            default:
                return;
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
    /*
     * Everything one generic environment names - `GenEnvironment`.
     *
     * The witness functions, and the **teardown of every type a descriptor slot describes**. The
     * second is what a drop inside the erased body becomes: the body knows only that a value of some
     * `a` dies, and what runs is the function the descriptor for `a` carries. Nothing in the body
     * names it, so the reachability has to follow the environment to find it - which is the same
     * argument the drop walk below makes about an `instance Drop(T)`, one indirection further out.
     *
     * Roots and not edges. Which of these the erased body *runs* is a fact about that body - the
     * slots its calls go through and the variables its drops tear down - and the edges are drawn
     * from `usedSlots` once it has been walked. Drawn from here, every witness handed over was an
     * invocation whether or not anything read it, which is the conflation the note on `edges` is
     * about.
     */
    template<class F>
    void reachEnvironment(DeclPtr<GenEnvironment> environment, F&& root) {
        for(auto witness: decl[environment]->slots.contents(decl)) {
            for(auto member: witness.functions.contents(decl)) root(member);
            if(witness.kind != GenSlotKind::Type) continue;
            reachTeardown(witness.type, [&](DeclPtr<Function> target) { root(target); });
        }
    }

    /*
     * The derivations a **specialization** may need - `viewMember`, rooted per type argument.
     *
     * A body compiled at `k := String` reaches a shared position of `k` holding an owner, and the
     * SSA conversion turns that into a call to `Viewed(String).view`. Nothing in the MIR body names
     * that function - the generic had no concrete type to name it at - so this walk is the only
     * thing that can keep it, and the conversion may not mint one: `convertProgram` asserts that the
     * function list does not grow.
     *
     * Deliberately blunter than the conversion's own test, on `opt/reach.cpp`'s terms for teardowns:
     * *which* positions of a body need the adaptation is a walk over its nodes under this
     * substitution, and reproducing it here would be a second statement of a rule that has one, in a
     * walk that would then have to be kept in step with it. What the over-approximation costs is a
     * derivation body emitted for a specialization that never adapts - two loads and an aggregate -
     * and it cannot be wrong in the direction that matters.
     */
    template<class F>
    void rootViewDerivations(Function& function, F&& root) {
        if(!function.specializes) return;

        for(auto argument: function.typeArguments.contents(decl)) {
            Access accesses[2] = { Access::Shared, Access::Unique };

            for(auto access: accesses) {
                if(!hasViewFor(decl, program, argument, access)) continue;
                if(auto member = viewMember(context, program, argument, access, kNullLocation)) {
                    root(member);
                }
            }
        }
    }

    // The types `reachTeardown` is currently inside - a boxed field can reach its own record, which
    // is what a recursive declaration is, so the walk needs somewhere to notice that it has been
    // here. Sixteen inline is more nesting than a member graph reaches in practice.
    SmallArray<DeclPtr<Type>, 16> reaching;

    template<class F>
    void reachTeardown(DeclPtr<Type> type, F&& root) {
        auto ownership = ownershipOf(decl, type);
        if(!ownership.needsTeardown()) return;

        /*
         * A record **can** contain itself now, through a box - `breakLayoutCycles`, which is what
         * makes `data List = Nil | Cons {tail: List}` have a layout at all. This walk used to say it
         * could not and recurse without a guard, which was true when the only answer to a
         * containment cycle was a diagnostic.
         *
         * What the guard has to keep is that the teardown of a type already on the stack is
         * *already being collected* - so everything it would root is rooted by the frame below, and
         * stopping loses nothing.
         */
        for(auto entry: reaching) {
            if(entry == type) return;
        }

        reaching.push(type);
        PopOnExit<DeclPtr<Type>> leaving { reaching };

        auto authored = [&](intrinsic::Known klass, TeardownKind kind) {
            if(kind != TeardownKind::Authored) return;

            // The *member*, through `teardownMember`, and not the instance's declaration: a
            // parametric head supplies a generic body and what runs is its specialization. Asking
            // the shared question here is what makes the function the reachability keeps the one
            // the lowering later names.
            if(auto member = teardownMember(context, program, klass, type, kNullLocation)) {
                root(member);
            }
        };

        authored(intrinsic::Known::drop, ownership.drop);
        authored(intrinsic::Known::reclaim, ownership.reclaim);

        /*
         * A fixed array recurses into its element, which is the same statement one field down.
         *
         * Without it, `[Guard *3]` reaches no teardown at all: nothing in the program names
         * `Guard.drop`, so its function is never marked reachable, never lowered, and the drop the
         * conversion emits has nothing to call. That is exactly the failure this walk exists to
         * prevent for a record, and an array is a record whose members happen to be identical.
         */
        if(decl[type]->kind == Type::Array) {
            reachTeardown(((ArrayType*)decl[type])->element, root);
            return;
        }

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

    /*
     * The strongest placement any object one activation needs - §10.4's published half.
     *
     * The base case is the body's own objects, which `analyze_escape.cpp` decided before this pass
     * ran, and the join is over the **call graph**: an activation that calls one that allocates
     * needs whatever that one needs. That reading rather than "this body's objects alone" is what a
     * caller actually wants to rank against - a `region` block asks whether anything inside it will
     * reach the allocator, and the answer is transitive.
     *
     * Ranked by *cost* and not by the enumerator's order, which is why `stronger` exists: `Region`
     * sits between the other two in `StorageClass` because that is the order a reader wants to read
     * the declaration in, and it sits between them in cost as well - but the two orders agreeing is
     * a coincidence worth not relying on.
     *
     * The same worklist `computeSuspension` uses, for its reasons: monotone, so it terminates; a
     * body with no body starts at the strongest rung, because §3.5's rule for an unknown target is
     * that it joins as whatever the caller cannot rule out.
     */
    static StorageClass stronger(StorageClass a, StorageClass b) {
        auto cost = [](StorageClass storage) {
            return storage == StorageClass::Frame ? 0 : storage == StorageClass::Region ? 1 : 2;
        };

        return cost(a) >= cost(b) ? a : b;
    }

    StorageClass bodyPlacement(Function& function) {
        if(!function.summary.bodyKnown) return StorageClass::Heap;
        if(!function.mir) return StorageClass::Frame;

        auto& body = *mir[function.mir];
        auto strongest = StorageClass::Frame;

        for(U32 i = 0; i < body.objects.size(); i++) {
            strongest = stronger(strongest, body.object(mir, ObjectId(i))->storage);
        }

        return strongest;
    }

    void computePlacement() {
        Array<U32> work;
        Size next = 0;

        for(U32 i = 0; i < reached.size(); i++) {
            auto function = decl[reached[i]];
            auto initial = bodyPlacement(*function);

            function->summary.setPlacement(initial, Knowledge::Assumed);
            if(initial != StorageClass::Frame) work.push(i);
        }

        while(next < work.size()) {
            auto index = work[next++];
            auto placed = decl[reached[index]]->summary.placement().value;

            for(auto slot = callerStart[index]; slot < callerStart[index + 1]; slot++) {
                auto caller = decl[reached[callerList[slot]]];
                auto joined = stronger(caller->summary.placement().value, placed);
                if(joined == caller->summary.placement().value) continue;

                caller->summary.setPlacement(joined, Knowledge::Assumed);
                work.push(callerList[slot]);
            }
        }

        // Published on `computeSuspension`'s terms: a body that is here has a settled answer, and a
        // declaration with no body has an assumption this pass made on no evidence.
        for(auto pointer: reached) {
            auto function = decl[pointer];
            if(!function->summary.bodyKnown) continue;

            function->summary.setPlacement(function->summary.placement().value, Knowledge::Known);
        }
    }


    /*
     * ------------------------------------------------------------------- the per-argument facts
     *
     * `escapes` and `mutates` - two of Analysis-Sharing.md §4's per-position rows, and one walk
     * that answers both, because they are the same question about two verbs. The other two rows §4
     * names were built and are gone: `retains` never had a reader that made a decision and
     * `consumes` restated the signature, and J48 deleted both rather than leave rows a later reader
     * would have to find out for himself were never trusted.
     *
     * What makes them answerable here rather than by the eight hundred lines the shipping stage
     * spends is the observation `analyze_escape.cpp` opens with: **nothing in an ownership MIR
     * reaches a caller's storage by being a value.** A record is copied, an owned argument is moved
     * into the callee's own slot, a returned value is written into storage the caller passed in. The
     * two ways back are a **loan**, which the loan checker has already resolved per value and per
     * interior position (`Elaboration::rootedIn`), and a **raw address**, which takes no loan and is
     * followed structurally here.
     *
     * So a body's own answer is one sweep over its nodes, and the interprocedural half is a join per
     * call site over the callee's rows. That join is what the worklist is for, exactly as with
     * suspension: a callee whose row grew is the only reason to look at a caller again.
     */

    // How far the two rooting walks follow a chain before answering the top of the lattice. A
    // reborrow through eight levels of pointer arithmetic is past anything a body writes, and the
    // bound is what makes a cycle through a block argument terminate rather than a visited set.
    static constexpr U32 kRootDepth = 8;

    // The per-body state the walks share, built once per body rather than once per fact.
    struct BodyRooting {
        Body& body;
        Function& owner;
        Elaboration* elaboration = nullptr;

        // Per loan, the parameters its borrowed place is rooted in - empty for a loan of a local, a
        // global or an address, which is every loan that names nothing a caller lent. A set rather
        // than an index because a loan through a raw address can name several.
        SmallArray<ArgumentSet, 8> lender;

        /*
         * Which block each `Argument` node is a parameter of, and which of them a walk is currently
         * standing on - `joinParameters`, which is what these two are for.
         *
         * An argument node names no operand of its own: what it holds arrives on the predecessors'
         * edges, and the edge lists live on the predecessors. So a walk that wants to follow one
         * backwards needs the block it belongs to, and a node does not record one.
         */
        Array<U32> argumentBlock;
        Array<U8> visiting;
    };

    static constexpr U32 kNoBlock = ~U32(0);

    /*
     * Whether a value at this occurrence can be a way back to storage somebody else owns.
     *
     * The filter that keeps these rows small enough to be worth having: an owned `Int` read out of a
     * parameter is a copy, and following it would put every parameter of every arithmetic body into
     * every row. A borrow is not owned, and a raw pointer is a machine address; those two are the
     * whole of what a value can carry.
     */
    bool carriesStorage(const Occurrence& occurrence) {
        if(!occurrence.contract.isOwned()) return true;
        return mentionsPointer(occurrence.type, 0);
    }

    // Whether a type has a raw pointer anywhere in it - `eachVariable`'s shape asked about pointers.
    // A type variable answers yes: a body compiled once cannot see the shape it was handed, so it
    // cannot rule one out either.
    bool mentionsPointer(DeclPtr<Type> type, U32 depth) {
        if(!type) return false;
        if(depth > kRootDepth) return true;

        switch(decl[type]->kind) {
            case Type::Pointer:
            case Type::Variable:
                return true;
            case Type::Array:
                return mentionsPointer(((ArrayType*)decl[type])->element, depth + 1);
            case Type::Record:
                for(auto field: ((RecordType*)decl[type])->fields.contents(decl)) {
                    if(mentionsPointer(field.occurrence.type, depth + 1)) return true;
                }
                return false;
            default:
                return false;
        }
    }

    /*
     * The parameters whose storage a place may reach.
     *
     * A walk up the interned parent chain to the root, which is what `Place` is interned for, and
     * then four answers: a **local** is a parameter or it is not, a **global** is nobody's argument,
     * a **loan** is the place it borrowed one level down, and an **address** is a value, so the
     * question becomes the node's.
     *
     * `throughDeref` is what makes an *owned* pointer parameter count. `p: %U8` arrives by value and
     * `*p = 0` writes storage the caller owns, so a writer asking only "is the root a borrow" would
     * call that the callee's own copy - the one shape where the contract and the reach disagree.
     */
    ArgumentSet placeParameters(BodyRooting& rooting, PlaceId id, bool& throughDeref, U32 depth) {
        if(depth > kRootDepth) return ArgumentSet::all();

        auto place = rooting.body.place(mir, id);

        while(place->parent) {
            if(place->kind == Place::Deref) throughDeref = true;
            place = mir[place->parent];
        }

        switch(place->kind) {
            case Place::Local: {
                auto local = rooting.body.local(mir, LocalId(place->payload));
                if(!local->isParameter()) return ArgumentSet();

                ArgumentSet set;
                set.add(local->parameter);
                return set;
            }

            case Place::Loan: {
                auto loan = rooting.body.loan(mir, LoanId(place->payload));
                if(!loan) return ArgumentSet::all();

                throughDeref = true;
                return placeParameters(rooting, loan->place, throughDeref, depth + 1);
            }

            case Place::Address:
                throughDeref = true;
                return nodeParameters(rooting, NodeId(place->payload), depth + 1);

            default:
                // A global root, and the projection kinds, which cannot be a root. The empty set
                // rather than an assertion: a malformed place is the verifier's to report and this
                // pass has no position to report one at.
                return ArgumentSet();
        }
    }

    /*
     * The same question about a value, and the two halves of the answer.
     *
     * The **loans** it is rooted in are the loan checker's own answer - `rootedInPosition` over every
     * interior position, which is exactly the union `ssa::Loan` is a may-set of - and each maps to a
     * parameter through the place it borrowed. That half is precise, and it is why this is a walk
     * rather than a dataflow: the dataflow already ran.
     *
     * The **structural** half is the raw pointer, which takes no loan and is therefore invisible to
     * the first. Those are followed through the operands, and a kind this does not recognize answers
     * the top of the lattice rather than the bottom: forgetting too much costs an optimization and
     * forgetting too little is a wrong answer a caller acts on.
     */
    /*
     * Where each `Argument` node belongs, filled once per body.
     *
     * `Block::arguments` is the only record of it - a node knows its own position in a block's
     * parameter list and not which block that is - so the map is the walk over the blocks, done
     * once rather than at each join a query reaches.
     */
    void mapJoins(BodyRooting& rooting) {
        auto count = rooting.body.nodes.size();

        rooting.argumentBlock.clear();
        rooting.visiting.clear();
        for(Size i = 0; i < count; i++) {
            rooting.argumentBlock.push(kNoBlock);
            rooting.visiting.push(0);
        }

        for(U32 b = 0; b < rooting.body.blocks.size(); b++) {
            auto block = rooting.body.block(mir, BlockId(b));
            if(!block) continue;

            for(auto pointer: block->arguments.contents(mir)) {
                auto index = mir[pointer]->id.index;
                if(index < rooting.argumentBlock.size()) rooting.argumentBlock[index] = b;
            }
        }
    }

    /*
     * A value merged at a join, followed to what the predecessors supply for it.
     *
     * This used to be the top of the lattice - "a value merged at a join, which this walk cannot
     * follow backwards without the predecessor list" - and the predecessor list is `Block::incoming`,
     * which `sealBody` fills for exactly this kind of question. What that cost is worth measuring by:
     * `pushUnsigned32` builds a pointer as `values(buffer) + at` where `at` is a loop-carried
     * argument, hands it to `String.appendUnits`, and every parameter of every caller three frames up
     * read as escaping because of it - which is what kept a bounded interpolation's buffer out of the
     * frame (`findings.md` §96.4).
     *
     * **Cutting the cycle contributes nothing**, which is what makes the recursion terminate without
     * losing an answer. This is a reachability question - which parameters are sources in the value
     * graph behind this node - and a back edge to a node already on the walk reaches only nodes the
     * walk is expanding anyway. Every node *on* the cycle is still visited, so every source hanging
     * off one is still collected; what is skipped is the second lap.
     *
     * The refusals stay the top of the lattice: a block with no predecessors, a position no edge
     * supplies, a predecessor with no terminator. None of them occurs in a sealed body, and each is
     * a shape where "this walk does not know" is the only honest answer.
     */
    template<class F>
    ArgumentSet joinParameters(BodyRooting& rooting, NodeId id, U32 depth, F&& follow) {
        auto index = id.index;
        if(index >= rooting.argumentBlock.size()) return ArgumentSet::all();

        auto owner = rooting.argumentBlock[index];
        if(owner == kNoBlock) return ArgumentSet::all();

        if(rooting.visiting[index]) return ArgumentSet();

        auto block = rooting.body.block(mir, BlockId(owner));
        if(!block || !block->incoming.size()) return ArgumentSet::all();

        auto position = ((NodeArgument*)rooting.body.node(mir, id))->index;

        rooting.visiting[index] = 1;

        ArgumentSet set;
        auto known = true;

        for(auto predecessorPointer: block->incoming.contents(mir)) {
            auto predecessor = mir[predecessorPointer];
            auto count = predecessor->nodes.size();
            if(!count) { known = false; break; }

            auto last = mir[predecessor->nodes.get(mir, count - 1)];

            forEachEdge(mir, *last, [&](Edge& edge) {
                if(!edge.target || mir[edge.target] != block) return;
                if(position >= edge.arguments.size()) { known = false; return; }

                set = set.join(follow(edge.arguments.get(mir, position), depth + 1));
            });

            if(!known) break;
        }

        rooting.visiting[index] = 0;
        return known ? set : ArgumentSet::all();
    }

    /*
     * Whether this call is the allocator answering fresh storage - `known.def`'s `allocateHeap`.
     *
     * The one call whose result the reach walks may stop at, and for `Node::RunSlots`' reason said
     * of a function instead of an instruction: a block the allocator just handed out points into
     * nothing the caller lent, so it is not a projection of any argument however the size was
     * computed. Without this the arm below reads `allocateHeap(byteSpan(self.items, wanted))` as
     * reaching `self` - the size argument is measured against the old slots - and `resize`'s fresh
     * block would carry the taint of the run it is replacing.
     *
     * Through `specializes`, because a generic allocator reaches its callers as an instantiation.
     */
    bool answersFreshStorage(Node& node) {
        if(node.kind != Node::Call || !heapRequest) return false;

        auto callee = ((NodeCall&)node).callee;
        if(!callee) return false;

        return callee == heapRequest || decl[callee]->specializes == heapRequest;
    }

    ArgumentSet nodeParameters(BodyRooting& rooting, NodeId id, U32 depth) {
        if(depth > kRootDepth) return ArgumentSet::all();

        auto node = rooting.body.node(mir, id);
        ArgumentSet set;

        if(rooting.elaboration && rooting.elaboration->loanCount) {
            auto& elaboration = *rooting.elaboration;
            auto positions = elaboration.originPositionCount(mir, id);

            for(U32 p = 0; p < positions; p++) {
                for(U32 l = 0; l < elaboration.loanCount; l++) {
                    if(!elaboration.rootedInPosition(mir, id, p, LoanId(l))) continue;
                    set = set.join(rooting.lender[l]);
                }
            }
        }

        // And the pointer half. A value carrying no pointer and owning what it holds is a copy, so
        // the walk stops rather than reporting every parameter the arithmetic touched.
        if(!carriesStorage(valueOccurrence(*node))) return set;

        switch(node->kind) {
            case Node::Address:
                return set.join(operandParameters(rooting, ((NodeAddress*)node)->of, depth + 1));

            case Node::Read:
                return set.join(operandParameters(rooting, ((NodeRead*)node)->place, depth + 1));

            // A pointer out of arithmetic, a choice or a construction points into whatever its
            // operands named. `forEachOperand` rather than a case each, because the answer is the
            // union and the union is what every one of them has.
            case Node::Unary:
            case Node::Binary:
            case Node::Select:
            case Node::Aggregate:
                forEachOperand(mir, *node, [&](Operand& operand, OperandRole) {
                    set = set.join(operandParameters(rooting, operand, depth + 1));
                });

                return set;

            /*
             * And a pointer a **call** answered, which points into whatever it was handed.
             *
             * The same union as the four above, and it belongs with them for a reason this walk
             * already relies on everywhere else: a callee's only way to name storage of *this*
             * frame is to have been given it. What it can otherwise answer is fresh storage, or
             * something read out of a global - and a global root is already "nobody's argument"
             * three cases down, on exactly the same reading.
             *
             * It was the `default` until J46, and the cost of that was not marginal: `values(xs)`
             * hands back a pointer, so `values(buffer) + at` read as every parameter of the body
             * computing it, and every caller of that body joined the same top through the fixpoint.
             * `pushUnsigned32` is the shape it was reaching - a frame array, a pointer out of it, an
             * append - and it put `String`'s slot outside `ssa::Contained` three frames up.
             */
            case Node::Call:
            case Node::LensCall:
            case Node::LensTry:
                if(answersFreshStorage(*node)) return set;

                forEachOperand(mir, *node, [&](Operand& operand, OperandRole) {
                    set = set.join(operandParameters(rooting, operand, depth + 1));
                });

                return set;

            /*
             * A block argument, which is where a value crosses an edge.
             *
             * The origins above already answer it for a borrow - the loan analysis carried its sets
             * across the edges - and what is left is a *pointer* merged at a join, which is
             * `joinParameters`' walk back through the predecessors' edge operands.
             */
            case Node::Argument:
                return set.join(joinParameters(rooting, id, depth, [&](Operand operand, U32 at) {
                    return operandParameters(rooting, operand, at);
                }));

            case Node::Const:
            case Node::Metric:
            case Node::GenValue:
            case Node::FunValue:

            // Fresh storage points into nothing the caller lent: whatever the allocation answered,
            // it is not a projection of any argument. That is the whole reason a run's slots can be
            // re-placed at all - see `resolve_ssa/opt/place.cpp`.
            case Node::RunSlots:
                return set;

            default:
                return ArgumentSet::all();
        }
    }

    ArgumentSet operandParameters(BodyRooting& rooting, Operand operand, U32 depth) {
        switch(operand.source()) {
            case SourceKind::Place: {
                auto place = rooting.body.place(mir, operand.placeId());
                if(!carriesStorage(place->occurrence)) return ArgumentSet();

                auto throughDeref = false;
                return placeParameters(rooting, operand.placeId(), throughDeref, depth);
            }

            case SourceKind::Result:
                return nodeParameters(rooting, operand.nodeId(), depth);

            default:
                return ArgumentSet();
        }
    }

    /*
     * ------------------------------------------------------------------ and the same three walks
     * for the **address** rather than for the data - the `escapes` row.
     *
     * The two walks look alike and are not. The value walk above asks whether anything *computed
     * out of* an argument is still alive after the call; this asks whether the **address the caller
     * handed over** was written anywhere, which is what `ssa::Contained` is about.
     * `growArray(&self, n)` is the body that separates them: it stores a run pointer read out of
     * `self` back into `self`, so the value reach of the written pointer contains parameter 0 - and
     * no address of `self`'s storage went anywhere.
     *
     * What makes the narrower walk sound is `Contained`'s own premise: an allocation it admits is
     * one whose address is never stored, so no pointer the callee **loads out of memory** can be it.
     * So a chain is followed only while it never passes through such a load - an `addressOf` of the
     * parameter's place or of a projection of it, the incoming pointer of a `%T` parameter, and a
     * place handed on at another call's argument position - and a value read out of the parameter's
     * bytes ends it.
     *
     * **`escapes` is the union of the two**, taken at the `Write` arm - see `Summary::escapes`,
     * which is where that reason lives. This walk is one half of the answer and not the answer.
     *
     * Three entry points rather than one, because the same operand means different things in
     * different positions: `Given` is a place whose *address* is handed over (a call argument, an
     * `addressOf`, a returned borrow) and `Carried` is a value that may *be* such an address (a
     * stored value, a returned owned value, an aggregate component).
     */
    ArgumentSet placeAddressParameters(BodyRooting& rooting, PlaceId id, U32 depth) {
        if(depth > kRootDepth) return ArgumentSet::all();

        auto place = rooting.body.place(mir, id);

        /*
         * A `Deref` whose parent is not the root reads a pointer *out of* the storage, and what it
         * names is wherever that pointer pointed rather than this parameter's slot. A `Deref` of the
         * root is the other case and is not a break: it is the incoming pointer of a `%T` parameter,
         * whose pointee is exactly the storage the caller handed over.
         */
        for(auto step = place; step->parent; step = mir[step->parent]) {
            if(step->kind == Place::Deref && mir[step->parent]->parent) return ArgumentSet();
        }

        while(place->parent) place = mir[place->parent];

        switch(place->kind) {
            case Place::Local: {
                auto local = rooting.body.local(mir, LocalId(place->payload));
                if(!local->isParameter()) return ArgumentSet();

                ArgumentSet set;
                set.add(local->parameter);
                return set;
            }

            case Place::Loan: {
                auto loan = rooting.body.loan(mir, LoanId(place->payload));
                if(!loan) return ArgumentSet::all();

                return placeAddressParameters(rooting, loan->place, depth + 1);
            }

            case Place::Address:
                return nodeAddressParameters(rooting, NodeId(place->payload), depth + 1);

            default:
                // A global root, which is nobody's argument, and the projection kinds, which the
                // walk to the root has already passed.
                return ArgumentSet();
        }
    }

    /*
     * Whether the **contents** of a place can be an address naming a parameter's storage.
     *
     * Only where the place is the parameter itself - the incoming pointer of `p: %T`, or a borrow
     * whose value is the caller's address. A projection's contents came out of the caller's bytes,
     * and by `Contained`'s premise the address of a contained allocation is never among them.
     */
    ArgumentSet readAddressParameters(BodyRooting& rooting, PlaceId id, U32 depth) {
        if(depth > kRootDepth) return ArgumentSet::all();

        auto place = rooting.body.place(mir, id);
        if(!carriesStorage(place->occurrence)) return ArgumentSet();
        if(place->parent) return ArgumentSet();

        switch(place->kind) {
            case Place::Local: {
                auto local = rooting.body.local(mir, LocalId(place->payload));
                if(!local->isParameter()) return ArgumentSet();

                ArgumentSet set;
                set.add(local->parameter);
                return set;
            }

            case Place::Loan: {
                auto loan = rooting.body.loan(mir, LoanId(place->payload));
                if(!loan) return ArgumentSet::all();

                return readAddressParameters(rooting, loan->place, depth + 1);
            }

            case Place::Address:
                return nodeAddressParameters(rooting, NodeId(place->payload), depth + 1);

            default:
                return ArgumentSet();
        }
    }

    ArgumentSet nodeAddressParameters(BodyRooting& rooting, NodeId id, U32 depth) {
        if(depth > kRootDepth) return ArgumentSet::all();

        auto node = rooting.body.node(mir, id);
        if(!carriesStorage(valueOccurrence(*node))) return ArgumentSet();

        switch(node->kind) {
            case Node::Address:
                return operandAddressGiven(rooting, ((NodeAddress*)node)->of, depth + 1);

            case Node::Read:
                return operandAddressCarried(rooting, ((NodeRead*)node)->place, depth + 1);

            // Arithmetic on an address is still that address, and a choice or a construction carries
            // whatever its operands carried.
            case Node::Unary:
            case Node::Binary:
            case Node::Select:
            case Node::Aggregate: {
                ArgumentSet set;
                forEachOperand(mir, *node, [&](Operand& operand, OperandRole) {
                    set = set.join(operandAddressCarried(rooting, operand, depth + 1));
                });

                return set;
            }

            /*
             * An address a call answered, which is one of the addresses it was handed - `Given` and
             * not `Carried`, because an argument position hands a *place* over and `Given` is the
             * larger of the two everywhere they differ.
             *
             * The narrow walk's premise carries it: a value the callee loaded out of the caller's
             * bytes is not an address of a contained allocation, so what a result can be is an
             * argument's own address or something the callee made.
             */
            case Node::Call:
            case Node::LensCall:
            case Node::LensTry: {
                ArgumentSet set;
                if(answersFreshStorage(*node)) return set;

                forEachOperand(mir, *node, [&](Operand& operand, OperandRole) {
                    set = set.join(operandAddressGiven(rooting, operand, depth + 1));
                });

                return set;
            }

            // A value merged at a join, followed through the predecessors' edge operands as a
            // *carried* address - the same position an aggregate component is in, and for the same
            // reason: what arrives here is a value that may be one, not a place handed over.
            case Node::Argument:
                return joinParameters(rooting, id, depth, [&](Operand operand, U32 at) {
                    return operandAddressCarried(rooting, operand, at);
                });

            case Node::Const:
            case Node::Metric:
            case Node::GenValue:
            case Node::FunValue:

            // Fresh storage points into nothing the caller lent: whatever the allocation answered,
            // it is not a projection of any argument. That is the whole reason a run's slots can be
            // re-placed at all - see `resolve_ssa/opt/place.cpp`.
            case Node::RunSlots:
                return ArgumentSet();

            default:
                return ArgumentSet::all();
        }
    }

    ArgumentSet operandAddressGiven(BodyRooting& rooting, Operand operand, U32 depth) {
        switch(operand.source()) {
            case SourceKind::Place:
                return placeAddressParameters(rooting, operand.placeId(), depth);

            case SourceKind::Result:
                return nodeAddressParameters(rooting, operand.nodeId(), depth);

            default:
                return ArgumentSet();
        }
    }

    ArgumentSet operandAddressCarried(BodyRooting& rooting, Operand operand, U32 depth) {
        switch(operand.source()) {
            case SourceKind::Place:
                return readAddressParameters(rooting, operand.placeId(), depth);

            case SourceKind::Result:
                return nodeAddressParameters(rooting, operand.nodeId(), depth);

            default:
                return ArgumentSet();
        }
    }

    // Whether a place is rooted in a global, which is the one root that outlives every frame and is
    // therefore what makes a write into it a retention of whatever it wrote.
    bool rootsInGlobal(BodyRooting& rooting, PlaceId id) {
        auto place = rooting.body.place(mir, id);
        while(place->parent) place = mir[place->parent];
        return place->kind == Place::Global;
    }

    /*
     * The parameters a **write** through this place reaches, which is not the same set as reading it.
     *
     * `p: Pair` arrives by value and `p.left = 3` writes the callee's own copy; `&p: Pair` and
     * `p: %Pair` both write the caller's. So a destination counts only where the walk reached the
     * caller's storage - through a `Deref`, a loan or an address, or from a root that is a reference
     * parameter - and that is the one thing separating this from `placeParameters`.
     *
     * `throughDeref` is left saying whether the walk crossed a `Deref`, loan or address, which the
     * `Write` arm needs to tell a write *into* a parameter from one into storage it points at.
     */
    ArgumentSet writtenParameters(BodyRooting& rooting, PlaceId id, bool& throughDeref) {
        auto reached = placeParameters(rooting, id, throughDeref, 0);
        if(!reached.any() || throughDeref) return reached;

        ArgumentSet written;
        for(auto pointer: rooting.owner.args.contents(decl)) {
            auto arg = decl[pointer];
            if(!reached.has(arg->index) || arg->occurrence.contract.isOwned()) continue;

            written.add(arg->index);
        }

        return written;
    }

    // The allocator, for `answersFreshStorage` - null in a program with no heap. A run's slots and
    // a fresh block are the same statement to the reach walks: neither is anything a caller lent.
    DeclPtr<Function> heapRequest = nullptr;

    /*
     * The two block-move ladders, or null - `known.def`'s `copyMemory` and `moveMemory` rows, where
     * the reason they are named rather than inferred is written out.
     *
     * A call to either is answered at the *site* by `blockMoveSite` rather than by joining the
     * callee's rows, because the fact that matters is about the destination and the destination is
     * the caller's expression.
     */
    DeclPtr<Function> blockCopyLadder = nullptr;
    DeclPtr<Function> blockMoveLadder = nullptr;

    bool isBlockMove(DeclPtr<Function> callee) {
        if(!callee) return false;
        auto root = decl[callee]->specializes ? decl[callee]->specializes : callee;
        return (blockCopyLadder && root == blockCopyLadder)
            || (blockMoveLadder && root == blockMoveLadder);
    }

    /*
     * What one block move at a call site does to the caller's own rows.
     *
     * The same statement the `Write` arm makes, with "the value stored" read as "whatever was at
     * `from`": if the destination reaches a parameter or roots in a global then this frame does not
     * own where those bytes landed, and the source's reach is what left. A destination the caller
     * allocated - `resize`'s `fresh` - reaches nothing and the move contributes nothing at all,
     * which is the answer a row on the callee could never give, since inside `copyMemory` the
     * destination is a parameter on every path.
     *
     */
    void blockMoveSite(BodyRooting& rooting, NodeId node, ArgumentSet& written, ArgumentSet& left) {
        auto& body = rooting.body;
        auto called = body.node(mir, node);
        if(!called || called->kind != Node::Call) return;

        ArgumentSet into;
        ArgumentSet moved;
        auto outside = false;
        auto position = 0;

        eachCallArgument(body, node, [&](U32, Operand operand) {
            if(position == 0) {
                // Written storage is the value question - which parameters' bytes the destination
                // names - and is exactly what the fixpoint used to join for `mutates` at this site.
                // The escape test is the wider one, because a destination that is a parameter's
                // *address* is as much the caller's as one that reaches it as a value.
                into = operandParameters(rooting, operand, 0);
                outside = into.any()
                    || operandAddressGiven(rooting, operand, 0).any()
                    || (operand.source() == SourceKind::Place
                        && rootsInGlobal(rooting, operand.placeId()));
            } else if(position == 1) {
                moved = operandParameters(rooting, operand, 0)
                    .join(operandAddressGiven(rooting, operand, 0));
            }

            position++;
        });

        written = written.join(into);
        if(outside) left = left.join(moved);
    }

    // One body's own rows, before anything it calls is joined in - one per verb.
    struct BodyFacts {
        ArgumentSet escapes;
        ArgumentSet mutates;
    };

    /*
     * What one body does to its own parameters.
     *
     * Three node kinds carry the whole of it. A **write** mutates what its destination reaches and
     * lets go of what it stored where the destination outlives this frame; and an **atomic** or a
     * **native** operation reaches storage through an address this tier cannot read further into, so
     * both verbs take the union of its operands. A **return** is here to say that it contributes
     * nothing, which is the J48 change. A call is not here at all: it is an edge, and the fixpoint
     * below is what walks it.
     */
    BodyFacts bodyArgumentFacts(BodyRooting& rooting) {
        BodyFacts facts;
        auto& body = rooting.body;

        for(U32 i = 0; i < body.nodes.size(); i++) {
            auto node = body.node(mir, NodeId(i));

            switch(node->kind) {
                case Node::Write: {
                    auto& write = *(NodeWrite*)node;

                    auto outside = false;
                    auto throughDeref = false;
                    ArgumentSet target;

                    if(write.destination.source() == SourceKind::Place) {
                        target = writtenParameters(rooting, write.destination.placeId(), throughDeref);
                        outside = target.any() || rootsInGlobal(rooting, write.destination.placeId());
                    } else {
                        // A destination that is a value is an address computed here, so what it
                        // names is that value's reach and the write is through it either way.
                        target = operandParameters(rooting, write.destination, 0);
                        outside = true;
                    }

                    facts.mutates = facts.mutates.join(target);
                    if(outside) {
                        auto stored = operandParameters(rooting, write.value, 0);

                        /*
                         * **A write back into the one parameter it reaches** keeps nothing of that
                         * parameter - `test/resolve/EscapeSelfWrite.yana`.
                         *
                         * `self.items = grow(self.items)` stores a value derived from `self` into
                         * `self`, and the value reach says parameter 0 - but whatever was reachable
                         * from `self` afterwards was reachable from it before, so nothing new outlives
                         * the call. Reporting it was what made `step` in that fixture, and every
                         * container body with the shape, read as keeping its subject.
                         *
                         * Exactly three conditions, and each is a case where dropping the position
                         * would be wrong. The destination reaches **one** parameter, since a write
                         * that may land in either of two lets one's value into the other. The walk to
                         * its root crossed **no `Deref`, loan or address**, since `self.child.items =
                         * self.items` puts the pointer into storage `self` merely points at - a place
                         * `opt/place.cpp` cannot follow a load out of. And only the **value** half is
                         * narrowed: an address of `self`'s own storage written into `self` is a
                         * self-reference `Contained` has to hear about, and the address walk below
                         * is left as it was.
                         */
                        if(!throughDeref && target.single()) stored.bits &= ~target.bits;

                        /*
                         * **And the value reach, not only the address reach** - J48.
                         *
                         * A store into storage that outlives this frame keeps whatever it stored,
                         * and "whatever it stored" includes a pointer this body *loaded out of* a
                         * parameter: `self.saved = r.items` puts a run's buffer somewhere the caller
                         * cannot see it go. The address walk is written not to follow a load of the
                         * caller's bytes - which is right for the question `ssa::Contained` asks,
                         * since an allocation it admits never had its address stored - and wrong for
                         * the one `opt/place.cpp` asks, because a run's buffer *is* a pointer living
                         * inside a descriptor. One row answers both by taking the union here.
                         *
                         * This is the whole of the extension. It is deliberately not the `Return`
                         * arm below: a value handed back is handed to the caller, which is walking
                         * it already - `admitsCall` taints every result - so counting it here is
                         * double counting - it is what made the deleted `retains` row unusable
                         * for placement.
                         */
                        facts.escapes = facts.escapes
                            .join(operandAddressCarried(rooting, write.value, 0))
                            .join(stored);
                    }

                    break;
                }

                case Node::Return: {
                    /*
                     * **A return contributes nothing here** - J48, and this is the change that lets
                     * one row answer both readers.
                     *
                     * What a call hands back is handed to the *caller*, which is walking the value
                     * it got: `opt/place.cpp` taints every call result and `ssa::Containment` now
                     * follows them too. Counting the return here as well is double counting, and it
                     * is what made this row unusable for placement - every accessor in the library
                     * hands back something derived from its subject, so `values`, `bitcast` and
                     * `byteSpan` all reported it kept, and the fixpoint carried that up through
                     * `resize` to `String.appendUnits`.
                     *
                     * So the row is now exactly "what the callee *stores* somewhere that outlives
                     * the call", which is the question both readers ask and neither could get.
                     */
                    break;
                }

                case Node::Atomic:
                case Node::Host:
                case Node::Native: {
                    /*
                     * **A block move is a write, and is read as one** - J48, and it is the last
                     * thing between a bounded interpolation and the frame.
                     *
                     * `copy.memory(to, from, count)` and `set.memory(to, value, count)` store
                     * neither pointer anywhere: they move bytes *through* them. What does travel is
                     * the source's **contents** into the destination's storage, which is precisely
                     * the `Write` arm above with "the value stored" replaced by "whatever was at
                     * `from`". So it is answered the same way - if the destination reaches a
                     * parameter or a global then this frame does not own where those bytes landed,
                     * and the source's reach is what escaped.
                     *
                     * Taking the union of every operand instead is what made `String.appendUnits`
                     * report its subject kept: its copy writes *into* `self`'s buffer, so `self` is
                     * the destination and appears in `reached`, and a caller's placed buffer could
                     * never reach it. `resize` is the same shape with the destination fresh from the
                     * allocator, which now contributes nothing at all.
                     *
                     * Everything else here keeps the blanket answer. A syscall stores through an
                     * address this tier cannot read, an atomic writes its operands into the location
                     * it was given, and the SHA and CRC rows are register arithmetic that would
                     * answer nothing either way.
                     */
                    if(node->kind == Node::Native) {
                        auto op = ((NodeNative*)node)->op;
                        if(op == NativeOp::CopyMemory || op == NativeOp::SetMemory) {
                            auto items = ((NodeNative*)node)->arguments.contents(mir);
                            if(items.size() >= 2) {
                                auto into = operandParameters(rooting, items[0], 0)
                                    .join(operandAddressGiven(rooting, items[0], 0));
                                auto outside = into.any()
                                    || (items[0].source() == SourceKind::Place
                                        && rootsInGlobal(rooting, items[0].placeId()));

                                facts.mutates = facts.mutates.join(into);
                                if(outside) {
                                    facts.escapes = facts.escapes
                                        .join(operandParameters(rooting, items[1], 0))
                                        .join(operandAddressGiven(rooting, items[1], 0));
                                }

                                break;
                            }
                        }
                    }

                    ArgumentSet reached;
                    ArgumentSet handed;
                    forEachOperand(mir, *node, [&](Operand& operand, OperandRole) {
                        reached = reached.join(operandParameters(rooting, operand, 0));
                        handed = handed.join(operandAddressGiven(rooting, operand, 0));
                    });

                    facts.mutates = facts.mutates.join(reached);

                    /*
                     * An address handed to a system call or a block move is one this tier cannot
                     * follow, so a place at any operand of one counts as given away - and so is a
                     * *value*, for the same reason the write above joins both. `copy.memory` moves
                     * whatever bytes it is pointed at, and a run's buffer pointer inside a
                     * descriptor is such a byte; a syscall is a store this tier cannot see at all.
                     * Sixteen rows, four of which touch memory, so a column here saying which would
                     * be worth having and is not what this round is about.
                     */
                    facts.escapes = facts.escapes.join(handed).join(reached);
                    break;
                }

                default:
                    break;
            }
        }

        return facts;
    }

    /*
     * The call sites, grouped both ways, and each site's arguments resolved to the caller's
     * parameters **once**.
     *
     * The fixpoint below is then pure set algebra: no body is walked twice, and a callee whose row
     * grows costs a join per site rather than a re-analysis of every caller. That is the same trade
     * the reversed call graph makes for suspension, one level of detail further in.
     */
    Array<U32> siteStart;      // per callee, the range of `siteOrder` naming sites that call it
    Array<U32> siteOrder;      // indices into `callEdges`
    Array<U32> madeStart;      // per caller, the range of `madeOrder` naming sites it made
    Array<U32> madeOrder;      // indices into `callEdges`
    Array<U32> siteArgStart;   // per site, the range of `siteArgs` holding its argument reaches
    Array<U32> siteArity;      // per site, how many of them there are
    Array<ArgumentSet> siteArgs;

    // The same range, in the address question's terms - which parameters' *storage* this position
    // hands over. Beside `siteArgs` rather than folded into it because the two are different sets
    // over the same positions, and resolving each once is what keeps the fixpoint set algebra.
    Array<ArgumentSet> siteAddrArgs;

    // Per site, whether the callee is a block-move ladder - a site answered by `blockMoveSite` when
    // the body was walked, and skipped by the fixpoint because it has no callee row to join.
    Array<U8> siteBlockMove;

    // Both groupings, by the same counting sort `buildCallers` uses. Two rather than one because the
    // two halves of the fixpoint ask opposite questions: the base case is per *caller* body and the
    // propagation is per *callee* answer, and a scan of the whole site list for either is the
    // quadratic this pass was rewritten to avoid once already.
    void groupSites() {
        auto count = U32(reached.size());
        auto sites = U32(callEdges.size());

        auto group = [&](Array<U32>& start, Array<U32>& order, auto&& keyOf) {
            start.clear();
            for(U32 i = 0; i <= count; i++) start.push(0);
            for(auto& site: callEdges) start[keyOf(site) + 1]++;
            for(U32 i = 0; i < count; i++) start[i + 1] += start[i];

            order.clear();
            for(U32 i = 0; i < sites; i++) order.push(0);

            Array<U32> cursor;
            for(U32 i = 0; i < count; i++) cursor.push(start[i]);
            for(U32 i = 0; i < sites; i++) order[cursor[keyOf(callEdges[i])]++] = i;
        };

        group(siteStart, siteOrder, [](const CallEdge& site) { return site.callee; });
        group(madeStart, madeOrder, [](const CallEdge& site) { return site.caller; });
    }

    /*
     * The arguments of one call, in the **callee's** own position order - `mapArguments` normalized
     * them there, so position i of the list is parameter i of the declaration.
     *
     * With one shift, and it is the whole of what a **site call** costs this pass. A call through a
     * site type reaches a lifted function whose parameter zero is the *environment*, and the node
     * holds that separately because which form the call is, is the substitution's answer rather than
     * the node's (`NodeCall::target`, and the conversion is where it is prepended for good). So the
     * target is position zero here too - without it a lambda that writes through its second
     * parameter would have its `mutates` row read against the caller's *first* argument, which is
     * the environment, and the write through what the caller actually lent would be invisible.
     *
     * Only where a call edge exists, which is what makes the test "calls a value" rather than
     * anything narrower: a call through a **pair** names no callee, so `markOpaque` has already put
     * its caller at the top of every lattice and no site of it is ever walked here.
     */
    template<class F> void eachCallArgument(Body& body, NodeId id, F&& f) {
        auto node = body.node(mir, id);

        if(node->kind == Node::Call) {
            auto& call = *(NodeCall*)node;
            auto items = call.arguments.contents(mir);
            auto shift = call.callsValue() ? 1u : 0u;

            if(shift) f(0, call.target);
            for(Size i = 0; i < items.size(); i++) f(U32(i + shift), items[i]);
            return;
        }

        auto& call = *(NodeLensCall*)node;
        auto items = call.arguments.contents(mir);
        for(Size i = 0; i < items.size(); i++) f(U32(i), items[i]);
    }

    /*
     * The four rows, over the whole program.
     *
     * The base case is each body's own verbs, and the top of every lattice is what a body with no
     * body and a body that made an unattributable call both start from - `isOpaque`, whose whole
     * content is that this pass could not say *which* position an argument went to.
     *
     * The schedule is `computeSuspension`'s: monotone, so it terminates, and a function is queued
     * when one of its rows grew because that is the only reason a caller has to be looked at again.
     */
    void computeArgumentFacts() {
        auto count = U32(reached.size());
        groupSites();

        heapRequest = program.roles.function(intrinsic::Known::allocateHeap);
        blockCopyLadder = program.roles.function(intrinsic::Known::copyMemory);
        blockMoveLadder = program.roles.function(intrinsic::Known::moveMemory);

        Array<ArgumentSet> escapes;
        Array<ArgumentSet> mutates;

        siteArgStart.clear();
        siteArity.clear();
        siteArgs.clear();
        siteAddrArgs.clear();
        siteBlockMove.clear();
        for(Size i = 0; i < callEdges.size(); i++) {
            siteArgStart.push(0);
            siteArity.push(0);
            siteBlockMove.push(0);
        }

        for(U32 i = 0; i < count; i++) {
            escapes.push(ArgumentSet());
            mutates.push(ArgumentSet());
        }

        // The bodies, once each: the base rows, and every site's arguments resolved in the terms of
        // the body that wrote them.
        for(U32 i = 0; i < count; i++) {
            auto function = decl[reached[i]];

            /*
             * The top of the lattice: nothing here established anything about this declaration, so
             * both rows say every argument. An **opaque call** reaches the same place by the other
             * road - a call this pass cannot attribute could have kept or written anything.
             */
            if((!function->summary.bodyKnown || !function->mir) || isOpaque(i)) {
                escapes[i] = ArgumentSet::all();
                mutates[i] = ArgumentSet::all();
                if(!function->mir) continue;
            }

            auto& body = *mir[function->mir];
            BodyRooting rooting { body, *function,
                                  body.elaboration ? mir[body.elaboration] : nullptr, {} };
            mapJoins(rooting);

            // The lenders first, and with the origin half switched off: a loan's own root is a
            // *place*, so the table can be built without reading the table being built.
            BodyRooting bare { body, *function, nullptr, {} };
            mapJoins(bare);
            for(U32 l = 0; l < body.loans.size(); l++) {
                auto loan = body.loan(mir, LoanId(l));
                auto throughDeref = false;
                rooting.lender.push(loan ? placeParameters(bare, loan->place, throughDeref, 0)
                                         : ArgumentSet());
            }

            auto facts = bodyArgumentFacts(rooting);
            escapes[i] = escapes[i].join(facts.escapes);
            mutates[i] = mutates[i].join(facts.mutates);

            for(auto slot = madeStart[i]; slot < madeStart[i + 1]; slot++) {
                auto s = madeOrder[slot];

                if(isBlockMove(reached[callEdges[s].callee])) {
                    blockMoveSite(rooting, callEdges[s].node, mutates[i], escapes[i]);
                    siteBlockMove[s] = 1;
                }

                siteArgStart[s] = U32(siteArgs.size());
                eachCallArgument(body, callEdges[s].node, [&](U32, Operand operand) {
                    siteArgs.push(operandParameters(rooting, operand, 0));
                    siteAddrArgs.push(operandAddressGiven(rooting, operand, 0));
                    siteArity[s]++;
                });
            }
        }

        Array<U32> work;
        Size next = 0;
        Array<U8> queued;
        for(U32 i = 0; i < count; i++) queued.push(1);
        for(U32 i = 0; i < count; i++) work.push(i);

        while(next < work.size()) {
            auto index = work[next++];
            queued[index] = 0;

            for(auto slot = siteStart[index]; slot < siteStart[index + 1]; slot++) {
                auto s = siteOrder[slot];

                // A block move was answered where the destination was visible - `blockMoveSite`,
                // called once when the caller's body was walked. Joining the ladder's own rows on
                // top would put back exactly the imprecision that model exists to avoid.
                if(siteBlockMove[s]) continue;

                auto caller = callEdges[s].caller;
                auto base = siteArgStart[s];

                ArgumentSet grownEscapes;
                ArgumentSet grownMutates;

                for(U32 a = 0; a < siteArity[s]; a++) {
                    if(mutates[index].has(a)) grownMutates = grownMutates.join(siteArgs[base + a]);

                    /*
                     * The escape row composes over **both** reaches - J48.
                     *
                     * It used to propagate over the address alone, which was right while the row
                     * meant "the address was handed away": a load out of a parameter is not that
                     * address, so it carried nothing. The row now also means "a value derived from
                     * this position was stored", and a value derived from a parameter is exactly
                     * what a load out of it is - so `outer(r)` calling `keep(self, r.items)`, where
                     * `keep` stores what it was given, has to report `r`. Address-only propagation
                     * would have said nothing there, and the buffer that leaked would be one this
                     * frame placed.
                     */
                    if(escapes[index].has(a)) {
                        grownEscapes = grownEscapes.join(siteAddrArgs[base + a])
                                                   .join(siteArgs[base + a]);
                    }
                }

                auto joinedEscapes = escapes[caller].join(grownEscapes);
                auto joinedMutates = mutates[caller].join(grownMutates);
                if(joinedEscapes == escapes[caller] && joinedMutates == mutates[caller]) continue;

                escapes[caller] = joinedEscapes;
                mutates[caller] = joinedMutates;

                if(queued[caller]) continue;
                queued[caller] = 1;
                work.push(caller);
            }
        }

        // Published on `computeSuspension`'s terms: a body that is here has a settled answer, and a
        // declaration with no body has an assumption this pass made on no evidence.
        for(U32 i = 0; i < count; i++) {
            auto function = decl[reached[i]];
            auto how = function->summary.bodyKnown ? Knowledge::Known : Knowledge::Assumed;

            function->summary.setEscapes(escapes[i], how);
            function->summary.setMutates(mutates[i], how);
        }
    }

    /*
     * ------------------------------------------------------------------------- the stack bound
     *
     * Whether an activation's frame is finite - Analysis-External-Iterators §4.4, and the half of
     * §4.5 that belongs to the resolver.
     *
     * §4.5 states the split outright: "the resolver can validate that the call graph is bounded, but
     * it cannot know the exact bytes", because spill slots, the outgoing argument area and saved
     * registers are decided after register allocation. So what this pass publishes is the
     * **eligibility** half - `StackBound::isBounded()` - and the byte count stays zero, which is
     * `plus` waiting for the backend term rather than a number this tier invented. A fake byte count
     * would be worse than none: a fiber's child stack would be reserved from it.
     *
     * The lattice is what makes that honest rather than lossy. `unbounded` is the top and it is what
     * recursion, an indirect call and a separately compiled callee all give; a bounded answer with no
     * bytes in it is a floor, and §4.5's stage 2 is what raises it.
     *
     * Recursion is the one thing here that is not a join, and it is why this pass is a peel rather
     * than a worklist over the reversed graph: nothing in a cycle ever *becomes* unbounded by a
     * callee changing its answer, because every member of the cycle starts bounded. Kahn's algorithm
     * over the callee graph finalizes exactly the functions that reach no cycle, and what is left
     * over is the answer.
     */
    void computeStack() {
        auto count = U32(reached.size());

        Array<U32> outDegree;
        for(U32 i = 0; i < count; i++) outDegree.push(0);
        for(auto edge: edges) outDegree[U32(edge >> 32)]++;

        // A function that already cannot answer starts finalized-as-unbounded, so that the peel
        // below does not have to distinguish "reaches a cycle" from "reaches an unknown".
        Array<U8> bounded;
        for(U32 i = 0; i < count; i++) {
            auto function = decl[reached[i]];
            bounded.push(function->summary.bodyKnown && !isOpaque(i) ? 1 : 0);
        }

        Array<U32> work;
        Size next = 0;
        Array<U8> settled;
        for(U32 i = 0; i < count; i++) settled.push(0);

        for(U32 i = 0; i < count; i++) {
            if(outDegree[i]) continue;
            settled[i] = 1;
            work.push(i);
        }

        while(next < work.size()) {
            auto index = work[next++];

            for(auto slot = callerStart[index]; slot < callerStart[index + 1]; slot++) {
                auto caller = callerList[slot];
                if(!bounded[index]) bounded[caller] = 0;

                if(--outDegree[caller]) continue;
                if(settled[caller]) continue;

                settled[caller] = 1;
                work.push(caller);
            }
        }

        // Whatever the peel never settled is in a cycle or reaches one - direct or mutual recursion,
        // which §4.4 rejects rather than bounds.
        for(U32 i = 0; i < count; i++) {
            auto function = decl[reached[i]];
            auto answer = settled[i] && bounded[i] ? StackBound { 0 } : StackBound::unbounded();

            function->summary.setStack(answer, function->summary.bodyKnown ? Knowledge::Known
                                                                           : Knowledge::Assumed);
        }
    }

    /*
     * --------------------------------------------------------------------------- divergence
     *
     * Whether calling this ever comes back - `Summary::diverges`, and what `endNonReturningBlocks`
     * in `resolve_ssa/opt` reads.
     *
     * The seed is a **role** and not a walk: `lib/Core/Check.yana`'s `checkFailed` is
     * `abortProcess()`, which is `exitProcess(134)`, which is a `syscall1` followed by an ordinary
     * `return` - so no CFG in the program says the process stopped, and nothing below the seed can
     * be inferred. Above it everything is a CFG answer: a body diverges when its entry cannot reach
     * a `return` with the block of every diverging call cut off at that call.
     *
     * False is the conservative direction and is what a body with no body gets. A caller that thinks
     * a diverging callee returns keeps a block it did not have to; one that thinks a returning callee
     * diverges deletes the code after it.
     *
     * The schedule is the suspension worklist again, and the base case is recomputed rather than
     * joined - a callee turning diverging changes which blocks of the caller reach a `return`, which
     * is a graph question and not a lattice one.
     */
    /*
     * The constant a branch's condition is, or nothing - which is the whole of what `while True`
     * needed.
     *
     * The optimizer folds a decided branch and this runs before it, so a body whose loop header
     * tests a literal still has both edges in its graph and the walk below reaches a `return`
     * through the one the program never takes. The constant is read here rather than folded into
     * MIR, because folding it is a CFG edit that would move every golden for the sake of a fact,
     * and this is the one reader of that fact.
     *
     * `Bool` is a declared enum, so its constants are constructor indices - `False` is zero and
     * `True` is one, which is the rule `checkRoles` holds the declaration to.
     */
    Maybe<bool> decidedCondition(Body& body, Operand condition) {
        if(condition.source() != SourceKind::Result) return Nothing();

        auto node = body.node(mir, NodeId(condition.id()));
        if(!node || node->kind != Node::Const) return Nothing();

        auto held = body.constant(mir, ((NodeConst*)node)->constant);
        if(held.kind == Constant::Enum) return Just(held.asConstructor() != 0);
        if(held.kind == Constant::Int) return Just(held.asInt() != 0);

        return Nothing();
    }

    // Every edge control can take out of one terminator, which for a branch on a literal is one of
    // its two. Anything else is `forEachEdge` unchanged.
    template<class F> void forEachLiveEdge(Body& body, Node& terminator, F&& f) {
        if(terminator.kind == Node::Branch) {
            auto& branch = (NodeBranch&)terminator;

            if(auto decided = decidedCondition(body, branch.condition)) {
                f(decided.unwrap() ? branch.whenTrue : branch.whenFalse);
                return;
            }
        }

        forEachEdge(mir, terminator, f);
    }

    bool bodyDiverges(U32 index, Array<U8>& diverges) {
        auto function = decl[reached[index]];
        if(!function->summary.bodyKnown || !function->mir) return false;

        auto& body = *mir[function->mir];
        auto blocks = body.blocks.size();
        if(!blocks) return false;

        /*
         * Which blocks end at a call that does not come back. The call's own node is what says so,
         * and `callEdges` is where the node and the callee meet - but a node does not carry the
         * block it is in (`Node` is a dense id and nothing else), so the map is built here from the
         * lists that do.
         */
        Array<U8> stops;
        for(Size i = 0; i < blocks; i++) stops.push(0);

        auto diverging = false;
        for(auto slot = madeStart[index]; slot < madeStart[index + 1]; slot++) {
            if(diverges[callEdges[madeOrder[slot]].callee]) diverging = true;
        }

        if(diverging) {
            Array<U32> blockOfNode;
            for(U32 i = 0; i < body.nodes.size(); i++) blockOfNode.push(~U32(0));

            for(U32 b = 0; b < blocks; b++) {
                for(auto pointer: body.block(mir, BlockId(b))->nodes.contents(mir)) {
                    blockOfNode[mir[pointer]->id.index] = b;
                }
            }

            for(auto slot = madeStart[index]; slot < madeStart[index + 1]; slot++) {
                auto& site = callEdges[madeOrder[slot]];
                if(!diverges[site.callee]) continue;

                auto found = blockOfNode[site.node.index];
                if(found != ~U32(0)) stops[found] = 1;
            }
        }

        // The blocks a `return` is reachable from, backwards. A stopping block reaches none: its own
        // terminator is behind a call that never returns.
        Array<U8> reachesReturn;
        for(Size i = 0; i < blocks; i++) reachesReturn.push(0);

        for(auto progress = true; progress;) {
            progress = false;

            for(U32 b = 0; b < blocks; b++) {
                if(reachesReturn[b] || stops[b]) continue;

                auto block = body.block(mir, BlockId(b));
                auto nodes = block->nodes.size();
                if(!nodes) continue;

                auto& terminator = *mir[block->nodes.get(mir, nodes - 1)];
                auto found = terminator.kind == Node::Return;

                forEachLiveEdge(body, terminator, [&](Edge& edge) {
                    if(edge.target && reachesReturn[mir[edge.target]->index.index]) found = true;
                });

                if(!found) continue;

                reachesReturn[b] = 1;
                progress = true;
            }
        }

        return !reachesReturn[mir[body.entry]->index.index];
    }

    void computeDivergence() {
        auto count = U32(reached.size());

        Array<U8> diverges;
        for(U32 i = 0; i < count; i++) diverges.push(0);

        Array<U32> work;
        Size next = 0;

        // The seed, and the one place the answer is not a CFG's.
        if(auto failed = program.roles.function(intrinsic::Known::checkFailed)) {
            if(auto found = indexOf.get(failed.offset)) {
                diverges[found.unwrap()] = 1;
                work.push(found.unwrap());
            }
        }

        // And everything whose own graph says so before any callee did - an endless loop, and a body
        // whose every arm calls the seed.
        for(U32 i = 0; i < count; i++) {
            if(diverges[i] || !bodyDiverges(i, diverges)) continue;

            diverges[i] = 1;
            work.push(i);
        }

        while(next < work.size()) {
            auto index = work[next++];

            for(auto slot = callerStart[index]; slot < callerStart[index + 1]; slot++) {
                auto caller = callerList[slot];
                if(diverges[caller] || !bodyDiverges(caller, diverges)) continue;

                diverges[caller] = 1;
                work.push(caller);
            }
        }

        for(U32 i = 0; i < count; i++) {
            auto function = decl[reached[i]];
            function->summary.setDiverges(diverges[i] != 0,
                                          function->summary.bodyKnown ? Knowledge::Known
                                                                      : Knowledge::Assumed);
        }
    }

    void run() {
        markReachable();
        buildCallers();
        computeSuspension();
        computePlacement();
        computeArgumentFacts();
        computeStack();
        computeDivergence();

    }
};

} // namespace

void computeSummaries(Context& context, Program& program) {
    Interprocedural pass(context, program);
    pass.run();
}

} // namespace mir
