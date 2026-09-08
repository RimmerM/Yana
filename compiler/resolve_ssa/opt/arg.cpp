#include "pass.h"

/*
 * A record parameter that becomes one parameter per field - the shipping stage's `flattenArguments`
 * (`compiler/opt/opt_arg.cpp`), built on this stage's terms.
 *
 * `fn multiply(a: [Int], b: [Int], n: Int)` takes two windows, and a window is a record of a base
 * and a length that `lower/README.md`'s first section passes **by address**. So the callee's loop
 * reads the length out of memory on every iteration, and the bounds test a subscript grafts compares
 * an induction variable against a value nothing can prove invariant - because the pointee of a
 * borrowed parameter has no root in this body, which is exactly why `hoist` declines to move the
 * read and is right to. Passing `a$items` and `a$length` instead makes both of them values, and the
 * check folds against the loop bound the way it does in the shipping stage.
 *
 * ## Why this is a calling convention rather than an optimization
 *
 * A caller and a callee decide it separately and never see each other, so the rule has to be a
 * function of what both of them can read: the declaration. That rules out the two shapes an
 * optimization would naturally have. It cannot be per call site, because a witness slot, a closure
 * and an indirect call have to agree with a direct one; and it cannot depend on the body, because a
 * caller does not have the callee's.
 *
 * It is therefore **one rewrite with two sides**, run once over the whole program in three walks:
 * every plan first, then every call site while every signature still says what it was declared to
 * say, then every signature. Computing the plans first is not only a cache - it is the only way to
 * guarantee that a caller and a callee apply the same plan when a function is usually both, since
 * rewriting a signature is exactly the thing that would change the answer.
 *
 * ## What the ceilings are
 *
 * `kMaxRecordFields` is 12 and `kMaxFlatArity` is 24, which are the shipping pass's numbers and its
 * measurement: past twelve fields the rewrite only pays where most call sites were *constructing*
 * the record, and twenty-four is where extra arguments stop being free. They are stated as constants
 * here rather than re-derived, because the measurement is about the shape of a calling convention
 * and not about which stage produced it.
 *
 * ## What is declined, and why each one
 *
 * A function whose **address** something holds is declined outright, because an address has no
 * declaration at the call site for the rule to be read from. `flattenBlocked` below reproduces
 * `reach.cpp`'s enumeration of exactly that - a witness table slot, a `fun.value`, the allocator and
 * the located check pair, and every teardown, which the *lowering* calls from a type rather than
 * from an instruction. A body compiled once (`GenForm::Erased`), a lifted lambda, a lens and a
 * declaration carrying `@convention` are declined by name for the same sentence: each is called
 * across a shape somebody else decided.
 *
 * Per parameter the whole test is **a plain shared, call-bounded borrow of a window** -
 * `argumentCanFlatten` states each clause with its reason, and the two worth naming here are that
 * the origin is what stands in for the shipping pass's `returnRoot`/`retained` pair, and that the
 * *form* is what keeps the rewrite to the one shape that has a record to take apart.
 *
 * A **leaf** is an integer, a float or a pointer - the three kinds that are certainly a value in a
 * register - held as an owned, trivially copyable field. Everything else has to be a
 * single-constructor record whose fields are themselves leaves, or the whole parameter is declined:
 * half a record flattened is not a convention. That is narrower than the shipping stage's
 * `!isMemoryType`, and deliberately: the manifestation table lives in `lower/` and a pass at this
 * tier that asked it would be answering a representation question one tier above the one that owns
 * it.
 *
 * Nothing here asks whether the parameter's type owes a teardown, which the shipping pass does, and
 * the leaf rule is why it does not have to: a parameter only reaches the rebuild by being a *borrow*
 * whose every leaf is trivially copyable, so what the callee's slot holds is bytes and owes nothing.
 * Asking the record's own type instead would have declined every window, since `Array(a)` owes a
 * teardown and the two words a view of one carries do not.
 *
 * ## What the callee is left with
 *
 * The body is not touched at all. The parameter becomes an `alloc` in the entry block with the
 * fields written into it, and every projection that named the parameter still names the same
 * storage - which is what makes this safe without understanding the body. It is not the
 * pessimisation it looks like: `promote`, `forwardMemory` and `removeDeadStorage` are three of the
 * passes that run over every body afterwards, and a slot written once and read is what all three
 * exist for. A body that genuinely needs the record whole - one that borrows it and passes the
 * borrow on - keeps the storage, and correctly so.
 */

