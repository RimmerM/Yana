#include "lower.h"
#include "../lower/lower_inst.h"
#include "../lower/lower_builder.h"

namespace ssa {

namespace {

/*
 * A constant as the bytes a global's storage starts at.
 *
 * Little-endian and scalar, which is every constant this milestone has. When aggregates arrive this
 * becomes a walk over the manifestation of the type rather than a switch over the constant, for the
 * reason `repr/constant.h` gives on the other side of the wall: the layout is the target's answer
 * and the constant only supplies the values.
 */
static U64 constantBits(DeclBase decl, const mir::Constant& constant) {
    switch(constant.kind) {
        case mir::Constant::None:
        case mir::Constant::Unit:
            return 0;
        case mir::Constant::Bool:
        case mir::Constant::Int:
            return constant.bits;
        case mir::Constant::Float: {
            // The constant holds the `double` pattern whatever the declared width, because the width
            // is the target's answer and this is the first place that has it.
            if(decl[constant.type]->kind == mir::Type::Float &&
               ((mir::FloatType*)decl[constant.type])->bits == 32) {
                auto narrow = F32(constant.asFloat());
                U32 word;
                copy((Byte*)&narrow, (Byte*)&word, 4);
                return word;
            }

            return constant.bits;
        }
    }

    return 0;
}

static void materialize(DeclBase decl, const Manifestation& manifest, const mir::Constant& constant,
                        Byte* into) {
    auto bits = constantBits(decl, constant);
    for(U32 i = 0; i < manifest.size; i++) into[i] = Byte((bits >> (i * 8)) & 0xff);
}

/*
 * Which machine operation an SSA operation over this type becomes.
 *
 * The one place signedness enters the pipeline, which is why op.def has no column for it: `I32` and
 * `U32` are two logical types over one `div`, and it is here - with the type in hand and the target
 * decided - that the two become `idiv` and `div`. The same reading makes `<` two condition codes and
 * `+` one instruction.
 */
static LowerInst::Kind binaryKind(DeclBase decl, mir::BinaryOp op, DeclPtr<mir::Type> operandType) {
    auto& logical = *decl[operandType];
    auto floating = logical.kind == mir::Type::Float;
    auto isSigned = logical.kind == mir::Type::Int && ((mir::IntType&)logical).isSigned;

    switch(op) {
        case mir::BinaryOp::Add: return LowerInst::Add;
        case mir::BinaryOp::Sub: return LowerInst::Sub;
        case mir::BinaryOp::Mul: return floating || !isSigned ? LowerInst::Mul : LowerInst::IMul;
        case mir::BinaryOp::Div: return floating || !isSigned ? LowerInst::Div : LowerInst::IDiv;
        case mir::BinaryOp::Rem: return isSigned ? LowerInst::IRem : LowerInst::Rem;
        case mir::BinaryOp::And: return LowerInst::And;
        case mir::BinaryOp::Or:  return LowerInst::Or;
        case mir::BinaryOp::Xor: return LowerInst::Xor;
        case mir::BinaryOp::Shl: return LowerInst::Shl;
        case mir::BinaryOp::Shr: return LowerInst::Shr;
        case mir::BinaryOp::Sar: return LowerInst::Sar;

        // The comparisons are one instruction with a condition code - see `lowerCmp`.
        case mir::BinaryOp::Eq:
        case mir::BinaryOp::Ne:
        case mir::BinaryOp::Lt:
        case mir::BinaryOp::Le:
        case mir::BinaryOp::Gt:
        case mir::BinaryOp::Ge:
        case mir::BinaryOp::Count:
            break;
    }

    return LowerInst::Cmp;
}

static LowerCmp lowerCmp(DeclBase decl, mir::BinaryOp op, DeclPtr<mir::Type> operandType) {
    auto& logical = *decl[operandType];
    auto isSigned = logical.kind == mir::Type::Int && ((mir::IntType&)logical).isSigned;

    switch(op) {
        case mir::BinaryOp::Eq: return LowerCmp::eq;
        case mir::BinaryOp::Ne: return LowerCmp::neq;

        // A float ordering is the unsigned code: the machine's ordered float compare is what the
        // backend selects from the operand type, and the signed codes are the integer ones.
        case mir::BinaryOp::Lt: return isSigned ? LowerCmp::ilt : LowerCmp::lt;
        case mir::BinaryOp::Le: return isSigned ? LowerCmp::ile : LowerCmp::le;
        case mir::BinaryOp::Gt: return isSigned ? LowerCmp::igt : LowerCmp::gt;
        case mir::BinaryOp::Ge: return isSigned ? LowerCmp::ige : LowerCmp::ge;
        default: break;
    }

    return LowerCmp::eq;
}

struct Lowering {
    Context& context;
    mir::Program& program;
    Converted& converted;
    LowerModule& to;

