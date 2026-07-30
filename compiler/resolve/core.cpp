#include "core.h"
#include "intrinsic.h"
#include "generic.h"
#include "witness.h"
#include "../parse/parser.h"

/*
 * Core's declarations.
 *
 * Everything here that can be written in the language is written in the language, and read by
 * the same parser and the same declaration passes a user module goes through. The compiler only
 * supplies what the language cannot express about itself: the five primitive types, and the
 * bodies of their class instances.
 *
 * Operators are declared with the fixities they have everywhere else, and the classes are the
 * ones Design.md names. Note that `and`/`or`/`not` appear in both Integral and Logic: an
 * integer's are bitwise and a Bool's are logical, and which is meant is decided by which class
 * has an instance for the operand type rather than by a special case anywhere in the resolver.
 *
 * The same is true of the two things the resolver used to do by itself. A literal is a call to
 * `FromInt`/`FromDecimal` and an implicit conversion is a call to `Widen`, so "what does `1`
 * mean" and "which conversions happen without being written" are answered by the declarations
 * below rather than by a table of the primitives inside the resolver - and a user type answers
 * them for itself by writing an instance. `default` names the type a literal takes when nothing
 * else decided one.
 */
static const char* kCoreSource = R"CORE(
infixl 0 +=
infixl 0 -=
infixl 0 *=
infixl 0 /=
infixl 1 ||
infixl 2 &&
infixl 3 ==
infixl 3 !=
infixl 3 >
infixl 3 >=
infixl 3 <
infixl 3 <=
infixl 4 `xor`
infixl 4 `or`
infixl 5 `and`
infixl 5 `shl`
infixl 5 `shr`
infixl 5 `sar`
infixl 6 +
infixl 6 -
infixl 7 *
infixl 7 /
infixl 7 `rem`
infixl 7 %

data Bool = False | True
data Ordering = LT | EQ | GT

data Maybe(a) = Nothing | Just(a)
data Result(e, a) = Err(e) | Ok(a)

class FromInt(a):
  fn fromInt(value: Long) -> a

class FromDecimal(a):
  fn fromDecimal(value: Double) -> a

default FromInt = Int
default FromDecimal = Float

-- The bodies below are class defaults: an instance that writes `==` has `!=` already, and one
-- that writes `compare` has all four comparisons. What each class still asks of an instance is the
-- one primitive operation everything else is derived from, which is why there is no choice of
-- which function to implement and no `MINIMAL` pragma to read. A default may only call class
-- functions of strictly lower rank than its own, so a pair defining each other in terms of the
-- other is a rejected declaration rather than a program that compiles and hangs.
--
-- Every primitive instance below still supplies all of these itself, so a Bool's `!=` is the one
-- comparison instruction it always was rather than a specialization of `!(lhs == rhs)`.
class Eq(a):
  fn ==(lhs: a, rhs: a) -> Bool
  fn !=(lhs: a, rhs: a) -> Bool = !(lhs == rhs)

-- The four comparisons are `compare` read four ways, and `compare` is the fold a derived instance
-- would have to produce. Lexicographic order over a record is therefore one function rather than
-- five, which is what makes Ord worth deriving at all.
class (Eq(a)) Ord(a):
  fn <(lhs: a, rhs: a) -> Bool = compare(lhs, rhs) == LT
  fn <=(lhs: a, rhs: a) -> Bool = compare(lhs, rhs) != GT
  fn >(lhs: a, rhs: a) -> Bool = compare(lhs, rhs) == GT
  fn >=(lhs: a, rhs: a) -> Bool = compare(lhs, rhs) != LT
  fn compare(lhs: a, rhs: a) -> Ordering

