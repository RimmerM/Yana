#include "build.h"

#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

namespace llvmgen {

/*
 * Types.
 */

llvm::Type* typeOf(Gen& gen, LowerType type) {
    // A vector is the lane type repeated, and a mask is `<N x i1>` - which is what LLVM's own
    // comparisons produce and its selects consume, rather than the full-width lanes this compiler's
    // Repr gives a mask. The two meet at a `sext` on the way out of a compare and a `trunc` on the
    // way into a select; matching our representation exactly would produce worse code and more of
    // it. See §6 of Implementation-Vector.md, whose instructions are not built yet.
    if(isVectorLike(type)) {
        auto lane = type.isMask() ? llvm::Type::getInt1Ty(gen.llvm) : typeOf(gen, type.laneType());
        return llvm::FixedVectorType::get(lane, type.lanes());
    }

    switch(type.lane) {
        case LowerLane::Int8:
            return llvm::Type::getInt8Ty(gen.llvm);
        case LowerLane::Int16:
            return llvm::Type::getInt16Ty(gen.llvm);
        case LowerLane::Int32:
            return llvm::Type::getInt32Ty(gen.llvm);
        case LowerLane::Int64:
            return llvm::Type::getInt64Ty(gen.llvm);
        case LowerLane::Float32:
            return llvm::Type::getFloatTy(gen.llvm);
        case LowerLane::Float64:
            return llvm::Type::getDoubleTy(gen.llvm);
        case LowerLane::Pointer:
            // Opaque, which is what makes this backend short: the lower IR has one pointer type and
            // states what memory holds at the access rather than in the type, and since LLVM 15 so
            // does LLVM. Every `bitcast` between pointer types the old backend emitted was work to
            // agree with a type system that no longer exists.
            return llvm::Type::getInt8PtrTy(gen.llvm);
    }

    assertTrue("unknown lower type" == nullptr);
    return nullptr;
}

llvm::IntegerType* intTypeOfWidth(Gen& gen, U32 bytes) {
    return llvm::IntegerType::get(gen.llvm, bytes * 8);
}

llvm::Type* resultTypeOf(Gen& gen, llvm::ArrayRef<llvm::Type*> results) {
    if(results.empty()) return llvm::Type::getVoidTy(gen.llvm);
    if(results.size() == 1) return results[0];

    return llvm::StructType::get(gen.llvm, results);
}

llvm::FunctionType* signatureOf(Gen& gen, llvm::ArrayRef<llvm::Type*> args, llvm::ArrayRef<llvm::Type*> results) {
    return llvm::FunctionType::get(resultTypeOf(gen, results), args, false);
}

llvm::FunctionType* signatureOf(Gen& gen, LowerFunction& fun) {
    llvm::SmallVector<llvm::Type*, 8> args;
    llvm::SmallVector<llvm::Type*, 2> results;

    for(auto a: fun.args.contents(gen.base)) {
        args.push_back(typeOf(gen, gen.base[a]->result.type));
    }

    for(auto r: fun.returnTypes.contents(gen.base)) {
        results.push_back(typeOf(gen, LowerType(r)));
    }

    return signatureOf(gen, args, results);
}

/*
 * Calling conventions.
 *
 * Three of the six are the compiler's own, and they exist to tell *our* register allocator how much
 * of the register file a call may take - which is not a question LLVM's allocator asks anyone. What
 * survives the translation is the part that is an ABI rather than an allocation policy: whether the
 * caller and the callee agree on the platform's convention, or on one LLVM is free to choose.
 *
 * So Complex and Clobber both become `fastcc`. They differ in what a caller may keep in a register
 * across the call, and LLVM decides that for itself from the call graph it can see - which is the
 * whole reason this backend exists beside the x64 one.
 */
llvm::CallingConv::ID conventionOf(LowerCallType type) {
    switch(type) {
        case LowerCallType::Sysv:
            // The C convention of the target triple. On x86-64 ELF that is System V by definition,
            // which is also why nothing here has to name the argument registers.
            return llvm::CallingConv::C;
        case LowerCallType::Win64:
            return llvm::CallingConv::Win64;
        case LowerCallType::Simple:
            // The caller keeps almost everything, which is exactly what preserve_most promises: the
            // callee saves every register but its result and one scratch.
            return llvm::CallingConv::PreserveMost;
        case LowerCallType::Complex:
        case LowerCallType::Clobber:
            return llvm::CallingConv::Fast;
        case LowerCallType::Syscall:
            // Never reaches a call instruction - genSyscall emits inline assembly instead.
            return llvm::CallingConv::C;
    }

    assertTrue("unknown calling convention" == nullptr);
    return llvm::CallingConv::C;
}

/*
 * The target the module is for.
 *
 * Stated rather than taken from a TargetMachine, so that generating IR needs no target backend
 * linked in and produces the same text wherever the compiler runs. `writeObjectFile` overwrites the
 * data layout with the target machine's own, which is the one place the two could disagree.
 */
struct TargetDescription {
    StringView triple;
    StringView dataLayout;
};

static TargetDescription targetDescriptionOf(Context& context) {
    static constexpr StringView kX64Layout =
        "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"_v;

    switch(context.settings.arch) {
        case TargetArch::ARM64:
            switch(context.settings.target) {
                case TargetType::MacOS:
                    return { "arm64-apple-darwin"_v, "e-m:o-i64:64-i128:128-n32:64-S128"_v };
                default:
                    return { "aarch64-unknown-linux-gnu"_v, "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"_v };
            }

        case TargetArch::X86:
        case TargetArch::ARM:
            // The lower IR is 64-bit: addresses are added to with 64-bit offsets, and every
            // compiler-built table lays out its slots eight bytes apart. A 32-bit target is a
            // different lowering rather than a different triple here.
            context.diagnostics.error("llvm: the 32-bit architectures are not supported yet"_v, nullptr);
            [[fallthrough]];

        case TargetArch::X64:
        default:
            switch(context.settings.target) {
                case TargetType::MacOS:
                    return { "x86_64-apple-darwin"_v, "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"_v };
                case TargetType::Win32:
                    return { "x86_64-pc-windows-msvc"_v, "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"_v };
                default:
                    return { "x86_64-unknown-linux-gnu"_v, kX64Layout };
            }
    }
}

static llvm::StringRef toRef(StringView view) {
    return { view.ptr, view.length };
}

/*
 * Globals.
 *
 * A global's contents are bytes and a list of addresses to write into them, which is the flat-buffer
 * spelling of a relocation - see LowerDataRelocation. LLVM says the same thing with a constant that
 * *contains* the address, so this splits the bytes at each relocation site and rebuilds them as a
 * packed struct of byte arrays and pointers. Packed because the offsets are the point: a witness
 * table's slot is at the offset the lowering computed, and a struct LLVM is free to pad is not.
 *
 * Built twice per global. A table naming a function is a forward reference as often as not - and
 * naming *itself* is what a recursive type descriptor does - so the first pass builds the same
 * shape with null addresses, which is what the variable is declared as, and the second fills them
 * in once everything in the module exists. Both walk this one function, so the type the variable
 * was declared with and the type of what is stored into it cannot drift apart.
 */
static llvm::Constant* byteChunk(Gen& gen, const Byte* bytes, Size length) {
    return llvm::ConstantDataArray::get(gen.llvm, llvm::ArrayRef<uint8_t>((const uint8_t*)bytes, length));
}

static llvm::Constant* initializerOf(Gen& gen, LowerGlobal& global, bool resolve) {
    auto contents = global.initialContents;

    // Each site as the address that goes into it. Resolved before being sorted so that what is
    // sorted is a pair of numbers rather than the IR's own record of the relocation.
    llvm::SmallVector<std::pair<U32, llvm::Constant*>, 8> sites;

    for(auto relocation: global.relocations.contents(gen.base)) {
        llvm::Constant* address = nullptr;

        if(!resolve) {
            address = llvm::ConstantPointerNull::get(llvm::Type::getInt8PtrTy(gen.llvm));
        } else if(relocation.function) {
            address = gen.functionOf(relocation.function);
        } else if(relocation.global) {
            address = gen.globalOf(relocation.global);
        }

        if(!address) {
            gen.context.diagnostics.error("llvm: global %@ relocates against something that does not exist"_v,
                                          &global.source, gen.context.findName(global.name));
            address = llvm::ConstantPointerNull::get(llvm::Type::getInt8PtrTy(gen.llvm));
        }

        sites.push_back({ relocation.offset, address });
    }

    if(sites.empty()) {
        if(contents.length == 0) return llvm::ConstantAggregateZero::get(llvm::ArrayType::get(llvm::Type::getInt8Ty(gen.llvm), 0));
        return byteChunk(gen, contents.ptr, contents.length);
    }

    llvm::sort(sites, [](const std::pair<U32, llvm::Constant*>& a, const std::pair<U32, llvm::Constant*>& b) {
        return a.first < b.first;
    });

    llvm::SmallVector<llvm::Constant*, 8> fields;
    Size at = 0;

    for(auto& site: sites) {
        if(site.first > at) {
            fields.push_back(byteChunk(gen, contents.ptr + at, site.first - at));
        }

        fields.push_back(site.second);
        at = site.first + 8;
    }

    if(at < contents.length) {
        fields.push_back(byteChunk(gen, contents.ptr + at, contents.length - at));
    }

    return llvm::ConstantStruct::getAnon(gen.llvm, fields, true);
}

static llvm::GlobalVariable* declareGlobal(Gen& gen, LowerGlobal& global) {
    auto initializer = initializerOf(gen, global, false);
    auto variable = new llvm::GlobalVariable(
        gen.module, initializer->getType(), !global.mut,
        llvm::GlobalValue::ExternalLinkage, initializer, nameOf(gen.context, global.name)
    );

    // The strongest thing anything in a compiler-built table needs, and the alignment every one of
    // them is laid out for: a slot holding an address has to be aligned to be atomically readable,
    // and nothing in the lower IR asks for more than a word.
    variable->setAlignment(llvm::Align(8));
    gen.globals.add((&global - gen.base).offset, variable);
    return variable;
}

/*
 * Functions.
 */

static llvm::Function* declareFunction(Gen& gen, LowerFunction& fun) {
    auto target = llvm::Function::Create(
        signatureOf(gen, fun), llvm::Function::ExternalLinkage,
        nameOf(gen.context, fun.name), gen.module
    );

    target->setCallingConv(conventionOf(fun.callType));

    U32 index = 0;
    for(auto a: fun.args.contents(gen.base)) {
        target->getArg(index++)->setName(nameOf(gen.context, gen.base[a]->result.name));
    }

    gen.functions.add((&fun - gen.base).offset, target);
    return target;
}

/*
 * The blocks of one function.
 *
 * Generated in reverse postorder so that everything a use names has been built by the time the use
 * is: the lower validator guarantees that a definition dominates every non-phi use of it, and a
 * dominator precedes what it dominates in reverse postorder. Phis are the exception the SSA form
 * always is, so they are created empty first and given their operands once every block exists.
 */
static void genFunctionBody(Gen& gen, LowerFunction& fun, llvm::Function& target) {
    FunGen f(gen, fun, target);

    U32 index = 0;
    for(auto a: fun.args.contents(gen.base)) {
        f.define(gen.base[a]->result, target.getArg(index++));
    }

    auto blockList = fun.blocks.contents(gen.base);
    auto postorder = fun.buildPostorder(gen.base);

    for(Size i = 0; i < blockList.size(); i++) {
        auto block = gen.base[blockList[i]];
        f.blocks.push(llvm::BasicBlock::Create(gen.llvm, nameOf(gen.context, block->name), &target));
    }

    f.entry = f.blocks[0];

    // Every phi in the function, before any of them can be named: a loop header's phi takes a value
    // the latch has not produced yet, and the latch's own instructions may read that phi.
    for(Size i = postorder.size(); i > 0; i--) {
        auto block = gen.base[blockList[postorder[i - 1]]];
        f.builder.SetInsertPoint(f.blocks[block->index]);

        for(auto p: block->phis.contents(gen.base)) {
            auto phi = gen.base[p];
            auto built = f.builder.CreatePHI(typeOf(gen, phi->result.type), phi->usedCount,
                                             nameOf(gen.context, phi->result.name));
            f.define(phi->result, built);
        }
    }

    for(Size i = postorder.size(); i > 0; i--) {
        auto block = gen.base[blockList[postorder[i - 1]]];
        f.builder.SetInsertPoint(f.blocks[block->index]);

        for(auto offset: block->instructions.contents(gen.base)) {
            genInst(f, *gen.base[offset]);
        }

        genInst(f, *gen.base[block->terminator]);
    }

    // A block the entry cannot reach is not something the lowering produces - the dominator
    // analysis the validator runs would not accept one - but a block without a terminator is not
    // valid LLVM, so an empty one is closed here rather than left for the verifier to find.
    for(Size i = 0; i < blockList.size(); i++) {
        auto block = f.blocks[i];
        if(block->getTerminator()) continue;

        f.builder.SetInsertPoint(block);
        f.builder.CreateUnreachable();
    }

    for(Size i = postorder.size(); i > 0; i--) {
        auto block = gen.base[blockList[postorder[i - 1]]];

        for(auto p: block->phis.contents(gen.base)) {
            auto phi = gen.base[p];
            auto built = (llvm::PHINode*)f.use(phi->result);
            auto values = phi->used();
            auto sources = phi->sources();

            for(Size j = 0; j < values.length; j++) {
                auto value = f.use(values[j]);
                if(!value) value = llvm::PoisonValue::get(typeOf(gen, phi->result.type));

                built->addIncoming(value, f.block(sources[j]));
            }
        }
    }
}

/*
 * The module.
 */

Ptr<llvm::Module> genModule(llvm::LLVMContext& llvm, Context& context, LowerModule& module) {
    auto description = targetDescriptionOf(context);
    auto built = new llvm::Module(nameOf(context, module.name), llvm);

    built->setTargetTriple(toRef(description.triple));
    built->setDataLayout(toRef(description.dataLayout));

    Gen gen(llvm, *built, context, module);
    auto base = gen.base;

    // Declarations first, and all of them: a call is a forward reference as often as not, and a
    // constant table holding the address of a function is one by definition.
    for(auto g: module.globalOrder) declareGlobal(gen, *base[g]);
    for(auto f: module.functionOrder) declareFunction(gen, *base[f]);

    // Now that everything has a name, what the constant tables hold can be filled in.
    for(auto g: module.globalOrder) {
        gen.globalOf(g)->setInitializer(initializerOf(gen, *base[g], true));
    }

    // Prefix data is emitted immediately in front of the entry point, which is what LLVM calls it
    // too: a reference to the function names the entry point, and the data sits at a negative
    // offset from it. That is exactly the contract LowerFunction::prefix states, so a closure
    // header needs nothing here beyond building the constant.
    for(auto f: module.functionOrder) {
        auto fun = base[f];
        if(!fun->prefix) continue;

        gen.functionOf(f)->setPrefixData(initializerOf(gen, *base[fun->prefix], true));
    }

    for(auto f: module.functionOrder) {
        genFunctionBody(gen, *base[f], *gen.functionOf(f));
    }

    return Ptr(built);
}

bool verifyGenModule(Context& context, llvm::Module& module) {
    std::string text;
    llvm::raw_string_ostream stream(text);

    if(!llvm::verifyModule(module, &stream)) return true;

    stream.flush();
    context.diagnostics.error("llvm: the generated module is invalid: %@"_v, nullptr,
                              StringView { text.data(), text.size() });
    return false;
}

void printModule(Net::Writer& writer, llvm::Module& module) {
    std::string text;
    llvm::raw_string_ostream stream(text);
    module.print(stream, nullptr);
    stream.flush();

    writer.writeString(StringView { text.data(), text.size() });
}

} // namespace llvmgen
