#pragma once

#include "convert.h"
#include "abi.h"
#include "../dominance.h"
#include "name.h"
#include "../../resolve_mir/specialize.h"

/*
 * The SSA-to-JSIR conversion's own state - `Conversion`, and the three SSA predicates in front of
 * it. Analysis-IR-JS.md §8.1's file list, which this header is what made possible.
 *
 * `convert.cpp` was 5,414 lines and every question the conversion answers was in it: the demand
 * pass, places, constants, teardowns, the host rows, the vector rows and the instruction switch.
 * §8.1 asks for those as files, and a file is a translation unit - so the struct they are methods
 * of has to be visible to more than one, which is what this is and is the whole of what it is.
 * Nothing outside `resolve_ssa/js` includes it, and nothing here includes it that is not one of
 * those files.
 *
 * The struct stays one object rather than becoming several passes, deliberately: what the files
 * split is the *text*, and the state - the builder, the per-function scratch, the SSA body being
 * read - is one thing the whole conversion shares and would only be threaded back through
 * parameters. `Conversion::at(id)` is the accessor they all reach for.
 */

namespace jsir {


/*
 * Whether a loan of this occurrence denotes storage rather than a value - §1.3.1's predicate.
 *
 * **`ssa::loanDenotesStorage`, and no second copy of it.** J65 wrote the rule out here rather than
 * reaching for `lower/manifest.h`, which is the include §12.3 calls a leak - and J75 took that
 * include out of the shared tier, so the rule now has a target-free home above this one and this is
 * the shim that names it. The window row, which is J71's, went up with it.
 */
inline bool loanDenotesStorageAt(JsTarget& target, const mir::Occurrence& occurrence) {
    return ssa::loanDenotesStorage(target.context, target.program, occurrence);
}

/*
 * Whether storage is handed to anybody else, following the reborrows.
 *
 * A worklist rather than a walk over the root's own uses, and the reborrow is why: SSA routinely
 * writes `%a = alloc; %b = load %a` where `%b` is a loan *of the same storage*, and a pass that
 * looked only at `%a` saw a load and concluded nothing had left. The set is every SSA value that
 * denotes this storage, and the question is asked of all of them.
 *
 * A projection is deliberately not followed: `field.addr` hands out a *different* place, whose own
 * escape is its own question, and what that one needs is the object rather than a box around the
 * root. `projected` is whether one was seen at all, which is a different question with a different
 * consumer - see `Conversion::projects`.
 */
/*
 * Whether this use hands the address to an **erased** `Cell` position - J70.
 *
 * A body compiled once reads a `Cell` argument as `x.$o[x.$k]` whatever is in it, so the one
 * reference form it accepts is a real descriptor. A `GcObject` local is otherwise its own reference
 * (a `Value` place), and the two cannot both be true - so the demand pass answers this second
 * question and the local gets a box, which is the one storage a descriptor can name.
 */
inline bool useWantsDescriptor(JsTarget& target, ssa::Inst& user, SsaPtr<ssa::Value> address) {
    if(user.kind != ssa::Inst::Call) return false;

    auto ssa = target.ssa;
    auto& call = (ssa::InstCall&)user;

    auto abiId = call.through == mir::CallThrough::Value
        ? jsAbiOfType(target, ssa[call.target.value()]->occurrence.type, user.source)
        : (call.callee ? jsAbiOf(target, call.callee, user.source) : JsAbiId());

    if(!abiId.valid()) return false;

    auto& abi = target.abi(abiId);
    auto index = U32(0);

    for(auto used: call.arguments.contents(ssa)) {
        if(index >= abi.parameters.size()) return false;

        auto& passing = abi.parameters[index++];
        if(used.value() != address) continue;
        if(passing.kind == JsPassingKind::Cell && passing.erased) return true;
    }

    return false;
}

/*
 * Whether storage is handed to anybody else, following the reborrows.
 *
 * A worklist rather than a walk over the root's own uses, and the reborrow is why: SSA routinely
 * writes `%a = alloc; %b = load %a` where `%b` is a loan *of the same storage*, and a pass that
 * looked only at `%a` saw a load and concluded nothing had left. The set is every SSA value that
 * denotes this storage, and the question is asked of all of them.
 *
 * The per-use decision is `ssa::useKeepsAddressLocal`, which is above this tier - see there. What
 * stays here is the walk, because both of its side-channels are this target's: `projected` feeds
 * `Conversion::projects` and `descriptor` feeds `useWantsDescriptor`, which reads a `JsAbi`.
 *
 * A projection is deliberately not followed: `field.addr` hands out a *different* place, whose own
 * escape is its own question, and what that one needs is the object rather than a box around the
 * root.
 */
inline bool walkAddress(JsTarget& target, ssa::Body& from, SsaPtr<ssa::Value> root,
                        Array<SsaPtr<ssa::Value>>& pending, IndexSet& seen, bool* projected,
                        bool* descriptor = nullptr) {
    auto ssa = target.ssa;

    pending.clear();
    seen.reset(from.values.size());
    pending.push(root);
    seen.set(ssa[root]->id.index, true);

    auto escaped = false;

    // The escape alone would stop the walk, and where a caller also asks about the descriptor it
    // must not: the two questions are about different uses of the same address and the first one
    // answered is not always the first one met.
    while(pending.size() && (!escaped || descriptor)) {
        auto address = pending[pending.size() - 1];
        pending.pop();

        for(auto userPointer: ssa[address]->uses(ssa)) {
            auto& user = *ssa[userPointer];

            if(projected && (user.kind == ssa::Inst::FieldAddress ||
                             user.kind == ssa::Inst::ElementAddress)) {
                *projected = true;
            }

            if(descriptor && useWantsDescriptor(target, user, address)) *descriptor = true;

            if(user.kind == ssa::Inst::Load && user.results.size()) {
                auto read = ssa[user.results.get(ssa, 0)];
                if(((ssa::InstLoad&)user).address.value() == address &&
                   loanDenotesStorageAt(target, read->occurrence)) {
                    if(!seen[read->id.index]) {
                        seen.set(read->id.index, true);
                        pending.push(user.results.get(ssa, 0));
                    }

                    continue;
                }
            }

            /*
             * `addressOf(x)` - a **reborrow** here too, and the reason it has to be one is the whole
             * of what `swap` and `exchange` need from this pass.
             *
             * The conversion answers the operand's own place for such a bitcast, so nothing about
             * the storage has left the frame; counting it as an escape made the local a `Value`
             * place - the host object *is* the storage - and a `Value` place cannot be **rebound**.
             * `exchange(a, fresh)` then wrote the fresh array's properties into the object the
             * caller had just parked, so the old value and the new one were one object and
             * `Exchange.test` read the new length back out of both.
             */
            if(user.kind == ssa::Inst::Unary && user.results.size() &&
               ((ssa::InstUnary&)user).op == mir::UnaryOp::Bitcast &&
               ((ssa::InstUnary&)user).value.value() == address) {
                auto read = ssa[user.results.get(ssa, 0)];
                if(!seen[read->id.index]) {
                    seen.set(read->id.index, true);
                    pending.push(user.results.get(ssa, 0));
                }

                continue;
            }

            if(!ssa::useKeepsAddressLocal(ssa, user, address)) {
                escaped = true;
                if(!descriptor) break;
            }
        }
    }

    return escaped;
}

struct Conversion {
    Conversion(JsTarget& target, JsBody& body, ssa::Body* from):
        target(target), body(body), from(from), decl(target.decl), ssa(target.ssa),
        builder(target, body) {}

    JsTarget& target;
    JsBody& body;

    // Null for the one body this file builds that has no SSA behind it - a lambda's environment
    // teardown, which is `emitTeardown` alone and reaches nothing that reads this.
    ssa::Body* from;
    DeclBase decl;
    SsaBase ssa;
    JsBuilder builder;

    /*
     * SSA value id -> what it became, and SSA block index -> the block it became.
     *
     * One dense row per value on the target's own scratch - `JsTargetValue`, where the four hash
     * maps this replaces are argued, including the invariant they claimed and did not have.
     *
     * A logical SSA value whose manifestation is `Parts` is **N JSIR values** and not one value of a
     * multi-part representation: that is the whole arrangement, and it is why a vector operation
     * builds nothing and a lane read emits no instruction. The parts of every such value live back
     * to back in `partValues` and a `JsPartsRef` is a range of it, rather than each value owning a
     * list. Indices rather than pointers for the ordinary reason: `partValues` grows while a body is
     * converted, so anything holding into it would dangle at the next push.
     */
    using PartsRef = JsPartsRef;

    Array<JsirPtr<JsValue>> partValues;

    JsTargetValue& at(U32 id) { return target.scratch.at(id); }
    JsTargetValue& at(ssa::Value& value) { return at(value.id.index); }

    /*
     * The four ways a row is filled, and only `form` tells a **definition** from a memo.
     *
     * A definer writes the form it produced; a lazy conversion writes the handle it found and
     * leaves the form alone. So `at(v).one` being set says "one host value of this is in hand" and
     * not "this value is one host value", which is the distinction the four maps could not make.
     */
    void setOne(U32 id, JsirPtr<JsValue> value) {
        auto& row = at(id);
        row.one = value;
        if(row.form == JsTargetValue::Form::None) row.form = JsTargetValue::Form::One;
    }

    void setPlace(U32 id, JsPlaceId place) {
        auto& row = at(id);
        row.place = place;
        if(row.form == JsTargetValue::Form::None) row.form = JsTargetValue::Form::Place;
    }

    void setParts(U32 id, const PartsRef& ref) {
        auto& row = at(id);
        row.parts = ref;
        if(row.form == JsTargetValue::Form::None) row.form = JsTargetValue::Form::Parts;
    }

    void setVoid(U32 id) { at(id).form = JsTargetValue::Form::Void; }
    void setNicheTag(U32 id, JsPlaceId place) { at(id).nicheTag = place; }

