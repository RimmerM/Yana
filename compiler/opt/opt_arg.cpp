#include "opt_pass.h"
#include "../resolve/type.h"

/*
 * Record arguments that become one argument per field.
 *
 * `fn sum(pair: Pair)` is one parameter that arrives as an address on native and as an object on
 * JS, and both of those exist only because the record does. Passing `pair.x` and `pair.y` instead
 * removes the address and, where the caller was building the record only in order to pass it, the
 * record too - which composes with opt_scalar.cpp exactly the way that pass composes with
 * forwarding: the construction becomes field writes, the field writes are forwarded into the
 * argument loads, and the local is left with no readers.
 *
 * ## Why this is a calling convention rather than an optimization
 *
 * A caller and a callee decide it separately and never see each other, so the rule has to be a
 * function of what both of them can read: the declaration. That is the same constraint the JS
 * narrow-reference flattening already works under - see refIsFlattened and signatureFlattens in
 * codegen/js - and it rules out the two shapes an optimization would naturally have. It cannot be
 * per call site, because a witness slot, a closure and an indirect call have to agree with a direct
 * one. It cannot be per parameter either, or rather it can, but the arity guard below is about the
 * whole signature and a per-parameter answer would need the whole signature anyway.
 *
 * Being a convention is also why it lands here rather than in either backend: one rule, applied to
 * the IR both of them read, cannot be a rule the two targets disagree about. What each backend then
 * emits is what it already emits for a function with more parameters.
 *
 * ## What the arity limit is, and why it is not the reference one
 *
 * `kFlatArityLimit` in codegen/js is 24 because that is where extra arguments stopped being free
 * for a *reference triple*, and a triple is always worth flattening: the object it replaces is one
 * the caller would have had to allocate on every call. A record's fields are not that. The record
 * may already exist, and then flattening moves N field reads to the caller and adds N-1 arguments
 * and saves nothing.
 *
 * Measured in benchmark/bits53-js/arity-benchmark.mjs, through a callee V8 declines to inline, as
 * the share of a signature's call sites that must be constructing the record for the flattening to
 * come out ahead:
 *
 *      fields    2     4     6     8    12    16    20    24    32
 *      share    0%   15%   22%   26%   37%   53%   65%   75%   76%
 *
 * `kMaxRecordFields` is 12: the last width where the rule pays off without needing most call sites
 * to be of the constructing kind. The arity ceiling stays 24 and is now shared - a signature is
 * flattened once, counting flattened fields and ordinary parameters together - which also makes it
 * compose with the reference rule for free, since by the time codegen/js counts a signature's arity
 * the fields are already parameters in it.
 *
 * ## What is declined, and why each one
 *
 * A function whose *address* is taken is declined, because an address has no declaration at the
 * call site for the rule to be read from: a witness table slot, a closure's code word and a
 * teardown reached through a descriptor are all called through a signature or through nothing at
 * all. Which functions those are is not guessed at - `addressTaken` below reproduces the one
 * enumeration that is already authoritative about how a function can be reached, the reachability
 * walk in resolve/module.cpp, where a `Call` or a `GenCall` names a callee and everything else
 * holds an address.
 *
 * That leaves the source-declared functions, and it leaves *all* of them: a plain function used as
 * a value does not have its address taken either, because `functionValue` interposes a one-line
 * thunk and takes the thunk's. The thunk is a code word, keeps its own shape, and the direct call
 * inside it is flattened like any other - which is also why the answer here does not depend on who
 * else in the program is looking. A separately compiled caller reaches the same conclusion about a
 * declaration for the same reason.
 *
 * Per parameter, on top of the type being a decomposable record:
 *
 *  - **`&` and `->` are declined.** A mutable borrow arrives as an address precisely so the callee
 *    can write back through it, and a sink transfers ownership. Only the ordinary immutable borrow
 *    is a read of the caller's value, which is what a copy of each field is a faithful substitute
 *    for.
 *  - **A type that owes a teardown is declined**, for the same reason opt_scalar.cpp declines one:
 *    field-by-field is only the same thing as whole-value where relocating is bytes rather than a
 *    call.
 *  - **`return` and a retained argument are declined.** The parameter is rebuilt into storage in
 *    the callee's own frame, so a borrow of it that outlives the call would outlive the frame. The
 *    declaration says the first (`returnRoot`) and the summary the second, and both are part of
 *    what a caller may read about a callee without its body.
 *  - **A record this target represents as one scalar is declined**, because there is nothing to
 *    remove: it is already being passed in a register.
 */

