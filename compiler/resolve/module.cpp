/*
 * The containers, and the order the declaration passes run in.
 *
 * A module is read in sweeps rather than in one walk, because a declaration may name anything the
 * module holds regardless of where it was written: a record's field may be a record declared below
 * it, and an instance may be written above the class it implements. `resolveProgramDecls` is that
 * order, and it is the one thing the passes it calls are not free to decide for themselves - which
 * is why they are separate files and this is not one of them.
 */

#include "module_internal.h"
#include "../compiler/stage.h"
#include "analyze.h"
#include "const.h"
#include "core.h"
#include "expr.h"
#include "generic.h"
#include "host.h"
#include "index.h"
#include "name.h"
#include "native.h"
#include "verify.h"
#include "witness.h"
#include "../parse/ast.h"

/*
 * Declaration resolution.
 *
 * A module is read in passes because each one needs the previous to have finished for the whole
 * module: a record's constructors may name a type declared later, a class signature needs every
 * type, an instance needs its class, and a function body needs every signature. The passes are
 * listed in prepareModule() below.
 *
 * Imports are resolved depth-first before the importing module is read at all, so by the time a
 * name is looked up every module it could come from is complete.
 */

// The Nth declaration as a pointer rather than a value, so a function can keep its own AST node
// and resolve its body once every signature in the program is known.
/*
 * The modules of a program, in the order the passes walk them - see orderModules.
 *
 * Sixteen inline: a program is the library's handful plus its own, and the guess is the ordinary
 * program rather than the largest (util/README.md). Three of these are built per compilation - the
 * condensation's output, the subset still being resolved, and Tarjan's own stack - and each was one
 * allocation apiece before the bound existed.
 */
using OrderedModules = SmallArray<Module*, 16>;

ast::ParsePtr<ast::Decl> declAt(ast::DeclList decls, Size index) {
    return ast::ParsePtr<ast::Decl>(decls.list.p.offset + U32(sizeof(ast::Decl) * index));
}

/*
 * `@platform(js)` / `@platform(native)` - Analysis-JS.md §2.4, and the mechanism two implementation
 * documents already cite as though it existed.
 *
 * A target-selected declaration, and deliberately nothing more than that: an excluded declaration
 * is not resolved, so it contributes no name, no type, no instance and no body. That is what makes
 * `Storage(a)` be a host `Array` on JS and a length/capacity/address record on native without the
 * compiler knowing that `push` means `.push` - the alternative §2.4 rejects, because host knowledge
 * in codegen can neither be tested nor extended.
 *
 * Selection happens here rather than at any later stage for the same reason: a declaration the
 * target does not have must not be *resolvable* on it. A JS build that could still call the native
 * `Storage.reserve` would have two implementations of one name and would pick by accident.
 *
 * **The vocabulary is the file selector's**, which is the same question asked of a declaration
 * instead of a file - Analysis-Modules.md §2.5. `js` and `native` are the platform axis, and an
 * operating system (`linux`, `mac`, `win32`) or an architecture (`x64`, `x86`, `arm`, `arm64`) name
 * the other two, spelled the way `-target` and `-arch` spell them. `targetSelector` is the one
 * place any of that is decided, so an attribute and a file name can never disagree about what a
 * target is called, and `Linux.x64.yana`'s statement can be made about a single declaration in a
 * file that is otherwise portable.
 *
 * Multiple names inside one attribute read as **or** (`@platform(js, native)` is every target and
 * therefore pointless), and `@platform` written twice on one declaration reads as **and**, so all
 * of them have to accept. That is the one place this differs from a file name, where the segments
 * chain and therefore read as "and": a list in parentheses is a set of alternatives and a name is a
 * sequence of qualifiers, which is what each of the two spellings already looks like.
 *
 * An unknown name is reported rather than silently excluding the declaration, since a typo would
 * otherwise delete a declaration from every build.
 *
 * `report` is false for every pass after the first. A module is read in eight passes and each of
 * them asks this question of each declaration, so reporting from all of them would say the same
 * thing eight times about one attribute.
 */