    DeclBase decl;
    SsaBase ssa;
    LowerBase lower;

    HashMap<U32, LowerPtr<LowerGlobal>> globals;
    HashMap<U32, LowerPtr<LowerFunction>> functions;

    // SSA value ID to the lower value it became, and SSA block index to the lower block. Per
    // function, reset rather than rebuilt - the containers guidance's rule, applied where it costs
    // one allocation per compilation instead of one per function.
    HashMap<U32, LowerPtr<LowerValue>> values;
    HashMap<U32, LowerPtr<LowerBlock>> blocks;

    /*
     * The phis a block's parameters became, and one alternative each edge owes them.
     *
     * A phi has to have one alternative per incoming edge and its alternatives are values in blocks
     * that have not been lowered yet, so it is built detached, filled from the edges as they arrive,
     * and added to its block last - which is also when its uses are registered. `lower_promote.cpp`
     * builds one the same way and says why.
     */
    struct PendingPhi {
        LowerInstPhi* phi = nullptr;
        LowerPtr<LowerBlock> block = nullptr;
        U32 filled = 0;
    };

    Array<PendingPhi> phis;

    // SSA block argument value ID to its index in `phis`.
    HashMap<U32, U32> phiOf;

    void lowerGlobal(DeclPtr<mir::Global> pointer) {
        auto source = decl[pointer];
        auto target = new (to.arena) LowerGlobal(source->name);

        // What is *written*, rather than what may be assigned - the same distinction
        // `LowerGlobal::mut` makes: clear becomes LLVM's `constant`, which is a promise rather than
        // a hint.
        target->mut = source->writable;

        auto manifest = manifestNative(decl, source->occurrence);
        target->initialContents = ByteBuffer((Byte*)to.arena.alloc(manifest.size), manifest.size);
        set(target->initialContents.ptr, manifest.size, 0);

        if(source->initial.isSet()) {
            materialize(decl, manifest, source->initial, target->initialContents.ptr);
        }

        auto handle = target - lower;
        globals.add(pointer.offset, handle);
        to.globals.add(source->name, handle);
        to.globalOrder.push(handle);
    }

    LowerValue* mapped(SsaPtr<Value> value) {
        auto found = values.getValue(ssa[value]->id.index);
        return found ? lower[found.unwrap()] : nullptr;
    }

    /*
     * How a value manifests - the whole occurrence, from the value itself.
     *
     * Every one of these used to pass `ownedContract()`, because an `ssa::Value` carried only its
     * logical type and there was nothing else to pass. That is a representation failure with a
     * silent wrong answer at the end of it: a shared window and its owner are one logical type, so
     * the table was asked a question that could only be answered as the owner. `Value::occurrence`
     * is what closes it, and this is the one place that reads it.
     */
    Manifestation manifestOf(Value& value) { return manifestNative(decl, value.occurrence); }

    /*
     * One edge's arguments, into the phis its target's parameters became.
     *
     * The alternative and the block it comes from are written as a pair, and the pair is what
     * `validateLowerModule` checks - so the order alternatives are filled in does not have to match
     * the order the target's `incoming` list was built in. That is worth not depending on: the
     * incoming list is built by `addInst` as terminators arrive, and this runs immediately before
     * the terminator that would extend it.
     */
    void supplyEdge(LowerBlock& from, Edge& edge) {
        auto target = ssa[edge.target];
        auto count = edge.arguments.size();

        for(U32 i = 0; i < count; i++) {
            auto used = edge.arguments.get(ssa, i);
            auto value = mapped(used.value());
            if(!value) continue;

            auto argument = ssa[target->arguments.get(ssa, i)];
            auto index = phiOf.getValue(argument->id.index);
            if(!index) continue;

            // One alternative per incoming edge, and the phi was sized from the SSA predecessor
            // count. More arriving than that means the two disagree about the CFG, which is a
            // conversion bug rather than a program error - and writing past the allocation is what
            // it would otherwise be.
            auto& pending = phis[index.unwrap()];
            assertTrue(pending.filled < pending.phi->usedCount);

            pending.phi->used()[pending.filled] = value - lower;
            pending.phi->sources()[pending.filled] = &from - lower;
            pending.filled++;
        }
    }

