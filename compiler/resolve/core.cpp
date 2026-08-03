#include "core.h"
#include "intrinsic.h"
#include "name.h"
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
infixl 4 ??
infixl 5 `xor`
infixl 5 `or`
infixl 6 `and`
infixl 6 `shl`
infixl 6 `shr`
infixl 6 `sar`
infixl 7 +
infixl 7 -
infixl 8 *
infixl 8 /
infixl 8 `rem`
infixl 8 %

data Bool = False | True
data Ordering = LT | EQ | GT

data Maybe(a) = Nothing | Just(a)
data Result(e, a) = Err(e) | Ok(a)

-- What a continuation reports back to the lens that called it - Analysis-Lens.md §5.1's exit
-- signal. `Proceed` is "the rest of the block finished normally, and here is its value"; `Exit`
-- carries the enclosing function's return value past a frame that has cleanup still to run, so a
-- `withLock` releases its lock on an early `return` from the block below it.
--
-- One type rather than three: `Step(r) = Next | Done(r)` is `Outcome({}, r)`, so the loop signal,
-- the exit signal and a skipping lens's own result are all this. Only the exit signal uses it so
-- far; the constructors are not named `Continue`/`Break` for that reason.
data Outcome(a, e) = Proceed(a) | Exit(e)

{-
   What a carrier type says about its two paths - Implementation-Semantics.md part 5.

   One class with two readers. `?` will use both halves: `toOutcome` to ask which path a value is on,
   and `fromExit` to rebuild the carrier at the enclosing function's own return type. A *skipping*
   lens's call site uses only the first, because where its skip goes is written at the call site
   rather than derived from a signature - which is the one difference between the two, and the whole
   of Analysis-Lens.md §3.3's argument for reading B.

   `m` decides the other two parameters. An instance head is keyed on it alone and `a` and `e` are
   read off whichever instance matched, because nothing at a use site constrains them: a lens
   returning `Maybe(r)` says what its continuation produced and says nothing at all about what a skip
   carries. That keying is what makes the check "is there an instance, and is its `a` the
   continuation's result" rather than an inference problem.

   Both operands are `->`. Asking which path a value is on is the last thing anyone does with it, so
   consuming it costs nothing and is what keeps the class usable for a carrier whose payload is
   owned - the alternative would borrow a wrapper in order to hand out what is inside it.
-}
class Try(m -> a, e):
  fn toOutcome(->value: m) -> Outcome(a, e)
  fn fromExit(->reason: e) -> m

-- `Nothing` carries no reason, so a skip through a `Maybe` reaches `| else -> ...` with nothing to
-- bind. That is the common case and the one Design.md's `findChar` is written in.
instance Try(Maybe(a), a, {}):
  fn toOutcome(->value: Maybe(a)) -> Outcome(a, {}) = match value:
      Just(->inner) -> Proceed(inner)
      Nothing -> Exit({})

  fn fromExit(->reason: {}) -> Maybe(a) = Nothing

instance Try(Result(e, a), a, e):
  fn toOutcome(->value: Result(e, a)) -> Outcome(a, e) = match value:
      Ok(->inner) -> Proceed(inner)
      Err(->reason) -> Exit(reason)

  fn fromExit(->reason: e) -> Result(e, a) = Err(reason)

-- The identity instance, which is what makes `Outcome` itself a usable lens result: a lens that
-- wants to report *why* it skipped and has no better type to say it in returns this one.
instance Try(Outcome(a, e), a, e):
  fn toOutcome(->value: Outcome(a, e)) -> Outcome(a, e) = value
  fn fromExit(->reason: e) -> Outcome(a, e) = Exit(reason)