static bool platformEnabled(Module& module, const ast::Decl& decl, bool report = false) {
    auto attributes = decl.attributes;
    if(attributes.isEmpty()) return true;

    auto& context = module.context;
    auto platform = context.addUnqualifiedName("platform", 8);
    auto enabled = true;

    for(auto attribute: attributes.contents(module.parse)) {
        if(attribute.name != platform) continue;

        if(attribute.args.isEmpty()) {
            if(report) {
                context.diagnostics.error("`@platform` needs at least one target - a platform (`js`, `native`), an operating system or an architecture"_v,
                                          attribute.source);
            }

            continue;
        }

        auto matched = false;
        for(auto arg: attribute.args.contents(module.parse)) {
            if(arg.value.kind != ast::Expr::Var) {
                if(report) {
                    context.diagnostics.error("a `@platform` target is a bare name - `js`, `native`, an operating system or an architecture"_v,
                                              arg.value.source);
                }

                continue;
            }

            auto name = context.findName(arg.value.var);
            auto answer = targetSelector(context.settings, StringView { name.text(), name.size() });

            if(answer == TargetSelector::Unknown) {
                if(report) {
                    context.diagnostics.error("unknown target %@ - expected `js`, `native`, an operating system (`linux`, `mac`, `win32`) or an architecture (`x64`, `x86`, `arm`, `arm64`)"_v,
                                              arg.value.source, name);
                }

                continue;
            }

            matched = matched || answer == TargetSelector::Matched;
        }

        enabled = enabled && matched;
    }

    return enabled;
}

Program::Program(Context& context, Size typeMemory, Size irMemory):
    context(context), types(typeMemory), arena(irMemory)
{
    // Sized once for the classes a program declares rather than grown into. Every rehash on the way
    // there re-probes every instance row already in the table, and the count is not a guess that has
    // to be right - it is one allocation either way, and Core alone declares most of them.
    instancesByClass.reserve(64);
}

Program::~Program() {
    for(auto module: modules) delete module;
    for(auto group: embeddedGroups) delete group;
    for(auto ast: embeddedAsts) delete ast;
    destroyAnalysisScratch(analysisScratch);
}

static Module* addModule(Program& program, StringId name) {
    auto module = new Module(program, name);
    program.modules.push(module);
    return module;
}

/*
 * One of the two modules the compiler builds itself, from a directory of `lib/`.
 *
 * The ASTs and the group are already owned by the program; this only lays a module over them. The
 * same shape as `Program::addModule` below, and separate only because the prelude is registered
 * before the program has a root to discover anything from.
 */
Module* addEmbeddedModule(Program& program, ast::ModuleGroup& group) {
    auto module = addModule(program, group.name);
    for(auto file: group.files) module->files.push(file);
    return module;
}

Module* Program::addModule(ast::ModuleGroup& group) {
    auto module = ::addModule(*this, group.name);
    for(auto file: group.files) module->files.push(file);
    return module;
}

U16 Module::fileOf(LocationId source) {
    if(source == kNullLocation || files.size() <= 1) return 0;

    auto location = context.getLocation(source);
    if(!location) return 0;

    for(U16 i = 0; i < files.size(); i++) {
        if(files[i]->name == location->sourceModule) return i;
    }

    return 0;
}

Module* Program::findModule(StringId name) {
    for(auto module: modules) {
        if(module->name == name) return module;
    }

    return nullptr;
}

Module::Module(Program& program, StringId name):
    program(program), context(program.context), types(program.types), arena(program.arena),
    scalar(program.scalar), coreClasses(program.coreClasses), name(name),
    parse(*program.context.parseRegion) {}

Function* Module::addFunction(StringId functionName, LocationId source) {
    auto found = functions.add(functionName);
    if(found.existed) {
        context.diagnostics.error("duplicate function %@"_v, source, context.findName(functionName));
        return (*arena)[*found.value];
    }

    auto function = new (arena) Function(this, functionName);
    function->source = source;
    *found.value = function - *arena;
    functionOrder.push(arena, function - *arena);

    // §1.2's declaration walk, taken here rather than in it: every named function in the program
    // arrives through this one call, including the ones a class instance and Core's own generated
    // definitions make, so there is one place rather than four.
    recordDefinition(context, functionSymbol(*this, function - *arena));

    function->addBlock(*this);
    return function;
}

Global* Module::addGlobal(StringId globalName, LocationId source) {
    auto found = globals.add(globalName);
    if(found.existed) {
        context.diagnostics.error("duplicate global %@"_v, source, context.findName(globalName));
        return (*arena)[*found.value];
    }

    auto global_ = new (arena) Global(this, globalName);
    global_->source = source;
    *found.value = global_ - *arena;
    globalOrder.push(arena, global_ - *arena);

    return global_;
}

// A function that is reachable through something other than its name - a class instance's
// implementation. It still gets a unique name so that printed and lowered output can tell two
// instances of the same method apart.
Function* addAnonymousFunction(Module& module, StringId functionName, LocationId source) {
    auto function = new (module.arena) Function(&module, functionName);
    function->source = source;
    function->anonymous = true;
    module.functionOrder.push(module.arena, function - *module.arena);
    function->addBlock(module);
    return function;
}

Block* Module::entry(Function& function) {
    return (*arena)[function.blocks.get(*arena, 0)];
}

