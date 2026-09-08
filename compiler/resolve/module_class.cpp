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

/*
 * Whether two written signatures are the same contract - what an instance member is checked against.
 *
 * `sameType` is the wrong question here by one axis, and only one. A loan group is an index
 * numbered inside a *single* signature: the class's `fn elements(self: source'c) -> source'[a]`
 * calls its group 1 because `source` is the first label that signature wrote, and an instance
 * restating the same declaration numbers its own from one as well - so two identical contracts
 * disagree by an index neither of them chose. Which group it is has no meaning across the pair
 * either: an instance member's groups are discarded and the class's are what the built function
 * gets, a few lines below where `implArg->loan` is assigned.
 *
 * Everything else is compared exactly. A window's *capability* is part of the contract and stays
 * part of it - `-> &[a]` against `-> '[a]` is a different promise, and this is what still catches
 * it - as does a borrow's, a pointer's pointee and a record's arguments.
 */
static bool sameContract(Module& module, TypePtr left, TypePtr right) {
    auto base = *module.types;
    if(left == right) return true;
    if(!left || !right) return false;
    if(base[left]->kind != base[right]->kind) return false;

    switch(base[left]->kind) {
        case Type::Borrow: {
            auto a = (BorrowType*)base[left];
            auto b = (BorrowType*)base[right];
            return a->mut == b->mut && sameContract(module, a->to, b->to);
        }
        case Type::Ptr:
            return sameContract(module, ((PtrType*)base[left])->to, ((PtrType*)base[right])->to);
        case Type::Record: {
            // Where the group is dropped, and the only place it is: `sameLoanedType` carries which
            // part of a capability sibling is the contract and which part is the index.
            if(sameLoanedType(base, left, right)) return true;

            auto a = (RecordType*)base[left];
            auto b = (RecordType*)base[right];
            if(a->sliceMut != b->sliceMut) return false;
            if(!a->instanceOf || a->instanceOf != b->instanceOf) return false;
            if(a->instanceArgs.size() != b->instanceArgs.size()) return false;

            // A record's *arguments* may carry groups of their own - `Maybe(src'T)` - so the walk
            // continues through them rather than stopping at the head.
            for(Size i = 0; i < a->instanceArgs.size(); i++) {
                auto x = a->instanceArgs.get(base, i);
                auto y = b->instanceArgs.get(base, i);
                if(!sameContract(module, x, y)) return false;
            }

            return true;
        }
        case Type::Array: {
            auto a = (ArrayType*)base[left];
            auto b = (ArrayType*)base[right];
            return a->count == b->count && sameContract(module, a->content, b->content);
        }
        case Type::Tup: {
            auto a = (TupType*)base[left];
            auto b = (TupType*)base[right];
            if(a->fields.size() != b->fields.size() || a->layout != b->layout) return false;

            for(Size i = 0; i < a->fields.size(); i++) {
                auto x = a->fields.get(base, i);
                auto y = b->fields.get(base, i);
                if(x.name != y.name || x.boxed != y.boxed || x.host != y.host) return false;
                if(!sameContract(module, x.type, y.type)) return false;
            }

            return true;
        }
        case Type::Fun: {
            auto a = (FunType*)base[left];
            auto b = (FunType*)base[right];
            if(a->kind != b->kind || a->resultBind != b->resultBind) return false;
            if(a->args.size() != b->args.size()) return false;
            if(!sameContract(module, a->result, b->result)) return false;

            for(Size i = 0; i < a->args.size(); i++) {
                auto x = a->args.get(base, i);
                auto y = b->args.get(base, i);
                if(x.convention != y.convention || x.lazy != y.lazy) return false;
                if(!sameContract(module, x.type, y.type)) return false;
            }

            return true;
        }
        default:
            // Every other kind is interned on the whole of what it is, so the pointers already
            // answered above and a difference here is a real one.
            return false;
    }
}

