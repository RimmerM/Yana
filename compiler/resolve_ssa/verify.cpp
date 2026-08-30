#include "verify.h"

namespace ssa {

namespace {

/*
 * The dominator tree, by the iterative algorithm over reverse postorder.
 *
 * Needed and not optional. "Defined before used" in a graph is *dominance*, and the walk this
 * replaces approximated it with block index order - which is neither sound nor complete: a value
 * defined in one arm of a branch and used in the other has a lower index and does not dominate, and
 * a loop body legitimately uses a value defined in a block it precedes in index order. Both were
 * silent.
 *
 * It is also what makes the linear rule checkable: an owned value's consumption dataflow is only
 * meaningful over the blocks its definition dominates, and without that a value defined inside a
 * loop conflicts with itself across the back edge.
 */
struct Dominators {
    // Reverse postorder, and the position of each block in it. Unreachable blocks are absent from
    // the order and have no position, which is how the walk skips them without a second flag.
    Array<U32> order;
    Array<U32> position;
    Array<U32> idom;

    static constexpr U32 kNone = ~U32(0);

    void build(SsaBase base, Body& body) {
        auto count = body.blocks.size();

        order.clear();
        position.clear();
        idom.clear();

        for(U32 i = 0; i < count; i++) { position.push(kNone); idom.push(kNone); }
        if(!count) return;

        // Postorder by an explicit stack: a body is not bounded by anything that makes recursion
        // over its CFG safe.
        Array<U32> stack;
        Array<U32> next;
        Array<bool> seen;
        for(U32 i = 0; i < count; i++) { next.push(0); seen.push(false); }

        auto entry = base[body.entry]->index.index;
        stack.push(entry);
        seen[entry] = true;

        Array<U32> postorder;

        while(stack.size()) {
            auto index = stack[stack.size() - 1];
            auto block = body.block(base, BlockId(index));

            auto successors = successorsOf(base, *block);
            if(next[index] < successors.size()) {
                auto successor = successors[next[index]++];
                if(!seen[successor]) {
                    seen[successor] = true;
                    stack.push(successor);
                }
            } else {
                postorder.push(index);
                stack.pop();
            }
        }

        for(Size i = postorder.size(); i > 0; i--) {
            position[postorder[i - 1]] = U32(order.size());
            order.push(postorder[i - 1]);
        }

        idom[entry] = entry;

        auto changed = true;
        while(changed) {
            changed = false;

            for(auto index: order) {
                if(index == entry) continue;

                auto block = body.block(base, BlockId(index));
                U32 candidate = kNone;

                for(auto predecessorPointer: block->incoming.contents(base)) {
                    auto predecessor = base[predecessorPointer]->index.index;
                    if(position[predecessor] == kNone || idom[predecessor] == kNone) continue;

                    candidate = candidate == kNone ? predecessor : intersect(candidate, predecessor);
                }

                if(candidate != kNone && idom[index] != candidate) {
                    idom[index] = candidate;
                    changed = true;
                }
            }
        }
    }

    bool isReachable(U32 index) const { return position[index] != kNone; }

    bool dominates(U32 a, U32 b) const {
        if(!isReachable(a) || !isReachable(b)) return false;

        for(auto at = b; ; at = idom[at]) {
            if(at == a) return true;
            if(idom[at] == at) return false;
        }
    }

private:
    // The two blocks' common dominator, by walking both up the tree until they meet - the standard
    // step of the Cooper/Harvey/Kennedy formulation, over reverse-postorder numbers.
    U32 intersect(U32 a, U32 b) const {
        while(a != b) {
            while(position[a] > position[b]) a = idom[a];
            while(position[b] > position[a]) b = idom[b];
        }

        return a;
    }

