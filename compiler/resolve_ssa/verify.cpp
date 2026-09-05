#include "verify.h"
#include "lower/manifest.h"

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
// Every block one block's terminator can reach. A callback rather than a list, because the two
// walks that ask for it ask once per block per visit and a list would be an allocation each time.
template<class F>
static void forEachSuccessor(SsaBase base, Block& block, F&& f) {
    auto count = block.instructions.size();
    if(!count) return;

    auto last = base[block.instructions.get(base, count - 1)];
    if(!isTerminator(last->kind)) return;

    forEachEdge(*last, [&](Edge& edge) {
        if(edge.target) f(base[edge.target]->index.index);
    });
}

struct Dominators {
    // Reverse postorder, and the position of each block in it. Unreachable blocks are absent from
    // the order and have no position, which is how the walk skips them without a second flag.
    Array<U32> order;
    Array<U32> position;
    Array<U32> idom;

    /*
     * The dominator tree, as an interval per block - what makes `dominates` a comparison.
     *
     * `a` dominates `b` exactly when `b` is in `a`'s subtree of the dominator tree, so a preorder
     * walk stamping each block with the range its subtree spans turns the question into two integer
     * comparisons. The walk this replaces climbed `b`'s idom chain to the root, and it is asked once
     * per operand of the body plus once per pair the linear rule compares - which is the
     * `uses x blocks` term in the bound over this file.
     *
     * The children are one CSR table rather than a list per block, which is the containers guidance
     * answer for a table indexed by block: two arrays reset per body instead of an allocation per
     * block per function.
     */
    Array<U32> childStart;
    Array<U32> childList;
    Array<U32> enter;
    Array<U32> leave;

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
        SmallArray<U32, 64> stack, next, postorder;
        IndexSet seen;

        seen.reset(count);
        for(U32 i = 0; i < count; i++) next.push(0);

        auto entry = base[body.entry]->index.index;
        stack.push(entry);
        seen.set(entry, true);

        while(stack.size()) {
            auto index = stack[stack.size() - 1];
            auto block = body.block(base, BlockId(index));

            SmallArray<U32, 4> successors;
            forEachSuccessor(base, *block, [&](U32 target) { successors.push(target); });

            if(next[index] < successors.size()) {
                auto successor = successors[next[index]++];
                if(seen.add(successor)) stack.push(successor);
                continue;
            }

            postorder.push(index);
            stack.pop();
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

        numberTree(count, entry);
    }

    bool isReachable(U32 index) const { return position[index] != kNone; }

