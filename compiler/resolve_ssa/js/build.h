#pragma once

#include "repr.h"

/*
 * The typed builder surface - Analysis-IR-JS.md §8.2.
 *
 * There is deliberately **no universal `useValue` returning an expression**. The old JavaScript
 * backend (deleted with `compiler/resolve` in J173) had one, and what it cost is the whole of
 * §1.2's table: a caller that wanted a place got one that happened to read correctly and wrote to a
 * copy; a caller that wanted one value got a flattened reference's first part; a caller whose value
 * had no mapping got `null`. Each of those is a function whose return type does not say what came
 * back.
 *
 * So the surface here states the form. `requireOne` answers a value or reports; `lowerPlace` answers
 * a place even when reading it would be a compound expression, so the two cannot be confused; and
 * `load`, `init` and `replace` are the only things that accept a place at all.
 *
 * Every builder keeps the body's def-use count and the block's incoming list in step, which is the
 * `IrEditor` discipline one tier up: a structure half of whose statement has been written is what
 * the verifier then has to catch instead of it being unrepresentable.
 */
namespace jsir {

struct JsBuilder {
    JsBuilder(JsTarget& target, JsBody& body): target(target), body(body), base(*target.arena) {}

    JsTarget& target;
    JsBody& body;
    JsirBase base;

    // Where instructions go. A terminator closes it; `switchTo` is how the conversion moves.
    JsBlock* current = nullptr;

    JsBlock* addBlock(StringId name) {
        auto index = JsBlockId(body.blocks.size());
        auto block = new (target.arena) JsBlock(index, name);
        body.blocks.push(target.arena, block - base);
        return block;
    }

    void switchTo(JsBlock* block) { current = block; }

    JsValue* addValue(JsRepId rep) {
        auto value = new (target.arena) JsValue(rep);
        value->id = JsValueId(body.values.size());
        body.values.push(target.arena, value - base);
        return value;
    }

    JsValue* addArgument(JsBlock* block, JsRepId rep) {
        auto value = addValue(rep);
        value->block = block - base;
        block->argumentList.push(target.arena, value - base);
        return value;
    }

    /*
     * One instruction, appended to the current block, with the result value its row says it has.
     *
     * `result` is the representation of that value and is invalid where the row has none. The
     * builder creates the value rather than the caller, so an instruction whose row says it produces
     * one cannot be built without it - which is the half of §4.1 that is a compile-time property
     * rather than a check.
     */
    template<class T, class... A>
    T* add(JsRepId result, A&&... args) {
        auto inst = new (target.arena) T(forward<A>(args)...);
        inst->id = JsInstId(body.insts.size());
        body.insts.push(target.arena, (JsInst*)inst - base);

        assertTrue(current != nullptr);
        inst->block = current - base;
        current->instructionList.push(target.arena, (JsInst*)inst - base);

        if(jsInstResult(inst->kind) != kNoResult && result.valid()) {
            auto value = addValue(result);
            value->definer = (JsInst*)inst - base;
            inst->result = value - base;
        }

        countUses(*(JsInst*)inst);
        return inst;
    }

    // The uses an instruction's operands make, counted as it is added. A list is built by whoever
    // needs one, over the body it is walking - §7.4's rule that a use row per value is the wrong
    // container for this tier.
    void countUses(JsInst& inst) {
        forEachUse(base, inst, [&](JsirPtr<JsValue> used) {
            if(used) base[used]->uses++;
        });
    }

    JsPlaceId addPlace(const JsPlace& place) {
        auto stored = new (target.arena) JsPlace(place);
        stored->id = JsPlaceId(body.places.size());
        body.places.push(target.arena, stored - base);
        return stored->id;
    }

    JsPlace& place(JsPlaceId id) { return *body.place(base, id); }