namespace {

constexpr Size kMaxRecordFields = 12;
constexpr Size kMaxFlatArity = 24;

// A record nested inside a record is taken apart too, but not indefinitely: the limit is on the
// path length rather than on the field count, which the leaf budget already bounds.
constexpr Size kMaxDepth = 4;

/*
 * One parameter's replacement, and one signature's.
 *
 * Held flat - the projections of every leaf in one array, the leaves of every argument in another -
 * because the natural shape is an array of arrays of arrays and this is read far more often than it
 * is built.
 */
struct Leaf {
    Size pathStart = 0;
    Size pathCount = 0;
    TypePtr type = nullptr;
    StringId name = 0;
};

struct Plan {
    SmallArray<Projection, 8> paths;
    SmallArray<Leaf, 8> leaves;

    // Per argument, in declaration order. A count of zero is a parameter that stays as it is.
    SmallArray<U32, 8> start;
    SmallArray<U32, 8> count;

    bool any = false;

    // Emptied rather than replaced, which is what a plan reused across functions wants anyway - see
    // SmallArray on why `plan = Plan {}` is not the way to say this.
    void clear() {
        paths.clear();
        leaves.clear();
        start.clear();
        count.clear();
        any = false;
    }
};

// The place one leaf names, relative to whatever place holds the record.
Place leafPlace(OptContext& opt, const Plan& plan, const Leaf& leaf, Place base) {
    Place result = base;

    result.projections = {};
    for(Size i = 0; i < base.projections.size(); i++) {
        result.projections.push(opt.program.arena, base.projections.get(opt.local, i));
    }

    for(Size i = 0; i < leaf.pathCount; i++) {
        result.projections.push(opt.program.arena, plan.paths[leaf.pathStart + i]);
    }

    return result;
}

// `pair$x`, so that the emitted parameters still say which record they came out of.
//
// Through `addQualifiedName` with one segment rather than `addUnqualifiedName`, because that is the
// overload that *copies*: the unqualified one keeps the pointer it was handed, and a name built in a
// local buffer then outlives the buffer. What that looked like was correct code with parameters
// called `et_wU`.
StringId leafName(OptContext& opt, StringId prefix, StringId field, Size index) {
    if(!prefix) return field;

    StringBuilder text;
    text << opt.context.findName(prefix) << "$";

    // A tuple's fields have no names, so the position is the only thing left to tell two of them
    // apart - and `{Int, Int}` flattening to two parameters both called `pair` is a signature the
    // reader cannot follow even where the compiler can.
    if(field) {
        text << opt.context.findName(field);
    } else {
        // Through `show` rather than `<<`, whose only integral overload takes a `char` and would
        // append the code point rather than the digit - field zero came out as a NUL byte.
        char digits[16];
        auto count = show(U64(index), digits, sizeof(digits));
        text << Buffer<const char> { digits, count };
    }

    return opt.context.addQualifiedName(text.pointer(), text.size(), 1);
}

/*
 * The leaves of one type, appended to the plan.
 *
 * A leaf is anything that is already a value - a scalar, a pointer, a record this target holds in a
 * register. Everything else has to be a single-constructor aggregate whose fields are themselves
 * leaves, or the whole parameter is declined: half a record flattened is not a convention.
 */
bool collectLeaves(OptContext& opt, Plan& plan, TypePtr type, StringId name,
                   Array<Projection>& path, Size depth) {
    if(!type || plan.leaves.size() > kMaxRecordFields) return false;

    // A unit field occupies nothing and has no value to pass. It cannot be a leaf, and a record
    // made only of them would flatten to no arguments at all.
    if(isUnit(opt.global, type)) return false;

    if(!isMemoryType(opt.global, type)) {
        auto leaf = Leaf { plan.paths.size(), path.size(), type, name };
        for(auto projection: path) plan.paths.push(projection);

        plan.leaves.push(leaf);
        return true;
    }

    if(depth >= kMaxDepth) return false;

    auto fields = fieldsOf(opt, type);
    if(!fields.exists()) return false;

    if(auto constructor = fields.constructor) {
        path.push(Projection { ProjectionKind::Downcast, constructor.unwrap(), nullptr });
    }

    for(U16 i = 0; i < U16(fields.count); i++) {
        path.push(Projection { ProjectionKind::Field, i, nullptr });

        auto member = leafName(opt, name, fieldName(opt, fields, i), i);
        if(!collectLeaves(opt, plan, fieldType(opt, fields, i), member, path, depth + 1)) return false;

        path.pop();
    }

    if(fields.constructor) path.pop();
    return true;
}

/*
 * Every function whose address something holds.
 *
 * This is the enumeration in `markProgramReachable` (resolve/module.cpp) minus the two cases that
 * name a callee rather than take one: `Call` and `GenCall`. That is the list to copy because it is
 * the one the program is already trusted to be complete - a way of reaching a function that is
 * missing from it is a function the linker drops - and the two have to stay in step, so a case
 * added there is a case to add here.
 *
 * `allocateHeap` and `freeHeap` are in for the same reason the walk names them: lowering emits calls
 * to them that no instruction in this IR mentions, so their signatures are not this pass's to move.
 *
 * Shared with opt_inline.cpp, which asks the same question for the same reason: a function reached
 * through an address is one no call site can be rewritten on behalf of.
 */

}