{-
   The same carrier, around a different payload - what `?.` needs and `Try` cannot say.

   `Try` relates a carrier to what is inside it, which is the direction `?` reads. `?.` reads the
   other way: it has a carrier `m` that came in and a value `b` that the rest of the chain produced,
   and it needs the type that is `m`'s wrapper around `b`. `Maybe(Row)` and `String` give
   `Maybe(String)`.

   Three parameters keyed on the first two, rather than a type constructor applied to a variable.
   That is deliberate and is the same trade `Try` makes: `m` applied to `b` would need the
   higher-kinded machinery Implementation-Generics.md part 7 fences off, while an instance head
   naming `Maybe(a)` and `Maybe(b)` in two positions is an ordinary one, and the dependency is what
   turns "which type is that" into a lookup.

   `rewrap` does not mention `m`, exactly as `Try.fromExit` does not mention `a`. Neither is callable
   from source for that reason: what selects them is the shape the compiler already worked out, not
   an argument list.
-}
class Rewrap(m, b -> n):
  fn rewrap(->value: b) -> n

instance Rewrap(Maybe(a), b, Maybe(b)):
  fn rewrap(->value: b) -> Maybe(b) = Just(value)

instance Rewrap(Result(e, a), b, Result(e, b)):
  fn rewrap(->value: b) -> Result(e, b) = Ok(value)

instance Rewrap(Outcome(a, e), b, Outcome(b, e)):
  fn rewrap(->value: b) -> Outcome(b, e) = Proceed(value)

{-
   `xs[i]` - Implementation-Containers.md §17, Analysis-Extensibility.md §3.

   The container decides what a key is and what an element is, and one container decides both: `c`
   determines `k` and `v`. That dependency is the whole reason this can be a class at all. Nothing
   at `xs[i]` binds `v`, and an index literal binds `k` to a literal rather than to a type - so
   without the arrow the instance could never be selected, which is what
   Implementation-Containers.md §17 recorded as the blocker and what functional dependencies
   removed.

   Two functions and not three. `get`/`getMut` rather than `get`/`set`/`modify`, because an
   assignment *through* a returned mutable borrow already is `modify` - `resolvePlace` roots the
   write at the borrow `getMut` handed back - and a container, unlike a property, always has a
   borrow to hand out. The `return` markers are what make that checked: the result points into
   whatever `self` points into, so the container stays borrowed for as long as the element is, and
   writing to the container while an element borrow is live is rejected by the ordinary rule
   rather than by anything written here.

   `get` takes `self` and `getMut` takes `&self`, which is Implementation-Containers.md §4.1's
   split: reading needs no exclusivity and writing does, and `xs[i] = v` on an immutable binding is
   rejected for that reason rather than by a second class.

   What this class deliberately does *not* cover is a container whose elements are computed rather
   than stored - there is nothing to point at, so there is no borrow to return. That is the case a
   weaker parent class would serve, and it is not written yet: an `iter fn` already hands over
   computed values without claiming they have a location, and until there are two containers that
   want it, the right members of such a parent are guesswork. See Implementation-Containers.md §17.
-}
class Index(c -> k, v):
  fn get(return self: c, index: k) -> &v
  fn getMut(return &self: c, index: k) -> &v

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
-- short-circuiting lands.
--
-- `@lazy` on the right operand is what makes short-circuiting a property of this declaration rather
-- than of the compiler: the argument is not evaluated at the call site, and reading `rhs` inside an
-- instance is what runs it. The defaults below force it exactly once, which is what the rule
-- requires; `xor` takes both strictly and says so, because it genuinely needs both.
--
-- What it costs differs by how the call reaches it. Bool's instance is an intrinsic the resolver
-- can see through, so `a && b` is a branch with the right operand emitted under it and no closure
-- anywhere; a call that cannot be seen through - a user instance inheriting these defaults - is
-- handed the nullary closure instead, and the default calls it exactly once.
--
-- What neither of them gives yet is a flow-sensitive binding: `if p is Just(v) && v > 0` does not
-- see `v`, because `is` outside condition position discards what it bound before `&&` is even
-- selected. Making that work is a change to the condition path rather than to `@lazy`.
class Logic(a):
  fn &&(lhs: a, @lazy rhs: a) -> a = and(lhs, rhs)
  fn ||(lhs: a, @lazy rhs: a) -> a = or(lhs, rhs)
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

