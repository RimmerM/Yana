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

Block* Editor::addBlock(StringId name) {
    auto index = BlockId(body.blocks.size());
    auto block = new (arena) Block(index, name, currentRegion);
    body.blocks.push(arena, block - base());

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
    assertTrue(block.instructions.size() == 0);

    auto value = new (arena) Value(occurrence, convention);
    value->id = ValueId(body.values.push(arena, value - base()));
    value->block = &block - base();
    block.arguments.push(arena, value - base());

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
    base()[used.value()]->uses.push(arena, &inst - base());
}

Inst* Editor::append(Inst* inst) {
    assertTrue(current != nullptr);

    auto count = current->instructions.size();
    if(count) {
        auto last = base()[current->instructions.get(base(), count - 1)];
        assertTrue(!isTerminator(last->kind));
    }

    inst->id = InstId(body.insts.push(arena, inst - base()));
    inst->block = current - base();
    current->instructions.push(arena, inst - base());

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
Inst* Editor::addFieldAddress(LocationId source, Use base, DeclPtr<mir::Type> record, U16 field) {
    auto declared = (mir::RecordType*)declBase()[record];
    assertTrue(declared->kind == mir::Type::Record);
    assertTrue(field < declared->fields.size());

    auto inst = new (arena) InstFieldAddress(source, base, record, field);
    append(inst);

    auto type = program.types.pointerType(declared->field(declBase(), field).type);
    addResult(*inst, mir::owned(type), Convention::Address);
    return inst;
}

Inst* Editor::addCopy(LocationId source, Use address, const Occurrence& result,
                      Convention convention) {
    // The evidence `UseMode::CopyTrivial` claimed, restated where the instruction is made. A type
    // with a teardown has no bitwise duplicate: two owners of one object release it twice.
    assertTrue(mir::isTriviallyCopyable(declBase(), result.type));

    auto inst = new (arena) InstCopy(source, address);
    append(inst);
    addResult(*inst, result, convention);
    return inst;
}

Inst* Editor::addAggregate(LocationId source, const Use* components, Size count,
                           const Occurrence& result, Convention convention) {
    auto inst = new (arena) InstAggregate(source);
    inst->components.reserve(arena, count);
    for(Size i = 0; i < count; i++) inst->components.push(arena, components[i]);

    append(inst);
    addResult(*inst, result, convention);
    return inst;
}

Inst* Editor::addDrop(LocationId source, Use address, const Occurrence& held) {
    // A drop of something with nothing to run is not an instruction with no effect - it is a
    // placement decision that should not have been made. `hasTeardown` is what the elaboration asked
    // before it placed one, and this is the same question restated where the instruction is built.
    assertTrue(mir::hasTeardown(declBase(), held.type));
    return append(new (arena) InstDrop(source, address, held));
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

Inst* Editor::addCall(LocationId source, DeclPtr<mir::Function> callee, const Use* arguments,
                      Size count, const Occurrence& result, Convention convention, bool hasResult) {
    auto inst = new (arena) InstCall(source, callee);
    inst->arguments.reserve(arena, count);
    for(Size i = 0; i < count; i++) inst->arguments.push(arena, arguments[i]);

    append(inst);
    if(hasResult) addResult(*inst, result, convention);
    return inst;
}

Inst* Editor::addReturn(LocationId source, const Use* values, Size count) {
    auto inst = new (arena) InstReturn(source);
    for(Size i = 0; i < count; i++) inst->values.push(arena, values[i]);
    return append(inst);
}

Inst* Editor::addJump(LocationId source, Block& target, const Use* arguments, Size count) {
    assertTrue(count == target.arguments.size());

    auto inst = new (arena) InstJump(source);
    inst->to.target = &target - base();
    for(Size i = 0; i < count; i++) inst->to.arguments.push(arena, arguments[i]);

    return append(inst);
}

Inst* Editor::addBranch(LocationId source, Use condition, Block& whenTrue, Block& whenFalse) {
    assertTrue(whenTrue.arguments.size() == 0 && whenFalse.arguments.size() == 0);

    auto inst = new (arena) InstBranch(source, condition);
    inst->whenTrue.target = &whenTrue - base();
    inst->whenFalse.target = &whenFalse - base();
    return append(inst);
}

Inst* Editor::addUnreachable(LocationId source) {
    return append(new (arena) InstUnreachable(source));
}

void Editor::seal() {
    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(base(), BlockId(i));
        auto count = block->instructions.size();
        if(!count) continue;

        auto last = base()[block->instructions.get(base(), count - 1)];
        if(!isTerminator(last->kind)) continue;

        forEachEdge(*last, [&](Edge& edge) {
            if(!edge.target) return;
            base()[edge.target]->incoming.push(arena, block - base());
        });
    }
}

} // namespace ssa