    JsBlock* blockFor(U32 index) {
        auto pointer = target.scratch.blocks[index];
        return pointer ? base()[pointer] : nullptr;
    }

    /*
     * The materialized form of a `freshEachUse` value, **per block** - see `JsRep::freshEachUse`.
     *
     * A scalarized record's object may not be shared between blocks, because the block that asked
     * first does not dominate the one that asks next. Sharing it *within* one is both sound - a
     * block is straight-line and the materialization goes in at the first ask - and worth doing:
     * without it a record read twice in one statement is two allocations where the tagged form was
     * one, which is what the first measurement of this rule showed as a 4% regression.
     */
    // A hash map and not a dense row, because the key is a *pair* and the live set is a handful of
    // values in the block being filled rather than a fact about every value in the body. Reset per
    // body on the target's scratch rather than rebuilt, which is what `HashMap::reset` is for.
    HashMap<U64, JsirPtr<JsValue>> blockMaterialized;

    // The key above: the value and the block it is being asked for in.
    U64 materializedKey(U32 value) {
        return (U64(value) << 32) | U64(builder.current ? builder.current->index.index : 0);
    }

    /*
     * The generic environment this body was handed, and the numbering it indexes.
     *
     * Null in every specialized body, which is what makes reading one a report rather than an
     * `undefined`: a slot read that got here without a table is a body whose form the ABI and the
     * conversion disagree about, and that is the one disagreement §3.7 exists to make impossible.
     */
    JsValue* genEnv = nullptr;
    DeclPtr<mir::GenSchema> genSchema = nullptr;

    JsValue* partAt(const PartsRef& ref, U32 index) {
        return base()[partValues[ref.first + index]];
    }

    // One `PartsRef` out of the values in hand, copied into the run. The caller keeps nothing.
    PartsRef makeParts(JsRepId rep, Buffer<JsValue*> values) {
        PartsRef ref;
        ref.rep = rep;
        ref.first = partValues.size();
        ref.count = U16(values.length);

        for(auto value: values) partValues.push(value - base());
        return ref;
    }

    /*
     * The demand pass's one answer per `Alloc`: does this local need a cell?
     *
     * An allocation whose address never leaves the body is a `var`. One whose address is handed to a
     * call, put on an edge or returned needs something a second holder can write through - and that
     * is the object itself where the content is a host object, and a `{$v}` box otherwise. §5.4's
     * first rule, and the direction the shipping backend gets wrong: it boxes on the first borrow,
     * whether or not the borrow escapes.
     */
    IndexSet escapes;

    /*
     * The demand pass's second answer per `Alloc`: is this local's storage ever reached field-wise?
     *
     * A `var` holding a host object is `undefined` until something writes it, and a whole-value store
     * is what makes it an object - so a local whose *only* writes are `s.$index = 0` beside its
     * siblings never becomes one. That is exactly how an inlined `f$new` fills an iterator's state
     * record (`test/resolve/iter/Inline.yana`), and it read as "cannot set properties of undefined"
     * rather than as a refusal. The answer is the escaping arm's, taken for one more reason: the
     * object is built at its zero here, and the place stays a `Var` so that a whole-value store is
     * still a rebinding rather than a property copy.
     */
    IndexSet projects;

    /*
     * And whether the address is handed to an **erased** `Cell` position, which is the one reason a
     * host object needs a box of its own - see `useWantsDescriptor`.
     */
    IndexSet descriptors;

    bool failed = false;

    JsirBase base() { return builder.base; }

    void report(StringView what, LocationId where) {
        target.notYet(what, where);
        failed = true;
    }

    void report(StringView what, DeclPtr<mir::Type> type, LocationId where) {
        target.notYet(what, type, where);
        failed = true;
    }

    /*
     * An invariant this file thought it had, reported once.
     *
     * Suppressed after the first report of any kind, and that is what makes a sweep readable: a
     * construct this tier cannot convert leaves every value downstream of it unmapped, so a body
     * with one unconverted expression in it produces a dozen of these and buries the sentence that
     * says what is actually missing.
     */
    void internal(StringView what, LocationId where) {
        if(!failed) target.internalError(what, where);
        failed = true;
    }

    /*
     * What **one host value** of an SSA value is.
     *
     * `jsStoredRep` of the manifestation, which is the invariant every position that holds one value
     * depends on - a place's content, a shape property, an element, a global, a returned value. The
     * multi-part form is `partsRepOf`'s answer and is asked for by name, so a caller that wants the
     * parts says so and every other caller cannot get them by accident. Getting this the other way
     * round put a `Parts` representation on an `alias` and the verifier caught it at a `return`.
     */
    JsRepId repOf(ssa::Value& value) { return jsStoredRep(target, manifestOf(value)); }

    // The manifestation itself, memoized by SSA value id - see `JsTargetValue::manifest`, where the
    // reason a memo is worth having for what reads like a table lookup is written down.
    JsRepId manifestOf(ssa::Value& value) {
        auto& row = at(value);
        if(!row.manifest.valid()) {
            row.manifest = jsManifest(target, value.occurrence, kNullLocation).rep;
        }

        return row.manifest;
    }

    bool isVoidRep(JsRepId rep) {
        return target.rep(rep).kind == JsRepKind::Void;
    }

    // ---------------------------------------------------------------- demand

    // The two demand answers, over every allocation in this body - `walkAddress` above is the walk
    // and `escapes`/`projects` are what the two bits it answers are for.
    void planDemand() {
        escapes.reset(from->values.size());
        projects.reset(from->values.size());
        descriptors.reset(from->values.size());

        Array<SsaPtr<ssa::Value>> pending;
        IndexSet seen;
        seen.reset(from->values.size());

        auto ask = [&](SsaPtr<ssa::Value> rootPointer) {
            auto root = ssa[rootPointer];

            pending.clear();
            seen.reset(from->values.size());
            pending.push(rootPointer);
            seen.set(root->id.index, true);

            auto projected = false;
            auto wantsDescriptor = false;
            auto escaped = walkAddress(target, *from, rootPointer, pending, seen, &projected,
                                       &wantsDescriptor);

            if(escaped) escapes.set(root->id.index, true);
            if(projected) projects.set(root->id.index, true);
            if(wantsDescriptor) descriptors.set(root->id.index, true);
        };

        for(U32 i = 0; i < from->blocks.size(); i++) {
            auto block = from->block(ssa, BlockId(i));
            if(block->deleted) continue;

            for(auto instPointer: block->instructions(ssa)) {
                auto inst = ssa[instPointer];
                if(inst->kind != ssa::Inst::Alloc || !inst->results.size()) continue;

                ask(inst->results.get(ssa, 0));
            }
        }

        /*
         * And a **parameter**, which is storage on a machine and is not one here - J70.
         *
         * A parameter arrives in memory natively, so `%r = load %param` at a loan that denotes
         * storage is the address it already had and nothing has to exist. Here a parameter the ABI
         * passes as one value is a host value with nothing behind it, so the same reborrow has
         * nowhere to point - which is how a continuation capturing a shared `text: String` supplied
         * the string where its environment's field wanted the `{$o, $k}` descriptor.
         *
         * The answer is the one an escaping local already gets: a `{$v}` box at the entry, with
         * every read and write in the body going through it. It is sound for exactly the parameters
         * it can reach - a position the caller writes through arrives as a `Cell` and has its place
         * already, so what is left is one the callee's own ABI says nobody outside writes.
         *
         * Asked of the **reborrow** and not of the parameter, which is the difference from a local
         * and is not a detail: `walkAddress` answers "did this storage leave", and every ordinary
         * use of a *value* - an addition, an argument - is a use that does not keep an address. A
         * parameter run through it directly is therefore always an escape, and every scalar
         * parameter in the corpus would get a box it never reads.
         */
        if(from->entry) {
            auto entry = ssa[from->entry];

            for(U32 i = 0; i < entry->argumentCount(); i++) {
                auto parameter = entry->argumentAt(ssa, i);

                for(auto userPointer: ssa[parameter]->uses(ssa)) {
                    auto& user = *ssa[userPointer];
                    if(user.kind != ssa::Inst::Load || !user.results.size()) continue;
                    if(((ssa::InstLoad&)user).address.value() != parameter) continue;

                    auto read = user.results.get(ssa, 0);
                    if(!loanDenotesStorageAt(target, ssa[read]->occurrence)) continue;

                    if(!jsAddressEscapes(target, *from, read, pending, seen)) continue;

                    escapes.set(ssa[parameter]->id.index, true);
                    break;
                }
            }
        }
    }

    // ---------------------------------------------------------------- values

    /*
     * The multi-part form of an SSA value, or an invalid `PartsRef` - §8.2's `requireParts`.
     *
     * Three things can be in hand and each is a legal source: the parts themselves, one materialized
     * value (a load out of storage, a call result, a `@host` row), or nothing multi-part at all.
     * The middle one is where the `split` goes in, which is what makes the boundary explicit rather
     * than a place where somebody indexes an array and hopes.
     */
    PartsRef requireParts(SsaPtr<ssa::Value> pointer, JsRepId wanted, LocationId where) {
        auto value = ssa[pointer];

        auto& already = at(*value).parts;
        if(already.valid() && (!wanted.valid() || already.rep == wanted)) return already;

        auto& rep = target.rep(wanted);
        if(rep.kind != JsRepKind::Parts) return PartsRef();

        auto one = valueOf(pointer, where);
        if(!one) return PartsRef();

        SmallArray<JsValue*, 16> split;
        for(U16 i = 0; i < rep.partCount; i++) {
            auto part = builder.splitPart(one, wanted, i, where);
            if(!part) {
                internal("could not split a multi-part value"_v, where);
                return PartsRef();
            }

            split.push(part);
        }

        auto ref = makeParts(wanted, held(split));
        setParts(value->id.index, ref);
        return ref;
    }

