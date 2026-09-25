#include "pass.h"

/*
 * Expanding a `drop` into what it runs, in front of the inliner - J137, and causes A1 and A2 of
 * `doc/progress/2026-09-23-J94-legacy-comparison.md` §6.2.
 *
 * `InstDrop` is expanded by `Lowering::emitTeardown`, after every pass here has run. That was a
 * deliberate choice (README.md's `dischargeOwnership` entry) and it is still right for the walk
 * itself, which is a lowering question - offsets, boxes, a sum's branch. What it cost was every
 * teardown that is **an ordinary call** by the time the machine sees it: an authored `Drop` or
 * `Reclaim` member, and above all `Reclaim(Array(a))` and `Reclaim(String)`, which is what nearly
 * every drop in a program is. The call is emitted after the optimizer, so nothing inlines it, and
 * it receives the *address* of the value, so nothing forwards through the slot either: `promote`
 * and `removeDeadStorage` decline an allocation a teardown-bearing `drop` names, and
 * `forwardMemory` treats the drop as a read and a write of it. `Dispatch.Borrow` was the case at
 * its smallest - a three-element literal placed on the frame, whose reclaim tests a flag the
 * aggregate set to "borrowed" and so does nothing, kept the literal in memory and a call to
 * `Array.reclaim` and `freeHeap` in the program: 51 instructions where the shipping stage (deleted
 * in J173) emitted `ret 12`.
 *
 * So the drop becomes the call it is, here, while the inliner and the folds are still ahead:
 *
 *   - **an authored half** is a `load` of the value out of its storage - the destructive read every
 *     hand-over in a converted body already is - and a `call` of the member consuming it. That is
 *     exactly the argument `emitTeardown` passes, stated as a value rather than as an address, so
 *     the inliner weighs the member like any other callee and, where it copies it, the reclaim's
 *     flag test folds against the aggregate the caller built.
 *   - **a derived walk over a plain record** becomes one `drop` per field it descends into, which
 *     is `emitTeardown`'s own loop moved up a tier; each of those is then expanded in turn. This is
 *     what brings an `Array` held in a record to the first rule - and since J172 it is done only
 *     where this body assembled the record, which is where a field's teardown has something to
 *     fold against (`expandFields`). Elsewhere the drop stays whole and the lowering calls the
 *     record's own teardown.
 *   - **a value moved out of storage only to be dropped** is dropped where it was
 *     (`fuseMoveDrop`, J172) - an element in a container's reclaim.
 *
 * Everything else stays a `drop` and keeps its lowering: a sum (the walk is a branch on the tag, and
 * a call to the per-type function is what `emitTeardown` makes of it), a boxed field (the recursion
 * a recursive type reaches its own teardown through), a function value or iterator object (one
 * shared `funTeardown`), a fixed array (the unrolled walk), a value at a type variable (a call
 * through the descriptor), and the one mixed shape - an authored half beside a derived one, whose
 * derived walk runs *one* half over the members, which no `drop` instruction can say.
 *
 * **Why before the inliner, and inside `optimizeBody` as well.** What the expansion buys is a call
 * the inliner can take, and `inlineCalls` runs once, in front of the per-body loop. A teardown
 * member's own body is expanded by the same walk, so a callee the inliner settles and copies has
 * already had its drops turned into calls - `Array(Array(I32))`'s reclaim reaches the inner one as a
 * call. The per-body run (`expandBuiltTeardowns`, J172) is for a record whose `Aggregate` a splice
 * brought in: the settle behind the splice expands it, and the inliner's next round sees the calls.
 *
 * **Native only**, beside `flattenArguments` and for a reason of the same shape: a teardown on
 * JavaScript is `jsTeardownStep`'s answer, and it is not this one - `Reclaim` is nothing on a managed
 * target (Design-Memory §4), so expanding an authored reclaim into a call there would *add* one to
 * every drop. A drop that reaches `resolve_ssa/js` is the lowering's to decide, as it was.
 *
 * **Not under `-no-ir-opt`**, because it is a pass and not a discharge: the unoptimized build reaches
 * every teardown through `emitTeardown`, and `ResolveTester`'s comparison of the two builds' answers
 * is then a check on this file - one side runs the member through the call written here, the other
 * through the call the lowering emits, and they have to agree on how often each value was released.
 */

