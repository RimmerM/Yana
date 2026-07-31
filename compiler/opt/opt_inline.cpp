#include "opt_pass.h"

/*
 * Inlining: a call replaced by a copy of what it would have done.
 *
 * The first one, and deliberately the narrow half of the problem. What it does is *straight-line*
 * callees - one block ending in a `ret` - spliced into the caller's block where the call was. That
 * is not a simplification of the general case so much as a different, much smaller one: there is no
 * control flow to graft, no block to split, no phi to build for a result several `ret`s agree on,
 * and no successor's phi inputs to rename. The whole of it is a value map and a local map.
 *
 * It is also where the wins are. The callees worth inlining first are the ones with nothing in them
 * - an accessor, a constructor, an operator, a one-line mutator through a `&` - and every one of
 * those is a single block. What is left out is a callee with a branch in it, which is the follow-on
 * and needs the CFG surgery this does not do.
 *
 * ## What it is for, which is not "removing the call"
 *
 * Removing a call is the small part. What inlining buys at this altitude is that the *other* passes
 * can then see through it:
 *
 *  - a constructor inlined into its caller becomes an `alloc` and some `init`s in the caller's own
 *    block, which is exactly the shape opt_scalar.cpp takes apart - so the record is never built at
 *    all. On a managed host that is an allocation and a hidden class that stop existing;
 *  - a mutator taking `&x` becomes reads and writes of the caller's own place, which forwarding then
 *    answers - so the borrow, and often the local behind it, go away with it;
 *  - a callee called with constants folds against them, and the dead-value pass collects whatever
 *    that made unreachable. This is the one that pays on every target, because it is the only way a
 *    constant crosses a call boundary in this compiler at all.
 *
 * Which is why this runs program-wide before any function is optimized, in the same place and for
 * the same reason `flattenArguments` does: what it leaves behind is work for the passes after it.
 *
 * ## The heuristics, and why they differ per target
 *
 * `InlinePolicy` is the whole cost model and it is a table rather than a computation. Two axes:
 *
 *  - **the level**, which is `-Os` against `-Ofast` and nothing more subtle. `Size` inlines only
 *    where the program cannot grow - a callee with exactly one call site in the program, which takes
 *    a body away rather than copying one - and `Speed` raises every budget.
 *  - **the target family**, which changes the *shape* of the answer rather than its size. See
 *    `policyFor` for the two rules that differ and the reasoning behind each.
 *
 * Every bonus is a budget increase rather than a decision, so they compose: a callee taking a
 * mutable borrow *and* called with a constant *and* called once gets all three, and one that is
 * simply small needs none of them.
 *
 * ## What is declined, and why each one
 *
 * The ownership instructions are the list that matters. `Drop`, `Move`, `Swap` and `Exchange` are
 * transfers the analyses already decided and spent, and copying one into a caller is asserting that
 * the decision travels - which for a drop is a double free if it does not. So a body containing any
 * of them is declined outright, as is a callee any of whose types owes a teardown. That is stricter
 * than necessary and it is the right place to start: what is left is bodies whose whole content is
 * computation, reads, writes and calls, and every case in the paragraphs above is one of those.
 *
 * A `->` sink parameter is declined for the same reason from the other side, and a `return`
 * parameter because the loan the caller took was sized against the summary rather than the body.
 * A parameter of a memory type passed by value is declined because it arrives as the *caller's*
 * storage, so the callee's places are rooted in a local this frame does not have - which is the one
 * case that needs a place root the callee never wrote and is worth doing separately.
 *
 * A recursive call is declined by the round budget rather than by a cycle check: a self-call is
 * refused outright, and a mutual pair simply stops being inlined when the rounds run out.
 */

namespace {

/*
 * What a call site is worth, in instructions.
 *
 * `budget` is the size a callee may be for the call to be worth inlining; everything else adjusts
 * it up or down for what this particular call site looks like. Numbers rather than named cases so
 * that a call site with two reasons to be inlined gets both.
 */
struct InlinePolicy {
    U32 budget = 0;

    // A callee whose only call site in the whole program is this one. Inlining it removes a body
    // rather than copying one, so this is the one bonus that is a size win as well as a speed one.
    U32 soleCallSite = 0;

    // Per argument that is a constant, capped at `constantCap` arguments so that a wide signature
    // of literals does not buy an unbounded body.
    U32 constantArgument = 0;
    U32 constantCap = 4;

