#pragma once

#include "expr.h"

/*
 * Generating the operations the language cannot write in itself.
 *
 * Core and Native both need this: `Int`'s `+` cannot be defined in terms of anything more basic,
 * and neither can a pointer dereference. What they share is the shape of the answer - a real
 * declaration with a real signature, plus a hook that generates its body's one instruction at the
 * call site instead of calling it - so the machinery for building one lives here and each module
 * says only which operations it has and what they expand to.
 *
 * Two kinds of intrinsic exist, and the difference is whether the operation depends on a type the
 * declaration does not fix:
 *
 *   A *concrete* intrinsic - Core's `Num(Int).+` - is a generated function with a generated body,
 *   so it can be printed, lowered and eventually have its address taken, while an ordinary call
 *   to it expands to the instruction it contains.
 *
 *   A *generic* intrinsic - Native's `fn *(it: %a) -> a` - is one operation per element type, so
 *   there is nothing to generate until a call says which. It is declared in source with no body
 *   at all and never reaches lowering; expandIntrinsic() generates it where it is called.
 */

// The IR one primitive operation expands to, shared by a generated body and the intrinsic hook so
// that a call and an inline expansion can never drift apart.
using Emit = ModulePtr<Value> (*)(ExprResolver&, Buffer<ModulePtr<Value>>, TypePtr, LocationId, StringId);

// One method of a generated instance: the name and arity it has in the class, and what it expands
// to. Arity is part of the key because `Num` declares `-` twice.
//
// `deferred` replaces `emit` for a signature with a `@lazy` parameter - see DeferredIntrinsic. The
// generated body uses it too, forcing through the thunk it was handed, so the body a call cannot
// see through and the expansion a call can are the same source.
struct IntrinsicMethod {
    StringView name;
    U16 arity;
    Emit emit = nullptr;
    DeferredIntrinsic deferred = nullptr;
};

/*
 * The emitters, in the order the classes need them.
 */

template<Value::Kind kind>
inline ModulePtr<Value> emitBinary(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                   LocationId source, StringId resultName) {
    return resolver.ref(resolver.emit<InstBinary>(source, resultName, type, kind, args[0], args[1]));
}

template<Value::Kind kind>
inline ModulePtr<Value> emitUnary(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                  LocationId source, StringId resultName) {
    return resolver.ref(resolver.emit<InstUnary>(source, resultName, type, kind, args[0]));
}

template<CompareOp op>
inline ModulePtr<Value> emitCompare(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                    LocationId source, StringId resultName) {
    return resolver.ref(resolver.emit<InstCmp>(source, resultName, type, args[0], args[1], op));
}

ModulePtr<Value> emitCast(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                          LocationId source, StringId resultName);

// `fromInt`/`fromDecimal` on a primitive is the literal itself, at the type that was asked for.
ModulePtr<Value> emitFromLiteral(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                 LocationId source, StringId resultName);

// The same over a vector, which is the literal in every lane - `class (FromInt(a)) Num(a)` forces
// the question and "every lane" is the only answer that is not arbitrary. See intrinsic.cpp.
ModulePtr<Value> emitVectorFromLiteral(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                       LocationId source, StringId resultName);

// `truthy` on a number: non-zero. `truthy` on a Bool: the value itself.
ModulePtr<Value> emitTruthy(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                            LocationId source, StringId resultName);
ModulePtr<Value> emitIdentity(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                              LocationId source, StringId resultName);

// `not` on a Bool is the one bit operation correct for a two-constructor discriminant, rather than
// an integer complement that would produce something outside the type.
ModulePtr<Value> emitLogicalNot(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                LocationId source, StringId resultName);

// `&&` and `||` on a Bool: a branch over the right operand, and no call. This is where Design.md's
// "short-circuiting is a property of the signature" stops costing anything - the signature declares
// `@lazy`, and the instance that implements it for the one type conditions are written at can see
// the argument and emit it under the test.
ModulePtr<Value> emitLogicalAnd(ExprResolver& resolver, Buffer<ResolvedArg> args, TypePtr type,
                                LocationId source, StringId resultName);
ModulePtr<Value> emitLogicalOr(ExprResolver& resolver, Buffer<ResolvedArg> args, TypePtr type,
                               LocationId source, StringId resultName);

/*
 * Building instances and declarations.
 */

// The class of this name, which must exist: these are the classes the module being built declared
// itself, or ones it imported from Core.
GlobalPtr<TypeClass> classNamed(Module& module, StringView name);

// Generates one instance of `typeClass` for `args`, with one generated function per method. A
// class function no method covers is generated from its own default implementation, which today
// means `Ord.compare`.
//
// `gen` is the head's own generic context, for an instance written over a type variable rather
// than for one type: `args` are then that context's types (`%a` rather than `%U8`) and every
// generated function is generic over it, exactly as a source-written parametric instance is.
//
// The instance is registered before it is handed back, so every caller that only wants it to exist
// may ignore the result. simd.cpp is the one that does not: it generates during instance *lookup*
// and answers with what it built, rather than making the search run a second time.
ModulePtr<ClassInstance> generateInstance(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                                          Buffer<IntrinsicMethod> methods, GlobalPtr<GenEnv> gen = nullptr);

// The standard instances of one primitive type. Each is exactly the class's methods mapped onto
// the machine operations that implement them.
void defineFromInt(Module& module, TypePtr type);
void defineFromDecimal(Module& module, TypePtr type);
void defineEq(Module& module, TypePtr type, GlobalPtr<GenEnv> gen = nullptr);
void defineOrd(Module& module, TypePtr type, GlobalPtr<GenEnv> gen = nullptr);
ModulePtr<ClassInstance> defineNum(Module& module, TypePtr type);
ModulePtr<ClassInstance> defineIntegral(Module& module, TypePtr type);
void defineLogic(Module& module, TypePtr type);
void defineTruth(Module& module, TypePtr type, Emit emit);

// One rung of the conversion ladder: `Widen(from, to)` or `Narrow(from, to)`, whose single method
// is a cast.
ModulePtr<ClassInstance> defineConversion(Module& module, StringView className, StringView method,
                                          TypePtr from, TypePtr to);

// One rung of the reinterpretation ladder. Only ever called for a same-width pair - the class's
// whole safety argument is that no other instance exists - and `gen` is for the pointer rungs,
// which are written over a type variable rather than over a type.
ModulePtr<ClassInstance> defineBitcast(Module& module, TypePtr from, TypePtr to, GlobalPtr<GenEnv> gen = nullptr);

// Attaches a hook to a signature the module declared in source but gave no body. This is how a
// generic intrinsic is written: the declaration says what it means to the type checker, and the
// hook says what it generates. Reports if no such function was declared.
void attachIntrinsic(Module& module, StringView name, Intrinsic intrinsic);

/*
 * The built-in containers' accessors - Implementation-Simplification.md §2.
 *
 * `Index` and `Length` over `Flat(a)`, `Array(a)` and a raw pointer, generated on exactly the terms
 * `defineNum` and the rest are: the head is written here rather than in source, and each method is
 * one machine operation. Why these and not the rest of a container's API is in intrinsic.cpp - the
 * short of it is that a subscript has to be free with no optimizer having run, and a source body
 * cannot be.
 *
 * Each is called once, on the module that owns the types, after its declarations are read - the
 * records have to exist for a head to name them - and before any body is: a body of that same module
 * may subscript, and an instance that does not exist yet is one the resolver reports.
 */
void defineNativeIndexInstances(Module& native);
void defineContainerInstances(Module& collections);
