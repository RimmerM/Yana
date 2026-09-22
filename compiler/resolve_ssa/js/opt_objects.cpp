#include "opt_internal.h"

namespace jsir {

/*
 * §6.6's fifth bullet at a second shape: the storage an escaping local *is*, taken from the
 * first thing written into it rather than built at its zero and overwritten.
 *
 * A local whose address escapes becomes a `Value` place - the storage *is* the host object, so
 * handing out a reference to it is handing out the object - and the conversion therefore has to
 * build one before anything can name it. Where the first thing that happens to it is a
 * whole-value store, that object is written property by property and then thrown away:
 *
 *     var v87 = { state: new Uint32Array([0, 0, 0, 0]), block: new Uint8Array([0, ... x64]), ... };
 *     var v98 = { state: new Uint32Array([1732584193, ...]), block: new Uint8Array([0, ... x64]), ... };
 *     v87.state = v98.state; v87.block = v98.block; v87.used = v98.used; v87.total = v98.total;
 *
 * That is `md5Answers` in `test/base`, five times over, and it is two allocations and a
 * sixty-four element literal where one of each is owed. So the place is **re-rooted** at the
 * value that was stored, the store goes, and the zero becomes dead for `dropDead` to take.
 *
 * The safety argument is about identity and is three conditions:
 *
 *  1. **every use of the zero is a place rooted at it.** If the object is an operand of anything
 *     else - an argument, a field of another object, an edge - then something outside this walk
 *     has its identity;
 *  2. **the store is the first of them**, which is checked as "in the same block as the
 *     allocation, with nothing naming it in between". A block is straight-line, so every path
 *     out of the allocation reaches the store before it reaches any other block - which is what
 *     makes it enough to look at one block rather than at dominance;
 *  3. **what is stored is a fresh allocation this store is the only use of.** Re-rooting makes
 *     the place *be* that object, so a second name for it anywhere would turn a copy into an
 *     alias - which is §6.3's question and is answered there, not here.
 *
 * The representations have to agree, which is what says the store writes the whole object and
 * not part of one. A store of a *borrowed* object is refused by condition 3 and has to be: the
 * place would become a second name for storage somebody else owns.
 */
void Optimizer::adoptInitialStores() {
    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(base, JsBlockId(i));
        if(block->deleted) continue;

        Size at = 0;
        while(at < block->instructionList.size()) {
            if(!adoptAt(*block, at)) at++;
        }
    }
}

bool Optimizer::adoptAt(JsBlock& block, Size at) {
    auto inst = base[block.instructionList.get(base, at)];
    if(inst->kind != JsInst::Store) return false;

    auto& written = (JsInstStore&)*inst;
    if(!written.value || !written.place.valid()) return false;

    auto& place = builder.place(written.place);
    if(place.kind != JsPlaceKind::Value || !place.owner) return false;

    auto zero = base[place.owner];
    auto stored = base[written.value];
    if(zero->rep != stored->rep || zero == stored) return false;

    // Both ends fresh and in this block: the zero so that the ordering below is one block's, the
    // stored value so that nothing outside this body can have named it.
    if(!zero->definer || !stored->definer) return false;
    if(!isFreshHere(zero) || !isFreshHere(stored)) return false;
    if(stored->id.index >= liveUses.size() || liveUses[stored->id.index] != 1) return false;

    Size zeroAt = block.instructionList.size();
    Size storedAt = block.instructionList.size();

    for(Size j = 0; j < at; j++) {
        auto other = base[block.instructionList.get(base, j)];
        if(other == base[zero->definer]) zeroAt = j;
        if(other == base[stored->definer]) storedAt = j;
    }

    if(zeroAt >= at || storedAt >= at) return false;

    /*
     * Nothing between the two may name the zero, which is the whole of condition 2 and is why
     * one block is enough: a block is straight-line, so a use anywhere else is one the store
     * already ran in front of.
     */
    auto clear = true;
    for(Size j = zeroAt + 1; j < at && clear; j++) {
        eachUse(*base[block.instructionList.get(base, j)], [&](JsValue* used, JsUseRole) {
            if(used->id.index == zero->id.index) clear = false;
        });
    }

    if(!clear) return false;

    /*
     * The allocation goes and the store becomes `zero = alias stored`, in the store's own
     * position - which is where the zero's value now begins, and it is behind every use of it
     * by the check above. An `Alias` rather than a rewrite of every user, for this file's
     * reason: the result value keeps its identity, so the places rooted at it and the calls
     * taking it as a reference are untouched.
     */
    release(*base[zero->definer]);
    block.instructionList.remove(base, zeroAt);

    auto storePosition = at - 1;
    release(*inst);

    auto replacement = new (target.arena) JsInstAlias(inst->source, written.value);
    replacement->id = inst->id;
    replacement->block = inst->block;
    replacement->result = place.owner;

    stored->uses++;
    if(stored->id.index < liveUses.size()) liveUses[stored->id.index]++;
    zero->definer = (JsInst*)replacement - base;

    block.instructionList.set(base, storePosition, (JsInst*)replacement - base);

    target.optimizer.adopted++;
    return true;
}