-- The other operator `@lazy` exists for: a fallback that is only computed when there is nothing to
-- fall back from. An ordinary generic function rather than anything the compiler knows about, which
-- is the point - `??` is short-circuiting for the same reason `&&` is, and for no other.
--
-- Deliberately not a class function. There is one Maybe and one meaning, and a class would invite
-- instances for Result and for pointers, where "is there a value here" and "did this succeed" stop
-- being the same question - the same reason Truth is not instanced for Maybe.
--
-- `->` on the operand, for the reason `Try`'s two functions take one: what this answers *is* what
-- was inside, so a borrowed operand would be handing out something it does not own. For a
-- `Maybe(Int)` the sink is a copy and nothing changes at any call site; for a `Maybe(Buffer)` it is
-- the difference between working and freeing the buffer twice.
fn ??(->value: Maybe(a), @lazy fallback: a) -> a = match value:
    Just(->inner) -> inner
    Nothing -> fallback

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
    auto type = new (module.types) IntType(bits, IntType::widthFor(bits), isSigned, id);

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

static void defineIntegerTypes(Module& module, TypeList& types) {
    struct Width { StringView name; U16 bits; bool isSigned; };
    static const Width widths[] = {
        { "I8"_v, 8, true },   { "U8"_v, 8, false },
        { "I16"_v, 16, true }, { "U16"_v, 16, false },
        { "I32"_v, 32, true }, { "U32"_v, 32, false },
        { "I64"_v, 64, true }, { "U64"_v, 64, false },

        /*
         * The widest integer that is still a machine primitive on every target.
         *
         * 64 bits is not: a JS `number` holds 53 consecutive integers and nothing wider, so `Long`
         * there is a `bigint` - a heap value, four times the retained size of a number, and off the
         * ordinary arithmetic path. `WideInt` is the type that stays a primitive everywhere: a
         * masked 64-bit integer natively, and a plain `number` on JS with codegen/js/wide.cpp
         * supplying the bitwise operators the host stops having above 32 bits.
         *
         * **Signed, and that is a correctness requirement rather than a preference.** Wrapping
         * addition on JS is a comparison and a subtraction, which is sound only while `a + b` is
         * exactly representable - true below 2^53. A signed 53-bit type has operands bounded by
         * 2^52, so the sum always is; an unsigned one reaches 2^54 and would silently round.
         * benchmark/bits53-js/findings.md is where that was measured.
         *
         * A primitive rather than `alias WideInt = @bits(53) I64`, because a refinement dispatches
         * to the instances of the type it refines: the alias would have done its arithmetic at 64
         * bits, as a `bigint` on JS, with a conversion at each end. See `isWideNumber` in
         * codegen/js/type.cpp.
         */
        { "WideInt"_v, 53, true },
    };

    for(auto& width: widths) types.push(addInteger(module, width.name, width.bits, width.isSigned));
}