    void lowerInst(LowerBlock& block, Inst& inst) {
        switch(inst.kind) {
            case Inst::Const: {
                auto& written = (InstConst&)inst;
                auto result = ssa[inst.results.get(ssa, 0)];
                auto manifest = manifestOf(*result);

                LowerInst* value;
                if(written.constant.kind == mir::Constant::Float) {
                    value = block.addInst(lower, new (to.arena) LowerImm(
                        StringId(), manifest.type, written.constant.asFloat()));
                } else {
                    value = block.addInst(lower, new (to.arena) LowerImm(
                        StringId(), manifest.type, written.constant.bits));
                }

                values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
                break;
            }
            case Inst::GlobalAddress: {
                auto& written = (InstGlobalAddress&)inst;
                auto found = globals.getValue(written.global.offset);
                if(!found) {
                    context.diagnostics.error("internal: a global was reached before it was lowered"_v,
                                              inst.source);
                    return;
                }

                auto result = ssa[inst.results.get(ssa, 0)];
                auto value = block.addInst(lower, new (to.arena) LowerInstGlobal(
                    decl[written.global]->name, found.unwrap()));

                values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
                break;
            }
            case Inst::Alloc: {
                auto& written = (InstAlloc&)inst;

                // The manifestation of what the slot *holds*, under the occurrence it holds it as -
                // which is why `InstAlloc::held` is an occurrence and not a type.
                auto manifest = manifestNative(decl, written.held);

                // A byte count as a value rather than a field, which is what `LowerInstAlloca` takes:
                // an allocation whose size is computed is the same instruction as one whose size is a
                // literal, and a generic type's size will be exactly that computation.
                auto size = block.addInst(lower, new (to.arena) LowerImm(
                    StringId(), LowerType::Int64, U64(manifest.size ? manifest.size : 1)));

                auto value = block.addInst(lower, new (to.arena) LowerInstAlloca(
                    StringId(), ((LowerInstSingle*)size)->created().ptr - lower,
                    manifest.align ? manifest.align : 1));

                auto result = ssa[inst.results.get(ssa, 0)];
                values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
                break;
            }
            case Inst::Store: {
                auto& written = (InstStore&)inst;
                auto address = mapped(written.address.value());
                auto value = mapped(written.value.value());
                if(!address || !value) return;

                auto manifest = manifestOf(*ssa[written.value.value()]);
                if(!manifest.size) break;

                block.addInst(lower, new (to.arena) LowerInstStore(
                    address - lower, value - lower, manifest.size));
                break;
            }
            case Inst::Binary: {
                auto& written = (InstBinary&)inst;
                auto lhs = mapped(written.lhs.value());
                auto rhs = mapped(written.rhs.value());
                if(!lhs || !rhs) return;

                auto operandType = ssa[written.lhs.value()]->type();
                auto result = ssa[inst.results.get(ssa, 0)];
                auto kind = binaryKind(decl, written.op, operandType);

                LowerInst* value;
                if(kind == LowerInst::Cmp) {
                    value = block.addInst(lower, new (to.arena) LowerInstCmp(
                        StringId(), lhs - lower, rhs - lower,
                        lowerCmp(decl, written.op, operandType)));
                } else {
                    auto manifest = manifestOf(*result);
                    value = block.addInst(lower, new (to.arena) LowerInstBinary(
                        StringId(), manifest.type, lhs - lower, rhs - lower, kind));
                }

                values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
                break;
            }
            case Inst::Unary: {
                auto& written = (InstUnary&)inst;
                auto operand = mapped(written.value.value());
                if(!operand) return;

                auto result = ssa[inst.results.get(ssa, 0)];
                auto manifest = manifestOf(*result);

                LowerInst* value;
                if(written.op == mir::UnaryOp::Not) {
                    /*
                     * `!b` is one exclusive or, not a complement.
                     *
                     * A `Bool` is one byte holding 0 or 1 in a 32-bit register, so inverting every
                     * bit of it answers 254 rather than False. That the two are different is exactly
                     * why `Bool` is not a one-bit integer up above: the operation is logical, and
                     * this is the file that knows what a `Bool` looks like.
                     */
                    auto one = block.addInst(lower, new (to.arena) LowerImm(
                        StringId(), manifest.type, U64(1)));

                    value = block.addInst(lower, new (to.arena) LowerInstBinary(
                        StringId(), manifest.type, operand - lower,
                        ((LowerInstSingle*)one)->created().ptr - lower, LowerInst::Xor));
                } else {
                    value = block.addInst(lower, new (to.arena) LowerInstUnary(
                        LowerInst::Neg, StringId(), manifest.type, operand - lower));
                }

                values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
                break;
            }
            case Inst::Load: {
                auto& written = (InstLoad&)inst;
                auto address = mapped(written.address.value());
                if(!address) return;

                auto result = ssa[inst.results.get(ssa, 0)];
                auto manifest = manifestOf(*result);

                // A type with no bytes has nothing to load, and `makeMemoryFlags` has no encoding
                // for a width of zero. Nothing produces one - a `{}` value is not read out of
                // storage - so this is the assertion that says so rather than a case to handle.
                assertTrue(manifest.size != 0);

                /*
                 * The access width is the declared one and the register is the manifestation's: a
                 * narrow scalar arrives in a 32-bit register and states its width here, which is the
                 * rule `LowerLane` is written against.
                 *
                 * In **bytes**, which is what `makeMemoryFlags` encodes and `getWidth` reads back.
                 * A load that stated bits validated anyway for every integer - the flags hold a
                 * log2 and 32 is as much a power of two as 4 - and was caught by `F64`, whose
                 * validation compares the width against the lane. The store below has always taken
                 * bytes, so the two disagreed about one instruction's meaning.
                 */
                auto isSigned = decl[result->type()]->kind == mir::Type::Int &&
                                ((mir::IntType*)decl[result->type()])->isSigned;

                auto value = block.addInst(lower, new (to.arena) LowerInstLoad(
                    address - lower, StringId(), manifest.type, manifest.size, isSigned));

                values.add(result->id.index, &((LowerInstSingle*)value)->result - lower);
                break;
            }
            case Inst::Call: {
                auto& written = (InstCall&)inst;
                auto found = functions.getValue(written.callee.offset);
                if(!found) {
                    context.diagnostics.error(
                        "internal: a call reached a callee that was not lowered"_v, inst.source);
                    return;
                }

                // The target as a value, which is what `LowerInstCall` takes in its first used
                // position - see the note on that instruction. A direct call names a function and an
                // indirect one will name any other pointer, so the call itself needs no second form.
                auto callee = block.addInst(lower, new (to.arena) LowerInstFun(
                    decl[written.callee]->name, found.unwrap()));

                SmallArray<LowerPtr<LowerValue>, 8> passed;
                for(auto used: written.arguments.contents(ssa)) {
                    auto value = mapped(used.value());
                    if(!value) return;
                    passed.push(value - lower);
                }

                /*
                 * The **callee's** convention, not this function's.
                 *
                 * They are the same today because every function here takes the default, and they
                 * are different the moment one declaration names another - a call has to agree with
                 * what it is calling, and reading the enclosing function's would agree with the
                 * caller instead. Nothing would report it: the registers simply differ.
                 */
                auto results = inst.results.size();
                auto convention = lower[found.unwrap()]->callType;

                auto value = call(lower, to, block, results, passed.size() + 1, convention,
                                  [&](LowerInstCall* built) {
                    for(Size i = 0; i < results; i++) {
                        auto result = ssa[inst.results.get(ssa, i)];
                        auto manifest = manifestOf(*result);

                        // An address-only result is returned through a hidden pointer the caller
                        // supplies, which is a second used position and an allocation before the
                        // call. No type at this milestone has one; the assertion is what says so
                        // rather than the absence of the code saying nothing.
                        assertTrue(!manifest.inMemory);
                        new (built->created().ptr + i) LowerValue(built, manifest.type, StringId());
                    }

                    built->used()[0] = ((LowerInstSingle*)callee)->created().ptr - lower;
                    for(Size i = 0; i < passed.size(); i++) built->used()[i + 1] = passed[i];
                });

                for(Size i = 0; i < results; i++) {
                    values.add(ssa[inst.results.get(ssa, i)]->id.index,
                               value->created().ptr + i - lower);
                }

                break;
            }
            case Inst::Return: {
                auto& written = (InstReturn&)inst;
                auto count = written.values.size();

                SmallArray<LowerPtr<LowerValue>, 4> returned;
                for(auto used: written.values.contents(ssa)) {
                    auto value = mapped(used.value());
                    if(!value) return;
                    returned.push(value - lower);
                }

                auto storage = to.arena.alloc(sizeof(LowerInstRet) + sizeof(LowerPtr<LowerValue>) * count);
                auto result = new (storage) LowerInstRet;
                result->usedCount = U16(count);
                for(Size i = 0; i < count; i++) result->used()[i] = returned[i];

                block.addInst(lower, result);
                break;
            }
            case Inst::Jump: {
                auto& written = (InstJump&)inst;
                auto target = blocks.getValue(ssa[written.to.target]->index.index);
                if(!target) return;

                supplyEdge(block, written.to);
                block.addInst(lower, new (to.arena) LowerInstJmp(target.unwrap()));
                break;
            }
            case Inst::Branch: {
                auto& written = (InstBranch&)inst;
                auto condition = mapped(written.condition.value());
                auto whenTrue = blocks.getValue(ssa[written.whenTrue.target]->index.index);
                auto whenFalse = blocks.getValue(ssa[written.whenFalse.target]->index.index);
                if(!condition || !whenTrue || !whenFalse) return;

                supplyEdge(block, written.whenTrue);
                supplyEdge(block, written.whenFalse);

                block.addInst(lower, new (to.arena) LowerInstJe(
                    condition - lower, whenTrue.unwrap(), whenFalse.unwrap()));
                break;
            }
            case Inst::Unreachable:
                block.addInst(lower, new (to.arena) LowerInstUnreachable());
                break;
            case Inst::KindCount:
                break;
        }
    }