    // A parameter taken by mutable borrow, and a result the target holds in memory. Both are
    // allocations the caller made for the call and both stop existing when it goes away.
    U32 mutableBorrow = 0;
    U32 memoryResult = 0;

    // Subtracted where the callee is called from more than one place, and again where it is called
    // from many. What this prices is code growth, which is paid once per call site.
    U32 repeatedPenalty = 0;
    U32 manyCallSites = 0;
    U32 manyPenalty = 0;

    /*
     * `@inline` on the declaration - see readInlineAttribute in resolve/module.cpp.
     *
     * A budget term like every other, which is the whole of what makes it an honest hint: it says
     * "weigh this callee as if it were smaller", not "inline this". A callee the checks in
     * `describe` refuse is still refused however large a number goes here, and the ceiling below
     * still applies - so the attribute cannot ask for something the pass would then quietly not do.
     *
     * Large enough to carry a callee well past the base budget, because a caller that wrote it knows
     * something about the payoff that the body does not show - a loop this sits in, or folding that
     * only happens two passes later. Not infinite, for the reason the ceiling exists.
     */
    U32 requested = 0;

    // The size past which no bonus helps. A ceiling rather than another term, because the thing it
    // exists to prevent is a large body being copied on the strength of enough small reasons.
    U32 ceiling = 0;
};

/*
 * The table, by level and by target family.
 *
 * Two rules differ between the families and both come from the same fact - that a managed host does
 * not have an optimizing backend under it, and does have a collector:
 *
 *  - **a managed target pays much more for an allocation**, so `mutableBorrow` and `memoryResult`
 *    are large there and small natively. On JS a record that stays a record is an object with a
 *    hidden class; removing the call is what lets opt_scalar.cpp remove the object. Natively the
 *    same record is bytes in a frame the function already has, and LLVM would have inlined the
 *    callee anyway - so the bonus buys little and is priced accordingly;
 *  - **a managed target pays much more for code size**, so `repeatedPenalty` and `manyPenalty` are
 *    larger and `ceiling` lower. Emitted JS is source text the host parses, and V8 has an inlining
 *    budget of its own that a function grown past it stops qualifying for - so inlining a large
 *    callee into six call sites can lose twice, once in bytes and once by pushing each caller out of
 *    the host's own budget. That is the user-visible half of "avoid inlining large functions that
 *    are called multiple times".
 *
 * What does *not* differ is the constant-argument bonus and the sole-call-site bonus, because
 * neither is about the machine. A callee folded against its arguments is smaller after inlining
 * than the call was, and a callee with one call site leaves nothing behind on either target.
 */
InlinePolicy policyFor(InlineLevel level, TargetFamily family) {
    auto managed = family == TargetFamily::Managed;
    InlinePolicy policy;

    switch(level) {
        case InlineLevel::None:
            return policy;

        case InlineLevel::Size:
            // Nothing but the case that cannot grow the program, which is why the base budget is
            // zero: a callee qualifies here only through `soleCallSite`.
            policy.budget = 0;
            policy.soleCallSite = 10;
            policy.constantArgument = 0;
            policy.mutableBorrow = 0;
            policy.memoryResult = 0;
            policy.repeatedPenalty = 0;
            policy.manyCallSites = 2;
            policy.manyPenalty = 0;
            policy.requested = 24;
            policy.ceiling = 24;
            break;

        case InlineLevel::Balanced:
            policy.budget = 8;
            policy.soleCallSite = 12;
            policy.constantArgument = 3;
            policy.mutableBorrow = managed ? 10 : 3;
            policy.memoryResult = managed ? 12 : 4;
            policy.repeatedPenalty = managed ? 3 : 1;
            policy.manyCallSites = managed ? 4 : 8;
            policy.manyPenalty = managed ? 6 : 2;
            policy.requested = 32;
            policy.ceiling = managed ? 40 : 48;
            break;

        case InlineLevel::Speed:
            policy.budget = 20;
            policy.soleCallSite = 32;
            policy.constantArgument = 5;
            policy.mutableBorrow = managed ? 16 : 6;
            policy.memoryResult = managed ? 20 : 8;
            policy.repeatedPenalty = managed ? 2 : 0;
            policy.manyCallSites = managed ? 8 : 16;
            policy.manyPenalty = managed ? 4 : 1;
            policy.requested = 64;
            policy.ceiling = managed ? 80 : 120;
            break;
    }

    return policy;
}

// How one parameter arrives, which decides what the callee's uses of it become.
enum class Binding: U8 {
    // A register value. The callee reads the `Arg` itself, and every read becomes the caller's
    // argument value.
    Value,