namespace ssa {

namespace {

constexpr Size kMaxRecordFields = 12;
constexpr Size kMaxFlatArity = 24;

// A record nested inside a record is taken apart too, but not indefinitely: the limit is on the
// path length rather than on the field count, which the leaf budget already bounds.
constexpr Size kMaxDepth = 4;

// One projection on the way to a leaf. The record is carried rather than derived, because
// `fieldRecordOf` answers a *view* for a window and the field indices are that record's.
struct Step {
    DeclPtr<mir::Type> record;
    U16 field;
};

struct Leaf {
    Size pathStart = 0;
    Size pathCount = 0;
    mir::Occurrence occurrence;
    StringId name {};
};

/*
 * One signature's replacement.
 *
 * Held flat - the projections of every leaf in one array, the leaves of every argument in another -
 * because the natural shape is an array of arrays and this is read far more often than it is built.
 */
struct Plan {
    SmallArray<Step, 8> paths;
    SmallArray<Leaf, 8> leaves;

    // Per argument, in declaration order. A count of zero is a parameter that stays as it is.
    SmallArray<U32, 8> start;
    SmallArray<U32, 8> count;

    bool any = false;

    void clear() {
        paths.clear();
        leaves.clear();
        start.clear();
        count.clear();
        any = false;
    }

