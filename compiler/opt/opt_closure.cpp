#include "opt_pass.h"
#include "../resolve/analyze.h"

/*
 * What a drop site can settle about a function value's teardown before it runs.
 *
 * A function value is `{code, env}` and nothing else, so the teardown written for one has to work
 * from those two words alone: it tests for an environment, finds the closure header the code word
 * leads to, and calls the two halves the header names. That is the right shape for a value that
 * arrived from somewhere - a parameter, a call's result - and it is a load, a test, a branch, six
 * instructions of header arithmetic natively and two indirect calls, to answer a question that at
 * most drop sites has one answer at compile time.
 *
 * `analyze_drop.cpp`'s `closureTeardown` already answers it for the shape it can see: one local, one
 * `alloc`, one write of each word, so the drop is rewritten to the environment's own long before
 * this stage runs. What it cannot answer is anything that *merges* - `let f = if c then A else B` -
 * and ClosureTeardown.yana calls that out as the reason a function value carries its teardown at run
 * time at all.
 *
 * It is a weaker reason than it looks. Which lambda a phi selects is a run-time fact; whether there
 * is anything to *run* need not be, because a closure over nothing droppable has a header holding
 * `teardown$none` twice. Where every lambda reaching a drop is one of those, the drop is not a
 * shorter sequence but an absent one - and a closure over copied captures is the ordinary closure.
 *
 * ## What is read
 *
 * The header table itself, rather than the environment type it was built from. It is the authority:
 * `setClosureRelease` overwrites the reclaim slot for an environment escape analysis put on the
 * heap, so the type answers one thing and the slot another, and the slot is what the generic
 * teardown would have called. A lambda that captured nothing has no header at all, which is the same
 * answer arrived at from the other side - see resolveLambda.
 *
 * ## The three answers, and the one that is missing
 *
 * `Empty` deletes the drop. `Present` means the header is there every time, so the *test* in front
 * of the search is dead and the site takes the form of the glue that has none - see
 * funTeardownKnownHeader. `Unknown` is both "this cannot be enumerated" and "the arms disagree", and
 * a merge of a lambda that has a header with one that has none is precisely what the test is for.
 *
 * What is missing between those two is a *direct call*, where the arms agree on a non-empty entry so
 * the search collapses as well - and it is unreachable rather than deferred. Entries are interned
 * per environment type and a lambda's environment tuple is minted fresh rather than interned
 * (resolveLambda says why), so two lambdas capturing identical shapes hold two distinct types and
 * therefore two distinct entries; they never agree. The one reaching definition that is left - no
 * merge at all - is the case `closureTeardown` took before this ever saw it.
 *
 * Measured rather than assumed: a build that emitted the call form logged it zero times over the
 * fixture suite, against three deletions. ClosurePhiDrop.yana is that boundary written down, and
 * the count it asserts is what an over-eager version of this pass would get wrong.
 *
 * ## What `Present` costs
 *
 * A second interned glue per function type, because the test lives inside a body shared by every
 * site rather than at any site. So it is a trade rather than a saving: one small function against a
 * load, a compare and a branch at every drop of that type - and natively against two basic blocks as
 * well, since the branch is a diamond the site no longer contains. It pays from the second site on,
 * and ClosurePhiDrop.yana with its single site is the worst case rather than a typical one.
 *
 * ## Where this runs, and the bound that comes with it
 *
 * From `dischargeDrop`, as the first thing it tries - so this is discharging a drop the same way
 * that function does, into the calls it stands for, where those turn out to be none. It cannot be
 * later: discharge is what removes `InstDrop` from the IR, and it is first in the stage because
 * everything after it is written against a body that has none.
 *
 * The bound is that it is also before `inlineCalls`. A closure built in one function and dropped in
 * another is two frames here and one afterwards, so the shape inlining exposes is not one this sees.
 * Recovering it would mean recognizing a *call* to the generic teardown rather than a drop, which is
 * a different pass against a different input; nothing here is in its way.
 */