namespace ssa {

namespace {

struct TeardownExpander {
    OptContext& opt;

    // A drop still to be expanded, and what this body built in its storage - the `Aggregate` or
    // `Const` `builtContents` found, and null where nothing was found. See `expandFields`.
    struct Pending {
        Inst* drop;
        Value* contents;

        // Whether the storage is one `writtenHere` says this body filled with its own stores, which
        // a field drop made from it inherits: it is the same storage one projection down.
        bool written;
    };

    SmallArray<Pending, 16> work;

    DeclPtr<mir::Function> memberFor(intrinsic::Known klass, DeclPtr<mir::Type> type,
                                     LocationId source) {
        auto key = (U64(type.offset) << 8) | U64(klass);
        if(auto found = opt.teardownMembers.getValue(key)) return found.unwrap();

        auto member = mir::teardownMember(opt.context, opt.program, klass, type, source);
        *opt.teardownMembers.add(key).value = member;
        return member;
    }

    /*
     * Whether a call of `member` can stand for this half: a body the inliner can see, one parameter
     * of exactly the dropped type, taken rather than borrowed. Each is true of every `Drop` and
     * `Reclaim` member the declaration walk admits, and each is checked anyway, because the answer
     * where one is not is to leave the drop to the lowering - never to build a call the verifier
     * would report.
     */
    bool callable(DeclPtr<mir::Function> member, const Occurrence& held) {
        auto callee = opt.decl[member];
        if(!callee->ssa || callee->gen && callee->form == mir::GenForm::Erased) return false;
        if(callee->args.size() != 1) return false;

        auto& parameter = *opt.decl[callee->args.get(opt.decl, 0)];
        if(!mir::sameType(parameter.occurrence.type, held.type)) return false;

        auto mode = mir::argumentMode(parameter.occurrence.contract, mir::SourceKind::Result);
        return mode == mir::UseMode::Take;
    }

    // The authored rule: `load` and `call`, or false where the drop is not that shape.
    bool expandAuthored(InstDrop& drop, const mir::Ownership& ownership) {
        auto type = drop.held.type;

        // An authored half beside a derived one runs the derived walk over one half only - see the
        // file comment. Both authored is two calls of the same subject, which no type in `lib/` or
        // `test/` declares; left to the lowering rather than guessed at.
        if(ownership.drop == mir::TeardownKind::Derived) return false;
        if(ownership.reclaim == mir::TeardownKind::Derived) return false;

        auto dropMember = ownership.drop == mir::TeardownKind::Authored
            ? memberFor(intrinsic::Known::drop, type, drop.source) : nullptr;
        auto reclaimMember = ownership.reclaim == mir::TeardownKind::Authored
            ? memberFor(intrinsic::Known::reclaim, type, drop.source) : nullptr;

        if(dropMember && reclaimMember) return false;

        /*
         * An **iterator object**'s teardown is its step entered with a cancel request, and the step
         * works on the record where it is: a suspended record's links name its own members
         * (J60-J61), so the record is not a value that may be read out of its storage and handed
         * over as a copy. Left to the lowering, which passes the address - and `iter/Inline.yana`,
         * `iter/Capture.yana` and `loans/FalseRefusals.yana` are what crashed when it was not.
         */
        if(opt.decl[type]->kind == mir::Type::Record && ((mir::RecordType*)opt.decl[type])->producer) {
            return false;
        }

        auto member = dropMember ? dropMember : reclaimMember;
        if(!member || !callable(member, drop.held)) return false;

        auto convention = conventionFor(opt.decl, drop.held);
        if(!isTracked(convention)) return false;

        auto editor = opt.ir();
        editor.setInsertBefore(drop);

        auto load = editor.addLoad(drop.source, drop.address.withKind(UseKind::Inspect), drop.held,
                                   convention);
        auto value = editor.use(*editor.resultOf(*load), UseKind::Consume);

        mir::CallSite site;
        site.target = member;

        editor.addCall(drop.source, site, Use(), &value, 1, Occurrence(), Convention::Trivial,
                       false);
        editor.eraseInst(drop);
        return true;
    }