    void lowerBody(mir::Function& source, LowerFunction& target) {
        auto& body = *ssa[source.ssa];

        values.reset();
        blocks.reset();
        phiOf.reset();
        phis.clear();

        /*
         * A lower block for every SSA block, the entry included.
         *
         * The lower IR's own entry block is a *stub*: `LowerFunction`'s constructor makes one, it
         * holds the argument references, nothing may jump to it, and `addBlock` gives it a jump to
         * the first real block. So the SSA entry maps to a new block rather than to that one -
         * mapping it to the stub leaves a function whose entry ends in a return, which is what
         * `validateLowerModule` reports as "entry block must end with unconditional jump".
         */
        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(ssa, BlockId(i));
            blocks.add(i, target.addBlock(lower, block->name) - lower);
        }

        /*
         * A phi per block parameter, detached and sized from the SSA predecessor count.
         *
         * Before any instruction, because the block's own code reads the parameter and every
         * predecessor's terminator writes an alternative into it. The count comes from the SSA
         * block's `incoming`, which `Editor::seal` filled: the lower block's own incoming list is
         * built by the terminators this walk is about to add, so it is empty right now and asking it
         * would size every phi at zero.
         */
        /*
         * The entry block's parameters are the function's own, and they are already values.
         *
         * `LowerArg`s, made by the declaration walk below and living in the lower function's stub
         * entry block. Not phis: nothing jumps to the SSA entry, so a phi there would be sized at
         * zero alternatives and answer nothing. This is the one block whose parameters come from
         * outside the function, which is exactly why it is the one block with a case of its own.
         */
        auto entryBlock = body.block(ssa, BlockId(0));
        for(U32 i = 0; i < entryBlock->arguments.size(); i++) {
            auto argument = ssa[entryBlock->arguments.get(ssa, i)];
            auto declared = lower[target.args.get(lower, i)];
            values.add(argument->id.index, &declared->result - lower);
        }

        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(ssa, BlockId(i));
            if(i == 0 || !block->arguments.size()) continue;