/*
 * §6.5's first and third bullets: an allocation made where the frame slot was declared, moved to
 * the block that dominates every use of it.
 *
 * A local whose address escapes is a `Value` place and the storage *is* the host object, so the
 * conversion builds one at the `alloc` - which is the entry block, because that is where a frame
 * slot is declared. Every path that leaves before the local is touched then pays for it:
 *
 *     function normalizeForm(text, compose, compatible) {   // before
 *       var v8 = { $v: "" };
 *       var v11 = { items: new Uint32Array([]), length: 0 };
 *       var s0 = { f0: 0, f1: 0 };
 *       if (stablePrefix(text, compose, compatible) === text.length) return text;
 *
 * That is five allocations on the path that returns the string unchanged, which is the path the
 * Unicode conformance cases take most of the time: `normalizeForm` allocates **1,581 MB** over
 * `test/base` here against the shipping backend's **67 MB**, and the shipping backend's
 * difference is not an optimization - it writes the same objects *inside* the `else`.
 *
 * The safety argument is two facts and a refusal:
 *
 *  1. **an allocation is not observable where nobody names it.** `Object`, `Array` and
 *     `Materialize` read no storage and take only SSA values, so moving one changes what runs
 *     only by not running it - which is the point. `Clone` is deliberately not in the set: it is
 *     `kJsAllocates` too, and what it copies is whatever its source holds at the moment it runs;
 *  2. **the destination dominates every use**, computed as the lowest common ancestor of the
 *     use blocks in the dominator tree. The definition dominates every use, so it dominates
 *     their common ancestor and the operands - which dominate the definition - still dominate
 *     the new position;
 *  3. **and it is in no loop the definition was not in.** Sinking an allocation into a loop is
 *     one object per iteration where one was owed, and where the object is a local's storage it
 *     is also the local being re-zeroed every time round. `loopMask` is the test and
 *     `test/resolve/SinkLoop.yana` is the program: an accumulator built in the entry block and
 *     used only inside the loop answers 77 where sinking it answers 45.
 */