    Array<U32> successorsOf(SsaBase base, Block& block) {
        Array<U32> result;

        auto count = block.instructions.size();
        if(!count) return result;

        auto last = base[block.instructions.get(base, count - 1)];
        if(!isTerminator(last->kind)) return result;

        forEachEdge(*last, [&](Edge& edge) {
            if(edge.target) result.push(base[edge.target]->index.index);
        });

        return result;
    }
};

/*
 * What is known about one owned value at one program point.
 *
 * Three states and a conflict marker, which is the whole lattice the linear rule needs.
 * `OutOfScope` is not the same as `Unconsumed`: a value whose definition does not dominate a point
 * has no obligation there, and a two-state dataflow that conflated the two would report a leak at
 * every exit a value's definition does not reach.
 */
enum class Linear: U8 {
    OutOfScope,
    Unconsumed,
    Consumed,
    Conflict,
};

static Linear merge(Linear a, Linear b) {
    if(a == b) return a;
    if(a == Linear::OutOfScope) return b;
    if(b == Linear::OutOfScope) return a;
    return Linear::Conflict;
}

/*
 * Everything the verifier indexes by value, block or the two together.
 *
 * Kept across functions and cleared per body, which is the containers guidance's rule and the same
 * one `ssa::Scratch` follows in the conversion: a verifier that runs after every ownership-sensitive
 * pass in an assertion build allocates these tables once per pass per function, and the peak size is
 * the largest body in the program rather than the sum of them.
 */
struct Scratch {
    Dominators dominators;

    // Value ID to the block its definition is in, or `kNone` where it has none.
    Array<U32> definedIn;

    /*
     * The owned values, densely renumbered.
     *
     * The consumption dataflow's state is per block and per owned value, so it is quadratic in
     * whatever it is indexed by - and almost every value in a body is `Trivial`. Renumbering the
     * owned ones keeps the product to the part of it that carries an obligation.
     */
    Array<U32> ownedIndex;
    Array<U32> ownedValue;

    // `blocks * owned` entries each. Indexed as `block * owned + value`.
    Array<Linear> in;
    Array<Linear> out;

    // Use-list bookkeeping: how many times each value is named by an operand, against how many times
    // its use list names the instruction back.
    Array<U32> operandUses;

    Array<U32> listed;
    Array<Linear> state;
};

struct Verifier {
    Context& context;
    mir::Program& program;
    mir::Function& function;
    Body& body;
    SsaBase base;
    Scratch& scratch;

    U32 errors = 0;

    Dominators& dominators = scratch.dominators;
    Array<U32>& definedIn = scratch.definedIn;
    Array<U32>& ownedIndex = scratch.ownedIndex;
    Array<U32>& ownedValue = scratch.ownedValue;
    Array<Linear>& in = scratch.in;
    Array<Linear>& out = scratch.out;
    Array<U32>& operandUses = scratch.operandUses;

    template<class... T>
    void report(LocationId source, StringView text, T&&... arguments) {
        errors++;
        context.diagnostics.error(text, source, forward<T>(arguments)...);
    }

    U32 owned() const { return U32(ownedValue.size()); }

    /*
     * The structural half: a use names a value of this body, defined at a point that dominates it,
     * and the use list agrees with the operand in both directions.
     *
     * The second half is what a mutation test needs and what the previous walk did not do: it
     * checked that a forward use appeared *somewhere* in the use list, so an entry naming an
     * instruction that no longer uses the value, or a use list that named an instruction once for
     * two operands, both read as correct.
     */
    void checkUse(Inst& inst, Use used, U32 blockIndex) {
        if(!used.isSet()) {
            report(inst.source, "internal: %@ has an operand naming no value"_v,
                   instMnemonic(inst.kind));
            return;
        }

        auto value = base[used.value()];
        if(value->id.index >= body.values.size() ||
           body.values.get(base, value->id.index) != used.value()) {
            report(inst.source, "internal: %@ names a value that is not this body's"_v,
                   instMnemonic(inst.kind));
            return;
        }

        operandUses[value->id.index]++;

        auto definer = definedIn[value->id.index];
        if(definer == Dominators::kNone) {
            report(inst.source, "internal: %@ names %@, which is defined nowhere"_v,
                   instMnemonic(inst.kind), value->id.index);
        } else if(!dominators.dominates(definer, blockIndex)) {
            /*
             * The rule the previous walk approximated with block index order.
             *
             * An operand on a terminator's *edge* is evaluated in the predecessor, which is this
             * block - so the same rule applies to it and the check needs no case of its own. What
             * it does need is dominance rather than order: an arm of a branch does not dominate its
             * sibling however the two are numbered.
             */
            report(inst.source, "internal: %@ names %@, whose definition does not dominate it"_v,
                   instMnemonic(inst.kind), value->id.index);
        }

        switch(used.kind()) {
            case UseKind::Consume:
                if(!mayConsume(inst.kind)) {
                    report(inst.source, "internal: %@ consumes an operand and is not declared to"_v,
                           instMnemonic(inst.kind));
                }

                if(value->convention == Convention::Guaranteed) {
                    report(inst.source, "internal: %@ consumes a guaranteed value"_v,
                           instMnemonic(inst.kind));
                }

                if(value->convention == Convention::Address) {
                    report(inst.source, "internal: %@ consumes an address"_v, instMnemonic(inst.kind));
                }
                break;
            case UseKind::Borrow:
            case UseKind::Inspect:
                if(value->convention == Convention::Owned && used.kind() == UseKind::Inspect) {
                    report(inst.source, "internal: %@ inspects an owned value"_v,
                           instMnemonic(inst.kind));
                }
                break;
        }
    }