    // The one host value a `PartsRef` becomes - §8.2's `requireOne`, which "inserts an explicit
    // materialization or reports that no legal materialization exists" and never answers null.
    JsValue* requireOne(const PartsRef& ref, LocationId where) {
        SmallArray<JsValue*, 16> lanes;
        for(U16 i = 0; i < ref.count; i++) lanes.push(partAt(ref, i));

        auto one = builder.materialize(ref.rep, held(lanes), where);
        if(!one) {
            report("a multi-part value at a position that holds one value and has no form for it"_v,
                   where);
            return nullptr;
        }

        return one;
    }

    JsValue* valueOf(SsaPtr<ssa::Value> pointer, LocationId where) {
        auto value = ssa[pointer];

        auto& row = at(*value);
        if(row.one) return base()[row.one];

        /*
         * A value carried as parts, asked for as one - the materialization goes in here, at the
         * position that asked, rather than beside the producer (§3.6's last paragraph).
         *
         * Memoized, because a value materialized twice is two rows of one vector: the second holder
         * is a different object and every identity question downstream has two answers.
         */
        if(row.parts.valid()) {
            auto ref = row.parts;

            /*
             * `freshEachUse` is the scalarized record's exception, and it is a correctness rule
             * rather than an economy: a memo built in the block that asked first is named by every
             * other block that asks, and one of those is not dominated by it - a `return` in two
             * arms is the shape. The memo is kept per *block* instead, which is sound for the reason
             * a block is straight-line. See `JsRep::freshEachUse`.
             */
            if(target.rep(ref.rep).freshEachUse) {
                auto key = materializedKey(value->id.index);
                if(auto seen = blockMaterialized.get(key)) return base()[seen.unwrap()];

                auto one = requireOne(ref, where);
                if(one) blockMaterialized.add(key, one - base());
                return one;
            }

            auto one = requireOne(ref, where);
            if(one) setOne(value->id.index, one - base());
            return one;
        }

        /*
         * A place asked for as a value - the three positions that make a place one value, and the
         * one that does not.
         */
        if(row.place.valid()) {
            auto& place = builder.place(row.place);

            /*
             * Storage that *is* an object reads as that object, and everything else reads through.
             *
             * `Value` only, and the `Cell` that used to be beside it was a wrong answer: a cell's
             * owner is the `{$o, $k}` descriptor, not the thing it names, so `readOnlyView(&c) = c`
             * returned the descriptor where its signature says a value. `referenceTo` is what
             * hands the descriptor out, at the three positions that ask for one.
             */
            // `nichePayload` excepted, because then the object *is* the value at a different
            // representation and the relabel is `builder.load`'s - see there.
            if(place.kind == JsPlaceKind::Value && !place.nichePayload) {
                auto owner = base()[place.owner];
                owner->uses++;
                return owner;
            }

            return builder.load(row.place, where);
        }

        if(row.form == JsTargetValue::Form::Void) return nullptr;

        internal("met an SSA value it has not converted"_v, where);
        return nullptr;
    }

    // An instruction's result, carried as its parts - the multi-part counterpart of `define`, and
    // the only way a `PartsRef` becomes something a later instruction can find.
    void defineParts(ssa::Inst& inst, const PartsRef& ref) {
        if(!inst.results.size()) return;
        if(!ref.valid()) return internal("met a multi-part result with no parts"_v, inst.source);

        setParts(ssa[inst.results.get(ssa, 0)]->id.index, ref);
    }

    void define(ssa::Inst& inst, JsValue* value) {
        if(!inst.results.size()) return;

        auto result = ssa[inst.results.get(ssa, 0)];
        if(!value) {
            setVoid(result->id.index);
            return;
        }

        setOne(result->id.index, value - base());
    }

    void definePlace(ssa::Inst& inst, JsPlaceId place) {
        if(!inst.results.size()) return;

        auto result = ssa[inst.results.get(ssa, 0)];
        setPlace(result->id.index, place);
    }

    /*
     * A place over a value that is already in hand - the lazy half of the `Address` split.
     *
     * Decided from the value's **representation** rather than from the occurrence's `needsCell`,
     * and the two are different questions: that one says how an occurrence is *passed*, and this one
     * says what the thing in hand actually is. A borrow that arrived as a `{$v}` box is a cell
     * whatever its declaration says; one that arrived as the host object is the object. Reading the
     * rep is what makes the two agree by construction, which the first version of this did not - it
     * wrapped an element's value in a cell place and the verifier caught a box that was not one.
     */
    /*
     * Whether a loan of this occurrence denotes storage rather than a value - §1.3.1's predicate,
     * which is `ssa::loanDenotesStorage` and lives one tier up since J75.
     */
    bool loanDenotesStorage(const mir::Occurrence& occurrence) {
        return loanDenotesStorageAt(target, occurrence);
    }

    /*
     * Whether a value **denotes storage** rather than a value - §1.3.1's discriminator, asked of a
     * block argument.
     *
     * A multi-part value that denotes storage stays *one* value across an edge, which is the one
     * place the flattening of §3.8 has to stand down: what the consumer does with it is
     * `placeOf`, and a place is a recipe for reading one host value. `VecFold.yana`'s
     * `for x in items(xs)` is the shape - the loop's element argument is a shared loan of a slot,
     * and taking it apart into lanes left the `load` through it with an address nothing had built.
     *
     * Asked at both ends of every edge with the same predicate, which is what keeps the arity
     * agreeing without a mapping table.
     */
    bool carriesStorage(ssa::Value& value) {
        return value.convention == ssa::Convention::Address || loanDenotesStorage(value.occurrence);
    }

    /*
     * A reference to what a place names - §3.6's `borrow.describe`.
     *
     * `{$o: owner, $k: key}`, which is what a `&Int` and a `&String` are on this target: a place is
     * already an `(object, key)` pair, so reifying it is a genuine reference with no commit point
     * rather than a copy with a write-back. A `Var` place has no owner to name, which is why the
     * demand pass turns an escaping local into a box first; a global has none either and is
     * reported, since a boxed global is a whole-program decision this slice does not take.
     */
    /*
     * Whether a multi-part representation is a **reference passed flat** - `(owner, key)`.
     *
     * Read off the materialized form rather than carried as a second bit, because that form *is*
     * the `{$o, $k}` descriptor and nothing else in this tier materializes into one.
     */
    bool isCellParts(JsRepId rep) {
        auto stored = jsStoredRep(target, rep);
        return rep.valid() && stored != rep && target.rep(stored).kind == JsRepKind::Cell;
    }

    /*
     * A reference to what a place names, as its two parts - `referenceTo` with no object around it.
     *
     * The two are one decision and have to stay one: a caller that pushed `(owner, key)` and a
     * callee that read `x.$o[x.$k]` would agree about nothing. `JsAbi` is what keeps them together,
     * and this is the arm the flat positions take.
     */
    bool referencePartsOf(JsPlaceId id, LocationId where, SmallArray<JsValue*, 8>& into) {
        auto& place = builder.place(id);
        auto erased = target.shape(jsRefShape(target)).properties[0].rep;

        if(place.kind == JsPlaceKind::Property) {
            auto owner = base()[place.owner];
            owner->uses++;

            JsConstant text;
            text.kind = JsConstant::Text;
            text.text = target.shape(place.shape).properties[place.slot].key;

            into.push(owner);
            into.push(builder.constant(text, erased, where));
            return true;
        }

        if(place.kind == JsPlaceKind::Element) {
            auto owner = base()[place.owner];
            auto index = base()[place.index];
            owner->uses++;
            index->uses++;

            into.push(owner);
            into.push(index);
            return true;
        }

        /*
         * Anything else is the descriptor taken apart, which is what a reborrow of a reference and
         * a reference that arrived as one value both are. `referenceTo` answers the object and the
         * split is two property reads - still no allocation, which is the point.
         */
        auto reference = referenceTo(id, where);
        if(!reference) return false;

        auto flat = jsCellPartsRep(target, builder.place(id).content);
        for(U16 i = 0; i < 2; i++) {
            auto part = builder.splitPart(reference, flat, i, where);
            if(!part) {
                internal("could not split a reference descriptor"_v, where);
                return false;
            }

            into.push(part);
        }

        return true;
    }