Block* Function::addBlock(Module& module, StringId blockName) {
    auto base = *module.arena;
    auto block = new (module.arena) Block(this - base, blockName, U16(blocks.size()));

    blocks.push(module.arena, block - base);
    return block;
}

Arg* Function::addArg(Module& module, StringId argName, TypePtr type, LocationId source) {
    auto base = *module.arena;
    auto arg = new (module.arena) Arg(module.entry(*this) - base, type, U16(args.size()));

    arg->name = argName;
    arg->source = source;
    arg->id = valueCounter++;
    args.push(module.arena, arg - base);

    return arg;
}

U32 Function::addLocal(Module& module, TypePtr type, StringId localName, ModulePtr<Value> value,
                       ast::BindType convention, bool borrowed, bool closureEnv) {
    auto index = U32(locals.size());

    // By name rather than by position. A field added to the middle of `Local` used to shift every
    // one after it silently - `closureEnv` took `borrowed`'s argument once, and what it looked like
    // was closures quietly never releasing their environments.
    locals.push(module.arena, Local {
        .type = type,
        .name = localName,
        .value = value,
        .convention = convention,
        .storage = StorageClass::Stack,
        .borrowed = borrowed,
        .closureEnv = closureEnv,
    });

    // The back edge, so that "which slot is this value the contents of" is a read rather than a
    // scan of this table - see Value::slot. The assertion is the half of that pairing this cannot
    // state in a type: a value filling two slots would answer with the later one, and every reader
    // of the old scan took the earlier.
    if(value) {
        assertTrue((*module.arena)[value]->slot == maxLimit<U32>);
        (*module.arena)[value]->slot = index;
    }

    return index;
}

/*
 * Whole-module passes.
 */

/*
 * The declaration passes, each over one file.
 *
 * Separated out because they are run two ways over two sets of modules. `resolveProgramDecls` runs
 * each of them over every module of the program before starting the next, which is what permits a
 * cycle in the import graph - Analysis-Modules.md §2.2; `definePrelude` runs the same sequence over
 * Core and Native, which are a cycle of exactly that kind.
 *
 * A pass over a *module* runs its files in path order and finishes each pass for every file before
 * the next one starts, for exactly the reason the passes exist at all: within a module there are no
 * imports and no exports, so a record at the bottom of one file may be named by a signature at the
 * top of another.
 */

/*
 * One per-file pass over a module, with that file the one being read.
 *
 * The `FileScope` is the reason this is a helper rather than a loop written out twice: an import is
 * in scope for the file that wrote it, so a pass that reads a file has to say which file that is -
 * Analysis-Modules.md §2.1.2 and Module::activeFile.
 */
template<class Pass>
static void eachFile(Module& module, Pass&& pass) {
    for(U16 i = 0; i < module.files.size(); i++) {
        FileScope scope(module, i);
        pass(module, *module.files[i]);
    }
}

static void passFixities(Module& module, ast::Module& ast) {
    for(auto fixity: ast.ops.contents(module.parse)) {
        *module.operatorFixity.add(fixity.op).value = OperatorFixity {
            U8(fixity.precedence), fixity.kind == ast::Fixity::Right, true,
        };
    }
}

static void passDeclare(Module& module, ast::Module& ast) {
    auto parse = module.parse;
    auto decls = ast.decls;

    for(Size i = 0; i < decls.size(); i++) {
        auto pointer = declAt(decls, i);
        auto& decl = *parse[pointer];

        // The one pass that reports: every declaration passes through it, and it is the first.
        if(!platformEnabled(module, decl, true)) continue;

        switch(decl.kind) {
            case ast::Decl::Data:
                declareRecord(module, decl);
                break;
            case ast::Decl::Alias:
                // A qualified alias is a newtype rather than a name for another type, so it
                // declares a record and never reaches the alias table.
                if(decl.qualified) {
                    declareNewtype(module, decl);
                } else {
                    declareAlias(module, decl, pointer);
                }
                break;
            case ast::Decl::Trait:
                declareClass(module, decl, pointer);
                break;
            case ast::Decl::Fun:
            case ast::Decl::Instance:
                break;
            case ast::Decl::Stmt:
                // Deferred: a global's type may name a record declared after it.
                break;
            case ast::Decl::Error:
                // The parser has already reported why this declaration is not one. Saying
                // anything else about it puts a second diagnostic on one mistake, which for a
                // file being edited is most of the file - Implementation-Tooling.md §3.2.
                break;
            default:
                module.context.diagnostics.error("this declaration is not available yet"_v, decl.source);
                break;
        }
    }
}

static void passDefine(Module& module, ast::Module& ast) {
    for(auto decl: ast.decls.contents(module.parse)) {
        if(!platformEnabled(module, decl)) continue;
        if(decl.kind == ast::Decl::Data) defineRecord(module, decl);
        else if(decl.kind == ast::Decl::Alias && decl.qualified) defineNewtype(module, decl);
    }
}