    void checkEdge(Inst& inst, Edge& edge, U32 blockIndex) {
        if(!edge.target) {
            report(inst.source, "internal: %@ has an edge with no target"_v, instMnemonic(inst.kind));
            return;
        }

        auto target = base[edge.target];
        if(edge.arguments.size() != target->arguments.size()) {
            report(inst.source,
                   "internal: %@ supplies %@ arguments to block %@, which declares %@"_v,
                   instMnemonic(inst.kind), edge.arguments.size(), target->index.index,
                   target->arguments.size());
            return;
        }

        for(U32 i = 0; i < edge.arguments.size(); i++) {
            auto used = edge.arguments.get(base, i);
            checkUse(inst, used, blockIndex);
            if(!used.isSet()) continue;

            auto supplied = base[used.value()];
            auto declared = base[target->arguments.get(base, i)];

            // Both halves of the occurrence, not the type alone: a merge that declares an owner and
            // an arm that hands over a borrow agree on the type and on nothing that matters.
            if(supplied->occurrence != declared->occurrence) {
                report(inst.source,
                       "internal: %@ hands argument %@ to block %@ as an occurrence it does not declare"_v,
                       instMnemonic(inst.kind), i, target->index.index);
            }

            if(supplied->convention != declared->convention) {
                report(inst.source,
                       "internal: %@ hands argument %@ to block %@ as %@, which declares %@"_v,
                       instMnemonic(inst.kind), i, target->index.index,
                       conventionName(supplied->convention), conventionName(declared->convention));
            }
        }
    }

    /*
     * A call against the signature it names.
     *
     * Checked again here, and not because MIR's check was in doubt: this tier is re-verified after
     * every ownership-sensitive pass, so what this catches is an *optimizer* that dropped an
     * argument, reordered two, or turned a consuming use into a borrowing one. Those are exactly the
     * rewrites a pass is entitled to make elsewhere and never at a call boundary, and none of them
     * has a symptom before the machine.
     *
     * The use kind and not merely the count, because "borrowed where the callee takes ownership" is
     * a leak and "consumed where the callee borrows" is a double free, and both read as a correct
     * argument list.
     */
    void checkCall(Inst& inst) {
        auto& written = (InstCall&)inst;
        auto decl = *program.decl;
        auto callee = decl[written.callee];

        if(!callee) {
            report(inst.source, "internal: call names no callee"_v);
            return;
        }

        if(written.arguments.size() != callee->args.size()) {
            report(inst.source, "internal: call to %@ supplies %@ arguments, and it declares %@"_v,
                   context.findName(callee->name), written.arguments.size(), callee->args.size());
            return;
        }

        for(U32 i = 0; i < written.arguments.size(); i++) {
            auto used = written.arguments.get(base, i);
            if(!used.isSet()) continue;

            auto supplied = base[used.value()];
            auto& parameter = *decl[callee->args.get(decl, i)];

            if(!mir::sameType(supplied->type(), parameter.occurrence.type)) {
                report(inst.source,
                       "internal: call to %@ supplies argument %@ of type %@, and it declares %@"_v,
                       context.findName(callee->name), i,
                       mir::typeText(decl, supplied->type()),
                       mir::typeText(decl, parameter.occurrence.type));
                continue;
            }

            // `argumentMode` and `useKindFor` are the same pair the conversion read, asked in the
            // same order - so this compares against the rule rather than against a copy of it.
            auto mode = mir::argumentMode(parameter.occurrence.contract, mir::SourceKind::Result);
            auto wanted = mode == mir::UseMode::Take ? UseKind::Consume : UseKind::Borrow;

            if(used.kind() != wanted) {
                report(inst.source,
                       "internal: call to %@ hands argument %@ over as %@, and it declares %@"_v,
                       context.findName(callee->name), i, useKindName(used.kind()),
                       useKindName(wanted));
            }
        }
    }