    // What the plan appended for one argument, undone. A refusal partway down leaves projections
    // and leaves that belong to no argument, and a plan describing parameters it does not claim is
    // one the two sides would apply differently.
    void rewind(Size leafCount) {
        while(leaves.size() > leafCount) {
            paths.resize(leaves[leaves.size() - 1].pathStart);
            leaves.pop();
        }
    }
};

/*
 * `a$length`, so that the emitted parameters still say which record they came out of.
 *
 * Through `addQualifiedName` with one segment rather than `addUnqualifiedName`, because that is the
 * overload that *copies*: the unqualified one keeps the pointer it was handed, and a name built in a
 * local buffer then outlives the buffer.
 */
StringId leafName(OptContext& opt, StringId prefix, StringId field, Size index) {
    if(!prefix) return field;

    StringBuilder text;
    text << opt.context.findName(prefix) << "$";

    // An anonymous field has no name, so the position is the only thing left to tell two of them
    // apart - and `{Int, Int}` flattening to two parameters both called `pair` is a signature the
    // reader cannot follow even where the compiler can.
    if(field) {
        text << opt.context.findName(field);
    } else {
        char digits[16];
        auto written = show(U64(index), digits, sizeof(digits));
        text << Buffer<const char> { digits, written };
    }

    return opt.context.addQualifiedName(text.pointer(), text.size(), 1);
}

// Whether one occurrence is a value this stage will pass in a parameter of its own - see the file
// comment for why the kinds are named rather than asked of the manifestation.
bool isLeafOccurrence(OptContext& opt, const mir::Occurrence& occurrence) {
    if(!occurrence.type || !occurrence.contract.isOwned() || occurrence.shape) return false;

    auto kind = opt.decl[occurrence.type]->kind;
    if(kind != mir::Type::Int && kind != mir::Type::Float && kind != mir::Type::Pointer) return false;

    return mir::isTriviallyCopyableAt(opt.decl, occurrence);
}

/*
 * The leaves of one occurrence, appended to the plan.
 *
 * `fieldRecordOf` is the whole of what makes a window work here: it answers `ArrayView(I32)` for an
 * `Array(I32)` occurrence in view form and the type itself for an ordinary record, which is the same
 * record every `field.addr` in the body is already written against.
 */
bool collectLeaves(OptContext& opt, Plan& plan, const mir::Occurrence& occurrence, StringId name,
                   SmallArray<Step, 8>& path, Size depth) {
    if(plan.leaves.size() > kMaxRecordFields) return false;

    if(isLeafOccurrence(opt, occurrence)) {
        Leaf leaf;
        leaf.pathStart = plan.paths.size();
        leaf.pathCount = path.size();
        leaf.occurrence = occurrence;
        leaf.name = name;

        for(auto& step: path) plan.paths.push(step);
        plan.leaves.push(leaf);

        return true;
    }

    if(depth >= kMaxDepth) return false;
    if(occurrence.shape) return false;

    auto record = mir::fieldRecordOf(opt.context, opt.program, occurrence);
    if(!record || opt.decl[record]->kind != mir::Type::Record) return false;

    auto& declared = *(mir::RecordType*)opt.decl[record];

    // One constructor only. A sum has a tag and one shared payload, so "its fields" is not a list a
    // caller may read without knowing which arm it is holding.
    if(declared.cons.size() != 1) return false;
    if(declared.fields.isEmpty()) return false;

    for(U16 i = 0; i < U16(declared.fields.size()); i++) {
        auto field = declared.field(opt.decl, i);

        // A boxed field is a pointer to the value rather than the value, which is a second layout
        // question this pass has no reason to answer.
        if(field.boxed) return false;

        path.push(Step { record, i });
        auto member = leafName(opt, name, field.anonymous ? StringId() : field.name, i);
        auto ok = collectLeaves(opt, plan, field.occurrence, member, path, depth + 1);
        path.pop();

        if(!ok) return false;
    }

    return true;
}

/*
 * Every function this rewrite may not move the signature of.
 *
 * `reach.cpp`'s enumeration minus the one edge that names a callee rather than taking one - an
 * `InstCall` whose `through` is `Declaration`. That is the list to copy because it is the one the
 * program is already trusted to be complete about: a way of reaching a function that is missing from
 * it is a function the linker drops, and the two have to stay in step.
 *
 * Every **teardown** is in it, and so is the entry. A teardown is called by the lowering from the
 * type being destroyed (`emitTeardown`), so no instruction here names it; the entry is called by the
 * runtime stub.
 */
void flattenBlocked(OptContext& opt, HashMap<U32, bool>& blocked) {
    auto hold = [&](DeclPtr<mir::Function> function) {
        if(function) *blocked.add(function.offset).value = true;
    };

    auto& program = opt.program;

    hold(program.entry);

    intrinsic::Known reached[5] = { intrinsic::Known::allocateHeap, intrinsic::Known::freeHeap,
                                    intrinsic::Known::isHeapAddress,
                                    intrinsic::Known::checkFailedAt,
                                    intrinsic::Known::checkConditionAt };

    for(auto role: reached) hold(program.roles.function(role));

    auto holdEnvironment = [&](DeclPtr<mir::GenEnvironment> environment) {
        if(!environment) return;

        for(auto witness: opt.decl[environment]->slots.contents(opt.decl)) {
            for(auto member: witness.functions.contents(opt.decl)) hold(member);
        }
    };

    for(auto modulePointer: program.modules) {
        auto module = opt.decl[modulePointer];

        for(auto functionPointer: module->functions.contents(opt.decl)) {
            auto function = opt.decl[functionPointer];
            if(function->teardown || function->derivation) hold(functionPointer);
            if(!function->ssa) continue;

            auto body = opt.base[function->ssa];

            for(U32 i = 0; i < body->blocks.size(); i++) {
                auto block = body->block(opt.base, BlockId(i));
                if(block->deleted) continue;

                for(auto pointer: block->instructions(opt.base)) {
                    auto inst = opt.base[pointer];

                    if(inst->kind == Inst::Call) {
                        auto& call = (InstCall&)*inst;

                        // A call through a slot or a value names the *signature* it was checked
                        // against, and what runs is whatever the caller of the erased body put in
                        // the slot - so both are addresses as far as this rule is concerned.
                        if(call.through != mir::CallThrough::Declaration) hold(call.callee);
                        holdEnvironment(call.environment);
                        continue;
                    }

                    if(inst->kind == Inst::FunValue) hold(((InstFunValue&)*inst).callee);
                }
            }
        }
    }
}

// Whether anything about the function itself rules it out - see the file comment for what each of
// these is and why an address rather than a name is what they have in common.
bool functionCanFlatten(OptContext& opt, DeclPtr<mir::Function> pointer, mir::Function& function,
                        HashMap<U32, bool>& blocked) {
    if(blocked.get(pointer.offset)) return false;
    if(!function.ssa || !function.reachable) return false;

    // A generic body's parameters have no layout to take apart, a code word's first parameter is a
    // convention rather than a declaration, and a lens's continuation is a position the desugaring
    // numbers. A declared `@convention` is somebody else's ABI.
    if(mir::isGeneric(opt.decl, function.gen) || function.environment) return false;
    if(function.lens || function.iterator) return false;
    if(function.continuationArg != mir::Function::kNoContinuation) return false;
    if(function.convention) return false;
    if(function.intrinsic != intrinsic::Op::Count) return false;
    if(function.klass) return false;

    // A summary that gave up says nothing about whether a borrow of a parameter outlives the call,
    // which is the question below that has no other answer.
    return true;
}

bool argumentCanFlatten(OptContext& opt, mir::Function& function, U32 index) {
    auto& arg = *opt.decl[function.args.get(opt.decl, index)];
    auto& occurrence = arg.occurrence;

    /*
     * A plain shared borrow, granted for the call, and that is the whole test.
     *
     * `Access::Shared` rules out `&` and `->`: a mutable borrow arrives as an address precisely so
     * the callee can write back through it, and a sink transfers ownership - neither is a read of
     * the caller's value that a copy of each field is a faithful substitute for.
     * `Origin::CallBounded` is the other half and it is this stage's statement of the shipping
     * pass's `returnRoot`/`retained` pair: a call-bounded loan **may not outlive the call**, which
     * `checkCarriedOrigins` refuses at every position that would let it, so nothing the caller can
     * still see afterwards is rooted in the storage this rebuilds in the callee's frame. A parameter
     * that *does* publish - `'T` - says the opposite in its own signature and is declined here,
     * which also means no origin slot is renumbered by the rewrite.
     *
     * The **summary's** `retains` row is deliberately not asked, and that is a correction rather
     * than an omission - and neither is `escapes`, which J37 added for `ssa::Contained` and which
     * answers about the *storage* of one call's argument rather than about what a signature promises
     * of every call. It is a reach set over values: `multiply` returns an `Int` computed out of
     * `a[row * n + k]`, so every one of its four parameters is in it, and gating on it declined
     * every window in the corpus. What it answers is "does anything derived from this argument leave"
     * and what this rewrite needs is "does anything rooted in this argument's *storage* leave",
     * which is the contract above and is a declaration fact rather than an inferred one.
     */
    if(occurrence.contract.access() != mir::Access::Shared) return false;
    if(occurrence.contract.origin() != mir::Origin::CallBounded) return false;
    if(occurrence.shape) return false;

    /*
     * A **window**, and nothing else - `AccessForm::View`.
     *
     * This is the one borrowed form whose value is a record of its own. Every other shared borrow of
     * a record manifests as a *pointer to the caller's* storage (`lower/README.md`'s
     * `isReferenceValue` against `isAddressOnly`), so it already arrives in one register and there is
     * nothing to take apart - and rebuilding it would be a copy of somebody else's object rather
     * than the two words a window is.
     *
     * That is narrower than the shipping stage, which flattens `fn sum(pair: Pair)` as well. It is
     * narrower on purpose: the payoff measured over there for an ordinary record was "747 -> 714
     * allocas" on native and most of the rest was JavaScript, where the whole of what this item is
     * about - a check whose bound is a register rather than a load - is the window. Widening it
     * needs `manifestNative`, which lives one tier down.
     */
    if(occurrence.contract.form() != mir::AccessForm::View) return false;

    // A deferred or omittable position is one a call may not have written at all, so the operands
    // this would take apart are not the ones the declaration lists.
    return !arg.lazy && !arg.omittable();
}

/*
 * One signature's plan, computed from the declaration and nothing else.
 *
 * Every caller asks this about its callee and every callee asks it about itself, so it has to be a
 * pure function of the declaration - and in particular it must be asked *before* anything is
 * rewritten, since rewriting a signature is exactly the thing that would change the answer. That is
 * what splits the pass into two program-wide phases below.
 */
bool planFor(OptContext& opt, DeclPtr<mir::Function> pointer, mir::Function& function,
             HashMap<U32, bool>& blocked, Plan& plan) {
    plan.clear();
    if(!functionCanFlatten(opt, pointer, function, blocked)) return false;

    Size arity = 0;
    SmallArray<Step, 8> path;

    for(U32 i = 0; i < U32(function.args.size()); i++) {
        auto start = plan.leaves.size();
        auto flattened = false;

        if(argumentCanFlatten(opt, function, i)) {
            auto& arg = *opt.decl[function.args.get(opt.decl, i)];

            path.clear();
            flattened = collectLeaves(opt, plan, arg.occurrence, arg.name, path, 0);
            if(!flattened) plan.rewind(start);
        }

        auto count = flattened ? plan.leaves.size() - start : 0;

        // A single leaf is a record already passed as one value's worth of bytes, and swapping one
        // address for one scalar is a convention change that buys nothing.
        if(count > kMaxRecordFields || count < 2) {
            plan.rewind(start);
            count = 0;
        }

        plan.start.push(U32(start));
        plan.count.push(U32(count));

        arity += count ? count : 1;
        if(count) plan.any = true;
    }

    if(!plan.any || arity > kMaxFlatArity) {
        plan.clear();
        return false;
    }

    return true;
}

// The address of one leaf, relative to whatever value holds the record.
Value* leafAddress(Editor& editor, const Plan& plan, const Leaf& leaf, Value& record,
                   LocationId source) {
    auto at = &record;

    for(Size i = 0; i < leaf.pathCount; i++) {
        auto& step = plan.paths[leaf.pathStart + i];
        auto use = editor.use(*at, isTracked(at->convention) ? UseKind::Borrow : UseKind::Inspect);
        at = editor.resultOf(*editor.addFieldAddress(source, use, step.record, step.field, nullptr));
    }

    return at;
}

/*
 * One call site, rewritten against its callee's plan.
 *
 * A fresh `call` rather than an edit of the operand list, because the list changes *length*: the
 * editor is the only thing that writes a body and what it offers for an argument list is building
 * one. The old call's result is pointed at the new one's on the same loan, and the old instruction
 * then has no readers left.
 */
bool rewriteCall(OptContext& opt, Block& block, InstCall& call, const Plan& plan) {
    if(call.arguments.size() != plan.count.size()) return false;

    auto editor = opt.ir();
    editor.setBlock(block);
    editor.setInsertBefore(call);

    SmallArray<Use, 8> replacement;

    for(Size i = 0; i < call.arguments.size(); i++) {
        auto used = call.arguments.get(opt.base, i);

        if(!plan.count[i]) {
            replacement.push(used);
            continue;
        }

        auto record = opt.base[used.value()];

        for(Size j = 0; j < plan.count[i]; j++) {
            auto& leaf = plan.leaves[plan.start[i] + j];
            auto address = leafAddress(editor, plan, leaf, *record, call.source);

            auto load = editor.addLoad(call.source, editor.use(*address, UseKind::Inspect),
                                       leaf.occurrence, Convention::Trivial);

            replacement.push(editor.use(*editor.resultOf(*load), UseKind::Consume));
        }
    }

    mir::CallSite site;
    site.target = call.callee;
    site.through = call.through;
    site.requirement = call.requirement;
    site.member = call.member;
    site.environment = call.environment;

    auto had = call.results.size() != 0;
    auto result = had ? opt.base[call.results.get(opt.base, 0)] : nullptr;

    auto fresh = editor.addCall(call.source, site, call.target, replacement.pointer(),
                                replacement.size(),
                                result ? result->occurrence : mir::Occurrence {},
                                result ? result->convention : Convention::Trivial, had);

    if(result) {
        auto produced = editor.resultOf(*fresh);

        // The loan the old result carried, kept rather than widened to the caller's: it is what the
        // extent rule is checked against, and `replaceAllUses` requires the two to agree.
        if(result->loan) editor.setDependency(*produced, *opt.base[result->loan]);
        editor.replaceAllUses(*result, *produced);
    }

    editor.eraseInst(call);
    return true;
}

/*
 * Every signature's plan, computed once.
 *
 * `planFor` is a pure function of the declaration and every call site asks it about its callee, so
 * asking per site is asking the same question a few thousand times: the library's own suite spent
 * 3.5% of the whole compile there. The map holds an index into `plans` for a signature that moves
 * and nothing for one that does not, which is almost all of them.
 */
struct Plans {
    Array<Plan> made;
    HashMap<U32, U32> index;