    /*
     * A variable place - what an allocation becomes when nothing takes its address away.
     *
     * Never a box by default, which is §5.4's first bullet and the one this target has historically
     * got wrong in the other direction: the old JavaScript backend boxed a local the moment
     * anything borrowed it, whether or not the borrow escaped. A box that is added here is a box
     * the demand pass proved is needed.
     */
    JsPlaceId addVariable(JsRepId content) {
        JsPlace place;
        place.kind = JsPlaceKind::Var;
        place.content = content;
        place.slot = body.variables.size();
        body.variables.push(target.arena, content);
        return addPlace(place);
    }

    // Storage reached through a descriptor - `reference.$o[reference.$k]`. The reference is a value
    // of `Cell(content)`; what it points at is a property or an element of something else.
    JsPlaceId addCell(JsValue* reference, JsRepId content) {
        JsPlace place;
        place.kind = JsPlaceKind::Cell;
        place.content = content;
        place.shape = jsRefShape(target);
        place.owner = reference - base;
        reference->uses++;
        return addPlace(place);
    }

    JsPlaceId addProperty(JsValue* owner, JsShapeId shape, U16 slot) {
        JsPlace place;
        place.kind = JsPlaceKind::Property;
        place.content = target.shape(shape).properties[slot].rep;
        place.shape = shape;
        place.slot = slot;
        place.owner = owner - base;
        owner->uses++;
        return addPlace(place);
    }

    JsPlaceId addElement(JsValue* owner, JsValue* index, JsRepId content) {
        JsPlace place;
        place.kind = JsPlaceKind::Element;
        place.content = content;
        place.owner = owner - base;
        place.index = index - base;
        owner->uses++;
        index->uses++;
        return addPlace(place);
    }

    JsPlaceId addGlobal(DeclPtr<mir::Global> global, JsRepId content) {
        JsPlace place;
        place.kind = JsPlaceKind::Global;
        place.content = content;
        place.global = global;
        return addPlace(place);
    }

    // The storage *is* this object - a `&Point` parameter, or a shared borrow whose value is the
    // value. A whole-value store into one writes its properties; see `JsPlaceKind::Value`.
    JsPlaceId addValuePlace(JsValue* object, JsRepId content) {
        JsPlace place;
        place.kind = JsPlaceKind::Value;
        place.content = content;
        place.owner = object - base;
        object->uses++;
        return addPlace(place);
    }

    /*
     * The same storage, read at a niche-folded value's **payload** representation.
     *
     * `Just(x)` *is* `x` here, so the payload has no place of its own: what a projection into it
     * names is the slot, the property or the element the value is already in, and only what a
     * reader may assume about the contents differs. A copy of the place rather than a kind of its
     * own, so that every existing arm - the variable, the descriptor, the property, the element,
     * the object that is its own storage - keeps working unchanged; `JsPlace::nichePayload` is the
     * flag the verifier reads as the licence for the one mismatch that follows.
     */
    JsPlaceId addNichePayload(JsPlaceId from, JsRepId payload) {
        JsPlace copy = place(from);
        copy.content = payload;
        copy.nichePayload = true;

        // The owner is named a second time, which `JsValue::uses` is allowed to over-count - see
        // `opt_alias.cpp`, where the two counts and what each answers are argued.
        if(copy.owner) base[copy.owner]->uses++;
        if(copy.index) base[copy.index]->uses++;
        return addPlace(copy);
    }