    JsValue* referenceTo(JsPlaceId id, LocationId where) {
        auto& place = builder.place(id);
        auto cellRep = jsCellRep(target, place.content);

        /*
         * Two places are already their own reference and answer the owner unchanged.
         *
         * A `Cell` place's owner *is* the `{$o, $k}` descriptor naming it. A `Value` place's owner is
         * the host object the storage is, and writing through a reference to one writes its
         * properties rather than rebinding anything - which is the same sentence `jsManifest` says
         * when it answers `needsCell = false` for a unique loan of a `GcObject`, so the two agree by
         * construction and a descriptor around the object would be a second reference form for the
         * one position that does not need one. J65 §1 is where the `Value` place came from and this
         * arm is the half it did not get: `Format.yana`'s `&Formatter` local is a record whose
         * address is handed to a call, and refusing it read as "a mutable borrow of this kind of
         * storage".
         */
        if(place.kind == JsPlaceKind::Cell ||
           (place.kind == JsPlaceKind::Value && jsIsItsOwnStorage(target, place.content))) {
            auto owner = base()[place.owner];
            owner->uses++;
            return owner;
        }

        /*
         * And a `Value` place over something that is **not** its own storage - the box, built here.
         *
         * A `Value` place is a recipe for reading one host value, and `placeOver` builds one over
         * whatever is in hand: a constant, a call result, a block argument, a reborrow of any of
         * those. For a host object that is the whole reference, which is the arm above. For a host
         * string or a number it is not - there is no property a write could land in - and answering
         * the owner handed the *value* out where the position wanted a descriptor. J78's second
         * defect is that read straight off: a continuation capturing a `String` the caller passed
         * supplied the string where its environment's field wanted `{$o, $k}`, and the object
         * verifier named the field. It is the same sentence `planDemand` already says about a
         * parameter ("a host value with nothing behind it, so the same reborrow has nowhere to
         * point") at the one root that pass has no way to reach.
         *
         * **A fresh box per demand and no memo**, which is sound because of what can get here. A
         * local is an `Alloc` and has a real place; a parameter whose reborrow escapes was boxed at
         * the entry, and one the caller writes through arrives as a `Cell`; a field and an element
         * are `Property` and `Element` places. What is left is a value no other name reaches, so
         * two descriptors over it are two temporaries and no write can be observed across them -
         * which is exactly what a native temporary in a frame slot is. A memo would be the stronger
         * claim and would need the box to dominate every asker, and a place is shared between
         * blocks.
         */
        if(place.kind == JsPlaceKind::Value) {
            auto held = base()[place.owner];
            auto boxShape = jsBoxShape(target, place.content);
            auto boxRep = jsBoxRep(target, place.content);

            auto box = builder.add<JsInstObject>(boxRep, where, boxShape);
            box->values.push(target.arena, held - base());
            held->uses++;

            return referenceTo(builder.addProperty(base()[box->result], boxShape, 0), where);
        }

        JsValue* owner = nullptr;
        JsValue* key = nullptr;
        auto erased = target.shape(jsRefShape(target)).properties[0].rep;

        if(place.kind == JsPlaceKind::Property) {
            owner = base()[place.owner];

            JsConstant text;
            text.kind = JsConstant::Text;
            text.text = target.shape(place.shape).properties[place.slot].key;
            key = builder.constant(text, erased, where);
        } else if(place.kind == JsPlaceKind::Element) {
            owner = base()[place.owner];
            key = base()[place.index];
        } else {
            // Named, because the two that are left are two different pieces of work: a `Var` is an
            // escape the demand pass did not see, and a `Global` needs the boxed-global decision.
            target.notYet(jsPlaceKindName(place.kind) == "global"_v
                ? "a mutable borrow of a global, which needs a boxed global"_v
                : "a mutable borrow of a variable whose address the demand pass did not see"_v,
                where);
            failed = true;
            return nullptr;
        }

        auto node = builder.add<JsInstObject>(cellRep, where, jsRefShape(target));
        node->values.push(target.arena, owner - base());
        node->values.push(target.arena, key - base());
        owner->uses++;
        key->uses++;

        return base()[node->result];
    }

    /*
     * A value at the representation a position wants, which is the one place a place becomes a
     * reference rather than a read.
     *
     * Three positions ask for one: an argument whose plan says `Cell`, a `return` whose result does,
     * and an edge whose block argument does. Each of them is handing storage to somebody else, which
     * is what a descriptor is for; everything else reads.
     */
    JsValue* valueAs(SsaPtr<ssa::Value> pointer, JsRepId wanted, LocationId where) {
        auto value = ssa[pointer];

        if(wanted.valid() && target.rep(wanted).kind == JsRepKind::Cell) {
            auto& place = at(*value).place;
            if(place.valid()) return referenceTo(place, where);
        }

        return valueOf(pointer, where);
    }

    /*
     * One cell of a table - §3.7's erased boundary, and the whole of what a slot read is here.
     *
     * A cell is an element of a host row, so "which slot" is already an index and there is nothing
     * to scale it by: `codegen/js/place.cpp`'s `tableCell` says the same in one line, and the reason
     * it is that short is the same reason - a host array has no padding and a function is a name
     * rather than eight bytes. The element representation is `Erased` because what a cell holds is
     * exactly what this tier declines to describe.
     */
    JsValue* tableCell(JsValue* table, U32 slot, LocationId where) {
        if(!table) return nullptr;

        auto index = builder.number(F64(slot), jsNumberRep(target, NumberDomain::Unsigned, 32),
                                    where);
        return builder.load(builder.addElement(table, index, jsErasedRep(target)), where);
    }

    // A slot of *this* body's environment. Reports where there is none, which is a body the ABI
    // called specialized and the conversion is reading a table in.
    JsValue* genSlot(U32 slot, LocationId where) {
        if(!genEnv) {
            internal("met a generic slot read in a body with no environment"_v, where);
            return nullptr;
        }

        return tableCell(genEnv, slot, where);
    }

    /*
     * Which slot answers one thing about this body - `lower/erase.cpp`'s `variableSlot` and
     * `requirementSlot`, which is the numbering `freezeSchema` decided read from the other end.
     *
     * A variable is found by identity rather than by counting, for that file's reason: the two
     * fixed-width groups are interleaved in the parameter list and separated in the numbering, so
     * counting declaration positions would re-derive the split the schema exists to have made.
     */
    bool variableSlot(DeclPtr<mir::Type> variable, U32& into) {
        if(!genSchema) return false;

        auto& numbering = *decl[genSchema];
        for(U32 index = 0; index < numbering.slots.size(); index++) {
            if(numbering.slots.get(decl, index).type == variable) {
                into = index;
                return true;
            }
        }

        return false;
    }

    bool requirementSlot(mir::GenSlotKind kind, U16 requirement, U32& into) {
        if(!genSchema) return false;

        auto& numbering = *decl[genSchema];
        switch(kind) {
            case mir::GenSlotKind::Class:    into = numbering.classStart + requirement; return true;
            case mir::GenSlotKind::Property: into = numbering.propertyStart + requirement; return true;
            case mir::GenSlotKind::Function: into = numbering.functionStart + requirement; return true;
            default: return false;
        }
    }

    JsPlaceId placeOver(JsValue* held) {
        auto& rep = target.rep(held->rep);
        if(rep.kind == JsRepKind::Cell) return builder.addCell(held, rep.content);

        return builder.addValuePlace(held, held->rep);
    }

    JsPlaceId placeOf(SsaPtr<ssa::Value> pointer, LocationId where, bool& ok) {
        auto value = ssa[pointer];
        ok = true;

        auto& row = at(*value);
        if(row.place.valid()) return row.place;

        /*
         * An address that arrived as a value - a block argument, a call result, a parameter.
         *
         * What it is depends on what the occurrence says it is carried as, which is the one question
         * `jsManifest` exists to answer: a box, or the host object itself. Built lazily here rather
         * than eagerly where the value was defined, so that an address nothing ever reads through
         * costs no place at all.
         */
        if(row.one) {
            auto held = base()[row.one];
            auto place = placeOver(held);
            setPlace(value->id.index, place);
            return place;
        }

        /*
         * A multi-part value something reads *through* - the materialization goes in here.
         *
         * `carriesStorage` keeps a loan of storage from being taken apart in the first place, so
         * what reaches this is a value that was genuinely computed as parts and is then handed to
         * something that wants a place. One row, built once and memoized, which is what a place over
         * it needs to be: two of them would be two rows and a write through one would be lost.
         */
        if(row.parts.valid()) {
            auto ref = row.parts;
            auto one = valueOf(pointer, where);
            if(one) {
                auto place = placeOver(one);

                // Memoized for everything but a scalarized record, whose materialization may not be
                // shared between blocks - `valueOf` above keeps that one per block. Nothing writes
                // through one of those: a unique loan of a scalarized record is the object, which is
                // `jsManifest`'s answer and the reason the memo is only about reads here.
                if(!target.rep(ref.rep).freshEachUse) {
                    setOne(value->id.index, one - base());
                    setPlace(value->id.index, place);
                }

                return place;
            }
        }

        ok = false;
        internal("met an address it has not converted"_v, where);
        return JsPlaceId();
    }

    /*
     * `placeOf` at the eleven sites whose only answer to a failure is to stop converting.
     *
     * The `bool& ok` is written out at each of them as three lines - declare, call, test - and the
     * failure it reports is already a diagnostic at the instruction, so the caller has nothing to
     * add. An invalid id is the same answer said once: `placeOf` returns one exactly where it sets
     * `ok` to false and nowhere else, so the two tests are the same test. The out parameter stays
     * for the callers that have something else to do first.
     */
    JsPlaceId placeOrFail(SsaPtr<ssa::Value> pointer, LocationId where) {
        auto ok = true;
        auto place = placeOf(pointer, where, ok);
        return ok ? place : JsPlaceId();
    }

    // ---------------------------------------------------------------- constants

    /*
     * The value a freshly built slot of a representation holds.
     *
     * Every property a value of a shape will ever have is present, which is Analysis-JS.md §2.3 read
     * as a representation rule: construction order is this target's field offsets, so a record built
     * two ways in two places is a polymorphic call site for every reader downstream. Filling one
     * afterwards writes properties that already exist rather than adding them.
     */
    // The representations `zeroOf` is currently inside, so that a recursive one closes - see there.
    SmallArray<U32, 16> zeroing;

    // ---------------------------------------------------------------- niches

    /*
     * The three operations a **niche-folded** sum has, and the only three - §3.2's `Nullable`.
     *
     * `Maybe(Person)` is `Person | null` and `Maybe(Int)` is `number | null`, so there is no `$tag`
     * property to read, no payload property to project and no object to build. What replaces each of
     * them is here rather than at the sites, because a sum is reached from five directions - a
     * `match`, an `is`, a `?`, a refutable `let` and a `Select` over constructors - and all five
     * arrive at the same two instructions: a projection of `InstFieldAddress::kDiscriminant` or of
     * the payload field, and an `InstAggregate`. One statement per operation is what makes a later
     * encoding - a packed tag, a second niche - a change to three functions.
     *
     * The absence test is `=== null` at `HostBool` rather than at `Bool01`, which is the difference
     * between `v === null ? 0 : 1` and `((v === null) ? 1 : 0) ? 0 : 1`: the tag is consumed by a
     * comparison the AST peephole then folds against the decode (`foldTernaryCompare` in
     * `codegen/js/opt.cpp`), and the second form is one round of folding further away.
     */
    const JsRep* nicheOf(JsRepId repId) {
        if(!repId.valid()) return nullptr;

        auto& rep = target.rep(repId);
        return rep.kind == JsRepKind::Nullable ? &rep : nullptr;
    }