-- Anything that can be added can be counted from, so `fn (Num(a)) inc(x: a) = x + 1` compiles as
-- written rather than making the author declare FromInt as well. FromInt stays its own class so
-- that a Duration or a units newtype can be integer-literal-constructible without also claiming
-- to support multiplication.
--
-- Negation is the one place the superclass earns its keep as a default: `0` is `FromInt(a)` and
-- the subtraction is this class's own, so unary `-` needs nothing an instance has not already
-- promised. An instance for which that is not the negation it wants overrides it.
class (FromInt(a)) Num(a):
  fn +(lhs: a, rhs: a) -> a
  fn -(lhs: a, rhs: a) -> a
  fn *(lhs: a, rhs: a) -> a
  fn /(lhs: a, rhs: a) -> a
  fn -(value: a) -> a = 0 - value

-- The compound assignments are ordinary functions, not syntax. `=` is the one reserved token, so
-- `+=` lexes as an operator like `==` does and needs only a fixity and a declaration - which is
-- why there is no desugaring of `x += e` to `x = x + e` anywhere in the parser.
--
-- That is worth the four lines rather than the one-line rewrite, and the reason is the mutable
-- borrow. On a `@bits`-packed field, a JS host property, or anything else whose Property is not a
-- plain load and store, the rewritten form reads and then writes independently while this one
-- passes one borrow and walks the property path once - the distinction Design.md's Properties and
-- field access draws between `set` and `modify`, inherited here for free.
--
-- Precedence 0 is below every other operator, which is what makes `x += a + b` group as it reads.
-- It is declared `infixl` because the resolver's precedence climbing is left-associative for every
-- operator - `infixr` parses and is not yet read - and `a += b += c` is nonsense under either
-- reading anyway, since the result is unit.
fn (Num(a)) +=(&target: a, amount: a) -> {}:
    target = target + amount

fn (Num(a)) -=(&target: a, amount: a) -> {}:
    target = target - amount

fn (Num(a)) *=(&target: a, amount: a) -> {}:
    target = target * amount

fn (Num(a)) /=(&target: a, amount: a) -> {}:
    target = target / amount

class (Num(a)) Integral(a):
  fn rem(lhs: a, rhs: a) -> a
  fn %(lhs: a, rhs: a) -> a
  fn shl(lhs: a, rhs: a) -> a
  fn shr(lhs: a, rhs: a) -> a
  fn sar(lhs: a, rhs: a) -> a
  fn and(lhs: a, rhs: a) -> a
  fn or(lhs: a, rhs: a) -> a
  fn xor(lhs: a, rhs: a) -> a
  fn not(value: a) -> a

-- For a type that is not Bool the short-circuiting pair and the bitwise pair are the same
-- operation, so `&&`, `||` and `!` default to `and`, `or` and `not` and an instance writes four
-- functions instead of seven. Bool's own instance supplies all seven, which is where
-- short-circuiting lands when the evaluation order of `&&` becomes a rule.
class Logic(a):
  fn &&(lhs: a, rhs: a) -> a = and(lhs, rhs)
  fn ||(lhs: a, rhs: a) -> a = or(lhs, rhs)
  fn and(lhs: a, rhs: a) -> a
  fn or(lhs: a, rhs: a) -> a
  fn xor(lhs: a, rhs: a) -> a
  fn not(value: a) -> a
  fn !(value: a) -> a = not(value)

-- What a condition means. `if x`, `if:` cases and `while x` ask this class rather than requiring
-- a Bool, so `if items:` and `if i - 1: continue` say what they look like they say. The rule that
-- keeps it from being JavaScript's truthiness is that it never applies through a conversion: the
-- instance is selected for the condition's own type, so what `if x` means depends on x's type
-- alone and not on which Widen instances happen to be in scope. Bool's instance is the identity.
--
-- Deliberately not instanced for Maybe or Result: they carry a payload, and `if maybeThing:`
-- invites unwrapping it on the next line. Truth answers "is this empty/zero/null", not "did this
-- succeed" - the `is` operator is what payload-carrying types use.
--
-- `value` is the default convention, which is already an immutable borrow: nothing about
-- truthiness needs to consume or write what it is asked about.
class Truth(a):
  fn truthy(value: a) -> Bool