static void defineIntegerInstances(Module& module, TypeList& types) {
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
    TypeList widthTypes;
    defineIntegerTypes(*module, widthTypes);

    /*
     * `Size` - what an index and a length are carried at, which is the target's width rather than
     * the language's. C's `size_t`, and it exists for the reason C's does.
     *
     * A **name for an existing primitive** and not a primitive of its own. A distinct type would
     * need its own `Eq`, `Ord`, `Num`, `Integral` and both conversion ladders, and would then need
     * converting to and from the very type it *is* on each target. What is wanted is the opposite -
     * that `Size` be whichever primitive the target already computes indices in - so this binds a
     * name and stops.
     *
     * Keyed on the target's word width and deliberately not on which backend is running. Those
     * coincide today, and they are not the same question: a thirty-two-bit native target wants a
     * thirty-two-bit `Size` for the same reason JS does, and writing the rule as "js or not" would
     * have to be found and rewritten the day one exists rather than simply being true.
     *
     * **Signed**, unlike the counts an owner stores (see `Count` in native.cpp). Those are unsigned
     * because it makes a bounds test one comparison; this one is signed because it is the type an
     * `Int` index widens *into*, and a signed-to-unsigned ladder does not widen. The choice is
     * between one free conversion at every subscript and one extra comparison in `checkBounds`, and
     * the subscript is the hotter of the two by a long way.
     *
     * Sixty-four bits natively rather than `WideInt`'s fifty-three, which is the point of asking the
     * target rather than picking the portable answer: `WideInt` is a *masked* 64-bit integer here,
     * so every operation on one pays for a width that only JS needs. JS gets `Int`, where a host
     * array's length is a `uint32` by specification and nothing wider can be described anyway.
     *
     * `I64` and not `Long`, which are two distinct primitives of one width here: `I64` is what
     * `Native`'s pointer arithmetic takes, and an index exists to be added to an address. `Int` and
     * not `I32` on the other side, for the mirror reason - `Int` is what a literal defaults to, and
     * a same-width pair does not widen, so an `I32` `Size` would reject `xs[i]` for the ordinary `i`.
     */
    auto sizeType = isJsMode(context.settings.mode) ? program.scalar.int_ : coreType(*module, "I64"_v);
    module->namedTypes.add(context.addQualifiedName("Size", 4, 1), sizeType);

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
    program.coreClasses.try_ = classNamed(*module, "Try"_v);
    program.coreClasses.rewrap = classNamed(*module, "Rewrap"_v);
    program.coreClasses.index = classNamed(*module, "Index"_v);
    program.coreClasses.copy = classNamed(*module, "Copy"_v);
    program.coreClasses.sink = classNamed(*module, "Sink"_v);
    program.coreClasses.reclaim = classNamed(*module, "Reclaim"_v);
    program.coreClasses.drop = classNamed(*module, "Drop"_v);
    program.coreClasses.trivialCopy = classNamed(*module, "TrivialCopy"_v);
    program.coreClasses.trivialSink = classNamed(*module, "TrivialSink"_v);

    // The exit signal's carrier. Its constructors are found by name rather than assumed to be
    // declared in this order, since the order is a detail of the source above and this is emitted
    // code that has no declaration to read.
    if(auto outcome = findType(*module, Context::nameHash("Outcome"_v), kNullLocation)) {
        program.outcomeType = (RecordType*)(*program.types)[outcome] - *program.types;

        if(auto proceed = findConstructor(*module, Context::nameHash("Proceed"_v), kNullLocation)) {
            program.outcomeProceed = proceed.unwrap().index;
        }

        if(auto exit = findConstructor(*module, Context::nameHash("Exit"_v), kNullLocation)) {
            program.outcomeExit = exit.unwrap().index;
        }
    }

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

   A run of slots and how many of them hold values. That split is the whole of
   Implementation-Containers.md §1: what a run *is* - where it came from, how much room it has, what
   releasing it means - belongs to `Run(a)`, and what this type adds is the one thing a run
   deliberately does not have, which is a notion of which slots are live.

   So there is no storage decision in this module at all. An array literal's run starts as storage
   the compiler placed, on the frame when it proved the array does not outlive it; growing past it
   moves the slots to the heap; and which of those happened is recorded in the run's own tag and
   read by the run's own `Reclaim`. This array's teardown is therefore *derived* - recurse into the
   members - and nothing here is written about it.

   `length` is a `Count` and not an `Int` for the two reasons `Native` gives at that alias: an
   unsigned bound turns every `index < 0 || index >= length` into one comparison, and thirty bits is
   what leaves the capacity and the placement room in the same word once the layout can put them
   there. The *interface* stays signed - `length()` answers an `Int`, `get` takes one - because the
   language's literals and its arithmetic are signed and a container is not the place to start
   arguing with that.
-}
data Array(a) {run: Run(a), length: Count}

