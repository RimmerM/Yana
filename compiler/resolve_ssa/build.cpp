#include "build.h"

namespace ssa {

// The body and its entry block, made together, for the reason the MIR builder does the same: a
// function with nowhere to start is not a state this should be able to be in for one statement.
static Body& createBody(SsaArena& arena, mir::Function& function) {
    auto body = new (arena) Body();

    // The body's own continuation region, made before the entry block, because a block has to name
    // one - see `Continuation`. It is the root and therefore has no parent.
    auto root = new (arena) Continuation(ContinuationId(0), Continuation::Body, Continuation::Once, ContinuationId());
    body->regions.push(arena, root - *arena);

    auto entry = new (arena) Block(BlockId(0), StringId(), root->id);
    body->entry = entry - *arena;
    body->blocks.push(arena, entry - *arena);

    // The root region's entry is the body's, set here rather than left for `addBlock` - which would
    // otherwise claim the first block made *after* this one, since this block is built directly.
    // Nothing enters the root by an edge, so the field is unread there and is filled anyway: an
    // "entry" that named the wrong block is worse than one nobody asks for.
    root->entry = entry->index;

    function.ssa = body - *arena;
    return *body;
}

Editor::Editor(Context& context, mir::Program& program, SsaArena& arena, mir::Function& function):
    context(context), program(program), arena(arena), function(function),
    body(createBody(arena, function))
{
    current = base()[body.entry];
    currentRegion = ContinuationId(0);
}

Editor::Editor(Context& context, mir::Program& program, SsaArena& arena, mir::Function& function,
               Body& body, bool* changed):
    context(context), program(program), arena(arena), function(function), body(body),
    changed(changed)
{
    // No insertion point until `setBlock` names one - see the note on the declaration. The region is
    // the entry's, because `addBlock` needs one and there is no "no region" a block may be in.
    current = nullptr;
    currentRegion = base()[body.entry]->region;
}

Block* Editor::addBlock(StringId name) {
    auto index = BlockId(body.blocks.size());
    auto block = new (arena) Block(index, name, currentRegion);
    body.blocks.push(arena, block - base());
    markBlocks();

    // The first block made inside a region is its entry - see `Continuation::entry`. Here rather
    // than an argument to `enterRegion`, because the block does not exist yet when the region is
    // opened and a caller that had to make one first would be making it in the wrong region.
    auto region = body.region(base(), currentRegion);
    if(region && !region->entry.valid()) region->entry = index;

    return block;
}

ContinuationId Editor::enterRegion(Continuation::Kind kind, Continuation::Multiplicity multiplicity) {
    auto id = ContinuationId(U32(body.regions.size()));
    auto region = new (arena) Continuation(id, kind, multiplicity, currentRegion);
    body.regions.push(arena, region - base());

    currentRegion = id;
    return id;
}

void Editor::rejoinRegion(ContinuationId id, Block& destination) {
    auto region = body.region(base(), id);
    assertTrue(region && region->parent.valid());

    // The destination is in the enclosing region and not in this one, which is what makes it an
    // exit. Asserted rather than checked, because the caller made both and knows.
    assertTrue(destination.region != id);
    region->destination = destination.index;
}

void Editor::leaveRegion() {
    auto region = body.region(base(), currentRegion);
    assertTrue(region && region->parent.valid());
    currentRegion = region->parent;
}

Value* Editor::addBlockArgument(Block& block, const Occurrence& occurrence, Convention convention) {
    assertTrue(block.instructionList.size() == 0);

    auto value = new (arena) Value(occurrence, convention);
    value->id = ValueId(body.values.push(arena, value - base()));
    value->block = &block - base();
    block.argumentList.push(arena, value - base());
    markValues();

    /*
     * A borrowed parameter's loan is the caller's, and so is a borrowed block argument's.
     *
     * The entry's is that by definition - the value arrived already borrowed. A *merge* of two
     * borrows is the case that is not, and it does not occur: a block argument is written by every
     * predecessor's edge, so the loan of one would have to be the join of the loans on those edges,
     * and this conversion never passes a `Guaranteed` value along an edge. When it does - a lens
     * frame, a yielded borrow - the join belongs here, over the incoming arguments, which is why
     * this is a statement about block arguments rather than about the entry block.
     */
    if(convention == Convention::Guaranteed) {
        value->loan = loanOn(kNullLocation, nullptr, 0, occurrence.contract.access()) - base();
    }

    return value;
}

Loan* Editor::loanOn(LocationId source, const SsaPtr<Value>* storage, Size count,
                     Access capability) {
    // A linear scan, because a body holds a handful of loans and a map would be a heap allocation
    // per body for a table that is read once per borrow.
    for(auto pointer: body.loans.contents(base())) {
        auto loan = base()[pointer];
        if(loan->capability != capability || loan->storage.size() != count) continue;

        auto same = true;
        for(Size i = 0; i < count; i++) {
            if(loan->storage.get(base(), i) != storage[i]) { same = false; break; }
        }

        if(same) return loan;
    }

    auto loan = new (arena) Loan(capability, source);
    for(Size i = 0; i < count; i++) loan->storage.push(arena, storage[i]);

    loan->id = LoanId(body.loans.push(arena, loan - base()));
    return loan;
}

void Editor::setDependency(Value& value, Loan& loan) {
    assertTrue(value.convention == Convention::Guaranteed);
    value.loan = &loan - base();
}

void Editor::setBlock(Block& block) {
    current = &block;
    cursor = kAppend;
}

void Editor::setInsertBefore(Inst& before) {
    auto block = base()[before.block];
    assertTrue(block != nullptr && !block->deleted);

    auto pointer = &before - base();
    for(Size i = 0; i < block->instructionList.size(); i++) {
        if(block->instructionList.get(base(), i) != pointer) continue;

        current = block;
        cursor = i;
        return;
    }

    // An instruction that is not in the block it names is one an erase has already unlinked, and
    // inserting in front of it is a caller that has lost track of what it removed.
    assertTrue(false);
}

Value* Editor::addResult(Inst& inst, const Occurrence& occurrence, Convention convention) {
    auto value = new (arena) Value(occurrence, convention);
    value->id = ValueId(body.values.push(arena, value - base()));
    value->definer = &inst - base();
    inst.results.push(arena, value - base());

    /*
     * Every borrowed value names what it borrowed from, and no other convention does - see
     * `Value::loan`. Here rather than at each `add*`, so that a kind added later cannot forget it.
     *
     * The caller's loan is the default, and the default is deliberately the *permissive* one: it
     * never ends inside this body, so a value nothing refines is not reported against. The
     * alternative - guessing a lender from the operands - is a verifier reporting on IR that is
     * fine, which is worse than one that misses a case: a call's result is rooted in whichever
     * argument its signature put the result's slot in, and the first address operand is simply not
     * that. `Conversion::attachLoan` is what knows, because the origin environment is what answers.
     */
    if(convention == Convention::Guaranteed) {
        value->loan = loanOn(inst.source, nullptr, 0, occurrence.contract.access()) - base();
    }

    return value;
}

Value* Editor::resultOf(Inst& inst, Size index) {
    assertTrue(index < inst.results.size());
    return base()[inst.results.get(base(), index)];
}

Use Editor::use(Value& value, UseKind kind) {
    // A tracked value - `Owned` or `Affine` - may only be named by a consuming or borrowing use;
    // inspecting one would be a use with no obligation attached, which is the shape §7.1's invariant
    // exists to forbid.
    if(isTracked(value.convention)) assertTrue(kind != UseKind::Inspect);

    return Use(&value - base(), kind);
}

void Editor::recordUse(Inst& inst, Use used) {
    if(!used.isSet()) return;
    base()[used.value()]->useList.push(arena, &inst - base());
}

Inst* Editor::append(Inst* inst) {
    assertTrue(current != nullptr);
    assertTrue(!current->deleted);

    inst->id = InstId(body.insts.push(arena, inst - base()));

    if(cursor == kAppend) {
        auto count = current->instructionList.size();
        if(count) {
            auto last = base()[current->instructionList.get(base(), count - 1)];
            assertTrue(!isTerminator(last->kind));
        }

        inst->block = current - base();
        current->instructionList.push(arena, inst - base());
    } else {
        // Behind whatever was placed here last, so that a caller adding two instructions gets them
        // in the order it wrote them - see `setInsertBefore`.
        insertInst(*current, cursor, *inst);
        cursor++;
    }

    markValues();

    // The use lists, updated in the same call that made the instruction - which is the whole reason
    // instructions are created through this and not with `new`.
    forEachUse(base(), *inst, [&](Use used) { recordUse(*inst, used); });
    forEachEdge(*inst, [&](Edge& edge) {
        for(auto used: edge.arguments.contents(base())) recordUse(*inst, used);
    });

    return inst;
}

Inst* Editor::addConst(LocationId source, const mir::Constant& constant) {
    auto inst = new (arena) InstConst(source, constant);
    append(inst);

    // A constant is trivial by construction: it is a value with static contents and no teardown.
    // A constant whose *type* is not trivial - a string owner - is a different instruction, because
    // what it produces is an owner and the difference has to be in the IR rather than in a comment.
    assertTrue(!mir::isLinear(declBase(), constant.type));
    addResult(*inst, mir::owned(constant.type), Convention::Trivial);
    return inst;
}

Inst* Editor::addGlobalAddress(LocationId source, DeclPtr<mir::Global> global) {
    auto inst = new (arena) InstGlobalAddress(source, global);
    append(inst);

    /*
     * The address of a global, as an owned occurrence of a pointer type.
     *
     * Owned because the *address* is a value this function holds outright; what it points at is the
     * global's, and the contract on the load that reads it is what says so. Conflating the two is
     * how a pointer-loaded temporary came to look like a freshly owned aggregate in the current
     * resolver - see `Convention::Address`.
     */
    auto declaration = declBase()[global];
    auto type = program.types.pointerType(declaration->occurrence.type);
    addResult(*inst, mir::owned(type), Convention::Address);
    return inst;
}

/*
 * A const parameter read out of the environment - `InstGenValue`.
 *
 * Trivial and owned, exactly as the `const` a specialization emits for the same node: what a const
 * parameter holds is a number, and where the number came from is not a fact about the value.
 */
Inst* Editor::addGenValue(LocationId source, DeclPtr<mir::Type> variable, DeclPtr<mir::Type> type) {
    auto inst = new (arena) InstGenValue(source, variable);
    append(inst);
    addResult(*inst, mir::owned(type), Convention::Trivial);
    return inst;
}

Inst* Editor::addMetric(LocationId source, mir::TypeMetric metric, DeclPtr<mir::Type> of,
                        DeclPtr<mir::Type> as) {
    auto inst = new (arena) InstMetric(source, metric, of);
    append(inst);
    addResult(*inst, mir::owned(as), Convention::Trivial);
    return inst;
}

Inst* Editor::addAlloc(LocationId source, const Occurrence& held, StorageClass storage) {
    auto inst = new (arena) InstAlloc(source, held, storage);
    append(inst);
    addResult(*inst, mir::owned(program.types.pointerType(held.type)), Convention::Address);
    return inst;
}

/*
 * The address of a field, as an `Address` value of a pointer to what is there.
 *
 * The same shape `addAlloc` produces and for the same reason: an address is a value this function
 * holds outright, and what it points at belongs to whoever owns the object - so conflating the two
 * would make a projection of a borrowed record look like a fresh owner of its field.
 */
Inst* Editor::addFieldAddress(LocationId source, Use base, DeclPtr<mir::Type> record, U16 field,
                              DeclPtr<mir::OwnershipSchema> shape) {
    auto declared = (mir::RecordType*)declBase()[record];
    assertTrue(declared->kind == mir::Type::Record);

    /*
     * The **discriminant** is a component the field list does not hold, and its type is the tag's -
     * see `InstFieldAddress::kDiscriminant`. Everything else about the instruction is the same, which
     * is why the two are one row.
     */
    auto held = field == InstFieldAddress::kDiscriminant
        ? mir::tagTypeOf(declBase(), program.types, record)
        : declared->field(declBase(), field).occurrence.type;

    assertTrue(held);
    assertTrue(field == InstFieldAddress::kDiscriminant || field < declared->fields.size());

    auto inst = new (arena) InstFieldAddress(source, base, record, field, shape);
    append(inst);

    addResult(*inst, mir::owned(program.types.pointerType(held)), Convention::Address);
    return inst;
}

/*
 * The address of one element, as an `Address` value of a pointer to the element type.
 *
 * The same shape `addFieldAddress` produces, and the same reason: an address is a value this
 * function holds outright and what it points at belongs to whoever owns the container.
 */
Inst* Editor::addElementAddress(LocationId source, Use base, Use index,
                                DeclPtr<mir::Type> container) {
    auto element = mir::elementTypeOf(declBase(), program, container);
    assertTrue(element != nullptr);

    auto inst = new (arena) InstElementAddress(source, base, index, container);
    append(inst);

    addResult(*inst, mir::owned(program.types.pointerType(element)), Convention::Address);
    return inst;
}

/*
 * The same projection, where the subject is a type parameter and the offset is a property slot.
 *
 * The result type is the one the *requirement* promised, which is the whole of what a structural
 * field requirement states: `a.size: I32` says the offset is somebody else's answer and the type at
 * it is `I32`. So the value below this instruction is typed exactly as a concrete projection's is,
 * and only where the number comes from differs.
 */
Inst* Editor::addPropertyAddress(LocationId source, Use base, U16 property,
                                 DeclPtr<mir::Type> held) {
    auto inst = new (arena) InstFieldAddress(source, base, nullptr, 0, nullptr);
    inst->property = property;
    append(inst);

    addResult(*inst, mir::owned(program.types.pointerType(held)), Convention::Address);
    return inst;
}

Inst* Editor::addCopy(LocationId source, Use address, const Occurrence& result,
                      Convention convention) {
    // The evidence `UseMode::CopyTrivial` claimed, restated where the instruction is made. A type
    // with a teardown has no bitwise duplicate: two owners of one object release it twice. Reported
    // with the position before the assertion, because a bare assertion names no line of source.
    if(!mir::isTriviallyCopyableAt(declBase(), result)) {
        context.diagnostics.error("internal: a copy of %@, which is not trivially copyable"_v,
                                  source, mir::typeText(context, declBase(), result.type));
    }

    assertTrue(mir::isTriviallyCopyableAt(declBase(), result));

    auto inst = new (arena) InstCopy(source, address);
    append(inst);
    addResult(*inst, result, convention);
    return inst;
}

Inst* Editor::addAggregate(LocationId source, U16 constructor, const Use* components, Size count,
                           const Occurrence& result, Convention convention) {
    auto inst = new (arena) InstAggregate(source);
    inst->constructor = constructor;
    inst->components.reserve(arena, count);
    for(Size i = 0; i < count; i++) inst->components.push(arena, components[i]);

    append(inst);
    addResult(*inst, result, convention);
    return inst;
}

Inst* Editor::addDrop(LocationId source, Use address, const Occurrence& held, bool boxesVisible) {
    // A drop of something with nothing to run is not an instruction with no effect - it is a
    // placement decision that should not have been made. `hasTeardown` is what the elaboration asked
    // before it placed one, and this is the same question restated where the instruction is built.
    assertTrue(mir::hasTeardownAt(declBase(), held));
    return append(new (arena) InstDrop(source, address, held, boxesVisible));
}

Inst* Editor::addStore(LocationId source, Use address, Use value) {
    return append(new (arena) InstStore(source, address, value));
}

Inst* Editor::addBinary(LocationId source, BinaryOp op, Use lhs, Use rhs,
                        const Occurrence& result, Convention convention) {
    auto inst = new (arena) InstBinary(source, op, lhs, rhs);
    append(inst);
    addResult(*inst, result, convention);
    return inst;
}

Inst* Editor::addUnary(LocationId source, UnaryOp op, Use value,
                       const Occurrence& result, Convention convention) {
    auto inst = new (arena) InstUnary(source, op, value);
    append(inst);
    addResult(*inst, result, convention);
    return inst;
}

Inst* Editor::addLoad(LocationId source, Use address, const Occurrence& result,
                      Convention convention) {
    auto inst = new (arena) InstLoad(source, address);
    append(inst);
    addResult(*inst, result, convention);
    return inst;
}

Inst* Editor::addFunValue(LocationId source, DeclPtr<mir::Function> callee, const Use* environment,
                          const Occurrence& result, Convention convention) {
    auto inst = new (arena) InstFunValue(source, callee);

    if(environment) {
        inst->capturing = true;
        inst->environment = *environment;
    }

    append(inst);
    addResult(*inst, result, convention);
    return inst;
}

Inst* Editor::addCall(LocationId source, const mir::CallSite& site, Use target, const Use* arguments,
                      Size count, const Occurrence& result, Convention convention, bool hasResult) {
    auto inst = new (arena) InstCall(source, site.target);
    inst->through = site.through;
    inst->target = target;
    inst->requirement = site.requirement;
    inst->member = site.member;
    inst->environment = site.environment;
    inst->arguments.reserve(arena, count);
    for(Size i = 0; i < count; i++) inst->arguments.push(arena, arguments[i]);

    append(inst);
    if(hasResult) addResult(*inst, result, convention);
    return inst;
}

Inst* Editor::addNative(LocationId source, mir::NativeOp op, const Use* arguments, Size count,
                        const Occurrence& result, Convention convention, bool hasResult) {
    auto inst = new (arena) InstNative(source, op);
    inst->arguments.reserve(arena, count);
    for(Size i = 0; i < count; i++) inst->arguments.push(arena, arguments[i]);

    append(inst);
    if(hasResult) addResult(*inst, result, convention);
    return inst;
}

Inst* Editor::addAtomic(LocationId source, mir::AtomicOp op, mir::AtomicOrder order,
                        mir::AtomicOrder failure, bool weak, const Use* arguments, Size count,
                        const Occurrence& result, Convention convention, bool hasResult) {
    auto inst = new (arena) InstAtomic(source, op, order, failure, weak);
    inst->arguments.reserve(arena, count);
    for(Size i = 0; i < count; i++) inst->arguments.push(arena, arguments[i]);

    append(inst);
    if(hasResult) addResult(*inst, result, convention);
    return inst;
}

Inst* Editor::addAtomicOk(LocationId source, Use cas, const Occurrence& result) {
    auto inst = new (arena) InstAtomicOk(source, cas);

    append(inst);

    // Trivial, and it is worth saying rather than leaving to `conventionFor`: what comes back is a
    // `Bool`, which owns nothing and releases nothing whatever the atomic's content is.
    addResult(*inst, result, Convention::Trivial);
    return inst;
}

Inst* Editor::addFma(LocationId source, Use lhs, Use rhs, Use addend, const Occurrence& result) {
    auto inst = new (arena) InstFma(source, lhs, rhs, addend);
    append(inst);
    addResult(*inst, result, Convention::Trivial);
    return inst;
}

Inst* Editor::addSelect(LocationId source, Use condition, Use whenTrue, Use whenFalse,
                        const Occurrence& result, Convention convention) {
    auto inst = new (arena) InstSelect(source, condition, whenTrue, whenFalse);
    append(inst);
    addResult(*inst, result, convention);
    return inst;
}

// The four are in build.h, beside their declarations.
Inst* Editor::addVecSplat(LocationId source, Use from, const Occurrence& result) {
    auto inst = new (arena) InstVecSplat(source, from);
    append(inst);
    addResult(*inst, result, Convention::Trivial);
    return inst;
}

Inst* Editor::addVecLane(LocationId source, Use from, U16 lane, const Occurrence& result) {
    auto inst = new (arena) InstVecLane(Inst::VecLane, source, from, lane);
    append(inst);
    addResult(*inst, result, Convention::Trivial);
    return inst;
}

Inst* Editor::addVecWithLane(LocationId source, Use from, U16 lane, Use value,
                             const Occurrence& result) {
    auto inst = new (arena) InstVecLane(Inst::VecWithLane, source, from, lane);
    inst->value = value;

    append(inst);
    addResult(*inst, result, Convention::Trivial);
    return inst;
}

Inst* Editor::addVecShuffle(LocationId source, Use left, Use right, const U8* pattern, Size count,
                            const Occurrence& result) {
    auto inst = new (arena) InstVecShuffle(source, left, right);
    inst->pattern.reserve(arena, count);
    for(Size i = 0; i < count; i++) inst->pattern.push(arena, pattern[i]);

    append(inst);
    addResult(*inst, result, Convention::Trivial);
    return inst;
}

Inst* Editor::addVecReduce(LocationId source, Use from, mir::ReduceOp reduce,
                           const Occurrence& result) {
    auto inst = new (arena) InstVecReduce(source, from, reduce);
    append(inst);
    addResult(*inst, result, Convention::Trivial);
    return inst;
}

Inst* Editor::makeReturn(LocationId source, const Use* values, Size count) {
    auto inst = new (arena) InstReturn(source);
    for(Size i = 0; i < count; i++) inst->values.push(arena, values[i]);
    return inst;
}

Inst* Editor::makeJump(LocationId source, Block& target, const Use* arguments, Size count) {
    assertTrue(count == target.argumentList.size());

    auto inst = new (arena) InstJump(source);
    inst->to.target = &target - base();
    for(Size i = 0; i < count; i++) inst->to.arguments.push(arena, arguments[i]);

    return inst;
}

Inst* Editor::makeBranch(LocationId source, Use condition, Block& whenTrue, Block& whenFalse,
                         const Use* arguments, Size count) {
    assertTrue(count == whenTrue.argumentList.size() && count == whenFalse.argumentList.size());

    auto inst = new (arena) InstBranch(source, condition);
    inst->whenTrue.target = &whenTrue - base();
    inst->whenFalse.target = &whenFalse - base();

    for(Size i = 0; i < count; i++) {
        inst->whenTrue.arguments.push(arena, arguments[i]);
        inst->whenFalse.arguments.push(arena, arguments[i]);
    }

    return inst;
}

Inst* Editor::makeUnreachable(LocationId source) {
    return new (arena) InstUnreachable(source);
}

Inst* Editor::addReturn(LocationId source, const Use* values, Size count) {
    return append(makeReturn(source, values, count));
}

Inst* Editor::addJump(LocationId source, Block& target, const Use* arguments, Size count) {
    return append(makeJump(source, target, arguments, count));
}

Inst* Editor::addBranch(LocationId source, Use condition, Block& whenTrue, Block& whenFalse,
                        const Use* arguments, Size count) {
    return append(makeBranch(source, condition, whenTrue, whenFalse, arguments, count));
}

Inst* Editor::addUnreachable(LocationId source) {
    return append(makeUnreachable(source));
}

void Editor::seal() {
    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(base(), BlockId(i));
        auto last = block->terminator(base());
        if(!last) continue;

        forEachEdge(*last, [&](Edge& edge) {
            if(!edge.target) return;
            base()[edge.target]->incomingList.push(arena, block - base());
        });
    }