-- The ownership classes a type can join by writing an instance.
--
-- What they are for is the cases the structural answer gets wrong, and each of them is a statement
-- that it does: `Copy` for a type that can be duplicated but not by copying its bytes, `Sink` for
-- one that cannot be relocated by copying its bytes because it refers to its own address,
-- `Reclaim` for one whose storage is released by something other than handing an allocation back,
-- and `Drop` for one whose lifetime ends in an effect.
--
-- A type that writes none of them still gets all of the behaviours - a bitwise copy, a bitwise
-- move, and the derived teardown that recurses into each member and then releases this owner's own
-- storage.
class Copy(a):
  fn copy(from: a) -> a

-- `to` arrives uninitialized and must be fully initialized before returning. That obligation is
-- Design.md's `@uninit &`, which is not implementable yet - nothing in the language produces
-- uninitialized storage for a caller to pass - so it is documented here and unchecked. When the
-- attribute lands this signature gains it and nothing else about the class changes.
class Sink(a):
  fn sink(&to: a, ->from: a) -> {}

{-
   The two halves of teardown - Design-Memory §4.

   Splitting them is what makes three otherwise separate rules one sentence each: `Reclaim` compiles
   to nothing on the JS target while `Drop` runs; `Reclaim` is elided for region-placed storage
   while `Drop` still runs at last use; and closing a region discharges every `Reclaim` inside it in
   bulk. Region eligibility therefore becomes structural - a type is implicitly region-placeable
   when it has no `Drop` - which is what lets a map of connections be arena-placed with its
   connection teardowns still running where they should.

   `Reclaim` releases this value's own storage and nothing else. An authored one is constrained by
   shape rather than trusted for purity: its body may contain control flow, arithmetic over its own
   metadata, reads of storage it owns, calls to the compiler's per-member teardown, and storage
   release, and no other call. The author is trusted about "I call nothing else", never about "my
   members are effect-free" - whether a container's teardown has effects is computed from whether
   its element types have a `Drop`.
-}
class Reclaim(a):
  fn reclaim(->value: a) -> {}

-- Run once when a live instance dies, at its last use rather than at the end of its scope. Never
-- run on a location a value has been moved out of, which is what the drop flags exist to know.
class Drop(a):
  fn drop(->value: a) -> {}

{-
   The two implicit ownership classes.

   No instance of either is ever written: they hold for a type exactly when they hold for every one
   of its members, and the compiler answers them structurally before typeclass dispatch is available
   at all. They are declared as classes anyway so that a *signature* can constrain a type variable
   by one - `fn (TrivialCopy(a)) dup(x: a) -> {a, a}` - and the body may then act on the fact.

   Design-Memory §2.1's rule is what makes that distinction load-bearing: an unconstrained parameter
   is treated as non-TrivialCopy inside the body regardless of what a caller later substitutes, so
   discovering the fact at one concrete call site may never upgrade a borrow to a copy. The
   constraint is the only thing that can.
-}
class TrivialCopy(a)
class TrivialSink(a)

-- A Widen instance is required to be lossless and total; that is a contract on whoever writes
-- one, checked no more than Copy's is. Which of the two classes relates a pair of types is the
-- whole of the rule for whether a conversion happens on its own or has to be written.
class Widen(a, b):
  fn widen(from: a) -> b

class Narrow(a, b):
  fn narrow(from: a) -> b

{-
   Exchanging the contents of storage.

   `->` takes a value out of a place and leaves it empty, which is why it needs the place to be one
   the compiler can prove things about: something has to know the hole is there, and something has
   to refill it or account for it at the end. That proof is a per-local lattice, and there are three
   kinds of storage it cannot cover - a global, a borrow, and an element a collection handed back.
   None of them has a slot in this frame to carry a state.

   These two are how a value comes out of those. Neither ever leaves a hole: both places hold a live
   value before and after, so there is no state to track and nothing to prove. `let ->old = theGlobal`
   is rejected; `let old = exchange(theGlobal, ->replacement)` is the same intent said in a way that
   is true.

   Two rather than one because they cost different amounts. `swap` cannot write either place until it
   has read both, so it relocates three times through a temporary. `exchange` is handed a value
   rather than a place, so it relocates twice and needs no temporary. A caller with a replacement in
   hand should not pay for the one that has none.
-}
fn swap(&left: a, &right: a) -> {}
fn exchange(&slot: a, ->value: a) -> a
)CORE";