void addressTaken(OptContext& opt, HashMap<U32, bool>& taken) {
    auto hold = [&](ModulePtr<Function> function) {
        if(function) *taken.add(U32(function)).value = true;
    };

    hold(opt.program.allocateHeap);
    hold(opt.program.freeHeap);

    for(auto module: opt.program.modules) {
        for(auto globalPointer: module->globalOrder.contents(opt.local)) {
            for(auto slot: opt.local[globalPointer]->table.contents(opt.local)) hold(slot.function);
        }

        for(auto pointer: module->functionOrder.contents(opt.local)) {
            auto function = opt.local[pointer];

            for(auto blockPointer: function->blocks.contents(opt.local)) {
                for(auto instructionPointer: opt.local[blockPointer]->instructions.contents(opt.local)) {
                    auto& instruction = *opt.local[instructionPointer];

                    switch(instruction.kind) {
                        case Value::Symbol:
                            hold(((InstSymbol&)instruction).callee);
                            break;
                        case Value::Move:
                            hold(((InstMove&)instruction).sink);
                            break;
                        case Value::Copy:
                            hold(((InstCopy&)instruction).copy);
                            break;
                        case Value::Drop:
                            hold(((InstDrop&)instruction).drop);
                            hold(((InstDrop&)instruction).reclaim);
                            break;
                        default:
                            break;
                    }
                }
            }
        }
    }
}

