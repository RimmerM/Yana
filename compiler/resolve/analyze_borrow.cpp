#include "analyze_pass.h"
#include "generic.h"

/*
 * The four checks.
 *
 * Every one of them is stated over the facts the passes before it computed and computes nothing of
 * its own, which is what makes them one file: a diagnostic here is either a rule written wrong or a
 * fact computed wrong, and the two are now in different files.
 *
 * What is deliberately *not* checked is recorded at the end of analyze.cpp.
 */

// One borrow, with the extent over which it holds. Exclusivity is a question about two of these
// overlapping in both extent and place.
struct LiveBorrow {
    ModulePtr<Inst> instruction;
    U32 from = 0;
    U32 to = 0;
    bool mut = false;
};

/*
 * Do two places name storage that can overlap?
 *
 * Same root, and one projection path a prefix of the other. `x.a` and `x.b` are disjoint, `x` and
 * `x.a` are not, and that precision is what makes a `&` parameter usable at all - `moveBy` borrows
 * `p.x` and `p.y` in turn and neither is a conflict with the other.
 *
 * Two deliberate conservatisms. A step through a Deref or an Index leads somewhere this analysis
 * cannot name, so from there on the two are assumed to overlap. And places rooted in raw pointers
 * never conflict with anything, including each other: `%T` carries no aliasing information by
 * construction, and reporting on it would be inventing a rule the language says it does not have.
 */
static bool placesOverlap(ModuleBase base, Place lhs, Place rhs) {
    if(lhs.root != rhs.root) return false;
    if(lhs.root == PlaceRoot::Pointer) return false;
    if(lhs.root == PlaceRoot::Local && lhs.local != rhs.local) return false;
    if(lhs.root == PlaceRoot::Global && lhs.global != rhs.global) return false;

    auto left = lhs.projections;
    auto right = rhs.projections;
    auto leftContents = left.contents(base);
    auto rightContents = right.contents(base);

    auto leftIterator = leftContents.begin();
    auto rightIterator = rightContents.begin();

    for(Size i = 0; i < min(left.size(), right.size()); i++) {
        auto a = *leftIterator;
        auto b = *rightIterator;
        ++leftIterator;
        ++rightIterator;

        if(a.kind == ProjectionKind::Deref || a.kind == ProjectionKind::Index) return true;
        if(b.kind == ProjectionKind::Deref || b.kind == ProjectionKind::Index) return true;
        if(a.kind != b.kind || a.index != b.index) return false;
    }

    // One path ran out, so it is a prefix of the other and names storage containing it.
    return true;
}

/*
 * Exclusivity - Design.md's second question, and the only one of the four with an extent to
 * compute first.
 */

/*
 * How far a borrow's extent reaches.
 *
 * To the last instruction that consumes the borrow value - and then, if one of those was a call
 * that may hand it back, to the last use of what the call produced. That second clause is the
 * whole of Design.md's "the caller conservatively keeps every member borrowed until the last use
 * of all result borrows", and it is why the loan on `objects` does not end at the call to
 * `getMutableEntry` but at the last use of the entry it returned.
 *
 * Transitive, because a caller may hand the result on again: a chain of selectors keeps the
 * original root borrowed for the whole chain. The `seen` list is what makes a value used by two
 * calls, or a loop, terminate instead of walking the graph forever.
 */
