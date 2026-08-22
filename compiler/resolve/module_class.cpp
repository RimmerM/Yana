/*
 * Classes and instances.
 *
 * A class declares signatures and an instance supplies bodies for them, and the whole of the
 * difficulty is that neither is complete on its own: a signature is generic over the class's
 * variables, an instance binds them, and what the instance must then provide is that signature
 * substituted. Superclass obligations are the same shape one level up - an instance of a class
 * whose superclass is unimplemented is an error against the instance, not against the class.
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

void declareClass(Module& module, ast::Decl& decl, ast::ParsePtr<ast::Decl> pointer) {
    auto found = module.classes.add(decl.trait.type.name);
    if(found.existed) {
        module.context.diagnostics.error("duplicate class %@"_v, decl.source,
                                         module.context.findName(decl.trait.type.name));
        return;
    }

    auto variables = decl.trait.type.kind;
    auto env = prepareGenEnv(module, GenEnv::Class, variables, decl.trait.constraints);
    auto typeClass = new (module.types) TypeClass(decl.trait.type.name, env);

    typeClass->module = &module;
    typeClass->ast = pointer;
    typeClass->source = decl.source;
    typeClass->exported = decl.exported;
    *found.value = typeClass - *module.types;

    recordDefinition(module.context, classSymbol(module, typeClass - *module.types));

    if((*module.types)[env]->types.isEmpty()) {
        module.context.diagnostics.error("class %@ must take at least one type argument"_v, decl.source,
                                         module.context.findName(decl.trait.type.name));
    }

    /*
     * The functional dependency, if the head wrote one.
     *
     * Both halves have to be non-empty. `Class(-> a)` would say that one instance serves the whole
     * program, which is a different promise from "these decide those" and is not what this records;
     * `Class(a ->)` says nothing at all. Neither is silently ignored, because a head that means
     * less than it appears to is worse than one that does not parse.
     */
    if(auto determined = decl.trait.type.determined) {
        auto arity = U16((*module.types)[env]->types.size());

        if(determined >= arity) {
            module.context.diagnostics.error("the `->` in class %@ leaves nothing after it - write the parameters the earlier ones determine, or drop the arrow"_v,
                                             decl.source, module.context.findName(decl.trait.type.name));
        } else {
            typeClass->determined = determined;
        }
    }

    U16 index = 0;
    auto decls = decl.trait.decls;

    for(auto member: decls.contents(module.parse)) {
        if(member.kind != ast::Decl::Fun) {
            module.context.diagnostics.error("a class body may only contain function signatures"_v, member.source);
            continue;
        }

        // Two functions of one class may share a name when their arities differ. That is what
        // lets `Num` declare both the binary and the unary `-`, which is the shape the language
        // already has whether or not the class machinery accommodates it.
        for(auto existing: typeClass->functions.contents(*module.types)) {
            if(existing.name != member.fun.name) continue;
            if(existing.arity != U16(member.fun.args.size())) continue;

            module.context.diagnostics.error("duplicate class function %@"_v, member.source,
                                             module.context.findName(member.fun.name));
        }

        typeClass->functions.push(module.types, ClassFun { member.fun.name, nullptr, index, U16(member.fun.args.size()) });
        module.classFunctions.push(ClassFunRef { typeClass - *module.types, member.fun.name, index });

        // The signature's own line rather than the class's, so that jumping to `show` lands on the
        // declaration of `show` - see classFunSymbol, which prefers the signature when it exists.
        auto entry = classFunSymbol(module, typeClass - *module.types, index);
        entry.definition = member.source;
        recordDefinition(module.context, entry);

        index++;
    }
}

