/*
 * The containers, and the order the declaration passes run in.
 *
 * A module is read in sweeps rather than in one walk, because a declaration may name anything the
 * module holds regardless of where it was written: a record's field may be a record declared below
 * it, and an instance may be written above the class it implements. `resolveModuleDecls` is that
 * order, and it is the one thing the passes it calls are not free to decide for themselves - which
 * is why they are separate files and this is not one of them.
 */

#include "module_internal.h"
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
 * Multiple platforms may be listed (`@platform(js, native)` is every target and therefore
 * pointless, but `@platform` written twice on one declaration reads as "and", so all of them have
 * to accept). An unknown name is reported rather than silently excluding the declaration, since a
 * typo would otherwise delete a declaration from every build.
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
    auto js = context.addUnqualifiedName("js", 2);
    auto native = context.addUnqualifiedName("native", 6);
    auto targetIsJs = isJsMode(context.settings.mode);
    auto enabled = true;

    for(auto attribute: attributes.contents(module.parse)) {
        if(attribute.name != platform) continue;

        if(attribute.args.isEmpty()) {
            if(report) {
                context.diagnostics.error("`@platform` needs at least one target - `@platform(js)` or `@platform(native)`"_v,
                                          attribute.source);
            }

            continue;
        }

        auto matched = false;
        for(auto arg: attribute.args.contents(module.parse)) {
            if(arg.value.kind != ast::Expr::Var) {
                if(report) {
                    context.diagnostics.error("a `@platform` target is a bare name - `js` or `native`"_v,
                                              arg.value.source);
                }

                continue;
            }

            if(arg.value.var == js) {
                matched = matched || targetIsJs;
            } else if(arg.value.var == native) {
                matched = matched || !targetIsJs;
            } else if(report) {
                context.diagnostics.error("unknown platform %@ - expected `js` or `native`"_v,
                                          arg.value.source, context.findName(arg.value.var));
            }
        }

        enabled = enabled && matched;
    }

    return enabled;
}

Program::Program(Context& context, Size typeMemory, Size irMemory):
    context(context), types(typeMemory), arena(irMemory) {}

Program::~Program() {
    for(auto module: modules) delete module;
    for(auto ast: embeddedAsts) delete ast;
    destroyAnalysisScratch(analysisScratch);
}

Module* Program::addModule(StringId name, ast::ParseBase parse) {
    auto module = new Module(*this, name, parse);
    modules.push(module);
    return module;
}

Module* Program::findModule(StringId name) {
    for(auto module: modules) {
        if(module->name == name) return module;
    }

    return nullptr;
}

Module::Module(Program& program, StringId name, ast::ParseBase parse):
    program(program), context(program.context), types(program.types), arena(program.arena),
    scalar(program.scalar), coreClasses(program.coreClasses), name(name), parse(parse) {}

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