static U32 lastUseOf(Analysis& analysis, ModulePtr<Inst> pointer) {
    auto found = analysis.indexOf.get(U32(pointer));
    auto last = found ? found.unwrap() : 0;

    ValueList pending;
    ValueList seen;
    pending.push((ModulePtr<Value>)pointer);

    while(pending.size()) {
        auto value = pending.pop().unwrap();

        auto visited = false;
        for(auto& entry: seen) visited = visited || entry == value;
        if(visited) continue;
        seen.push(value);

        for(auto user: analysis.local[value]->uses.contents(analysis.local)) {
            auto index = analysis.indexOf.get(U32(user));
            if(index && index.unwrap() > last) last = index.unwrap();

            auto& instruction = *analysis.local[user];

            /*
             * A borrow written into a closure's environment is live for as long as the closure is.
             *
             * Design-Memory §8: "a by-reference capture is a mutable borrow live for as long as the
             * closure, so while such a closure exists the captured binding may not be borrowed again
             * by the enclosing frame". The extent therefore follows the storage the borrow was
             * written into, then the address of that storage, then the function value the address
             * became a word of - which is exactly the chain a capture takes to reach a `calldyn`.
             */
            Place carrier;
            auto storedInto = instruction.kind == Value::Init && firstPlace(instruction, carrier);
            auto derivedFrom = (instruction.kind == Value::Address || instruction.kind == Value::Borrow) &&
                               firstPlace(instruction, carrier);

            /*
             * A raw pointer read out of borrowed storage is live for as long as whatever holds it.
             *
             * Implementation-Containers.md §4: borrowing `[T]` copies the run's base address and the
             * length into a descriptor, and hands *that* over. The borrow's own last use is the read
             * - so without this the loan would end before the call that is about to index through
             * the address it produced, and the drop pass would be entitled to release the run first.
             *
             * Narrow on purpose. Only a *pointer* read through the borrow extends it, because only a
             * pointer still names the storage after the read; an `Int` copied out of a borrowed
             * record is a value with no relationship to where it came from, and following that would
             * keep every field read's owner borrowed to the end of the function.
             */
            auto aliases = instruction.kind == Value::LoadPlace &&
                           isPointer(analysis.global, instruction.type);

            if(aliases) pending.push((ModulePtr<Value>)user);

            if(storedInto || derivedFrom) {
                auto root = rootLocal(analysis, carrier);

                if(root != maxLimit<U32>) {
                    auto slot = analysis.function.localAt(analysis.local, root);

                    // Into an environment, or into the function value the environment ends up in -
                    // and into anything at all when what is written is one of the aliasing pointers
                    // above, which is how the slice descriptor carries the loan to its call site.
                    auto carries = slot.closureEnv || isFunction(analysis.global, slot.type) ||
                                   isPointer(analysis.global, analysis.local[value]->type);

                    if(storedInto && carries && slot.value) pending.push(slot.value);

                    // Out of an environment: the address is what the function value holds.
                    if(derivedFrom && slot.closureEnv) pending.push((ModulePtr<Value>)user);
                }
            }

            /*
             * A loan handed to a call's `return` group outlives the call, whichever way the callee
             * was named.
             *
             * A direct call reads the group off the callee's summary and a call through a function
             * value reads it off the signature, which is the same contract in the two places it can
             * be written down - and the reason FunArg carries the marker at all. A chain of
             * selectors keeps the original root borrowed for the whole chain either way.
             */
            U64 roots = 0;

            if(instruction.kind == Value::Call) {
                auto summary = summaryOf(analysis, ((InstCall&)instruction).callee);
                if(summary) roots = summary->declaredRoots;
            } else if(instruction.kind == Value::CallDyn) {
                auto signature = ((InstCallDyn&)instruction).signature;
                if(signature && analysis.global[signature]->kind == Type::Fun) {
                    roots = ((FunType*)analysis.global[signature])->returnRoots;
                }
            }

            if(!roots) continue;

            U16 position = 0;
            auto args = instruction.kind == Value::Call ? &((InstCall&)instruction).args
                                                        : &((InstCallDyn&)instruction).args;

            for(auto arg: args->contents(analysis.local)) {
                if(arg == value && (roots & rootBit(position))) pending.push((ModulePtr<Value>)user);
                position++;
            }
        }
    }

    return last;
}

