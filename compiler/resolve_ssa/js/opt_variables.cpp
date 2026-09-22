#include "opt_internal.h"

namespace jsir {

// An instruction stops naming one place and names another - `release`'s place half for the old
// one. The new places here are variables and bit ranges over them, which have no owner to count.
void Optimizer::retarget(JsInst& inst, JsPlaceId to) {
    auto from = placeOfAccess(inst);
    if(inst.kind == JsInst::Load) ((JsInstLoad&)inst).place = to;
    else ((JsInstStore&)inst).place = to;

    if(!from.valid() || from.index >= placeRefs.size()) return;
    if(!placeRefs[from.index] || --placeRefs[from.index]) return;

    auto& place = builder.place(from);
    if(place.owner && base[place.owner]->uses) base[place.owner]->uses--;
    if(place.index && base[place.index]->uses) base[place.index]->uses--;
    if(place.bitScale && base[place.bitScale]->uses) base[place.bitScale]->uses--;
}

// The position of the first whole store of `slot` in `block` at or after `from`, or
// `maxLimit<U32>` where there is none.
U32 Optimizer::firstWholeStore(JsBlock* block, U32 slot, U32 from) {
    auto list = block->instructions(base);
    for(U32 i = from; i < list.size(); i++) {
        auto& next = *base[list[i]];
        if(next.kind != JsInst::Store) continue;

        bool whole = false;
        if(slotOfPlace(((JsInstStore&)next).place, whole) == slot && whole) return i;
    }

    return maxLimit<U32>;
}

/*
 * Which blocks a load in `from` reaches with the slot still holding what it loaded - per block
 * in `s.blockState`, 1 where every path from the load arrives with no whole store of `slot`
 * on it and 2 where some path has one. `leavesClean` says whether the load's own block stores
 * after the load. The walk does not re-enter the load's block: going round through it runs the
 * load again, which is a new value with its own window.
 */
void Optimizer::floodClean(JsBlock* from, U32 slot, bool leavesClean) {
    auto& state = s.blockState;
    state.clear();
    for(U32 i = 0; i < body.blocks.size(); i++) state.push(0);

    auto& work = s.work;
    work.clear();

    auto push = [&](JsBlock* block, U8 entering) {
        if(block == from) return;
        auto index = block->index.index;
        if(state[index] >= entering) return;
        state[index] = entering;
        work.push(index);
    };

    auto successors = [&](JsBlock* block, U8 leaving) {
        auto terminator = block->terminator(base);
        if(!terminator) return;
        forEachEdge(base, *terminator, [&](JsEdge& edge) {
            if(edge.target) push(base[edge.target], leaving);
        });
    };

    successors(from, leavesClean ? 1 : 2);
    while(work.size()) {
        auto block = body.block(base, JsBlockId(work.pop().unwrap()));
        auto entering = state[block->index.index];
        auto leaving = entering == 1 && firstWholeStore(block, slot, 0) == maxLimit<U32> ? 1 : 2;
        successors(block, U8(leaving));
    }
}

/*
 * Scalar replacement of a record local - analysis category 7, and the fold the shipping stage
 * does by forwarding a whole-record store into a field load.
 *
 * A `Var` is the demand pass's statement that the local's address never left the body, so the
 * one thing that can still give the *object* in it an identity is what happens to the value a
 * load of it hands back. Where every store into the slot is an object literal nothing else
 * names, and every load is only ever the owner of a property read, a property write or a bit
 * range over one property, the object is never observed as an object: nobody compares it,
 * passes it, keeps it, returns it or captures it. So the slot becomes one variable per
 * property, each store the stores of its literal's values, and each property access an access
 * of that property's variable - `var s = { file: "a", line: 3 }; ... s.file` is `var s0 = "a";
 * ... s0`, and the literal is dead.
 *
 * A record built on two paths needs nothing further: each path stores its own literal, which
 * becomes each path storing its fields, and a variable is already what a join reads. What this
 * leaves is a run of scalar stores, which `forwardVariables` and `dropDeadStores` finish.
 *
 * What a load observes is the object in the slot *at that moment*, not only an object nobody
 * else names: after `let q = p; p = Point {...}`, `q.x` still reads the old record, and once
 * the slot is fields it would read the new one (J91; `aliasCopies` makes exactly that shape
 * out of `h(p, p)` with `&r` rebinding `p`). So every access through a load's names must lie
 * where no path from the load to it passes a whole store to the slot - after it and before the
 * next store in its own block, or in a block `floodClean` finds reached cleanly. A rule keeping
 * every access in the load's block was the first version: it cost `Base.js` 47 bytes and
 * `BitTag`'s `written` its split, since an inlined `match` reads the fields in its arms.
 *
 * A store of a *constant* record refuses the slot. `shapedValues` answers an `Object` or a
 * `Materialize`, whose operands are values in scope at the store; a `Const` record's fields are
 * one constant, and splitting it would mean minting a constant per field at every store. That
 * was left for a measurement to ask for: the shape is `{x: 1, y: 2}` rebinding a local round a
 * loop, which stays a literal (review A2's `loopSnap`).
 */
void Optimizer::splitRecordVariables() {
    auto count = U32(body.variables.size());
    auto& state = s.slotState;
    auto& shapeOf = s.slotShape;
    state.clear();
    shapeOf.clear();
    for(U32 i = 0; i < count; i++) {
        state.push(0);
        shapeOf.push(maxLimit<U32>);
    }

    // Every instruction's position in its block, for the window a load's names may be used in.
    indexPositions();

    // 0: untouched, 1: loaded only so far, 2: stored and still a candidate, 3: refused.
    constexpr U32 kRefused = 3;

    eachInst([&](JsInst& inst) {
        auto placeId = placeOfAccess(inst);
        if(!placeId.valid()) return;

        bool whole = false;
        auto slot = slotOfPlace(placeId, whole);
        if(slot >= count || state[slot] == kRefused) return;
        if(!whole) { state[slot] = kRefused; return; }

        auto& place = builder.place(placeId);
        auto content = body.variables.get(base, slot);
        if(target.rep(content).kind != JsRepKind::GcObject || place.nichePayload || place.content != content) {
            state[slot] = kRefused;
            return;
        }

        auto agree = [&](JsShapeId shape) {
            if(!shape.valid()) return false;
            if(shapeOf[slot] == maxLimit<U32>) shapeOf[slot] = shape.index;
            return shapeOf[slot] == shape.index;
        };

        if(inst.kind == JsInst::Store) {
            auto value = ((JsInstStore&)inst).value;
            auto stored = value ? base[value] : nullptr;
            auto definer = stored && stored->definer ? base[stored->definer] : nullptr;

            JsShapeId shape;
            auto list = definer ? shapedValues(*definer, shape) : nullptr;
            auto fresh = list && stored->id.index < liveUses.size() && liveUses[stored->id.index] == 1;
            if(fresh) fresh = agree(shape) && fieldsMatch(*list, shape);

            state[slot] = fresh ? 2 : kRefused;
            return;
        }

        if(!state[slot]) state[slot] = 1;
        if(!inst.result) return;

        // The window: from the load to the next whole store of the slot in the same block, and
        // every other block the load reaches with no whole store of the slot on the way.
        auto block = base[inst.block];
        auto from = positionOf(inst);
        auto limit = firstWholeStore(block, slot, from + 1);
        auto flooded = false;

        auto within = [&](JsInst& access) {
            auto at = positionOf(access);
            if(at == maxLimit<U32>) return false;
            auto in = base[access.block];
            if(in == block) return at > from && at < limit;

            if(!flooded) {
                floodClean(block, slot, limit == maxLimit<U32>);
                flooded = true;
            }

            auto index = in->index.index;
            return s.blockState[index] == 1 && at < firstWholeStore(in, slot, 0);
        };

        if(!namesOnlyFields(base[inst.result], agree, within)) state[slot] = kRefused;
    });

    auto& fieldStart = s.fieldStart;
    auto& fieldPlaces = s.fieldPlaces;
    fieldStart.clear();
    fieldPlaces.clear();

    auto any = false;
    for(U32 slot = 0; slot < count; slot++) {
        fieldStart.push(maxLimit<U32>);
        if(state[slot] != 2) continue;

        fieldStart[slot] = fieldPlaces.size();
        for(auto& property: target.shape(JsShapeId(shapeOf[slot])).properties) {
            fieldPlaces.push(builder.addVariable(property.rep));
        }

        target.optimizer.split++;
        any = true;
    }

    if(!any) return;

    auto& accesses = s.candidates;
    accesses.clear();
    eachInst([&](JsInst& inst) {
        bool whole = false;
        auto slot = slotOfPlace(placeOfAccess(inst), whole);
        if(slot < count && state[slot] == 2) accesses.push(&inst);
    });

    // A variable place for field variable `field`, at the content the access it replaces read.
    auto fieldPlace = [&](JsPlaceId field, const JsPlace& was) {
        auto place = builder.place(field);
        if(place.content == was.content && !was.nichePayload) return field;

        place.content = was.content;
        place.nichePayload = was.nichePayload;
        return builder.addPlace(place);
    };

    for(auto inst: accesses) {
        bool whole = false;
        auto slot = slotOfPlace(placeOfAccess(*inst), whole);
        auto first = fieldStart[slot];

        if(inst->kind == JsInst::Store) {
            auto& store = (JsInstStore&)*inst;
            JsShapeId shape;
            auto list = shapedValues(*base[base[store.value]->definer], shape);
            auto block = base[inst->block];
            auto at = indexIn(block, inst);

            auto values = list->contents(base);
            for(U32 i = 0; i < values.size(); i++) {
                auto written = makeInst<JsInstStore>(block, JsRepId(), inst->source, fieldPlaces[first + i], values[i]);
                insertAt(block, at + i, written);
                base[values[i]]->uses++;
            }

            removeInst(inst);
            continue;
        }

        if(!inst->result) continue;

        // Every name `namesOnlyFields` accepted, and so every use is a field access of one.
        forEachName(base[inst->result], [&](JsValue*, Use& use, auto&&) {
            auto& user = *use.inst;
            auto was = builder.place(placeOfAccess(user));
            if(was.kind == JsPlaceKind::Property) {
                retarget(user, fieldPlace(fieldPlaces[first + was.slot], was));
                return true;
            }

            auto word = builder.place(was.word);
            auto range = was;
            range.word = fieldPlace(fieldPlaces[first + word.slot], word);
            range.owner = nullptr;
            range.index = nullptr;
            range.slot = builder.place(range.word).slot;
            range.shape = JsShapeId();
            retarget(user, builder.addPlace(range));
            return true;
        });

    }
}

/*
 * A literal stored whole into storage that *is* an object is its values stored property by
 * property - which is how `structure.cpp` writes a `Value` store anyway, reading each property
 * back out of the literal it has just built. `v.items = new Uint8Array([]); v.length = 0`
 * rather than the same through a `{ items, length }` nobody else sees.
 */
void Optimizer::storeLiteralFields() {
    auto& stores = s.candidates;
    stores.clear();

    eachInst([&](JsInst& inst) {
        if(inst.kind != JsInst::Store) return;

        auto& store = (JsInstStore&)inst;
        auto& place = builder.place(store.place);
        if(place.kind != JsPlaceKind::Value || place.nichePayload || !place.owner || !store.value) return;

        auto& content = target.rep(place.content);
        if(content.kind != JsRepKind::GcObject || !content.shape.valid()) return;

        auto stored = base[store.value];
        if(!stored->definer || stored->id.index >= liveUses.size() || liveUses[stored->id.index] != 1) return;

        JsShapeId shape;
        auto list = shapedValues(*base[stored->definer], shape);
        if(!list || shape != content.shape || !fieldsMatch(*list, shape)) return;

        stores.push(&inst);
    });

    for(auto inst: stores) {
        auto& store = (JsInstStore&)*inst;
        auto owner = base[builder.place(store.place).owner];
        auto shape = target.rep(builder.place(store.place).content).shape;

        JsShapeId built;
        auto values = shapedValues(*base[base[store.value]->definer], built)->contents(base);
        auto block = base[inst->block];
        auto at = indexIn(block, inst);

        for(U32 i = 0; i < values.size(); i++) {
            auto place = builder.addProperty(owner, shape, U16(i));
            auto written = makeInst<JsInstStore>(block, JsRepId(), inst->source, place, values[i]);
            insertAt(block, at + i, written);
            base[values[i]]->uses++;
        }

        removeInst(inst);
        target.optimizer.split++;
    }

    stores.clear();
}

/*
 * A load of a variable one store fills and that store dominates is the value stored - the
 * store-to-load half of promoting a slot to SSA, at the one shape that needs no phi.
 *
 * What `splitRecordVariables` leaves is exactly this shape: a `@caller` `Location` built on the
 * success path of an inlined `check` is four constant stores and four loads in the failure arm,
 * and forwarding them leaves the stores with nothing to read them. The value dominates the
 * store, so it dominates every load the store does; a variable a `BitRange` touches or that is
 * read through a niche payload is refused, since what is read is then not what was stored.
 */
void Optimizer::forwardVariables() {
    if(!computeDominance()) return;
    indexPositions();

    auto count = U32(body.variables.size());
    auto& state = s.slotState;
    auto& stores = s.slotStore;
    state.clear();
    stores.clear();
    for(U32 i = 0; i < count; i++) {
        state.push(0);
        stores.push(nullptr);
    }

    constexpr U32 kRefused = maxLimit<U32>;

    eachInst([&](JsInst& inst) {
        auto placeId = placeOfAccess(inst);
        bool whole = false;
        auto slot = slotOfPlace(placeId, whole);
        if(slot >= count) return;

        if(!whole || builder.place(placeId).nichePayload) { state[slot] = kRefused; return; }
        if(inst.kind != JsInst::Store || state[slot] == kRefused) return;

        auto& store = (JsInstStore&)inst;
        if(!store.value || state[slot]) { state[slot] = kRefused; return; }
        state[slot] = 1;
        stores[slot] = &inst;
    });

    eachInst([&](JsInst& inst) {
        if(inst.kind != JsInst::Load || !inst.result) return;

        bool whole = false;
        auto slot = slotOfPlace(((JsInstLoad&)inst).place, whole);
        if(slot >= count || state[slot] != 1) return;

        auto store = stores[slot];
        auto value = base[((JsInstStore&)*store).value];
        if(value->rep != base[inst.result]->rep) return;
        if(!runsBefore(*store, inst)) return;

        target.optimizer.promoted++;
        pending.push(Rewrite { &inst, value });
    });

    applyRewrites();
}

/*
 * A whole store into a variable that no path reads before the next whole store - analysis
 * category 9, and what `splitRecordVariables` and `forwardVariables` leave behind.
 *
 * The conversion writes a record local's zero - `{ items: new Int32Array([]), length: 0 }` or
 * `{ x: 0, y: 0 }` - where the frame slot is declared, and J77 sinks it but never asks whether
 * anything reads it. Liveness over the one slot answers that: a store is dead where the next
 * access of its slot in its own block is another whole store, or where it is the last access
 * in its block and the slot is live into no successor. A variable is JSIR-local, so no call
 * and no other place can read it; a bit range over one reads it, and writes only part of it.
 *
 * Asked of the slots where the answer can remove something that costs: a slot no load reads,
 * and a slot some store fills with an allocation or a constant. Liveness per slot is a walk of
 * every block, and a thousand-variable body asking it of every scalar would pay for nothing.
 */
void Optimizer::dropDeadStores() {
    if(!flow) flow = &jsCfgOf(target, body);

    auto count = U32(body.variables.size());
    auto blocks = U32(body.blocks.size());

    indexTouches();
    auto& start = s.touchStart;
    auto& touches = s.touches;

    auto isWholeStore = [&](JsInst* inst) {
        if(inst->kind != JsInst::Store) return false;
        bool whole = false;
        slotOfPlace(((JsInstStore*)inst)->place, whole);
        return whole;
    };

    auto& gen = s.blockGen;
    auto& kill = s.blockKill;
    auto& live = s.liveIn;
    gen.clear();
    kill.clear();
    live.clear();
    for(U32 i = 0; i < blocks; i++) {
        gen.push(maxLimit<U32>);
        kill.push(maxLimit<U32>);
        live.push(0);
    }

    auto& dead = s.deadStores;
    dead.clear();

    for(U32 slot = 0; slot < count; slot++) {
        auto first = start[slot];
        auto last = start[slot + 1];
        if(first == last) continue;

        auto loaded = false;
        auto costly = false;
        for(auto i = first; i < last; i++) {
            auto inst = touches[i];
            if(!isWholeStore(inst)) { loaded = true; continue; }

            auto value = base[((JsInstStore*)inst)->value];
            auto definer = value && value->definer ? base[value->definer] : nullptr;
            if(definer && (definer->kind == JsInst::Object || definer->kind == JsInst::Array ||
                           definer->kind == JsInst::Materialize || definer->kind == JsInst::Const)) {
                costly = true;
            }
        }

        if(!loaded) {
            for(auto i = first; i < last; i++) dead.push(touches[i]);
            continue;
        }

        if(!costly) continue;

        // The touches are in block order and in order within one - `eachInst`'s own order.
        for(auto i = first; i < last; i++) {
            auto block = base[touches[i]->block]->index.index;
            if(kill[block] == slot) continue;
            if(isWholeStore(touches[i])) kill[block] = slot;
            else gen[block] = slot;
        }

        for(U32 i = 0; i < blocks; i++) live[i] = 0;

        auto again = true;
        while(again) {
            again = false;
            // `order` holds the reachable blocks only; an unreachable one reads nothing.
            for(U32 at = U32(flow->order.size()); at-- > 0;) {
                auto block = flow->order[at];
                if(body.block(base, JsBlockId(block))->deleted) continue;

                U8 in = gen[block] == slot;
                if(!in && kill[block] != slot) {
                    for(auto k = flow->succStart[block]; k < flow->succStart[block + 1]; k++) {
                        if(live[flow->succList[k]]) { in = 1; break; }
                    }
                }

                if(in && !live[block]) {
                    live[block] = 1;
                    again = true;
                }
            }
        }

        for(auto i = first; i < last; i++) {
            auto inst = touches[i];
            if(!isWholeStore(inst)) continue;

            auto block = base[inst->block]->index.index;
            auto next = i + 1 < last && base[touches[i + 1]->block]->index.index == block
                ? touches[i + 1] : nullptr;

            if(next) {
                if(isWholeStore(next)) dead.push(inst);
                continue;
            }

            auto liveOut = false;
            for(auto k = flow->succStart[block]; k < flow->succStart[block + 1]; k++) {
                if(live[flow->succList[k]]) { liveOut = true; break; }
            }

            if(!liveOut) dead.push(inst);
        }
    }

    for(auto inst: dead) {
        if(!isWholeStore(inst)) continue;
        removeInst(inst);
        target.optimizer.removed++;
    }

    dead.clear();
}

/*
 * A `Load` is `kJsReads` and therefore not removable above, which is right for a place whose
 * read the host could observe and wrong for the two it cannot: a `Var`, which is a JSIR-local
 * and reads `undefined` at worst, and a property of an object *this body built*, which is a
 * property read that cannot throw because the owner is known not to be `undefined`. Those are
 * what `forwardFields` leaves behind when the object is read more times than it is forwarded.
 *
 * Deliberately not every property read: `owner.f` where the owner came from elsewhere throws if
 * the owner turns out to be `undefined`, and removing a throw is a change to what the program
 * does even where the program is wrong.
 */
void Optimizer::dropDeadLoads() {
    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(base, JsBlockId(i));
        if(block->deleted) continue;

        Size at = 0;
        while(at < block->instructionList.size()) {
            auto inst = base[block->instructionList.get(base, at)];

            auto used = inst->result && base[inst->result]->id.index < liveUses.size()
                ? liveUses[base[inst->result]->id.index] : 1;

            if(inst->kind != JsInst::Load || !inst->result || used) {
                at++;
                continue;
            }

            auto& place = builder.place(((JsInstLoad&)*inst).place);
            auto removable = place.kind == JsPlaceKind::Var ||
                             (place.kind == JsPlaceKind::Property && place.owner &&
                              isFreshHere(base[place.owner]));

            if(!removable) { at++; continue; }

            release(*inst);
            block->instructionList.remove(base, at);

            target.optimizer.removed++;
        }
    }
}

} // namespace jsir