    // The absent value itself - `null` at the folded representation, which is what `Nothing` is.
    JsValue* absentOf(JsRepId nullable, LocationId where) {
        JsConstant constant;
        constant.kind = JsConstant::Null;
        return builder.constant(constant, nullable, where);
    }

    // `v === null`, as the host's own boolean.
    JsValue* isAbsent(JsValue* value, JsRepId nullable, LocationId where) {
        return binaryOf(mir::BinaryOp::Eq, jsHostBoolRep(target), value,
                        absentOf(nullable, where), where);
    }

    // The constructor index a `Discriminant` load of a folded value produces.
    JsValue* decodeNicheTag(JsValue* value, JsRepId nullable, LocationId where) {
        auto& rep = target.rep(nullable);
        auto test = isAbsent(value, nullable, where);
        return selectOf(test, builder.number(F64(rep.absentTag), rep.tag, where),
                        builder.number(F64(rep.presentTag), rep.tag, where), rep.tag, where);
    }

    JsValue* zeroOf(JsRepId repId, LocationId where, U32 depth = 0) {
        // Storage holds one host value, so the zero of a multi-part representation is the zero of
        // its materialized form - `jsStoredRep`, asked once here rather than at every caller.
        repId = jsStoredRep(target, repId);

        auto& rep = target.rep(repId);

        if(failed || depth > 32) return nullptr;

        /*
         * A **recursive** payload's zero is `null`, and the cycle is what says so.
         *
         * `data Branch = Leaf | Node {left: Branch, right: Branch}` has no finite zero object: the
         * shape's properties include the other arm's, whose zero would include its own. Natively
         * the answer is zero bytes and nothing recurses; here the honest value is the absent one,
         * and it is unreachable by construction - a property of an arm the tag does not name is
         * never read, and a copy copies `null` to `null`.
         *
         * A cycle test rather than a depth cap because a cap is the wrong shape twice: thirty-two
         * levels of a branching record is an exponential number of objects built before the cap
         * fires, which exhausted an arena list on `iter/Witness.yana` rather than reporting.
         */
        for(auto seen: zeroing) {
            if(seen == repId.index) {
                JsConstant absent;
                absent.kind = JsConstant::Null;
                return builder.constant(absent, repId, where);
            }
        }

        switch(rep.kind) {
            case JsRepKind::Void:
                return nullptr;

            case JsRepKind::Number:
                return builder.number(0, repId, where);

            case JsRepKind::HostBool:
                return builder.boolean(false, where);

            case JsRepKind::BigInt: {
                JsConstant constant;
                constant.kind = JsConstant::BigInt;
                constant.isSigned = rep.domain == NumberDomain::Signed;
                return builder.constant(constant, repId, where);
            }

            case JsRepKind::HostString: {
                JsConstant constant;
                constant.kind = JsConstant::Text;
                constant.text = jsInternText(target.context, target.interned, ""_v);
                return builder.constant(constant, repId, where);
            }

            /*
             * A row, and its elements where the type says how many there are.
             *
             * A `[T *n]` and a vector's materialized form both know their length, and a slot holding
             * one has to be that long from the start: a `Float32Array` built empty stays empty and
             * every write into it is dropped, and a plain row built empty reads back `undefined` at
             * every index the zero should have answered.
             */
            case JsRepKind::HostArray: {
                SmallArray<JsValue*, 16> elements;
                auto fills = rep.length && rep.content.valid() &&
                             target.rep(rep.content).kind != JsRepKind::Void;

                for(U32 i = 0; fills && i < rep.length; i++) {
                    auto element = zeroOf(rep.content, where, depth + 1);
                    if(!element) {
                        internal("met a row element with no zero"_v, where);
                        return nullptr;
                    }

                    elements.push(element);
                }

                auto inst = builder.add<JsInstArray>(repId, where);
                for(auto element: elements) {
                    inst->values.push(target.arena, element - base());
                    element->uses++;
                }

                return base()[inst->result];
            }

            /*
             * Both are an object of a shape, so both are the shape's properties at their own zero -
             * a `Cell` being the `{$o, $k}` descriptor and not a box around the content.
             *
             * One arm for the two, because a `Cell` whose zero was `{$v: <content's zero>}` was a
             * construction of a two-property shape with one value, which is a verifier failure
             * waiting for the first record to hold a `&` field. It waited until `Closure.yana`.
             */
            case JsRepKind::GcObject:
            case JsRepKind::Cell: {
                auto& shape = target.shape(rep.shape);

                zeroing.push(repId.index);
                SmallArray<JsValue*, 8> members;
                for(auto& property: shape.properties) members.push(zeroOf(property.rep, where, depth + 1));
                zeroing.pop();

                auto inst = builder.add<JsInstObject>(repId, where, rep.shape);
                for(auto member: members) {
                    if(!member) {
                        internal("met a shape property with no zero"_v, where);
                        return nullptr;
                    }

                    inst->values.push(target.arena, member - base());
                    member->uses++;
                }

                return base()[inst->result];
            }

            case JsRepKind::Erased:
            case JsRepKind::Nullable:
            case JsRepKind::Parts: {
                JsConstant constant;
                constant.kind = JsConstant::Null;
                return builder.constant(constant, repId, where);
            }
        }

        return nullptr;
    }

    /*
     * A source constant as a host value.
     *
     * Every branch has a counterpart in `zeroOf` above and produces the *same shape*, which is the
     * rule the two have to keep: a record built from a constant and a record built by filling a
     * fresh slot are values of one type, and a property one of them has and the other does not is a
     * second hidden class for every reader downstream.
     */
    JsValue* constantOf(const mir::Constant& constant, JsRepId repId, LocationId where) {
        auto& rep = target.rep(repId);

        /*
         * A **scalar** constant at a row representation is that value in every lane.
         *
         * `let &sink = 0 :: Vec(I16, 4)` is what reaches this: the literal is an integer and the
         * binding holds four of them, because `FromInt` over a vector means "the literal in every
         * lane" - the only answer that is not arbitrary (Design-Vector §3.1). It is reached from a
         * *global* and not from a body: inside one `convertVectorInst` takes a multi-part `Const`
         * before this switch, and a global has no body for it to run in.
         */
        if(rep.kind == JsRepKind::HostArray && rep.length && rep.content.valid() &&
           (constant.kind == mir::Constant::Int || constant.kind == mir::Constant::Float)) {
            SmallArray<JsValue*, 16> lanes;
            for(U32 i = 0; i < rep.length; i++) {
                lanes.push(constantOf(constant, rep.content, where));
            }

            auto inst = builder.add<JsInstArray>(repId, where);
            for(auto lane: lanes) {
                if(!lane) { internal("met a lane constant with no value"_v, where); return nullptr; }
                inst->values.push(target.arena, lane - base());
                lane->uses++;
            }

            return base()[inst->result];
        }

        switch(constant.kind) {
            case mir::Constant::None:
            case mir::Constant::Unit:
                return nullptr;

            case mir::Constant::Int: {
                auto bits = constant.asInt();

                if(rep.kind == JsRepKind::BigInt) {
                    JsConstant literal;
                    literal.kind = JsConstant::BigInt;
                    literal.bits = bits;
                    literal.isSigned = rep.domain == NumberDomain::Signed;
                    return builder.constant(literal, repId, where);
                }

                // `asInt` sign-extends from the declared width, so an unsigned type's pattern is
                // read back as the number it stands for rather than as a negative double.
                auto number = rep.domain == NumberDomain::Signed || rep.domain == NumberDomain::Bool01
                    ? F64(I64(bits))
                    : F64(rep.bits >= 64 ? bits : (bits & ((U64(1) << rep.bits) - 1)));

                return builder.number(number, repId, where);
            }

            case mir::Constant::Float: {
                JsConstant literal;
                literal.kind = JsConstant::Number;
                /*
                 * Narrowed to the position's width, which is where a `Float` literal becomes one.
                 *
                 * `mir::Constant::Float` is the IEEE pattern of the *double* whatever the declared
                 * width, and its own note says narrowing happens "where the bytes are laid out" -
                 * which on this target is here, since there are no bytes. Without it `0.1 :: Float`
                 * is the double `0.1` and compares unequal to every `Math.fround` of it, which is
                 * what `FloatShow.test` and `FloatRead.test` read back.
                 */
                literal.number = rep.domain == NumberDomain::Float32
                    ? F64(F32(constant.asFloat())) : constant.asFloat();
                literal.integral = false;
                return builder.constant(literal, repId, where);
            }

            case mir::Constant::Enum: {
                auto record = mir::recordDeclarationOf(decl, constant.type);
                auto index = constant.asConstructor();
                auto tag = record ? ((mir::RecordType*)decl[constant.type])->cons.get(decl, index).value
                                  : I64(index);

                if(rep.kind == JsRepKind::Number) return builder.number(F64(tag), repId, where);
                return aggregateConstant(constant, repId, index, where);
            }

            case mir::Constant::Text: {
                JsConstant literal;
                literal.kind = JsConstant::Text;
                literal.text = constant.asText();

                // A host string is its text and has nothing under it - the one place where this
                // target has no counterpart for what the native walk does, which is to point a run
                // at the bytes.
                return builder.constant(literal, jsStringRep(target), where);
            }

            case mir::Constant::Aggregate: {
                auto aggregate = decl[constant.aggregate()];
                return aggregateConstant(constant, repId, aggregate->constructor, where);
            }

            case mir::Constant::Function: {
                auto inst = builder.add<JsInstFunCode>(repId, where, constant.asFunction());
                return base()[inst->result];
            }

            case mir::Constant::Address:
                report("a constant naming a global's address"_v, where);
                return nullptr;
        }

        internal("met a constant kind it has no arm for"_v, where);
        return nullptr;
    }

