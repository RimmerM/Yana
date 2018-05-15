#pragma once

#define __STDC_CONSTANT_MACROS
#define __STDC_FORMAT_MACROS
#define __STDC_LIMIT_MACROS

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/IRBuilder.h>
#include "../../compiler/context.h"
#include "../../resolve/module.h"
#include <llvm/Support/raw_os_ostream.h>
#include <File.h>

struct LLVMTypes {
    llvm::Type* stringData;
    llvm::Type* stringRef;
};

struct Gen {
    llvm::LLVMContext* llvm;
    llvm::Module* module;
    llvm::IRBuilder<>* builder;
    LLVMTypes types;
    Arena* mem;

    Context* context;
};

struct FileStream: llvm::raw_ostream {
    static Result<File, FileError> open(const String& path, File::CreateMode mode = File::CreateAlways) {
        return File::open(path, writeAccess(), mode);
    }

    explicit FileStream(File&& file): file(forward<File>(file)) {}

    File file;
    U64 offset = 0;

    void write_impl(const char *ptr, size_t size) override {
        file.write({ptr, size});
        offset += size;
    }

    uint64_t current_pos() const override {
        return offset;
    }
};

llvm::Module* genModule(llvm::LLVMContext* llvm, Context* context, Module* module);
llvm::Module* linkModules(llvm::LLVMContext* llvm, Context* context, llvm::Module** modules, U32 count);

void printModules(llvm::LLVMContext* llvm, Context* context, Buffer<Module*> modules, const String& path);