void resolveClassSignatures(Module& module, TypeClass& typeClass) {
    if(typeClass.ready) return;
    typeClass.ready = true;

    auto env = (*module.types)[typeClass.gen];
    resolveConstraintClasses(module, *env);

    auto& decl = *module.parse[typeClass.ast];
    auto decls = decl.trait.decls;
    Size index = 0;

    for(Size memberIndex = 0; memberIndex < decls.size(); memberIndex++) {
        auto pointer = declAt(decls, memberIndex);
        auto& member = *module.parse[pointer];

        if(member.kind != ast::Decl::Fun) continue;
        if(index >= typeClass.functions.size()) break;

        if(!member.fun.ret) {
            module.context.diagnostics.error("a class function requires an explicit return type"_v, member.source);
        }

        auto signature = resolveSignature(module, member, env, member.fun.name, true, true);
        signature->instanceOf = (TypeClass*)&typeClass - *module.types;
        signature->signature = true;

        auto stored = typeClass.functions.get(*module.types, index);
        stored.fun = signature - *module.arena;

        /*
         * A default for a `lens fn` or `iter fn` member is not available.
         *
         * A default is a generic function over the *class's* variables, and the desugaring a lens or
         * an iterator needs adds one the class does not have - so the body would be resolved against
         * a continuation nothing in the class's context names. The instance is where such a body
         * belongs, and it is one line there: Implementation-Containers.md §5's contiguous container
         * writes `iter fn chunks(self: C) -> &[a] = yield elements(self)` in its `Chunked` instance.
         */
        if(member.fun.body && signature->funKind != ast::FunKind::Plain) {
            module.context.diagnostics.error("a class member declared %@ cannot have a default body - the continuation it hands over to would be a type variable the class head does not declare, so write the body in each instance instead"_v,
                                             member.source,
                                             signature->funKind == ast::FunKind::Iter ? "`iter fn`"_v : "`lens fn`"_v);
        } else if(member.fun.body) {
            stored.defaultFun = resolveClassDefault(module, typeClass, member, pointer, *signature);
        }

        typeClass.functions.set(*module.types, index, stored);
        index++;
    }

    checkDefaultRanks(module, typeClass);
}

/*
 * Instances.
 */

StringId instanceFunctionName(Module& module, TypeClass& typeClass, Buffer<TypePtr> args, StringId method) {
    Scratch<StringBuilder> held(module.program.names);
    auto& text = *held;

    text << module.context.findName(typeClass.name) << '(';
    describeTypes(module.context, *module.types, args, text);
    text << ")." << module.context.findName(method);

    return module.context.addQualifiedName(text.pointer(), text.size(), 1);
}

// Whether one variable of a generic context is reachable inside a type. Instance selection binds
// by variable index, so occurrence is asked by index too rather than by identity.
bool mentionsVariable(GlobalBase global, TypePtr type, U16 index) {
    if(!isGeneric(global, type)) return false;

    switch(global[type]->kind) {
        case Type::Gen:
            return ((GenType*)global[type])->index == index;
        case Type::Ptr:
            return mentionsVariable(global, ((PtrType*)global[type])->to, index);
        case Type::Array: {
            // The count is a child like any other - the `n` of `[a *n]` occurs in the type exactly
            // as `a` does. See Implementation-Const-Generics.md §2.3.
            auto array = (ArrayType*)global[type];
            return mentionsVariable(global, array->content, index) ||
                   mentionsVariable(global, array->count, index);
        }
        case Type::Vector: {
            auto vector = (VectorType*)global[type];
            return mentionsVariable(global, vector->content, index) ||
                   mentionsVariable(global, vector->count, index);
        }
        case Type::Tup: {
            auto tuple = (TupType*)global[type];

            for(Size i = 0; i < tuple->fields.size(); i++) {
                if(mentionsVariable(global, tuple->fields.get(global, i).type, index)) return true;
            }

            return false;
        }
        case Type::Record: {
            auto record = (RecordType*)global[type];

            for(auto arg: record->instanceArgs.contents(global)) {
                if(mentionsVariable(global, arg, index)) return true;
            }

            return false;
        }
        default:
            return false;
    }
}