namespace {

/*
 * Which lambdas still need the header in front of them.
 *
 * The teardown above is not the only thing that stops reading a header. `analyze_drop`'s
 * `closureTeardown` has been rewriting a contained closure's drop to its *environment's* since long
 * before this file existed - by name, so the header is never consulted - and nothing then went back
 * to ask whether the header was still worth emitting. `closureNeedsTeardown` answers a question
 * about the environment's *type* ("could there be anything to run"), which is the wrong question:
 * the right one is whether any path leads to the table at run time. A closure over a `Guard` that is
 * built, called and dropped in one frame emits a header table, a store of it, and the `dropAt$`
 * entry wrapper that only the table can reach - three things, all dead, per lambda.
 *
 * ## What makes this decidable
 *
 * A header is found through the code word and through nothing else, and a code word is produced by
 * `InstSymbol` and by nothing else. So the question is answerable by enumerating symbols: a lambda's
 * header is unreachable when every `InstSymbol` naming it goes straight into the code word of a
 * function value that never leaves the frame it was built in.
 *
 * Which is a stricter test than it may look, and deliberately. The obvious cheaper one - "the
 * environment is frame-placed, so the value dies here, so only this frame's drops can see it" - is
 * **wrong**, and it is wrong in a shape the fixtures contain:
 *
 *     fn consume(->f: (Int) -> Int) -> Int = f(1)
 *     fn make(id: Int) -> Int:
 *         let g = Guard {id: id}
 *         return consume((x: Int) -> x + g.id)
 *
 * `consume` returns before `make` does, so the environment is contained and escape analysis
 * correctly leaves it on the stack - and ownership of the *value* still moves to another frame,
 * where the drop is generic and reads the header. Environment placement answers a different question
 * from this one. The `Move` that hands the value over is what this sees, and it is what makes the
 * whitelist below a whitelist rather than a list of exclusions.
 */
struct HeaderReach {
    OptContext& opt;

    /*
     * Whether a function value's storage never leaves the frame that filled it.
     *
     * Everything admitted here either writes one of the two words or reads them where they are; a
     * use this does not recognize is a use that could hand the value to something whose drop is not
     * in view, so the answer is no. A `Drop` is on the list of things it is not: one that survived
     * `closureTeardown` is one that goes through the generic teardown, which is the reader this is
     * looking for.
     */
    bool contained(U32 index, ModulePtr<Value> storage) {
        for(auto user: opt.local[storage]->uses(opt.local)) {
            auto& instruction = *opt.local[user];

            auto names = false;
            eachPlace(instruction, [&](const Place& place) {
                if(place.root == PlaceRoot::Local && place.local == index) names = true;
            });

            switch(instruction.kind) {
                case Value::Init: {
                    if(!names) return false;

                    auto& write = (InstInit&)instruction;
                    if(write.place.projections.size() != 1) return false;

                    auto projection = write.place.projections.get(opt.local, 0);
                    if(projection.kind != ProjectionKind::Field) return false;
                    if(projection.index != FunValueLayout::kCode &&
                       projection.index != FunValueLayout::kEnv) return false;

                    break;
                }

                // Reading a word where it lies. Calling the value is this plus the `CallDyn` below,
                // and neither moves anything.
                case Value::LoadPlace: {
                    if(!names) return false;

                    auto& read = (InstLoadPlace&)instruction;
                    if(read.place.projections.isEmpty()) return false;

                    auto projection = read.place.projections.get(opt.local, 0);
                    if(projection.kind != ProjectionKind::Field) return false;
                    if(projection.index != FunValueLayout::kCode &&
                       projection.index != FunValueLayout::kEnv) return false;

                    break;
                }

                // Calling it. The value is the callee rather than an argument, which is the one way
                // a function value is named by a call without being handed over.
                case Value::CallDyn:
                    if(names) return false;
                    if(((InstCallDyn&)instruction).callable != storage) return false;
                    break;

                default:
                    return false;
            }
        }

        return true;
    }

