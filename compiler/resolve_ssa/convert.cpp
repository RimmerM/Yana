#include "convert.h"
#include "manifest.h"
#include "../resolve_mir/specialize.h"

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

    void reset(U32 nodes) {
        produced.clear();
        blocks.clear();
        slots.clear();
        flags.clear();

        produced.reserve(nodes);
        for(U32 i = 0; i < nodes; i++) produced.push(nullptr);
    }
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

    Editor editor;

    Array<SsaPtr<Value>>& produced;
    Array<SsaPtr<Block>>& blocks;
    Array<SsaPtr<Value>>& slots;
    Array<SsaPtr<Value>>& flags;

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

    Conversion(Context& context, mir::Program& program, SsaArena& arena, mir::Function& function,
               Scratch& scratch):
        context(context), program(program), arena(arena), function(function),
        source(*(*program.mir)[mir::bodyOwnerOf(*program.decl, function).mir]),
        mir(*program.mir), scratch(scratch),
        with(mir::substitutionOf(*program.decl, function)),
        editor(context, program, arena, function),
        produced(scratch.produced), blocks(scratch.blocks), slots(scratch.slots),
        flags(scratch.flags)
    {
        scratch.reset(source.nodes.size());
        if(source.elaboration) elaboration = mir[source.elaboration];
    }

    SsaBase base() { return *arena; }
    DeclBase decl() { return *program.decl; }

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
        if(mir::isLinear(decl(), occurrence.type)) return Convention::Owned;

        return mir::isTriviallyCopyable(decl(), occurrence.type)
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

                // The field *index*, not an offset. Which bytes it is at is the manifestation's
                // answer and is asked once, at lowering - see `InstFieldAddress`.
                auto inst = editor.addFieldAddress(at, addressUse(*base),
                                                   substituted(parent->occurrence.type),
                                                   U16(place.payload));
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

            case mir::Place::Loan:
            case mir::Place::Address:
            case mir::Place::Index:
            case mir::Place::KindCount:
                break;
        }

        report(at, "internal: the ownership pipeline cannot lower this place yet"_v);
        return nullptr;
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
    void emitDrop(const mir::DropStep& step) {
        auto& path = *elaboration->path(mir, step.path);
        auto place = source.place(mir, path.place);
        auto at = substituted(place->occurrence);

        /*
         * A teardown the *generic* body needed and this specialization does not.
         *
         * An unconstrained `a` is assumed to release something, which is what puts the step here at
         * all; substituting `I32` for it leaves a point at which nothing runs. Skipped rather than
         * refused, and that asymmetry is Implementation-Generics.md §1's invariant working: the
         * *placement* is the generic body's answer and is identical either way, and what a
         * specialization is allowed to change is only whether there is any code at it.
         */
        if(!mir::hasTeardown(decl(), at.type)) return;

        auto address = addressOf(*place, function.source);
        if(!address) return;

        if(!step.conditional) {
            editor.addDrop(function.source, addressUse(*address), at);
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
        auto held = mir::owned(program.types.boolean);
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
        editor.addDrop(function.source, addressUse(*address), at);
        editor.addJump(function.source, *after, nullptr, 0);

        editor.setBlock(*after);
    }

    // Every step the analysis scheduled before this node. Both lists are sorted, so each is a cursor
    // rather than a search - see the two of them on `Conversion`.
    void emitSteps(U32 block, U32 position) {
        if(!elaboration) return;

        while(dropCursor < elaboration->drops.size()) {
            auto step = elaboration->drops.get(mir, dropCursor);
            if(step.block != block || step.position != position) break;

            dropCursor++;
            emitDrop(step);
        }

        while(flagCursor < elaboration->flags.size()) {
            auto step = elaboration->flags.get(mir, flagCursor);
            if(step.position == mir::FlagStep::kEntry) { flagCursor++; continue; }
            if(step.block != block || step.position != position) break;

            flagCursor++;
            emitFlag(step);
        }
    }

    void emitFlag(const mir::FlagStep& step) {
        if(step.path.index >= flags.size() || !flags[step.path.index]) return;

        auto flag = base()[flags[step.path.index]];
        auto constant = mir::boolConstant(program.types.boolean, step.value);
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

    // One outgoing edge, converted. Shared by `jump` and `yield`, which differ in what the edge
    // means and not in what it carries - see the `Yield` case below.
    void convertEdge(mir::Node& node, mir::Edge& written) {
        auto target = base()[blocks[mir[written.target]->index.index]];

        SmallArray<Use, 8> arguments;
        for(auto operand: written.arguments.contents(mir)) {
            Use used;
            if(!useOf(operand, node.source, used)) return;
            arguments.push(used);
        }

        editor.addJump(node.source, *target, arguments.pointer(), arguments.size());
    }

    /*
     * What an MIR region kind and multiplicity become here - one mapping, written out.
     *
     * The two enums are deliberately separate types rather than one shared header: MIR's kinds are
     * what a *resolver* built and this tier's are what an optimizer reads, and the day one of them
     * gains a row the other does not is the day a shared enum would have silently agreed.
     */
    static Continuation::Kind regionKind(mir::Continuation::Kind kind) {
        switch(kind) {
            case mir::Continuation::Body: return Continuation::Body;
            case mir::Continuation::Frame: return Continuation::Frame;
            case mir::Continuation::Handover: return Continuation::Handover;
        }

        assertTrue(false);
        return Continuation::Body;
    }

    static Continuation::Multiplicity regionMultiplicity(mir::Continuation::Multiplicity how) {
        switch(how) {
            case mir::Continuation::Once: return Continuation::Once;
            case mir::Continuation::AtMostOnce: return Continuation::AtMostOnce;
        }

        assertTrue(false);
        return Continuation::Once;
    }

    void convertNode(mir::Node& node) {
        switch(node.kind) {
            case mir::Node::Const: {
                auto& written = (mir::NodeConst&)node;
                auto constant = source.constant(mir, written.constant);
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
                              isAddressOnly(decl(), result);

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
                auto inst = editor.addAggregate(node.source, components.pointer(),
                                                components.size(), result, conventionFor(result));

                produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            case mir::Node::Call: {
                auto& written = (mir::NodeCall&)node;

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
                mir::callTarget(context, program, function, written, site);

                auto inst = editor.addCall(node.source, site, arguments.pointer(),
                                           arguments.size(), result, conventionFor(result),
                                           hasResult);

                if(hasResult) produced[node.id.index] = editor.resultOf(*inst) - base();
                break;
            }
            case mir::Node::Write: {
                auto& written = (mir::NodeWrite&)node;
                auto place = source.place(mir, written.destination.placeId());

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
                   !place->occurrence.contract.isOwned()) {
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
                    editor.addReturn(node.source, nullptr, 0);
                    break;
                }

                Use used;
                if(!useOf(written.value, node.source, used)) return;

                editor.addReturn(node.source, &used, 1);
                break;
            }
            case mir::Node::Jump:
                convertEdge(node, ((mir::NodeJump&)node).to);
                break;

            /*
             * A `yield` converts to an ordinary jump, and the region is what carries the rest.
             *
             * Which is §10.1 arriving rather than being lost: what a continuation needs at this tier
             * is that its exits are *edges* the optimizer's liveness and ownership rules already
             * walk, plus a region that says which blocks they are and where control comes back. Both
             * are here - `ssa::Continuation` is copied across below - so a second terminator kind
             * would be a distinction with nothing at this tier reading it.
             */
            case mir::Node::Yield:
                convertEdge(node, ((mir::NodeYield&)node).to);
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
         * The continuation regions first, and in MIR's own order, so that the two tables are one
         * numbering. Both start with the body's own at index zero - the editor made it - so copying
         * from one leaves a region at the index it had, and a block or a destination naming one
         * needs no map.
         */
        for(U32 i = 1; i < source.regions.size(); i++) {
            auto region = source.region(mir, ContinuationId(i));
            editor.addRegion(regionKind(region->kind), regionMultiplicity(region->multiplicity),
                             region->parent);
        }

        blocks.reserve(source.blocks.size());
        blocks.push(editor.body.entry);

        for(U32 i = 1; i < source.blocks.size(); i++) {
            auto block = source.block(mir, BlockId(i));

            // Each block into the region MIR put it in. `addBlock` fills a region's entry the first
            // time one is made inside it, and the creation order is MIR's, so the entries agree
            // without being copied.
            editor.setRegion(block->region);
            blocks.push(editor.addBlock(block->name) - base());
        }

        editor.setRegion(ContinuationId(0));

        // The two things a region cannot state until its blocks exist: where it rejoins its parent,
        // and what its `yield` declared it hands over.
        for(U32 i = 1; i < source.regions.size(); i++) {
            auto region = source.region(mir, ContinuationId(i));
            editor.body.region(base(), ContinuationId(i))->handover = region->handover;

            if(!region->destination.valid()) continue;
            editor.rejoinRegion(ContinuationId(i), *base()[blocks[region->destination.index]]);
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
        for(auto argumentPointer: function.args.contents(decl())) {
            auto& declared = *decl()[argumentPointer];
            editor.addBlockArgument(*base()[blocks[0]], declared.occurrence,
                                    conventionFor(declared.occurrence));
        }

        for(U32 i = 0; i < source.blocks.size(); i++) {
            auto block = source.block(mir, BlockId(i));
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
        editor.setBlock(*base()[blocks[0]]);
        slots.reserve(source.locals.size());

        auto entry = base()[blocks[0]];

        for(U32 i = 0; i < source.locals.size(); i++) {
            auto local = source.local(mir, LocalId(i));

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
             * Every such local is address-only and that is asserted rather than assumed: a loan of a
             * trivially copyable type is a copy at every position that makes one (`readMode`), and
             * every position that does not make one asks the manifestation table for an address.
             */
            auto held = substituted(local->occurrence);
            auto borrowed = !held.contract.isOwned();
            if(borrowed && local->isParameter() && isReferenceValue(decl(), held)) {
                slots.push(entry->arguments.get(base(), local->parameter));
                continue;
            }

            if(borrowed && !local->isParameter()) {
                assertTrue(isReferenceValue(decl(), held));
                slots.push(nullptr);
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
         * A `Bool` in the frame for every path the analysis said needs one.
         *
         * In the entry block for the reason the slots are: a flag is read on one path and written on
         * another, and neither of those is entitled to decide where it lives.
         */
        if(elaboration) {
            flags.reserve(elaboration->paths.size());

            for(U32 i = 0; i < elaboration->paths.size(); i++) {
                if(!elaboration->flagged.get(mir, i)) { flags.push(nullptr); continue; }

                auto held = mir::owned(program.types.boolean);
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
        for(U32 i = 0; i < function.args.size(); i++) {
            auto& declared = *decl()[function.args.get(decl(), i)];
            auto incoming = base()[entry->arguments.get(base(), i)];

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
            editor.setBlock(*base()[blocks[i]]);

            // And the region it is in, because this walk *builds*: a conditional teardown opens a
            // cleanup arm here, and an arm nested in the body's region rather than in the lens frame
            // whose block it is standing in would be a region that outlives what it is cleaning up.
            editor.setRegion(block->region);

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

        editor.seal();
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

    for(auto functionPointer: functions) {
        auto function = decl[functionPointer];
        if(!function->reachable || !function->mir) continue;

        Conversion conversion(context, program, into.arena, *function, scratch);
        converted &= conversion.run();
    }

    Size declared = 0;
    for(auto modulePointer: program.modules) declared += decl[modulePointer]->functions.size();
    assertTrue(declared == functions.size());

    return converted;
}

} // namespace ssa