    /*
     * The component a field of a built record holds, where it is itself built - an `Aggregate` or a
     * `Const` - and null otherwise. Read off the `Aggregate` rather than through `builtContents`,
     * because that walk asks `Contained`, and the body has already been edited by the time a field
     * drop is expanded: the answer would be recomputed per field. A component that is a `load` is
     * one this does not chase, and its drop stays whole.
     */
    Value* componentAt(Value* contents, U16 index) {
        if(!contents || !contents->definer) return nullptr;

        auto definer = opt.base[contents->definer];
        if(definer->kind != Inst::Aggregate) return nullptr;

        auto& built = (InstAggregate&)*definer;
        if(built.constructor != 0 || index >= built.components.size()) return nullptr;

        auto used = built.components.get(opt.base, index);
        if(!used.isSet()) return nullptr;

        auto component = opt.base[used.value()];
        if(!component->definer) return nullptr;

        auto kind = opt.base[component->definer]->kind;
        return kind == Inst::Aggregate || kind == Inst::Const ? component : nullptr;
    }

    /*
     * The derived rule over a plain record: one `drop` per field `emitTeardown` would descend into -
     * **where this body built the record**, and nowhere else (J172).
     *
     * J137 expanded every such drop, so that a record assembled here folds: a literal's
     * `String.reclaim` tests a flag the aggregate set, and the field's drop is nothing once the load
     * is answered from the component. That is the whole of what the expansion buys, and it needs
     * the component. Where the record arrived from somewhere else - a call's result slot, a field of
     * `self`, an element - the expansion bought nothing and wrote the walk out at every drop:
     * twelve bytes a field, which J171's `shareTeardownRuns` then had to find again at the lower
     * tier and put back into one function. So the drop now stays whole there, and the lowering
     * calls the record's own teardown (`Lowering::teardownHalves`) - one call handed the address,
     * which is the per-type function by construction rather than by recognition.
     *
     * "Built here" is either of two answers about the storage. `contents` is `builtContents`': the
     * one `Aggregate` or constant the slot holds for the whole body, and a field's contents are the
     * component that `Aggregate` holds. `written` is `writtenHere`'s: every store into the slot is
     * this body's and says what it wrote, which catches a record assembled field by field or lent
     * to a callee that only reads it - the shapes `soleContents` refuses and `forwardMemory` still
     * answers a field's load in. A slot a call's result was stored into, or that a callee writes
     * through, is neither. Asked here and in the per-body loop (`expandBuiltTeardowns`), so a
     * record an inlined callee built is expanded after the splice that brought its `Aggregate` in.
     *
     * Measured over `test/base` against expanding every drop: 5.0 KB smaller at the default level
     * together with the lowering's half (`sharesRecordTeardown`), and the corpus's lowering goldens
     * 0.9884x -> 0.9867x of the shipping stage's.
     */
    bool expandFields(InstDrop& drop, Value* contents, bool written) {
        auto type = drop.held.type;
        if(opt.decl[type]->kind != mir::Type::Record) return false;
        if(!contents && !written) return false;

        auto& record = *(mir::RecordType*)opt.decl[type];
        if(record.layout != mir::RecordLayout::Single) return false;

        // A shaped occurrence says something about which members are loans, and `fieldAddress`
        // takes the shape for that reason; the lowering's walk reads the type alone. A shape is
        // rare on a drop, and declining it keeps the two walks provably the same one.
        if(drop.held.shape) return false;

        /*
         * Each half is derived or absent - `expandAuthored` took every authored one - and a derived
         * half is one some member owes. So "both halves over each field that tears down" is the walk
         * the lowering makes: a half the record does not owe is a half no field owes either, and the
         * record's own ownership row has nothing to add here.
         */
        SmallArray<U16, 8> fields;
        for(auto field: record.fields.contents(opt.decl)) {
            if(!mir::tearsDownField(opt.decl, field)) continue;

            // The one field shape the lowering answers with a call of its own - see the file comment.
            if(mir::indirectField(opt.decl, field)) return false;
            if(!hasTeardownAt(opt, mir::owned(field.occurrence.type))) return false;

            fields.push(field.index);
        }

        auto editor = opt.ir();
        editor.setInsertBefore(drop);

        for(auto index: fields) {
            auto field = record.field(opt.decl, index);

            auto address = editor.addFieldAddress(drop.source,
                                                  drop.address.withKind(UseKind::Inspect), type,
                                                  index);
            auto made = editor.addDrop(drop.source,
                                       editor.use(*editor.resultOf(*address), UseKind::Inspect),
                                       mir::owned(field.occurrence.type), drop.boxesVisible);
            work.push(Pending { made, componentAt(contents, index), written });
        }

        editor.eraseInst(drop);
        return true;
    }

