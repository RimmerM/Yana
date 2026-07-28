#include "analyze_pass.h"

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

    Array<ModulePtr<Value>> pending;
    Array<ModulePtr<Value>> seen;
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

            if(storedInto || derivedFrom) {
                auto root = rootLocal(analysis, carrier);

                if(root != maxLimit<U32>) {
                    auto slot = analysis.function.localAt(analysis.local, root);

                    // Into an environment, or into the function value the environment ends up in.
                    if(storedInto && (slot.closureEnv || isFunction(analysis.global, slot.type)) && slot.value) {
                        pending.push(slot.value);
                    }

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
 * Use after move, and the moves that cannot be represented at all.
 */
void checkMoves(Analysis& analysis) {
    for(Size i = 0; i < analysis.instructionCount; i++) {
        auto& instruction = *analysis.local[analysis.order[i]];
        auto& states = analysis.stateBefore[i];
        auto& effects = analysis.effects[i];

        if(instruction.kind == Value::Move) {
            auto& moved = (InstMove&)instruction;

            // A partial move would leave the slot half-owned, and every later drop of it would
            // have to know which half. That is a drop flag per field and a drop that runs over a
            // subset of members - real work, deferred deliberately rather than approximated.
            if(moved.place.projections.isNotEmpty()) {
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

    // A borrow rooted in a local, a global, or a sunk parameter has no caller-side root that could
    // keep it alive, which is a different mistake from being rooted in the wrong argument.
    if(summary.invalidRoot) {
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
void checkClosureEnvironments(Analysis& analysis) {
    auto global = analysis.global;

    for(Size l = 0; l < analysis.localCount; l++) {
        auto slot = analysis.function.localAt(analysis.local, U32(l));
        if(!slot.closureEnv || !analysis.escaped[l]) continue;
        if(!slot.type || global[slot.type]->kind != Type::Tup) continue;

        auto source = slot.value ? analysis.local[slot.value]->source : analysis.function.source;

        for(auto field: ((TupType*)global[slot.type])->fields.contents(global)) {
            if(!isBorrow(global, field.type)) continue;

            report(analysis, "this closure outlives the frame that built it, so it cannot capture %@ by reference - the enclosing binding is %@, and a capture of mutable storage is always by reference (Design-Memory §8)"_v,
                   source, analysis.context.findName(field.name),
                   ((BorrowType*)global[field.type])->mut ? "mutable"_v : "borrowed from somewhere else"_v);
        }
    }
}
