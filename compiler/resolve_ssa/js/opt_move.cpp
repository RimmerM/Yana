#include "opt_internal.h"

namespace jsir {

/*
 * Whether a load of the slot is preceded, in its own iteration, by a store to it - so that a
 * clone moved out of the slot in one iteration is never what the next iteration reads.
 *
 * `runsBeforeInIteration` has already put the load in the clone's loops, so any path from the
 * clone back to it passes the innermost header; a store in a block dominating the load and in
 * the same loops is then on every such path, after the header and before the load.
 * `markItems`' `xs[i] = {xs[i] | ...}` is the shape: the update's copy is stored into its slot
 * at the top of the body and moved into the array at the bottom.
 */
bool Optimizer::refilledFirst(JsInst& load, U32 first, U32 last, Array<JsInst*>& touches) {
    if(load.kind != JsInst::Load) return true;

    for(U32 t = first; t < last; t++) {
        auto& store = *touches[t];
        if(store.kind != JsInst::Store) continue;
        if(runsBeforeInIteration(store, load)) return true;
    }

    return false;
}

/*
 * A value whose object, and every object under it, nobody else has a name for.
 *
 * An allocation this body made, used exactly once, whose object-valued operands are the same -
 * or a `clone` that is still one, which is a deep copy by construction. What it deliberately
 * does not accept is an `alias`: an object a rewrite here already decided to share is the one
 * thing a move must not hand out a second name for, and `AliasLastUse.yana`'s record built
 * around an aliased field is the program that would answer wrongly if it did.
 */
bool Optimizer::ownedOutright(JsValue* value, U32 depth) {
    if(target.rep(value->rep).isPrimitive()) return true;
    if(depth > 8 || !value->definer) return false;
    if(value->id.index >= liveUses.size() || liveUses[value->id.index] != 1) return false;

    auto& definer = *base[value->definer];
    if(definer.kind == JsInst::Clone) return true;

    if(definer.kind != JsInst::Object && definer.kind != JsInst::Array &&
       definer.kind != JsInst::Materialize) {
        return false;
    }

    auto owned = true;
    forEachUse(base, definer, [&](JsirPtr<JsValue> used) {
        if(owned && used && !ownedOutright(base[used], depth + 1)) owned = false;
    });

    return owned;
}

/*
 * J72 §9 item 5 - the live-range half of §6.3: a `clone` at the **last use** of a local is a
 * move, and becomes an alias whatever is done with the copy afterwards.
 *
 * `readMode` copies a trivially copyable value where the source is still *nameable*, and it
 * cannot know that nothing names it again - `return p` from a `let &p` is the program, and so is
 * `let q = p` as the last mention of `p`. The four conditions in `aliasCopies` refuse both: the
 * first because a returned copy is kept, the third because the walk over the slot is
 * whole-body and so counts a write that happened *before* the copy. A move needs neither
 * condition, and needs three others instead:
 *
 *  1. **every name for the object is dead at the clone** - every load of the slot, every alias
 *     of one and every object-valued property read out of one is used only before the clone,
 *     and the slot itself is neither read nor written after it. "Before" is the clone's own
 *     block, earlier, or a block dominating it (see `runsBefore`). In a loop (J86) it is "earlier
 *     in this iteration" (`runsBeforeInIteration`), stores into the slot may be anywhere, and
 *     every load of the slot must follow a store in its own iteration (`refilledFirst`);
 *  2. **none of those names was ever kept** - not stored into another place, not built into an
 *     aggregate, not handed to a call at a position it may keep or write, not given to a host
 *     row. A name that went somewhere is a name this walk cannot see die;
 *  3. **every object the slot ever holds is owned outright** - every store into the slot, and
 *     every store of an object into a property of one of its names, stores something
 *     `ownedOutright` accepts. This is the condition the move adds and the one it cannot do
 *     without: aliasing the copy hands out the *nested* objects too, and one of them might be a
 *     field `aliasCopies` shared with a record still in use.
 *
 * The alternative was a liveness analysis over the slot, which is what the plan called for.
 * Refusing every clone in a loop and every name used in a block the clone's does not dominate
 * is narrower and needed no new analysis; the shapes the measurement named are straight-line.
 */
void Optimizer::moveLastUses() {
    if(!computeDominance()) return;
    indexPositions();
    indexTouches();

    auto& touchStart = s.touchStart;
    auto& touches = s.touches;

    auto& names = s.seen;
    auto& work = s.work;

    eachInst([&](JsInst& inst) {
        if(inst.kind != JsInst::Clone || !inst.result) return;

        auto& written = (JsInstClone&)inst;
        if(!written.from) return;

        auto block = base[inst.block]->index.index;
        if(!flow->reachable[block]) return;

        // In a loop, "before" is "earlier in this iteration" - see `runsBeforeInIteration` -
        // and the slot has to be filled again before anything reads it on the next one.
        auto inLoop = flow->loopMask[block] != 0;
        auto before = [&](JsInst& other) {
            return inLoop ? runsBeforeInIteration(other, inst) : runsBefore(other, inst);
        };

        // The slot: the source, through aliases and object-valued property reads, is a load of
        // a variable.
        auto source = base[written.from];
        auto root = rootOf(source);
        for(U32 depth = 0; depth < 8 && root->definer; depth++) {
            auto definer = base[root->definer];
            if(definer->kind != JsInst::Load) break;

            auto& place = builder.place(((JsInstLoad&)*definer).place);
            if(place.kind == JsPlaceKind::Var || !place.owner) break;
            if(place.kind != JsPlaceKind::Property) return;

            root = rootOf(base[place.owner]);
        }

        if(!root->definer || base[root->definer]->kind != JsInst::Load) return;

        auto& rootPlace = builder.place(((JsInstLoad&)*base[root->definer]).place);
        if(rootPlace.kind != JsPlaceKind::Var) return;
        auto slot = rootPlace.slot;

        names.reset(body.values.size());
        work.clear();

        // Every touch of the slot: before the clone, and only ever holding what it owns.
        auto ok = slot + 1 < touchStart.size();
        for(U32 t = ok ? touchStart[slot] : 0; ok && t < touchStart[slot + 1]; t++) {
            auto& other = *touches[t];

            // A store in a loop may be anywhere: it rebinds the slot, and what it rebinds it to
            // is asked below. What makes that safe is `refilledFirst`, asked of every load.
            if(inLoop && other.kind == JsInst::Store) {
                auto value = ((JsInstStore&)other).value;
                if(!value || !ownedOutright(base[value])) ok = false;
                continue;
            }

            if(!before(other)) { ok = false; break; }
            if(inLoop && !refilledFirst(other, touchStart[slot], touchStart[slot + 1], touches)) {
                ok = false;
                break;
            }

            if(other.kind == JsInst::Store) {
                auto value = ((JsInstStore&)other).value;
                if(!value || !ownedOutright(base[value])) ok = false;
                continue;
            }

            if(!other.result) continue;
            auto loaded = base[other.result];
            if(!names[loaded->id.index]) {
                names.set(loaded->id.index, true);
                work.push(loaded->id.index);
            }
        }

        // Conditions 1 and 2 over every name the slot's object goes by.
        U32 steps = 0;
        for(Size i = 0; ok && i < work.size(); i++) {
            if(++steps > 4096) { ok = false; break; }

            auto name = base[body.values.get(base, work[i])];
            for(auto& use: usesOf(name)) {
                if(use.inst == &inst) continue;
                if(!before(*use.inst)) { ok = false; break; }

                auto follow = [&](JsValue* value) {
                    if(names[value->id.index]) return;
                    names.set(value->id.index, true);
                    work.push(value->id.index);
                };

                switch(use.role) {
                    case JsUseRole::Read:
                        if(use.inst->kind == JsInst::Load && use.inst->result &&
                           !target.rep(base[use.inst->result]->rep).isPrimitive()) {
                            follow(base[use.inst->result]);
                        }
                        continue;

                    case JsUseRole::Aliased:
                        if(!use.inst->result) { ok = false; break; }
                        follow(base[use.inst->result]);
                        continue;

                    // A write *into* the object is a statement about what the copy will hold,
                    // and it ran first - so only what it wrote has to be owned.
                    case JsUseRole::Written: {
                        if(use.inst->kind != JsInst::Store) { ok = false; break; }

                        auto value = ((JsInstStore&)*use.inst).value;
                        if(value && !ownedOutright(base[value])) ok = false;
                        continue;
                    }

                    case JsUseRole::Kept:
                    case JsUseRole::Opaque:
                        ok = false;
                        break;
                }

                if(!ok) break;
            }
        }

        if(!ok || !names[source->id.index]) return;

        target.optimizer.aliased++;
        pending.push(Rewrite { &inst, source });
    });

    applyRewrites();
}

/*
 * A `clone` read only for numbers before anything could write - `%c = clone %x` followed by
 * `load property %c.a` and `%c.b` - answers the reads from the source.
 *
 * That is `let f = s.flags` taken apart field by field in the next two statements, and it is
 * `ScalarRecord`'s `{a: s0.flags.a, b: s0.flags.b}` built only to be read twice. `aliasCopies`
 * refuses it whenever the source is a property (condition 2) or was written anywhere in the body
 * (condition 3), and neither matters here: a read of a number sees the value the object has
 * *when it runs*, so the only question is whether anything between the clone and the read could
 * have changed it. Every read in the clone's own block, and nothing with `kJsWrites` or
 * `kJsCalls` between the clone and the last of them - a store anywhere, since this pass does not
 * know which places overlap, and a call, since a callee may write whatever it can reach. A store
 * into a `Var` is the one write let through, because what it writes is a JavaScript local and
 * not a property of anything.
 *
 * The copy may also go through **one** variable first, since `let i = o.inner` is a store of
 * the clone into `i`'s slot and a load back out of it: every touch of that slot in the clone's
 * block, every other store to it before this one and every load of it after, so each load
 * answers this copy and nothing else.
 *
 * Deliberately primitive reads only: an object-valued property read out of the copy would be a
 * second name for something inside it, whose lifetime is the name's and not the window's.
 */
void Optimizer::readThroughCopies() {
    indexPositions();
    indexTouches();

    auto& touchStart = s.touchStart;
    auto& touches = s.touches;

    eachInst([&](JsInst& inst) {
        if(inst.kind != JsInst::Clone || !inst.result) return;

        auto& written = (JsInstClone&)inst;
        if(!written.from) return;

        auto copy = base[inst.result];
        auto at = positionOf(inst);
        auto last = at;
        auto any = false;

        // One primitive read of `name`, in this block after the clone.
        auto readOf = [&](JsValue* name, Use& use) {
            if(use.role != JsUseRole::Read || use.inst->kind != JsInst::Load) return false;
            if(use.inst->block != inst.block || !use.inst->result) return false;

            auto& place = builder.place(((JsInstLoad&)*use.inst).place);
            if(place.kind != JsPlaceKind::Property || base[place.owner] != name) return false;
            if(!target.rep(place.content).isPrimitive()) return false;

            auto position = positionOf(*use.inst);
            if(position <= at || position == maxLimit<U32>) return false;
            if(position > last) last = position;
            any = true;
            return true;
        };

        auto throughVariable = false;
        for(auto& use: usesOf(copy)) {
            if(readOf(copy, use)) continue;

            // The one variable the copy may pass through.
            if(throughVariable || use.role != JsUseRole::Kept || use.inst->kind != JsInst::Store) return;
            if(use.inst->block != inst.block) return;

            auto& into = builder.place(((JsInstStore&)*use.inst).place);
            if(into.kind != JsPlaceKind::Var || ((JsInstStore&)*use.inst).value != inst.result) return;
            throughVariable = true;

            auto slot = into.slot;
            auto storedAt = positionOf(*use.inst);
            if(slot + 1 >= touchStart.size()) return;

            for(U32 t = touchStart[slot]; t < touchStart[slot + 1]; t++) {
                auto& other = *touches[t];
                if(other.block != inst.block) return;
                if(&other == use.inst) continue;

                auto position = positionOf(other);
                if(other.kind == JsInst::Store) {
                    if(position > storedAt) return;
                    continue;
                }

                if(position < storedAt) return;
                if(!other.result) continue;

                auto loaded = base[other.result];
                for(auto& read: usesOf(loaded)) {
                    if(!readOf(loaded, read)) return;
                }
            }
        }

        if(!any) return;

        auto block = base[inst.block];
        for(auto i = at + 1; i < last; i++) {
            auto& between = *base[block->instructionList.get(base, i)];
            if(clobbersHeap(between)) return;
        }

        target.optimizer.aliased++;
        pending.push(Rewrite { &inst, base[written.from] });
    });

    applyRewrites();
}

} // namespace jsir