    void checkStructure(Block& block) {
        auto count = block.instructions.size();
        if(!count) {
            report(function.source, "internal: block %@ is empty"_v, block.index.index);
            return;
        }

        for(Size i = 0; i < count; i++) {
            auto inst = base[block.instructions.get(base, i)];
            auto last = i + 1 == count;

            if(isTerminator(inst->kind) != last) {
                report(inst->source, last
                    ? "internal: block %@ does not end in a terminator"_v
                    : "internal: block %@ has a terminator in the middle"_v, block.index.index);
            }

            forEachUse(base, *inst, [&](Use used) { checkUse(*inst, used, block.index.index); });
            forEachEdge(*inst, [&](Edge& edge) { checkEdge(*inst, edge, block.index.index); });

            if(inst->kind == Inst::Call) checkCall(*inst);
        }
    }

    /*
     * The use lists, in both directions and with multiplicity.
     *
     * A use list is the reverse of the operand relation, and the two are maintained by one API for
     * exactly this reason - so a disagreement is an editor bug and is caught here. Counting rather
     * than checking membership is what makes it a *check*: an instruction that names a value twice
     * must appear twice, and an entry left behind by a rewrite that dropped an operand has nothing
     * pointing back at it.
     */
    void checkUseLists() {
        auto& listed = scratch.listed;
        listed.clear();
        for(U32 i = 0; i < body.values.size(); i++) listed.push(0);

        for(U32 i = 0; i < body.values.size(); i++) {
            auto value = body.value(base, ValueId(i));

            for(auto userPointer: value->uses.contents(base)) {
                auto user = base[userPointer];
                U32 names = 0;

                forEachUse(base, *user, [&](Use used) {
                    if(used.isSet() && used.value() == value - base) names++;
                });

                forEachEdge(*user, [&](Edge& edge) {
                    for(auto used: edge.arguments.contents(base)) {
                        if(used.isSet() && used.value() == value - base) names++;
                    }
                });

                if(!names) {
                    report(user->source, "internal: %@ is listed as using %@ and names it nowhere"_v,
                           instMnemonic(user->kind), i);
                }

                listed[i]++;
            }

            if(listed[i] != operandUses[i]) {
                auto at = value->definer ? base[value->definer]->source : function.source;
                report(at, "internal: value %@ is named by %@ operands and lists %@ uses"_v,
                       i, operandUses[i], listed[i]);
            }
        }
    }