    // The code word this symbol becomes, and whether it stays put. A symbol used for anything other
    // than filling one is a code word going somewhere this cannot follow.
    bool containedSymbol(Function& function, ModulePtr<Value> symbol) {
        auto value = opt.local[symbol];
        if(value->useCount() != 1) return false;

        auto& user = *opt.local[value->useAt(opt.local, 0)];
        if(user.kind != Value::Init) return false;

        auto& write = (InstInit&)user;
        if(write.value != symbol) return false;
        if(write.place.root != PlaceRoot::Local) return false;
        if(write.place.projections.size() != 1) return false;

        auto projection = write.place.projections.get(opt.local, 0);
        if(projection.kind != ProjectionKind::Field) return false;
        if(projection.index != FunValueLayout::kCode) return false;
        if(write.place.local >= function.localCount()) return false;

        auto slot = function.localAt(opt.local, write.place.local);
        if(!slot.value || opt.local[slot.value]->kind != Value::Alloc) return false;

        return contained(write.place.local, slot.value);
    }
};

/*
 * What a drop site knows about the headers that can reach it.
 *
 * Three answers rather than two, because the generic teardown does two things and each is separately
 * avoidable. `Empty` means it would find nothing and the drop is deleted. `Present` means it would
 * find something every time, so the test in front of the search is dead and the site can take the
 * form of the glue that does not have one - which is the whole of what the site can save, since the
 * entries themselves never agree (see the header).
 *
 * `Unknown` is both "this cannot be enumerated" and "the arms disagree". A merge of a lambda that
 * has a header with one that has none is exactly what the test exists for.
 */
enum class Reach: U8 {
    Unknown,
    Empty,
    Present,
};

static Reach mergeReach(Reach a, Reach b) {
    if(a == b) return a;
    return Reach::Unknown;
}

struct Devirtualize {
    OptContext& opt;

    // The reclaim half is the collector's on a managed target, so a header slot holding one is not
    // something this has to account for - the same carve-out `dischargeDrop` applies to the generic
    // form. It is what lets a closure whose only teardown is a reclaim vanish there and not natively.
    bool collected;

    // The phis already on the path. A loop-carried function value walks back into one, and a
    // definition seen twice contributes what it contributed the first time. Four inline: this is the
    // depth of a merge of merges, not the size of anything in the program.
    SmallArray<ModulePtr<Value>, 4> visiting;

    // What tearing down a value built by this lambda would find.
    Reach lambdaReach(ModulePtr<Function> lambda) {
        // No header is the answer for a lambda that captured nothing, and it is an answer rather
        // than a gap: the value's environment word is null, so the generic teardown tests it, takes
        // the other branch and returns. See resolveLambda, where the two go together.
        auto header = opt.local[lambda]->closureHeader;
        if(!header) return Reach::Empty;

        auto table = opt.local[header];
        auto slots = ClosureHeaderFields::kCount;
        if(!table->isTable || table->table.size() < slots) return Reach::Unknown;

        auto isEmpty = [&](U16 slot) {
            auto cell = table->table.get(opt.local, slot);
            if(cell.kind != TableCell::Function) return true;
            return cell.function() == opt.program.emptyTeardown;
        };

        if(isEmpty(ClosureHeaderFields::kTeardown)) return Reach::Empty;

        /*
         * Whether the table this describes is one the backends actually put anywhere.
         *
         * `closureHeaderRead` is markClosureHeaders' answer, and a lambda it cleared has no header
         * at run time whatever the table says - it is computed before this pass, so it is final
         * here. `Unknown` rather than a guess: the cost of being wrong is a teardown reading a
         * header that was never written.
         *
         * The other half of this test was "the drop slot is empty", which the JS side gates its
         * store on - a header whose drop half is empty is present natively and absent there. With
         * one slot per target that question is the emptiness test above: the slot holds the drop
         * half on a managed target and the merged teardown natively, so each backend's answer is
         * about the table its own build emits.
         */
        if(!opt.local[lambda]->closureHeaderRead) return Reach::Unknown;
        return Reach::Present;
    }

    /*
     * The lambda a slot this frame filled holds, tested for having nothing to tear down.
     *
     * Every write to the slot has to be one of the two the closure builder emits, which is what
     * makes "what is in the code word" a question with an answer at all. A second write of either
     * word, a write of the whole value, or an alias something else could write through, and what the
     * slot holds is settled somewhere this cannot see.
     */
    Reach localReach(U32 index, ModulePtr<Value> storage) {
        ModulePtr<Function> lambda = nullptr;

        for(auto user: opt.local[storage]->uses(opt.local)) {
            auto& instruction = *opt.local[user];

            // Whether this use is *of the slot* rather than of the value that made it. A phi
            // alternative and a call argument name the allocation and touch no place at all, and
            // neither can change what the words hold.
            auto names = false;
            eachPlace(instruction, [&](const Place& place) {
                if(place.root == PlaceRoot::Local && place.local == index) names = true;
            });

            if(!names) continue;

            switch(instruction.kind) {
                case Value::Init: {
                    auto& write = (InstInit&)instruction;
                    if(write.place.projections.size() != 1) return Reach::Unknown;

                    auto projection = write.place.projections.get(opt.local, 0);
                    if(projection.kind != ProjectionKind::Field) return Reach::Unknown;

                    // The environment word decides nothing here: what is in it matters only to a
                    // teardown that runs, and the code word is what says whether one does.
                    if(projection.index == FunValueLayout::kEnv) break;
                    if(projection.index != FunValueLayout::kCode || lambda) return Reach::Unknown;

                    auto& code = *opt.local[write.value];
                    if(code.kind != Value::Symbol) return Reach::Unknown;

                    lambda = ((InstSymbol&)code).callee;
                    if(!lambda) return Reach::Unknown;

                    break;
                }

                // Reads, and the relocation that is a read with a name. None of them writes a word.
                case Value::LoadPlace:
                case Value::Move:
                case Value::Copy:
                case Value::Drop:
                    break;

                // Anything else naming this slot - an assignment, a borrow handed to a callee, an
                // address, an exchange - is a way for the words to become something other than what
                // the writes above put there.
                default:
                    return Reach::Unknown;
            }
        }

        return lambda ? lambdaReach(lambda) : Reach::Unknown;
    }

