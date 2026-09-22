#include "verify.h"
#include "cfg.h"

/*
 * See verify.h. The checks are grouped the way part 4 groups them - construction, finished CFG,
 * representation - and each one names what it found rather than answering a bare false, because the
 * consumer of a failure here is somebody reading a diagnostic and not a branch.
 */

namespace jsir {

namespace {

struct Verifier {
    Verifier(JsTarget& target, JsBody& body):
        target(target), body(body), base(*target.arena) {}

    JsTarget& target;
    JsBody& body;
    JsirBase base;

    bool ok = true;

    // What failed and where - the mnemonic included, because a verifier report is read by somebody
    // looking for the instruction and an invariant without one names a body rather than a node.
    JsInst* at = nullptr;

    void fail(StringView what, LocationId where) {
        ok = false;
        target.reported = true;

        if(at) {
            target.context.diagnostics.error("internal: JSIR verification failed at `%@` - %@"_v,
                                             where, jsInstMnemonic(at->kind), what);
            return;
        }

        target.context.diagnostics.error("internal: JSIR verification failed - %@"_v, where, what);
    }

    StringView functionName() {
        return body.function ? stringView(target.context.findName(target.decl[body.function]->name))
                             : "?"_v;
    }

    // ------------------------------------------------------------- dominance

    /*
     * The dominator tree, which is `jsir::JsCfg`'s since 2026-09-22 and was this file's own walk.
     *
     * Three copies of one graph is what there was - this one, `opt_alias.cpp`'s and the structurer's
     * - and this is the one that ran twice per body, since an assertion build verifies what the
     * conversion produced and what the optimizer returned. `jsCfgOf` computes it once.
     */
    const JsCfg* flow = nullptr;

    void computeDominance() { flow = &jsCfgOf(target, body); }

    bool dominates(U32 owner, U32 block) {
        // A block the entry cannot reach dominates nothing and is dominated by everything: what
        // reaches it is not a path, so an operand check over one would report a body the emitter
        // never walks.
        if(!flow->reachable[block]) return true;
        return flow->dominates(owner, block);
    }

    // ------------------------------------------------------------- checks

    void checkOperand(JsInst& inst, JsirPtr<JsValue> used) {
        if(!used) return fail("an instruction with a null operand"_v, inst.source);

        auto value = base[used];

        // `Void` is not a value. An AST expression may write `null` where the syntax needs one, but
        // that is a spelling and this is the boundary that keeps the two apart.
        if(target.rep(value->rep).kind == JsRepKind::Void) {
            fail("a `void` value used as an operand"_v, inst.source);
        }

        /*
         * A value carried as several may not be used as one without an explicit `materialize`.
         *
         * No JSIR value ever *has* a `Parts` representation: the parts are ordinary values of the
         * part representations, and `materialize` answers the materialized form. So this is the
         * check that says so - §3.4's "asking for one value inserts `materialize` or fails", read as
         * an invariant about what may reach an operand at all - and it is what the mutation suite
         * breaks to prove the boundary is real.
         */
        if(target.rep(value->rep).kind == JsRepKind::Parts) {
            fail("a multi-part value used as one value without a materialization"_v, inst.source);
        }

        if(!value->definer && !value->block) {
            fail("an operand with no definition"_v, inst.source);
        }

        if(value->definer) {
            auto definer = base[value->definer];
            if(!definer->block) return fail("an operand defined outside any block"_v, inst.source);
            if(!inst.block) return fail("an instruction outside any block"_v, inst.source);

            auto from = base[definer->block]->index.index;
            auto at = base[inst.block]->index.index;
            if(!dominates(from, at)) {
                fail("an operand whose definition does not dominate its use"_v, inst.source);
            }
        }
    }

