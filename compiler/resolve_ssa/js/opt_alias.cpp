#include "opt_internal.h"

/*
 * §6.3's copy-to-alias elision, and the two rewrites it needs to pay.
 *
 * ## What is being removed
 *
 * `readMode`'s first clause turns a shared, call-bounded loan of a *trivially copyable* value into a
 * `copy` - Analysis-IR-JS.md §1.3.1 category 2, where the cost guard that used to stop it is
 * surveyed. Natively that is a stack copy of a few words and `promote` takes it away again; here it
 * is a structural duplicate, property by property and recursively (`cloneOf` in
 * `codegen/js/structure.cpp`), so `showWith(s)` over a two-field record of two records writes
 * eighteen property reads at every borrow. Measured on `test/base` before this file existed: 2,954
 * of them in `Base.js`, 262 KB of object literals against the shipping backend's 5.5 KB, which is a
 * quarter of the whole size gap. The shipping backend never copies at a borrow at all - its
 * `keptValue` duplicates only at a *store* or a `return` whose source still names live storage.
 *
 * ## The safety argument, in the order the code asks it
 *
 * A clone may become an alias when the duplicate and the original cannot be told apart for as long
 * as either is reachable. Four conditions, and the third is the one that does the work:
 *
 *  1. **the copy is never written and never kept** - every use of it, following `Alias` through, is
 *     a property read, another clone, or an argument at a position the callee's frozen `JsAbi`
 *     marks `shared`. An `Object`, a `Store`, a `return`, an edge operand and a `Host` call all
 *     refuse, because each of them keeps the reference somewhere this body stops being able to see;
 *  2. **the source names storage nothing outside this body can reach** - a `Var` slot, which is
 *     exactly the demand pass's statement that the address never escaped, or a parameter at a
 *     `shared` position, which is the caller's loan and holds for the call, or an allocation this
 *     body made. A `Property`, an `Element`, a `Cell` or a `Global` root is storage some other name
 *     in the program can reach, where the walk below proves nothing;
 *  3. **the object in that storage is never written** - the same read-only walk applied to the
 *     source, and to every other value loaded from the same slot, and to every value stored into
 *     it. A `Var` whose object is handed to a `&` position, or written field-wise, or passed at a
 *     consuming position, fails here;
 *  4. **what a `Var` holds was built here** - every store into the slot stores a fresh allocation,
 *     so the object cannot be one somebody else also has a name for.
 *
 * Conditions 2 and 3 are what make condition 1's call case safe without inspecting the call's
 * *other* arguments. `f(p, &p)` is legal for a trivially copyable `p` precisely because the copy
 * holds no loan - and the walk is asked of the **source** as well, which sees `&p` as a position the
 * callee may write and as a `Value` place a store names. So the call is refused from the source's
 * side rather than by reasoning about the argument list.
 *
 * `test/resolve/AliasWrite.yana` is that program and `test/resolve/AliasLoop.yana` is the pair for a
 * `Var` whose object is rewritten round a loop; each answers a different number under Node when its
 * condition is weakened, and `doc/progress/2026-09-22-J72-jsir-alias-scalarize.md` §7 is the table
 * of which weakening breaks which - including the two that break nothing the corpus can see.
 *
 * ## The two rewrites beside it
 *
 * A clone whose result goes straight into an object literal is refused by condition 1, and the
 * literal is very often one the next instruction takes apart again - `Failure {site, claim, detail}`
 * built to hand its three fields to `recordAt`. So `forwardFields` answers a property read from the
 * literal that built it where the literal never escapes and is never written, which turns those
 * clones into clones-of-clones and property reads, and `dropDead` then removes the literal. Neither
 * is general CSE or general store forwarding; both are §6.6's first and fifth bullets at exactly the
 * shape this slice's measurement named.
 *
 * Every rewrite here replaces one instruction with an `Alias` of a value that is already in scope,
 * which is why none of them has to rewrite an operand list: the result value keeps its identity and
 * its users never learn anything changed. `Alias` emits nothing at all in `structure.cpp`.
 */