void checkBorrows(Analysis& analysis) {
    Array<LiveBorrow> borrows;

    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto pointer = analysis.order[i];
        auto& instruction = *analysis.local[pointer];
        if(instruction.kind != Value::Borrow) continue;

        borrows.push(LiveBorrow {
            pointer, U32(i), lastUseOf(analysis, pointer), ((InstBorrow&)instruction).mut,
        });
    }

    for(auto& borrow: borrows) {
        auto& borrowed = (InstBorrow&)*analysis.local[borrow.instruction];

        for(Size i = borrow.from + 1; i <= borrow.to; i++) {
            auto other = analysis.order[i];
            auto& instruction = *analysis.local[other];

            Place places[kMaxPlaces];
            auto touched = instructionPlaces(instruction, places);
            if(!touched) continue;

            auto overlaps = false;
            for(Size p = 0; p < touched; p++) {
                overlaps = overlaps || placesOverlap(analysis.local, borrowed.place, places[p]);
            }

            if(!overlaps) continue;

            // The instructions that consume the borrow reach the storage *through* it, which is
            // the whole point of handing one out rather than a conflict with it.
            auto consumed = false;
            auto uses = analysis.local[borrow.instruction]->uses;
            for(auto user: uses.contents(analysis.local)) {
                if(user == other) consumed = true;
            }

            if(consumed) continue;

            auto otherBorrow = instruction.kind == Value::Borrow;
            auto otherMutable = otherBorrow && ((InstBorrow&)instruction).mut;

            // Two immutable borrows of one place are exactly what borrows are for.
            if(!borrow.mut && otherBorrow && !otherMutable) continue;

            // Reading through a live immutable borrow is fine; it is the mutable one that is
            // exclusive. A write is a conflict with either.
            auto writes = instruction.kind == Value::Assign || instruction.kind == Value::Init ||
                          instruction.kind == Value::Move || otherMutable ||
                          instruction.kind == Value::Address ||
                          instruction.kind == Value::Swap || instruction.kind == Value::Exchange;

            if(!borrow.mut && !writes) continue;

            report(analysis,
                   borrow.mut
                       ? "this use conflicts with a mutable borrow of the same storage, which is exclusive while it is live"_v
                       : "this write conflicts with an immutable borrow of the same storage that is still live"_v,
                   instruction.source);

            note(analysis, "the borrow it conflicts with is here"_v, borrowed.source);
        }
    }
}

/*
 * Whether a move out of this place empties the slot rather than half of it.
 *
 * The empty path is the obvious one. The other is a lone `Downcast`, and it is not an exception to
 * the rule so much as the case where the rule's premise does not hold: a record's storage is a
 * discriminant and one payload, the discriminant owns nothing, and on the path a `Just(->v)` reached
 * there is no second payload to be left behind. So the slot really is empty afterwards, and every
 * later use of it is rejected and every drop of it skipped, which is what "moved" has to mean.
 *
 * Two steps do not qualify, and both matter. `Downcast` then `Field` is one member of a payload
 * written with braces, which is a genuine partial move. `Downcast` then `Deref` is a boxed payload -
 * `project` appends that `Deref` - and there the box is storage of its own: taking the target out
 * leaves the allocation with nothing to free it, since the reclaim that would have is the one this
 * move just told the compiler not to run.
 */
static bool wholeMove(Analysis& analysis, Place place) {
    auto count = place.projections.size();
    if(count == 0) return true;
    if(count > 1) return false;

    return place.projections.get(analysis.local, 0).kind == ProjectionKind::Downcast;
}

/*
 * Ownership leaving the frame through a value that was never moved out of anything.
 *
 * The four points where ownership departs - a write into another place, an exchange, a return, and a
 * phi input - all take a *value*, and transferFrom stops at whichever slot that value is the whole
 * contents of. A projected load has no such slot: `p.first` and the `v` a `Held(v)` pattern binds are
 * both a LoadPlace over a path into a slot somebody else still owns, so the handover finds nothing to
 * mark moved, the source is dropped at its last use, and whoever received the bytes drops them too.
 *
 * That is the same statement `InstMove` makes with a projected place, and it is refused for the same
 * reason - a slot half given away needs a drop flag per field, which the state lattice does not have.
 * The only difference is that `->` says it out loud and this does not, so this is where it has to be
 * noticed. Written as a check rather than as a move: turning these into InstMove would make the
 * *accepted* programs depend on a rule that exists to reject, and a partial move is not something to
 * start representing here.
 *
 * Only droppable types, on the same terms as transferFrom itself. Copying the bytes of something
 * nobody is responsible for is a copy, and that is all `let x = p.count` has ever been.
 */