    // What every definition that can reach one function value agrees on. A merge is as good as its
    // weakest arm, which is the whole of what a phi contributes here.
    Reach reachOf(ModulePtr<Value> value) {
        if(!value) return Reach::Unknown;
        auto& instruction = *opt.local[value];

        if(instruction.kind == Value::Phi) {
            // A definition already on the path contributed its answer the first time, and `merge`
            // has no identity to answer with - so the arms that are not on the path decide, which is
            // what skipping it leaves.
            for(auto seen: visiting) if(seen == value) return Reach::Unknown;

            if(visiting.size() >= 4) return Reach::Unknown;
            visiting.push(value);

            Maybe<Reach> merged;
            for(auto input: ((InstPhi&)instruction).inputs.contents(opt.local)) {
                auto arm = reachOf(input.value);
                merged = merged ? Just(mergeReach(merged.unwrap(), arm)) : Just(arm);
                if(merged.unwrap() == Reach::Unknown) break;
            }

            visiting.pop();
            return merged ? merged.unwrap() : Reach::Unknown;
        }

        if(instruction.kind != Value::Alloc) return Reach::Unknown;

        auto& allocation = (InstAlloc&)instruction;
        if(allocation.local >= opt.function->localCount()) return Reach::Unknown;

        return localReach(allocation.local, value);
    }
};

}

void markClosureHeaders(OptContext& opt) {
    HeaderReach reach { opt };

    // Optimistic, and then every symbol that escapes containment takes one back. A lambda no symbol
    // names has no way to become a value at all, so it keeps the `true` it arrived with rather than
    // being cleared by a scan that found nothing - the two are the same emitted output, and this way
    // round the clearing is only ever done by evidence.
    HashMap<U32, bool> contained;
    Array<ModulePtr<Function>> seen;

    for(auto module: opt.program.modules) {
        for(auto pointer: module->functionOrder.contents(opt.local)) {
            auto function = opt.local[pointer];
            if(function->signature || function->blocks.isEmpty()) continue;

            opt.function = function;

            for(auto blockPointer: function->blocks.contents(opt.local)) {
                for(auto instructionPointer: opt.local[blockPointer]->instructions(opt.local)) {
                    auto& instruction = *opt.local[instructionPointer];
                    if(instruction.kind != Value::Symbol) continue;

                    auto lambda = ((InstSymbol&)instruction).callee;
                    if(!lambda || !opt.local[lambda]->closureHeader) continue;

                    auto entry = contained.add(U32(lambda));
                    if(!entry.existed) {
                        *entry.value = true;
                        seen.push(lambda);
                    }

                    auto symbol = (ModulePtr<Value>)(&instruction - opt.local);
                    if(!reach.containedSymbol(*function, symbol)) *entry.value = false;
                }
            }
        }
    }

    for(auto lambda: seen) {
        if(contained.get(U32(lambda)).unwrap()) opt.local[lambda]->closureHeaderRead = false;
    }

    /*
     * And then the program-wide form of the same question, which is what makes the answer worth
     * more than the tables it saves.
     *
     * Only a lambda has a header - a thunk over a plain function has a null environment and none by
     * construction - so where no lambda has one that survives, *no function value in the program*
     * can carry a teardown. Every `(a) -> b` teardown is then a call that tests a property nothing
     * has, and every drop of a function value is dead however little the site knows about it. That
     * covers what the per-site analysis cannot: a value in a record field, one reached through a
     * borrow, one inside derived glue.
     *
     * A whole-program fact, and it has to be: a lambda declared in any module can be the one that
     * makes it false. That is the same footing `markProgramReachable` stands on and the same footing
     * this compiler stands on - one program, resolved together.
     *
     * **A lambda the program cannot reach is not in it**, which is that same footing read the other
     * way and is what keeps the fact a property of the program rather than of the standard library.
     * Without the filter, one `iter fn` added to Collections withdraws the elision from every
     * program in the language whether or not the program calls it - measured when the vector
     * iteration protocol landed, as a `drop$`/`reclaim$` pair appearing in four unrelated closure
     * fixtures. `markProgramReachable` has already run and its answer is what `used` holds.
     */
    for(auto module: opt.program.modules) {
        for(auto pointer: module->functionOrder.contents(opt.local)) {
            auto function = opt.local[pointer];
            if(!function->used) continue;

            if(function->closureHeader && function->closureHeaderRead) {
                opt.function = nullptr;
                return;
            }
        }
    }

    opt.program.funValuesCarryTeardown = false;
    opt.function = nullptr;
}