    // A `&` parameter: the callee has a local whose storage is the caller's, and reads and writes
    // go through places rooted in it. Those places become places rooted in the caller's borrow.
    Borrowed,
};

struct Parameter {
    Binding binding = Binding::Value;

    // The callee local the `&` case rewrites away, and `kNone` for the value case.
    U32 local = maxLimit<U32>;
};

struct Candidate {
    ModulePtr<Function> pointer = nullptr;
    Function* callee = nullptr;
    Block* body = nullptr;
    Array<Parameter> parameters;

    // The callee local holding what a memory-typed result is returned out of, and `kNone` where the
    // result is a register value or nothing at all.
    U32 resultLocal = maxLimit<U32>;

    U32 size = 0;

    static constexpr U32 kNone = maxLimit<U32>;
};

struct Inliner {
    OptContext& opt;
    InlinePolicy policy;
    HashMap<U32, bool> taken;
    HashMap<U32, U32> callSites;

    /*
     * Whether cloning this instruction into another function is something this pass knows how to do.
     *
     * An allow-list rather than a deny-list, and the header comment says why the ownership four are
     * out. The rest is what is left: computation, storage, reads, writes, borrows and direct calls.
     * `Native`, `CallDyn` and `GenCall` are declined not because they are unsound but because each
     * carries state - an intrinsic's arguments, a signature, a type argument list - that would have
     * to be copied correctly and is not exercised by anything this currently inlines.
     */
    bool clonableKind(Value::Kind kind) {
        switch(kind) {
            case Value::Alloc: case Value::LoadPlace: case Value::Init: case Value::Assign:
            case Value::Borrow: case Value::Copy: case Value::TypeMetric: case Value::Symbol:
            case Value::Cast: case Value::Neg: case Value::Not:
            case Value::Add: case Value::Sub: case Value::Mul: case Value::Div: case Value::Rem:
            case Value::Shl: case Value::Shr: case Value::Sar:
            case Value::And: case Value::Or: case Value::Xor: case Value::Cmp:
            case Value::Call:
                return true;
            default:
                return false;
        }
    }

    /*
     * One callee, checked once and described for every call site of it.
     *
     * Everything here is a property of the *declaration*, which is what makes it worth computing
     * once: whether it can be inlined at all does not depend on who is calling, and only the budget
     * below does.
     */
    // Whether any place in the body is rooted in this local, which is the difference between a slot
    // that is storage and a slot that is only a name.
    bool namesLocal(Block& body, U32 local) {
        auto found = false;

        auto visit = [&](Value& instruction) {
            eachPlace(instruction, [&](const Place& place) {
                if(place.root == PlaceRoot::Local && place.local == local) found = true;
            });
        };

        for(auto pointer: body.instructions.contents(opt.local)) visit(*opt.local[pointer]);
        if(body.terminator) visit(*opt.local[body.terminator]);

        return found;
    }