void Optimizer::sinkAllocations() {
    auto& candidates = s.candidates;
    candidates.clear();

    eachInst([&](JsInst& inst) {
        /*
         * The three allocations, and **`Alias` with them** - which is not an allocation and is
         * what pins one where it was.
         *
         * The conversion writes `%4 = alias %10` at every local whose storage is the object it
         * was built from, in the block the local was declared in. That alias is the object's
         * only use, so the lowest common ancestor of its uses is that block and nothing moves.
         * An `Alias` emits nothing at all, so moving it is free and sinking it first - the
         * order below is backwards - is what lets the allocation follow it.
         */
        auto sinkable = ((effectsOf(inst) & kJsAllocates) != 0 && inst.kind != JsInst::Clone)
                        || inst.kind == JsInst::Alias;
        if(!sinkable) return;

        if(inst.result) candidates.push(&inst);
    });

    if(!candidates.size() || !computeDominance()) return;

    /*
     * **Backwards**, so that a producer sinks behind the consumer that has already moved.
     *
     * `{items: new Uint32Array([]), length: 0}` is two allocations and the row is the object's
     * only operand. Taken in program order the object moves and the row stays where it was, so
     * `codegen/js/opt.cpp` stops writing the row at its single use and it becomes a `var` of
     * its own. Taken backwards the object moves first and the row then moves in front of it.
     */
    for(Size index = candidates.size(); index > 0; index--) {
        auto inst = candidates[index - 1];
        auto value = base[inst->result];
        if(value->id.index >= liveUses.size() || !liveUses[value->id.index]) continue;

        auto from = base[inst->block]->index.index;
        if(!flow->reachable[from]) continue;

        auto to = kNoBlock;
        auto reachable = true;

        for(auto& use: usesOf(value)) {
            auto at = base[use.inst->block]->index.index;
            if(!flow->reachable[at]) { reachable = false; break; }

            to = to == kNoBlock ? at : commonDominator(at, to);
        }

        /*
         * **Only across a block boundary**, and the refusal is measured rather than argued.
         *
         * Moving an allocation down inside its own block skips nothing: the block is
         * straight-line, so the same paths reach it. What it does do is separate the allocation
         * from operands nothing else reads, and `codegen/js/opt.cpp` writes a value at its
         * single use only where the two are adjacent - so each of them becomes a `var`.
         * `VecMove.yana` is the program, a `new Uint8Array` of sixteen `c ? 1 : 0` selects:
         * **+592 bytes** at each of three goldens and +523 over `Base.js`, for no movement in
         * the time (0.8719x against 0.8783x, which is inside this machine's resolution).
         *
         * An operand rule was written instead and then removed: with the within-block case
         * gone it changes no `.jsir.js.expect` golden at all and costs 1,252 bytes of
         * `Base.js`, because the operands of an allocation that crosses a block boundary are
         * in the block it left.
         */
        if(!reachable || to == kNoBlock || to == from) continue;
        if(flow->loopMask[to] & ~flow->loopMask[from]) continue;

        auto& source = *body.block(base, JsBlockId(from));
        auto& destination = *body.block(base, JsBlockId(to));

        Size at = source.instructionList.size();
        for(Size i = 0; i < source.instructionList.size(); i++) {
            if(base[source.instructionList.get(base, i)] == inst) { at = i; break; }
        }

        if(at == source.instructionList.size()) continue;

        // Ahead of the first use in the destination, or - where the uses are all further down -
        // ahead of the terminator.
        Size before = destination.instructionList.size() ? destination.instructionList.size() - 1 : 0;
        for(Size i = 0; i < destination.instructionList.size(); i++) {
            auto other = base[destination.instructionList.get(base, i)];
            auto names = false;

            eachUse(*other, [&](JsValue* used, JsUseRole) {
                if(used->id.index == value->id.index) names = true;
            });

            if(names) { before = i; break; }
        }

        source.instructionList.remove(base, at);

        destination.instructionList.push(target.arena, inst - base);
        for(Size i = destination.instructionList.size() - 1; i > before; i--) {
            destination.instructionList.set(base, i, destination.instructionList.get(base, i - 1));
        }

        destination.instructionList.set(base, before, inst - base);
        inst->block = &destination - base;

        if(inst->kind != JsInst::Alias) target.optimizer.sunk++;
    }
}

/*
 * An object this body built that nothing ever reads - every use of it is the owner of a
 * property store - is removed together with the stores. J86.
 *
 * `dropDead` cannot see it, because a store is `kJsWrites` and keeps its owner alive, and the
 * owner keeps the store. The shape is a writable local boxed as `{ $v: 0 }` because the SSA it
 * was converted from reached it through an address, where every reader of that address was
 * then inlined and folded away: `PackBench`'s `sumCells` loop wrote `v108.$v = 0` a million
 * times into a box nobody opened. A property store into an object whose identity nothing else
 * observed is invisible, so the store goes and the object is then `dropDead`'s.
 */