    /*
     * A drop of a slot holding a constant whose teardown was run at compile time and did nothing -
     * `mir::Program::teardownIsNothing`, and a string literal is what it is for.
     *
     * Erased rather than expanded, because it is the one drop whose expansion is known to be empty.
     * `removeDeadStorage` admits exactly this drop beside the literal's slot and removes the three
     * together; the `load` and `call` the rule below would write instead are a hand-over it rightly
     * declines, so expanding it would have kept every literal's storage alive until the inliner
     * happened to take `String.reclaim`.
     */
    bool erasesNothing(InstDrop& drop, const Contained& contained) {
        auto address = opt.base[drop.address.value()];
        auto root = allocationRoot(opt, address);
        if(!root) return false;

        // One slot's answer rather than a table - J138: the table built for every body was a
        // third of what this pass cost `test/base`'s compile. `soleContents` since J148.
        return holdsNothingOwed(opt, contained, *root);
    }

    void expand(InstDrop& drop, Value* contents, bool written) {
        auto type = drop.held.type;
        if(opt.decl[type]->kind == mir::Type::Variable) return;

        auto ownership = mir::ownershipOf(opt.decl, type);

        if(ownership.drop == mir::TeardownKind::Authored ||
           ownership.reclaim == mir::TeardownKind::Authored) {
            expandAuthored(drop, ownership);
            return;
        }

        expandFields(drop, contents, written);
    }

    /*
     * A value moved out of storage only to be dropped, dropped where it was - J172.
     *
     * `let ->doomed = *(value.run.items + i)` in `Reclaim(Array(a))` is a take out of the element, a
     * store into the local, and the local's drop at the end of the iteration, three instructions in
     * a row. Where the element is a record in memory that is a copy of the whole element into a
     * frame temporary - `Array(a).reclaim(Case)` moved 89 bytes per element - and the temporary is
     * what the teardown then reads. The lower tier cannot take the copy away, since the calls the
     * teardown makes are calls, and nothing there says what they may write.
     *
     * Dropping the element where it is ends the same life at the same point: the take leaves the
     * element's storage empty, and so does the drop, and between the three nothing else runs. The
     * local then has no use left and `removeDeadStorage` takes it.
     */
    bool fuseMoveDrop(Block& block, Size index) {
        if(index < 2) return false;

        auto drop = opt.base[block.instructionAt(opt.base, index)];
        auto store = opt.base[block.instructionAt(opt.base, index - 1)];
        auto load = opt.base[block.instructionAt(opt.base, index - 2)];
        if(drop->kind != Inst::Drop || store->kind != Inst::Store || load->kind != Inst::Load) return false;

        auto& dropped = (InstDrop&)*drop;
        auto& written = (InstStore&)*store;
        auto& taken = (InstLoad&)*load;
        if(written.isVolatile || taken.isVolatile || !load->results.size()) return false;

        auto slot = opt.base[dropped.address.value()];
        if(opt.base[written.address.value()] != slot) return false;
        if(!slot->definer || opt.base[slot->definer]->kind != Inst::Alloc) return false;

        auto value = opt.base[load->results.get(opt.base, 0)];
        if(opt.base[written.value.value()] != value || !isTracked(value->convention)) return false;
        if(!mir::sameType(value->occurrence.type, dropped.held.type)) return false;

        if(value->useCount() != 1 || slot->useCount() != 2) return false;

        auto editor = opt.ir();
        editor.setInsertBefore(*drop);
        editor.addDrop(drop->source, taken.address.withKind(UseKind::Inspect), dropped.held,
                       dropped.boxesVisible);

        editor.eraseInst(*drop);
        editor.eraseInst(*store);
        editor.eraseInst(*load);
        return true;
    }