void resolveModuleDecls(Module& module, ast::Module& ast, ModuleProvider* provider, bool importsResolved) {
    auto parse = module.parse;

    // Set before the imports rather than after them, because an import resolves the module it names
    // and that module's own imports can lead back here - which is the whole of what this records.
    module.declState = Module::DeclState::Resolving;

    if(!importsResolved) resolveImports(module, ast, provider);

    for(auto fixity: ast.ops.contents(parse)) {
        *module.operatorPrecedence.add(fixity.op).value = U8(fixity.precedence);
    }

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
            case ast::Decl::Default:
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

    for(auto decl: decls.contents(parse)) {
        if(!platformEnabled(module, decl)) continue;
        if(decl.kind == ast::Decl::Data) defineRecord(module, decl);
        else if(decl.kind == ast::Decl::Alias && decl.qualified) defineNewtype(module, decl);
    }

    for(auto decl: decls.contents(parse)) {
        if(!platformEnabled(module, decl)) continue;
        if(decl.kind == ast::Decl::Data) declareRecordDefaults(module, decl);
    }

    completePendingInstances(module);

    for(auto decl: decls.contents(parse)) {
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
         */
        auto record = declaredRecord(module, newtype ? decl.alias.type.name : decl.data.type.name);
        if(record && !record->generic) {
            auto type = (Type*)record - *module.types;
            breakLayoutCycles(module, type, decl.source);
            checkTypeAcyclic(module, type, decl.source);
        }
    }

    for(auto decl: decls.contents(parse)) {
        if(!platformEnabled(module, decl)) continue;
        if(decl.kind == ast::Decl::Stmt) declareGlobal(module, decl);
    }

    for(auto& entry: module.classes) {
        resolveClassSignatures(module, *(*module.types)[entry]);
    }

    for(Size i = 0; i < decls.size(); i++) {
        auto pointer = declAt(decls, i);
        auto& decl = *parse[pointer];
        if(decl.kind != ast::Decl::Fun) continue;
        if(!platformEnabled(module, decl)) continue;

        // Every function is resolved in an open context: a type variable in the signature is what
        // makes the function generic, and the constraints written in front of it are the ones the
        // body does not have to prove. The context is dropped again when nothing used it.
        auto env = prepareGenEnv(module, GenEnv::Function, {}, decl.fun.constraints, true);

        auto function = resolveSignature(module, decl, (*module.types)[env], decl.fun.name, false);
        function->ast = pointer;
        readInlineAttribute(module, decl, *function);

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

    for(auto decl: decls.contents(parse)) {
        if(!platformEnabled(module, decl)) continue;
        if(decl.kind == ast::Decl::Instance) resolveInstance(module, decl);
    }

    // Core's instances are generated after its source has been read, so it runs this pass itself
    // once they exist rather than here (see defineCore).
    if(&module != module.program.core) checkModuleClasses(module, ast);

    completePendingInstances(module);

    module.declState = Module::DeclState::Resolved;
}

void checkModuleClasses(Module& module, ast::Module& ast) {
    // Superclasses and defaults are checked once every instance of the module exists, so that
    // neither depends on the order the declarations were written in.
    auto instanceCount = module.instances.size();
    for(Size i = 0; i < instanceCount; i++) {
        checkSuperclasses(module, *(*module.arena)[module.instances[i]]);
    }

    auto decls = ast.decls;
    for(auto decl: decls.contents(module.parse)) {
        if(!platformEnabled(module, decl)) continue;
        if(decl.kind == ast::Decl::Default) resolveDefault(module, decl);
    }
}

Ptr<Program> resolveProgram(Context& context, ast::Module& root, ModuleProvider* provider,
                            Program::Specialization specialization) {
    auto program = Ptr<Program>(new Program(context));

    // Set before anything is resolved, and never after: which form a call site takes has to be the
    // same answer for every call site in one compilation, or the two would not be comparable.
    // Core, Native and Collections are built under it too - they are where most generic code is.
    program->specialization = specialization;

    /*
     * The order is the dependency order and every step of it is load-bearing - see §17 of
     * Implementation-Simplification.md, and Program::nativeText for the cycle the last two break.
     *
     * `Text` is last because it is implicitly imported, and a module built before it is one that
     * never receives that import: `resolveImports` reads `program.text`, which is null until the
     * line below has run. That is what keeps Collections and NativeText - both of which Text
     * imports - from importing it back.
     */
    defineCore(*program);
    defineNative(*program);
    defineHost(*program);
    defineCollections(*program);
    defineNativeText(*program);
    defineText(*program);

    auto module = program->addModule(root.name, *root.region);
    module->root = true;
    program->root = module;

    resolveModuleDecls(*module, root, provider);

    // Bodies come last, and for every module at once: a Core instance may call a function that
    // only the root module's signatures made resolvable.
    for(auto entry: program->modules) resolveModuleBodies(*entry);

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
    runProgramOwnership(*program);

    verifyIrProgram(*program, VerifyStage::Ownership, "after inserting drops"_v);

    markProgramReachable(*program);
    return program;
}
