#include "convert_internal.h"

/*
 * The two teardown **bodies** this tier synthesizes - `runEnvironmentTeardown` and
 * `runTypeTeardown`.
 *
 * Neither is a body the program wrote. A lambda's environment is released through one function per
 * environment record, and a sum is released through one `TypeDrop` per (type, halves) - because its
 * walk branches on the tag and a branch is a CFG change in the middle of a block the caller is
 * filling. `codegen/js/assemble.cpp` asks for one by handle and emits what comes back.
 *
 * Which arm a type's teardown is, and which halves its members inherit, is **not decided here**:
 * `jsTeardownStep` in `repr.cpp` is the one owner, and `jsOwesTeardown` walks it for its answer
 * while `emitTeardown` walks it for its effect. The two were transcripts of one shape and had
 * already drifted in two places (J79 0.2).
 */

namespace jsir {

/*
 * A lambda's environment teardown - one block, one parameter, `emitTeardown` and a `return`.
 *
 * The parameter is a `Value` place over the argument rather than a cell, which is what the
 * declaration says it is: the member takes `->value: T`, so what arrives is the record itself and
 * there is nothing to rebind.
 */
bool Conversion::runEnvironmentTeardown(DeclPtr<mir::Type> environment, LocationId where,
                                        const JsPassing& passing) {
    auto entry = builder.addBlock(StringId());
    body.entry = entry - base();
    builder.switchTo(entry);

    auto argument = builder.addArgument(entry, carriedRepOf(target, passing));
    body.parameters.push(target.arena, argument - base());

    /*
     * The parameter at the **lambda's own** parameter-zero plan, which is what `$fd` hands it.
     *
     * The pair's second word is one value and both readers of it - the lifted body and this - have
     * to agree about which. A `Cell` is the descriptor the body reads through; anything else is the
     * record itself, and the place over it is the value.
     */
    auto place = passing.kind == JsPassingKind::Cell
        ? builder.addCell(argument, passing.rep)
        : builder.addValuePlace(argument, passing.rep);

    emitTeardown(place, environment, true, true, where);
    builder.add<JsInstReturn>(JsRepId(), where);
    return !failed;
}

/*
 * A sum's teardown, as a body of its own - the tag read once, one block per arm that owes anything,
 * and one `return`.
 *
 * `emitTeardown` is what fills each arm, so a nested sum inside one becomes another call rather
 * than another branch here, and a record or a fixed array is the same walk a field of it gets. What
 * this adds over the caller's inline form is exactly the branch, which is why it is the one shape
 * that has a function.
 *
 * The arms are the *constructors*, not the properties: a constructor with nothing to release gets no
 * block and no case at all, so `Maybe(Int)` - whose `Just` owes nothing - would never reach here in
 * the first place (`jsOwesTeardown` is what the caller asked) and `Maybe(Handle)` has one case.
 */
bool Conversion::runTypeTeardown(DeclPtr<mir::Type> type, bool runDrop, bool runReclaim,
                                 LocationId where, const JsPassing& passing) {
    auto entry = builder.addBlock(StringId());
    body.entry = entry - base();
    builder.switchTo(entry);

    auto argument = builder.addArgument(entry, carriedRepOf(target, passing));
    body.parameters.push(target.arena, argument - base());

    auto place = passing.kind == JsPassingKind::Cell
        ? builder.addCell(argument, passing.rep)
        : builder.addValuePlace(argument, passing.rep);

    auto step = jsTeardownStep(target, type, runDrop, runReclaim);

    /*
     * Anything that is **not** a sum is the ordinary inline walk, written into a body of its own.
     *
     * The sum is the one shape that needs a function - its derived walk branches on the tag - and
     * this used to be the only caller, so the two were one. A type *descriptor*'s teardown cell is
     * the second, and it needs a body for every type an erased argument list may bind, most of which
     * are not sums. `emitTeardown` cannot be called for a sum here: it would redirect straight back
     * to a call of this function.
     */
    if(step.kind != JsTeardownKind::Sum) {
        if(!emitTeardown(place, type, runDrop, runReclaim, where)) return false;

        builder.add<JsInstReturn>(JsRepId(), where);
        return !failed;
    }

    auto& record = *(mir::RecordType*)decl[type];

    // The authored halves first, exactly as the inline walk runs them, and then the derived one.
    if(!emitAuthored(place, type, step, where)) return false;

    auto childDrop = step.childDrop;
    auto childReclaim = step.childReclaim;

    auto rep = jsManifest(target, mir::owned(type), where).rep;

    /*
     * A **niche-folded** sum, whose branch is the absence test and whose one arm is the payload -
     * §3.9's "a teardown of a nullable representation is guarded by the representation's absence
     * test", said at the one place a folded value's release is written.
     *
     * The same function-per-type the tagged form uses, for the same reason: the guard is a CFG
     * change and the caller is in the middle of a block. It is also what a *descriptor*'s teardown
     * cell points at, so an erased position holding a folded value releases it correctly without
     * the erased body knowing anything about the encoding.
     */
    if(auto niche = nicheOf(rep)) {
        if(!childDrop && !childReclaim) {
            builder.add<JsInstReturn>(JsRepId(), where);
            return !failed;
        }

        auto value = builder.load(place, where);
        if(!value) {
            internal("met a teardown of a folded sum with no value"_v, where);
            return false;
        }

        auto payload = record.fields.get(decl, niche->payloadField);
        auto done = builder.addBlock(StringId());
        auto arm = builder.addBlock(StringId());

        builder.switchTo(arm);
        if(mir::tearsDownField(decl, payload)) {
            auto held = builder.addNichePayload(place, niche->content);
            if(!emitTeardown(held, payload.occurrence.type, childDrop, childReclaim, where, 1)) {
                return false;
            }
        }

        auto jump = builder.add<JsInstJump>(JsRepId(), where);
        builder.connect(jump->to, arm, done);

        builder.switchTo(entry);
        auto test = isAbsent(value, rep, where);
        auto branch = builder.add<JsInstBranch>(JsRepId(), where, test - base());
        builder.connect(branch->whenTrue, entry, done);
        builder.connect(branch->whenFalse, entry, arm);

        builder.switchTo(done);
        builder.add<JsInstReturn>(JsRepId(), where);
        return !failed;
    }

    auto& shape = target.shape(target.rep(rep).shape);
    auto tagSlot = shape.tagSlot();

    if(!childDrop && !childReclaim || tagSlot == JsProperty::kNoField) {
        builder.add<JsInstReturn>(JsRepId(), where);
        return !failed;
    }

    auto owner = builder.load(place, where);
    auto tag = builder.load(builder.addProperty(owner, target.rep(rep).shape, tagSlot), where);

    auto done = builder.addBlock(StringId());

    // The arms, built before the terminator so that each one's own instructions go in its own
    // block - the switch is attached once every target exists, which is `collectEdge`'s rule for
    // the same reason.
    SmallArray<JsBlock*, 8> arms;
    SmallArray<F64, 8> values;

    for(U16 i = 0; i < record.cons.size(); i++) {
        auto arm = record.cons.get(decl, i);

        auto owes = false;
        for(U16 f = 0; f < arm.count; f++) {
            auto field = record.fields.get(decl, U16(arm.first + f));
            if(mir::tearsDownField(decl, field) &&
               jsOwesTeardown(target, field.occurrence.type)) {
                owes = true;
            }
        }

        if(!owes) continue;

        auto block = builder.addBlock(StringId());
        builder.switchTo(block);

        for(U16 f = 0; f < arm.count; f++) {
            auto field = record.fields.get(decl, U16(arm.first + f));
            if(!mir::tearsDownField(decl, field)) continue;

            // A `@box` field holds its content in the property itself - the box is the collector's
            // on this target, which is what `fieldRepOf` says by not distinguishing one - so what
            // a teardown owes through it is the content's, reached the way an unboxed field's is.
            // It was refused by name until J136, whatever the content owed; `Box.yana`'s
            // `Diagnostics` owes nothing, and its record's teardown was never going to reach it.

            auto slot = shape.slotOfField(U16(arm.first + f));
            if(slot == JsProperty::kNoField) continue;

            auto held = builder.addProperty(owner, target.rep(rep).shape, slot);
            if(!emitTeardown(held, field.occurrence.type, childDrop, childReclaim, where, 1)) {
                return false;
            }
        }

        auto jump = builder.add<JsInstJump>(JsRepId(), where);
        builder.connect(jump->to, block, done);

        arms.push(block);
        values.push(F64(arm.value));
    }

    builder.switchTo(entry);
    auto node = builder.add<JsInstSwitch>(JsRepId(), where, tag - base());
    tag->uses++;

    for(U32 i = 0; i < arms.size(); i++) node->cases.push(target.arena, JsSwitchCase { values[i] });

    auto items = node->cases.pointer(base());
    for(U32 i = 0; i < arms.size(); i++) builder.connect(items[i].edge, entry, arms[i]);

    builder.connect(node->fallback, entry, done);

    builder.switchTo(done);
    builder.add<JsInstReturn>(JsRepId(), where);
    return !failed;
}

} // namespace jsir