/*
 * `swap` and `exchange`.
 *
 * Both take their places from mutable borrows, which is what lets one declaration cover a local, a
 * field, a global and an element the collection handed back: whatever produced the borrow already
 * answered where the storage is, and the exclusivity check already answered whether two of them may
 * be live at once. `swap(x, x)` is two mutable borrows of one place and is rejected by the rule that
 * was there before either of these existed.
 */

// The relocation, on exactly the terms sinkValue records one for a `->`. Asked the same way for the
// same reason: a body that cannot see the type leaves this null and relocates through the caller's
// descriptor instead, and a specialization asks again for the type it turned out to be.
static ModulePtr<Function> relocationFor(ExprResolver& resolver, TypePtr type, LocationId source) {
    auto ownership = ownershipIn(resolver.module, functionGen(resolver.global, resolver.function), type);
    if(ownership.trivialSink) return nullptr;

    return sinkFor(resolver.module, type, source);
}

/*
 * The mutable borrow a `&` parameter would have made.
 *
 * A generic intrinsic reaches its emitter through expandIntrinsic, which hands over the arguments
 * as the call wrote them - the conventions are applied by emitDirectCall, and a generic signature
 * never goes through it. So the borrow is made here instead, by the same call emitDirectCall would
 * have made.
 *
 * Which is not a formality. The borrow is what puts these operations in front of the borrow
 * checker: `swap(x, x)` is two mutable borrows of one place, and it is rejected by the exclusivity
 * rule that was there before swap existed rather than by anything written for it.
 */
static ModulePtr<Value> exchangedPlace(ExprResolver& resolver, ModulePtr<Value> argument, TypePtr type,
                                       LocationId source) {
    return resolver.borrowArgument(argument, type, source);
}

static ModulePtr<Value> emitSwap(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                                 LocationId source, StringId) {
    // The exchanged type comes off the argument rather than off the declaration: `swap` returns
    // unit, so the substituted result type says nothing about what is being swapped.
    auto type = resolver.valueType(args[0]);

    auto a = exchangedPlace(resolver, args[0], type, source);
    auto b = exchangedPlace(resolver, args[1], type, source);
    if(!a || !b) return nullptr;

    auto swap = resolver.emit<InstSwap>(source, 0, resolver.module.scalar.unit,
                                        Place::inBorrow(a), Place::inBorrow(b), type);

    swap->sink = relocationFor(resolver, type, source);
    return nullptr;
}

static ModulePtr<Value> emitExchange(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                     LocationId source, StringId name) {
    auto slot = exchangedPlace(resolver, args[0], type, source);
    if(!slot) return nullptr;

    // And the `->` convention on the incoming value, for the same reason: expandIntrinsic did not
    // apply it, and what goes into the slot has to be a value this operation owns.
    auto incoming = resolver.sinkValue(resolver.convert(args[1], type, source), source);
    if(!incoming) return nullptr;

    auto exchange = resolver.emit<InstExchange>(source, name, type, Place::inBorrow(slot), incoming);

    exchange->sink = relocationFor(resolver, type, source);

    auto result = resolver.ref(exchange);

    // Storage for what came out, for the reason rootSink gives: a value has no address, so a name
    // bound to this would have nothing to read a field out of. A scalar came out in a register and
    // wants no slot.
    if(isMemoryType(resolver.global, type)) {
        exchange->local = resolver.function.addLocal(resolver.module, type, name, result);
    }

    return result;
}

/*
 * Assembling the module.
 */

static TypePtr addPrimitive(Program& program, Module& module, StringView name, Type* value) {
    auto pointer = value - *program.types;
    auto id = module.context.addQualifiedName(name.ptr, name.length, 1);

    // An integer type is printed by name, so it has to know the one it was declared under.
    if(value->kind == Type::Int) ((IntType*)value)->name = id;

    module.namedTypes.add(id, pointer);
    return pointer;
}

