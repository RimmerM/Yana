#include "convert.h"
#include "lower/manifest.h"
#include "../resolve_mir/specialize.h"
#include "../resolve_mir/own.h"

namespace ssa {

namespace {

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

    // Which exits the region actually has. A region with one needs no test at the call site.
    bool fallsOut = false;
    bool leaves = false;
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
    }

    // The local a place is rooted in, or nothing - a global, a raw address, a loan.
    void captureRoot(Outline& outline, PlaceId id) {
        auto place = source.place(mir, id);
        while(place->parent) place = mir[place->parent];
        if(place->kind != mir::Place::Local) return;

        capture(outline, LocalId(place->payload));
    }

    void plan(ContinuationId region) {
        Outline outline;
        outline.region = region;

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

                mir::forEachEdge(node, [&](mir::Edge& edge) {
                    if(!edge.target) return;
                    if(mir[edge.target]->region == region) return;

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
             * A **drop flag** inside a continuation, which is a hole rather than a feature and is
             * reported rather than left silent.
             *
             * A flag is a `Bool` in the frame that one path writes and another reads, and the frame
             * it is in is whichever scope allocated it - so a flag written by the caller and read by
             * the continuation would be two slots that never meet. Closing it properly is what the
             * result carrier already does for a returned value: one slot for the nest, captured by
             * everything that touches it. Nothing reaches it today, because a drop sinks to the
             * earliest dead point on each path and the `Maybe` state is dormant - which is exactly
             * why the day it stops being dormant should be a message and not a wrong answer.
             */
            for(U32 i = 0; i < elaboration->flags.size(); i++) {
                auto step = elaboration->flags.get(mir, i);
                if(step.position == mir::FlagStep::kEntry) continue;
                if(source.block(mir, BlockId(step.block))->region != region) continue;

                context.diagnostics.error(
                    "internal: a drop flag inside a continuation has no storage the nest shares"_v,
                    function.source);
                break;
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

            for(auto held: child.captures) {
                if(!held.valid()) { outline.resultField = 0; continue; }
                capture(outline, held);
            }

            /*
             * A child that leaves makes its *parent* leave, because propagating is what the call
             * site emits: the discriminant comes back here and there is no frame in between that
             * could act on it. Direct children only - a grandchild's answer is already in its own
             * parent's.
             */
            if(source.region(mir, child.region)->parent == region) outline.leaves |= child.leaves;
        }

        if(outline.resultField != Outline::kNoField) {
            outline.resultField = U16(outline.captures.size());
            outline.captures.push(LocalId());
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

        for(auto held: outline.captures) {
            mir::RecordField field;
            field.name = held.valid() ? source.local(mir, held)->name : carried;
            field.source = function.source;
            field.index = U16(record->fields.size());

            /*
             * An **external** field, in an origin slot of its own - the same statement a lambda's
             * borrowed capture makes. One slot each rather than one for all of them, because they
             * are not one object.
             */
            auto type = held.valid() ? substituted(source.local(mir, held)->occurrence).type
                                     : substituted(function.result).type;

            field.occurrence = mir::Occurrence {
                type, mir::slotContract(mir::Access::Unique, record->originSlots), nullptr
            };

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
        lifted->result = mir::owned(program.types.exitCode());
        lifted->reachable = true;
        lifted->summary.bodyKnown = true;

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

    U32 errors = 0;

    /*
     * Which region's blocks this conversion emits, and which outline it *is* - W-C.
     *
     * A body with lens calls in it becomes several functions, and every one of them reads the same
     * MIR body: the enclosing function emits the blocks of region zero, and each `Handover` region
     * emits its own. So one conversion is one *scope*, `outlining` is null for the first of them,
     * and everything below asks the scope rather than testing for a lens.
     */
    ContinuationId scope { 0 };
    Array<Outline>& outlines;
    Outline* outlining = nullptr;

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

    // Whether a block belongs to the scope this conversion emits.
    bool inScope(BlockId block) {
        return source.block(mir, block)->region == scope;
    }

    Outline* outlineFor(ContinuationId region) {
        for(auto& entry: outlines) if(entry.region == region) return &entry;
        return nullptr;
    }

    SsaBase base() { return *arena; }
    DeclBase decl() { return *program.decl; }

    /*
     * The region paired with the machine, for the two questions here that are about a value's
     * *representation* rather than its type - whether it is reached through an address, and whether
     * duplicating it is an operation. See `Layout`.
     *
     * This walk is below the line Analysis-Modules.md Move 2 draws: the MIR it reads is
     * target-independent, and it is already asking the manifestation table questions, so it is one
     * of the stages that knows which target it is reading for.
     */
    Layout layout() { return layoutFor(context, program); }

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

    void report(LocationId at, StringView text) {
        errors++;
        context.diagnostics.error(text, at);
    }

    /*
     * What an MIR occurrence becomes at this tier.
     *
     * The mapping is here and nowhere else. A borrowed occurrence is `Guaranteed` whatever its type,
     * because the fact that has to survive is the dependency on the loan rather than anything about
     * the representation. An owned one asks `Ownership`'s two independent questions in turn, and the
     * order is the point:
     *
     *  - **linear** - something to run, or a disposal the source has to write - is `Owned`;
     *  - otherwise **not trivially copyable** is `Affine`: nothing to run, so it may be abandoned,
     *    and a second owner is the author's `Copy` rather than the bytes;
     *  - otherwise `Trivial`.
     *
     * `mir::isLinear` and not `mir::hasTeardown`, which is what this used to ask. The two agree on
     * every type that does not use `@explicitDrop` and disagree on the one the obligation is *for*:
     * a linear token with nothing to run has no teardown and is linear, so keying this off the
     * teardown made exactly the values that must be consumed exactly once into `Trivial` ones the
     * verifier lets a pass duplicate and discard.
     *
     * The `Affine` rung is the same mistake made one cell over. `isLinear` alone sent an authored
     * `Copy` type to `Trivial`, whose contract is *freely duplicable* - so the one rule the instance
     * exists to state ("duplicating a `Counter` is not the bytes") was the one rule the tier below
     * did not carry. `mir::isTracked` is the union of the two questions and is what decides whether
     * MIR follows the value at all; this is the same pair read for what the value *owes*.
     */
    Convention conventionFor(const mir::Occurrence& occurrence) {
        if(!occurrence.contract.isOwned()) return Convention::Guaranteed;
        if(mir::isLinearAt(decl(), occurrence)) return Convention::Owned;

        return mir::isTriviallyCopyableAt(decl(), occurrence)
            ? Convention::Trivial : Convention::Affine;
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
     */
    bool aliasesLender(const mir::Occurrence& held) {
        return !held.contract.isOwned() && isReferenceValue(layout(), held);
    }

    Use addressUse(Value& value) {
        return editor.use(value, isTracked(value.convention) ? UseKind::Borrow : UseKind::Inspect);
    }

    Value* addressOf(mir::Place& place, LocationId at) {
        switch(place.kind) {
            case mir::Place::Global: {
                auto global = DeclPtr<mir::Global>(place.payload);
                return editor.resultOf(*editor.addGlobalAddress(at, global));
            }
            case mir::Place::Local: {
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
            case mir::Place::Address:
                return valueProducedBy(NodeId(place.payload), at);

            case mir::Place::Loan:
            case mir::Place::KindCount:
                break;
        }

        report(at, "internal: the ownership pipeline cannot lower this place yet"_v);
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
            report(at, "internal: a place names a node that produced no value"_v);
            return nullptr;
        }

        return base()[value];
    }

    Value* valueOf(mir::Operand operand, LocationId at) {
        switch(operand.source()) {
            case mir::SourceKind::Result: {
                auto value = produced[operand.nodeId().index];
                if(!value) {
                    report(at, "internal: an operand names a node that produced no value"_v);
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
     * One teardown, at the point the analysis put it.
     *
     * The address is computed in the block the walk is currently in, *before* the flag is read, so
     * that it dominates both arms of a conditional drop. That is the whole of why the order here is
     * address, flag, branch and not branch, address: an address computed inside the arm would
     * dominate nothing else, and a later projection of the same place would recompute it.
     */
    /*
     * Whether a teardown at this place can see where the value's interior boxes came from -
     * `ssa::InstDrop::boxesVisible`, and `BoxRelease` for what it decides.
     *
     * The **root** of the place answers it, and nothing else does. Storage this frame owns had its
     * boxes allocated by whatever built the value, and that is code in this program with this
     * program's allocator; storage reached through a borrow, a global or a raw address belongs to
     * somebody whose placement decision is not visible here.
     *
     * A **local** is the case that goes both ways: an owned local is this frame's, and a borrowed
     * parameter is a local whose contract says the storage is the caller's. `Contract::isOwned` is
     * the same question `buildPaths` asks to decide whether a local gets a move path at all.
     */
    bool boxesVisibleAt(mir::Place& place) {
        auto root = &place;
        while(root->parent) root = mir[root->parent];

        switch(root->kind) {
            case mir::Place::Local:
                return root->occurrence.contract.isOwned();

            // A global outlives every frame and nothing here placed it; a loan and a raw address
            // name storage whose owner is somewhere else by construction.
            case mir::Place::Global:
            case mir::Place::Loan:
            case mir::Place::Address:
                return false;

            /*
             * Not a root. Every projection has a parent, so the walk above stops at one of the four
             * above and this arm is unreachable - written out rather than defaulted so that a new
             * root kind is a compiler error here instead of a silent `true`, which is the answer
             * that frees somebody else's memory.
             */
            case mir::Place::Field:
            case mir::Place::Property:
            case mir::Place::Discriminant:
            case mir::Place::Index:
            case mir::Place::Deref:
                break;
        }

        assertTrue(false);
        return false;
    }

    void emitDrop(const mir::DropStep& step) {
        // The step's own place, and not its path's: an overwrite of a global, a borrow or an element
        // releases what it replaces and has no path at all - see `mir::DropStep`.
        auto place = source.place(mir, step.place);
        auto at = substituted(place->occurrence);
        auto boxes = boxesVisibleAt(*place);

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

        if(!step.conditional) {
            editor.addDrop(function.source, addressUse(*address), at, boxes);
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

        editor.setBlock(*dropBlock);
        editor.addDrop(function.source, addressUse(*address), at, boxes);
        editor.addJump(function.source, *after, nullptr, 0);

        editor.setBlock(*after);
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
    Value* storageOfLoan(LoanId id) {
        auto place = source.place(mir, source.loan(mir, id)->place);
        while(place->parent) place = mir[place->parent];

        if(place->kind != mir::Place::Local) return nullptr;
        if(place->payload >= slots.size() || !slots[place->payload]) return nullptr;

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

        auto include = [&](LoanId id) {
            auto found = storageOfLoan(id);
            if(!found) return;

            for(auto& held: storage) if(held == found - base()) return;
            storage.push(found - base());
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

        if(to->region != scope) {
            /*
             * The one edge that may leave, held to what the region declared.
             *
             * Falling out is the exit that goes to `destination`; `break` and `continue` are edges to
             * a block further out and are rows in the switch this does not have yet - they are
             * refused in the resolver by name, so an edge here that is not the destination means the
             * two have come apart rather than that the program is unusual.
             */
            auto declared = outlining ? source.region(mir, scope)->destination : BlockId();

            if(to->index != declared || written.arguments.size()) {
                report(node.source,
                       "internal: an edge leaves a continuation for somewhere other than where it rejoins"_v);
                return;
            }

            emitExit(node.source, exit);
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
            arguments.push(used);
        }

        editor.addJump(node.source, *target, arguments.pointer(), arguments.size());
    }

    // A continuation answering with which of its exits it took - the whole of the exit signal, and
    // the one thing a `Handover` region's terminators become that an ordinary block's do not.
    void emitExit(LocationId at, U64 code) {
        auto constant = mir::intConstant(program.types.exitCode(), code);
        auto value = editor.resultOf(*editor.addConst(at, constant));
        auto used = editor.use(*value, UseKind::Consume);
        editor.addReturn(at, &used, 1);
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
                    editor.addJump(at, *grafting->join, &use, 1);
                    return;
                }
            }

            editor.addJump(at, *grafting->join, value, value ? 1 : 0);
            return;
        }

        if(!outlining) {
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
    Value* convertSkip(mir::NodeLensCall& node, mir::Function& callee, mir::Continuation& region,
                       Value& wrapped) {
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

        auto& record = *(mir::RecordType*)decl()[callee.result.type];

        // One projection, three times: the tag and one field of each arm. A lambda rather than three
        // spellings of the same two instructions, because what differs between them is the index and
        // the type and nothing else.
        auto read = [&](U16 field, mir::Occurrence held) {
            auto address = editor.addFieldAddress(node.source, addressUse(*storage),
                                                  callee.result.type, field, callee.result.shape);
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
        auto tagType = mir::tagTypeOf(decl(), program.types, callee.result.type);
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

            if(held.valid()) {
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
         * skipping lens's is the record `declareOutcome` built, which is concrete by
         * `declareSignature`'s rule and therefore is - so reading the declaration is right for one
         * of them and the binding this file already makes is right for the other.
         */
        auto answered = node.kind == mir::Node::LensTry ? mir::owned(callee.result.type)
                                                        : mir::owned(program.types.exitCode());
        auto call = editor.addCall(node.source, site, Use(), arguments.pointer(), arguments.size(),
                                   answered, conventionFor(answered), true);

        auto region = source.region(mir, node.region);
        auto code = editor.resultOf(*call);

        if(node.kind == mir::Node::LensTry) {
            code = convertSkip(node, callee, *region, *code);
            if(!code) return;
        }

        /*
         * Which exit it took, where there is more than one.
         *
         * A region that only falls out and one that only leaves each have a single answer, so the
         * discriminant is dead and nothing tests it - which is every scoped-acquisition call, and is
         * what keeps the signal from costing anything where it says nothing.
         */
        if(!plan->leaves) {
            if(!region->destination.valid()) {
                report(node.source, "internal: a lens call falls out to nowhere"_v);
                return;
            }

            editor.addJump(node.source, *base()[blocks[region->destination.index]], nullptr, 0);
            return;
        }

        if(!plan->fallsOut || !region->destination.valid()) {
            emitLeaveFromCarrier(node.source);
            return;
        }

        /*
         * `== kExitLeave`, and deliberately not `!= kExitFallOut`.
         *
         * `kExitBreak` is a third row and the caller treats it as falling out: the loop is over, and
         * where the loop was over is the destination. Testing for the one row that means something
         * *here* is what makes adding a row below a change to the callee alone.
         */
        auto left = editor.resultOf(
            *editor.addConst(node.source, mir::intConstant(program.types.exitCode(), kExitLeave)));

        auto test = mir::owned(mir::booleanType(program));
        auto leaves = editor.resultOf(*editor.addBinary(node.source, BinaryOp::Eq,
                                                        editor.use(*code, UseKind::Inspect),
                                                        editor.use(*left, UseKind::Inspect),
                                                        test, conventionFor(test)));

        auto leaving = editor.addBlock(StringId());
        editor.addBranch(node.source, editor.use(*leaves, UseKind::Inspect),
                         *leaving, *base()[blocks[region->destination.index]], nullptr, 0);

        editor.setBlock(*leaving);
        emitLeaveFromCarrier(node.source);
    }

    // The other half of `emitLeave`: an exit the continuation already parked, propagated. The
    // outermost scope is the one that reads the slot; every scope in between says only that it left.
    void emitLeaveFromCarrier(LocationId at) {
        if(outlining) {
            emitExit(at, kExitLeave);
            return;
        }

        if(!resultCarrier) {
            editor.addReturn(at, nullptr, 0);
            return;
        }

        auto result = substituted(function.result);
        auto loaded = editor.resultOf(*editor.addLoad(at, editor.use(*base()[resultCarrier],
                                                                     UseKind::Inspect),
                                                      result, conventionFor(result)));

        auto used = editor.use(*loaded, UseKind::Consume);
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

        Graft graft { arguments.pointer(), arguments.size(), entry, join, value };

        // A scratch of its own, because this one is in use: the callee's nodes and locals are
        // numbered in its own body. The outlines are none by `checkAlwaysInline`'s refusal.
        Scratch inner;
        Array<Outline> none;

        Conversion nested(context, program, arena, callee, emitted, editor, inner, none, nullptr,
                          &graft, graftDepth + 1);
        if(!nested.run()) errors++;

        editor.setBlock(*join);
        editor.setRegion(join->region);
        return value;
    }

    void convertNode(mir::Node& node) {
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
                produced[node.id.index] =
                    editor.resultOf(*editor.addConst(node.source,
                                                     mir::intConstant(value.type, value.value)))
                    - base();
                break;
            }
            case mir::Node::Read: {
                auto& written = (mir::NodeRead&)node;
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
                auto used = addressUse(*address);

                /*
                 * A duplicate, where duplicating is an operation - see `InstCopy`.
                 *
                 * `CopyTrivial` says a second owner is made, and for a value carried in a register a
                 * load already is one: the bytes arrive in a register that nothing else names. For a
                 * value that lives in storage it is not, and handing a callee the caller's own record
                 * where a copy was meant is the miscompile this line exists to prevent. Which of the
                 * two a value is is the manifestation's answer and is asked here, once.
                 */
                auto copies = written.place.mode() == mir::UseMode::CopyTrivial &&
                              isAddressOnly(layout(), result);

                auto inst = copies
                    ? editor.addCopy(node.source, used, result, conventionFor(result))
                    : editor.addLoad(node.source, used, result, conventionFor(result));

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            case mir::Node::Argument:
                // Made when its block was, before anything could name it - see `run`. Nothing to do
                // here, and nothing to skip either: the entry is already filled.
                break;
            case mir::Node::Binary: {
                auto& written = (mir::NodeBinary&)node;
                Use lhs, rhs;
                if(!useOf(written.lhs, node.source, lhs)) return;
                if(!useOf(written.rhs, node.source, rhs)) return;

                auto result = substituted(mir::valueOccurrence(node));
                auto inst = editor.addBinary(node.source, written.op, lhs, rhs, result,
                                             conventionFor(result));

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            case mir::Node::Unary: {
                auto& written = (mir::NodeUnary&)node;
                Use value;
                if(!useOf(written.value, node.source, value)) return;

                auto result = substituted(mir::valueOccurrence(node));
                auto inst = editor.addUnary(node.source, written.op, value, result,
                                            conventionFor(result));

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            case mir::Node::Aggregate: {
                auto& written = (mir::NodeAggregate&)node;

                SmallArray<Use, 8> components;
                for(auto operand: written.components.contents(mir)) {
                    Use used;
                    if(!useOf(operand, node.source, used)) return;
                    components.push(used);
                }

                auto result = substituted(mir::valueOccurrence(node));
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
                if(!arrivesAsAddress(layout(), held)) {
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

            case mir::Node::FunValue: {
                auto& written = (mir::NodeFunValue&)node;
                auto result = substituted(mir::valueOccurrence(node));

                // The environment, where the lambda captured anything. A borrow of a local of this
                // frame, so it converts like any other operand and arrives here as its address.
                Use environment;
                if(written.capturing && !useOf(written.environment, node.source, environment)) {
                    return;
                }

                auto inst = editor.addFunValue(node.source, written.callee,
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
                    if(!useOf(operand, node.source, used)) return;
                    arguments.push(used);
                }

                auto result = substituted(mir::valueOccurrence(node));
                auto hasResult = decl()[result.type]->kind != mir::Type::Unit;

                auto inst = editor.addNative(node.source, written.op, arguments.pointer(),
                                             arguments.size(), result, conventionFor(result),
                                             hasResult);

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
                                             result, conventionFor(result), hasResult);

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

                SmallArray<Use, 8> arguments;
                for(auto operand: written.arguments.contents(mir)) {
                    Use used;
                    if(!useOf(operand, node.source, used)) return;
                    arguments.push(used);
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

                /*
                 * Which function this call reaches, asked of `callTarget` and of nothing else.
                 *
                 * A call to a generic names the generic and its type arguments in *this* body's
                 * terms, so the callee is a question about the substitution the body is being
                 * converted under - see `specialize.h`. Reachability asked the same question with
                 * the same function, which is what makes the target this names one that was
                 * discovered and summarized rather than one invented here.
                 */
                mir::CallSite site;
                mir::callTarget(context, program, function, mir::callRequestOf(written), site);

                // A callee declared `@inline(always)`, reached directly: its body, here, in place of
                // the call - `graftCall`. A callee reached through a slot or a value has no body in
                // hand to graft and is called.
                if(site.through == mir::CallThrough::Declaration && site.target &&
                   decl()[site.target]->alwaysInline && !written.callsValue()) {
                    auto value = graftCall(node, *decl()[site.target], arguments, result, hasResult);
                    if(hasResult && value) produced[node.id.index] = value - base();
                    break;
                }

                auto inst = editor.addCall(node.source, site, target, arguments.pointer(),
                                           arguments.size(), result, conventionFor(result),
                                           hasResult);

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

                editor.addStore(node.source, editor.use(*address, UseKind::Inspect), value);
                break;
            }
            case mir::Node::Return: {
                auto& written = (mir::NodeReturn&)node;
                if(!written.hasValue) {
                    emitLeave(node.source, nullptr);
                    break;
                }

                Use used;
                if(!useOf(written.value, node.source, used)) return;

                emitLeave(node.source, &used);
                break;
            }
            case mir::Node::Jump:
                convertEdge(node, ((mir::NodeJump&)node).to, kExitFallOut);
                break;
            case mir::Node::Break:
                convertEdge(node, ((mir::NodeJump&)node).to, kExitBreak);
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

                SmallArray<Use, 8> arguments;
                for(auto operand: written.whenTrue.arguments.contents(mir)) {
                    Use used;
                    if(!useOf(operand, node.source, used)) return;
                    arguments.push(used);
                }

                editor.addBranch(node.source, editor.use(*value, UseKind::Inspect),
                                 *base()[blocks[mir[written.whenTrue.target]->index.index]],
                                 *base()[blocks[mir[written.whenFalse.target]->index.index]],
                                 arguments.pointer(), arguments.size());
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

        // A graft's entry is a block the caller made where the call was; a function's is its own.
        blocks[entryBlock.index] = grafting ? grafting->entry - base() : editor.body.entry;

        for(U32 i = 0; i < source.blocks.size(); i++) {
            if(i == entryBlock.index) continue;

            auto block = source.block(mir, BlockId(i));
            if(block->region != scope) continue;

            blocks[i] = editor.addBlock(block->name) - base();
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
        auto entry = base()[blocks[entryBlock.index]];

        // A grafted body's parameters are the call's arguments, already values; only a function of
        // its own receives them as its entry's arguments.
        if(!grafting) {
            for(auto argumentPointer: emitted.args.contents(decl())) {
                auto& declared = *decl()[argumentPointer];
                editor.addBlockArgument(*entry, declared.occurrence,
                                        conventionFor(declared.occurrence));
            }
        }

        // What parameter `i` arrived as - the entry's argument, or the call's.
        auto incomingArgument = [&](U32 i) -> Value* {
            if(grafting) return base()[grafting->arguments[i].value()];
            return base()[entry->arguments.get(base(), i)];
        };

        for(U32 i = 0; i < source.blocks.size(); i++) {
            auto block = source.block(mir, BlockId(i));
            if(block->region != scope) continue;

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
                produced[argument.id.index] = entry->arguments.get(base(), 1);
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
            environment = entry->arguments.get(base(), 0);

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

        for(U32 i = 0; i < source.locals.size(); i++) {
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
             * The placement is the **object's**, decided by MIR ownership analysis - `storageOf`.
             * This walk carries it down and does not choose one.
             *
             * Of the object and not of the slot, which is the whole of §10.4's identity question:
             * two locals a move connects hold one object, and asking each slot separately where it
             * lives is how one object comes to have two allocations that disagree. `Local::object`
             * is the lineage and `storageOf` is the join over it, in one place.
             */
            auto alloc = editor.addAlloc(local->source, held,
                                         mir::storageOf(mir, source, *local));
            slots.push(editor.resultOf(*alloc) - base());
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

            // Concrete by `declareSignature`'s rule about the failure type, so there is nothing to
            // substitute: the wrapper's two fields are the exit code and a type of no variables.
            auto held = mir::owned(decl()[((mir::NodeLensCall*)last)->callee]->result.type);
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
        for(U32 i = 0; !outlining && i < function.args.size(); i++) {
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

                flagCursor++;
                emitFlag(step);
            }
        }

        for(U32 i = 0; i < source.blocks.size(); i++) {
            auto block = source.block(mir, BlockId(i));
            if(block->region != scope) continue;

            editor.setBlock(*base()[blocks[i]]);
            editor.setRegion(base()[blocks[i]]->region);

            auto count = block->nodes.size();
            for(U32 n = 0; n < count; n++) {
                // The teardowns first: a drop is placed *before* the node that made it due - the
                // `return` that ends the scope, the assignment that replaces the value, the first
                // instruction of the arm that did not use it.
                emitSteps(i, n);

                auto& node = *mir[block->nodes.get(mir, n)];
                convertNode(node);

                // The dependency, once the value exists - see `attachLoan`. After the node rather
                // than inside each case of it, so that a kind added to the switch cannot be the one
                // that forgets.
                if(produced[node.id.index]) attachLoan(node, *base()[produced[node.id.index]]);
            }
        }

        // Sealed once, by the conversion that owns the body: a graft's blocks are sealed with the
        // caller's, and sealing them here as well would count every edge twice.
        if(!grafting) editor.seal();
        return errors == 0;
    }
};

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