    markBlocks();
}

/*
 * Every operand slot of one instruction, rewritten - the mutating half of `forEachUse`.
 *
 * A switch of its own rather than a flag on that walk, because the two shapes of operand are written
 * back differently and only one of them can be handed out by reference at all. A plain `Use` member
 * - `InstBinary::lhs` - is a field and `eachUse` passes it as one; an entry of a `UseList` is not,
 * because `contents` unmasks into a copy (`util/README.md`'s trap), so a rewrite driven from that
 * walk would assign a temporary and say nothing.
 *
 * Being a *second* statement of the operand relation is a property rather than a cost, and it is the
 * same one the old stage's `recordUses` is a switch for: `verifyBody` checks the use lists against
 * `forEachUse`, so a slot this walk invents or misses is a use count that never balances.
 */
template<class F>
static void mapUses(SsaBase base, Inst& inst, F&& f) {
    auto one = [&](Use& used) { if(used.isSet()) used = f(used); };
    auto many = [&](UseList& list) {
        for(U32 i = 0; i < list.size(); i++) {
            auto used = list.get(base, i);
            if(used.isSet()) list.set(base, i, f(used));
        }
    };

    switch(inst.kind) {
        case Inst::Const:
        case Inst::GlobalAddress:
        case Inst::GenValue:
        case Inst::Metric:
        case Inst::Alloc:
        case Inst::Unreachable:
            break;

        case Inst::Native:   many(((InstNative&)inst).arguments); break;
        case Inst::Atomic:   many(((InstAtomic&)inst).arguments); break;
        case Inst::AtomicOk: one(((InstAtomicOk&)inst).cas); break;

        case Inst::Fma: {
            auto& written = (InstFma&)inst;
            one(written.lhs); one(written.rhs); one(written.addend);
            break;
        }

        case Inst::Select: {
            auto& written = (InstSelect&)inst;
            one(written.condition); one(written.whenTrue); one(written.whenFalse);
            break;
        }

        case Inst::VecSplat: one(((InstVecSplat&)inst).from); break;
        case Inst::VecLane:  one(((InstVecLane&)inst).from); break;

        case Inst::VecWithLane: {
            auto& written = (InstVecLane&)inst;
            one(written.from); one(written.value);
            break;
        }

        case Inst::VecShuffle: {
            auto& written = (InstVecShuffle&)inst;
            one(written.left); one(written.right);
            break;
        }

        case Inst::VecReduce: one(((InstVecReduce&)inst).from); break;
        case Inst::FieldAddress: one(((InstFieldAddress&)inst).base); break;

        case Inst::ElementAddress: {
            auto& written = (InstElementAddress&)inst;
            one(written.base); one(written.index);
            break;
        }

        case Inst::Load: one(((InstLoad&)inst).address); break;
        case Inst::Copy: one(((InstCopy&)inst).address); break;

        case Inst::Store: {
            auto& written = (InstStore&)inst;
            one(written.address); one(written.value);
            break;
        }

        case Inst::Binary: {
            auto& written = (InstBinary&)inst;
            one(written.lhs); one(written.rhs);
            break;
        }

        case Inst::Unary: one(((InstUnary&)inst).value); break;
        case Inst::Aggregate: many(((InstAggregate&)inst).components); break;
        case Inst::Drop: one(((InstDrop&)inst).address); break;

        case Inst::FunValue: {
            auto& written = (InstFunValue&)inst;
            if(written.capturing) one(written.environment);
            break;
        }

        case Inst::Call: {
            auto& written = (InstCall&)inst;
            if(written.through == mir::CallThrough::Value) one(written.target);
            many(written.arguments);
            break;
        }

        case Inst::Return: many(((InstReturn&)inst).values); break;
        case Inst::Jump:   many(((InstJump&)inst).to.arguments); break;

        case Inst::Branch: {
            auto& written = (InstBranch&)inst;
            one(written.condition);
            many(written.whenTrue.arguments);
            many(written.whenFalse.arguments);
            break;
        }

        case Inst::KindCount: assertTrue(false); break;
    }
}

