#include "opt.h"
#include "cfg.h"

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

namespace {

// `JsUseRole` and `JsUse` are `module.h`'s, because the scratch that holds a run of them is.
using Use = JsUse;

// Whether a call's argument at `ordinal` is a position the callee may only read - one bit of
// `JsAbi::sharedMask`, which is that walk done once where the plan was frozen rather than once per
// argument of every call this pass looks at.
bool argumentIsShared(JsTarget& target, JsAbiId abiId, U32 ordinal) {
    if(!abiId.valid() || abiId.index >= target.abis.size()) return false;
    return target.abi(abiId).sharedAt(ordinal);
}

struct Optimizer {
    Optimizer(JsTarget& target, JsBody& body):
        target(target), body(body), base(*target.arena), builder(target, body),
        s(target.optScratch) {}

    JsTarget& target;
    JsBody& body;
    JsirBase base;
    JsBuilder builder;

    /*
     * Every list this pass keeps, on the target rather than here - `JsOptScratch`.
     *
     * The pass is constructed per body, so an `Array` declared as a member of it is an allocation
     * per body per list. The names below are references so that the code reads as it did.
     */
    JsOptScratch& s;

    // One entry per value, into `useList` - the CSR index §7.4 asks for instead of a list per value.
    Array<U32>& useStart = s.useStart;
    Array<Use>& useList = s.useList;

    /*
     * How many live `Load`s and `Stores` name each place.
     *
     * The bookkeeping half of this pass, and the one thing about `JsValue::uses` that is not one use
     * per instruction: `convert.cpp` caches a place per SSA value, so several instructions may read
     * one `JsPlace` while `addProperty` counted its owner **once**. Decrementing per instruction
     * would take an object's count to zero while a live load still named it, and the dead-code walk
     * would then remove the object literal a later statement reads - which is exactly the "met a
     * value with no expression" this pass produced before the count was kept this way.
     */
    Array<U32>& placeRefs = s.placeRefs;

    /*
     * How many uses of each value the *index* sees - one per live instruction that names it,
     * including as a place's owner.
     *
     * Deliberately not `JsValue::uses`, which this pass also maintains and which answers a different
     * question. That one is the construction count `codegen/js/opt.cpp` reads to decide whether an
     * expression may be written at its single use, and it is an over-count by design: a place built
     * by `placeOf` and never read by any instruction still counted its owner, and a `Value` place's
     * load counted the object twice. An over-count is harmless there and fatal here - an object
     * literal whose three property reads have all been forwarded would never reach zero, so the
     * literal would survive with nothing reading it. `Failure {site, claim, detail}` is where that
     * showed.
     */
    Array<U32>& liveUses = s.liveUses;

    // Which body parameters are at a `shared` position, by value id. A parameter this is true of is
    // storage the caller's loan says nothing writes for the length of the call.
    IndexSet& sharedParameter = s.sharedParameter;

    bool changed = false;

    // ------------------------------------------------------------------ the use index

    /*
     * Every use one instruction makes, with what that position does to the value.
     *
     * Places are walked here rather than through `forEachUse`, because a place's owner is a use the
     * instruction's own operand list does not carry - `JsInstLoad` holds a `JsPlaceId` and nothing
     * else - and because what the owner of a place *is* depends on whether the instruction reads it
     * or writes it.
     */
    template<class F> void eachUse(JsInst& inst, F&& f) {
        switch(inst.kind) {
            case JsInst::Const:
            case JsInst::FunCode:
            case JsInst::GenTable:
            case JsInst::Unreachable:
                return;

            case JsInst::Load: {
                auto& place = builder.place(((JsInstLoad&)inst).place);
                if(place.owner) f(base[place.owner], JsUseRole::Read);
                if(place.index) f(base[place.index], JsUseRole::Read);
                return;
            }

            case JsInst::Store: {
                auto& written = (JsInstStore&)inst;
                auto& place = builder.place(written.place);
                if(place.owner) f(base[place.owner], JsUseRole::Written);
                if(place.index) f(base[place.index], JsUseRole::Read);
                if(written.value) f(base[written.value], JsUseRole::Kept);
                return;
            }

            // The pair of nodes whose result *is* their operand's object.
            case JsInst::Alias:
                f(base[((JsInstAlias&)inst).from], JsUseRole::Aliased);
                return;

            // A duplicate reads its source and hands back something else, so the source is only
            // read - which is what makes a clone of a clone collapse.
            case JsInst::Clone:
                f(base[((JsInstClone&)inst).from], JsUseRole::Read);
                return;

            case JsInst::Split:
                f(base[((JsInstSplit&)inst).from], JsUseRole::Read);
                return;

            case JsInst::Object:
            case JsInst::Array:
            case JsInst::Materialize: {
                forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
                    if(used) f(base[used], JsUseRole::Kept);
                });
                return;
            }