    Maybe<Candidate> describe(ModulePtr<Function> pointer) {
        auto callee = opt.local[pointer];

        // `@noinline`, which is a directive rather than a weight: declining to inline is always
        // possible, so this is the one input to the decision that nothing below can outvote.
        if(callee->noInline) return Nothing();

        if(callee->signature || callee->intrinsic || callee->gen || callee->takesEnv) return Nothing();
        if(callee->blocks.size() != 1) return Nothing();
        if(taken.get(U32(pointer))) return Nothing();

        auto body = opt.local[callee->blocks.get(opt.local, 0)];
        if(body->phis.isNotEmpty() || !body->terminator) return Nothing();
        if(opt.local[body->terminator]->kind != Value::Ret) return Nothing();

        Candidate candidate;
        candidate.pointer = pointer;
        candidate.callee = callee;
        candidate.body = body;

        for(auto instructionPointer: body->instructions.contents(opt.local)) {
            auto& instruction = *opt.local[instructionPointer];
            if(!clonableKind(instruction.kind)) return Nothing();

            candidate.size++;
        }

        /*
         * Which parameter each local belongs to, which is the map the parameter walk below needs and
         * the local table does not have: a local records the *value* its storage is, so the question
         * "does this argument have a local" is answered by looking for it.
         */
        for(Size i = 0; i < callee->args.size(); i++) {
            auto argPointer = callee->args.get(opt.local, i);
            auto arg = opt.local[argPointer];

            // A sink transfers ownership into the callee and a `return` parameter is one the
            // caller's loan was sized against. Both are decisions taken at the call rather than in
            // the body, and neither survives being spliced away.
            if(arg->convention == ast::BindType::Sink || arg->returnRoot) return Nothing();

            Parameter parameter;
            parameter.binding = Binding::Value;

            for(U32 local = 0; local < callee->localCount(); local++) {
                auto slot = callee->localAt(opt.local, local);
                if(slot.value != (ModulePtr<Value>)argPointer) continue;

                // A `&` parameter's slot is the caller's storage, which the rewrite below turns
                // into a place rooted in the caller's borrow.
                if(slot.borrowed) {
                    parameter.binding = Binding::Borrowed;
                    parameter.local = local;
                    break;
                }

                /*
                 * Any other parameter with a slot, and the question is whether the body ever reaches
                 * the parameter *through* it.
                 *
                 * Where it does, the slot is the caller's address - a memory-typed value parameter -
                 * and there is no rewrite here that would follow it, so the callee is declined.
                 *
                 * Where it does not, the slot is bookkeeping and the parameter is an ordinary SSA
                 * value that `mapValue` already substitutes correctly. Telling the two apart matters
                 * more than it sounds: a scalar value parameter gets a slot too, so assuming a slot
                 * meant memory refused every function with one - which is most of Core's operators.
                 * `+=(Int)` is four instructions over a mutable borrow, the single most rewarding
                 * shape there is on a managed target, and it was being declined on its *second*
                 * parameter for having storage nothing reads.
                 */
                if(namesLocal(*body, local)) return Nothing();
                break;
            }

            candidate.parameters.push(parameter);
        }

        for(U32 local = 0; local < callee->localCount(); local++) {
            auto slot = callee->localAt(opt.local, local);
            if(slot.closureEnv) return Nothing();
            if(slot.type && needsTeardown(*opt.module, slot.type)) return Nothing();
        }

        auto& ret = (InstRet&)*opt.local[body->terminator];

        if(ret.value) {
            auto type = opt.local[ret.value]->type;
            if(type && needsTeardown(*opt.module, type)) return Nothing();

            /*
             * A result the target holds in memory is returned out of storage rather than in a
             * register, and the caller allocated that storage for the call. So the callee's local
             * has to *become* the caller's, which needs the returned value to be an allocation this
             * body made - a returned parameter or global has no such correspondence.
             */
            if(type && isMemoryType(opt.global, type)) {
                auto returned = opt.local[ret.value];
                if(returned->kind != Value::Alloc) return Nothing();

                candidate.resultLocal = ((InstAlloc&)*returned).local;
                if(candidate.resultLocal >= callee->localCount()) return Nothing();
            }
        }

        return Just(::move(candidate));
    }

    /*
     * Whether this call site is worth what the copy costs.
     *
     * The budget is the callee's size against a limit built from what the *call* looks like, and
     * every term is named in `InlinePolicy`. A call site that clears the ceiling is refused whatever
     * else it has going for it.
     */
    bool worthInlining(Candidate& candidate, InstCall& call) {
        if(candidate.size > policy.ceiling) return false;

        auto sites = callSites.getValue(U32(candidate.pointer));
        auto count = sites ? sites.unwrap() : U32(0);

        auto limit = I64(policy.budget);
        if(count <= 1) limit += policy.soleCallSite;
        else if(count >= policy.manyCallSites) limit -= policy.manyPenalty;
        else limit -= policy.repeatedPenalty;

        U32 constants = 0;
        for(auto argument: call.args.contents(opt.local)) {
            if(!argument) continue;

            switch(opt.local[argument]->kind) {
                case Value::ConstInt: case Value::ConstFloat: case Value::ConstDouble:
                    constants++;
                    break;
                default:
                    break;
            }
        }

        limit += I64(min(constants, policy.constantCap)) * policy.constantArgument;

        for(auto& parameter: candidate.parameters) {
            if(parameter.binding == Binding::Borrowed) limit += policy.mutableBorrow;
        }

        if(candidate.resultLocal != Candidate::kNone) limit += policy.memoryResult;
        if(candidate.callee->inlineHint) limit += policy.requested;

        return I64(candidate.size) <= limit;
    }

    /*
     * The clone itself.
     *
     * One arena holds the whole program - `OptContext::local` is the program's, not a module's - so
     * a type, a constant, a global and a function pointer are the same handle in the caller as in
     * the callee, and none of them needs translating. What does need translating is exactly three
     * things: a value defined in the body, a block (there is only one, and it disappears), and a
     * local index, which a `Place` carries by number.
     */
    struct Clone {
        HashMap<U32, U32> values;
        Array<U32> locals;
        Array<Inst*> emitted;
        Block* into = nullptr;
    };

