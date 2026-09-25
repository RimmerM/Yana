#include "convert_internal.h"

/*
 * SSA -> JSIR: the instruction switch, the terminators, the edges, aggregates and calls.
 *
 * See convert.h for the two passes and `convert_internal.h` for the state they share. Four families
 * are files of their own beside this one - `constant.cpp`, `teardown.cpp`, `host.cpp` and
 * `vector.cpp` - which is §8.1's split; what is worth knowing before reading any of them:
 *
 * **An SSA value whose convention is `Address` becomes a place, and everything else becomes a
 * value.** That one discriminator is what replaces the shipping backend's scattered "is this a
 * reference or a box" questions: `ssa::Convention::Address` is a statement the tier above already
 * made and checked, so this file never asks a type whether something is storage.
 *
 * **A place becomes one value only where it has to.** Crossing an edge, filling a `Cell` argument or
 * being returned are the three, and each of them names the value the place is rooted in rather than
 * building a descriptor. Where a place has no such value - a property, an element - the site reports
 * rather than boxing, because a box with a write-back is a copy that aliases nothing and that is the
 * form `Implementation-JS-Repr.md` part B removed.
 *
 * **Nothing falls through.** Every arm of every switch either converts or reports by name. The sweep
 * over the corpus is then a worklist rather than a list of fixtures that answer the wrong number.
 */