    void checkPlace(JsInst& inst, JsPlaceId id) {
        if(!id.valid() || id.index >= body.places.size()) {
            return fail("an access through a place that does not exist"_v, inst.source);
        }

        auto& place = *body.place(base, id);

        /*
         * Storage holds **one** host value, whatever the value's logical type says.
         *
         * `jsStoredRep` is the rule and this is the check: a place whose content were the `Parts`
         * representation would be one a `load` cannot read, since no single value has that
         * representation. Stated once here rather than at every builder, so that the next multi-part
         * family inherits it.
         */
        if(place.content.valid() && target.rep(place.content).kind == JsRepKind::Parts) {
            fail("a place whose content is a multi-part value rather than one host value"_v,
                 inst.source);
        }

        switch(place.kind) {
            case JsPlaceKind::Var:
                if(place.slot >= body.variables.size()) {
                    fail("a variable place naming a slot that does not exist"_v, inst.source);
                }

                return;

            case JsPlaceKind::Global:
                if(!place.global) fail("a global place naming no global"_v, inst.source);
                return;

            case JsPlaceKind::Cell: {
                if(!place.owner) return fail("a cell place with no box"_v, inst.source);

                auto& owner = target.rep(base[place.owner]->rep);
                if(owner.kind != JsRepKind::Cell) {
                    fail("a cell place whose box is not a cell"_v, inst.source);
                }

                return;
            }

            case JsPlaceKind::Value:
                if(!place.owner) fail("a value place with no value"_v, inst.source);
                return;

            case JsPlaceKind::Element: {
                if(!place.owner || !place.index) {
                    return fail("an element place with no owner or index"_v, inst.source);
                }

                auto& owner = target.rep(base[place.owner]->rep);
                if(owner.kind != JsRepKind::HostArray && owner.kind != JsRepKind::Erased) {
                    fail("an element read out of something that is not a host array"_v, inst.source);
                }

                return;
            }

            case JsPlaceKind::Property: {
                if(!place.owner) return fail("a property place with no owner"_v, inst.source);

                auto& shape = target.shape(place.shape);
                auto& owner = target.rep(base[place.owner]->rep);

                /*
                 * The check part 4.3 calls the most valuable new boundary: a field access cannot
                 * target a number, a bigint, a host string or a nullable.
                 *
                 * A `Host` shape is where the owner is legitimately not one of this tier's objects -
                 * `Array(Handle)` is the bare host row and `length` is a property that row keeps
                 * itself. The exception is the shape's kind rather than a relaxation of the check,
                 * so everything the compiler laid out is still held to its own hidden class.
                 */
                if(shape.kind == JsShapeKind::Host) {
                    if(!owner.isObjectLike() && owner.kind != JsRepKind::HostString &&
                       owner.kind != JsRepKind::Erased) {
                        fail("a host property read out of something that has none"_v, inst.source);
                    }
                } else if(owner.kind != JsRepKind::GcObject) {
                    return fail("a property read out of something that is not a host object"_v,
                                inst.source);
                } else if(owner.shape != place.shape) {
                    return fail("a property place whose shape is not its owner's"_v, inst.source);
                }

                if(place.slot >= shape.properties.size()) {
                    return fail("a property place naming a slot the shape does not have"_v,
                                inst.source);
                }

                /*
                 * The one licence the property check grants, and it is a licence for exactly one
                 * mismatch: a place over the **payload of a niche-folded value** names the same
                 * property and reads it at the payload's representation, because `Just(x)` *is* `x`
                 * on this target. Anything else that disagrees with the shape is a value built one
                 * way and read the other.
                 */
                auto held = shape.properties[place.slot].rep;
                if(place.nichePayload) {
                    auto& folded = target.rep(held);
                    if(folded.kind != JsRepKind::Nullable || folded.content != place.content) {
                        fail("a niche payload place whose property is not that fold"_v, inst.source);
                    }

                    return;
                }

                if(held != place.content) {
                    fail("a property place whose content is not the shape's"_v, inst.source);
                }

                return;
            }
        }
    }

    /*
     * §3.2's nesting rule - "nesting is legal only when the inner rep does not already admit the
     * same absent pattern".
     *
     * A `Nullable` whose payload can itself be `null` is one `null` standing for two values, so
     * `Maybe(Maybe(T))` keeps its `$tag` over a folded inner one. Asked of a value's representation
     * rather than of the whole table, because a representation that reaches no value decides
     * nothing - and asking it here is one dense load per instruction rather than a walk.
     */
    void checkNiche(JsRepId id, LocationId where) {
        if(!id.valid() || target.rep(id).kind != JsRepKind::Nullable) return;

        if(jsAdmitsAbsent(target, target.rep(id).content)) {
            fail("a niche fold over a payload that is already absent-valued"_v, where);
        }
    }