// One entry, not every entry: an instruction that names a value twice is in its use list twice and
// the list has to keep saying so.
void Editor::dropUse(Value& value, Inst& user) {
    auto pointer = &user - base();

    for(Size i = 0; i < value.useList.size(); i++) {
        if(value.useList.get(base(), i) != pointer) continue;

        value.useList.remove(base(), i);
        return;
    }

    // A use that is not in the list is a list that was already wrong, and unrecording is not where
    // that should first be noticed - the verifier is. Loud here anyway, because the alternative is
    // an editor that quietly tolerates the state it exists to prevent.
    assertTrue(false);
}

void Editor::dropUses(Inst& inst) {
    forEachUse(base(), inst, [&](Use used) {
        if(used.isSet()) dropUse(*base()[used.value()], inst);
    });

    forEachEdge(inst, [&](Edge& edge) {
        for(auto used: edge.arguments.contents(base())) {
            if(used.isSet()) dropUse(*base()[used.value()], inst);
        }
    });
}

void Editor::unlinkInst(Inst& inst) {
    auto block = base()[inst.block];
    auto pointer = &inst - base();

    for(Size i = 0; i < block->instructionList.size(); i++) {
        if(block->instructionList.get(base(), i) != pointer) continue;

        block->instructionList.remove(base(), i);
        inst.block = nullptr;
        return;
    }

    assertTrue(false);
}

