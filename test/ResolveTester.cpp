#include <Core.h>
#include <File.h>
#include <cstdlib>
#include <cstring>
#include "../compiler/parse/parser.h"
#include "../compiler/resolve/lower.h"
#include "../compiler/resolve/print.h"
#include "../compiler/lower/lower_print.h"
#include "../compiler/lower/lower_validate.h"
#include "../compiler/codegen/x64/gen.h"
#include "Net/Stream.h"
#include "Net/File.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace Tritium;

struct TestProvider: SourceProvider {
    StringView source;
    Context* context = nullptr;

    StringView getSource(StringId) override { return source; }
    const Location* getNode(LocationId id) override {
        return context ? context->getLocation(id) : nullptr;
    }
};

static bool compareText(const String& path, ByteBuffer actual) {
    auto opened = File::openFile(path, readAccess());
    if(opened.isErr()) {
        println("cannot open file %@: error %@", path, (U32)opened.unwrapErr());
        return false;
    }

    auto file = opened.moveUnwrapOk();
    auto size = file.size();
    Ptr<Byte, HeapDeleter> expected { (Byte*)hAlloc(size) };
    file.read({ expected.get(), size });

    auto equal = size == actual.length && compareMem(expected.get(), actual.ptr, size) == 0;
    if(!equal) {
        println("Fail (%@). Got:", path);
        print(StringView { (const char*)actual.ptr, actual.length });
        println("\nExpected:");
        print(StringView { (const char*)expected.get(), size });
        println("");
    }
    return equal;
}

template<class Print>
static void writeText(const String& path, Print&& printValue) {
    try {
        Net::FileStream file;
        file.open(path, writeAccess(), File::CreateAlways);
        Net::Writer writer(Net::WriteStream(file), 16384);
        printValue(writer);
        writer.flush();
    } catch(const Net::Exception& error) {
        logError("cannot create expect file \"%@\": %@", path, error.description);
    }
}

static Maybe<I64> readExpectedRun(const String& path) {
    auto info = File::info(path);
    if(!info || info.unwrapOk().isDirectory) return Nothing();

    auto opened = File::openFile(path, readAccess());
    if(opened.isErr()) return Nothing();
    auto file = opened.moveUnwrapOk();
    auto size = file.size();
    Ptr<char, HeapDeleter> text { (char*)hAlloc(size + 1) };
    file.read({ (Byte*)text.get(), size });
    text.get()[size] = 0;

    char* end = nullptr;
    auto value = strtoll(text.get(), &end, 10);
    return end == text.get() ? Nothing() : Just(I64(value));
}