    /*
     * `width` bits at `offset` of the number `word` holds, read at `content` - a field of a packed
     * record or of a `$pN` word.
     *
     * **Nested ranges compose**: a range of a range is a range of the outer word at the summed
     * offset, so `pair.flags.optionA` of a scalarized `Pair` whose `flags` is a scalarized `Flags` is
     * one place over `pair`'s word rather than two, and the verifier can hold every `BitRange` to
     * "`word` is not one". The word's owner, index and key are copied - see `JsPlaceKind::BitRange`
     * for why - and the owner is named a second time, which `JsValue::uses` may over-count.
     */
    JsPlaceId addBitRange(JsPlaceId word, U32 offset, U32 width, bool isSigned, JsRepId content) {
        JsPlace outer = place(word);
        JsirPtr<JsValue> scale = nullptr;
        if(outer.kind == JsPlaceKind::BitRange) {
            offset += outer.bitOffset;
            scale = outer.bitScale;
            word = outer.word;
            outer = place(word);
        }

        JsPlace range = outer;
        range.kind = JsPlaceKind::BitRange;
        range.content = content;
        range.nichePayload = false;
        range.word = word;
        range.bitOffset = U8(offset);
        range.bitWidth = U8(width);
        range.bitSigned = isSigned;

        // A field of a range a reference carried keeps the reference's scale and is no longer the
        // whole of it - see `JsPlace::bitWhole`.
        range.bitScale = scale;
        range.bitWhole = false;

        if(range.owner) base[range.owner]->uses++;
        if(range.index) base[range.index]->uses++;
        if(range.bitScale) base[range.bitScale]->uses++;
        return addPlace(range);
    }

    /*
     * The storage a **three-part narrow reference** names - `(owner, key, scale)` - as a range of
     * `owner[key]` at a scale known only at run time. `content` is the reference's content; the
     * word is read as a float because nothing here knows how wide it is (see `JsPlace::bitScale`).
     */
    JsPlaceId addScaledRange(JsValue* owner, JsValue* key, JsValue* scale, JsRepId content,
                             U32 width, bool isSigned) {
        auto word = addElement(owner, key, jsNumberRep(target, NumberDomain::Float64, 64));

        JsPlace range = place(word);
        range.kind = JsPlaceKind::BitRange;
        range.content = content;
        range.word = word;
        range.bitOffset = 0;
        range.bitWidth = U8(width);
        range.bitSigned = isSigned;
        range.bitScale = scale - base;
        range.bitWhole = true;

        owner->uses++;
        key->uses++;
        scale->uses++;
        return addPlace(range);
    }

    /*
     * A packed word built from its fields - `JsTarget::packings[packing]`, one value per field in
     * that row's order.
     *
     * **Folded** where every operand is a numeric constant, which is what makes a constant record
     * a constant here as it is natively: `Flags {a: True, b: False}` is the literal `1`, and a
     * `zeroOf` a packed record is the literal `0` without asking. Each field is reduced to its own
     * `width` bits first, so a signed field contributes its pattern rather than its value - the same
     * reduction `codegen/js/bits.cpp`'s `encodeRange` makes at run time.
     */
    JsValue* pack(U32 packing, Buffer<JsValue*> values, LocationId source) {
        auto& row = target.packings[packing];

        auto constant = values.size() == row.fields.size();
        F64 folded = 0;
        for(U32 i = 0; constant && i < values.size(); i++) {
            auto value = values[i];
            auto definer = value && value->definer ? base[value->definer] : nullptr;
            if(!definer || definer->kind != JsInst::Const) { constant = false; break; }

            auto& literal = ((JsInstConst*)definer)->constant;
            if(literal.kind != JsConstant::Number || !literal.integral) { constant = false; break; }

            folded += F64(jsFieldPattern(literal.number, row.fields[i].width)) *
                      F64(U64(1) << row.fields[i].offset);
        }

        if(constant) return number(folded, row.word, source);

        auto inst = add<JsInstPack>(row.word, source, packing);
        for(auto value: values) {
            inst->values.push(target.arena, value - base);
            value->uses++;
        }

        return base[inst->result];
    }

    JsValue* constant(const JsConstant& value, JsRepId rep, LocationId source) {
        auto inst = add<JsInstConst>(rep, source, value);
        return base[inst->result];
    }