    /*
     * One operand, against the caller.
     *
     * A constant is copied rather than shared, and that is not tidiness. A constant belongs to no
     * block, so nothing about it is *wrong* in another function - but `mapConstant` in
     * resolve/lower.cpp materializes each one once and caches the result in a map that lives for
     * the whole program, keyed by the resolve handle. That cache is correct today because a handle
     * is only ever reached from the one function that built it, and inlining is the first thing that
     * would have made two functions share one: the second would have got the first's `LowerImm`,
     * in the first's entry block, which `validateLowerModule` reports as a value from the wrong
     * function. Copying keeps the invariant the cache rests on rather than weakening the cache.
     *
     * Everything else the body names is genuinely outside it - a global, a callee, a type - and
     * those are program-level handles that mean the same thing here.
     */
    ModulePtr<Value> mapValue(Clone& clone, Candidate& candidate, InstCall& call, ModulePtr<Value> value) {
        if(!value) return nullptr;

        if(auto found = clone.values.getValue(value)) return ModulePtr<Value>(found.unwrap());

        auto& module = *opt.module;
        auto& function = *opt.function;
        auto& constant = *opt.local[value];
        Value* copy = nullptr;

        switch(constant.kind) {
            case Value::ConstInt:
                copy = addConstant<ConstInt>(module, function, *clone.into, constant.source,
                                             constant.type, ((ConstInt&)constant).value);
                break;
            case Value::ConstFloat:
                copy = addConstant<ConstFloat>(module, function, *clone.into, constant.source,
                                               constant.type, ((ConstFloat&)constant).value);
                break;
            case Value::ConstDouble:
                copy = addConstant<ConstDouble>(module, function, *clone.into, constant.source,
                                               constant.type, ((ConstDouble&)constant).value);
                break;
            default:
                return value;
        }

        auto copied = (ModulePtr<Value>)(copy - opt.local);
        *clone.values.add(U32(value)).value = U32(copied);
        return copied;
    }

    /*
     * A place, rebuilt against the caller.
     *
     * A local root is renumbered. A root that is the `&` parameter's local becomes a *borrow* root
     * on whatever the caller passed - which is what makes `n = n + 1` in the callee into a read and
     * a write of the caller's own storage without this pass having to know what that storage is.
     */
    Place clonePlace(Clone& clone, Candidate& candidate, InstCall& call, const Place& place) {
        Place result;
        result.root = place.root;
        result.global = place.global;
        result.local = place.local;
        result.pointer = mapValue(clone, candidate, call, place.pointer);

        if(place.root == PlaceRoot::Local) {
            auto rewritten = false;

            for(Size i = 0; i < candidate.parameters.size(); i++) {
                auto& parameter = candidate.parameters[i];
                if(parameter.binding != Binding::Borrowed || parameter.local != place.local) continue;

                result.root = PlaceRoot::Borrow;
                result.local = 0;
                result.pointer = call.args.get(opt.local, i);
                rewritten = true;
                break;
            }

            if(!rewritten) result.local = clone.locals[place.local];
        }

        auto& projections = const_cast<Place&>(place).projections;
        for(Size i = 0; i < projections.size(); i++) {
            auto projection = projections.get(opt.local, i);
            projection.value = mapValue(clone, candidate, call, projection.value);
            result.projections.push(opt.program.arena, projection);
        }

        return result;
    }