    /*
     * The linear rule, as the dataflow it actually is - Analysis-IR.md §7.1.
     *
     * "Consumed exactly once on every reachable path" is a statement about paths, and a whole-body
     * count is **not** a weaker version of it - it is a different statement that is wrong in both
     * directions. Consuming an owned value once in each arm of an `if` is correct and counts two;
     * consuming it in one arm only is a leak on the other path and counts one. So the count rejected
     * the first program and accepted the second, and no amount of ordering it before the real check
     * would have made it a safe interim answer.
     *
     * This is a forward dataflow to a fixpoint over reverse postorder, with the three-state lattice
     * `Linear` describes. A value enters `Unconsumed` at the block its definition dominates from,
     * becomes `Consumed` at a consuming use, and is `Conflict` where two paths disagree. A path that
     * leaves the function with a value still `Unconsumed` is a leak, and a use of one that is already
     * `Consumed` is a use after the transfer.
     */
    void checkLinear() {
        auto blocks = body.blocks.size();
        auto values = owned();
        if(!values) return;

        for(U32 i = 0; i < blocks * values; i++) { in.push(Linear::OutOfScope); out.push(Linear::OutOfScope); }

        auto changed = true;
        while(changed) {
            changed = false;

            for(auto index: dominators.order) {
                auto block = body.block(base, BlockId(index));

                // The entry state is the meet of the predecessors', with unreachable ones absent.
                for(U32 v = 0; v < values; v++) {
                    auto state = Linear::OutOfScope;
                    auto first = true;

                    for(auto predecessorPointer: block->incoming.contents(base)) {
                        auto predecessor = base[predecessorPointer]->index.index;
                        if(!dominators.isReachable(predecessor)) continue;

                        auto from = out[predecessor * values + v];
                        state = first ? from : merge(state, from);
                        first = false;
                    }

                    if(in[index * values + v] != state) {
                        in[index * values + v] = state;
                        changed = true;
                    }
                }

                auto& state = scratch.state;
                state.clear();
                for(U32 v = 0; v < values; v++) state.push(in[index * values + v]);

                transfer(*block, state, false);

                for(U32 v = 0; v < values; v++) {
                    if(out[index * values + v] != state[v]) {
                        out[index * values + v] = state[v];
                        changed = true;
                    }
                }
            }
        }

        // The fixpoint is settled; walk once more to report, so that a diagnostic is made once
        // rather than once per iteration.
        for(auto index: dominators.order) {
            auto block = body.block(base, BlockId(index));

            auto& state = scratch.state;
            state.clear();
            for(U32 v = 0; v < owned(); v++) state.push(in[index * owned() + v]);

            for(U32 v = 0; v < owned(); v++) {
                if(state[v] != Linear::Conflict) continue;

                report(function.source,
                       "internal: owned value %@ is consumed on some paths into block %@ and not others"_v,
                       ownedValue[v], index);

                // Reported once. Continuing as consumed keeps the rest of the walk from repeating
                // the same disagreement at every block downstream of it.
                state[v] = Linear::Consumed;
            }

            transfer(*block, state, true);
        }
    }

    /*
     * One block's effect on the consumption state.
     *
     * Run twice: once per fixpoint iteration with `reporting` clear, and once at the end with it
     * set. The alternative - reporting during the fixpoint - names the same value once per
     * iteration, and the iteration count is not something a diagnostic should depend on.
     */
    void transfer(Block& block, Array<Linear>& state, bool reporting) {
        auto values = owned();

        // A block's parameters are defined on entry, which is where their obligation starts.
        for(auto argumentPointer: block.arguments.contents(base)) {
            auto argument = base[argumentPointer];
            if(argument->convention != Convention::Owned) continue;

            state[ownedIndex[argument->id.index]] = Linear::Unconsumed;
        }

        for(auto instPointer: block.instructions.contents(base)) {
            auto inst = base[instPointer];

            auto visit = [&](Use used) {
                if(!used.isSet()) return;

                auto value = base[used.value()];
                if(value->convention != Convention::Owned) return;

                auto slot = ownedIndex[value->id.index];
                if(slot >= values) return;

                if(used.kind() == UseKind::Consume) {
                    if(state[slot] == Linear::Consumed && reporting) {
                        report(inst->source, "internal: %@ consumes %@, which was already consumed"_v,
                               instMnemonic(inst->kind), value->id.index);
                    }

                    state[slot] = Linear::Consumed;
                } else if(state[slot] == Linear::Consumed && reporting) {
                    report(inst->source, "internal: %@ uses %@ after it was consumed"_v,
                           instMnemonic(inst->kind), value->id.index);
                }
            };

            forEachUse(base, *inst, visit);
            forEachEdge(*inst, [&](Edge& edge) {
                for(auto used: edge.arguments.contents(base)) visit(used);
            });

            /*
             * A leak is checked where a path *leaves*, not at the end of the body.
             *
             * `Return` and `Unreachable` are the two exits. An owned value still unconsumed at one
             * of them was never handed anywhere and has no drop, which is precisely the case the
             * whole-body count could not see: the count is satisfied by one arm consuming it.
             */
            if(reporting && (inst->kind == Inst::Return || inst->kind == Inst::Unreachable)) {
                for(U32 v = 0; v < values; v++) {
                    if(state[v] != Linear::Unconsumed) continue;

                    report(inst->source, "internal: owned value %@ reaches %@ unconsumed"_v,
                           ownedValue[v], instMnemonic(inst->kind));

                    state[v] = Linear::Consumed;
                }
            }

            // The results are defined here, and their obligation starts after the operands above
            // were read - which is what makes an instruction that consumes an operand and produces
            // an owner two separate facts about one point.
            for(auto resultPointer: inst->results.contents(base)) {
                auto result = base[resultPointer];
                if(result->convention != Convention::Owned) continue;

                state[ownedIndex[result->id.index]] = Linear::Unconsumed;
            }
        }
    }

