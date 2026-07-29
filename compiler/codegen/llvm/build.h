#pragma once

#include <llvm/IR/IRBuilder.h>

#include "gen.h"
#include "../../lower/lower_inst.h"

/*
 * What the backend sees of itself.
 *
 * Two levels of state, because the LLVM form of a module is built in two: what the whole module
 * agrees on - the functions and globals every call and address reference names - and what one
 * function is in the middle of building.
 */
namespace llvmgen {

struct Gen {
    Gen(llvm::LLVMContext& llvm, llvm::Module& module, Context& context, LowerModule& lower):
        llvm(llvm), module(module), context(context), base(*lower.arena), lower(lower) {}

    llvm::LLVMContext& llvm;
    llvm::Module& module;
    Context& context;
    LowerBase base;
    LowerModule& lower;

    // The declarations every reference to a function or a global resolves through, keyed by the
    // region offset of what they were built from. Both are filled before any body is built: a call
    // is an ordinary forward reference here, and a witness table holds the address of a function
    // that has not been generated yet.
    HashMap<U32, llvm::Function*> functions;
    HashMap<U32, llvm::GlobalVariable*> globals;

    llvm::Function* functionOf(LowerPtr<LowerFunction> fun) {
        auto found = functions.getValue(fun.offset);
        return found ? found.unwrap() : nullptr;
    }

    llvm::GlobalVariable* globalOf(LowerPtr<LowerGlobal> global) {
        auto found = globals.getValue(global.offset);
        return found ? found.unwrap() : nullptr;
    }
};

/*
 * One function being built.
 *
 * The value map is keyed by region offset rather than by anything in the IR, because a LowerValue
 * has nowhere to keep a backend pointer - which is the same reason the x64 backend keeps its own
 * side tables. Blocks are indexed by LowerBlock::index instead, which buildPostorder has just
 * renumbered to be the position in the function's block list.
 */
struct FunGen {
    FunGen(Gen& gen, LowerFunction& fun, llvm::Function& target):
        gen(gen), base(gen.base), context(gen.context), builder(gen.llvm), fun(fun), target(target) {}

    Gen& gen;
    LowerBase base;
    Context& context;
    llvm::IRBuilder<> builder;
    LowerFunction& fun;
    llvm::Function& target;

    HashMap<U32, llvm::Value*> values;
    Array<llvm::BasicBlock*> blocks;

    // Where a fixed-size allocation goes, whichever block it was written in - see genAlloca.
    llvm::BasicBlock* entry = nullptr;

    void define(LowerValue& value, llvm::Value* built) {
        auto offset = (&value - base).offset;
        values.add(offset, built);
    }

    // The LLVM form of a value the IR names. Every use is dominated by its definition (the lower
    // validator checks exactly that), and blocks are generated in reverse postorder, so whatever a
    // use names has already been built - the null answer means the IR was invalid.
    llvm::Value* use(LowerPtr<LowerValue> value) {
        auto found = values.getValue(value.offset);
        return found ? found.unwrap() : nullptr;
    }

    llvm::Value* use(LowerValue& value) {
        return use(&value - base);
    }

    llvm::BasicBlock* block(LowerPtr<LowerBlock> b) {
        return blocks[base[b]->index];
    }
};

/*
 * Types.
 */

llvm::Type* typeOf(Gen& gen, LowerType type);

// The integer type a memory access of `bytes` bytes moves. Load and store state their width in
// bytes rather than in the type of the value, so this is where the two meet.
llvm::IntegerType* intTypeOfWidth(Gen& gen, U32 bytes);

// What a function or an indirect call answers: `void` for no result, the result itself for one, and
// an anonymous struct for several. Nothing in the lower IR returns an aggregate of its own, so the
// struct is only ever the multi-result form, and the extractvalues that follow a call are the only
// place it is ever seen.
llvm::Type* resultTypeOf(Gen& gen, llvm::ArrayRef<llvm::Type*> results);

llvm::FunctionType* signatureOf(Gen& gen, llvm::ArrayRef<llvm::Type*> args, llvm::ArrayRef<llvm::Type*> results);
llvm::FunctionType* signatureOf(Gen& gen, LowerFunction& fun);

llvm::CallingConv::ID conventionOf(LowerCallType type);

inline llvm::StringRef nameOf(Context& context, StringId name) {
    if(!name) return {};

    auto text = context.findName(name);
    return { text.text(), text.size() };
}

/*
 * Instructions.
 */

// Builds one instruction into the block the builder is positioned at, defining whatever values it
// creates. Terminators included; phis are created empty beforehand and filled once every block
// exists - see genFunctionBody.
void genInst(FunGen& fun, LowerInst& inst);

// Names what an instruction produced: the value itself for one result, and one extractvalue per
// result for the several a call or an intrinsic can answer with.
void defineResults(FunGen& fun, LowerInst& inst, llvm::Value* value);

// The machine operations named directly rather than described. Answers false for one this target
// has no encoding for, having reported it.
bool genIntrinsic(FunGen& fun, LowerInstIntrinsic& inst);

// A Linux system call, which is inline assembly here for the same reason it is a fixed-register
// form in the x64 backend: the callee is the kernel, and its convention is not one LLVM knows.
llvm::Value* genSyscall(FunGen& fun, LowerInstCall& inst);

} // namespace llvmgen