    Inst* cloneInstruction(Clone& clone, Candidate& candidate, InstCall& call, Block& into,
                           Value& instruction) {
        auto& module = *opt.module;
        auto& function = *opt.function;
        auto source = instruction.source;
        auto name = instruction.name;
        auto type = instruction.type;

        auto value = [&](ModulePtr<Value> operand) {
            return mapValue(clone, candidate, call, operand);
        };

        auto place = [&](const Place& from) {
            return clonePlace(clone, candidate, call, from);
        };

        switch(instruction.kind) {
            case Value::Alloc: {
                auto& alloc = (InstAlloc&)instruction;
                auto cloned = createInst<InstAlloc>(module, function, into, source, name, type,
                                                    clone.locals[alloc.local]);

                // The escape decision travels with the allocation. A callee local that went to the
                // heap goes to the heap here, and one the callee released itself is released here -
                // this frame outlives the region the call occupied, so neither answer changes.
                cloned->storage = alloc.storage;
                cloned->releasedHere = alloc.releasedHere;
                cloned->storageFlag = value(alloc.storageFlag);
                cloned->closure = alloc.closure;
                return (Inst*)cloned;
            }
            case Value::LoadPlace:
                return (Inst*)createInst<InstLoadPlace>(module, function, into, source, name, type,
                                                        place(((InstLoadPlace&)instruction).place));
            case Value::Init:
            case Value::Assign: {
                auto& write = (InstInit&)instruction;
                return (Inst*)createInst<InstInit>(module, function, into, source, name, type,
                                                   place(write.place), value(write.value),
                                                   instruction.kind);
            }
            case Value::Borrow: {
                auto& borrow = (InstBorrow&)instruction;
                return (Inst*)createInst<InstBorrow>(module, function, into, source, name, type,
                                                     place(borrow.place), borrow.mut);
            }
            case Value::Copy: {
                auto& copy = (InstCopy&)instruction;
                auto cloned = createInst<InstCopy>(module, function, into, source, name, type,
                                                   place(copy.place));
                cloned->copy = copy.copy;
                cloned->local = copy.local == maxLimit<U32> ? maxLimit<U32> : clone.locals[copy.local];
                return (Inst*)cloned;
            }
            case Value::TypeMetric: {
                auto& metric = (InstTypeMetric&)instruction;
                return (Inst*)createInst<InstTypeMetric>(module, function, into, source, name, type,
                                                         metric.of, metric.metric);
            }
            case Value::Symbol: {
                auto& symbol = (InstSymbol&)instruction;
                return (Inst*)createInst<InstSymbol>(module, function, into, source, name, type,
                                                     symbol.callee, symbol.global);
            }
            case Value::Cast: case Value::Neg: case Value::Not: {
                auto& unary = (InstUnary&)instruction;
                return (Inst*)createInst<InstUnary>(module, function, into, source, name, type,
                                                    instruction.kind, value(unary.from));
            }
            case Value::Cmp: {
                auto& compare = (InstCmp&)instruction;
                return (Inst*)createInst<InstCmp>(module, function, into, source, name, type,
                                                  value(compare.lhs), value(compare.rhs), compare.cmp);
            }
            case Value::Add: case Value::Sub: case Value::Mul: case Value::Div: case Value::Rem:
            case Value::Shl: case Value::Shr: case Value::Sar:
            case Value::And: case Value::Or: case Value::Xor: {
                auto& binary = (InstBinary&)instruction;
                return (Inst*)createInst<InstBinary>(module, function, into, source, name, type,
                                                     instruction.kind, value(binary.lhs), value(binary.rhs));
            }
            case Value::Call: {
                auto& inner = (InstCall&)instruction;
                auto cloned = createInst<InstCall>(module, function, into, source, name, type,
                                                   inner.callee);

                for(auto argument: inner.args.contents(opt.local)) {
                    cloned->args.push(opt.program.arena, value(argument));
                }

                cloned->local = inner.local == maxLimit<U32> ? maxLimit<U32> : clone.locals[inner.local];
                return (Inst*)cloned;
            }
            default:
                // `describe` refused every other kind before this call site was ever considered.
                return nullptr;
        }
    }