-- An array with room for nothing. The first push allocates.
fn emptyArray() -> Array(a) = Array {run: emptyRun(), length: 0}

fn capacity(self: Array(a)) -> Int = self.run.capacity :: Int

{-
   Room for `wanted` elements.

   Growth policy and nothing else - doubling, with a floor - because relocation is the run's. The
   `&` on `resize`'s own `self` is what makes that safe with no rule of this module's own: a mutable
   borrow of the run conflicts with any live borrow into it, so a caller holding an element borrow
   across a push is rejected by the ordinary check rather than by anything written here.
-}
fn reserve(&self: Array(a), wanted: Int) -> {}:
    let room = self.run.capacity :: Int
    if wanted <= room then return

    let &wide = room + room :: Int
    if wide < wanted then wide = wanted
    if wide < 4 then wide = 4

    resize(self.run, wide)

{-
   Appends an element.

   `->item` and not `item`, because the array *takes* it: what the store writes into the run is a
   live value the caller no longer owns, and a borrowed parameter would leave the caller's own drop
   to release something the array is still holding. The write is what transfers it - a store through
   a raw pointer is an assignment the ownership pass reads as a hand-over - so nothing is released
   at the end of this body on the path that stored.

   And on the path that did not, it is. A failed reserve leaves the array as it was rather than
   writing past the end of it, and the element it was given dies here rather than being leaked or
   handed back. There is no way to report the failure yet; `Result` is the eventual answer and needs
   the array first.
-}
fn push(&self: Array(a), ->item: a) -> {}:
    let count = self.length :: Int
    reserve(self, count + 1)
    if count >= (self.run.capacity :: Int) then return

    store(self.run.items + count, item)
    self.length = count + 1 :: Count

{-
   The slice - Implementation-Containers.md §4.

   What a `[a]` parameter receives. `length` is declared over the *slice* and not over the owner,
   which is not an omission: reading is structural, so the operation that reads should ask for the
   borrow, and an owner reaches it by the ordinary conversion at the call - see convertSlice. §5's
   `Chunked` is this rule generalized to containers that are not contiguous.

   Reading an element lives one layer down, in `Native`'s `instance Index(Flat(a), Size, a)`, for
   the same reason and by the same conversion. Growth is not here at all - it is nominal, and `push`
   says `Array(a)`.
-}
fn length(self: Flat(a)) -> Size = self.length

{-
   Subscripting the owner - Core's `Index`.

   A second instance rather than the conversion alone, and the reason is generic code. `xs[i]`
   written against a concrete `Array(a)` could go on reaching `Index(Flat(a), Size, a)` through
   convertSlice, exactly as it always did; `fn (Index(c, k, v)) first(xs: c)` cannot, because an
   instance is selected by the type `c` *binds to* and a caller passing an array binds `Array(a)`.
   Instance selection does not convert - nothing about a class says which of its parameters may be
   widened on the way in - so an owner that is to be a `c` has to be one.

   The bodies are the slice's, minus the descriptor. `self.run.items` is the base the slice would
   have copied, so this is one address computation rather than two loads and a temporary; the
   conversion was never buying anything at a subscript, and its absence is why `Array.yana`'s lowered
   form got shorter rather than longer when the class landed.

   The count is not consulted, which is the same tier the slice's accessor is on and goes the same
   way when `checkBounds` (Implementation-Containers.md §15) lands - in one place, since by then
   there is one place.
-}
instance Index(Array(a), Size, a):
    fn get(return self: Array(a), index: Size) -> &a = borrow(self.run.items + index)
    fn getMut(return &self: Array(a), index: Size) -> &a = borrowMut(self.run.items + index)