namespace jsir {

// `convert.h`'s exported predicate, defined here because a header cannot: `planBoxedGlobals` asks
// the same question of a global that the demand pass asks of a local, and one answer is the point.
bool jsAddressEscapes(JsTarget& target, ssa::Body& from, SsaPtr<ssa::Value> address,
                      Array<SsaPtr<ssa::Value>>& pending, IndexSet& seen) {
    return walkAddress(target, from, address, pending, seen, nullptr);
}




/*
 * Which of a block's SSA arguments become JSIR arguments.
 *
 * A `Void` argument is not one: it occupies no position and carries nothing, so every edge would be
 * supplying a value that does not exist. Filtered by the same predicate at both ends, which is what
 * keeps the arity agreeing without a mapping table.
 */
static bool keepsArgument(Conversion& c, ssa::Value& argument) {
    return !c.isVoidRep(c.repOf(argument)) && c.at(argument).sameAs == ~0u;
}

/*
 * Whether a value is a **reference** as this tier carries one - storage a place reads, or a value
 * that is a `{$o, $k}` cell. The two ends of `findSameReferenceArguments` ask it.
 */
bool Conversion::isReferenceArgument(ssa::Value& value) {
    if(isVoidRep(repOf(value))) return false;
    return carriesStorage(value) || jsManifest(target, value.occurrence, kNullLocation).needsCell;
}

/*
 * A **reference block argument that every edge hands the same reference** is that reference - the
 * trivial-phi rule, asked of the one kind of argument where keeping the phi costs an allocation
 * (J136, J94 §6.3 C).
 *
 * `bump(t, if c then 3 else 5)` borrows `t` before the condition, so the loan crosses the join the
 * condition makes as a block argument - on both arms, and one level further for each nested
 * conditional. The shared tier keeps those arguments on purpose: a loan's extent is what the
 * ownership passes read, and it is carried by the argument. Here it is a representation question
 * only. A reference argument is one host value, so each edge built a `{$o, $k}` descriptor over the
 * same `(owner, key)` and the call took it apart again: two objects per iteration of a loop that
 * natively does nothing but an add. With the argument gone the call site reads the place of the
 * value itself, which is the flat `(owner, key)` a direct `bump(t, 3)` passes.
 *
 * An optimistic fixpoint over the three-point lattice - no answer yet, one value, more than one -
 * so a loan carried around a loop is found too: the back edge hands the argument itself, which
 * contributes nothing. Every argument that settles on one value is redirected; the rest stay real.
 * The value an argument settles on dominates the argument's block, because it reaches the end of
 * every predecessor - so it is converted before that block in reverse postorder, which is where its
 * row is copied. Only blocks the order reaches are asked, for the same reason: a block converted
 * after the order in table order has no such guarantee.
 *
 * Asked of references and nothing else, because nothing else pays for the argument: a scalar or a
 * part crossing an edge is a variable assignment, and the shared optimizer already removes the
 * trivial phis of ordinary values. The redirect copies the row rather than resolving each read,
 * which is what keeps it one line at the block's start instead of a lookup at every operand.
 */
void Conversion::findSameReferenceArguments(Array<U32>& order, ssa::Block* entry) {
    constexpr U32 kTop = ~0u - 1;
    constexpr U32 kBottom = ~0u - 2;

    SmallArray<ssa::Value*, 16> candidates;
    SmallArray<ssa::Block*, 16> owners;

    for(auto i: order) {
        auto block = from->block(ssa, BlockId(i));
        if(block == entry) continue;

        for(auto pointer: block->arguments(ssa)) {
            auto argument = ssa[pointer];
            if(!isReferenceArgument(*argument)) continue;

            at(*argument).sameAs = kTop;
            candidates.push(argument);
            owners.push(block);
        }
    }

    if(!candidates.size()) return;

    IndexSet candidate;
    candidate.reset(from->values.size());
    for(auto argument: candidates) candidate.set(argument->id.index, true);

    // What one edge operand says the argument is, under the current answers.
    auto resolve = [&](ssa::Value& operand) -> U32 {
        if(!candidate[operand.id.index]) {
            return isReferenceArgument(operand) ? operand.id.index : kBottom;
        }

        auto state = at(operand).sameAs;
        if(state == kTop) return kTop;
        if(state == kBottom) return operand.id.index;
        return state;
    };

    for(auto changed = true; changed;) {
        changed = false;

        for(U32 k = 0; k < candidates.size(); k++) {
            auto argument = candidates[k];
            auto& row = at(*argument);
            if(row.sameAs == kBottom) continue;

            auto target_ = owners[k];
            auto position = ~0u;
            for(U32 p = 0; p < target_->argumentCount(); p++) {
                if(ssa[target_->argumentAt(ssa, p)] == argument) position = p;
            }

            auto meet = kTop;
            for(auto i: order) {
                auto last = from->block(ssa, BlockId(i))->terminator(ssa);
                if(!last) continue;

                ssa::forEachEdge(ssa, *last, [&](ssa::Edge& edge) {
                    if(meet == kBottom || ssa[edge.target] != target_) return;
                    if(position >= edge.arguments.size()) {
                        meet = kBottom;
                        return;
                    }

                    auto said = resolve(*ssa[edge.arguments.get(ssa, position).value()]);
                    if(said == kTop || said == argument->id.index) return;
                    if(said == kBottom || (meet != kTop && meet != said)) meet = kBottom;
                    else meet = said;
                });
            }

            if(meet != row.sameAs) {
                row.sameAs = meet;
                changed = true;
            }
        }
    }

    for(auto argument: candidates) {
        auto& row = at(*argument);
        if(row.sameAs == kTop || row.sameAs == kBottom) row.sameAs = ~0u;
    }
}

/*
 * The values an edge hands over, read **before** the terminator that carries them exists.
 *
 * Reading one may emit a `load` - a place crossing an edge is materialized where it is used - and an
 * instruction emitted after the terminator is an instruction in a block that has already ended.
 * That is not a theoretical ordering worry: it produced a block whose last instruction was a load,
 * so the structurer found no terminator and the body could not be built at all. So every terminator
 * here collects first and attaches second.
 */
void Conversion::collectEdge(ssa::Edge& source, LocationId where, SmallArray<JsValue*, 8>& into) {
    auto to = ssa[source.target];
    auto arguments = source.arguments;

    for(U32 i = 0; i < arguments.size(); i++) {
        auto argument = ssa[to->argumentAt(ssa, i)];
        if(!keepsArgument(*this, *argument)) continue;

        auto wanted = jsManifest(target, argument->occurrence, where);

        // A multi-part target argument takes its parts, in order - the other end of §3.8's rule,
        // and the one the verifier compares position for position against the block's arguments.
        if(!wanted.needsCell && target.rep(wanted.rep).kind == JsRepKind::Parts &&
           !carriesStorage(*argument)) {
            auto supplied = requireParts(arguments.get(ssa, i).value(), wanted.rep, where);
            if(!supplied.valid()) {
                internal("met a multi-part edge argument with no parts"_v, where);
                return;
            }

            for(U16 part = 0; part < supplied.count; part++) into.push(partAt(supplied, part));
            continue;
        }

        auto value = valueAs(arguments.get(ssa, i).value(),
                             wanted.needsCell ? jsCellRep(target, wanted.rep) : wanted.rep, where);
        if(!value) {
            internal("met an edge argument with no value"_v, where);
            return;
        }

        into.push(value);
    }
}

void Conversion::supplyEdge(JsEdge& edge, JsBlock* from, ssa::Edge& source, LocationId where,
                            Buffer<JsValue*> values) {
    auto to = ssa[source.target];

    // A block the conversion declined to build - which is only a deleted one, since every live block
    // is created before any instruction. An edge naming one would be a dangling target, so it is an
    // internal error here rather than an unwrap of nothing.
    auto found = blockFor(to->index.index);
    if(!found) {
        internal("met an edge naming a block it did not convert"_v, where);
        return;
    }

    builder.connect(edge, from, found);
    for(auto value: values) builder.edgeArgument(edge, value);

    (void)where;
}

void Conversion::convertAggregate(ssa::InstAggregate& aggregate) {
    auto where = aggregate.source;
    auto result = ssa[aggregate.results.get(ssa, 0)];
    auto repId = repOf(*result);

    // Asked here, beside the stored representation, because both are questions about the same
    // occurrence and asking one of them later would be asking it while a reference into the
    // interning table is live.
    auto multi = partsRepOf(*result);
    auto& rep = target.rep(repId);
    auto type = result->occurrence.type;

    auto record_ = decl[type]->kind == mir::Type::Record ? (mir::RecordType*)decl[type] : nullptr;

    /*
     * The components, each at the **property's** representation rather than at its own.
     *
     * A `&` field holds the `{$o, $k}` descriptor, and what the component is holding is the value
     * the place reads as - so a record capturing a mutable local is the one shape where the two
     * differ, and it is the same adaptation an argument at a `Cell` parameter gets. Read before the
     * object node exists, for `collectEdge`'s reason.
     */
    SmallArray<JsValue*, 8> components;
    {
        auto arm = record_ ? record_->cons.get(decl, aggregate.constructor) : mir::RecordCon {};
        auto index = U16(0);

        for(auto used: aggregate.components.contents(ssa)) {
            JsRepId wanted;

            /*
             * Off the **shape** rather than re-derived from the field's declaration, which is the
             * one place the two can disagree: a qualified type argument makes `Maybe('v)`'s payload
             * a reference where `Maybe(v)`'s is the value, and the shape is keyed on that while the
             * declaration says nothing about it. Asking the property the object is about to be
             * built with is the answer that cannot drift from the object's own hidden class.
             */
            if(record_ && index < arm.count && rep.kind == JsRepKind::GcObject) {
                auto slot = target.shape(rep.shape).slotOfField(U16(arm.first + index));
                if(slot != JsProperty::kNoField) {
                    auto held = target.shape(rep.shape).properties[slot].rep;
                    if(target.rep(held).kind == JsRepKind::Cell) wanted = held;
                }
            }

            // A niche-folded value has no shape to read the property off, and the payload is the
            // whole of it - so what the fold interned is the answer to the same question.
            if(rep.kind == JsRepKind::Nullable && target.rep(rep.content).kind == JsRepKind::Cell) {
                wanted = rep.content;
            }

            components.push(wanted.valid() ? valueAs(used.value(), wanted, where)
                                           : valueOf(used.value(), where));
            index++;
        }
    }

    if(rep.kind == JsRepKind::Void) return define(aggregate, nullptr);

    /*
     * A **scalarized** record: the construction is a `Pack` of the components, in record order,
     * which is the order the packing row lists them in. A constant construction folds to one number
     * in the builder, so `Flags {a: True, b: False}` is the literal `1`.
     */
    if(record_ && rep.kind == JsRepKind::Number) {
        auto packing = jsWholePackingOf(target, type);
        if(packing && packing->word == repId) {
            auto row = jsWholePackingRow(target, type);
            if(auto value = packOf(row, record_->cons.get(decl, aggregate.constructor).first,
                                   held(components), where)) {
                return define(aggregate, value);
            }

            return;
        }
    }

    /*
     * An enum, and a sum whose payloads all substituted away: the tag is the whole value.
     *
     * `RecordLayout::Enum` and not "the representation is a number", which is the test this had
     * first and which is wrong for exactly one shape: a **transparent** record over an integer is a
     * number too, and its constructor's tag is zero - so `Metres {value: truncate(v)}` compiled to
     * `return 0` and every use of it answered the wrong number.
     */
    if(record_ && record_->layout == mir::RecordLayout::Enum && rep.kind == JsRepKind::Number) {
        auto tag = record_->cons.get(decl, aggregate.constructor).value;
        return define(aggregate, builder.number(F64(tag), repId, where));
    }

    /*
     * A **niche-folded** construction: `Nothing` is `null` and `Just(x)` is `x` itself.
     *
     * The whole of what this slice removes is here - one object and one hidden class per `Just`,
     * over a type the library answers from `find`, `next`, every parser and every lookup. The
     * present arm is an `Alias` rather than nothing so that the dump says the constructor was
     * built, exactly as a transparent record's is.
     */
    if(auto niche = nicheOf(repId)) {
        if(!record_) {
            report("an aggregate of a niche-folded value of a non-record type"_v, type, where);
            return;
        }

        auto arm = record_->cons.get(decl, aggregate.constructor);
        auto tag = I32(arm.value);
        if(tag != niche->presentTag) return define(aggregate, absentOf(repId, where));

        /*
         * The payload by its **position in the constructor**, not the only component there is: the
         * present arm may carry unit fields beside the one thing the fold kept, and each of those is
         * a component with no value at all.
         */
        auto at = U32(niche->payloadField - arm.first);
        if(at >= components.size() || !components[at]) {
            // A component with no value is one an earlier report already named, and the tagged form
            // filled it with the representation's zero for exactly that reason - so this says what
            // is missing once and does not turn a failed conversion into a second diagnostic about
            // its own consequence.
            if(!failed) report("an aggregate of a niche-folded value with no payload"_v, type, where);
            return define(aggregate, absentOf(repId, where));
        }

        auto inst = builder.add<JsInstAlias>(repId, where, components[at] - base());
        return define(aggregate, base()[inst->result]);
    }

    if(rep.kind == JsRepKind::HostArray && decl[type]->kind == mir::Type::Array) {
        auto inst = builder.add<JsInstArray>(repId, where);
        for(auto component: components) {
            if(!component) continue;
            inst->values.push(target.arena, component - base());
            component->uses++;
        }

        return define(aggregate, base()[inst->result]);
    }

    if(rep.kind != JsRepKind::GcObject) {
        /*
         * A transparent record is its one stored field, so the aggregate is that component handed
         * on and the `@host` ones are discarded - they are properties the value already has, and
         * writing one would be writing over the host's own answer.
         */
        if(transparentContentOf(target, type) && decl[type]->kind == mir::Type::Record) {
            auto& record = *(mir::RecordType*)decl[type];
            auto arm = record.cons.get(decl, aggregate.constructor);

            for(U16 i = 0; i < arm.count && i < components.size(); i++) {
                auto field = record.fields.get(decl, U16(arm.first + i));
                if(field.host || !components[i]) continue;

                auto inst = builder.add<JsInstAlias>(repId, where, components[i] - base());
                return define(aggregate, base()[inst->result]);
            }
        }

        if(components.size() == 1 && components[0]) {
            auto inst = builder.add<JsInstAlias>(repId, where, components[0] - base());
            return define(aggregate, base()[inst->result]);
        }

        report("an aggregate of this representation"_v, type, where);
        return;
    }

    if(decl[type]->kind != mir::Type::Record) {
        report("an aggregate of a non-record object type"_v, type, where);
        return;
    }

    auto& record = *(mir::RecordType*)decl[type];
    auto arm = record.cons.get(decl, aggregate.constructor);
    auto& shape = target.shape(rep.shape);

    SmallArray<JsValue*, 8> members;
    for(auto& property: shape.properties) {
        if(property.role == JsPropertyRole::Tag) {
            members.push(builder.number(F64(arm.value), property.rep, where));
            continue;
        }

        // A `$pN` word is the `Pack` of the fields it holds - `JsShape::packedFields`.
        if(property.role == JsPropertyRole::StorageUnit) {
            auto value = packOf(property.packing, arm.first, held(components), where);
            if(!value) return;
            members.push(value);
            continue;
        }

        if(property.field >= arm.first && property.field < U16(arm.first + arm.count)) {
            auto index = U32(property.field - arm.first);
            if(index < components.size() && components[index]) {
                members.push(components[index]);
                continue;
            }
        }

        // Every property a value of the shape will ever have, including the other constructors' -
        // see `zeroOf`. Their values are the representation's zero and copy to the same zero.
        members.push(zeroOf(property.rep, where));
    }

    /*
     * A **window** is built as its parts and no object exists - the point of J71's flattening said
     * at the one instruction that would otherwise allocate.
     *
     * The members are already in shape order, which is the order the parts are interned in, so the
     * two lists are the same list read twice. A consumer that needs one value materializes exactly
     * this object at the position that asked (`requireOne`), and one that does not - a field read, a
     * direct call, an edge - never sees it.
     */
    if(multi.valid() && !jsPartsAreLanes(target, multi) &&
       target.rep(multi).partCount == members.size()) {
        for(auto member: members) {
            if(member) continue;
            internal("met a multi-part aggregate component with no value"_v, where);
            return;
        }

        return defineParts(aggregate, makeParts(multi, held(members)));
    }

    auto inst = builder.add<JsInstObject>(repId, where, rep.shape);
    for(auto member: members) {
        if(!member) {
            internal("met an aggregate property with no value"_v, where);
            return;
        }

        inst->values.push(target.arena, member - base());
        member->uses++;
    }

    define(aggregate, base()[inst->result]);
}


void Conversion::convertCall(ssa::InstCall& call) {
    auto where = call.source;

    /*
     * A dispatch out of this body's own environment - a class member, or the one callable a
     * `f: (a) -> b` requirement names.
     *
     * Two reads and no search, exactly as on native: the witness out of the environment slot, the
     * member out of the witness. What the cell holds is a **concrete** function, so the call needs
     * no environment of its own - `assemble.cpp`'s witness is what holds both ends to that, by
     * refusing to build a cell whose function does not take what the member's signature promised.
     *
     * `CallWitness` is not one of these: it is the synthesized cell a structural `Call(f, S)`
     * requirement gets, whose entry splits a pair, and it is reported by name below.
     */
    JsValue* slotCallee = nullptr;

    if(call.through == mir::CallThrough::ClassSlot ||
       call.through == mir::CallThrough::FunctionSlot) {
        auto slot = U32(0);
        auto kind = call.through == mir::CallThrough::ClassSlot ? mir::GenSlotKind::Class
                                                                : mir::GenSlotKind::Function;
        if(!requirementSlot(kind, call.requirement, slot)) {
            internal("met a slot dispatch in a body with no numbering"_v, where);
            return;
        }

        slotCallee = genSlot(slot, where);
        if(!slotCallee) return;

        // A class witness is a table and a function requirement is the function itself - the one
        // difference between the two rows, and it is one indexing step.
        if(call.through == mir::CallThrough::ClassSlot) {
            slotCallee = tableCell(slotCallee, call.member, where);
            if(!slotCallee) return;
        }
    } else if(call.through != mir::CallThrough::Declaration &&
              call.through != mir::CallThrough::Value) {
        report("a call through a structural `Call` witness"_v, where);
        return;
    }

    /*
     * The plan, from the **declaration** where there is one and from the function *type* where the
     * call goes through a value.
     *
     * `InstCall::callee` is documented as meaningful in every form, and for a call through a value
     * that form is "the signature it was checked against" - which the conversion leaves null at the
     * two step sites that build one, so it is not a plan this tier may read. The target value's own
     * type is, and holding both ends to it is what §3.7 asks for.
     */
    auto abiId = call.through == mir::CallThrough::Value
        ? jsAbiOfType(target, ssa[call.target.value()]->occurrence.type, where)
        : (call.callee ? jsAbiOf(target, call.callee, where) : JsAbiId());

    if(!abiId.valid()) {
        report(call.through == mir::CallThrough::Value
            ? "a call through a function value of a raw calling convention"_v
            : "a call through a value with no declared signature"_v, where);
        return;
    }

    auto& abi = target.abi(abiId);

    /*
     * The arguments, against the callee's frozen plan rather than against what each happens to be.
     *
     * `Omitted` takes no position at all, `Cell` names the box a mutable borrow is carried in, and
     * `One` is the value. A mismatch between what the caller holds and what the plan asks for is
     * reported here rather than repaired, because the repair - a box with a write-back after the
     * call - is a copy that aliases nothing and outlives nothing.
     */
    SmallArray<JsValue*, 8> arguments;
    auto index = U32(0);

    // The tier-1 temporaries this call's arguments were given, each written back into its field
    // once the call has returned - native's `commitPackedBorrow`, re-reading the word at that
    // moment so two co-packed fields lent to one call both keep their update.
    struct PackedCommit { JsPlaceId field; JsPlaceId temporary; };
    SmallArray<PackedCommit, 2> commits;
    auto commit = [&] {
        for(auto& c: commits) {
            auto narrow = builder.place(c.field).content;
            builder.store(c.field, convertedTo(builder.load(c.temporary, where), narrow, where), where);
        }
    };

    for(auto used: call.arguments.contents(ssa)) {
        if(index >= abi.parameters.size()) {
            internal("met a call with more arguments than its callee declares"_v, where);
            return;
        }

        auto& passing = abi.parameters[index++];
        if(passing.kind == JsPassingKind::Omitted) continue;

        auto supplied = ssa[used.value()];

        if(passing.kind == JsPassingKind::Cell) {
            auto place = placeOrFail(used.value(), where);
            if(!place.valid()) return;

            auto reference = referenceTo(place, where);
            if(!reference) return;

            arguments.push(reference);
            continue;
        }

        // `Parts` crosses flat - one argument per part, in order. The verifier holds each of them
        // to `partRepAt` of the callee's plan, which is what makes "the lanes in the wrong order"
        // a report rather than a wrong answer.
        // A **reference** crossing flat is the place taken apart, not a value split: the argument
        // is storage the callee writes through, so what the parts are is `borrow.describe`'s two
        // halves and never a load of anything.
        if(passing.kind == JsPassingKind::Parts && isCellParts(passing.rep)) {
            auto place = placeOrFail(used.value(), where);
            if(!place.valid()) return;

            /*
             * A `@bits(n)` place into a `&` of what it refines - repr.md's tier 1, which MIR
             * admits unconverted (`fillsParameter`). The callee takes `(owner, key)` and the place
             * is three parts, so it gets what native's `materializeNarrow` gives it: a temporary
             * of the full width - here a one-slot box holding the field's value - that it reads and
             * writes as any other `&U32`, committed back masked after the call (`commits`, below).
             *
             * Not `$bitCell`'s accessor box, which is what this was until J149: its setter masks
             * at every write, so a callee that reads `x` back after writing a value the field
             * cannot hold saw the narrowed value (`x = x + 20; return x` answered 7 where native
             * answers 23 - `Packed.ReadBack.yana`). Repr.md's step 2 is that the callee works on the
             * hidden local exactly as on any other `&Int`, and only the commit narrows.
             */
            if(target.reps[passing.rep.index].partCount == 2 &&
               jsRefCarriesScale(target, builder.place(place).content)) {
                auto wide = target.reps[target.reps[passing.rep.index].content.index].content;
                auto current = convertedTo(builder.load(place, where), wide, where);

                auto boxShape = jsBoxShape(target, wide);
                auto box = builder.add<JsInstObject>(jsBoxRep(target, wide), where, boxShape);
                box->values.push(target.arena, current - base());
                current->uses++;

                auto temporary = builder.addProperty(base()[box->result], boxShape, 0);
                if(!pairParts(temporary, where, arguments)) {
                    internal("could not take a temporary's reference apart"_v, where);
                    return;
                }

                commits.push(PackedCommit { place, temporary });
                continue;
            }

            if(!referencePartsOf(place, where, arguments)) return;
            continue;
        }

        if(passing.kind == JsPassingKind::Parts) {
            auto supplied = requireParts(used.value(), passing.rep, where);
            if(!supplied.valid()) {
                internal("met a multi-part call argument with no parts"_v, where);
                return;
            }

            for(U16 part = 0; part < supplied.count; part++) arguments.push(partAt(supplied, part));
            continue;
        }

        (void)supplied;
        auto value = valueOf(used.value(), where);
        if(!value) {
            internal("met a call argument with no value"_v, where);
            return;
        }

        arguments.push(value);
    }

    /*
     * The result's representation - the callee's plan, except where the plan says the declaration
     * wrote a type it does not know.
     *
     * There the *site* is what knows: `widen(value) :: Int` inside a body constrained
     * `Widen(a, Int)` hands back an `I32`, and the member's own signature can only say `b`. Taking
     * the plan's `Erased` there left a number labelled as a value this tier declines to describe,
     * and the first thing done with one - a bitcast to `U32` - read as a host reference meeting a
     * number.
     */
    JsRepId resultRep;
    if(abi.result.kind != JsPassingKind::Omitted) {
        resultRep = abi.result.erased && call.results.size()
            ? repOf(*ssa[call.results.get(ssa, 0)])
            : carriedRepOf(target, abi.result);
    }

    /*
     * A dispatch out of a slot - the cell called with the arguments and nothing in front of them.
     *
     * What the cell holds is a concrete function, which is what makes the erased side of the
     * boundary cheap on this target: a host value of any type is one value, so the class member's
     * plan and the instance's agree at every position the two spell the same way, and `assemble.cpp`
     * refuses to fill a cell where they do not rather than adapting silently.
     */
    if(slotCallee) {
        auto inst = builder.add<JsInstCallValue>(resultRep, where, slotCallee - base(), nullptr,
                                                 abiId);
        slotCallee->uses++;

        finishCall(inst, call, arguments, resultRep);
        commit();
        return;
    }

    if(call.through == mir::CallThrough::Declaration) {
        /*
         * The environment a body compiled once is entered with - one module-level table, named here
         * and filled by `assemble.cpp`.
         *
         * Built **before** the call node exists, which is this file's standing rule read at one more
         * site: the builder appends as it is called, so a node made after the instruction that uses
         * it lands after it. Every cell of it is a compile-time constant, because this milestone
         * builds one only where the argument list at the call is concrete - a call *inside* an
         * erased body that would have to forward its own slots is refused one tier up, in
         * `callTarget`, rather than arriving here with nothing to write.
         */
        JsValue* table = nullptr;
        if(call.environment) {
            auto node = builder.add<JsInstGenTable>(jsGenTableRep(target), where, call.environment);
            table = base()[node->result];
        }

        auto inst = builder.add<JsInstCall>(resultRep, where, call.callee, abiId);

        if(table) {
            inst->genEnv = table - base();
            table->uses++;
        }

        finishCall(inst, call, arguments, resultRep);
        commit();
        return;
    }

    /*
     * A call through a function value - the pair taken apart and applied.
     *
     * `code(env, ...)` unconditionally, which is what makes one convention serve both halves: a
     * lifted lambda already begins with its environment, and a named function that does not gets a
     * thunk at the `fun.value` that produced the pair. The alternative - two call shapes chosen from
     * what the code word turned out to be - is a decision at a site that cannot see it. It is also
     * what the shipping backend spells (`f.$c(f.$e, ...)`), so the two generators' emitted calls are
     * the same call.
     */
    auto pair = valueOf(call.target.value(), where);
    if(!pair) {
        internal("met a call through a value with no pair"_v, where);
        return;
    }

    auto shape = jsFunPairShape(target);
    auto code = builder.load(builder.addProperty(pair, shape, 0), where);
    auto environment = builder.load(builder.addProperty(pair, shape, 1), where);

    auto inst = builder.add<JsInstCallValue>(resultRep, where, code - base(),
                                             environment - base(), abiId);
    code->uses++;
    environment->uses++;

    finishCall(inst, call, arguments, resultRep);
    commit();
}


void Conversion::convertInst(ssa::Inst& inst) {
    auto where = inst.source;

    // Every multi-part operation first - see `convertVectorInst`, where the representation rather
    // than the type being the discriminator is what makes that a legal test at this tier.
    if(convertVectorInst(inst)) return;

    switch(inst.kind) {
        case ssa::Inst::Const: {
            auto& written = (ssa::InstConst&)inst;
            auto result = ssa[inst.results.get(ssa, 0)];
            auto repId = repOf(*result);
            define(inst, constantOf(written.constant, repId, where));
            return;
        }

        case ssa::Inst::GlobalAddress: {
            auto& written = (ssa::InstGlobalAddress&)inst;
            auto global = decl[written.global];
            auto manifest = jsManifest(target, global->occurrence, where);

            // A binding holds one host value - `jsStoredRep`, which is what makes a global of
            // vector type the lane row rather than a representation nothing can be stored at.
            manifest.rep = jsStoredRep(target, manifest.rep);

            /*
             * A **boxed** global - `counter.$v` rather than `counter`.
             *
             * Which globals are boxed is `planBoxedGlobals`' whole-program answer, and reading it
             * here is what makes the two functions that share one global agree: the binding holds
             * `{$v: ...}` for every reader, and an ordinary load or store is one property deeper
             * than it would otherwise be. `assemble.cpp` wraps the initializer in the same shape.
             */
            if(target.boxedGlobals.contains(U32(written.global.offset))) {
                auto boxShape = jsBoxShape(target, manifest.rep);
                auto slot = builder.addGlobal(written.global, jsBoxRep(target, manifest.rep));
                auto box = builder.load(slot, where);
                definePlace(inst, builder.addProperty(box, boxShape, 0));
                return;
            }

            definePlace(inst, builder.addGlobal(written.global, manifest.rep));
            return;
        }

        /*
         * An allocation - a `var` or a `{$v}` box, and nothing else.
         *
         * The demand pass decided which; what is left here is building the box where one is needed.
         * Its initial contents are the representation's zero rather than nothing, so the shape of
         * the object is complete from its first assignment - which is what `zeroOf` is about.
         */
        case ssa::Inst::Alloc: {
            auto& written = (ssa::InstAlloc&)inst;
            auto result = ssa[inst.results.get(ssa, 0)];
            auto manifest = jsManifest(target, written.held, where);

            // Storage holds one value, so a slot for a multi-part value holds its materialized
            // form. That is what a vector local costs under `-no-ir-opt` and nothing under the
            // optimizer, which promotes such a slot to values before this tier ever sees it.
            manifest.rep = jsStoredRep(target, manifest.rep);

            if(isVoidRep(manifest.rep)) {
                definePlace(inst, builder.addVariable(manifest.rep));
                return;
            }

            auto escaping = escapes[result->id.index];
            auto& rep = target.rep(manifest.rep);

            if(!escaping) {
                auto place = builder.addVariable(manifest.rep);

                // A `var` nothing has written is `undefined`, and a record filled field-wise never
                // writes it whole - see `projects`. The zero goes in at the allocation so that the
                // first `s.$index = 0` has an object to write into, and the place stays a `Var`, so
                // a whole-value store is still the rebinding it should be.
                if(rep.kind == JsRepKind::GcObject && projects[result->id.index]) {
                    auto object = zeroOf(manifest.rep, where);
                    if(!object) {
                        internal("could not build a local's initial object"_v, where);
                        return;
                    }

                    builder.store(place, object, where);
                }

                definePlace(inst, place);
                return;
            }

            /*
             * An escaping local whose value **is** a host object - the object itself, built here.
             *
             * `GcObject` and not `isObjectLike`, for `jsManifest`'s reason: a host array reached
             * through a borrow has no property to write, so rebinding it needs a cell.
             *
             * Built at its zero rather than left as a `var`, and the place is a `Value` rather than
             * a `Var` - which are the same decision twice. What escapes is the *object*, so it has
             * to exist before the reference is taken and a whole-value store into it has to write
             * its properties rather than rebind the variable: a `var` holding nothing captured
             * `undefined`, and a later `s = Pair {...}` would have left the holder looking at the
             * old object. `Iter.yana`'s `firstPairOver` is both at once - the continuation writes
             * the record the `return` carries into a slot the enclosing frame reads afterwards -
             * and it answered six short rather than refusing.
             */
            if(rep.kind == JsRepKind::GcObject && !descriptors[result->id.index]) {
                auto object = zeroOf(manifest.rep, where);
                if(!object) {
                    internal("could not build an escaping local's object"_v, where);
                    return;
                }

                definePlace(inst, builder.addValuePlace(object, manifest.rep));
                return;
            }

            /*
             * A `{$v}` box, and a property place into it.
             *
             * The box is the *storage*; a reference to this local is then an ordinary descriptor
             * naming that property, which is what makes one reference form serve a local, a field
             * and an element alike. Two objects where the shipping backend also uses two, and for
             * the same reason: a descriptor that could only ever point at a whole local would be a
             * second reference form for the other two to be converted into.
             */
            auto boxShape = jsBoxShape(target, manifest.rep);
            auto boxRep = jsBoxRep(target, manifest.rep);

            auto content = zeroOf(manifest.rep, where);
            if(!content) {
                internal("could not build a box's initial contents"_v, where);
                return;
            }

            auto box = builder.add<JsInstObject>(boxRep, where, boxShape);
            box->values.push(target.arena, content - base());
            content->uses++;

            definePlace(inst, builder.addProperty(base()[box->result], boxShape, 0));
            return;
        }

        // A collected target hands nothing back - §3.9's "derived reclaim of GC storage emits no
        // JSIR operation", read at the storage end rather than at the value's.
        case ssa::Inst::Free:
            return;

        case ssa::Inst::FieldAddress: {
            auto& written = (ssa::InstFieldAddress&)inst;

            /*
             * A field of a value carried as **parts** is one of the parts, and there is no storage
             * to name at all - which is the whole of what J71's window flattening buys at a read.
             *
             * `length(xs)` and `xs[i]` are library bodies grafted by `@inline(always)`, so every one
             * of them arrives here as `field.addr .length` over a window; answering the part is what
             * makes the inlined body cost nothing. The address is registered as an ordinary *value*
             * rather than as a place, which is the state `placeOf` already knows ("an address that
             * arrived as a value") - a `load` through it reads the part back out and a `store` is
             * refused by name, since a window's own field is not somebody else's storage.
             *
             * Parts and shape properties are one list interned twice, so the index is the slot.
             */
            if(written.property == ssa::InstFieldAddress::kNoProperty &&
               written.field != ssa::InstFieldAddress::kDiscriminant) {
                auto ref = at(*ssa[written.base.value()]).parts;
                if(ref.valid()) {
                    auto stored = jsStoredRep(target, ref.rep);

                    if(!jsPartsAreLanes(target, ref.rep) &&
                       target.rep(stored).kind == JsRepKind::GcObject) {
                        auto slot = target.shape(target.rep(stored).shape).slotOfField(written.field);
                        if(slot != JsProperty::kNoField && slot < ref.count) {
                            return define(inst, partAt(ref, slot));
                        }
                    }
                }
            }

            auto basePlace = placeOrFail(written.base.value(), where);
            if(!basePlace.valid()) return;

            /*
             * A field of a **scalarized** record - a range of the number the base place holds.
             *
             * Asked of the place's content before anything is loaded, because the word is not read
             * here: a `BitRange` names the storage, and a load or a store of it is the decode or the
             * read-modify-write. The row is the record's (`jsWholePackingOf`), and the content has to
             * be that row's word too - a transparent record over a narrow integer is a number of the
             * same width, and it is not one of these.
             */
            if(written.property == ssa::InstFieldAddress::kNoProperty &&
               written.field != ssa::InstFieldAddress::kDiscriminant) {
                auto content = builder.place(basePlace).content;
                auto packing = target.rep(content).kind == JsRepKind::Number
                             ? jsWholePackingOf(target, written.record) : nullptr;

                if(packing && packing->word == content) {
                    for(auto& bits: packing->fields) {
                        if(bits.field != written.field) continue;

                        auto range = builder.addBitRange(basePlace, bits.offset, bits.width,
                                                         bits.isSigned, bits.rep);
                        return definePlace(inst, range);
                    }

                    // A unit field, which has no bits - see the object arm's `kNoField` below.
                    return definePlace(inst, builder.addVariable(jsVoidRep(target)));
                }
            }

            auto owner = builder.load(basePlace, where);
            if(!owner) {
                internal("met a projection of storage with no value"_v, where);
                return;
            }

            if(builder.place(basePlace).kind != JsPlaceKind::Value) markProjected(inst);

            /*
             * A **field of a type variable**, in a body compiled once - `mir::GenSlotKind::Property`,
             * and the one slot kind J69 reported rather than filled.
             *
             * A property is a *name* on this target, so the cell the caller wrote is that name and
             * the whole of the access is `owner[key]` - an `Element` place whose index happens to be
             * a string. The three shapes `lower/erase.cpp` gives the native slot - a byte offset, an
             * offset with a bit shift, and a `{read, set}` pair for a field whose width the body
             * cannot know - are all answers to "how wide is it and where does it start", and a host
             * object's property has neither a width nor a start.
             *
             * The content is the **requirement's declared result type**, which is the same rule
             * `lower/erase.cpp` states for the native slot: `a.size: I32` is a promise the signature
             * made, so both ends know the field's representation before either has seen the other.
             * Where the requirement names a variable - `fn (a.field: b) f(x: a)` - that is `Erased`,
             * which is exactly right for the erased ABI's own reason: a host value of any type is
             * one value.
             */
            if(written.property != ssa::InstFieldAddress::kNoProperty) {
                U32 slotIndex = 0;
                if(!requirementSlot(mir::GenSlotKind::Property, written.property, slotIndex)) {
                    internal("met a property requirement with no slot"_v, where);
                    return;
                }

                if(!genEnv) {
                    internal("met a property requirement in a body with no environment"_v, where);
                    return;
                }

                auto result = ssa[inst.results.get(ssa, 0)];
                auto pointer = result->occurrence.type;
                if(!pointer || decl[pointer]->kind != mir::Type::Pointer) {
                    internal("met a property projection whose result is not a pointer"_v, where);
                    return;
                }

                auto content = jsStoredRep(target,
                    jsRep(target, ((mir::PointerType*)decl[pointer])->to, nullptr, where));

                auto key = tableCell(genEnv, slotIndex, where);
                if(!key) return;

                definePlace(inst, builder.addElement(owner, key, content));
                return;
            }

            auto& ownerRep = target.rep(owner->rep);

            /*
             * A **niche-folded** sum, whose two projections are the two halves of §3.2's encoding.
             *
             * The discriminant is not in storage to be read, so it is intercepted here exactly as
             * the native `VirtualPlace::Folded` intercepts it at the load: the address is registered
             * as a *value* - the decoded constructor index - which is the state `placeOf` already
             * knows, so a `load` through it hands the tag back and nothing had to build a place for
             * a property that does not exist. A `store` into it is the one thing that needs the
             * storage back, and `nicheTags` is what keeps it.
             *
             * The payload *is* the value, so its place is the base's place read at the payload's
             * representation - which is the same sentence `Downcast` being the identity says in the
             * shipping backend.
             */
            if(auto niche = nicheOf(owner->rep)) {
                auto nullable = owner->rep;

                if(written.field == ssa::InstFieldAddress::kDiscriminant) {
                    auto tag = decodeNicheTag(owner, nullable, where);
                    if(!tag) return;

                    setNicheTag(ssa[inst.results.get(ssa, 0)]->id.index, basePlace);
                    return define(inst, tag);
                }

                if(written.field == niche->payloadField) {
                    return definePlace(inst, builder.addNichePayload(basePlace, niche->content));
                }

                // The absent constructor carries nothing and a unit field has no value, so the only
                // other projection a folded record has is one of those - a `Var` of `Void`, which
                // loads as nothing and stores nothing.
                return definePlace(inst, builder.addVariable(jsVoidRep(target)));
            }

            /*
             * The discriminant, which is a property of the object where the sum has one and *is* the
             * value where the record is its tag. Both are the same projection at this tier - the
             * address of a component whose position the representation decides - which is why
             * `InstFieldAddress` reserves an index for it rather than there being a second row.
             */
            if(written.field == ssa::InstFieldAddress::kDiscriminant) {
                if(ownerRep.kind == JsRepKind::Number) {
                    definePlace(inst, basePlace);
                    return;
                }

                if(ownerRep.kind != JsRepKind::GcObject) {
                    report("a discriminant of this representation"_v, written.record, where);
                    return;
                }

                auto slot = target.shape(ownerRep.shape).tagSlot();
                if(slot == JsProperty::kNoField) {
                    report("a discriminant of a record with no tag"_v, written.record, where);
                    return;
                }

                definePlace(inst, builder.addProperty(owner, ownerRep.shape, slot));
                return;
            }

            /*
             * A **transparent** record, whose value is its one stored field.
             *
             * Two answers and neither is a property of an object of this record's own, because there
             * is no such object: the stored field *is* the value, so its place is the base's; and a
             * `@host` field is a property the value already has, under the name the host gives it.
             * `Array(Handle)` is both at once - the bare host array, whose `length` is its occupancy.
             */
            if(ownerRep.kind != JsRepKind::GcObject) {
                auto content = transparentContentOf(target, written.record);
                if(!content) {
                    report("a field of this representation"_v, written.record, where);
                    return;
                }

                auto& record = *(mir::RecordType*)decl[written.record];
                auto field = record.fields.get(decl, written.field);

                if(!field.host) {
                    definePlace(inst, basePlace);
                    return;
                }

                auto hostShape = jsHostShapeOf(target, written.record, where);
                auto hostSlot = target.shape(hostShape).slotOfField(written.field);
                if(hostSlot == JsProperty::kNoField) {
                    report("a field of this representation"_v, written.record, where);
                    return;
                }

                definePlace(inst, builder.addProperty(owner, hostShape, hostSlot));
                return;
            }

            /*
             * A field that shares a `$pN` word - `JsTarget::packFields`. The word is an ordinary
             * property and the field is a range of it, so a store is a read-modify-write of the
             * property and everything that walks the shape per property copies the word whole.
             */
            if(auto packed = target.shape(ownerRep.shape).packedField(written.field)) {
                auto word = builder.addProperty(owner, ownerRep.shape, packed->slot);
                auto bits = packed->bits;
                return definePlace(inst, builder.addBitRange(word, bits.offset, bits.width,
                                                             bits.isSigned, bits.rep));
            }

            auto slot = target.shape(ownerRep.shape).slotOfField(written.field);
            if(slot == JsProperty::kNoField) {
                // A unit field has no property at all, so there is nothing to project into. A place
                // holding nothing is a `Var` of `Void`, which loads as nothing and stores nothing.
                auto unit = mir::aggregateComponent(decl, written.record, 0, 0);
                (void)unit;
                definePlace(inst, builder.addVariable(jsVoidRep(target)));
                return;
            }

            definePlace(inst, builder.addProperty(owner, ownerRep.shape, slot));
            return;
        }

        case ssa::Inst::ElementAddress: {
            auto& written = (ssa::InstElementAddress&)inst;

            auto basePlace = placeOrFail(written.base.value(), where);
            if(!basePlace.valid()) return;

            auto owner = builder.load(basePlace, where);
            auto index = valueOf(written.index.value(), where);
            if(!owner || !index) {
                internal("met an element address with no owner"_v, where);
                return;
            }

            if(builder.place(basePlace).kind != JsPlaceKind::Value) markProjected(inst);

            auto& ownerRep = target.rep(owner->rep);
            if(ownerRep.kind != JsRepKind::HostArray) {
                report("an element of this representation"_v, written.container, where);
                return;
            }

            // An element holds one host value, so a row of vectors holds `length` lane rows -
            // `jsStoredRep`, which is the same rule the shape properties and the places follow.
            auto element = mir::elementTypeOf(decl, target.program, written.container);
            auto elementRep = element ? jsStoredRep(target, jsRep(target, element, where))
                                      : ownerRep.content;

            definePlace(inst, builder.addElement(owner, index, elementRep));
            return;
        }

        case ssa::Inst::Load: {
            auto& written = (ssa::InstLoad&)inst;

            // A read of a pointer slot whose one store kept a place is that place - `pointerSlots`.
            {
                auto& slot = at(*ssa[written.address.value()]);
                if(slot.pointee.valid()) {
                    definePlace(inst, slot.pointee);
                    return;
                }
            }

            /*
             * A reborrow of a value that is already carried as **parts** - the parts, unchanged.
             *
             * A window is a value here, so `%6: Array(I32) [shared call view] = load %0` over a
             * window parameter is a relabelling rather than a read, exactly as the `represent` above
             * it is. Asked before `placeOf`, because that one would materialize the object in order
             * to have something to name a place over - which is the allocation this slice removes.
             */
            {
                auto held = ssa[inst.results.get(ssa, 0)];
                auto multi = partsRepOf(*held);

                if(multi.valid() && !jsPartsAreLanes(target, multi)) {
                    // By value, because `defineParts` writes the same row and a table that grew
                    // under a reference into it is a `JsPartsRef` of 0xfe bytes - which cost one
                    // debug session back when this was a hash map.
                    auto ref = at(*ssa[written.address.value()]).parts;
                    if(ref.valid() && ref.rep == multi) return defineParts(inst, ref);
                }
            }

            auto place = placeOrFail(written.address.value(), where);
            if(!place.valid()) return;

            auto result = ssa[inst.results.get(ssa, 0)];
            auto repId = repOf(*result);

            if(isVoidRep(repId)) return define(inst, nullptr);

            /*
             * A load whose result is itself an address - reading a borrow out of storage.
             *
             * The value read is the reference, so the result is a place rooted in it rather than a
             * value: what a later projection wants is the object, and `placeOf` builds the right
             * kind of place from the occurrence when it is asked.
             */
            /*
             * A load whose result is itself a **loan of the same storage** - a reborrow.
             *
             * Natively this is a real load: the address goes into a register and the value is that
             * address. Here the place *is* the answer, because a host value read out of a slot is
             * the same host value the slot holds - so keeping the place is both the shorter emission
             * and the one that lets a write through the reborrow reach the storage. `%28: I32
             * [shared slot 0] = load %27` over an element address is the shape, and without this the
             * unique half of it became a write into a copy of the element.
             *
             * `loanDenotesStorage` is §1.3.1's predicate, which is `readMode`'s own decision
             * procedure and reads no layout: a unique access or a published origin is storage, a
             * trivially copyable unpublished loan is a value. The extra condition is that the place
             * holds what the result claims to be - a slot holding a reference to something *else*
             * would be a place naming the wrong storage, and the representations are what says so.
             */
            // `jsStoredRep` on the claim, because a place holds one host value: a reborrow of a
            // vector slot names the row the slot holds, and comparing against the parts would have
            // said the place holds something else and read a copy out of it.
            if(loanDenotesStorage(result->occurrence) &&
               builder.place(place).content == jsStoredRep(target, repId)) {
                definePlace(inst, place);
                return;
            }

            auto value = builder.load(place, where);
            if(!value) {
                internal("met a load of storage with no value"_v, where);
                return;
            }

            if(result->convention == ssa::Convention::Address ||
               loanDenotesStorage(result->occurrence)) {
                definePlace(inst, placeOver(value));
                return;
            }

            /*
             * Taking a value **out of storage that is the object** - one level of it, copied.
             *
             * A `Value` place is storage whose host value is the object itself (§3.5), so a whole
             * value read out of one is that object and a whole value written into one writes its
             * properties. Both are right on their own and wrong together: `exchange(a, fresh)` reads
             * the old value out, writes the new one in, and hands the old one back - and the old one
             * was the object the write had just overwritten.
             *
             * The copy is **one level**, which is what a native relocation out of storage is: the
             * bytes move and every member that is a pointer or a row moves with them, so nothing
             * below the first level is duplicated. That is the difference from `Clone`, which is a
             * *second owner* and therefore recurses.
             *
             * Only at an owning read, and only out of a `Value` place - every other place is a slot
             * the write rebinds, so the value read out of it is nobody's but the reader's.
             */
            if(result->occurrence.contract.isOwned() &&
               builder.place(place).kind == JsPlaceKind::Value &&
               target.rep(repId).kind == JsRepKind::GcObject) {
                auto shapeId = target.rep(repId).shape;
                auto& shape = target.shape(shapeId);

                SmallArray<JsValue*, 8> held;
                for(U32 i = 0; i < shape.properties.size(); i++) {
                    held.push(builder.load(builder.addProperty(value, shapeId, U16(i)), where));
                }

                auto node = builder.add<JsInstObject>(repId, where, shapeId);
                for(auto property: held) {
                    node->values.push(target.arena, property - base());
                    property->uses++;
                }

                return define(inst, base()[node->result]);
            }

            define(inst, value);
            return;
        }

        /*
         * `copy.trivial` - a second owner made by duplicating the bytes, here by duplicating the
         * object.
         *
         * A value this target duplicates by reading it - a number, a bigint, a host string - becomes
         * an `Alias`, which says a sharing decision was taken and emits nothing. Anything else
         * becomes a `Clone`, which §6.3 is about turning back into an alias once mutation facts
         * agree.
         */
        case ssa::Inst::Copy: {
            auto& written = (ssa::InstCopy&)inst;

            /*
             * A second owner of a **scalarized record** is its parts named again.
             *
             * Every part of one is a primitive - `scalarizesHere` is where that condition is argued
             * - so there is nothing to duplicate and nothing to materialize: going through the place
             * below would build the object this representation exists not to build, at a borrow that
             * is the most common thing that happens to a record.
             */
            auto carried = at(*ssa[written.address.value()]).parts;
            if(carried.valid() && target.rep(carried.rep).freshEachUse) {
                return defineParts(inst, carried);
            }

            auto place = placeOrFail(written.address.value(), where);
            if(!place.valid()) return;

            auto result = ssa[inst.results.get(ssa, 0)];
            auto repId = repOf(*result);
            if(isVoidRep(repId)) return define(inst, nullptr);

            /*
             * A value this target duplicates by **reading** it, and the second kind of one.
             *
             * The first is a primitive - a number, a bigint, a host string. The second is a raw
             * pointer, and it is a statement about what a `%T` *is* rather than about its
             * representation: it names storage the program does not own, so a second owner of the
             * name is the name. `data Collator {host: %U8}` is the shape that says why - the handle
             * is an `Intl.Collator`, its representation is the same `HostArray` a byte row has, and
             * `.slice()` over one answers "not a function".
             */
            auto value = builder.load(place, where);
            auto held = result->occurrence.type;
            while(held && decl[held]->kind == mir::Type::Record) {
                auto content = transparentContentOf(target, held);
                if(!content) break;
                held = content;
            }

            /*
             * A **niche-folded** value whose payload is a primitive is read too - `Maybe(Int)` is
             * `number | null` and both halves of that are duplicated by being named.
             *
             * A folded value whose payload is an object still owes a duplicate, and one guarded by
             * the absence test: `cloneExpr` is where that guard is written, for the reason the
             * shipping backend writes it in `cloneValue` - it is an expression rather than a branch,
             * so nothing about the block the caller is filling changes.
             */
            auto niche = nicheOf(repId);
            if(target.rep(repId).isPrimitive() ||
               (niche && target.rep(niche->content).isPrimitive()) ||
               (held && decl[held]->kind == mir::Type::Pointer)) {
                auto aliased = builder.add<JsInstAlias>(repId, where, value - base());
                return define(inst, base()[aliased->result]);
            }

            auto cloned = builder.add<JsInstClone>(repId, where, value - base());
            define(inst, base()[cloned->result]);
            return;
        }

        case ssa::Inst::Store: {
            auto& written = (ssa::InstStore&)inst;

            // A pointer slot written once with a *place* keeps the place rather than a value -
            // see `pointerSlots`. A `Place` form and not a memo, so a pointer that is a host row
            // still goes through the store below.
            {
                auto& slot = *ssa[written.address.value()];
                auto& stored = at(*ssa[written.value.value()]);
                if(pointerSlots[slot.id.index] && stored.form == JsTargetValue::Form::Place &&
                   stored.place.valid()) {
                    at(slot).pointee = stored.place;
                    return;
                }
            }

            /*
             * A write of a **niche-folded** discriminant, which is the encode half of `nicheOf`.
             *
             * Setting the present constructor writes nothing - the payload's own store is what makes
             * the value identifiable, which is the same statement the native encode makes - and
             * setting the absent one is `v = null`. A tag this cannot read off a literal is reported
             * rather than spelled as a conditional rebinding of the storage: nothing in `lib/` or the
             * corpus computes a constructor index and stores it, and a rewrite that would have to
             * re-derive the payload is a family rather than a line.
             */
            auto tagged = at(*ssa[written.address.value()]).nicheTag;
            if(tagged.valid()) {
                auto basePlace = tagged;
                auto nullable = builder.place(basePlace).content;
                auto& rep = target.rep(nullable);

                auto tag = valueOf(written.value.value(), where);
                auto literal = tag && tag->definer && base()[tag->definer]->kind == JsInst::Const
                    ? &((JsInstConst*)base()[tag->definer])->constant : nullptr;

                if(!literal || literal->kind != JsConstant::Number) {
                    report("a computed discriminant written into a niche-folded value"_v, where);
                    return;
                }

                if(I32(literal->number) == rep.absentTag) {
                    builder.store(basePlace, absentOf(nullable, where), where);
                }

                return;
            }

            auto place = placeOrFail(written.address.value(), where);
            if(!place.valid()) return;

            auto content = builder.place(place).content;
            if(isVoidRep(content)) return;

            /*
             * A write into storage that **is** a host value rather than an object.
             *
             * A `Value` place holds its own object, so a whole-value store into one writes that
             * object's properties (§3.5) - and a host value with no properties has nothing for one
             * to write. The place kind can now reach that state, because a field of a window is a
             * part registered as an ordinary value and `placeOf` names a `Value` place over it: a
             * write through one would be a write into a copy nobody reads. Nothing in `lib/` or the
             * corpus writes a window's own `length`, so this is a refusal rather than a family to
             * build - what rebinds a window is a fresh one out of `slice`.
             */
            if(builder.place(place).kind == JsPlaceKind::Value && target.rep(content).isPrimitive()) {
                report("a write into storage that is a host value rather than an object"_v, where);
                return;
            }

            // The same statement one level in: a field of a packed record that arrived as a value
            // is a range of a number nobody stores, so there is no word to read-modify-write.
            if(builder.place(place).kind == JsPlaceKind::BitRange &&
               builder.place(builder.place(place).word).kind == JsPlaceKind::Value) {
                report("a write into a field of a packed value that is not in storage"_v, where);
                return;
            }

            /*
             * A `%T` field written with an **interior reference** - a lending iterator's hand-over
             * slot, which is the one storage this target has no representation for yet.
             *
             * `store %19 inspect %6` over a `$slot: %Held` is the shape: what is written is the
             * address of a member of the state record, and `jsRep` gives a `Type::Pointer` the host
             * *row* because `Host.js.yana` opens by saying a `%a` here is the array. Both readings
             * are right about different pointers, and telling them apart is the narrow-reference
             * family §3.2's `Parts` row names. Reported by name rather than left to the verifier,
             * which called it a store at the wrong representation and named no construct.
             */
            auto value = valueAs(written.value.value(), content, where);
            if(!value) {
                internal("met a store with no value"_v, where);
                return;
            }

            if(value->rep != content && at(*ssa[written.value.value()]).place.valid()) {
                report("a reference to storage written into a pointer field"_v, where);
                return;
            }

            /*
             * A number stored at another number's width - a `U32` written through a `&U32` that is
             * lent a `@bits(n) U32` place, repr.md's tier 1, where the callee's store has reached
             * the caller. The store narrows, which is the same step `convertValue` builds for every
             * other store into a refinement. Narrowing at each store is the call's answer only
             * because, since J149, the shared inliner splices such a call just where the callee
             * never reads the parameter after writing it (`storesThenNeverReads`); a callee that
             * reads back what it wrote stays a call and gets the temporary above.
             */
            if(value->rep != content && target.rep(value->rep).kind == JsRepKind::Number &&
               target.rep(content).kind == JsRepKind::Number &&
               mir::fillsParameter(decl, ssa[written.address.value()]->type(),
                                   ssa[written.value.value()]->type())) {
                value = base()[builder.add<JsInstConvert>(content, where, value - base())->result];
            }

            builder.store(place, value, where);
            return;
        }

        case ssa::Inst::Binary: {
            auto& written = (ssa::InstBinary&)inst;
            auto lhs = valueOf(written.lhs.value(), where);
            auto rhs = valueOf(written.rhs.value(), where);
            if(!lhs || !rhs) {
                internal("met a binary operation with no operands"_v, where);
                return;
            }

            /*
             * Only the operations this target has a spelling for - `structure.cpp`'s `binaryExpr`.
             *
             * Checked here rather than there because here is where there is still an alternative:
             * the emitter runs after the verifier, so a report from it is a report about a body that
             * has already been accepted, and §4.4's rule is that the AST converter may not have an
             * arm that produces nothing.
             */
            auto result = ssa[inst.results.get(ssa, 0)];
            auto repId = repOf(*result);

            switch(written.op) {
                case mir::BinaryOp::Add: case mir::BinaryOp::Sub: case mir::BinaryOp::Mul:
                case mir::BinaryOp::Div: case mir::BinaryOp::Rem:
                case mir::BinaryOp::And: case mir::BinaryOp::Or:  case mir::BinaryOp::Xor:
                case mir::BinaryOp::Shl: case mir::BinaryOp::Shr: case mir::BinaryOp::Sar:
                case mir::BinaryOp::Eq:  case mir::BinaryOp::Ne:
                case mir::BinaryOp::Lt:  case mir::BinaryOp::Le:
                case mir::BinaryOp::Gt:  case mir::BinaryOp::Ge:
                    break;

                /*
                 * The five this host has no operator for, which are a call to a helper the two
                 * generators share - `JsShared`'s families, reached by tag.
                 *
                 * The helper answers the raw operation at the width and the coercion below puts it
                 * in the type's own normal form, which is what lets one function serve both
                 * signednesses. The same division of labour the shipping backend's `rotateCall` and
                 * `bitOpCall` make, and the reason those are now written once.
                 */
                case mir::BinaryOp::Rol:
                case mir::BinaryOp::Ror:
                case mir::BinaryOp::BitsUpTo:
                case mir::BinaryOp::GatherBits:
                case mir::BinaryOp::ScatterBits: {
                    auto op = written.op == mir::BinaryOp::Rol        ? JsHelperOp::Rol
                            : written.op == mir::BinaryOp::Ror        ? JsHelperOp::Ror
                            : written.op == mir::BinaryOp::BitsUpTo   ? JsHelperOp::BitsUpTo
                            : written.op == mir::BinaryOp::GatherBits ? JsHelperOp::GatherBits
                                                                      : JsHelperOp::ScatterBits;

                    auto node = builder.add<JsInstHelper>(repId, where, op);
                    node->arguments.push(target.arena, lhs - base());
                    node->arguments.push(target.arena, rhs - base());
                    lhs->uses++;
                    rhs->uses++;

                    return define(inst, coerced(base()[node->result], repId, where));
                }

                default:
                    report(mir::binaryOpName(written.op), where);
                    return;
            }

            auto node = builder.add<JsInstBinary>(repId, where, written.op, lhs - base(),
                                                  rhs - base());
            auto value = base()[node->result];

            // A comparison answers a `Bool` and needs no reduction; everything else is reduced into
            // its own type's range - see `needsCoercion`.
            if(mir::isComparison(written.op)) return define(inst, value);
            define(inst, coerced(value, repId, where));
            return;
        }

        case ssa::Inst::Unary: {
            auto& written = (ssa::InstUnary&)inst;
            auto result = ssa[inst.results.get(ssa, 0)];
            auto repId = repOf(*result);

            /*
             * `represent` over **storage** - the same storage under another occurrence, and no
             * instruction at all.
             *
             * `lower/inst.cpp` answers the operand's own value for every `represent` and says why: a
             * view record and the view are one address-only value laid out one way, and a pointer
             * restated at a result that names its origin is the same register. The same holds here
             * and is easier to get wrong, because a place and its content have *different*
             * representations on this target: `%41: %I32 address = represent %7` over a `%7` that
             * became a place read as a load of a number and then converted it to a host array, and
             * the store through `%41` wrote a number into a place typed as the row. Every `return`
             * and `yield` out of an iterator or lens hand-over slot is that shape, which is what the
             * fifteen `store` verifier failures in J65's sweep were.
             */
            if(written.op == mir::UnaryOp::Represent &&
               (result->convention == ssa::Convention::Address ||
                loanDenotesStorage(result->occurrence))) {
                auto named = at(*ssa[written.value.value()]).place;
                if(named.valid()) {
                    definePlace(inst, named);
                    return;
                }

                /*
                 * An operand that is a **reference carried as one value** - a block argument whose
                 * manifestation needed a cell, which has no place until something asks. The place
                 * is the `Cell` over it, the storage the descriptor names, and not a `Value` place
                 * over a conversion of it: that relabelled the descriptor as the `Bool` it points
                 * at and the store through the result was refused as "a write into storage that is
                 * a host value". `Packed.yana`'s `loaned` is the shape once `pick` is inlined - the
                 * chosen reference arrives at a join as a block argument and is `represent`ed at
                 * the owner form before it is written through (J84).
                 */
                auto& operandRow = at(*ssa[written.value.value()]);
                if(operandRow.one && target.rep(base()[operandRow.one]->rep).kind == JsRepKind::Cell) {
                    auto place = placeOrFail(written.value.value(), where);
                    if(!place.valid()) return;

                    definePlace(inst, place);
                    return;
                }

                /*
                 * And an operand whose representation is **multi-part**, which is where one value
                 * has to exist.
                 *
                 * `represent` from a window value to a `%Array(a)` address is the shape - a
                 * reborrow of a window taken as storage, which `joinWith`'s `copy(separator)` and
                 * `runMain`'s `cases` both reach. The parts have no address, so this is the one
                 * position a window materializes at that J71 did not remove: `placeOf` builds the
                 * view object once and names a `Value` place over it, which is exactly the place
                 * the window had before it was flat.
                 *
                 * Asked of the **representation** and not of the parts table, which is J78's
                 * correction and was a wrong answer rather than a missing one. A window reaches
                 * this node as parts *usually* and as the one host value they materialize into
                 * whenever something upstream already asked for one - a `load` whose address is a
                 * place rather than parts is the case, and it is reached exactly when the call
                 * below it is left out of line. With the table as the test the node fell past both
                 * arms into the scalar `Represent`, which relabelled the view object as the
                 * `%Array(a)` row; the place built over it then held a bare host array and the
                 * `split` that read the window back out of it is what the verifier reported.
                 * `placeOf` answers all three carriers, so asking it is what makes the arm say what
                 * it means: whichever way the window is in hand, a place over it is the address.
                 */
                if(partsRepOf(*ssa[written.value.value()]).valid()) {
                    auto place = placeOrFail(written.value.value(), where);
                    if(!place.valid()) return;

                    definePlace(inst, place);
                    return;
                }
            }

            /*
             * `represent` between two occurrences of one **multi-part** value - the parts, unchanged.
             *
             * `slice` builds an owned `ArrayView` and a `represent` relabels it as the window, so
             * this is the node between every window's construction and its first use. Taken as a
             * value it would materialize the object and split it back apart one instruction later,
             * which is the allocation the flattening is about.
             *
             * `requireParts` is what makes the other direction work as well: a window that arrived
             * as one value - a call result, a field - is split here and the split is the three
             * property reads it honestly is.
             */
            if(written.op == mir::UnaryOp::Represent) {
                auto multi = partsRepOf(*result);
                if(multi.valid() && !jsPartsAreLanes(target, multi)) {
                    auto supplied = requireParts(written.value.value(), multi, where);
                    if(supplied.valid()) return defineParts(inst, supplied);
                }
            }

            /*
             * `addressOf(x)` - **the place, unchanged**, and no instruction at all.
             *
             * `resolve_ssa/convert.cpp`'s `Node::Address` arm is a `Bitcast` over a value that
             * `arrivesAsAddress`, on the grounds that "the value that arrived already holds exactly
             * the bits the pointer holds and what is left is the change of type". That sentence is
             * true here too and says something stronger: on this target a value that arrives as an
             * address *is* a place, so the change of type is the whole of the node and the place is
             * what the loads and stores behind it want. Reading it as a value instead is what made
             * every `swap` and `exchange` in `lib/` - 144 of J68 §3's 310 diagnostics, the whole of
             * that family - a bitcast between a host reference and a number.
             *
             * Keyed on the *operand* rather than on the result, because the result of this node is
             * an owned `%T` - a trivial scalar occurrence natively - and neither `Address` nor
             * `loanDenotesStorage` answers yes for one. The destination being a `%T` is what
             * separates it from the two bitcasts that genuinely compute: `%T` to `Size`, which is a
             * number an address has no answer for and is still reported below, and `[T *n]` to `%T`,
             * which is one host row under two names and wants the row rather than the slot holding
             * it.
             */
            if(written.op == mir::UnaryOp::Bitcast &&
               target.rep(repId).kind == JsRepKind::HostArray &&
               target.rep(repOf(*ssa[written.value.value()])).kind != JsRepKind::HostArray) {
                auto named = at(*ssa[written.value.value()]).place;
                if(named.valid()) {
                    definePlace(inst, named);
                    return;
                }

                /*
                 * An operand that is a **reference carried as one value** - a block argument whose
                 * manifestation needed a cell, which has no place until something asks. The place
                 * is the `Cell` over it, the storage the descriptor names, and not a `Value` place
                 * over a conversion of it: that relabelled the descriptor as the `Bool` it points
                 * at and the store through the result was refused as "a write into storage that is
                 * a host value". `Packed.yana`'s `loaned` is the shape once `pick` is inlined - the
                 * chosen reference arrives at a join as a block argument and is `represent`ed at
                 * the owner form before it is written through (J84).
                 */
                auto& operandRow = at(*ssa[written.value.value()]);
                if(operandRow.one && target.rep(base()[operandRow.one]->rep).kind == JsRepKind::Cell) {
                    auto place = placeOrFail(written.value.value(), where);
                    if(!place.valid()) return;

                    definePlace(inst, place);
                    return;
                }

                /*
                 * And a reference that arrived as a **value** - a call result, a parameter, a block
                 * argument - is one this target can still name a place over, which is what
                 * `placeOf` does for every other reader of one.
                 *
                 * `exchange(valueAtMut(m, at), v)` is where it arrives: the `&` argument is a call
                 * result rather than a place the caller wrote, so the arm above finds nothing and
                 * the node fell through to "a host reference read as a number". The guard is the
                 * operand's own occurrence rather than its representation, because a `%T` result
                 * says nothing about the operand and a number bitcast into a pointer is a genuine
                 * computation this must not swallow.
                 */
                auto& operand = *ssa[written.value.value()];
                if(operand.convention == ssa::Convention::Address ||
                   loanDenotesStorage(operand.occurrence)) {
                    auto place = placeOrFail(written.value.value(), where);
                    if(!place.valid()) return;

                    definePlace(inst, place);
                    return;
                }
            }

            auto value = valueOf(written.value.value(), where);
            if(!value) {
                internal("met a unary operation with no operand"_v, where);
                return;
            }

            switch(written.op) {
                /*
                 * A conversion between representations, and the one that is not.
                 *
                 * `Represent` is a change of *logical* type that the representation may or may not
                 * follow - a newtype's wrapper, a view's record. Where the two representations agree
                 * it is an alias and emits nothing; where they do not it is the same conversion a
                 * cast is.
                 */
                case mir::UnaryOp::Represent: {
                    if(value->rep == repId) {
                        auto aliased = builder.add<JsInstAlias>(repId, where, value - base());
                        return define(inst, base()[aliased->result]);
                    }

                    auto node = builder.add<JsInstConvert>(repId, where, value - base());
                    node->fromSigned = target.rep(value->rep).isSigned();
                    return define(inst, base()[node->result]);
                }

                case mir::UnaryOp::Convert: {
                    auto node = builder.add<JsInstConvert>(repId, where, value - base());
                    node->fromSigned = target.rep(value->rep).isSigned();
                    return define(inst, base()[node->result]);
                }

                /*
                 * A reinterpretation of the bits - and between two integers of one width on this
                 * target it is the same node a conversion is.
                 *
                 * `bitcast(i) :: USize` is what every bounds test in `lib/` opens with, so the two
                 * being one arm is not a shortcut: the coercion a `Number` of 32 unsigned bits gets
                 * is `x >>> 0`, which is exactly the reinterpretation, and a value-preserving
                 * conversion between two integers of one width is the identity on top of it.
                 *
                 * A bitcast that crosses *domains* is not this. A float's bits are reachable only
                 * through a typed-array pair (`JsShared::floatBitsBuffer`), which is real work and
                 * is reported by name rather than approximated.
                 */
                case mir::UnaryOp::Bitcast: {
                    auto& from = target.rep(value->rep);
                    auto& to = target.rep(repId);

                    auto integral = [](const JsRep& rep) {
                        return (rep.kind == JsRepKind::Number && !rep.isFloat()) ||
                               rep.kind == JsRepKind::BigInt;
                    };

                    /*
                     * Two integers this target holds, at any pair of widths and in either of the
                     * two integer domains: the conversion the destination gets *is* the
                     * reinterpretation, since a narrow value's bits are its two's-complement
                     * pattern in the wider word and `BigInt`/`Number` is a change of domain rather
                     * than of value. That is `bitcastLane`'s own answer - "two integer types of one
                     * width are the same host value in two normal forms" - and `convertExpr` is
                     * where the `bigint` crossing is written once.
                     */
                    /*
                     * A `[T *n]` and a `%T` are one host row under two names, so a reinterpretation
                     * between them moves nothing - which is what `Contiguous([a *n], a).elements`
                     * is written as here and what makes a fixed array reach a window at all.
                     */
                    auto hostLike = [](const JsRep& rep) {
                        return rep.kind == JsRepKind::HostArray || rep.kind == JsRepKind::Erased;
                    };

                    if(hostLike(from) && hostLike(to)) {
                        auto aliased = builder.add<JsInstAlias>(repId, where, value - base());
                        return define(inst, base()[aliased->result]);
                    }

                    /*
                     * A **float meeting an integer**, which no operator here expresses and one
                     * shared typed-array pair does - `JsShared::floatBitsBuffer`.
                     *
                     * The tag says the direction; the width is the value's own, read by the emitter
                     * off the representation, and the two sides are equal by construction because a
                     * `Bitcast` relates two types of one size. Which is also why this asks only
                     * which side is the float: the other is whatever integer of that width the
                     * position wanted, and the coercion below takes it to that type's normal form.
                     */
                    if(from.isFloat() != to.isFloat()) {
                        auto node = builder.add<JsInstHelper>(repId, where,
                            from.isFloat() ? JsHelperOp::FloatToBits : JsHelperOp::BitsToFloat);
                        node->arguments.push(target.arena, value - base());
                        value->uses++;

                        return define(inst, coerced(base()[node->result], repId, where));
                    }

                    /*
                     * A host reference read as a number, which is what `addressOf` and a pointer
                     * comparison are - and what this target has no answer for rather than no
                     * implementation of. A host object has no address, so there is no number those
                     * bits could be; the four fixtures that reach it are about native addressing.
                     */
                    if(hostLike(from) != hostLike(to)) {
                        report("a bitcast between a host reference and a number, which this "
                               "target has no addresses for"_v, where);
                        return;
                    }

                    if(!integral(from) || !integral(to)) {
                        report("a bitcast that crosses the number, bigint and float domains"_v, where);
                        return;
                    }

                    auto node = builder.add<JsInstConvert>(repId, where, value - base());
                    node->fromSigned = from.isSigned();
                    return define(inst, base()[node->result]);
                }

                case mir::UnaryOp::Neg:
                case mir::UnaryOp::Not:
                case mir::UnaryOp::Abs:
                case mir::UnaryOp::Sqrt:
                case mir::UnaryOp::Trunc:
                case mir::UnaryOp::Floor:
                case mir::UnaryOp::Ceil:
                case mir::UnaryOp::Round: {
                    auto node = builder.add<JsInstUnary>(repId, where, written.op, value - base());
                    return define(inst, coerced(base()[node->result], repId, where));
                }

                // The four counts, on the binary families' terms - the helper answers the raw count
                // and the coercion puts it at the operand's own type. A `U64`'s count is a `bigint`
                // even though it is at most 64, because that is what its type is here.
                case mir::UnaryOp::CountBits:
                case mir::UnaryOp::LeadingZeros:
                case mir::UnaryOp::TrailingZeros:
                case mir::UnaryOp::ByteSwap: {
                    auto op = written.op == mir::UnaryOp::CountBits    ? JsHelperOp::CountBits
                            : written.op == mir::UnaryOp::LeadingZeros ? JsHelperOp::LeadingZeros
                            : written.op == mir::UnaryOp::TrailingZeros ? JsHelperOp::TrailingZeros
                                                                        : JsHelperOp::ByteSwap;

                    auto node = builder.add<JsInstHelper>(repId, where, op);
                    node->arguments.push(target.arena, value - base());
                    value->uses++;

                    return define(inst, coerced(base()[node->result], repId, where));
                }

                default:
                    report(mir::unaryOpName(written.op), where);
                    return;
            }
        }

        case ssa::Inst::Select: {
            auto& written = (ssa::InstSelect&)inst;
            auto condition = valueOf(written.condition.value(), where);
            auto whenTrue = valueOf(written.whenTrue.value(), where);
            auto whenFalse = valueOf(written.whenFalse.value(), where);
            if(!condition || !whenTrue || !whenFalse) {
                internal("met a select with no operands"_v, where);
                return;
            }

            auto repId = repOf(*ssa[inst.results.get(ssa, 0)]);
            auto node = builder.add<JsInstSelect>(repId, where, condition - base(),
                                                  whenTrue - base(), whenFalse - base());
            define(inst, base()[node->result]);
            return;
        }

        case ssa::Inst::Map: {
            auto& written = (ssa::InstMap&)inst;
            auto selector = valueOf(written.selector.value(), where);
            if(!selector) {
                internal("met a map with no selector"_v, where);
                return;
            }

            auto repId = repOf(*ssa[inst.results.get(ssa, 0)]);

            /*
             * An arm whose answer is not a **literal** - a record or a fixed array of constants,
             * which is most of what a placed table is for natively.
             *
             * `JsInstMap` compares against literals because that is what makes the emitted chain one
             * expression; an object is a value with an instruction behind it, so the chain is
             * written out of `Select`s instead. Both spell the same ternary and the difference is
             * where the arms are computed: the arms of a select chain are built before it and only
             * one of them is kept, which is `lib/Date/Format.yana`'s `datePartsOf` building four
             * tuples where it uses one.
             *
             * **Not a table**, which is what the same node is natively. A slot of a placed table is
             * copied out of static storage into the result, and the JavaScript counterpart - one
             * module-level row of frozen objects - would hand the caller the table's own object, so
             * a write through the binding it fills would change what every later call answers. A
             * table plus a clone at every read is what that costs, which is the allocation this
             * form already pays without the module-level row.
             */
            auto literalArms = true;
            for(auto entry: written.entries.contents(ssa)) {
                if(!constantLiteral(entry.value, repId)) literalArms = false;
            }

            auto literalFallback = !written.fallback.isSet() ||
                                   bool(constantLiteral(written.fallback, repId));

            if(!literalArms || !literalFallback) {
                /*
                 * A **closed** map's fallback is its last arm, on `NodeMap::closed`'s own terms -
                 * the selector is a discriminant and the arms are every constructor, so the last
                 * test is one whose failure cannot happen.
                 */
                auto count = U32(written.entries.size());
                auto closed = written.closed && count && !written.fallback.isSet();

                // What the chain answers when no test holds, as a constant - so that an arm
                // answering the same is neither built nor tested for (J88). `_ -> Pair {...}` over
                // four constructors was four tests, each building the same record.
                auto otherwise = closed ? written.entries.get(ssa, count - 1).value : written.fallback;

                SmallArray<JsValue*, 8> answers;
                SmallArray<bool, 8> repeats;
                for(U32 i = 0; i < count; i++) {
                    auto entry = written.entries.get(ssa, i);
                    auto repeat = (!closed || i + 1 < count) && otherwise.isSet() &&
                                  ssa::sameConstant(decl, entry.value, otherwise);
                    repeats.push(repeat);
                    answers.push(repeat ? nullptr : constantOf(entry.value, repId, where));
                }

                auto answer = closed ? answers[answers.size() - 1]
                                     : constantOf(written.fallback, repId, where);
                if(!answer) {
                    report("a map with no fallback this target can build"_v, where);
                    return;
                }

                auto boolRep = jsNumberRep(target, NumberDomain::Bool01, 1);
                auto arms = U32(answers.size() - (closed ? 1 : 0));

                for(U32 i = arms; i > 0; i--) {
                    auto entry = written.entries.get(ssa, i - 1);
                    if(repeats[i - 1]) continue;
                    if(!answers[i - 1]) return;

                    auto same = binaryOf(mir::BinaryOp::Eq, boolRep, selector,
                                         builder.number(F64(entry.selector), selector->rep, where),
                                         where);
                    answer = selectOf(same, answers[i - 1], answer, repId, where);
                    if(!answer) return;
                }

                define(inst, answer);
                return;
            }

            auto node = builder.add<JsInstMap>(repId, where, selector - base());

            for(auto entry: written.entries.contents(ssa)) {
                // The arms, as the numbers the emitted chain compares against.
                JsMapEntry row;
                row.from = F64(entry.selector);
                row.to = constantLiteral(entry.value, repId).unwrap();
                node->entries.push(target.arena, row);
            }

            /*
             * A **closed** map has no fallback: its selector is a discriminant and the arms are
             * every constructor, so the last test is one whose failure cannot happen. The last arm
             * becomes the `else` rather than a test of its own, which is both correct and one
             * comparison shorter - `mir::NodeMap`'s `closed` is the statement this reads.
             */
            auto fallback = constantLiteral(written.fallback, repId);

            if(!fallback && written.closed && node->entries.size()) {
                auto last = node->entries.size() - 1;
                node->fallback = node->entries.get(base(), last).to;
                node->closed = true;
                node->closedFrom = node->entries.get(base(), last).from;
                node->entries.remove(base(), last);
                define(inst, base()[node->result]);
                return;
            }

            if(!fallback) {
                report("a map with no literal fallback"_v, where);
                return;
            }

            node->fallback = fallback.unwrap();
            define(inst, base()[node->result]);
            return;
        }

        case ssa::Inst::Aggregate:
            convertAggregate((ssa::InstAggregate&)inst);
            return;

        /*
         * The end of one object's life.
         *
         * Derived reclamation of GC storage emits nothing - §3.9 - so what is left is the authored
         * half, which has to run in OSSA order and which this tier cannot name yet. Reported rather
         * than skipped, because a teardown that silently does not run is a resource leak the fixture
         * suite has no way to see.
         */
        case ssa::Inst::Drop: {
            auto& written = (ssa::InstDrop&)inst;
            if(!mir::ownershipOf(decl, written.held.type).needsTeardown()) return;

            auto place = placeOrFail(written.address.value(), where);
            if(!place.valid()) return;

            emitTeardown(place, written.held.type, true, true, where);
            return;
        }

        case ssa::Inst::FunValue: {
            auto& written = (ssa::InstFunValue&)inst;
            auto repId = repOf(*ssa[inst.results.get(ssa, 0)]);
            auto& rep = target.rep(repId);

            if(rep.kind != JsRepKind::GcObject) {
                report("a function value of this representation"_v, where);
                return;
            }

            auto code = builder.add<JsInstFunCode>(target.shape(rep.shape).properties[0].rep,
                                                   where, written.callee);
            auto codeValue = base()[code->result];

            JsValue* environment = nullptr;
            if(written.capturing) {
                auto place = placeOrFail(written.environment.value(), where);
                if(!place.valid()) return;

                /*
                 * The environment word at the **callee's** parameter zero, not at whatever the
                 * place happens to read as.
                 *
                 * A lifted lambda's environment is an ordinary parameter with an ordinary plan, and
                 * on this target that plan is a `Cell` wherever the record is not a host object -
                 * an environment holding one captured `Int` is a number, and a body that writes
                 * through it needs somewhere to rebind. Reading the place instead put the *number*
                 * in the pair while the lambda read `env.$o[env.$k]`, which `ClosureDrop.yana`
                 * answers wrong rather than refusing.
                 */
                auto plan = jsAbiOf(target, written.callee, where);
                auto& parameters = target.abi(plan).parameters;

                environment = parameters.size() && parameters[0].kind == JsPassingKind::Cell
                    ? referenceTo(place, where)
                    : builder.load(place, where);

                if(!environment) return;
            } else {
                JsConstant nothing;
                nothing.kind = JsConstant::Null;
                environment = builder.constant(nothing,
                                               target.shape(rep.shape).properties[1].rep, where);
            }

            auto pair = builder.add<JsInstObject>(repId, where, rep.shape);
            pair->values.push(target.arena, codeValue - base());
            pair->values.push(target.arena, environment - base());
            codeValue->uses++;
            environment->uses++;

            define(inst, base()[pair->result]);
            return;
        }

        case ssa::Inst::Call:
            convertCall((ssa::InstCall&)inst);
            return;

        case ssa::Inst::Host:
            convertHost((ssa::InstHost&)inst);
            return;

        /*
         * A measurement of a type - two of the five are compile-time answers about ownership and
         * three are about bytes, which this target has an answer for exactly where the value has a
         * typed row to be an element of.
         *
         * `jsScalarBytes` is that answer and its header says who is entitled to give it. What
         * reaches this on a JavaScript build is never a user's `sizeOf` - that declaration is
         * `@platform(native)` - but the *compiler's* own measurement of a type it can see: the
         * width `bitWidth(x: Size)` subtracts from is `sizeOf(Size) << 3`, which is how
         * `expand_provided.cpp` writes a target-width integer's width down without knowing the
         * target. Thirty-nine of J68 §3's 310 diagnostics were that one expression.
         *
         * An aggregate and a type variable have none, and the sentence is the shipping backend's:
         * how many bytes a host object occupies is the engine's answer rather than this compiler's.
         * Align and stride are the same number as the size here, because a typed row's element is
         * its own alignment and its own stride - there is no padding on this target to make them
         * differ.
         */
        case ssa::Inst::Metric: {
            auto& written = (ssa::InstMetric&)inst;
            auto repId = repOf(*ssa[inst.results.get(ssa, 0)]);

            switch(written.metric) {
                case mir::TypeMetric::TrivialCopy:
                case mir::TypeMetric::HasTeardown: {
                    auto wanted = written.metric == mir::TypeMetric::TrivialCopy
                        ? kDescriptorTrivialCopy : kDescriptorHasTeardown;

                    /*
                     * The two rows of metric.def that are facts rather than measurements, and the
                     * one pair a *descriptor* has to carry: a body compiled once is asked them about
                     * a type it was not compiled for, and neither is derivable from a value in hand.
                     */
                    if(!mir::isConcreteType(decl, written.of)) {
                        auto slot = U32(0);
                        if(!variableSlot(written.of, slot)) {
                            report("a measurement of a type a body compiled once cannot see"_v,
                                   written.of, where);
                            return;
                        }

                        auto descriptor = genSlot(slot, where);
                        auto cell = tableCell(descriptor, wanted, where);
                        if(!cell) return;

                        return define(inst, coerced(cell, repId, where));
                    }

                    auto answer = written.metric == mir::TypeMetric::TrivialCopy
                        ? mir::isTriviallyCopyable(decl, written.of)
                        : mir::hasTeardown(decl, written.of);

                    return define(inst, builder.number(answer ? 1 : 0, repId, where));
                }
                default: {
                    /*
                     * A measurement of a type the body cannot see - the descriptor's, where the
                     * descriptor has it.
                     *
                     * The two facts about ownership are cells of it; the three about bytes are not,
                     * because a byte count is the engine's answer here whatever the type is. So an
                     * erased size is reported at the site exactly as a concrete one over a host
                     * object is, which is the same refusal said in one place.
                     */
                    if(!mir::isConcreteType(decl, written.of)) {
                        report("a size, alignment or stride of a type a body compiled once cannot "
                               "see"_v, written.of, where);
                        return;
                    }

                    if(auto bytes = jsScalarBytes(target, jsRep(target, written.of, where))) {
                        // At the metric's own representation, which for a 64-bit answer is a
                        // `bigint` - the reduction the position applies is `BigInt.asIntN`, which
                        // throws on a `number` rather than converting one.
                        if(target.rep(repId).kind == JsRepKind::BigInt) {
                            JsConstant wide;
                            wide.kind = JsConstant::BigInt;
                            wide.bits = bytes;
                            wide.isSigned = target.rep(repId).isSigned();
                            return define(inst, builder.constant(wide, repId, where));
                        }

                        return define(inst, builder.number(F64(bytes), repId, where));
                    }

                    report("a size, alignment or stride of a type, which on this target is the "
                           "engine's answer rather than this compiler's"_v, written.of, where);
                    return;
                }
            }
        }

        case ssa::Inst::Native:
            report(mir::nativeOpName(((ssa::InstNative&)inst).op), where);
            return;

        /*
         * A const parameter read as a value in a body compiled once - one cell, and no step past it.
         *
         * A type variable's slot holds a descriptor a metric then indexes into; a const one holds
         * the number itself, so the erased read of a count is *shorter* than the erased read of a
         * size rather than longer. `codegen/js/place.cpp`'s `genConstValue` says the same.
         */
        case ssa::Inst::GenValue: {
            auto& written = (ssa::InstGenValue&)inst;
            auto slot = U32(0);

            if(!variableSlot(written.variable, slot)) {
                report("a const parameter read in a body compiled once"_v, where);
                return;
            }

            auto cell = genSlot(slot, where);
            if(!cell) return;

            return define(inst, coerced(cell, repOf(*ssa[inst.results.get(ssa, 0)]), where));
        }

        case ssa::Inst::RunSlots:
            report("a run of slots"_v, where);
            return;

        case ssa::Inst::Atomic:
        case ssa::Inst::AtomicOk:
            report("an atomic operation"_v, where);
            return;

        case ssa::Inst::Fma:
            report("a fused multiply-add"_v, where);
            return;

        case ssa::Inst::VecSplat:
        case ssa::Inst::VecLane:
        case ssa::Inst::VecWithLane:
        case ssa::Inst::VecShuffle:
        case ssa::Inst::VecReduce:
            report("a vector operation"_v, where);
            return;

        case ssa::Inst::Return:
        case ssa::Inst::Jump:
        case ssa::Inst::Branch:
        case ssa::Inst::Switch:
        case ssa::Inst::Unreachable:
        case ssa::Inst::KindCount:
            // Terminators are converted by `convertTerminator`, which is the one caller that has the
            // block in hand and can connect the edges.
            return;
    }
}

void Conversion::convertTerminator(ssa::Inst& inst, JsBlock* block) {
    auto where = inst.source;

    switch(inst.kind) {
        case ssa::Inst::Jump: {
            auto& written = (ssa::InstJump&)inst;

            SmallArray<JsValue*, 8> values;
            collectEdge(written.to, where, values);

            auto node = builder.add<JsInstJump>(JsRepId(), where);
            supplyEdge(node->to, block, written.to, where, toBuffer(values));
            return;
        }

        case ssa::Inst::Branch: {
            auto& written = (ssa::InstBranch&)inst;
            auto condition = valueOf(written.condition.value(), where);
            if(!condition) {
                internal("met a branch with no condition"_v, where);
                return;
            }

            SmallArray<JsValue*, 8> whenTrue;
            SmallArray<JsValue*, 8> whenFalse;
            collectEdge(written.whenTrue, where, whenTrue);
            collectEdge(written.whenFalse, where, whenFalse);

            auto node = builder.add<JsInstBranch>(JsRepId(), where, condition - base());
            supplyEdge(node->whenTrue, block, written.whenTrue, where, toBuffer(whenTrue));
            supplyEdge(node->whenFalse, block, written.whenFalse, where, toBuffer(whenFalse));
            return;
        }

        case ssa::Inst::Switch: {
            auto& written = (ssa::InstSwitch&)inst;
            auto selector = valueOf(written.selector.value(), where);
            if(!selector) {
                internal("met a switch with no selector"_v, where);
                return;
            }

            Array<SmallArray<JsValue*, 8>> arms;
            auto sources = written.cases.pointer(ssa);
            for(Size i = 0; i < written.cases.size(); i++) {
                arms.push(SmallArray<JsValue*, 8> {});
                collectEdge(sources[i].edge, where, arms[arms.size() - 1]);
            }

            SmallArray<JsValue*, 8> fallback;
            collectEdge(written.fallback, where, fallback);

            auto node = builder.add<JsInstSwitch>(JsRepId(), where, selector - base());

            for(auto entry: written.cases.contents(ssa)) {
                JsSwitchCase row;
                row.value = F64(entry.value);
                node->cases.push(target.arena, row);
            }

            auto items = node->cases.pointer(base());
            for(Size i = 0; i < written.cases.size(); i++) {
                supplyEdge(items[i].edge, block, sources[i].edge, where, toBuffer(arms[i]));
            }

            supplyEdge(node->fallback, block, written.fallback, where, toBuffer(fallback));
            return;
        }

        case ssa::Inst::Return: {
            auto& written = (ssa::InstReturn&)inst;
            auto& abi = target.abi(body.abi);

            if(abi.result.kind == JsPassingKind::Omitted) {
                builder.add<JsInstReturn>(JsRepId(), where);
                return;
            }

            if(!written.values.size()) {
                internal("met a return with no value where the ABI declares one"_v, where);
                return;
            }

            // Before the `return` exists, for `collectEdge`'s reason: reading a place may emit a
            // load, and one emitted after a terminator is in a block that has ended.
            auto value = valueAs(written.values.get(ssa, 0).value(),
                                 carriedRepOf(target, abi.result), where);
            if(!value) {
                internal("met a return with no value"_v, where);
                return;
            }

            auto node = builder.add<JsInstReturn>(JsRepId(), where);
            node->value = value - base();
            value->uses++;
            return;
        }

        case ssa::Inst::Unreachable:
            builder.add<JsInstUnreachable>(JsRepId(), where);
            return;

        default:
            internal("met a terminator it has no arm for"_v, where);
            return;
    }
}


bool Conversion::run() {
    planDemand();

    // The side tables for this body - one row per SSA value and one per SSA block, on storage the
    // widest body so far already grew. See `JsConvertScratch`.
    target.scratch.reset(from->values.size(), from->blocks.size());

    /*
     * One JSIR block per live SSA block, all of them before any instruction.
     *
     * §5.3: "create all block arguments first, and then convert instructions in source order". Phis
     * do not need an eager lookup of back-edge definitions because edge operands are attached after
     * every target block and argument exists.
     */
    for(U32 i = 0; i < from->blocks.size(); i++) {
        auto source = from->block(ssa, BlockId(i));
        if(source->deleted) continue;

        auto block = builder.addBlock(source->name);
        target.scratch.blocks[i] = block - base();
    }

    if(!from->entry) {
        internal("met a body with no entry block"_v, kNullLocation);
        return false;
    }

    auto entrySource = ssa[from->entry];
    auto entry = blockFor(entrySource->index.index);
    body.entry = entry - base();

    /*
     * The entry block's arguments are the emitted parameters, in ABI order.
     *
     * An `Omitted` parameter has no JS position at all, so the SSA argument it stands for becomes a
     * void and every use of it is a use of nothing. A `Cell` one is the box, and the place is built
     * lazily by `placeOf` when something reads through it.
     */
    auto& abi = target.abi(body.abi);
    builder.switchTo(entry);

    /*
     * The generic environment, in front of everything the signature wrote - the erased ABI's one
     * position, and the same one `lower/erase.cpp` puts it in.
     *
     * It stands for no SSA argument at all, which is why it is added here rather than inside the
     * loop: the entry block's arguments are the *declaration's* parameters and the table is not one
     * of them. `JsAbi::takesGenEnv` is what both ends read, so a caller that passed one and a body
     * that did not take one cannot happen.
     */
    if(abi.takesGenEnv) {
        genEnv = builder.addArgument(entry, jsGenTableRep(target));
        body.parameters.push(target.arena, genEnv - base());

        if(body.function) {
            if(auto gen = decl[body.function]->gen) genSchema = decl[gen]->schema;
        }
    }

    for(U32 i = 0; i < entrySource->argumentCount(); i++) {
        auto argument = ssa[entrySource->argumentAt(ssa, i)];

        if(i >= abi.parameters.size()) {
            internal("met an entry argument with no parameter"_v, kNullLocation);
            return false;
        }

        auto& passing = abi.parameters[i];
        if(passing.kind == JsPassingKind::Omitted) {
            setVoid(argument->id.index);
            continue;
        }

        /*
         * A multi-part parameter is `positions` emitted parameters - §3.8's rule stated at the
         * entry block, which is where an ABI and a block argument list are the same thing.
         *
         * Registered as parts rather than as one value, so a body that only computes with them
         * never materializes a row: `fn f(v: Vec(Int)) = v + v` takes four arguments, adds four
         * times and returns one row, and the one row is the `return`'s.
         */
        if(passing.kind == JsPassingKind::Parts) {
            SmallArray<JsValue*, 16> lanes;

            for(U16 part = 0; part < passing.positions; part++) {
                auto value = builder.addArgument(entry, partRepAt(target, passing.rep, part));
                body.parameters.push(target.arena, value - base());
                lanes.push(value);
            }

            setParts(argument->id.index, makeParts(passing.rep, held(lanes)));

            /*
             * A **reference** that crossed flat is an `Element` place over its two halves, which is
             * where the second half of J71's saving is: the callee reads `owner[key]` where it read
             * `x.$o[x.$k]`, and the descriptor is built again only if something hands the reference
             * on as one value - which `valueOf` does out of the parts above.
             */
            if(isCellParts(passing.rep)) {
                auto content = target.rep(jsStoredRep(target, passing.rep)).content;

                /*
                 * And a **narrow** one that crossed with its scale is a range of `owner[key]` at
                 * that scale - the shipping backend's `refShift`, as a place. Every read decodes and
                 * every write read-modify-writes, including when the caller's storage was a whole
                 * local, which is what a scale of one (or zero, for a signed content) says.
                 */
                if(passing.positions == 3) {
                    auto& held = target.rep(content);
                    auto width = held.domain == NumberDomain::Bool01 ? 1u : U32(held.bits);
                    setPlace(argument->id.index,
                             builder.addScaledRange(lanes[0], lanes[1], lanes[2], content, width,
                                                    held.domain == NumberDomain::Signed));
                    continue;
                }

                setPlace(argument->id.index,
                           builder.addElement(lanes[0], lanes[1], content));
            }

            continue;
        }

        auto value = builder.addArgument(entry, carriedRepOf(target, passing));
        setOne(argument->id.index, value - base());
        body.parameters.push(target.arena, value - base());

        if(passing.kind == JsPassingKind::Cell) {
            setPlace(argument->id.index, builder.addCell(value, passing.rep));
            continue;
        }

        /*
         * A parameter whose address leaves this body, boxed - the demand pass's answer, and the
         * same one an escaping local gets (see `Inst::Alloc`).
         *
         * A host object is already its own reference and `placeOver` answers a `Value` place over
         * it, so only a parameter carried as something else needs one; `jsManifest`'s `needsCell`
         * says the same thing from the other side.
         */
        if(!escapes[argument->id.index]) continue;

        // `jsIsItsOwnStorage` and not the three kinds written out again - the same question
        // `referenceTo` asks of a `Value` place, and two spellings of it is two answers waiting to
        // drift.
        if(jsIsItsOwnStorage(target, value->rep)) continue;

        auto boxShape = jsBoxShape(target, value->rep);
        auto boxRep = jsBoxRep(target, value->rep);

        auto box = builder.add<JsInstObject>(boxRep, kNullLocation, boxShape);
        box->values.push(target.arena, value - base());
        value->uses++;

        setPlace(argument->id.index, builder.addProperty(base()[box->result], boxShape, 0));
    }

    /*
     * The order first, because which reference arguments are only a name for one value is decided
     * over the blocks it reaches - see `findSameReferenceArguments`.
     */
    Array<U32> order;
    reversePostorder(ssa, *from, order);
    findSameReferenceArguments(order, entrySource);

    // And every other block's, one per SSA argument that is not a void.
    for(U32 i = 0; i < from->blocks.size(); i++) {
        auto source = from->block(ssa, BlockId(i));
        if(source->deleted || source == entrySource) continue;

        auto block = blockFor(i);

        for(auto argumentPointer: source->arguments(ssa)) {
            auto argument = ssa[argumentPointer];
            auto repId = repOf(*argument);

            if(!keepsArgument(*this, *argument)) {
                setVoid(argument->id.index);
                continue;
            }

            auto manifest = jsManifest(target, argument->occurrence, kNullLocation);
            auto multi = partsRepOf(*argument);

            /*
             * A multi-part block argument is `parts` target arguments - §3.8, and the whole of what
             * fixes the shipping backend's tenth known gap.
             *
             * A vector carried around a loop is `lanes` arguments and `lanes` edge operands, so an
             * accumulator in a vector loop builds nothing per iteration. The shipping backend
             * materializes there because storage on that target holds one value and a mutable local
             * is storage; here the promotion above this tier has already made the accumulator a
             * block argument, and a block argument is where this representation is at its strongest.
             */
            if(!manifest.needsCell && multi.valid() && !carriesStorage(*argument)) {
                auto& rep = target.rep(multi);
                SmallArray<JsValue*, 16> lanes;

                for(U16 part = 0; part < rep.partCount; part++) {
                    lanes.push(builder.addArgument(block, target.partList[rep.firstPart + part]));
                }

                setParts(argument->id.index, makeParts(multi, held(lanes)));
                continue;
            }

            // `jsStoredRep`, for the reason a place's content is one: an argument that carries
            // storage carries the one host value in it, not the parts the value has.
            auto held = manifest.needsCell ? jsCellRep(target, manifest.rep)
                                           : jsStoredRep(target, repId);

            auto value = builder.addArgument(block, held);
            setOne(argument->id.index, value - base());
        }
    }

    /*
     * The bodies, in reverse postorder.
     *
     * Unreachable blocks are absent from that order and are converted afterwards in table order -
     * the same two-pass shape `lower/gen.cpp` uses, and for the same reason: a block nothing reaches
     * still has to exist so that an edge naming it is not dangling.
     */
    IndexSet reached;
    reached.reset(from->blocks.size());

    auto convertBlock = [&](U32 i) {
        auto source = from->block(ssa, BlockId(i));
        auto block = blockFor(i);
        builder.switchTo(block);

        // A reference argument that is a name for one value is that value's row - re-projected
        // here first when it was a projection, which is the load a use in this block would do.
        for(auto argumentPointer: source->arguments(ssa)) {
            auto argument = ssa[argumentPointer];
            auto same = at(*argument).sameAs;
            if(same == ~0u) continue;

            auto value = from->value(ssa, ValueId(same));
            reprojectIfStale(*value);

            auto& was = at(*value);
            auto& row = at(*argument);
            row.form = was.form;
            row.one = was.one;
            row.place = was.place;
            row.parts = was.parts;
            row.nicheTag = was.nicheTag;
        }

        for(auto instPointer: source->instructions(ssa)) {
            auto inst = ssa[instPointer];
            if(ssa::isTerminator(inst->kind)) convertTerminator(*inst, block);
            else convertInst(*inst);
        }

        // A block whose SSA terminator was missing would be a half-built body; the SSA verifier has
        // already refused one, so this is an assertion rather than a repair.
        if(!block->terminator(base())) {
            internal("met a block with no terminator"_v, kNullLocation);
        }
    };

    for(auto i: order) {
        reached.set(i, true);
        convertBlock(i);
    }

    for(U32 i = 0; i < from->blocks.size(); i++) {
        if(reached[i] || from->block(ssa, BlockId(i))->deleted) continue;
        convertBlock(i);
    }

    return !failed;
}

bool convertFunction(JsTarget& target, DeclPtr<mir::Function> function, JsBody& body) {
    auto declared = target.decl[function];
    if(!declared->ssa) return false;

    body.function = function;
    body.abi = jsAbiOf(target, function, declared->source);

    Conversion conversion(target, body, target.ssa[declared->ssa]);
    return conversion.run();
}

bool convertConstant(JsTarget& target, const mir::Constant* constant, JsRepId rep, bool boxed,
                     LocationId where, JsBody& body) {
    Conversion conversion(target, body, nullptr);
    return conversion.runConstant(constant, rep, boxed, where);
}

bool convertEnvironmentTeardown(JsTarget& target, DeclPtr<mir::Function> lambda, JsBody& body) {
    auto declared = target.decl[lambda];
    auto environment = declared->environment;
    if(!environment) return false;

    body.function = lambda;

    /*
     * A plan of its own - one value in, nothing out - through `jsTeardownBodyAbi`'s memo rather than
     * pushed here, because this function is not a declaration and `abiOfFunction` is keyed on
     * handles. Interning it on the position keeps two teardown bodies that are entered the same way
     * naming one plan, which is what a frozen ABI table is for.
     *
     * The parameter is the *lambda's* parameter zero, copied: what `$fd` hands this is exactly the
     * word the pair holds, which is the word the lifted body takes.
     */
    auto lambdaAbi = jsAbiOf(target, lambda, declared->source);
    if(!target.abi(lambdaAbi).parameters.size()) return false;

    auto passing = target.abi(lambdaAbi).parameters[0];
    body.abi = jsTeardownBodyAbi(target, passing);

    Conversion conversion(target, body, nullptr);
    return conversion.runEnvironmentTeardown(environment, declared->source, passing);
}

bool convertTypeTeardown(JsTarget& target, DeclPtr<mir::Type> type, bool drop, bool reclaim,
                         LocationId where, JsBody& body) {
    // The value itself, owned - which is what a `TypeDrop` hands over and what the walk reads
    // fields out of. One plan, interned for the same reason the environment teardown's is: a sum
    // asked for drop-only and asked for both is two bodies entered across one convention.
    auto passing = JsPassing { JsPassingKind::One, jsRep(target, type, where), 1 };
    body.abi = jsTeardownBodyAbi(target, passing);

    Conversion conversion(target, body, nullptr);
    return conversion.runTypeTeardown(type, drop, reclaim, where, passing);
}

} // namespace jsir