// Appended and then shifted down, because the embedded and the plain list forms both have `push`,
// `set` and `remove` and neither has an insert - and a list written by hand would be one more place
// for the two directions to disagree.
void Editor::insertInst(Block& block, Size position, Inst& inst) {
    auto pointer = &inst - base();
    assertTrue(position <= block.instructionList.size());

    block.instructionList.push(arena, pointer);
    for(auto i = block.instructionList.size() - 1; i > position; i--) {
        block.instructionList.set(base(), i, block.instructionList.get(base(), i - 1));
    }

    block.instructionList.set(base(), position, pointer);
    inst.block = &block - base();
}

void Editor::dropIncoming(Block& target, Block& from) {
    auto pointer = &from - base();

    for(Size i = 0; i < target.incomingList.size(); i++) {
        if(target.incomingList.get(base(), i) != pointer) continue;

        target.incomingList.remove(base(), i);
        return;
    }

    assertTrue(false);
}

void Editor::addIncoming(Block& target, Block& from) {
    target.incomingList.push(arena, &from - base());
}

void Editor::forgetStorage(SsaPtr<Value> storage) {
    for(auto loanPointer: body.loans.contents(base())) {
        auto loan = base()[loanPointer];

        for(Size i = 0; i < loan->storage.size(); ) {
            if(loan->storage.get(base(), i) != storage) { i++; continue; }
            loan->storage.remove(base(), i);
        }
    }
}