bool devirtualizeClosureDrop(OptContext& opt, Block& block, Size index, InstDrop& drop) {
    auto type = placeType(*opt.module, *opt.function, drop.place);
    if(!type || opt.global[type]->kind != Type::Fun) return false;

    // Storage this frame also has to hand back is not this shortcut's to take: the release belongs to
    // this place whatever the value in it turned out to be, and deleting the drop would delete it.
    // The same line `closureTeardown` draws, for the same reason.
    if(drop.releaseStorage) return false;

    /*
     * Nothing in the program can carry a teardown, so this one has nothing to find - see
     * markClosureHeaders.
     *
     * Ahead of everything below because it needs none of it. What the site knows about the value is
     * the whole subject of the analysis that follows, and here the answer does not depend on the
     * value at all: a field of a record, a place behind a borrow, the subject of derived glue.
     */
    if(!opt.program.funValuesCarryTeardown) {
        opt.ir().eraseInstruction((ModulePtr<Inst>)(&drop - opt.local));
        opt.changed = true;
        return true;
    }

    // The whole slot, which is what the drop pass emits for a value whose lifetime ended. A function
    // value reached through a field or a pointer is not one whose definitions this can enumerate:
    // what settles it is a write somewhere else.
    if(drop.place.root != PlaceRoot::Local) return false;
    if(drop.place.projections.isNotEmpty()) return false;
    if(drop.place.local >= opt.function->localCount()) return false;

    auto slot = opt.function->localAt(opt.local, drop.place.local);

    Devirtualize devirtualize { opt, opt.repr.target.family == TargetFamily::Managed };
    auto reach = devirtualize.reachOf(slot.value);

    if(reach == Reach::Empty) {
        opt.ir().eraseInstruction((ModulePtr<Inst>)(&drop - opt.local));
        opt.changed = true;
        return true;
    }

    if(reach != Reach::Present) return false;

    /*
     * The header is there every time, so the test in front of the search is dead.
     *
     * Which is all this site can save. Which lambda the value holds is still a run-time fact and the
     * entry is still reached through the header, so the search itself stays - see the header. What
     * changes is the callee: the same glue, interned separately, without the branch.
     *
     * One rewrite, because the drop names one function. This used to be two, and the second had to
     * *carry the pairing across* rather than recompute it: a reclaim naming the same function as the
     * drop is one traversal serving both halves, and rewriting the two independently would have
     * broken that identity and run the walk twice. There is no identity left to preserve.
     *
     * Answered `false` rather than `true`, because the drop has not been taken. Whoever asked
     * expands it in the ordinary way from here - `dischargeDrop`, or `inlineTeardown`, which asks
     * this first so that what it copies is the body without the branch - and what they expand is now
     * one call shorter.
     *
     * Only where the answer actually changes, because there are two askers and the first of them
     * runs inside a fixpoint: reporting a rewrite for a drop already holding the known form would be
     * a round of work per round for a body nothing is doing anything to.
     */
    if(drop.teardown) {
        auto known = funTeardownKnownHeader(*opt.module, type, drop.source);
        if(known == drop.teardown) return false;

        drop.teardown = known;
    }

    opt.changed = true;
    return false;
}