/*
 * The one parameter whose written type names the **owner** rather than what a shared occurrence of
 * it is - Analysis-Manifestations.md Part 8, and the same rule the ownership stage applies through
 * `AccessForm::Owner`.
 *
 * `Contiguous.elements` and `Writable.elementsMut` are how an owner *becomes* a view, so their
 * subject cannot already be one: `elements` on an `Array(a)` reads the owner's run and length to
 * build the descriptor it hands back, and a body handed the descriptor would have nothing to build
 * it out of. Member zero's first parameter of a class carrying either role is therefore the owner,
 * and this is the whole of the exception.
 *
 * `Viewed` is the third derivation and needs no entry, because its subject is a `String` and a
 * `String` is not a container in this stage - so a borrow of one is already the owner and nothing
 * folded it away.
 *
 * It is needed *here* and not in `resolveType` because the fold that costs the owner is a fact
 * about the spelling and not about the position: `source'Array(a)` is the window everywhere in the
 * language, and this position is where the instance writer means the container. The parameter the
 * body actually gets is the class's substituted type either way - see `expectedType` - so what this
 * decides is only whether the restatement is accepted.
 */
static bool ownerSubject(Module& module, GlobalPtr<TypeClass> typeClass, Size member, Size argument) {
    if(member != 0 || argument != 0) return false;

    auto& classes = module.coreClasses;
    return typeClass == classes.contiguous || typeClass == classes.writable;
}

/*
 * A shared borrow of one of the *class's own variables* is the parameter annotated, not a
 * reference-valued parameter - Analysis-Borrows.md §4.8 and §7.1.
 *
 * `bindingType` folds `&self: src'T` into the `Ref` convention and leaves `self: src'T` alone, and
 * the reason it leaves it alone is concrete: `x: 'Int` has meant a reference to the caller's Int
 * since before groups existed, and folding it would hand the body a copy of its own. That argument
 * is about a written type. It does not survive contact with a class variable, and this is where the
 * exception is taken.
 *
 * `class Index(c -> k, v)` declares `fn get(self: source'c, index: k)`, and `'c` is a reference to a
 * type no instance of the class has: the instances are at `Array(a)`, at `Flat(a)` - where the type
 * already is a window and a reference to one is a second indirection nothing builds - at `Map(k, v)`
 * and at `%a`. Every one of them writes the type itself, which is what `return self: c` said before
 * either spelling existed. So a class member's shared borrow of a class variable is that: the
 * parameter, in the group the label named, and the group travels on the Arg exactly as `&` puts it
 * there.
 *
 * Parameters only. A *result* written `-> source'v` is a reference and stays one - that is the whole
 * of what `get` hands back - and a class variable is no different there from any other type.
 */