void Editor::repointStorage(SsaPtr<Value> from, SsaPtr<Value> to) {
    for(auto loanPointer: body.loans.contents(base())) {
        auto loan = base()[loanPointer];

        for(Size i = 0; i < loan->storage.size(); ) {
            if(loan->storage.get(base(), i) != from) { i++; continue; }

            // A **set** - see `Loan::storage`. A lender that is already a member absorbs the entry
            // rather than being named twice, since the loan ends when any member is released and
            // naming one of them twice states the same fact twice.
            auto present = false;
            for(auto held: loan->storage.contents(base())) {
                if(held == to) present = true;
            }

            if(present) {
                loan->storage.remove(base(), i);
            } else {
                loan->storage.set(base(), i, to);
                i++;
            }
        }
    }
}

void Editor::eraseInst(Inst& inst) {
    // The precondition every caller that removes a live value has already satisfied by pointing its
    // readers somewhere - see `replaceAllUses`.
    for(auto resultPointer: inst.results.contents(base())) {
        assertTrue(base()[resultPointer]->useList.size() == 0);
    }

    // The edges before the uses, because a terminator's targets record one predecessor entry per
    // arm and this is where those end.
    auto from = base()[inst.block];
    forEachEdge(inst, [&](Edge& edge) {
        if(edge.target) dropIncoming(*base()[edge.target], *from);
    });

    dropUses(inst);
    unlinkInst(inst);

    for(auto resultPointer: inst.results.contents(base())) forgetStorage(resultPointer);

    if(isTerminator(inst.kind)) markBlocks(); else markValues();
    markChanged();
}