namespace jsir {

/*
 * Whether every use of this object, and of every second name for it, only reads it.
 *
 * `allowStoreInto` is the one store that is not a write of the object but a write of the *slot*
 * holding it, and it is how a local's initializer is let through: `store var $0, %4` rebinds the
 * variable and leaves `%4`'s properties alone. A store into any other place with `%4` as its
 * value is a `Kept` and refuses, because the object then has a name this body cannot follow.
 *
 * Depth-bounded and budgeted rather than worklisted, and both bounds only ever *refuse*: an
 * alias chain in converted JSIR is one or two links, and the walk re-enters through an aggregate
 * and through a variable, so a cycle is reachable and the cost of one query is otherwise the
 * size of the body times the depth.
 */
bool Optimizer::readOnly(JsValue* value, U32 allowStoreInto, U32 depth) {
    if(depth > 8 || !budget) return false;
    budget--;

    for(auto& use: usesOf(value)) {
        switch(use.role) {
            /*
             * A read of the value, and the one read that is also a *name for something inside
             * it*.
             *
             * `load property o.f` does not write `o` and is a read of it - but where `f` holds
             * an object, what comes back is a second name for that object, and a write through
             * it lands inside `o`. So a copy of `o` aliased on the strength of this arm would
             * share whatever `f` points at, which is exactly what `cloneOf` recurses for.
             * `AggregateAlias.yana`'s second case is the program: a `[Int *4]` in a record
             * field, written through the copy and read back through the source.
             *
             * A property whose content is a number, a bigint or a host string has nothing under
             * it to write, which is why the test is the content's representation rather than
             * the place's kind.
             */
            case JsUseRole::Read: {
                if(use.inst->kind != JsInst::Load || !use.inst->result) continue;

                auto& place = builder.place(((JsInstLoad&)*use.inst).place);
                if(place.owner && base[place.owner] != value) continue;
                if(target.rep(place.content).isPrimitive()) continue;

                if(!readOnly(base[use.inst->result], allowStoreInto, depth + 1)) return false;
                continue;
            }

            case JsUseRole::Aliased:
                if(!use.inst->result) return false;
                if(!readOnly(base[use.inst->result], allowStoreInto, depth + 1)) return false;
                continue;

            /*
             * The reference goes somewhere, and two somewheres are still inside this walk.
             *
             * An **aggregate** built here is another name for reaching what is in it: writing a
             * field of the aggregate replaces the field rather than the object, so a reference
             * kept in one is read-only exactly when the aggregate is. `{value: v, style: s}`
             * handed to a shared parameter is where that arrives - the duplicate is refused by
             * the aggregate and the aggregate is passed read-only.
             *
             * A **variable** is the same statement for the storage the demand pass proved has no
             * address: what can be done to the object through the slot is what can be done to
             * every value read out of it.
             */
            case JsUseRole::Kept: {
                if(use.inst->kind == JsInst::Object || use.inst->kind == JsInst::Array ||
                   use.inst->kind == JsInst::Materialize) {
                    if(!use.inst->result) return false;
                    if(!readOnly(base[use.inst->result], allowStoreInto, depth + 1)) return false;
                    continue;
                }

                if(use.inst->kind != JsInst::Store) return false;

                auto& place = builder.place(((JsInstStore&)*use.inst).place);
                if(place.kind != JsPlaceKind::Var) return false;
                if(place.slot == allowStoreInto) continue;
                if(!variableIsReadOnly(place.slot, depth + 1)) return false;
                continue;
            }

            case JsUseRole::Written:
            case JsUseRole::Opaque:
                return false;
        }
    }

    return true;
}

// Every value read out of one variable slot, read-only - see the `Kept` arm above.
bool Optimizer::variableIsReadOnly(U32 slot, U32 depth) {
    if(depth > 8 || !budget) return false;
    budget--;

    auto ok = true;
    eachInst([&](JsInst& inst) {
        if(!ok || inst.kind != JsInst::Load || !inst.result) return;

        auto& from = builder.place(((JsInstLoad&)inst).place);
        if(from.kind != JsPlaceKind::Var || from.slot != slot) return;
        if(!readOnly(base[inst.result], slot, depth + 1)) ok = false;
    });

    return ok;
}

/*
 * A value nobody else can have a name for: this body built the object. What a `Var` has to hold
 * for condition 4, and the reason an object arriving from a call or out of a field refuses.
 *
 * **The effects column and not a list.** This restated `inst.def`'s four `kJsAllocates` rows,
 * and the two had to agree without anything saying so - an opcode added there with that flag
 * and not added here is a fresh object this pass declines to see, which costs nothing anybody
 * would notice and is exactly the kind of drift a generated schema exists to remove (§8.3).
 * `Host` is the one kind whose effects are a row rather than a column and no row of
 * `host.def` allocates, so the answer is the same four opcodes said once.
 */
bool Optimizer::isFreshHere(JsValue* value) {
    if(!value->definer) return false;
    return (effectsOf(*base[value->definer]) & kJsAllocates) != 0;
}

/*
 * Through every `Alias` to the value that named the object first.
 *
 * An alias is a second name and not a second object, so the question "what storage is this" is
 * the root's. Without this a clone of a value this pass has *already* aliased answers "neither a
 * `Var` nor a parameter", which is how the second duplicate of a `Site` survived the round that
 * removed the first.
 */
JsValue* Optimizer::rootOf(JsValue* value) {
    for(U32 depth = 0; depth < 8 && value->definer; depth++) {
        auto definer = base[value->definer];
        if(definer->kind != JsInst::Alias) break;

        auto from = ((JsInstAlias&)*definer).from;
        if(!from) break;

        value = base[from];
    }

    return value;
}

/*
 * Whether the storage this value was read out of holds an object nothing writes - conditions 2,
 * 3 and 4 together.
 *
 * The two roots that qualify are the two the resolver has already made a statement about. A
 * `Var` is the demand pass's answer that the address never escaped, so no `{$o, $k}` naming it
 * exists anywhere in the program and the only writer it can have is a place in this body rooted
 * at a value loaded from it - which the read-only walk covers. A **parameter at a `shared`
 * position** is the caller's loan, which forbids a write for the length of the call; the walk
 * covers this body's own half of it.
 *
 * Everything else refuses by construction: a `Property`, an `Element`, a `Cell` or a `Global`
 * root names storage some other name can reach, and a `Value` place is storage that *is* the
 * object - so a whole-value store into it writes the properties the alias would be reading.
 */
bool Optimizer::sourceIsStable(JsValue* value) {
    auto source = rootOf(value);

    if(source->id.index < sharedParameter.size() && sharedParameter[source->id.index]) {
        return readOnly(source, kNoVariable);
    }

    if(!source->definer) return false;

    /*
     * An allocation this body made and nothing has a second name for - the third root, and the
     * one a clone of a clone reaches.
     *
     * `Failure {site, claim, detail}` built to hand its three fields to `recordAt` is where it
     * arrives: `forwardFields` answers the `.site` read from the literal, and what is left is a
     * duplicate of a duplicate. The read-only walk is the whole condition here, because a value
     * that was allocated in this body starts unshared - so "never written and never handed to a
     * position that could write it" is "never written by anybody".
     */
    if(isFreshHere(source)) return readOnly(source, kNoVariable);

    auto definer = base[source->definer];
    if(definer->kind != JsInst::Load) return false;

    auto& place = builder.place(((JsInstLoad&)*definer).place);
    if(place.kind != JsPlaceKind::Var) return false;

    auto slot = place.slot;
    auto stable = true;

    eachInst([&](JsInst& inst) {
        if(!stable) return;

        if(inst.kind == JsInst::Store) {
            auto& written = (JsInstStore&)inst;
            auto& into = builder.place(written.place);
            if(into.kind != JsPlaceKind::Var || into.slot != slot) return;

            // Condition 4: what the slot holds was built here, so it is nobody else's.
            if(!written.value || !isFreshHere(base[written.value])) stable = false;
            return;
        }

        if(inst.kind != JsInst::Load) return;

        auto& from = builder.place(((JsInstLoad&)inst).place);
        if(from.kind != JsPlaceKind::Var || from.slot != slot) return;

        // Condition 3, for every name the slot's object is read out under - not only the one
        // the clone happens to hold.
        if(!inst.result || !readOnly(base[inst.result], slot)) stable = false;
    });

    return stable;
}

/*
 * §6.3, at every clone whose four conditions hold.
 *
 * The clone is not deleted - it becomes an `Alias`, which emits nothing and says in the dump
 * that a sharing decision was taken. That is `inst.def`'s reason for the node existing at all.
 */
void Optimizer::aliasCopies(bool count) {
    eachInst([&](JsInst& inst) {
        if(inst.kind != JsInst::Clone) return;

        if(count) target.optimizer.clones++;

        auto& written = (JsInstClone&)inst;
        if(!written.from || !inst.result) return;

        auto source = base[written.from];
        auto copy = base[inst.result];

        // Condition 1, then 2-4. In this order because the copy's own users are the cheap half
        // and are what refuses most sites.
        budget = 1024;
        if(!readOnly(copy, kNoVariable)) return;

        budget = 1024;
        if(!sourceIsStable(source)) return;

        target.optimizer.aliased++;
        pending.push(Rewrite { &inst, source });
    });

    applyRewrites();
}

// The result of a whole read of a `Value` place - which is the owner itself - or null.
JsValue* Optimizer::wholeValueRead(JsInst& inst) {
    if(inst.kind != JsInst::Load || !inst.result) return nullptr;

    auto& place = builder.place(((JsInstLoad&)inst).place);
    if(place.kind != JsPlaceKind::Value || place.nichePayload || !place.owner) return nullptr;
    return base[inst.result];
}

/*
 * Whether every name for `value` - it and its aliases - only reads it. A clone reads its source
 * and a shared argument is the callee's loan, so both are reads; anything that writes through
 * a name, keeps one or hands it back is not.
 */
bool Optimizer::namesOnlyRead(JsValue* value) {
    return forEachName(value, [&](JsValue*, Use& use, auto&& follow) {
        // A read of a `Value` place is the object itself under a second name.
        if(auto whole = wholeValueRead(*use.inst)) {
            follow(whole);
            return true;
        }

        return use.role == JsUseRole::Read;
    });
}

/*
 * The literal a value names, through aliases and through clones nothing writes (J89) - or null.
 *
 * An `Alias` is the same object; a clone nobody writes holds what its source held when it was
 * made, and a source nobody writes holds what it was built with. So a field read at the end of
 * the chain is the literal's operand either way - for a primitive, since a clone copies an
 * object-valued field and `copied` says one was crossed. The literal itself must only be read,
 * which is `forwardFields`'s original condition, now asked of every name for it rather than of
 * one.
 */
JsInst* Optimizer::literalOf(JsValue* value, bool& copied) {
    copied = false;
    for(U32 depth = 0; value && value->definer && depth < 16; depth++) {
        auto definer = base[value->definer];

        if(wholeValueRead(*definer)) {
            value = base[builder.place(((JsInstLoad&)*definer).place).owner];
            continue;
        }

        if(definer->kind == JsInst::Alias) {
            value = base[((JsInstAlias&)*definer).from];
            continue;
        }

        if(definer->kind == JsInst::Clone) {
            if(!namesOnlyRead(value)) return nullptr;
            copied = true;
            value = base[((JsInstClone&)*definer).from];
            continue;
        }

        if(definer->kind != JsInst::Object && definer->kind != JsInst::Materialize) return nullptr;
        return namesOnlyRead(value) ? definer : nullptr;
    }

    return nullptr;
}

/*
 * The values an object-building instruction puts in its shape's properties, in shape order: an
 * object literal's, or a materialization's where what it builds is one host object - which
 * `structure.cpp` spells as the literal of the result's shape, so part `i` is property `i`.
 */
JsirList<JsirPtr<JsValue>, false>* Optimizer::shapedValues(JsInst& inst, JsShapeId& shape) {
    if(inst.kind == JsInst::Object) {
        shape = ((JsInstObject&)inst).shape;
        return &((JsInstObject&)inst).values;
    }

    if(inst.kind != JsInst::Materialize || !inst.result) return nullptr;

    auto& to = target.rep(base[inst.result]->rep);
    if(to.kind != JsRepKind::GcObject || !to.shape.valid()) return nullptr;

    auto& values = ((JsInstMaterialize&)inst).values;
    if(values.size() != target.shape(to.shape).properties.size()) return nullptr;

    shape = to.shape;
    return &values;
}

/*
 * Whether a literal's values can each be stored into its own property as they are: one per
 * property, none missing, and each at the property's representation. A literal may hold a
 * value at another representation than its property's - `test/base`'s unoptimized row has one
 * at every `check(False, note: ...)` - and storing that into a place of the property's content
 * is a store the verifier refuses.
 */
bool Optimizer::fieldsMatch(JsirList<JsirPtr<JsValue>, false>& list, JsShapeId shape) {
    auto& properties = target.shape(shape).properties;
    if(list.size() != properties.size()) return false;

    auto values = list.contents(base);
    for(U32 i = 0; i < values.size(); i++) {
        if(!values[i] || base[values[i]]->rep != properties[i].rep) return false;
    }

    return true;
}

/*
 * §6.6's load forwarding, at the one shape the measurement named: a property read of an object
 * literal this body built, never wrote and never handed anywhere.
 *
 * The object having no `Written`, `Kept` or `Opaque` use is what makes its properties constant
 * from its definition onward, and SSA dominance is what makes the operand available at the read:
 * the load uses the object, so the object's definition dominates it.
 */
void Optimizer::forwardFields() {
    eachInst([&](JsInst& inst) {
        if(!inst.result) return;

        /*
         * A `split` of a materialized record is the part it was built from - the `@caller`
         * `Location` an inlined `check` hands to `recordAt` is four parts materialized into an
         * object and split straight back out of it, on the failure path, per check (J89).
         */
        if(inst.kind == JsInst::Split) {
            auto& split = (JsInstSplit&)inst;
            auto copied = false;
            auto definer = literalOf(base[split.from], copied);
            if(!definer || definer->kind != JsInst::Materialize) return;

            auto& built = (JsInstMaterialize&)*definer;
            if(built.parts != split.parts || split.part >= built.values.size()) return;

            auto part = built.values.get(base, split.part);
            if(!part || (copied && !target.rep(base[part]->rep).isPrimitive())) return;

            target.optimizer.forwarded++;
            pending.push(Rewrite { &inst, base[part] });
            return;
        }

        if(inst.kind != JsInst::Load) return;

        auto& place = builder.place(((JsInstLoad&)inst).place);
        if(place.kind != JsPlaceKind::Property || !place.owner) return;

        auto copied = false;
        auto definer = literalOf(base[place.owner], copied);
        JsShapeId shape;
        auto list = definer ? shapedValues(*definer, shape) : nullptr;
        if(!list || shape != place.shape) return;

        auto values = list->contents(base);
        if(place.slot >= values.size() || !values[place.slot]) return;

        // A clone copies an object-valued field, so the literal's own object is not the one
        // the read would have handed back - `ErasedCopy.yana` answers -7 rather than 25.
        if(copied && !target.rep(base[values[place.slot]]->rep).isPrimitive()) return;

        target.optimizer.forwarded++;
        pending.push(Rewrite { &inst, base[values[place.slot]] });
    });

    applyRewrites();
}

/*
 * Whatever the two rewrites above left with no user at all - §6.6's fifth bullet.
 *
 * An allocation counts as removable exactly because it has no user: `inst.def` records that
 * `Object` is `kJsAllocates` rather than pure because its *identity* is observable, and an
 * object nothing names has no identity anybody observed. Everything that reads storage, writes
 * it, calls or throws stays whatever its result count is.
 */
void Optimizer::dropDead() {
    auto again = true;

    while(again) {
        again = false;

        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(base, JsBlockId(i));
            if(block->deleted) continue;

            Size at = 0;
            while(at < block->instructionList.size()) {
                auto inst = base[block->instructionList.get(base, at)];
                auto effects = effectsOf(*inst);

                auto removable = (effects & (kJsPure | kJsAllocates)) != 0 &&
                                 (effects & (kJsReads | kJsWrites | kJsThrows | kJsControl |
                                             kJsCalls | kJsTerminator)) == 0;

                auto used = inst->result && base[inst->result]->id.index < liveUses.size()
                    ? liveUses[base[inst->result]->id.index] : 1;

                if(!removable || !inst->result || used) { at++; continue; }

                release(*inst);
                block->instructionList.remove(base, at);

                target.optimizer.removed++;
                again = true;
            }
        }
    }
}

} // namespace jsir
