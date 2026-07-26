#pragma once

#include "block.h"
#include "class.h"

namespace ast {
struct Module;
struct Decl;
struct Expr;
}

struct Program;
struct ExprResolver;

enum class StorageClass: U8 {
    Stack,
};

// A named storage slot in a function. `convention` and `storage` are the two halves of what
// Implementation-IR.md part 2 asks a Local to carry; both stay at their defaults until the
// ownership milestone gives the resolver something to put in them.
struct Local {
    TypePtr type = nullptr;
    StringId name = 0;
    ModulePtr<Value> value = nullptr;
    ast::BindType convention = ast::BindType::Borrow;
    StorageClass storage = StorageClass::Stack;
};

// A function whose body the resolver generates at the call site instead of calling. The
// primitive operations are the only ones today: `+` on Int has a real Function with a real body
// so that it can be printed, lowered and taken the address of, but an ordinary call to it
// expands to the one instruction it contains rather than to a call the backend would have to
// inline later.
using Intrinsic = ModulePtr<Value> (*)(ExprResolver& resolver, Buffer<ModulePtr<Value>> args,
                                       TypePtr type, LocationId source, StringId name);

struct Function {
    Function(Module* module, StringId name): module(module), name(name) {}

    Block* addBlock(Module& module, StringId name = 0);
    Arg* addArg(Module& module, StringId name, TypePtr type, LocationId source);
    U32 addLocal(Module& module, TypePtr type, StringId name, ModulePtr<Value> value);

    Local localAt(ModuleBase base, U32 index) { return locals.get(base, index); }
    Size localCount() { return locals.size(); }

    Module* module;
    StringId name;
    LocationId source = kNullLocation;
    TypePtr returnType = nullptr;
    ModuleList<ModulePtr<Arg>, false> args;
    ModuleList<ModulePtr<Block>, false> blocks;
    ModuleList<Local, false> locals;
    ast::ParsePtr<ast::Decl> ast = nullptr;

    // Set when this function implements a class signature, for diagnostics and printing.
    GlobalPtr<TypeClass> instanceOf = nullptr;
    ModuleList<TypePtr, false> instanceArgs;

    // Set when the function is generic: its type variables, and the class requirements its
    // signature declared or its body turned out to need. The body is resolved once against these
    // and specialized by cloning - see generic.h.
    GlobalPtr<GenEnv> gen = nullptr;
    ModuleList<ModulePtr<Function>, false> specializations;

    // Set on a specialization: the generic function it was cloned from, and for which types.
    ModulePtr<Function> specializationOf = nullptr;
    ModuleList<TypePtr, false> genericArgs;

    Intrinsic intrinsic = nullptr;
    U32 valueCounter = 0;
    bool resolving = false;
    bool used = false;

    // Set while this function is being cloned for one set of type arguments, so that a request to
    // clone it again for different ones is recognized as polymorphic recursion instead of
    // instantiating forever.
    bool instantiating = false;

    // A class function's declared signature. It has arguments and a return type but no body and
    // never will: it exists so that selection has something to match against, and is the one
    // kind of Function that must not reach printing or lowering.
    bool signature = false;
};

// One module made visible in another. `include`/`exclude` are the parsed symbol lists; an empty
// `include` means everything the module exports.
struct Import {
    Module* module = nullptr;
    StringId localName = 0;
    Array<StringId> include;
    Array<StringId> exclude;
    bool qualified = false;
};

struct Module {
    Module(Program& program, StringId name, ast::ParseBase parse);

    Function* addFunction(StringId name, LocationId source);
    Block* entry(Function& function);

    // True when `name` may be looked up in this module from outside it, per one import's
    // include/exclude lists. Symbol visibility is checked here so that every lookup path
    // applies the same rule.
    static bool visible(const Import& import, StringId name);

    Program& program;
    Context& context;

    // Both regions belong to the program rather than to one module: a type resolved in Core has
    // to be the same TypePtr everywhere, and a call from a user module to a Core function has to
    // name the same ModulePtr<Function> its own calls do.
    Region<GlobalRegion>& types;
    Region<ModuleRegion>& arena;
    ScalarTypes& scalar;
    CoreClasses& coreClasses;

    StringId name;
    ast::ParseBase parse;
    Array<Import> imports;

    HashMap<StringId, TypePtr> namedTypes;
    HashMap<StringId, TypeAlias> aliases;
    HashMap<StringId, ConstructorRef> constructors;
    HashMap<StringId, ModulePtr<Function>> functions;
    HashMap<StringId, GlobalPtr<TypeClass>> classes;
    HashMap<StringId, U8> operatorPrecedence;

    // Class functions and instances are scanned rather than hashed: a name may belong to several
    // classes, and an instance is found by class and argument types rather than by name. Both
    // lists are small enough that the linear scan is not worth avoiding.
    Array<ClassFunRef> classFunctions;
    Array<ModulePtr<ClassInstance>> instances;

    ModuleList<ModulePtr<Function>, false> functionOrder;

    // The module the program was asked to compile. Its functions are emitted whether or not
    // anything calls them; every other module contributes only what is reached.
    bool root = false;
};

// Supplies the parsed source of an imported module. The resolver asks for a module the first
// time an `import` names it and never twice.
struct ModuleProvider {
    virtual ~ModuleProvider() = default;
    virtual ast::Module* getModule(StringId name) = 0;
};

struct Program {
    explicit Program(Context& context, Size typeMemory = 4 * 1024 * 1024, Size irMemory = 16 * 1024 * 1024);
    ~Program();

    Module* addModule(StringId name, ast::ParseBase parse);
    Module* findModule(StringId name);

    Context& context;
    Region<GlobalRegion> types;
    Region<ModuleRegion> arena;
    ScalarTypes scalar;
    CoreClasses coreClasses;

    // Numbers the literal variables of the whole program, so that two `?n` in one diagnostic are
    // never the same name for different literals.
    U32 literalCounter = 0;

    Array<Module*> modules;
    GlobalList<GlobalPtr<TupType>> tupleTypes;

    // Instantiations created before the declaration they came from had been read, waiting for
    // their constructor contents. Drained by completePendingInstances().
    Array<GlobalPtr<RecordType>> pendingInstances;

    Module* core = nullptr;
    Module* root = nullptr;

    // Core is parsed from source embedded in the compiler, so the program owns that AST for as
    // long as anything can still resolve against it.
    ast::Module* coreAst = nullptr;
};

// Resolves `root` and everything it imports, with Core built and implicitly imported first.
Ptr<Program> resolveProgram(Context& context, ast::Module& root, ModuleProvider* provider = nullptr);

// Resolves the declarations of one already-registered module. Exposed because Core is assembled
// from both parsed source and directly generated definitions.
void resolveModuleDecls(Module& module, ast::Module& ast, ModuleProvider* provider);
bool resolveModuleBodies(Module& module);

// Checks each instance against its class's superclasses and resolves the module's `default`
// declarations. Both need every instance of the module to exist, so this runs after them - which
// for Core means after the generated instances, not after its source.
void checkModuleClasses(Module& module, ast::Module& ast);

// Resolves one function's body if it has not been resolved yet. Exposed because instantiating a
// generic function needs its body, which may belong to a module whose bodies have not been
// reached in program order.
bool resolveFunctionBody(Module& module, Function& function);

// The printed name of one instance implementation: `Num(Int).+`. Instances are not addressable by
// name in source, but every function reaching the backend needs a unique one - both the ones
// resolved from source and the ones Core generates.
StringId instanceFunctionName(Module& module, TypeClass& typeClass, Buffer<TypePtr> args, StringId method);