static void passFieldDefaults(Module& module, ast::Module& ast) {
    for(auto decl: ast.decls.contents(module.parse)) {
        if(!platformEnabled(module, decl)) continue;
        if(decl.kind == ast::Decl::Data) declareRecordDefaults(module, decl);
    }
}

static void passLayoutCycles(Module& module, ast::Module& ast) {
    for(auto decl: ast.decls.contents(module.parse)) {
        if(!platformEnabled(module, decl)) continue;

        auto newtype = decl.kind == ast::Decl::Alias && decl.qualified;
        if(decl.kind != ast::Decl::Data && !newtype) continue;

        /*
         * Every content type in the module is resolved by now, which is the earliest point a
         * containment cycle can be seen at all - a record may name one declared further down.
         *
         * In *declaration order*, which is what makes mutual recursion deterministic: laying out `A`
         * first makes `B`'s reference to `A` the back edge, so reordering two declarations does
         * change which of them holds the pointer. That is the honest cost of not asking, and a
         * programmer wanting the other cut writes `@box` on the field.
         *
         * The order across files is not written down anywhere a reader can see, which is why
         * breakLayoutCycles refuses a cycle whose edges cross one - Analysis-Modules.md §2.3.
         */
        auto record = declaredRecord(module, newtype ? decl.alias.type.name : decl.data.type.name);
        if(record && !record->generic) {
            auto type = (Type*)record - *module.types;
            breakLayoutCycles(module, type, decl.source);
            checkTypeAcyclic(module, type, decl.source);
        }
    }
}

static void passGlobals(Module& module, ast::Module& ast) {
    // By index rather than by iterator, because a top-level statement is kept as the pointer to the
    // declaration it was written as - the entry sequence resolves it later.
    auto decls = ast.decls;

    for(Size i = 0; i < decls.size(); i++) {
        auto pointer = declAt(decls, i);
        auto& decl = *module.parse[pointer];
        if(!platformEnabled(module, decl)) continue;
        if(decl.kind == ast::Decl::Stmt) declareGlobal(module, decl, pointer);
    }
}

static void passClassSignatures(Module& module) {
    for(auto& entry: module.classes) {
        auto typeClass = (*module.types)[entry];

        // A member signature names types, so this reads the file the class was written in rather
        // than whichever one the loop above left active.
        FileScope scope(module, typeClass->source);
        resolveClassSignatures(module, *typeClass);
    }
}

static void passFunctionSignatures(Module& module, ast::Module& ast) {
    auto decls = ast.decls;

    for(Size i = 0; i < decls.size(); i++) {
        auto pointer = declAt(decls, i);
        auto& decl = *module.parse[pointer];
        if(decl.kind != ast::Decl::Fun) continue;
        if(!platformEnabled(module, decl)) continue;

        // Every function is resolved in an open context: a type variable in the signature is what
        // makes the function generic, and the constraints written in front of it are the ones the
        // body does not have to prove. The context is dropped again when nothing used it.
        auto env = prepareGenEnv(module, GenEnv::Function, {}, decl.fun.constraints, true);

        auto function = resolveSignature(module, decl, (*module.types)[env], decl.fun.name, false);
        function->ast = pointer;
        function->exported = decl.exported;
        readInlineAttribute(module, decl, *function);

        // Plain declarations only, and not the class-member calls to readInlineAttribute in
        // module_class.cpp and module_default.cpp. Both of these describe how a function is
        // *entered* and how its body is *encoded*, and a class member has neither to itself: what a
        // call site does at an instance member is fixed by the class signature it was selected
        // through, exactly as the argument conventions are.
        readConventionAttribute(module, decl, *function);
        readLegacySseAttribute(module, decl, *function);

        // Here rather than in the declare pass, because a prefix that names a type is only
        // answerable once every type of the module exists - see registerNamespace.
        registerNamespace(module, decl.fun.name, decl.source);

        auto& context = *(*module.types)[env];

        if(context.types.isNotEmpty()) {
            function->gen = env;
            resolveConstraintClasses(module, context);
        } else if(context.classes.isNotEmpty() || context.properties.isNotEmpty() ||
                  context.functions.isNotEmpty()) {
            // Every kind, not only the classes. A context with no variables is never instantiated,
            // so there is no site at which any of its requirements would be proved - and a
            // requirement nothing proves is decoration, which is what proveRequirements exists to
            // stop this compiler from accepting.
            module.context.diagnostics.error("%@ has constraints but no type variables"_v, decl.source,
                                             module.context.findName(decl.fun.name));
        }
    }
}