    void checkInst(JsInst& inst) {
        at = &inst;
        forEachUse(base, inst, [&](JsirPtr<JsValue> used) { checkOperand(inst, used); });

        if(jsInstResult(inst.kind) == kOneResult && !inst.result) {
            fail("an instruction whose row declares a result and has none"_v, inst.source);
        }

        if(jsInstResult(inst.kind) == kNoResult && inst.result) {
            fail("an instruction with a result its row does not declare"_v, inst.source);
        }

        if(inst.result) checkNiche(base[inst.result]->rep, inst.source);

        switch(inst.kind) {
            /*
             * `null` is the **absent niche** and is not a value of anything else - §3.2's rule read
             * at the one node that writes one.
             *
             * What it refuses is a `null` where a number, a bigint or a string was promised - the
             * shape §1.2 records as a shipping-backend defect, which reads back as the wrong value
             * everywhere instead of throwing where it was written. A host object's `null` is legal
             * and `jsNullIsAValue` is where the two reasons are written.
             */
            case JsInst::Const: {
                auto& written = (JsInstConst&)inst;
                if(written.constant.kind != JsConstant::Null || !inst.result) return;

                if(!jsNullIsAValue(target, base[inst.result]->rep)) {
                    fail("a null at a representation that has no absent value"_v, inst.source);
                }

                return;
            }

            case JsInst::Load: {
                auto& written = (JsInstLoad&)inst;
                checkPlace(inst, written.place);

                if(written.place.valid() && written.place.index < body.places.size() && inst.result) {
                    auto& place = *body.place(base, written.place);
                    if(place.kind != JsPlaceKind::Value &&
                       place.content != base[inst.result]->rep) {
                        fail("a load whose result is not the place's content"_v, inst.source);
                    }
                }

                return;
            }

            case JsInst::Store: {
                auto& written = (JsInstStore&)inst;
                checkPlace(inst, written.place);

                if(written.place.valid() && written.place.index < body.places.size() &&
                   written.value) {
                    auto& place = *body.place(base, written.place);
                    if(place.content != base[written.value]->rep) {
                        fail("a store whose value is not the place's content"_v, inst.source);
                    }
                }

                return;
            }

            /*
             * An object construction supplies every property of its shape, once and in order.
             *
             * Construction order is this target's field offsets, so this is the check that keeps one
             * record to one hidden class - and a missing property is exactly the shape that reads
             * back as `undefined` at some unrelated place later.
             */
            case JsInst::Object: {
                auto& written = (JsInstObject&)inst;
                auto& shape = target.shape(written.shape);

                if(written.values.size() != shape.properties.size()) {
                    return fail("an object construction that does not supply every property"_v,
                                inst.source);
                }

                for(U32 i = 0; i < shape.properties.size(); i++) {
                    auto supplied = written.values.get(base, i);
                    if(!supplied) continue;

                    // An `Erased` property accepts anything: what it holds is a host value whose
                    // shape this tier does not describe - a reference's owner and key, a function
                    // pair's two words. That is the whole of what the kind means.
                    if(target.rep(shape.properties[i].rep).kind == JsRepKind::Erased) continue;

                    /*
                     * Named, because the shape is the thing to look at and "a property" names a
                     * whole record. Three of J67's five fixtures were one property of one shape and
                     * the bare sentence said which body rather than which field.
                     *
                     * And **the types the two representations were computed from**, which J70 added
                     * for the reason the property name was added: `object(3)` against `object(7)`
                     * says two shapes disagree and `Array(I32)` against `ArrayView(I32)` says which
                     * two, which is the whole diagnosis of J67 §5 read straight off the message.
                     * The record is named the same way, so the sentence reaches the declaration that
                     * has to change without a debugger.
                     */
                    if(base[supplied]->rep != shape.properties[i].rep) {
                        auto& want = target.rep(shape.properties[i].rep);
                        auto& got = target.rep(base[supplied]->rep);

                        target.reported = true;
                        ok = false;
                        target.context.diagnostics.error(
                            "internal: JSIR verification failed at `object` - the property %@ of %@ "
                            "wants %@(%@) and was supplied %@(%@)"_v, inst.source,
                            stringView(target.context.findName(shape.properties[i].key)),
                            mir::typeText(target.context, target.decl, shape.source),
                            jsRepKindName(want.kind),
                            mir::typeText(target.context, target.decl, want.source),
                            jsRepKindName(got.kind),
                            mir::typeText(target.context, target.decl, got.source));
                    }
                }

                return;
            }

            /*
             * What is returned is what this function's own plan promised.
             *
             * The half of §3.7 the call check cannot make: a caller is held to the callee's plan,
             * and this is the callee held to its own. Both ends read one `JsAbi`, so a body that
             * answered something else is a disagreement no golden and no caller would report -
             * `loans/Origins.yana` returned a `{$o, $k}` descriptor where the signature says the
             * number it names, and the program answered a plausible wrong value.
             */
            case JsInst::Return: {
                auto& written = (JsInstReturn&)inst;
                if(!body.abi.valid() || body.abi.index >= target.abis.size()) return;

                auto& abi = target.abi(body.abi);
                if(abi.result.kind == JsPassingKind::Omitted) {
                    if(written.value) fail("a return of a value its plan omits"_v, inst.source);
                    return;
                }

                if(!written.value) return fail("a return of nothing where its plan declares a value"_v,
                                               inst.source);

                auto held = base[written.value]->rep;
                if(held != carriedRepOf(target, abi.result) &&
                   target.rep(abi.result.rep).kind != JsRepKind::Erased &&
                   target.rep(held).kind != JsRepKind::Erased) {
                    fail("a return whose representation is not its own plan's"_v, inst.source);
                }

                return;
            }

            /*
             * A materialization names a multi-part representation that **has** a single-value form,
             * and supplies exactly its parts, in order and at their own representations.
             *
             * §3.2 says "some parts never have a materialized form; a verifier rejects an attempt to
             * build one", and this is that rejection. Nothing this slice produces is in that state -
             * a vector's form is the lane row and a pair's is the two-property object - so the check
             * is what keeps the next family honest rather than something the corpus exercises.
             */
            case JsInst::Materialize: {
                auto& written = (JsInstMaterialize&)inst;
                auto& parts = target.rep(written.parts);

                if(parts.kind != JsRepKind::Parts) {
                    return fail("a materialization of a representation that has no parts"_v,
                                inst.source);
                }

                if(!parts.content.valid()) {
                    return fail("a materialization of parts that have no single-value form"_v,
                                inst.source);
                }

                if(inst.result && base[inst.result]->rep != parts.content) {
                    fail("a materialization whose result is not its parts' single-value form"_v,
                         inst.source);
                }

                if(written.values.size() != parts.partCount) {
                    return fail("a materialization that does not supply every part"_v, inst.source);
                }

                for(U16 i = 0; i < parts.partCount; i++) {
                    auto supplied = written.values.get(base, i);
                    if(!supplied) continue;

                    if(base[supplied]->rep != target.partList[parts.firstPart + i]) {
                        fail("a materialization supplied a part at the wrong representation"_v,
                             inst.source);
                    }
                }

                return;
            }

            // And the other direction: what is split has to be the materialized form of the
            // representation the node names, and the part has to be one that representation has.
            case JsInst::Split: {
                auto& written = (JsInstSplit&)inst;
                auto& parts = target.rep(written.parts);

                if(parts.kind != JsRepKind::Parts) {
                    return fail("a split of a representation that has no parts"_v, inst.source);
                }

                if(written.part >= parts.partCount) {
                    return fail("a split naming a part the representation does not have"_v,
                                inst.source);
                }

                // The materialization check's own row, said from this side: parts with no
                // single-value form have nothing a split could be reading, and the sentence below
                // reads that form's representation.
                if(!parts.content.valid()) {
                    return fail("a split of parts that have no single-value form"_v, inst.source);
                }

                /*
                 * Named with both representations and the types they were computed from, for the
                 * reason J70 gave the `object` check above the same treatment: `%r151` against
                 * `%r153` says two multi-part families disagree and `ArrayView(String)` against
                 * `Array(String)` says which two, which is the whole diagnosis off the message.
                 * J78's defect was exactly this pair and cost a dump to read.
                 */
                if(written.from && base[written.from]->rep != parts.content) {
                    auto& want = target.rep(parts.content);
                    auto& got = target.rep(base[written.from]->rep);

                    target.reported = true;
                    ok = false;
                    target.context.diagnostics.error(
                        "internal: JSIR verification failed at `split` - a split of something that "
                        "is not its parts' single-value form: %@ wants %@(%@) and was given "
                        "%@(%@)"_v, inst.source,
                        mir::typeText(target.context, target.decl, target.rep(written.parts).source),
                        jsRepKindName(want.kind),
                        mir::typeText(target.context, target.decl, want.source),
                        jsRepKindName(got.kind),
                        mir::typeText(target.context, target.decl, got.source));
                }

                if(inst.result &&
                   base[inst.result]->rep != target.partList[parts.firstPart + written.part]) {
                    fail("a split whose result is not the part's representation"_v, inst.source);
                }

                return;
            }

            case JsInst::Call: {
                auto& written = (JsInstCall&)inst;

                /*
                 * A body compiled once is entered with its caller's table and nothing else is -
                 * `JsAbi::takesGenEnv`, held from both sides.
                 *
                 * The two halves of a disagreement here are a call that shifts every argument by
                 * one and a body that reads a slot out of its first parameter. Neither shows up as
                 * anything but a wrong number, which is what §4.1 exists to stop.
                 */
                if(written.abi.valid() && written.abi.index < target.abis.size()) {
                    auto takes = target.abi(written.abi).takesGenEnv;
                    if(takes != bool(written.genEnv)) {
                        fail("a call that disagrees with its callee about the generic "
                             "environment"_v, inst.source);
                    }

                    if(written.genEnv &&
                       target.rep(base[written.genEnv]->rep).kind != JsRepKind::HostArray) {
                        fail("a generic environment that is not a table"_v, inst.source);
                    }
                }

                checkCall(inst, written.abi, written.arguments.size(), written.arguments, false);
                return;
            }

            case JsInst::CallValue: {
                auto& written = (JsInstCallValue&)inst;
                checkCall(inst, written.abi, written.arguments.size(), written.arguments, true);
                return;
            }

            default:
                return;
        }
    }