void Editor::replaceAllUses(Value& from, Value& to) {
    assertTrue(&from != &to);

    // The two rules, asserted rather than checked - the note on the declaration is where each of
    // them is argued.
    assertTrue(isTracked(from.convention) == isTracked(to.convention));

    auto borrowed = from.convention == Convention::Guaranteed;
    assertTrue(borrowed == (to.convention == Convention::Guaranteed));

    if(borrowed) {
        assertTrue(to.loan != nullptr);
        assertTrue(to.loan == from.loan || base()[to.loan]->storage.size() == 0);
    }

    auto replaced = &from - base();
    auto with = &to - base();

    while(from.useList.size()) {
        auto userPointer = from.useList.get(base(), 0);

        /*
         * Every slot this instruction reads `from` through, in one go, with the kind of each kept.
         *
         * An instruction that names the value twice has two entries, so this runs once per entry
         * and is a no-op the second time round - a handful of pointers against a loop that would
         * otherwise have to carry a set of the users it had already rewritten.
         */
        mapUses(base(), *base()[userPointer], [&](Use used) {
            return used.value() == replaced ? Use(with, used.kind()) : used;
        });

        from.useList.remove(base(), 0);
        to.useList.push(arena, userPointer);
    }

    repointStorage(replaced, with);

    markValues();
    markChanged();
}