    /*
     * A small integer at whatever representation the position holds it in - a `number` or a
     * `bigint`.
     *
     * `number` below is the one that does not ask, because most callers are building an index or a
     * lane count and know the domain. This is for the ones whose width is the *operand's*: a 64-bit
     * vector lane is a `bigint`, and `lanes - 1` written as a `number` beside one is the host
     * refusing to mix the two rather than a wrong answer.
     */
    JsValue* integer(U64 value, JsRepId rep, LocationId source) {
        if(rep.valid() && target.rep(rep).kind == JsRepKind::BigInt) {
            JsConstant wide;
            wide.kind = JsConstant::BigInt;
            wide.bits = value;
            wide.isSigned = target.rep(rep).isSigned();
            return constant(wide, rep, source);
        }

        return number(F64(value), rep, source);
    }

    JsValue* number(F64 value, JsRepId rep, LocationId source) {
        JsConstant constant;
        constant.kind = JsConstant::Number;
        constant.number = value;
        return this->constant(constant, rep, source);
    }

    JsValue* boolean(bool value, LocationId source) {
        JsConstant constant;
        constant.kind = JsConstant::Bool;
        constant.number = value ? 1 : 0;
        return this->constant(constant, jsHostBoolRep(target), source);
    }

    /*
     * N parts as the one host value a position can hold - §3.6's `parts.materialize`.
     *
     * Answers null where the representation has no materialized form, which is the one failure this
     * cannot paper over: there is no value to produce and `null` is what §3.4 forbids. The caller
     * reports, because it is the one with a position to name.
     */
    JsValue* materialize(JsRepId parts, Buffer<JsValue*> values, LocationId source) {
        auto stored = jsStoredRep(target, parts);
        if(stored == parts || !stored.valid()) return nullptr;

        auto inst = add<JsInstMaterialize>(stored, source, parts);
        for(auto value: values) {
            inst->values.push(target.arena, value - base);
            value->uses++;
        }

        return base[inst->result];
    }

    // One part back out of a materialized value - §3.6's `parts.split`.
    JsValue* splitPart(JsValue* from, JsRepId parts, U16 part, LocationId source) {
        auto rep = partRepAt(target, parts, part);
        if(!rep.valid()) return nullptr;

        auto inst = add<JsInstSplit>(rep, source, from - base, parts, part);
        return base[inst->result];
    }

    JsValue* load(JsPlaceId from, LocationId source) {
        auto& p = place(from);

        // Reading storage that *is* an object is the object. A node would be a node every consumer
        // has to see through, and §5.4's "build useful form directly" is exactly about not emitting
        // one to trust an optimizer to remove it.
        if(p.kind == JsPlaceKind::Value) {
            auto owner = base[p.owner];

            /*
             * The **payload of a niche-folded value** whose storage is the value: the same host
             * value read at the payload's representation, which is an `Alias` and emits nothing.
             *
             * Without it the shortcut above hands back the object at the *folded* representation -
             * a `Maybe(I32)` where the reader was promised the `I32` - and a `match` arm then
             * carried the wrong representation across its merge. Which is the verifier working:
             * `valueOf(m: Maybe(Int))` is where it arrived.
             */
            if(p.nichePayload && owner->rep != p.content) {
                auto alias = add<JsInstAlias>(p.content, source, owner - base);
                return base[alias->result];
            }

            owner->uses++;
            return owner;
        }

        auto inst = add<JsInstLoad>(p.content, source, from);
        return base[inst->result];
    }

    void store(JsPlaceId into, JsValue* value, LocationId source) {
        add<JsInstStore>(JsRepId(), source, into, value - base);
    }

    // The edge a terminator carries, with the block's incoming list updated in the same call - the
    // half of the statement a caller cannot be trusted to write separately.
    void connect(JsEdge& edge, JsBlock* from, JsBlock* to) {
        edge.target = to - base;
        to->incomingList.push(target.arena, from - base);
    }

    void edgeArgument(JsEdge& edge, JsValue* value) {
        edge.arguments.push(target.arena, value - base);
        value->uses++;
    }
};

} // namespace jsir