static Maybe<I64> executeMain(Context& context, Module& resolved, LowerModule& module) {
#if defined(__x86_64__) && (defined(__unix__) || defined(__APPLE__))
    auto base = *module.arena;
    AsmModule assembly;

    for(auto functionPointer: module.functions) {
        auto function = base[functionPointer];
        MachineFunction machine;
        transformFunction(context, base, *function, machine);
        auto registers = allocateRegisters(context, base, *function, machine);
        genFunction(context, base, assembly, *function, machine, registers);
    }
    assembly.resolveRelocations();

    auto mainName = Context::nameHash("main", 4);
    auto foundMain = module.functions.get(mainName);
    if(!foundMain) return Nothing();
    auto mainFunction = base[foundMain.unwrap()];
    auto offset = assembly.functionOffsets.getValue(mainFunction);
    if(!offset) return Nothing();

    auto byteCount = assembly.buffer.offset();
    auto page = Size(sysconf(_SC_PAGESIZE));
    auto allocationSize = (byteCount + page - 1) & ~(page - 1);
    auto memory = mmap(nullptr, allocationSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(memory == MAP_FAILED) return Nothing();

    copy(assembly.buffer.buffer, (Byte*)memory, byteCount);
    if(mprotect(memory, allocationSize, PROT_READ | PROT_EXEC) != 0) {
        munmap(memory, allocationSize);
        return Nothing();
    }

    auto resolvedMain = resolved.functions.get(mainName);
    if(!resolvedMain) {
        munmap(memory, allocationSize);
        return Nothing();
    }
    auto returnType = (*resolved.types)[(*resolved.arena)[resolvedMain.unwrap()]->returnType];

    I64 result;
    auto address = (Byte*)memory + offset.unwrap();
    if(returnType->kind == Type::Int &&
       ((IntType*)returnType)->width == IntType::Long) {
        result = ((I64 (*)())address)();
    } else {
        result = ((I32 (*)())address)();
    }
    munmap(memory, allocationSize);
    return Just(result);
#else
    (void)context;
    (void)resolved;
    (void)module;
    return Nothing();
#endif
}

static bool runTest(const String& path, StringView source, bool generate) {
    TestProvider provider;
    provider.source = source;
    PrintDiagnostics diagnostics(provider);
    Context context(diagnostics);
    provider.context = &context;

    Lexer lexer(context, diagnostics, source);
    Parser parser(context, lexer, context.addUnqualifiedName("ResolveTest", 11));
    auto ast = parser.parseModule();
    auto module = resolveModule(context, ast);
    if(diagnostics.errorCount()) {
        println("Fail (%@): resolver produced %@ diagnostics.", path, diagnostics.errorCount());
        return false;
    }

    auto resolvePath = path + String(".resolve.expect");
    auto lowerPath = path + String(".lower.expect");

    if(generate) {
        writeText(resolvePath, [&](Net::Writer& writer) {
            printModule(writer, context, *module);
        });
    }

    Net::Writer resolveWriter(16384);
    printModule(resolveWriter, context, *module);
    auto pass = compareText(resolvePath, resolveWriter.getBuffered());

    auto lowered = lowerModule(context, *module);
    if(!validateLowerModule(&diagnostics, lowered.get())) {
        println("Fail (%@): lowering produced invalid lower IR.", path);
        return false;
    }
    if(generate) {
        writeText(lowerPath, [&](Net::Writer& writer) {
            printModule(writer, context, *lowered->arena, *lowered);
        });
    }

    Net::Writer lowerWriter(16384);
    printModule(lowerWriter, context, *lowered->arena, *lowered);
    pass = compareText(lowerPath, lowerWriter.getBuffered()) && pass;

    auto runPath = path + String(".run.expect");
    if(auto expected = readExpectedRun(runPath)) {
        auto actual = executeMain(context, *module, *lowered);
        if(!actual || actual.unwrap() != expected.unwrap()) {
            println("Fail (%@): amd64 main returned %@, expected %@.", path,
                    actual ? actual.unwrap() : I64(-1), expected.unwrap());
            pass = false;
        }
    }

    println("Running test \"%@\"... %@", path, pass ? "Pass."_v : "Fail."_v);
    return pass;
}

int main(int argc, const char** argv) {
    auto generate = argc > 1 && String(argv[1]) == "generate";
    Array<String> tests;

    listDirectory("resolve", [&](const String& name, bool directory) {
        if(directory) return;
        if(auto dot = findLastChar(stringView(name), '.')) {
            if(String(dot + 1, name.text() + name.size() - dot - 1) == "yana") {
                tests.push(String("resolve/") + name);
            }
        }
    });

    if(tests.isEmpty()) {
        println("no resolve tests found");
        return 1;
    }

    auto pass = true;
    for(auto& test: tests) {
        auto opened = File::openFile(test, readAccess());
        if(opened.isErr()) {
            println("cannot open file %@: error %@", test, (U32)opened.unwrapErr());
            pass = false;
            continue;
        }

        auto file = opened.moveUnwrapOk();
        auto size = file.size();
        Ptr<char, HeapDeleter> source { (char*)hAlloc(size) };
        file.read({ (Byte*)source.get(), size });
        pass = runTest(test, { source.get(), size }, generate) && pass;
    }

    return pass ? 0 : 1;
}