    JsValue* aggregateConstant(const mir::Constant& constant, JsRepId repId, U16 constructor,
                               LocationId where) {
        auto& rep = target.rep(repId);
        auto type = constant.type;

        // A constructor with no payload has no children at all, which is a different thing from a
        // missing aggregate: `Nothing` is an `Enum` constant and reaches here with nothing to read.
        const mir::Constant* childData = nullptr;
        U32 childCount = 0;

        if(constant.kind == mir::Constant::Aggregate) {
            auto aggregate = decl[constant.aggregate()];
            childData = aggregate->values.pointer(decl);
            childCount = U32(aggregate->values.size());
        }

        if(rep.kind == JsRepKind::HostArray) {
            auto element = decl[type]->kind == mir::Type::Array
                ? ((mir::ArrayType*)decl[type])->element : nullptr;

            auto elementRep = element ? jsRep(target, element, where) : rep.content;

            SmallArray<JsValue*, 8> members;
            for(U32 i = 0; i < childCount; i++) {
                members.push(constantOf(childData[i], elementRep, where));
            }

            auto inst = builder.add<JsInstArray>(repId, where);
            for(auto member: members) {
                if(!member) { internal("met an array constant element with no value"_v, where); return nullptr; }
                inst->values.push(target.arena, member - base());
                member->uses++;
            }

            return base()[inst->result];
        }

        /*
         * A **niche-folded** constant: `Nothing` is `null` and `Just(k)` is `k`'s own constant.
         *
         * Reached through `Constant::Enum` for the absent arm, which carries no aggregate at all,
         * and through `Constant::Aggregate` for the present one - which is why this is asked of the
         * representation rather than of the constant's kind.
         */
        if(auto niche = nicheOf(repId)) {
            if(decl[type]->kind != mir::Type::Record) {
                report("a constant of a niche-folded value of a non-record type"_v, type, where);
                return nullptr;
            }

            auto arm = ((mir::RecordType*)decl[type])->cons.get(decl, constructor);
            if(I32(arm.value) != niche->presentTag) return absentOf(repId, where);

            // The payload by its position in the constructor - see `convertAggregate`, where the
            // unit field beside it is what made the count the wrong question.
            auto at = U32(niche->payloadField - arm.first);
            if(at < childCount) {
                // The payload's own constant, **relabelled** to the folded representation: `Just(4)`
                // is the number four and is still a `Maybe(I32)`, and a property that wants one is
                // held to the representation rather than to the host value. The `Alias` emits
                // nothing, which is what makes this a statement rather than a conversion.
                auto payload = constantOf(childData[at], niche->content, where);
                if(!payload) return nullptr;

                auto inst = builder.add<JsInstAlias>(repId, where, payload - base());
                return base()[inst->result];
            }

            report("a constant of a niche-folded value with no payload"_v, type, where);
            return nullptr;
        }

        if(rep.kind != JsRepKind::GcObject) {
            /*
             * A transparent record, whose constant is its one field's - the same statement `jsRep`
             * makes about the type, reached from the value side.
             */
            if(childCount == 1) return constantOf(childData[0], repId, where);
            report("a constant of this representation"_v, type, where);
            return nullptr;
        }

        auto record = (mir::RecordType*)decl[type];
        auto arm = record->cons.get(decl, constructor);
        auto& shape = target.shape(rep.shape);

        SmallArray<JsValue*, 8> members;
        for(auto& property: shape.properties) {
            if(property.role == JsPropertyRole::Tag) {
                members.push(builder.number(F64(arm.value), property.rep, where));
                continue;
            }

            if(property.field >= arm.first && property.field < U16(arm.first + arm.count)) {
                auto index = U32(property.field - arm.first);
                if(index < childCount) {
                    members.push(constantOf(childData[index], property.rep, where));
                    continue;
                }
            }

            members.push(zeroOf(property.rep, where));
        }

        auto inst = builder.add<JsInstObject>(repId, where, rep.shape);
        for(auto member: members) {
            if(!member) { internal("met a constant property with no value"_v, where); return nullptr; }
            inst->values.push(target.arena, member - base());
            member->uses++;
        }

        return base()[inst->result];
    }

    // ---------------------------------------------------------------- coercion

    /*
     * Whether a value of this representation has to be brought back into range after an operation.
     *
     * The same rule the shipping backend's `coerce` follows and for the same reason: `Int` is a
     * wrapping 32-bit integer on both targets and JS has no 32-bit integer, so every arithmetic
     * result is reduced. What is *not* coerced is what the host's own operator already produces - a
     * `Bool01` from a bitwise operator, an `F64` from `+`, anything that is not a number at all.
     */
    bool needsCoercion(JsRepId repId) {
        auto& rep = target.rep(repId);
        if(rep.kind == JsRepKind::BigInt) return true;
        if(rep.kind != JsRepKind::Number) return false;

        switch(rep.domain) {
            case NumberDomain::Bool01:  return false;
            case NumberDomain::Float64: return false;
            case NumberDomain::Float32: return true;
            case NumberDomain::Signed:
            case NumberDomain::Unsigned: return true;
        }

        return false;
    }

    JsValue* coerced(JsValue* value, JsRepId repId, LocationId where) {
        if(!value || !needsCoercion(repId)) return value;

        auto inst = builder.add<JsInstCoerce>(repId, where, value - base());
        return base()[inst->result];
    }

    // ---------------------------------------------------------------- lanes

    /*
     * The multi-part arithmetic - Implementation-Vector.md §7, and §3.2's `Parts` row for a vector.
     *
     * What is *not* here is the whole point: a lane is an ordinary JSIR value of an ordinary scalar
     * representation, so `structure.cpp` spells a lane's multiply as `Math.imul` and a lane's
     * division as `$div` by reading the result's representation, exactly as it does for a scalar. So
     * none of the thirty spellings the shipping backend's `laneBinary` restates is restated here.
     *
     * What is left is the four things a lane genuinely differs in: the two `Core.Bits` families are
     * declared at widths the helpers do not have, a mask lane is a host boolean and its bitwise
     * operators are the logical ones, a reduction is a tree, and the saturating pair has to be exact
     * before it clamps.
     */
    JsRepId partsRepOf(ssa::Value& value) {
        auto id = manifestOf(value);
        return id.valid() && target.rep(id).kind == JsRepKind::Parts ? id : JsRepId();
    }

    /*
     * The same, asked only of the parts an *operation* distributes over - `jsPartsAreLanes`.
     *
     * Every use inside `convertVectorInst` goes through this one rather than through `partsRepOf`,
     * because that switch runs in front of the scalar one and J71 gave `Parts` a second producer: a
     * window is three components of a record and `represent` over one is not a lane-wise conversion
     * of a host array and two numbers.
     */
    JsRepId lanesRepOf(ssa::Value& value) {
        auto id = partsRepOf(value);
        return jsPartsAreLanes(target, id) ? id : JsRepId();
    }

    // Whether a multi-part representation is a **mask** - its lanes are host booleans, which is the
    // representation question and not `VectorType::isMask` read down here.
    bool isMaskRep(JsRepId partsRep) {
        auto lane = partRepAt(target, partsRep, 0);
        return lane.valid() && target.rep(lane).kind == JsRepKind::HostBool;
    }

    JsValue* binaryOf(mir::BinaryOp op, JsRepId repId, JsValue* lhs, JsValue* rhs, LocationId where) {
        if(!lhs || !rhs) return nullptr;

        auto node = builder.add<JsInstBinary>(repId, where, op, lhs - base(), rhs - base());
        return base()[node->result];
    }

    JsValue* unaryOf(mir::UnaryOp op, JsRepId repId, JsValue* value, LocationId where) {
        if(!value) return nullptr;

        auto node = builder.add<JsInstUnary>(repId, where, op, value - base());
        return base()[node->result];
    }

    JsValue* helperOf(JsHelperOp op, JsRepId repId, Buffer<JsValue*> arguments, LocationId where) {
        auto node = builder.add<JsInstHelper>(repId, where, op);
        for(auto argument: arguments) {
            if(!argument) return nullptr;
            node->arguments.push(target.arena, argument - base());
            argument->uses++;
        }

        return base()[node->result];
    }

    JsValue* mathOf(JsMathOp op, JsRepId repId, JsValue* lhs, JsValue* rhs, LocationId where) {
        if(!lhs || !rhs) return nullptr;

        auto node = builder.add<JsInstMath>(repId, where, op, lhs - base(), rhs - base());
        return base()[node->result];
    }

    JsValue* selectOf(JsValue* condition, JsValue* whenTrue, JsValue* whenFalse, JsRepId repId,
                      LocationId where) {
        if(!condition || !whenTrue || !whenFalse) return nullptr;

        auto node = builder.add<JsInstSelect>(repId, where, condition - base(), whenTrue - base(),
                                              whenFalse - base());
        return base()[node->result];
    }

    JsValue* convertedTo(JsValue* value, JsRepId repId, LocationId where) {
        if(!value || value->rep == repId) return value;

        auto node = builder.add<JsInstConvert>(repId, where, value - base());
        node->fromSigned = target.rep(value->rep).isSigned();
        return base()[node->result];
    }

    // The mask `(1 << bits) - 1` at a lane narrower than the helpers' word, as a literal of the
    // lane's own representation. Null where the lane is wide enough to need none.
    JsValue* laneMask(JsRepId lane, LocationId where) {
        auto& rep = target.rep(lane);
        if(rep.kind != JsRepKind::Number || rep.bits >= 32) return nullptr;

        return builder.number(F64((U64(1) << rep.bits) - 1), lane, where);
    }