    const Plan* find(DeclPtr<mir::Function> function) const {
        auto found = index.getValue(function.offset);
        return found ? &made[found.unwrap()] : nullptr;
    }
};

void rewriteCalls(OptContext& opt, const Plans& plans, mir::Function& function) {
    auto body = opt.base[function.ssa];

    for(U32 b = 0; b < body->blocks.size(); b++) {
        auto block = body->block(opt.base, BlockId(b));
        if(block->deleted) continue;

        for(Size i = 0; i < block->instructionCount(); i++) {
            auto inst = opt.base[block->instructionAt(opt.base, i)];
            if(inst->kind != Inst::Call) continue;

            auto& call = (InstCall&)*inst;
            if(call.through != mir::CallThrough::Declaration || !call.callee) continue;

            auto plan = plans.find(call.callee);
            if(!plan) continue;

            // The fresh call stands where this one did, so the cursor is left on it and the walk
            // steps past both together.
            if(rewriteCall(opt, *block, call, *plan)) i++;
        }
    }
}

/*
 * The leaf reads a body already makes of a flattened parameter - the prologue that emits **nothing**,
 * and the answer to the compile-time half of the convention.
 *
 * Rebuilding the record in a frame slot is the shape the shipping stage's `flattenArguments` uses
 * and the one this pass shipped with, and it is honest work thrown away: an `alloc`, one
 * `field.addr` and one `store` per leaf, a `load` to name it again - and then `promote`,
 * `forwardMemory` and `removeDeadStorage` take all of it out in the loop below. J36 measured what
 * that costs on the library and it is **0.18 s of a 0.30 s gap**, in `cse` and `simplify` rather
 * than here: the churn is IR the passes below have to walk before they can decide it is dead.
 *
 * So the entry is written as block arguments outright wherever the body's every use of the
 * parameter is a projection down to a leaf and a `load` of it, which is what a body that reads a
 * window reads. That is "understanding the body", which the shipping pass declines on purpose - and
 * the whole of the understanding is here: a chain of `field.addr` whose steps are the plan's own
 * path, ending in a `load` whose occurrence is the leaf's. Anything else - a `represent`, a read of
 * the whole record, a projection the plan did not flatten - is not matched and the slot is built,
 * so what the rule costs when it fails is one walk of a use list.
 *
 * `reads` is one entry per `load`, because a body that reads a field twice has two of them and both
 * become the same parameter; `projections` is every `field.addr` on the way, deepest last, so that
 * erasing backwards erases each one after everything that named it.
 */
struct LeafRead {
    Inst* load = nullptr;
    Size leaf = 0;
};

// Which leaf of this argument the path so far names exactly, or `~Size(0)`.
Size leafAt(const Plan& plan, Size start, Size count, const SmallArray<Step, 8>& path) {
    for(Size i = 0; i < count; i++) {
        auto& leaf = plan.leaves[start + i];
        if(leaf.pathCount != path.size()) continue;

        auto same = true;
        for(Size j = 0; j < path.size(); j++) {
            auto& step = plan.paths[leaf.pathStart + j];
            if(step.record != path[j].record || step.field != path[j].field) same = false;
        }

        if(same) return i;
    }

    return ~Size(0);
}

// Whether any leaf's path begins with the one walked so far - a projection into a field the plan did
// not take apart is one this match has nothing to offer.
bool leafBeginsWith(const Plan& plan, Size start, Size count, const SmallArray<Step, 8>& path) {
    for(Size i = 0; i < count; i++) {
        auto& leaf = plan.leaves[start + i];
        if(leaf.pathCount < path.size()) continue;

        auto same = true;
        for(Size j = 0; j < path.size(); j++) {
            auto& step = plan.paths[leaf.pathStart + j];
            if(step.record != path[j].record || step.field != path[j].field) same = false;
        }

        if(same) return true;
    }

    return false;
}

bool collectLeafReads(OptContext& opt, const Plan& plan, Size start, Size count, Value& value,
                      SmallArray<Step, 8>& path, SmallArray<LeafRead, 8>& reads,
                      SmallArray<Inst*, 16>& projections) {
    auto here = leafAt(plan, start, count, path);

    for(auto userPointer: value.uses(opt.base)) {
        auto& user = *opt.base[userPointer];

        if(here != ~Size(0)) {
            // At a leaf, and the only thing a body may do with one is read it.
            if(user.kind != Inst::Load) return false;
            if(opt.base[((InstLoad&)user).address.value()] != &value) return false;
            if(user.results.size() != 1) return false;

            auto result = opt.base[user.results.get(opt.base, 0)];
            if(result->convention != Convention::Trivial) return false;
            if(result->occurrence != plan.leaves[start + here].occurrence) return false;

            reads.push(LeafRead { &user, here });
            continue;
        }

        // Above one, and the only thing a body may do is project further towards it.
        if(user.kind != Inst::FieldAddress) return false;

        auto& projection = (InstFieldAddress&)user;
        if(opt.base[projection.base.value()] != &value) return false;
        if(user.results.size() != 1) return false;

        path.push(Step { projection.record, projection.field });
        auto ok = leafBeginsWith(plan, start, count, path);
        if(ok) {
            projections.push(&user);
            ok = collectLeafReads(opt, plan, start, count,
                                  *opt.base[user.results.get(opt.base, 0)], path, reads,
                                  projections);
        }

        path.pop();
        if(!ok) return false;
    }

    return true;
}

/*
 * One signature, and the prologue that puts the record back together.
 *
 * The new parameters are declared first and the old ones are removed afterwards, from the front, so
 * that what the entry block ends up declaring is the list in the order the declaration now states.
 * Nothing in between is emitted, so the window in which the block declares both is one this function
 * opens and closes.
 */
void flattenSignature(OptContext& opt, mir::Function& function, const Plan& plan) {
    auto body = opt.base[function.ssa];
    auto entry = opt.base[body->entry];

    auto editor = opt.ir();
    editor.setBlock(*entry);
    if(entry->instructionCount()) editor.setInsertBefore(*opt.base[entry->instructionAt(opt.base, 0)]);

    auto retired = entry->argumentCount();

    SmallArray<DeclPtr<mir::Arg>, 8> arguments;
    SmallArray<Value*, 8> incoming;

    for(Size i = 0; i < retired; i++) incoming.push(opt.base[entry->argumentAt(opt.base, i)]);

    for(U32 i = 0; i < U32(function.args.size()); i++) {
        auto argPointer = function.args.get(opt.decl, i);
        auto& arg = *opt.decl[argPointer];

        if(!plan.count[i]) {
            auto fresh = editor.declareBlockArgument(*entry, arg.occurrence,
                                                     incoming[i]->convention);
            editor.replaceAllUses(*incoming[i], *fresh);
            arguments.push(argPointer);
            continue;
        }

        /*
         * The leaves, declared first, and then one of two prologues - see `collectLeafReads`, which
         * decides between them by walking the parameter's use list once.
         */
        SmallArray<Step, 8> path;
        SmallArray<LeafRead, 8> reads;
        SmallArray<Inst*, 16> projections;

        auto direct = collectLeafReads(opt, plan, plan.start[i], plan.count[i], *incoming[i], path,
                                       reads, projections);

        SmallArray<Value*, 8> leafValues;

        for(Size j = 0; j < plan.count[i]; j++) {
            auto& leaf = plan.leaves[plan.start[i] + j];

            auto declared = new (opt.program.decl) mir::Arg();
            declared->name = leaf.name;
            declared->occurrence = leaf.occurrence;
            declared->source = arg.source;
            declared->index = U32(arguments.size());

            arguments.push(declared - *opt.program.decl);
            leafValues.push(editor.declareBlockArgument(*entry, leaf.occurrence,
                                                        Convention::Trivial));
        }

        /*
         * The body already reads this parameter one leaf at a time, so each read *is* one of the new
         * parameters and there is no record to rebuild. The loads go first and the projections after
         * them, deepest last so that erasing backwards erases each one after everything that named
         * it; what is left naming `incoming[i]` is then nothing, which is what the removal below
         * requires.
         */
        if(direct) {
            for(auto& read: reads) {
                editor.replaceAllUses(*opt.base[read.load->results.get(opt.base, 0)],
                                      *leafValues[read.leaf]);
                editor.eraseInst(*read.load);
            }

            for(Size j = projections.size(); j-- > 0;) editor.eraseInst(*projections[j]);
            continue;
        }

        /*
         * And the record, rebuilt in this frame, where it does not. `promote`, `forwardMemory` and
         * `removeDeadStorage` are what take it away again - see the file comment, where the reason
         * this is not the pessimisation it looks like is written out.
         */
        auto storage = editor.resultOf(
            *editor.addAlloc(arg.source, arg.occurrence, mir::StorageClass::Frame));

        for(Size j = 0; j < plan.count[i]; j++) {
            auto& leaf = plan.leaves[plan.start[i] + j];
            auto address = leafAddress(editor, plan, leaf, *storage, arg.source);

            editor.addStore(arg.source, editor.use(*address, UseKind::Inspect),
                            editor.use(*leafValues[j], UseKind::Consume));
        }

        /*
         * And the borrow of it the body already holds - a `load` whose result is `Guaranteed`, which
         * is how a second name for storage is spelled at this tier and is no instruction at all in
         * the lowering. It keeps the parameter's own loan, which is the caller's: the slot is an
         * entry-block allocation that outlives every use of it, and `replaceAllUses` requires the
         * two loans to agree.
         */
        auto held = editor.addLoad(arg.source, editor.use(*storage, UseKind::Inspect),
                                   arg.occurrence, incoming[i]->convention);

        auto produced = editor.resultOf(*held);
        if(incoming[i]->loan) editor.setDependency(*produced, *opt.base[incoming[i]->loan]);

        editor.replaceAllUses(*incoming[i], *produced);
    }

    // The parameters this signature had, gone - from the front, because every new one was appended
    // behind them and the order they are in is the order the declaration now states.
    for(Size i = 0; i < retired; i++) editor.removeBlockArgument(*entry, 0);

    /*
     * The list, in place. `set` where a position already exists and `push` for the rest: flattening
     * only ever makes the list longer, and `DeclList` has no clear - a list emptied and refilled
     * would also change whether it is held embedded, which is a representation decision this has no
     * business making.
     */
    for(Size i = 0; i < arguments.size(); i++) {
        if(i < function.args.size()) function.args.set(opt.decl, i, arguments[i]);
        else function.args.push(opt.program.decl, arguments[i]);
    }

    for(U32 i = 0; i < U32(function.args.size()); i++) {
        opt.decl[function.args.get(opt.decl, i)]->index = i;
    }

    /*
     * The four argument sets, kept over the positions the signature now has.
     *
     * Every leaf inherits what the record's position said, which is conservative and uninteresting
     * in equal measure: a parameter only got here by being an unretained, unmutated, unconsumed
     * shared borrow, so there is nothing in the entry for a leaf to inherit wrongly. Rebuilt at all
     * because a set whose bits no longer match the arguments is a trap for whoever reads it next -
     * `mayWriteStorage` reads `mutates` by position on every call.
     */
    auto remap = [&](mir::ArgumentSet set) {
        mir::ArgumentSet moved;
        U32 at = 0;

        for(Size i = 0; i < plan.count.size(); i++) {
            auto width = plan.count[i] ? plan.count[i] : 1;
            if(set.has(U32(i))) {
                for(Size j = 0; j < width; j++) moved.add(at + U32(j));
            }

            at += U32(width);
        }

        return moved;
    };

    auto& summary = function.summary;
    auto retained = summary.retains();
    auto kept = summary.escapes();
    auto mutated = summary.mutates();
    auto consumed = summary.consumes();

    summary.setRetains(remap(retained.value), retained.knowledge);
    summary.setEscapes(remap(kept.value), kept.knowledge);
    summary.setMutates(remap(mutated.value), mutated.knowledge);
    summary.setConsumes(remap(consumed.value), consumed.knowledge);
}

} // namespace

void flattenArguments(OptContext& opt) {
    HashMap<U32, bool> blocked;
    flattenBlocked(opt, blocked);

    auto& program = opt.program;

    /*
     * Three walks, and the order between the last two is the correctness argument.
     *
     * The plans first, because a plan has to be the same on both sides of every call and rewriting a
     * signature is exactly the thing that would change one. Then every call site, while every
     * signature still says what it was declared to say. Then the signatures. Rewriting a function and
     * its callers together would work only if no function were ever both, which is not a property a
     * program has.
     */
    Plans plans;

    for(auto modulePointer: program.modules) {
        auto module = opt.decl[modulePointer];

        for(auto functionPointer: module->functions.contents(opt.decl)) {
            auto function = opt.decl[functionPointer];

            Plan plan;
            if(!planFor(opt, functionPointer, *function, blocked, plan)) continue;

            plans.index.add(functionPointer.offset, U32(plans.made.size()));
            plans.made.push(::move(plan));
        }
    }

    if(!plans.made.size()) return;

    for(auto modulePointer: program.modules) {
        auto module = opt.decl[modulePointer];

        for(auto functionPointer: module->functions.contents(opt.decl)) {
            auto function = opt.decl[functionPointer];
            if(!function->ssa || !function->reachable) continue;

            opt.function = function;
            opt.body = opt.base[function->ssa];
            rewriteCalls(opt, plans, *function);
        }
    }

    for(auto modulePointer: program.modules) {
        auto module = opt.decl[modulePointer];

        for(auto functionPointer: module->functions.contents(opt.decl)) {
            auto plan = plans.find(functionPointer);
            if(!plan) continue;

            auto function = opt.decl[functionPointer];
            opt.function = function;
            opt.body = opt.base[function->ssa];
            flattenSignature(opt, *function, *plan);
        }
    }
}

} // namespace ssa