/*
 * One instance declaration.
 *
 * The head is resolved in an open generic context of its own, so `instance Ord(Ptr(a))` introduces
 * `a` by using it exactly as a function signature does. A head that used no variable leaves the
 * context empty and the instance is the concrete one it always was; a head that used one is
 * selected by matching instead of by equality (see matchInstance), and each of its implementations
 * becomes a generic function over that context, specialized for what a selection bound.
 */
void resolveInstance(Module& module, ast::Decl& decl) {
    auto& type = decl.instance.type;
    StringId className {};
    TypeList args;

    // The constraints are read first, since they name the variables the head is written over -
    // and a constraint may name one the head does not, which is what the check below reports.
    auto genPointer = prepareGenEnv(module, GenEnv::Instance, {}, decl.instance.constraints, true);
    auto gen = (*module.types)[genPointer];

    if(type.kind == ast::Type::App) {
        auto& app = *module.parse[type.app];
        if(app.base.kind != ast::Type::Con) {
            module.context.diagnostics.error("an instance must name a class"_v, decl.source);
            return;
        }

        className = app.base.name;
        auto appArgs = app.args;
        for(auto arg: appArgs.contents(module.parse)) args.push(resolveType(module, arg, gen));
    } else {
        module.context.diagnostics.error(
            "type-namespaced instances are not available yet - write `instance Class(Type)`"_v, decl.source);
        return;
    }

    auto classPointer = findClass(module, className, decl.source);
    if(!classPointer) {
        module.context.diagnostics.error("unknown class %@"_v, decl.source, module.context.findName(className));
        return;
    }

    // The two implicit classes have no instances to write: whether a type is TrivialCopy is decided
    // by whether every one of its members is, and an instance saying otherwise would be a claim the
    // compiler has already contradicted. What a type that needs different behaviour writes is
    // `Copy` or `Sink`, which is exactly what those are for.
    if(classPointer == module.coreClasses.trivialCopy || classPointer == module.coreClasses.trivialSink) {
        module.context.diagnostics.error("%@ is decided structurally and has no instances to write - a type that cannot be duplicated or relocated bitwise says so by writing `Copy` or `Sink`"_v,
                                         decl.source, module.context.findName(className));
        return;
    }

    auto typeClass = (*module.types)[classPointer];
    resolveClassSignatures(*typeClass->module, *typeClass);

    auto expected = (*module.types)[typeClass->gen]->types.size();
    if(args.size() != expected) {
        module.context.diagnostics.error("class %@ takes %@ arguments but this instance gives %@"_v, decl.source,
                                         module.context.findName(className), U32(expected), U32(args.size()));
        return;
    }

    // A variable the head does not mention is one nothing could ever bind, so a constraint over it
    // says something no selection can check.
    for(auto variable: gen->types.contents(*module.types)) {
        auto index = (*module.types)[variable]->index;
        auto mentioned = args.contains([&](TypePtr arg) { return mentionsVariable(*module.types, arg, index); });
        if(mentioned) continue;

        module.context.diagnostics.error("type variable %@ does not appear in the instance head"_v, decl.source,
                                         module.context.findName((*module.types)[variable]->name));
        return;
    }

    // Closed once the head has been read: from here on a lowercase name in the body means one of
    // the variables the head introduced, and anything else is the error it would be in a function.
    gen->open = false;
    resolveConstraintClasses(module, *gen);
    auto parametric = gen->types.isNotEmpty();

    auto instance = new (module.arena) ClassInstance(classPointer);
    instance->module = &module;
    instance->source = decl.source;
    instance->gen = parametric ? genPointer : nullptr;
    for(auto arg: args) instance->forTypes.push(module.arena, arg);
    for(Size i = 0; i < typeClass->functions.size(); i++) instance->functions.push(module.arena, nullptr);

    // Two instances one of which is no more specific than the other would make selection depend on
    // declaration order, whether they are written for the same types or for heads that mean the
    // same thing - `Eq(Ptr(a))` twice, under two names for `a`.
    // Borrowed rather than declared, for the same reason matchInstanceAt borrows it: this collects
    // every instance of the class in the whole program, and does it once per instance declaration.
    Scratch<Array<ModulePtr<ClassInstance>>> existing(module.program.instanceCandidates);
    findInstances(module, classPointer, *existing);

    for(auto other: *existing) {
        auto& previous = *(*module.arena)[other];

        if(instanceCovers(module, previous, *instance) && instanceCovers(module, *instance, previous)) {
            module.context.diagnostics.error("duplicate instance of %@ for these types"_v, decl.source,
                                             module.context.findName(className));
            return;
        }

        // The functional dependency the class declared. Two instances that agree on what determines
        // and disagree on what is determined are not duplicates and neither covers the other, so
        // this is the only thing that rejects them - and a call that leaves the determined position
        // open would otherwise be answered by whichever was declared first.
        if(!breaksDependency(module, previous, *instance)) continue;

        StringBuilder determining;
        auto prefix = Buffer<TypePtr> { args.pointer(), typeClass->determined };
        describeTypes(module.context, *module.types, prefix, determining);

        module.context.diagnostics.error("this instance of %@ disagrees with an existing one about what (%@) determines - the class head's `->` promises one answer, so the two cannot both hold"_v,
                                         decl.source, module.context.findName(className), determining.view());
        return;
    }

    auto decls = decl.instance.decls;
    for(Size memberIndex = 0; memberIndex < decls.size(); memberIndex++) {
        auto memberPointer = declAt(decls, memberIndex);
        auto& member = *module.parse[memberPointer];

        if(member.kind != ast::Decl::Fun) {
            module.context.diagnostics.error("an instance body may only contain function definitions"_v, member.source);
            continue;
        }

        Size index = maxLimit<Size>;
        for(Size i = 0; i < typeClass->functions.size(); i++) {
            auto entry = typeClass->functions.get(*module.types, i);
            if(entry.name != member.fun.name || entry.arity != U16(member.fun.args.size())) continue;

            index = i;
            break;
        }

        if(index == maxLimit<Size>) {
            module.context.diagnostics.error("class %@ has no function %@"_v, member.source,
                                             module.context.findName(className),
                                             module.context.findName(member.fun.name));
            continue;
        }

        if(instance->functions.get(*module.arena, index)) {
            module.context.diagnostics.error("duplicate implementation of %@"_v, member.source,
                                             module.context.findName(member.fun.name));
            continue;
        }

        auto signature = (*module.arena)[typeClass->functions.get(*module.types, index).fun];
        if(!signature) continue;

        // The implementation's signature is the class signature with the instance's types
        // substituted in, so an instance body does not have to repeat types the class already
        // fixed. Anything the source does write is checked against it rather than replacing it.
        auto function = addAnonymousFunction(
            module, instanceFunctionName(module, *typeClass, toBuffer(args), member.fun.name), member.source);

        function->instanceOf = classPointer;
        function->gen = instance->gen;
        for(auto arg: args) function->instanceArgs.push(module.arena, arg);
        function->returnType = substituteType(module, signature->returnType, toBuffer(args), member.source);
        function->ast = nullptr;

        auto declaredArgs = member.fun.args.contents(module.parse);
        if(declaredArgs.size() != signature->args.size()) {
            module.context.diagnostics.error("%@ takes %@ arguments in class %@, but %@ here"_v, member.source,
                                             module.context.findName(member.fun.name), U32(signature->args.size()),
                                             module.context.findName(className), U32(declaredArgs.size()));
            continue;
        }

        for(Size i = 0; i < signature->args.size(); i++) {
            auto classArg = (*module.arena)[signature->args.get(*module.arena, i)];
            auto expectedType = substituteType(module, classArg->type, toBuffer(args), member.source);
            auto declared = declaredArgs[i];

            // What the implementation writes is the *declared* type, which for a `@lazy` parameter
            // is not the type the parameter has: the thunk is what arrives, and `T` is what the
            // signature says. Both are needed here - one to check what was written, one to build
            // the parameter with.
            auto expectedDeclared = classArg->isLazy()
                ? substituteType(module, classArg->lazyType, toBuffer(args), member.source)
                : expectedType;

            if(declared.type) {
                auto written = resolveType(module, *module.parse[declared.type], gen);
                if(!sameType(written, expectedDeclared)) {
                    module.context.diagnostics.error("argument %@ has type %@ here but %@ in class %@"_v, declared.source,
                                                     module.context.findName(declared.name),
                                                     describeType(module.context, *module.types, written),
                                                     describeType(module.context, *module.types, expectedDeclared),
                                                     module.context.findName(className));
                }
            }

            // A default is decided by the class signature for the same reason strictness is: what a
            // call site may leave out is read off the class before it knows which instance it
            // reached, so one written here would be a constant nothing ever passes.
            if(declared.def) {
                module.context.diagnostics.error("argument %@ cannot have a default value in an instance - which arguments a call site may leave out is fixed by class %@, because it is read off the class signature before the instance is selected"_v,
                                                 declared.source, module.context.findName(declared.name),
                                                 module.context.findName(className));
            }

            // Strictness is as much of the contract as the convention is, and for a sharper reason:
            // the call site decides what to evaluate from the *class* signature, before it knows
            // which instance it reached. An implementation that dropped the marker would be handed
            // an argument nobody evaluated, and one that added a marker the class did not declare
            // would be handed a value where it expected a thunk.
            if(declared.lazy && !classArg->isLazy()) {
                module.context.diagnostics.error("argument %@ is `@lazy` here but not in class %@ - which arguments are evaluated is fixed by the class signature, because a call site decides it before it knows which instance it reached"_v,
                                                 declared.source, module.context.findName(declared.name),
                                                 module.context.findName(className));
            } else if(!declared.lazy && classArg->isLazy()) {
                module.context.diagnostics.error("argument %@ is `@lazy` in class %@ and has to say so here too - the call site left it unevaluated, so this body would be handed a thunk it never runs"_v,
                                                 declared.source, module.context.findName(declared.name),
                                                 module.context.findName(className));
            }

            // The convention is as much of the contract as the type is: an instance whose `drop`
            // borrows what the class said it consumes would be called by every generic caller as
            // though the value were gone. It is taken from the class rather than from what the
            // implementation wrote, and disagreeing is reported the same way a type does.
            if(declared.bind != classArg->convention) {
                module.context.diagnostics.error("argument %@ is declared %@ here but %@ in class %@"_v, declared.source,
                                                 module.context.findName(declared.name),
                                                 conventionName(declared.bind), conventionName(classArg->convention),
                                                 module.context.findName(className));
            }

            auto implArg = function->addArg(module, declared.name ? declared.name : classArg->name,
                                            expectedType, declared.source);
            implArg->convention = classArg->convention;
            implArg->returnRoot = classArg->returnRoot;
            if(classArg->isLazy()) implArg->lazyType = expectedDeclared;
        }

        if(member.fun.ret) {
            auto written = resolveType(module, *module.parse[member.fun.ret], gen);

            /*
             * A mutable borrow has no written form.
             *
             * `&mut T` is what the `return &` markers *produce* - applyReturnRoots promotes the
             * declared `&T` once every root is mutable - and the grammar has no way to spell it. So
             * an implementation of `fn getMut(return &self: c, index: k) -> &v` writes `-> &a`, and
             * comparing that against the class's already-promoted `&mut a` would ask the author for
             * a type no program can write. Promoting what was written by the class's own rule is
             * what makes the two comparable; a member with no root, or a mixed group, is unaffected
             * because applyReturnRootMutability is then the identity.
             */
            if(signature->returnRoots && signature->returnRootsMutable) {
                written = applyReturnRootMutability(module, written, true);
            }

            if(!sameType(written, function->returnType)) {
                module.context.diagnostics.error("%@ returns %@ here but %@ in class %@"_v, member.source,
                                                 module.context.findName(member.fun.name),
                                                 describeType(module.context, *module.types, written),
                                                 describeType(module.context, *module.types, function->returnType),
                                                 module.context.findName(className));
            }
        }

        /*
         * The desugaring of a `lens fn` or `iter fn` implementation - Implementation-Containers.md
         * §5's `instance Chunked(Array(a), a)`.
         *
         * The class declared the written shape and stopped there (see resolveSignature), so this is
         * where the continuation parameter is synthesized and where the result becomes the step
         * signal. It needs a context of its own for the continuation's result variable: the
         * instance's own is closed by now and is the one selection binds by index, and appending to
         * it would give the head a position no selection fills.
         *
         * The instance's variables go in first and at their own indices, so the argument types
         * already built over them keep meaning what they meant; the constraints come along for the
         * same reason a class default carries its class's, which is that the body may call what the
         * head requires. `$r` lands after them, and every call site infers it from the continuation
         * it passes - which is exactly how a free-standing `iter fn` over one variable works.
         */
        if(signature->funKind != ast::FunKind::Plain) {
            if(member.fun.kind != signature->funKind) {
                module.context.diagnostics.error("%@ is declared %@ in class %@ and has to say so here too"_v,
                                                 member.source, module.context.findName(member.fun.name),
                                                 signature->funKind == ast::FunKind::Iter ? "`iter fn`"_v : "`lens fn`"_v,
                                                 module.context.findName(className));
            }

            auto memberEnv = new (module.types) GenEnv(GenEnv::Function);
            memberEnv->open = true;

            for(auto variable: gen->types.contents(*module.types)) memberEnv->types.push(module.types, variable);

            for(auto constraint: gen->classes.contents(*module.types)) {
                ClassConstraint copied;
                copied.typeClass = constraint.typeClass;
                copied.name = constraint.name;
                copied.source = constraint.source;
                for(auto arg: constraint.args.contents(*module.types)) copied.args.push(module.types, arg);
                memberEnv->classes.push(module.types, copied);
            }

            function->funKind = signature->funKind;
            resolveLensSignature(module, *function, memberEnv, member);
            memberEnv->open = false;
            function->gen = memberEnv - *module.types;

            // The continuation this desugaring just synthesized is one a deferred dispatch has to
            // assume the extent of, because it has no body to read - see Function::classContinuation.
            function->classContinuation = function->yieldForm;
        }

        function->ast = memberPointer;
        readInlineAttribute(module, member, *function);
        instance->functions.set(*module.arena, index, function - *module.arena);
    }

    for(Size i = 0; i < typeClass->functions.size(); i++) {
        if(instance->functions.get(*module.arena, i)) continue;

        // A signature the class wrote a body for asks nothing of the instance. The default stands
        // in the slot as it is - generic over the class's variables - and is specialized for these
        // types the first time a call reaches it; see emitInstanceCall.
        auto entry = typeClass->functions.get(*module.types, i);
        if(entry.defaultFun) {
            instance->functions.set(*module.arena, i, entry.defaultFun);
            continue;
        }

        module.context.diagnostics.error("instance of %@ does not implement %@"_v, decl.source,
                                         module.context.findName(className),
                                         module.context.findName(entry.name));
    }

    registerInstance(module, instance - *module.arena);
}