static void passInstances(Module& module, ast::Module& ast) {
    for(auto decl: ast.decls.contents(module.parse)) {
        if(!platformEnabled(module, decl)) continue;
        if(decl.kind == ast::Decl::Instance) resolveInstance(module, decl);

        // A `deriving` clause expands into instance declarations and is resolved in the same sweep,
        // in declaration order with the written ones. That is what makes the duplicate check see
        // both: an `instance Logic(OpenFlags)` beside `deriving (Logic)` is one class and one type
        // twice, and it is reported at whichever of the two the reader wrote second.
        else if(decl.kind == ast::Decl::Alias && decl.qualified) deriveNewtypeInstances(module, decl);
    }
}

/*
 * Top-level statements are permitted in at most one file of a module - Analysis-Modules.md §2.1.
 *
 * The root module runs its top-level statements in written order and that is where a program starts.
 * Across several files "written order" would have to be invented, and inventing it would make a
 * program's startup sequence depend on its filenames. Reporting the second file is better than
 * ordering it, and this could not arise while a module was a file.
 *
 * **The root module only**, which is the whole of what "a top-level statement" means: a `let` in any
 * other module is a *declaration* of a constant and nothing runs it, so several files of one module
 * may each declare some - the library's own files do. What a non-root module may not have is a
 * statement, and `declareGlobal` reports each of those where it is written rather than reporting the
 * file that holds the second one.
 */
static void checkModuleTopLevel(Module& module) {
    if(!module.root) return;

    ast::Module* found = nullptr;

    for(auto file: module.files) {
        // The first one, which is what the report points at: a file has no location of its own, and
        // the statement is the thing that has to move.
        auto statement = kNullLocation;

        for(auto decl: file->decls.contents(module.parse)) {
            if(decl.kind != ast::Decl::Stmt) continue;
            if(!platformEnabled(module, decl)) continue;

            statement = decl.source;
            break;
        }

        if(statement == kNullLocation) continue;

        if(found) {
            module.context.diagnostics.error("%@ already has this module's top-level statements - a program's start is the statements of one file in written order, and two files have no order this compiler is willing to invent"_v,
                                             statement, module.context.findName(found->name));
            return;
        }

        found = file;
    }
}

/*
 * The prelude - Analysis-Modules.md §2.4.
 *
 * Core and Native, read from `lib/Core/` and `lib/Native/`, resolved through the same passes and in
 * the same interleaving the program-wide walk uses. They import each other and that is the point:
 * an array is built out of raw pointers and the heap, and a pointer is written over `Int` and Core's
 * classes, so no order of two whole modules exists and the passes are what give the pair a meaning.
 *
 * The compiler's own contribution arrives at exactly three points, and each is forced by a pass on
 * one side of it:
 *
 * - **Before any source is read**: the five primitives and the fixed-width family, because Core's
 *   declarations are written in terms of them.
 * - **After `passDefine`**: the declarations the compiler itself names - the classes the language's
 *   syntax rests on, `Outcome`, `Array`, `Map`, `Run`, `Flat`. Lookups only, and they are here
 *   rather than later because two declaration passes need them: an `iter fn` signature is rewritten
 *   around `Outcome`, and a `[T]` parameter becomes a `Flat(T)`.
 * - **After `passFunctionSignatures` and before `passInstances`**: everything generated - the
 *   instances, the intrinsic hooks, and the functions the compiler emits calls to without a name to
 *   reach them through. After the signatures because a hook is attached to a declared function;
 *   before the instances because `deriving` forwards to one, and the platform file's newtypes over
 *   `I64` need `Bitwise(I64)` to exist.
 *
 * That is the whole of what six `define*` functions in dependency order were carrying. The order
 * *within* the third hook is still a dependency order and is the one the six had: Core's instances,
 * then Native's, then the host's, then the containers, then what a string is made of, then the
 * format primitives.
 */