void Editor::moveInst(Inst& inst, Block& target, Size position) {
    // A terminator moves with the edges it owns, which is `replaceTerminator` and `splitBlock`.
    assertTrue(!isTerminator(inst.kind));
    assertTrue(!target.deleted);

    unlinkInst(inst);

    // Never behind the terminator: `checkStructure` reports a terminator in the middle and a block
    // that does not end in one, and landing there produces both at once.
    auto last = target.terminator(base());
    assertTrue(position + (last ? 1 : 0) <= target.instructionList.size());

    insertInst(target, position, inst);

    markValues();
    markChanged();
}

Block* Editor::splitBlock(Block& block, Size position) {
    assertTrue(!block.deleted);
    assertTrue(position <= block.instructionList.size());

    // Built here rather than through `addBlock`, for two reasons that are both about the region: it
    // is the *split* block's and not whatever the cursor last left current, and the new block must
    // not claim the region's entry - control still enters at the half that kept the identity.
    auto index = BlockId(body.blocks.size());
    auto made = new (arena) Block(index, block.name, block.region);
    body.blocks.push(arena, made - base());

    // The tail, in order, by unlink and relink: the operands already name these instructions, so
    // nothing about a use list changes here and what does is the block each one says it is in.
    auto moved = block.instructionList.size() - position;
    for(Size i = 0; i < moved; i++) {
        auto pointer = block.instructionList.get(base(), position);
        block.instructionList.remove(base(), position);

        made->instructionList.push(arena, pointer);
        base()[pointer]->block = made - base();
    }

    // The edges went with the terminator, so every successor is told they now leave from the new
    // block - one entry per edge, which is what makes `branch %c, X, X` two repointings.
    if(auto last = made->terminator(base())) {
        forEachEdge(*last, [&](Edge& edge) {
            if(!edge.target) return;

            dropIncoming(*base()[edge.target], block);
            addIncoming(*base()[edge.target], *made);
        });
    }

    markBlocks();
    markChanged();
    return made;
}

void Editor::deleteBlock(Block& block) {
    assertTrue(!block.deleted);
    assertTrue(&block != base()[body.entry]);
    assertTrue(block.incomingList.size() == 0);

    // Last first, which is what makes each erase legal: the terminator goes before anything it
    // named, and inside a block a definition always precedes its readers.
    while(block.instructionList.size()) {
        auto count = block.instructionList.size();
        eraseInst(*base()[block.instructionList.get(base(), count - 1)]);
    }

    while(block.argumentList.size()) {
        assertTrue(base()[block.argumentList.get(base(), 0)]->useList.size() == 0);
        block.argumentList.remove(base(), 0);
    }

    block.deleted = true;

    markBlocks();
    markChanged();
}