static void checkTransfer(Analysis& analysis, ModulePtr<Value> value, LocationId source) {
    if(!value || analysis.local[value]->kind != Value::LoadPlace) return;

    auto& place = ((InstLoadPlace*)analysis.local[value])->place;
    if(place.projections.isEmpty()) return;

    // A raw pointer's target is outside the ownership model - `%T` carries no owner - so `return *p`
    // stays what Native needs it to be.
    if(place.root == PlaceRoot::Pointer) return;

    // Asked of the *context* rather than of the type, exactly as sinkValue asks it: an unconstrained
    // `a` owns something inside the body whatever a caller substitutes, and a declared
    // `TrivialCopy(a)` is what makes reading one out a copy. Without that agreement a generic
    // accessor would be rejected here and then compile the copy anyway.
    auto ownership = ownershipIn(analysis.module, functionGen(analysis.global, analysis.function),
                                 analysis.local[value]->type);
    if(!ownership.needsTeardown()) return;

    auto root = rootLocal(analysis, place);
    auto borrowed = place.root == PlaceRoot::Borrow ||
                    (root != maxLimit<U32> && !analysis.tracked[root].owned);

    if(borrowed || place.root == PlaceRoot::Global) {
        report(analysis, "this hands ownership on out of storage this frame only borrows - take the whole value with `->` in the signature, or answer a borrow instead"_v,
               source);
        return;
    }

    // A payload the pivot is the only owner of: the move is representable, it just was not written.
    // Pointing at the spelling is the whole value of separating this case out.
    if(wholeMove(analysis, place)) {
        report(analysis, "this hands ownership on out of a value that still owns it - write `->` on the name that binds it, as in `Just(->v)`, to take it out"_v,
               source);
        return;
    }

    report(analysis, "cannot move a part of a value out of it - move the whole value instead"_v, source);
}

// The value each of the four departure points hands over, or none for everything else.
static ModulePtr<Value> transferredValue(Inst& instruction) {
    switch(instruction.kind) {
        case Value::Init:
        case Value::Assign:
            return ((InstInit&)instruction).value;
        case Value::Exchange:
            return ((InstExchange&)instruction).value;
        case Value::Ret:
            return ((InstRet&)instruction).value;
        default:
            return nullptr;
    }
}

/*
 * Use after move, and the moves that cannot be represented at all.
 */
void checkMoves(Analysis& analysis) {
    // A phi input departs on the edge into the join rather than at the join, which is where
    // attributePhiEdges puts its transfer. The diagnostic points at the phi, because the operand is
    // a value with no instruction of its own to name.
    for(Size b = 0; b < analysis.blockCount(); b++) {
        for(auto phiPointer: analysis.blockAt(b)->phis.contents(analysis.local)) {
            auto& phi = *analysis.local[phiPointer];
            for(auto input: phi.inputs.contents(analysis.local)) {
                checkTransfer(analysis, input.value, phi.source);
            }
        }
    }

    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto& instruction = *analysis.local[analysis.order[i]];
        auto& states = analysis.stateBefore[i];
        auto& effects = analysis.effects[i];

        checkTransfer(analysis, transferredValue(instruction), instruction.source);

        if(instruction.kind == Value::Move) {
            auto& moved = (InstMove&)instruction;

            // A partial move would leave the slot half-owned, and every later drop of it would
            // have to know which half. That is a drop flag per field and a drop that runs over a
            // subset of members - real work, deferred deliberately rather than approximated.
            if(!wholeMove(analysis, moved.place)) {
                report(analysis, "cannot move a part of a value out of it - move the whole value instead"_v,
                       instruction.source);
                continue;
            }

            /*
             * Taking ownership out of storage this frame does not own.
             *
             * A `&` parameter is the case with a local behind it, and the one this check was
             * written for. A borrow root is the same mistake with no local to find: `let &e = xs[i]`
             * names storage the collection owns, and `let ->x = e` would take the element out from
             * under it. A global's storage outlives every frame there is. Both were reaching the
             * state test below and finding nothing to test, because rootLocal has no answer for
             * either - so both were accepted in silence.
             *
             * The borrow half is load-bearing rather than tidy. placeOverwriteDrops releases what a
             * write through a borrow replaces without consulting any state, which is sound only
             * while nothing can empty borrowed storage behind its owner's back. This is what makes
             * that true.
             *
             * A raw pointer root is deliberately not here. `let ->x = *p` is Native taking
             * ownership of memory it is holding the only address of, which is the one thing the
             * module exists to be able to do.
             */
            auto root = rootLocal(analysis, moved.place);
            auto borrowed = moved.place.root == PlaceRoot::Borrow ||
                            (root != maxLimit<U32> && !analysis.tracked[root].owned);

            if(borrowed) {
                report(analysis, "cannot take ownership of borrowed storage - a `&` binding never owns what it refers to"_v,
                       instruction.source);
                continue;
            }

            if(moved.place.root == PlaceRoot::Global) {
                report(analysis, "cannot take ownership of a global - its storage outlives every frame that could take it"_v,
                       instruction.source);
                continue;
            }
        }

        for(auto use: effects.uses) {
            if(states[use] == OwnState::Owned || states[use] == OwnState::Uninitialized) continue;

            auto name = analysis.tracked[use].name;
            auto moved = states[use] == OwnState::Moved;

            if(name) {
                report(analysis,
                       moved ? "%@ has been moved out of and cannot be used again"_v
                             : "%@ may have been moved out of on some paths reaching here"_v,
                       instruction.source, analysis.context.findName(name));
            } else {
                report(analysis,
                       moved ? "this value has been moved out of and cannot be used again"_v
                             : "this value may have been moved out of on some paths reaching here"_v,
                       instruction.source);
            }
        }
    }
}