void definePrelude(Program& program) {
    auto coreGroup = parsePreludeGroup(program, "Core"_v);
    auto nativeGroup = parsePreludeGroup(program, "Native"_v);

    /*
     * A library that could not be read is where a compilation stops.
     *
     * Reported above rather than here - what this adds is that the reports are the *end* of the
     * compile rather than a prelude to a crash. Every pass below assumes `Int` has a `+`, that `[a]`
     * names a type and that a string literal has somewhere to be built, and none of those is true of
     * a program whose library is absent. Reported and then resolved anyway, the first program to
     * name any of them segfaulted.
     */
    if(!coreGroup || !nativeGroup) return;

    auto core = addEmbeddedModule(program, *coreGroup);
    auto native = addEmbeddedModule(program, *nativeGroup);

    // Set before the passes run, and both of them: `resolveImports` reads `Program::core` to decide
    // the implicit import, and Core imports Native by name from its own source.
    program.core = core;
    program.native = native;

    Module* prelude[] = { core, native };

    // Which form a call site takes has to be the same answer for every call site in one compilation,
    // so the prelude is built under the setting the program will be - see resolveProgram, which sets
    // it before this runs.
    TypeList widthTypes;
    definePreludeTypes(program, *core, widthTypes);

    for(auto module: prelude) module->declState = Module::DeclState::Resolving;
    for(auto module: prelude) resolveImports(*module);

    for(auto module: prelude) eachFile(*module, passFixities);
    for(auto module: prelude) eachFile(*module, passDeclare);
    for(auto module: prelude) eachFile(*module, passDefine);
    for(auto module: prelude) eachFile(*module, passFieldDefaults);

    for(auto module: prelude) completePendingInstances(*module);

    // The second hook: the declarations the compiler itself names. Between `passDefine` and the
    // signature passes because those need what it records - an `iter fn` signature is rewritten
    // around `Outcome`, and a `[T]` parameter becomes a `Flat(T)`.
    definePreludeLookups(program, *core);
    definePreludeNativeTypes(program, *native);

    for(auto module: prelude) eachFile(*module, passLayoutCycles);
    for(auto module: prelude) eachFile(*module, passGlobals);

    for(auto module: prelude) passClassSignatures(*module);

    for(auto module: prelude) eachFile(*module, passFunctionSignatures);

    // The third hook, in the dependency order the six modules used to stand in. Before
    // `passInstances` rather than after it, because `deriving` forwards to an instance: the
    // newtypes over `I64` in the platform file need `Bitwise(I64)` to have been generated.
    definePreludeCore(program, *core, widthTypes);
    definePreludeNative(program, *native);
    definePreludeHost(program, *native);
    definePreludeContainers(program, *core);
    definePreludeNativeText(program, *native);
    definePreludeText(program, *core);

    for(auto module: prelude) eachFile(*module, passInstances);

    for(auto module: prelude) checkModuleTopLevel(*module);

    // After the hook and not before it: what these check is every instance the module has, and most
    // of Core's were generated a dozen lines above.
    for(auto module: prelude) checkModuleClasses(*module);
    for(auto module: prelude) completePendingInstances(*module);

    // Last, because what it checks is what the signatures above resolved to rather than what was
    // written: an alias has been substituted through by now, and a generic head has become the
    // declaration its instantiations point back at.
    for(auto module: prelude) checkModuleExports(*module);

    for(auto module: prelude) module->declState = Module::DeclState::Resolved;
}

/*
 * The module graph, and the order the passes walk it in - Analysis-Modules.md §2.2, §2.3.
 *
 * Discovery is separate from resolution and that is the change. An import is syntactic, so the whole
 * graph is known once every reachable file is parsed, and nothing about reading it needs any module
 * to have been resolved first. What used to happen instead was that an import resolved its target
 * before the importing module was read at all, which made the graph a spanning tree of the order the
 * imports happened to be written in - and a cycle in it a program with no meaning.
 */

// Every module reachable from `program.root`, added to the program. Silent on a module it cannot
// find: `resolveImports` reports that against the import that named it, which is where it is
// readable, and reporting from here would say it once per importer.
static void discoverModules(Program& program, ModuleProvider* provider) {
    for(Size i = 0; i < program.modules.size(); i++) {
        auto module = program.modules[i];
        if(module->declState == Module::DeclState::Resolved) continue;

        for(auto file: module->files) {
            for(auto imported: file->imports.contents(module->parse)) {
                if(imported.from == module->name) continue;
                if(program.findModule(imported.from)) continue;

                /*
                 * The project's own files first, and `lib/` second - see findLibraryModule.
                 *
                 * That order is what lets a program shadow a library module by putting a file of
                 * that name in its own source tree, which is the direction that has to work: a
                 * library is shared and a program is not, so the one that can be changed to resolve
                 * a collision is the program, and it should not have to be changed by *renaming* the
                 * module it wanted.
                 */
                auto source = provider ? provider->getModule(imported.from) : nullptr;
                if(!source) source = findLibraryModule(program, imported.from);
                if(!source) continue;

                program.addModule(*source);
            }
        }
    }
}

/*
 * Tarjan over the import graph, emitting each strongly connected component once every component it
 * points at has been emitted - which is reverse topological order of the condensation, and is the
 * order §2.3 asks for.
 *
 * Within a component the order is by module name, as text rather than by interned id: an id is a
 * hash, so ordering by it would be deterministic and would change when an unrelated name was added.
 * What the order decides is which edge of a mutually recursive type pair holds the pointer, so it
 * has to be stable under edits that have nothing to do with either.
 */
struct ModuleOrder {
    Program& program;
    OrderedModules& result;