    /*
     * Caller and callee agree - §3.7's "a verifier property instead of two predicates which can
     * quietly agree on the same wrong answer".
     *
     * The arity first, because that is the failure that shifts every argument after it, and then the
     * representation of each position against the frozen plan.
     */
    void checkCall(JsInst& inst, JsAbiId abiId, U32 count,
                   JsirList<JsirPtr<JsValue>, false>& arguments, bool indirect) {
        if(!abiId.valid() || abiId.index >= target.abis.size()) {
            return fail("a call with no calling convention"_v, inst.source);
        }

        auto& abi = target.abi(abiId);

        if(count != positionCount(abi)) {
            return fail("a call whose arity disagrees with its callee's convention"_v, inst.source);
        }

        forEachPosition(abi, [&](U32 position, U32, const JsPassing& parameter, U16 part) {
            auto supplied = arguments.get(base, position);
            if(!supplied) return;

            auto held = base[supplied]->rep;

            /*
             * A multi-part parameter is `positions` arguments, each held to its **own** part's
             * representation - §3.8's edge rule, read at a call.
             *
             * The arity check above already caught a missing one, which is the failure that shifts
             * every argument after it; this is what catches the lanes arriving in the wrong order
             * or at the wrong width, which nothing downstream would report.
             */
            if(parameter.kind == JsPassingKind::Parts) {
                // An `Erased` part accepts anything, which is the same waiver an `Erased` shape
                // property gets and for the same reason: a flat reference's owner is any host
                // object and its key is a string or a number, so the part can only state that one
                // host value goes there. What is still checked is the *position* - two parts, in
                // order, at the plan the callee was compiled against.
                auto wantedPart = partRepAt(target, parameter.rep, part);
                if(target.rep(wantedPart).kind == JsRepKind::Erased) return;

                if(held != wantedPart) {
                    fail("a call argument whose representation disagrees with its callee's "
                         "convention"_v, inst.source);
                }

                return;
            }

            /*
             * Exactly the plan's representation, and the one waiver is the **declaration's**:
             * `JsPassing::erased` says the signature wrote a type its own context binds there, which
             * is the one position whose two ends are two interned representations of one thing.
             *
             * It used to be "either side happens to be `Erased`", which is a waiver keyed on what a
             * rep turned out to be rather than on what the declaration promised - so a genuinely
             * mismatched pair went through whenever one of them was a type variable's.
             *
             * And the waiver is about the **content**, not about the convention - J70. A body
             * compiled once reads a `Cell` position as `x.$o[x.$k]` whatever is in it, so an erased
             * `&a` supplied the host object itself (which is what a `Value` place answers when it is
             * asked for its own reference) is a descriptor the callee will index into and find
             * nothing at. What that needs is a caller whose storage for the value is a box, which is
             * §6.4's; until then it is a report rather than a `undefined[undefined]`.
             */
            auto wanted = parameter.kind == JsPassingKind::Cell
                ? target.rep(held).kind == JsRepKind::Cell &&
                  (parameter.erased || target.rep(held).content == parameter.rep)
                : parameter.erased || held == parameter.rep;

            if(!wanted) {
                fail("a call argument whose representation disagrees with its callee's convention"_v,
                     inst.source);
            }
        });

        if(inst.result) {
            if(abi.result.kind == JsPassingKind::Omitted) {
                fail("a call with a result its callee does not return"_v, inst.source);
            } else if(base[inst.result]->rep != carriedRepOf(target, abi.result) &&
                      !abi.result.erased) {
                fail("a call result whose representation disagrees with its callee's"_v, inst.source);
            }
        }

        (void)indirect;
    }