    void run() {
        work.clear();

        for(auto pointer: opt.body->blocks.contents(opt.base)) {
            auto block = opt.base[pointer];
            if(block->deleted) continue;

            for(Size i = 2; i < block->instructionCount(); i++) {
                if(fuseMoveDrop(*block, i)) i -= 2;
            }
        }

        for(auto pointer: opt.body->blocks.contents(opt.base)) {
            auto block = opt.base[pointer];
            if(block->deleted) continue;

            for(Size i = 0; i < block->instructionCount(); i++) {
                auto inst = opt.base[block->instructionAt(opt.base, i)];
                if(inst->kind == Inst::Drop) work.push(Pending { inst, nullptr, false });
            }
        }

        if(!work.size()) return;

        /*
         * The drops of a constant whose teardown is nothing are settled first, against the body as
         * it arrived, and erased together: the answer is about the slot's root, which a field drop
         * made below shares with the drop it came from, so asking before any rewrite is asking
         * every drop - and it keeps `Contained` from being read across edits that add values.
         *
         * What each remaining drop's storage was built from is asked in the same sweep and for the
         * same reason: `builtContents` reads `Contained`, and the first rewrite below makes it stale.
         */
        auto& contained = containedOf(opt);
        SmallArray<Inst*, 16> erased;

        Size kept = 0;
        for(Size i = 0; i < work.size(); i++) {
            auto& drop = (InstDrop&)*work[i].drop;
            if(erasesNothing(drop, contained)) {
                erased.push(work[i].drop);
                continue;
            }

            auto address = opt.base[drop.address.value()];
            work[i].contents = builtContents(opt, address, 0);

            auto root = allocationRoot(opt, address);
            work[i].written = root && writtenHere(opt, *root);
            work[kept++] = work[i];
        }

        while(work.size() > kept) work.pop();
        for(auto inst: erased) opt.ir().eraseInst(*inst);

        // A field drop made here is pushed and expanded in turn, so a record holding a record
        // holding an `Array` reaches the array's reclaim.
        while(work.size()) {
            auto pending = work[work.size() - 1];
            work.pop();
            expand((InstDrop&)*pending.drop, pending.contents, pending.written);
        }
    }
};

} // namespace

void expandTeardowns(OptContext& opt) {
    TeardownExpander expander { opt };

    /*
     * The bodies are listed before any is rewritten, because `teardownMember` is an instance search
     * that may mint a specialization - it answers from the summary walk's own minting on every
     * program the suite holds, and a walk of `module->functions` that the list grows under would be
     * the `contents()` trap the moment it did not.
     */
    SmallArray<DeclPtr<mir::Function>, 64> bodies;

    for(auto modulePointer: opt.program.modules) {
        auto module = opt.decl[modulePointer];

        for(auto functionPointer: module->functions.contents(opt.decl)) {
            auto function = opt.decl[functionPointer];
            if(!function->reachable || !function->ssa) continue;

            bodies.push(functionPointer);
        }
    }

    for(auto functionPointer: bodies) {
        auto function = opt.decl[functionPointer];

        opt.function = function;
        opt.body = opt.base[function->ssa];

        // Verified where something was rewritten: the verifier is most of an assert build's
        // optimizer time, and a body this left alone is the body the last checkpoint passed.
        auto before = opt.body->version;
        expander.run();

        auto after = opt.body->version;
        if(before.values != after.values || before.blocks != after.blocks) {
            checkpoint(opt, "after expandTeardowns"_v);
        }
    }
}

void expandBuiltTeardowns(OptContext& opt) {
    if(isJsMode(opt.context.settings)) return;

    TeardownExpander expander { opt };
    expander.run();

    /*
     * And a teardown call whose subject owes nothing, which the expansion above leaves wherever a
     * field's component is a literal. The inliner erases these at the sites it visits
     * (`eraseInertTeardown`); a record whose `Aggregate` only became visible in this loop - behind
     * the inliner's last round - reaches the call here instead.
     */
    SmallArray<InstCall*, 16> calls;
    for(auto pointer: opt.body->blocks.contents(opt.base)) {
        auto block = opt.base[pointer];
        if(block->deleted) continue;

        for(auto instPointer: block->instructions(opt.base)) {
            auto inst = opt.base[instPointer];
            if(inst->kind != Inst::Call) continue;

            auto& call = (InstCall&)*inst;
            if(call.through != mir::CallThrough::Declaration || !call.callee) continue;
            if(opt.decl[call.callee]->teardown) calls.push(&call);
        }
    }

    for(auto call: calls) eraseInertTeardown(opt, *call);
}

/*
 * A teardown call whose subject this site can name, and whose end runs nothing - erased with the
 * read that handed the subject over, rather than copied or kept. J138; the inliner's until J172,
 * which moved it here for `expandBuiltTeardowns` to ask as well.
 *
 * `builtContents` is exact where it answers: every step is a slot `aggregateFillOf` says nothing but
 * its fill writes, so what the read hands over is the component the fill built. `owesNothing` then
 * says the teardown has nothing to do. It is `@caller`'s `Site` that this is for: `check` drops its
 * `at`, the drop is a `String.reclaim` of each literal in it after `expandTeardowns`, and copied into
 * a caller that built the `Site` those were releases of two literals - tested at run time through a
 * flag the forwarding does not reach, at every assertion in the program.
 */
static bool literalSlotRead(OptContext& opt, Inst& read) {
    if(read.kind != Inst::Load) return false;

    auto address = opt.base[((InstLoad&)read).address.value()];
    return constantHeldBy(opt, containedOf(opt), *address).isSet();
}

bool eraseInertTeardown(OptContext& opt, InstCall& call) {
    if(call.arguments.size() != 1) return false;

    for(auto resultPointer: call.results.contents(opt.base)) {
        if(opt.base[resultPointer]->useCount()) return false;
    }

    auto used = call.arguments.get(opt.base, 0);
    if(!used.isSet()) return false;

    auto argument = opt.base[used.value()];
    if(!isTracked(argument->convention) || !argument->definer) return false;

    if(argument->useCount() != 1) return false;

    auto definer = opt.base[argument->definer];
    if(definer->kind == Inst::Load && ((InstLoad*)definer)->isVolatile) return false;
    if(definer->kind != Inst::Load && definer->kind != Inst::Const) return false;

    // Or a read of a slot holding a literal, which `builtContents` leaves to the table because its
    // question costs a scan of the body - asked here, of the one slot, where it pays.
    auto contents = builtContents(opt, argument, 0);
    if(contents ? !owesNothing(opt, *contents, 0) : !literalSlotRead(opt, *definer)) return false;

    auto editor = opt.ir();
    editor.eraseInst(call);
    editor.eraseInst(*definer);
    return true;
}

} // namespace ssa