static TypePtr coreType(Module& module, StringView name) {
    auto found = module.namedTypes.get(Context::nameHash(name));
    assertTrue(found.isJust());
    return found.unwrap();
}

/*
 * The fixed-width integer family.
 *
 * Design.md's `I8`/`U8` through `I64`/`U64`, sitting alongside `Int` and `Long` rather than
 * replacing them: `Int` is the type a bare literal takes and the one ordinary arithmetic is
 * written in, and these are what a program reaches for when the width is part of what it means.
 *
 * They live in Core rather than Native because naming a width is not an unsafe act, and because
 * on every target now - not just the machine ones - a narrow field buys something. A record of
 * `U8`s packs into one JS number where a record of `Int`s cannot, so requiring `import Native`
 * to write one would have made the JS target's best representation reachable only through the
 * raw-pointer module.
 *
 * The declaration is split in two because the classes these types join are written in Core's own
 * source: `defineIntegerTypes` creates the types, which the source may then name, and
 * `defineIntegerInstances` joins them to the classes once that source has been read.
 */

// `bits` is the type's size in memory and the width is the primitive it occupies once loaded, so
// everything below 64 bits arrives in a 32-bit register and only the widest family needs a wider one.
static TypePtr addInteger(Module& module, StringView name, U16 bits, bool isSigned) {
    auto id = module.context.addQualifiedName(name.ptr, name.length, 1);
    auto width = bits == 64 ? IntType::Long : IntType::Int;
    auto type = new (module.types) IntType(bits, width, isSigned, id);

    auto pointer = (Type*)type - *module.types;
    module.namedTypes.add(id, pointer);
    return pointer;
}

// Whether a value of `from` fits in `to` without losing anything: more bits, or the same bits
// without a sign to lose. This decides which of the two conversion ladders a pair joins, which
// is the whole of the rule for whether a conversion happens on its own or has to be written.
static bool widens(GlobalBase global, TypePtr from, TypePtr to) {
    auto source = (IntType*)global[from];
    auto target = (IntType*)global[to];

    if(source->isSigned && !target->isSigned) return false;
    if(source->isSigned == target->isSigned) return target->bits > source->bits;

    // Unsigned into signed needs a bit to spare for the sign.
    return target->bits > source->bits;
}

static void defineIntegerTypes(Module& module, Array<TypePtr>& types) {
    struct Width { StringView name; U16 bits; bool isSigned; };
    static const Width widths[] = {
        { "I8"_v, 8, true },   { "U8"_v, 8, false },
        { "I16"_v, 16, true }, { "U16"_v, 16, false },
        { "I32"_v, 32, true }, { "U32"_v, 32, false },
        { "I64"_v, 64, true }, { "U64"_v, 64, false },
    };

    for(auto& width: widths) types.push(addInteger(module, width.name, width.bits, width.isSigned));
}

static void defineIntegerInstances(Module& module, Array<TypePtr>& types) {
    GlobalBase global = *module.types;

    // FromInt first, because Num declares it as a superclass: `1` has to mean something for a
    // type before `+` on it can be told what `x + 1` is.
    for(auto type: types) defineFromInt(module, type);

    for(auto type: types) {
        defineEq(module, type);
        defineOrd(module, type);
        defineNum(module, type);
        defineIntegral(module, type);
        defineTruth(module, type, emitTruthy);
    }

    // The conversion ladder, over these types and the two integer types they sit alongside. The
    // `Int`/`Long` pair already has its rung from the numeric ladder below and is skipped rather
    // than declared twice, which would leave instance selection with two answers to one question.
    auto widthCount = types.size();
    types.push(module.scalar.int_);
    types.push(module.scalar.long_);

    for(Size from = 0; from < types.size(); from++) {
        for(Size to = 0; to < types.size(); to++) {
            if(from == to || (from >= widthCount && to >= widthCount)) continue;

            if(widens(global, types[from], types[to])) {
                defineConversion(module, "Widen"_v, "widen"_v, types[from], types[to]);
            } else {
                defineConversion(module, "Narrow"_v, "narrow"_v, types[from], types[to]);
            }
        }
    }
}

