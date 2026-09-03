#include "inline.h"
#include "clone.h"
#include "generic.h"

/*
 * The shape a body has to have, and every clause is one of the inliner's jobs made empty.
 *
 * Read them against opt_inline.cpp, which is the thing this is deliberately not: **one block** is
 * "no returns to join and no edges to rewire"; **no place and no ownership** is "no storage to
 * re-root and no drop state to carry"; **no call** is "nothing whose cost has to be weighed". What
 * survives is a run of computation that runs once, in order, exactly where the call stood - so
 * copying it into the block that is already current is the whole transform.
 *
 * `Alloc` is named rather than derived, and it is the one clause that is not a trait: it is the only
 * kind that takes a frame slot without naming a place, so the three questions above all answer
 * "fine" for it and the copy has nowhere to put the local. In a body that names no place an
 * allocation is dead anyway; refusing it outright is what keeps that an argument rather than a
 * coincidence the next reader has to reconstruct.
 *
 * ## The two strictnesses, and why there are two
 *
 * `settled` is false for the check at the declaration and true for the expansion at a call, and the
 * one thing it changes is whether a `gencall` is admitted.
 *
 * A generic body's class calls are `gencall`s until something binds the type arguments. `bitcast(0)`
 * in `fn null() -> %a` is one, and what it *becomes* is the whole question: at `%U8` it is one
 * `bitcast` instruction, because `Bitcast` is a class the compiler instances - and at some other
 * type it could be a call to a written body. Nothing at the declaration can tell those apart, so
 * refusing a `gencall` there would refuse every generic one-liner in the library, and accepting one
 * at a call site would graft an instruction that reads the *caller's* environment at the callee's
 * slot numbers.
 *
 * So the declaration is checked for what no substitution can fix - a branch, a `let`, an owner - and
 * the specialization is what the site actually grafts. `instantiateFunction` is what stands between
 * them, and it is why a generic `@inline(always)` callee is specialized rather than erased.
 */
static bool isGraftable(Module& module, Block& block, Value& value, bool settled) {
    auto local = *module.arena;
    auto flags = instructionTraits(value.kind).flags;

    if(!settled && value.kind == Value::GenCall) return true;

    if(flags & (kInstCall | kInstGeneric | kInstAnchored | kInstTerminator)) return false;
    if(value.kind == Value::Alloc) return false;

    Place places[kMaxPlaces];
    if(instructionPlaces(value, places)) return false;

    auto transfers = false;
    eachTransferOperand(local, value, [&](ModulePtr<Value> operand, LocationId) {
        transfers = transfers || operand != nullptr;
    });

    if(transfers) return false;

    /*
     * Every operand is one this call site can produce.
     *
     * A parameter, a constant, or something defined earlier in this same block - and nothing else,
     * which is what closes the cases the clauses above do not name. A generic body reaches its
     * witness tables and its type descriptors through values the environment supplies, and a copy of
     * an instruction reading one would read the *caller's* environment at the same slot number. The
     * operand is what says so, so the operand is what is asked.
     */
    auto ok = true;

    mapOperands(local, value, [&](ModulePtr<Value> operand) {
        if(!operand) return operand;

        auto& defined = *local[operand];
        auto elsewhere = !isConstant(defined) && defined.kind != Value::Arg &&
                         defined.block != (&block - local);

        ok = ok && !elsewhere;
        return operand;
    });

    return ok;
}

// The one block a graftable body has, or null. Kept apart from the walk so that the check at the
// declaration and the expansion at the call ask one question at two strictnesses.
static Block* graftableBlock(Module& module, Function& function, bool settled) {
    auto local = *module.arena;
    if(function.blocks.size() != 1) return nullptr;

    auto block = local[function.blocks.get(local, 0)];
    if(block->phiCount()) return nullptr;

    auto terminator = block->terminator();
    if(!terminator || local[terminator]->kind != Value::Ret) return nullptr;

    for(auto pointer: block->instructions(local)) {
        auto& instruction = *local[pointer];
        if(isTerminator(instruction)) continue;

        if(!isGraftable(module, *block, instruction, settled)) return nullptr;
    }

    return block;
}

void checkAlwaysInline(Module& module, Function& function) {
    if(!function.alwaysInline) return;

    /*
     * A `@lazy` parameter is refused before the body is looked at.
     *
     * What a deferred position is handed is a thunk over the caller's frame, and what makes it worth
     * having is a callee that decides whether to run it - which is a branch, and a branch is the
     * first thing the shape below rules out. So the two features have no case in common, and saying
     * so here is what keeps the expansion from having to carry a promise it can never force.
     */
    auto deferred = false;
    for(auto pointer: function.args.contents(*module.arena)) {
        deferred = deferred || (*module.arena)[pointer]->isLazy();
    }

    if(!deferred && graftableBlock(module, function, false)) return;

    /*
     * Refused where it is written, which is what makes the attribute a directive.
     *
     * The message names the rule rather than the instruction that broke it, on purpose: what a
     * reader can act on is "this has to be one expression", and the kind that failed is a name from
     * the IR they have no reason to know. `@inline` is offered because it is the honest version of
     * what they asked for - a hint the optimizer weighs - and it is the attribute they want for any
     * body with a branch in it.
     */
    module.context.diagnostics.error("`@inline(always)` on %@ needs a body that is one expression - no branches, no call, and nothing that allocates or owns storage - because it is expanded where it is called rather than compiled and inlined. Use `@inline` for a body the optimizer should weigh instead"_v,
                                     function.source, module.context.findName(function.name));

    function.alwaysInline = false;
}