/*
 * Defaults and superclasses.
 */

/*
 * `class FromInt(a = Int)` - the type an unconstrained variable of this class settles to.
 *
 * Written in the head, which is where every other fact about a parameter is written. It used to be
 * a declaration of its own, `default FromInt = Int`, and folding it in removes a top-level form
 * without removing anything it said: the four checks below are the four that declaration made, and
 * three of them are now about the head rather than about a second place that had to agree with it.
 * A default has to be declared where the class is because it answers for the class everywhere -
 * that was the coherence rule the old form needed a check for, and a head cannot be written
 * anywhere else.
 *
 * **This is a settle-time default and not the syntactic fill-in of an omitted argument**, which is
 * the one thing about the shared spelling worth stating rather than assuming. `1 + 1` never writes
 * `FromInt` at all, so there is no position for a default to stand in; what it answers is "nothing
 * decided this variable, so what is it". `Solver::settle` reads it, through `literalDefaultType`.
 * The two triggers meet in the type-parameter default of a `data` head, which does both.
 *
 * Late, after the instances, because the last check needs them.
 */
void resolveClassDefault(Module& module, GlobalPtr<TypeClass> classPointer) {
    auto& context = module.context;
    auto global = *module.types;
    auto typeClass = global[classPointer];
    if(typeClass->module != &module || !typeClass->gen) return;

    auto env = global[typeClass->gen];
    resolveGenDefaults(module, typeClass->gen);

    // The parameter that carries one, and the arity rule stated as the search: a class with two
    // parameters has no single variable for a literal to settle, so a default on either of them
    // answers a question nobody can ask.
    GlobalPtr<GenType> defaulted = nullptr;
    for(auto variable: env->types.contents(global)) {
        if(!global[variable]->def) continue;

        if(defaulted || env->types.size() != 1) {
            context.diagnostics.error("only a class with one type argument can have a default"_v,
                                      global[variable]->source);
            return;
        }

        defaulted = variable;
    }

    if(!defaulted) return;
    auto type = global[defaulted]->def;
    auto source = global[defaulted]->source;

    // The concrete check is resolveGenDefault's and has already run; what is left is the one a
    // *class* default has and no other default does.
    if(!findInstance(module, classPointer, { &type, 1 })) {
        context.diagnostics.error("%@ is the default of %@ but has no instance of it"_v, source,
                                  describeType(context, global, type), context.findName(typeClass->name));
        return;
    }

    typeClass->defaultType = type;
    typeClass->defaultSource = source;
}