    /*
     * The three bit counts at a **lane**, which is the one place a width narrower than the two
     * `Core.Bits` is declared at appears - `Bits(Vec(a, n))` is instanced at every integer lane
     * width and the scalar class stays at thirty-two and sixty-four.
     *
     * The helpers count a 32-bit word, so a byte or word lane is counted there and corrected. The
     * lane arrives as a *signed* host number wherever its type is signed, so the correction starts
     * by masking it to its own bits - otherwise `countBits` of `-1 :: I8` would be thirty-two.
     *
     *   countBits      helper(v & mask)
     *   leadingZeros   helper(v & mask) - (32 - bits)
     *   trailingZeros  helper((v & mask) | (1 << bits))
     *
     * The trailing count's extra bit is what gives the zero lane the language's answer - the lane's
     * own width - without a test: it is the lowest set bit of every value the mask cannot hold, and
     * it is above every bit the mask can.
     */
    JsValue* laneBitCount(JsHelperOp op, JsRepId lane, JsValue* value, LocationId where) {
        auto mask = laneMask(lane, where);
        if(!mask) {
            JsValue* one[] = { value };
            return helperOf(op, lane, Buffer<JsValue*> { one, 1 }, where);
        }

        auto bits = U32(target.rep(lane).bits);
        auto masked = binaryOf(mir::BinaryOp::And, lane, value, mask, where);

        if(op == JsHelperOp::TrailingZeros) {
            auto bit = builder.number(F64(U64(1) << bits), lane, where);
            masked = binaryOf(mir::BinaryOp::Or, lane, masked, bit, where);
        }

        JsValue* one[] = { masked };
        auto counted = helperOf(op, lane, Buffer<JsValue*> { one, 1 }, where);

        if(op != JsHelperOp::LeadingZeros) return counted;

        return binaryOf(mir::BinaryOp::Sub, lane, counted,
                        builder.number(F64(32 - bits), lane, where), where);
    }

    /*
     * `bitsUpTo` at a lane, on the same terms.
     *
     * A narrow lane masks both operands to its own width first. The value's mask is what makes a
     * signed lane's high bits stop existing, and the *count's* is the unsigned reading the
     * operation's ruling asks for - a count of `-1 :: I8` is 255 and saturates.
     */
    JsValue* laneBitsUpTo(JsRepId lane, JsValue* value, JsValue* count, LocationId where) {
        if(auto mask = laneMask(lane, where)) {
            value = binaryOf(mir::BinaryOp::And, lane, value, mask, where);
            count = binaryOf(mir::BinaryOp::And, lane, count, laneMask(lane, where), where);
        }

        JsValue* two[] = { value, count };
        return helperOf(JsHelperOp::BitsUpTo, lane, Buffer<JsValue*> { two, 2 }, where);
    }

    /*
     * `saturatingAdd` and `saturatingSub` at a lane - Analysis-Avx512.md §3.8.
     *
     * **The sum is exact before it is clamped**, which is what makes one formula serve every lane
     * width here. A lane of 32 bits or fewer is a host `number`, and the sum of two of them is below
     * 2^33 - well inside the 2^53 a double holds exactly; a 64-bit lane is a `bigint`, whose
     * arithmetic is exact by construction. So neither addend has wrapped by the time the comparison
     * looks at it, which is precisely the mistake `min(a + b, maxValue())` makes in source.
     *
     * A `number` lane clamps through `Math.min`/`Math.max`, where each subexpression is written
     * once. A `bigint` lane has no such pair - they coerce their arguments to numbers and throw on
     * one - so the comparison chain writes the sum three times, which is what a 64-bit saturating
     * lane costs here.
     */
    JsValue* laneSaturating(mir::BinaryOp op, JsRepId lane, JsValue* lhs, JsValue* rhs,
                            LocationId where) {
        auto& rep = target.rep(lane);
        auto width = U32(rep.bits ? rep.bits : 32);
        auto isSigned = rep.domain == NumberDomain::Signed;

        auto sign = U64(1) << (width - 1);
        auto mask = width >= 64 ? ~U64(0) : (U64(1) << width) - 1;

        auto bound = [&](U64 bits) -> JsValue* {
            if(rep.kind == JsRepKind::BigInt) {
                JsConstant constant;
                constant.kind = JsConstant::BigInt;
                constant.bits = bits;
                constant.isSigned = isSigned;
                return builder.constant(constant, lane, where);
            }

            return builder.number(isSigned ? F64(I64(I64(bits << (64 - width)) >> (64 - width)))
                                           : F64(bits), lane, where);
        };

        auto low = bound(isSigned ? sign : 0);
        auto high = bound(isSigned ? sign - 1 : mask);
        auto plain = op == mir::BinaryOp::SaturatingAdd ? mir::BinaryOp::Add : mir::BinaryOp::Sub;

        if(rep.kind != JsRepKind::BigInt) {
            auto exact = binaryOf(plain, lane, lhs, rhs, where);
            return mathOf(JsMathOp::Min, lane, mathOf(JsMathOp::Max, lane, exact, low, where), high,
                          where);
        }

        auto exact = [&]() { return binaryOf(plain, lane, lhs, rhs, where); };
        auto tooHigh = binaryOf(mir::BinaryOp::Gt, lane, exact(), high, where);
        auto clampedHigh = selectOf(tooHigh, high, exact(), lane, where);
        auto tooLow = binaryOf(mir::BinaryOp::Lt, lane, exact(), low, where);
        return selectOf(tooLow, low, clampedHigh, lane, where);
    }

    /*
     * One lane of a binary operation, at the lane's own representation.
     *
     * Three families and the rest: the mask's bitwise operators, the two `Core.Bits` rows a lane
     * width changes, and the saturating pair. Everything else is the scalar node - the same
     * `JsInstBinary` a scalar gets, whose spelling `structure.cpp` reads off the result's
     * representation, followed by the same coercion. A comparison is not coerced, for the scalar
     * path's reason: what it answers is a `Bool` or a mask lane and neither has a range to reduce.
     */
    JsValue* laneBinary(mir::BinaryOp op, JsRepId lane, JsValue* lhs, JsValue* rhs,
                        LocationId where) {
        switch(op) {
            case mir::BinaryOp::BitsUpTo:
                return coerced(laneBitsUpTo(lane, lhs, rhs, where), lane, where);

            case mir::BinaryOp::SaturatingAdd:
            case mir::BinaryOp::SaturatingSub:
                return laneSaturating(op, lane, lhs, rhs, where);

            case mir::BinaryOp::Rol:
            case mir::BinaryOp::Ror:
            case mir::BinaryOp::GatherBits:
            case mir::BinaryOp::ScatterBits: {
                auto helper = op == mir::BinaryOp::Rol        ? JsHelperOp::Rol
                            : op == mir::BinaryOp::Ror        ? JsHelperOp::Ror
                            : op == mir::BinaryOp::GatherBits ? JsHelperOp::GatherBits
                                                              : JsHelperOp::ScatterBits;

                JsValue* two[] = { lhs, rhs };
                return coerced(helperOf(helper, lane, Buffer<JsValue*> { two, 2 }, where), lane,
                               where);
            }

            default:
                break;
        }

        auto value = binaryOf(op, lane, lhs, rhs, where);
        return mir::isComparison(op) ? value : coerced(value, lane, where);
    }

    JsValue* laneUnary(mir::UnaryOp op, JsRepId lane, JsValue* value, LocationId where) {
        switch(op) {
            case mir::UnaryOp::CountBits:
            case mir::UnaryOp::LeadingZeros:
            case mir::UnaryOp::TrailingZeros: {
                auto helper = op == mir::UnaryOp::CountBits    ? JsHelperOp::CountBits
                            : op == mir::UnaryOp::LeadingZeros ? JsHelperOp::LeadingZeros
                                                               : JsHelperOp::TrailingZeros;

                return coerced(laneBitCount(helper, lane, value, where), lane, where);
            }

            case mir::UnaryOp::ByteSwap: {
                JsValue* one[] = { value };
                return coerced(helperOf(JsHelperOp::ByteSwap, lane, Buffer<JsValue*> { one, 1 },
                                        where), lane, where);
            }

            default:
                return coerced(unaryOf(op, lane, value, where), lane, where);
        }
    }

    // ---------------------------------------------------------------- teardown

    /*
     * The end of one object's life - §3.9, and the one place a collected target still emits code.
     *
     * Derived reclamation of GC storage emits nothing: the host collector is what hands storage
     * back, so a record whose teardown is the compiler's walk over members costs exactly the members
     * that owe something. An **authored** teardown is an ordinary effectful call and stays, in OSSA
     * order, because the author is the only thing that knows what it does.
     *
     * The walk mirrors `lower/inst.cpp`'s `emitTeardown`. The one shape it does not write inline is
     * a *sum* whose teardown is derived: that branches on the tag, and a branch is a CFG change made
     * in the middle of a block the caller is still filling - so it becomes one `TypeDrop`, naming a
     * function `runTypeTeardown` builds. Which is what the native walk does with the same shape.
     */

    /*
     * The **authored** halves of one type's teardown, which is all a collected target owes for a
     * type that owns nothing structurally.
     *
     * Its own function rather than a lambda inside `emitTeardown` because the sum's own body runs
     * exactly these two and then its arm walk - calling `emitTeardown` there would redirect straight
     * back to a call of the function being built.
     */
    bool emitAuthored(JsPlaceId place, DeclPtr<mir::Type> type, const JsTeardownStep& step,
                      LocationId where) {
        auto one = [&](intrinsic::Known klass, bool run) {
            if(!run) return true;

            auto member = mir::teardownMember(target.context, target.program, klass, type, where);
            if(!member) return true;

            /*
             * The member takes `->value: T`, so what it is handed is the value at the place - which
             * on this target is the host object itself rather than an address. That the two agree is
             * not luck: the declaration walk checked the signature, and the plan this reads is the
             * same one an ordinary call to it reads.
             */
            auto abi = jsAbiOf(target, member, where);
            auto& plan = target.abi(abi);
            if(plan.parameters.size() != 1 ||
               plan.parameters[0].kind == JsPassingKind::Omitted) {
                report("a teardown whose member takes more than one value"_v, type, where);
                return false;
            }

            auto value = builder.load(place, where);
            if(!value) {
                internal("met a teardown of storage with no value"_v, where);
                return false;
            }

            JsRepId resultRep;
            if(plan.result.kind != JsPassingKind::Omitted) resultRep = plan.result.rep;

            auto node = builder.add<JsInstCall>(resultRep, where, member, abi);
            node->arguments.push(target.arena, value - base());
            value->uses++;
            return true;
        };

        // The effect first, then the storage: `Drop` runs at last use and `Reclaim` hands back what
        // the value occupied, and a release that happened first would be running an effect over
        // memory that has already been returned.
        if(!one(intrinsic::Known::drop, step.authoredDrop)) return false;
        return one(intrinsic::Known::reclaim, step.authoredReclaim);
    }