    /*
     * Every predecessor operand matches the block's arguments, exactly - §3.8.
     *
     * This is what catches a loop-carried value whose representation changed on one edge, before
     * structuring turns the argument into a `var` that could hold any JavaScript value at all.
     */
    void checkEdges() {
        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = base[body.blocks.get(base, i)];
            auto terminator = block->terminator(base);
            if(!terminator) {
                fail("a block with no terminator"_v, kNullLocation);
                continue;
            }

            forEachEdge(base, *terminator, [&](JsEdge& edge) {
                if(!edge.target) return fail("an edge with no target"_v, terminator->source);

                auto to = base[edge.target];
                if(edge.arguments.size() != to->argumentCount()) {
                    return fail("an edge whose arity disagrees with its target's arguments"_v,
                                terminator->source);
                }

                for(U32 a = 0; a < to->argumentCount(); a++) {
                    auto supplied = edge.arguments.get(base, a);
                    auto argument = base[to->argumentAt(base, a)];
                    if(!supplied) {
                        fail("an edge with a missing argument"_v, terminator->source);
                        continue;
                    }

                    if(base[supplied]->rep != argument->rep) {
                        fail("an edge argument whose representation is not the block argument's"_v,
                             terminator->source);
                    }
                }
            });

            for(U32 at = 0; at + 1 < block->instructionCount(); at++) {
                auto inst = base[block->instructionList.get(base, at)];
                if(isTerminator(inst->kind)) {
                    fail("a terminator that is not the last instruction of its block"_v, inst->source);
                }
            }
        }
    }

    bool run() {
        computeDominance();

        for(U32 i = 0; i < body.values.size(); i++) {
            auto value = base[body.values.get(base, i)];
            if(value->definer && value->block) {
                fail("a value that is both defined by an instruction and a block argument"_v,
                     kNullLocation);
            }
        }

        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = base[body.blocks.get(base, i)];
            for(auto instPointer: block->instructions(base)) checkInst(*base[instPointer]);
        }

        at = nullptr;
        checkEdges();
        return ok;
    }
};

} // namespace

bool verifyBody(JsTarget& target, JsBody& body) {
    Verifier verifier(target, body);
    return verifier.run();
}

} // namespace jsir