// Every superclass its class declares has to hold for the instance's own types. Without this,
// `class (FromInt(a)) Num(a)` promises generic code something an `instance Num(Metres)` need not
// have delivered, and the promise would only fail much later inside a body nobody wrote that way.
void checkSuperclasses(Module& module, ClassInstance& instance) {
    auto& context = module.context;
    auto global = *module.types;
    auto typeClass = global[instance.typeClass];

    Scratch<TypeList> heldArgs(module.program.typeLists);
    auto& args = *heldArgs;
    for(auto arg: instance.forTypes.contents(*module.arena)) args.push(arg);

    for(auto constraint: global[typeClass->gen]->classes.contents(global)) {
        if(!constraint.typeClass) continue;

        Scratch<TypeList> heldConcrete(module.program.typeLists);
        auto& concrete = *heldConcrete;

        for(auto arg: constraint.args.contents(global)) {
            concrete.push(substituteType(module, arg, toBuffer(args), instance.source));
        }

        if(findInstance(module, constraint.typeClass, toBuffer(concrete))) continue;

        // A parametric head answers for a superclass with what it requires of its own variables as
        // well as with what has an instance: `instance (Ord(a)) Foo(Ptr(a))` has `Eq(a)` in hand
        // because `Ord` declares it, exactly as a generic function's body does.
        if(instance.gen && provesClass(module, *global[instance.gen], constraint.typeClass, toBuffer(concrete))) {
            continue;
        }

        StringBuilder text;
        describeTypes(context, global, toBuffer(concrete), text);

        context.diagnostics.error("%@ requires %@, and there is no instance of it for (%@)"_v, instance.source,
                                  context.findName(typeClass->name),
                                  context.findName(global[constraint.typeClass]->name), text.view());
    }
}