{-
   `xs[from..to]`, as an ordinary function.

   Half-open, clamped rather than checked: an out-of-range bound produces a shorter slice instead of
   a trap, which is the same tier as the missing bounds check on `get` and goes the same way when
   `checkBounds` (§15) lands.

   `return self` for the same reason `get` has one: the result points into whatever `self` points
   into, so the loan taken where that descriptor was built has to cover the subslice as well. A slice
   is a borrow whose representation is a record, and the marker is what makes the checker treat it as
   one - without it this function is rejected for handing back a view of its own argument.
-}
fn slice(return self: Flat(a), from: Size, to: Size) -> Flat(a):
    let &start = from :: Size
    if start < 0 then start = 0
    if start > self.length then start = self.length

    let &end = to :: Size
    if end < start then end = start
    if end > self.length then end = self.length

    return Flat {items: self.items + start, length: end - start}

{-
   Removes element `index`, moving whatever followed it down over the gap, and answers what it took.

   The element is taken out before the gap is closed, rather than being left to the `copyMemory` that
   writes over it. An assignment releases what it replaces, which is the rule that would have covered
   this - but a block copy is not an assignment the compiler can see. It is Native moving bytes, and
   bytes moving over a live value is exactly the operation that owes its own bookkeeping.

   So `doomed` is where the element goes, and a `->` binding owns what it holds.

   Handing it *back* rather than dropping it here is what makes this usable for an element type the
   caller has somewhere else to put - Implementation-Containers.md §13.3. A caller who does not want
   it writes `remove(xs, i)` and discards the answer, which is the same program it always was: a
   `Maybe` with no name and no use has its last use at the call, and the drop runs there. A caller who
   does writes `Just(->item)` and decides for itself. Nothing in the container has to know which, and
   that is the whole reason this is a return value rather than a second function.

   The move is out of a raw pointer, which is unchecked by construction and correct here for the
   reason the bounds test above it establishes: `index` is inside the initialized prefix, so there is
   a live value at that address to take. Design-Memory's checked world cannot state that, which is
   why the collection is the thing written against Native rather than the caller.
-}
fn remove(&self: Array(a), index: Int) -> Maybe(a):
    -- One comparison and not two. `index` reaches this type unsigned, so a negative one arrives as a
    -- number above every length there is and fails the same test the too-large case fails - which is
    -- the whole reason `Count` is unsigned, and the shape `checkBounds` (§15) will have.
    --
    -- The length needs no ascription of its own: a `@bits` refinement dispatches as the type it
    -- refines, so this is the ordinary `U32` comparison.
    if (index :: U32) >= self.length then return Nothing

    let ->doomed = *(self.run.items + index)

    let rest = (self.length :: Int) - index - 1
    if rest > 0:
        copyMemory(cast(self.run.items + index) :: %U8, cast(self.run.items + index + 1) :: %U8,
                   byteSpan(self.run.items, rest))

    self.length = (self.length :: Int) - 1 :: Count
    return Just(doomed)

{-
   The teardown - Implementation-Containers.md §13.

   One traversal over the live elements, and the release of the run. That is the whole of what a
   container has to write, and it is deliberately written once rather than twice: which *halves* of
   Design-Memory §4's split this supplies is computed from the element type, not declared here. An
   `Array(Int)` has nothing to run at each element, so this is a reclaim and folds to the release; an
   `Array(Connection)` has, so the same body is also the array's `Drop` and the array stops being
   region-eligible - which is the answer that keeps a connection's teardown from being discharged in
   bulk at a point the program never chose.

   `let ->doomed` is how a body says "hand me this element, owned": the binding owes its release and
   never reads it, which is exactly what should happen to a value with nowhere to go. It is the same
   line `remove` uses for the one element it takes out.

   The move is out of a raw pointer, which is unchecked by construction and correct here for the
   reason the loop bound establishes: every index below `length` names a live element. That is what
   `Run(a)` having no notion of occupancy buys - the count is this type's, and so is this walk.
-}
instance Reclaim(Array(a)):
    fn reclaim(->value: Array(a)) -> {}:
        let count = value.length :: Int
        let &i = 0 :: Int

        while i < count:
            let ->doomed = *(value.run.items + i)
            i = i + 1

        releaseRun(value.run)
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