void Optimizer::dropWriteOnlyObjects() {
    auto& dead = s.seen;
    dead.reset(body.values.size());
    auto any = false;

    eachInst([&](JsInst& inst) {
        if(inst.kind != JsInst::Object || !inst.result) return;

        auto object = base[inst.result];
        auto uses = usesOf(object);
        if(!uses.size()) return;

        for(auto& use: uses) {
            if(use.role != JsUseRole::Written || use.inst->kind != JsInst::Store) return;

            // The owner, and not the stored value or an index: `{ $v: o }` stored into itself
            // is a use as both, and the value half is a name that escapes.
            auto& written = (JsInstStore&)*use.inst;
            auto& place = builder.place(written.place);
            if(place.kind != JsPlaceKind::Property || !place.owner) return;
            if(base[place.owner] != object) return;
            if(written.value && base[written.value] == object) return;
        }

        dead.set(object->id.index, true);
        any = true;
    });

    if(!any) return;

    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(base, JsBlockId(i));
        if(block->deleted) continue;

        Size at = 0;
        while(at < block->instructionList.size()) {
            auto inst = base[block->instructionList.get(base, at)];
            if(inst->kind != JsInst::Store) { at++; continue; }

            auto& place = builder.place(((JsInstStore&)*inst).place);
            if(place.kind != JsPlaceKind::Property || !place.owner ||
               !dead[base[place.owner]->id.index]) {
                at++;
                continue;
            }

            release(*inst);
            block->instructionList.remove(base, at);

            target.optimizer.removed++;
        }
    }
}

/*
 * A box nobody else sees is a variable.
 *
 * A local whose address a `&` takes is a `Value` place stored as `{ $v: x }` - the demand pass
 * boxes it because the address may reach a callee that rebinds it, and on this target a plain
 * host array *can* be rebound: `growJsArray` assigns `self.items`, which for the bare row is the
 * whole value, and `xs = [...]` through `&xs` is the same write. So the box is right where the
 * descriptor leaves the body. Where every such call was inlined and folded away, every use of
 * the box is a read or a write of its one property, and a property of an object whose identity
 * nothing observes is a variable - `{ $v: [] }` becomes `var v = []` and `v.$v` becomes `v`.
 *
 * A bit range over the box's word, a niche view of it, a descriptor over it, or the box stored
 * anywhere is a name this does not follow, and it refuses.
 */
void Optimizer::unboxLocals() {
    eachInst([&](JsInst& inst) {
        if(inst.kind != JsInst::Object || !inst.result) return;

        auto& object = (JsInstObject&)inst;
        auto& shape = target.shape(object.shape);
        if(shape.kind != JsShapeKind::Cell || shape.properties.size() != 1) return;
        if(object.values.size() != 1) return;

        auto box = base[inst.result];
        auto uses = usesOf(box);

        for(auto& use: uses) {
            JsPlaceId placeId;
            if(use.inst->kind == JsInst::Load) placeId = ((JsInstLoad&)*use.inst).place;
            else if(use.inst->kind == JsInst::Store) {
                auto& store = (JsInstStore&)*use.inst;
                if(store.value && base[store.value] == box) return;
                placeId = store.place;
            } else {
                return;
            }

            auto& place = builder.place(placeId);
            if(place.kind != JsPlaceKind::Property || place.nichePayload) return;
            if(!place.owner || base[place.owner] != box || place.slot != 0) return;
        }

        s.boxes.push(&inst);
    });

    for(auto inst: s.boxes) {
        auto& object = (JsInstObject&)*inst;
        auto box = base[inst->result];
        auto content = target.shape(object.shape).properties[0].rep;
        auto variable = builder.addVariable(content);

        for(auto& use: usesOf(box)) {
            if(use.inst->kind == JsInst::Load) ((JsInstLoad&)*use.inst).place = variable;
            else ((JsInstStore&)*use.inst).place = variable;
        }

        auto initial = object.values.get(base, 0);
        auto store = makeInst<JsInstStore>(base[inst->block], JsRepId(), inst->source, variable, initial);
        replaceInst(inst, store);
        if(initial) base[initial]->uses++;

        target.optimizer.unboxed++;
    }

    s.boxes.clear();
}