            auto into = lower[blocks.getValue(i).unwrap()];
            auto incoming = block->incoming.size();

            for(auto argumentPointer: block->arguments.contents(ssa)) {
                auto argument = ssa[argumentPointer];
                auto manifest = manifestOf(*argument);

                auto storage = to.arena.alloc(
                    sizeof(LowerInstPhi) +
                    sizeof(LowerPtr<LowerValue>) * incoming +
                    sizeof(LowerPtr<LowerBlock>) * incoming);

                auto phi = new (storage) LowerInstPhi(StringId(), manifest.type);
                phi->usedCount = U8(incoming);

                // Zeroed rather than left as the arena found it: an alternative that is still null
                // is what says this phi is not answerable yet, and reading whatever the arena last
                // held there is a wild pointer. `lower_promote.cpp` records that having happened.
                for(Size u = 0; u < incoming; u++) phi->used()[u] = nullptr;

                phiOf.add(argument->id.index, U32(phis.size()));
                phis.push(PendingPhi { phi, into - lower, 0 });
                values.add(argument->id.index, &((LowerInstSingle*)phi)->result - lower);
            }
        }

        for(U32 i = 0; i < body.blocks.size(); i++) {
            auto block = body.block(ssa, BlockId(i));
            auto into = lower[blocks.getValue(i).unwrap()];

            for(auto instPointer: block->instructions.contents(ssa)) {
                lowerInst(*into, *ssa[instPointer]);
            }
        }