            case JsInst::Call: {
                auto& written = (JsInstCall&)inst;
                if(written.genEnv) f(base[written.genEnv], JsUseRole::Kept);

                auto arguments = written.arguments.contents(base);
                for(U32 i = 0; i < arguments.size(); i++) {
                    if(!arguments[i]) continue;
                    f(base[arguments[i]], argumentIsShared(target, written.abi, i)
                        ? JsUseRole::Read : JsUseRole::Written);
                }
                return;
            }

            case JsInst::CallValue: {
                auto& written = (JsInstCallValue&)inst;
                if(written.code) f(base[written.code], JsUseRole::Kept);
                if(written.environment) f(base[written.environment], JsUseRole::Kept);

                auto arguments = written.arguments.contents(base);
                for(U32 i = 0; i < arguments.size(); i++) {
                    if(!arguments[i]) continue;
                    f(base[arguments[i]], argumentIsShared(target, written.abi, i)
                        ? JsUseRole::Read : JsUseRole::Written);
                }
                return;
            }

            /*
             * A host row is one `resolve_mir/host.def` row and this tier does not know what it does
             * with a reference - `hostCall1` may keep it, and a helper's roster includes one that
             * runs an authored teardown. Both refuse rather than being grouped with the calls, whose
             * positions have a declared convention to read.
             */
            case JsInst::Host:
            case JsInst::Helper:
            case JsInst::TypeDrop: {
                forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
                    if(used) f(base[used], JsUseRole::Opaque);
                });
                return;
            }

            // Arithmetic over a value reads it; a `select` or a `map` hands one of its operands
            // back, which is the same thing an edge does with it.
            case JsInst::Binary:
            case JsInst::Unary:
            case JsInst::Coerce:
            case JsInst::Convert:
            case JsInst::Math: {
                forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
                    if(used) f(base[used], JsUseRole::Read);
                });
                return;
            }

            case JsInst::Select:
            case JsInst::Map: {
                forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
                    if(used) f(base[used], JsUseRole::Kept);
                });
                return;
            }

            case JsInst::Throw:
            case JsInst::Return: {
                forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
                    if(used) f(base[used], JsUseRole::Kept);
                });
                return;
            }

            case JsInst::Jump:
            case JsInst::Branch:
            case JsInst::Switch: {
                forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
                    if(used) f(base[used], JsUseRole::Read);
                });

                forEachEdge(base, inst, [&](JsEdge& edge) {
                    for(auto supplied: edge.arguments.contents(base)) {
                        if(supplied) f(base[supplied], JsUseRole::Kept);
                    }
                });
                return;
            }

            case JsInst::KindCount:
                return;
        }
    }

    template<class F> void eachInst(F&& f) {
        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(base, JsBlockId(i));
            if(block->deleted) continue;

            for(auto pointer: block->instructions(base)) f(*base[pointer]);
        }
    }

    void buildUses() {
        auto count = body.values.size();

        useStart.clear();
        useStart.reserve(count + 1);
        for(U32 i = 0; i <= count; i++) useStart.push(0);

        eachInst([&](JsInst& inst) {
            eachUse(inst, [&](JsValue* value, JsUseRole) { useStart[value->id.index + 1]++; });
        });

        for(U32 i = 0; i < count; i++) useStart[i + 1] += useStart[i];

        useList.clear();
        useList.reserve(useStart[count]);
        for(U32 i = 0; i < useStart[count]; i++) useList.push(Use {});

        auto& fill = s.fill;
        fill.clear();
        fill.reserve(count);
        for(U32 i = 0; i < count; i++) fill.push(useStart[i]);

        eachInst([&](JsInst& inst) {
            eachUse(inst, [&](JsValue* value, JsUseRole role) {
                useList[fill[value->id.index]++] = Use { &inst, role };
            });
        });

        liveUses.clear();
        liveUses.reserve(count);
        for(U32 i = 0; i < count; i++) liveUses.push(useStart[i + 1] - useStart[i]);
    }

    /*
     * The use counts one instruction stops making - `JsBuilder::countUses` run backwards, which is
     * what makes the number `codegen/js/opt.cpp` reads for its one-use inlining still true.
     *
     * A place's owner goes with the **last** instruction naming that place rather than with each of
     * them; see `placeRefs`.
     */
    void release(JsInst& inst) {
        eachUse(inst, [&](JsValue* used, JsUseRole) {
            if(used->id.index < liveUses.size() && liveUses[used->id.index]) {
                liveUses[used->id.index]--;
            }
        });

        forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
            if(used && base[used]->uses) base[used]->uses--;
        });

        auto placeId = JsPlaceId();
        if(inst.kind == JsInst::Load) placeId = ((JsInstLoad&)inst).place;
        else if(inst.kind == JsInst::Store) placeId = ((JsInstStore&)inst).place;
        else return;

        if(!placeId.valid() || placeId.index >= placeRefs.size()) return;
        if(!placeRefs[placeId.index] || --placeRefs[placeId.index]) return;

        auto& place = builder.place(placeId);
        if(place.owner && base[place.owner]->uses) base[place.owner]->uses--;
        if(place.index && base[place.index]->uses) base[place.index]->uses--;
    }

    void countPlaceRefs() {
        placeRefs.clear();
        placeRefs.reserve(body.places.size());
        for(U32 i = 0; i < body.places.size(); i++) placeRefs.push(0);

        eachInst([&](JsInst& inst) {
            auto placeId = JsPlaceId();
            if(inst.kind == JsInst::Load) placeId = ((JsInstLoad&)inst).place;
            else if(inst.kind == JsInst::Store) placeId = ((JsInstStore&)inst).place;
            else return;

            if(placeId.valid() && placeId.index < placeRefs.size()) placeRefs[placeId.index]++;
        });
    }

    Buffer<Use> usesOf(JsValue* value) {
        auto first = useStart[value->id.index];
        return Buffer<Use> { useList.pointer() + first, useStart[value->id.index + 1] - first };
    }

    // ------------------------------------------------------------------ the read-only walk

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
    U32 budget = 0;

    bool readOnly(JsValue* value, U32 allowStoreInto, U32 depth = 0) {
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
    bool variableIsReadOnly(U32 slot, U32 depth) {
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

    static constexpr U32 kNoVariable = maxLimit<U32>;

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
    bool isFreshHere(JsValue* value) {
        if(!value->definer) return false;
        return (effectsOf(*base[value->definer]) & kJsAllocates) != 0;
    }

    // ------------------------------------------------------------------ the source's storage

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
    /*
     * Through every `Alias` to the value that named the object first.
     *
     * An alias is a second name and not a second object, so the question "what storage is this" is
     * the root's. Without this a clone of a value this pass has *already* aliased answers "neither a
     * `Var` nor a parameter", which is how the second duplicate of a `Site` survived the round that
     * removed the first.
     */
    JsValue* rootOf(JsValue* value) {
        for(U32 depth = 0; depth < 8 && value->definer; depth++) {
            auto definer = base[value->definer];
            if(definer->kind != JsInst::Alias) break;

            auto from = ((JsInstAlias&)*definer).from;
            if(!from) break;

            value = base[from];
        }

        return value;
    }

    bool sourceIsStable(JsValue* value) {
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

    // ------------------------------------------------------------------ dominance, for the sink

    /*
     * Reverse postorder, immediate dominators and which natural loops hold each block - all of it
     * `jsir::JsCfg`'s since 2026-09-22 and none of it computed here.
     *
     * This file had its own copy, the verifier had another and `codegen/js/structure.cpp` had a
     * third in a different algorithm. They are one graph: nothing in this pass touches an edge, so
     * the CFG a body is converted with is the CFG it is structured with. `jsCfgOf` computes it on
     * the first ask and `JsTarget::beginBody` throws it away.
     *
     * `JsCfg::reducible` is what `sinkable` was: an **irreducible** graph is the one shape the loop
     * masks cannot describe, since a retreating edge whose target does not dominate its source
     * names a cycle with two entries and a block's "which loops am I in" is then not a set of
     * natural loops. The structurer makes the same distinction for its own reasons and finds no
     * such body in `test/base`; here it is a refusal rather than a fallback.
     */
    static constexpr U32 kNoBlock = JsCfg::kNoBlock;

    const JsCfg* flow = nullptr;

    bool computeDominance() {
        if(!flow) flow = &jsCfgOf(target, body);
        return flow->reducible;
    }

    // The lowest block dominating both - the dominator tree's own meet, as the two chains walked
    // towards each other by reverse postorder. `JsCfg::rpo` is a total order, so this terminates.
    U32 commonDominator(U32 a, U32 b) {
        while(a != b) {
            while(flow->rpo[a] > flow->rpo[b]) a = flow->idom[a];
            while(flow->rpo[b] > flow->rpo[a]) b = flow->idom[b];
        }

        return a;
    }

    // ------------------------------------------------------------------ the rewrites

    /*
     * One instruction becomes an `Alias` of a value already in scope, keeping its result. The only
     * shape any rewrite here takes, which is what keeps every user of the result untouched.
     *
     * Collected first and applied second - see `pending`. Applying one rewrite while the use index
     * is being read would leave the index describing a body that no longer exists, and the entries
     * it would be missing are exactly the `Aliased` ones this creates.
     */
    // `JsOptRewrite` is `module.h`'s, beside the scratch that holds the list of them.
    using Rewrite = JsOptRewrite;

    Array<Rewrite>& pending = s.pending;

    void applyRewrites() {
        /*
         * Where each instruction sits in its own block, in one pass over the body.
         *
         * A scan of the block per rewrite is what this was, and the two numbers multiply: a body
         * the elision fires on a hundred times has a hundred scans of the block each rewrite is in.
         * The index is rebuilt here rather than maintained, because the only thing that moves an
         * instruction is this loop and it replaces in place.
         */
        auto& instAt = s.instAt;
        instAt.clear();
        instAt.reserve(U32(body.insts.size()));
        for(U32 i = 0; i < body.insts.size(); i++) instAt.push(maxLimit<U32>);

        for(U32 b = 0; b < body.blocks.size(); b++) {
            auto block = body.block(base, JsBlockId(b));
            for(Size i = 0; i < block->instructionList.size(); i++) {
                instAt[base[block->instructionList.get(base, i)]->id.index] = U32(i);
            }
        }

        for(auto& rewrite: pending) {
            auto& inst = *rewrite.inst;
            auto block = base[inst.block];

            auto at = Size(inst.id.index < instAt.size() ? instAt[inst.id.index] : maxLimit<U32>);
            if(at >= block->instructionList.size()) continue;
            if(base[block->instructionList.get(base, at)] != &inst) continue;

            release(inst);

            auto replacement = new (target.arena) JsInstAlias(inst.source, rewrite.from - base);
            replacement->id = inst.id;
            replacement->block = inst.block;
            replacement->result = inst.result;
            rewrite.from->uses++;
            if(rewrite.from->id.index < liveUses.size()) liveUses[rewrite.from->id.index]++;

            if(inst.result) base[inst.result]->definer = (JsInst*)replacement - base;

            block->instructionList.set(base, at, (JsInst*)replacement - base);
            changed = true;
        }

        pending.clear();
    }

    /*
     * §6.3, at every clone whose four conditions hold.
     *
     * The clone is not deleted - it becomes an `Alias`, which emits nothing and says in the dump
     * that a sharing decision was taken. That is `inst.def`'s reason for the node existing at all.
     */
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
    void adoptInitialStores() {
        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(base, JsBlockId(i));
            if(block->deleted) continue;

            Size at = 0;
            while(at < block->instructionList.size()) {
                if(!adoptAt(*block, at)) at++;
            }
        }
    }

    bool adoptAt(JsBlock& block, Size at) {
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
        changed = true;
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
    void sinkAllocations() {
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
            changed = true;
        }
    }

    void aliasCopies(bool count) {
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

    /*
     * §6.6's load forwarding, at the one shape the measurement named: a property read of an object
     * literal this body built, never wrote and never handed anywhere.
     *
     * The object having no `Written`, `Kept` or `Opaque` use is what makes its properties constant
     * from its definition onward, and SSA dominance is what makes the operand available at the read:
     * the load uses the object, so the object's definition dominates it.
     */
    void forwardFields() {
        eachInst([&](JsInst& inst) {
            if(inst.kind != JsInst::Load || !inst.result) return;

            auto& place = builder.place(((JsInstLoad&)inst).place);
            if(place.kind != JsPlaceKind::Property || !place.owner) return;

            auto owner = base[place.owner];
            if(!owner->definer) return;

            auto definer = base[owner->definer];
            if(definer->kind != JsInst::Object) return;

            auto& built = (JsInstObject&)*definer;
            if(built.shape != place.shape) return;

            auto values = built.values.contents(base);
            if(place.slot >= values.size() || !values[place.slot]) return;

            for(auto& use: usesOf(owner)) {
                if(use.role != JsUseRole::Read) return;
            }

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
    void dropDead() {
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
                    changed = true;
                }
            }
        }
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
    void dropDeadLoads() {
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
                changed = true;
            }
        }
    }

    bool run() {
        sharedParameter.reset(body.values.size());

        if(body.abi.valid() && body.abi.index < target.abis.size()) {
            auto& abi = target.abi(body.abi);
            auto count = body.parameters.size();

            auto lead = abi.takesGenEnv ? 1u : 0u;
            forEachPosition(abi, [&](U32 position, U32, const JsPassing& parameter, U16) {
                if(!parameter.shared || lead + position >= count) return;

                auto value = base[body.parameters.get(base, lead + position)];
                sharedParameter.set(value->id.index, true);
            });
        }

        /*
         * Forwarding first, then aliasing, then the dead form both leave - and then once more,
         * because a forwarded field turns a clone whose result went into an object literal into a
         * clone of a clone, which is a shape the second pass can alias.
         *
         * Two rounds rather than a worklist to a fixed point: the second round is what the
         * measurement asked for and a third changed nothing over `test/base`, so a fixpoint here
         * would be a loop that always runs twice.
         */
        countPlaceRefs();
        buildUses();

        /*
         * Sinking first, because the two rewrites below it are asked in one block: `adoptInitialStores`
         * wants the zero and the store together, and the zero is in the entry block until this has
         * run. `md5Answers` was already in one block and `normalizeForm`'s `runs` slot was not.
         */
        sinkAllocations();

        countPlaceRefs();
        buildUses();
        adoptInitialStores();
        countPlaceRefs();

        for(U32 round = 0; round < 2; round++) {
            buildUses();
            forwardFields();

            buildUses();
            aliasCopies(round == 0);

            dropDead();
            dropDeadLoads();
            dropDead();
        }

        // And once more, because a forwarded field and a removed literal take uses away: an
        // allocation whose remaining users all sit in one arm is one this could not see before.
        buildUses();
        sinkAllocations();

        return true;
    }
};

} // namespace

bool optimizeBody(JsTarget& target, JsBody& body) {
    Optimizer optimizer(target, body);
    return optimizer.run();
}

} // namespace jsir