    bool emitTeardown(JsPlaceId place, DeclPtr<mir::Type> type, bool runDrop, bool runReclaim,
                      LocationId where, U32 depth = 0) {
        if(!type || depth > 16) return true;

        /*
         * Which arm and which halves is `jsTeardownStep`'s answer and not this file's, because
         * `jsOwesTeardown` reads the same one: the two used to be transcripts of one shape and had
         * drifted in two places, and the one that mattered is that only this half had a
         * `Type::Variable` arm - so a sum whose payload is a type variable was gated off a
         * predicate that said it owed nothing and never got its `TypeDrop`.
         */
        auto step = jsTeardownStep(target, type, runDrop, runReclaim);
        if(!emitAuthored(place, type, step, where)) return false;

        auto childDrop = step.childDrop;
        auto childReclaim = step.childReclaim;

        switch(step.kind) {
            case JsTeardownKind::Nothing:
                return true;

            case JsTeardownKind::Unsupported:
                report("a teardown of this representation"_v, type, where);
                return false;

            /*
             * A **function value**, whose release is the pair's two questions - §3.2.1's last three
             * paragraphs, and `lower/fun.cpp`'s `funTeardown` on a target whose collector owns the
             * block.
             *
             * The step answers `Nothing` where no lambda in the program captures anything with an
             * authored teardown, which is the common case and is §3.9 rather than an optimization:
             * what the native function does is run the captures' effects *and* hand the block back,
             * and the second half has no counterpart here.
             *
             * A site that can see which lambda this is does not reach here at all - `ownershipOf`
             * answers "nothing to release" for a value carrying `Shapes::closure` - so this is the
             * escaped case only, and one call is what it costs.
             *
             * An **erased iterator object** is the same two words over the same block and is
             * released by the same helper, which is `mir::IterType`'s own decision one tier down.
             */
            case JsTeardownKind::FunValue: {
                auto pair = builder.load(place, where);
                if(!pair) {
                    internal("met a teardown of a function value with nothing in it"_v, where);
                    return false;
                }

                auto node = builder.add<JsInstHelper>(JsRepId(), where, JsHelperOp::FunDrop);
                node->arguments.push(target.arena, pair - base());
                pair->uses++;
                return true;
            }

            case JsTeardownKind::FixedArray: {
                // `jsOwesTeardown` and not `mir::hasTeardown`: the second is a question about
                // storage, and asking it here gave a row of `[Handle *n]` elements a place and an
                // index each where the walk below had nothing to write into them.
                if(!jsOwesTeardown(target, step.content)) return true;

                auto& array = *(mir::ArrayType*)decl[type];

                // Unrolled, for the reason the native walk gives: a loop would need control flow
                // and this emits into a block its caller is in the middle of.
                auto owner = builder.load(place, where);
                auto elementRep = jsStoredRep(target, jsRep(target, step.content, where));

                for(U64 i = 0; i < array.length; i++) {
                    auto index = builder.number(F64(i),
                                                jsNumberRep(target, NumberDomain::Signed, 32), where);
                    auto slot = builder.addElement(owner, index, elementRep);
                    if(!emitTeardown(slot, step.content, childDrop, childReclaim, where, depth + 1)) {
                        return false;
                    }
                }

                return true;
            }

            /*
             * A value of a type **this body was not compiled for** - the descriptor's teardown
             * cell, called with the value.
             *
             * Unconditional, which is why the cell is never null: a test and a branch in the middle
             * of a block the caller is filling is exactly the CFG change the sum arm below exists to
             * avoid, and the alternative - one shared no-op per program - costs a call that does
             * nothing at a site a body compiled once reaches rarely. That is the same reasoning
             * `lower/erase.cpp` records for its own never-null cell, from the other direction.
             */
            case JsTeardownKind::Descriptor: {
                auto slot = U32(0);
                if(!variableSlot(type, slot)) {
                    report("a teardown of a type a body compiled once cannot see"_v, type, where);
                    return false;
                }

                auto descriptor = genSlot(slot, where);
                auto callee = tableCell(descriptor, kDescriptorTeardown, where);
                auto value = builder.load(place, where);
                if(!callee || !value) return false;

                auto node = builder.add<JsInstCallValue>(JsRepId(), where, callee - base(), nullptr,
                                                         jsErasedDropAbi(target));
                callee->uses++;
                node->arguments.push(target.arena, value - base());
                value->uses++;
                return true;
            }

            /*
             * A **sum**, whose derived walk branches on the tag - one call to the type's own
             * function.
             *
             * A branch is a CFG change in the middle of a block the caller is still filling, which
             * is the same reason the fixed array above is unrolled and the reason this cannot be.
             * Natively it is `teardownHalves`, a function per (type, halves) that every site calls,
             * and the same answer works here - `JsInstTypeDrop`, whose body `convertTypeTeardown`
             * builds.
             */
            case JsTeardownKind::Sum: {
                if(!jsOwesTeardown(target, type)) return true;

                auto value = builder.load(place, where);
                if(!value) {
                    internal("met a teardown of a sum with no value"_v, where);
                    return false;
                }

                auto node = builder.add<JsInstTypeDrop>(JsRepId(), where, value - base(), type,
                                                        childDrop, childReclaim);
                value->uses++;
                return true;
            }

            case JsTeardownKind::Record:
                break;
        }

        auto& record = *(mir::RecordType*)decl[type];
        auto manifest = jsManifest(target, mir::owned(type), where);
        auto& rep = target.rep(manifest.rep);
        if(rep.kind != JsRepKind::GcObject) {
            // A transparent record is its one field, so the walk goes straight through it.
            auto content = transparentContentOf(target, type);
            if(!content) return true;

            return emitTeardown(place, content, childDrop, childReclaim, where, depth + 1);
        }

        auto owner = builder.load(place, where);
        if(!owner) return true;

        for(U16 i = 0; i < record.fields.size(); i++) {
            auto field = record.fields.get(decl, i);

            // `tearsDownField` and not `hasTeardown` on the type: a field in an origin slot is
            // storage somebody else owns.
            if(!mir::tearsDownField(decl, field)) continue;
            if(field.boxed) {
                report("a teardown through a boxed field"_v, type, where);
                return false;
            }

            auto slot = target.shape(rep.shape).slotOfField(i);
            if(slot == JsProperty::kNoField) continue;

            auto held = builder.addProperty(owner, rep.shape, slot);
            if(!emitTeardown(held, field.occurrence.type, childDrop, childReclaim, where, depth + 1)) {
                return false;
            }
        }

        return true;
    }

    // ---------------------------------------------------------------- instructions

    Maybe<JsConstant> constantLiteral(const mir::Constant& constant, JsRepId repId);

    void convertInst(ssa::Inst& inst);
    bool convertVectorInst(ssa::Inst& inst);
    JsValue* reduceLanes(mir::ReduceOp reduce, const PartsRef& from, JsRepId repId, bool mask,
                         U16 at, U16 count, LocationId where);
    void convertLaneMove(ssa::Inst& inst, ssa::InstNative& written, JsRepId repId, LocationId where);
    void convertDotProduct(ssa::Inst& inst, ssa::InstNative& written, JsRepId repId,
                           LocationId where);
    void convertTerminator(ssa::Inst& inst, JsBlock* block);
    void convertCall(ssa::InstCall& call);

    /*
     * What every one of `convertCall`'s three arms ends with: the arguments attached, and the one
     * result bound.
     *
     * Three arms - a slot dispatch, a named callee and a call through a pair - each built a
     * different node and then said the same eleven lines. The counting is the half that is easy to
     * leave out of a fourth: `JsValue::uses` is what `codegen/js/opt.cpp` reads to decide whether an
     * expression may be written at its single use, so an argument nothing counted is one written
     * twice. A template over the node because `JsInstCall` and `JsInstCallValue` are two rows with
     * the same argument list and no common base that carries it.
     */
    template<class Node, class Arguments>
    void finishCall(Node* inst, ssa::InstCall& call, const Arguments& arguments, JsRepId resultRep) {
        for(auto argument: arguments) {
            inst->arguments.push(target.arena, argument - base());
            argument->uses++;
        }

        if(!call.results.size()) return;

        auto result = ssa[call.results.get(ssa, 0)];
        if(!resultRep.valid()) setVoid(result->id.index);
        else setOne(result->id.index, inst->result);
    }
    void convertAggregate(ssa::InstAggregate& aggregate);
    void convertHost(ssa::InstHost& host);
    void collectEdge(ssa::Edge& source, LocationId where, SmallArray<JsValue*, 8>& into);
    void supplyEdge(JsEdge& edge, JsBlock* from, ssa::Edge& source, LocationId where,
                    Buffer<JsValue*> values);

    bool run();
    bool runConstant(const mir::Constant* constant, JsRepId rep, bool boxed, LocationId where);
    bool runEnvironmentTeardown(DeclPtr<mir::Type> environment, LocationId where,
                                const JsPassing& passing);
    bool runTypeTeardown(DeclPtr<mir::Type> type, bool runDrop, bool runReclaim, LocationId where,
                         const JsPassing& passing);
};

} // namespace jsir
