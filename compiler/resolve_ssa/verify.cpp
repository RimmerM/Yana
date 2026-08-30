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

    /*
     * The loan endpoint dataflow - `blocks * loans` bytes each, zero for live and one for ended.
     *
     * A separate lattice from `Linear` and not a fourth state on it, because the two disagree about
     * what a disagreement means. Two paths that disagree about whether an owned value was consumed
     * is a bug in the program and is reported as one; two paths that disagree about whether a loan
     * has ended is a *use* of that borrow being invalid on one of them, so the join is "ended" and
     * the report happens where the borrow is used rather than where the paths meet.
     */
    Array<U8> loanIn;
    Array<U8> loanOut;
    Array<U8> loanState;

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

    // The declaration region, which every type question goes through. A member rather than a local
    // in the one function that used to need it: three checks need it now, and three copies of one
    // `*program.decl` is three places to get the region wrong.
    DeclBase decl = *program.decl;

    U32 errors = 0;

    Dominators& dominators = scratch.dominators;
    Array<U32>& definedIn = scratch.definedIn;
    Array<U32>& definedAt = scratch.definedAt;
    Array<U32>& ownedIndex = scratch.ownedIndex;
    Array<U32>& ownedValue = scratch.ownedValue;
    Array<Linear>& in = scratch.in;
    Array<Linear>& out = scratch.out;
    Array<U8>& loanIn = scratch.loanIn;
    Array<U8>& loanOut = scratch.loanOut;
    Array<U32>& operandUses = scratch.operandUses;

    template<class... T>
    void report(LocationId source, StringView text, T&&... arguments) {
        errors++;
        context.diagnostics.error(text, source, forward<T>(arguments)...);
    }

    U32 owned() const { return U32(ownedValue.size()); }
    U32 loans() const { return U32(body.loans.size()); }

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
                if(value->convention == Convention::Owned && used.kind() == UseKind::Inspect) {
                    report(inst.source, "internal: %@ inspects an owned value"_v,
                           instMnemonic(inst.kind));
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
        if(!type || decl[type]->kind != mir::Type::Record) {
            report(inst.source, "internal: aggregate produces %@, which is not a record"_v,
                   mir::typeText(context, decl, type));
            return;
        }

        auto record = (mir::RecordType*)decl[type];
        if(written.components.size() != record->fields.size()) {
            report(inst.source, "internal: aggregate supplies %@ components, and %@ declares %@"_v,
                   written.components.size(), mir::typeText(context, decl, type),
                   record->fields.size());
            return;
        }

        for(U32 i = 0; i < written.components.size(); i++) {
            auto used = written.components.get(base, i);
            if(!used.isSet()) continue;

            auto supplied = base[used.value()]->type();
            auto declared = record->field(decl, U16(i)).type;

            if(!mir::sameType(supplied, declared)) {
                report(inst.source,
                       "internal: aggregate supplies component %@ of type %@, and the field is %@"_v,
                       i, mir::typeText(context, decl, supplied),
                       mir::typeText(context, decl, declared));
            }
        }
    }

    void checkFieldAddress(Inst& inst) {
        auto& written = (InstFieldAddress&)inst;

        if(!written.record || decl[written.record]->kind != mir::Type::Record) {
            report(inst.source, "internal: field.addr projects %@, which is not a record"_v,
                   mir::typeText(context, decl, written.record));
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
        if(mir::hasTeardown(decl, written.held.type)) return;

        report(inst.source, "internal: drop names %@, which has no teardown"_v,
               mir::typeText(context, decl, written.held.type));
    }

    /*
     * The continuation regions and the nesting rule over them - `Continuation`.
     *
     * Three statements: the table is a forest with one root, every block names a region that exists,
     * and every edge either stays in a region, enters a *direct* child of it, or leaves to an
     * ancestor. The third is the rule; the other two are what make it answerable.
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
    }

    // One edge against the nesting rule - see `checkRegions`. Called from the structural walk,
    // where the terminators are.
    void checkEdgeRegion(Inst& inst, Block& from, Block& target) {
        if(!from.region.valid() || !target.region.valid()) return;
        if(from.region == target.region) return;

        auto region = body.region(base, target.region);
        if(region && region->parent == from.region) return;
        if(body.encloses(base, target.region, from.region)) return;

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
                case Inst::Drop:         checkDrop(*inst); break;
                default: break;
            }
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
        auto extents = loans();
        if(!values && !extents) return;

        for(U32 i = 0; i < blocks * values; i++) { in.push(Linear::OutOfScope); out.push(Linear::OutOfScope); }

        // Live on entry, which is the permissive end: a loan minted further down has not ended, and
        // the definition-dominance check above is what says a borrow is not used before it exists.
        for(U32 i = 0; i < blocks * extents; i++) { loanIn.push(0); loanOut.push(0); }

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

                /*
                 * The loans' entry state, joined the other way: a *union*.
                 *
                 * A loan that ended on any path into this block has ended here, because a use of a
                 * value that depends on it is invalid on that path - and "invalid on one path" is
                 * the whole content of the rule. The consumption lattice above meets instead,
                 * because two paths disagreeing about a consumption is a statement about the value
                 * rather than about any one use of it.
                 */
                for(U32 l = 0; l < extents; l++) {
                    U8 ended = 0;
                    for(auto predecessorPointer: block->incoming.contents(base)) {
                        auto predecessor = base[predecessorPointer]->index.index;
                        if(!dominators.isReachable(predecessor)) continue;

                        ended |= loanOut[predecessor * extents + l];
                    }

                    if(loanIn[index * extents + l] != ended) {
                        loanIn[index * extents + l] = ended;
                        changed = true;
                    }
                }

                auto& state = scratch.state;
                state.clear();
                for(U32 v = 0; v < values; v++) state.push(in[index * values + v]);

                auto& live = scratch.loanState;
                live.clear();
                for(U32 l = 0; l < extents; l++) live.push(loanIn[index * extents + l]);

                transfer(*block, state, live, false);

                for(U32 v = 0; v < values; v++) {
                    if(out[index * values + v] != state[v]) {
                        out[index * values + v] = state[v];
                        changed = true;
                    }
                }

                for(U32 l = 0; l < extents; l++) {
                    if(loanOut[index * extents + l] != live[l]) {
                        loanOut[index * extents + l] = live[l];
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

            auto& live = scratch.loanState;
            live.clear();
            for(U32 l = 0; l < loans(); l++) live.push(loanIn[index * loans() + l]);

            for(U32 v = 0; v < owned(); v++) {
                if(state[v] != Linear::Conflict) continue;

                report(function.source,
                       "internal: owned value %@ is consumed on some paths into block %@ and not others"_v,
                       ownedValue[v], index);

                // Reported once. Continuing as consumed keeps the rest of the walk from repeating
                // the same disagreement at every block downstream of it.
                state[v] = Linear::Consumed;
            }

            transfer(*block, state, live, true);
        }
    }

    /*
     * One block's effect on the consumption state.
     *
     * Run twice: once per fixpoint iteration with `reporting` clear, and once at the end with it
     * set. The alternative - reporting during the fixpoint - names the same value once per
     * iteration, and the iteration count is not something a diagnostic should depend on.
     */
    void transfer(Block& block, Array<Linear>& state, Array<U8>& live, bool reporting) {
        auto values = owned();

        // A loan starts, or starts over, where the value that depends on it is defined. That is what
        // makes the rule work inside a loop: the allocation is in the entry block and the borrow is
        // in the body, so the back edge carries "ended" into the header and the borrow below it
        // restarts the loan the next iteration depends on.
        auto beginLoan = [&](Value& value) {
            if(value.convention != Convention::Guaranteed) return;

            auto loan = loanOf(value);
            if(loan) live[loan->id.index] = 0;
        };

        // Every loan taken on this storage is over. A scan rather than an index, because a body
        // holds a handful of loans and the alternative is a side table for one instruction kind.
        auto endLoansOn = [&](SsaPtr<Value> storage) {
            for(U32 l = 0; l < loans(); l++) {
                auto loan = body.loan(base, LoanId(l));
                for(auto held: loan->storage.contents(base)) {
                    if(held == storage) { live[l] = 1; break; }
                }
            }
        };

        // A block's parameters are defined on entry, which is where their obligation starts.
        for(auto argumentPointer: block.arguments.contents(base)) {
            auto argument = base[argumentPointer];
            beginLoan(*argument);
            if(argument->convention != Convention::Owned) continue;

            state[ownedIndex[argument->id.index]] = Linear::Unconsumed;
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
                    if(reporting && loan && live[loan->id.index]) {
                        report(inst->source,
                               "internal: %@ uses %@, whose loan has already ended"_v,
                               instMnemonic(inst->kind), value->id.index);

                        // Reported once. Live again, so that the rest of the block is checked
                        // against the borrow rather than repeating this at every later use.
                        live[loan->id.index] = 0;
                    }
                }

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
             * Where a loan ends: the storage it was taken from is released, or handed away.
             *
             * A `drop` is the release, and a consuming use is the handover - an owner that left this
             * frame takes every borrow of it with it. Both are read off the operands rather than
             * stated by a marker instruction, which is §13.2's question answered the other way: an
             * `end_loan` in the IR would be a second thing every pass has to move in step with the
             * thing it actually names.
             */
            if(inst->kind == Inst::Drop) {
                forEachUse(base, *inst, [&](Use used) {
                    if(used.isSet()) endLoansOn(used.value());
                });
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
                beginLoan(*result);
                if(result->convention != Convention::Owned) continue;

                state[ownedIndex[result->id.index]] = Linear::Unconsumed;
            }
        }
    }

    void prepare() {
        auto count = body.values.size();

        definedIn.clear();
        definedAt.clear();
        ownedIndex.clear();
        ownedValue.clear();
        operandUses.clear();
        in.clear();
        out.clear();
        loanIn.clear();
        loanOut.clear();

        for(U32 i = 0; i < count; i++) {
            definedIn.push(Dominators::kNone);
            definedAt.push(0);
            ownedIndex.push(~U32(0));
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
            if(value->convention != Convention::Owned) continue;

            ownedIndex[i] = U32(ownedValue.size());
            ownedValue.push(i);
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
