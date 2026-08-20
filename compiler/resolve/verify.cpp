#include "verify.h"
#include "generic.h"
#include "place.h"

namespace {

/*
 * Where one value is defined, as a position the ordering checks can compare.
 *
 * `block` is the value's position in `Function::blocks` rather than `Block::index`, because the two
 * disagreeing is itself one of the things being checked. `order` is the position within that block
 * in the order the backends walk it: the phis first, then the instructions, then the terminator. An
 * argument is defined before all of them, which is what zero means here and why the instructions
 * start at one.
 */
struct Definition {
    U32 block = 0;
    U32 order = 0;
};

struct Verifier {
    Verifier(Module& module, Function& function, VerifyStage stage, StringView where):
        module(module), function(function), context(module.context), global(*module.types),
        local(*module.arena), stage(stage), where(where) {}

    Module& module;
    Function& function;
    Context& context;
    GlobalBase global;
    ModuleBase local;
    VerifyStage stage;
    StringView where;

    bool ok = true;

    // Every value this function defines - the arguments, the phis, the instructions and each
    // block's terminator - by arena offset. Constants are deliberately not among them: one belongs
    // to no block and is reached only through whatever names it, so "is this still defined" is not
    // a question about a constant.
    HashMap<U32, Definition> definitions;

    // How many times each value is named as an operand, by arena offset. Counted over the whole
    // function and then compared against that value's own use list, which is the check the two-sided
    // structure exists to make possible.
    HashMap<U32, U32> operandCounts;

    template<class... T>
    void fail(LocationId source, StringView text, T&&... args) {
        ok = false;

        char detail[2048];
        auto length = Tritium::format(toBuffer(detail), toString(text), forward<T>(args)...);

        context.diagnostics.error("internal error: the resolve IR of %@ is inconsistent %@ - %@"_v,
                                  source, context.findName(function.name), where,
                                  StringView { detail, length });
    }

    // How a value is named in a message. Its id is what the dump prints it as, so a finding can be
    // matched against `-print-resolve` output without counting instructions.
    U32 idOf(ModulePtr<Value> value) { return value ? local[value]->id : maxLimit<U32>; }

    void run();

    void verifyBlockStructure();
    void collectDefinitions();
    void verifyControlFlow();
    void verifyPhis();
    void verifyOperands();
    void verifyUseLists();
    void verifyLocals();
    void verifyArgs();
    void verifyInstruction(Value& instruction);
    void verifyPlace(Value& instruction, const Place& place);
    void verifyGenCall(InstGenCall& call);

    Size blockCount() { return function.blocks.size(); }
    Block* blockAt(Size index) { return local[function.blocks.get(local, index)]; }

    /*
     * Every value this function defines, in the order the backends walk them: the parameters, then
     * each block's phis, its instructions and its terminator.
     *
     * Constants are not among them and cannot be - one belongs to no block and is reached only
     * through whatever names it - so what is said about a constant here is said by whoever names it.
     */
    template<class F>
    void eachValue(F&& f) {
        for(auto arg: function.args.contents(local)) f((ModulePtr<Value>)arg);

        for(Size b = 0; b < blockCount(); b++) {
            auto block = blockAt(b);

            for(auto phi: block->phis(local)) f((ModulePtr<Value>)phi);
            for(auto instruction: block->instructions(local)) f((ModulePtr<Value>)instruction);
            if(block->terminator()) f((ModulePtr<Value>)block->terminator());
        }
    }