void defineCore(Program& program) {
    auto& context = program.context;

    program.scalar.error = (Type*)new (program.types) Type(Type::Error) - *program.types;
    program.scalar.unit = (Type*)new (program.types) Type(Type::Unit) - *program.types;

    auto name = context.addQualifiedName("Core", 4, 1);
    Lexer lexer(context, context.diagnostics, StringView { kCoreSource, stringLength(kCoreSource) }, name);
    Parser parser(context, lexer, name);

    // `swap` and `exchange` are declared with no body, like Native's generic intrinsics: there is one
    // operation per type being exchanged, so there is nothing to generate until a call says which.
    parser.allowSignatures = true;

    auto ast = new ast::Module(parser.parseModule());

    auto module = program.addModule(ast->name, *ast->region);
    program.core = module;
    program.embeddedAsts.push(ast);

    addPrimitive(program, *module, "Unit"_v, (Type*)(*program.types)[program.scalar.unit]);
    program.scalar.int_ = addPrimitive(program, *module, "Int"_v, new (program.types) IntType(32, IntType::Int, true));
    program.scalar.long_ = addPrimitive(program, *module, "Long"_v, new (program.types) IntType(64, IntType::Long, true));
    program.scalar.float_ = addPrimitive(program, *module, "Float"_v, new (program.types) FloatType(FloatType::Float));
    program.scalar.double_ = addPrimitive(program, *module, "Double"_v, new (program.types) FloatType(FloatType::Double));

    // Before the source is read, so that Core's own declarations may name a width.
    Array<TypePtr> widthTypes;
    defineIntegerTypes(*module, widthTypes);

    resolveModuleDecls(*module, *ast, nullptr);

    attachIntrinsic(*module, "swap"_v, emitSwap);
    attachIntrinsic(*module, "exchange"_v, emitExchange);

    program.scalar.bool_ = coreType(*module, "Bool"_v);
    program.scalar.ordering = coreType(*module, "Ordering"_v);

    TypePtr numeric[] = {
        program.scalar.int_,
        program.scalar.long_,
        program.scalar.float_,
        program.scalar.double_,
    };

    // FromInt comes first because Num declares it as a superclass: `1` has to mean something for
    // a type before `+` on that type can be told what `x + 1` is.
    for(auto type: numeric) defineFromInt(*module, type);

    defineFromDecimal(*module, program.scalar.float_);
    defineFromDecimal(*module, program.scalar.double_);

    for(auto type: numeric) {
        defineEq(*module, type);
        defineOrd(*module, type);
        defineNum(*module, type);
    }

    defineIntegral(*module, program.scalar.int_);
    defineIntegral(*module, program.scalar.long_);

    defineEq(*module, program.scalar.bool_);
    defineLogic(*module, program.scalar.bool_);
    defineEq(*module, program.scalar.ordering);

    // A Bool is already the answer; every number is asked whether it is non-zero. NaN is therefore
    // truthy, which is worth knowing rather than surprising: the instance says "not zero", and no
    // amount of floating-point special-casing would make `if x` mean something better.
    defineTruth(*module, program.scalar.bool_, emitIdentity);
    for(auto type: numeric) defineTruth(*module, type, emitTruthy);

    // Widening and narrowing are ordinary class operations, so a user type can join either
    // ladder later without the resolver learning anything new about conversion. The ladder is
    // written out rather than searched: one step, never a chain.
    for(Size from = 0; from < 4; from++) {
        for(Size to = 0; to < 4; to++) {
            if(from == to) continue;

            if(from < to) {
                defineConversion(*module, "Widen"_v, "widen"_v, numeric[from], numeric[to]);
            } else {
                defineConversion(*module, "Narrow"_v, "narrow"_v, numeric[from], numeric[to]);
            }
        }
    }

    // The width types join the same classes, after the ladder above rather than before it: their
    // own ladder reaches `Int` and `Long`, and skips that one pair on the grounds that it has just
    // been declared here.
    defineIntegerInstances(*module, widthTypes);

    // The classes the language's own syntax is written in terms of - a literal, an implicit
    // conversion, a condition, and the three points a binding convention compiles to. Looked up by
    // name once, here, so that nothing downstream has to search for them by string.
    program.coreClasses.fromInt = classNamed(*module, "FromInt"_v);
    program.coreClasses.fromDecimal = classNamed(*module, "FromDecimal"_v);
    program.coreClasses.widen = classNamed(*module, "Widen"_v);
    program.coreClasses.narrow = classNamed(*module, "Narrow"_v);
    program.coreClasses.truth = classNamed(*module, "Truth"_v);
    program.coreClasses.copy = classNamed(*module, "Copy"_v);
    program.coreClasses.sink = classNamed(*module, "Sink"_v);
    program.coreClasses.reclaim = classNamed(*module, "Reclaim"_v);
    program.coreClasses.drop = classNamed(*module, "Drop"_v);
    program.coreClasses.trivialCopy = classNamed(*module, "TrivialCopy"_v);
    program.coreClasses.trivialSink = classNamed(*module, "TrivialSink"_v);

    // Core's own instances exist only now, so its superclass checks and its `default`
    // declarations run here rather than as part of reading its source.
    checkModuleClasses(*module, *ast);
}