/*
 * The return-root check (Design.md's "Borrows in return position").
 *
 * The declaration is the contract and the body is what has to fit it, so this compares two things
 * the summary already holds: the group the signature declared, and the roots resolving every return
 * path actually found. Nothing here looks at a callee's body - a call's result arrived with the
 * callee's declared group already mapped through the operands, which is what makes provenance
 * compose transitively through a helper without inspecting one.
 */
void checkReturnRoots(Analysis& analysis) {
    auto& function = analysis.function;
    auto& summary = function.summary;
    if(!summary.returnsBorrow) return;

    auto source = function.source;

    /*
     * A borrow rooted in a local, a global, or a sunk parameter has no caller-side root that could
     * keep it alive, which is a different mistake from being rooted in the wrong argument.
     *
     * Asked only of a result that *is* a reference, while the group check below is asked of one that
     * merely contains one. The asymmetry is provenance being field-insensitive: a record holding both
     * an owned array and a slice has local roots for the array, and they are not a mistake - the
     * array goes with the result. Reporting on them would reject `data Mixed {view: &[Int], owned:
     * [Int]}` for the half that is doing nothing wrong.
     *
     * Nothing is lost by that. The case it would have caught - a record holding a view of *this*
     * frame's container - is checkEscapingViews', which asks about the descriptor's own slot and so
     * is not confused by what else the record contains.
     */
    if(summary.invalidRoot && isBorrowLike(analysis.module, function.returnType)) {
        report(analysis,
               "a borrow returned from this function is rooted in storage the caller does not own - it must come from an argument marked `return`"_v,
               source);
    }

    auto undeclared = summary.actualRoots & ~summary.declaredRoots;
    if(!undeclared) return;

    U16 index = 0;
    for(auto argPointer: function.args.contents(analysis.local)) {
        auto arg = analysis.local[argPointer];

        if(undeclared & rootBit(index)) {
            report(analysis,
                   "a borrow returned from this function is rooted in %@, which the signature did not mark `return`"_v,
                   arg->source, analysis.context.findName(arg->name));
        }

        index++;
    }
}

/*
 * A closure that outlives the frame cannot hold a borrow of it.
 *
 * Design-Memory §8's third case says a closure that must outlive the frame that built it has to own
 * what it captures, and this is where that is checked: the environment escaped, so anything in it
 * that is a `&T` names storage this frame is about to stop guaranteeing. The capture conventions are
 * chosen before any of this is known - a capture is decided at the name that made it, and whether
 * the closure escapes is a whole-function fact - so the two meet here rather than at the lambda.
 *
 * A closure that is merely *called* does not trip this. Nothing marks the environment escaped at an
 * InstCallDyn, deliberately: a lifted body has no way to name its own environment, so it cannot
 * store one, and treating every call as a handover would reject every closure that is used.
 */
/*
 * The one borrow that is still a temporary, escaping.
 *
 * A `&` argument whose declared type is wider than the field's - `increment(&x: Int)` given a
 * `@bits(13)` field - is widened into a temporary and narrowed back when the call returns, because
 * a reference cannot convert. That is right for a callee that reads and writes through it and wrong
 * for one that keeps it: the temporary dies with the frame, and nothing would ever narrow.
 *
 * Nothing about packing is left in this check. A borrow of a narrow field at its *own* type is a
 * reference that carries the field's shift, works wherever the field is, and outlives whatever it
 * likes - see NarrowRef in resolve/lower.cpp. What is reported here is a conversion with nowhere to
 * happen, and the fix is in the signature.
 */