    /*
     * Which blocks dominate which, computed here rather than borrowed from `compiler/opt`.
     *
     * The verifier runs at four points in the pipeline and two of them are below that stage, so
     * there is nothing to borrow at the earliest of them. It is the textbook iterative bit-vector
     * solution over the block list, which is the same answer `computeDominance` reaches by the same
     * route - and computing it separately is the point rather than duplication to be removed: a
     * check that reads the same dominator tree the pass under test read would agree with it about a
     * tree they were both wrong about.
     */
    IndexSetList dominators;
    void computeDominators();
    bool dominates(U32 dominator, U32 block) { return dominators[block][dominator]; }
};

void Verifier::computeDominators() {
    auto count = blockCount();
    dominators.reset(count, count);

    // Everything dominates everything to start with, except the entry, which only dominates itself:
    // a greatest fixed point, which is the direction that gets loops right.
    for(Size b = 0; b < count; b++) {
        for(Size d = 0; d < count; d++) dominators[b].set(d, b != 0);
    }

    if(count) dominators[0].set(0, true);

    IndexSet incoming;
    for(auto changed = true; changed;) {
        changed = false;

        for(Size b = 1; b < count; b++) {
            auto block = blockAt(b);
            auto first = true;

            incoming.reset(count);

            for(auto predecessorPointer: block->incoming(local)) {
                auto predecessor = local[predecessorPointer]->index;
                if(predecessor >= count) continue;

                for(Size d = 0; d < count; d++) {
                    auto value = dominators[predecessor][d];
                    incoming.set(d, first ? value : (incoming[d] && value));
                }

                first = false;
            }

            // A block nothing reaches keeps the initial full set, which makes every question about
            // it answer "dominated" and stops unreachable code from producing findings of its own.
            if(first) continue;
            incoming.set(b, true);

            for(Size d = 0; d < count; d++) {
                if(incoming[d] == dominators[b][d]) continue;

                dominators[b].set(d, incoming[d]);
                changed = true;
            }
        }
    }
}

void Verifier::verifyBlockStructure() {
    for(Size b = 0; b < blockCount(); b++) {
        auto blockPointer = function.blocks.get(local, b);
        auto block = local[blockPointer];

        if(block->index != b) {
            fail(block->source, "block %@ is listed at position %@"_v, U32(block->index), U32(b));
        }

        if(block->function != &function - local) {
            fail(block->source, "block %@ belongs to a different function"_v, U32(b));
        }

        if(!block->terminator()) {
            fail(block->source, "block %@ has no terminator"_v, U32(b));
            continue;
        }

        for(auto phiPointer: block->phis(local)) {
            auto phi = local[phiPointer];

            if(phi->kind != Value::Phi) {
                fail(phi->source, "%%@ is in block %@'s phi list and is not a phi"_v,
                     phi->id, U32(b));
            }

            if(phi->block != blockPointer) fail(phi->source, "phi %%@ names another block"_v, phi->id);
        }

        for(auto instructionPointer: block->instructions(local)) {
            auto instruction = local[instructionPointer];

            if(isTerminator(*instruction)) {
                fail(instruction->source, "terminator %%@ is in block %@'s instruction list"_v,
                     instruction->id, U32(b));
            }

            if(instruction->kind == Value::Phi) {
                fail(instruction->source, "phi %%@ is in block %@'s instruction list"_v,
                     instruction->id, U32(b));
            }

            if(instruction->block != blockPointer) {
                fail(instruction->source, "%%@ is in block %@ and names another"_v,
                     instruction->id, U32(b));
            }
        }

        auto terminator = local[block->terminator()];
        if(!isTerminator(*terminator)) {
            fail(terminator->source, "block %@ ends with %%@, which is not a terminator"_v,
                 U32(b), terminator->id);
        }

        if(terminator->block != blockPointer) {
            fail(terminator->source, "block %@ ends with a terminator that names another block"_v, U32(b));
        }
    }
}

void Verifier::collectDefinitions() {
    auto define = [&](ModulePtr<Value> value, U32 block, U32 order) {
        auto entry = definitions.add(U32(value));
        if(entry.existed) {
            fail(local[value]->source, "%%@ is defined twice"_v, local[value]->id);
            return;
        }

        *entry.value = Definition { block, order };
    };

    for(auto argPointer: function.args.contents(local)) define((ModulePtr<Value>)argPointer, 0, 0);

    for(Size b = 0; b < blockCount(); b++) {
        auto block = blockAt(b);
        U32 order = 1;

        for(auto phiPointer: block->phis(local)) {
            define((ModulePtr<Value>)phiPointer, U32(b), order++);
        }

        for(auto instructionPointer: block->instructions(local)) {
            define((ModulePtr<Value>)instructionPointer, U32(b), order++);
        }

        if(block->terminator()) define((ModulePtr<Value>)block->terminator(), U32(b), order);
    }
}

/*
 * The three places one edge is recorded, checked against each other - see the CFG invariants in
 * Implementation-IR.md and `retargetEdge` in opt_branch.cpp.
 *
 * Counted rather than tested for membership, because a `Je` whose two arms are the same block is a
 * real shape: `IrEditor::append` pushes the predecessor into that successor's `incoming` twice, and
 * a pass that removes one of the two edges has to remove one of the two entries.
 *
 * No fixture produces one, measured - so every per-arm case in `IrEditor` is reasoned rather than
 * exercised, and this count is the whole of what stands behind them. A membership test here would
 * make all of them unfalsifiable at once.
 */
void Verifier::verifyControlFlow() {
    auto successorCount = [&](Block& block, ModulePtr<Block> successor) {
        Size count = 0;
        for(auto outgoing: block.successors()) {
            if(outgoing == successor) count++;
        }
        return count;
    };

    auto predecessorCount = [&](Block& block, ModulePtr<Block> predecessor) {
        Size count = 0;
        for(auto incoming: block.incoming(local)) {
            if(incoming == predecessor) count++;
        }
        return count;
    };

    for(Size b = 0; b < blockCount(); b++) {
        auto blockPointer = function.blocks.get(local, b);
        auto block = local[blockPointer];
        if(!block->terminator()) continue;

        auto terminator = local[block->terminator()];

        /*
         * What the terminator says, against what the block's own edge list says. They are written
         * together by `IrEditor::append` and repointed separately by everything else.
         *
         * Per slot rather than per kind: the arms are the terminator's own (see
         * instructionSuccessorSlots) and a block's outgoing slots are the same list by ordinal, so
         * this checks a terminator added to the IR without being told anything about it. The slots
         * an instruction does not use are null on both sides, which is what makes "returns and has
         * successors" the same comparison as the other two rather than a case of its own.
         */
        ModulePtr<Block>* arms[kMaxSuccessors];
        auto armCount = instructionSuccessorSlots(*terminator, arms);

        for(Size arm = 0; arm < kMaxSuccessors; arm++) {
            auto named = arm < armCount ? *arms[arm] : nullptr;
            if(block->successor(arm) == named) continue;

            fail(terminator->source, "block %@'s %@ leaves somewhere its successor list does not name"_v,
                 U32(b), instructionMnemonic(terminator->kind));
            break;
        }

        for(auto outgoing: block->successors()) {
            if(!outgoing) continue;

            auto successor = local[outgoing];
            if(successor->function != &function - local) {
                fail(terminator->source, "block %@ has a successor in another function"_v, U32(b));
                continue;
            }

            if(predecessorCount(*successor, blockPointer) != successorCount(*block, outgoing)) {
                fail(terminator->source,
                     "the edge from block %@ to block %@ is not recorded by both ends"_v,
                     U32(b), U32(successor->index));
            }
        }

        for(auto incoming: block->incoming(local)) {
            auto predecessor = local[incoming];

            if(predecessor->function != &function - local) {
                fail(block->source, "block %@ has a predecessor in another function"_v, U32(b));
                continue;
            }

            if(successorCount(*predecessor, blockPointer) != predecessorCount(*block, incoming)) {
                fail(block->source, "block %@ names block %@ as a predecessor, which does not branch here"_v,
                     U32(b), U32(predecessor->index));
            }
        }
    }
}

// The third place an edge lives: one alternative per phi of the successor. A phi left naming a
// predecessor that no longer branches here prints correctly and produces no copy on that edge.
void Verifier::verifyPhis() {
    for(Size b = 0; b < blockCount(); b++) {
        auto block = blockAt(b);

        for(auto phiPointer: block->phis(local)) {
            auto phi = local[phiPointer];

            if(phi->inputs.size() != block->predecessorCount()) {
                fail(phi->source, "phi %%@ has %@ alternatives and block %@ has %@ predecessors"_v,
                     phi->id, U32(phi->inputs.size()), U32(b), U32(block->predecessorCount()));
                continue;
            }

            for(auto input: phi->inputs.contents(local)) {
                Size found = 0;
                for(auto incoming: block->incoming(local)) {
                    if(incoming == input.block) found++;
                }

                Size alternatives = 0;
                for(auto other: phi->inputs.contents(local)) {
                    if(other.block == input.block) alternatives++;
                }

                if(found != alternatives) {
                    fail(phi->source, "phi %%@ has %@ alternatives for a predecessor that reaches block %@ %@ times"_v,
                         phi->id, U32(alternatives), U32(b), U32(found));
                }

                if(!input.value) {
                    fail(phi->source, "phi %%@ has an alternative with no value"_v, phi->id);
                    continue;
                }

                if(isConstant(*local[input.value])) continue;

                auto definition = definitions.get(U32(input.value));
                if(!definition) {
                    fail(phi->source, "phi %%@ reads %%@, which this function does not define"_v,
                         phi->id, idOf(input.value));
                    continue;
                }

                // A phi's alternative is read on the edge rather than at the phi, so what it needs
                // is to reach the *end* of the predecessor it arrives from.
                auto from = local[input.block]->index;
                if(from < blockCount() && !dominates(definition.unwrap().block, from)) {
                    fail(phi->source, "phi %%@ reads %%@ on an edge it does not reach"_v,
                         phi->id, idOf(input.value));
                }
            }
        }
    }
}

void Verifier::verifyOperands() {
    auto check = [&](Value& user, const Definition& at, ModulePtr<Value> operand) {
        // `HashMap::add` does not construct the value it hands back, so a fresh entry starts at
        // whatever the bucket held.
        auto entry = operandCounts.add(U32(operand));
        *entry.value = entry.existed ? *entry.value + 1 : 1;

        /*
         * The kind's own claim about itself, checked - see `producesValue`, and inst.def where each
         * kind makes it.
         *
         * A store, an aggregate, a drop, a swap and the three terminators define nothing, so an
         * operand naming one is a read of something that was never a value. It is worth a check of
         * its own rather than being left to the dominance rules below, which every one of them would
         * pass: they are ordinary instructions in ordinary blocks, and what is wrong is that the
         * reader believes they hand something back.
         */
        if(!producesValue(*local[operand])) {
            fail(user.source, "%%@ names %%@, which produces no value"_v, user.id, idOf(operand));
            return;
        }

        if(isConstant(*local[operand])) return;

        auto definition = definitions.get(U32(operand));
        if(!definition) {
            // An operand naming an instruction that has been taken out of its block, or a value
            // belonging to another function entirely.
            fail(user.source, "%%@ names %%@, which this function does not define"_v,
                 user.id, idOf(operand));
            return;
        }

        // A phi's alternatives are checked against their own edges above, since the block the value
        // has to reach is the predecessor rather than the phi's own.
        if(user.kind == Value::Phi) return;

        auto& from = definition.unwrap();

        /*
         * `Function::blocks` has to stay in an order where a definition precedes its uses, because
         * `lowerProgram` walks the list in that order and asserts exactly this - a block appended
         * after one that reads what it defines fails as "resolve value was used before it was
         * lowered", from inside `mappedValue` and with nothing to say about which pass appended it.
         */
        if(from.block > at.block) {
            fail(user.source, "%%@ in block %@ reads %%@, which block %@ defines later in the list"_v,
                 user.id, U32(at.block), idOf(operand), U32(from.block));
        } else if(from.block == at.block) {
            if(from.order >= at.order) {
                fail(user.source, "%%@ reads %%@, which is defined after it"_v, user.id, idOf(operand));
            }
        } else if(!dominates(from.block, at.block)) {
            fail(user.source, "%%@ reads %%@, which block %@ does not dominate block %@ to define"_v,
                 user.id, idOf(operand), U32(from.block), U32(at.block));
        }
    };

    auto walk = [&](ModulePtr<Value> valuePointer) {
        auto& value = *local[valuePointer];
        auto at = definitions.get(U32(valuePointer));
        if(!at) return;

        auto position = at.unwrap();
        eachOperand(local, value, [&](ModulePtr<Value> operand) { check(value, position, operand); });
        eachPlaceRootValue(local, function, value,
                           [&](ModulePtr<Value> root) { check(value, position, root); });

        verifyInstruction(value);
    };

    eachValue(walk);
}

/*
 * Both directions of the def-use relation, against each other.
 *
 * The count has to match rather than the membership, because an instruction naming one value twice
 * appears in its use list twice - `dropUse` removes one entry for exactly that reason - so a list
 * that merely *contains* every user is not a list a pass may remove one entry from.
 */
void Verifier::verifyUseLists() {
    auto verify = [&](ModulePtr<Value> valuePointer) {
        auto& value = *local[valuePointer];

        auto expected = operandCounts.getValue(U32(valuePointer));
        auto count = expected ? expected.unwrap() : 0;

        if(value.useCount() != count) {
            fail(value.source, "%%@ is named by %@ operands and its use list holds %@"_v,
                 value.id, count, U32(value.useCount()));
        }

        for(auto userPointer: value.uses(local)) {
            if(!definitions.get(U32(userPointer))) {
                fail(value.source, "%%@ is used by an instruction this function does not define"_v,
                     value.id);
                continue;
            }

            auto names = false;
            eachOperand(local, *local[userPointer], [&](ModulePtr<Value> operand) {
                if(operand == valuePointer) names = true;
            });

            eachPlaceRootValue(local, function, *local[userPointer], [&](ModulePtr<Value> root) {
                if(root == valuePointer) names = true;
            });

            if(!names) {
                fail(value.source, "%%@ is used by %%@, which does not name it"_v,
                     value.id, local[userPointer]->id);
            }
        }
    };

    eachValue(verify);
}

/*
 * `Local::value` and `Value::slot` - one fact written in two fields, checked in the direction that
 * has to hold.
 *
 * The relation is not a bijection and cannot be made one: `inlineCalls` points every slot that named
 * an inlined call's storage at the value that replaced it, so several slots legitimately hold one
 * value. What is asked of the *value* is single-valued, though - `Value::slot` is what `findPlace`
 * and `backingLocal` answer with - so the check is that the slot a value names names it back, which
 * is exactly what the assertion in `backingLocal` relies on.
 */
void Verifier::verifyLocals() {
    for(Size i = 0; i < function.localCount(); i++) {
        auto slot = function.localAt(local, i);

        // A slot may hold a constant, which belongs to no block and is therefore not something this
        // function "defines" - see collectDefinitions.
        if(slot.value && !isConstant(*local[slot.value]) && !definitions.get(U32(slot.value))) {
            fail(function.source, "local %@ is filled by %%@, which this function does not define"_v,
                 U32(i), idOf(slot.value));
        }

        if(slot.viewOf != maxLimit<U32> && slot.viewOf >= function.localCount()) {
            fail(function.source, "local %@ is a view of slot %@, which does not exist"_v,
                 U32(i), slot.viewOf);
        }
    }

    auto pairing = [&](ModulePtr<Value> valuePointer) {
        auto& value = *local[valuePointer];
        if(value.slot == maxLimit<U32>) return;

        if(value.slot >= function.localCount()) {
            fail(value.source, "%%@ names slot %@, which does not exist"_v, value.id, value.slot);
            return;
        }

        if(function.localAt(local, value.slot).value != valuePointer) {
            fail(value.source, "%%@ names slot %@, which holds something else"_v, value.id, value.slot);
        }
    };

    eachValue(pairing);
}

/*
 * The argument list, and the contracts a caller reads off it.
 *
 * A parameter is reached by index from three directions - the list, the `Arg` itself and the
 * summary - and a specialization that rebuilds one of them is what makes them able to disagree.
 */
void Verifier::verifyArgs() {
    auto entry = function.blocks.isNotEmpty() ? function.blocks.get(local, 0) : ModulePtr<Block>(nullptr);

    for(Size i = 0; i < function.args.size(); i++) {
        auto arg = local[function.args.get(local, i)];

        if(arg->kind != Value::Arg) {
            fail(arg->source, "argument %@ is not an argument value"_v, U32(i));
            continue;
        }

        if(arg->index != i) {
            fail(arg->source, "argument %@ is indexed as %@"_v, U32(i), U32(arg->index));
        }

        if(arg->block != entry) {
            fail(arg->source, "argument %@ is not defined in the entry block"_v, U32(i));
        }

        if(!arg->type) fail(arg->source, "argument %@ has no type"_v, U32(i));

        // A `@lazy` parameter arrives as a nullary thunk over the caller's frame, so its own type is
        // the function type and `lazyType` is what the signature declared - see Arg::lazyType.
        if(arg->isLazy() && arg->type && global[arg->type]->kind != Type::Fun) {
            fail(arg->source, "lazy argument %@ does not arrive as a function"_v, U32(i));
        }

        /*
         * The `return` marker is a member of the declared group, which is a mask over argument
         * indices and therefore cannot describe one past the 64th - resolveSignature says so rather
         * than dropping it silently.
         *
         * Only once ownership has run, because the group is `deriveSummary`'s answer: the marker is
         * on the declaration from the moment the signature is resolved, and the mask that has to
         * agree with it does not exist until the summaries settle.
         */
        if(stage == VerifyStage::Resolved) continue;

        if(arg->returnRoot && i < 64 && !(function.summary.declaredRoots & (U64(1) << i))) {
            fail(arg->source, "argument %@ is a return root and is not in the declared group"_v, U32(i));
        }
    }

    if(function.summary.ready && !function.summary.opaque &&
       function.summary.args.size() != function.args.size()) {
        fail(function.source, "the summary describes %@ arguments and the function takes %@"_v,
             U32(function.summary.args.size()), U32(function.args.size()));
    }
}

void Verifier::verifyPlace(Value& instruction, const Place& place) {
    /*
     * The tail-read guarantee - Implementation-Vector.md §3.3 and §9.7, Design-Vector §5.3.
     *
     * An overreading load reads up to a vector's width past the extent of what it names, and that is
     * safe exactly where the storage carries the guarantee: the heap, the frame and static data are
     * all padded by the runtime and the linker, and a slice of one inherits it.
     *
     * **§3.3 asks for the check to be "not rooted in a raw pointer", and that rule is not one this
     * IR can hold.** Natively *everything under a slice is a raw pointer* - `Flat(a)` is `{items:
     * %a, length}` and `Index(Flat(a)).get` is `borrow(self.items + index)` - so an overreading load
     * of a slice's storage is rooted the way a subscript of the same slice is rooted, and the borrow
     * that says so does not even survive: `collapseBorrows` rewrites a borrow root into the place
     * the borrow was taken of, which is the pointer. Written as §3.3 asks, this refused every
     * program `loadVectorTail` appears in, and refused it in the assertion build alone.
     *
     * Which storage a slice was taken *of* is not a question this level can ask at all, so the rule
     * that does hold the invariant is one layer up and is a *type*: `Collections.loadVectorTail`
     * takes a `Flat(a)`, `Native.vectorPast` is the only thing that sets this flag, and an
     * `Unpadded(a)` is refused by having no slice to hand over. What is left here is the half that
     * is checkable and is still worth checking - the flag belongs to a *vector* transfer, and a
     * scalar load carrying it is a load nothing exempted from the bounds reasoning downstream.
     * `lower_validate.cpp` says the same thing from the other side of the seam.
     */
    if(instruction.kind == Value::LoadPlace && ((InstLoadPlace&)instruction).overread) {
        if(!isVectorType(global, instruction.type)) {
            fail(instruction.source, "%%@ reads past the end of the place it names and is not a vector load - only a vector transfer spends the tail-read guarantee"_v,
                 instruction.id);
        }
    }

    switch(place.root) {
        case PlaceRoot::Local:
            if(place.local >= function.localCount()) {
                fail(instruction.source, "%%@ names local %@, which does not exist"_v,
                     instruction.id, place.local);
                return;
            }
            break;
        case PlaceRoot::Global:
            if(!place.global) {
                fail(instruction.source, "%%@ is rooted in no global"_v, instruction.id);
                return;
            }
            break;
        case PlaceRoot::Pointer:
        case PlaceRoot::Borrow:
            if(!place.pointer) {
                fail(instruction.source, "%%@ is rooted in no reference"_v, instruction.id);
                return;
            }
            break;
    }

    auto projections = place.projections;
    auto count = projections.size();

    for(Size i = 0; i < count; i++) {
        auto projection = projections.get(local, i);

        switch(projection.kind) {
            /*
             * The word a packed field lives in, appended by compiler/opt and by nothing else. Two
             * invariants everything downstream relies on: it is the last step of a path, and the one
             * before it is the packed field - see ProjectionKind::Unit, and `mayAlias` in
             * opt_place.cpp, which is entitled to say two paths differing before it do not alias.
             */
            case ProjectionKind::Unit:
                if(i + 1 != count) {
                    fail(instruction.source, "%%@ projects a storage unit and then steps further"_v,
                         instruction.id);
                }

                if(i == 0 || projections.get(local, i - 1).kind != ProjectionKind::Field) {
                    fail(instruction.source, "%%@ projects a storage unit of something that is not a field"_v,
                         instruction.id);
                }
                break;

            // An element is selected by a value, which travels as an ordinary operand of whatever
            // access it appears in.
            case ProjectionKind::Index:
                if(!projection.value) {
                    fail(instruction.source, "%%@ indexes with no index"_v, instruction.id);
                }
                break;

            /*
             * The only projection that names a requirement rather than a structure, so it is legal
             * exactly where the requirement exists: inside a body that has a schema to hold the
             * slot. `clonePlace` rewrites it into the Downcast and Field it always meant when the
             * body is specialized, which is why nothing downstream has a case for it.
             */
            case ProjectionKind::Property:
                if(!functionGen(global, function)) {
                    fail(instruction.source, "%%@ reads a constrained field in a body with no requirements"_v,
                         instruction.id);
                }
                break;

            default:
                break;
        }
    }

    // And the path as a whole, through the one walk that decides what a step arrives at. A step it
    // cannot resolve is a path that does not fit the type it is rooted in, which every consumer
    // reads as a null type and then steps off the end of.
    walkPlace(module, function, place, [&](const PlaceStep& step) {
        if(!step.broken) return true;

        fail(instruction.source, "%%@ names a place whose step %@ does not fit what it is taken of"_v,
             instruction.id, U32(step.at));
        return false;
    });
}

/*
 * A generic call's slot numbers, against the schema they index.
 *
 * A slot number is what emitted code *loads* - the caller writes slot 3 and the callee reads slot 3
 * - so a number that outran its schema is a load of whatever happens to be next in the environment
 * rather than a failure. `genSchemaOf` derives the numbering from the context, and adding a
 * requirement renumbers it, which is what makes a stale index reachable at all.
 */
void Verifier::verifyGenCall(InstGenCall& call) {
    if(call.typeClass) {
        auto& typeClass = *global[call.typeClass];

        if(call.index >= typeClass.functions.size()) {
            fail(call.source, "%%@ dispatches to member %@ of a class with %@ of them"_v,
                 call.id, U32(call.index), U32(typeClass.functions.size()));
        }
    }

    auto env = functionGen(global, function);
    auto slots = env ? genSchemaOf(module, *env).slots.size() : 0;

    if(call.classSlot != maxLimit<U16>) {
        if(call.classSlot >= slots) {
            fail(call.source, "%%@ dispatches through slot %@ of a schema with %@ slots"_v,
                 call.id, U32(call.classSlot), U32(slots));
        } else if(genSchemaOf(module, *env).slots.get(global, call.classSlot).kind != GenSlotKind::Class) {
            fail(call.source, "%%@ dispatches through slot %@, which holds no class witness"_v,
                 call.id, U32(call.classSlot));
        }
    }

    for(Size i = 0; i < call.fill.size(); i++) {
        auto slot = call.fill.get(local, i);
        if(!slot.isForwarded()) continue;

        if(slot.forwarded >= slots) {
            fail(call.source, "%%@ fills slot %@ from slot %@ of a schema with %@ slots"_v,
                 call.id, U32(i), U32(slot.forwarded), U32(slots));
            continue;
        }

        /*
         * A count forwarded from a slot that is not one - Implementation-Const-Generics.md §3.1.
         *
         * The two cell shapes are the whole of what this checks: a count holds a number and every
         * other slot holds an anchor-relative address, so forwarding one into the other would decode
         * a number as a pointer with nothing anywhere to say so. It is exactly the failure the §2.4
         * rename existed to prevent, one layer down.
         */
        auto forwarded = genSchemaOf(module, *env).slots.get(global, slot.forwarded).kind;
        if((forwarded == GenSlotKind::Const) != slot.count) {
            fail(call.source, "%%@ fills slot %@ from slot %@, which holds the other kind of cell"_v,
                 call.id, U32(i), U32(slot.forwarded));
        }
    }
}

void Verifier::verifyInstruction(Value& instruction) {
    if(!instruction.type) {
        fail(instruction.source, "%%@ has no type"_v, instruction.id);
    }

    eachPlace(instruction, [&](const Place& place) { verifyPlace(instruction, place); });

    switch(instruction.kind) {
        case Value::Alloc: {
            auto& allocation = (InstAlloc&)instruction;

            if(allocation.local != maxLimit<U32> && allocation.local >= function.localCount()) {
                fail(instruction.source, "%%@ allocates local %@, which does not exist"_v,
                     instruction.id, allocation.local);
            }

            // Reserved and never selected: regions are deliberately not part of this milestone, and
            // the rung exists so that adding one later is a third case in an existing decision.
            if(stage != VerifyStage::Resolved && allocation.storage == StorageClass::Region) {
                fail(instruction.source, "%%@ is placed in a region"_v, instruction.id);
            }
            break;
        }

        /*
         * Inserted by the drop pass and never by the AST resolver, which is what makes its absence
         * checkable: a `Drop` in a body ownership has not run over means something built one early,
         * and one built early is one placed by nothing.
         */
        case Value::Drop: {
            auto& drop = (InstDrop&)instruction;

            /*
             * Inserted by the drop pass and never by the AST resolver, which is what makes its
             * absence checkable at all: a `Drop` in a body the placement pass has not run over is
             * one placed by nothing.
             *
             * Asked only of a body that came from source. Compiler-generated teardown *is* a body
             * made of drops - `analyze_teardown.cpp` writes one per member as it generates the glue,
             * and the erased entry a descriptor slot holds is one instruction and that instruction
             * is a drop - so the statement is about who wrote the body rather than about the stage
             * alone. `Function::anonymous` is what marks a body reached through something other than
             * its name, which is every piece of that glue.
             */
            if(stage == VerifyStage::Resolved && !function.anonymous) {
                fail(instruction.source, "%%@ drops before ownership has run"_v, instruction.id);
            }

            // The pass elides an empty teardown rather than emitting one, so reaching this is a
            // decision that was taken and then not acted on.
            if(drop.isEmpty()) {
                fail(instruction.source, "%%@ drops nothing, runs nothing and releases nothing"_v,
                     instruction.id);
            }
            break;
        }

        case Value::Je: {
            auto& branch = (InstJe&)instruction;

            if(!branch.cond) fail(instruction.source, "%%@ branches on nothing"_v, instruction.id);
            if(!branch.thenBlock || !branch.elseBlock) {
                fail(instruction.source, "%%@ branches to nowhere"_v, instruction.id);
            }
            break;
        }

        case Value::Jmp:
            if(!((InstJmp&)instruction).target) {
                fail(instruction.source, "%%@ jumps to nowhere"_v, instruction.id);
            }
            break;

        case Value::Phi:
            if(((InstPhi&)instruction).inputs.isEmpty()) {
                fail(instruction.source, "%%@ has no alternatives"_v, instruction.id);
            }
            break;

        case Value::Call:
            if(!((InstCall&)instruction).callee) {
                fail(instruction.source, "%%@ calls nothing"_v, instruction.id);
            }
            break;

        // Exactly one of the two is set: a function value carries its own environment convention and
        // a bare code address has none - see InstCallDyn.
        case Value::CallDyn: {
            auto& call = (InstCallDyn&)instruction;

            if(bool(call.callable) == bool(call.address)) {
                fail(instruction.source, "%%@ calls through both a value and an address, or neither"_v,
                     instruction.id);
            }
            break;
        }

        case Value::GenCall:
            verifyGenCall((InstGenCall&)instruction);
            break;

        // Exactly one of the two is set, for the same reason: the instruction is one operation - an
        // address the module decides - over two kinds of thing that have one.
        case Value::Symbol: {
            auto& symbol = (InstSymbol&)instruction;

            if(bool(symbol.callee) == bool(symbol.global)) {
                fail(instruction.source, "%%@ names both a function and a global, or neither"_v,
                     instruction.id);
            }
            break;
        }

        // Both arms are evaluated, which is what the instruction means, so neither may be absent.
        case Value::Select: {
            auto& select = (InstSelect&)instruction;

            if(!select.cond || !select.whenTrue || !select.whenFalse) {
                fail(instruction.source, "%%@ selects between values that are not both there"_v,
                     instruction.id);
            }
            break;
        }

        /*
         * The two floating-point operations, whose one rule is that they are floating-point.
         *
         * Checked here rather than left to the lower validator because this is where the *type* is
         * still a language type: what reaches the lower IR is a lane kind, so a square root of an
         * integer arrives there as "a square root of an i32" with nothing left to say which
         * declaration asked for it.
         */
        /*
         * The magnitude, whose two rules are what the instruction is for - see inst.def.
         *
         * A *vector*, because nothing produces a scalar one: the library's `abs` is a vector
         * intrinsic, and a rule admitting a shape no program can write is one no test exercises.
         * Lanes that are floats or **signed** integers, an unsigned lane being its own magnitude and
         * answered by `emitAbs` without an instruction at all.
         *
         * Asked here rather than left to the lower validator for the reason the two above are: this
         * is where the type is still a language type. What reaches the lower IR is a lane kind, and
         * an `i32` lane there has already forgotten whether it was declared signed.
         */
        case Value::Abs: {
            auto lane = vectorLane(global, instruction.type);
            auto element = lane ? global[lane] : nullptr;

            if(!element) {
                fail(instruction.source, "%%@ takes the magnitude of a value that is not a vector"_v,
                     instruction.id);
            } else if(element->kind == Type::Int) {
                if(!((IntType*)element)->isSigned) {
                    fail(instruction.source, "%%@ takes the magnitude of an unsigned lane, which is itself"_v,
                         instruction.id);
                }
            } else if(element->kind != Type::Float) {
                fail(instruction.source, "%%@ takes the magnitude of a lane that is neither a number nor a float"_v,
                     instruction.id);
            }

            break;
        }

        /*
         * The byte reversal, whose two rules are the two things the operation has to be able to
         * name: which bytes there are, and that they are the whole of the value.
         *
         * A scalar integer of 16, 32 or 64 bits. A vector is refused because a lane-wise reversal is
         * a shuffle against a pattern rather than this instruction widened; a `@bits` refinement is
         * refused because its declared width is not a whole number of bytes of anything - `WideInt`
         * occupies 53 bits of a 64-bit register, and neither reading is what a caller means.
         *
         * Asked here for the reason the two rules below are: this is the last stage where the type
         * is a language type. `lowerCalc` spends the 16-bit case on the way out, so what reaches the
         * lower validator is an `Int32` or an `Int64` with nothing left to say which width the
         * program wrote.
         */
        case Value::ByteSwap: {
            auto type = instruction.type ? global[instruction.type] : nullptr;

            if(!type || type->kind != Type::Int || vectorLanes(global, instruction.type)) {
                fail(instruction.source, "%%@ reverses the bytes of a value that is not a scalar integer"_v,
                     instruction.id);
            } else {
                auto& integer = *(IntType*)type;

                if(integer.canonical || (integer.bits != 16 && integer.bits != 32 && integer.bits != 64)) {
                    fail(instruction.source, "%%@ reverses the bytes of a %@-bit integer, which is not a whole number of them"_v,
                         instruction.id, integer.bits);
                }
            }

            break;
        }

        /*
         * The three bit counts, whose two rules are the byte reversal's two rules at a different
         * width set - see the note in inst.def, which is where the set is argued.
         *
         * A scalar integer of 32 or 64 bits. A vector is refused because a per-lane population count
         * is a nibble-table shuffle rather than this instruction widened; a narrower width is
         * refused because the lower IR has no scalar below `Int32` to hold one, so the answer would
         * be its storage's rather than its own; a `@bits` refinement is refused because a
         * leading-zero count is a question about a width `WideInt` does not have.
         */
        case Value::CountBits:
        case Value::LeadingZeros:
        case Value::TrailingZeros: {
            auto type = instruction.type ? global[instruction.type] : nullptr;

            if(!type || type->kind != Type::Int || vectorLanes(global, instruction.type)) {
                fail(instruction.source, "%%@ counts the bits of a value that is not a scalar integer"_v,
                     instruction.id);
            } else {
                auto& integer = *(IntType*)type;

                if(integer.bits != 32 && integer.bits != 64) {
                    fail(instruction.source, "%%@ counts the bits of a %@-bit integer, which is not a width this operation has"_v,
                         instruction.id, integer.bits);
                }
            }

            break;
        }

        case Value::Sqrt:
        case Value::Trunc:
        case Value::Floor:
        case Value::Ceil:
        case Value::Round:
        case Value::Fma: {
            auto lane = vectorLane(global, instruction.type);
            auto element = lane ? lane : instruction.type;

            if(!element || global[element]->kind != Type::Float) {
                fail(instruction.source, "%%@ is a floating-point operation over a type that is not one"_v,
                     instruction.id);
            }

            if(instruction.kind == Value::Fma) {
                auto& fma = (InstFma&)instruction;

                if(!fma.a || !fma.b || !fma.c) {
                    fail(instruction.source, "%%@ multiplies and adds values that are not all there"_v,
                         instruction.id);
                }
            }

            break;
        }

        /*
         * A `Cast` between two vectors has to keep the lane count - Implementation-Vector.md §3.2's
         * first consequence, and the one rule in this stage that is easy to get wrong silently.
         *
         * Nothing else distinguishes the two things such a cast could mean: `f32x8` to `f64x8` is a
         * widening of every lane, and `f32x8` to `f64x4` is `unpackLow`. They differ in the result
         * type alone, and only one of them is a `Cast` - the other is a `VecShuffle` and a `Cast`,
         * which is why no instruction changes a lane count.
         */
        case Value::Cast:
        case Value::Bitcast: {
            auto& unary = (InstUnary&)instruction;
            auto from = unary.from ? local[unary.from]->type : nullptr;
            auto to = instruction.type;

            auto fromLanes = vectorLanes(global, from);
            auto toLanes = vectorLanes(global, to);

            if(instruction.kind == Value::Cast && fromLanes && toLanes && fromLanes != toLanes) {
                fail(instruction.source, "%%@ casts %@ lanes to %@ - a conversion that changes the lane count is a shuffle and a cast, not a cast"_v,
                     instruction.id, fromLanes, toLanes);
            }

            // One end a vector and the other a scalar is neither conversion: a lane's worth of a
            // vector is `VecLane`, and a whole vector out of a scalar is `VecSplat`.
            if(bool(fromLanes) != bool(toLanes) && from && to) {
                fail(instruction.source, "%%@ converts between a vector and a scalar - read a lane with `vlane` and build one with `vsplat`"_v,
                     instruction.id);
            }
            break;
        }

        /*
         * A comparison of vectors answers a mask of the same shape - Design-Vector §2.4, and §3.1's
         * "a typing rule rather than a new instruction". A `Bool` here would be an `all` or an `any`
         * that nobody chose.
         */
        case Value::Cmp: {
            auto& compare = (InstCmp&)instruction;
            auto lhs = compare.lhs ? local[compare.lhs]->type : nullptr;

            if(auto lanes = vectorLanes(global, lhs)) {
                if(!isMaskType(global, instruction.type) || vectorLanes(global, instruction.type) != lanes) {
                    fail(instruction.source, "%%@ compares %@ lanes and does not answer a mask of that shape"_v,
                         instruction.id, lanes);
                }
            }
            break;
        }

        case Value::VecSplat: {
            auto& splat = (InstVecSplat&)instruction;

            if(!splat.from) fail(instruction.source, "%%@ splats nothing"_v, instruction.id);
            if(!vectorLanes(global, instruction.type)) {
                fail(instruction.source, "%%@ splats into something that is not a vector"_v, instruction.id);
            }
            break;
        }

        case Value::VecLane:
        case Value::VecWithLane: {
            auto& lane = (InstVecLane&)instruction;
            auto reading = instruction.kind == Value::VecLane;

            // The vector is the operand for a read and the result for a write, so the lane index is
            // bounded by whichever of the two is the vector.
            auto vector = reading
                ? (lane.from ? local[lane.from]->type : nullptr) : instruction.type;
            auto lanes = vectorLanes(global, vector);

            if(!lanes) {
                fail(instruction.source, "%%@ names a lane of something that is not a vector"_v,
                     instruction.id);
            } else if(lane.lane >= lanes) {
                fail(instruction.source, "%%@ names lane %@ of a vector with %@ of them"_v,
                     instruction.id, U32(lane.lane), lanes);
            }

            if(reading == bool(lane.value)) {
                fail(instruction.source, "%%@ reads a lane and writes one, or does neither"_v,
                     instruction.id);
            }
            break;
        }

        case Value::VecShuffle: {
            auto& shuffle = (InstVecShuffle&)instruction;
            auto lanes = vectorLanes(global, instruction.type);
            auto sources = vectorLanes(global, shuffle.left ? local[shuffle.left]->type : nullptr);

            if(!lanes || !sources) {
                fail(instruction.source, "%%@ shuffles something that is not a vector"_v, instruction.id);
                break;
            }

            // One entry per lane of the *result*, each naming a lane of the two sources
            // concatenated - see InstVecShuffle, which is where the numbering is stated.
            if(shuffle.pattern.size() != lanes) {
                fail(instruction.source, "%%@ shuffles into %@ lanes with a pattern of %@"_v,
                     instruction.id, lanes, U32(shuffle.pattern.size()));
            }

            for(auto entry: shuffle.pattern) {
                if(entry < sources * 2) continue;

                fail(instruction.source, "%%@ shuffles in lane %@, and its two sources have %@ between them"_v,
                     instruction.id, U32(entry), sources * 2);
                break;
            }
            break;
        }

        case Value::VecReduce: {
            auto& reduce = (InstVecReduce&)instruction;

            if(!vectorLanes(global, reduce.from ? local[reduce.from]->type : nullptr)) {
                fail(instruction.source, "%%@ reduces something that is not a vector"_v, instruction.id);
            }
            break;
        }

        default:
            break;
    }
}

void Verifier::run() {
    computeDominators();

    verifyBlockStructure();
    collectDefinitions();
    verifyControlFlow();
    verifyPhis();

    // The operand walk fills `operandCounts`, which is what the use lists are then compared against.
    verifyOperands();
    verifyUseLists();

    verifyLocals();
    verifyArgs();
}

/*
 * Whether this function has a body to be consistent about.
 *
 * A class signature never will have one, an intrinsic's body is generated at each call site, and
 * `addFunction` gives every function an entry block before anything is resolved into it - so an
 * empty entry block is a declaration whose body has not been built rather than a function whose
 * control flow is broken.
 */
bool hasBody(ModuleBase base, Function& function) {
    if(function.signature || function.intrinsic || function.deferredIntrinsic) return false;
    if(function.resolving || function.blocks.isEmpty()) return false;

    return base[function.blocks.get(base, 0)]->terminator() != nullptr;
}

} // namespace

bool verifyFunction(Module& module, Function& function, VerifyStage stage, StringView where) {
    if(!hasBody(*module.arena, function)) return true;

    Verifier verifier(module, function, stage, where);
    verifier.run();
    return verifier.ok;
}

bool verifyProgram(Program& program, VerifyStage stage, StringView where) {
    // A program the resolver rejected has an IR built out of error types and half-resolved bodies,
    // and every finding about it would be a consequence of the diagnostic that was already reported.
    // Nothing here is ever a statement about the input, so there is nothing to say about a bad one.
    if(program.context.diagnostics.errorCount()) return true;

    auto base = *program.arena;
    auto ok = true;

    for(auto module: program.modules) {
        for(auto pointer: module->functionOrder.contents(base)) {
            ok = verifyFunction(*module, *base[pointer], stage, where) && ok;
        }
    }

    return ok;
}