        // Attached last, which is what registers their alternatives as uses - see `PendingPhi`. A
        // phi that did not receive one alternative per edge is a lowering bug rather than a program
        // error, and `validateLowerModule` is where it stops.
        for(auto& pending: phis) lower[pending.block]->addInst(lower, pending.phi);
    }

    void run() {
        // Globals first: a function's first instruction may take the address of one, and the module
        // resolves that by name.
        for(auto modulePointer: program.modules) {
            auto module = decl[modulePointer];
            for(auto globalPointer: module->globals.contents(decl)) {
                if(decl[globalPointer]->reachable) lowerGlobal(globalPointer);
            }
        }

        for(auto modulePointer: program.modules) {
            auto module = decl[modulePointer];

            for(auto functionPointer: module->functions.contents(decl)) {
                auto function = decl[functionPointer];
                if(!function->reachable || !function->ssa) continue;

                auto target = to.addFunction(function->name);
                target->source = function->source;

                // The parameters, before any body is lowered: a call in one function names the
                // arguments of another, so every signature has to exist before the first call is
                // built. Their manifestation is the declared occurrence's, which is what makes a
                // shared window and its owner two different signatures for one logical type.
                for(auto argumentPointer: function->args.contents(decl)) {
                    auto argument = decl[argumentPointer];
                    target->addArg(lower, argument->name,
                                   manifestNative(decl, argument->occurrence).type);
                }

                auto manifest = manifestNative(decl, function->result);
                if(decl[function->result.type]->kind != mir::Type::Unit) {
                    target->returnTypes.push(to.arena, manifest.type);
                }

                if(functionPointer == program.entry) to.entry = target->name;

                functions.add(functionPointer.offset, target - lower);
            }
        }

        for(auto modulePointer: program.modules) {
            auto module = decl[modulePointer];

            for(auto functionPointer: module->functions.contents(decl)) {
                auto function = decl[functionPointer];
                if(!function->reachable || !function->ssa) continue;

                lowerBody(*function, *lower[functions.getValue(functionPointer.offset).unwrap()]);
            }
        }
    }
};

} // namespace

Ptr<LowerModule> lowerProgram(Context& context, mir::Program& program, Converted& converted) {
    auto result = Ptr<LowerModule>(new LowerModule(8 * 1024 * 1024));

    Lowering lowering {
        context, program, converted, *result,
        *program.decl, *converted.arena, *result->arena
    };

    lowering.run();
    return result;
}

} // namespace ssa