    struct State {
        U32 index = 0;
        U32 low = 0;
        bool onStack = false;
        bool visited = false;
    };

    SmallArray<State, 16> states;
    OrderedModules stack;
    U32 counter = 1;

    U32 indexOf(Module* module) {
        for(U32 i = 0; i < program.modules.size(); i++) {
            if(program.modules[i] == module) return i;
        }

        return maxLimit<U32>;
    }

    void visit(U32 self) {
        auto module = program.modules[self];
        auto& state = states[self];

        state.visited = true;
        state.index = counter;
        state.low = counter;
        counter++;
        state.onStack = true;
        stack.push(module);

        for(auto& import: module->imports) {
            auto target = indexOf(import.module);
            if(target == maxLimit<U32>) continue;

            if(!states[target].visited) {
                visit(target);
                if(states[target].low < states[self].low) states[self].low = states[target].low;
            } else if(states[target].onStack) {
                if(states[target].index < states[self].low) states[self].low = states[target].index;
            }
        }

        if(states[self].low != states[self].index) return;

        // The component's root has finished, so everything above it on the stack is its component.
        auto start = result.size();
        while(true) {
            auto member = stack[stack.size() - 1];
            stack.pop();
            states[indexOf(member)].onStack = false;
            result.push(member);

            if(member == module) break;
        }

        // By name within the component. Insertion sort: a component is one module in every program
        // that has no cycle in it, and a handful in one that does.
        for(auto i = start + 1; i < result.size(); i++) {
            auto value = result[i];
            auto name = program.context.findName(value->name);
            auto j = i;

            while(j > start && program.context.findName(result[j - 1]->name) > name) {
                result[j] = result[j - 1];
                j--;
            }

            result[j] = value;
        }
    }
};

static void orderModules(Program& program, OrderedModules& order) {
    ModuleOrder walk { program, order };
    walk.states.reserve(program.modules.size());
    for(Size i = 0; i < program.modules.size(); i++) walk.states.push(ModuleOrder::State {});

    for(U32 i = 0; i < program.modules.size(); i++) {
        if(!walk.states[i].visited) walk.visit(i);
    }
}

void resolveProgramDecls(Program& program, ModuleProvider* provider) {
    // Every module the root reaches, interned and with its files attached. An import is syntactic,
    // so this needs nothing resolved - which is the whole reason the graph may now have a cycle in
    // it. Nothing below this line asks for a module that is not already here.
    discoverModules(program, provider);

    /*
     * Imports first and for every module, which is the one pass that has to precede the ordering
     * rather than follow it: the order is over the import graph, and `Import::module` is what an
     * edge of it is. It is also the pass that used to trigger everything below it.
     */
    for(auto module: program.modules) {
        if(module->declState == Module::DeclState::Resolved) continue;

        module->declState = Module::DeclState::Resolving;
        resolveImports(*module);
    }

    OrderedModules ordered;
    orderModules(program, ordered);

    // The prelude is built before this runs and is Resolved, so it drops out here rather than being
    // walked again - it goes through the same sequence in definePrelude, with the compiler's own
    // definitions around it. Everything else is Resolving, which the loop above just set and the
    // last line below clears.
    OrderedModules order;
    for(auto module: ordered) {
        if(module->declState == Module::DeclState::Resolving) order.push(module);
    }

    // Each pass over every module before the next one starts. This is the whole of §2.2: no pass
    // below asks whether another module has finished, only whether a name exists yet.
    for(auto module: order) eachFile(*module, passFixities);
    for(auto module: order) eachFile(*module, passDeclare);
    for(auto module: order) eachFile(*module, passDefine);
    for(auto module: order) eachFile(*module, passFieldDefaults);

    for(auto module: order) completePendingInstances(*module);

    for(auto module: order) eachFile(*module, passLayoutCycles);
    for(auto module: order) eachFile(*module, passGlobals);

    for(auto module: order) passClassSignatures(*module);

    for(auto module: order) eachFile(*module, passFunctionSignatures);
    for(auto module: order) eachFile(*module, passInstances);

    for(auto module: order) checkModuleTopLevel(*module);
    for(auto module: order) checkModuleClasses(*module);
    for(auto module: order) completePendingInstances(*module);
    for(auto module: order) checkModuleExports(*module);

    for(auto module: order) module->declState = Module::DeclState::Resolved;
}