namespace {

// Whether anything about the function itself rules it out - see the header comment for what each of
// these is and why an address rather than a name is the thing they have in common.
bool functionCanFlatten(OptContext& opt, ModulePtr<Function> pointer, Function& function,
                        HashMap<U32, bool>& taken) {
    if(taken.get(U32(pointer))) return false;

    // A generic body's parameters have no layout to take apart, and a code word's first parameter is
    // a convention rather than a declaration. Both are already excluded above - one is only ever
    // reached by `GenCall` through an environment, the other only ever by its address - and both are
    // named here anyway, because relying on that would be relying on the absence of an instruction.
    if(function.gen || function.intrinsic || function.signature || function.takesEnv) return false;
    if(function.blocks.isEmpty()) return false;

    // A summary that was never computed, or one that gave up, says nothing about whether a borrow of
    // a parameter outlives the call - which is the question below that has no other answer.
    if(!function.summary.ready || function.summary.opaque) return false;

    return true;
}

bool argumentCanFlatten(OptContext& opt, Function& function, Size index) {
    auto arg = opt.local[function.args.get(opt.local, index)];

    if(arg->convention != ast::BindType::Borrow) return false;
    if(arg->returnRoot) return false;

    if(index < function.summary.args.size() &&
       function.summary.args.get(opt.local, index).retained) {
        return false;
    }

    auto type = arg->type;
    if(!isMemoryType(opt.global, type)) return false;
    if(needsTeardown(*function.module, type)) return false;

    // Already one register on this target, or a type with no layout at all. Neither has fields to
    // pass in place of it.
    auto& repr = opt.repr.of(type);
    if(repr.scalarBits || repr.opaque) return false;

    return true;
}

/*
 * One signature's plan, computed from the declaration and nothing else.
 *
 * Every caller asks this about its callee and every callee asks it about itself, so it has to be a
 * pure function of the declaration - and in particular it must be asked *before* anything is
 * rewritten, since rewriting a signature is exactly the thing that would change the answer. That is
 * what splits the pass into two program-wide phases below.
 */
bool planFor(OptContext& opt, ModulePtr<Function> pointer, Function& function,
             HashMap<U32, bool>& taken, Plan& plan) {
    plan.clear();
    if(!functionCanFlatten(opt, pointer, function, taken)) return false;

    Size arity = 0;

    for(Size i = 0; i < function.args.size(); i++) {
        auto start = plan.leaves.size();
        auto flattened = false;

        if(argumentCanFlatten(opt, function, i)) {
            auto arg = opt.local[function.args.get(opt.local, i)];

            Array<Projection> path;
            flattened = collectLeaves(opt, plan, arg->type, arg->name, path, 0);

            // A refusal partway down leaves whatever it had already appended, which belongs to no
            // argument. Dropping it here keeps the plan describing exactly the parameters it claims.
            if(!flattened) {
                while(plan.leaves.size() > start) {
                    plan.paths.resize(plan.leaves[plan.leaves.size() - 1].pathStart);
                    plan.leaves.pop();
                }
            }
        }

        auto count = flattened ? plan.leaves.size() - start : 0;
        if(count > kMaxRecordFields) {
            while(plan.leaves.size() > start) {
                plan.paths.resize(plan.leaves[plan.leaves.size() - 1].pathStart);
                plan.leaves.pop();
            }

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

/*
 * The place a value came out of, or nothing where it came out of no storage this function can name.
 *
 * The same two answers ExprResolver::findPlace gives, for the same reason: a value loaded out of a
 * place is addressed through that place again rather than through a copy, and every other value of
 * a memory type is some local's storage - an allocation, a call's result, an exchanged temporary.
 */
Maybe<Place> storageOf(OptContext& opt, ModulePtr<Value> value) {
    if(!value) return Nothing();

    if(opt.local[value]->kind == Value::LoadPlace) {
        return Just(((InstLoadPlace*)opt.local[value])->place);
    }

    for(U32 i = 0; i < opt.function->localCount(); i++) {
        if(opt.function->localAt(opt.local, i).value == value) return Just(Place::inLocal(i));
    }

    return Nothing();
}

/*
 * A fresh local holding one value, for the arguments `storageOf` cannot name a place for.
 *
 * There is nothing to project out of a value of an aggregate type without storage to project from,
 * so this makes some. The write is a copy, which is only the same thing as what was there because a
 * type owing a teardown was declined long before this.
 *
 * No fixture reaches it: every memory-typed value the resolver produces is either a load of a place
 * or some local's storage. It is here because the callee has already been flattened by the time a
 * caller finds out, so "decline this argument" is not one of the answers a call site may give.
 */
Place materialize(OptContext& opt, Block& block, InstList& into, Value& at, ModulePtr<Value> value) {
    auto type = opt.local[value]->type;

    auto allocation = createInst<InstAlloc>(*opt.module, *opt.function, block, at.source, 0, type,
                                            maxLimit<U32>);
    auto storage = (ModulePtr<Value>)(allocation - opt.local);

    allocation->local = opt.function->addLocal(*opt.module, type, 0, storage);
    into.push(allocation);

    into.push(createInst<InstInit>(*opt.module, *opt.function, block, at.source, 0,
                                   opt.program.scalar.unit, Place::inLocal(allocation->local),
                                   value, Value::Init));

    return Place::inLocal(allocation->local);
}

/*
 * One call site, rewritten against its callee's plan.
 *
 * Answers how many instructions it put in front of the call, so that the walk over the block can
 * step past them: they are loads of the caller's own storage and there is nothing in them for this
 * pass to look at again.
 */
Size rewriteCall(OptContext& opt, HashMap<U32, bool>& taken, Block& block, Size index, Inst& call,
                 ModuleList<ModulePtr<Value>, false>& args, ModulePtr<Function> callee) {
    if(!callee) return 0;

    Plan plan;
    if(!planFor(opt, callee, *opt.local[callee], taken, plan)) return 0;

    // A call whose argument list does not match the declaration is one of the compiler's own - an
    // erased shape with hidden storage in front - and the positions would not line up.
    if(args.size() != plan.count.size()) return 0;

    InstList loads;
    ValueList replacement;

    for(Size i = 0; i < args.size(); i++) {
        auto value = args.get(opt.local, i);

        if(!plan.count[i]) {
            replacement.push(value);
            continue;
        }

        auto base = storageOf(opt, value);
        auto record = base ? base.unwrap() : materialize(opt, block, loads, call, value);

        for(Size j = 0; j < plan.count[i]; j++) {
            auto& leaf = plan.leaves[plan.start[i] + j];

            auto load = createInst<InstLoadPlace>(*opt.module, *opt.function, block, call.source,
                                                  leaf.name, leaf.type,
                                                  leafPlace(opt, plan, leaf, record));

            loads.push(load);
            replacement.push((ModulePtr<Value>)(load - opt.local));
        }
    }

    insertInstructions(opt, block, index, loads);

    // The argument list itself. Use lists are not repaired here and do not need to be: this whole
    // pass runs before the first `rebuildUses`, which recomputes every one of them from what the
    // instructions say.
    args.clear();
    for(auto value: replacement) args.push(opt.program.arena, value);

    return loads.size();
}

void rewriteCalls(OptContext& opt, HashMap<U32, bool>& taken, Function& function) {
    opt.function = &function;

    for(auto blockPointer: function.blocks.contents(opt.local)) {
        auto block = opt.local[blockPointer];

        for(Size i = 0; i < block->instructions.size(); i++) {
            auto instruction = opt.local[block->instructions.get(opt.local, i)];

            switch(instruction->kind) {
                case Value::Call: {
                    auto& call = (InstCall&)*instruction;
                    i += rewriteCall(opt, taken, *block, i, call, call.args, call.callee);
                    break;
                }
                case Value::GenCall: {
                    // A generic callee is declined by `functionCanFlatten`, so this never rewrites
                    // anything today. It is here because the alternative is a call kind naming a
                    // callee that this pass does not know about, which is how a signature and its
                    // call sites come to disagree.
                    auto& call = (InstGenCall&)*instruction;
                    i += rewriteCall(opt, taken, *block, i, call, call.args, call.callee);
                    break;
                }
                default:
                    break;
            }
        }
    }
}

/*
 * One signature, and the prologue that puts the record back together.
 *
 * The body is not touched at all: the parameter's local becomes an ordinary allocation, the fields
 * are written into it, and every place that named the parameter still names the same thing. That is
 * what makes this safe to do without understanding the body - and it is not the pessimisation it
 * looks like, because the writes are what opt_place.cpp forwards out of and what opt_scalar.cpp
 * removes the allocation for once nothing reads it. A body that genuinely needs the record whole -
 * one that borrows it and passes the borrow on - keeps the storage, and correctly so.
 */
void flattenSignature(OptContext& opt, Function& function, const Plan& plan) {
    opt.function = &function;

    auto entryPointer = function.blocks.get(opt.local, 0);
    auto entry = opt.local[entryPointer];

    Array<ModulePtr<Arg>> arguments;
    InstList prologue;

    // The parameters that stop existing, and the allocations that stand in for them. A record
    // parameter is not only a place root: it is a *value* of an aggregate type, which is what a call
    // handing the whole record on names, and an `Alloc`'s result means exactly the same thing.
    ValueList retired;
    ValueList standIn;

    for(Size i = 0; i < function.args.size(); i++) {
        auto argPointer = function.args.get(opt.local, i);
        auto arg = opt.local[argPointer];

        if(!plan.count[i]) {
            arguments.push(argPointer);
            continue;
        }

        // The slot bindFunctionArgs made for the parameter. It held the caller's address; it is
        // about to hold this frame's storage, which is the only thing about the body that changes.
        auto local = maxLimit<U32>;
        for(U32 j = 0; j < function.localCount(); j++) {
            if(function.localAt(opt.local, j).value == (ModulePtr<Value>)argPointer) local = j;
        }

        assertTrue(local != maxLimit<U32>);

        auto allocation = createInst<InstAlloc>(*opt.module, *opt.function, *entry, arg->source,
                                                arg->name, arg->type, local);
        prologue.push(allocation);

        auto slot = function.localAt(opt.local, local);
        slot.value = (ModulePtr<Value>)(allocation - opt.local);
        slot.borrowed = false;
        slot.convention = ast::BindType::Borrow;
        function.locals.set(opt.local, local, slot);

        retired.push((ModulePtr<Value>)argPointer);
        standIn.push(slot.value);

        for(Size j = 0; j < plan.count[i]; j++) {
            auto& leaf = plan.leaves[plan.start[i] + j];

            auto base = *opt.module->arena;
            auto fresh = new (opt.module->arena) Arg(entryPointer, leaf.type, U16(arguments.size()));

            fresh->name = leaf.name;
            fresh->source = arg->source;
            fresh->id = function.valueCounter++;

            auto value = (ModulePtr<Value>)(fresh - base);
            arguments.push((ModulePtr<Arg>)(fresh - base));

            prologue.push(createInst<InstInit>(*opt.module, *opt.function, *entry, arg->source, 0,
                                               opt.program.scalar.unit,
                                               leafPlace(opt, plan, leaf, Place::inLocal(local)),
                                               value, Value::Init));
        }
    }

    /*
     * Every remaining mention of a retired parameter, pointed at its allocation.
     *
     * A place rooted in the parameter's local needed nothing - the root is an index and the local
     * table is what says where its storage is, which the loop above has already updated. This is for
     * the other half: `call g, %it` hands the whole record on and names the parameter as a value,
     * and `ret %it` returns it. Both mean "the storage this names", which is what an `Alloc` result
     * is, so the substitution is exact.
     *
     * Done by walking rather than through `replaceValue`, because use lists at this point are
     * whatever the call rewriting above left them as. `rebuildUses` recomputes them all before the
     * first pass runs.
     */
    if(retired.isNotEmpty()) {
        auto substitute = [&](ModulePtr<Value> operand) {
            for(Size i = 0; i < retired.size(); i++) {
                if(operand == retired[i]) return standIn[i];
            }

            return operand;
        };

        for(auto blockPointer: function.blocks.contents(opt.local)) {
            auto block = opt.local[blockPointer];

            for(auto phi: block->phis.contents(opt.local)) {
                mapOperands(opt.local, *opt.local[phi], substitute);
            }

            for(auto instruction: block->instructions.contents(opt.local)) {
                mapOperands(opt.local, *opt.local[instruction], substitute);
            }

            if(block->terminator) mapOperands(opt.local, *opt.local[block->terminator], substitute);
        }
    }

    function.args.clear();
    for(auto argument: arguments) function.args.push(opt.program.arena, argument);

    // `index` is what a parameter answers about itself, so it has to say where the parameter now is.
    for(Size i = 0; i < function.args.size(); i++) {
        opt.local[function.args.get(opt.local, i)]->index = U16(i);
    }

    /*
     * The summary, kept the same length as the signature.
     *
     * Every leaf inherits what the record's entry said, which is conservative and uninteresting in
     * equal measure: a parameter only got here by being an unretained immutable borrow with no
     * `return` marker, so there is nothing in the entry for a leaf to inherit wrongly. Rebuilt at
     * all because a summary whose positions no longer match the arguments is a trap for whatever
     * reads it next.
     */
    Array<ArgSummary> summaries;
    for(Size i = 0; i < plan.count.size(); i++) {
        auto entryOf = i < function.summary.args.size()
            ? function.summary.args.get(opt.local, i) : ArgSummary {};

        for(Size j = 0; j < (plan.count[i] ? plan.count[i] : 1); j++) summaries.push(entryOf);
    }

    function.summary.args.clear();
    for(auto& summary: summaries) function.summary.args.push(opt.program.arena, summary);

    insertInstructions(opt, *entry, 0, prologue);
}

}

/*
 * Two phases over the whole program, and the order between them is the correctness argument.
 *
 * Every call site is rewritten first, while every signature still says what it was declared to say,
 * because the plan a caller applies has to be the plan the callee will apply to itself - and the
 * only way to guarantee that with one function computing both is to ask it before either side has
 * moved. Rewriting a function and its callers together would work only if no function were ever
 * both, which is not a property a program has.
 */
void flattenArguments(OptContext& opt) {
    HashMap<U32, bool> taken;
    addressTaken(opt, taken);

    for(auto module: opt.program.modules) {
        opt.module = module;

        for(auto pointer: module->functionOrder.contents(opt.local)) {
            auto function = opt.local[pointer];
            if(function->signature || function->blocks.isEmpty()) continue;

            rewriteCalls(opt, taken, *function);
        }
    }

    for(auto module: opt.program.modules) {
        opt.module = module;

        for(auto pointer: module->functionOrder.contents(opt.local)) {
            auto function = opt.local[pointer];

            Plan plan;
            if(planFor(opt, pointer, *function, taken, plan)) flattenSignature(opt, *function, plan);
        }
    }
}