    /*
     * One call, replaced.
     *
     * The caller's local table grows by the callee's locals, minus the `&` parameters' - those name
     * the caller's own storage and are rewritten to borrow roots instead of being given slots here.
     * A memory-typed result reuses the slot the call already had rather than adding one, which is
     * what keeps the caller's existing places rooted in it pointing at the same storage.
     */
    bool inlineCall(Block& block, Size index, ModulePtr<Inst> pointer) {
        auto& call = (InstCall&)*opt.local[pointer];
        if(!call.callee) return false;
        if(call.callee == (ModulePtr<Function>)(opt.function - opt.local)) return false;

        auto described = describe(call.callee);
        if(!described) return false;

        auto candidate = described.unwrap();
        if(candidate.callee->args.size() != call.args.size()) return false;

        /*
         * The `&` arguments, which have to be borrows of storage that exists rather than of storage
         * a target stood in for.
         *
         * Design.md's tier 1: a mutable borrow of a *packed* field has no address to hand over, so
         * a target materializes the field into a temporary, passes that, and writes it back when the
         * loan ends - and the point the loan ends is the call. Splicing the call away takes the
         * write-back with it, which is a value silently not stored: `flushWritebacks` in
         * codegen/js/inst.cpp is that write-back, and it is keyed on the call node.
         *
         * A borrow of a *whole local* is never that. There is nothing above it to be packed into, so
         * both targets hand over the storage itself - which is also the case worth inlining, since
         * the box a managed target keeps such a local in is exactly the allocation that stops
         * existing once the callee's reads and writes are the caller's own.
         *
         * Anything that is not an `InstBorrow` is a reference the program already had - a `&`
         * parameter passed straight on, a borrow returned by something - and nothing was
         * materialized for it.
         */
        for(Size i = 0; i < candidate.parameters.size(); i++) {
            if(candidate.parameters[i].binding != Binding::Borrowed) continue;

            auto argument = call.args.get(opt.local, i);
            if(!argument || opt.local[argument]->kind != Value::Borrow) continue;

            auto& borrow = (InstBorrow&)*opt.local[argument];
            if(borrow.place.root != PlaceRoot::Local) return false;
            if(borrow.place.projections.isNotEmpty()) return false;
        }

        if(!worthInlining(candidate, call)) return false;

        Clone clone;
        clone.into = &block;
        auto& module = *opt.module;

        /*
         * The caller's slots for this call's result, of which there can be more than one.
         *
         * `call.local` is the slot the call was *given*, and it is not always the slot the body
         * reads through: a class default reached through an instance ends up with two slots naming
         * one call, and `storageOf` in opt_arg.cpp - which is what wrote the reads, when it took
         * the record apart at the next call - answers with the lowest. So the callee's returned
         * storage is mapped onto the lowest, and every one of them is repointed at the clone
         * afterwards. Getting this wrong is invisible in the resolve IR and shows up as a backend
         * reading a local nothing allocated.
         */
        Array<U32> resultSlots;
        for(U32 local = 0; local < opt.function->localCount(); local++) {
            if(opt.function->localAt(opt.local, local).value != (ModulePtr<Value>)pointer) continue;

            resultSlots.push(local);
        }

        auto resultSlot = resultSlots.size() ? resultSlots[0] : call.local;

        for(U32 local = 0; local < candidate.callee->localCount(); local++) {
            auto slot = candidate.callee->localAt(opt.local, local);

            auto borrowed = false;
            for(auto& parameter: candidate.parameters) {
                if(parameter.binding == Binding::Borrowed && parameter.local == local) borrowed = true;
            }

            if(borrowed) {
                // Never read: every place rooted in it is rewritten to a borrow root instead. Given
                // an out-of-range value so that a path missing that rewrite trips rather than
                // silently naming local zero.
                clone.locals.push(maxLimit<U32>);
                continue;
            }

            if(local == candidate.resultLocal && resultSlot != maxLimit<U32>) {
                clone.locals.push(resultSlot);
                continue;
            }

            clone.locals.push(opt.function->addLocal(module, slot.type, slot.name, nullptr,
                                                     slot.convention));
        }

        // The callee's arguments, as the values the caller passed. A `&` parameter's `Arg` is
        // reached through its local rather than as an operand, so this covers the value case and
        // costs nothing in the other.
        for(Size i = 0; i < candidate.callee->args.size(); i++) {
            auto argPointer = (ModulePtr<Value>)candidate.callee->args.get(opt.local, i);
            *clone.values.add(U32(argPointer)).value = U32(call.args.get(opt.local, i));
        }

        for(auto instructionPointer: candidate.body->instructions.contents(opt.local)) {
            auto& instruction = *opt.local[instructionPointer];
            auto cloned = cloneInstruction(clone, candidate, call, block, instruction);
            if(!cloned) return false;

            *clone.values.add(U32(instructionPointer)).value = U32(cloned - opt.local);
            clone.emitted.push(cloned);
        }

        /*
         * What gives each new local its storage, before anything rooted in one is added to a block.
         *
         * From the callee's own slot rather than from the instruction kind, which is what makes this
         * complete: an `Alloc` is the common case, but a `Copy` of an aggregate and a `Call`
         * returning one each own a local too, and a slot left holding null is storage that later
         * looks to every pass like a local nothing allocated. Order matters as well - `addPlaceUse`
         * reads the slot to record a use, and `insertInstructions` below is what runs it.
         */
        for(U32 local = 0; local < candidate.callee->localCount(); local++) {
            auto index = clone.locals[local];
            if(index == maxLimit<U32>) continue;

            auto source = candidate.callee->localAt(opt.local, local).value;
            if(!source) continue;

            auto mapped = clone.values.getValue(U32(source));
            if(!mapped) continue;

            auto slot = opt.function->localAt(opt.local, index);
            slot.value = ModulePtr<Value>(mapped.unwrap());
            opt.function->locals.set(opt.local, index, slot);
        }

        insertInstructions(opt, block, index, clone.emitted);

        auto& ret = (InstRet&)*opt.local[candidate.body->terminator];
        auto result = ret.value ? mapValue(clone, candidate, call, ret.value) : nullptr;

        if(result && opt.local[pointer]->uses.isNotEmpty()) {
            replaceValue(opt, (ModulePtr<Value>)pointer, result);
        }

        // And the slots that named the call as their storage, which is not a use and so is not
        // something `replaceValue` reaches - a place rooted in one of them is recorded against the
        // *value* the slot holds, and that value is about to stop existing.
        for(auto local: resultSlots) {
            auto slot = opt.function->localAt(opt.local, local);
            slot.value = result;
            opt.function->locals.set(opt.local, local, slot);
        }

        // By hand rather than through `eraseInstruction`, which asserts that nothing reads the
        // instruction - true of a call whose result was replaced above and not of one returning
        // unit, whose place-root uses are recorded on the locals it named.
        for(auto argument: call.args.contents(opt.local)) dropUse(opt, argument, pointer);

        for(Size i = 0; i < block.instructions.size(); i++) {
            if(block.instructions.get(opt.local, i) != pointer) continue;

            block.instructions.remove(opt.local, i);
            break;
        }

        opt.changed = true;
        return true;
    }