void checkModuleClasses(Module& module) {
    // Superclasses and defaults are checked once every instance of the module exists, so that
    // neither depends on the order the declarations were written in.
    auto instanceCount = module.instances.size();
    for(Size i = 0; i < instanceCount; i++) {
        auto instance = (*module.arena)[module.instances[i]];

        FileScope scope(module, instance->source);
        checkSuperclasses(module, *instance);
    }

    // Every default this module wrote, spent whether or not anything applied the declaration -
    // see Module::defaultedContexts for why a lazily-resolved default still needs a fixed point at
    // which its diagnostics are reported.
    for(auto context: module.defaultedContexts) {
        // A GenEnv records the module it was declared in but not the position, so the file comes off
        // the first default written in it - which is what `resolveGenDefaults` reports against, and
        // a context is only in this list because it wrote one.
        auto env = (*module.types)[context];
        auto source = env->writtenDefaults.isEmpty()
            ? kNullLocation
            : env->writtenDefaults.get(*module.types, 0).source;

        FileScope scope(module, source);
        resolveGenDefaults(module, context);
    }

    // Over the classes rather than over the declarations, because a default is now written in the
    // head - so what carries one is a class and not a declaration of its own.
    for(auto& entry: module.classes) {
        FileScope scope(module, (*module.types)[entry]->source);
        resolveClassDefault(module, entry);
    }
}

Ptr<Program> resolveProgram(Context& context, ast::Module& root, ModuleProvider* provider,
                            Program::Specialization specialization) {
    ast::ModuleGroup group { .name = root.name };
    group.files.push(&root);

    return resolveProgram(context, group, provider, specialization);
}

Ptr<Program> resolveProgram(Context& context, ast::ModuleGroup& root, ModuleProvider* provider,
                            Program::Specialization specialization) {
    auto program = Ptr<Program>(new Program(context));

    // Set before anything is resolved, and never after: which form a call site takes has to be the
    // same answer for every call site in one compilation, or the two would not be comparable.
    // Core and Native are built under it too - they are where most generic code is.
    program->specialization = specialization;

    // Core and Native, resolved together - Analysis-Modules.md §2.4. Both, rather than Core alone,
    // because a partial library is a real thing to be handed: a `-lib` pointing at a directory from
    // an older tree has a `Core/` in it and may not have the rest.
    definePrelude(*program);
    if(!program->core || !program->native) return nullptr;

    auto module = program->addModule(root);
    module->root = true;
    program->root = module;

    // Every module the root reaches, resolved together and in passes - Analysis-Modules.md §2.2.
    // The prelude above is already Resolved and drops out of the walk.
    resolveProgramDecls(*program, provider);

    // Every instance the program will ever have now exists, which is what makes a type's ownership
    // answerable once and for all - see Program::declarationsComplete for what asking too early
    // cost. Nothing below adds a declaration: a body may *instantiate* a generic, and a
    // specialization inherits the head's instances rather than declaring new ones.
    program->declarationsComplete = true;

    // The program's start, ahead of every other body: the root module's top-level statements are
    // what decide the type of a dynamically initialized global, and any body may name one.
    resolveProgramEntry(*program);

    // Bodies come last, and for every module at once: a Core instance may call a function that
    // only the root module's signatures made resolvable.
    for(auto entry: program->modules) resolveModuleBodies(*entry);

    // What each module's globals owe, once every type's instances are settled - see
    // checkGlobalTeardown, and why it is a sweep rather than a check beside each initializer.
    for(auto entry: program->modules) checkGlobalTeardown(*entry);

    // A completion request stops here - Implementation-Tooling.md §8.2. Ownership and the generic
    // environments are the expensive half of a compile and they answer nothing completion asks, and
    // the answer itself was recorded while the body holding the cursor was being resolved.
    if(context.completion) return program;

    // The generic environments come before ownership, because filling them generates real
    // functions - the erased entry thunk of every class method a witness holds - and those need
    // drops inserted like any other body. They come after every body is resolved for the opposite
    // reason: a slot number is derived from a finished context, and a body collects requirements
    // while it is being resolved.
    prepareGenericCalls(*program);

    // The first of the three checkpoints - see resolve/verify.h. Here rather than only at the end
    // because what it separates is the resolver's own bookkeeping from the passes that rewrite it:
    // a use list that is already wrong before ownership runs is a body that was built wrongly, and
    // one that is wrong only afterwards is a pass.
    verifyIrProgram(*program, VerifyStage::Resolved, "after resolving every body"_v);

    // Ownership runs over the finished program rather than per module, because a generic
    // function's specializations only exist once every body that calls one has been resolved -
    // and it is the specializations, not the generic body, that get drops.
    {
        StageScope stage(CompileStage::Ownership);
        runProgramOwnership(*program);
    }

    verifyIrProgram(*program, VerifyStage::Ownership, "after inserting drops"_v);

    // Before the walk, because the walk decides which globals exist and this is one - see
    // ensureImageAnchor, which is also why it is a root there rather than something reached.
    ensureImageAnchor(*program);
    markProgramReachable(*program);
    return program;
}