// The next instruction on the one path from `block` at `at` - the next in the block, or the first
// of the block its `jump` enters where nothing else enters it. False where the path ends.
bool Optimizer::straightSuccessor(JsBlock*& block, Size& at) {
    auto& list = block->instructionList;
    if(at + 1 < list.size()) {
        at++;
        return true;
    }

    auto last = base[list.get(base, list.size() - 1)];
    if(last->kind != JsInst::Jump) return false;

    auto next = base[((JsInstJump&)*last).to.target];
    if(!next || next->deleted || next->predecessorCount() != 1 || next == block) return false;
    if(!next->instructionList.size()) return false;

    block = next;
    at = 0;
    return true;
}

// The property slot of the copy that `id` writes, through a load of the copy's variable.
bool Optimizer::copyField(JsPlaceId id, IndexSet& owners, U32& slot, bool& bits) {
    auto& place = builder.place(id);
    if(place.nichePayload || place.index || place.bitScale) return false;
    if(!place.owner || !owners[base[place.owner]->id.index]) return false;

    if(place.kind == JsPlaceKind::Property) {
        slot = place.slot;
        bits = false;
        return true;
    }

    if(place.kind == JsPlaceKind::BitRange) {
        auto& word = builder.place(place.word);
        if(word.kind != JsPlaceKind::Property || word.nichePayload || !word.owner) return false;
        if(base[word.owner] != base[place.owner]) return false;
        slot = word.slot;
        bits = true;
        return true;
    }

    return false;
}

/*
 * A record update of an object is one literal.
 *
 * `{v | a: x, b: y}` converts to a clone of `v` into a variable, a store into each named field
 * of the copy - a read-modify-write of `$pN` for a packed one - and the variable read once more
 * where the value goes. What the host is then asked for is an object built with the source's
 * shape, filled in twice. This builds it once, at that last read: every field the update did not
 * name is read out of the source there, a plain field takes the last value stored into it, and a
 * packed word is one variable seeded from the source's word and run through the updates' bit
 * stores - so `{ weight: v.weight, level: v.level, $p0: w }` with `w` computed before it.
 *
 * Reading the source at the last read rather than at the clone is what needs the walk. Every
 * instruction from the clone to that read is on one straight path - a block's successor through
 * a `jump` into a block nothing else enters - and none of them writes anything but a variable or
 * the copy, and none calls. So the source's fields are what they were at the clone, and the
 * replacements, computed on that path, are where they were. Evaluation order is the program's:
 * the source was evaluated once, before the clone, and a replacement that reads the source reads
 * the same fields the literal does.
 *
 * The copy's variable must be stored once (the clone), and every read of it but the last must be
 * nothing but the owner of those field stores. A read of the copy's own field in between, a
 * second store, or a read on another path refuses - `LoopCleanup`'s `stamp` is that shape.
 */