    // How many times each function in the program is named by a `Call`. Recomputed per round, since
    // inlining is exactly the thing that changes it.
    void countCallSites() {
        callSites.clear();

        for(auto module: opt.program.modules) {
            for(auto pointer: module->functionOrder.contents(opt.local)) {
                for(auto blockPointer: opt.local[pointer]->blocks.contents(opt.local)) {
                    for(auto instructionPointer: opt.local[blockPointer]->instructions.contents(opt.local)) {
                        auto& instruction = *opt.local[instructionPointer];
                        if(instruction.kind != Value::Call) continue;

                        auto callee = ((InstCall&)instruction).callee;
                        if(!callee) continue;

                        auto entry = callSites.add(U32(callee));
                        *entry.value = entry.existed ? *entry.value + 1 : 1;
                    }
                }
            }
        }
    }

    bool runFunction(Function& function) {
        opt.function = &function;
        rebuildUses(opt);

        auto inlined = false;

        for(auto blockPointer: function.blocks.contents(opt.local)) {
            auto block = opt.local[blockPointer];

            /*
             * Forwards, and re-reading the size each step, because the splice inserts the callee's
             * instructions in front of the call and removes the call itself: the net effect on the
             * index is that the position now holds whatever followed the call, so the walk does not
             * advance on a successful inline. A callee that itself contains a call is therefore
             * considered on this pass too rather than on the next round.
             */
            for(Size i = 0; i < block->instructions.size();) {
                auto pointer = block->instructions.get(opt.local, i);

                if(opt.local[pointer]->kind != Value::Call) {
                    i++;
                    continue;
                }

                if(inlineCall(*block, i, pointer)) inlined = true;
                else i++;
            }
        }

        return inlined;
    }
};

// A cap on the cascade rather than a termination proof, on the same terms as the driver's own round
// limit: a chain of callees each of which calls the next collapses a level per round, and a mutual
// recursion that would otherwise grow for ever stops here.
constexpr Size kMaxInlineRounds = 3;

}

void inlineCalls(OptContext& opt) {
    auto policy = policyFor(opt.context.settings.inlining, opt.repr.target.family);

    // `ceiling` is zero only at `InlineLevel::None`, where nothing qualifies and the walk below
    // would be a whole-program traversal that decided nothing.
    if(policy.ceiling == 0) return;

    Inliner inliner { opt, policy };
    addressTaken(opt, inliner.taken);


    for(Size round = 0; round < kMaxInlineRounds; round++) {
        inliner.countCallSites();
        auto inlined = false;

        for(auto module: opt.program.modules) {
            opt.module = module;

            for(auto pointer: module->functionOrder.contents(opt.local)) {
                auto function = opt.local[pointer];
                if(function->signature || function->blocks.isEmpty()) continue;

                inlined = inliner.runFunction(*function) || inlined;
            }
        }

        if(!inlined) break;
    }


}