/*
 * Collections.
 *
 * The growable array of Design.md's "Collection types", written in the language over Native rather
 * than generated by the compiler. It is a separate module from Core for one reason: an array is
 * built out of raw pointers and the heap, and Core is imported by Native rather than the other way
 * round, so nothing in Core can name either.
 *
 * It is nonetheless implicitly imported, because `[a]` is a type the grammar produces and a type
 * whose operations a program cannot reach would be a strange thing to be able to write.
 *
 * What this is not, yet, is Implementation-Regions.md part 5's shared `Storage(a)` primitive with a
 * derived Drop - the thing collections are supposed to be written on so that region placement
 * applies to all of them at once. This is one collection with an authored Drop, which is the
 * smaller thing that makes the storage decisions of Milestone 6 testable; the primitive belongs
 * with the standard library that does not exist yet.
 */
static const char* kCollectionsSource = R"COLLECTIONS(
import Native

{-
   A growable array.

   Three numbers and a pointer, which is the ordinary shape: where the elements are, how many there
   are, and how many there is room for. The fourth field is the one the language's own storage
   decisions put there - `onHeap` says whether the buffer came from the allocator, which is what
   the drop has to know and what nothing else can tell by looking at an address.

   An array literal's buffer starts as storage the compiler placed, on the frame when it proved the
   array does not outlive it. Growing past that buffer moves the elements to the heap and says so,
   so the two cases differ at run time by one bit and not by two types.
-}
data Array(a) {items: %a, length: Int, capacity: Int, onHeap: Bool}

{-
   How many bytes `count` elements occupy.

   Written as a pointer difference rather than with `sizeOf`, because what is wanted is the size of
   a *type* and pointer arithmetic already scales by exactly that - `from` is never read, only
   measured against. Counts are `Int` and byte quantities are `I64`, which is the split the two
   sides of this function have: an index is a number of elements the program wrote, and a size is
   what the allocator and the block operations take.
-}
fn byteSpan(from: %a, count: Int) -> I64 =
    difference(cast(from) :: %U8, cast(from + count) :: %U8)

-- An array with room for nothing. The first push allocates.
fn emptyArray() -> Array(a) = Array {items: null(), length: 0, capacity: 0, onHeap: False}

fn length(self: Array(a)) -> Int = self.length
fn capacity(self: Array(a)) -> Int = self.capacity