static void foldClassSubjects(Module& module, GenEnv& env, Function& signature) {
    auto base = *module.types;

    for(auto pointer: signature.args.contents(*module.arena)) {
        auto argument = (*module.arena)[pointer];
        if(!argument->type || base[argument->type]->kind != Type::Borrow) continue;

        auto borrow = (BorrowType*)base[argument->type];
        if(borrow->mut || !borrow->to || base[borrow->to]->kind != Type::Gen) continue;

        // The class's own variables and not a member's. A member may introduce variables of its own
        // - `$r` on an `iter fn`, and anything a signature names that the head did not - and those
        // are as concrete to an instance as a written type is.
        auto index = ((GenType*)base[borrow->to])->index;
        if(index >= env.types.size()) continue;

        argument->type = borrow->to;
        if(argument->loan == kNoLoan) argument->loan = borrow->loan;
    }
}

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

    // The `known.def` role this class claims, if any - `Program::knownClasses`. Here rather than at
    // the lookup sites for the reason `readIntrinsicRole` is where it is: what a role binds is a
    // *declaration*, and the attribute is written on one.
    readKnownClassRole(module, decl, typeClass - *module.types);

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

        // What a shared borrow of a class variable means - see `foldClassSubjects`.
        foldClassSubjects(module, *env, *signature);

        auto stored = typeClass.functions.get(*module.types, index);
        stored.fun = signature - *module.arena;

        // A `lens fn` or `iter fn` default is an ordinary default that is also desugared - see
        // resolveClassDefault, which does that in a context of its own for the reason a member
        // signature could not.
        if(member.fun.body) {
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
        case Type::Atomic:
            return mentionsVariable(global, ((AtomicType*)global[type])->content, index);
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
/*
 * The orphan rule - doc/spec/classes.md, and Analysis-Modules.md §2.1.3.
 *
 * "An instance must be declared either in the module declaring the class or in a module declaring
 * one of the types in its head." It is a *modularity* guarantee rather than a soundness one:
 * coherence is enforced program-wide by duplicate detection over `Program::instancesByClass`, at any
 * module size, and was enforced that way before this existed. What this promises is that adding a
 * module cannot introduce an instance for a class and a type that both belong to someone else, so no
 * third party can change how your call sites resolve.
 *
 * The unit is the module, which is now a directory. That is what makes "put the display logic in a
 * display file" legal - `MyApp/Model.yana` declaring `MyType` and `MyApp/Display.yana` writing
 * `instance Show(MyType)` are one module - and it is why grouping is not optional: under
 * file-modules a class's instances cannot leave the file declaring the class, so `Text.yana` could
 * not be split at all.
 */

// Whether this module is the one that declared `type`. By identity against the module's own type
// table rather than by reading the declaration's location, because that table is what "a module
// declares this type" means everywhere else - and it holds the primitives, which have no location.
static bool declaresType(Module& module, TypePtr type) {
    if(!type) return false;

    for(auto& entry: module.namedTypes) {
        if(entry == type) return true;
    }

    return false;
}

/*
 * Whether any nominal type reachable from this head argument is one this module declared.
 *
 * Reachable rather than just the argument itself: `instance Show(Maybe(MyType))` is written in
 * MyType's module and `MyType` is a type in its head, which is the reading that makes the rule
 * usable. An instantiation is asked about as its declaration - `Maybe(Int)` is `Maybe` - since that
 * is what a module declared.
 *
 * The structural types are descended into and are never themselves an answer: nobody declares
 * `[a]`, `%a` or `{x: Int}`, so an instance for one of those is placed by what is inside it. Which
 * ones those are is `kTypeChildren` in type.def, and the arms below are checked against it - see the
 * `default:`, where the two that were missing are named.
 */
static bool headMentionsLocalType(Module& module, TypePtr type, U32 depth = 0) {
    if(!type || depth > 8) return false;

    auto global = *module.types;
    auto value = global[type];

    if(declaresType(module, type)) return true;

    switch(value->kind) {
        case Type::Record: {
            auto record = (RecordType*)value;
            if(record->instanceOf && declaresType(module, (TypePtr)record->instanceOf)) return true;

            for(auto arg: record->instanceArgs.contents(global)) {
                if(headMentionsLocalType(module, arg, depth + 1)) return true;
            }

            return false;
        }
        case Type::Ptr:
        case Type::Borrow:
            return headMentionsLocalType(module, ((PtrType*)value)->to, depth + 1);
        case Type::Array:
            return headMentionsLocalType(module, ((ArrayType*)value)->content, depth + 1);
        case Type::Vector:
            // A lane is an integer or a float by construction, so nothing a module declares can be
            // inside one. Named rather than left to the arm below, because "this kind holds a type
            // and the answer is still no" is a statement, and the alternative is a walk into a
            // child that cannot answer.
            return false;
        case Type::Atomic:
            return headMentionsLocalType(module, ((AtomicType*)value)->content, depth + 1);
        case Type::Tup: {
            for(auto field: ((TupType*)value)->fields.contents(global)) {
                if(headMentionsLocalType(module, field.type, depth + 1)) return true;
            }

            return false;
        }
        case Type::Fun: {
            auto function = (FunType*)value;
            for(auto arg: function->args.contents(global)) {
                if(headMentionsLocalType(module, arg.type, depth + 1)) return true;
            }

            return headMentionsLocalType(module, function->result, depth + 1);
        }
        default:
            // Everything that names no type - which this walk got wrong until `kTypeChildren` was a
            // column to check it against. `Fun` and `Atomic` were missing, so
            // `instance Eq((Local) -> Int)` and `instance Eq(Atomic(%Local))` written in `Local`'s
            // own module were reported as orphans: the walk found nothing local in a head that is
            // nothing but local, and the advice it then gave was to move the instance to the module
            // it was already in.
            assertTrue("a kind with children needs an arm here - see kTypeChildren in type.def"
                       && !kindHoldsTypes(value->kind));
            return false;
    }
}

static void checkOrphanRule(Module& module, TypeClass& typeClass, Buffer<TypePtr> args, StringId className,
                            LocationId source) {
    // The class is ours, which is the first of the two ways to be placed.
    if(typeClass.module == &module) return;

    for(auto arg: args) {
        if(headMentionsLocalType(module, arg)) return;
    }

    // A generated instance has no source and no module to be an orphan in - Core builds its
    // primitives' instances itself, and they are Core's on both counts anyway.
    if(source == kNullLocation) return;

    module.context.diagnostics.error("this instance of %@ is an orphan - it is in %@, which declares neither %@ nor any type in the instance head, so a third module could declare the same instance and which one a call site sees would depend on what happened to be compiled. Put it in the module declaring the class or in the one declaring the type, or wrap the type in a qualified alias"_v,
                                     source, module.context.findName(className),
                                     module.context.findName(module.name),
                                     module.context.findName(className));
}

/*
 * What a written instance-member parameter restates - the whole of the question, asked once.
 *
 * An instance member does not declare its parameters. The class did, and what is written here is a
 * *restatement* of that declaration, checked against `expected` and otherwise not used. The
 * difficulty is that one spelling has more than one reading and an instance is entitled to any of
 * them, so this had become five `if(!sameContract(...))` fallbacks in a row, each adding a reading
 * to the one before - and the fifth had stopped being a check at all and started deciding the
 * parameter's type. Written once it is three readings of what the author wrote against two things
 * the class's type may be called.
 *
 * **The three readings**, which are the three things this syntax means elsewhere in the language:
 *
 *  - The **type** reading, `resolveType`: `[T]` is the owner and `'[T]` is the window.
 *  - The **binding** reading, `bindingType`: `[T]` is the window, and `x: &T` folds into the `Ref`
 *    convention. This is what a parameter's type means in an ordinary signature, and an instance
 *    member is an ordinary signature in every respect except that the class already fixed it.
 *  - The type reading with a **shared borrow taken off**, because `foldClassSubjects` took one off
 *    the class's side: `Viewed.view(self: source'a)` against `fn view(self: source'String)` is one
 *    declaration, and the class is the half that lost the reference.
 *
 * **The two denotations**: the class's type, and - where that is a container - its window. The
 * second is what makes `instance Chunked([a], a)` writable, since `class Chunked(c -> a)` says
 * `self: c` and substitutes to the owner while the member wrote `[a]`, which in a binding position
 * is the window. It is also what accepts `Contiguous.elements`' `self: source'Array(a)`, where the
 * *spelling* names the owner and the reading is the window - the same pair read from the other end,
 * and the reason both used to need a fallback of their own.
 *
 * The window is tried first, because a spelling with both readings meant the narrower one: `[a]` is
 * the owner as a type and the window as a binding, and a parameter is a binding. `self: Array(a)`
 * has no window reading at all, which is how the growable container is still asked for by name.
 *
 * What is *done* about a window is not decided here and deliberately: whether one may stand at this
 * position is a question about the class's signature rather than about this spelling, and it is
 * answered where the rest of that question is - see `windowStandIns`. All this reports is which of
 * the two the author wrote.
 */
struct Restatement {
    // The reading the comparison ended on: the one that matched, or the type reading where none
    // did, which is the type the diagnostic names.
    TypePtr reading = nullptr;

    // The convention, carrying the binding reading's fold where that is the reading that matched.
    // `self: &Cell` against a class's `&self: c` is one declaration, and without carrying the fold
    // out the convention check below would call it two.
    ast::BindType bind = ast::BindType::Borrow;

    bool matched = false;

    // It denoted the container's *window* rather than the container itself.
    bool window = false;
};

static Restatement restateParameter(Module& module, const ast::Type& written, ast::BindType bind,
                                    GenEnv* gen, TypePtr expected, TypePtr window) {
    auto base = *module.types;

    Restatement result;
    result.bind = bind;
    result.reading = resolveType(module, written, gen);

    /*
     * The group the binding reading asks for is thrown away, and it has to be asked for:
     * `bindingType` folds `&self: 'T` into the `Ref` convention only when it has somewhere to put
     * the group, so without it an instance would keep a reference type where the class - whose own
     * signature went through the same call with a group - folded it away. Which group it is has no
     * meaning across the pair, which is what `sameContract` is for.
     */
    LoanGroup group = kNoLoan;
    auto boundBind = bind;
    auto bound = bindingType(module, written, boundBind, gen, &group);

    auto borrow = base[result.reading]->kind == Type::Borrow ? (BorrowType*)base[result.reading] : nullptr;
    auto shared = borrow && !borrow->mut ? borrow->to : nullptr;

    // The readings in the order they were added, which is also cheapest-first. Each records what it
    // matched *as*, so that the convention fold and the diagnostic both describe the same reading.
    auto denotes = [&](TypePtr wanted) {
        if(!wanted) return false;
        if(sameContract(module, result.reading, wanted)) return true;

        if(sameContract(module, bound, wanted)) {
            result.reading = bound;
            result.bind = boundBind;
            return true;
        }

        if(shared && sameContract(module, shared, wanted)) {
            result.reading = wanted;
            return true;
        }

        return false;
    };

    if(window != expected && denotes(window)) {
        result.matched = true;
        result.window = true;
    } else if(denotes(expected)) {
        result.matched = true;
    }

    return result;
}

/*
 * Which of an instance's head positions a **window** may be selected at - `ClassInstance::windowArgs`.
 *
 * The question this answers is the one `matchInstance` asks when nothing matched a window: may
 * `Eq(ArrayView(I32))` be served by `instance Eq([a])`? A window is a shared borrow of its
 * container, so the answer is yes exactly where the class only *reads* the container, and no
 * everywhere else. Deciding it here rather than at selection is what keeps selection a lookup: the
 * class's signatures and this instance's implementations are both in hand at this point and never
 * change afterwards.
 *
 * The test is a comparison of two substitutions rather than a walk looking for the variable, which
 * is the same trick that keeps this from being a second, less careful copy of `substituteType`:
 * substitute the head twice, once with the container at the position and once with its window, and
 * read off what moved.
 *
 *  - A member position that did **not** move does not mention the variable, and says nothing.
 *  - One that moved has to have moved *wholly* - the container itself, not `Maybe(c)` around it -
 *    be a `Borrow`, and be a position this instance's own implementation already takes the window
 *    at. That last clause is what ties this to the member's spelling: an instance whose member
 *    wrote `self: Array(a)` asked for the growable container, and handing it a window would be a
 *    different function.
 *  - A **result** or hand-over that moved disqualifies the position outright, because a member that
 *    produces the class variable would have to produce a window, and nothing here can.
 *
 * A **defaulted** member is the one place the last clause is waived, because there is no written
 * signature for it to be read off: the class's default is one body compiled at the substituted
 * types, and `emitInstanceCall` compiles it at the window where the call arrived with one. The
 * first three still hold of it, which is what makes the waiver small - a default that produced the
 * class variable or consumed it has already disqualified the position two lines above.
 */
static void windowStandIns(Module& module, TypeClass* typeClass, ClassInstance& instance) {
    auto global = *module.types;
    auto local = *module.arena;

    if(instance.forTypes.size() > 8) return;

    TypeList head;
    for(auto type: instance.forTypes.contents(local)) head.push(type);

    TypeList swapped;

    for(Size k = 0; k < head.size(); k++) {
        auto window = sliceOf(module, head[k]);
        if(!window || window == head[k]) continue;

        replaceContents(swapped, head);
        swapped[k] = window;

        auto moved = [&](TypePtr written) {
            auto owner = substituteType(module, written, toBuffer(head), instance.source);
            auto view = substituteType(module, written, toBuffer(swapped), instance.source);
            return owner == view ? nullptr : view;
        };

        bool stands = true;

        for(Size m = 0; stands && m < typeClass->functions.size(); m++) {
            auto signature = local[typeClass->functions.get(global, m).fun];
            if(!signature) continue;

            // A result that moved - which on an `iter fn` or a `lens fn` is the hand-over, since
            // that is what the class wrote there. Either would have to produce a window.
            if(moved(signature->returnType)) {
                stands = false;
                break;
            }

            auto slot = instance.functions.get(local, m);
            auto implemented = local[slot];
            auto inherited = slot == typeClass->functions.get(global, m).defaultFun;

            for(Size a = 0; stands && a < signature->args.size(); a++) {
                auto declared = local[signature->args.get(local, a)];
                auto view = moved(declared->type);
                if(!view) continue;

                auto owner = substituteType(module, declared->type, toBuffer(head),
                                            instance.source);

                // The variable itself, read - and, where this instance wrote the member, taken as
                // the window there. A **default** is exempt from that last clause: it has no
                // written signature to have asked with, so `emitInstanceCall` reads the window back
                // off the arguments and specializes the one body at that instead.
                stands = declared->convention == ast::BindType::Borrow &&
                         view == sliceOf(module, owner) &&
                         (inherited || (implemented && a < implemented->args.size() &&
                                        local[implemented->args.get(local, a)]->type == view));
            }
        }

        if(stands) instance.windowArgs |= U8(1u << k);
    }
}

void resolveInstance(Module& module, ast::Decl& decl) {
    auto& type = decl.instance.type;
    StringId className {};
    TypeList args;

    // The constraints are read first, since they name the variables the head is written over -
    // and a constraint may name one the head does not, which is what the check below reports.
    auto genPointer = prepareGenEnv(module, GenEnv::Instance, {}, decl.instance.constraints, true);
    auto gen = (*module.types)[genPointer];

    if(type.kind != ast::Type::App) {
        module.context.diagnostics.error(
            "type-namespaced instances are not available yet - write `instance Class(Type)`"_v, decl.source);
        return;
    }

    auto& app = *module.parse[type.app];
    if(app.base.kind != ast::Type::Con) {
        module.context.diagnostics.error("an instance must name a class"_v, decl.source);
        return;
    }

    className = app.base.name;

    /*
     * The class is found *before* its arguments are resolved, which is what lets a head position be a
     * count - Implementation-Const-Generics.md §1.1's `class Chunked(c, a, n: Int -> m)`.
     *
     * Which positions are counts is a fact about the declaration and not about the shape of what was
     * written, exactly as it is for `data` and for `Vec` (see resolveAppArg, which is this same
     * question and was already shared with a class *constraint*'s argument list). Resolved without it,
     * `instance Digest(Sha256, 32)` reported "a number is not a type" and `instance (n: Int) Sized(c,
     * n)` reported that `n` was a number rather than binding it - so a class could declare a const
     * parameter, a constraint could name one, and no instance could ever be written to satisfy either.
     *
     * Ordering costs nothing here. A `data` or `class` head has to resolve its constraints late
     * because the class may be declared further down the file; an instance's context resolves in
     * place, since every class is declared by the time instances are read.
     */
    auto classPointer = findClass(module, className, decl.source);
    if(!classPointer) {
        module.context.diagnostics.error("unknown class %@"_v, decl.source, module.context.findName(className));
        return;
    }

    auto declared = (*module.types)[classPointer]->gen;

    Size index = 0;
    for(auto arg: app.args.contents(module.parse)) {
        args.push(resolveAppArg(module, declared, index++, arg, gen));
    }

    // The implicit classes have no instances to write: whether a type is TrivialCopy is decided by
    // whether every one of its members is, and an instance saying otherwise would be a claim the
    // compiler has already contradicted. What a type that needs different behaviour writes is
    // `Copy` or `Sink`, which is exactly what those are for.
    if(classPointer == module.coreClasses.trivialCopy || classPointer == module.coreClasses.trivialSink) {
        module.context.diagnostics.error("%@ is decided structurally and has no instances to write - a type that cannot be duplicated or relocated bitwise says so by writing `Copy` or `Sink`"_v,
                                         decl.source, module.context.findName(className));
        return;
    }

    /*
     * And §3.2's two, on the same terms and for a reason of their own: the family an atomic may
     * hold is closed because every admitted operation is **always lock-free**, and the only way to
     * keep that promise is to refuse the widths a target would have to synthesize with a library
     * lock. An instance is exactly a library claiming one of those.
     */
    if(classPointer == module.coreClasses.atomicValue || classPointer == module.coreClasses.atomicInteger) {
        module.context.diagnostics.error("%@ is decided by the compiler and has no instances to write - the family is closed because every operation on an atomic is always lock-free, which no declaration can promise of a type the target has no indivisible access for"_v,
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

    /*
     * A variable the head does not mention is one nothing could ever bind, so a constraint over it
     * says something no selection can check - unless a functional dependency binds it, which is what
     * the closure below is about.
     *
     * `instance (Magnitude(a, w), ...) Show(NumberFormat(a))` names `w` nowhere in the head and does
     * not have to: `Magnitude(a -> w)` says the first argument decides the rest, so a selection that
     * binds `a` has bound `w` with it and there is still exactly one instance to find. The rule this
     * relaxes is about a variable *nothing* decides, and that one is still an error.
     *
     * So: start from the variables the head binds, and add the determined positions of every
     * constraint whose determiners are all decided already, until a pass adds nothing. A fixpoint
     * rather than one sweep, because one dependency may feed another - `C(a -> b)` beside `D(b -> c)`
     * decides `c` and the order the two were written in is not the caller's business.
     */
    Array<bool> decided;
    for(Size i = 0; i < gen->types.size(); i++) decided.push(false);

    // Whether every variable a type mentions is decided - the question asked of a constraint's
    // determiners, and of nothing else.
    auto allDecided = [&](TypePtr type) {
        for(Size i = 0; i < gen->types.size(); i++) {
            auto index = (*module.types)[gen->types.get(*module.types, i)]->index;
            if(!decided[i] && mentionsVariable(*module.types, type, index)) return false;
        }

        return true;
    };

    for(Size i = 0; i < gen->types.size(); i++) {
        auto index = (*module.types)[gen->types.get(*module.types, i)]->index;
        decided[i] = args.contains([&](TypePtr arg) { return mentionsVariable(*module.types, arg, index); });
    }

    for(auto changed = true; changed;) {
        changed = false;

        for(auto constraint: gen->classes.contents(*module.types)) {
            if(!constraint.typeClass) continue;

            auto determined = (*module.types)[constraint.typeClass]->determined;
            if(!determined || constraint.args.size() <= determined) continue;

            auto determiners = true;
            for(Size i = 0; i < determined; i++) {
                if(!allDecided(constraint.args.get(*module.types, i))) determiners = false;
            }

            if(!determiners) continue;

            for(Size i = determined; i < constraint.args.size(); i++) {
                auto arg = constraint.args.get(*module.types, i);

                for(Size v = 0; v < gen->types.size(); v++) {
                    if(decided[v]) continue;

                    auto index = (*module.types)[gen->types.get(*module.types, v)]->index;
                    if(!mentionsVariable(*module.types, arg, index)) continue;

                    decided[v] = true;
                    changed = true;
                }
            }
        }
    }

    for(Size i = 0; i < gen->types.size(); i++) {
        if(decided[i]) continue;

        auto variable = gen->types.get(*module.types, i);
        module.context.diagnostics.error("type variable %@ does not appear in the instance head, and no functional dependency decides it - nothing at a call site could bind it"_v,
                                         decl.source, module.context.findName((*module.types)[variable]->name));
        return;
    }

    // Closed once the head has been read: from here on a lowercase name in the body means one of
    // the variables the head introduced, and anything else is the error it would be in a function.
    gen->open = false;
    resolveConstraintClasses(module, *gen);
    auto parametric = gen->types.isNotEmpty();

    checkOrphanRule(module, *typeClass, toBuffer(args), className, decl.source);

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
            // Unless the compiler generated it and said the library may write it instead - the one
            // container bridge, `ClassInstance::Origin` in class.h. The generated instance is
            // retired here rather than at its head, because this is the only point in the passes at
            // which both it and the written one exist.
            if(previous.origin == ClassInstance::Origin::Replaceable) {
                previous.origin = ClassInstance::Origin::Superseded;
                continue;
            }

            // And the head set where it is the *written* one that stands down - `Index`, D14. A
            // return rather than a `Superseded` mark, because nothing of this declaration may be
            // built at all: its members would be resolved, exported and emitted for a stage that
            // never calls them, and one of the two bodies the library writes is native-only.
            if(previous.origin == ClassInstance::Origin::Overriding) return;

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

        // `Viewed.view` at `String` returns the view record rather than the `'String` the class wrote
        // - `derivedViewResult`, applied to the written result below on the same terms.
        function->returnType = derivedViewResult(module, classPointer, function->returnType);

        // And the convention it hands that over under, for the same reason and from the same place:
        // an instance need not restate the result at all, so the class is the only thing that knows.
        // See Function::handoverBind.
        function->handoverBind = signature->handoverBind;
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

            // The window of what the class said, where it has one - the second thing this parameter
            // may be called, and the type it becomes where one may stand. See `restateParameter`.
            auto window = sliceOf(module, expectedDeclared);
            Restatement restated;

            if(declared.type) {
                restated = restateParameter(module, *module.parse[declared.type], declared.bind, gen,
                                            expectedDeclared, window);

                // The convention the reading that matched folded to, so that the check below sees
                // `self: &Cell` and the class's `&self: c` as the one declaration they are.
                declared.bind = restated.bind;

                if(!restated.matched) {
                    module.context.diagnostics.error("argument %@ has type %@ here but %@ in class %@"_v, declared.source,
                                                     module.context.findName(declared.name),
                                                     describeType(module.context, *module.types, restated.reading),
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

            /*
             * And what an author who wrote the window meant by it, which is the one outcome of that
             * comparison that is not a diagnostic.
             *
             * `class Chunked(c -> a)` writes `self: c`, and at `c = Array(a)` that substitutes to
             * the owner - so `instance Chunked([a], a)` used to be handed the container and every
             * body in it began by converting. What the member wrote is `[a]`, the window, and this
             * is where it gets one.
             *
             * Read-only, and that is what makes it sound rather than a convenience: a window is a
             * shared borrow of the container, so it can stand where the container is only read and
             * nowhere else. An exclusive or consuming position keeps the class's type, because
             * `&self` writes the container's own storage and `->self` takes it. It is the same
             * sentence `windowStandIns` makes about a whole instance, said here about one
             * parameter, and the two have to agree: that one decides whether a window may *select*
             * this instance, and a window selecting an instance whose member took the owner would
             * be handed something it cannot convert back.
             *
             * `ownerSubject` is excluded, and it is not a special case so much as the same
             * exception said once: `elements` is where a window *comes from*, so its subject cannot
             * already be one. It has to be said here because the spelling does not distinguish
             * them - `source'Array(a)` reads as the window, so `restated.window` is set for the one
             * member that needs the owner. It was not, once: `Contiguous.elements` became an
             * identity, inlined, and the borrow lost the `return`-marked call that roots it -
             * `test/resolve/MergeRoot.yana` read a freed array's first bytes.
             *
             * The two facts this does *not* change are the class's signature and the ABI of a call
             * to it: `expectedDeclared` is what the restatement was checked against, and it is
             * still the class's. What changes is only the function this instance compiles, and
             * nothing had to be taught the difference, because an owner reaching a window parameter
             * is what `convertArgument` already does at every `[a]` parameter in the language - so
             * `emitInstanceCall`'s two paths to the implementation both cross it, and a deferred
             * dispatch through a witness calls what those built.
             */
            if(restated.window && !classArg->isLazy() &&
               classArg->convention == ast::BindType::Borrow &&
               !ownerSubject(module, classPointer, index, i)) {
                expectedType = window;
            }

            auto implArg = function->addArg(module, declared.name ? declared.name : classArg->name,
                                            expectedType, declared.source);
            implArg->convention = classArg->convention;
            implArg->loan = classArg->loan;
            if(classArg->isLazy()) implArg->lazyType = expectedDeclared;
        }

        if(member.fun.ret) {
            auto written = derivedViewResult(module, classPointer,
                                             resolveType(module, *module.parse[member.fun.ret], gen));

            /*
             * Both readings of `[T]` again, and for the reason the argument loop above gives - but
             * on a `lens fn` or an `iter fn` member the binding reading is the one the class itself
             * took, since what it wrote as a result is the type it hands over and resolveSignature
             * resolves that through bindingType. So an instance writing `-> [a]` against a class
             * writing `-> '[a]` is the same declaration, and so is the pair the other way round.
             */
            if(!sameContract(module, written, function->returnType) &&
               signature->funKind != ast::FunKind::Plain) {
                auto bind = member.fun.retBind;
                auto bound = bindingType(module, *module.parse[member.fun.ret], bind, gen, nullptr);
                if(sameContract(module, bound, function->returnType)) written = bound;
            }

            if(!sameContract(module, written, function->returnType)) {
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

    windowStandIns(module, typeClass, *instance);

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
