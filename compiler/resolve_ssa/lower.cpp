#include "lower_internal.h"

namespace ssa {

namespace {

/*
 * A constant as the bytes a global's storage starts at.
 *
 * Little-endian and scalar, which is every constant this milestone has. When aggregate constants
 * arrive this becomes a walk over the manifestation of the type rather than a switch over the
 * constant, for the reason `repr/constant.h` gives on the other side of the wall: the layout is the
 * target's answer and the constant only supplies the values.
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

} // namespace

void Lowering::lowerGlobal(DeclPtr<mir::Global> pointer) {
    auto source = decl[pointer];
    auto target = new (to.arena) LowerGlobal(source->name);

    // What is *written*, rather than what may be assigned - the same distinction `LowerGlobal::mut`
    // makes: clear becomes LLVM's `constant`, which is a promise rather than a hint.
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

LowerValue* Lowering::mapped(SsaPtr<Value> value) {
    auto found = values.getValue(ssa[value]->id.index);
    return found ? lower[found.unwrap()] : nullptr;
}

/*
 * One edge's arguments, into the phis its target's parameters became.
 *
 * The alternative and the block it comes from are written as a pair, and the pair is what
 * `validateLowerModule` checks - so the order alternatives are filled in does not have to match the
 * order the target's `incoming` list was built in. That is worth not depending on: the incoming list
 * is built by `addInst` as terminators arrive, and this runs immediately before the terminator that
 * would extend it.
 */
void Lowering::supplyEdge(LowerBlock& from, Edge& edge) {
    auto target = ssa[edge.target];
    auto count = edge.arguments.size();

    for(U32 i = 0; i < count; i++) {
        auto used = edge.arguments.get(ssa, i);
        auto value = mapped(used.value());
        if(!value) continue;

        auto argument = ssa[target->arguments.get(ssa, i)];
        auto index = phiOf.getValue(argument->id.index);
        if(!index) continue;

        // One alternative per incoming edge, and the phi was sized from the SSA predecessor count.
        // More arriving than that means the two disagree about the CFG, which is a conversion bug
        // rather than a program error - and writing past the allocation is what it would otherwise be.
        auto& pending = phis[index.unwrap()];
        assertTrue(pending.filled < pending.phi->usedCount);

        pending.phi->used()[pending.filled] = value - lower;
        pending.phi->sources()[pending.filled] = &from - lower;
        pending.filled++;
    }
}

void Lowering::lowerBody(mir::Function& source, LowerFunction& target) {
    auto& body = *ssa[source.ssa];

    values.reset();
    blocks.reset();
    phiOf.reset();
    phis.clear();

    /*
     * A lower block for every SSA block, the entry included.
     *
     * The lower IR's own entry block is a *stub*: `LowerFunction`'s constructor makes one, it holds
     * the argument references, nothing may jump to it, and `addBlock` gives it a jump to the first
     * real block. So the SSA entry maps to a new block rather than to that one - mapping it to the
     * stub leaves a function whose entry ends in a return, which is what `validateLowerModule`
     * reports as "entry block must end with unconditional jump".
     */
    for(U32 i = 0; i < body.blocks.size(); i++) {
        auto block = body.block(ssa, BlockId(i));
        blocks.add(i, target.addBlock(lower, block->name) - lower);
    }

    /*
     * The entry block's parameters are the function's own, and they are already values.
     *
     * `LowerArg`s, made by the declaration walk in `run` and living in the lower function's stub
     * entry block. Not phis: nothing jumps to the SSA entry, so a phi there would be sized at zero
     * alternatives and answer nothing. This is the one block whose parameters come from outside the
     * function, which is exactly why it is the one block with a case of its own.
     *
     * The hidden result pointer, where there is one, is argument zero - so the declared parameters
     * start one later. That offset is the *only* thing about the convention this walk knows; what
     * decides whether there is one is the manifestation, asked once in `run`.
     */
    auto entryBlock = body.block(ssa, BlockId(0));
    auto place = returnPlaces.getValue((&source - decl).offset);
    auto given = environmentPlaces.getValue((&source - decl).offset);

    currentReturnPlace = place ? place.unwrap() : LowerPtr<LowerValue>(nullptr);

    /*
     * The environment this body reads its own layouts out of, and the numbering it reads it by.
     *
     * Both set together and both null for every ordinary function, which is what makes an erased
     * read fail loudly rather than index a schema that is not there - a slot number means nothing
     * except relative to one context, and the context is this body's own.
     */
    currentEnvironment = given ? given.unwrap() : LowerPtr<LowerValue>(nullptr);
    currentGen = given ? source.gen : DeclPtr<mir::GenContext>(nullptr);

    // The environment, then the hidden result pointer, then the declared parameters - see `run`,
    // where the order is decided and the reason is written.
    auto leading = (given ? 1u : 0u) + (place ? 1u : 0u);

    for(U32 i = 0; i < entryBlock->arguments.size(); i++) {
        auto argument = ssa[entryBlock->arguments.get(ssa, i)];
        auto declared = lower[target.args.get(lower, i + leading)];
        values.add(argument->id.index, &declared->result - lower);
    }

    /*
     * A phi per block parameter, detached and sized from the SSA predecessor count.
     *
     * Before any instruction, because the block's own code reads the parameter and every
     * predecessor's terminator writes an alternative into it. The count comes from the SSA block's
     * `incoming`, which `Editor::seal` filled: the lower block's own incoming list is built by the
     * terminators this walk is about to add, so it is empty right now and asking it would size every
     * phi at zero.
     */
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

            // Zeroed rather than left as the arena found it: an alternative that is still null is
            // what says this phi is not answerable yet, and reading whatever the arena last held
            // there is a wild pointer. `lower_promote.cpp` records that having happened.
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

    // Attached last, which is what registers their alternatives as uses - see `PendingPhi`. A phi
    // that did not receive one alternative per edge is a lowering bug rather than a program error,
    // and `validateLowerModule` is where it stops.
    for(auto& pending: phis) lower[pending.block]->addInst(lower, pending.phi);
}

void Lowering::run() {
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

            // The symbol, which is what one name per emitted function means - see `Function::symbol`.
            auto target = to.addFunction(function->symbol);
            target->source = function->source;

            auto result = manifestNative(decl, function->result);

            /*
             * The generic environment, before everything - Analysis-IR.md §11.4's G5.
             *
             * First rather than after the hidden result pointer, because everything after it may
             * depend on it: the storage that pointer names is sized from a descriptor slot wherever
             * the result is a type parameter, so a caller reads the environment to build the rest of
             * the call. An order where the dependent argument came first would be legal and would
             * read backwards.
             */
            if(mir::isGeneric(decl, function->gen) && function->form == mir::GenForm::Erased) {
                auto given = target->addArg(lower, StringId(), LowerType::Pointer);
                environmentPlaces.add(functionPointer.offset, &given->result - lower);
            }

            /*
             * The hidden result pointer, before every declared parameter.
             *
             * A value with no register form comes back through storage the caller supplies, so the
             * caller says where. It is a *leading* argument because that is what the shared IR's
             * other producer does, and two producers that disagreed about the position would build
             * calls no callee could read.
             */
            if(result.inMemory) {
                auto place = target->addArg(lower, StringId(), LowerType::Pointer);
                returnPlaces.add(functionPointer.offset, &place->result - lower);
            }

            // The parameters, before any body is lowered: a call in one function names the arguments
            // of another, so every signature has to exist before the first call is built. Their
            // manifestation is the declared occurrence's, which is what makes a shared window and
            // its owner two different signatures for one logical type.
            for(auto argumentPointer: function->args.contents(decl)) {
                auto argument = decl[argumentPointer];
                target->addArg(lower, argument->name,
                               manifestNative(decl, argument->occurrence).type);
            }

            // A `{}` and an address-only result both return nothing: the first has no bytes and the
            // second was written into the caller's storage before the `ret`.
            if(decl[function->result.type]->kind != mir::Type::Unit && !result.inMemory) {
                target->returnTypes.push(to.arena, result.type);
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