namespace {

/*
 * What `cloneComputation` needs of this graft - clone.h argues the four hooks.
 *
 * The three fixed fields are the *caller's*, which is the whole of what makes this an expansion
 * rather than a specialization: the copy is built in the block that is already current, so the
 * instructions it appends stand exactly where the call would have.
 */
struct Graft {
    Graft(ExprResolver& resolver, Buffer<ModulePtr<Value>> args):
        module(resolver.module), function(resolver.function), block(resolver.block()), args(args) {}

    ModulePtr<Value> value(ModulePtr<Value> operand) {
        if(!operand) return nullptr;

        auto local = *module.arena;
        auto& original = *local[operand];

        // A parameter is the argument this call site passed - the substitution the whole expansion
        // is for. The list is positional and was converted against the declared types before this
        // was reached, so nothing here re-converts.
        if(original.kind == Value::Arg) {
            auto index = ((Arg&)original).index;
            return index < args.length ? args[index] : nullptr;
        }

        if(auto found = copied.getValue(U32(operand))) return ModulePtr<Value>(found.unwrap());

        // A constant belongs to its function rather than to a block, so the copy is a new one - the
        // same four kinds the specializer re-creates, from the one list in clone.h.
        auto made = cloneConstantValue(module, function, block, original, original.type);
        if(made) copied.add(U32(operand), U32(made));

        return made;
    }

    /*
     * The identity, and it is worth saying why rather than leaving an empty hook.
     *
     * The other two copiers substitute here - that is what makes a specialization one - and this one
     * has nothing to substitute because the callee it grafts is already settled: a plain function,
     * or the specialization `instantiateFunction` made for the types the call decided. A generic
     * body never reaches this, which is the same fact `isGraftable`'s `settled` states from the
     * other side.
     */
    TypePtr type(TypePtr original) { return original; }

    /*
     * A graft's callee is the function it already was - clone.h's own wording.
     *
     * The specializer's answer here is different and that is the whole of what this hook is for: a
     * continuation lifted out of the body being specialized needs a clone of its own, because its
     * body names the type variables that are being substituted. Nothing is being substituted here,
     * so a `Symbol` in a grafted body refers to the same function or global it always did.
     */
    ModulePtr<Function> callee(ModulePtr<Function> original) { return original; }

    void placed(Inst* instruction) { IrEditor(module, function).append(block, instruction); }

    Module& module;
    Function& function;
    Block& block;
    Buffer<ModulePtr<Value>> args;

    // Keyed by region offset, which is the identity the rest of the resolver uses too.
    HashMap<U32, U32> copied;
};

} // namespace

Maybe<ModulePtr<Value>> foldTrivialBody(ExprResolver& resolver, ModulePtr<Function> callee,
                                        Buffer<ModulePtr<Value>> args, StringId resultName) {
    auto& module = resolver.module;
    auto local = *module.arena;
    auto& function = *local[callee];

    if(!function.alwaysInline) return Nothing();

    /*
     * The body, resolved if this call reached it first.
     *
     * A forward reference is ordinary - a caller written above its callee, or in a module read
     * earlier - and the check at the declaration runs when the body is resolved, so asking for it
     * here is what puts the two in the same order however the call arrived.
     */
    resolveFunctionBody(*function.module, function);

    /*
     * A specialization is checked here and not at a declaration, because it has none.
     *
     * `checkAlwaysInline` ran on the generic body and passed it with its `gencall`s in place; what
     * those turned into is decided by the specialization, and a class call that landed on a written
     * body rather than on an instance the compiler generates is a call this cannot graft. So the
     * site declines and the ordinary call is emitted - the attribute still held, in that the one
     * thing it promised (no call to *this* function) is what the specialization already delivered.
     */
    auto block = graftableBlock(module, function, true);
    if(!block) return Nothing();

    Graft graft(resolver, args);

    for(auto pointer: block->instructions(local)) {
        auto& instruction = *local[pointer];
        if(isTerminator(instruction)) continue;

        auto made = cloneComputation(graft, instruction);

        /*
         * A kind the copier has no arm for, which `isGraftable` was supposed to have refused.
         *
         * An assertion and not a diagnostic, for the reason clone.h states beside the same case in
         * the inliner: the two lists are one statement made twice - the clauses there and the arms
         * here - and a kind that reaches one and not the other is a compiler defect rather than
         * anything a program did. Nothing has been appended for it, so a release build carries on
         * with the call it was going to make.
         */
        assertTrue(made != nullptr);
        if(!made) return Nothing();

        graft.copied.add(U32(pointer), U32(made - local));
    }

    auto value = ((InstRet*)local[block->terminator()])->value;
    auto returned = graft.value(value);

    /*
     * The call's own result name, on whatever produced the value.
     *
     * Only where this expansion is what produced it: `fn id(x: a) = x` hands back the caller's own
     * argument, and renaming that would rename a value the caller named itself.
     */
    if(returned && resultName) {
        if(auto found = graft.copied.getValue(U32(value))) {
            local[ModulePtr<Value>(found.unwrap())]->name = resultName;
        }
    }

    return Just(returned);
}
