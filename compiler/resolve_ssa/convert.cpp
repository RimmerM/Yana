#include "convert.h"
#include "../resolve_mir/specialize.h"
#include "../resolve_mir/own.h"

namespace ssa {

namespace {

/*
 * A value that **is** an address rather than one whose bytes are at one - `lower/manifest.h`'s
 * `isReferenceValue`, restated with no `Layout` in it (Analysis-IR-JS.md §1.3.1).
 *
 * `denotesStorage` answers the pair - an owner in memory and a loan that is a pointer - and this is
 * the half that names only the second. A free function because `convertWitnessEntry` below is not a
 * member of the conversion and asks the same question about the same pair of occurrences.
 */
bool referenceValued(Context& context, mir::Program& program, const mir::Occurrence& occurrence) {
    return !occurrence.contract.isOwned() && denotesStorage(context, program, occurrence);
}

/*
 * The three dense side tables a conversion needs, kept across functions.
 *
 * Cleared and refilled per body rather than constructed per body, which is the containers guidance's
 * rule: a conversion that built three fresh `Array`s per function paid a heap allocation per table
 * per function for tables whose peak size is the largest body in the program. One instance holds the
 * high-water mark and every function after the first reuses it.
 */
struct Scratch {
    // MIR node ID to the SSA value it produced. Dense, because a node ID is dense - §8.6's rule
    // about side tables rather than fields, applied to the tables a conversion needs.
    Array<SsaPtr<Value>> produced;
    Array<SsaPtr<Block>> blocks;

    // The storage each MIR local became, by local index. One `Alloc` in the entry block per local,
    // made before any block is converted, so that a read in the loop body and the write that
    // precedes it name the same address without either of them deciding where it came from.
    Array<SsaPtr<Value>> slots;

    // The drop flag each conditionally released move path got, by path index. Empty for a path that
    // is always owned or never is, which is every path in a body with no branch over an owner.
    Array<SsaPtr<Value>> flags;

    /*
     * The storage each `lens.try` destructures its answer out of, by node ID.
     *
     * The wrapper a skipping callee returns is the *conversion's* own temporary and exists in no MIR
     * local: MIR says the skip with an edge and a block parameter, so there is no node there whose
     * result it is. It still needs a slot for the reason every owned aggregate does - a field is
     * moved out of storage, and `Store` is the instruction that takes ownership of a value.
     */
    Array<SsaPtr<Value>> outcomes;

    void reset(U32 nodes) {
        produced.clear();
        blocks.clear();
        slots.clear();
        flags.clear();
        outcomes.clear();

        produced.reserve(nodes);
        outcomes.reserve(nodes);

        for(U32 i = 0; i < nodes; i++) {
            produced.push(nullptr);
            outcomes.push(nullptr);
        }
    }
};

/*
 * One continuation region, as the function it becomes - Analysis-IR.md §11.4's W-C.
 *
 * The CPS lowering, and the whole of what it is: a `Handover` region's blocks are the callee's
 * continuation, so they become an ordinary function taking the caller's frame by reference, the
 * call site passes it as the last argument, and how the continuation *left* comes back as the
 * value that function answers with.
 *
 * The exit signal is a compiler ABI and not a source type (§12.1's fifth item). A continuation
 * answers with a small discriminant - it fell out, or it left the enclosing function - and anything
 * an exit carries travels in a caller-frame slot the environment already reaches. That is what
 * makes `break` and `continue` further rows rather than a second mechanism, and it is why nothing
 * here waits on `Outcome` or `Try`: those are the *outer* join, which is a skipping lens's.
 *
 * What licenses the outlining being mechanical is a property checked for other reasons.
 * `checkValueScope` says an operand names a value of its own block, so the only things that cross
 * the boundary are the region entry's block arguments - which become parameters - and the *storage*
 * the region reads, which becomes the environment. Nothing else can.
 */
struct Outline {
    static constexpr U16 kNoField = 0xffff;

    ContinuationId region;

    // The function the region becomes, and the record its captures live in. Both are made before
    // any body is converted, because the call site names them and is converted first.
    DeclPtr<mir::Function> lifted = nullptr;
    DeclPtr<mir::Type> environment = nullptr;

    /*
     * What the environment holds, one field each: the *address* of a local the region reads and its
     * own scope does not declare.
     *
     * An invalid `LocalId` is the enclosing function's **pending result** rather than a local - the
     * one slot an exit carries a value in. It is one slot for the whole nest rather than one per
     * region, so a `return` written three continuations deep fills the same storage the outermost
     * frame reads, and the regions in between propagate a discriminant and nothing else.
     */
    Array<LocalId> captures;
    U16 resultField = kNoField;

    /*
     * And per capture, the **drop flag** it is, by move path - `kNoFlag` for a local or the result.
     *
     * A flag is a `Bool` in the frame of whichever scope allocated it, so one written inside a
     * continuation and read by the caller - `drop.maybe (s)` at a `for`'s destination after a
     * `break` path moved `s` - has to be the caller's storage, reached through the environment
     * exactly as a captured local is. It was an internal error until J185 ("a drop flag inside a
     * continuation has no storage the nest shares"), on the premise that nothing reached it; a
     * conditional move out of an outer local followed by `break` reaches it. Parallel to `captures`
     * rather than a third meaning of an invalid `LocalId`, which already means the result.
     */
    static constexpr U32 kNoFlag = ~U32(0);
    Array<U32> flagOf;

    // Which exits the region actually has. A region with one needs no test at the call site.
    bool fallsOut = false;
    bool leaves = false;

    /*
     * The **outward rows** the continuation can answer - the landings of a `break` or `continue`
     * written in a lens call's block for a loop outside it (`kExitOutward`). What the call site
     * tests for besides `kExitLeave`.
     */
    SmallArray<U64, 2> outward;
};

// The two rows of the exit signal. Numbers rather than names below the resolver, because what a
// continuation answers with is an `I32` chosen at the call site - see `Types::exitCode`.
static constexpr U64 kExitFallOut = 0;
static constexpr U64 kExitLeave = 1;

/*
 * `break` - the row a `for` over an `iter fn` adds, and the reason the call site's test is written
 * the way it is below.
 *
 * Only the callee reads it: an iterator's `yield` returns whatever the body answered and stops
 * where that is not `kExitFallOut`, so a `break` stops the iterator exactly as a `return` does. The
 * *caller* does not distinguish it from falling out - the loop is over either way and control
 * resumes at the region's destination - which is why the call site tests for `kExitLeave` rather
 * than against `kExitFallOut`.
 */
static constexpr U64 kExitBreak = 2;

/*
 * `break` and `continue` out of a lens call's block - the rows after the three above, one per block
 * such an exit can land on (K2).
 *
 * The resolver builds the exit as a chain of edges, each leaving one lens region for a *landing*
 * block of the region the call was made in (`Resolver::resolveExit`), so a row is answered by the
 * continuation, returned by the lens after its cleanup as it returns anything its continuation
 * answered, and taken by the call site as a jump to that landing - which may answer the next row
 * outwards. Numbered by the landing's own block index, a number the MIR already gives it, so the
 * two ends agree without a table between them.
 */
static constexpr U64 kExitOutward = 3;

static U64 outwardRow(BlockId landing) { return kExitOutward + U64(landing.index); }
static BlockId outwardLanding(U64 row) { return BlockId(U32(row - kExitOutward)); }

/*
 * The resume index a **finished** iterator object carries - `resolve_mir/resolve_object.cpp`, where
 * the same number is written for the record's other end.
 *
 * Repeated rather than shared because the two tiers do not include each other, and it is deliberately
 * a number no `yield` can be given: a `yield`'s index is `1 + k`, so what is left over says the
 * producer ran out or was cancelled.
 */
static constexpr I64 kIterFinished = 0x7fffffff;

// And a failing step's - `resolve_object.cpp`'s `kIterFailed`, repeated for the same reason.
static constexpr I64 kIterFailed = kIterFinished - 1;

/*
 * A **delegated** level that failed and has told its trampoline nothing yet - C1c, J218, and written
 * by `f$erased.step` alone (`emitIterErasedRaw`).
 *
 * The trampoline reads a null answer as "this level is done" and steps the level above, which is
 * exactly what a failure needs - it is that level's alternative that runs - so the raw shim answers
 * null the first time and moves the index here. The level above, resumed at its `delegateResume`,
 * asks the same pair once more, and this is what tells that second call to answer the failure rather
 * than null. Like `kIterFailed` it is out of reach of a `yield` and falls through the dispatch, so a
 * record left here is finished to every other reader.
 */
static constexpr I64 kIterFailedHeld = kIterFinished - 2;

static Value* iterErasedReason(mir::Program& program, Editor& editor, Value* answer,
                               const mir::IterType& iterated, LocationId at);
static Value* iterAnswerTagged(mir::Program& program, Editor& editor, Value* answer, LocationId at);

/*
 * What a generated caller sends a step when it is sending a **row** rather than a value.
 *
 * The step's second argument is the continuation's answer at the type J49 gave it: the bare exit
 * signal for a plain producer, and `Outcome(R, exit)` where the declaration wrote `<- R`. Three
 * callers here choose a row of their own rather than passing a consumer's - the teardown's "leave",
 * a nested member's "fall out", and the cancellation of a suspended child - and each of them wrote
 * the bare signal. At a resuming producer that is a mistyped call, which the verifier reports as
 * `internal: call to g$step supplies argument 1 of type I32, and it declares Outcome(Cmd, I32)`;
 * the teardown is the one that made the whole declaration fail, because a record owing anything at
 * all has one (`../../doc/progress/2026-09-16-J57-object-defects.md` §2).
 *
 * One function rather than the wrapper written out three times, so that the day a fourth caller
 * appears it cannot get this wrong: `emitIterStepper` and `emitErasedStepper` build the same
 * aggregate from the other direction, where the payload is a `Proceed(v)` a consumer supplied.
 */
static Use exitRow(mir::Program& program, Editor& editor, DeclPtr<mir::Type> sent, U64 code,
                   LocationId at) {
    auto decl = *program.decl;
    auto row = editor.addConst(at, mir::intConstant(program.types.exitCode(), code));

    if(decl[sent]->kind != mir::Type::Record) {
        return editor.use(*editor.resultOf(*row), UseKind::Consume);
    }

    // `Exit(code)` - the second constructor of `Outcome(R, exit)`, whose payload is the same signal
    // the bare form is. The first is `Proceed(v)` and is what a consumer's `resume(v)` builds.
    auto& wrapper = *(mir::RecordType*)decl[sent];
    auto held = mir::owned(sent);
    Use component[1] = { editor.use(*editor.resultOf(*row), UseKind::Consume) };
    auto exit = wrapper.cons.get(decl, 1).index;
    auto built = editor.addAggregate(at, exit, component, 1, held, conventionFor(decl, held));
    return editor.use(*editor.resultOf(*built), UseKind::Consume);
}

/*
 * Everything the outlining needs, worked out before a single instruction is emitted.
 *
 * Before, and not as the walk goes, for the reason the block map is built first: the call site is
 * converted before the region it opens, and it names the function and the environment record. It is
 * also where the *nesting* is resolved - a region's captures include whatever its children capture
 * and it does not itself declare, because a child's environment is built by its parent's blocks.
 * Innermost first, which is the reverse of the creation order.
 */
struct Planner {
    Context& context;
    mir::Program& program;
    mir::Function& function;
    mir::Body& source;
    MirBase mir;
    const mir::Substitution& with;
    Array<Outline>& into;

    DeclBase decl() { return *program.decl; }

    mir::Occurrence substituted(const mir::Occurrence& occurrence) {
        return mir::substituteOccurrence(context, program, occurrence, with);
    }

    // Whether `region` is `outer` or is nested in it.
    bool within(ContinuationId inner, ContinuationId outer) {
        return inner == outer || source.encloses(mir, outer, inner);
    }

    void capture(Outline& outline, LocalId local) {
        auto declared = source.local(mir, local);
        if(within(declared->region, outline.region)) return;

        for(auto held: outline.captures) if(held == local) return;
        outline.captures.push(local);
        outline.flagOf.push(Outline::kNoFlag);
    }

    // A drop flag the region writes or tests, which lives in the frame of the function the nest
    // is outlined from - see `Outline::flagOf`.
    void captureFlag(Outline& outline, U32 path) {
        for(auto held: outline.flagOf) if(held == path) return;
        outline.captures.push(LocalId());
        outline.flagOf.push(path);
    }

    // The local a place is rooted in, or nothing - a global, a raw address, a loan.
    void captureRoot(Outline& outline, PlaceId id) {
        auto place = source.place(mir, id);
        while(place->parent) place = mir[place->parent];
        if(place->kind != mir::Place::Local) return;

        capture(outline, LocalId(place->payload));
    }

    static void addOutward(Outline& outline, U64 row) {
        for(auto held: outline.outward) if(held == row) return;
        outline.outward.push(row);
    }

    void plan(ContinuationId region) {
        Outline outline;
        outline.region = region;
        auto own = source.region(mir, region);

        auto elaboration = source.elaboration ? mir[source.elaboration] : nullptr;

        for(U32 i = 0; i < source.blocks.size(); i++) {
            auto block = source.block(mir, BlockId(i));
            if(block->region != region) continue;

            for(auto nodePointer: block->nodes.contents(mir)) {
                auto& node = *mir[nodePointer];

                mir::forEachOperand(mir, node, [&](mir::Operand& operand, mir::OperandRole) {
                    if(operand.source() == mir::SourceKind::Place) {
                        captureRoot(outline, operand.placeId());
                    }
                });

                // The storage a borrow produced here is rooted in - `attachLoan`, which reads the
                // same table. Not reachable through an operand: a value's loan is the analysis'
                // answer about the value rather than something the node names.
                if(elaboration) {
                    for(U32 l = 0; l < elaboration->loanCount; l++) {
                        if(!elaboration->rootedIn(mir, node.id, LoanId(l))) continue;
                        captureRoot(outline, source.loan(mir, LoanId(l))->place);
                    }
                }

                if(node.kind == mir::Node::Return) {
                    outline.leaves = true;
                    if(((mir::NodeReturn&)node).hasValue) outline.resultField = 0;
                }

                /*
                 * An edge that leaves this region, which is the fall-out exit.
                 *
                 * A `lens.call`'s edge is not one and is the reason this asks the kind: it *enters*
                 * a region nested in this one, and control comes back at that region's destination -
                 * which is a block of this one.
                 */
                if(mir::isLensCall(node.kind)) continue;

                mir::forEachEdge(mir, node, [&](mir::Edge& edge) {
                    if(!edge.target) return;
                    auto to = mir[edge.target];
                    if(to->region == region) return;

                    // Anywhere but the destination is the landing of a `break` or `continue` for
                    // a loop outside the lens call - a row of its own (`kExitOutward`).
                    if(to->index != own->destination) {
                        addOutward(outline, outwardRow(to->index));
                        return;
                    }

                    outline.fallsOut = true;
                });
            }
        }

        // The teardowns the analysis placed in these blocks name storage too, and a drop is the one
        // consumer that reaches a place no operand does - a survivor of a partial move.
        if(elaboration) {
            for(U32 i = 0; i < elaboration->drops.size(); i++) {
                auto step = elaboration->drops.get(mir, i);
                if(source.block(mir, BlockId(step.block))->region != region) continue;

                captureRoot(outline, step.place);
            }

            /*
             * The **drop flags** the region writes, and the ones its conditional drops test - the
             * caller's storage, captured (`Outline::flagOf`).
             *
             * A flag is a `Bool` in the frame that one path writes and another reads, so a flag
             * written by the continuation and read by the caller has to be one slot for the nest:
             * what the result carrier already is for a returned value.
             */
            for(U32 i = 0; i < elaboration->flags.size(); i++) {
                auto step = elaboration->flags.get(mir, i);
                if(step.position == mir::FlagStep::kEntry) continue;
                if(source.block(mir, BlockId(step.block))->region != region) continue;

                captureFlag(outline, step.path.index);
            }

            for(U32 i = 0; i < elaboration->drops.size(); i++) {
                auto step = elaboration->drops.get(mir, i);
                if(!step.conditional) continue;
                if(source.block(mir, BlockId(step.block))->region != region) continue;

                captureFlag(outline, step.path.index);
            }
        }

        /*
         * What the regions nested in this one need, which this one has to be able to hand them.
         *
         * A child's environment is built by *this* region's blocks, so a local the child captures
         * and this region does not declare has to reach here first. Children are planned before
         * parents, so their answers are complete.
         */
        for(auto& child: into) {
            if(!source.encloses(mir, region, child.region)) continue;

            for(U32 i = 0; i < child.captures.size(); i++) {
                auto held = child.captures[i];
                if(child.flagOf[i] != Outline::kNoFlag) { captureFlag(outline, child.flagOf[i]); continue; }
                if(!held.valid()) { outline.resultField = 0; continue; }
                capture(outline, held);
            }

            /*
             * A child that leaves makes its *parent* leave, because propagating is what the call
             * site emits: the discriminant comes back here and there is no frame in between that
             * could act on it. Direct children only - a grandchild's answer is already in its own
             * parent's.
             */
            // A child's outward rows are not: each lands in this region, so the child's call
            // site takes them as jumps.
            if(source.region(mir, child.region)->parent == region) outline.leaves |= child.leaves;
        }

        if(outline.resultField != Outline::kNoField) {
            outline.resultField = U16(outline.captures.size());
            outline.captures.push(LocalId());
            outline.flagOf.push(Outline::kNoFlag);
        }

        build(outline);
        into.push(static_cast<Outline&&>(outline));
    }

    /*
     * The lifted declaration and its environment record - `resolve_lambda.cpp`'s shape, one tier
     * down and for the same reason.
     *
     * A continuation *is* a closure over the caller's frame, so it is built exactly as a lambda is:
     * one record of borrowed fields, an environment parameter in front of the ones the position
     * declares, and a `fun.value` pairing the two. What differs is only who wrote it - nothing in
     * the source - and what it answers with, which is the exit discriminant.
     */
    void build(Outline& outline) {
        auto module = decl()[function.module];
        auto region = source.region(mir, outline.region);

        auto text = Tritium::format("%@.cont", context.findName(function.symbol));
        auto name = program.uniqueSymbol(function.module, context,
                                         StringView(text.text(), text.size()));

        auto envText = Tritium::format("%@.env", context.findName(name));
        auto envName = program.uniqueSymbol(function.module, context,
                                            StringView(envText.text(), envText.size()));

        auto record = program.types.recordType(envName, function.module, function.source);
        auto recordPointer = asBase<mir::Type>(DeclPtr<mir::RecordType>(record - *program.decl));

        auto carried = context.addUnqualifiedName("$leaving", 8);
        auto flagName = context.addUnqualifiedName("$flag", 5);

        for(U32 c = 0; c < outline.captures.size(); c++) {
            auto held = outline.captures[c];
            auto isFlag = outline.flagOf[c] != Outline::kNoFlag;

            mir::RecordField field;
            field.name = held.valid() ? source.local(mir, held)->name : isFlag ? flagName : carried;
            field.source = function.source;
            field.index = U16(record->fields.size());

            /*
             * An **external** field, in an origin slot of its own - the same statement a lambda's
             * borrowed capture makes. One slot each rather than one for all of them, because they
             * are not one object.
             */
            auto captured = held.valid() ? substituted(source.local(mir, held)->occurrence)
                          : isFlag         ? mir::owned(mir::booleanType(program))
                                           : substituted(function.result);

            /*
             * And the field says **what the value is**, not only what type it has - the access form
             * and the access come from the binding being captured.
             *
             * A field is filled by a load out of the local's storage *at this occurrence*, so the
             * occurrence is what decides which value that load produces. A shared window local is
             * `Array(a) [shared call view]` and holds a view record; a field spelled
             * `Array(a) [unique slot n]` asks that same storage for the owner, and on a machine the
             * two are one address so nothing showed it - on JavaScript they are two different host
             * objects and the construction is rejected (J67 §5, fixed in J70).
             *
             * The **access stays exclusive** and that is not an oversight. A field holds the address
             * of the caller's storage whatever the binding was - `captureOf`'s reader takes it as an
             * owned `%T` and every use of the local in the region goes through it - so the loan the
             * field states is the one the region may write through, and only the form says which
             * record that storage holds. Weakening it to the binding's own access was tried and is
             * worse than a spelling: a record of nothing but shared external fields is *trivially
             * copyable*, so the pair's environment word stops naming the frame's own record and the
             * library's suite aborts inside `entries`.
             */
            field.occurrence = mir::Occurrence {
                captured.type,
                mir::slotContract(mir::Access::Unique, record->originSlots,
                                  captured.contract.form()), nullptr
            };

            /*
             * And the field says it holds that **address**, which the occurrence cannot: with the
             * form carried, `[unique slot n view]` reads natively as a writable window *by value*,
             * and the environment was built as a sixteen-byte copy of it that `captureOf` then read
             * one word of as a pointer - `RecordField::holdsAddress`. Dropping the form again
             * is J70's JavaScript defect; the bit is the half that only the native layout reads.
             */
            field.holdsAddress = true;

            record->originSlots++;
            record->fields.push(program.decl, field);
        }

        mir::RecordCon only;
        only.name = envName;
        only.source = function.source;
        only.count = U16(record->fields.size());
        record->cons.push(program.decl, only);
        record->layout = mir::RecordLayout::Single;

        mir::foldRecordSchema(context, program, *record);
        mir::classifyRecord(context, program, *record);

        auto lifted = new (program.decl) mir::Function(name, function.module);
        lifted->source = function.source;
        lifted->environment = recordPointer;
        /*
         * What the continuation answers - the bare exit signal, or `Outcome(R, exit)` where the
         * iterator this is a pass of declared `-> (Y <- R)`.
         *
         * Read off the region rather than rebuilt here: `handoverCall` put the callee's declared
         * answer on it under the call's substitution, and a conversion that interned the wrapper
         * itself would be resolving a type at the tier that resolves none. A region with none is one
         * no `handoverCall` opened, and answers the bare signal.
         */
        lifted->result = region->answer.type ? substituted(region->answer)
                                             : mir::owned(program.types.exitCode());
        lifted->reachable = true;
        lifted->summary.bodyKnown = true;

        /*
         * And the one published fact this declaration would otherwise have none of: a continuation's
         * blocks *are* the lifter's blocks, so it suspends exactly when they do.
         *
         * `computeSummaries` ran over the MIR, before this declaration existed, and it does not run
         * again - so an unset fact here is read at every consumer's conservative default. That
         * default is "may suspend", and the inliner declined to copy such a body until E5 (J220)
         * stopped it reading the fact: an outlined continuation left unanswered was a caller's own
         * code that its caller could never take back. Still published for `explain` and the MIR
         * dump, and for the stackless kind that will be refused again. Copied from the lifter
         * rather than computed, because the two are one body.
         */
        auto suspends = function.summary.suspension();
        lifted->summary.setSuspension(suspends.get(false), suspends.knowledge == mir::Knowledge::Unknown
                                                               ? mir::Knowledge::Known
                                                               : suspends.knowledge);

        auto envArg = new (program.decl) mir::Arg();
        envArg->name = envName;
        envArg->occurrence = mir::borrowed(recordPointer, mir::Access::Unique);
        envArg->source = function.source;
        envArg->index = 0;
        lifted->args.push(program.decl, envArg - *program.decl);

        auto handed = new (program.decl) mir::Arg();
        handed->name = context.addUnqualifiedName("$handed", 7);
        handed->occurrence = substituted(region->handover);
        handed->source = function.source;
        handed->index = 1;
        lifted->args.push(program.decl, handed - *program.decl);

        module->records.push(program.decl, DeclPtr<mir::RecordType>(record - *program.decl));
        module->functions.push(program.decl, DeclPtr<mir::Function>(lifted - *program.decl));

        outline.lifted = lifted - *program.decl;
        outline.environment = recordPointer;
    }

    void run() {
        // Innermost first: a parent has to be able to hand a child what the child captures, and the
        // creation order is outermost first.
        for(U32 i = source.regions.size(); i-- > 1;) {
            auto region = source.region(mir, ContinuationId(i));
            if(region->kind == mir::Continuation::Handover) plan(ContinuationId(i));
        }
    }
};

/*
 * A body grafted into its caller - `@inline(always)`, `Function::alwaysInline`.
 *
 * The third shape a conversion takes, beside a function of its own and an outlined continuation:
 * the callee's blocks are made in the *caller's* function, its parameters are the call's prepared
 * arguments, and its `return` is a jump to `join` carrying the result as that block's one argument.
 * Everything else about the body converts exactly as it would in a function of its own, reading the
 * callee's own analysis - which is what makes this a graft and not an inliner: no cost is weighed,
 * no summary is re-derived, and a body that could not be treated this way was refused where it was
 * declared.
 *
 * Why here and not in the resolver: the callee is generic until the call names its arguments, and
 * this is the tier that converts a body under a substitution. Why here and not in the optimizer: the
 * corpus reads what the resolver produced, `-no-ir-opt` is supported, and a `length` that is one
 * load after optimization and a call before it is a function whose cost depends on a flag.
 */
struct Graft {
    const Use* arguments;
    Size count;

    // The callee's entry, made by the caller in its current region so that the graft's blocks nest
    // where the call was, and jumped to from the block the call was in.
    Block* entry;

    // Where `return` lands. `result` is its one block argument, or null for a unit result.
    Block* join;
    Value* result;

    /*
     * Where the graft was written - the **outermost** call site, kept across nested grafts.
     *
     * One reader, and it is what makes `-check-locations` name the subscript rather than the
     * library: `Index.get` writes its own bounds test and is grafted, so a `checkFailed` inside it
     * carries `lib/Core/Array.yana`'s line and the site an author wants named is `xs[i]`. That is
     * `#[track_caller]` semantics obtained from the graft, and it needs no parameter, no `Site`
     * record materialized on the passing path and no change to a class signature - which is why a
     * `@caller` fill was not the mechanism: `Index.get`'s parameter list is the class's.
     *
     * Outermost rather than nearest, because a check three library bodies deep is still the
     * author's subscript and every frame in between is the compiler's own.
     */
    LocationId site;
};

/*
 * An iterator object's **step**, as a second reading of the producer's own MIR body -
 * `doc/inflight/Design-Iterator-Objects.md` §3.2, and `resolve_mir/resolve_object.cpp` for the type.
 *
 * The fourth shape a conversion takes, beside a function of its own, an outlined continuation and a
 * graft - and the only one that changes what the *body* means rather than where it is emitted.
 * Three things are read differently and nothing else is:
 *
 *  - **a local's storage.** A parameter is a field of `self` - the captured argument - and a local
 *    the producer keeps across a `yield` is a field too. Everything else is an ordinary `alloc`, in
 *    the step's own frame, because it is dead by the time the step returns.
 *  - **a `yield`**, which in MIR is a call through the continuation parameter: write the hand-over
 *    into the slot, write the index, and return `True`. The call's *result* - what the continuation
 *    answered - is the step's own `$resume` argument, in the block the next step resumes at.
 *  - **a `return`**, which is the producer having run out or having been left: write the finished
 *    index and return `False`.
 *
 * So the producer's departure path is the record's teardown with nothing added: the step is handed
 * "leave" by `f$drop`, `resolveYield`'s own test takes the branch, `emitDeparture` walks out of the
 * loops, the statements below them run, and drop elaboration releases every local that is still
 * live. §3.3's "shared across indices" is what converting one body once already does.
 */
struct StepPlan {
    static constexpr U32 kNoField = ~U32(0);
    static constexpr U32 kNotAYield = ~U32(0);

    mir::RecordType* record = nullptr;

    // Per MIR local: which field of the record holds it, or `kNoField` for a step-frame local. Per
    // MIR node: which `yield` it is, and whether it is the read of the continuation parameter that
    // one goes through - the one node in the body with no storage and no value at all.
    Array<U32> localField;
    Array<U32> yieldOf;
    Array<U8> readsContinuation;

    // And per MIR local: whether its field holds a run's address rather than the value - `@heap` on
    // the producer's binding, `RecordType::kHeapMember`. The fields of those members, in field
    // order, are what the exits free.
    Array<U8> localRun;
    Array<U16> runFields;

    // And per MIR local: whether the record carries it as a **borrow** - a pointer field written
    // where the binding is established and read at every use. `RecordType::kBorrowMember`.
    Array<U8> localBorrow;

    /*
     * The **embedded regions**, per `lens.call` node - `RecordType::kNestMember`, §3.2's first step.
     *
     * `nestOf` is which field holds each region's child, `nestFields` is the same list in field order
     * and is what the exits cancel, and `nestAdvance` is the block that steps the child - made where
     * the call is embedded and jumped to by every block of the region that falls out. Keyed by
     * *region* rather than by node, because that is what an edge leaving one has in hand, and the
     * record's entry is the node the region was opened by.
     *
     * A `for x in g(...)` and a `withLock(m):` are the same three lists: what differs is only what
     * the second entry to `nestAdvance` does, which is the callee's own business - the next element
     * for an `iter fn` and the code after the `yield` for a `lens fn`. See `convertStepNest`.
     */
    Array<U32> nestOf;
    Array<U16> nestFields;
    Array<SsaPtr<Block>> nestAdvance;

    /*
     * And which of them are **boxed** - §6.3's recursion, `RecordType::nestBoxed`.
     *
     * The `g$erased.heap` shim per region, or null where the member holds the child's record inline.
     * A boxed member's field is the protocol `iter(Y)`, so the shim is the only thing that names
     * what is in the box: its `generatedRecord` is the child's site type, and its own address is the
     * pair's code word.
     *
     * `nestFieldBoxed` is the same answer in `nestFields` order, which is what the exits walk: they
     * have a field in hand rather than a region, and a boxed member is released where a concrete one
     * is cancelled.
     */
    Array<DeclPtr<mir::Function>> nestBoxed;
    Array<DeclPtr<mir::Function>> nestFieldBoxed;

    /*
     * And which of them **delegate** - §6.3's `yield from`, `RecordType::nestDelegates`.
     *
     * Per region, beside `nestBoxed`, and what it changes is the whole of the emission: a delegating
     * region is never *stepped* here. The call stores the child's pair into the member's link, writes
     * the link's address into `$down`, and suspends this level - so there is no `advance`, and the
     * region's own blocks are not converted at all (`inScope`), because what they contain is a
     * hand-over of the item and nothing else and the consumer is about to take that item from the
     * child directly.
     *
     * `delegateResume` is where the level comes back when the child has run out: it releases the box
     * and carries on at the region's destination, which is exactly what `advance`'s false arm does
     * for a level that steps its own child.
     */
    Array<U8> nestDelegate;

    /*
     * And where a delegating level **comes back** - the block the dispatch enters it at once the
     * child has run out, one index per region past the hand-overs' own.
     *
     * What it does is the two things the region's own blocks would have done and this level no
     * longer runs: it writes what the consumer answered into `guardYields`' local, so the code after
     * the region can see that the consumer left, and it releases the box. Then it carries on where
     * the region rejoins, which is `advance`'s false arm.
     */
    Array<SsaPtr<Block>> delegateResume;

    /*
     * The **values** the record spills - what crosses a hand-over without a name, which a counted
     * `for`'s own block arguments are (`resolve_mir/resolve_object.cpp`'s `LiveAcross`).
     *
     * Three parallel lists rather than a table keyed by node, because what reads them is a loop over
     * the ones live at *this* `yield`: `spillEntry` is the payload entry the record holds each in,
     * and `RecordType::keepsAcross` is what says whether this hand-over keeps it - a spill it does
     * not keep is one whose value does not exist yet on this path.
     */
    Array<U32> spillValue;
    Array<U32> spillField;
    Array<U32> spillEntry;

    /*
     * Which definition of each spilled value a block **uses** - `planSpillReach`, J164.
     *
     * A spilled value has one definition in MIR and one more per hand-over that keeps it: the reload
     * the resume block makes. Replacing the value "for everything converted from here on" was right
     * only while the last reload converted dominated every later block, which a second `yield` in
     * one loop body breaks - `yield a; if c: yield a` rejoins with the first reload on one arm and
     * the second on the other, and the verifier said "jump names N, whose definition does not
     * dominate it". So the reaching definition is computed per block, over the MIR graph the step
     * converts, and a block whose uses see one definition takes that one's value while a block that
     * sees several, or one converted after it, reads the field - which the definition then also
     * writes, so that every path into the block has put the value there.
     *
     * One entry per spill and block, `spillBlocks` to a row: `kSpillNothing` where the block does
     * not use the value or defines it, `kSpillFromDefinition`, `kSpillLoad`, or the hand-over whose
     * reload it takes.
     */
    static constexpr U32 kSpillNothing = ~U32(0);
    static constexpr U32 kSpillFromDefinition = ~U32(1);
    static constexpr U32 kSpillLoad = ~U32(2);

    U32 spillBlocks = 0;
    Array<U32> spillAction;
    Array<U8> spillStoreAtDefinition;

    // Where each is defined, and whether as that block's argument rather than by one of its nodes.
    Array<U32> spillDefinedIn;
    Array<U8> spillIsArgument;

    // And what the conversion made for each, filled as it goes: the definition's value, and one
    // reload per spill and hand-over (`yields` to a row). Zero where it has not been made.
    Array<SsaPtr<Value>> spillDefined;
    Array<SsaPtr<Value>> spillReloaded;

    U32 continuationArg = 0;
    U32 yields = 0;

    // Filled as the conversion goes: where the step resumes for each index. `1 + k` is the index of
    // `yield` k, so index zero stays "has not started" - see `resolve_object.cpp`.
    Array<SsaPtr<Block>> resume;

    Value* self = nullptr;
    Value* resumeValue = nullptr;
    Value* cancel = nullptr;

    Block* fresh = nullptr;
    Block* finished = nullptr;
    Block* bodyEntry = nullptr;
};

struct Conversion {
    Context& context;
    mir::Program& program;
    SsaArena& arena;
    mir::Function& function;
    /*
     * The body being converted, which for a specialization is the **generic's** - `specialize.h`.
     *
     * There is one resolved body per generic function and one conversion per specialization of it,
     * so this is where a generic program stops being generic: every type the walk reads is put
     * through `with` on its way out, and everything else - the nodes, the places, the use modes, the
     * drop points - is used exactly as the resolver and the ownership analysis left it.
     *
     * That is Implementation-Generics.md §1's invariant made structural rather than checked. A
     * specialization cannot select a different instance or drop a value at a different point,
     * because it is not a second body: it is this one read at concrete types.
     */
    mir::Body& source;
    MirBase mir;
    Scratch& scratch;

    // Empty for every ordinary function, which is what makes `substituted` free where nothing
    // is generic.
    mir::Substitution with;

    // Shared with a graft: a callee expanded into this function appends to the same body through
    // the same editor, which is what makes its blocks this function's.
    Editor& editor;

    Array<SsaPtr<Value>>& produced;
    Array<SsaPtr<Block>>& blocks;
    Array<SsaPtr<Value>>& slots;
    Array<SsaPtr<Value>>& flags;
    Array<SsaPtr<Value>>& outcomes;

    /*
     * Where the ownership analysis said the teardowns go, and how far through them this walk is.
     *
     * Two cursors rather than a search per node: both lists are sorted by block and then by
     * position, and the blocks are converted in index order, so each is read once from front to
     * back. Null where the body was converted before the analysis ran, which nothing does - the
     * pipeline orders them - and is checked rather than assumed.
     */
    mir::Elaboration* elaboration = nullptr;
    U32 dropCursor = 0;
    U32 flagCursor = 0;

    // The MIR block being converted, which a lens call's leave path reads its steps at (K6).
    U32 convertingBlock = 0;

    U32 errors = 0;

    /*
     * Which region's blocks this conversion emits, and which outline it *is* - W-C.
     *
     * A body with lens calls in it becomes several functions, and every one of them reads the same
     * MIR body: the enclosing function emits the blocks of region zero, and each `Handover` region
     * emits its own. So one conversion is one *scope*, `outlining` is null for the first of them,
     * and everything below asks the scope rather than testing for a lens.
     *
     * A **step** is the exception and is one scope over the whole body: a nested traversal's region
     * is embedded rather than outlined, because the producer it belongs to is suspended rather than
     * running. `inScope` is where that is said once.
     */
    ContinuationId scope { 0 };
    Array<Outline>& outlines;
    Outline* outlining = nullptr;

    /*
     * The slots this scope allocated off the frame - `mir::StorageClass::Heap`, and the addresses
     * `releaseSlots` hands back.
     *
     * A member of the conversion rather than of `Scratch`, because the pairing is per *scope*: a
     * grafted body and a continuation each allocate their own and each leaves by its own
     * terminators, and a shared table would release one scope's storage at another's exit.
     */
    Array<SsaPtr<Value>> heapSlots;

    // The environment parameter, in a continuation - what every capture is loaded out of.
    SsaPtr<Value> environment = nullptr;

    // Where an exit that leaves the enclosing function parks what it carries. One slot for the whole
    // nest - see `Outline::resultField` - allocated by the outermost scope and captured by the rest.
    SsaPtr<Value> resultCarrier = nullptr;

    // The function being filled, which is the enclosing one only in the first scope. `function` is
    // still the *owner* - whose body this is, whose substitution it is read under, and whose
    // declaration `callTarget` resolves a call against.
    mir::Function& emitted;

    // Set where this conversion is a body grafted into `emitted` - see `Graft`. `graftDepth` is how
    // many grafts deep this one is, which is what stops an always-inline body that calls one.
    Graft* grafting = nullptr;
    U32 graftDepth = 0;

    // Whether a graft below this conversion was refused at the depth limit. What the refused call
    // would have produced is then missing on purpose, so an operand naming it is that refusal's
    // consequence and not a second fault - counted, and not reported as an internal error.
    bool graftRefused = false;

    // Set where this conversion is the **step** of an iterator object - `StepPlan`, which is where
    // the three things it reads differently are written down.
    StepPlan* stepping = nullptr;

    Conversion(Context& context, mir::Program& program, SsaArena& arena, mir::Function& owner,
               mir::Function& target, Editor& editor, Scratch& scratch, Array<Outline>& outlines,
               Outline* outlining, Graft* graft = nullptr, U32 depth = 0):
        context(context), program(program), arena(arena), function(owner),
        source(*(*program.mir)[mir::bodyOwnerOf(*program.decl, owner).mir]),
        mir(*program.mir), scratch(scratch),
        with(mir::substitutionOf(*program.decl, owner)),
        editor(editor),
        produced(scratch.produced), blocks(scratch.blocks), slots(scratch.slots),
        flags(scratch.flags), outcomes(scratch.outcomes),
        scope(outlining ? outlining->region : ContinuationId(0)),
        outlines(outlines), outlining(outlining), emitted(target), grafting(graft),
        graftDepth(depth)
    {
        scratch.reset(source.nodes.size());
        if(source.elaboration) elaboration = mir[source.elaboration];
    }

    /*
     * Whether a block belongs to the scope this conversion emits.
     *
     * Every block, in a **step**: a nested traversal's region is *embedded* rather than outlined, so
     * the producer's whole body is one function here - §3.2's first step, and the one place a step
     * and the continuation-passing form disagree about which blocks are whose.
     */
    bool inScope(BlockId block) {
        /*
         * A **delegated** region's blocks are not converted at all - §6.3.
         *
         * The pass is a hand-over of the child's item and nothing else (`Continuation::reyield`), and
         * under delegation the consumer takes that item from the child directly - so there is nothing
         * of this level between the two and its blocks would be unreachable. Skipped rather than
         * emitted and left unreachable, because dominance is not defined in a block the entry cannot
         * reach and `verifyBody` says so.
         *
         * What the level still owes is the one write those blocks would have made: `guardYields`'
         * own local, which the code after the region reads to find out whether the consumer left.
         * `stepDelegateResume` is where it is written.
         */
        if(stepping) return !insideDelegated(source.block(mir, block)->region);
        return source.block(mir, block)->region == scope;
    }

    bool insideDelegated(ContinuationId region) {
        auto at = region;

        while(at.valid()) {
            if(at.index < stepping->nestDelegate.size() && stepping->nestDelegate[at.index]) {
                return true;
            }

            auto held = source.region(mir, at);
            if(!held) break;
            at = held->parent;
        }

        return false;
    }

    // The region of the block being converted, which is `scope` everywhere but in a step - see
    // `inScope`. What reads it is `convertEdge`, where an edge leaving a nested region is the one
    // that asks the child for its next element.
    ContinuationId emitting { 0 };

    Outline* outlineFor(ContinuationId region) {
        for(auto& entry: outlines) if(entry.region == region) return &entry;
        return nullptr;
    }

    SsaBase base() { return *arena; }
    DeclBase decl() { return *program.decl; }

    /*
     * **There is no `Layout` here any more** - Analysis-IR-JS.md §12.3's first open question, closed
     * by J75.
     *
     * This walk used to hold one, and ask the native manifestation table at fifteen sites whether a
     * value was reached through an address and whether duplicating it was an operation. Which meant
     * the shared tier's SSA was shaped by *one* target's layout, and §1.3.1's survey is the reason
     * that was worth removing: almost all of it was a contract fact reached through a table that
     * also knows about machines. What is left are three target-free predicates in `build.h` -
     * `loanDenotesStorage`, `denotesStorage` and `holdsAggregate` - and the rule that nothing in
     * `resolve_ssa` outside `lower/` includes `lower/`, which `tools/check-layering.sh` holds.
     */

    /*
     * One MIR type or occurrence, as this function sees it - the whole of what a specialization is.
     *
     * Every occurrence that crosses into OSSA goes through here, and the reason it is a two-line
     * function rather than a call at each site spelled out is that the sites are what has to be
     * exhaustive: an occurrence that reached the tier below unsubstituted would be a value typed
     * `a` at a point where a register has to be chosen, and nothing between here and the machine
     * would say which one.
     */
    mir::Occurrence substituted(const mir::Occurrence& occurrence) {
        return mir::substituteOccurrence(context, program, occurrence, with);
    }

    DeclPtr<mir::Type> substituted(DeclPtr<mir::Type> type) {
        return mir::substituteType(context, program, type, with);
    }

    /*
     * What a skipping call answers - `Outcome(exit, E)` - in this body's terms.
     *
     * Built from the call's own `reason` rather than read off the callee, because the callee's
     * declaration is written over *its* variables: a class member's `| error` is the class's, and a
     * generic traversal's `| e` is its own, so the declared wrapper names a type no value here has.
     * `reason` is that failure already substituted into the enclosing body's terms (`NodeLensCall`),
     * and substituting once more is this specialization's. The wrapper is interned, so a concrete
     * callee answers the very record its declaration names - which is what this used to read, and
     * what it could read only while `declareSignature` refused a failure type with variables in it.
     */
    mir::Occurrence skipOutcome(mir::NodeLensCall& node, LocationId source) {
        auto failure = substituted(node.reason.type);
        if(!failure) return mir::Occurrence {};

        DeclPtr<mir::Type> arguments[2] = { program.types.exitCode(), failure };
        auto type = mir::instantiateRecord(context, program,
                                           program.roles.record(intrinsic::Known::outcome),
                                           arguments, 2, source);
        return type ? mir::owned(type) : mir::Occurrence {};
    }

    void report(LocationId at, StringView text) {
        errors++;
        context.diagnostics.error(text, at);
    }

    /*
     * The float unaries' operand check, made **here** because here is where the type is known.
     *
     * `sqrt`, `fma` and the four roundings are `op.def` roles over an unconstrained `a` - there is
     * no class in the language that says "this is a float", so `expandVecFloatUnary` holds the
     * argument to one by looking at the type at the call. Inside a generic body there is no type to
     * look at, and refusing there is what kept `lib/Math` out of this stage: every one of
     * `modulusOf`, `logOf`, `hypotenuseOf` and the trigonometric bodies rounds or takes a root of
     * its own `Real(a, m)` parameter.
     *
     * So the check moves rather than going away, and this is the one point it can move to: a
     * specialization is the generic's body read at concrete types, so the substituted occurrence is
     * exactly what the emitter would have seen had the call been concrete. The node's own source is
     * the call the author wrote, so the message lands where it did before.
     */
    bool requireFloatOperand(mir::UnaryCategory category, DeclPtr<mir::Type> type, LocationId at,
                             StringView what) {
        if(category != mir::UnaryCategory::Float) return true;
        if(mir::operandTypeAllowed(program, category, type)) return true;

        errors++;
        context.diagnostics.error("%@ takes a float or a vector of floats, and this is %@"_v, at,
                                  what, mir::typeText(context, decl(), type));
        return false;
    }

    // §7.2's four rungs, in `build.h` - a free function because `Editor::addConst` asks the same
    // question of a constant's type with no conversion in hand.
    Convention conventionFor(const mir::Occurrence& occurrence) {
        return ssa::conventionFor(decl(), occurrence);
    }

    // The use an MIR mode becomes. `Forward` must not reach here: liveness resolves it to `Take`
    // or reports that the source needs an explicit spelling, and a `Forward` in a converted body is
    // that pass having been skipped.
    bool useKindFor(mir::UseMode mode, UseKind& into) {
        switch(mode) {
            case mir::UseMode::Take:
            case mir::UseMode::CopyTrivial:
                into = UseKind::Consume;
                return true;
            case mir::UseMode::BorrowShared:
            case mir::UseMode::BorrowUnique:
                into = UseKind::Borrow;
                return true;
            case mir::UseMode::Inspect:
                into = UseKind::Inspect;
                return true;
            case mir::UseMode::Forward:
            case mir::UseMode::Count:
                return false;
        }

        return false;
    }

    /*
     * The address of a place.
     *
     * A place is a root and a projection path, and this is the walk that turns it into arithmetic.
     * Only the two forms this milestone builds; a projection becomes a field address and an index a
     * computed one, and both belong here rather than at each consumer, which is the single-place rule
     * the whole rewrite is organized around.
     */
    /*
     * A value used for the storage it names rather than for what is in it.
     *
     * `Inspect` where the value owns nothing, and `Borrow` where it does. `Editor::use` refuses to
     * inspect a value the linear dataflow tracks, and rightly: an inspection carries no obligation,
     * and a tracked value's uses are exactly what that dataflow counts. Taking the address of one is
     * a dependency on it, which is what a borrow says.
     */
    /*
     * Whether a local of this occurrence *is* its lender's storage rather than storage of its own.
     *
     * The one predicate two walks have to agree on - the slot loop below, which decides whether a
     * local gets an allocation, and the `Init` that establishes a binding, which decides whether
     * there is anything to store. They used to be two tests and the second was the coarser one, so
     * a loan of a register-valued type made a local with no slot and a write with nowhere to go.
     *
     * **Not `loanDenotesStorage`**, and this is the one site where §1.3.1's survey is worth
     * qualifying. What this decides is whether the *lowering* can use the incoming pointer as the
     * local's storage, so the question is whether the loan manifests as one - the calling
     * convention's question, which `denotesStorage` answers target-free. The semantic predicate
     * answers "value" for a shared call-bounded loan of a trivially copyable aggregate, and a local
     * of one would then be allocated at the **loan's** occurrence - a pointer-wide slot that a
     * `field.addr` would project *into* rather than through. See `lower/README.md`.
     */
    bool aliasesLender(const mir::Occurrence& held) {
        return referenceValued(held);
    }

    // The same question at the occurrences an *owner* is among - `arrivesAsAddress`, target-free.
    // A call's question rather than a loan's; see `denotesStorage`.
    bool reachedAsAddress(const mir::Occurrence& held) {
        return denotesStorage(context, program, held);
    }

    // Today's `isReferenceValue`: a value that **is** an address rather than one whose bytes are at
    // one. The pair `denotesStorage` answers together, split where a site needs only this half.
    bool referenceValued(const mir::Occurrence& held) {
        return ssa::referenceValued(context, program, held);
    }

    /*
     * **`readMode`'s first clause, re-asked with the substitution in hand.**
     *
     * `readMode` answers `CopyTrivial` for a trivially copyable place and a shared loan for
     * everything else, and a *generic* body asks it at a type variable: `a` owes a teardown as far
     * as the MIR builder knows, so `fn (Num(a)) g(x: a) = x + x` reads its parameter
     * `borrow.shared` where the same function written at `Int` reads it `copy.trivial`.
     * Substitution is where that stops being true, and this tier is the first that has the
     * substituted type.
     *
     * `specialize.h` says a specialization decides nothing, and it still does not: the instance
     * selection, the use modes and the drop points are the generic body's and are read unchanged.
     * What this changes is the *representation* of one occurrence, which `manifest.cpp` already
     * states is the same either way - "a trivial value's loan and its copy are physically the same
     * thing". This restores an invariant the manifestation table already assumes rather than adding
     * one, and `lower/gen.cpp` builds a signature from the *declared* occurrence, so nothing about
     * the ABI moves.
     *
     * Both guards carry weight. Without triviality a copy is a second owner of something that owes
     * a teardown; without `referenceValued` a shared loan of an aggregate would become a copy of the
     * aggregate - correct, and a pessimization, since the loan of one of those is its address. That
     * second guard is §1.3.1's category 2 and it is a *cost* question; what changed is where it is
     * asked from. It used to reach the native manifestation table, and it now reads
     * `holdsAggregate`, which is the same rows with the numbers left off.
     *
     * What it is worth is the *slot*. A borrow is a second name for storage, so a parameter read
     * through one keeps its frame slot: `promote` declines a slot whose store installs a
     * `Guaranteed` value and whose loads read back `Trivial`, a convention change being no
     * substitution, and the inliner's `sizeOf` then counts a store and a load per parameter that is
     * never emitted. Before this, every specialized generic body and every borrowed scalar
     * parameter carried both.
     */
    mir::Occurrence valueOccurrenceOf(const mir::Occurrence& held) {
        if(held.contract.isOwned()) return held;
        if(!mir::isTriviallyCopyableAt(decl(), held)) return held;
        if(referenceValued(held)) return held;

        return mir::owned(held.type);
    }

    Use addressUse(Value& value) {
        return editor.use(value, isTracked(value.convention) ? UseKind::Borrow : UseKind::Inspect);
    }

    /*
     * A type with **one value** read out of storage is that value, and never a load - the rule the
     * `Read` case gives the reason for, and a `lens.try`'s `| {}` failure the second place it
     * applies. Null for any other type, and the caller loads. One member rather than the test
     * written at each, since a load of zero bytes is what `lower/inst.cpp` asserts against and a
     * site that forgot it was found only by an unoptimized build of `-> (Int | {})`.
     */
    Value* unitValue(LocationId at, const mir::Occurrence& held) {
        if(decl()[held.type]->kind != mir::Type::Unit) return nullptr;
        return editor.resultOf(*editor.addConst(at, mir::unitConstant(held.type)));
    }

    Value* addressOf(mir::Place& place, LocationId at) {
        switch(place.kind) {
            case mir::Place::Global: {
                auto global = DeclPtr<mir::Global>(place.payload);
                return editor.resultOf(*editor.addGlobalAddress(at, global));
            }
            case mir::Place::Local: {
                /*
                 * A **borrow** the record carries across a hand-over - the pointer read out of its
                 * field, here rather than once at the step's entry.
                 *
                 * Per use, which is the one thing that makes it correct: the field is written where
                 * the producer's own `let held = base` establishes the binding, and a value cached
                 * at the entry would be the contents from before that write - while a value cached
                 * at the write would not reach the resume blocks, which the dispatch enters without
                 * passing through it. The address the load is made from is `self`, a block argument,
                 * so nothing about this depends on the order blocks are emitted in.
                 */
                if(stepping && place.payload < stepping->localBorrow.size() &&
                   stepping->localBorrow[place.payload]) {
                    auto field = U16(stepping->localField[place.payload]);
                    auto held = mir::owned(stepping->record->field(decl(), field).occurrence.type);
                    auto slot = stepField(at, field);

                    return editor.resultOf(*editor.addLoad(at, editor.use(*slot, UseKind::Inspect),
                                                           held, conventionFor(held), false));
                }

                if(place.payload >= slots.size() || !slots[place.payload]) {
                    report(at, "internal: a place names a local with no storage"_v);
                    return nullptr;
                }

                return base()[slots[place.payload]];
            }
            case mir::Place::Field: {
                auto parent = mir[place.parent];
                auto base = addressOf(*parent, at);
                if(!base) return nullptr;

                /*
                 * The field *index*, not an offset. Which bytes it is at is the manifestation's
                 * answer and is asked once, at lowering - see `InstFieldAddress`.
                 *
                 * On the record the **occurrence** manifests as - `fieldRecordOf`, the same
                 * function the resolver looked the name up through - so a window's field is at the
                 * view's offset. A view has no interior positions of the owner's, so the shape
                 * that travels with it is empty.
                 */
                auto held = substituted(parent->occurrence);
                auto record = mir::fieldRecordOf(context, program, held);
                auto viewed = record != held.type;

                auto inst = editor.addFieldAddress(at, addressUse(*base), record, U16(place.payload),
                                                   viewed ? nullptr : parent->occurrence.shape);
                return editor.resultOf(*inst);
            }
            /*
             * A structural field requirement, spent - `mir::Place::Property`.
             *
             * The place named the requirement and this is where it becomes an index: the subject is
             * substituted, the field is found by the name the requirement stated, and what comes out
             * is the same `field.addr` a concrete projection would have produced. That is the whole
             * of what a property witness is under specialization, and Implementation-Generics.md
             * says so - "specialization replaces the witness call with the same direct projection
             * concrete code would have used".
             *
             * A subject still open after substituting is a *generic* body, where the offset has to
             * come out of a property slot. That is G5's, and refused by name until then.
             */
            case mir::Place::Property: {
                auto parent = mir[place.parent];
                auto subject = substituted(parent->occurrence.type);
                auto required = decl()[source.gen]->properties.get(decl(), U32(place.payload));

                /*
                 * A subject still open after substituting is a body compiled once, where the offset
                 * is what the caller wrote into the property slot. The instruction names the
                 * requirement and the lowering finds the slot - the same division `GenValue` above
                 * makes, and for the same reason: a slot number is the numbering's answer.
                 */
                if(subject && decl()[subject]->kind == mir::Type::Variable) {
                    auto address = addressOf(*parent, at);
                    if(!address) return nullptr;

                    auto inst = editor.addPropertyAddress(at, addressUse(*address),
                                                          U16(place.payload),
                                                          substituted(required.result));
                    return editor.resultOf(*inst);
                }

                if(!subject || decl()[subject]->kind != mir::Type::Record) {
                    context.diagnostics.error(
                        "internal: a structural field requirement stands over a type with no fields"_v,
                        at);
                    return nullptr;
                }
                auto& record = *(mir::RecordType*)decl()[subject];

                for(auto field: record.fields.contents(decl())) {
                    if(field.name != required.field) continue;

                    /*
                     * The field the requirement named, at the type it promised.
                     *
                     * Checked here rather than trusted, and this is the one place it *can* be
                     * checked: the requirement is written about a variable and the field belongs to
                     * whatever was substituted, so the two only meet once an argument list exists.
                     * A mismatch is a body typed one way over storage laid out another.
                     */
                    if(!mir::sameType(field.occurrence.type, substituted(required.result))) {
                        context.diagnostics.error(
                            "%@ has a field called %@ of type %@, and the signature requires %@"_v,
                            at, mir::typeText(context, decl(), subject),
                            context.findName(required.field),
                            mir::typeText(context, decl(), field.occurrence.type),
                            mir::typeText(context, decl(), substituted(required.result)));
                        return nullptr;
                    }

                    auto address = addressOf(*parent, at);
                    if(!address) return nullptr;

                    auto inst = editor.addFieldAddress(at, addressUse(*address), subject,
                                                       field.index);
                    return editor.resultOf(*inst);
                }

                context.diagnostics.error("%@ has no field called %@, and the signature requires one"_v,
                                          at, mir::typeText(context, decl(), subject),
                                          context.findName(required.field));
                return nullptr;
            }

            /*
             * A field that holds a reference: the object is at the address the field contains.
             *
             * The load is a real one, and the occurrence it is given says so - a raw pointer to the
             * field's type, which manifests as a pointer in a register. Giving it the *borrowed*
             * occurrence instead would be a load the lowering elides, because a borrowed occurrence
             * is reached through storage and a load of one is no instruction: correct where the
             * place *is* the object, which is every other loan, and exactly wrong here.
             */
            case mir::Place::Deref: {
                auto parent = mir[place.parent];
                auto slot = addressOf(*parent, at);
                if(!slot) return nullptr;

                /*
                 * A stored borrow of a container is the container's **descriptor**, and a descriptor
                 * is not a pointer to one.
                 *
                 * `data Formatted(s, a) {value: 'a, style: s}` at `a := Array(U8)` holds a base and
                 * a length inline - `manifestNative` puts the view row above the reference-value row
                 * for exactly this reason - so the field's own address already names the object and
                 * the load below would read the first element's bytes as an address. The test is
                 * made here rather than in `Builder::fieldPlace` because a generic body is where the
                 * field is projected and `a` is open there: the substitution is known at this tier
                 * and nowhere above it, which is the same argument `deriveView` is placed on.
                 */
                if(mir::viewRecordOf(context, program, substituted(place.occurrence))) return slot;

                auto held = mir::owned(program.types.pointerType(substituted(place.occurrence.type)));
                auto inst = editor.addLoad(at, editor.use(*slot, UseKind::Inspect), held,
                                           Convention::Address);
                return editor.resultOf(*inst);
            }

            /*
             * The discriminant - the same projection with a reserved index, see
             * `InstFieldAddress::kDiscriminant`. Where the tag sits is the manifestation's answer,
             * exactly as a field's offset is, so nothing here computes one.
             */
            case mir::Place::Discriminant: {
                auto parent = mir[place.parent];
                auto address = addressOf(*parent, at);
                if(!address) return nullptr;

                auto inst = editor.addFieldAddress(at, addressUse(*address),
                                                   substituted(parent->occurrence.type),
                                                   InstFieldAddress::kDiscriminant,
                                                   parent->occurrence.shape);
                return editor.resultOf(*inst);
            }

            /*
             * One element - `xs[i]`, whose index is a *value* and therefore an operand.
             *
             * The base is the container's address in both cases the language has, and which address
             * that is belongs to the manifestation: a fixed array's storage is its elements, so its
             * own address is the base; a window's is the first of its two words, and reading it is a
             * load `lowerInst` emits. Nothing here knows which, and that is the layering - the same
             * reason a field's offset is not computed here either.
             */
            case mir::Place::Index: {
                auto parent = mir[place.parent];
                auto address = addressOf(*parent, at);
                if(!address) return nullptr;

                /*
                 * The index, which the payload names as a node rather than as an operand.
                 *
                 * A place is interned and an operand is not - `Place::payload` is thirty-two bits of
                 * identity - so what an `Index` step carries is the node ID, and the mode is not a
                 * question: an index is a number that is read, never moved.
                 */
                auto produced = valueProducedBy(NodeId(place.payload), at);
                if(!produced) return nullptr;

                auto inst = editor.addElementAddress(at, addressUse(*address),
                                                     editor.use(*produced, UseKind::Inspect),
                                                     substituted(parent->occurrence.type));
                return editor.resultOf(*inst);
            }

            /*
             * Storage named by a raw address - the root with no parent, and where the checked model
             * stops.
             *
             * The payload names the node that produced the pointer, so this is the one root whose
             * address is a *value already in hand* rather than a slot, a global or a projection.
             * Nothing is loaded and nothing is computed: a `%T` in a register is the address, which
             * is the same identity `mir::Node::Address` relies on from the other direction.
             */
            case mir::Place::Address: {
                auto pointer = valueProducedBy(NodeId(place.payload), at);
                if(!pointer) return nullptr;

                /*
                 * Except a **loan of a pointer**, which is where the pointer is and not where it
                 * points: `*id(p)` over `fn id(p: '%Int) -> '%Int` names the call's result, a
                 * `%I32 [shared slot 0]`, and taking that as the address read the pointer's own
                 * bytes as the `Int` (J164). A binding in between loaded it - `let q = id(p)` is a
                 * copy out of the loan - so only a place rooted straight in such a value met it.
                 */
                if(!referenceValued(pointer->occurrence)) return pointer;

                auto held = mir::owned(pointer->occurrence.type);
                auto loaded = editor.addLoad(at, editor.use(*pointer, UseKind::Inspect), held,
                                             conventionFor(held), false);
                return editor.resultOf(*loaded);
            }

            case mir::Place::Loan:
            case mir::Place::KindCount:
                break;
        }

        report(at, "internal: this place cannot be lowered yet"_v);
        return nullptr;
    }

    /*
     * The value one node produced, by ID.
     *
     * `valueOf` takes an *operand* and answers the same question for the ordinary case; this one is
     * for a place, whose steps name nodes rather than carrying operands. Split rather than given a
     * synthetic operand, because an operand carries a use mode and a place step has none to state.
     */
    Value* valueProducedBy(NodeId node, LocationId at) {
        auto value = produced[node.index];
        if(!value) {
            if(graftRefused) errors++;
            else report(at, "internal: a place names a node that produced no value"_v);
            return nullptr;
        }

        return base()[value];
    }

    Value* valueOf(mir::Operand operand, LocationId at) {
        switch(operand.source()) {
            case mir::SourceKind::Result: {
                auto value = produced[operand.nodeId().index];
                if(!value) {
                    if(graftRefused) errors++;
                    else report(at, "internal: an operand names a node that produced no value"_v);
                    return nullptr;
                }

                return base()[value];
            }
            case mir::SourceKind::Constant: {
                // Copied out of the body's table by value, which is what lets the MIR region be
                // released the moment this walk finishes - see `InstConst`.
                auto constant = source.constant(mir, operand.constantId());
                return editor.resultOf(*editor.addConst(at, constant));
            }
            case mir::SourceKind::Place:
                report(at, "internal: a place reached a position that wants a value"_v);
                return nullptr;
        }

        return nullptr;
    }

    /*
     * One MIR operand as the SSA use it becomes: the value, and what is done to it.
     *
     * The pair together, because both halves fail for reasons the caller has to report at the same
     * place - a `Forward` that liveness never resolved, and an operand naming a node that produced
     * nothing. Returning false means a diagnostic has already been made.
     */
    bool useOf(mir::Operand operand, LocationId at, Use& into) {
        UseKind kind;
        if(!useKindFor(operand.mode(), kind)) {
            report(at, "internal: an unresolved forward reached SSA conversion"_v);
            return false;
        }

        auto value = valueOf(operand, at);
        if(!value) return false;

        into = editor.use(*value, kind);
        return true;
    }

    /*
     * The same operand where the position holds the value and not a reference to it - the load a
     * borrowed **scalar** needs before it can be added to anything.
     *
     * `fn firstOf(xs: [I32]) -> 'I32` answers a loan, and `firstOf(xs) + 1` puts that loan straight
     * into an operand position. The two are not the same bits: a shared loan of a scalar that
     * published an origin manifests as its *address* (`isReferenceValue`), so the operand the
     * arithmetic gets is a pointer where the type says `I32`. It reached the machine as an `add` of
     * a `Ptr` and an `I32` - "inconsistent argument types to operation" where the verifier is on,
     * and a number that is an address where it is not.
     *
     * Here rather than in MIR, and that is the layer split: MIR is right that reading a loan of a
     * `TrivialCopy` scalar is a read and not a copy, and which of the two forms that read *is* -
     * the value, or the address it is reached through - is a manifestation question and this is the
     * first tier that has one. `xs[i]` is the same shape and is why `Bounds.yana` could not run.
     *
     * A loan whose object is itself in memory is left alone: a record's shared loan is its address
     * and the operand positions that take one want exactly that. So the test is the pair - the
     * occurrence is a reference, and the thing it refers to is not.
     */
    bool valueUseOf(mir::Operand operand, LocationId at, Use& into) {
        if(!useOf(operand, at, into)) return false;

        auto value = base()[into.value()];
        auto held = value->occurrence;

        if(!referenceValued(held)) return true;
        if(holdsAggregate(decl(), held.type)) return true;

        auto read = mir::Occurrence { held.type, mir::ownedContract(), nullptr };
        auto loaded = editor.addLoad(at, editor.use(*value, UseKind::Inspect), read,
                                     conventionFor(read), false);

        // The operand's own kind, carried over: what the position asked for is unchanged by the
        // load, and a call whose parameter says `borrow` is verified against that word.
        into = editor.use(*editor.resultOf(*loaded), into.kind());
        return true;
    }

    /*
     * And the other direction - a value at a position that holds a **reference** to one.
     *
     * `hold(9)` where `hold` takes a `'Int`: the argument is a number in a register and the
     * position is the address of one, so the temporary that address names has to exist. The
     * lowering does exactly this at a call it still sees (`frameStorage` in `lower/inst.cpp`) and
     * that was enough while every call reached the machine as a call - but the inliner and
     * `graftCall` *erase* the call, and what is left behind is the callee's body reading a
     * parameter that is now a register value. `test/resolve/BorrowTemporary.yana` is that program,
     * and it segfaulted at every optimization level above none.
     *
     * So the slot is made here, where the call is still one, and by the time anything splices the
     * callee in the two sides already agree.
     *
     * **A raw pointer only, since J147.** Every other value reaching a position that names an origin
     * is given its temporary by the resolver (`Resolver::lentPastPosition`), where the loan analysis
     * sees it: a slot made here is after the loan check, so nothing kept a result rooted in it from
     * outliving it, and `push(hs, keep(i + 1))` in a loop left every element pointing at one slot. A
     * pointer at `self: source'%a` is the one position the resolver leaves alone, because what the
     * callee answers is an address *through* the pointer rather than of the slot it sits in - so the
     * slot here is representation and nothing outlives it. Since J162 that is `p[i]`'s call alone
     * (`Resolver::subscriptingPointer`): J147 left every pointer-typed position here, and a user's
     * `wrap(p: '%Int) -> RP {r: p}` kept this slot's address, one slot for every pass of a loop.
     *
     * Concrete positions of the argument's own type only. An **erased** position takes an address
     * whatever it was bound to and its declared type is a variable this body cannot allocate, so it
     * stays the lowering's - which is where the descriptor that sizes it is read.
     */
    bool referenceUseOf(mir::Operand operand, const mir::Occurrence& position, LocationId at,
                        Use& into) {
        if(!useOf(operand, at, into)) return false;

        auto value = base()[into.value()];
        auto held = value->occurrence;

        if(reachedAsAddress(held)) return true;
        if(!mir::sameType(held.type, position.type)) return true;
        assertTrue(decl()[held.type]->kind == mir::Type::Pointer);

        auto slot = editor.resultOf(*editor.addAlloc(at, mir::owned(held.type),
                                                     mir::StorageClass::Frame));
        editor.addStore(at, editor.use(*slot, UseKind::Inspect), editor.use(*value, into.kind()));

        auto loaded = editor.addLoad(at, editor.use(*slot, UseKind::Inspect), position,
                                     Convention::Guaranteed, false);

        auto borrowed = editor.resultOf(*loaded);
        SsaPtr<Value> lender[1] = { slot - base() };
        editor.setDependency(*borrowed, *editor.loanOn(at, lender, 1,
                                                       position.contract.access()));

        into = editor.use(*borrowed, into.kind());
        return true;
    }

    /*
     * What the storage a place names **holds** - the owner, or a view of one.
     *
     * Not `substituted(place.occurrence)`, and the difference is the whole of why this is a
     * function. A place's contract is the *parent's* qualification carried down, so a field or an
     * element declared at a type variable reaches `substitutedContract` looking exactly like a
     * borrowed position - and that rule turns a position into the view, which is right for a
     * parameter and wrong for storage. `data W(a) = W {v: a}` at `a := String` holds a `String`
     * owner in `v` however the record was reached; the position that reads it shared is the view,
     * and the two being told apart is what makes the derivation happen at all.
     *
     * A **local** is the other way round and is left alone: a parameter's occurrence *is* a
     * position - `key: k` at `k := String` was handed the view by its caller - and a binding that
     * aliases its lender holds whatever the lender produced.
     */
    mir::Occurrence storageAt(mir::Place& place) {
        auto held = substituted(place.occurrence);

        auto owns = [&] {
            if(place.kind == mir::Place::Index) return true;
            if(place.kind != mir::Place::Field) return false;

            // The record the field is *on* - `fieldRecordOf`, the same question `addressOf` asks -
            // and the field's own declared contract on it. A field that refers to somebody else's
            // storage is a position and keeps the answer above.
            auto parent = substituted(mir[place.parent]->occurrence);
            auto record = mir::fieldRecordOf(context, program, parent);
            if(!record || decl()[record]->kind != mir::Type::Record) return false;

            auto& fields = ((mir::RecordType*)decl()[record])->fields;
            if(place.payload >= fields.size()) return false;

            return fields.get(decl(), place.payload).occurrence.contract.isOwned();
        };

        if(!owns()) return held;

        held.contract = mir::Contract { held.contract.access(), held.contract.origin(),
                                        mir::AccessForm::WholeValue, held.contract.slot() };
        return held;
    }

    /*
     * An **owner** reaching a view position under substitution - the adaptation the generic body
     * could not write, made here because here is the one place the substitution is known.
     *
     * `fn (Hash(k)) probe(->key: k) -> U32 = hashKey(key)` at `k := String` is the program. The
     * parameter's storage holds the owner - three words - and `hashKey`'s shared position of `k` is
     * the view under `substitutedContract`, two words in `StringView`'s layout. The resolver derives
     * one from the other wherever the type is concrete (`Resolver::adaptToView`), and inside the
     * generic it cannot: `k` is open where the body is resolved, and a specialization has no body of
     * its own to adapt in. So the same derivation is emitted here, through the same member the
     * resolver calls - `mir::viewMember` off the `ViewRow` - and never a second one: a body that
     * already produced the view is a no-op by `needsViewDerivation`'s own test.
     *
     * The **erased** form asks for none and cannot answer one: a body compiled once holds an address
     * and a descriptor, `substitutedContract` needs a concrete substitution to turn a position into
     * a view at all, and a descriptor carries no per-access entry (Analysis-Manifestations.md §5.3).
     * So an open type never reaches this, and a call that did would be refused by `viewMember`
     * answering null below rather than lowered to a derivation of something with no layout.
     */
    Value* deriveView(Value& owner, const mir::Occurrence& position, LocationId at) {
        auto access = position.contract.access();
        auto target = mir::viewMember(context, program, position.type, access, at);

        /*
         * Reachability is what keeps it, and this walk may not mint one - `convertProgram` asserts
         * that the function list does not grow. `rootViewDerivations` in the MIR summary is the
         * rooting, deliberately blunter than this test, so a miss here is the two walks having come
         * apart rather than a program the compiler cannot express.
         */
        if(!target || !decl()[target]->reachable) {
            report(at, "internal: no derivation is reachable for a view of this owner"_v);
            return nullptr;
        }

        mir::CallSite site;
        site.target = target;
        site.through = mir::CallThrough::Declaration;

        Use argument = editor.use(owner, UseKind::Borrow);
        auto inst = editor.addCall(at, site, Use(), &argument, 1, position,
                                   conventionFor(position), true);

        return editor.resultOf(*inst);
    }

    // One value at one position, adapted where the substitution made them the owner and the view -
    // see `deriveView`. The reverse is not derivable and is reported by the caller that has a
    // position to name.
    Value* adaptToView(Value* value, const mir::Occurrence& position, LocationId at) {
        if(!value) return value;
        if(!refuseViewForOwner(value->occurrence, position, at)) return nullptr;
        if(!mir::needsViewDerivation(decl(), program, value->occurrence, position)) return value;

        return deriveView(*value, position, at);
    }

    /*
     * The direction that is not a derivation - a window where the owner is wanted.
     *
     * `Resolver::adaptToView` reports it wherever the type is concrete; here is where the
     * *substitution* is what makes the two forms differ, which is a generic body whose value is at a
     * variable and whose position the role marked `Owner`. Nothing reconstructs a container out of
     * two words, so this is the program's error and not an adaptation with no site.
     */
    bool refuseViewForOwner(const mir::Occurrence& held, const mir::Occurrence& position,
                            LocationId at) {
        if(position.contract.form() != mir::AccessForm::Owner) return true;
        if(held.contract.form() != mir::AccessForm::View) return true;
        if(!mir::sameType(held.type, position.type)) return true;

        errors++;
        context.diagnostics.error(
            "this is a window into %@ and this position takes the container itself, which a window "
            "cannot be turned back into"_v,
            at, mir::typeText(context, decl(), position.type));
        return false;
    }

    // The same, for an operand already turned into a use - the argument and hand-over positions.
    bool adaptUseToView(Use& into, const mir::Occurrence& position, LocationId at) {
        auto value = base()[into.value()];
        if(!refuseViewForOwner(value->occurrence, position, at)) return false;
        if(!mir::needsViewDerivation(decl(), program, value->occurrence, position)) return true;

        auto derived = deriveView(*value, position, at);
        if(!derived) return false;

        into = editor.use(*derived, into.kind());
        return true;
    }

    /*
     * One teardown, at the point the analysis put it.
     *
     * The address is computed in the block the walk is currently in, *before* the flag is read, so
     * that it dominates both arms of a conditional drop. That is the whole of why the order here is
     * address, flag, branch and not branch, address: an address computed inside the arm would
     * dominate nothing else, and a later projection of the same place would recompute it.
     */
    void emitDrop(const mir::DropStep& step) {
        // The step's own place, and not its path's: an overwrite of a global, a borrow or an element
        // releases what it replaces and has no path at all - see `mir::DropStep`.
        auto place = source.place(mir, step.place);
        auto at = substituted(place->occurrence);
        auto boxes = mir::boxesVisibleAt(mir, *place);

        /*
         * A teardown the *generic* body needed and this specialization does not.
         *
         * An unconstrained `a` is assumed to release something, which is what puts the step here at
         * all; substituting `I32` for it leaves a point at which nothing runs. Skipped rather than
         * refused, and that asymmetry is Implementation-Generics.md §1's invariant working: the
         * *placement* is the generic body's answer and is identical either way, and what a
         * specialization is allowed to change is only whether there is any code at it.
         */
        if(!mir::hasTeardownAt(decl(), at)) return;

        auto address = addressOf(*place, function.source);
        if(!address) return;

        /*
         * The subject of an authored `Drop` that finishes its value, dying whole - which the analysis
         * places only for a **sum** (`Analysis::subjectOwnsMembers`, K5).
         *
         * Its type's teardown is the function this body is, so what runs is the rest of it: a tag
         * test per constructor and the members' own drops, which is `emitResidueDrop`'s expansion
         * with nothing qualified away. That the step is on the subject is read off the place, the
         * same way the analysis found it: the first parameter of a teardown, not projected.
         */
        auto subject = place->kind == mir::Place::Local &&
                       source.local(mir, LocalId(place->payload))->parameter == 0 &&
                       mir::ownsTeardownSubject(decl(), function);

        /*
         * A member of a sum nothing proved the constructor of - `mir::DropStep::guarded`. The sums'
         * addresses here, beside the member's, for the reason above; their tags are read behind the
         * flag, since a sum that is not there on some path has no tag worth reading on it.
         */
        SmallArray<SumGuard, 2> guards;
        if(step.guarded && !collectGuards(*place, guards)) return;

        if(subject && !step.conditional && !guards.size()) {
            emitResidueDrop(*address, at, boxes);
            return;
        }

        if(!step.conditional) {
            emitGuarded(guards, 0, [&] { emitDropAt(*address, at, boxes); });
            return;
        }

        /*
         * A path that was owned on some incoming edges and not on others - §5.10's `Maybe`.
         *
         * The flag is a `Bool` in the frame, written at every transition the analysis found, and the
         * drop is the arm of a branch over it. Two blocks and a merge, exactly as an `if` in the
         * source would be: a teardown behind a condition is control flow, and giving it a shape of
         * its own here would be a second thing for every later pass to understand.
         */
        if(step.path.index >= flags.size() || !flags[step.path.index]) {
            report(function.source, "internal: a conditional drop has no flag"_v);
            return;
        }

        auto flag = base()[flags[step.path.index]];
        auto held = mir::owned(mir::booleanType(program));
        auto read = editor.addLoad(function.source, editor.use(*flag, UseKind::Inspect), held,
                                   Convention::Trivial);

        /*
         * The arm is a **cleanup region** - `ssa::Continuation`.
         *
         * These two blocks are not the source's; they are a teardown the elaboration decided to run
         * behind a condition, and the difference matters to anything that walks the graph looking
         * for what the program said. The `after` block is in the enclosing region because it is
         * where the arm rejoins, which is exactly the nesting rule: a cleanup is entered from its
         * parent and leaves to it.
         *
         * Both halves of that are *stated* rather than left to be read off the graph - `after` is
         * the region's `destination`, and the arm runs `AtMostOnce` because it sits behind the flag
         * test. A pass that rerouted the arm's exit elsewhere, or turned it into a loop over its own
         * entry, is what the verifier catches with them.
         */
        auto arm = editor.enterRegion(ssa::Continuation::Cleanup, ssa::Continuation::AtMostOnce);
        auto dropBlock = editor.addBlock(StringId());
        editor.leaveRegion();

        auto after = editor.addBlock(StringId());
        editor.rejoinRegion(arm, *after);

        editor.addBranch(function.source, editor.use(*editor.resultOf(*read), UseKind::Inspect),
                         *dropBlock, *after, nullptr, 0);

        // The arm's region is made current while it is filled, because a residue's teardown
        // (`emitResidueDrop`) opens cleanup arms of its own, and they nest in whatever is current.
        editor.setBlock(*dropBlock);
        editor.setRegion(dropBlock->region);
        emitGuarded(guards, 0, [&] {
            if(subject) emitResidueDrop(*address, at, boxes);
            else emitDropAt(*address, at, boxes);
        });
        editor.addJump(function.source, *after, nullptr, 0);

        editor.setBlock(*after);
        editor.setRegion(after->region);
    }

    // One sum a guarded step tests: its storage, its occurrence, and the tag of the constructor
    // the member belongs to.
    struct SumGuard {
        Value* address;
        mir::Occurrence at;
        I64 value;
    };

    // Every sum on the member's way up, outermost first - the order the tests nest in.
    bool collectGuards(mir::Place& member, SmallArray<SumGuard, 2>& guards) {
        for(auto place = &member; place->parent; place = mir[place->parent]) {
            auto parent = mir[place->parent];
            if(place->kind != mir::Place::Field) continue;
            if(!mir::isSumRecord(decl(), parent->occurrence.type)) continue;

            auto at = substituted(parent->occurrence);
            auto& record = *(mir::RecordType*)decl()[at.type];
            auto index = record.constructorOf(decl(), U16(place->payload));

            auto address = addressOf(*parent, function.source);
            if(!address) return false;

            for(auto constructor: record.cons.contents(decl())) {
                if(constructor.index != index) continue;
                guards.push(SumGuard { address, at, constructor.value });
            }
        }

        for(Size i = 0, j = guards.size(); i + 1 < j; i++, j--) {
            auto held = guards[i];
            guards[i] = guards[j - 1];
            guards[j - 1] = held;
        }

        return true;
    }

    /*
     * `body` behind a tag test per guard, each a cleanup arm - the shape `emitResidueDrop` gives a
     * constructor's members, and the test a sum's derived teardown makes before reaching them.
     */
    template<class F>
    void emitGuarded(SmallArray<SumGuard, 2>& guards, Size i, F&& body) {
        if(i == guards.size()) {
            body();
            return;
        }

        auto& guard = guards[i];
        auto source = function.source;
        auto tagType = mir::tagTypeOf(decl(), program.types, guard.at.type);
        if(!tagType) {
            report(source, "internal: a guarded drop's sum has no discriminant"_v);
            return;
        }

        auto held = mir::owned(tagType);
        auto slot = editor.addFieldAddress(source, addressUse(*guard.address), guard.at.type,
                                           InstFieldAddress::kDiscriminant, guard.at.shape);
        auto tag = editor.addLoad(source, addressUse(*editor.resultOf(*slot)), held,
                                  conventionFor(held));
        auto value = editor.addConst(source, mir::intConstant(tagType, guard.value));

        auto truth = mir::owned(mir::booleanType(program));
        auto is = editor.addBinary(source, BinaryOp::Eq,
                                   editor.use(*editor.resultOf(*tag), UseKind::Inspect),
                                   editor.use(*editor.resultOf(*value), UseKind::Inspect),
                                   truth, conventionFor(truth));

        auto arm = editor.enterRegion(ssa::Continuation::Cleanup, ssa::Continuation::AtMostOnce);
        auto dropBlock = editor.addBlock(StringId());
        editor.leaveRegion();

        auto after = editor.addBlock(StringId());
        editor.rejoinRegion(arm, *after);

        editor.addBranch(source, editor.use(*editor.resultOf(*is), UseKind::Inspect),
                         *dropBlock, *after, nullptr, 0);

        editor.setBlock(*dropBlock);
        editor.setRegion(dropBlock->region);
        emitGuarded(guards, i + 1, body);
        editor.addJump(source, *after, nullptr, 0);

        editor.setBlock(*after);
        editor.setRegion(after->region);
    }

    // The teardown of one value in storage - the type's own, or the residue's where a qualified
    // type argument took a member away.
    void emitDropAt(Value& address, const mir::Occurrence& at, bool boxes) {
        if(mir::qualifiesInterior(decl(), at)) {
            emitResidueDrop(address, at, boxes);
            return;
        }

        editor.addDrop(function.source, addressUse(address), at, boxes);
    }

    /*
     * The teardown of a value whose qualified type argument took a member away and left one that
     * owes something - `Result(String, '[U8])`, P15 (J187).
     *
     * **Expanded here, into drops of the surviving members at their own types**, rather than a
     * teardown per shape in every tier that builds one: the derived teardown of `Result(String,
     * [U8])` would free the view, and teaching the native walk, the descriptor, the JavaScript release
     * and the optimizer's expansion that one logical type has two teardowns is a change to each of
     * them for a position this tier can already reach with the instructions it has. What goes down
     * is a tag test and ordinary drops, which every later pass understands already. `own.cpp`'s
     * `residualObligation` is where the shapes this cannot serve are refused.
     *
     * A single-constructor record is its members in order. A **sum** tests the tag once per
     * constructor that owes anything, each drop behind its own test as a cleanup arm - the shape a
     * conditional drop above already has, so nothing downstream meets a new one. Most carriers have
     * exactly one such constructor (the error), so this is one test and one branch.
     */
    void emitResidueDrop(Value& address, const mir::Occurrence& at, bool boxes) {
        auto& record = *(mir::RecordType*)decl()[at.type];
        auto source = function.source;

        auto owes = [&](const mir::RecordField& field) {
            if(mir::fieldPosition(decl(), at.shape, field.index)) return false;
            if(!mir::ownsField(field)) return false;
            return mir::hasTeardown(decl(), field.occurrence.type);
        };

        auto dropMembers = [&](U16 first, U16 count) {
            for(U16 i = first; i < first + count; i++) {
                auto field = record.field(decl(), i);
                if(!owes(field)) continue;

                auto member = editor.addFieldAddress(source, addressUse(address), at.type,
                                                     field.index, at.shape);
                editor.addDrop(source, addressUse(*editor.resultOf(*member)),
                               mir::owned(field.occurrence.type), boxes);
            }
        };

        if(record.layout == mir::RecordLayout::Single) {
            dropMembers(0, U16(record.fields.size()));
            return;
        }

        auto tagType = mir::tagTypeOf(decl(), program.types, at.type);
        if(!tagType) {
            report(source, "internal: a qualified sum's teardown found no discriminant"_v);
            return;
        }

        for(auto constructor: record.cons.contents(decl())) {
            auto owing = false;
            for(U16 i = 0; i < constructor.count; i++) {
                if(owes(record.field(decl(), U16(constructor.first + i)))) owing = true;
            }

            if(!owing) continue;

            auto held = mir::owned(tagType);
            auto slot = editor.addFieldAddress(source, addressUse(address), at.type,
                                               InstFieldAddress::kDiscriminant, at.shape);
            auto tag = editor.addLoad(source, addressUse(*editor.resultOf(*slot)), held,
                                      conventionFor(held));
            auto value = editor.addConst(source, mir::intConstant(tagType, constructor.value));

            auto truth = mir::owned(mir::booleanType(program));
            auto is = editor.addBinary(source, BinaryOp::Eq,
                                       editor.use(*editor.resultOf(*tag), UseKind::Inspect),
                                       editor.use(*editor.resultOf(*value), UseKind::Inspect),
                                       truth, conventionFor(truth));

            auto arm = editor.enterRegion(ssa::Continuation::Cleanup, ssa::Continuation::AtMostOnce);
            auto dropBlock = editor.addBlock(StringId());
            editor.leaveRegion();

            auto after = editor.addBlock(StringId());
            editor.rejoinRegion(arm, *after);

            editor.addBranch(source, editor.use(*editor.resultOf(*is), UseKind::Inspect),
                             *dropBlock, *after, nullptr, 0);

            editor.setBlock(*dropBlock);
            dropMembers(constructor.first, constructor.count);
            editor.addJump(source, *after, nullptr, 0);

            editor.setBlock(*after);
        }
    }

    // Every step the analysis scheduled before this node. Both lists are sorted, so each is a cursor
    // rather than a search - see the two of them on `Conversion`.
    void emitSteps(U32 block, U32 position) {
        if(!elaboration) return;

        /*
         * The steps of blocks this scope does not emit, passed over.
         *
         * One conversion per scope means one cursor per scope, and the blocks a scope walks are a
         * subsequence of the sorted list rather than all of it. Skipping here rather than at the
         * match keeps the walk a cursor: a step of somebody else's block is never *between* two of
         * this scope's positions, because both lists are ordered by block.
         */
        while(dropCursor < elaboration->drops.size() &&
              !inScope(BlockId(elaboration->drops.get(mir, dropCursor).block))) {
            dropCursor++;
        }

        while(dropCursor < elaboration->drops.size()) {
            auto step = elaboration->drops.get(mir, dropCursor);
            if(step.block != block || step.position != position) break;

            dropCursor++;
            emitDrop(step);
        }

        while(flagCursor < elaboration->flags.size()) {
            auto step = elaboration->flags.get(mir, flagCursor);
            if(step.position == mir::FlagStep::kEntry) { flagCursor++; continue; }
            if(!inScope(BlockId(step.block))) { flagCursor++; continue; }
            if(step.block != block || step.position != position) break;

            flagCursor++;
            emitFlag(step);
        }
    }

    // The steps at `(block, position)` passed over without emitting any - see the block loop.
    void skipSteps(U32 block, U32 position) {
        if(!elaboration) return;

        while(dropCursor < elaboration->drops.size()) {
            auto step = elaboration->drops.get(mir, dropCursor);
            if(step.block != block || step.position != position) break;
            dropCursor++;
        }
    }

    void emitFlag(const mir::FlagStep& step) {
        if(step.path.index >= flags.size() || !flags[step.path.index]) return;

        auto flag = base()[flags[step.path.index]];
        auto constant = mir::truthConstant(program, step.value);
        auto value = editor.resultOf(*editor.addConst(function.source, constant));

        editor.addStore(function.source, editor.use(*flag, UseKind::Inspect),
                        editor.use(*value, UseKind::Consume));
    }

    /*
     * The SSA storage one MIR loan is rooted in, or null where this body cannot name it.
     *
     * The loan's *root*, not its place: a loan on `p.left` is a borrow of storage inside `p`, and
     * what releases it is the drop of `p`. Walking to the root is also what keeps this from emitting
     * instructions - a local's slot is already there, and asking `addressOf` for the exact place
     * would build a second `field.addr` for a question that is not about the field.
     *
     * A root this body does not allocate - a global, a raw address - answers null and is left out of
     * the set. That is the permissive direction and it is the right one: nothing here releases a
     * global, so a loan on one does not end, and a set that named it would be a lender the endpoint
     * rule watches for an event that cannot happen.
     */
    Value* storageOfLoan(LoanId id, bool& secondName) {
        secondName = false;

        auto place = source.place(mir, source.loan(mir, id)->place);
        while(place->parent) place = mir[place->parent];

        if(place->kind != mir::Place::Local) return nullptr;
        if(place->payload >= slots.size() || !slots[place->payload]) return nullptr;

        auto local = source.local(mir, LocalId(place->payload));
        secondName = !local->isParameter() && aliasesLender(substituted(local->occurrence));

        return base()[slots[place->payload]];
    }

    /*
     * What a borrowed value depends on - `ssa::Loan`, from the origin environment.
     *
     * The one honest source for it. `Elaboration::rootedIn` is §5.11's `OriginEnvironment(value)`,
     * computed over the places by the pass that decides borrow legality in the first place, so the
     * dependency this tier records is the same answer the diagnostics were made from rather than a
     * second one reconstructed from the operands. A call is why that matters: its result is rooted
     * in whichever argument its signature named, and `middle(a, b, c) -> q'Guard` is rooted in `b`
     * with nothing in the instruction's operand order to say so.
     *
     * A read also contributes its **own** loan, which the environment does not list separately - a
     * read is where a loan is minted, so the value it produces is rooted in it by construction.
     */
    void attachLoan(mir::Node& node, Value& value) {
        if(value.convention != Convention::Guaranteed) return;

        SmallArray<SsaPtr<Value>, 4> storage;

        auto add = [&](SsaPtr<Value> member) {
            for(auto& held: storage) if(held == member) return;
            storage.push(member);
        };

        auto include = [&](LoanId id) {
            auto secondName = false;
            auto found = storageOfLoan(id, secondName);
            if(!found) return;

            /*
             * A loan on a local that holds a borrow is a loan on **its lender's** storage.
             *
             * Such a local has no storage of its own - its slot is the borrowed value the `let`
             * established (see the slot loop in `run`) - so naming that value here would name a
             * definition rather than storage. It is also wrong: the value is defined wherever the
             * binding was, which need not dominate the borrow. `let text = match m: Just(c) -> c`
             * binds `c` in one arm, and everything derived from `text` after the join is rooted in
             * the loan on `c` too. Whether `c`'s value was in the set then depended on whether its
             * arm happened to be converted before the use - blocks go in index order, not dominance
             * order - and a use past a second join named a value only the arm defines.
             *
             * What the value is borrowed from is already a loan set, so that set is what goes in.
             * Nothing is lost: the origin environment lists the lender's loans beside this one, so
             * a binding not converted yet contributes the same members anyway. A parameter is the
             * exception and keeps its slot - it is defined on entry, and it is the member the
             * inliner maps onto the caller's own lenders.
             *
             * **Only where that set names something.** A binding whose value has no lender this
             * body can name - a window built over a raw address, or one the origin environment did
             * not trace - keeps naming the value, as it did before. Dropping it would leave an
             * empty set, and an empty set is not "unknown": it is the caller's loan, which
             * `canSubstitute` in `opt/cfg.cpp` reads as one that never ends inside this body, and
             * which the inliner's `rootLender` would stop walking at.
             */
            if(secondName && found->loan) {
                auto& lent = *base()[found->loan];

                if(lent.storage.size()) {
                    for(auto member: lent.storage.contents(base())) add(member);
                    return;
                }
            }

            add(found - base());
        };

        if(node.kind == mir::Node::Read && ((mir::NodeRead&)node).loan.valid()) {
            include(((mir::NodeRead&)node).loan);
        }

        if(elaboration) {
            for(U32 l = 0; l < elaboration->loanCount; l++) {
                if(elaboration->rootedIn(mir, node.id, LoanId(l))) include(LoanId(l));
            }
        }

        // Nothing this body can name: the caller's loan, which `addResult` already gave it.
        if(!storage.size()) return;

        editor.setDependency(value, *editor.loanOn(node.source, storage.pointer(), storage.size(),
                                                  value.occurrence.contract.access()));
    }

    /*
     * One outgoing edge, converted - and where a continuation *ends*.
     *
     * An edge that stays inside the scope is an ordinary jump. One that leaves it is the region
     * falling out, which in the CPS form is this function answering: what the caller does with the
     * discriminant is `convertLensCall`'s, and there is nothing to jump to here because the block
     * on the far side belongs to a different function.
     */
    void convertEdge(mir::Node& node, mir::Edge& written, U64 exit) {
        auto to = mir[written.target];

        /*
         * A **step**, where a region is embedded rather than outlined - §3.2's first step, and the
         * whole of what the two exits mean once the child is a field.
         *
         * Falling out of the region is the continuation ending, which here steps the member: the
         * edge goes to the block the call made for that rather than to where the region rejoins.
         * For a `Many` region that is the next element; for a lens's `Once` region it is the code
         * after the lens's `yield`, which is its cleanup, and the member then answers `False` and
         * the same block carries on at the destination. **A departure takes this edge too** - a
         * `yield` inside a lens region whose consumer has left jumps to the region's destination
         * (`emitDeparture`), which is a plain `Jump` - so the lens's cleanup runs on the way past
         * exactly as it does in the continuation-passing form.
         *
         * `break` - which is what a departure out of the outer object emits where the region holds a
         * loop, since `emitDeparture` leaves the loops the `yield` was written in - ends the child,
         * so the child is cancelled and control resumes at the region's destination exactly as the
         * caller of an outlined continuation resumes there. The **landing** of a `break` or
         * `continue` written in a lens call's block (`Resolver::resolveExit`) would be the same
         * cancellation followed by a jump to the landing, and is refused below.
         */
        if(stepping && to->region != emitting) {
            auto region = source.region(mir, emitting);
            auto landing = region && to->index != region->destination &&
                           to->region == region->parent &&
                           region->multiplicity != mir::Continuation::Many;

            if(!region || (to->index != region->destination && !landing) ||
               written.arguments.size()) {
                report(node.source,
                       "internal: an edge leaves an embedded region for somewhere other than where it rejoins"_v);
                return;
            }

            /*
             * Refused by name, and only in this form: the landing reads the loop's carried values,
             * and on the path that resumes inside the lens region those were defined by an earlier
             * step - which the spill plan (`planSpillReach`, `LiveAcross`) does not yet see, so the
             * step read a value of another activation. The continuation-passing form of the same
             * producer is built.
             */
            if(landing) {
                report(node.source,
                       "the compiler does not implement `break` or `continue` out of a lens call in an `iter fn` built as an object yet"_v);
                return;
            }

            if(exit == kExitFallOut) {
                auto advance = stepping->nestAdvance[emitting.index];

                if(!advance) {
                    report(node.source, "internal: an embedded region falls out before it was entered"_v);
                    return;
                }

                editor.addJump(node.source, *base()[advance], nullptr, 0);
                return;
            }

            stepCancelNest(node.source, emitting);
            editor.addJump(node.source, *base()[blocks[to->index.index]], nullptr, 0);
            return;
        }

        if(to->region != scope && !stepping) {
            /*
             * The one edge that may leave, held to what the region declared.
             *
             * Falling out is the exit that goes to `destination`, and `break` out of a `for` body
             * is the row beside it. Anywhere else is the landing of a `break` or `continue` for a
             * loop outside a lens call - the MIR verifier's `checkEdgeRegion` holds it to a block
             * of the region the call was made in - and is the outward row naming it.
             */
            auto declared = outlining ? source.region(mir, scope)->destination : BlockId();

            if(!outlining || written.arguments.size()) {
                report(node.source,
                       "internal: an edge leaves a continuation for somewhere other than where it rejoins"_v);
                return;
            }

            emitExit(node.source, to->index == declared ? exit : outwardRow(to->index));
            return;
        }

        // The MIR verifier's rule - `checkEdgeRegion` - restated where acting on it would be wrong,
        // because emitting an exit for an edge that does not leave is a miscompile rather than a
        // malformed body.
        if(exit != kExitFallOut) {
            report(node.source, "internal: a `break` inside the region it was written in"_v);
            return;
        }

        auto target = base()[blocks[to->index.index]];

        SmallArray<Use, 8> arguments;
        for(auto operand: written.arguments.contents(mir)) {
            Use used;
            if(!useOf(operand, node.source, used)) return;

            // The block argument is a position like any other, and under substitution it can be the
            // view where the edge supplies the owner - `deriveView`.
            if(!adaptEdgeArgument(*target, arguments.size(), used, node.source)) return;
            arguments.push(used);
        }

        editor.addJump(node.source, *target, arguments.pointer(), arguments.size());
    }

    // One edge operand at the argument it fills. The block's arguments were declared with the
    // substituted occurrences, so the position is read off the value that is already there.
    bool adaptEdgeArgument(Block& target, Size index, Use& used, LocationId at) {
        if(index >= target.argumentCount()) return true;

        auto position = base()[target.argumentAt(base(), index)]->occurrence;
        return adaptUseToView(used, position, at);
    }

    /*
     * Every allocation this scope made off the frame, handed back - the release half of §10.4.
     *
     * Before each terminator that ends this activation rather than at the point the *value* died,
     * and the two are different questions: a heap-placed local whose value left by a `->` argument
     * owes no drop and still owes this. One allocation per activation and one release per path out
     * of it is the pairing, and it is what `test/resolve/DropFlagHeap.yana` measures against the
     * allocator's own bump pointer.
     *
     * Nothing is emitted on a path that does not leave - an `Unreachable`, an abort - which is
     * right for the same reason a teardown is not run there: the activation does not end, the
     * program does.
     */
    void releaseSlots(LocationId at) {
        for(Size i = heapSlots.size(); i > 0; i--) {
            auto slot = base()[heapSlots[i - 1]];
            editor.addFree(at, editor.use(*slot, UseKind::Inspect));
        }
    }

    /*
     * What this scope's terminators answer with - the exit signal, or the wrapper a resume channel
     * turns it into.
     *
     * Read off the lifted declaration the planner already filled, so that the two cannot disagree:
     * what a continuation returns is stated once, where the function is made.
     */
    mir::Occurrence answered() {
        if(!outlining || !outlining->lifted) return mir::owned(program.types.exitCode());
        return decl()[outlining->lifted]->result;
    }

    // A continuation answering with which of its exits it took - the whole of the exit signal, and
    // the one thing a `Handover` region's terminators become that an ordinary block's do not.
    void emitExit(LocationId at, U64 code) {
        releaseSlots(at);

        auto constant = mir::intConstant(program.types.exitCode(), code);
        auto value = editor.resultOf(*editor.addConst(at, constant));

        /*
         * `Exit(code)` where the continuation answers a wrapper - Design-Iterator-Objects.md §2.5.
         *
         * Every exit that is not `continue v` comes through here, so `break`, `return` and a pass
         * that was already over are one row of the wrapper and need no case: which of them it was is
         * still the number inside, and still only the callee's business.
         */
        auto answer = answered();

        if(decl()[answer.type]->kind == mir::Type::Record) {
            auto component = editor.use(*value, UseKind::Consume);
            value = editor.resultOf(*editor.addAggregate(at, mir::kOutcomeExit, &component, 1,
                                                         answer, conventionFor(answer)));
        }

        auto used = editor.use(*value, UseKind::Consume);
        editor.addReturn(at, &used, 1);
    }

    /*
     * `continue v` - the pass's own answer, which is the other row of the same wrapper.
     *
     * The one exit that carries something, and the reason the wrapper exists: a bare `I32` has
     * nowhere to put it. Nothing else about the exit changes - the caller still reads the *iterator's*
     * result, which is the signal it always was, and this travels only from the continuation back
     * into the `yield` that called it.
     */
    void emitResume(LocationId at, Use value) {
        auto answer = answered();

        if(decl()[answer.type]->kind != mir::Type::Record) {
            report(at, "internal: a `continue` carries a value out of a continuation that answers no wrapper"_v);
            return;
        }

        auto wrapped = editor.resultOf(*editor.addAggregate(at, mir::kOutcomeProceed, &value, 1,
                                                            answer, conventionFor(answer)));

        releaseSlots(at);

        auto used = editor.use(*wrapped, UseKind::Consume);
        editor.addReturn(at, &used, 1);
    }

    /*
     * The `Resume` terminator - `convertEdge` with a value, and the same two checks about where the
     * edge goes.
     *
     * It can only leave: `Node::Resume` is built by `resolveExit` for a loop that *is* a
     * continuation region, so an edge of one that stayed inside its own region is the two having
     * come apart rather than a program being unusual - the same reading `convertEdge` gives a
     * `Break` in its own region.
     *
     * **No `stepping` arm, and that is a fact about what can reach here rather than an omission.**
     * `convertEdge` has one because an embedded region's ordinary exits go to the block that steps
     * the member; a `Resume` cannot be in one. A nested region whose passes hand a value back is
     * refused by name where the nest is collected - "an object's step does not carry one into a
     * nested pass yet" - and a `<- {}` region builds no `Resume` at all, because a unit channel
     * leaves the continuation answering the bare signal and the exemption takes the fall-out edge
     * (`mir::resumesWithValue`). Before those two agreed, a `<- {}` producer nested inside an object
     * reached this report: both spellings of the exemption, falling off the pass and a bare
     * `continue`, were "a `continue` with a value leaves a continuation for somewhere other than
     * where it rejoins". So the check below is the consistency check it reads as, and an arm added
     * here would be building a row the tier above has refused.
     */
    void convertResume(mir::NodeResume& node) {
        auto to = mir[node.to.target];
        auto declared = outlining ? source.region(mir, scope)->destination : BlockId();

        if(to->region == scope || to->index != declared || node.to.arguments.size()) {
            report(node.source,
                   "internal: a `continue` with a value leaves a continuation for somewhere other than where it rejoins"_v);
            return;
        }

        Use used;
        if(!useOf(node.value, node.source, used)) return;

        emitResume(node.source, used);
    }

    /*
     * `return` - the enclosing function's, wherever it is written.
     *
     * In the outermost scope it is one. Inside a continuation the frame that has to return is not
     * this one, so the value is parked in the slot the whole nest shares and the discriminant
     * carries the fact that it left; the caller reads both. That the two are the same statement is
     * what `doc/spec/lenses.md`'s "observationally identical" requires of the pair.
     */
    void emitLeave(LocationId at, const Use* value) {
        /*
         * A grafted body leaves by joining the caller, and what it returns is the join's argument.
         *
         * Restated where the body's value is not the declared result to the letter - a `values`
         * body returns the pointer it read out of a field, an owned `%a`, at a result declared
         * `%source'a`. A call boundary tolerates that silently, because the callee's `return` is not
         * held to the declaration and the caller reads the declaration; a jump is held to its
         * target's argument, so the same restatement is made explicit. `represent` is the
         * instruction that moves no bytes, and the lowering passes the value through.
         */
        if(grafting) {
            if(value && grafting->result) {
                auto& declared = grafting->result->occurrence;
                auto supplied = base()[value->value()];

                // A tracked value is not restated: an owner's declaration and its `return` agree
                // to the letter, and a pure instruction may not consume - the verifier says which.
                if(supplied->occurrence != declared && !isTracked(supplied->convention)) {
                    auto restated = editor.addUnary(at, mir::UnaryOp::Represent,
                                                    editor.use(*supplied, UseKind::Inspect),
                                                    declared, conventionFor(declared));
                    auto use = editor.use(*editor.resultOf(*restated), value->kind());
                    releaseSlots(at);
                    editor.addJump(at, *grafting->join, &use, 1);
                    return;
                }
            }

            releaseSlots(at);
            editor.addJump(at, *grafting->join, value, value ? 1 : 0);
            return;
        }

        if(!outlining) {
            releaseSlots(at);
            editor.addReturn(at, value, value ? 1 : 0);
            return;
        }

        if(value) {
            if(!resultCarrier) {
                report(at, "internal: a continuation returns a value with no carrier"_v);
                return;
            }

            editor.addStore(at, editor.use(*base()[resultCarrier], UseKind::Inspect), *value);
        }

        emitExit(at, kExitLeave);
    }

    /*
     * The failure half of a `lens.try` - Analysis-Lens.md §7.1's skip, read off the wrapper
     * `declareOutcome` built.
     *
     * Its own function because it is its own question, and the outer one: everything in
     * `convertLensCall` below the call asks which *exit* the continuation took, and this asks
     * whether there was a continuation at all. Answers the exit signal, which is what that question
     * is then asked of - so the transparent case is this function not having run rather than a
     * second path through the one below.
     *
     * Null having reported, on the same terms as every other internal check here.
     */
    Value* convertSkip(mir::NodeLensCall& node, mir::Continuation& region, Value& wrapped) {
        if(!region.otherwise.valid()) {
            report(node.source, "internal: a lens.try has no block to skip to"_v);
            return nullptr;
        }

        auto slot = outcomes[node.id.index];
        if(!slot) {
            report(node.source, "internal: a lens.try has no storage for what it answered"_v);
            return nullptr;
        }

        /*
         * Into storage first, which is what makes the rest of this ordinary.
         *
         * The wrapper arrives as a value and one of its two fields is **owned** where the failure
         * type is - so it owes a consumption on every path out of here, and moving a field out of a
         * value is not one. `Store` is: it takes the aggregate, and what is left is storage that
         * each arm reads its own field out of, with the shell owing nothing because the field the
         * other arm would have read was never written.
         *
         * Unconditionally, and not only where the failure type owns something. The slot is dead
         * weight for a wrapper of two trivial fields - and promoting one stored once and loaded
         * twice is what the promotion pass is for, where a second path here would be a second
         * statement of when a value owes a consumption, kept in step with the verifier by hand.
         */
        auto storage = base()[slot];
        editor.addStore(node.source, addressUse(*storage), editor.use(wrapped, UseKind::Consume));

        auto outcome = skipOutcome(node, node.source);
        if(!outcome.type) return nullptr;

        auto& record = *(mir::RecordType*)decl()[outcome.type];

        // One projection, three times: the tag and one field of each arm. A lambda rather than three
        // spellings of the same two instructions, because what differs between them is the index and
        // the type and nothing else.
        auto read = [&](U16 field, mir::Occurrence held) {
            if(auto unit = unitValue(node.source, held)) return unit;

            auto address = editor.addFieldAddress(node.source, addressUse(*storage),
                                                  outcome.type, field, outcome.shape);
            return editor.resultOf(
                *editor.addLoad(node.source, addressUse(*editor.resultOf(*address)), held,
                                conventionFor(held)));
        };

        /*
         * The tag, at the width the record actually stores it in - `tagTypeOf`.
         *
         * Not an `I32`: a two-constructor record's discriminant is a byte, and reading four of them
         * takes the payload's first bytes along with it. The width is the manifestation's answer, so
         * it is asked for rather than assumed, and the constant beside it is built at the same width
         * so that the comparison is one type.
         */
        auto tagType = mir::tagTypeOf(decl(), program.types, outcome.type);
        if(!tagType) {
            report(node.source, "internal: a lens.try's result carries no discriminant"_v);
            return nullptr;
        }

        auto tag = read(InstFieldAddress::kDiscriminant, mir::owned(tagType));
        auto exit = editor.resultOf(
            *editor.addConst(node.source, mir::intConstant(tagType, mir::kOutcomeExit)));

        auto test = mir::owned(mir::booleanType(program));
        auto failed = editor.resultOf(*editor.addBinary(node.source, BinaryOp::Eq,
                                                        editor.use(*tag, UseKind::Inspect),
                                                        editor.use(*exit, UseKind::Inspect),
                                                        test, conventionFor(test)));

        auto proceeding = editor.addBlock(StringId());
        auto skipping = editor.addBlock(StringId());

        editor.addBranch(node.source, editor.use(*failed, UseKind::Inspect), *skipping, *proceeding,
                         nullptr, 0);

        /*
         * The reason, handed to the block that binds it - the one argument of the `otherwise` edge,
         * which is where the resolver put the alternative's parameter.
         *
         * The field index is the arm's own and not a number this file picked: `RecordCon::first` is
         * where the constructor's fields begin, and each of these two constructors has one.
         */
        editor.setBlock(*skipping);

        auto failure = record.fields.get(decl(), record.cons.get(decl(), mir::kOutcomeExit).first);

        // A `| {}` failure is that value and never a load of it - `unitValue`, which `read` asks.
        auto reason = read(failure.index, failure.occurrence);
        auto handed = editor.use(*reason, UseKind::Consume);
        editor.addJump(node.source, *base()[blocks[region.otherwise.index]], &handed, 1);

        // And the signal, on the arm that has one.
        editor.setBlock(*proceeding);

        auto kept = record.cons.get(decl(), mir::kOutcomeProceed);
        auto proceeded = record.fields.get(decl(), kept.first);
        return read(proceeded.index, mir::owned(program.types.exitCode()));
    }

    /*
     * A lens call, in the CPS form - Analysis-IR.md §11.4's W-C.
     *
     * Four instructions and a test. The environment is built from the addresses the region reads,
     * the pair `{code, env}` is made of it, the callee is called with it as its last argument, and
     * what comes back says which exit the continuation took. A region with one exit needs no test at
     * all, which is every `withLock`-shaped call.
     */
    void convertLensCall(mir::NodeLensCall& node) {
        auto plan = outlineFor(node.region);
        if(!plan) {
            report(node.source, "internal: a lens call names a region with no outline"_v);
            return;
        }

        auto& record = *(mir::RecordType*)decl()[plan->environment];

        // The captures, as the addresses this scope holds for them. A nested region's environment is
        // built here out of what this scope captured, which is why the planner made them agree.
        SmallArray<Use, 8> components;

        for(U32 i = 0; i < plan->captures.size(); i++) {
            auto held = plan->captures[i];
            Value* address = nullptr;

            if(plan->flagOf[i] != Outline::kNoFlag) {
                auto path = plan->flagOf[i];
                if(path >= flags.size() || !flags[path]) {
                    report(node.source, "internal: a continuation captures a drop flag with no storage"_v);
                    return;
                }

                address = base()[flags[path]];
            } else if(held.valid()) {
                if(held.index >= slots.size() || !slots[held.index]) {
                    report(node.source, "internal: a continuation captures a local with no storage"_v);
                    return;
                }

                address = base()[slots[held.index]];
            } else {
                if(!resultCarrier) {
                    report(node.source, "internal: a continuation captures a carrier with none"_v);
                    return;
                }

                address = base()[resultCarrier];
            }

            /*
             * The address, read as the *field's own* borrow - which is what `buildEnvironment` does
             * one tier up and is the only shape the aggregate accepts.
             *
             * A slot is an `Address` value typed `%T`, and a borrowed field is typed `T` and
             * manifests as a pointer. The two are the same bytes and different statements, and the
             * load is where one becomes the other.
             */
            auto field = record.fields.get(decl(), i);
            auto borrowedField = editor.addLoad(node.source, addressUse(*address), field.occurrence,
                                                Convention::Guaranteed);

            UseKind component;
            if(!useKindFor(mir::argumentMode(field.occurrence.contract, mir::SourceKind::Result),
                           component)) {
                report(node.source, "internal: an environment field has no handover mode"_v);
                return;
            }

            components.push(editor.use(*editor.resultOf(*borrowedField), component));
        }

        auto held = mir::Occurrence { plan->environment, mir::ownedContract(), nullptr };
        auto environmentValue = editor.resultOf(
            *editor.addAggregate(node.source, 0, components.pointer(), components.size(), held,
                                 conventionFor(held)));

        /*
         * The pair, which is an ordinary closure - `lower/fun.cpp`, and the one place this differs
         * from a lambda is that nothing in the source wrote it.
         */
        auto& lifted = *decl()[plan->lifted];
        auto& handedArg = *decl()[lifted.args.get(decl(), 1)];

        mir::FunParam parameter { handedArg.occurrence };
        auto funType = program.types.funType(decl(), &parameter, 1, lifted.result);
        auto funValue = mir::Occurrence { funType, mir::ownedContract(),
                                          program.shapes.closure() };

        // `addressUse` and not an inspection: an environment holding a `&` capture is not trivially
        // copyable (`classifyRecord`), so the value is one the linear dataflow follows and naming it
        // without an obligation is what §7.1's invariant forbids. What the pair does with it is
        // borrow it, which is what a continuation capturing its caller's frame by reference *is*.
        auto environmentUse = addressUse(*environmentValue);
        auto continuation = editor.resultOf(
            *editor.addFunValue(node.source, plan->lifted, &environmentUse, funValue,
                                conventionFor(funValue)));

        // The written arguments, then the continuation. `callTarget` resolves the callee by the same
        // rules a `call` is resolved by - see `CallRequest`.
        SmallArray<Use, 8> arguments;
        for(auto operand: node.arguments.contents(mir)) {
            Use used;
            if(!useOf(operand, node.source, used)) return;
            arguments.push(used);
        }

        auto& callee = *decl()[node.callee];
        auto& continuationArg = *decl()[callee.args.get(decl(), callee.continuationArg)];

        UseKind kind;
        if(!useKindFor(mir::argumentMode(continuationArg.occurrence.contract,
                                         mir::SourceKind::Result), kind)) {
            report(node.source, "internal: a continuation parameter has no handover mode"_v);
            return;
        }

        arguments.push(editor.use(*continuation, kind));

        mir::CallSite site;
        mir::callTarget(context, program, function, mir::callRequestOf(node), site);

        /*
         * The signal for a transparent lens, the wrapper for a skipping one - and the two are read
         * from different places on purpose.
         *
         * A transparent lens's declared result is `$r`, a *variable* the call site bound to the
         * exit code (`handoverCall`), so its declaration is not the type that comes back here. A
         * skipping lens's is `Outcome(exit, E)` at the failure the call site substituted -
         * `skipOutcome`, since `E` may be a variable of the callee's own signature.
         */
        auto answered = node.kind == mir::Node::LensTry ? skipOutcome(node, node.source)
                                                        : mir::owned(program.types.exitCode());
        if(!answered.type) return;
        auto call = editor.addCall(node.source, site, Use(), arguments.pointer(), arguments.size(),
                                   answered, conventionFor(answered), true);

        auto region = source.region(mir, node.region);
        auto code = editor.resultOf(*call);

        if(node.kind == mir::Node::LensTry) {
            code = convertSkip(node, *region, *code);
            if(!code) return;
        }

        /*
         * Which exit it took, where there is more than one.
         *
         * A region that only falls out and one that only leaves each have a single answer, so the
         * discriminant is dead and nothing tests it - which is every scoped-acquisition call, and is
         * what keeps the signal from costing anything where it says nothing.
         */
        if(!plan->leaves && !plan->outward.size()) {
            if(!region->destination.valid()) {
                report(node.source, "internal: a lens call falls out to nowhere"_v);
                return;
            }

            editor.addJump(node.source, *base()[blocks[region->destination.index]], nullptr, 0);
            return;
        }

        /*
         * One test per row that means something *here* - `kExitLeave`, then each outward row - and
         * falling out as what is left, deliberately not a test against `kExitFallOut`.
         *
         * `kExitBreak` is a row the caller treats as falling out: the loop is over, and where the
         * loop was over is the destination. Testing for the rows that mean something here is what
         * makes adding a row below a change to the callee alone. Where the region never falls
         * out, the last row is what is left instead and is taken without a test.
         */
        auto fallback = plan->fallsOut && region->destination.valid();
        auto rows = plan->outward.size() + (plan->leaves ? 1 : 0);

        for(Size i = 0; i < rows; i++) {
            auto leaving = plan->leaves && i == 0;
            auto row = leaving ? kExitLeave : plan->outward[i - (plan->leaves ? 1 : 0)];

            // An outward row lands in this scope by construction, so taking it is a jump.
            auto take = [&] {
                if(leaving) {
                    /*
                     * What the region held and a `return` inside it left owned, released now that
                     * the callee's cleanup has run with it - placed past the call's terminator by
                     * `walkBlock` (K6). Before the exit propagates, so an enclosing traversal's
                     * cleanup runs after this one's storage is gone and before its own is.
                     */
                    emitSteps(convertingBlock, source.block(mir, BlockId(convertingBlock))->nodes.size());
                    emitLeaveFromCarrier(node.source);
                    return;
                }

                auto landing = base()[blocks[outwardLanding(row).index]];
                editor.addJump(node.source, *landing, nullptr, 0);
            };

            if(!fallback && i + 1 == rows) {
                take();
                return;
            }

            auto constant = editor.resultOf(
                *editor.addConst(node.source, mir::intConstant(program.types.exitCode(), row)));

            auto test = mir::owned(mir::booleanType(program));
            auto matches = editor.resultOf(*editor.addBinary(node.source, BinaryOp::Eq,
                                                             editor.use(*code, UseKind::Inspect),
                                                             editor.use(*constant, UseKind::Inspect),
                                                             test, conventionFor(test)));

            auto taking = editor.addBlock(StringId());
            auto next = i + 1 == rows ? base()[blocks[region->destination.index]]
                                      : editor.addBlock(StringId());

            editor.addBranch(node.source, editor.use(*matches, UseKind::Inspect), *taking, *next,
                             nullptr, 0);

            editor.setBlock(*taking);
            take();

            if(i + 1 == rows) return;
            editor.setBlock(*next);
        }
    }

    // The other half of `emitLeave`: an exit the continuation already parked, propagated. The
    // outermost scope is the one that reads the slot; every scope in between says only that it left.
    void emitLeaveFromCarrier(LocationId at) {
        if(outlining) {
            emitExit(at, kExitLeave);
            return;
        }

        if(!resultCarrier) {
            releaseSlots(at);
            editor.addReturn(at, nullptr, 0);
            return;
        }

        auto result = substituted(function.result);
        auto loaded = editor.resultOf(*editor.addLoad(at, editor.use(*base()[resultCarrier],
                                                                     UseKind::Inspect),
                                                      result, conventionFor(result)));

        /*
         * Under the result's own mode, which is a **borrow** where the function answers one - the
         * mode an ordinary `return` of the same value carries. A slot-named hand-over returned from
         * inside its region (`let w = firstOf(s) in: return w`, J122's rule) is the caller's storage
         * under the named origin, so the carrier holds a loan and the value loaded out of it is
         * guaranteed; returning it with `Consume` was the verifier's "return consumes a guaranteed
         * value" (J131).
         */
        UseKind kind;
        if(!useKindFor(mir::argumentMode(result.contract, mir::SourceKind::Result), kind)) {
            kind = UseKind::Consume;
        }

        auto used = editor.use(*loaded, kind);
        releaseSlots(at);
        editor.addReturn(at, &used, 1);
    }

    /*
     * The graft - `Graft`, and `Function::alwaysInline` for why it exists.
     *
     * Three blocks of the caller's are involved: the one the call was in, which jumps to the entry;
     * the entry and everything the callee's body makes after it, converted by a conversion of its
     * own over the callee's MIR and analysis; and the join, where every `return` in the body lands
     * with the result as the block argument and where this conversion continues. The callee is the
     * specialization `callTarget` chose, so the nested conversion reads the generic body under the
     * call's substitution exactly as a function of its own would.
     *
     * Recursion is refused at a depth rather than detected, because a body that is always expanded
     * and calls itself is a body with no bottom, and eight grafts deep is where "this is that" stops
     * being a guess.
     */
    Value* graftCall(mir::Node& node, mir::Function& callee, SmallArray<Use, 8>& arguments,
                     const mir::Occurrence& result, bool hasResult) {
        static constexpr U32 kMaxGraftDepth = 8;

        if(graftDepth >= kMaxGraftDepth) {
            report(node.source,
                   "this call to an `@inline(always)` function is eight expansions deep - a body that is always expanded cannot call itself"_v);
            graftRefused = true;
            return nullptr;
        }

        // Interned over a copy - `Program::internName` says why the formatted bytes cannot be.
        auto text = Tritium::format("%@.inline", context.findName(callee.name));
        auto name = program.internName(context, StringView(text.text(), text.size()));

        auto entry = editor.addBlock(name);
        auto join = editor.addBlock(context.addUnqualifiedName("join", 4));

        Value* value = nullptr;
        if(hasResult) value = editor.addBlockArgument(*join, result, conventionFor(result));

        editor.addJump(node.source, *entry, nullptr, 0);

        Graft graft { arguments.pointer(), arguments.size(), entry, join, value,
                      grafting ? grafting->site : node.source };

        // A scratch of its own, because this one is in use: the callee's nodes and locals are
        // numbered in its own body. The outlines are none by `checkAlwaysInline`'s refusal.
        Scratch inner;
        Array<Outline> none;

        Conversion nested(context, program, arena, callee, emitted, editor, inner, none, nullptr,
                          &graft, graftDepth + 1);
        if(!nested.run()) errors++;
        graftRefused = graftRefused || nested.graftRefused;

        editor.setBlock(*join);
        editor.setRegion(join->region);
        return value;
    }

    /*
     * The record's address, and one of its field addresses - the two things every part of a step
     * starts from.
     */
    Use stepSelf() { return addressUse(*stepping->self); }

    Value* stepField(LocationId at, U16 field) {
        auto record = asBase<mir::Type>(DeclPtr<mir::RecordType>(stepping->record - decl()));
        return editor.resultOf(*editor.addFieldAddress(at, stepSelf(), record, field));
    }

    void stepStoreIndex(LocationId at, I64 index) {
        auto slot = stepField(at, 0);
        auto written = editor.addConst(at, mir::intConstant(program.types.exitCode(), U64(index)));

        editor.addStore(at, editor.use(*slot, UseKind::Inspect),
                        editor.use(*editor.resultOf(*written), UseKind::Consume));
    }

    void stepReturn(LocationId at, bool more) {
        auto answer = editor.addConst(at, mir::truthConstant(program, more));
        Use values[1] = { editor.use(*editor.resultOf(*answer), UseKind::Consume) };
        editor.addReturn(at, values, 1);
    }

    /*
     * A **failing** producer's `return` - P2, J216. What it returns is `Outcome(exit, E)`, stored
     * whole into `$failure` and its tag read back: `Exit` leaves the record at `kIterFailed` for the
     * consumer's `f$failure` to take, and `Proceed` - ran out - is finished like any other producer.
     *
     * One exception, and it is the teardown: a producer entered with `$cancel` can still reach a
     * `return e` in the statements below its loops, and nobody is asking for its failure then - the
     * consumer left. So the failure is dropped where it lies and the record finished, which is the
     * `for` form's reading too: an alternative does not run after a `break`.
     */
    bool stepFail(LocationId at, mir::NodeReturn& written) {
        Use used;
        if(!useOf(written.value, at, used)) return true;

        auto& record = *stepping->record;
        auto held = record.field(decl(), record.failureField).occurrence;
        auto slot = stepField(at, record.failureField);
        editor.addStore(at, editor.use(*slot, UseKind::Inspect), used);

        auto tagType = mir::tagTypeOf(decl(), program.types, held.type);
        if(!tagType) {
            report(at, "internal: a failing object's `$failure` carries no discriminant"_v);
            return true;
        }

        auto tagAddress = editor.addFieldAddress(at, editor.use(*slot, UseKind::Inspect), held.type,
                                                 InstFieldAddress::kDiscriminant, held.shape);
        auto tagHeld = mir::owned(tagType);
        auto tag = editor.addLoad(at, editor.use(*editor.resultOf(*tagAddress), UseKind::Inspect),
                                  tagHeld, conventionFor(tagHeld));
        auto exit = editor.addConst(at, mir::intConstant(tagType, mir::kOutcomeExit));

        auto test = mir::owned(mir::booleanType(program));
        auto failed = editor.addBinary(at, BinaryOp::Eq,
                                       editor.use(*editor.resultOf(*tag), UseKind::Inspect),
                                       editor.use(*editor.resultOf(*exit), UseKind::Inspect),
                                       test, conventionFor(test));

        auto failing = editor.addBlock(StringId());
        auto done = editor.addBlock(StringId());
        editor.addBranch(at, editor.use(*editor.resultOf(*failed), UseKind::Inspect), *failing,
                         *done, nullptr, 0);

        editor.setBlock(*done);
        editor.setRegion(done->region);
        stepStoreIndex(at, I64(kIterFinished));
        stepReturn(at, false);

        editor.setBlock(*failing);
        editor.setRegion(failing->region);

        auto dropping = editor.addBlock(StringId());
        auto kept = editor.addBlock(StringId());
        editor.addBranch(at, editor.use(*stepping->cancel, UseKind::Inspect), *dropping, *kept,
                         nullptr, 0);

        editor.setBlock(*dropping);
        editor.setRegion(dropping->region);

        auto& outcome = *(mir::RecordType*)decl()[held.type];
        auto reason = outcome.fields.get(decl(), outcome.cons.get(decl(), mir::kOutcomeExit).first);
        if(mir::hasTeardownAt(decl(), reason.occurrence)) {
            auto address = editor.addFieldAddress(at, editor.use(*slot, UseKind::Inspect), held.type,
                                                  reason.index, held.shape);
            editor.addDrop(at, editor.use(*editor.resultOf(*address), UseKind::Inspect),
                           mir::owned(reason.occurrence.type));
        }

        stepStoreIndex(at, I64(kIterFinished));
        stepReturn(at, false);

        editor.setBlock(*kept);
        editor.setRegion(kept->region);
        stepStoreIndex(at, I64(kIterFailed));
        stepReturn(at, false);
        return true;
    }

    /*
     * The runs a `@heap`-placed payload member lives in, handed back - `RecordType::kHeapMember`.
     *
     * At each path out of the *object*, which is a path out of the step that leaves the record
     * finished: `f$new` made one allocation per member and the record owes one free each. Not at a
     * hand-over, which is a path out of the step that leaves the record alive.
     */
    void stepReleaseRuns(LocationId at) {
        for(Size i = stepping->runFields.size(); i > 0; i--) {
            auto address = stepField(at, stepping->runFields[i - 1]);
            auto held = mir::owned(stepping->record->field(decl(),
                                   stepping->runFields[i - 1]).occurrence.type);

            auto run = editor.addLoad(at, editor.use(*address, UseKind::Inspect), held,
                                      conventionFor(held), false);
            editor.addFree(at, editor.use(*editor.resultOf(*run), UseKind::Inspect));
        }
    }

    /*
     * One **embedded region**'s member ended - `g$step(&member, "leave", True)`, which is the child's
     * own teardown reached without going through `Drop`.
     *
     * For a **lens** member this is where §6.6's "the lens's cleanup runs at the right time on every
     * path" is discharged: a cancellation is the lens's step entered with "leave", which its `yield`
     * reads as the consumer being gone - so `emitDeparture` walks out of whatever loops the `yield`
     * was in, the statements below them are the cleanup, and drop elaboration releases what the lens
     * still holds. The same call that ends an iterator's pass early releases a lock.
     *
     * The child's cancellation is its step entered with a cancel request, exactly as `f$drop` enters
     * the outer one: at a suspend index the child's departure path runs and releases what it holds,
     * and at the **finished** index the dispatch's fallback returns having released nothing. That
     * second row is what makes this idempotent and is why nothing here needs a flag saying whether
     * the child is running: `f$new` writes the finished index into every nested member, so a child
     * that was never entered, one that ran out and one that was already cancelled are all a call
     * that does nothing.
     *
     * Emitted where the region is left by a `break` - which is also a departure out of the outer
     * object - and again at the producer's `return`, so that every path out of the object cancels a
     * suspended child exactly once.
     */
    /*
     * Where a nest member's **pair** is, and what type it has - the member itself for an ordinary
     * box, and the link's first field for a delegating one (`RecordType::linkType`).
     *
     * Two spellings of one member, so the cancel path, the empty pair and the construction all read
     * it through here rather than each testing which shape the field has.
     */
    DeclPtr<mir::Type> stepBoxType(U16 field) {
        auto held = stepping->record->field(decl(), field).occurrence.type;
        if(held != stepping->record->linkType) return held;

        return ((mir::RecordType*)decl()[held])->field(decl(), 0).occurrence.type;
    }

    Value* stepBoxPair(LocationId at, U16 field) {
        auto held = stepping->record->field(decl(), field).occurrence.type;
        auto address = stepField(at, field);
        if(held != stepping->record->linkType) return address;

        return editor.resultOf(*editor.addFieldAddress(at, editor.use(*address, UseKind::Inspect),
                                                       held, 0));
    }

    void stepCancelField(LocationId at, U16 field, DeclPtr<mir::Function> shim) {
        auto held = stepBoxType(field);

        /*
         * A **boxed** level, which is ended by releasing the box rather than by stepping it - §6.3.
         *
         * A drop of the pair is the child's whole teardown: `funTeardown` finds the shim's release
         * through the cell in front of its code, which runs the child record's own `Drop` - its
         * step entered with a cancel request - and hands the block back. So one instruction runs
         * the departure path of every level below this one, and the recursion is the machine's
         * rather than something this walk has to spell.
         *
         * The **empty pair** written back is what makes it idempotent, and it is the same value
         * `f$new` writes: a box is released by the region it was built in *and* by the producer's
         * `return`, exactly as a concrete child is cancelled twice, and a second free is not the
         * no-op a second cancel is. A drop of a pair whose environment word is null does nothing -
         * `lower/fun.cpp`'s `funTeardown`, where that word is the first of its two questions.
         */
        if(decl()[held]->kind == mir::Type::Iter) {
            if(!shim) return;

            editor.addDrop(at, editor.use(*stepBoxPair(at, field), UseKind::Inspect),
                           mir::owned(held));
            stepStoreEmptyBox(at, field, shim);
            return;
        }

        auto& child = *(mir::RecordType*)decl()[held];
        if(!child.producerStep) return;

        auto& childStep = *decl()[child.producerStep];

        auto borrow = mir::borrowed(held, mir::Access::Unique);
        auto self = editor.resultOf(*editor.addLoad(at, editor.use(*stepField(at, field),
                                                                   UseKind::Inspect),
                                                    borrow, conventionFor(borrow), false));

        auto leave = exitRow(program, editor, decl()[childStep.args.get(decl(), 1)]->occurrence.type,
                             kExitLeave, at);
        auto stop = editor.addConst(at, mir::truthConstant(program, true));

        mir::CallSite site;
        site.target = child.producerStep;
        site.through = mir::CallThrough::Declaration;

        Use arguments[3] = {
            editor.use(*self, UseKind::Borrow),
            leave,
            editor.use(*editor.resultOf(*stop), UseKind::Consume),
        };

        editor.addCall(at, site, Use(), arguments, 3, childStep.result,
                       conventionFor(childStep.result), true);
    }

    void stepCancelNest(LocationId at, ContinuationId region) {
        auto field = stepping->nestOf[region.index];
        if(field == StepPlan::kNoField) return;

        stepCancelField(at, U16(field), stepping->nestBoxed[region.index]);
    }

    /*
     * The **empty pair** a boxed member holds while its region has not been entered - §6.3.
     *
     * A function value whose environment word is null, built out of the same shim a real box points
     * at: the code word is therefore honest, and a drop of a pair with no environment does nothing,
     * so there is no separate flag to say whether the level is there. That the shim is *shared* by
     * the empty pair and a filled one is why `funTeardown` tests the word at all and not only the
     * cell in front of the code - D17, `lower/fun.cpp`. `emitIterNew` writes the same value into
     * the field, which is why this is a method rather than two spellings.
     */
    void stepStoreEmptyBox(LocationId at, U16 field, DeclPtr<mir::Function> shim) {
        auto occurrence = mir::owned(stepBoxType(field));

        auto empty = editor.resultOf(*editor.addFunValue(at, shim, nullptr, occurrence,
                                                         conventionFor(occurrence)));

        editor.addStore(at, editor.use(*stepBoxPair(at, field), UseKind::Inspect),
                        editor.use(*empty, UseKind::Consume));
    }

    // Every embedded region's member, at a path out of the **object** - the producer's `return`,
    // which is exhaustion or the departure a cancellation takes. `stepReleaseRuns`' neighbour and for
    // its reason: what the record still owes at that point is not something any drop the ownership
    // analysis placed can reach, because a region's member is not a local of the producer's body.
    //
    // In field order reversed, which is body order reversed: a region nested inside another opened
    // later and so holds a later field, and the inner one has to be ended before the outer - a lens
    // inside a lens releases in the order the two `yield`s would have returned in.
    void stepCancelNests(LocationId at) {
        for(Size i = stepping->nestFields.size(); i > 0; i--) {
            stepCancelField(at, stepping->nestFields[i - 1], stepping->nestFieldBoxed[i - 1]);
        }
    }

    /*
     * The branch a delegating region makes on the departure guard - §6.3, and `stepStoreGuard`'s
     * counterpart: one writes the guard and this reads it.
     *
     * Answers false where there is no guard to read, having left the conversion where it found it -
     * a producer with no `yield` at all has no `$yielded`, and a region inside one cannot be on a
     * departure path.
     */
    bool stepDeparting(LocationId at, Block& departing, Block& delegating) {
        auto local = source.yieldedLocal;
        if(!local.valid() || local.index >= slots.size() || !slots[local.index]) return false;

        auto held = mir::owned(program.types.exitCode());
        auto loaded = editor.addLoad(at, editor.use(*base()[slots[local.index]], UseKind::Inspect),
                                     held, conventionFor(held), false);

        auto zero = editor.addConst(at, mir::intConstant(held.type, 0));
        auto truth = mir::owned(mir::booleanType(program));

        auto left = editor.addBinary(at, mir::BinaryOp::Ne,
                                     editor.use(*editor.resultOf(*loaded), UseKind::Inspect),
                                     editor.use(*editor.resultOf(*zero), UseKind::Inspect), truth,
                                     conventionFor(truth));

        editor.addBranch(at, editor.use(*editor.resultOf(*left), UseKind::Inspect), departing,
                         delegating, nullptr, 0);
        return true;
    }

    /*
     * A region entered on the **departure** path, which is the non-delegating emission with the
     * region's own blocks taken out - §6.3.
     *
     * One step of the child and then the box released, which is exactly what those blocks do when
     * the guard is set: the item arrives, the entry sees that the consumer has gone, and the `break`
     * arm cancels the member and carries on where the region rejoins. The item itself is dropped on
     * the floor, which is what the skipped `yield` would have done with it.
     */
    /*
     * A boxed or delegated **failing** child's answer, split - C1c, J218. A tagged answer is the
     * child's failure: moved out of the child's record first, since releasing the box frees it, then
     * the box released and the reason handed to the region's `otherwise` edge - the edge the inline
     * form (C1b) and `convertSkip` hand it on. Anything else continues in the block this leaves
     * current.
     */
    void stepBoxFailure(LocationId at, Value* answer, U16 field, DeclPtr<mir::Function> shim,
                        const mir::IterType& iterated, Block& otherwise) {
        auto failure = editor.addBlock(StringId());
        auto rest = editor.addBlock(StringId());
        editor.addBranch(at, editor.use(*iterAnswerTagged(program, editor, answer, at),
                                        UseKind::Inspect), *failure, *rest, nullptr, 0);

        editor.setBlock(*failure);
        editor.setRegion(failure->region);

        auto reason = iterErasedReason(program, editor, answer, iterated, at);
        stepCancelField(at, field, shim);

        Use handed = editor.use(*reason, UseKind::Consume);
        editor.addJump(at, otherwise, &handed, 1);

        editor.setBlock(*rest);
        editor.setRegion(rest->region);
    }

    void stepDrainBox(LocationId at, U16 field, DeclPtr<mir::Function> shim, Block& destination,
                      Block* otherwise = nullptr) {
        auto held = stepBoxType(field);
        auto& iterated = *(mir::IterType*)decl()[held];

        auto borrow = mir::borrowed(held, mir::Access::Unique);
        auto self = editor.resultOf(*editor.addLoad(at, editor.use(*stepBoxPair(at, field),
                                                                   UseKind::Inspect),
                                                    borrow, conventionFor(borrow), false));

        auto next = exitRow(program, editor, iterated.sent, kExitFallOut, at);
        auto carry = editor.addConst(at, mir::truthConstant(program, false));

        mir::CallSite through;
        through.target = nullptr;
        through.through = mir::CallThrough::Value;

        auto answered = mir::owned(program.types.pointerType(iterated.handover.type));
        Use arguments[2] = { next, editor.use(*editor.resultOf(*carry), UseKind::Consume) };

        auto found = editor.resultOf(*editor.addCall(at, through, editor.use(*self, UseKind::Inspect),
                                                     arguments, 2, answered,
                                                     conventionFor(answered), true));

        // A failing child's one step may be its failure, which the alternative takes on this path
        // too - the inline form's reading when the guard is set (C1c).
        if(otherwise && iterated.failure) {
            stepBoxFailure(at, found, field, shim, iterated, *otherwise);
        }

        stepCancelField(at, field, shim);
        editor.addJump(at, destination, nullptr, 0);
    }

    /*
     * `guardYields`' own local, written with what the consumer answered - §6.3, and the one write a
     * delegating level owes that its own blocks would have made.
     *
     * A traversal's departure is carried in `$yielded`: the `yield` writes what the continuation
     * answered there, and everything below the loops reads it to find out whether the consumer left.
     * A level that delegates never runs its `yield`, so its step writes the same thing when the
     * child hands control back - and what it writes is `$resume`, which is the same value one tier
     * over. `Body::yieldedLocal` is what names the storage.
     */
    void stepStoreGuard(LocationId at) {
        auto local = source.yieldedLocal;
        if(!local.valid() || local.index >= slots.size() || !slots[local.index]) return;

        editor.addStore(at, editor.use(*base()[slots[local.index]], UseKind::Inspect),
                        editor.use(*stepping->resumeValue, UseKind::Consume));
    }

    void stepStoreHeld(LocationId at, bool holding) {
        auto flag = stepField(at, stepping->record->heldField);
        auto written = editor.addConst(at, mir::truthConstant(program, holding));

        editor.addStore(at, editor.use(*flag, UseKind::Inspect),
                        editor.use(*editor.resultOf(*written), UseKind::Consume));
    }

    /*
     * The slot released where `$held` says it still holds one, and the flag cleared.
     *
     * Three places ask for it and each is a point where the record stops being responsible for what
     * it handed over: the next hand-over, which is about to overwrite the slot; the producer's own
     * `return`, which is exhaustion or a departure; and the cancellation of a record nothing has
     * taken from. A branch and not an unconditional drop, because a consumer that took the value is
     * the common case and the slot is then a value that no longer exists.
     *
     * Leaves the conversion in the block execution continues in, which is why it is written as a
     * diamond rather than as a call: everything after it is emitted into the join.
     */
    void stepReleaseHeld(LocationId at) {
        if(!stepping->record->heldField) return;

        auto flag = stepField(at, stepping->record->heldField);
        auto held = mir::owned(mir::booleanType(program));
        auto loaded = editor.addLoad(at, editor.use(*flag, UseKind::Inspect), held,
                                     conventionFor(held), false);

        auto release = editor.addBlock(StringId());
        auto join = editor.addBlock(StringId());

        editor.addBranch(at, editor.use(*editor.resultOf(*loaded), UseKind::Inspect), *release,
                         *join, nullptr, 0);

        editor.setBlock(*release);
        editor.setRegion(release->region);

        auto item = stepping->record->field(decl(), 1).occurrence;
        editor.addDrop(at, editor.use(*stepField(at, 1), UseKind::Inspect),
                       mir::owned(item.type));
        stepStoreHeld(at, false);
        editor.addJump(at, *join, nullptr, 0);

        editor.setBlock(*join);
        editor.setRegion(join->region);
    }

    /*
     * A **region**, embedded - `for x in g(...)` or `withLock(m):` written inside the producer,
     * §3.2's first step and the fifth shape a step's conversion takes.
     *
     *      %child = g$new(...)              -- the member, initialized where the call was
     *      store %child -> self.$g
     *      jump advance
     *
     *    advance:
     *      %more = g$step(&self.$g, fallOut, False)
     *      branch %more -> obtain -> <the region's destination>
     *
     *    obtain:
     *      %item = <the child's slot, under g's own convention>
     *      jump <the region's entry>(%item)
     *
     * `convertLensCall` **outlines** the region into a function and lets the callee call it. A step
     * cannot: the outer producer is suspended rather than running, so there is no frame for the
     * callee to hand control back into. So the region's blocks are emitted as this function's own
     * and the three exits are mapped where the outlined form maps them - fall-out asks the member
     * for the next element (`convertEdge`), `break` cancels the child and resumes at the region's
     * destination, and a `return` or a departure propagates outward. A `yield` written inside the
     * region needs nothing of its own: it writes the *outer* record's slot and index exactly as one
     * written anywhere else does, and the child stays suspended in its field.
     *
     * § A lens region is this picture unchanged, which is the finding of §6.6
     *
     * `withLock(m): yield x` opens a `Once` region rather than a `Many` one, and J53 read that as a
     * different mapping of the same three exits: falling out of a lens's continuation is the lens
     * *finishing* rather than the loop coming round. It is the same mapping. A lens's step runs to
     * its one `yield` and answers `True`; stepped again it runs the code after the `yield` - the
     * cleanup - and answers `False`. So `advance` entered a second time is exactly "run the
     * cleanup", and the `False` arm it already has is exactly "carry on at the region's
     * destination". Nothing in this function distinguishes the two multiplicities, and the block
     * that loops for an iterator is the block that releases for a lens.
     *
     * What that leaves is the three refusals `collectIterNests` makes: a skipping region, because
     * the step's answer is a `Bool` with no room for a failure; a `Many` region whose passes hand a
     * value back; and a callee this stage cannot name a body for.
     *
     * Answers true having reported, on the same terms as the rest of this file.
     */
    bool convertStepNest(mir::NodeLensCall& node) {
        auto at = node.source;
        auto region = source.region(mir, node.region);
        auto field = U16(stepping->nestOf[node.region.index]);

        auto held = stepping->record->field(decl(), field).occurrence.type;

        /*
         * A **boxed** level - §6.3's recursion, and the one nest whose member does not name its own
         * child.
         *
         * The field is the protocol `iter(Y)`, so the record to build is read off the shim
         * (`RecordType::nestBoxed`), and what goes in the field is the pair rather than the record:
         * `g$new` into a local of this step, one `fun.value` over it, and the lowering relocates it
         * into a heap block with the child's teardown in front of it. So the allocation is one per
         * level *entered* rather than one per level the type mentions, which is what makes the
         * record finite at all.
         */
        auto shim = stepping->nestBoxed[node.region.index];
        auto boxed = shim != nullptr;

        // And whether the level **delegates** rather than steps - §6.3. Its member is the link and
        // its pair is the raw shim, so what is read out of the field below is the link's first field.
        auto delegates = stepping->nestDelegate[node.region.index] != 0;
        if(delegates) held = stepBoxType(field);

        if(boxed && decl()[held]->kind != mir::Type::Iter) {
            report(at, "internal: a boxed region's member is not an erased object"_v);
            return true;
        }

        // A failing child held as a box - recursive, or delegated to - answers its failure on the
        // erased channel (C1c), and the region's `otherwise` is where it goes.
        auto otherwise = region->otherwise.valid() ? base()[blocks[region->otherwise.index]]
                                                   : nullptr;
        auto boxFails = (boxed || delegates) && otherwise &&
                        ((mir::IterType*)decl()[held])->failure;

        auto childType = boxed ? decl()[shim]->generatedRecord : held;

        if(!childType || decl()[childType]->kind != mir::Type::Record) {
            report(at, "internal: an embedded region has no object to step"_v);
            return true;
        }

        auto& child = *(mir::RecordType*)decl()[childType];

        if(!region || !child.producerNew || !child.producerStep || !region->entry.valid() ||
           !region->destination.valid()) {
            report(at, "internal: an embedded region has no object to step"_v);
            return true;
        }

        /*
         * The child, built where the call was - one call of `g$new`, whose signature is where the
         * borrowing is stated, exactly as a `let it = g(...)` in the consumer builds one.
         */
        auto& make = *decl()[child.producerNew];

        SmallArray<Use, 8> arguments;
        for(auto operand: node.arguments.contents(mir)) {
            Use used;
            if(!useOf(operand, at, used)) return true;

            /*
             * Under **`g$new`'s** conventions and not the lens call's.
             *
             * The node was built against `g`'s own parameters - a shared loan of a counter, say -
             * and the constructor takes the same argument under whatever the *record* holds it by,
             * which for a trivially copyable one is by value. The value is the same; the statement
             * about it is the constructor's, exactly as it is at a `let it = g(...)`, where the
             * resolver makes the call against `g$new`'s declaration for this reason.
             */
            if(arguments.size() >= make.args.size()) break;

            auto& declared = *decl()[make.args.get(decl(), arguments.size())];

            UseKind kind;
            if(!useKindFor(mir::argumentMode(declared.occurrence.contract,
                                             mir::SourceKind::Result), kind)) {
                report(at, "internal: an embedded region's capture has no handover mode"_v);
                return true;
            }

            /*
             * And the **bytes** the constructor's position takes, which is a load where the producer
             * holds a reference and the field holds a copy.
             *
             * `items(buffer)` over the producer's own fixed array is where it shows: the record
             * captures a trivially copyable whole value by value, so `g$new` consumes one - and what
             * the producer has in hand is a loan, which is that array's address. A call boundary
             * would make the adaptation from the two declarations; this call is built here and makes
             * it here.
             */
            auto value = base()[used.value()];

            if(referenceValued(value->occurrence) && !referenceValued(declared.occurrence)) {
                auto held = mir::owned(declared.occurrence.type);
                value = editor.resultOf(*editor.addLoad(at, editor.use(*value, UseKind::Inspect),
                                                        held, conventionFor(held), false));
            }

            arguments.push(editor.use(*value, kind));
        }

        mir::CallSite making;
        making.target = child.producerNew;
        making.through = mir::CallThrough::Declaration;

        auto built = editor.resultOf(*editor.addCall(at, making, Use(), arguments.pointer(),
                                                     arguments.size(), make.result,
                                                     conventionFor(make.result), true));

        if(boxed) {
            /*
             * The box: storage in this step's own frame for the child, and a pair naming it.
             *
             * The frame local is where the record is built and the pair's environment word points
             * at it *here*; `emitFunValue` reads the shim's `environmentStorage` and moves the bytes
             * into a heap block, so what survives this step is the block and not the local. That is
             * a lambda's own arrangement one construct over - `adaptIterValue` makes the same pair
             * at an owning position - and it is why nothing here allocates by name.
             */
            auto storage = mir::owned(childType);
            auto local = editor.resultOf(*editor.addAlloc(at, storage, mir::StorageClass::Frame));

            editor.addStore(at, editor.use(*local, UseKind::Inspect),
                            editor.use(*built, UseKind::Consume));

            /*
             * And which of the child's two shims the pair names, which for a **delegating** region
             * is decided here rather than at the mint - §6.3.
             *
             * A level the consumer has already left does not delegate: a departure runs the code
             * below the producer's loops, and that code may open this very region again with nobody
             * left to step what it builds. So the guard - `guardYields`' own local, the same one the
             * region's entry block tests - picks the path, and the two paths want two different
             * pairs over one record: the raw shim where the level delegates, because only that one
             * may answer the third row, and the ordinary one where the level steps the child itself
             * and a third row would be an answer nobody is listening for.
             */
            auto departing = delegates ? editor.addBlock(StringId()) : nullptr;
            auto delegating = delegates ? editor.addBlock(StringId()) : nullptr;

            if(delegates && !stepDeparting(at, *departing, *delegating)) {
                delegates = false;
                departing = nullptr;
                delegating = nullptr;
            }

            auto fill = [&](DeclPtr<mir::Function> named) {
                auto occurrence = mir::owned(held);
                Use environment = editor.use(*local, UseKind::Inspect);
                auto pair = editor.resultOf(*editor.addFunValue(at, named, &environment, occurrence,
                                                                conventionFor(occurrence)));

                editor.addStore(at, editor.use(*stepBoxPair(at, field), UseKind::Inspect),
                                editor.use(*pair, UseKind::Consume));
            };

            if(delegates) {
                editor.setBlock(*departing);
                editor.setRegion(departing->region);

                auto plain = child.producerBox;

                if(!plain) {
                    report(at, "internal: a delegating region has no ordinary shim to drain with"_v);
                    return true;
                }

                fill(plain);
                stepDrainBox(at, field, plain, *base()[blocks[region->destination.index]],
                             boxFails ? otherwise : nullptr);

                editor.setBlock(*delegating);
                editor.setRegion(delegating->region);
            }

            fill(shim);
        } else {
            editor.addStore(at, editor.use(*stepField(at, field), UseKind::Inspect),
                            editor.use(*built, UseKind::Consume));
        }

        /*
         * A **delegating** level stops here - §6.3, and this is the whole of what delegation emits
         * in a step.
         *
         * The child is built and its pair is in the member's link; what is left is to say so. Which
         * member it is goes into `$down` - the third row of this step's answer, which `f$erased.step`
         * reads and nothing else does - and the level suspends at an index of its own. From here the
         * consumer's trampoline steps the child directly, and comes back to this level only when the
         * child runs out.
         *
         * `$up` is left where it is: the trampoline writes it as it pushes, because the level above
         * this one is a fact about the *chain* and not about this call.
         */
        if(delegates) {
            auto slot = editor.resultOf(*editor.addFieldAddress(
                at, editor.use(*stepField(at, field), UseKind::Inspect),
                stepping->record->linkType, 1));

            /*
             * And **which member** it is, rather than where that member currently is: `field << 1 |
             * 1` is the chain word's tagged reading (`iterLinkAddress`), resolved against whatever
             * `self` the reader was entered with. A record moves with whatever holds it, so an
             * address of a field of *this* record is a value one relocation of the object makes
             * wrong - and the root's own link is exactly the link a consumer can relocate.
             */
            auto word = mir::owned(program.types.words[0]);
            auto link = editor.resultOf(*editor.addConst(
                at, mir::intConstant(word.type, (U64(field) << 1) | 1)));

            auto zero = editor.addConst(at, mir::intConstant(word.type, 0));
            editor.addStore(at, editor.use(*slot, UseKind::Inspect),
                            editor.use(*editor.resultOf(*zero), UseKind::Consume));

            editor.addStore(at, editor.use(*stepField(at, stepping->record->downField),
                                           UseKind::Inspect),
                            editor.use(*link, UseKind::Consume));

            /*
             * And the level suspends - at an index of its own, past the hand-overs', because the
             * hand-over this region holds is one it is never going to run.
             */
            stepStoreIndex(at, I64(1 + stepping->yields + node.region.index));
            stepReturn(at, true);

            /*
             * And where it comes back once the child has run out.
             *
             * Two things, and they are the two the region's own blocks would have done. **The guard**
             * first: `guardYields`' local is what the code after the region reads to find out whether
             * the consumer has left, and a level that never runs its own `yield x` never writes it -
             * so this writes what the consumer answered, exactly as the hand-over would have. Then
             * the box, released, which is `advance`'s false arm with the step taken out. Without the
             * first of those a consumer's `break` deep in a walk ran the rest of every level above
             * it, which is the defect this pass found by running `iter/Recursion.yana`.
             */
            auto resumed = editor.addBlock(StringId());
            stepping->delegateResume[node.region.index] = resumed - base();

            editor.setBlock(*resumed);
            editor.setRegion(resumed->region);

            stepStoreGuard(at);

            /*
             * A **failing** child comes back here having failed or run out, and the trampoline
             * could not say which - it read null either way, which is what made it step this level.
             * So the level asks its child's pair once more: `f$erased.step` answers the failure it
             * parked (`kIterFailedHeld`) or null, and a failure goes to this region's alternative,
             * run in this level's own step - which may `yield` (the trampoline hands the item on),
             * fail this level too (and the level above asks the same question of it), or fall
             * through. One extra call per level *left*, and nothing per element.
             */
            if(boxFails) {
                auto& iterated = *(mir::IterType*)decl()[held];
                auto borrow = mir::borrowed(held, mir::Access::Unique);
                auto pair = editor.resultOf(*editor.addLoad(at, editor.use(*stepBoxPair(at, field),
                                                                           UseKind::Inspect),
                                                            borrow, conventionFor(borrow), false));

                auto next = exitRow(program, editor, iterated.sent, kExitFallOut, at);
                auto carry = editor.addConst(at, mir::truthConstant(program, false));

                mir::CallSite through;
                through.target = nullptr;
                through.through = mir::CallThrough::Value;

                auto answered = mir::owned(program.types.pointerType(iterated.handover.type));
                Use asking[2] = { next, editor.use(*editor.resultOf(*carry), UseKind::Consume) };

                auto found = editor.resultOf(*editor.addCall(at, through,
                                                             editor.use(*pair, UseKind::Inspect),
                                                             asking, 2, answered,
                                                             conventionFor(answered), true));

                stepBoxFailure(at, found, field, shim, iterated, *otherwise);
            }

            stepCancelField(at, field, shim);
            editor.addJump(at, *base()[blocks[region->destination.index]], nullptr, 0);
            return true;
        }

        auto advance = editor.addBlock(StringId());
        stepping->nestAdvance[node.region.index] = advance - base();
        editor.addJump(at, *advance, nullptr, 0);

        /*
         * A **lens** region's fall-out gets a block of its own - `release` - rather than the second
         * entry to `advance` described below.
         *
         * The two are the same code, and J53's reading of them as one is right about what runs: the
         * second step of a lens is its cleanup and answers `False`. What it was wrong about is the
         * graph. With one block, `advance`'s true arm is reachable from the fall-out, so the region's
         * entry has the resume blocks among its predecessors - and a value the producer defined before
         * the region and spills across a `yield` inside it is then used at the entry on a path that
         * never defined it. `planSpillReach` and `LiveAcross` walk MIR, where a `Once` region's
         * fall-out goes to its destination and nowhere else, so they answered "the definition
         * dominates" and the verifier said "store names N, whose definition does not dominate it"
         * (Implementation-Streams P3, J185). Giving the SSA the graph MIR states is one block; teaching
         * both analyses an edge no program takes would have been two, and would have spilled values a
         * lens region never needs.
         *
         * `checkHandover` is what makes ignoring the answer sound: a lens hands over exactly once on
         * every path, so the second step's answer is always `False`. A boxed region is an `iter fn`'s
         * recursion and is never `Once`.
         */
        Block* release = nullptr;
        if(region->multiplicity == mir::Continuation::Once && !boxed) {
            release = editor.addBlock(StringId());
            stepping->nestAdvance[node.region.index] = release - base();
        }

        /*
         * One step of the member, asked for here and again at every block of the region that falls
         * out - which is the same block for the two multiplicities and means two things.
         *
         * "Fall out" is the exit signal `kExitFallOut` one tier over and means the pass ended, which
         * is what a consumer's `for` says by coming round its own back edge. The child answers
         * whether its slot now holds something, so the false arm is the loop being over and goes to
         * where the region rejoins - which is a block of *this* body rather than a discriminant the
         * caller tests, and is the whole of the difference from the outlined form.
         *
         * For a **lens** the first entry is the acquisition and the second is the release: a lens
         * hands over exactly once on every path (`checkHandover`), so the first step answers `True`
         * and the second runs the code after its `yield` and answers `False`. The false arm is then
         * the cleanup having run rather than the elements having run out, and it goes to the same
         * place. Which is why §6.6's "a different mapping of the same three exits" turned out to be
         * the same mapping.
         */
        editor.setBlock(*advance);
        editor.setRegion(advance->region);

        auto borrow = mir::borrowed(held, mir::Access::Unique);
        auto slot = stepField(at, field);

        auto self = editor.resultOf(*editor.addLoad(at, editor.use(*slot, UseKind::Inspect), borrow,
                                                    conventionFor(borrow), false));

        auto obtain = editor.addBlock(StringId());
        auto& destination = *base()[blocks[region->destination.index]];
        Value* found = nullptr;
        DeclPtr<mir::Type> carried = nullptr;
        auto foundLends = false;

        if(boxed) {
            /*
             * One step of the box, through the pair - `CallThrough::Value` against the signature
             * `IterType::step` carries, which is the same call `Iterate`'s erased `next` makes.
             *
             * The answer is **where the item is** rather than a `Bool` and a slot, because a pair
             * carries no record for this block to read a field out of - so the loop test is the one
             * value an address cannot have. That indirect call, once per level between the root and
             * the leaf, is the cost §6.3 states.
             */
            auto& iterated = *(mir::IterType*)decl()[held];
            carried = iterated.handover.type;
            foundLends = iterated.lends;

            auto next = exitRow(program, editor, iterated.sent, kExitFallOut, at);
            auto carry = editor.addConst(at, mir::truthConstant(program, false));

            mir::CallSite through;
            through.target = nullptr;
            through.through = mir::CallThrough::Value;

            auto answered = mir::owned(program.types.pointerType(iterated.handover.type));
            Use boxArguments[2] = {
                next,
                editor.use(*editor.resultOf(*carry), UseKind::Consume),
            };

            found = editor.resultOf(*editor.addCall(at, through,
                                                    editor.use(*self, UseKind::Inspect),
                                                    boxArguments, 2, answered,
                                                    conventionFor(answered), true));

            auto word = mir::owned(program.types.words[0]);
            auto number = editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Bitcast,
                                                           editor.use(*found, UseKind::Inspect),
                                                           word, conventionFor(word)));

            auto zero = editor.addConst(at, mir::intConstant(word.type, 0));
            auto truth = mir::owned(mir::booleanType(program));

            auto more = editor.addBinary(at, mir::BinaryOp::Ne,
                                         editor.use(*number, UseKind::Inspect),
                                         editor.use(*editor.resultOf(*zero), UseKind::Inspect),
                                         truth, conventionFor(truth));

            /*
             * And the level released where it ran out, which a concrete child does not need: what
             * ends one of those is a finished index the next cancellation reads, and what ends this
             * is an allocation nobody else will hand back. The empty pair written in its place is
             * what keeps the producer's own `return` from freeing it a second time.
             */
            auto ended = editor.addBlock(StringId());

            if(boxFails) {
                // A failing child's answer is tested for the tag only where it was not null, so an
                // element pays the `and` and the null test it already paid (C1c).
                auto answeredSome = editor.addBlock(StringId());
                editor.addBranch(at, editor.use(*editor.resultOf(*more), UseKind::Inspect),
                                 *answeredSome, *ended, nullptr, 0);

                editor.setBlock(*answeredSome);
                editor.setRegion(answeredSome->region);

                stepBoxFailure(at, found, field, shim, iterated, *otherwise);
                editor.addJump(at, *obtain, nullptr, 0);
            } else {
                editor.addBranch(at, editor.use(*editor.resultOf(*more), UseKind::Inspect), *obtain,
                                 *ended, nullptr, 0);
            }

            editor.setBlock(*ended);
            editor.setRegion(ended->region);

            editor.addDrop(at, editor.use(*stepField(at, field), UseKind::Inspect),
                           mir::owned(held));
            stepStoreEmptyBox(at, field, shim);
            editor.addJump(at, destination, nullptr, 0);
        } else {
            /*
             * A child the outer producer holds **inline**, stepped where it is - and through its own
             * trampoline where it has one (§6.3).
             *
             * A delegating record's step may answer "I entered a child", which only `f$walk` knows
             * what to do with, so a region over one has to go through the same door every other
             * consumer of it goes through. What comes back is then an address rather than a `Bool`,
             * which is the boxed arm's own reading one indirection less - so the item is read the
             * same way and the two arms differ only in how the step is reached.
             */
            auto walking = child.producerWalk != nullptr;
            auto& childStep = *decl()[walking ? child.producerWalk : child.producerStep];

            auto callStep = [&](Value* stepped) {
                auto next = exitRow(program, editor,
                                    decl()[childStep.args.get(decl(), 1)]->occurrence.type,
                                    kExitFallOut, at);
                auto carry = editor.addConst(at, mir::truthConstant(program, false));

                mir::CallSite stepping3;
                stepping3.target = walking ? child.producerWalk : child.producerStep;
                stepping3.through = mir::CallThrough::Declaration;

                Use stepArguments[3] = {
                    editor.use(*stepped, UseKind::Borrow),
                    next,
                    editor.use(*editor.resultOf(*carry), UseKind::Consume),
                };

                return editor.addCall(at, stepping3, Use(), stepArguments, 3, childStep.result,
                                      conventionFor(childStep.result), true);
            };

            auto answered = callStep(self);
            auto more = editor.resultOf(*answered);

            if(release) {
                auto resumeAt = editor.currentBlock();

                editor.setBlock(*release);
                editor.setRegion(release->region);

                auto again = editor.resultOf(*editor.addLoad(at, editor.use(*stepField(at, field),
                                                                            UseKind::Inspect),
                                                             borrow, conventionFor(borrow), false));
                callStep(again);
                editor.addJump(at, destination, nullptr, 0);

                editor.setBlock(*resumeAt);
                editor.setRegion(resumeAt->region);
            }

            if(walking) {
                found = more;
                carried = ((mir::PointerType*)decl()[childStep.result.type])->to;
                foundLends = decl()[child.field(decl(), 1).occurrence.type]->kind ==
                             mir::Type::Pointer;

                auto word = mir::owned(program.types.words[0]);
                auto number = editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Bitcast,
                                                               editor.use(*found, UseKind::Inspect),
                                                               word, conventionFor(word)));

                auto zero = editor.addConst(at, mir::intConstant(word.type, 0));
                auto truth = mir::owned(mir::booleanType(program));

                more = editor.resultOf(*editor.addBinary(at, mir::BinaryOp::Ne,
                                                         editor.use(*number, UseKind::Inspect),
                                                         editor.use(*editor.resultOf(*zero),
                                                                    UseKind::Inspect),
                                                         truth, conventionFor(truth)));
            }

            /*
             * A **failing** child's "no" is two answers - C1b, and `resolveObjectFor`'s `stopped`
             * block one tier in. A failed step leaves the child's `$index` at `kIterFailed`, so the
             * index is read where the step said no and only there; `f$failure` moves the reason out
             * and finishes the child, and the reason is the one argument of this region's
             * `otherwise` edge, exactly as `convertSkip` hands it in the continuation-passing form.
             */
            if(region->otherwise.valid() && child.producerFailure) {
                auto stopped = editor.addBlock(StringId());
                editor.addBranch(at, editor.use(*more, UseKind::Inspect), *obtain, *stopped,
                                 nullptr, 0);

                editor.setBlock(*stopped);
                editor.setRegion(stopped->region);

                auto again = editor.resultOf(*editor.addLoad(at, editor.use(*stepField(at, field),
                                                                            UseKind::Inspect),
                                                             borrow, conventionFor(borrow), false));
                auto index = editor.resultOf(*editor.addFieldAddress(
                    at, editor.use(*again, UseKind::Inspect), childType, 0));

                auto counter = mir::owned(program.types.exitCode());
                auto loaded = editor.resultOf(*editor.addLoad(at, editor.use(*index, UseKind::Inspect),
                                                              counter, conventionFor(counter), false));
                auto sentinel = editor.addConst(at, mir::intConstant(counter.type, U64(kIterFailed)));

                auto truth = mir::owned(mir::booleanType(program));
                auto failed = editor.addBinary(at, mir::BinaryOp::Eq,
                                               editor.use(*loaded, UseKind::Inspect),
                                               editor.use(*editor.resultOf(*sentinel), UseKind::Inspect),
                                               truth, conventionFor(truth));

                auto failure = editor.addBlock(StringId());
                editor.addBranch(at, editor.use(*editor.resultOf(*failed), UseKind::Inspect), *failure,
                                 destination, nullptr, 0);

                editor.setBlock(*failure);
                editor.setRegion(failure->region);

                auto& take = *decl()[child.producerFailure];
                mir::CallSite taking;
                taking.target = child.producerFailure;
                taking.through = mir::CallThrough::Declaration;

                Use childSelf[1] = { editor.use(*again, UseKind::Borrow) };
                auto unit = decl()[take.result.type]->kind == mir::Type::Unit;
                auto called = editor.addCall(at, taking, Use(), childSelf, 1, take.result,
                                             conventionFor(take.result), !unit);
                auto reason = unit ? unitValue(at, take.result) : editor.resultOf(*called);

                auto handed = editor.use(*reason, UseKind::Consume);
                editor.addJump(at, *base()[blocks[region->otherwise.index]], &handed, 1);
            } else {
                editor.addBranch(at, editor.use(*more, UseKind::Inspect), *obtain, destination,
                                 nullptr, 0);
            }
        }

        editor.setBlock(*obtain);
        editor.setRegion(obtain->region);

        auto& entry = *base()[blocks[region->entry.index]];

        if(!entry.argumentCount()) {
            editor.addJump(at, entry, nullptr, 0);
            return true;
        }

        auto position = base()[entry.argumentAt(base(), 0)]->occurrence;
        auto item = found ? stepBoxItem(at, found, position, carried, foundLends)
                          : stepNestItem(at, field, child, position);
        if(!item) return true;

        UseKind kind;
        if(!useKindFor(mir::argumentMode(base()[entry.argumentAt(base(), 0)]->occurrence.contract,
                                         mir::SourceKind::Result), kind)) {
            report(at, "internal: an embedded region's hand-over has no mode"_v);
            return true;
        }

        Use handed[1] = { editor.use(*item, kind) };
        editor.addJump(at, entry, handed, 1);
        return true;
    }

    /*
     * What a **boxed** level handed over - §6.3, and the erased half of `stepNestItem` below.
     *
     * A concrete child hands its item over out of a field of a record this step has in hand; a box
     * answers an **address** and nothing else (`emitIterErasedStep`), which is where the item is
     * whatever the slot holds - the value for a copying producer, the value itself for a lending
     * one. So there are two rows rather than three: a position that is itself a reference takes the
     * address as it stands, and one that takes the value loads through it.
     *
     * The value stays the child's until its next step either way, which is the same extent §2.6
     * gives a concrete nest's item and is what the outer producer's own liveness is computed over.
     */
    Value* stepBoxItem(LocationId at, Value* found, const mir::Occurrence& position,
                       DeclPtr<mir::Type> carriedType, bool lends) {
        if(!found) {
            report(at, "internal: a boxed region's member hands over nothing"_v);
            return nullptr;
        }

        // What was answered is the child's slot, and a lending slot holds the value's address
        // (`iterSlotAddress`).
        if(lends) {
            auto through = mir::owned(found->occurrence.type);
            found = editor.resultOf(*editor.addLoad(at, editor.use(*found, UseKind::Inspect),
                                                    through, conventionFor(through), false));
        }

        if(referenceValued(position)) {
            return editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Represent,
                                                    editor.use(*found, UseKind::Inspect), position,
                                                    conventionFor(position)));
        }

        auto carried = mir::owned(carriedType);
        auto value = editor.resultOf(*editor.addLoad(at, editor.use(*found, UseKind::Inspect),
                                                     carried, conventionFor(carried), false));

        if(carried == position) return value;

        return editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Represent,
                                                editor.use(*value, UseKind::Inspect), position,
                                                conventionFor(position)));
    }

    /*
     * What the child handed over, out of its own slot and under its own convention - the same three
     * rows `resolveObjectFor` reads one tier up, read here because the region's entry is a block of
     * this body rather than a `for` the resolver built.
     *
     * A **lending** producer answers through `g$item`, whose signature roots the value in the child;
     * one with a `$held` flag answers through `g$take`, which moves the value out and clears the flag
     * in the same place; everything else is a copy of the slot. And where the position the region
     * declared is a **window**, the two words are derived from the owner by the same member
     * `emitIterStepper` calls - which is the one adaptation a hand-over can need at this boundary.
     */
    Value* stepNestItem(LocationId at, U16 field, mir::RecordType& child,
                        const mir::Occurrence& position) {
        auto held = stepping->record->field(decl(), field).occurrence.type;
        auto slot = stepField(at, field);

        auto item = child.field(decl(), 1).occurrence;
        Value* value = nullptr;
        mir::Occurrence carried;

        auto reach = [&](DeclPtr<mir::Function> target) {
            auto& called = *decl()[target];
            auto& declared = *decl()[called.args.get(decl(), 0)];

            auto self = editor.resultOf(*editor.addLoad(at, editor.use(*slot, UseKind::Inspect),
                                                        declared.occurrence,
                                                        conventionFor(declared.occurrence), false));

            mir::CallSite site;
            site.target = target;
            site.through = mir::CallThrough::Declaration;

            Use arguments[1] = { editor.use(*self, UseKind::Borrow) };
            carried = called.result;
            value = editor.resultOf(*editor.addCall(at, site, Use(), arguments, 1, called.result,
                                                    conventionFor(called.result), true));
        };

        if(child.producerItem) {
            reach(child.producerItem);
        } else if(child.producerTake) {
            reach(child.producerTake);
        } else {
            auto address = editor.resultOf(*editor.addFieldAddress(at, editor.use(*slot,
                                                                                  UseKind::Inspect),
                                                                   held, 1));

            /*
             * A **copying** child under a position that is a reference - `'Int` handed over by a
             * producer whose slot holds the `Int` (`objectLends` copies a trivially copyable item),
             * which is `items(xs)` at a scalar since J129 named its slot. What the position takes is
             * where the value is, and that is the slot: the same bytes `f$next` lends its consumer
             * out of a standalone record. Loading the slot and restating the loaded bytes as a
             * reference - the row below - handed the region an `Int` read as an address, which the
             * lowering refused as a pointer-typed immediate (J131). The address is the child's until
             * its next step, which is the extent §2.6 gives any nested item.
             *
             * Not a **window**, which `referenceValued` also answers yes to: a view position takes
             * the descriptor's own two words (`slotWindow`), which is the row below.
             */
            if(referenceValued(position) && position.contract.form() != mir::AccessForm::View &&
               !referenceValued(mir::owned(item.type))) {
                return editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Represent,
                                                        editor.use(*address, UseKind::Inspect),
                                                        position, conventionFor(position)));
            }

            // A lens handing over `{}` - `withLock(m):`, the common lens - has a slot of no bytes,
            // which is the value itself rather than a load (`unitValue`).
            carried = mir::owned(item.type);
            value = unitValue(at, carried);
            if(!value) {
                value = editor.resultOf(*editor.addLoad(at, editor.use(*address, UseKind::Inspect),
                                                        carried, conventionFor(carried), false));
            }
        }

        if(!value) {
            report(at, "internal: an embedded region's member hands over nothing"_v);
            return nullptr;
        }

        if(carried == position) return value;

        /*
         * The **window**, where the position takes one: a borrowed `String` is that string's
         * descriptor rather than its address, so what the region's entry receives is two words built
         * from the owner. `emitIterStepper` makes the same derivation at the same boundary and says
         * why a generated body has to name the member itself.
         */
        if(mir::needsViewDerivation(decl(), program, carried, position)) {
            auto target = mir::viewMember(context, program, position.type,
                                          position.contract.access(), at);

            if(!target || !decl()[target]->reachable) {
                report(at, "internal: no derivation is reachable for an embedded hand-over's window"_v);
                return nullptr;
            }

            mir::CallSite window;
            window.target = target;
            window.through = mir::CallThrough::Declaration;

            Use owner[1] = { editor.use(*value, UseKind::Borrow) };
            return editor.resultOf(*editor.addCall(at, window, Use(), owner, 1, position,
                                                   conventionFor(position), true));
        }

        /*
         * And otherwise the same bytes said as what the position declares - `represent`, which moves
         * none of them. A slot-rooted loan and a call-bounded one are one address and two statements,
         * and the block argument is held to its own occurrence where a call boundary would not be.
         */
        return editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Represent,
                                                editor.use(*value, UseKind::Inspect), position,
                                                conventionFor(position)));
    }

    /*
     * The three nodes a step reads differently - `StepPlan`, where the reason for each is written.
     *
     * Answers true where the node has been dealt with, so that the switch below is the ordinary
     * conversion with nothing added to it: everything between two hand-overs is the producer's own
     * code and is converted exactly as the continuation-passing form converts it.
     */
    bool convertStepNode(mir::Node& node) {
        if(stepping->readsContinuation[node.id.index]) return true;

        // An embedded region - a `for x in g(...)` or a lens over part of the producer's own body,
        // which is a call in the continuation-passing form and a **member** here. `convertStepNest`,
        // §3.2's first step.
        if(mir::isLensCall(node.kind) &&
           stepping->nestOf[((mir::NodeLensCall&)node).region.index] != StepPlan::kNoField) {
            return convertStepNest((mir::NodeLensCall&)node);
        }

        auto index = stepping->yieldOf[node.id.index];

        if(index != StepPlan::kNotAYield) {
            auto& call = (mir::NodeCall&)node;
            auto items = call.arguments.contents(mir);

            /*
             * The hand-over, into the slot - the value where the convention owns or copies, and the
             * lent value's **address** where it lends.
             *
             * The address and not the loan, and the difference is a manifestation one: a shared loan
             * of a small record *is* the record, scalarized into a register, so storing the loan
             * would put four bytes where the slot holds eight. What has to travel is where the value
             * lives, which is the place the `yield`'s argument was read from - a local of the
             * producer, and therefore a member of this record. The deleted stack handle's own
             * `yield` took the same address one tier up, with `addressOf` written in the source
             * (`../../doc/progress/2026-09-16-J55-iterator-objects.md`).
             */
            auto slotType = stepping->record->field(decl(), 1).occurrence.type;
            auto lends = decl()[slotType]->kind == mir::Type::Pointer;

            Use handed;
            auto ready = !items.size();

            if(items.size() && lends) {
                auto& argument = *items.pointerAt(0);

                if(argument.source() == mir::SourceKind::Result) {
                    auto& defined = *source.node(mir, argument.nodeId());

                    if(defined.kind == mir::Node::Read) {
                        auto place = source.place(mir, ((mir::NodeRead&)defined).place.placeId());

                        if(auto address = addressOf(*place, node.source)) {
                            handed = editor.use(*address, UseKind::Inspect);
                            ready = true;
                        }
                    }
                }

                if(!ready) {
                    report(node.source,
                           "internal: an object's lending hand-over is not a read of a place"_v);
                    return true;
                }
            } else if(items.size()) {
                if(!useOf(*items.pointerAt(0), node.source, handed)) return true;

                /*
                 * And the value at the **slot's** own reading, which is a load where what the
                 * producer built is a reference and the slot holds bytes.
                 *
                 * A call boundary makes this adaptation silently - the lowering has the parameter's
                 * declared occurrence and the argument's - and a store does not. `xs[i]` inside a
                 * *generic* producer is where it shows: `Index`'s member answers `source'a`, which is
                 * an address whatever `a` turns out to be, and the slot of a producer handing over a
                 * trivially copyable item holds the item. Storing the address there is what made
                 * `items(xs)` hand its consumer a pointer read as an `I32`.
                 */
                auto& built = *base()[handed.value()];
                auto stored = mir::owned(slotType);

                if(referenceValued(built.occurrence) && !referenceValued(stored)) {
                    auto loaded = editor.addLoad(node.source, editor.use(built, UseKind::Inspect),
                                                 stored, conventionFor(stored), false);
                    handed = editor.use(*editor.resultOf(*loaded), UseKind::Consume);
                }

                ready = true;
            }

            if(items.size() && ready) {
                // Whatever the last hand-over left in the slot, where the consumer did not take it -
                // `stepReleaseHeld`, which is a branch this returns from in the join.
                stepReleaseHeld(node.source);

                /*
                 * Not where the slot has no bytes: a `{}` hand-over is read back as the constant
                 * (`stepNestItem`), and a store of it is a write of nothing that the lowering has no
                 * width for and JSIR no value for (J185, a `lens fn` handing over `{}` inside an
                 * object).
                 */
                if(decl()[slotType]->kind != mir::Type::Unit) {
                    auto slot = stepField(node.source, 1);
                    editor.addStore(node.source, editor.use(*slot, UseKind::Inspect), handed);
                }

                if(stepping->record->heldField) stepStoreHeld(node.source, true);
            }

            /*
             * And whatever crosses this hand-over without a name, into its own member - the spill a
             * hand-written state machine makes, for the values `checkObjectBody` has already held to
             * being copies.
             */
            for(U32 j = 0; j < stepping->spillValue.size(); j++) {
                if(!stepping->record->keepsAcross(decl(), stepping->spillEntry[j], index)) continue;

                auto value = produced[stepping->spillValue[j]];
                if(!value) continue;

                auto address = stepField(node.source, U16(stepping->spillField[j]));
                editor.addStore(node.source, editor.use(*address, UseKind::Inspect),
                                editor.use(*base()[value], UseKind::Consume));
            }

            stepStoreIndex(node.source, I64(1 + index));
            stepReturn(node.source, true);

            /*
             * And the block the *next* step resumes at, which is where the rest of this MIR block
             * goes. What the `yield` produced is the step's own `$resume` argument - the value the
             * continuation answered, one tier over - so `resolveYield`'s test, `emitDeparture` and
             * the statements below the loops are the producer's code unchanged.
             */
            auto resume = editor.addBlock(StringId());
            stepping->resume[index] = resume - base();

            editor.setBlock(*resume);
            editor.setRegion(resume->region);

            // The spilled values back out, replacing what the conversion holds for them. Every block
            // emitted from here on reads the reload, which is what `checkObjectBody`'s ordering rule
            // is about.
            for(U32 j = 0; j < stepping->spillValue.size(); j++) {
                if(!stepping->record->keepsAcross(decl(), stepping->spillEntry[j], index)) continue;
                if(!produced[stepping->spillValue[j]]) continue;

                auto held = mir::owned(stepping->record->field(decl(),
                                       U16(stepping->spillField[j])).occurrence.type);
                auto address = stepField(node.source, U16(stepping->spillField[j]));
                auto loaded = editor.addLoad(node.source, editor.use(*address, UseKind::Inspect),
                                             held, conventionFor(held), false);

                produced[stepping->spillValue[j]] = editor.resultOf(*loaded) - base();
                stepping->spillReloaded[Size(j) * stepping->yields + index] =
                    produced[stepping->spillValue[j]];
            }

            produced[node.id.index] = stepping->resumeValue - base();
            return true;
        }

        if(node.kind == mir::Node::Return) {
            // The producer is done - by exhaustion or by the departure a cancellation takes - so
            // whatever the slot still holds is the record's last obligation, and so are its runs and
            // whatever a nested traversal is still suspended in the middle of.
            stepReleaseHeld(node.source);
            stepCancelNests(node.source);
            stepReleaseRuns(node.source);

            auto& written = (mir::NodeReturn&)node;
            if(stepping->record->failureField && written.hasValue) {
                return stepFail(node.source, written);
            }

            stepStoreIndex(node.source, I64(kIterFinished));
            stepReturn(node.source, false);
            return true;
        }

        return false;
    }

    void convertNode(mir::Node& node) {
        // The three things a step reads differently - `convertStepNode`, which answers true where it
        // has emitted them itself.
        if(stepping && convertStepNode(node)) return;

        switch(node.kind) {
            case mir::Node::Const: {
                auto& written = (mir::NodeConst&)node;
                auto constant = source.constant(mir, written.constant);

                /*
                 * A `{}` is absent here for the same reason a `{}` call result is - see the Call
                 * arm below, and the README section this and it are the two halves of.
                 *
                 * Nothing can name it: `Builder::resultOf` answers the constant itself for a node
                 * whose result is a `{}`, so a use of one arrives as an operand and is minted at
                 * the position that wants it. Producing a value here as well would leave a dead
                 * `imm` behind in every body that mentions `{}`, and this stage runs no pass that
                 * would remove it.
                 */
                if(constant.kind == mir::Constant::Unit) break;

                /*
                 * The constant's own **type** is substituted, exactly as every other type on this
                 * walk is. `zero()` at a generic lane produces `const 0 : a`, and a constant left at
                 * the generic's variable is a value whose ownership class is the conservative one a
                 * variable has - which is what `Editor::addConst`'s "a constant is trivial by
                 * construction" asserts against. The number is what it was; the width it is read at
                 * is the specialization's.
                 */
                auto constantType = substituted(constant.type);

                if(constantType != constant.type) {
                    auto moved = constant;
                    moved.type = constantType;
                    produced[node.id.index] =
                        editor.resultOf(*editor.addConst(node.source, moved)) - base();
                    break;
                }

                produced[node.id.index] =
                    editor.resultOf(*editor.addConst(node.source, constant)) - base();
                break;
            }
            /*
             * A const parameter read as a value - `mir::NodeGenValue`.
             *
             * The substitution is what turns the parameter into the number, which is the same move
             * every type in this walk makes and is why the node carries no value of its own. A
             * position still open after substituting is a *generic* body being converted, which
             * needs a const slot to load from - G5's work, and refused by name until then rather
             * than lowered to whatever the parameter's default happens to be.
             */
            case mir::Node::GenValue: {
                auto& written = (mir::NodeGenValue&)node;
                auto bound = substituted(written.variable);

                /*
                 * No substitution to turn it into a number, which is a body compiled once: what the
                 * parameter holds is in the const slot the schema numbers for it, and the
                 * instruction names the parameter for the lowering to find the slot.
                 */
                if(bound && decl()[bound]->kind == mir::Type::Variable) {
                    auto counts = ((mir::VariableType*)decl()[bound])->constType;
                    produced[node.id.index] =
                        editor.resultOf(*editor.addGenValue(node.source, bound, counts)) - base();
                    break;
                }

                if(!bound || decl()[bound]->kind != mir::Type::Const) {
                    context.diagnostics.error(
                        "internal: a const parameter was neither bound to a number nor left open"_v,
                        node.source);
                    return;
                }

                auto& value = *(mir::ConstType*)decl()[bound];

                /*
                 * A count that is still the *expression* - `Vec(a)`'s natural lane count with `a`
                 * erased. It has no const slot to read, because nothing declared it as a parameter,
                 * so this is the erased half of G5's gap and is refused by name rather than answered
                 * with the zero the expression carries where its number would be.
                 */
                if(value.isDerived()) {
                    context.diagnostics.error(
                        "a vector's natural lane count is not available in an erased body yet - the type it is the count of has no layout here"_v,
                        node.source);
                    return;
                }
                produced[node.id.index] =
                    editor.resultOf(*editor.addConst(node.source,
                                                     mir::intConstant(value.type, value.value)))
                    - base();
                break;
            }
            case mir::Node::Read: {
                auto& written = (mir::NodeRead&)node;

                /*
                 * A type with **one value** is that value, and never a read of the storage it was
                 * given - `Resolver::readPlace`'s rule, restated here because this is the first tier
                 * that can see it.
                 *
                 * MIR asks it of the written type and answers it there; a *generic* body reads a
                 * payload of type `a` and only a specialization at `a := {}` makes it this. `toOutcome`
                 * at `Result(e, {})` is the one in the library - `?` on a function that answers
                 * nothing - and what it produced was a load stating a width of zero, which
                 * `makeMemoryFlags` has no encoding for and `lower/inst.cpp` asserts against by name.
                 */
                auto held = substituted(mir::valueOccurrence(node));
                if(auto unit = unitValue(node.source, held)) {
                    produced[node.id.index] = unit - base();
                    break;
                }

                auto place = source.place(mir, written.place.placeId());
                auto address = addressOf(*place, node.source);
                if(!address) return;

                /*
                 * The convention the result carries.
                 *
                 * Taken from the *read's own occurrence*, which the MIR builder decided from the
                 * mode - so a trivially copied scalar is `Trivial`, a taken owner is `Owned` and a
                 * borrow is `Guaranteed`, and this stage neither re-derives that nor is entitled to a
                 * second opinion about it.
                 */
                auto result = substituted(mir::valueOccurrence(node));

                // A shared read of a register-valued trivially copyable place is that value - see
                // `valueOccurrenceOf`, which is `readMode`'s own first clause re-asked with the
                // substitution in hand. `attachLoan` then makes no loan for it, having none to make.
                if(written.place.mode() == mir::UseMode::BorrowShared) {
                    result = valueOccurrenceOf(result);
                }

                auto used = addressUse(*address);

                /*
                 * The storage this read reaches, which under substitution can be the **owner**
                 * where the read's own occurrence is the view - `deriveView`.
                 *
                 * `->key: k` at `k := String` is an owned local of three words, and a shared read of
                 * it is the view by `substitutedContract`. The two are not the same bytes, and
                 * nothing between here and the machine would say so: the load answers the owner and
                 * the derivation makes the view out of it, exactly as the resolver does where the
                 * type was concrete. A field of a record holding the owner is the same statement one
                 * projection down, and reaches it through the same place.
                 */
                auto derives = mir::needsViewDerivation(decl(), program, storageAt(*place), result);
                auto view = result;

                if(derives) {
                    result.contract = mir::Contract { result.contract.access(),
                                                      result.contract.origin(),
                                                      mir::AccessForm::WholeValue,
                                                      result.contract.slot() };
                }

                /*
                 * A duplicate, where duplicating is an operation - see `InstCopy`.
                 *
                 * `CopyTrivial` says a second owner is made, and for a value carried in a register a
                 * load already is one: the bytes arrive in a register that nothing else names. For a
                 * value that lives in storage it is not, and handing a callee the caller's own record
                 * where a copy was meant is the miscompile this line exists to prevent. Which of the
                 * two a value is is the manifestation's answer and is asked here, once.
                 *
                 * **And a relocation out of a raw address**, which is the same duplication for the
                 * other reason. A move out of a *local* leaves the address alone because nothing
                 * may read the source again - that is what the move said - and a move out of a
                 * place rooted in a pointer says no such thing: the storage is outside the
                 * ownership model, so a later `store` through the same pointer is an ordinary
                 * operation and the "moved" value would follow it. `let ->doomed = *(items + at)`
                 * followed by `store(items + at, moved)` is `lib/Core/Map.native.yana`'s
                 * swap-remove, and read as an alias it answers the entry that was moved *into* the
                 * gap. `compiler/resolve` (deleted in J173) always copied here - `InstMove` out of
                 * a pointer place was bytes - and this is that sentence at this tier.
                 *
                 * At a **trivially copyable** result and no other, which is what an `InstCopy` means:
                 * a value that owes a teardown cannot be duplicated, and a relocation of one out of
                 * a raw address is a hand-over whose destination write is what moves the bytes.
                 */
                auto mode = written.place.mode();
                auto relocates = mode == mir::UseMode::Take || mode == mir::UseMode::Forward;

                auto outside = [&] {
                    return mir::rootPlace(mir, *place).kind == mir::Place::Address;
                };

                auto copies = holdsAggregate(decl(), result.type) &&
                              (mode == mir::UseMode::CopyTrivial ||
                               (relocates && mir::isTriviallyCopyableAt(decl(), result) &&
                                outside()));

                /*
                 * The overread flag rides along, and only a load can carry it: a `Copy` is a
                 * duplication of storage the caller already has, and nothing in the language
                 * duplicates a place it is reading past the end of.
                 */
                auto inst = copies
                    ? editor.addCopy(node.source, used, result, conventionFor(result))
                    : editor.addLoad(node.source, used, result, conventionFor(result),
                                     written.overread, written.isVolatile);

                auto answer = editor.resultOf(*inst);

                if(derives) {
                    // The owner is rooted in the same storage the view will be, and `attachLoan`
                    // runs once per node on whatever `produced` holds - so the load's own loan is
                    // attached here rather than being left at the permissive default.
                    attachLoan(node, *answer);

                    answer = deriveView(*answer, view, node.source);
                    if(!answer) return;
                }

                produced[node.id.index] = answer - base();
                break;
            }
            case mir::Node::Argument:
                // Made when its block was, before anything could name it - see `run`. Nothing to do
                // here, and nothing to skip either: the entry is already filled.
                break;
            case mir::Node::Binary: {
                auto& written = (mir::NodeBinary&)node;
                Use lhs, rhs;
                if(!valueUseOf(written.lhs, node.source, lhs)) return;
                if(!valueUseOf(written.rhs, node.source, rhs)) return;

                auto result = substituted(mir::valueOccurrence(node));
                auto inst = editor.addBinary(node.source, written.op, lhs, rhs, result,
                                             conventionFor(result));

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            case mir::Node::Unary: {
                auto& written = (mir::NodeUnary&)node;
                Use value;
                if(!valueUseOf(written.value, node.source, value)) return;

                auto result = substituted(mir::valueOccurrence(node));
                if(!requireFloatOperand(mir::unaryCategory(written.op), result.type, node.source,
                                        mir::unaryOpName(written.op))) {
                    return;
                }

                auto inst = editor.addUnary(node.source, written.op, value, result,
                                            conventionFor(result));

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            case mir::Node::Aggregate: {
                auto& written = (mir::NodeAggregate&)node;

                auto result = substituted(mir::valueOccurrence(node));

                /*
                 * A record's fields are positions exactly as a call's parameters are, and they
                 * have the same manifestation mismatch `valueUseOf` adapts (J141):
                 * `P {x: firstOf(xs)}` is a loan of a scalar - an address - into a field that holds
                 * the number, which reached the verifier as an aggregate consuming a guaranteed
                 * value. The instantiation's own fields are the substituted positions, so a generic
                 * record is asked at the type it was built at.
                 *
                 * The other direction - `Held {value: 5}`, a number into a field declared `'a` at
                 * `a := I32`, which holds an *address* - used to be `referenceUseOf` here, and a
                 * frame slot made after the loan check was a slot nothing kept the record from
                 * outliving (J147: returned, it read a dead frame; pushed in a loop, every element
                 * pointed at one slot). The resolver gives such a value its temporary now
                 * (`Resolver::lentPastPosition`), so the component arrives as a loan of it and a
                 * reference field is a plain use.
                 */
                auto fields = [&]() -> mir::RecordType* {
                    if(written.fill || !result.type) return nullptr;
                    if(decl()[result.type]->kind != mir::Type::Record) return nullptr;

                    auto record = (mir::RecordType*)decl()[result.type];
                    if(record->lambda || written.constructor >= record->cons.size()) return nullptr;

                    auto con = record->cons.get(decl(), written.constructor);
                    return con.count == written.components.size() ? record : nullptr;
                }();

                SmallArray<Use, 8> components;
                for(auto operand: written.components.contents(mir)) {
                    Use used;
                    if(fields) {
                        auto con = fields->cons.get(decl(), written.constructor);
                        auto field = fields->field(decl(), U16(con.first + components.size()));

                        /*
                         * Only a position the *declaration* settled. A field declared at a bare
                         * variable and qualified by the application - `Pair('I32, 'I32)`,
                         * `Entry('k, 'v)` - is a reference because of the result's shape and not
                         * its own occurrence, and its component already arrives as one; so an
                         * owned field is adapted only where the result carries no shape at all.
                         */
                        auto adapts = mir::ownsField(field) && !result.shape
                            ? valueUseOf(operand, node.source, used)
                            : useOf(operand, node.source, used);
                        if(!adapts) return;
                    } else if(!useOf(operand, node.source, used)) {
                        return;
                    }

                    components.push(used);
                }

                /*
                 * `[0] :: [U8 *n]` - the fill whose count arrives with the substitution.
                 *
                 * Spread here and nowhere earlier, for `NodeAggregate::fill`'s reason: this is the
                 * first point at which `n` is a number. The one operand is repeated rather than
                 * re-minted, which is what the resolver's own spreading does at a written count -
                 * an element of a fill is `TrivialCopy` by the rule `arrayLiteralShape` applies, so
                 * one value in every slot is one value copied and not one object shared.
                 */
                if(written.fill) {
                    auto& array = *(mir::ArrayType*)decl()[result.type];
                    if(!array.hasFixedLength()) {
                        report(node.source,
                               "internal: a fixed array's length is still a const parameter at the SSA conversion"_v);
                        return;
                    }

                    while(components.size() < Size(array.length)) components.push(components[0]);
                }
                auto inst = editor.addAggregate(node.source, written.constructor,
                                                components.pointer(), components.size(), result,
                                                conventionFor(result));

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }


            /*
             * `addressOf(x)` - the raw address of storage a loan names.
             *
             * A **bitcast**, and this is the tier entitled to say so: a loan that named an origin
             * manifests as its address (`isReferenceValue`), so the value that arrived already holds
             * exactly the bits the pointer holds and what is left is the change of type. Deriving
             * that here rather than in MIR is the same division `field.addr` is under - the place
             * says which storage and the manifestation says what a value of it physically is.
             *
             * The check is not decoration. A loan of a trivially copyable scalar that named *no*
             * origin is the scalar itself in a register, so a bitcast of one would hand back a
             * number as an address; `addressOf`'s declaration publishes an origin precisely so that
             * cannot happen, and this is where that declaration is relied on rather than assumed.
             */
            case mir::Node::Address: {
                auto& written = (mir::NodeAddress&)node;
                Use of;
                if(!useOf(written.of, node.source, of)) return;

                auto& held = base()[of.value()]->occurrence;
                if(!reachedAsAddress(held)) {
                    report(node.source,
                           "internal: the address of a value that is not reached through storage"_v);
                    return;
                }

                auto result = substituted(mir::valueOccurrence(node));
                auto inst = editor.addUnary(node.source, mir::UnaryOp::Bitcast, of, result,
                                            conventionFor(result));

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }

            // A measurement of a type, carried down unchanged: the answer is the *lowering's*,
            // because at a type parameter it is a descriptor read rather than a number.
            case mir::Node::Metric: {
                auto& written = (mir::NodeMetric&)node;
                auto result = substituted(mir::valueOccurrence(node));
                auto inst = editor.addMetric(node.source, written.metric, substituted(written.of),
                                             result.type);

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }

            /*
             * A run's slots - one instruction down to one instruction, which is the whole of what
             * this node was made for.
             *
             * The element is substituted, because a literal in a generic body asks for slots of
             * whatever the instantiation says `a` is; the count is a number the resolver chose and
             * is carried through unchanged.
             */
            case mir::Node::RunSlots: {
                auto& written = (mir::NodeRunSlots&)node;
                auto inst = editor.addRunSlots(node.source, substituted(written.element),
                                               written.capacity);

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }

            case mir::Node::FunValue: {
                auto& written = (mir::NodeFunValue&)node;
                auto result = substituted(mir::valueOccurrence(node));

                // The environment, where the lambda captured anything. A borrow of a local of this
                // frame, so it converts like any other operand and arrives here as its address.
                Use environment;
                if(written.capturing && !useOf(written.environment, node.source, environment)) {
                    return;
                }

                // The lambda's specialization where it was lifted in a generic body - the one
                // reachability made, under the same substitution (`funValueTarget`).
                auto callee = mir::funValueTarget(context, program, function, written.callee,
                                                  node.source);
                auto inst = editor.addFunValue(node.source, callee,
                                               written.capturing ? &environment : nullptr, result,
                                               conventionFor(result));

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            /*
             * An operation the machine has and this IR does not - carried down with nothing added.
             *
             * The `{}` answer is handled the way a call's is and for the same reason: a value of no
             * bytes has no register to be in, so it is absent rather than present-and-unmapped.
             */
            case mir::Node::Native: {
                auto& written = (mir::NodeNative&)node;

                SmallArray<Use, 8> arguments;
                for(auto operand: written.arguments.contents(mir)) {
                    Use used;
                    if(!valueUseOf(operand, node.source, used)) return;
                    arguments.push(used);
                }

                auto result = substituted(mir::valueOccurrence(node));
                auto hasResult = decl()[result.type]->kind != mir::Type::Unit;

                auto inst = editor.addNative(node.source, written.op, arguments.pointer(),
                                             arguments.size(), result, conventionFor(result),
                                             hasResult, written.immediate);

                if(hasResult) produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            /*
             * And the host's - carried down with nothing added either, which is what "a node that
             * passes through" means (Analysis-IR-JS.md §3.10).
             *
             * `valueUseOf` and not `useOf`, exactly as the arm above uses it: an operand whose
             * occurrence is a reference to something this tier holds as a value needs the load in
             * front of it, and a host operation's receiver is as likely to be one as a system
             * call's address is.
             */
            case mir::Node::Host: {
                auto& written = (mir::NodeHost&)node;

                SmallArray<Use, 8> arguments;
                for(auto operand: written.arguments.contents(mir)) {
                    Use used;
                    if(!valueUseOf(operand, node.source, used)) return;
                    arguments.push(used);
                }

                auto result = substituted(mir::valueOccurrence(node));
                auto hasResult = decl()[result.type]->kind != mir::Type::Unit;

                auto inst = editor.addHost(node.source, written.op, written.member,
                                           arguments.pointer(), arguments.size(), result,
                                           conventionFor(result), hasResult);

                if(hasResult) produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            // A fused multiply-add and a branch-free choice - three operands and one result each,
            // and nothing this tier decides beyond the modes the MIR node gave them.
            case mir::Node::Fma: {
                auto& written = (mir::NodeFma&)node;

                Use lhs, rhs, addend;
                if(!useOf(written.lhs, node.source, lhs)) return;
                if(!useOf(written.rhs, node.source, rhs)) return;
                if(!useOf(written.addend, node.source, addend)) return;

                auto result = substituted(mir::valueOccurrence(node));
                if(!requireFloatOperand(mir::UnaryCategory::Float, result.type, node.source,
                                        "fma"_v)) {
                    return;
                }

                auto inst = editor.addFma(node.source, lhs, rhs, addend, result);

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            case mir::Node::Select: {
                auto& written = (mir::NodeSelect&)node;

                Use condition, whenTrue, whenFalse;
                if(!useOf(written.condition, node.source, condition)) return;
                if(!useOf(written.whenTrue, node.source, whenTrue)) return;
                if(!useOf(written.whenFalse, node.source, whenFalse)) return;

                auto result = substituted(mir::valueOccurrence(node));
                auto inst = editor.addSelect(node.source, condition, whenTrue, whenFalse, result,
                                             conventionFor(result));

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }

            /*
             * A `match` over constants - the selector carried down and the table carried with it.
             *
             * Nothing is decided here: which shape the mapping becomes is the lowering's question,
             * and what this tier owes is the ownership of the result. That is `conventionFor` on the
             * occurrence the recognizer stated, exactly as a `const` of the same type gets.
             */
            case mir::Node::Map: {
                auto& written = (mir::NodeMap&)node;

                Use selector;
                if(!useOf(written.selector, node.source, selector)) return;

                auto result = substituted(mir::valueOccurrence(node));
                auto entries = written.entries.contents(mir);

                /*
                 * Each entry's constant carries its own type, substituted the way `Const`'s is: a
                 * recognizer inside a generic body may state `a` and the specialization is what
                 * says which width the number is read at.
                 */
                SmallArray<mir::MapEntry, 16> copied;
                for(auto entry: entries) {
                    entry.value.type = substituted(entry.value.type);
                    copied.push(entry);
                }

                auto fallback = written.fallback;
                if(fallback.isSet()) fallback.type = substituted(fallback.type);

                auto inst = editor.addMap(node.source, selector, copied.pointer(), copied.size(),
                                          fallback, written.closed, result,
                                          conventionFor(result));

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }

            /*
             * The four vector operations - the operands carried down and the fields carried with
             * them. Nothing is decided here: what a lane index may be and what a pattern may name
             * were settled where the call was written, and this tier's business is ownership.
             */
            case mir::Node::VecSplat: {
                auto& written = (mir::NodeVecSplat&)node;

                Use from;
                if(!useOf(written.from, node.source, from)) return;

                auto result = substituted(mir::valueOccurrence(node));
                auto inst = editor.addVecSplat(node.source, from, result);

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            case mir::Node::VecLane:
            case mir::Node::VecWithLane: {
                auto& written = (mir::NodeVecLane&)node;

                Use from;
                if(!useOf(written.from, node.source, from)) return;

                auto result = substituted(mir::valueOccurrence(node));

                Inst* inst = nullptr;
                if(written.writesLane()) {
                    Use value;
                    if(!useOf(written.value, node.source, value)) return;

                    inst = editor.addVecWithLane(node.source, from, written.lane, value, result);
                } else {
                    inst = editor.addVecLane(node.source, from, written.lane, result);
                }

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            case mir::Node::VecShuffle: {
                auto& written = (mir::NodeVecShuffle&)node;

                Use left, right;
                if(!useOf(written.left, node.source, left)) return;
                if(!useOf(written.right, node.source, right)) return;

                // Copied into this region rather than pointed at: nothing below the conversion may
                // hold an MIR handle, and the MIR arena is released whole once it has run.
                SmallArray<U8, 16> pattern;
                for(auto lane: written.pattern.contents(mir)) pattern.push(lane);

                auto result = substituted(mir::valueOccurrence(node));

                // A rearrangement the body could not count - `NodeVecShuffle::Deferred`, written
                // now that the substitution has made the count a number.
                if(written.deferred != mir::NodeVecShuffle::Deferred::None) {
                    auto& vector = *(mir::VectorType*)decl()[result.type];
                    auto lanes = vector.hasFixedCount() ? vector.lanes : 0;
                    if(!lanes) {
                        report(node.source, "internal: a vector's lane count is still open at the SSA conversion"_v);
                        return;
                    }

                    if(!mir::deferredLanePattern(written.deferred, lanes, written.amount,
                                                 [&](U8 lane) { pattern.push(lane); })) {
                        errors++;
                        context.diagnostics.error("a window of %@ lanes starting at %@ runs past the end of the pair - the start is between 0 and %@"_v,
                                                  node.source, U32(lanes), I32(written.amount), U32(lanes));
                        return;
                    }
                }
                auto inst = editor.addVecShuffle(node.source, left, right, pattern.pointer(),
                                                 pattern.size(), result);

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            case mir::Node::VecReduce: {
                auto& written = (mir::NodeVecReduce&)node;

                Use from;
                if(!useOf(written.from, node.source, from)) return;

                auto result = substituted(mir::valueOccurrence(node));
                auto inst = editor.addVecReduce(node.source, from, written.reduce, result);

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            /*
             * `iota` at a count only this conversion knows - inst.def's `VecIota`.
             *
             * The chain the concrete emitter builds, built here instead: a zero splat with lane `i`
             * written as `i`. It is not a second definition of what `iota` means - it is the *same*
             * definition run at the first point the count is a number, which for a body compiled
             * once is here and nowhere earlier. Every value in it is a constant, so the folder
             * answers the whole chain and what reaches a backend is the finished vector, exactly as
             * on the concrete path.
             */
            case mir::Node::VecIota: {
                auto result = substituted(mir::valueOccurrence(node));
                auto& vector = *(mir::VectorType*)decl()[result.type];
                auto lanes = vector.hasFixedCount() ? vector.lanes : 0;

                if(!lanes) {
                    report(node.source, "internal: a vector's lane count is still open at the SSA conversion"_v);
                    return;
                }

                auto laneConstant = [&](U64 index) {
                    return decl()[vector.lane]->kind == mir::Type::Float
                        ? mir::floatConstant(vector.lane, double(index))
                        : mir::intConstant(vector.lane, index);
                };

                auto zero = editor.addConst(node.source, laneConstant(0));
                auto value = editor.addVecSplat(node.source,
                                                editor.use(*editor.resultOf(*zero), UseKind::Inspect),
                                                result);

                for(U32 i = 1; i < lanes; i++) {
                    auto number = editor.addConst(node.source, laneConstant(i));
                    value = editor.addVecWithLane(
                        node.source, editor.use(*editor.resultOf(*value), UseKind::Inspect), U16(i),
                        editor.use(*editor.resultOf(*number), UseKind::Inspect), result);
                }

                produced[node.id.index] = editor.resultOf(*value) - base();
                break;
            }
            /*
             * An atomic operation - the operands carried down under the modes the MIR node gave
             * them, and the order carried with them.
             *
             * Nothing is decided here, which is the point: what a location is, which shape the
             * operands are in and how strongly the operation orders were all settled where the
             * declaration was read, and this tier's business is ownership. The one thing it adds is
             * the same one every other producer gets - whether the result may be named at all,
             * which for a store and a fence is no.
             */
            case mir::Node::Atomic: {
                auto& written = (mir::NodeAtomic&)node;

                SmallArray<Use, 4> arguments;
                for(auto operand: written.arguments.contents(mir)) {
                    Use used;
                    if(!useOf(operand, node.source, used)) return;
                    arguments.push(used);
                }

                auto result = substituted(mir::valueOccurrence(node));
                auto hasResult = decl()[result.type]->kind != mir::Type::Unit;

                auto inst = editor.addAtomic(node.source, written.op, written.order, written.failure,
                                             written.weak, arguments.pointer(), arguments.size(),
                                             result, conventionFor(result), hasResult,
                                             written.isVolatile);

                if(hasResult) produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            case mir::Node::AtomicOk: {
                auto& written = (mir::NodeAtomicOk&)node;

                Use cas;
                if(!useOf(written.from, node.source, cas)) return;

                auto result = substituted(mir::valueOccurrence(node));
                auto inst = editor.addAtomicOk(node.source, cas, result);

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            case mir::Node::Call: {
                auto& written = (mir::NodeCall&)node;

                // The callee, where the call goes through a value. Read before the arguments for
                // the reason the operand walk publishes it first - see `NodeCall::eachOperand`.
                Use target;
                if(written.callsValue() && !useOf(written.target, node.source, target)) return;

                /*
                 * Which function this call reaches, asked of `callTarget` and of nothing else.
                 *
                 * A call to a generic names the generic and its type arguments in *this* body's
                 * terms, so the callee is a question about the substitution the body is being
                 * converted under - see `specialize.h`. Reachability asked the same question with
                 * the same function, which is what makes the target this names one that was
                 * discovered and summarized rather than one invented here.
                 *
                 * Asked before the arguments rather than after them, because the *position* is what
                 * says whether an argument is a reference or a value - `valueUseOf` below.
                 */
                mir::CallSite site;
                mir::callTarget(context, program, function, mir::callRequestOf(written), site);

                auto declared = site.through == mir::CallThrough::Declaration && site.target
                    ? decl()[site.target] : nullptr;

                /*
                 * And the positions of a call through a **value**, which are the target's own
                 * function type rather than a declaration.
                 *
                 * The same rule applied to the other half of the boundary: a `yield` is a call
                 * through the continuation, and `items`' `yield chunk[i]` supplies a subscript's
                 * published loan - an address - to a position declared `a`, which at a scalar holds
                 * the number. `verify.cpp`'s `checkValueCall` compares exactly those two, so the
                 * adaptation belongs here for the same reason it belongs at a declared call.
                 */
                mir::FunType* through = nullptr;

                /*
                 * A **derivation applied to the view it makes**, which is that view.
                 *
                 * `fn (Writable(c, a)) sort(&xs: c)` writes `elementsMut(xs)` because at `c :=
                 * [a *n]` the parameter is the storage and the window has to be built. At `c :=
                 * Array(a)` the same parameter already *is* the window - a unique variable position
                 * bound to a type with a view is the view - so the call asks a window to become one,
                 * which is the recursion `Resolver::adaptToView` refuses inside the derivation's own
                 * body and is here an identity. Folded rather than called: the member reads the
                 * owner's fields and a window has none of them.
                 *
                 * Only a derivation's subject. Every other `Owner` position is a type written by
                 * name - `xs: Array(Int)`, which asks for the container itself - and a window there
                 * is a program error that `refuseViewForOwner` reports.
                 */
                auto derivation = site.through == mir::CallThrough::Declaration && site.target &&
                                  decl()[site.target]->derivation;

                if(written.callsValue() && target.isSet()) {
                    auto value = base()[target.value()];

                    if(value->type() && decl()[value->type()]->kind == mir::Type::Fun) {
                        through = (mir::FunType*)decl()[value->type()];
                    }
                }

                SmallArray<Use, 8> arguments;
                U32 position = 0;

                /*
                 * A call through a **site type**: the environment, as argument zero - `CallSite::
                 * siteCall`, and the one place the two tiers' argument lists differ.
                 *
                 * MIR holds the value separately because *which* form the call is, is the
                 * substitution's answer and a node cannot hold two argument lists; by here the
                 * answer is in, so what is emitted is an ordinary direct call and every pass below
                 * this line - the verifier, the optimizer, the lowering - sees one. `position`
                 * starts at one for the same reason: the positions below are the callee's own, and
                 * the environment has filled the first of them.
                 *
                 * Nothing is adapted on the way in, and nothing has to be: the MIR read already
                 * asked for what the environment parameter declares (`resolveValueCall` hands
                 * `holdBorrowed` that parameter's own request), so the operand is a borrow of the
                 * record - or, where the record is trivially copyable, the copy `readMode` answers
                 * a shared request with, which the lowering places exactly as it places any other
                 * value at a position that takes an address.
                 *
                 * A call the **`Call` constraint** typed leads with the same operand for a different
                 * reason - `CallThrough::CallWitness`, §4. There the callee is a cell the caller
                 * filled and the value is what the cell's entry is *about*, so it cannot ride in the
                 * environment word: that word is where an erased callee's own generic environment
                 * goes. Both forms therefore put it in the ordinary list and pass no target, which is
                 * what `valueLeads` says.
                 */
                auto valueLeads = site.siteCall ||
                                  site.through == mir::CallThrough::CallWitness;

                auto& named = *decl()[written.callee];
                auto opensSite = !written.callsValue() && site.target != written.callee &&
                                 named.generated == mir::Function::Generated::IterNew &&
                                 named.generatedRecord &&
                                 ((mir::RecordType*)decl()[named.generatedRecord])->openSite;

                if(valueLeads && target.isSet()) {
                    arguments.push(target);
                    position = 1;
                }

                for(auto operand: written.arguments.contents(mir)) {
                    /*
                     * A borrowed **scalar** at a position that holds the value - `valueUseOf`, and
                     * the same manifestation mismatch an arithmetic operand has.
                     *
                     * `fn (Index(c, k, v), Num(v)) atBoth(...) = xs[a] + xs[b]` specialized at `I32`
                     * is the program: the subscript answers a loan with a published origin, which is
                     * an address, and `I32.+` takes two registers. The lowering already spills the
                     * other way round - a register into the frame slot an erased position wants -
                     * and this is that adaptation's other half, made here so the value the optimizer
                     * sees is the number and not the pointer.
                     *
                     * Only where the position is in hand and concrete - a declaration's parameter,
                     * or a function value's own parameter type. A call through a *slot* is checked
                     * against one signature both ends read, and an erased position takes an address
                     * whatever it was bound to.
                     */
                    auto held = declared && position < declared->args.size()
                        ? &decl()[declared->args.get(decl(), position)]->occurrence : nullptr;

                    mir::Occurrence valuePosition;
                    if(!held && through && position < through->params.size()) {
                        valuePosition = through->params.get(decl(), position).occurrence;
                        held = &valuePosition;
                    }

                    Use used;
                    auto adapts = held && !reachedAsAddress(*held)
                        ? valueUseOf(operand, node.source, used)
                        : held ? referenceUseOf(operand, *held, node.source, used)
                               : useOf(operand, node.source, used);

                    if(!adapts) return;

                    // And the owner-to-view adaptation, at the same position and for the same
                    // reason the two above are here: the substitution is known here and was not
                    // where the generic body was resolved. See `deriveView`. A derivation's own
                    // subject is left alone - the fold below is what answers it.
                    if(held && !(derivation && position == 0) &&
                       !adaptUseToView(used, *held, node.source)) {
                        return;
                    }

                    /*
                     * A constructor of an object a generic body built at its own variables (P16),
                     * which the node names in the body's terms and `callTarget` replaced with the
                     * record minted here. The body's record borrows a capture whose type was a
                     * variable, and this one may hold the same argument **by value** - a trivially
                     * copyable one is copied into the record (`IterCaptureKind::Value`). The value
                     * is already loaded above; handing it over is the copy, on the terms an embedded
                     * region's capture is adapted to `g$new` (`convertStepNest`).
                     */
                    if(opensSite && held && held->contract.isOwned() &&
                       used.kind() == UseKind::Borrow &&
                       !referenceValued(base()[used.value()]->occurrence)) {
                        used = editor.use(*base()[used.value()], UseKind::Consume);
                    }

                    arguments.push(used);
                    position++;
                }

                /*
                 * The result, where there is one to have.
                 *
                 * A `{}` answer manifests as no bytes, so there is no register for a value to be in
                 * and nothing below could map one. Absent rather than present-and-unmapped: an
                 * operand naming it then fails here, with a source position, instead of lowering to
                 * a null the machine walk quietly drops.
                 */
                auto result = substituted(mir::valueOccurrence(node));
                auto hasResult = decl()[result.type]->kind != mir::Type::Unit;

                // The derivation's identity - see the note where `derivation` is set. The subject is
                // the answer, and no instruction is emitted for it.
                if(derivation && arguments.size() == 1 && hasResult) {
                    auto subject = base()[arguments[0].value()];
                    auto& occurrence = subject->occurrence;

                    if(occurrence.contract.form() == mir::AccessForm::View &&
                       occurrence.contract.access() == result.contract.access() &&
                       mir::sameType(occurrence.type, result.type)) {
                        produced[node.id.index] = subject - base();
                        break;
                    }
                }

                // A callee declared `@inline(always)`, reached directly: its body, here, in place of
                // the call - `graftCall`. A callee reached through a slot or a value has no body in
                // hand to graft and is called.
                if(site.through == mir::CallThrough::Declaration && site.target &&
                   decl()[site.target]->alwaysInline && !written.callsValue()) {
                    auto value = graftCall(node, *decl()[site.target], arguments, result, hasResult);
                    if(hasResult && value) produced[node.id.index] = value - base();
                    break;
                }

                /*
                 * A check inside a grafted body reports the **graft site**, which is the one thing
                 * about a call this walk changes the source of - see `Graft::site`. Unconditional
                 * rather than gated on `-check-locations`, because it is a better answer either way
                 * and nothing but a located check reads a call's location.
                 */
                auto at = node.source;
                if(grafting && site.through == mir::CallThrough::Declaration &&
                   site.target == program.roles.function(intrinsic::Known::checkFailed)) {
                    at = grafting->site;
                }

                auto inst = editor.addCall(at, site, valueLeads ? Use() : target,
                                           arguments.pointer(), arguments.size(), result,
                                           conventionFor(result), hasResult);

                if(hasResult) produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            case mir::Node::Write: {
                auto& written = (mir::NodeWrite&)node;
                auto place = source.place(mir, written.destination.placeId());

                /*
                 * A `{}` destination, which is the write half of the rule the load half states.
                 *
                 * `Resolver::readPlace` answers a `{}` place with the constant rather than a load,
                 * because a type with one value is that value and there are no bytes at the place to
                 * read. A store into one writes those same no bytes, so it is not an instruction
                 * either - and emitting it anyway leaves the value it stored with no consumer, which
                 * in a stage that runs no dead-code pass is a dead `imm` in the output.
                 *
                 * Before the loan arm below rather than after it, because a `{}` binding is not a
                 * loan of anything: nothing borrows a value that has no storage.
                 */
                if(decl()[substituted(place->occurrence).type]->kind == mir::Type::Unit) break;

                /*
                 * The `Init` that establishes a binding which is itself a loan.
                 *
                 * There is nothing to store: the binding *is* the storage the loan names, and the
                 * write is what says which storage that is - see the slot loop in `run`. Emitting a
                 * store here would put the lender's address into a slot the size of the lender's
                 * object and read it back as one, which is the miscompile this arm exists to
                 * prevent rather than an inefficiency.
                 */
                if(written.mode == mir::Destination::Init && place->kind == mir::Place::Local &&
                   aliasesLender(substituted(place->occurrence))) {
                    auto value = valueOf(written.value, node.source);
                    if(!value) return;

                    /*
                     * Unless the record **carries** the binding, where the storage it names is a
                     * field rather than a value this conversion holds: a value would be one the
                     * dispatch's jump into a resume block never passed through, which is the
                     * dominance failure a `@heap` self-borrow used to report.
                     *
                     * Whichever kind of field it is, and that is one store either way: a pointer
                     * field takes the address the loan is, and a window's field takes the two words
                     * it is. What differs is what the *reads* do with it, which is the storage walk's
                     * answer and not this one's.
                     */
                    if(stepping && place->payload < stepping->localField.size() &&
                       stepping->localField[place->payload] != StepPlan::kNoField) {
                        auto field = U16(stepping->localField[place->payload]);
                        auto slot = stepField(node.source, field);

                        /*
                         * And a **value** member, where the mint held the loan's bytes rather than
                         * its address: `captureKindOf` answers `Value` for a shared call-bounded
                         * loan of a trivially copyable aggregate - `for e in pairs(m)` over
                         * `Entry('I32, 'I16)` - which is the calling convention's reading, and this
                         * arm's `aliasesLender` answers "reference" for the same occurrence. The
                         * field is the record's bytes at the local's own type and shape, so what
                         * goes in is a copy of what the loan names, which no program can tell from
                         * the loan. Storing the loan itself put an address in the record's first
                         * word and the consumer read it as the entry's key (J131).
                         */
                        auto member = stepping->record->field(decl(), field).occurrence;

                        if(!stepping->localBorrow[place->payload] &&
                           member.type == substituted(place->occurrence).type &&
                           referenceValued(value->occurrence) && !referenceValued(member)) {
                            auto copied = editor.addLoad(node.source,
                                                         editor.use(*value, UseKind::Inspect),
                                                         member, conventionFor(member), false);
                            value = editor.resultOf(*copied);
                        }

                        editor.addStore(node.source, editor.use(*slot, UseKind::Inspect),
                                        editor.use(*value, UseKind::Inspect));
                        break;
                    }

                    slots[place->payload] = value - base();
                    break;
                }

                auto address = addressOf(*place, node.source);
                if(!address) return;

                /*
                 * What was there before is already gone.
                 *
                 * `Destination::Assign` over a type with a teardown releases the old contents first,
                 * and that release is not implied by this store - it is a `DropStep` the ownership
                 * analysis placed at this position, emitted by `emitSteps` before this node. One
                 * rule ("released at the first point at which it is owned and no longer live") covers
                 * the replaced value, the end of a scope and the arm that did not use it, and a store
                 * that dropped its own destination would be a second answer for one of them.
                 */
                Use value;
                if(!useOf(written.value, node.source, value)) return;

                editor.addStore(node.source, editor.use(*address, UseKind::Inspect), value,
                                written.isVolatile);
                break;
            }
            case mir::Node::Return: {
                auto& written = (mir::NodeReturn&)node;
                if(!written.hasValue) {
                    emitLeave(node.source, nullptr);
                    break;
                }

                /*
                 * The result is a position, and under substitution it can be the view where the
                 * body answers with the owner - `deriveView`. The *enclosing* function's result and
                 * not `emitted`'s: a continuation returns what the function it was outlined from
                 * declared, which is what the carrier holds.
                 */
                auto position = grafting && grafting->result
                    ? grafting->result->occurrence : substituted(function.result);

                /*
                 * A result that is `{}` only under substitution - `fn (a) f() -> a` at `a = {}`.
                 *
                 * The body wrote `return v`, but a value of no bytes is absent rather than present
                 * (see the call cases, which produce nothing for one), so there is no operand to
                 * hand over and a leave with no value is what a declared `{}` would have been.
                 * Reading the operand anyway reports a node that produced nothing.
                 */
                if(decl()[position.type]->kind == mir::Type::Unit) {
                    emitLeave(node.source, nullptr);
                    break;
                }

                Use used;
                if(!useOf(written.value, node.source, used)) return;

                if(!adaptUseToView(used, position, node.source)) return;

                emitLeave(node.source, &used);
                break;
            }
            case mir::Node::Jump:
                convertEdge(node, ((mir::NodeJump&)node).to, kExitFallOut);
                break;
            case mir::Node::Break:
                convertEdge(node, ((mir::NodeJump&)node).to, kExitBreak);
                break;
            case mir::Node::Resume:
                convertResume((mir::NodeResume&)node);
                break;
            case mir::Node::LensCall:
            case mir::Node::LensTry:
                convertLensCall((mir::NodeLensCall&)node);
                break;


            case mir::Node::Branch: {
                auto& written = (mir::NodeBranch&)node;
                auto value = valueOf(written.condition, node.source);
                if(!value) return;

                // One list, read off the true edge and given to both. The two MIR edges hold the
                // same operands by construction - `mir::Builder::addBranch` fills them from one
                // list - so converting the second would be converting the first again.
                assertTrue(written.whenTrue.arguments.size() == written.whenFalse.arguments.size());

                auto whenTrue = base()[blocks[mir[written.whenTrue.target]->index.index]];

                SmallArray<Use, 8> arguments;
                for(auto operand: written.whenTrue.arguments.contents(mir)) {
                    Use used;
                    if(!useOf(operand, node.source, used)) return;
                    if(!adaptEdgeArgument(*whenTrue, arguments.size(), used, node.source)) return;
                    arguments.push(used);
                }

                editor.addBranch(node.source, editor.use(*value, UseKind::Inspect),
                                 *base()[blocks[mir[written.whenTrue.target]->index.index]],
                                 *base()[blocks[mir[written.whenFalse.target]->index.index]],
                                 arguments.pointer(), arguments.size());
                break;
            }
            /*
             * A multi-way branch - the selector, the cases and one argument list carried down.
             *
             * One list read off the fallback and given to every edge, for the reason the `Branch`
             * arm above gives: `mir::Builder::addSwitch` fills them all from one list, so converting
             * the others would be converting the first again.
             */
            case mir::Node::Switch: {
                auto& written = (mir::NodeSwitch&)node;
                auto value = valueOf(written.selector, node.source);
                if(!value) return;

                auto items = written.cases.pointer(mir);
                auto count = written.cases.size();

                SmallArray<I64, 16> values;
                SmallArray<Block*, 16> targets;

                for(Size i = 0; i < count; i++) {
                    values.push(items[i].value);
                    targets.push(base()[blocks[mir[items[i].edge.target]->index.index]]);
                }

                auto fallback = base()[blocks[mir[written.fallback.target]->index.index]];

                SmallArray<Use, 8> arguments;
                for(auto operand: written.fallback.arguments.contents(mir)) {
                    Use used;
                    if(!useOf(operand, node.source, used)) return;
                    if(!adaptEdgeArgument(*fallback, arguments.size(), used, node.source)) return;
                    arguments.push(used);
                }

                editor.addSwitch(node.source, editor.use(*value, UseKind::Inspect), values.pointer(),
                                 targets.pointer(), count, *fallback, arguments.pointer(),
                                 arguments.size());
                break;
            }
            case mir::Node::Unreachable:
                editor.addUnreachable(node.source);
                break;
            case mir::Node::KindCount:
                break;
        }
    }

    bool run() {
        /*
         * Blocks first, then contents.
         *
         * A terminator names a successor that may not have been converted yet, so the block map has
         * to be complete before any instruction is built. The entry block already exists - the
         * editor made it - so it is mapped rather than created.
         */
        /*
         * Only this scope's blocks - W-C.
         *
         * A `Handover` region becomes a function of its own, so its blocks are not this function's
         * and its entry is not a block anything here jumps to. What is left in `blocks` for one of
         * them is null, and the two places that would have reached it are the two the outlining
         * replaces: an edge into a region is the call, and an edge out of one is the discriminant.
         *
         * No region is copied for the same reason. MIR's `Handover` rows each became a function, and
         * the only regions this body has are the `Cleanup` arms the walk below opens itself.
         */
        auto entryBlock = outlining ? source.region(mir, scope)->entry : BlockId(0);

        blocks.reserve(source.blocks.size());
        for(U32 i = 0; i < source.blocks.size(); i++) blocks.push(nullptr);

        /*
         * A graft's entry is a block the caller made where the call was; a function's is its own -
         * and a **step's** is the dispatch, so the producer's own entry is a block beside it.
         *
         * The three made here are the ones nothing in MIR corresponds to: the arm a record that has
         * not started takes, the arm a finished one takes, and the producer's first statement.
         */
        if(stepping) {
            stepping->fresh = editor.addBlock(StringId());
            stepping->finished = editor.addBlock(StringId());
            stepping->bodyEntry = editor.addBlock(StringId());
            blocks[entryBlock.index] = stepping->bodyEntry - base();
        } else {
            blocks[entryBlock.index] = grafting ? grafting->entry - base() : editor.body.entry;
        }

        for(U32 i = 0; i < source.blocks.size(); i++) {
            if(i == entryBlock.index) continue;
            if(!inScope(BlockId(i))) continue;

            blocks[i] = editor.addBlock(source.block(mir, BlockId(i))->name) - base();
        }

        /*
         * Every block's parameters, before any instruction anywhere.
         *
         * A parameter holds on entry rather than occupying a position among the instructions, and
         * `addBlockArgument` holds a caller to that by refusing a block that already has one - so
         * this cannot be interleaved with the allocations below even if it wanted to be.
         *
         * The *entry* block's parameters are the function's own, and they are the one list not read
         * out of MIR: a MIR parameter is a `Local` (see `mir::Local::parameter`), because it is
         * storage the body projects and borrows from. What arrives on entry is the value that fills
         * that storage, which is an SSA question and is answered here.
         */
        // The function's entry, which is the block the *parameters* arrive on. In a step that is the
        // dispatch rather than the producer's first block - see above.
        auto entry = stepping ? base()[editor.body.entry] : base()[blocks[entryBlock.index]];

        // A grafted body's parameters are the call's arguments, already values; only a function of
        // its own receives them as its entry's arguments.
        if(!grafting) {
            for(auto argumentPointer: emitted.args.contents(decl())) {
                auto& declared = *decl()[argumentPointer];

                // The same clause as the `Read` case's, at the other end of the same handover: a
                // shared loan of a register-valued trivially copyable type *is* its copy, so what
                // arrives is an ordinary value. Declaring it as a borrow was what put every such
                // parameter's slot beyond `promote` - the store installed a `Guaranteed` value and
                // the reads took `Trivial` ones back out, which is a convention change and not a
                // substitution. The lowered signature is untouched: `lower/gen.cpp` manifests a
                // parameter from its *declared* occurrence, and `isReferenceValue` is false here
                // exactly because the two manifest alike.
                editor.addBlockArgument(*entry, valueOccurrenceOf(declared.occurrence),
                                        conventionFor(valueOccurrenceOf(declared.occurrence)));
            }
        }

        // What parameter `i` arrived as - the entry's argument, or the call's.
        auto incomingArgument = [&](U32 i) -> Value* {
            if(grafting) return base()[grafting->arguments[i].value()];
            return base()[entry->argumentAt(base(), i)];
        };

        /*
         * A step's own three - the record, what the continuation answered, and whether this is a
         * cancellation. None of them is a parameter of the producer, so none is in the loop above.
         */
        if(stepping) {
            stepping->self = base()[entry->argumentAt(base(), 0)];
            stepping->resumeValue = base()[entry->argumentAt(base(), 1)];
            stepping->cancel = base()[entry->argumentAt(base(), 2)];
        }

        for(U32 i = 0; i < source.blocks.size(); i++) {
            auto block = source.block(mir, BlockId(i));
            if(!inScope(BlockId(i))) continue;

            /*
             * The scope's own entry declares its parameters from the *function*, not from MIR.
             *
             * A continuation's entry has one MIR block argument - what the callee handed over - and
             * in the outlined function that is a parameter rather than an edge operand, which is
             * exactly the boundary: the value comes from inside a call this frame is in the middle
             * of, so there is no predecessor to have supplied it.
             */
            if(BlockId(i) == entryBlock) {
                auto count = block->arguments.size();
                if(!outlining || !count) continue;

                auto& argument = *mir[block->arguments.get(mir, 0)];
                produced[argument.id.index] = entry->argumentAt(base(), 1);
                continue;
            }

            auto target = base()[blocks[i]];

            for(auto argumentPointer: block->arguments.contents(mir)) {
                auto& argument = *mir[argumentPointer];
                auto occurrence = substituted(mir::valueOccurrence(argument));
                auto value = editor.addBlockArgument(*target, occurrence,
                                                     conventionFor(occurrence));

                // The parameter is a node in MIR and a value here, so the map from one to the other
                // is the same `produced` table every other result goes through - which is what lets
                // an operand naming a parameter need no case of its own.
                produced[argument.id.index] = value - base();
            }
        }

        /*
         * The locals, as storage in the entry block.
         *
         * Before any block is converted, because a loop body reads a local the header wrote and
         * neither of them is entitled to decide where it lives. The entry is where they go for the
         * ordinary reason a frame slot does: it dominates everything, so no path reaches a read
         * whose storage has not been established.
         */
        editor.setBlock(*entry);
        slots.reserve(source.locals.size());

        /*
         * The environment, and what is in it - W-C.
         *
         * A continuation's captures are addresses its caller stored, so a slot here is a *load*
         * rather than an allocation: the storage is the enclosing frame's and stays there, which is
         * the by-reference capture `doc/spec/lenses.md` says a continuation makes and is what lets
         * the two lowerings agree about when anything dies.
         */
        Array<SsaPtr<Value>> captured;

        if(outlining) {
            environment = entry->argumentAt(base(), 0);

            auto& record = *(mir::RecordType*)decl()[outlining->environment];

            for(U32 i = 0; i < outlining->captures.size(); i++) {
                auto field = record.fields.get(decl(), i);
                auto address = editor.addFieldAddress(emitted.source,
                                                      addressUse(*base()[environment]),
                                                      outlining->environment, U16(i));

                /*
                 * An **owned pointer** occurrence, and that is the whole of what makes this a real
                 * load - `lower/inst.cpp`'s note on `Inst::Load`.
                 *
                 * A load whose result is a loan is no instruction, because a value that lives in
                 * storage *is* its address. A field that *holds* a reference is the one place that
                 * is not true, and it is told apart by the occurrence the conversion gives the load:
                 * what comes out here is the caller's slot address, which is a pointer this frame
                 * holds and not a borrow of the field.
                 */
                auto held = mir::owned(program.types.pointerType(field.occurrence.type));
                auto loaded = editor.addLoad(emitted.source,
                                             editor.use(*editor.resultOf(*address),
                                                        UseKind::Inspect),
                                             held, Convention::Address);

                captured.push(editor.resultOf(*loaded) - base());
            }

            if(outlining->resultField != Outline::kNoField) {
                resultCarrier = captured[outlining->resultField];
            }
        }

        auto captureOf = [&](LocalId local) -> SsaPtr<Value> {
            if(!outlining) return nullptr;

            for(U32 i = 0; i < outlining->captures.size(); i++) {
                if(outlining->captures[i] == local) return captured[i];
            }

            return nullptr;
        };

        /*
         * A **step**'s parameters, and the record's own members.
         *
         * This is the whole of the first thing a step reads differently. The producer's parameters
         * are the captured arguments, the locals it keeps across a `yield` are the payload, and both
         * are fields of `self`; the continuation parameter is the one local with no storage at all,
         * because a `yield` is not a call here. Everything else falls through to the walk below and
         * gets an ordinary frame slot, which is right because it is dead by the time a step returns.
         *
         * A capture the record holds **by address** is the one that is not a field address: the
         * field holds the lender's pointer, so the local's storage is what is *in* it - which is
         * exactly what `aliasesLender` gives a borrowed parameter in an ordinary conversion.
         */
        if(stepping) {
            auto& record = *stepping->record;
            auto recordType = asBase<mir::Type>(DeclPtr<mir::RecordType>(&record - decl()));
            auto self = addressUse(*stepping->self);

            for(U32 i = 0; i < source.locals.size(); i++) {
                auto local = source.local(mir, LocalId(i));

                if(local->isParameter() && local->parameter == stepping->continuationArg) {
                    slots.push(nullptr);
                    continue;
                }

                auto field = local->isParameter() ? 2 + local->parameter : stepping->localField[i];

                // A **borrow** the record carries has no slot at all: which storage it names is read
                // out of its field at every use - `addressOf`'s `Local` case, where the reason is.
                if(!local->isParameter() && stepping->localBorrow[i]) {
                    slots.push(nullptr);
                    continue;
                }

                if(field == StepPlan::kNoField) {
                    auto held = substituted(local->occurrence);
                    if(aliasesLender(held)) { slots.push(nullptr); continue; }

                    auto alloc = editor.addAlloc(local->source, held,
                                                 mir::storageOf(mir, source, *local));
                    slots.push(editor.resultOf(*alloc) - base());
                    continue;
                }

                auto address = editor.addFieldAddress(local->source, self, recordType, U16(field));
                auto slot = editor.resultOf(*address);

                /*
                 * A capture the record holds **by address**: what the local names is the lender's
                 * storage, so the slot is the pointer *in* the field rather than the field.
                 *
                 * At the field's own `%T` and not at the local's borrow, which is the one place a
                 * load has to be an instruction: a value that lives in storage *is* its address, so
                 * a load whose result is declared a loan lowers to nothing at all and the "address"
                 * that came back was the field's rather than what is in it. `lower/README.md`'s note
                 * about a continuation's environment is the same sentence about the same shape.
                 *
                 * A **window** is the third kind and is not one of these: the descriptor's own bytes
                 * are the field, so the field's address is the object exactly as it is for a capture
                 * held by value. `resolve_mir/resolve_object.cpp`'s `IterCaptureKind` is the list,
                 * and `Address` is its one row that names storage somewhere else.
                 */
                auto byAddress = local->isParameter() &&
                                 record.captureKinds.get(decl(), local->parameter) ==
                                     mir::RecordType::kCaptureAddress;

                // And a `@heap`-placed payload member, which is the same shape for the other reason:
                // the value lives in a run of its own so that the record may move without taking it
                // along, and the field holds where it is - `RecordType::kHeapMember`, §6.1.
                if(!local->isParameter() && stepping->localRun[i]) byAddress = true;

                if(byAddress) {
                    auto held = mir::owned(record.field(decl(), U16(field)).occurrence.type);
                    auto loaded = editor.addLoad(local->source, editor.use(*slot, UseKind::Inspect),
                                                 held, conventionFor(held), false);
                    slot = editor.resultOf(*loaded);
                }

                slots.push(slot - base());
            }
        }

        for(U32 i = 0; !stepping && i < source.locals.size(); i++) {
            auto local = source.local(mir, LocalId(i));

            // Storage of an enclosing scope, reached through the environment, and storage of a
            // scope this one does not emit at all - the second is not a name anything here says.
            if(auto held = captureOf(LocalId(i))) { slots.push(held); continue; }
            if(local->region != scope) { slots.push(nullptr); continue; }

            /*
             * A local that holds a **loan** is a second name for storage, not a second object.
             *
             * The lender's object did not travel - what travelled is where it is - so a slot of its
             * own would be a copy of somebody else's value, and a borrow that copies is not a
             * borrow. An **owned** local still gets a slot: the object becomes this frame's, this
             * frame is what releases it, and the store that fills the slot is the relocation that
             * says so. That the relocation is a block copy the caller could have avoided by
             * allocating the storage itself is §5.6's destination passing, which is an optimization
             * over this and not a different meaning.
             *
             * Where the address comes from is the one thing the two kinds of loan differ in. A
             * borrowed **parameter** arrived as one, so its slot is the incoming block argument. A
             * `let d = c` gets its address from the `Init` that establishes the binding, which is
             * inside the body - so the entry is left null and the write fills it, and a read before
             * that point is `addressOf`'s report rather than a slot holding nothing.
             *
             * All of which is true of a loan that **is** an address, and `aliasesLender` is where
             * that is asked. A loan of a register-valued type is not one: the bytes are the value, so
             * the binding needs storage of its own exactly as an owner does and the write that
             * establishes it is an ordinary store. It is not a shape the source can write directly -
             * `readMode` copies at every position that makes one - and it is what a **generic** body
             * produces, where a loan of `a` is written because nothing there knows `a` is `I32`.
             */
            auto held = substituted(local->occurrence);
            if(aliasesLender(held)) {
                if(local->isParameter()) {
                    slots.push(incomingArgument(local->parameter) - base());
                } else {
                    slots.push(nullptr);
                }

                continue;
            }

            /*
             * **"An owned parameter that arrived in memory already has its storage" was tried here
             * and needs a convention change first**, which is worth the note because it looks free.
             *
             * `arrivesAsAddress` is the calling convention's own question and an *owned* one of
             * those is a handover of the bytes at that address, so the second slot this walk gives
             * the local and the store that fills it are a block copy of a record the callee already
             * owns - `->at: Site` in `lib/Test/Check.yana` is forty bytes copied on entry to every
             * assertion, on the path a *passing* one takes. `compiler/resolve` (deleted in J173)
             * did not: a parameter was a place there, and the place reached through the incoming
             * pointer was the place.
             *
             * What stops it is that the incoming value is `Convention::Owned`. A tracked value must
             * be consumed exactly once and may not be named by an `Inspect`, and using it as a slot
             * is exactly naming it by inspecting uses - `verifyBody` answers "owned value 0 reaches
             * return unconsumed" for every such parameter, which is the linear rule working. The
             * value would have to *arrive* as an `Address`, and that is a change to what a call
             * hands over rather than to what this walk does with it: the site, the inliner's
             * argument binding and `handoverChecks` all read the owner. `aliasesLender` above works
             * for the borrowed case only because a borrow is `Guaranteed` and untracked.
             */

            /*
             * The placement is the **object's**, decided by MIR ownership analysis - `storageOf`.
             * This walk carries it down and does not choose one.
             *
             * Of the object and not of the slot, which is the whole of §10.4's identity question:
             * two locals a move connects hold one object, and asking each slot separately where it
             * lives is how one object comes to have two allocations that disagree. `Local::object`
             * is the lineage and `storageOf` is the join over it, in one place.
             */
            auto placed = mir::storageOf(mir, source, *local);
            auto alloc = editor.addAlloc(local->source, held, placed);
            slots.push(editor.resultOf(*alloc) - base());

            // The other end of it, remembered here because this is the only point at which the
            // allocation and its placement are both in hand - see `heapSlots`.
            // `Heap` and not "anything but the frame": `Region` is the third rung, and what hands a
            // region-placed object back is the region's own bulk discharge rather than a `free` per
            // object - so one pushed here would be released twice. Nothing selects that rung yet
            // (`resolve_mir/verify.cpp` refuses it for want of a witness), which is exactly why the
            // test has to say which rung it means rather than which one it is not.
            if(placed == mir::StorageClass::Heap) heapSlots.push(slots[slots.size() - 1]);
        }

        /*
         * Where an exit that leaves this function parks what it carries - `Outline::resultField`.
         *
         * One slot for the whole nest and allocated here, in the frame that actually returns: a
         * `return` written three continuations deep fills this storage and every frame between it
         * and here propagates a discriminant. The alternative - a slot per region and a copy at each
         * boundary - would be the same value described once per level.
         */
        if(!outlining) {
            auto carried = false;
            for(auto& plan: outlines) carried |= plan.resultField != Outline::kNoField;

            if(carried) {
                auto held = substituted(function.result);
                resultCarrier = editor.resultOf(
                    *editor.addAlloc(function.source, held, mir::StorageClass::Frame)) - base();
            }
        }

        /*
         * And one per skipping lens call, for what that call answers with - `convertSkip`.
         *
         * In the entry block for the reason every other slot here is, and the reason is sharper for
         * this one: a `lens.try` written inside a loop is ordinary code, and an `alloc` in a block
         * that runs twice is two allocations. The wrapper is a temporary of *this* scope, so the
         * walk asks each block whose region it is rather than converting the whole body.
         */
        for(auto blockPointer: source.blocks.contents(mir)) {
            auto block = mir[blockPointer];
            if(block->region != scope || !block->nodes.size()) continue;

            auto last = mir[block->nodes.get(mir, block->nodes.size() - 1)];
            if(last->kind != mir::Node::LensTry) continue;

            // At the call's own failure type in this specialization's terms - `skipOutcome`.
            auto held = skipOutcome(*(mir::NodeLensCall*)last, last->source);
            if(!held.type) continue;
            outcomes[last->id.index] = editor.resultOf(
                *editor.addAlloc(last->source, held, mir::StorageClass::Frame)) - base();
        }

        /*
         * A `Bool` in the frame for every path the analysis said needs one.
         *
         * In the entry block for the reason the slots are: a flag is read on one path and written on
         * another, and neither of those is entitled to decide where it lives.
         */
        if(elaboration) {
            flags.reserve(elaboration->paths.size());

            for(U32 i = 0; i < elaboration->paths.size(); i++) {
                if(!elaboration->flagged.get(mir, i)) { flags.push(nullptr); continue; }

                auto held = mir::owned(mir::booleanType(program));
                auto alloc = editor.addAlloc(function.source, held, mir::StorageClass::Frame);
                flags.push(editor.resultOf(*alloc) - base());
            }

            // A continuation's flags are its caller's, where the planner captured them - see
            // `Outline::flagOf`. The frame slot above is then simply unused.
            for(U32 i = 0; outlining && i < outlining->captures.size(); i++) {
                auto path = outlining->flagOf[i];
                if(path != Outline::kNoFlag && path < flags.size()) flags[path] = captured[i];
            }
        }

        /*
         * Each parameter's incoming value, into the storage its local names.
         *
         * The mode is `argumentMode`'s, the same function the call site read to build the operand -
         * so an owned parameter installs an owner and a borrowed one installs a loan, and the two
         * ends of one handover cannot come to disagree about which it was. That is the whole reason
         * it is a shared function rather than a switch on each side.
         *
         * The load-and-store traffic this creates for a scalar that is never borrowed is the lower
         * IR's `promoteStackSlots` to remove, on the same terms as any other local: doing it here
         * would make the SSA dump depend on an optimization.
         */
        for(U32 i = 0; !outlining && !stepping && i < function.args.size(); i++) {
            auto& declared = *decl()[function.args.get(decl(), i)];
            auto incoming = incomingArgument(i);

            // A parameter whose slot *is* the incoming address has nothing to store - see above.
            if(base()[slots[i]] == incoming) continue;

            UseKind kind;
            if(!useKindFor(mir::argumentMode(declared.occurrence.contract,
                                             mir::SourceKind::Result), kind)) {
                report(declared.source, "internal: a parameter has no handover mode"_v);
                continue;
            }

            editor.addStore(declared.source, editor.use(*base()[slots[i]], UseKind::Inspect),
                            editor.use(*incoming, kind));
        }

        /*
         * The drop flags' initial values, at the function's own entry.
         *
         * A parameter arrives owned and a local arrives with nothing in it, which is the same
         * distinction the initialization dataflow starts from - see `Local::parameter`. They are at
         * the front of the flag list, so the cursor the block walk uses starts after them.
         */
        if(elaboration) {
            while(flagCursor < elaboration->flags.size()) {
                auto step = elaboration->flags.get(mir, flagCursor);
                if(step.position != mir::FlagStep::kEntry) break;

                // Not in a continuation, whose flags are the caller's and already hold the state
                // the region was entered in.
                flagCursor++;
                if(!outlining) emitFlag(step);
            }
        }

        // A spilled value that is a block's argument exists from here on: every block argument was
        // made above, before any block is converted and so before any reload replaced it.
        if(stepping) {
            for(U32 j = 0; j < stepping->spillValue.size(); j++) {
                if(stepping->spillIsArgument[j]) {
                    stepping->spillDefined[j] = produced[stepping->spillValue[j]];
                }
            }
        }

        for(U32 i = 0; i < source.blocks.size(); i++) {
            auto block = source.block(mir, BlockId(i));
            if(!inScope(BlockId(i))) continue;

            editor.setBlock(*base()[blocks[i]]);
            editor.setRegion(base()[blocks[i]]->region);
            emitting = block->region;

            if(stepping) stepSpillsOnEntry(i);

            auto count = block->nodes.size();
            convertingBlock = i;

            for(U32 n = 0; n < count; n++) {
                // The teardowns first: a drop is placed *before* the node that made it due - the
                // `return` that ends the scope, the assignment that replaces the value, the first
                // instruction of the arm that did not use it.
                emitSteps(i, n);

                auto& node = *mir[block->nodes.get(mir, n)];
                convertNode(node);

                if(stepping) stepSpillDefined(node);

                // The dependency, once the value exists - see `attachLoan`. After the node rather
                // than inside each case of it, so that a kind added to the switch cannot be the one
                // that forgets.
                if(produced[node.id.index]) attachLoan(node, *base()[produced[node.id.index]]);
            }

            // A lens call's leave-path steps sit past its terminator, and `convertLensCall` has
            // emitted them on that path - or had no such path to put them on.
            skipSteps(i, count);
        }

        // The dispatch, last: the blocks a `yield` resumes at are made while the body is converted,
        // so the switch that names them cannot be written before it.
        if(stepping) emitStepDispatch();

        // Sealed once, by the conversion that owns the body: a graft's blocks are sealed with the
        // caller's, and sealing them here as well would count every edge twice.
        if(!grafting) editor.seal();
        return errors == 0;
    }

    /*
     * Which definition of each spilled value this block's uses read - `StepPlan::spillAction`.
     *
     * Set on entry to every block that uses one, so that no block inherits whatever the block
     * converted before it in index order left in `produced`: that was the whole of the defect, since
     * index order is not dominance order once a loop body has two hand-overs in it.
     */
    void stepSpillsOnEntry(U32 block) {
        auto at = emitted.source;

        for(U32 j = 0; j < stepping->spillValue.size(); j++) {
            auto value = stepping->spillValue[j];
            auto action = stepping->spillAction[Size(j) * stepping->spillBlocks + block];

            // The definition's own block, where it is a block argument: written to the field before
            // anything else, where some later block reads it back from there.
            if(stepping->spillDefinedIn[j] == block) {
                if(stepping->spillIsArgument[j]) stepSpillStore(at, j);
                continue;
            }

            if(action == StepPlan::kSpillNothing) continue;

            SsaPtr<Value> known;
            if(action == StepPlan::kSpillFromDefinition) {
                known = stepping->spillDefined[j];
            } else if(action != StepPlan::kSpillLoad) {
                known = stepping->spillReloaded[Size(j) * stepping->yields + action];
            }

            if(known) {
                produced[value] = known;
                continue;
            }

            // A hand-over that was never converted - one inside a delegated region, whose blocks
            // this level does not run - leaves the value as the order of conversion had it.
            if(action != StepPlan::kSpillLoad) continue;

            // Several definitions reach this block, or the one that does is converted after it -
            // so the field is where the value is, which every one of them has written.
            assertTrue(stepping->spillStoreAtDefinition[j]);

            auto held = mir::owned(stepping->record->field(decl(),
                                   U16(stepping->spillField[j])).occurrence.type);
            auto address = stepField(at, U16(stepping->spillField[j]));
            auto loaded = editor.addLoad(at, editor.use(*address, UseKind::Inspect), held,
                                         conventionFor(held), false);

            produced[value] = editor.resultOf(*loaded) - base();
        }
    }

    // A node that is a spilled value's definition: its value, kept for the blocks that take it,
    // and into the field where a block reads it back from there.
    void stepSpillDefined(mir::Node& node) {
        for(U32 j = 0; j < stepping->spillValue.size(); j++) {
            if(stepping->spillValue[j] != node.id.index || stepping->spillIsArgument[j]) continue;

            stepping->spillDefined[j] = produced[node.id.index];
            stepSpillStore(node.source, j);
        }
    }

    void stepSpillStore(LocationId at, U32 j) {
        if(!stepping->spillStoreAtDefinition[j]) return;

        auto value = produced[stepping->spillValue[j]];
        if(!value) return;

        auto address = stepField(at, U16(stepping->spillField[j]));
        editor.addStore(at, editor.use(*address, UseKind::Inspect),
                        editor.use(*base()[value], UseKind::Consume));
    }

    /*
     * The step's entry - a `switch` on the resume index - and the two arms nothing in MIR answers.
     *
     * Index 0 is a record that has **not started**, which is §2.6's rule as a block: a cancellation
     * there releases the captured arguments and stops, because none of the producer's body has run.
     * The finished index is the fallback and returns `False` having nothing to release: whatever the
     * record owed was released by the `return` that set it.
     */
    void emitStepDispatch() {
        auto at = emitted.source;
        auto& record = *stepping->record;

        auto cancelled = editor.addBlock(StringId());

        editor.setBlock(*stepping->fresh);
        editor.setRegion(stepping->fresh->region);
        editor.addBranch(at, editor.use(*stepping->cancel, UseKind::Inspect), *cancelled,
                         *stepping->bodyEntry, nullptr, 0);

        editor.setBlock(*cancelled);
        editor.setRegion(cancelled->region);

        for(U32 i = 0; i < record.captureKinds.size(); i++) {
            auto captured = record.field(decl(), U16(2 + i));

            // What the record **holds**, and not what it reaches - `ownsField`, which is the same
            // question `classifyRecord` asks of the same field. A window's field is in an origin
            // slot, so releasing it would run a `Drop` on storage the lender still owns.
            if(!mir::ownsField(captured)) continue;
            if(!mir::hasTeardownAt(decl(), captured.occurrence)) continue;

            auto address = stepField(at, U16(2 + i));
            editor.addDrop(at, editor.use(*address, UseKind::Inspect),
                           mir::owned(captured.occurrence.type));
        }

        // The runs `f$new` allocated, which exist at index zero exactly as they do everywhere else.
        stepReleaseRuns(at);

        stepStoreIndex(at, kIterFinished);
        stepReturn(at, false);

        editor.setBlock(*stepping->finished);
        editor.setRegion(stepping->finished->region);
        stepReturn(at, false);

        auto dispatch = base()[editor.body.entry];
        editor.setBlock(*dispatch);
        editor.setRegion(dispatch->region);

        SmallArray<I64, 8> values;
        SmallArray<Block*, 8> targets;

        values.push(0);
        targets.push(stepping->fresh);

        for(U32 i = 0; i < stepping->yields; i++) {
            if(!stepping->resume[i]) continue;

            values.push(I64(1 + i));
            targets.push(base()[stepping->resume[i]]);
        }

        // And the **delegated** regions, whose indices continue the same numbering - §6.3. A level
        // suspended at one of these has a child the consumer is stepping directly, and what it comes
        // back to is the release of that child (`convertStepNest`).
        for(U32 r = 0; r < stepping->delegateResume.size(); r++) {
            if(!stepping->delegateResume[r]) continue;

            values.push(I64(1 + stepping->yields + r));
            targets.push(base()[stepping->delegateResume[r]]);
        }

        auto held = mir::owned(program.types.exitCode());
        auto index = stepField(at, 0);
        auto loaded = editor.addLoad(at, editor.use(*index, UseKind::Inspect), held,
                                     conventionFor(held), false);

        auto selector = editor.resultOf(*loaded);

        /*
         * A `switch` where there is a switch to make, and a chain of comparisons where there is not.
         *
         * Below three cases the IR states that a multi-way branch is a branch - `makeSwitch` asserts
         * it - and a producer with one `yield` has exactly two arms, which is the common shape.
         */
        if(values.size() >= 3) {
            editor.addSwitch(at, editor.use(*selector, UseKind::Inspect), values.pointer(),
                             targets.pointer(), values.size(), *stepping->finished, nullptr, 0);
            return;
        }

        /*
         * And the chain tests the **resume** index first and the fresh one last. Index 0 is entered
         * once per object and the resume index once per item, so testing 0 first put a compare and a
         * branch that never answers on every item's path: `mov`, `test`, `je`, `cmp`, `je` where
         * `mov`, `cmp`, `je` is the step. Two instructions a token on `test/bench/lexer`'s iter rows
         * (J179). The `switch` above is left in index order: its lowering picks its own.
         */
        auto freshValue = values[0];
        auto freshTarget = targets[0];
        values.remove(0);
        targets.remove(0);
        values.push(freshValue);
        targets.push(freshTarget);

        auto answer = mir::owned(mir::booleanType(program));

        for(Size i = 0; i < values.size(); i++) {
            auto written = editor.addConst(at, mir::intConstant(held.type, U64(values[i])));
            auto same = editor.addBinary(at, mir::BinaryOp::Eq,
                                         editor.use(*selector, UseKind::Inspect),
                                         editor.use(*editor.resultOf(*written), UseKind::Inspect),
                                         answer, conventionFor(answer));

            auto otherwise = i + 1 < values.size() ? editor.addBlock(StringId())
                                                   : stepping->finished;

            editor.addBranch(at, editor.use(*editor.resultOf(*same), UseKind::Inspect), *targets[i],
                             *otherwise, nullptr, 0);

            if(i + 1 == values.size()) break;

            editor.setBlock(*otherwise);
            editor.setRegion(otherwise->region);
        }
    }
};

/*
 * `StepPlan::spillAction` - which definition of each spilled value reaches each block, J164.
 *
 * A forward walk per spill over the MIR graph, whose lattice is one definition or several: the value's
 * own definition, and each hand-over that keeps it, whose resume block reloads it. A hand-over that
 * does not keep it ends it - the value is dead below that `yield`, so nothing reached from there reads
 * it before it is defined again - and is the walk's bottom rather than a definition.
 *
 * The graph is the one the step is converted from, with the two edges it has that MIR's terminators
 * do not state: a `Many` region's fall-out back to its entry (`LiveAcross::meetSuccessors`), and a
 * nested traversal's call straight to its region's destination, which is the path of a child that
 * hands over nothing (`convertStepNest`'s advance). Where one hand-over rejoins another, or a block
 * reads a definition converted after it in index order, the block reads the field, and the
 * definition writes it - which is the only cost, and is paid only by a producer that has such a
 * block; every other one converts exactly as it did before.
 */
static void planSpillReach(DeclBase decl, MirBase mir, mir::Body& body, StepPlan& plan) {
    constexpr U32 kUndefined = ~U32(0);
    constexpr U32 kSeveral = ~U32(1);
    constexpr U32 kDefinition = ~U32(2);

    auto blockCount = U32(body.blocks.size());
    auto spills = U32(plan.spillValue.size());
    plan.spillBlocks = blockCount;

    Array<U32> definedIn;
    Array<U8> isArgument;
    for(U32 i = 0; i < body.nodes.size(); i++) { definedIn.push(kUndefined); isArgument.push(0); }

    for(U32 b = 0; b < blockCount; b++) {
        auto block = body.block(mir, BlockId(b));
        for(auto argument: block->arguments.contents(mir)) {
            definedIn[mir[argument]->id.index] = b;
            isArgument[mir[argument]->id.index] = 1;
        }
        for(auto node: block->nodes.contents(mir)) definedIn[mir[node]->id.index] = b;
    }

    auto successors = [&](U32 b, auto&& f) {
        auto block = body.block(mir, BlockId(b));
        auto count = block->nodes.size();
        if(!count) return;

        auto& last = *mir[block->nodes.get(mir, count - 1)];
        mir::forEachEdge(mir, last, [&](mir::Edge& edge) {
            if(edge.target) f(mir[edge.target]->index.index);
        });

        if(mir::isLensCall(last.kind)) {
            auto region = body.region(mir, ((mir::NodeLensCall&)last).region);
            if(region && region->destination.valid()) f(region->destination.index);
            return;
        }

        if(last.kind != mir::Node::Jump) return;

        auto region = body.region(mir, block->region);
        if(!region || region->multiplicity != mir::Continuation::Many) return;

        auto leaves = false;
        mir::forEachEdge(mir, last, [&](mir::Edge& edge) {
            if(edge.target && mir[edge.target]->region != block->region) leaves = true;
        });

        if(leaves) f(region->entry.index);
    };

    auto join = [](U32 a, U32 b) {
        if(a == kUndefined) return b;
        if(b == kUndefined || a == b) return a;
        return kSeveral;
    };

    Array<U32> entry;
    for(U32 b = 0; b < blockCount; b++) entry.push(kUndefined);

    for(U32 j = 0; j < spills; j++) {
        auto value = plan.spillValue[j];
        auto home = definedIn[value];

        plan.spillDefinedIn.push(home);
        plan.spillIsArgument.push(isArgument[value]);
        plan.spillStoreAtDefinition.push(0);
        plan.spillDefined.push(nullptr);
        for(U32 k = 0; k < plan.yields; k++) plan.spillReloaded.push(nullptr);

        for(U32 b = 0; b < blockCount; b++) entry[b] = kUndefined;

        // Where a hand-over sits, for "converted after this block": the block that holds it.
        auto yieldBlock = [&](U32 k) {
            for(U32 i = 0; i < body.nodes.size(); i++) {
                if(plan.yieldOf[i] == k) return definedIn[i];
            }
            return kUndefined;
        };

        for(auto changed = true; changed;) {
            changed = false;

            for(U32 b = 0; b < blockCount; b++) {
                auto state = entry[b];
                if(b == home && isArgument[value]) state = kDefinition;

                auto block = body.block(mir, BlockId(b));
                for(auto pointer: block->nodes.contents(mir)) {
                    auto& node = *mir[pointer];
                    if(node.id.index == value) state = kDefinition;

                    auto k = plan.yieldOf[node.id.index];
                    if(k != StepPlan::kNotAYield) {
                        state = plan.record->keepsAcross(decl, plan.spillEntry[j], k) ? k
                                                                                    : kUndefined;
                    }
                }

                successors(b, [&](U32 target) {
                    auto joined = join(entry[target], state);
                    if(joined == entry[target]) return;

                    entry[target] = joined;
                    changed = true;
                });
            }
        }

        for(U32 b = 0; b < blockCount; b++) {
            auto action = StepPlan::kSpillNothing;
            auto block = body.block(mir, BlockId(b));

            // A use is what `LiveAcross` counts as one: an operand naming the value, an index or a
            // raw address a place is reached through, and an edge's argument - and a hand-over that
            // keeps it, which stores it into its field.
            auto uses = false;
            auto useOf = [&](mir::Operand& operand) {
                if(operand.source() == mir::SourceKind::Result) {
                    if(operand.nodeId().index == value) uses = true;
                    return;
                }
                if(operand.source() != mir::SourceKind::Place) return;

                for(auto step = body.place(mir, operand.placeId()); step;
                    step = step->parent ? mir[step->parent] : nullptr) {
                    if((step->kind == mir::Place::Index || step->kind == mir::Place::Address) &&
                       step->payload == value) {
                        uses = true;
                    }
                }
            };

            for(auto pointer: block->nodes.contents(mir)) {
                auto& node = *mir[pointer];
                auto k = plan.yieldOf[node.id.index];
                if(k != StepPlan::kNotAYield && plan.record->keepsAcross(decl, plan.spillEntry[j], k)) {
                    uses = true;
                }

                mir::forEachOperand(mir, node, [&](mir::Operand& operand, mir::OperandRole) {
                    useOf(operand);
                });
                mir::forEachEdge(mir, node, [&](mir::Edge& edge) {
                    for(auto operand: edge.arguments.contents(mir)) useOf(operand);
                });
            }

            auto state = entry[b];
            if(b != home && uses && state != kUndefined) {
                if(state == kDefinition && home < b) {
                    action = StepPlan::kSpillFromDefinition;
                } else if(state != kDefinition && state != kSeveral && yieldBlock(state) < b) {
                    action = state;
                } else {
                    action = StepPlan::kSpillLoad;
                    plan.spillStoreAtDefinition[j] = 1;
                }
            }

            plan.spillAction.push(action);
        }
    }
}

/*
 * The two generated bodies that are **not** a reading of the producer - `f$new` and `f$drop`.
 *
 * `f$new` is the construction, and it is here rather than in MIR because of what it does *not* do:
 * a record with its captured arguments in it and its payload untouched is a shape MIR has no write
 * for, since every write it has initializes something and a half-initialized record is not a thing
 * the move paths can describe. Here there is no such question - the storage is allocated, the fields
 * the call supplied are stored into, and what the record owes is answered by its own teardown rather
 * than by a walk over its members.
 *
 * `f$drop` is one call of the step with `$cancel`, which is §3.3's whole teardown.
 */
static bool emitIterNew(Context& context, mir::Program& program, SsaArena& arena,
                        mir::Function& function) {
    auto decl = *program.decl;
    auto recordType = function.generatedRecord;
    auto at = function.source;

    Editor editor(context, program, arena, function);
    auto entry = editor.currentBlock();

    for(auto argumentPointer: function.args.contents(decl)) {
        auto& declared = *decl[argumentPointer];
        editor.addBlockArgument(*entry, declared.occurrence, conventionFor(decl, declared.occurrence));
    }

    auto held = mir::owned(recordType);
    auto slot = editor.resultOf(*editor.addAlloc(at, held, mir::StorageClass::Frame));

    auto fieldAt = [&](U16 field) {
        return editor.resultOf(*editor.addFieldAddress(at, editor.use(*slot, UseKind::Inspect),
                                                       recordType, field));
    };

    // The index, at zero: a record that has not started, which is what §2.6 says a call produces.
    auto zero = editor.addConst(at, mir::intConstant(program.types.exitCode(), 0));
    editor.addStore(at, editor.use(*fieldAt(0), UseKind::Inspect),
                    editor.use(*editor.resultOf(*zero), UseKind::Consume));

    for(U32 i = 0; i < function.args.size(); i++) {
        auto& declared = *decl[function.args.get(decl, i)];
        auto incoming = (*arena)[entry->argumentAt(*arena, i)];
        auto stored = mir::owned(((mir::RecordType*)decl[recordType])->field(decl, U16(2 + i))
                                     .occurrence.type);

        /*
         * A **window** is restated as the descriptor before it is stored.
         *
         * The argument arrives at `Array(a) [shared slot n view]`, which is two things at once to
         * the tiers below: a view record to the manifestation table and a *reference* to every rule
         * that reads the published origin first. What the field holds is the descriptor, so the
         * store is made of a value that says only that - `represent`, which moves no bytes and is
         * the same pair the deleted stack handle's entry made when it restated a captured window the
         * other way (`../../doc/progress/2026-09-16-J55-iterator-objects.md`).
         */
        if(declared.occurrence.contract.form() == mir::AccessForm::View) {
            auto window = editor.addUnary(at, mir::UnaryOp::Represent,
                                          editor.use(*incoming, UseKind::Inspect), stored,
                                          conventionFor(decl, stored));

            editor.addStore(at, editor.use(*fieldAt(U16(2 + i)), UseKind::Inspect),
                            editor.use(*editor.resultOf(*window), UseKind::Consume));
            continue;
        }

        UseKind kind = declared.occurrence.contract.isOwned() ? UseKind::Consume : UseKind::Inspect;
        editor.addStore(at, editor.use(*fieldAt(U16(2 + i)), UseKind::Inspect),
                        editor.use(*incoming, kind));
    }

    /*
     * The **runs** a `@heap`-placed payload member lives in - one allocation each, for the object's
     * whole life, with the field holding the address (`RecordType::kHeapMember`).
     *
     * Here rather than at the step that first reaches the binding, which would need a flag per
     * member saying whether there has been one. The record owes the frees either way, and the step
     * is where they are made - `stepReleaseRuns`.
     */
    auto& made = *(mir::RecordType*)decl[recordType];

    for(U32 j = 0; j < made.payloadLocals.size(); j++) {
        auto member = made.payloadLocals.get(decl, j);
        if(!(member & mir::RecordType::kHeapMember)) continue;

        // The field this entry lives in, which is no longer its position: slot sharing put two
        // members in one field where their ranges never met - `RecordType::payloadFields`. A placed
        // member never shares, so this is always a field of its own, and it is read the same way.
        auto index = made.payloadFields.get(decl, j);
        auto& pointer = *(mir::PointerType*)decl[made.field(decl, index).occurrence.type];
        auto run = mir::owned(pointer.to);

        auto allocated = editor.resultOf(*editor.addAlloc(at, run, mir::StorageClass::Heap));
        editor.addStore(at, editor.use(*fieldAt(index), UseKind::Inspect),
                        editor.use(*allocated, UseKind::Inspect));
    }

    /*
     * And every **nested traversal** at the finished index - `RecordType::kNestMember`, §3.2's first
     * step, and what makes the outer object's teardown a call it can always make.
     *
     * A record that has not started has entered none of its nested traversals, so what the member
     * holds is not a child at index zero - which would be one whose captured arguments the
     * cancellation would release - but one that is *over*. The step writes a real child where the
     * call is embedded and cancels it where the region is left; this is what stands in the field
     * until then, and it is why `stepCancelField` needs no flag beside the member.
     */
    U32 box = 0;

    for(U32 j = 0; j < made.payloadLocals.size(); j++) {
        auto member = made.payloadLocals.get(decl, j);
        if(!(member & mir::RecordType::kNestMember)) continue;

        auto index = made.payloadFields.get(decl, j);
        auto nested = made.field(decl, index).occurrence.type;

        auto shim = box < made.nestBoxed.size() ? made.nestBoxed.get(decl, box)
                                                : DeclPtr<mir::Function>(nullptr);
        box++;

        /*
         * A **boxed** level is the empty pair rather than a finished index - §6.3.
         *
         * There is no child record in the field to write an index into: the member is two words, and
         * what says "this level is not there" is an environment word of null, which is what a
         * drop asks first. So a cancellation that reaches an unentered box drops a pair that owns
         * nothing, which is the same "a call that does nothing" the finished index buys one row up.
         * The code word is the real shim either way, so the pair is never a value that could be
         * stepped into nothing.
         */
        /*
         * A **delegating** member is the link rather than the bare pair - §6.3,
         * `RecordType::linkType`. Its pair starts empty for the reason every box's does, and its
         * chain word starts at zero: nothing has pushed this level onto a chain yet, and what would
         * is a trampoline three calls away.
         */
        if(nested == made.linkType) {
            auto& link = *(mir::RecordType*)decl[nested];
            auto boxType = link.field(decl, 0).occurrence.type;

            if(!shim) continue;

            auto pair = editor.resultOf(*editor.addFieldAddress(at, editor.use(*fieldAt(index),
                                                                               UseKind::Inspect),
                                                                nested, 0));

            auto occurrence = mir::owned(boxType);
            auto empty = editor.resultOf(*editor.addFunValue(at, shim, nullptr, occurrence,
                                                             conventionFor(decl, occurrence)));

            editor.addStore(at, editor.use(*pair, UseKind::Inspect),
                            editor.use(*empty, UseKind::Consume));

            auto word = mir::owned(program.types.words[0]);
            auto up = editor.resultOf(*editor.addFieldAddress(at, editor.use(*fieldAt(index),
                                                                             UseKind::Inspect),
                                                              nested, 1));

            auto none = editor.addConst(at, mir::intConstant(word.type, 0));
            editor.addStore(at, editor.use(*up, UseKind::Inspect),
                            editor.use(*editor.resultOf(*none), UseKind::Consume));
            continue;
        }

        if(decl[nested]->kind == mir::Type::Iter) {
            if(!shim) continue;

            auto occurrence = mir::owned(nested);
            auto empty = editor.resultOf(*editor.addFunValue(at, shim, nullptr, occurrence,
                                                             conventionFor(decl, occurrence)));

            editor.addStore(at, editor.use(*fieldAt(index), UseKind::Inspect),
                            editor.use(*empty, UseKind::Consume));
            continue;
        }

        auto inside = editor.resultOf(*editor.addFieldAddress(at, editor.use(*fieldAt(index),
                                                                             UseKind::Inspect),
                                                              nested, 0));

        auto over = editor.addConst(at, mir::intConstant(program.types.exitCode(), kIterFinished));
        editor.addStore(at, editor.use(*inside, UseKind::Inspect),
                        editor.use(*editor.resultOf(*over), UseKind::Consume));
    }

    /*
     * And **delegation's** two words at zero - `$down`, which is the step's third answer and says
     * "nothing entered", and `$inner`, which is the trampoline's own state and says "this record is
     * the innermost level there is".
     */
    if(made.linkType) {
        auto word = mir::owned(program.types.words[0]);
        auto none = editor.addConst(at, mir::intConstant(word.type, 0));

        editor.addStore(at, editor.use(*fieldAt(made.downField), UseKind::Inspect),
                        editor.use(*editor.resultOf(*none), UseKind::Consume));

        auto inner = editor.addConst(at, mir::intConstant(word.type, 0));
        editor.addStore(at, editor.use(*fieldAt(made.innerField), UseKind::Inspect),
                        editor.use(*editor.resultOf(*inner), UseKind::Consume));
    }

    // And `$held` at false, where the record has one: a record that has not started has handed
    // nothing over, so the slot owes nothing - `RecordType::heldField`.
    if(auto flag = made.heldField) {
        auto empty = editor.addConst(at, mir::truthConstant(program, false));
        editor.addStore(at, editor.use(*fieldAt(flag), UseKind::Inspect),
                        editor.use(*editor.resultOf(*empty), UseKind::Consume));
    }

    auto loaded = editor.addLoad(at, editor.use(*slot, UseKind::Inspect), held,
                                 conventionFor(decl, held), false);

    Use answer[1] = { editor.use(*editor.resultOf(*loaded), UseKind::Consume) };
    editor.addReturn(at, answer, 1);
    editor.seal();
    return true;
}

static bool emitIterDrop(Context& context, mir::Program& program, SsaArena& arena,
                         mir::Function& function) {
    auto decl = *program.decl;
    auto& record = *(mir::RecordType*)decl[function.generatedRecord];
    auto at = function.source;

    Editor editor(context, program, arena, function);
    auto entry = editor.currentBlock();

    auto& declared = *decl[function.args.get(decl, 0)];
    editor.addBlockArgument(*entry, declared.occurrence, conventionFor(decl, declared.occurrence));

    auto incoming = (*arena)[entry->argumentAt(*arena, 0)];
    auto& step = *decl[record.producerStep];

    /*
     * The subject, into storage of its own - which is what consumes it, and is exactly what an
     * authored teardown's body does with its parameter.
     *
     * A teardown's subject is not an ownership root of its own frame (`Function::teardown`), so the
     * slot is never released: the value ceases to exist here and its members are this body's to
     * dispose of, which is what the step below does.
     */
    auto held = mir::owned(function.generatedRecord);
    auto slot = editor.resultOf(*editor.addAlloc(at, held, mir::StorageClass::Frame));
    editor.addStore(at, editor.use(*slot, UseKind::Inspect),
                    editor.use(*incoming, UseKind::Consume));

    auto borrow = mir::borrowed(function.generatedRecord, mir::Access::Unique);
    auto self = editor.resultOf(*editor.addLoad(at, editor.use(*slot, UseKind::Inspect), borrow,
                                                conventionFor(decl, borrow), false));

    /*
     * "Leave", at the type the step's own argument is - the continuation's answer, one tier over.
     *
     * A resuming producer's step takes `Outcome(R, exit)` and a plain one takes the signal, and the
     * `yield` reads whichever it is exactly as it reads a continuation's return. `exitRow` is what
     * puts the row in whichever of the two this producer declared - written out here until
     * 2026-09-16 as the bare signal, which made *every* resuming producer that owned anything fail
     * to compile, since owning anything is what gives a record this teardown at all.
     */
    auto leave = exitRow(program, editor, decl[step.args.get(decl, 1)]->occurrence.type, kExitLeave,
                         at);
    auto cancel = editor.addConst(at, mir::truthConstant(program, true));

    mir::CallSite site;
    site.target = record.producerStep;
    site.through = mir::CallThrough::Declaration;

    Use arguments[3] = {
        editor.use(*self, UseKind::Borrow),
        leave,
        editor.use(*editor.resultOf(*cancel), UseKind::Consume),
    };

    editor.addCall(at, site, Use(), arguments, 3, step.result, conventionFor(decl, step.result), true);
    editor.addReturn(at, nullptr, 0);
    editor.seal();
    return true;
}

/*
 * `Iterate`'s two members - `f$next` and `f$resume`, §2.3's class.
 *
 *      %more = f$step(self, <what the pass answered>, False)
 *      branch %more -> just -> nothing
 *
 * The item comes straight out of the slot, which is what makes the class's reading of a hand-over
 * one reading: the record holds the item between two steps whatever the producer declared, so a body
 * compiled once takes it by value. A producer that *lends* has no instance of the class at all -
 * `resolve_mir/resolve_object.cpp` says why, and `for x in it` is the spelling that honours a
 * lending convention.
 *
 * **`next` on a resuming producer is its first step, and only that.** What it sends is
 * `Exit(kExitFallOut)`, which at index zero is never read - the producer has not reached a `yield`.
 * At a **suspend** index it is a pass that answered nothing, and that is a failure rather than an
 * early end: the producer asked for a value and the consumer wrote the member that has none. A
 * witness cannot know which producer it holds, so the check is the index and it is here;
 * `refuseNextOnResuming` is the same rule where the site type is written out, reported instead of
 * run. So "`next` starts it and `resume` answers each `yield`" is the rule, and this is what
 * enforces it.
 */
// The tail both steppers end with - see the definition, below the first of them.
static bool answerItem(Context& context, mir::Program& program, Editor& editor,
                       mir::Function& function, Value* slot, DeclPtr<mir::Type> stored, bool window,
                       Block& nothing, LocationId at, Value* failing = nullptr,
                       bool stepRows = false);

/*
 * Where a record's item is - **the slot's address**, whatever the slot holds.
 *
 * The one line `f$erased`, `f$erased.step` and `f$walk` share and the one thing erasure changes
 * about a step: a pair carries no record for the consumer to read a field out of, so what comes back
 * has to be the address the field would have been read at. A window slot holds the descriptor rather
 * than the owner, so the bitcast to the result's own type covers both and lowers to nothing.
 *
 * A **lending** slot holds the value's address, and the answer is still the slot's rather than the
 * value's: the consumer loads it (`iter(T)$item`, `stepBoxItem`, a delegating `f$item`). Answering
 * the value's own address saved that load and made the channel unsound - an `&U8` item is at any
 * byte, and bit zero is what tells an item from `&$failure | 1` and from a delegating level's tagged
 * link. A slot is laid out at least two-aligned (`membersOf`), so the slot's address never has it.
 */
static Value* iterSlotAddress(mir::Program& program, Editor& editor, mir::Function& function,
                              Value* self, LocationId at) {
    auto decl = *program.decl;
    auto slot = editor.resultOf(*editor.addFieldAddress(at, editor.use(*self, UseKind::Inspect),
                                                        function.generatedRecord, 1));

    return editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Bitcast,
                                            editor.use(*slot, UseKind::Inspect), function.result,
                                            conventionFor(decl, function.result)));
}

// `null()` - the same `bitcast(0 :: Size)` `lib/Native/Pointer.yana` writes, which is what a producer
// that ran out answers and is the one value no item can have.
static Value* iterNoItem(mir::Program& program, Editor& editor, mir::Function& function,
                         LocationId at) {
    auto decl = *program.decl;
    auto zero = editor.addConst(at, mir::intConstant(program.types.words[0], 0));

    return editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Bitcast,
                                            editor.use(*editor.resultOf(*zero), UseKind::Inspect),
                                            function.result, conventionFor(decl, function.result)));
}

/*
 * The failure an erased step answered, moved out - C1c, J218. `answer` is `&$failure | 1`: untagged
 * by a subtract, since the tag was set by an `or` on a word that had it clear, and read as the
 * `Outcome(exit, E)` every record behind an `iter(T | E)` holds. The shim finished the record before
 * it answered, which is what hands the `Exit` payload over in place (`f$failure`'s contract).
 *
 * **Why bit zero is free** - two halves. The failure's address has it clear because an
 * `Outcome(exit, E)` holds the `I32` exit code in its `Proceed` arm, so it is at least four-aligned
 * whatever `E` is (`{}` and a one-byte `E` included, which `iter/Failing.ErasedType.yana` runs). And
 * no *item* answer has it set because an item answer is a slot's address and a slot is at least
 * two-aligned (`iterSlotAddress`, `membersOf`) - the half J218 did not state, and an odd lent `&U8`
 * read as a failure until it held (`iter/Failing.ErasedOddItem.yana`).
 */
static Value* iterErasedReason(mir::Program& program, Editor& editor, Value* answer,
                               const mir::IterType& iterated, LocationId at) {
    auto decl = *program.decl;
    auto word = mir::owned(program.types.words[0]);

    auto number = editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Bitcast,
                                                   editor.use(*answer, UseKind::Inspect), word,
                                                   conventionFor(decl, word)));
    auto one = editor.addConst(at, mir::intConstant(word.type, 1));
    auto bare = editor.addBinary(at, mir::BinaryOp::Sub, editor.use(*number, UseKind::Inspect),
                                 editor.use(*editor.resultOf(*one), UseKind::Inspect), word,
                                 conventionFor(decl, word));

    auto held = mir::borrowed(iterated.failed, mir::Access::Unique);
    auto outcomeAt = editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Bitcast,
                                                      editor.use(*editor.resultOf(*bare),
                                                                 UseKind::Inspect),
                                                      held, conventionFor(decl, held)));

    auto& outcome = *(mir::RecordType*)decl[iterated.failed];
    auto reason = outcome.fields.get(decl, outcome.cons.get(decl, mir::kOutcomeExit).first);
    auto taken = mir::owned(reason.occurrence.type);

    // A `{}` failure is its one value and never a load - `unitValue`'s rule, which a zero-byte load
    // is what `lower/inst.cpp` asserts against.
    if(decl[taken.type]->kind == mir::Type::Unit) {
        return editor.resultOf(*editor.addConst(at, mir::unitConstant(taken.type)));
    }

    auto address = editor.addFieldAddress(at, editor.use(*outcomeAt, UseKind::Inspect),
                                          iterated.failed, reason.index);
    auto payload = editor.use(*editor.resultOf(*address), UseKind::Inspect);

    /*
     * A payload that lives in storage is **relocated into the frame**, not left where it is (K7): a
     * load of one is its address, so the reason would go on naming the record's bytes - and every
     * caller releases the record before the reason is consumed. `stepBoxFailure` frees the box on
     * the next line, and a consumer owning a heap-boxed object drops it right after
     * `iter(T | E)$failure`, which is where J220's reason read as zero once the call was inlined.
     *
     * A trivially copyable payload is a `Copy` out of the loaded address - the conversion's own
     * answer for a relocation out of an `Address` place (`copies` in the `Read` row). A store and a
     * load through a frame slot was tried first and is not enough: `forwardMemory` hands a `Trivial`
     * value straight through the slot, which is the alias again. One with a teardown (a `Tag`
     * reason) has no `Copy`, and is the `Owned` handover `forwardMemory` declines, so the slot is
     * where it lives. A register-sized payload is a real load and needs nothing.
     */
    auto aggregate = holdsAggregate(decl, taken.type);

    if(aggregate && mir::isTriviallyCopyableAt(decl, taken)) {
        return editor.resultOf(*editor.addCopy(at, payload, taken, conventionFor(decl, taken)));
    }

    auto loaded = editor.resultOf(*editor.addLoad(at, payload, taken, conventionFor(decl, taken),
                                                  false));
    if(!aggregate) return loaded;

    auto slot = editor.resultOf(*editor.addAlloc(at, taken, mir::StorageClass::Frame));
    editor.addStore(at, editor.use(*slot, UseKind::Inspect), editor.use(*loaded, UseKind::Consume));

    return editor.resultOf(*editor.addLoad(at, editor.use(*slot, UseKind::Inspect), taken,
                                           conventionFor(decl, taken), false));
}

// Whether an erased answer carries the failure tag - the `and` a consumer of `iter(T | E)` pays per
// element, and nothing on a protocol that cannot fail because nobody asks.
static Value* iterAnswerTagged(mir::Program& program, Editor& editor, Value* answer, LocationId at) {
    auto decl = *program.decl;
    auto word = mir::owned(program.types.words[0]);

    auto number = editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Bitcast,
                                                   editor.use(*answer, UseKind::Inspect), word,
                                                   conventionFor(decl, word)));
    auto one = editor.addConst(at, mir::intConstant(word.type, 1));
    auto tag = editor.addBinary(at, mir::BinaryOp::And, editor.use(*number, UseKind::Inspect),
                                editor.use(*editor.resultOf(*one), UseKind::Inspect), word,
                                conventionFor(decl, word));

    auto zero = editor.addConst(at, mir::intConstant(word.type, 0));
    auto truth = mir::owned(mir::booleanType(program));
    return editor.resultOf(*editor.addBinary(at, mir::BinaryOp::Ne,
                                             editor.use(*editor.resultOf(*tag), UseKind::Inspect),
                                             editor.use(*editor.resultOf(*zero), UseKind::Inspect),
                                             truth, conventionFor(decl, truth)));
}

/*
 * A failing record's "no" said on the erased channel - C1c, J218. Emitted where the step answered
 * no, so an element costs nothing for it; the index says which no it was.
 *
 * `f$erased` (`raw` false): `kIterFailed` finishes the record and answers `&$failure | 1`, handing
 * the payload over in place; anything else answers null.
 *
 * `f$erased.step` (`raw` true) answers **null** at `kIterFailed` and parks the record at
 * `kIterFailedHeld`, because its caller is a trampoline, whose one reading of a tagged answer is "a
 * child was entered" and whose reading of null - pop this level, step the one above - is exactly what
 * a failure needs. The level above then asks the same pair again from its `delegateResume`, and that
 * second call finds `kIterFailedHeld` and answers the tagged failure to the one caller who expects it.
 */
static void iterAnswerFailed(mir::Program& program, Editor& editor, mir::Function& function,
                             Value* self, bool raw, LocationId at) {
    auto decl = *program.decl;
    auto& record = *(mir::RecordType*)decl[function.generatedRecord];

    auto counter = mir::owned(program.types.exitCode());
    auto index = editor.resultOf(*editor.addFieldAddress(at, editor.use(*self, UseKind::Inspect),
                                                         function.generatedRecord, 0));
    auto loaded = editor.resultOf(*editor.addLoad(at, editor.use(*index, UseKind::Inspect), counter,
                                                  conventionFor(decl, counter), false));

    auto truth = mir::owned(mir::booleanType(program));
    auto is = [&](I64 value) {
        auto constant = editor.addConst(at, mir::intConstant(counter.type, U64(value)));
        return editor.resultOf(*editor.addBinary(at, mir::BinaryOp::Eq,
                                                 editor.use(*loaded, UseKind::Inspect),
                                                 editor.use(*editor.resultOf(*constant),
                                                            UseKind::Inspect),
                                                 truth, conventionFor(decl, truth)));
    };

    auto store = [&](I64 value) {
        auto constant = editor.addConst(at, mir::intConstant(counter.type, U64(value)));
        auto slot = editor.resultOf(*editor.addFieldAddress(at, editor.use(*self, UseKind::Inspect),
                                                            function.generatedRecord, 0));
        editor.addStore(at, editor.use(*slot, UseKind::Inspect),
                        editor.use(*editor.resultOf(*constant), UseKind::Consume));
    };

    auto answerNull = [&]() {
        Use none[1] = { editor.use(*iterNoItem(program, editor, function, at), UseKind::Consume) };
        editor.addReturn(at, none, 1);
    };

    auto answerTagged = [&]() {
        store(kIterFinished);

        auto word = mir::owned(program.types.words[0]);
        auto slot = editor.resultOf(*editor.addFieldAddress(at, editor.use(*self, UseKind::Inspect),
                                                            function.generatedRecord,
                                                            record.failureField));
        auto number = editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Bitcast,
                                                       editor.use(*slot, UseKind::Inspect), word,
                                                       conventionFor(decl, word)));
        auto one = editor.addConst(at, mir::intConstant(word.type, 1));
        auto tagged = editor.addBinary(at, mir::BinaryOp::Or, editor.use(*number, UseKind::Inspect),
                                       editor.use(*editor.resultOf(*one), UseKind::Inspect), word,
                                       conventionFor(decl, word));
        auto answer = editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Bitcast,
                                                       editor.use(*editor.resultOf(*tagged),
                                                                  UseKind::Inspect),
                                                       function.result,
                                                       conventionFor(decl, function.result)));
        Use answered[1] = { editor.use(*answer, UseKind::Consume) };
        editor.addReturn(at, answered, 1);
    };

    auto failed = editor.addBlock(StringId());
    auto other = editor.addBlock(StringId());
    editor.addBranch(at, editor.use(*is(kIterFailed), UseKind::Inspect), *failed, *other, nullptr, 0);

    editor.setBlock(*failed);
    editor.setRegion(failed->region);

    if(raw) {
        store(kIterFailedHeld);
        answerNull();
    } else {
        answerTagged();
    }

    editor.setBlock(*other);
    editor.setRegion(other->region);

    if(!raw) {
        answerNull();
        return;
    }

    auto held = editor.addBlock(StringId());
    auto none = editor.addBlock(StringId());
    editor.addBranch(at, editor.use(*is(kIterFailedHeld), UseKind::Inspect), *held, *none, nullptr,
                     0);

    editor.setBlock(*held);
    editor.setRegion(held->region);
    answerTagged();

    editor.setBlock(*none);
    editor.setRegion(none->region);
    answerNull();
}

// One word of a record, loaded and stored - `$down` and `$inner`, which are the two delegation adds.
static Value* iterWordAt(mir::Program& program, Editor& editor, Value* self,
                         DeclPtr<mir::Type> recordType, U16 field, LocationId at) {
    auto decl = *program.decl;
    auto word = mir::owned(program.types.words[0]);

    auto address = editor.resultOf(*editor.addFieldAddress(at, editor.use(*self, UseKind::Inspect),
                                                           recordType, field));

    return editor.resultOf(*editor.addLoad(at, editor.use(*address, UseKind::Inspect), word,
                                           conventionFor(decl, word), false));
}

static void iterStoreWordAt(Editor& editor, Value* self, DeclPtr<mir::Type> recordType, U16 field,
                            Value* value, LocationId at) {
    auto address = editor.resultOf(*editor.addFieldAddress(at, editor.use(*self, UseKind::Inspect),
                                                           recordType, field));

    editor.addStore(at, editor.use(*address, UseKind::Inspect), editor.use(*value, UseKind::Consume));
}

/*
 * A **chain word** turned into the link address it names - `$inner`, `$up` and `$down` read, which
 * is the one place delegation's representation is not simply an address.
 *
 * A chain word holds either the address of a link that lives in a **heap block** - every level below
 * the root is boxed, and a block never moves - or, with **bit zero set**, `field << 1 | 1`: the
 * index of one of *this* record's own link members.
 *
 * The second reading is what makes a suspended delegating object relocatable, and it is the only
 * link in the chain that needs one. `let moved = it`, an argument taken by `->`, a `return` and a
 * store into a field all relocate the record a consumer holds, and the design's §6 is that a record
 * moves with whatever holds it - so an absolute address into the root's own link member, which is
 * what J60 stored, named where the object used to be from the next step onwards. Naming the field
 * instead is resolved against whatever `self` the step was entered with, so a root that has moved
 * finds its own link exactly where it now is.
 *
 * **The invariant that makes one reading enough**: the only word that ever enters the chain with the
 * tag set is `$self.$down`, written by this record's own step about one of this record's own fields,
 * and read by `f$walk` and `f$erased.step` with that same `self` in hand. Every other value pushed -
 * a level's tagged answer, minus its tag - is an address inside a block. So a tagged word in `$inner`
 * or in a level's `$up` always names a field of the **root**, and is resolved against the root.
 *
 * `@heap` placement of the link member was the alternative and is J60's own suggestion; it costs an
 * allocation and a free per level entered, which is the number §6.3's second bullet exists to bring
 * *down*. This costs one compare and one branch-free select per chain word read, and nothing at all
 * to a producer that does not delegate.
 */
static Value* iterLinkAddress(mir::Program& program, Editor& editor, Value* self,
                              DeclPtr<mir::Type> recordType, Value* word, LocationId at) {
    auto decl = *program.decl;
    auto& record = *(mir::RecordType*)decl[recordType];
    auto held = mir::owned(program.types.words[0]);
    auto truth = mir::owned(mir::booleanType(program));
    auto resolved = word;

    // One select per link member the record has, which is one in every producer the corpus holds:
    // a level is suspended inside exactly one region, so at most one of these can be the live
    // reading - but which one is a fact about the index and a compare against the word is cheaper
    // than recovering it.
    for(auto field: record.fields.contents(decl)) {
        if(field.occurrence.type != record.linkType) continue;

        auto named = editor.addConst(at, mir::intConstant(held.type,
                                                          (U64(field.index) << 1) | 1));

        auto is = editor.resultOf(*editor.addBinary(at, mir::BinaryOp::Eq,
                                                    editor.use(*word, UseKind::Inspect),
                                                    editor.use(*editor.resultOf(*named),
                                                               UseKind::Inspect),
                                                    truth, conventionFor(decl, truth)));

        auto own = editor.resultOf(*editor.addFieldAddress(at, editor.use(*self, UseKind::Inspect),
                                                           recordType, field.index));

        auto number = editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Bitcast,
                                                       editor.use(*own, UseKind::Inspect), held,
                                                       conventionFor(decl, held)));

        resolved = editor.resultOf(*editor.addSelect(at, editor.use(*is, UseKind::Inspect),
                                                     editor.use(*number, UseKind::Inspect),
                                                     editor.use(*resolved, UseKind::Inspect),
                                                     held, conventionFor(decl, held)));
    }

    return resolved;
}

// Whether a word is nonzero, which is what every test in the two bodies below is.
static Value* iterNonZero(mir::Program& program, Editor& editor, Value* word, LocationId at) {
    auto decl = *program.decl;
    auto zero = editor.addConst(at, mir::intConstant(program.types.words[0], 0));
    auto truth = mir::owned(mir::booleanType(program));

    return editor.resultOf(*editor.addBinary(at, mir::BinaryOp::Ne,
                                             editor.use(*word, UseKind::Inspect),
                                             editor.use(*editor.resultOf(*zero), UseKind::Inspect),
                                             truth, conventionFor(decl, truth)));
}

/*
 * `f$erased.step` - the **raw** shim, and the one function that ever writes a step's third row -
 * Design-Iterator-Objects.md §6.3, built 2026-09-16.
 *
 *      %more = f$step($env, $sent, $cancel)
 *      just:  %down = load $env.$down
 *             if %down != 0: store $env.$down <- 0; return link($env, %down) | 1
 *             return <where the item is>
 *      nothing: return null
 *
 * `f$erased` and this differ in those three lines and nothing else. **Which shim a pair names is how
 * the third row is kept off every channel with no use for it**: a delegable box's pair names this
 * one and is stepped only by a trampoline, and every pair a consumer holds - and every box a
 * non-delegating region builds - names `f$erased`, which cannot answer it. So a step that never
 * delegates pays nothing at all for delegation existing, which is the reason this row is a member of
 * the record rather than a wider answer on the step.
 *
 * The **low bit** is free because a link is a field of a record and is word-aligned, which is the
 * same test `funTeardown` makes of a pair's environment word one construct over.
 */
static bool emitIterErasedRaw(Context& context, mir::Program& program, SsaArena& arena,
                              mir::Function& function) {
    auto decl = *program.decl;
    auto& record = *(mir::RecordType*)decl[function.generatedRecord];
    auto& step = *decl[record.producerStep];
    auto at = function.source;

    Editor editor(context, program, arena, function);
    auto entry = editor.currentBlock();

    for(auto argumentPointer: function.args.contents(decl)) {
        auto& declared = *decl[argumentPointer];
        editor.addBlockArgument(*entry, declared.occurrence, conventionFor(decl, declared.occurrence));
    }

    auto self = (*arena)[entry->argumentAt(*arena, 0)];
    auto sent = (*arena)[entry->argumentAt(*arena, 1)];
    auto cancel = (*arena)[entry->argumentAt(*arena, 2)];

    mir::CallSite site;
    site.target = record.producerStep;
    site.through = mir::CallThrough::Declaration;

    Use arguments[3] = {
        editor.use(*self, UseKind::Borrow),
        editor.use(*sent, UseKind::Consume),
        editor.use(*cancel, UseKind::Consume),
    };

    auto more = editor.addCall(at, site, Use(), arguments, 3, step.result,
                               conventionFor(decl, step.result), true);

    auto just = editor.addBlock(StringId());
    auto nothing = editor.addBlock(StringId());

    editor.addBranch(at, editor.use(*editor.resultOf(*more), UseKind::Inspect), *just, *nothing,
                     nullptr, 0);

    editor.setBlock(*just);
    editor.setRegion(just->region);

    auto down = iterWordAt(program, editor, self, function.generatedRecord, record.downField, at);

    auto entered = editor.addBlock(StringId());
    auto handed = editor.addBlock(StringId());

    editor.addBranch(at, editor.use(*iterNonZero(program, editor, down, at), UseKind::Inspect),
                     *entered, *handed, nullptr, 0);

    editor.setBlock(*entered);
    editor.setRegion(entered->region);

    auto word = mir::owned(program.types.words[0]);
    auto clear = editor.addConst(at, mir::intConstant(word.type, 0));

    iterStoreWordAt(editor, self, function.generatedRecord, record.downField,
                    editor.resultOf(*clear), at);

    /*
     * The link this level has entered, **resolved** before it crosses the boundary: what the step
     * wrote into `$down` names a field of this record (`iterLinkAddress`), and the trampoline that
     * reads this answer holds a different `self` and could not resolve it. An address into this
     * record is safe to hand out because a delegable box never moves - it is a heap block, which is
     * what `RecordType::producerRaw`'s own comment says a pair naming this shim always is.
     */
    auto reached = iterLinkAddress(program, editor, self, function.generatedRecord, down, at);

    auto one = editor.addConst(at, mir::intConstant(word.type, 1));
    auto tagged = editor.addBinary(at, mir::BinaryOp::Or, editor.use(*reached, UseKind::Inspect),
                                   editor.use(*editor.resultOf(*one), UseKind::Inspect), word,
                                   conventionFor(decl, word));

    auto answer = editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Bitcast,
                                                   editor.use(*editor.resultOf(*tagged),
                                                              UseKind::Inspect),
                                                   function.result,
                                                   conventionFor(decl, function.result)));

    Use answerEntered[1] = { editor.use(*answer, UseKind::Consume) };
    editor.addReturn(at, answerEntered, 1);

    editor.setBlock(*handed);
    editor.setRegion(handed->region);

    Use answerJust[1] = { editor.use(*iterSlotAddress(program, editor, function, self, at),
                                     UseKind::Consume) };
    editor.addReturn(at, answerJust, 1);

    editor.setBlock(*nothing);
    editor.setRegion(nothing->region);

    // A failing level answers null to the trampoline first and its failure to its parent after -
    // `iterAnswerFailed`, where the two calls are argued.
    if(record.failureField) {
        iterAnswerFailed(program, editor, function, self, true, at);
        editor.seal();
        return true;
    }

    Use answerNone[1] = { editor.use(*iterNoItem(program, editor, function, at), UseKind::Consume) };
    editor.addReturn(at, answerNone, 1);

    editor.seal();
    return true;
}

/*
 * `f$obj` - the **witness's "make an object" entry**, Design-Iterator-Objects.md §6.4.
 *
 *      fn f$obj(<the constructor's parameters>) -> iter(Y)
 *
 *      %r = alloc frame f.Iter
 *      store %r <- f$new(<the parameters>)
 *      return fun.value f$erased.heap inspect %r
 *
 * One per selected instance member, and it is what a body compiled **once** calls where a body
 * compiled per substitution would have built the record itself. The record is the instance's, so the
 * cell is the instance's; what the caller holds is the two words every erased object is.
 *
 * The block is `emitFunValue`'s, exactly as a boxed nest's is (`convertStepNest`) and as an object
 * at an owning position is (`adaptIterValue`): the frame local is where `f$new` writes and the shim's
 * `environmentStorage` is what moves the bytes into a heap block with the record's teardown in front
 * of them. So nothing here allocates by name, and the pair a witness hands back is released by the
 * same `funTeardown` that releases any other.
 */
static bool emitIterObject(Context& context, mir::Program& program, SsaArena& arena,
                           mir::Function& function) {
    auto decl = *program.decl;
    auto& record = *(mir::RecordType*)decl[function.generatedRecord];
    auto& make = *decl[record.producerNew];
    auto at = function.source;

    Editor editor(context, program, arena, function);
    auto entry = editor.currentBlock();

    for(auto argumentPointer: function.args.contents(decl)) {
        auto& declared = *decl[argumentPointer];
        editor.addBlockArgument(*entry, declared.occurrence, conventionFor(decl, declared.occurrence));
    }

    SmallArray<Use, 8> arguments;

    for(U32 i = 0; i < function.args.size() && i < make.args.size(); i++) {
        auto& declared = *decl[make.args.get(decl, U16(i))];

        // The same pair `convertStepNest` reads of an embedded region's capture - what the
        // constructor's position takes, said as a use.
        auto kind = UseKind::Consume;

        switch(mir::argumentMode(declared.occurrence.contract, mir::SourceKind::Result)) {
            case mir::UseMode::BorrowShared:
            case mir::UseMode::BorrowUnique:
                kind = UseKind::Borrow;
                break;
            case mir::UseMode::Inspect:
                kind = UseKind::Inspect;
                break;
            default:
                kind = UseKind::Consume;
                break;
        }

        /*
         * And the **bytes** the constructor's position takes, which is a load where this signature
         * holds a reference and the field holds a copy - `convertStepNest`'s own adaptation, made
         * here for the same reason: this function's parameters are the class member's and the
         * constructor's are the record's, and a call boundary is where the two are reconciled.
         */
        auto value = (*arena)[entry->argumentAt(*arena, i)];

        if(referenceValued(context, program, value->occurrence) &&
           !referenceValued(context, program, declared.occurrence)) {
            auto held = mir::owned(declared.occurrence.type);
            value = editor.resultOf(*editor.addLoad(at, editor.use(*value, UseKind::Inspect), held,
                                                    conventionFor(decl, held), false));
        }

        arguments.push(editor.use(*value, kind));
    }

    mir::CallSite making;
    making.target = record.producerNew;
    making.through = mir::CallThrough::Declaration;

    auto built = editor.resultOf(*editor.addCall(at, making, Use(), arguments.pointer(),
                                                 arguments.size(), make.result,
                                                 conventionFor(decl, make.result), true));

    auto storage = mir::owned(function.generatedRecord);
    auto local = editor.resultOf(*editor.addAlloc(at, storage, mir::StorageClass::Frame));

    editor.addStore(at, editor.use(*local, UseKind::Inspect), editor.use(*built, UseKind::Consume));

    Use environment = editor.use(*local, UseKind::Inspect);
    auto pair = editor.resultOf(*editor.addFunValue(at, record.producerBox, &environment,
                                                    function.result,
                                                    conventionFor(decl, function.result)));

    Use answer[1] = { editor.use(*pair, UseKind::Consume) };
    editor.addReturn(at, answer, 1);

    editor.seal();
    return true;
}

/*
 * `f$walk` - the **trampoline**, §6.3, and the whole of what delegation costs a consumer.
 *
 *      loop:  %at = link($self, load $self.$inner)
 *             if %at == 0:  %more = f$step($self, ...)      -- this record is the innermost
 *                           if !%more: return null
 *                           %down = load $self.$down; if %down: push(%down); goto loop
 *                           return <where this record's item is>
 *             %p = <the pair at %at>($sent, $cancel)        -- the innermost, stepped directly
 *             if %p == 0:   $self.$inner = load %at.$up; goto loop
 *             if %p & 1:    push(%p - 1); goto loop
 *             return %p
 *
 *      push(%link):  store link($self, %link).$up <- load $self.$inner
 *                    store $self.$inner <- %link
 *
 * `link` is `iterLinkAddress`: a chain word is an address inside a heap block, or - tagged - one of
 * `$self`'s own link members, which is what keeps the chain good when the **root is moved** while it
 * is suspended. It is stored untagged into `$inner` and into a level's `$up`, so the reading travels
 * with the word rather than being decided where it is written.
 *
 * **A step is O(1) in the depth of the chain**, which is what §6.3 asks for: the levels between the
 * root and the leaf are not entered at all, because a level that delegates has nothing to run
 * between its child's item and its own hand-over. What each level still costs is the allocation its
 * box is, once, when it is entered.
 *
 * The chain is **intrusive**, through the link each level's parent holds it by
 * (`RecordType::linkType`). A stack the root grew was the alternative: the levels of a cycle are
 * several record types of several sizes, so the root would be keeping an array of pairs it has to
 * grow - an allocation and a library role inside a generated body - where the chain word is one word
 * in storage the level above already owns, already initializes and already frees.
 *
 * Nothing here owns anything. `$inner` and `$up` name links the levels above them own, and a level
 * is released by the level that built it - the box is dropped where the region it belongs to is left,
 * which is `stepCancelField` and is unchanged. So a teardown of the whole object is still one drop
 * of the root record and still recurses down the chain on the machine's stack rather than on this
 * loop.
 */
static bool emitIterWalk(Context& context, mir::Program& program, SsaArena& arena,
                         mir::Function& function) {
    auto decl = *program.decl;
    auto& record = *(mir::RecordType*)decl[function.generatedRecord];
    auto& step = *decl[record.producerStep];
    auto at = function.source;
    auto link = record.linkType;

    Editor editor(context, program, arena, function);
    auto entry = editor.currentBlock();

    for(auto argumentPointer: function.args.contents(decl)) {
        auto& declared = *decl[argumentPointer];
        editor.addBlockArgument(*entry, declared.occurrence, conventionFor(decl, declared.occurrence));
    }

    auto self = (*arena)[entry->argumentAt(*arena, 0)];
    auto sent = (*arena)[entry->argumentAt(*arena, 1)];
    auto cancel = (*arena)[entry->argumentAt(*arena, 2)];

    auto word = mir::owned(program.types.words[0]);
    auto held = mir::borrowed(link, mir::Access::Unique);

    auto loop = editor.addBlock(StringId());
    auto own = editor.addBlock(StringId());
    auto inner = editor.addBlock(StringId());
    auto nothing = editor.addBlock(StringId());

    /*
     * The pushed link arrives as a block argument, which is the one value this loop carries: the
     * two places that push - this record's own step and a level below it - have found it in two
     * different words and everything after that is one piece of code.
     */
    auto push = editor.addBlock(StringId());
    editor.addBlockArgument(*push, word, conventionFor(decl, word));

    editor.addJump(at, *loop, nullptr, 0);

    editor.setBlock(*loop);
    editor.setRegion(loop->region);

    auto innermost = iterWordAt(program, editor, self, function.generatedRecord, record.innerField, at);

    editor.addBranch(at, editor.use(*iterNonZero(program, editor, innermost, at), UseKind::Inspect),
                     *inner, *own, nullptr, 0);

    // This record is the innermost level there is, so the step is its own - the ordinary call, and
    // the one place `f$step`'s `Bool` is still read as a `Bool`.
    editor.setBlock(*own);
    editor.setRegion(own->region);

    mir::CallSite site;
    site.target = record.producerStep;
    site.through = mir::CallThrough::Declaration;

    Use arguments[3] = {
        editor.use(*self, UseKind::Borrow),
        editor.use(*sent, UseKind::Consume),
        editor.use(*cancel, UseKind::Consume),
    };

    auto more = editor.addCall(at, site, Use(), arguments, 3, step.result,
                               conventionFor(decl, step.result), true);

    auto answered = editor.addBlock(StringId());

    editor.addBranch(at, editor.use(*editor.resultOf(*more), UseKind::Inspect), *answered, *nothing,
                     nullptr, 0);

    editor.setBlock(*answered);
    editor.setRegion(answered->region);

    auto down = iterWordAt(program, editor, self, function.generatedRecord, record.downField, at);

    auto handed = editor.addBlock(StringId());
    auto entered = editor.addBlock(StringId());

    editor.addBranch(at, editor.use(*iterNonZero(program, editor, down, at), UseKind::Inspect),
                     *entered, *handed, nullptr, 0);

    // Entered a child of its own: clear the word the step wrote - nothing else reads it - and push.
    editor.setBlock(*entered);
    editor.setRegion(entered->region);

    auto clear = editor.addConst(at, mir::intConstant(word.type, 0));
    iterStoreWordAt(editor, self, function.generatedRecord, record.downField,
                    editor.resultOf(*clear), at);

    Use pushed[1] = { editor.use(*down, UseKind::Consume) };
    editor.addJump(at, *push, pushed, 1);

    editor.setBlock(*handed);
    editor.setRegion(handed->region);

    Use answerOwn[1] = { editor.use(*iterSlotAddress(program, editor, function, self, at),
                                    UseKind::Consume) };
    editor.addReturn(at, answerOwn, 1);

    /*
     * The innermost level, stepped **through its pair** - `CallThrough::Value` against the signature
     * `IterType::step` carries, which is the same call a boxed nest makes. The three rows it may
     * answer are what this loop is for.
     */
    editor.setBlock(*inner);
    editor.setRegion(inner->region);

    // The chain word resolved - it names a link in a block, or one of this record's own members
    // (`iterLinkAddress`), and after a move of the object the second is the only good reading.
    auto named = iterLinkAddress(program, editor, self, function.generatedRecord, innermost, at);

    auto reached = editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Bitcast,
                                                    editor.use(*named, UseKind::Inspect), held,
                                                    conventionFor(decl, held)));

    auto& shape = *(mir::RecordType*)decl[link];
    auto boxType = shape.field(decl, 0).occurrence.type;
    auto& iterated = *(mir::IterType*)decl[boxType];

    auto slot = editor.resultOf(*editor.addFieldAddress(at, editor.use(*reached, UseKind::Inspect),
                                                        link, 0));

    auto borrowed = mir::borrowed(boxType, mir::Access::Unique);
    auto pair = editor.resultOf(*editor.addLoad(at, editor.use(*slot, UseKind::Inspect), borrowed,
                                                conventionFor(decl, borrowed), false));

    mir::CallSite through;
    through.target = nullptr;
    through.through = mir::CallThrough::Value;

    auto result = mir::owned(program.types.pointerType(iterated.handover.type));

    Use stepping[2] = {
        editor.use(*sent, UseKind::Consume),
        editor.use(*cancel, UseKind::Consume),
    };

    auto found = editor.resultOf(*editor.addCall(at, through, editor.use(*pair, UseKind::Inspect),
                                                 stepping, 2, result, conventionFor(decl, result),
                                                 true));

    auto number = editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Bitcast,
                                                   editor.use(*found, UseKind::Inspect), word,
                                                   conventionFor(decl, word)));

    auto alive = editor.addBlock(StringId());
    auto pop = editor.addBlock(StringId());

    editor.addBranch(at, editor.use(*iterNonZero(program, editor, number, at), UseKind::Inspect),
                     *alive, *pop, nullptr, 0);

    editor.setBlock(*alive);
    editor.setRegion(alive->region);

    // The third row - `f$erased.step`'s tagged link, which is the one thing an item answer cannot
    // be, since it is a slot's address and a slot is two-aligned (`iterSlotAddress`). The tag is cleared by subtracting it rather than masking, because it was set by an `or` on
    // a word that had it clear.
    auto one = editor.addConst(at, mir::intConstant(word.type, 1));
    auto tag = editor.addBinary(at, mir::BinaryOp::And, editor.use(*number, UseKind::Inspect),
                                editor.use(*editor.resultOf(*one), UseKind::Inspect), word,
                                conventionFor(decl, word));

    auto deeper = editor.addBlock(StringId());
    auto item = editor.addBlock(StringId());

    editor.addBranch(at, editor.use(*iterNonZero(program, editor, editor.resultOf(*tag), at),
                                    UseKind::Inspect), *deeper, *item, nullptr, 0);

    editor.setBlock(*deeper);
    editor.setRegion(deeper->region);

    auto bare = editor.addBinary(at, mir::BinaryOp::Sub, editor.use(*number, UseKind::Inspect),
                                 editor.use(*editor.resultOf(*one), UseKind::Inspect), word,
                                 conventionFor(decl, word));

    Use below[1] = { editor.use(*editor.resultOf(*bare), UseKind::Consume) };
    editor.addJump(at, *push, below, 1);

    editor.setBlock(*item);
    editor.setRegion(item->region);

    Use answerItem[1] = { editor.use(*found, UseKind::Consume) };
    editor.addReturn(at, answerItem, 1);

    // The level ran out, so the one above it is the innermost again - and the level above it is what
    // the chain word says. Releasing the child is *its* business, and it does it on its next step.
    editor.setBlock(*pop);
    editor.setRegion(pop->region);

    auto up = editor.resultOf(*editor.addFieldAddress(at, editor.use(*reached, UseKind::Inspect),
                                                      link, 1));

    auto above = editor.resultOf(*editor.addLoad(at, editor.use(*up, UseKind::Inspect), word,
                                                 conventionFor(decl, word), false));

    iterStoreWordAt(editor, self, function.generatedRecord, record.innerField, above, at);
    editor.addJump(at, *loop, nullptr, 0);

    // One level pushed - the chain word written from what `$inner` says now, and `$inner` moved down.
    editor.setBlock(*push);
    editor.setRegion(push->region);

    auto below2 = (*arena)[push->argumentAt(*arena, 0)];

    // Resolved to write `$up` through, and pushed **raw**: what goes into `$inner` is the word as it
    // arrived, so that a level named by one of this record's own fields stays named that way.
    auto pushedAt = iterLinkAddress(program, editor, self, function.generatedRecord, below2, at);

    auto chain = editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Bitcast,
                                                  editor.use(*pushedAt, UseKind::Inspect), held,
                                                  conventionFor(decl, held)));

    auto parent = iterWordAt(program, editor, self, function.generatedRecord, record.innerField, at);

    auto chainUp = editor.resultOf(*editor.addFieldAddress(at, editor.use(*chain, UseKind::Inspect),
                                                           link, 1));

    editor.addStore(at, editor.use(*chainUp, UseKind::Inspect),
                    editor.use(*parent, UseKind::Consume));

    iterStoreWordAt(editor, self, function.generatedRecord, record.innerField, below2, at);
    editor.addJump(at, *loop, nullptr, 0);

    editor.setBlock(*nothing);
    editor.setRegion(nothing->region);

    Use answerNone[1] = { editor.use(*iterNoItem(program, editor, function, at), UseKind::Consume) };
    editor.addReturn(at, answerNone, 1);

    editor.seal();
    return true;
}

/*
 * `f$erased` - the one function an erased `{step, block}` pair is called across, §3.4.
 *
 *      fn f$erased(&$env: f.Iter, $sent: S, $cancel: Bool) -> %Y
 *
 *      %more = f$step($env, $sent, $cancel)
 *      branch %more -> just -> nothing
 *      just:     return <where the item is>
 *      nothing:  return null
 *
 * **An address rather than a `Bool` and a slot**, which is the one thing erasure changes about a
 * step: a pair carries no record for the consumer to read a field out of, so what comes back has to
 * be the address the field would have been read at. A lending producer's slot already holds one and
 * every other slot is the value, which is `emitIterStepper`'s own two cases - the difference is only
 * that the consumer of *this* answer cannot tell them apart and does not need to.
 *
 * Shaped as a **lifted lambda**, with the environment first and exclusive, because that is what
 * makes the erased value a closure: `emitFunValue` places the record in the frame or in a heap block
 * with its teardown in front of it, and `funTeardown` releases it from a site that cannot name the
 * producer. `resolve_mir/resolve_object.cpp`'s `iterErasedStep` is where the signature is declared.
 */
static bool emitIterErasedStep(Context& context, mir::Program& program, SsaArena& arena,
                               mir::Function& function) {
    auto decl = *program.decl;
    auto& record = *(mir::RecordType*)decl[function.generatedRecord];
    auto& step = *decl[record.producerStep];
    auto at = function.source;

    Editor editor(context, program, arena, function);
    auto entry = editor.currentBlock();

    for(auto argumentPointer: function.args.contents(decl)) {
        auto& declared = *decl[argumentPointer];
        editor.addBlockArgument(*entry, declared.occurrence, conventionFor(decl, declared.occurrence));
    }

    auto self = (*arena)[entry->argumentAt(*arena, 0)];
    auto sent = (*arena)[entry->argumentAt(*arena, 1)];
    auto cancel = (*arena)[entry->argumentAt(*arena, 2)];

    /*
     * A **delegating** record's pair is its trampoline - §6.3, and the two signatures already agree.
     *
     * `f$walk` answers where the item is, which is what this shim answers, so what a consumer holds
     * over such a record is one call rather than a step and an adaptation. What must *not* be here
     * is `f$step`: an object whose step may say "I entered a child" is one only a trampoline can
     * read, and the pair a consumer holds is the one channel that has to stay two rows wide.
     */
    if(record.producerWalk) {
        auto& walk = *decl[record.producerWalk];

        mir::CallSite through;
        through.target = record.producerWalk;
        through.through = mir::CallThrough::Declaration;

        Use walking[3] = {
            editor.use(*self, UseKind::Borrow),
            editor.use(*sent, UseKind::Consume),
            editor.use(*cancel, UseKind::Consume),
        };

        auto answered = editor.addCall(at, through, Use(), walking, 3, walk.result,
                                       conventionFor(decl, walk.result), true);
        auto found = editor.resultOf(*answered);

        /*
         * A failing root's "no" is two answers here as everywhere else, and the trampoline answers
         * only null for both - the failure is still in this record's `$failure`, where the root's own
         * step (or a level's alternative, composed up the chain) left it.
         */
        if(record.failureField) {
            auto word = mir::owned(program.types.words[0]);
            auto number = editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Bitcast,
                                                           editor.use(*found, UseKind::Inspect),
                                                           word, conventionFor(decl, word)));

            auto item = editor.addBlock(StringId());
            auto none = editor.addBlock(StringId());
            editor.addBranch(at, editor.use(*iterNonZero(program, editor, number, at),
                                            UseKind::Inspect), *item, *none, nullptr, 0);

            editor.setBlock(*item);
            editor.setRegion(item->region);

            Use forwarded[1] = { editor.use(*found, UseKind::Consume) };
            editor.addReturn(at, forwarded, 1);

            editor.setBlock(*none);
            editor.setRegion(none->region);
            iterAnswerFailed(program, editor, function, self, false, at);
            editor.seal();
            return true;
        }

        Use forwarded[1] = { editor.use(*found, UseKind::Consume) };
        editor.addReturn(at, forwarded, 1);
        editor.seal();
        return true;
    }

    mir::CallSite site;
    site.target = record.producerStep;
    site.through = mir::CallThrough::Declaration;

    Use arguments[3] = {
        editor.use(*self, UseKind::Borrow),
        editor.use(*sent, UseKind::Consume),
        editor.use(*cancel, UseKind::Consume),
    };

    auto more = editor.addCall(at, site, Use(), arguments, 3, step.result,
                               conventionFor(decl, step.result), true);

    auto just = editor.addBlock(StringId());
    auto nothing = editor.addBlock(StringId());

    editor.addBranch(at, editor.use(*editor.resultOf(*more), UseKind::Inspect), *just, *nothing,
                     nullptr, 0);

    editor.setBlock(*just);
    editor.setRegion(just->region);

    /*
     * The address at the **result's** own type, which is the one place the two pointers differ.
     *
     * A window slot holds the descriptor, so the field's address is a `%ArrayView(a)` where the
     * protocol says `%Array(a)`; everything else lines up already. One bitcast covers both and
     * lowers to nothing, which is what `addressOf`'s own retype does one tier up.
     */
    Use answerJust[1] = { editor.use(*iterSlotAddress(program, editor, function, self, at),
                                     UseKind::Consume) };
    editor.addReturn(at, answerJust, 1);

    editor.setBlock(*nothing);
    editor.setRegion(nothing->region);

    if(record.failureField) {
        iterAnswerFailed(program, editor, function, self, false, at);
        editor.seal();
        return true;
    }

    Use answerNone[1] = { editor.use(*iterNoItem(program, editor, function, at), UseKind::Consume) };
    editor.addReturn(at, answerNone, 1);

    editor.seal();
    return true;
}

/*
 * `Iterate`'s two members for the **erased** type - one pair for the whole program, §3.4.
 *
 *      %at = <the pair>(<what the pass answered>, False)
 *      branch %at != null -> just -> nothing
 *
 * The step is an indirect call through `$self` itself, which is the pair: a borrow of an
 * address-only value *is* its address, and that is what the two words are read out of. So the member
 * names no producer and no record - which is the whole of what the erased form buys - and what it
 * knows is the protocol its type carries.
 *
 * `next` sends the fall-out row and `resume(v)` sends `Proceed(v)`, exactly as the concrete steppers
 * do. There is no index to test here, so the check `emitIterStepper` makes about `next()` on a
 * suspended resuming object has no counterpart: an erased object's index is inside the block and the
 * step is the only thing that can see it.
 */
static bool emitErasedStepper(Context& context, mir::Program& program, SsaArena& arena,
                              mir::Function& function, bool resuming) {
    auto decl = *program.decl;
    auto& iterated = *(mir::IterType*)decl[function.generatedRecord];
    auto at = function.source;

    Editor editor(context, program, arena, function);
    auto entry = editor.currentBlock();

    for(auto argumentPointer: function.args.contents(decl)) {
        auto& declared = *decl[argumentPointer];
        editor.addBlockArgument(*entry, declared.occurrence, conventionFor(decl, declared.occurrence));
    }

    auto self = (*arena)[entry->argumentAt(*arena, 0)];

    // What the pass answered, at the type the step takes - `Outcome(R, exit)` where the producer
    // asked for a value, and the bare signal where it did not. `emitIterStepper` builds the same
    // pair for the concrete form.
    auto held = mir::owned(iterated.sent);
    Use answer;

    if(decl[iterated.sent]->kind == mir::Type::Record) {
        auto& wrapper = *(mir::RecordType*)decl[iterated.sent];

        if(resuming) {
            auto value = (*arena)[entry->argumentAt(*arena, 1)];
            Use component[1] = { editor.use(*value, UseKind::Consume) };
            auto built = editor.addAggregate(at, 0, component, 1, held, conventionFor(decl, held));
            answer = editor.use(*editor.resultOf(*built), UseKind::Consume);
        } else {
            auto row = editor.addConst(at, mir::intConstant(program.types.exitCode(), 0));
            Use component[1] = { editor.use(*editor.resultOf(*row), UseKind::Consume) };
            auto exit = wrapper.cons.get(decl, 1).index;
            auto built = editor.addAggregate(at, exit, component, 1, held, conventionFor(decl, held));
            answer = editor.use(*editor.resultOf(*built), UseKind::Consume);
        }
    } else {
        auto row = editor.addConst(at, mir::intConstant(program.types.exitCode(), 0));
        answer = editor.use(*editor.resultOf(*row), UseKind::Consume);
    }

    auto stop = editor.addConst(at, mir::truthConstant(program, false));

    /*
     * The step, through the pair - `CallThrough::Value` with the object itself as the target.
     *
     * The signature the call is checked against is the one the type carries (`IterType::step`),
     * which is what makes this a call and not a leap: both ends read one declaration of what a step
     * is, and the shim on the other side was declared from the same three fields.
     */
    mir::CallSite site;
    site.target = nullptr;
    site.through = mir::CallThrough::Value;

    auto answered = mir::owned(program.types.pointerType(iterated.handover.type));

    Use arguments[2] = { answer, editor.use(*editor.resultOf(*stop), UseKind::Consume) };
    auto found = editor.resultOf(*editor.addCall(at, site, editor.use(*self, UseKind::Inspect),
                                                 arguments, 2, answered,
                                                 conventionFor(decl, answered), true));

    // ... and whether there is anything there, which for an address is the one value an item cannot
    // have. Compared as a number because a comparison is over integers; the bitcast is free.
    auto word = mir::owned(program.types.words[0]);
    auto number = editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Bitcast,
                                                   editor.use(*found, UseKind::Inspect), word,
                                                   conventionFor(decl, word)));

    auto zero = editor.addConst(at, mir::intConstant(word.type, 0));
    auto truth = mir::owned(mir::booleanType(program));

    auto more = editor.addBinary(at, mir::BinaryOp::Ne, editor.use(*number, UseKind::Inspect),
                                 editor.use(*editor.resultOf(*zero), UseKind::Inspect), truth,
                                 conventionFor(decl, truth));

    auto just = editor.addBlock(StringId());
    auto nothing = editor.addBlock(StringId());

    editor.addBranch(at, editor.use(*editor.resultOf(*more), UseKind::Inspect), *just, *nothing,
                     nullptr, 0);

    editor.setBlock(*just);
    editor.setRegion(just->region);

    /*
     * `iter(T | E)`'s third row - C1c, J218. A tagged answer is the failure, handed over as
     * `Failed(e)`; the shim finished the record before answering, so the next call answers `Done`
     * exactly as `f$next`'s `f$failure` arranges one tier in.
     */
    if(iterated.failure) {
        auto failure = editor.addBlock(StringId());
        auto item = editor.addBlock(StringId());
        editor.addBranch(at, editor.use(*iterAnswerTagged(program, editor, found, at),
                                        UseKind::Inspect), *failure, *item, nullptr, 0);

        editor.setBlock(*failure);
        editor.setRegion(failure->region);

        auto reason = iterErasedReason(program, editor, found, iterated, at);
        auto rows = mir::iterStepRows(decl, function.result.type);
        auto held = function.result;

        Use carried[1] = { editor.use(*reason, UseKind::Consume) };
        auto failedAnswer = editor.addAggregate(at, rows.failed, carried, 1, held,
                                                conventionFor(decl, held));
        Use answerFailed[1] = { editor.use(*editor.resultOf(*failedAnswer), UseKind::Consume) };
        editor.addReturn(at, answerFailed, 1);

        editor.setBlock(*item);
        editor.setRegion(item->region);
    }

    /*
     * And what is **at** that address for a window hand-over: the descriptor's two words, not the
     * container - `iter(T)$item` reads the same slot the same way, and `emitIterStepper` has the
     * record's own field to read it off. Loading `Array(a)` here instead would be loading an owning
     * container out of a slot that holds a borrow of one.
     */
    auto stored = iterated.handover.type;

    if(iterated.window) {
        if(auto view = mir::viewRecordOf(context, program, iterated.handover)) stored = view;
    }

    // A lending slot holds the value's address, and the answer is the slot's (`iterSlotAddress`).
    if(iterated.lends) {
        auto through = mir::owned(found->occurrence.type);
        found = editor.resultOf(*editor.addLoad(at, editor.use(*found, UseKind::Inspect), through,
                                                conventionFor(decl, through), false));
    }

    return answerItem(context, program, editor, function, found, stored, iterated.window, *nothing,
                      at, nullptr, iterated.failure != nullptr);
}

static bool emitIterStepper(Context& context, mir::Program& program, SsaArena& arena,
                            mir::Function& function, bool resuming) {
    auto decl = *program.decl;
    auto& record = *(mir::RecordType*)decl[function.generatedRecord];
    auto& step = *decl[record.producerStep];
    auto at = function.source;

    Editor editor(context, program, arena, function);
    auto entry = editor.currentBlock();

    for(auto argumentPointer: function.args.contents(decl)) {
        auto& declared = *decl[argumentPointer];
        editor.addBlockArgument(*entry, declared.occurrence, conventionFor(decl, declared.occurrence));
    }

    auto self = (*arena)[entry->argumentAt(*arena, 0)];

    /*
     * What the pass answered, at the type the step takes - the exit signal for a plain producer, and
     * `Outcome(R, exit)` where the producer declared `<- R`.
     */
    auto sent = decl[step.args.get(decl, 1)]->occurrence;
    Use answer;

    auto resumes = decl[sent.type]->kind == mir::Type::Record &&
                   ((mir::RecordType*)decl[sent.type])->cons.size() > 1;

    /*
     * `next()` on a resuming object at a **suspend** index - the check the class can make.
     *
     * The producer asked for a value and this member has none to send, so what a step here would be
     * is a pass that answered nothing. Index zero is the honest case and the finished index is a
     * second `next()` after the end, so what is refused is everything between them.
     *
     * `checkCondition` and not a branch of its own, for the reason `lib/Core/Check.yana` gives: what
     * a failed check *does* is a policy the library owns, and a build with `-check-locations` gets
     * the located form through the same substitution every other check gets it through. It is rooted
     * for this function by `summary.cpp`, which is where a generated body's edges are drawn.
     */
    if(!resuming && resumes) {
        if(auto guard = program.roles.function(intrinsic::Known::checkCondition)) {
            auto index = editor.resultOf(*editor.addFieldAddress(
                at, editor.use(*self, UseKind::Inspect), function.generatedRecord, 0));

            auto counter = mir::owned(program.types.exitCode());
            auto loaded = editor.resultOf(*editor.addLoad(at, editor.use(*index, UseKind::Inspect),
                                                          counter, conventionFor(decl, counter),
                                                          false));

            auto fresh = editor.addConst(at, mir::intConstant(counter.type, 0));
            auto done = editor.addConst(at, mir::intConstant(counter.type, kIterFinished));

            auto answer = mir::owned(mir::booleanType(program));

            auto started = editor.addBinary(at, mir::BinaryOp::Ne,
                                            editor.use(*loaded, UseKind::Inspect),
                                            editor.use(*editor.resultOf(*fresh), UseKind::Inspect),
                                            answer, conventionFor(decl, answer));

            auto running = editor.addBinary(at, mir::BinaryOp::Ne,
                                            editor.use(*loaded, UseKind::Inspect),
                                            editor.use(*editor.resultOf(*done), UseKind::Inspect),
                                            answer, conventionFor(decl, answer));

            // A `select` rather than a bitwise `and`: the conjunction is over two truths and
            // `OpCategory::Bits` is about integers, so this is the branch-free spelling that says so.
            auto no = editor.addConst(at, mir::truthConstant(program, false));
            auto suspended = editor.addSelect(at,
                                              editor.use(*editor.resultOf(*started), UseKind::Inspect),
                                              editor.use(*editor.resultOf(*running), UseKind::Inspect),
                                              editor.use(*editor.resultOf(*no), UseKind::Inspect),
                                              answer, conventionFor(decl, answer));

            mir::CallSite check;
            check.target = guard;
            check.through = mir::CallThrough::Declaration;

            Use failed[1] = { editor.use(*editor.resultOf(*suspended), UseKind::Borrow) };
            auto& guarding = *decl[guard];
            editor.addCall(at, check, Use(), failed, 1, guarding.result,
                           conventionFor(decl, guarding.result), false);
        }
    }

    if(resumes) {
        auto& wrapper = *(mir::RecordType*)decl[sent.type];
        auto held = mir::owned(sent.type);

        if(resuming) {
            // `Proceed(v)` - the pass ended with `continue v`, which is what `resume(v)` means.
            auto value = (*arena)[entry->argumentAt(*arena, 1)];
            Use component[1] = { editor.use(*value, UseKind::Consume) };
            auto built = editor.addAggregate(at, 0, component, 1, held, conventionFor(decl, held));
            answer = editor.use(*editor.resultOf(*built), UseKind::Consume);
        } else {
            auto row = editor.addConst(at, mir::intConstant(program.types.exitCode(), 0));
            Use component[1] = { editor.use(*editor.resultOf(*row), UseKind::Consume) };
            auto exit = wrapper.cons.get(decl, 1).index;
            auto built = editor.addAggregate(at, exit, component, 1, held, conventionFor(decl, held));
            answer = editor.use(*editor.resultOf(*built), UseKind::Consume);
        }
    } else {
        auto row = editor.addConst(at, mir::intConstant(program.types.exitCode(), 0));
        answer = editor.use(*editor.resultOf(*row), UseKind::Consume);
    }

    auto stop = editor.addConst(at, mir::truthConstant(program, false));

    auto just = editor.addBlock(StringId());
    auto nothing = editor.addBlock(StringId());

    /*
     * A **delegating** record is stepped through its trampoline, and what comes back is an address
     * rather than a `Bool` - §6.3, and it is the erased form's own answer read here.
     *
     * The item a delegating object hands over is the innermost level's and is not in this record's
     * slot at all, so what the class's member borrows is where the step said it is. That is
     * `emitErasedStepper`'s tail exactly, which is why both end in `answerItem`.
     */
    if(record.producerWalk) {
        auto& walk = *decl[record.producerWalk];

        mir::CallSite through;
        through.target = record.producerWalk;
        through.through = mir::CallThrough::Declaration;

        Use walking[3] = {
            editor.use(*self, UseKind::Borrow),
            answer,
            editor.use(*editor.resultOf(*stop), UseKind::Consume),
        };

        auto found = editor.resultOf(*editor.addCall(at, through, Use(), walking, 3, walk.result,
                                                     conventionFor(decl, walk.result), true));

        auto word = mir::owned(program.types.words[0]);
        auto number = editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Bitcast,
                                                       editor.use(*found, UseKind::Inspect), word,
                                                       conventionFor(decl, word)));

        editor.addBranch(at, editor.use(*iterNonZero(program, editor, number, at), UseKind::Inspect),
                         *just, *nothing, nullptr, 0);

        editor.setBlock(*just);
        editor.setRegion(just->region);

        auto& iterated = *(mir::RecordType*)decl[record.linkType];
        auto& protocol = *(mir::IterType*)decl[iterated.field(decl, 0).occurrence.type];

        return answerItem(context, program, editor, function, found, protocol.handover.type,
                          protocol.window, *nothing, at, record.failureField ? self : nullptr);
    }

    mir::CallSite site;
    site.target = record.producerStep;
    site.through = mir::CallThrough::Declaration;

    Use arguments[3] = {
        editor.use(*self, UseKind::Borrow),
        answer,
        editor.use(*editor.resultOf(*stop), UseKind::Consume),
    };

    auto more = editor.addCall(at, site, Use(), arguments, 3, step.result,
                               conventionFor(decl, step.result), true);

    editor.addBranch(at, editor.use(*editor.resultOf(*more), UseKind::Inspect), *just, *nothing,
                     nullptr, 0);

    /*
     * The item, out of the slot - a field of the record here, and the address the erased step
     * answered there. `answerItem` is the same tail either way.
     *
     * A **lending** producer's slot holds the value's address, so what is carried is the pointer in
     * it; every other slot holds the value, so what is carried is the slot's own address. The record
     * keeps the item either way, which is what makes the loan true - and what makes `$held`
     * load-bearing, since nothing here takes.
     */
    editor.setBlock(*just);
    editor.setRegion(just->region);

    auto item = record.field(decl, 1).occurrence;
    auto lends = decl[item.type]->kind == mir::Type::Pointer;

    auto slot = editor.resultOf(*editor.addFieldAddress(at, editor.use(*self, UseKind::Inspect),
                                                        function.generatedRecord, 1));

    if(lends) {
        auto through = mir::owned(item.type);
        slot = editor.resultOf(*editor.addLoad(at, editor.use(*slot, UseKind::Inspect), through,
                                               conventionFor(decl, through), false));
    }

    return answerItem(context, program, editor, function, slot, item.type, record.slotWindow,
                      *nothing, at, record.failureField ? self : nullptr);
}

/*
 * `Just(<the item>)` on one block and `Nothing` on the other - the tail both steppers end with.
 *
 * Shared between the concrete form, where the item is a field of the record in hand, and the erased
 * one, where it is the address the step answered - which is the whole of what erasure changes at
 * this boundary. `slot` is where the item is and `stored` is what is at it; everything below reads
 * the *result's* own schema, because what says the payload position is a reference is the signature
 * rather than anything about the subject.
 */
static bool answerItem(Context& context, mir::Program& program, Editor& editor,
                       mir::Function& function, Value* slot, DeclPtr<mir::Type> stored, bool window,
                       Block& nothing, LocationId at, Value* failing, bool stepRows) {
    auto decl = *program.decl;
    auto& answered = *(mir::RecordType*)decl[function.result.type];

    // The **declared** result and not `owned(type)`: the shape is what says the payload position is
    // a reference, and an aggregate built without it is one whose component is checked against the
    // member the record would have had.
    auto held = function.result;

    // Which constructor carries the item, and which carries nothing - by what each one holds rather
    // than by its position, so that the order `lib/Core` writes them in is not a second fact here.
    U16 someIndex = 0;
    U16 noneIndex = 0;

    for(auto constructor: answered.cons.contents(decl)) {
        if(constructor.count) someIndex = constructor.index;
        else noneIndex = constructor.index;
    }

    // `IterStep` carries two things, so its rows are read off the declaration (C1b).
    // `stepRows` is the erased form's: the answer is `IterStep`, and the failure was taken off the
    // tagged address before this tail, so `nothing` is `Done` alone.
    auto rows = mir::iterStepRows(decl, function.result.type);
    if(failing || stepRows) {
        someIndex = rows.yield;
        noneIndex = rows.done;
    }

    /*
     * The address, said as the **loan** it stands for - `represent`, which moves no bytes.
     *
     * A raw `%Y` and a `source'Y` are the same eight bytes and the occurrence is the only thing that
     * says which they are, so without this the component would be checked against the payload's own
     * type and reported as a pointer where a value was declared. `f$item` makes the same pair at a
     * result, where a call boundary tolerates it silently; an aggregate is held to its position.
     */
    auto payload = mir::aggregateComponent(decl, function.result.type, someIndex, 0).type;
    auto position = decl[function.result.shape]->slots.get(decl, 0);
    mir::Occurrence borrowed { payload, decl[position]->contract, nullptr };

    auto finish = [&](Value* shown) {
        Use carried[1] = { editor.use(*shown, UseKind::Inspect) };
        auto some = editor.addAggregate(at, someIndex, carried, 1, held, conventionFor(decl, held));

        Use answerJust[1] = { editor.use(*editor.resultOf(*some), UseKind::Consume) };
        editor.addReturn(at, answerJust, 1);

        editor.setBlock(nothing);
        editor.setRegion(nothing.region);

        /*
         * A **failing** object's "no" is two answers, told apart by the index the step left - the
         * reading `resolveObjectFor`'s `stopped` block makes, here once per call (C1b). `f$failure`
         * moves the failure out and finishes the record, so `Failed(e)` hands it over exactly once
         * and the next call answers `Done`.
         */
        if(failing) {
            auto& record = *(mir::RecordType*)decl[function.generatedRecord];
            auto index = editor.resultOf(*editor.addFieldAddress(
                at, editor.use(*failing, UseKind::Inspect), function.generatedRecord, 0));

            auto counter = mir::owned(program.types.exitCode());
            auto loaded = editor.resultOf(*editor.addLoad(at, editor.use(*index, UseKind::Inspect),
                                                          counter, conventionFor(decl, counter),
                                                          false));
            auto sentinel = editor.addConst(at, mir::intConstant(counter.type, U64(kIterFailed)));

            auto truth = mir::owned(mir::booleanType(program));
            auto failed = editor.addBinary(at, mir::BinaryOp::Eq, editor.use(*loaded, UseKind::Inspect),
                                           editor.use(*editor.resultOf(*sentinel), UseKind::Inspect),
                                           truth, conventionFor(decl, truth));

            auto failure = editor.addBlock(StringId());
            auto done = editor.addBlock(StringId());
            editor.addBranch(at, editor.use(*editor.resultOf(*failed), UseKind::Inspect), *failure,
                             *done, nullptr, 0);

            editor.setBlock(*failure);
            editor.setRegion(failure->region);

            auto& take = *decl[record.producerFailure];
            mir::CallSite site;
            site.target = record.producerFailure;
            site.through = mir::CallThrough::Declaration;

            Use self[1] = { editor.use(*failing, UseKind::Borrow) };
            auto unit = decl[take.result.type]->kind == mir::Type::Unit;
            auto called = editor.addCall(at, site, Use(), self, 1, take.result,
                                         conventionFor(decl, take.result), !unit);
            auto reason = unit ? editor.resultOf(*editor.addConst(at, mir::unitConstant(take.result.type)))
                               : editor.resultOf(*called);

            Use carriedFailure[1] = { editor.use(*reason, UseKind::Consume) };
            auto answer = editor.addAggregate(at, rows.failed, carriedFailure, 1, held,
                                              conventionFor(decl, held));
            Use answerFailed[1] = { editor.use(*editor.resultOf(*answer), UseKind::Consume) };
            editor.addReturn(at, answerFailed, 1);

            editor.setBlock(*done);
            editor.setRegion(done->region);
        }

        auto none = editor.addAggregate(at, noneIndex, nullptr, 0, held, conventionFor(decl, held));
        Use answerNone[1] = { editor.use(*editor.resultOf(*none), UseKind::Consume) };
        editor.addReturn(at, answerNone, 1);

        editor.seal();
    };

    /*
     * A **window** in the slot, which is the one shape that needs no derivation at all - the record
     * already holds the two words (`RecordType::slotWindow`), so what the position takes is what is
     * in the field rather than something built out of an owner's address.
     */
    if(window) {
        auto loaded = editor.resultOf(*editor.addLoad(at, editor.use(*slot, UseKind::Inspect),
                                                      mir::owned(stored),
                                                      conventionFor(decl, mir::owned(stored)),
                                                      false));

        finish(editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Represent,
                                                editor.use(*loaded, UseKind::Inspect), borrowed,
                                                conventionFor(decl, borrowed))));
        return true;
    }

    // The slot's contents said as the **whole-value** loan they are, which manifests as exactly the
    // address in hand. `f$item` makes the same pair at a result.
    mir::Occurrence owner { payload, mir::slotContract(borrowed.contract.access(), 0), nullptr };
    auto shown = editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Represent,
                                                 editor.use(*slot, UseKind::Inspect), owner,
                                                 conventionFor(decl, owner)));

    /*
     * And the **window**, where the item has one: a borrowed `String` is that string's descriptor
     * rather than its address, so what the position takes is two words derived from the owner. The
     * same derivation `deriveView` makes for a specialization, called here because a generated body
     * has no node for that walk to find - `summary.cpp` is where the member is kept reachable.
     */
    if(mir::needsViewDerivation(decl, program, owner, borrowed)) {
        auto target = mir::viewMember(context, program, payload, borrowed.contract.access(), at);

        if(!target || !decl[target]->reachable) {
            context.diagnostics.error(
                "internal: no derivation is reachable for a window onto an object's item"_v, at);
            return false;
        }

        mir::CallSite derivation;
        derivation.target = target;
        derivation.through = mir::CallThrough::Declaration;

        Use owned[1] = { editor.use(*shown, UseKind::Borrow) };
        shown = editor.resultOf(*editor.addCall(at, derivation, Use(), owned, 1, borrowed,
                                               conventionFor(decl, borrowed), true));
    }

    finish(shown);
    return true;
}

/*
 * One generated body - see `StepPlan` for the step, which is the one that reads a body it did not
 * build.
 */
static bool convertGenerated(Context& context, mir::Program& program, Converted& into,
                             Scratch& scratch, Array<Outline>& outlines, mir::Function& function) {
    auto decl = *program.decl;

    if(function.generated == mir::Function::Generated::IterNew) {
        return emitIterNew(context, program, into.arena, function);
    }

    if(function.generated == mir::Function::Generated::IterDrop) {
        return emitIterDrop(context, program, into.arena, function);
    }

    /*
     * `f$item` - the lent value's address, out of the slot and answered as a borrow of it.
     *
     * Two instructions, and the declaration is where the work is: the result occurrence is a loan
     * rooted in the record's origin slot, and a loan of a value that is not trivially copyable
     * manifests as its address - which is exactly what the slot holds.
     */
    if(function.generated == mir::Function::Generated::IterItem) {
        auto& record = *(mir::RecordType*)decl[function.generatedRecord];
        auto at = function.source;

        Editor editor(context, program, into.arena, function);
        auto entry = editor.currentBlock();

        auto& declared = *decl[function.args.get(decl, 0)];
        editor.addBlockArgument(*entry, declared.occurrence, conventionFor(decl, declared.occurrence));

        /*
         * A **delegating** record's item is where the trampoline said it is - §6.3, and the
         * signature has a second parameter to say so (`iterItem`). The same body `iter(T)$item` has:
         * the address arrives and the result is it, read as the loan of the object the signature
         * declares.
         */
        if(function.args.size() > 1) {
            for(auto argumentPointer: function.args.contents(decl)) {
                auto& taken = *decl[argumentPointer];
                if(taken.index == 0) continue;
                editor.addBlockArgument(*entry, taken.occurrence, conventionFor(decl, taken.occurrence));
            }

            auto address = (*into.arena)[entry->argumentAt(*into.arena, 1)];

            // The answer is a slot's address, and a lending slot holds the value's (`iterSlotAddress`).
            if(decl[record.field(decl, 1).occurrence.type]->kind == mir::Type::Pointer) {
                auto through = mir::owned(address->occurrence.type);
                address = editor.resultOf(*editor.addLoad(at, editor.use(*address, UseKind::Inspect),
                                                          through, conventionFor(decl, through),
                                                          false));
            }

            Use where[1] = { editor.use(*address, UseKind::Consume) };
            editor.addReturn(at, where, 1);
            editor.seal();
            return true;
        }

        auto self = (*into.arena)[entry->argumentAt(*into.arena, 0)];
        auto slot = editor.resultOf(*editor.addFieldAddress(
            at, editor.use(*self, UseKind::Inspect), function.generatedRecord, 1));

        /*
         * At the **pointer's own** occurrence, which is the one place a load has to be an
         * instruction: a value that lives in storage *is* its address, so a load whose result is
         * declared a loan lowers to nothing at all - and what is in this field is a reference to
         * somebody else's storage rather than the storage itself. `lower/README.md`'s note about a
         * continuation's environment is the same sentence about the same shape.
         *
         * The result is handed back at the *declared* occurrence, which manifests as exactly this
         * pointer - which is what makes the two the same eight bytes and the signature the only
         * thing that says what they mean.
         */
        auto held = mir::owned(record.field(decl, 1).occurrence.type);
        auto loaded = editor.addLoad(at, editor.use(*slot, UseKind::Inspect), held,
                                     conventionFor(decl, held), false);

        Use answer[1] = { editor.use(*editor.resultOf(*loaded), UseKind::Consume) };
        editor.addReturn(at, answer, 1);
        editor.seal();
        return true;
    }

    /*
     * `f$take` - the item moved out of the slot, and `$held` cleared beside it.
     *
     * A load that consumes and a store of `False`, which is the whole of it. What it is *for* is the
     * declaration: a `for` reading the field itself would move out of a path it uses again on the
     * back edge, and only the step knows the slot is refilled.
     */
    if(function.generated == mir::Function::Generated::IterTake) {
        auto& record = *(mir::RecordType*)decl[function.generatedRecord];
        auto at = function.source;

        Editor editor(context, program, into.arena, function);
        auto entry = editor.currentBlock();

        auto& declared = *decl[function.args.get(decl, 0)];
        editor.addBlockArgument(*entry, declared.occurrence, conventionFor(decl, declared.occurrence));

        auto self = (*into.arena)[entry->argumentAt(*into.arena, 0)];
        auto fieldAt = [&](U16 index) {
            return editor.resultOf(*editor.addFieldAddress(at, editor.use(*self, UseKind::Inspect),
                                                           function.generatedRecord, index));
        };

        auto held = mir::owned(record.field(decl, 1).occurrence.type);
        auto loaded = editor.addLoad(at, editor.use(*fieldAt(1), UseKind::Inspect), held,
                                     conventionFor(decl, held), false);

        auto taken = editor.addConst(at, mir::truthConstant(program, false));
        editor.addStore(at, editor.use(*fieldAt(record.heldField), UseKind::Inspect),
                        editor.use(*editor.resultOf(*taken), UseKind::Consume));

        Use answer[1] = { editor.use(*editor.resultOf(*loaded), UseKind::Consume) };
        editor.addReturn(at, answer, 1);
        editor.seal();
        return true;
    }

    /*
     * `f$failure` - the `Exit` arm of `$failure` moved out, and `$index` finished beside it (P2,
     * J216). Called only where the index read `kIterFailed`, so the field holds an `Exit`.
     */
    if(function.generated == mir::Function::Generated::IterFailure) {
        auto& record = *(mir::RecordType*)decl[function.generatedRecord];
        auto at = function.source;

        Editor editor(context, program, into.arena, function);
        auto entry = editor.currentBlock();

        auto& declared = *decl[function.args.get(decl, 0)];
        editor.addBlockArgument(*entry, declared.occurrence, conventionFor(decl, declared.occurrence));

        auto self = (*into.arena)[entry->argumentAt(*into.arena, 0)];
        auto fieldAt = [&](U16 index) {
            return editor.resultOf(*editor.addFieldAddress(at, editor.use(*self, UseKind::Inspect),
                                                           function.generatedRecord, index));
        };

        auto held = record.field(decl, record.failureField).occurrence;
        auto& outcome = *(mir::RecordType*)decl[held.type];
        auto reason = outcome.fields.get(decl, outcome.cons.get(decl, mir::kOutcomeExit).first);

        auto slot = fieldAt(record.failureField);
        auto taken = mir::owned(reason.occurrence.type);

        // A `{}` failure is its one value rather than a load of zero bytes (C1c found it: nothing
        // had failed with `{}` before).
        Inst* loaded = nullptr;
        if(decl[taken.type]->kind == mir::Type::Unit) {
            loaded = editor.addConst(at, mir::unitConstant(taken.type));
        } else {
            auto address = editor.addFieldAddress(at, editor.use(*slot, UseKind::Inspect), held.type,
                                                  reason.index, held.shape);
            loaded = editor.addLoad(at, editor.use(*editor.resultOf(*address), UseKind::Inspect),
                                    taken, conventionFor(decl, taken), false);
        }

        auto finished = editor.addConst(at, mir::intConstant(program.types.exitCode(),
                                                             U64(kIterFinished)));
        editor.addStore(at, editor.use(*fieldAt(0), UseKind::Inspect),
                        editor.use(*editor.resultOf(*finished), UseKind::Consume));

        if(decl[taken.type]->kind == mir::Type::Unit) {
            editor.addReturn(at, nullptr, 0);
        } else {
            Use answer[1] = { editor.use(*editor.resultOf(*loaded), UseKind::Consume) };
            editor.addReturn(at, answer, 1);
        }

        editor.seal();
        return true;
    }

    if(function.generated == mir::Function::Generated::IterNext) {
        return emitIterStepper(context, program, into.arena, function, false);
    }

    if(function.generated == mir::Function::Generated::IterResume) {
        return emitIterStepper(context, program, into.arena, function, true);
    }

    // The erased form's three - the step a pair is called across, and `Iterate`'s two members for
    // it. See their definitions, and §3.4.
    if(function.generated == mir::Function::Generated::IterObject) {
        return emitIterObject(context, program, into.arena, function);
    }

    if(function.generated == mir::Function::Generated::IterWalk) {
        return emitIterWalk(context, program, into.arena, function);
    }

    if(function.generated == mir::Function::Generated::IterErasedRaw) {
        return emitIterErasedRaw(context, program, into.arena, function);
    }

    if(function.generated == mir::Function::Generated::IterErasedStep) {
        return emitIterErasedStep(context, program, into.arena, function);
    }

    if(function.generated == mir::Function::Generated::IterErasedNext) {
        return emitErasedStepper(context, program, into.arena, function, false);
    }

    if(function.generated == mir::Function::Generated::IterErasedResume) {
        return emitErasedStepper(context, program, into.arena, function, true);
    }

    /*
     * `iter(T)$item` - the address the erased step answered, said as the loan of the object it is.
     *
     * Nothing at all in the ordinary case: the result is handed back at the *declared* occurrence,
     * which manifests as exactly the pointer that arrived, and the signature is the only thing that
     * says what it means. `f$item` makes the same pair out of a field read one tier over.
     *
     * The one exception is a hand-over that is a **window**: a `source'[a]` is the descriptor's two
     * words rather than an address, so what is at the pointer is loaded and restated as the view it
     * is. `emitIterStepper`'s window branch reads the same slot the same way.
     */
    // `iter(T | E)$failure` - the tagged answer untagged and its `Exit` payload moved out (C1c).
    if(function.generated == mir::Function::Generated::IterErasedFailure) {
        auto& iterated = *(mir::IterType*)decl[function.generatedRecord];
        auto at = function.source;

        Editor editor(context, program, into.arena, function);
        auto entry = editor.currentBlock();

        // `$self` is the loan that keeps the object alive to here, and is not read.
        for(auto argumentPointer: function.args.contents(decl)) {
            auto& declared = *decl[argumentPointer];
            editor.addBlockArgument(*entry, declared.occurrence,
                                    conventionFor(decl, declared.occurrence));
        }

        auto answer = (*into.arena)[entry->argumentAt(*into.arena, 1)];
        auto reason = iterErasedReason(program, editor, answer, iterated, at);

        // A `{}` result is returned as no values at all, as every unit-returning body is.
        if(decl[function.result.type]->kind == mir::Type::Unit) {
            editor.addReturn(at, nullptr, 0);
        } else {
            Use answered[1] = { editor.use(*reason, UseKind::Consume) };
            editor.addReturn(at, answered, 1);
        }

        editor.seal();
        return true;
    }

    if(function.generated == mir::Function::Generated::IterErasedItem) {
        auto& iterated = *(mir::IterType*)decl[function.generatedRecord];
        auto at = function.source;

        Editor editor(context, program, into.arena, function);
        auto entry = editor.currentBlock();

        for(auto argumentPointer: function.args.contents(decl)) {
            auto& declared = *decl[argumentPointer];
            editor.addBlockArgument(*entry, declared.occurrence,
                                    conventionFor(decl, declared.occurrence));
        }

        auto address = (*into.arena)[entry->argumentAt(*into.arena, 1)];
        auto found = address;

        // A lending slot holds the value's address, and the answer is the slot's (`iterSlotAddress`).
        if(iterated.lends) {
            auto through = mir::owned(address->occurrence.type);
            found = editor.resultOf(*editor.addLoad(at, editor.use(*address, UseKind::Inspect),
                                                    through, conventionFor(decl, through), false));
        }

        if(iterated.window) {
            auto view = mir::viewRecordOf(context, program, function.result);
            auto stored = mir::owned(view ? view : iterated.handover.type);

            auto loaded = editor.resultOf(*editor.addLoad(at, editor.use(*address,
                                                                        UseKind::Inspect),
                                                          stored, conventionFor(decl, stored),
                                                          false));

            found = editor.resultOf(*editor.addUnary(at, mir::UnaryOp::Represent,
                                                     editor.use(*loaded, UseKind::Inspect),
                                                     function.result,
                                                     conventionFor(decl, function.result)));
        }

        /*
         * A **window** is handed back as the loan it is and not consumed, which is the one thing the
         * two shapes do not share: what the ordinary case returns is the pointer that arrived, owned
         * and therefore consumable, and what the window case returns is the descriptor restated at
         * this signature's own `source'[a]` - a guaranteed value, which has no obligation for a
         * `return` to take over.
         */
        auto kind = found->convention == Convention::Owned ? UseKind::Consume : UseKind::Inspect;

        Use answer[1] = { editor.use(*found, kind) };
        editor.addReturn(at, answer, 1);
        editor.seal();
        return true;
    }

    auto& record = *(mir::RecordType*)decl[function.generatedRecord];
    auto& producer = *decl[function.generatedFrom];
    auto& owner = mir::bodyOwnerOf(decl, producer);
    auto mir = *program.mir;
    auto& body = *mir[owner.mir];

    StepPlan plan;
    plan.record = &record;
    plan.continuationArg = owner.continuationArg;

    for(U32 i = 0; i < body.locals.size(); i++) {
        plan.localField.push(StepPlan::kNoField);
        plan.localRun.push(0);
        plan.localBorrow.push(0);
    }

    /*
     * Which region each node opened, for the nested traversals - the record's entry names the
     * `lens.call` and what the conversion has in hand at an edge is the region.
     */
    for(U32 i = 0; i < body.regions.size(); i++) {
        plan.nestOf.push(StepPlan::kNoField);
        plan.nestAdvance.push(nullptr);
        plan.nestBoxed.push(nullptr);
        plan.nestDelegate.push(0);
        plan.delegateResume.push(nullptr);
    }

    U32 nested = 0;

    for(U32 j = 0; j < record.payloadLocals.size(); j++) {
        auto member = record.payloadLocals.get(decl, j);

        // And which field it lives in, read rather than derived: since slot sharing there are more
        // entries than fields - `RecordType::payloadFields`.
        auto field = U32(record.payloadFields.get(decl, j));

        // A node index with the high bit set is a **value** - see `kIterValueMember`.
        if(member & 0x80000000u) {
            plan.spillValue.push(member & ~0x80000000u);
            plan.spillField.push(field);
            plan.spillEntry.push(j);
            continue;
        }

        // And one with the nest bit is a **nested traversal**, whose payload is the `lens.call` that
        // opened the region rather than a local - `RecordType::kNestMember`, §3.2's first step.
        if(member & mir::RecordType::kNestMember) {
            auto& call = (mir::NodeLensCall&)*body.node(mir, NodeId(member &
                                                                    ~mir::RecordType::kNestMember));
            plan.nestOf[call.region.index] = field;
            plan.nestFields.push(U16(field));

            // And whether it is a **box** - the k-th nest member is `nestBoxed`'s k-th entry, which
            // is what the mint's own list is indexed by (`RecordType::nestBoxed`).
            auto shim = nested < record.nestBoxed.size() ? record.nestBoxed.get(decl, nested)
                                                         : DeclPtr<mir::Function>(nullptr);

            plan.nestBoxed[call.region.index] = shim;
            plan.nestFieldBoxed.push(shim);

            // And whether it **delegates**, read off the same k-th entry - `RecordType::nestDelegates`.
            if(nested < record.nestDelegates.size() && record.nestDelegates.get(decl, nested)) {
                plan.nestDelegate[call.region.index] = 1;
            }

            nested++;
            continue;
        }

        // And the bit beside it says the field holds the member's **run** - `@heap` on the binding,
        // `RecordType::kHeapMember`. The step reaches the value through the pointer and the exits
        // free it.
        if(member & mir::RecordType::kHeapMember) {
            plan.localRun[member & ~mir::RecordType::kHeapMember] = 1;
            plan.runFields.push(U16(field));
        }

        // And the bit below it says the field holds a **loan's** address - written where the
        // binding is established, read at every use.
        if(member & mir::RecordType::kBorrowMember) {
            plan.localBorrow[member & ~mir::RecordType::kBorrowMember] = 1;
        }

        plan.localField[member & ~(mir::RecordType::kHeapMember |
                                   mir::RecordType::kBorrowMember)] = field;
    }

    /*
     * Which nodes are the hand-overs, and which is the read of the continuation parameter one goes
     * through - the same walk `iterSiteTypeOf` made to find the payload, made again here rather than
     * carried, because what it produces is dense over node IDs and dies with the body.
     */
    for(U32 i = 0; i < body.nodes.size(); i++) {
        auto& node = *body.node(mir, NodeId(i));
        auto reads = false;

        if(node.kind == mir::Node::Read) {
            auto place = body.place(mir, ((mir::NodeRead&)node).place.placeId());
            reads = place->kind == mir::Place::Local &&
                    body.local(mir, LocalId(place->payload))->parameter == plan.continuationArg;
        }

        plan.readsContinuation.push(reads ? 1 : 0);
    }

    for(U32 i = 0; i < body.nodes.size(); i++) {
        auto& node = *body.node(mir, NodeId(i));
        auto isYield = false;

        if(node.kind == mir::Node::Call) {
            auto& call = (mir::NodeCall&)node;
            isYield = call.callsValue() && call.target.source() == mir::SourceKind::Result &&
                      plan.readsContinuation[call.target.nodeId().index];
        }

        plan.yieldOf.push(isYield ? plan.yields : StepPlan::kNotAYield);
        if(isYield) plan.yields++;
    }


    for(U32 i = 0; i < plan.yields; i++) plan.resume.push(nullptr);

    planSpillReach(decl, mir, body, plan);

    Editor editor(context, program, into.arena, function);
    Conversion conversion(context, program, into.arena, producer, function, editor, scratch,
                          outlines, nullptr);

    conversion.stepping = &plan;
    return conversion.run();
}

} // namespace

bool convertProgram(Context& context, mir::Program& program, Converted& into) {
    auto decl = *program.decl;
    auto converted = true;

    // One set of side tables for the whole program, reset per body - see `Scratch`.
    Scratch scratch;

    /*
     * The function list is snapshotted, and then checked not to have grown.
     *
     * `callTarget` makes a specialization if the argument list has none, so this walk *can* extend
     * the list it is reading - and a list that grew under a `Contents` snapshot would be a walk
     * reading a buffer that has moved. That is why it is copied.
     *
     * That it does not grow is the stronger statement, and it is the one that matters: reachability
     * asked `callTarget` the same questions first, so every specialization this program calls exists
     * before conversion starts. One made here would be a function nothing marked reachable and
     * nothing converts, which lowering meets as a call to a body that is not there. Asserting it is
     * what makes the two walks' agreement a check rather than a hope.
     */
    Array<DeclPtr<mir::Function>> functions;
    for(auto modulePointer: program.modules) {
        for(auto pointer: decl[modulePointer]->functions.contents(decl)) functions.push(pointer);
    }

    /*
     * One function, and the continuations its lens calls become - Analysis-IR.md §11.4's W-C.
     *
     * The outlines are planned before any of them is emitted, because the *call site* is converted
     * first and names both the function and the environment record. Everything after that is one
     * conversion per scope over the same MIR body, in the order MIR made the regions - outermost
     * first, which is the order a nested environment has to be built in.
     */
    Array<Outline> outlines;
    Size outlined = 0;

    for(auto functionPointer: functions) {
        auto function = decl[functionPointer];
        if(!function->reachable || !function->mir) continue;

        auto& body = *(*program.mir)[mir::bodyOwnerOf(decl, *function).mir];

        outlines.clear();
        Planner planner {
            context, program, *function, body, *program.mir,
            mir::substitutionOf(decl, *function), outlines
        };

        planner.run();
        outlined += outlines.size();

        Editor editor(context, program, into.arena, *function);
        Conversion conversion(context, program, into.arena, *function, *function, editor, scratch,
                              outlines, nullptr);

        converted &= conversion.run();

        for(Size i = outlines.size(); i-- > 0;) {
            Editor liftedEditor(context, program, into.arena, *decl[outlines[i].lifted]);
            Conversion lifted(context, program, into.arena, *function, *decl[outlines[i].lifted],
                              liftedEditor, scratch, outlines, &outlines[i]);

            converted &= lifted.run();
        }
    }

    /*
     * And the bodies this stage did not build - an iterator object's constructor, step and teardown,
     * `Function::Generated`.
     *
     * After every ordinary function, because a step is a second reading of a producer's body and the
     * producer's own conversion is what put its specialization in a settled state. They contribute no
     * outlines of their own: a step **embeds** every region the producer opens (`convertStepNest`) -
     * a `for x in g(...)` and a lens over part of the producer's own body alike - and what it cannot
     * embed is refused at the mint. That is why the planner does not run for a step at all, and is
     * the answer to §6.6's second question: a lens region whose continuation contains no `yield`
     * would have been an ordinary outlined call, and it is a member with one hand-over instead. One
     * shape for one construct beats two, and it is what keeps the record's layout the same whether
     * or not a `yield` happens to be written inside the region.
     */
    Array<Outline> none;

    for(auto functionPointer: functions) {
        auto function = decl[functionPointer];
        if(!function->reachable) continue;
        if(function->generated == mir::Function::Generated::None) continue;

        converted &= convertGenerated(context, program, into, scratch, none, *function);
    }

    /*
     * The list grew by exactly the continuations, and by nothing else.
     *
     * `callTarget` makes a specialization if the argument list has none, and one made *here* would
     * be a function nothing marked reachable and nothing converts - which lowering meets as a call
     * to a body that is not there. The outlined continuations are the one legitimate growth: they
     * are made by this walk and converted by it in the same breath.
     */
    Size declared = 0;
    for(auto modulePointer: program.modules) declared += decl[modulePointer]->functions.size();
    assertTrue(declared == functions.size() + outlined);

    return converted;
}

} // namespace ssa