bool Optimizer::updateAsLiteral(JsInstClone& clone) {
    auto copy = base[clone.result];
    auto source = base[clone.from];
    auto& rep = target.rep(copy->rep);
    if(rep.kind != JsRepKind::GcObject || !rep.shape.valid()) return false;

    auto shapeId = rep.shape;
    auto& shape = target.shape(shapeId);
    if(shape.kind != JsShapeKind::Object || !shape.properties.size() || shape.properties.size() > 64) {
        return false;
    }

    // The clone's one use: the store into the copy's variable.
    auto cloneUses = usesOf(copy);
    if(cloneUses.size() != 1 || cloneUses[0].inst->kind != JsInst::Store) return false;
    auto initial = (JsInstStore*)cloneUses[0].inst;
    auto& initialPlace = builder.place(initial->place);
    if(initialPlace.kind != JsPlaceKind::Var || base[initial->value] != copy) return false;
    auto slot = initialPlace.slot;

    // Every read of the variable, anywhere in the body - `indexTouches`' slot, not a body walk.
    if(slot + 1 >= s.touchStart.size()) return false;
    auto& owners = s.seen;
    owners.reset(body.values.size());
    U32 loads = 0;
    for(auto t = s.touchStart[slot]; t < s.touchStart[slot + 1]; t++) {
        auto touch = s.touches[t];
        if(touch->kind == JsInst::Load && isWholeVariable(((JsInstLoad&)*touch).place, slot)) loads++;
    }
    if(!loads) return false;

    // The straight path from the clone, to the first read of the copy that is used as a value.
    auto block = base[clone.block];
    auto at = indexIn(block, &clone);
    if(at == block->instructionList.size()) return false;

    JsInst* last = nullptr;
    auto stored = false;
    U32 seenLoads = 0;
    U32 steps = 0;
    auto& path = s.path;
    path.clear();

    while(straightSuccessor(block, at)) {
        if(++steps > 512) return false;

        auto inst = base[block->instructionList.get(base, at)];
        path.push(inst);

        if(inst == initial) {
            stored = true;
            continue;
        }

        /*
         * Another store into the variable is allowed only off the path. The zero a variable
         * written field-wise is built at (`Conversion::projects`) is one, and since every read
         * of the variable is on the path after the clone's store, nothing reads what it stored.
         */
        if(inst->kind == JsInst::Store && isWholeVariable(((JsInstStore&)*inst).place, slot)) {
            return false;
        }

        if(inst->kind == JsInst::Load && isWholeVariable(((JsInstLoad&)*inst).place, slot)) {
            if(!stored) return false;
            seenLoads++;
            if(!inst->result) continue;

            // An owner of field stores and nothing else, or the last read.
            auto onlyOwner = true;
            for(auto& use: usesOf(base[inst->result])) {
                if(use.inst->kind != JsInst::Store) { onlyOwner = false; break; }
                auto& store = (JsInstStore&)*use.inst;
                if(store.value && base[store.value] == base[inst->result]) { onlyOwner = false; break; }

                U32 field = 0;
                auto bits = false;
                owners.set(base[inst->result]->id.index, true);
                if(!copyField(store.place, owners, field, bits)) { onlyOwner = false; break; }
            }

            if(onlyOwner) continue;

            owners.set(base[inst->result]->id.index, false);
            for(auto& use: usesOf(base[inst->result])) {
                if(use.inst->kind == JsInst::Store && use.role == JsUseRole::Written) return false;
            }

            last = inst;
            break;
        }

        if(inst->kind == JsInst::Store) {
            auto& store = (JsInstStore&)*inst;
            U32 field = 0;
            auto bits = false;
            if(copyField(store.place, owners, field, bits)) continue;
        }

        // A `jump` is how the path goes on; any other way out ends it, and `last` is then unset.
        if(inst->kind == JsInst::Jump) continue;
        if(clobbersHeap(*inst)) return false;
    }

    if(!last) return false;

    // Whether the variable is read again past the path - a `let` of the update read twice.
    // Then the literal is stored into it where the copy was first used, rather than being that
    // use: nothing between the clone and here could have read the variable, and everything
    // after reads the finished object either way.
    auto readLater = seenLoads != loads;
    auto variable = initial->place;

    /*
     * A `clone` copies an object-valued field as deeply as the clone does, and a literal reading
     * the source's field would share it: `{o | .inner.b: 20}` then writes into `o.inner`. So
     * every field the update did not replace must be a value the host cannot write into.
     */
    auto count = shape.properties.size();
    for(Size field = 0; field < count; field++) {
        auto replaced = false;
        for(auto inst: path) {
            if(inst == last) break;
            if(inst->kind != JsInst::Store || inst == initial) continue;

            U32 written = 0;
            auto bits = false;
            if(copyField(((JsInstStore&)*inst).place, owners, written, bits) && written == field) {
                replaced = true;
                break;
            }
        }
        if(replaced) continue;

        switch(target.rep(shape.properties[field].rep).kind) {
            case JsRepKind::Void:
            case JsRepKind::Number:
            case JsRepKind::HostBool:
            case JsRepKind::BigInt:
            case JsRepKind::HostString:
                break;
            default:
                return false;
        }
    }

    // The rewrite. Values per property, in shape order.
    auto& values = s.fieldValues;
    values.clear();
    for(Size i = 0; i < count; i++) values.push(nullptr);

    auto& words = s.fieldWords;
    words.clear();
    for(Size i = 0; i < count; i++) words.push(JsPlaceId());

    auto readSource = [&](JsBlock* where, JsInst* before, U32 field) {
        auto place = builder.addProperty(source, shapeId, U16(field));
        auto load = makeInst<JsInstLoad>(where, target.shape(shapeId).properties[field].rep,
                                         clone.source, place);
        insertAt(where, indexIn(where, before), load);
        return base[load->result];
    };

    for(auto inst: path) {
        if(inst == last) break;
        if(inst->kind != JsInst::Store || inst == initial) continue;

        auto& store = (JsInstStore&)*inst;
        U32 field = 0;
        auto bits = false;
        if(!copyField(store.place, owners, field, bits)) continue;

        auto where = base[inst->block];
        if(bits || words[field].valid()) {
            if(!words[field].valid()) {
                auto content = target.shape(shapeId).properties[field].rep;
                words[field] = builder.addVariable(content);

                auto seed = values[field] ? values[field] : readSource(where, inst, field);
                auto seedStore = makeInst<JsInstStore>(where, JsRepId(), inst->source, words[field],
                                                      seed - base);
                insertAt(where, indexIn(where, inst), seedStore);
                seed->uses++;
            }

            auto& place = builder.place(store.place);
            if(bits) {
                store.place = builder.addBitRange(words[field], place.bitOffset, place.bitWidth,
                                                  place.bitSigned, place.content);
            } else {
                store.place = words[field];
            }
            continue;
        }

        values[field] = base[store.value];
        base[store.value]->uses++;
        removeInst(inst);
    }

    auto where = base[last->block];
    for(Size field = 0; field < count; field++) {
        if(words[field].valid()) {
            auto load = makeInst<JsInstLoad>(where, target.shape(shapeId).properties[field].rep,
                                             clone.source, words[field]);
            insertAt(where, indexIn(where, last), load);
            values[field] = base[load->result];
        } else if(!values[field]) {
            values[field] = readSource(where, last, U32(field));
        }
    }

    auto literal = makeInst<JsInstObject>(where, readLater ? copy->rep : JsRepId(), clone.source, shapeId);
    for(auto value: values) {
        literal->values.push(target.arena, value - base);
        value->uses++;
    }

    // The field stores that were not removed own nothing any more; the reads of the copy go.
    for(auto inst: path) {
        if(inst == last) break;
        if(inst->kind == JsInst::Load && isWholeVariable(((JsInstLoad&)*inst).place, slot)) {
            removeInst(inst);
        }
    }

    removeInst(initial);

    if(readLater) {
        insertAt(where, indexIn(where, last), literal);
        auto store = makeInst<JsInstStore>(where, JsRepId(), clone.source, variable, literal->result);
        insertAt(where, indexIn(where, last), store);
        base[literal->result]->uses++;
        removeInst(&clone);

        target.optimizer.updated++;
        return true;
    }

    replaceInst(last, literal);
    removeInst(&clone);

    // Nothing reads the variable now, so what else was stored into it is dead. The index is this
    // rewrite's own: `initial` is in it and already gone, which `removeInst` answers by doing nothing.
    for(auto t = s.touchStart[slot]; t < s.touchStart[slot + 1]; t++) {
        auto touch = s.touches[t];
        if(touch->kind == JsInst::Store && isWholeVariable(((JsInstStore&)*touch).place, slot)) removeInst(touch);
    }

    target.optimizer.updated++;
    return true;
}

void Optimizer::updatesAsLiterals() {
    auto& clones = s.boxes;
    clones.clear();
    eachInst([&](JsInst& inst) {
        if(inst.kind == JsInst::Clone && inst.result) clones.push(&inst);
    });

    // A refusal changes nothing, so the indexes are rebuilt only after a rewrite - which is what
    // the next clone's question reads. Every question in between is its own slot's touches.
    indexTouches();
    for(auto inst: clones) {
        if(!updateAsLiteral((JsInstClone&)*inst)) continue;
        countPlaceRefs();
        buildUses();
        indexTouches();
    }

    clones.clear();
}

} // namespace jsir