void Editor::redirectEdge(Inst& terminator, Size arm, Block& to) {
    assertTrue(isTerminator(terminator.kind));
    assertTrue(!to.deleted);

    Edge* found = nullptr;
    Size at = 0;
    forEachEdge(terminator, [&](Edge& edge) { if(at++ == arm) found = &edge; });

    assertTrue(found != nullptr && found->target != nullptr);

    // The shape the target declares, which the edge already supplies - see the note on the
    // declaration for why a redirect to a block of a different shape is a different operation.
    assertTrue(found->arguments.size() == to.argumentList.size());

    auto from = base()[terminator.block];
    dropIncoming(*base()[found->target], *from);
    found->target = &to - base();
    addIncoming(to, *from);

    markBlocks();
    markChanged();
}

void Editor::removeBlockArgument(Block& block, Size index) {
    assertTrue(index < block.argumentList.size());
    assertTrue(base()[block.argumentList.get(base(), index)]->useList.size() == 0);

    /*
     * One removal per **edge**, with each predecessor visited once.
     *
     * A block that lands here on both arms of its branch appears twice in `incoming` and owns two
     * edges, so walking the predecessor list and then that predecessor's edges would drop four
     * operands from two lists of one.
     */
    auto target = &block - base();

    for(Size i = 0; i < block.incomingList.size(); i++) {
        auto predecessorPointer = block.incomingList.get(base(), i);

        auto seen = false;
        for(Size k = 0; k < i; k++) {
            if(block.incomingList.get(base(), k) == predecessorPointer) seen = true;
        }

        if(seen) continue;

        auto terminator = base()[predecessorPointer]->terminator(base());
        if(!terminator) continue;

        forEachEdge(*terminator, [&](Edge& edge) {
            if(edge.target != target) return;

            auto used = edge.arguments.get(base(), index);
            if(used.isSet()) dropUse(*base()[used.value()], *terminator);

            edge.arguments.remove(base(), index);
        });
    }

    block.argumentList.remove(base(), index);

    markValues();
    markChanged();
}

Inst* Editor::replaceTerminator(Block& block, Inst* terminator) {
    assertTrue(!block.deleted);
    assertTrue(terminator != nullptr && isTerminator(terminator->kind));

    // Two arms is what the kinds have today. Asserted rather than sized from the instruction,
    // because a kind with more owes this function a wider array rather than a silent truncation.
    static constexpr Size kMaxEdges = 4;

    Edge* before[kMaxEdges] = {};
    Edge* after[kMaxEdges] = {};
    bool keptOld[kMaxEdges] = {};
    bool keptNew[kMaxEdges] = {};
    Size old = 0;
    Size made = 0;

    auto existing = block.terminator(base());
    if(existing) {
        forEachEdge(*existing, [&](Edge& edge) { assertTrue(old < kMaxEdges); before[old++] = &edge; });
    }

    forEachEdge(*terminator, [&](Edge& edge) { assertTrue(made < kMaxEdges); after[made++] = &edge; });

    /*
     * The multiset match, greedy by ordinal - the one idea from the old stage's `setTerminator`
     * worth copying verbatim.
     *
     * An edge present in both keeps its `incoming` entry and the arguments it was carrying, over
     * whatever the caller supplied: folding `branch %c, then, else` into `jump then` is a fold whose
     * site has no way of knowing what `then` was being handed, and removing and re-adding the edge
     * would take those arguments and put none back.
     */
    for(Size j = 0; j < made; j++) {
        for(Size i = 0; i < old; i++) {
            if(keptOld[i] || !before[i]->target || before[i]->target != after[j]->target) continue;

            after[j]->arguments.clear();
            for(auto used: before[i]->arguments.contents(base())) {
                after[j]->arguments.push(arena, used);
            }

            keptOld[i] = true;
            keptNew[j] = true;
            break;
        }
    }

    for(Size i = 0; i < old; i++) {
        if(!keptOld[i] && before[i]->target) dropIncoming(*base()[before[i]->target], block);
    }

    if(existing) {
        assertTrue(existing->results.size() == 0);
        dropUses(*existing);
        unlinkInst(*existing);
    }

    // Through `append`, which is what records the new terminator's uses - the edge arguments above
    // included, which is why the copy happens before this and not after.
    //
    // The cursor is forced to the end as well as the block, because `append` honours whatever
    // `setInsertBefore` left there: a pass replacing a terminator with its cursor still parked
    // inside a block - its own or another's - would otherwise put the terminator in the middle
    // of the instruction list. A terminator has exactly one legal position, so the cursor is not
    // this operation's to consult.
    auto saved = current;
    auto savedCursor = cursor;
    current = &block;
    cursor = kAppend;
    append(terminator);
    current = saved;
    cursor = savedCursor;

    for(Size j = 0; j < made; j++) {
        if(!after[j]->target) continue;

        assertTrue(after[j]->arguments.size() == base()[after[j]->target]->argumentList.size());
        if(!keptNew[j]) addIncoming(*base()[after[j]->target], block);
    }

    markBlocks();
    markChanged();
    return terminator;
}

} // namespace ssa