    // Two comparisons: `b` is dominated by `a` exactly where it sits inside `a`'s subtree.
    bool dominates(U32 a, U32 b) const {
        if(!isReachable(a) || !isReachable(b)) return false;
        return enter[a] <= enter[b] && leave[b] <= leave[a];
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

    // The children of each block in the dominator tree, then a preorder walk of it stamping the
    // interval each subtree spans.
    void numberTree(U32 count, U32 entry) {
        childStart.clear();
        childList.clear();
        enter.clear();
        leave.clear();

        for(U32 i = 0; i <= count; i++) childStart.push(0);
        for(U32 i = 0; i < count; i++) { enter.push(0); leave.push(0); }

        for(auto index: order) {
            if(index == entry) continue;
            childStart[idom[index] + 1]++;
        }

        for(U32 i = 0; i < count; i++) childStart[i + 1] += childStart[i];
        for(U32 i = 0; i < childStart[count]; i++) childList.push(0);

        SmallArray<U32, 64> filled;
        for(U32 i = 0; i < count; i++) filled.push(0);

        for(auto index: order) {
            if(index == entry) continue;

            auto parent = idom[index];
            childList[childStart[parent] + filled[parent]++] = index;
        }

        // The same explicit stack, for the same reason. A block is stamped on the way down and
        // closed on the way up, so the interval it spans is exactly its subtree.
        SmallArray<U32, 64> stack, cursor;
        for(U32 i = 0; i < count; i++) cursor.push(0);

        U32 clock = 0;
        stack.push(entry);
        enter[entry] = clock++;

        while(stack.size()) {
            auto index = stack[stack.size() - 1];
            auto first = childStart[index];
            auto last = childStart[index + 1];

            if(first + cursor[index] < last) {
                auto child = childList[first + cursor[index]++];
                stack.push(child);
                enter[child] = clock++;
                continue;
            }

            leave[index] = clock++;
            stack.pop();
        }
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
     * Value ID to *where* in that block it is defined - the position dominance test's other half.
     *
     * Block arguments are 0 and the instruction at index `i` is `i + 1`, so "defined at entry"
     * needs no case of its own and every real definition compares strictly against a use. Without
     * this the check was block dominance alone, and a block dominates itself: a use above its own
     * definition inside one block passed, which is precisely the shape an optimizer that sinks an
     * instruction one step too far produces.
     */
    Array<U32> definedAt;

    /*
     * The tracked values - `Owned` and `Affine` - densely renumbered.
     *
     * The consumption dataflow's state is per block and per tracked value, so it is quadratic in
     * whatever it is indexed by - and almost every value in a body is `Trivial`. Renumbering the
     * tracked ones keeps the product to the part of it that carries an obligation.
     *
     * `trackedExact` is which of the two each renumbered slot is: the dataflow is one walk for both,
     * and the convention is read back only at the two comparisons where "exactly once" and "at most
     * once" differ. A byte per tracked value rather than a lookup through `trackedValue` into the
     * body at each of them, because one of the two is asked per block per value.
     */
    Array<U32> trackedIndex;
    Array<U32> trackedValue;
    Array<U8> trackedExact;

    // `blocks * tracked` entries each. Indexed as `block * tracked + value`.
    Array<Linear> in;
    Array<Linear> out;

    /*
     * The loan endpoint dataflow - one bit per (block, loan), clear for live and set for ended.
     *
     * A separate lattice from `Linear` and not a fourth state on it, because the two disagree about
     * what a disagreement means. Two paths that disagree about whether an owned value was consumed
     * is a bug in the program and is reported as one; two paths that disagree about whether a loan
     * has ended is a *use* of that borrow being invalid on one of them, so the join is "ended" and
     * the report happens where the borrow is used rather than where the paths meet.
     */
    BitMatrix loanIn;
    BitMatrix loanOut;
    Array<U64> loanState;

    // Use-list bookkeeping: how many times each value is named by an operand, against how many times
    // its use list names the instruction back.
    Array<U32> operandUses;

    /*
     * The operand relation forwards, as a CSR table over values - which instructions name each one.
     *
     * Built so that the use lists can be checked against it by a stamp per instruction rather than
     * by re-reading every operand of a user once per entry that names it. That re-read is bounded by
     * the user's arity, so an instruction naming one value many times cost the square of that.
     */
    Array<U32> userStart;
    Array<U32> userList;
    Array<U32> userCursor;
    Array<U32> stamp;

    /*
     * How many operands of the stamped instruction name the stamped value - the multiplicity the
     * stamp alone does not carry.
     *
     * The stamp says *whether* an instruction is in the row and the row total says how many entries
     * there are altogether, and a check made of those two is not a check of the relation: `A` and
     * `B` each naming a value once, against a use list holding `A` twice, satisfies both - `A` is
     * stamped, `B` is never asked about, and the total is two either way. So each entry spends one
     * unit of the pair's forward count, and an entry with none left is the duplicate. `B`'s absence
     * then falls out of the row total, which is now the only thing it can be.
     */
    Array<U32> pairCount;

    // The reverse of `Loan::storage`: which loans each value is the lender of. A scan over every
    // loan's storage list is what this replaces, and it happened at every consuming use.
    Array<U32> lenderStart;
    Array<U32> lenderList;

    Array<U32> listed;
    Array<Linear> state;

    // Which blocks the linear fixpoint still has to visit, and which it has visited at all.
    IndexSet pending;
    IndexSet walked;
};

struct Verifier {
    Context& context;
    mir::Program& program;
    mir::Function& function;
    Body& body;
    SsaBase base;
    Scratch& scratch;

    // The declaration region, which every type question goes through. A member rather than a local
    // in the one function that used to need it: three checks need it now, and three copies of one
    // `*program.decl` is three places to get the region wrong.
    DeclBase decl = *program.decl;

    // The same region paired with the machine, for the checks that ask a *representation* question -
    // see `Layout`. Built from the settings here rather than handed in, because this tier runs
    // before the lowering does and there is nothing to hand it one.
    // Through `layoutFor`, which is the one place a layout is filled: the manifestation reads the
    // program's view table, and a layout built by hand here was one with nothing to read it from.
    Layout layout = layoutFor(context, program);

    U32 errors = 0;

    Dominators& dominators = scratch.dominators;
    Array<U32>& definedIn = scratch.definedIn;
    Array<U32>& definedAt = scratch.definedAt;
    Array<U32>& trackedIndex = scratch.trackedIndex;
    Array<U32>& trackedValue = scratch.trackedValue;
    Array<U8>& trackedExact = scratch.trackedExact;
    Array<Linear>& in = scratch.in;
    Array<Linear>& out = scratch.out;
    BitMatrix& loanIn = scratch.loanIn;
    BitMatrix& loanOut = scratch.loanOut;
    Array<U32>& operandUses = scratch.operandUses;

    template<class... T>
    void report(LocationId source, StringView text, T&&... arguments) {
        errors++;
        context.diagnostics.error(text, source, forward<T>(arguments)...);
    }

    U32 tracked() const { return U32(trackedValue.size()); }
    U32 loans() const { return U32(body.loans.size()); }
    U32 loanWords() const { return (loans() + 63) / 64; }

    /*
     * The loan a value depends on, or null - `Value::loan`, with the handle checked.
     *
     * Every reader goes through this rather than dereferencing the field, because a stale handle is
     * exactly what a rewrite produces and a verifier that trusted it would fault instead of
     * reporting.
     */
    Loan* loanOf(Value& value) {
        if(!value.loan) return nullptr;

        auto loan = base[value.loan];
        if(loan->id.index >= body.loans.size()) return nullptr;
        if(body.loans.get(base, loan->id.index) != value.loan) return nullptr;

        return loan;
    }

    /*
     * Whether one value's definition point dominates another's - the same two-part test `checkUse`
     * makes, asked about a pair of definitions rather than about a use.
     *
     * A value defined nowhere dominates nothing, which is the answer that makes the caller report
     * rather than the answer that makes it silent.
     */
    bool definitionDominates(U32 earlier, U32 later) {
        auto from = definedIn[earlier];
        auto to = definedIn[later];
        if(from == Dominators::kNone || to == Dominators::kNone) return false;
        if(!dominators.dominates(from, to)) return false;

        return from != to || definedAt[earlier] < definedAt[later];
    }

    /*
     * Every value's dependency, against the loan table - §7.1's lifetime half.
     *
     * Two statements, and the pair is the point. A `Guaranteed` value names a loan and no other
     * convention does, so "is this borrowed" has one answer per value rather than being re-derived
     * from the occurrence at each consumer. And a loan's storage is defined before the value that
     * depends on it, which is what a rewrite breaks: an operand can be redirected without the
     * recorded dependency following it, and the whole reason the dependency is *recorded* rather
     * than read back off the operands is that after such a rewrite the operands no longer say what
     * the value actually depends on.
     */
    void checkLoans() {
        for(U32 i = 0; i < body.values.size(); i++) {
            auto value = body.value(base, ValueId(i));
            auto borrowed = value->convention == Convention::Guaranteed;

            if(!borrowed) {
                if(value->loan) {
                    report(function.source,
                           "internal: value %@ is %@ and names a loan"_v, i,
                           conventionName(value->convention));
                }

                continue;
            }

            auto loan = loanOf(*value);
            if(!loan) {
                report(function.source, "internal: value %@ is borrowed and names no loan"_v, i);
                continue;
            }

            // The caller's loan - see `Loan`. Its lender is outside this body, so there is no
            // definition here for it to be dominated by and nothing in this body can end it.
            for(auto held: loan->storage.contents(base)) {
                auto storage = base[held];
                if(storage->id.index >= body.values.size() ||
                   body.values.get(base, storage->id.index) != held) {
                    report(function.source,
                           "internal: the loan on value %@ names storage that is not this body's"_v,
                           i);
                    continue;
                }

                if(!definitionDominates(storage->id.index, i)) {
                    report(function.source,
                           "internal: value %@ borrows %@, whose definition does not dominate it"_v,
                           i, storage->id.index);
                }
            }
        }
    }

    /*
     * The structural half: a use names a value of this body, defined at a point that dominates it,
     * and the use list agrees with the operand in both directions.
     *
     * The second half is what a mutation test needs and what the previous walk did not do: it
     * checked that a forward use appeared *somewhere* in the use list, so an entry naming an
     * instruction that no longer uses the value, or a use list that named an instruction once for
     * two operands, both read as correct.
     */
    void checkUse(Inst& inst, Use used, U32 blockIndex, U32 position) {
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
        } else if(definer == blockIndex && definedAt[value->id.index] >= position) {
            /*
             * Dominance *inside* the defining block, which block dominance cannot state: every
             * block dominates itself, so the test above says nothing about two points within one.
             *
             * `>=` and not `>`, so that an instruction naming its own result is this error rather
             * than an accepted cycle. A block argument is at position 0 and every use is at 1 or
             * more, so "defined at entry" falls out of the numbering instead of needing a case.
             */
            report(inst.source, "internal: %@ names %@, which is defined below it in block %@"_v,
                   instMnemonic(inst.kind), value->id.index, blockIndex);
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
                // A tracked value's uses are exactly what the linear dataflow counts, so an
                // inspection of one - a use with no obligation attached - is not representable.
                if(isTracked(value->convention) && used.kind() == UseKind::Inspect) {
                    report(inst.source, "internal: %@ inspects %@, which is %@"_v,
                           instMnemonic(inst.kind), value->id.index,
                           conventionName(value->convention));
                }
                break;
        }
    }

    void checkEdge(Inst& inst, Edge& edge, U32 blockIndex, U32 position) {
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
            checkUse(inst, used, blockIndex, position);
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
     * A call through a function value - `CallThrough::Value`.
     *
     * The arity and each argument's handover, read out of the target's type. Types are *not*
     * compared here for the reason the erased positions below are exempted: what a caller supplies
     * and what a signature declares are related by a substitution this tier does not have where
     * either side is open, and the shared fact at every boundary is the convention.
     */
    void checkValueCall(Inst& inst) {
        auto& written = (InstCall&)inst;

        if(!written.target.isSet()) {
            report(inst.source, "internal: a call through a value names no value"_v);
            return;
        }

        auto target = base[written.target.value()];
        if(!target->type() || decl[target->type()]->kind != mir::Type::Fun) {
            report(inst.source, "internal: a call through a value of type %@"_v,
                   mir::typeText(context, decl, target->type()));
            return;
        }

        auto& shape = *(mir::FunType*)decl[target->type()];
        if(written.arguments.size() != shape.params.size()) {
            report(inst.source, "internal: a call through %@ supplies %@ arguments"_v,
                   mir::typeText(context, decl, target->type()), written.arguments.size());
            return;
        }

        for(U32 i = 0; i < written.arguments.size(); i++) {
            auto used = written.arguments.get(base, i);
            if(!used.isSet()) continue;

            auto declared = shape.params.get(decl, i).occurrence;
            auto supplied = base[used.value()];

            if(arrivesAsAddress(layout, declared) != arrivesAsAddress(layout, supplied->occurrence)) {
                report(inst.source,
                       "internal: a call through %@ supplies argument %@ under a different handover than its position"_v,
                       mir::typeText(context, decl, target->type()), i);
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

        // A call through a function value is checked against the value's *type*, which is the whole
        // signature there is; the MIR verifier states the same rule over the node it came from.
        if(written.through == mir::CallThrough::Value) return checkValueCall(inst);

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

        /*
         * A target loaded out of an environment, and the two things that makes checkable.
         *
         * Which requirement it names has to be one the enclosing signature declared - a slot number
         * out of range is a caller and a callee indexing different numberings, which is the one
         * failure the split of `GenContext` from `GenSchema` exists to prevent and the one this tier
         * is in a position to notice. And it may only appear in a body that *has* an environment,
         * because a slot means nothing without one.
         */
        if(written.through != mir::CallThrough::Declaration) {
            auto owner = function.gen ? decl[function.gen] : nullptr;
            auto count = !owner ? 0u
                : written.through == mir::CallThrough::ClassSlot ? owner->classes.size()
                                                                 : owner->callables.size();

            if(function.form != mir::GenForm::Erased || written.requirement >= count) {
                report(inst.source,
                       "internal: a call through a %@ slot names requirement %@ of %@"_v,
                       mir::callThroughName(written.through), written.requirement,
                       context.findName(function.name));
                return;
            }
        }

        for(U32 i = 0; i < written.arguments.size(); i++) {
            auto used = written.arguments.get(base, i);
            if(!used.isSet()) continue;

            auto supplied = base[used.value()];
            auto& parameter = *decl[callee->args.get(decl, i)];

            /*
             * A position the declaration **erased** is checked as a convention rather than as a
             * type, and it has to be: what the callee declared is a type parameter, what the caller
             * supplied is whatever that was bound to, and there is no substitution here to relate
             * them - a body compiled once has no argument list.
             *
             * What is checked is that the lowering can *place* what the caller is holding, because
             * placing it is what the boundary does: an erased position takes a pointer, so a caller
             * with the value in a register spills it to a frame slot and passes the address. That
             * adaptation is a representation question and lives in the lowering beside the thunk
             * that undoes it; what this tier is in a position to say is that the value has a
             * representation to spill - a concrete manifestation with a size - since the one thing
             * with no answer is a value that is neither an address nor of a type anything here
             * knows the width of.
             */
            if(!mir::isConcreteType(decl, parameter.occurrence.type) ||
               !mir::isConcreteType(decl, supplied->type())) {
                if(arrivesAsAddress(layout, parameter.occurrence) &&
                   !arrivesAsAddress(layout, supplied->occurrence) &&
                   manifestNative(layout, supplied->occurrence).dynamic) {
                    report(inst.source,
                           "internal: call to %@ supplies argument %@ as a value of unknown width, and the position it fills is erased"_v,
                           context.findName(callee->name), i);
                }

                continue;
            }

            if(!mir::sameType(supplied->type(), parameter.occurrence.type)) {
                report(inst.source,
                       "internal: call to %@ supplies argument %@ of type %@, and it declares %@"_v,
                       context.findName(callee->name), i,
                       mir::typeText(context, decl, supplied->type()),
                       mir::typeText(context, decl, parameter.occurrence.type));
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

    /*
     * An aggregate against the record it builds, and a field address against the record it projects.
     *
     * The same statements MIR's verifier makes about the node these came from, restated where the
     * conversion's own answer is what is being checked: the two verifiers ask about two different
     * things going wrong, and a component list that survived one and not the other is exactly the
     * conversion bug the split exists to attribute.
     */
    void checkAggregate(Inst& inst) {
        auto& written = (InstAggregate&)inst;
        if(!inst.results.size()) {
            report(inst.source, "internal: aggregate produces no value"_v);
            return;
        }

        auto type = base[inst.results.get(base, 0)]->type();
        auto kind = type ? decl[type]->kind : mir::Type::KindCount;

        if(kind != mir::Type::Record && kind != mir::Type::Array) {
            report(inst.source,
                   "internal: aggregate produces %@, which is neither a record nor a fixed array"_v,
                   mir::typeText(context, decl, type));
            return;
        }

        if(kind == mir::Type::Record &&
           written.constructor >= ((mir::RecordType*)decl[type])->cons.size()) {
            report(inst.source, "internal: aggregate builds constructor %@, and %@ declares %@"_v,
                   U64(written.constructor), mir::typeText(context, decl, type),
                   U64(((mir::RecordType*)decl[type])->cons.size()));
            return;
        }

        // `aggregateComponentCount` is the one place the record/array split is made - the MIR
        // verifier and the builder ask it too, so the three cannot disagree about how many
        // components a type has.
        auto expected = mir::aggregateComponentCount(decl, type, written.constructor);
        if(written.components.size() != expected) {
            report(inst.source, "internal: aggregate supplies %@ components, and %@ holds %@"_v,
                   written.components.size(), mir::typeText(context, decl, type), U64(expected));
            return;
        }

        for(U32 i = 0; i < written.components.size(); i++) {
            auto used = written.components.get(base, i);
            if(!used.isSet()) continue;

            auto supplied = base[used.value()]->type();
            auto declared = mir::aggregateComponent(decl, type, written.constructor, i).type;

            if(!mir::sameType(supplied, declared)) {
                report(inst.source,
                       "internal: aggregate supplies component %@ of type %@, and the position is %@"_v,
                       i, mir::typeText(context, decl, supplied),
                       mir::typeText(context, decl, declared));
            }
        }
    }

    /*
     * `element.addr` - the container it projects, and the index it projects with.
     *
     * What there is to check is that the container has elements at all: the *index* is a value of
     * the word width and its bounds are a runtime question, which is what makes an out-of-range
     * subscript a trap rather than a verification failure.
     */
    void checkElementAddress(Inst& inst) {
        auto& written = (InstElementAddress&)inst;

        if(!mir::elementTypeOf(decl, program, written.container)) {
            report(inst.source,
                   "internal: element.addr projects %@, which has no elements"_v,
                   mir::typeText(context, decl, written.container));
        }
    }

    void checkFieldAddress(Inst& inst) {
        auto& written = (InstFieldAddress&)inst;

        /*
         * A projection through a **property slot**, which names a requirement rather than a record.
         *
         * What there is to check is that the requirement exists: the subject is a type parameter, so
         * there is no field list to bound an index against and the offset is not this body's answer
         * at all. That is the whole of the difference, and it is why the two are one instruction -
         * everything below the address is identical.
         */
        if(written.property != InstFieldAddress::kNoProperty) {
            auto owner = function.gen ? decl[function.gen] : nullptr;

            if(!owner || written.property >= owner->properties.size()) {
                report(inst.source, "internal: field.addr names field requirement %@ of %@"_v,
                       written.property, context.findName(function.name));
            }

            return;
        }

        if(!written.record || decl[written.record]->kind != mir::Type::Record) {
            report(inst.source, "internal: field.addr projects %@, which is not a record"_v,
                   mir::typeText(context, decl, written.record));
            return;
        }

        /*
         * The discriminant, which is a component the field list does not hold - and the check is
         * that the record *has* one: a tag projected out of a record with a single constructor is a
         * read of bytes that are somebody's field.
         */
        if(written.field == InstFieldAddress::kDiscriminant) {
            if(!mir::tagBitsOf(decl, *(mir::RecordType*)decl[written.record])) {
                report(inst.source, "internal: field.addr names the discriminant of %@, which has one constructor"_v,
                       mir::typeText(context, decl, written.record));
            }

            return;
        }

        if(written.field >= ((mir::RecordType*)decl[written.record])->fields.size()) {
            report(inst.source, "internal: field.addr names field %@ of %@"_v, written.field,
                   mir::typeText(context, decl, written.record));
        }
    }

    // A drop of something with nothing to run is a placement decision that should not have been
    // made, not an instruction with no effect - see `Editor::addDrop`, which refuses to build one.
    void checkDrop(Inst& inst) {
        auto& written = (InstDrop&)inst;
        if(mir::hasTeardownAt(decl, written.held)) return;

        report(inst.source, "internal: drop names %@, which has no teardown"_v,
               mir::typeText(context, decl, written.held.type));
    }

    /*
     * The continuation regions and the nesting rule over them - `Continuation`.
     *
     * Four statements: the table is a forest with one root, every block names a region that exists,
     * every edge either stays in a region, enters a *direct* child of it at its entry, or leaves it
     * to the destination it declared, and a region that runs at most once is not re-entered from
     * inside itself. The last two are the facts §10.1 named and this milestone added - see
     * `Continuation::destination` and `Continuation::multiplicity`.
     *
     * Checkable now because the conversion already builds one nested region - the arm of a
     * conditional teardown - so the rule is not vacuous while the kinds it will eventually carry are
     * still to come. An edge that jumped straight into a cleanup arm from somewhere other than its
     * entry point, or out of one to somewhere that is not enclosing, is what a pass rewriting the
     * graph produces and what nothing but this notices.
     */
    void checkRegions() {
        if(!body.regions.size()) {
            report(function.source, "internal: %@ has no continuation region"_v,
                   context.findName(function.name));
            return;
        }

        for(U32 i = 0; i < body.regions.size(); i++) {
            auto region = body.region(base, ContinuationId(i));

            if(region->id.index != i) {
                report(function.source, "internal: continuation region %@ is stored at index %@"_v,
                       region->id.index, i);
                continue;
            }

            // The root is region zero and has no parent; every other names one made before it,
            // which is what makes the parent walk terminate without a visited set.
            if(!i) {
                if(region->parent.valid()) {
                    report(function.source, "internal: the root continuation region has a parent"_v);
                }

                continue;
            }

            if(!region->parent.valid() || region->parent.index >= i) {
                report(function.source,
                       "internal: continuation region %@ names an enclosing region that is not"_v, i);
            }
        }

        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(base, BlockId(i));
            if(block->region.valid() && block->region.index < body.regions.size()) continue;

            report(function.source, "internal: block %@ names no continuation region"_v, i);
        }

        /*
         * A region that runs **at most once** is not re-entered from within itself.
         *
         * The one thing multiplicity says that the graph does not already: a cleanup arm behind a
         * flag test runs or does not, and an edge from inside it back to its entry makes it run as
         * many times as the loop does - which is a teardown running repeatedly, and which nothing
         * about the shape of a diamond would have objected to.
         *
         * From anywhere *inside*, descendants included: a nested region is part of what the outer
         * one runs, so an edge out of a child back to the outer entry re-enters it just the same.
         * The edge rule below does not cover this, because it is a legal exit to an ancestor.
         */
        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(base, BlockId(i));
            if(!block->region.valid()) continue;

            forEachEdgeTarget(*block, [&](Inst& inst, Block& target) {
                if(!target.region.valid()) return;

                auto region = body.region(base, target.region);
                if(!region || region->multiplicity != Continuation::AtMostOnce) return;
                if(region->entry != target.index) return;

                // `encloses` is reflexive, so this is "from the region itself or from anything
                // nested in it". An edge from the **parent** to the entry is the ordinary way in and
                // is the one thing that is not a re-entry.
                if(!body.encloses(base, target.region, block->region)) return;

                report(inst.source,
                       "internal: %@ re-enters continuation region %@, which runs at most once"_v,
                       instMnemonic(inst.kind), target.region.index);
            });
        }
    }

    // Every block a terminator names, with the terminator - the walk the two region rules above and
    // below share, so that "an edge of this block" means one thing here.
    template<class F>
    void forEachEdgeTarget(Block& block, F&& f) {
        auto count = block.instructions.size();
        if(!count) return;

        auto last = base[block.instructions.get(base, count - 1)];
        forEachEdge(*last, [&](Edge& edge) {
            if(edge.target) f(*last, *base[edge.target]);
        });
    }

    // One edge against the nesting rule - see `checkRegions`. Called from the structural walk,
    // where the terminators are.
    void checkEdgeRegion(Inst& inst, Block& from, Block& target) {
        if(!from.region.valid() || !target.region.valid()) return;
        if(from.region == target.region) return;

        auto region = body.region(base, target.region);

        if(region && region->parent == from.region) {
            /*
             * Entering a child, which is legal **at its entry and nowhere else**.
             *
             * The rule without this half accepts an edge from the parent into any block of the
             * region, which is the reroute a pass makes when it threads a branch past the flag test
             * a cleanup arm begins with. A region entered in the middle is a subgraph, and the whole
             * reason the region is recorded rather than read off the graph is that afterwards
             * nothing can tell the two apart.
             */
            if(!region->entry.valid() || region->entry == target.index) return;

            report(inst.source,
                   "internal: %@ enters continuation region %@ at block %@, which is not its entry %@"_v,
                   instMnemonic(inst.kind), target.region.index, target.index.index,
                   region->entry.index);
            return;
        }

        if(body.encloses(base, target.region, from.region)) {
            /*
             * Leaving, which is legal **at the destination the region declared**, where it declared
             * one.
             *
             * Optional on purpose, and the asymmetry with `entry` above is the point: a region is
             * entered at one place by definition and is not left at one by definition - §10.1's
             * lens continuation has four exits. So the check holds a region to what its builder
             * said rather than to single-exit as a property of regions, and a kind with several
             * exits leaves `destination` unset and keeps the enclosing rule alone.
             */
            auto leaving = body.region(base, from.region);
            if(!leaving || !leaving->destination.valid()) return;
            if(leaving->destination == target.index) return;

            report(inst.source,
                   "internal: %@ leaves continuation region %@ for block %@, which is not the destination %@ it rejoins at"_v,
                   instMnemonic(inst.kind), from.region.index, target.index.index,
                   leaving->destination.index);
            return;
        }

        report(inst.source,
               "internal: %@ leaves continuation region %@ for %@, which neither encloses it nor is entered from it"_v,
               instMnemonic(inst.kind), from.region.index, target.region.index);
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

            // The position an operand is read at, in `definedAt`'s numbering - see `Scratch`.
            auto position = U32(i) + 1;

            forEachUse(base, *inst, [&](Use used) {
                checkUse(*inst, used, block.index.index, position);
            });
            forEachEdge(*inst, [&](Edge& edge) {
                checkEdge(*inst, edge, block.index.index, position);
                if(edge.target) checkEdgeRegion(*inst, block, *base[edge.target]);
            });

            switch(inst->kind) {
                case Inst::Call:         checkCall(*inst); break;
                case Inst::Aggregate:    checkAggregate(*inst); break;
                case Inst::FieldAddress: checkFieldAddress(*inst); break;
                case Inst::ElementAddress: checkElementAddress(*inst); break;
                case Inst::Drop:         checkDrop(*inst); break;
                default: break;
            }
        }
    }

    /*
     * The use lists, in both directions and with multiplicity.
     *
     * A use list is the reverse of the operand relation, and the two are maintained by one API for
     * exactly this reason - so a disagreement is an editor bug and is caught here.
     *
     * **Per pair, not per row.** The check is the multiplicity of every (value, instruction) pair,
     * because membership plus a row total is a strictly weaker statement that this walk used to make
     * and that a real editor bug slips through: two instructions naming a value once each, against a
     * use list holding the first of them twice, passes the stamp at both entries and passes the
     * total at two. Spending one unit of the pair's forward count per entry catches the duplicate at
     * the second entry, and the missing instruction then falls out of the row total.
     */
    void checkUseLists() {
        auto values = body.values.size();
        auto instructions = body.insts.size();

        auto& start = scratch.userStart;
        auto& list = scratch.userList;
        auto& cursor = scratch.userCursor;
        auto& stamp = scratch.stamp;
        auto& pairs = scratch.pairCount;
        auto& listed = scratch.listed;

        start.clear();
        list.clear();
        cursor.clear();
        stamp.clear();
        pairs.clear();
        listed.clear();

        // `operandUses` is the row length, counted by `checkUse` as it walked the operands - so the
        // forward relation is a prefix sum away rather than a second count.
        start.push(0);
        for(U32 i = 0; i < values; i++) { start.push(start[i] + operandUses[i]); cursor.push(0); listed.push(0); }
        for(U32 i = 0; i < start[values]; i++) list.push(0);
        for(U32 i = 0; i < instructions; i++) { stamp.push(~U32(0)); pairs.push(0); }

        auto record = [&](Inst& inst, Use used) {
            if(!used.isSet()) return;

            // The same guard `checkUse` applies before counting, so that the two agree about which
            // operands are in the relation at all.
            auto value = base[used.value()];
            if(value->id.index >= values) return;
            if(body.values.get(base, value->id.index) != used.value()) return;

            auto id = value->id.index;
            if(cursor[id] >= operandUses[id]) return;

            list[start[id] + cursor[id]++] = inst.id.index;
        };

        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(base, BlockId(i));

            for(auto instPointer: block->instructions.contents(base)) {
                auto inst = base[instPointer];

                forEachUse(base, *inst, [&](Use used) { record(*inst, used); });
                forEachEdge(*inst, [&](Edge& edge) {
                    for(auto used: edge.arguments.contents(base)) record(*inst, used);
                });
            }
        }

        for(U32 i = 0; i < values; i++) {
            auto value = body.value(base, ValueId(i));

            /*
             * Every instruction that names this value, marked with it *and counted*. One pass over
             * the row rather than a rescan of a user's operands per entry in the use list.
             *
             * The row holds one entry per naming operand, so an instruction that names the value
             * twice is stamped twice and its count reaches two - which is what the entries below
             * spend against.
             */
            for(U32 k = start[i]; k < start[i] + cursor[i]; k++) {
                auto user = list[k];
                if(stamp[user] != i) { stamp[user] = i; pairs[user] = 0; }

                pairs[user]++;
            }

            for(auto userPointer: value->uses.contents(base)) {
                auto user = base[userPointer];

                if(user->id.index >= instructions || stamp[user->id.index] != i) {
                    report(user->source, "internal: %@ is listed as using %@ and names it nowhere"_v,
                           instMnemonic(user->kind), i);
                } else if(!pairs[user->id.index]) {
                    report(user->source,
                           "internal: %@ is listed as using %@ more often than it names it"_v,
                           instMnemonic(user->kind), i);
                } else {
                    pairs[user->id.index]--;
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
     * Which loans each value is the lender of - the reverse of `Loan::storage`, built once.
     *
     * `endLoansOn` used to scan every loan and every entry of its storage list at every consuming
     * use, which is the loan count multiplied into the operand count for an answer that is the same
     * at every point in the body.
     */
    void buildLenderIndex() {
        auto values = body.values.size();

        auto& start = scratch.lenderStart;
        auto& list = scratch.lenderList;
        auto& cursor = scratch.userCursor;

        start.clear();
        list.clear();
        cursor.clear();

        for(U32 i = 0; i <= values; i++) start.push(0);
        for(U32 i = 0; i < values; i++) cursor.push(0);

        for(U32 l = 0; l < loans(); l++) {
            for(auto held: body.loan(base, LoanId(l))->storage.contents(base)) {
                auto id = base[held]->id.index;
                if(id < values) start[id + 1]++;
            }
        }

        for(U32 i = 0; i < values; i++) start[i + 1] += start[i];
        for(U32 i = 0; i < start[values]; i++) list.push(0);

        for(U32 l = 0; l < loans(); l++) {
            for(auto held: body.loan(base, LoanId(l))->storage.contents(base)) {
                auto id = base[held]->id.index;
                if(id < values) list[start[id] + cursor[id]++] = l;
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
        auto values = tracked();
        auto extents = loans();
        if(!values && !extents) return;

        auto words = loanWords();

        in.clear();
        out.clear();
        for(U32 i = 0; i < blocks * values; i++) { in.push(Linear::OutOfScope); out.push(Linear::OutOfScope); }

        // Live on entry, which is the permissive end: a loan minted further down has not ended, and
        // the definition-dominance check above is what says a borrow is not used before it exists.
        loanIn.reset(blocks, extents);
        loanOut.reset(blocks, extents);

        buildLenderIndex();

        auto& state = scratch.state;
        auto& live = scratch.loanState;

        state.clear();
        live.clear();
        for(U32 v = 0; v < values; v++) state.push(Linear::OutOfScope);
        for(U32 w = 0; w < words; w++) live.push(0);

        /*
         * A worklist over reverse postorder rather than rounds over every block.
         *
         * The lattices here are not the growing sets the origin fixpoint has - `Linear` meets and
         * the loan lattice unions - so a block is revisited when a predecessor's *exit* moves and at
         * no other time, and the first visit counts as a move because the successors have nothing to
         * have met with yet.
         */
        auto& pending = scratch.pending;
        auto& walked = scratch.walked;

        pending.reset(blocks);
        walked.reset(blocks);
        for(auto index: dominators.order) pending.set(index, true);

        for(auto working = true; working; ) {
            working = false;

            for(auto index: dominators.order) {
                if(!pending.get(index)) continue;

                pending.set(index, false);
                working = true;

                auto block = body.block(base, BlockId(index));

                // The entry state is the meet of the predecessors', with unreachable ones absent.
                for(U32 v = 0; v < values; v++) {
                    auto merged = Linear::OutOfScope;
                    auto first = true;

                    for(auto predecessorPointer: block->incoming.contents(base)) {
                        auto predecessor = base[predecessorPointer]->index.index;
                        if(!dominators.isReachable(predecessor)) continue;

                        auto from = out[predecessor * values + v];
                        merged = first ? from : merge(merged, from);
                        first = false;
                    }

                    in[index * values + v] = merged;
                    state[v] = merged;
                }

                /*
                 * The loans' entry state, joined the other way: a *union*.
                 *
                 * A loan that ended on any path into this block has ended here, because a use of a
                 * value that depends on it is invalid on that path - and "invalid on one path" is
                 * the whole content of the rule. The consumption lattice above meets instead,
                 * because two paths disagreeing about a consumption is a statement about the value
                 * rather than about any one use of it.
                 */
                BitMatrix::zero(live.pointer(), words);

                for(auto predecessorPointer: block->incoming.contents(base)) {
                    auto predecessor = base[predecessorPointer]->index.index;
                    if(!dominators.isReachable(predecessor)) continue;

                    BitMatrix::unionInto(live.pointer(), loanOut.row(predecessor), words);
                }

                BitMatrix::copy(loanIn.row(index), live.pointer(), words);

                transfer(*block, state, live.pointer(), false);

                auto changed = walked.add(index);

                for(U32 v = 0; v < values; v++) {
                    if(out[index * values + v] == state[v]) continue;

                    out[index * values + v] = state[v];
                    changed = true;
                }

                changed |= BitMatrix::replace(loanOut.row(index), live.pointer(), words);

                if(changed) forEachSuccessor(base, *block, [&](U32 target) { pending.set(target, true); });
            }
        }

        // The fixpoint is settled; walk once more to report, so that a diagnostic is made once
        // rather than once per iteration.
        for(auto index: dominators.order) {
            auto block = body.block(base, BlockId(index));

            for(U32 v = 0; v < values; v++) state[v] = in[index * values + v];
            BitMatrix::copy(live.pointer(), loanIn.row(index), words);

            for(U32 v = 0; v < values; v++) {
                if(state[v] != Linear::Conflict) continue;

                /*
                 * A disagreement is a leak on one of the two paths, and only "exactly once" has a
                 * leak to report. An `Affine` value consumed in one arm of an `if` and abandoned in
                 * the other is the shape the convention exists for - `Counter` handed to a sink on
                 * the failure path and simply left behind on the success one - and nothing runs at
                 * either end of it.
                 */
                if(trackedExact[v]) {
                    report(function.source,
                           "internal: owned value %@ is consumed on some paths into block %@ and not others"_v,
                           trackedValue[v], index);
                }

                // Reported once. Continuing as consumed keeps the rest of the walk from repeating
                // the same disagreement at every block downstream of it, and is what makes a later
                // use of the value the use-after-consume it is on one of the two paths.
                state[v] = Linear::Consumed;
            }

            transfer(*block, state, live.pointer(), true);
        }
    }

    /*
     * One block's effect on the consumption state.
     *
     * Run twice: once per fixpoint iteration with `reporting` clear, and once at the end with it
     * set. The alternative - reporting during the fixpoint - names the same value once per
     * iteration, and the iteration count is not something a diagnostic should depend on.
     */
    void transfer(Block& block, Array<Linear>& state, U64* live, bool reporting) {
        auto values = tracked();

        // A loan starts, or starts over, where the value that depends on it is defined. That is what
        // makes the rule work inside a loop: the allocation is in the entry block and the borrow is
        // in the body, so the back edge carries "ended" into the header and the borrow below it
        // restarts the loan the next iteration depends on.
        auto beginLoan = [&](Value& value) {
            if(value.convention != Convention::Guaranteed) return;

            auto loan = loanOf(value);
            if(loan) BitMatrix::clear(live, loan->id.index);
        };

        // Every loan taken on this storage is over - read off the reverse index rather than by
        // scanning every loan's storage list, which is the same answer at every point in the body.
        auto endLoansOn = [&](SsaPtr<Value> storage) {
            auto id = base[storage]->id.index;
            if(id >= body.values.size()) return;

            auto first = scratch.lenderStart[id];
            auto last = scratch.lenderStart[id + 1];

            for(U32 k = first; k < last; k++) BitMatrix::set(live, scratch.lenderList[k]);
        };

        // A block's parameters are defined on entry, which is where their obligation starts.
        for(auto argumentPointer: block.arguments.contents(base)) {
            auto argument = base[argumentPointer];
            beginLoan(*argument);
            if(!isTracked(argument->convention)) continue;

            state[trackedIndex[argument->id.index]] = Linear::Unconsumed;
        }

        for(auto instPointer: block.instructions.contents(base)) {
            auto inst = base[instPointer];

            auto visit = [&](Use used) {
                if(!used.isSet()) return;

                auto value = base[used.value()];

                /*
                 * A borrow used after its lender's storage was released - §7.1's other half.
                 *
                 * This is the rule an OSSA-local loan identity exists for, and it is not implied by
                 * dominance: the `drop` and the use are both dominated by the allocation, so every
                 * structural check passes and the program reads the storage it just handed back.
                 * Nothing in a converted body reaches it - the MIR analysis places a teardown at the
                 * last use of what it releases - which is exactly why it belongs to the verifier
                 * rather than to the conversion: what it guards against is a *pass* moving one of
                 * the two.
                 */
                if(value->convention == Convention::Guaranteed) {
                    auto loan = loanOf(*value);
                    if(reporting && loan && BitMatrix::test(live, loan->id.index)) {
                        report(inst->source,
                               "internal: %@ uses %@, whose loan has already ended"_v,
                               instMnemonic(inst->kind), value->id.index);

                        // Reported once. Live again, so that the rest of the block is checked
                        // against the borrow rather than repeating this at every later use.
                        BitMatrix::clear(live, loan->id.index);
                    }
                }

                if(!isTracked(value->convention)) return;

                auto slot = trackedIndex[value->id.index];
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
             * Where a loan ends: the storage it was taken from is released, emptied, or overwritten.
             *
             * Four events, and they are read off the operands rather than stated by a marker
             * instruction - which is §13.2's question answered the other way: an `end_loan` in the
             * IR would be a second thing every pass has to move in step with the thing it actually
             * names.
             *
             *  - a `drop` of the storage - the release;
             *  - a consuming use of the storage itself - an owner that left this frame takes every
             *    borrow of it with it;
             *  - a **`load` whose result is tracked** - a destructive read, which takes the object
             *    out and leaves the slot empty;
             *  - a **`store` to the storage** - a whole-slot replacement, which puts a different
             *    object where the borrowed one was.
             *
             * The last two are the pair a rule made of the first two could not see, and they are
             * how nearly every handover in a converted body is actually written: an owner is handed
             * over by loading it out of its slot and consuming the *loaded value*, so the loan names
             * the allocation and the consuming operand names something else entirely. Nothing
             * related the two, and a pass that sank a borrow past the load was accepted.
             *
             * A `store` through a field address or through a loan is neither, and gets that answer
             * for free: `storageOfLoan` roots every loan at the allocation it was taken from, so a
             * `field.addr` result and a `Guaranteed` value are lenders of nothing and `endLoansOn`
             * finds no row for them. That is `analyze_loan.cpp`'s `replacedLocal` rule - a partial
             * write and a write *through* a loan are uses and not replacements - arrived at from the
             * operands rather than restated here.
             *
             * A load's endpoint is the load and not the consumption of what it produced, which is
             * the earlier of the two and the correct one: the slot is empty from the load onwards,
             * so a borrow of it used in between is reading storage the program has already emptied.
             */
            if(inst->kind == Inst::Drop) {
                forEachUse(base, *inst, [&](Use used) {
                    if(used.isSet()) endLoansOn(used.value());
                });
            }

            if(inst->kind == Inst::Store) {
                auto used = ((InstStore*)inst)->address;
                if(used.isSet()) endLoansOn(used.value());
            }

            if(inst->kind == Inst::Load && inst->results.size()) {
                auto used = ((InstLoad*)inst)->address;
                auto produced = base[inst->results.get(base, 0)];

                if(used.isSet() && isTracked(produced->convention)) endLoansOn(used.value());
            }

            forEachUse(base, *inst, [&](Use used) {
                if(used.isSet() && used.kind() == UseKind::Consume) endLoansOn(used.value());
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

                    // The other half of `Affine`: abandoning one is what it is allowed to do, and
                    // there is nothing to run when it happens. `Owned` is the convention that owes a
                    // consumption here, including a drop.
                    if(trackedExact[v]) {
                        report(inst->source, "internal: owned value %@ reaches %@ unconsumed"_v,
                               trackedValue[v], instMnemonic(inst->kind));
                    }

                    state[v] = Linear::Consumed;
                }
            }

            // The results are defined here, and their obligation starts after the operands above
            // were read - which is what makes an instruction that consumes an operand and produces
            // an owner two separate facts about one point.
            for(auto resultPointer: inst->results.contents(base)) {
                auto result = base[resultPointer];
                beginLoan(*result);
                if(!isTracked(result->convention)) continue;

                state[trackedIndex[result->id.index]] = Linear::Unconsumed;
            }
        }
    }

    void prepare() {
        auto count = body.values.size();

        definedIn.clear();
        definedAt.clear();
        trackedIndex.clear();
        trackedValue.clear();
        trackedExact.clear();
        operandUses.clear();
        in.clear();
        out.clear();

        for(U32 i = 0; i < count; i++) {
            definedIn.push(Dominators::kNone);
            definedAt.push(0);
            trackedIndex.push(~U32(0));
            operandUses.push(0);
        }

        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(base, BlockId(i));

            for(auto argumentPointer: block->arguments.contents(base)) {
                definedIn[base[argumentPointer]->id.index] = i;
                definedAt[base[argumentPointer]->id.index] = 0;
            }

            for(U32 position = 0; position < block->instructions.size(); position++) {
                auto inst = base[block->instructions.get(base, position)];
                for(auto resultPointer: inst->results.contents(base)) {
                    definedIn[base[resultPointer]->id.index] = i;
                    definedAt[base[resultPointer]->id.index] = position + 1;
                }
            }
        }

        for(U32 i = 0; i < count; i++) {
            auto value = body.value(base, ValueId(i));
            if(!isTracked(value->convention)) continue;

            trackedIndex[i] = U32(trackedValue.size());
            trackedValue.push(i);
            trackedExact.push(mustConsume(value->convention) ? 1 : 0);
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
        checkRegions();
        checkLoans();
        checkLinear();

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