{-
   Element `index`, as a borrow of storage the caller owns.

   This is what the `return` marker is for: the result points into the array's buffer, so the array
   has to stay borrowed for as long as the result is live, and saying so in the signature is what
   lets a caller be checked without seeing this body.
-}
fn get(return self: Array(a), index: Int) -> &a = borrow(self.items + index)
fn getMut(return &self: Array(a), index: Int) -> &a = borrowMut(self.items + index)


{-
   Room for `wanted` elements.

   The buffer moves to the heap the first time it has to grow, whatever it was before: a literal's
   frame-placed buffer is not the allocator's to free, which is what `onHeap` records and why the
   old one is released only when it was heap storage to begin with.
-}
fn reserve(&self: Array(a), wanted: Int) -> {}:
    if wanted <= self.capacity then return

    let &wide = self.capacity + self.capacity :: Int
    if wide < wanted then wide = wanted
    if wide < 4 then wide = 4

    let fresh = cast(allocateHeap(byteSpan(self.items, wide))) :: %a
    if isNull(fresh) then return

    copyMemory(cast(fresh) :: %U8, cast(self.items) :: %U8, byteSpan(self.items, self.length))
    if self.onHeap then freeHeap(cast(self.items) :: %U8)

    self.items = fresh
    self.capacity = wide
    self.onHeap = True

fn push(&self: Array(a), item: a) -> {}:
    reserve(self, self.length + 1)

    -- A failed reserve leaves the array as it was rather than writing past the end of it. There is
    -- no way to report the failure yet; `Result` is the eventual answer and needs the array first.
    if self.length >= self.capacity then return

    store(self.items + self.length, item)
    self.length = self.length + 1

{-
   Removes element `index`, moving whatever followed it down over the gap.

   The element is taken out before the gap is closed, rather than being left to the `copyMemory` that
   writes over it. An assignment releases what it replaces, which is the rule that would have covered
   this - but a block copy is not an assignment the compiler can see. It is Native moving bytes, and
   bytes moving over a live value is exactly the operation that owes its own bookkeeping.

   So `doomed` is where the element goes, and the binding is the whole of the fix: a `->` binding
   owns what it holds, and what it holds is released when this returns. It is never read, which is
   the point - the value has nowhere to go and being dropped is what should happen to it.

   The move is out of a raw pointer, which is unchecked by construction and correct here for the
   reason the bounds test above it establishes: `index` is inside the initialized prefix, so there is
   a live value at that address to take. Design-Memory's checked world cannot state that, which is
   why the collection is the thing written against Native rather than the caller.
-}
fn remove(&self: Array(a), index: Int) -> {}:
    if index < 0 || index >= self.length then return

    let ->doomed = *(self.items + index)

    let rest = self.length - index - 1
    if rest > 0:
        copyMemory(cast(self.items + index) :: %U8, cast(self.items + index + 1) :: %U8,
                   byteSpan(self.items, rest))

    self.length = self.length - 1

-- Handing the buffer back is storage release and nothing else, so this is a `Reclaim` rather than
-- a `Drop` - which is what keeps an array of elements that have no effect of their own
-- region-placeable (Design-Memory §4). An array whose *elements* have a `Drop` gets one derived
-- from them, and running it is the erased loop the generic model supplies.
instance Reclaim(Array(a)):
    fn reclaim(->value: Array(a)) -> {}:
        if value.onHeap then freeHeap(cast(value.items) :: %U8)
)COLLECTIONS";

void defineCollections(Program& program) {
    auto& context = program.context;

    auto name = context.addQualifiedName("Collections", 11, 1);
    Lexer lexer(context, context.diagnostics, StringView { kCollectionsSource, stringLength(kCollectionsSource) }, name);
    Parser parser(context, lexer, name);
    auto ast = new ast::Module(parser.parseModule());

    auto module = program.addModule(ast->name, *ast->region);
    program.embeddedAsts.push(ast);

    resolveModuleDecls(*module, *ast, nullptr);
    resolveModuleBodies(*module);

    program.collections = module;
    auto array = module->namedTypes.get(context.addQualifiedName("Array", 5, 1));
    if(array) program.arrayType = (RecordType*)(*program.types)[array.unwrap()] - *program.types;
}