    void prepare() {
        auto count = body.values.size();

        definedIn.clear();
        ownedIndex.clear();
        ownedValue.clear();
        operandUses.clear();
        in.clear();
        out.clear();

        for(U32 i = 0; i < count; i++) {
            definedIn.push(Dominators::kNone);
            ownedIndex.push(~U32(0));
            operandUses.push(0);
        }

        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(base, BlockId(i));

            for(auto argumentPointer: block->arguments.contents(base)) {
                definedIn[base[argumentPointer]->id.index] = i;
            }

            for(auto instPointer: block->instructions.contents(base)) {
                for(auto resultPointer: base[instPointer]->results.contents(base)) {
                    definedIn[base[resultPointer]->id.index] = i;
                }
            }
        }

        for(U32 i = 0; i < count; i++) {
            auto value = body.value(base, ValueId(i));
            if(value->convention != Convention::Owned) continue;

            ownedIndex[i] = U32(ownedValue.size());
            ownedValue.push(i);
        }
    }

    /*
     * The lifetime half of `Guaranteed` - §7.1, and the reason `Value::loan` exists.
     *
     * A borrowed value that names no loan has no term for "may not outlive the loan it depends on"
     * to be checked against, so the rule is unenforceable for it. Nothing produces a loan at this
     * milestone, so this is stated and not yet required: the check is written as the *shape* of what
     * it becomes, and the first pass that mints a loan is what turns the report on.
     */
    void checkLoans() {
        for(U32 i = 0; i < body.values.size(); i++) {
            auto value = body.value(base, ValueId(i));

            if(value->convention != Convention::Guaranteed && value->loan.valid()) {
                auto at = value->definer ? base[value->definer]->source : function.source;
                report(at, "internal: value %@ names a loan and is not guaranteed"_v, i);
            }
        }
    }

    bool run() {
        prepare();
        dominators.build(base, body);

        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(base, BlockId(i));

            // An unreachable block is still checked structurally - it is IR someone built - and is
            // left out of the dataflow, which is about paths a program can take.
            checkStructure(*block);
        }

        checkUseLists();
        checkLinear();
        checkLoans();

        auto entry = base[body.entry];
        if(entry->incoming.size()) {
            report(function.source, "internal: the entry block of %@ has predecessors"_v,
                   context.findName(function.name));
        }

        return errors == 0;
    }
};

} // namespace

static bool verifyOne(Context& context, mir::Program& program, SsaBase base,
                     mir::Function& function, Scratch& scratch) {
    if(!function.ssa) return true;

    Verifier verifier { context, program, function, *base[function.ssa], base, scratch };
    return verifier.run();
}

bool verifyBody(Context& context, mir::Program& program, SsaBase base, mir::Function& function) {
    Scratch scratch;
    return verifyOne(context, program, base, function, scratch);
}

bool verifyProgram(Context& context, mir::Program& program, Converted& converted) {
    auto decl = *program.decl;
    auto base = *converted.arena;
    auto valid = true;

    // One set of tables for the whole program, reset per body - see `Scratch`.
    Scratch scratch;

    for(auto modulePointer: program.modules) {
        auto module = decl[modulePointer];
        for(auto functionPointer: module->functions.contents(decl)) {
            valid &= verifyOne(context, program, base, *decl[functionPointer], scratch);
        }
    }

    return valid;
}

} // namespace ssa