/*
 * A slice may not outlive the container it is a view of - Implementation-Containers.md §4.
 *
 * The descriptor holds a `%T` into the run, and a raw pointer is outside the ownership model by
 * construction, so nothing about the *fields* of a slice says it refers to anything. What says so is
 * Local::viewOf, written where the descriptor was built out of an owned array, and this is the check
 * that spends it: the slice escaped this frame while the array it points into is this frame's, so
 * the frame is about to run that array's teardown and free the run underneath it.
 *
 * Precise rather than provenance-shaped, deliberately. Asking "does the returned value contain a
 * borrow" would have to go through the containment relation, which is field-insensitive - so a
 * record holding both an owned array and a slice would be rejected for the owned half. This asks
 * about the slice's own slot and nothing else, and a slice's slot is only ever a view when this
 * frame made it one.
 *
 * A slice that came *in* as a parameter has no `viewOf` and is not reported here: what it refers to
 * is the caller's, and whether the caller may hand it on is the caller's own return-root check.
 */
void checkEscapingViews(Analysis& analysis) {
    for(Size l = 0; l < analysis.localCount; l++) {
        auto slot = analysis.function.localAt(analysis.local, U32(l));
        if(slot.viewOf == maxLimit<U32> || !analysis.escaped[l]) continue;

        auto viewed = analysis.function.localAt(analysis.local, slot.viewOf);
        auto source = slot.value ? analysis.local[slot.value]->source : analysis.function.source;

        if(viewed.name) {
            report(analysis, "this borrow of %@ outlives the frame that owns it - a slice is a view into the container's storage, and the container is released when this function returns"_v,
                   source, analysis.context.findName(viewed.name));
        } else {
            report(analysis, "this borrow of an array outlives the frame that owns it - a slice is a view into the container's storage, and the container is released when this function returns"_v,
                   source);
        }
    }
}

void checkMaterializedBorrows(Analysis& analysis) {
    for(Size l = 0; l < analysis.localCount; l++) {
        auto slot = analysis.function.localAt(analysis.local, U32(l));
        if(!slot.materialized || !analysis.escaped[l]) continue;

        auto source = slot.value ? analysis.local[slot.value]->source : analysis.function.source;

        report(analysis, "cannot borrow this beyond the call - the callee declared a wider type than the field has, so what it receives is a temporary written back when the call returns, and this callee keeps it. Declare the parameter at the field's own type"_v,
               source);
    }
}

void checkClosureEnvironments(Analysis& analysis) {
    auto global = analysis.global;

    for(Size l = 0; l < analysis.localCount; l++) {
        auto slot = analysis.function.localAt(analysis.local, U32(l));
        if(!slot.closureEnv || !analysis.escaped[l]) continue;
        if(!slot.type || global[slot.type]->kind != Type::Tup) continue;

        auto source = slot.value ? analysis.local[slot.value]->source : analysis.function.source;

        for(auto field: ((TupType*)global[slot.type])->fields.contents(global)) {
            /*
             * A captured slice is a captured reference - see isBorrowLike - and it is reported
             * separately because the two have nothing to say to each other. A `&T` capture is about
             * the *convention* the capture was made under, which is what the enclosing binding's
             * mutability decided; a slice is a reference whatever convention it travelled by, since
             * copying the descriptor copies the address inside it.
             */
            if(isBorrow(global, field.type)) {
                report(analysis, "this closure outlives the frame that built it, so it cannot capture %@ by reference - the enclosing binding is %@, and a capture of mutable storage is always by reference (Design-Memory §8)"_v,
                       source, analysis.context.findName(field.name),
                       ((BorrowType*)global[field.type])->mut ? "mutable"_v : "borrowed from somewhere else"_v);
            } else if(isBorrowLike(analysis.module, field.type)) {
                report(analysis, "this closure outlives the frame that built it, so it cannot capture %@ - it is a slice, which is a view into a container someone else owns, and copying the descriptor copies the address inside it (Design-Memory §8)"_v,
                       source, analysis.context.findName(field.name));
            }
        }
    }
}
