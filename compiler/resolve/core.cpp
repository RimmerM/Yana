#include "core.h"
#include "intrinsic.h"
#include "simd.h"
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

pub data Bool = False | True
pub data Ordering = LT | EQ | GT

pub data Maybe(a) = Nothing | Just(a)
pub data Result(e, a) = Err(e) | Ok(a)

-- What a continuation reports back to the lens that called it - Analysis-Lens.md §5.1's exit
-- signal. `Proceed` is "the rest of the block finished normally, and here is its value"; `Exit`
-- carries the enclosing function's return value past a frame that has cleanup still to run, so a
-- `withLock` releases its lock on an early `return` from the block below it.
--
-- One type rather than three: `Step(r) = Next | Done(r)` is `Outcome({}, r)`, so the loop signal,
-- the exit signal and a skipping lens's own result are all this. Only the exit signal uses it so
-- far; the constructors are not named `Continue`/`Break` for that reason.
pub data Outcome(a, e) = Proceed(a) | Exit(e)

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
pub class Try(m -> a, e):
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
pub class Rewrap(m, b -> n):
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
pub class Index(c -> k, v):
  fn get(return self: c, index: k) -> &v
  fn getMut(return &self: c, index: k) -> &v

pub class FromInt(a):
  fn fromInt(value: Long) -> a

pub class FromDecimal(a):
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
pub class Eq(a):
  fn ==(lhs: a, rhs: a) -> Bool
  fn !=(lhs: a, rhs: a) -> Bool = !(lhs == rhs)

-- The four comparisons are `compare` read four ways, and `compare` is the fold a derived instance
-- would have to produce. Lexicographic order over a record is therefore one function rather than
-- five, which is what makes Ord worth deriving at all.
pub class (Eq(a)) Ord(a):
  fn <(lhs: a, rhs: a) -> Bool = compare(lhs, rhs) == LT
  fn <=(lhs: a, rhs: a) -> Bool = compare(lhs, rhs) != GT
  fn >(lhs: a, rhs: a) -> Bool = compare(lhs, rhs) == GT
  fn >=(lhs: a, rhs: a) -> Bool = compare(lhs, rhs) != LT
  fn compare(lhs: a, rhs: a) -> Ordering

{-
   Folding a value into a hash state - Implementation-Map.md §3.

   **One member, and it is a fold rather than a function to a number.** That is what makes a
   structural hash compose without an ad-hoc combining function: a record's instance folds field by
   field, a string's folds its units, and a sum's folds its tag and then its payload, each of them
   `hash(part, state)` threaded through. It is the shape Analysis-Derive.md's "for each field"
   template already generates, so `deriving (Hash)` will be one template rather than a special case.

   **`Eq(a)` is a superclass and the obligation is real**: two values that `==` says are equal must
   fold to the same state. Nothing checks it - nothing can - and every instance in this tree is
   written beside the `Eq` it has to agree with for that reason.

   **The state is a `U32` because JS decides it.** A 64-bit state is one multiply natively and three
   operations plus a `bigint` hazard on JS, where `Math.imul` is the only cheap multiply there is.
   That binds the *class*; it does not bind what a map does between the instance and the slot, which
   is why Collections' native fold below is free to do its arithmetic at sixty-four bits and hand
   back thirty-two.

   An instance may be as cheap as its type allows - `fn hash(value, state) = state `xor` value` is
   legal and is one instruction - because a map finalizes with `mix32` before it splits the word.
   That is what makes a weak instance merely slow instead of quadratic.
-}
pub class (Eq(a)) Hash(a):
  fn hash(value: a, state: U32) -> U32

{-
   The finalizer - murmur3's `fmix32`, and the one piece of the hash that is the same on both
   targets.

   Five operations, no table and no branch, and every one of them is something a 32-bit machine and
   `Math.imul` both do in one step. What it buys is stated in Implementation-Map.md §3: a map takes
   the slot from the *low* bits of the finalized word and the seven-bit tag from the *top*, so a
   hash whose entropy sits at one end would otherwise put every key of a table into one tag. This is
   what makes both ends of the word carry the whole of it.

   In `Core` and not in `Collections` because it is arithmetic and nothing else - no storage, no
   target, no allocation - and because an instance author writing a hash for their own type has the
   same use for it that the map does.
-}
pub fn mix32(value: U32) -> U32:
    let &h = value
    h = h `xor` (h `shr` 16)
    h = h * 2246822507
    h = h `xor` (h `shr` 13)
    h = h * 3266489909
    return h `xor` (h `shr` 16)

-- Anything that can be added can be counted from, so `fn (Num(a)) inc(x: a) = x + 1` compiles as
-- written rather than making the author declare FromInt as well. FromInt stays its own class so
-- that a Duration or a units newtype can be integer-literal-constructible without also claiming
-- to support multiplication.
--
-- Negation is the one place the superclass earns its keep as a default: `0` is `FromInt(a)` and
-- the subtraction is this class's own, so unary `-` needs nothing an instance has not already
-- promised. An instance for which that is not the negation it wants overrides it.
pub class (FromInt(a)) Num(a):
  fn +(lhs: a, rhs: a) -> a
  fn -(lhs: a, rhs: a) -> a
  fn *(lhs: a, rhs: a) -> a
  fn /(lhs: a, rhs: a) -> a
  fn -(value: a) -> a = 0 - value

{-
   Joining two of something - what `"a" + "b"` is, and what `Num` must not be made to mean.

   `+` on strings is Implementation-String.md part 8's, and the shape of the problem is that `Num`'s
   `+` cannot serve it: `Num` has `FromInt` as a superclass, so an instance of it would have to say
   what the string `3` is, and it would also bring `-`, `*` and `/`, none of which joining has an
   answer for. Widening the class to fit would make every numeric type promise something about
   concatenation. So this is a second class, and `+` is overloaded on it exactly as a name may be
   overloaded on any two classes - selection is by the operand type, and a type in only one of them
   reaches only that one.

   Declared here rather than beside `String` for the reason `Index` is: it is a class a *user* type
   may join - a rope, a builder, a list - and a class only Collections could name would not be one.
   It is deliberately not called `Monoid`: there is no identity element in it, because a type that
   can join two values need not have an empty one and requiring it would rule out the non-empty
   containers that are the obvious second instance.
-}
pub class Append(a):
  fn +(lhs: a, rhs: a) -> a

{-
   Turning a value into text - Implementation-Storage.md part 7, whose `Storage(StringUnit)` sink is
   `String` itself under Implementation-Containers.md §0.

   Two methods, and the second is what makes the whole scheme cheap.

   **The sink is a `String` and not a `Writer` interface.** Growable `String` *is* the sink, so
   formatting builds the result in place and finishing it is a reinterpretation rather than a copy.
   An abstraction over sinks - formatting straight to a file descriptor - is a later addition and
   deliberately not something every type in the standard library has to implement against.

   **`showBound` is a `Maybe(Size)`, and its constant-ness is not in the type.** `Just(11)` for `Int`
   is an ordinary expression that folds to `11` once the instance method is inlined at a concrete
   call site; `Just(length(s))` for `String` is an ordinary expression that does not. The compiler
   learns "this is a compile-time constant" from the folder rather than from a second type-level
   channel, so there is exactly one thing for an instance author to write and no way for two channels
   to disagree. `Nothing` is the honest answer for a container, where measuring costs what formatting
   costs and there is no reason to pay twice.

   **The contract is that `show` writes at most `showBound` units when it is `Just`**, and an
   instance that lies about it is unsafe in the same trust tier as `@reclaimOnly`. What makes the
   sizing sound without a purity rule is the borrow checker: the formatter holds the value across
   both the measure and the write, so nothing can change it in between. A `showBound` with side
   effects is still a bad instance; it cannot make the buffer wrong.
-}
pub class Show(a):
  fn show(value: a, &to: String) -> {}
  fn showBound(value: a) -> Maybe(Size) = Nothing

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
-- It is declared `infixl` rather than `infixr` because `a += b += c` is nonsense under either
-- reading, the result being unit, and the left one at least reports it at the outer operator.
pub fn (Num(a)) +=(&target: a, amount: a) -> {}:
    target = target + amount

pub fn (Num(a)) -=(&target: a, amount: a) -> {}:
    target = target - amount

pub fn (Num(a)) *=(&target: a, amount: a) -> {}:
    target = target * amount

pub fn (Num(a)) /=(&target: a, amount: a) -> {}:
    target = target / amount

pub class (Num(a)) Integral(a):
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
pub class Logic(a):
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
pub class Truth(a):
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
pub fn ??(->value: Maybe(a), @lazy fallback: a) -> a = match value:
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
pub class Copy(a):
  fn copy(from: a) -> a

-- `to` arrives uninitialized and must be fully initialized before returning. That obligation is
-- Design.md's `@uninit &`, which is not implementable yet - nothing in the language produces
-- uninitialized storage for a caller to pass - so it is documented here and unchecked. When the
-- attribute lands this signature gains it and nothing else about the class changes.
pub class Sink(a):
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
pub class Reclaim(a):
  fn reclaim(->value: a) -> {}

-- Run once when a live instance dies, at its last use rather than at the end of its scope. Never
-- run on a location a value has been moved out of, which is what the drop flags exist to know.
pub class Drop(a):
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
pub class TrivialCopy(a)
pub class TrivialSink(a)

-- A Widen instance is required to be lossless and total; that is a contract on whoever writes
-- one, checked no more than Copy's is. Which of the two classes relates a pair of types is the
-- whole of the rule for whether a conversion happens on its own or has to be written.
pub class Widen(a, b):
  fn widen(from: a) -> b

-- The lossy direction, and the only way to spell it: `::` widens and selects, it does not remove
-- data. The method is `truncate` rather than `narrow` because "narrow" says the type got smaller
-- and "truncate" says what happened to the value, which is the part a reader needs. The class keeps
-- its name so that the pair reads as one rule.
pub class Narrow(a, b):
  fn truncate(from: a) -> b

{-
   The third thing a conversion can be: the same bits, read as another type.

   Two parameters and no functional dependency, which is `Widen`/`Narrow`'s shape exactly - so
   selection from context and `bitcast(x) :: Float` both work with no machinery of their own.

   **Instances only where the widths match**, which is what makes this a class rather than a second
   `cast`: every rung is generated over a same-width pair, so there are about thirty of them against
   the ladder's ninety and none of them can lose or invent a bit. It replaces four unrelated
   spellings - `Native.cast` between pointers, `asInt` and `asPtr` between a pointer and an integer,
   and nothing at all between `Float` and `I32`.

   The pointer rungs are declared in `Native` and are the one deliberate weakening this took:
   instance visibility is program-wide, so `bitcast` reaches a pointer from any module in a program
   where any module imported Native, where `Native.cast` had to be named through the module. Taken
   knowingly - one spelling for reinterpretation was judged worth more than the qualifier.
-}
pub class Bitcast(a, b):
  fn bitcast(from: a) -> b

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
pub fn swap(&left: a, &right: a) -> {}
pub fn exchange(&slot: a, ->value: a) -> a

{-
   Building a vector, reading one lane of it, and folding one down to a scalar.

   The part of Design-Vector §3.3 that has to exist before anything else about vectors can be
   written at all: `Vec(a)` is a type a program can name from stage 1 onward, and until there is a
   `splat` there is no way to *make* one, so every backend below the resolver had to be tested from
   hand-written IR. These seven are what close that, and they are the smallest set that does -
   construction, one lane in each direction, and the four reductions.

   Generic intrinsics with no bodies, like `swap` above: there is one operation per lane type, so
   there is nothing to generate until a call says which, and `expandIntrinsic` generates it where it
   is called. None of them is ever a call in the IR.

   `index` is a compile-time constant and the expansion says so - Design-Vector §3.3, and the reason
   is that a runtime lane index is `pshufb` on x86 and does not exist at all on some targets.

   The count is a const parameter wherever an *argument* supplies it - Implementation-Const-Generics.md
   §5's first row, and Implementation-Vector.md §9.3's gap closed. `Vec(a)` in a parameter is the
   target's natural width and nothing else, so `lane(v, 0)` on a `Vec(I16, 4)` had no overload at all
   on a target whose natural count is eight. `splat`, `zero` and `iota` keep the natural form: what
   decides their count is the *result*, and a const parameter there would be one nothing supplies.
-}
pub fn splat(value: a) -> Vec(a)
pub fn (n: Int) lane(vector: Vec(a, n), index: Int) -> a
pub fn (n: Int) withLane(vector: Vec(a, n), index: Int, value: a) -> Vec(a, n)

{-
   The reductions, whose *order* is a stated language property rather than an implementation detail -
   Design-Vector §4.5. Each of these is the adjacent-pair tree `(a0+a1) + (a2+a3)`, on every target,
   which is what keeps a float sum from answering different bits under two vector widths of one
   backend or under two backends of one width.
-}
pub fn (n: Int) horizontalSum(vector: Vec(a, n)) -> a
pub fn (n: Int) horizontalProduct(vector: Vec(a, n)) -> a
pub fn (n: Int) horizontalMin(vector: Vec(a, n)) -> a
pub fn (n: Int) horizontalMax(vector: Vec(a, n)) -> a

{-
   The rest of the portable set - Design-Vector §3.3.

   `lanes` is the one that is not an instruction: it answers a number the *type* already carries, so
   the expansion is a constant and there is nothing left of the call. `zero` and `iota` take no
   argument at all and are selected by what the result is asked to be, which is the ordinary
   return-type selection `truncate(x) :: U8` uses.

   `min` and `max` are here rather than in `Ord` for the reason `Eq` and `Ord` have no vector
   instances: they are lanewise, they answer a vector, and the class that would relate them to a
   scalar comparison answers a `Bool`. `abs` is the same family - the value with its sign removed,
   per lane.

   The rearrangements take their pattern from the *declaration* and not from an argument, which is
   what keeps them portable: a runtime shuffle is `pshufb` on x86 and does not exist at all on some
   targets, so a pattern this resolver cannot read is not expressible rather than slow. `rotate`'s
   distance is therefore a constant, checked where it is written.
-}
pub fn zero() -> Vec(a)
pub fn iota() -> Vec(a)
pub fn (n: Int) lanes(vector: Vec(a, n)) -> Int

pub fn (n: Int) min(lhs: Vec(a, n), rhs: Vec(a, n)) -> Vec(a, n)
pub fn (n: Int) max(lhs: Vec(a, n), rhs: Vec(a, n)) -> Vec(a, n)
pub fn (n: Int) abs(vector: Vec(a, n)) -> Vec(a, n)

{-
   The square root and the fused multiply-add, which are the two operations in the portable set that
   needed an instruction of their own rather than an arrangement of the ones that were here.

   **One declaration each, over an unconstrained `a`, and it covers the scalar and the vector both.**
   Nothing in this language had a square root at any width, so what §3.3 asks for over lanes is the
   thing a scalar wanted all along - and `sqrt(x)` on a `Double` and `sqrt(v)` on a `Vec(Float)` are
   the same instruction over two types rather than two functions. The intrinsic reads which it was
   handed; the signature does not have to say, and a second declaration is not expressible anyway,
   one function per name per module being a hard limit.

   `a` is unconstrained rather than bounded by a class, because the class that would bound it does
   not exist: there is no `Float(a)` relating a float to a vector of floats, and inventing one to
   carry a fact the compiler can read off the type would be a class per operation. What holds the
   argument to a float instead is the intrinsic's own diagnostic, at the call, naming the type.

   `fma` is `a * b + c` with **at most one rounding**, which is what makes it a function rather than
   two operators. Design-Vector §3.3 rules that it *may* fuse: a target with a fused instruction
   gives one rounding, a target without gives two, and a program that must not fuse writes
   `a * b + c` and gets two everywhere. Both answers are correct and the difference is observable,
   which is why the permission is stated rather than assumed.

   Floating lanes only, on both. A square root of an integer is a question about rounding no machine
   answers, and a fused multiply-add of two is the ordinary pair with nothing fused about it - so
   each is refused at its call with a diagnostic naming the type rather than silently truncating.
-}
pub fn sqrt(value: a) -> a
pub fn fma(lhs: a, rhs: a, addend: a) -> a

{-
   The conversions that change the lane *count*. The ones that keep it are `Widen`, `Narrow` and
   `Bitcast` over vector types and are not functions at all - Design-Vector §3.4.

   Each is a shuffle and a cast, which is what the verifier's refusal of a lane-count-changing `Cast`
   already said one is. What decides the result is the *call*, since nothing in the argument does:
   `unpackLow(v) :: Vec(I32)` is how one is written, and asking for a shape that is not half and whole
   of one width is reported at the call.
-}
pub fn unpackLow(vector: Vec(a)) -> Vec(b)
pub fn unpackHigh(vector: Vec(a)) -> Vec(b)
pub fn packLanes(lhs: Vec(a), rhs: Vec(a)) -> Vec(b)

pub fn (n: Int) reverse(vector: Vec(a, n)) -> Vec(a, n)
pub fn (n: Int) rotate(vector: Vec(a, n), by: Int) -> Vec(a, n)
pub fn (n: Int) interleaveLow(lhs: Vec(a, n), rhs: Vec(a, n)) -> Vec(a, n)
pub fn (n: Int) interleaveHigh(lhs: Vec(a, n), rhs: Vec(a, n)) -> Vec(a, n)

{-
   Comparing lanes, and choosing between two vectors by the answer - Design-Vector §3.2.

   A class rather than six functions, and the functional dependency `v -> m` is what earns it: `a .<
   b` has to infer its own result without an ascription, and one vector shape has exactly one mask
   shape. It is the same machinery `class Chunked(c -> a)` rides on.

   The dotted spellings are borrowed from Fortran and Julia and are ordinary declarable operators
   here - a symbol run beginning with `.` lexes as one operator, so nothing in the parser changes.
   They sit at the precedence of the comparisons they mirror, which is what makes `a .< b `and` c .<
   d` group the way `a < b and c < d` does.

   `select` is in the class rather than beside it because it is the only operation that relates a
   mask back to the vector it came from, and a class keyed on the pair is where that relation lives.
-}
infixl 3 .==
infixl 3 .!=
infixl 3 .<
infixl 3 .<=
infixl 3 .>
infixl 3 .>=

pub class Lanewise(v -> m):
  fn .==(lhs: v, rhs: v) -> m
  fn .!=(lhs: v, rhs: v) -> m
  fn .<(lhs: v, rhs: v) -> m
  fn .<=(lhs: v, rhs: v) -> m
  fn .>(lhs: v, rhs: v) -> m
  fn .>=(lhs: v, rhs: v) -> m
  fn select(mask: m, ifTrue: v, ifFalse: v) -> v

{-
   Reading a mask - Design-Vector §3.2.

   `firstSet` is the one that makes a search loop terminate correctly, and it is why this family is
   portable rather than left to a platform module: without it, "which lane matched" is a `movemask`
   and a bit scan on one target and something else everywhere else. It answers the lane count when
   nothing is set, so `firstSet(m)` indexes a chunk without a preceding `any`.

   Masks get `and`, `or`, `xor` and `not` from `Logic`, whose instance simd.cpp generates - so the
   combining of two masks is written the way the combining of two conditions is.
-}
pub fn any(mask: Mask(a)) -> Bool
pub fn all(mask: Mask(a)) -> Bool
pub fn none(mask: Mask(a)) -> Bool
pub fn count(mask: Mask(a)) -> Int
pub fn firstSet(mask: Mask(a)) -> Int

{-
   The first `count` lanes set and the rest clear - the tail mask, and the one operation in this
   family that is about a *length* rather than about a comparison somebody wrote.

   It is here rather than left to `iota() .< splat(count)` for a reason that is not brevity: written
   that way the count has to reach the lane type, and a lane narrower than an `Int` cannot be handed
   one. `Vec(I16)`'s tail mask is not expressible in source at all, and every wider one is expressible
   only because the conversion happens to widen. The count is a lane *index*, bounded by the lane
   count and so by 64, so what the conversion means is settled - which is exactly the kind of fact a
   signature cannot carry and an intrinsic can.

   Design-Vector §4.4 asks for this to be a load from a `.rodata` table indexed by the count, on the
   grounds that a short chunk has no full-width iterations to amortize a computed mask against. It is
   the comparison today; the table is a change behind this name and nothing that calls it moves.
-}
pub fn maskUpTo(count: Int) -> Mask(a)
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

    auto swap = resolver.emit<InstSwap>(source, StringId(), resolver.module.scalar.unit,
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

    // The compiler supplies these rather than Core's source, so nothing wrote `pub` on one. They are
    // as public as anything else in Core: `Int` has to be nameable from every module there is.
    value->exported = true;

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
    type->exported = true;

    auto pointer = (Type*)type - *module.types;
    module.namedTypes.add(id, pointer);
    return pointer;
}

// Whether a value of `from` fits in `to` without losing anything: more bits, or the same bits
// without a sign to lose. This decides which of the two conversion ladders a pair joins, which
// is the whole of the rule for whether a conversion happens on its own or has to be written.
static bool widens(GlobalBase global, TypePtr from, TypePtr to) {
    return integerRangeFits(*(IntType*)global[from], *(IntType*)global[to]);
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

/*
 * How wide a primitive is for the purpose of reinterpreting it, or zero where the question does not
 * apply.
 *
 * The type's *own* bits and not its register's, because that is what decides whether two types hold
 * the same value: `WideInt` is 53 bits in a 64-bit register, so nothing else has its shape and it
 * gets no rung. `Bool` is excluded by the same rule for a better reason - it is one bit, and a
 * reinterpretation of one bit is a question about the other seven that nothing here can answer.
 */
static U16 reinterpretWidth(GlobalBase global, TypePtr type) {
    if(global[type]->kind == Type::Float) {
        return ((FloatType*)global[type])->width == FloatType::Float ? 32 : 64;
    }

    if(global[type]->kind != Type::Int) return 0;

    auto& integer = *(IntType*)global[type];
    return integer.canonical || integer.bits <= 1 ? 0 : integer.bits;
}

/*
 * The reinterpretation ladder - one `Bitcast` rung per ordered pair of distinct primitives of one
 * width, which is about thirty against the conversion ladder's ninety.
 *
 * Same width is the whole of the safety argument, so there is no test anywhere else: nothing
 * downstream has to ask whether a `bitcast` fits, because no instance relating a pair that does not
 * was ever generated.
 *
 * **JS declines the pairs that cross between a 64-bit integer and a `Double`.** Not because they
 * cannot be expressed - a `DataView` round trip would - but because a `bigint` going through one is
 * not a reinterpretation in any useful sense: it is a heap value on one side and a `number` on the
 * other, and the cost of the trip is larger than anything a program would have reached for a
 * bitcast to save. The 32-bit `Float`/`I32` pairs *are* generated on both targets, through the
 * scratch typed-array pair codegen/js/inst.cpp emits, because there a bitcast is the only way to
 * see a float's bits at all.
 */
static void defineBitcastLadder(Module& module, TypeList& types) {
    GlobalBase global = *module.types;
    auto onJs = isJsMode(module.context.settings.mode);

    for(Size from = 0; from < types.size(); from++) {
        auto fromWidth = reinterpretWidth(global, types[from]);
        if(!fromWidth) continue;

        for(Size to = 0; to < types.size(); to++) {
            if(from == to || reinterpretWidth(global, types[to]) != fromWidth) continue;

            auto crossesFloat = (global[types[from]]->kind == Type::Float) !=
                                (global[types[to]]->kind == Type::Float);

            if(onJs && fromWidth == 64 && crossesFloat) continue;

            defineBitcast(module, types[from], types[to]);
        }
    }
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
                defineConversion(module, "Narrow"_v, "truncate"_v, types[from], types[to]);
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

    /*
     * `String` - a primitive here rather than a `data` declaration in Collections, for the reason
     * Type::String gives: the two targets disagree about what a string *is*, and on JS it has to be
     * the bare host value rather than a record wrapping one.
     *
     * Its content type is filled in by `defineNative`, since the record naming it is declared there
     * and this runs first. Nothing asks for a string's layout until lowering.
     */
    program.scalar.string_ = addPrimitive(program, *module, "String"_v, new (program.types) StringType());

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
    program.scalar.size = sizeType;

    // `Size` read the other way, which is not a second index type: it is what a bounds test compares
    // at, so that one comparison rejects a negative index as well as one past the end. See the note
    // above on why the index itself is signed, and Implementation-Containers.md §10.2 for the
    // unsigned counts this meets.
    program.scalar.unsignedSize = isJsMode(context.settings.mode) ? coreType(*module, "U32"_v)
                                                                  : coreType(*module, "U64"_v);

    /*
     * `CodeUnit` - one unit of a string's native encoding, target-selected exactly as `Size` is -
     * Implementation-Vector.md §9 item 8, Design-Vector §4.6.
     *
     * A UTF-8 byte natively and a UTF-16 unit on JS, which is the same split
     * Implementation-String.md part 3 already makes for `length`: what is uniform across targets is
     * the *complexity class* of an operation over units, not the number of them. A program that
     * names this type is saying "the encoding's own unit", which is what the ASCII scanning family
     * takes and what a `Chunked(String, CodeUnit)` yields - and an ASCII value means the same thing
     * in both, which is the self-synchronizing property that whole family rests on.
     */
    auto unitType = isJsMode(context.settings.mode) ? coreType(*module, "U16"_v) : coreType(*module, "U8"_v);
    module->namedTypes.add(context.addQualifiedName("CodeUnit", 8, 1), unitType);

    /*
     * `F32` and `F64` - Implementation-Vector.md §9 item 1.
     *
     * Names for `Float` and `Double` and nothing else, on exactly the terms `Size` is a name for
     * `I64`: no type, no instances, no conversion to or from what they are. What they buy is that a
     * signature which names widths can name all of them the same way - `Vec(F32)` beside `Vec(I32)`
     * reads as one family where `Vec(Float)` beside `Vec(I32)` reads as two - and vector code is
     * where that comes up, because a lane width is the thing being said.
     */
    module->namedTypes.add(context.addQualifiedName("F32", 3, 1), program.scalar.float_);
    module->namedTypes.add(context.addQualifiedName("F64", 3, 1), program.scalar.double_);

    /*
     * The vector constructors - Design-Vector §2, Implementation-Vector.md §1.4.
     *
     * Two interned names and no declarations, because there is nothing for a declaration to say: a
     * `Vec(Float)` is four lanes or eight depending on the target, so what it *is* comes from
     * `targetVectorBytes` rather than from a body. `resolveApp` recognizes the names; see
     * Program::vecTypeName for what reserving them costs.
     *
     * The signed family beside them is the integer of each lane width, which is what a lane *number*
     * is counted in - see `ScalarTypes::signedLanes` and `maskUpTo`.
     */
    program.vecTypeName = context.addQualifiedName("Vec", 3, 1);
    program.maskTypeName = context.addQualifiedName("Mask", 4, 1);

    program.scalar.signedLanes[0] = coreType(*module, "I8"_v);
    program.scalar.signedLanes[1] = coreType(*module, "I16"_v);
    program.scalar.signedLanes[2] = coreType(*module, "I32"_v);
    program.scalar.signedLanes[3] = coreType(*module, "I64"_v);

    resolveModuleDecls(*module, *ast, nullptr);

    attachIntrinsic(*module, "swap"_v, emitSwap);
    attachIntrinsic(*module, "exchange"_v, emitExchange);

    // The portable vector set, whose declarations are in the source above and whose expansions are
    // simd.cpp's - Design-Vector §3.3.
    defineVectorIntrinsics(*module);

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

    /*
     * The same instances over the vector of each are **not** here - simd.cpp generates them where
     * they are asked for, and simd.h says why.
     *
     * The short of it: this loop is what Implementation-Vector.md §9 item 1 describes, and it worked
     * for the four natural-width vectors it covered. Item 1's remaining half is every lane type at
     * every lane count and item 2 is the conversion ladder over the *pairs* of those, which is about
     * seven hundred instances - carried by every program in the language, in an IR arena that holds
     * a program of one to two thousand functions. Generating one when a head is asked for and not
     * before is the same rules at a cost the language can pay.
     */

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
                defineConversion(*module, "Narrow"_v, "truncate"_v, numeric[from], numeric[to]);
            }
        }
    }

    // The width types join the same classes, after the ladder above rather than before it: their
    // own ladder reaches `Int` and `Long`, and skips that one pair on the grounds that it has just
    // been declared here.
    defineIntegerInstances(*module, widthTypes);

    /*
     * And the identity rung of the narrowing ladder, one per type, which exists so that `truncate`
     * is *total* over the types it relates.
     *
     * Taking the low bits of a value at its own width is the identity, so each of these emits
     * nothing - and none of them is ever selected implicitly or by `::`, both of which answer
     * `sameType` and return before any instance is looked for. What they are for is portable source.
     *
     * `Size` is `I64` natively and `Int` on JS, so `truncate(length(xs)) :: Int` is a real
     * truncation on one target and the identity on the other. Without a rung for the second case
     * there is *no* spelling of "this length is an `Int` now" that compiles on both, and the program
     * would have to be split by `@platform` over a conversion. §0.1.1 is what made that visible:
     * while `::` narrowed, the identity case went through `sameType` and the question never arose.
     */
    widthTypes.push(program.scalar.float_);
    widthTypes.push(program.scalar.double_);

    for(auto type: widthTypes) defineConversion(*module, "Narrow"_v, "truncate"_v, type, type);

    // And the reinterpretation ladder over everything both ladders cover, which is why it runs last:
    // `defineIntegerInstances` appended `Int` and `Long` to this list, and the two floating types
    // have just joined it, so this is the whole of what `Bitcast` is generated over.
    defineBitcastLadder(*module, widthTypes);

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
    program.coreClasses.show = classNamed(*module, "Show"_v);
    program.coreClasses.copy = classNamed(*module, "Copy"_v);
    program.coreClasses.sink = classNamed(*module, "Sink"_v);
    program.coreClasses.reclaim = classNamed(*module, "Reclaim"_v);
    program.coreClasses.drop = classNamed(*module, "Drop"_v);
    program.coreClasses.trivialCopy = classNamed(*module, "TrivialCopy"_v);
    program.coreClasses.trivialSink = classNamed(*module, "TrivialSink"_v);

    // The five a vector joins on demand, for the reason CoreClasses gives: no instance of any of
    // them over a vector is declared anywhere, so "could a vector join this" is asked at every
    // instance lookup that finds nothing and must not be a string lookup.
    program.coreClasses.num = classNamed(*module, "Num"_v);
    program.coreClasses.integral = classNamed(*module, "Integral"_v);
    program.coreClasses.logic = classNamed(*module, "Logic"_v);
    program.coreClasses.bitcast = classNamed(*module, "Bitcast"_v);
    program.coreClasses.lanewise = classNamed(*module, "Lanewise"_v);

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
import Host

{-
   Where a check the compiler inserted goes when it does not hold - Implementation-Containers.md §15.

   Here rather than in `Native` or in `Host` because it is the one thing that has to exist on *both*
   targets, and neither of those does: a native build resolves no declaration of `Host` at all, and a
   JS build excludes every function of `Native` from emission. Collections imports both and is
   implicitly visible everywhere, which makes it the lowest module a check can be emitted into from
   anywhere in the program.

   No message, and that is a real limitation rather than an oversight: printing one needs `Text`,
   which imports this module. What each target does say is the most its own conventions allow -
   status 134 natively, which is what a process killed by SIGABRT reports, and a thrown string on JS,
   where the emitter supplies the sentence (see NativeOp::HostThrow).

   The compiler calls this rather than a program doing so; `Program::checkFailed` is how it is found.
   Not `pub`, and that is the difference between it and `checkCondition` below: what a check *is* is
   this module's to decide, and a program that reached in and called it would be aborting for a
   reason no check found.
-}
@platform(native) fn checkFailed() -> {} = abortProcess()
@platform(js) fn checkFailed() -> {} = hostFail()

{-
   One check, with its branch inside it.

   The compiler emits a *call* to this rather than the `if` itself, and that is the whole reason it
   exists: a subscript is expanded inside whatever expression contains it, so a branch emitted there
   splits a block underneath a construct that had already decided which blocks it owns. Here the
   branch is in a function of its own, where nothing is looking. Ordinary inlining removes the call
   in an optimized build; an unoptimized one pays for it, which is the right way round for a check.

   Written as the *mistake* rather than as the invariant - the caller passes `index >= length` - so
   that the branch this contains is predicted the way it will actually go.
-}
pub fn checkCondition(failed: Bool) -> {}:
    if failed then checkFailed()
    return

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
@platform(native) pub data Array(a) {run: Run(a), length: Count}

{-
   And the same array on JS - Implementation-Containers.md §14.

   The host array, and how many of its slots are live. There is no run because there is no allocation
   to place and no capacity because the host reports its own - which is §1 working exactly as stated:
   what varies between implementations is the *owner*, and the borrow every reader was compiled
   against is `Flat(a)` on both targets.

   `@host` is what keeps the count from costing anything on the row that does not need it. §14 has
   two rows and the element chooses: a plain host array's `length` *is* its occupancy and assigning
   it truncates, so the count is a number the host already keeps and the record is the bare array;
   a `TypedArray`'s is its fixed capacity, so there the field is stored and the record is an object
   holding both. One declaration, one set of bodies, and the layout follows the element - see
   `Field::host` and `hostPropertiesElided`.

   So every function below has two bodies and one signature, and nothing outside this module and the
   two compiler paths that *build* a container (`resolveArray` and `convertSlice`) knows which target
   it is on. A teardown is still the authored traversal over the live elements, because whether an
   element has a teardown is a question about the element rather than about where the container keeps
   it.
-}
@platform(js) pub data Array(a) {items: %a, length: @host Count}

-- An array with room for nothing. The first push allocates natively; on JS `[]` is the allocation
-- and there is nothing left to defer.
@platform(native) pub fn emptyArray() -> Array(a) = Array {run: emptyRun(), length: 0}
@platform(js) pub fn emptyArray() -> Array(a) = Array {items: hostArray(), length: 0}

-- What the container can hold before it has to grow. A host array has no such number to report and
-- answers what it holds, which is the honest answer for a container that never has to grow.
@platform(native) pub fn capacity(self: Array(a)) -> Size = self.run.capacity :: Size
@platform(js) pub fn capacity(self: Array(a)) -> Size = hostLength(self.items) :: Size

{-
   Room for `wanted` elements.

   The capacity test and nothing else, with the growth behind `growArray`. Split that way on purpose,
   and the split is what makes this function *disappear* at its call sites: written as one body it is
   twenty instructions in ten blocks, which is over any budget the inliner has, so `push` was a call
   per element and the caller's own bounds check reloaded the capacity one instruction after that call
   returned - a load nothing may forward across a call, a check nothing may discharge, and an address
   nothing may hoist out of the loop the push sits in.

   Written as a test and a tail call it is four instructions in two blocks, the inliner takes it under
   the ordinary budget with no rule of its own, and what lands in the caller is exactly the test. See
   §14.2 of `test/bench/findings.md` for what that is worth and for why the same shape recognised in
   the *compiler* is not: inlining the whole body carries the growth into the caller's hot loop, and
   the two cancel.
-}
@platform(native) pub fn reserve(&self: Array(a), wanted: Size) -> {}:
    if wanted <= (self.run.capacity :: Size) then return
    growArray(self, wanted)

{-
   The growth, which runs once per doubling.

   Doubling with a floor, and nothing else - relocation is the run's. The `&` on `resize`'s own `self`
   is what makes that safe with no rule of this module's own: a mutable borrow of the run conflicts
   with any live borrow into it, so a caller holding an element borrow across a push is rejected by
   the ordinary check rather than by anything written here.

   Not `pub`: it is `reserve`'s cold half and re-tests nothing, so calling it directly would grow an
   array that already had the room.

   `@noinline` because the whole point of the split is that this body stays where it is, and the one
   term that would otherwise take it back is `soleCallSite`: a program with a single `push` leaves
   this with one caller, and a body with one caller is one the inliner moves rather than copies. That
   is a size win and a speed loss - what it moves is a doubling and a `resize` call, into the loop the
   push sits in - so the attribute says which of the two this function is for.
-}
@platform(native) @noinline fn growArray(&self: Array(a), wanted: Size) -> {}:
    let room = self.run.capacity :: Size
    let &wide = room + room :: Size
    if wide < wanted then wide = wanted
    if wide < 4 then wide = 4

    resize(self.run, wide)

{-
   Room for `wanted` elements on JS, which is two answers chosen by the *element* -
   Implementation-Containers.md §14's two rows.

   A host array grows on its own: writing at its length appends, so the engine's own policy applies
   and reserving in front of it would be asking for a second policy on top of one that is not this
   module's to see. A `TypedArray` has a fixed length and cannot, so it doubles exactly as the native
   run does.

   The row is chosen by `hostFixedCapacity`, which is a constant per element type - so one of the two
   bodies below is folded away before any call site sees it, and this is one function rather than a
   `@platform` split that could not have been written: `@platform` chooses by *target*, and both rows
   are this target.
-}
@platform(js) pub fn reserve(&self: Array(a), wanted: Size) -> {}:
    if hostFixedCapacity(self.items) == False then return
    if wanted <= hostLength(self.items) then return
    growJsArray(self, wanted)

-- The doubling, which is the native `growArray` with `hostGrow` where `resize` is. `@noinline` and
-- not `pub` for the two reasons that one gives.
@platform(js) @noinline fn growJsArray(&self: Array(a), wanted: Size) -> {}:
    let room = hostLength(self.items)
    let &wide = room + room :: Size
    if wide < wanted then wide = wanted
    if wide < 4 then wide = 4

    self.items = hostGrow(self.items, wide)

{-
   Appends an element.

   `->item` and not `item`, because the array *takes* it: what the store writes into the run is a
   live value the caller no longer owns, and a borrowed parameter would leave the caller's own drop
   to release something the array is still holding. The write is what transfers it - a store through
   a raw pointer is an assignment the ownership pass reads as a hand-over - so nothing is released
   at the end of this body on the path that stored.

   A reserve that could not get the room is a check that failed, and it is spelled as one.

   It used to `return` instead: the array was left as it was, the element died here rather than being
   leaked, and the caller was told nothing. That is a silent wrong answer - the array simply stops
   growing and every push after it discards its argument - and it was reachable, because the heap was
   a single 4 MiB region and an `[Int]` hit the end of it at 131072 elements (see `growHeap` in
   Native, which is the reason that particular failure no longer happens). What is left is a genuine
   refusal from the kernel or a `resize` past what `Count` can hold, and neither is something a
   program can be allowed to walk past.

   `checkCondition` and not a diagnostic with a message, on the same terms as every bounds check:
   what it can say is 134 natively and a throw on JS, because printing a sentence needs `Text` and
   `Text` imports this module. Reporting it *as a value* is still `Result`'s job, which still needs
   the array first - the difference is that until then the program stops instead of continuing on a
   container that quietly lost an element.

   The store is now on the only path out, which is why nothing is released at the end of this body:
   `item` is handed over on the path that continues and `checkFailed` does not return.
-}
@platform(native) pub fn push(&self: Array(a), ->item: a) -> {}:
    let count = self.length :: Size
    reserve(self, count + 1)
    checkCondition(count >= (self.run.capacity :: Size))

    store(self.run.items + count, item)
    self.length = truncate(count + 1) :: Count

{-
   The same transfer, and the same *kind* of transfer - which is the whole reason this is a write
   rather than `.push`.

   `arr[arr.length] = item` is an assignment through a place, so the ownership pass reads the
   hand-over out of it exactly as it reads the one out of `store(self.run.items + count, item)`
   above, and nothing is released at the end of this body. `.push(item)` appends just as well and
   passes the element as an *operand*, which is a use - so the frame went on owning a value the array
   was holding and released it. Host's own note says the same thing from the other side.
-}
@platform(js) pub fn push(&self: Array(a), ->item: a) -> {}:
    let count = self.length :: Size
    reserve(self, count + 1)

    hostWrite(self.items, count, item)
    self.length = truncate(count + 1) :: Count

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
{-
   How many elements a container holds, as a class - Implementation-Containers.md §17's reasoning
   applied to the other universal operation.

   `length` was a plain function over `Flat(a)`, which served every container by the ordinary
   owner-to-slice conversion at the call. `String` cannot reach it that way: it is not a container of
   elements with a buffer address, so there is no slice for it to convert to, and its length lives in
   a different place on each target. A second plain `fn length` is a duplicate declaration rather
   than an overload, so the name has to become a class for the two to coexist - which is exactly what
   happened to `xs[i]` when `Index` landed, and for the same reason.

   The container instances are the same set `Index` has and are there for the same reason: instance
   selection does not convert, so an `Array(a)` binds `Array(a)` and needs an instance of its own
   rather than reaching the slice's through the conversion a *call* would have performed.

   **Generated rather than written, on the same terms `Index`'s are - see the note in `Native`.**
   Written out they would read:

       instance Length(Flat(a)):
           fn length(self: Flat(a)) -> Size = self.length

       @platform(native) instance Length(Array(a)):
           fn length(self: Array(a)) -> Size = self.length :: Size

       @platform(js) instance Length(Array(a)):
           fn length(self: Array(a)) -> Size = hostLength(self.items)

   One field load each, and the JS one had a second reason beyond the shared one: `hostLength` is an
   `InstNative`, and `Value::Native` was not in `clonableKind`, so no call site could ever have seen
   through `Length(Array(a)).length` however willing the inliner was made. That half has since been
   closed - a host node is clonable now - and what is left is the shared argument, which is enough on
   its own. `String`'s instances below are ordinary functions and stay in source, because neither of
   those arguments is about them.
-}
pub class Length(c):
  fn length(self: c) -> Size

{-
   `Chunked` and `Contiguous` - Implementation-Containers.md §5.

   Contiguity is the one-chunk case of a more general property, and both are wanted. `Chunked` is
   what a function that only *reads elements* should ask for: it accepts every container, and after
   specialization there is no dispatch, because the push-style iterator lowering means a contiguous
   container yields one slice and fuses into the loop it compiles to today while a chunked one
   becomes a nested loop whose inner loop runs at full contiguous speed. `Contiguous` is a promise
   about a buffer address - one load to index, passable to a syscall, `memcpy`-able - which is what
   `[a]` means, and the conversion that lets a container of your own be passed as one lives in
   `convert`.

   The dependency is what makes either usable: the container decides the element type, so `for x in
   chunks(xs)` and `fn (Chunked(c, a)) count(xs: c)` both bind `c` from what the caller wrote and
   read `a` back off the instance. `Widen`'s entry in classes.md says why that is declared rather
   than inferred.

   `chunks` hands over a *view* - `&[a]` in any module that can write it, spelled `Flat(a)` here
   because `Array` is this module's own declaration and `[a]` does not exist until it has been read.
   What a chunk is is a window into the container's storage, so a mutable one writes through and
   neither ever copies. The superclass is what stops a
   container from supplying half of this - a `Contiguous` instance with no `Chunked` one is rejected
   where it is declared, and its `Chunked` instance is the one line below.

   There is deliberately no implicit flattening: satisfying `Contiguous` from chunks costs an O(n)
   allocation, and hiding one behind an argument convention is the performance surprise Design.md
   rules out. A `flatten` returning an owner is fine; a coercion is not, which is why a container
   that is `Chunked` and not `Contiguous` is rejected where `[a]` is expected with a diagnostic that
   names `Chunked` as what to ask for instead.
-}
pub class Chunked(c -> a):
  iter fn chunks(self: c) -> Flat(a)

pub class (Chunked(c, a)) Contiguous(c -> a):
  fn elements(return self: c) -> Flat(a)

{-
   The two instances the library itself supplies, which is §5's "the standard library adopts it in
   the same change": every container that exists is one of these, so a signature written against
   `Chunked` accepts everything a `[a]` one does.

   `elements` on the owner goes through a *call* rather than building the descriptor by hand, and the
   reason is ownership rather than brevity: the conversion at that call is what records the result as
   a view of `self` (Local::viewOf), which is what makes the returned borrow rooted in the argument
   the signature marked `return`. A hand-built `Flat` would be rooted in nothing and would type-check
   for the wrong reason.

   Which call it is is `unclampedSlice` rather than `slice`, and that is a size decision - see the
   note there. The two are the same conversion at the same argument position, so the rooting is
   unchanged; what goes is three clamps that a whole container can never trip.

   On the slice itself `elements` is the identity, because a `Flat(a)` already is the descriptor.
-}
@platform(native) instance Contiguous(Array(a), a):
    fn elements(return self: Array(a)) -> Flat(a) = unclampedSlice(self, 0, self.length :: Size)

@platform(js) instance Contiguous(Array(a), a):
    fn elements(return self: Array(a)) -> Flat(a) = unclampedSlice(self, 0, self.length :: Size)

instance Chunked(Array(a), a):
    iter fn chunks(self: Array(a)) -> Flat(a) = yield elements(self)

instance Contiguous(Flat(a), a):
    fn elements(return self: Flat(a)) -> Flat(a) = self

instance Chunked(Flat(a), a):
    iter fn chunks(self: Flat(a)) -> Flat(a) = yield elements(self)

{-
   Reading a whole vector out of a chunk, and writing one back - Implementation-Vector.md §12's "a
   way to read a vector out of a `Flat(a)`", which is what the iteration protocol below is built on.

   Core's portable set deliberately has no load in it: `splat`, `lane` and the rearrangements are
   operations on a value, and where a value comes *from* is a question about storage. So these three
   are the crossing, and they are here rather than in `Native` because the type they take is what
   makes them safe: a `Flat(a)` is a window into storage the language allocated, and §5.2 of
   Design-Vector is what says such storage is padded. A raw address promises nothing, which is why
   `Native.vectorPast` takes one and this does not.

   Two implementations and one signature, which is the split every container operation here already
   has. Natively a vector transfer is one instruction at an address; on JS it is `lanes` element
   accesses, because a vector on that target *is* `lanes` values (Design-Vector §7.2).

   `loadVectorTail` is the one place the two targets differ in what they *mean* rather than in how
   they spell it. Natively it overreads - it loads a whole vector from `at`, reading up to a vector's
   width past the end of the object, which §8's guarantee says is safe and which is the whole reason
   that guarantee exists. On JS there is no such guarantee to have and a read past the end of a host
   array is `undefined` rather than an unspecified byte, so the lanes past the end repeat the last
   element instead. Both are values the caller is about to mask off; neither is a number that could
   poison an accumulator before it does.
-}
@platform(native) pub fn loadVector(from: Flat(a), at: Size) -> Vec(a) = vectorAt(from.items + at)
@platform(js) pub fn loadVector(from: Flat(a), at: Size) -> Vec(a) = hostVector(from.items, from.offset + at)

@platform(native) pub fn loadVectorTail(from: Flat(a), at: Size) -> Vec(a) = vectorPast(from.items + at)
@platform(js) pub fn loadVectorTail(from: Flat(a), at: Size) -> Vec(a) =
    hostVectorUpTo(from.items, from.offset + at, from.offset + from.length - 1)

@platform(native) pub fn storeVector(to: Flat(a), at: Size, value: Vec(a)) -> {} = setVectorAt(to.items + at, value)
@platform(js) pub fn storeVector(to: Flat(a), at: Size, value: Vec(a)) -> {} =
    hostSetVector(to.items, to.offset + at, value)

{-
   The iteration protocol - Design-Vector §4.1, Implementation-Vector.md §9 item 5.

   This is what the whole vector design is for: a loop written over `vectors(xs)` is one loop, it
   reads every element of the container exactly once, and the last iteration over each chunk is the
   same body as the others rather than a scalar epilogue somebody had to remember to write.

   Over `Chunked` and not `Contiguous`, per Design-Vector §4.1: a contiguous container yields one
   chunk and the outer loop folds away, so the general formulation costs the common case nothing,
   while a deque or a rope reaches full speed inside each of its runs.

   **`vectors` forces the identity into the lanes past the end.** It is not `vectorsMasked` with the
   mask thrown away, and this is a ruling rather than a convenience - §4.4 of the design says a body
   handles the padding lanes "by selecting the identity into the masked-off lanes before
   accumulating", which the *body* cannot do, because it is the mask that says which lanes those are
   and this is the iterator that does not hand one over. Zero is the identity for `+`, `-`, `|` and
   `^`; a body accumulating with `*`, `min` or `max` writes `vectorsMasked` and forces its own. The
   cost is one `select` per chunk tail and nothing in the full-width iterations, where the mask is
   all-set and the select folds to its own operand.

   The live count travels as an `Int` rather than as the `Mask(a)` it stands for, which is a choice
   and not a limitation - a mask may be held in memory since §9.9. One integer against sixteen to
   sixty-four bytes, `maskUpTo(live)` folds to all-set on every full-width iteration, and §4.4 wants
   the tail mask read out of a table indexed by exactly this number.

   **The chunk index is a `Size` and the test is against `n - width`, and those two lines are worth
   more than everything else in this iterator put together** - §58.5 of test/bench/findings.md.
   Written the obvious way, `let &i = 0 :: Int` with `while i + width <= n`, this loop costs two
   instructions that have nothing to do with the work:

     - `i` is an `Int`, so every `loadVector(chunk, i :: Size)` sign-extends it into the address and
       the loop carries a `movslq` it did not ask for. `widenInductionVariables` exists for exactly
       this and cannot take it: its no-wrap proofs want a strict test with a step of one, and a chunk
       walk has an inclusive test with a step of four, eight or sixteen.
     - `i + width <= n` recomputes the *next* index to test it, so the loop carries a second `add`
       (emitted as a `lea`) beside the one that steps. Hoisting `n - width` out makes the test read
       the index the step already produced.

   Measured on `maximum` over `[Int]` at v2, with each loop transcribed out of the linked binaries
   into `test/bench/chunkloop.s`: 1.66x against `llc -Os` as emitted, 1.39x with the `lea` gone,
   1.34x with the index widened, and **0.96x with both**. Neither alone is worth much and together
   they are worth all of it - which is the reason this reads the way it does rather than the way it
   would be written.

   `Size` is `I64` natively and `Int` on JS, so `n - width` is a signed subtraction on both and goes
   negative rather than wrapping when a chunk is shorter than one vector - which is the case the test
   has to reject, and does. It is the same guard `i + width <= n` made at `i = 0`.

   Native only for now, and the reason is not vectors: an `iter fn` that yields from inside a `for`
   captures the continuation it was handed, and a reference to a function value the JS backend
   flattened into two variables has no object to name. Every adaptor in the language has that gap.
-}
pub iter fn (Chunked(c, a)) vectorsMasked(self: c) -> {Vec(a), Int}:
    let width = lanes(zero() :: Vec(a)) :: Size

    for chunk in chunks(self):
        let n = length(chunk)
        let last = n - width
        let &i = 0 :: Size

        while i <= last:
            yield {loadVector(chunk, i), truncate(width) :: Int}
            i = i + width

        if i < n:
            yield {loadVectorTail(chunk, i), truncate(n - i) :: Int}

pub iter fn (Chunked(c, a)) vectors(self: c) -> Vec(a):
    let empty = zero() :: Vec(a)
    let width = lanes(empty) :: Size

    for chunk in chunks(self):
        let n = length(chunk)
        let last = n - width
        let &i = 0 :: Size

        while i <= last:
            yield loadVector(chunk, i)
            i = i + width

        if i < n:
            let live = maskUpTo(truncate(n - i) :: Int) :: Mask(a)
            yield select(live, loadVectorTail(chunk, i), empty)

{-
   The bulk operations - Implementation-Vector.md §9 items 6 and 7, Design-Vector §4.3.

   This is where "idiomatic code is fast code" is delivered. `sum(xs)` over a container of a lane
   type is a vector loop; over a container of anything else it is the loop it always was; and the
   program that wrote it said neither. Every one of them is over `Chunked`, so every container in the
   language has them, and none of them mentions a vector in its signature.

   **Each is one declaration with no body and two implementations.** The declaration is the
   intrinsic: `expandBulk` in the compiler picks the pair by asking whether the element has a vector
   *on this target*, which is a question about the lane's stride and the target's register width and
   so is not a question a constraint could carry. The two implementations below are ordinary source
   and are the whole of what runs.

   The vector halves are written over `vectors` and `vectorsMasked` rather than over `loadVector`,
   which is what makes them four lines each: the protocol already walks the chunks, reads the tail
   under a mask and folds to one loop for a contiguous container. Which of the two an operation takes
   is decided by its identity - `+` and the counting ones take `vectors`, because the lanes past the
   end contribute zero; `*`, `min` and `max` take `vectorsMasked` and force their own, because zero
   is not the identity of any of them.

   `maximum` and `minimum` take the answer for an empty container as an argument rather than
   answering a `Maybe`. It is what a fold over a container of borrows can promise without copying an
   element out of one, and it is what the vector half wants anyway: the seed is what the accumulator
   is splatted from and what the masked-off lanes hold.
-}
pub fn (Chunked(c, a), Num(a)) sum(xs: c) -> a
pub fn (Chunked(c, a), Num(a)) product(xs: c) -> a
pub fn (Chunked(c, a), Ord(a), TrivialCopy(a)) maximum(xs: c, ifEmpty: a) -> a
pub fn (Chunked(c, a), Ord(a), TrivialCopy(a)) minimum(xs: c, ifEmpty: a) -> a
pub fn (Chunked(c, a), Eq(a)) occurrences(xs: c, wanted: a) -> Int
pub fn (Chunked(c, a), Eq(a)) indexOf(xs: c, wanted: a) -> Maybe(Size)

-- Reached through `indexOf`, so it is one function rather than a seventh pair. A search that stops
-- at the first hit is what both halves of that one already are.
pub fn (Chunked(c, a), Eq(a)) contains(xs: c, wanted: a) -> Bool = indexOf(xs, wanted) is Just(_)

fn (Chunked(c, a), Num(a), Num(Vec(a))) sumVectors(xs: c) -> a:
    let &acc = zero() :: Vec(a)

    for v in vectors(xs):
        acc = acc + v

    return horizontalSum(acc)

fn (Chunked(c, a), Num(a)) sumElements(xs: c) -> a:
    let &acc = 0 :: a

    for chunk in chunks(xs):
        let n = length(chunk)
        let &i = 0 :: Size

        while i < n:
            acc = acc + chunk[i]
            i = i + 1

    return acc

fn (Chunked(c, a), Num(a), Num(Vec(a))) productVectors(xs: c) -> a:
    let ones = splat(1 :: a)
    let &acc = ones

    for {v, live} in vectorsMasked(xs):
        acc = acc * select(maskUpTo(live) :: Mask(a), v, ones)

    return horizontalProduct(acc)

fn (Chunked(c, a), Num(a)) productElements(xs: c) -> a:
    let &acc = 1 :: a

    for chunk in chunks(xs):
        let n = length(chunk)
        let &i = 0 :: Size

        while i < n:
            acc = acc * chunk[i]
            i = i + 1

    return acc

fn (Chunked(c, a), Ord(a), TrivialCopy(a)) maximumVectors(xs: c, ifEmpty: a) -> a:
    let &acc = splat(ifEmpty)

    for {v, live} in vectorsMasked(xs):
        acc = max(acc, select(maskUpTo(live) :: Mask(a), v, acc))

    return horizontalMax(acc)

fn (Chunked(c, a), Ord(a), TrivialCopy(a)) maximumElements(xs: c, ifEmpty: a) -> a:
    let &acc = ifEmpty

    for chunk in chunks(xs):
        let n = length(chunk)
        let &i = 0 :: Size

        while i < n:
            if chunk[i] > acc then acc = chunk[i]
            i = i + 1

    return acc

fn (Chunked(c, a), Ord(a), TrivialCopy(a)) minimumVectors(xs: c, ifEmpty: a) -> a:
    let &acc = splat(ifEmpty)

    for {v, live} in vectorsMasked(xs):
        acc = min(acc, select(maskUpTo(live) :: Mask(a), v, acc))

    return horizontalMin(acc)

fn (Chunked(c, a), Ord(a), TrivialCopy(a)) minimumElements(xs: c, ifEmpty: a) -> a:
    let &acc = ifEmpty

    for chunk in chunks(xs):
        let n = length(chunk)
        let &i = 0 :: Size

        while i < n:
            if chunk[i] < acc then acc = chunk[i]
            i = i + 1

    return acc

{-
   Counting, where the mask is the answer rather than something applied to one: `count` on a mask is
   how many lanes hold, and the lanes past the end of a chunk hold nothing because `vectors` has
   already selected the identity into them - and the identity of a comparison against `wanted` is
   whatever `wanted` is not. That is not something this can rely on, so the masked form is what it
   takes and the live mask is what it counts against.
-}
fn (Chunked(c, a), Eq(a), Logic(Mask(a))) occurrencesVectors(xs: c, wanted: a) -> Int:
    let sought = splat(wanted)
    let &total = 0

    for {v, live} in vectorsMasked(xs):
        total = total + count(and(v .== sought, maskUpTo(live) :: Mask(a)))

    return total

fn (Chunked(c, a), Eq(a)) occurrencesElements(xs: c, wanted: a) -> Int:
    let &total = 0

    for chunk in chunks(xs):
        let n = length(chunk)
        let &i = 0 :: Size

        while i < n:
            if chunk[i] == wanted then total = total + 1
            i = i + 1

    return total

{-
   The search, and the one operation here that leaves its loop early.

   `firstSet` is what makes the vector half a search rather than a scan: it answers the lowest set
   lane of a mask in three instructions and with no branch, so the iteration that found the element
   is also the one that says where in it - and the running count of live lanes is the position of the
   vector, which is exactly what `vectorsMasked` hands over beside it.
-}
fn (Chunked(c, a), Eq(a), Logic(Mask(a))) indexOfVectors(xs: c, wanted: a) -> Maybe(Size):
    let sought = splat(wanted)
    let &at = 0

    for {v, live} in vectorsMasked(xs):
        let hits = and(v .== sought, maskUpTo(live) :: Mask(a))
        if any(hits) then return Just((at + firstSet(hits)) :: Size)
        at = at + live

    return Nothing

fn (Chunked(c, a), Eq(a)) indexOfElements(xs: c, wanted: a) -> Maybe(Size):
    let &at = 0 :: Size

    for chunk in chunks(xs):
        let n = length(chunk)
        let &i = 0 :: Size

        while i < n:
            if chunk[i] == wanted then return Just(at + i)
            i = i + 1

        at = at + n

    return Nothing

{-
   Subscripting the owner - Core's `Index`.

   A second instance rather than the conversion alone, and the reason is generic code. `xs[i]`
   written against a concrete `Array(a)` could go on reaching `Index(Flat(a), Size, a)` through
   convertSlice, exactly as it always did; `fn (Index(c, k, v)) first(xs: c)` cannot, because an
   instance is selected by the type `c` *binds to* and a caller passing an array binds `Array(a)`.
   Instance selection does not convert - nothing about a class says which of its parameters may be
   widened on the way in - so an owner that is to be a `c` has to be one.

   **Generated rather than written, on the same terms the slice's are - see the note in `Native`.**
   Written out, the owner's pair and the two `Length` instances above would read:

       @platform(native) instance Index(Array(a), Size, a):
           fn get(return self: Array(a), index: Size) -> &a = borrow(self.run.items + index)
           fn getMut(return &self: Array(a), index: Size) -> &a = borrowMut(self.run.items + index)

       @platform(js) instance Index(Array(a), Size, a):
           fn get(return self: Array(a), index: Size) -> &a = hostAt(self.items, index)
           fn getMut(return &self: Array(a), index: Size) -> &a = hostAtMut(self.items, index)

   The bodies are the slice's, minus the descriptor. `self.run.items` is the base the slice would
   have copied, so this is one address computation rather than two loads and a temporary; the
   conversion was never buying anything at a subscript, and its absence is why `Array.yana`'s lowered
   form got shorter rather than longer when the class landed.

   On JS `hostAt` is a place rather than an operation, which is why one line is the whole of it:
   `arr[i]` is an lvalue in the host language exactly as `*(p + i)` is one here, so the borrow it
   hands back writes through to the array's own storage and no copy, box or write-back stands between
   them. That is what makes `xs[i] = v` on JS the same program it is natively rather than a read
   followed by a store somebody had to remember to emit.

   The count is not consulted, which is the same tier the slice's accessor is on and goes the same
   way when `checkBounds` (Implementation-Containers.md §15) lands - in one place, since by then
   there is one place.
-}

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
@platform(native) pub fn slice(return self: Flat(a), from: Size, to: Size) -> Flat(a):
    let &start = from :: Size
    if start < 0 then start = 0
    if start > self.length then start = self.length

    let &end = to :: Size
    if end < start then end = start
    if end > self.length then end = self.length

    return Flat {items: self.items + start, length: end - start}

-- The same clamping, and the window moves rather than the base. `items` is the whole host array in
-- every slice of it, which is why `values` has no JS body: what a caller may read is `offset` up to
-- `offset + length` and nothing outside it.
@platform(js) pub fn slice(return self: Flat(a), from: Size, to: Size) -> Flat(a):
    let &start = from :: Size
    if start < 0 then start = 0
    if start > self.length then start = self.length

    let &end = to :: Size
    if end < start then end = start
    if end > self.length then end = self.length

    return Flat {items: self.items, length: end - start, offset: self.offset + start}

{-
   The same descriptor, for a window whose bounds are already known good.

   `slice` clamps both ends because a caller may write any two numbers, and every one of those clamps
   is a branch and a merge. A *whole* container is the case where none of them can fire - `from` is
   zero and `count` is the length the descriptor was read from - and the clamped form cannot be folded
   down to that: `start` and `end` are whole scalar locals, which opt_promote.cpp declines to promote
   by the rule in its header, so nothing at the resolve tier ever sees the two constants meet. The
   callee stayed seven blocks, the inliner sized it at seven blocks, and `elements` was a real call
   with a real frame in every vector loop in the language.

   So this is `slice` with the clamping taken out and the obligation moved to the caller, which is
   the only reason it is not `pub`: the two call sites below are the whole of it, and both pass the
   container's own length. `return self` for the same reason `slice` has it - the result points into
   whatever `self` points into.

   `count` rather than `to`, because a caller who has already established its bounds knows the length
   directly and subtracting a zero back off is the thing this exists to avoid.
-}
@platform(native) fn unclampedSlice(return self: Flat(a), from: Size, count: Size) -> Flat(a) =
    Flat {items: self.items + from, length: count}

@platform(js) fn unclampedSlice(return self: Flat(a), from: Size, count: Size) -> Flat(a) =
    Flat {items: self.items, length: count, offset: self.offset + from}

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
@platform(native) pub fn remove(&self: Array(a), index: Int) -> Maybe(a):
    -- One comparison and not two. `index` is *reinterpreted* unsigned rather than converted, so a
    -- negative one arrives as a number above every length there is and fails the same test the
    -- too-large case fails - which is the whole reason `Count` is unsigned, and the shape
    -- `checkBounds` (§15) will have. `bitcast` and not `truncate`: the two are the same bits at this
    -- width, and only one of them says that reading them differently is the point.
    --
    -- The length needs no ascription of its own: a `@bits` refinement dispatches as the type it
    -- refines, so this is the ordinary `U32` comparison.
    if (bitcast(index) :: U32) >= self.length then return Nothing

    let ->doomed = *(self.run.items + index)

    let rest = (self.length :: Size) - index - 1
    if rest > 0:
        copyMemory(bitcast(self.run.items + index) :: %U8, bitcast(self.run.items + index + 1) :: %U8,
                   byteSpan(self.run.items, rest))

    self.length = truncate((self.length :: Int) - 1) :: Count
    return Just(doomed)

{-
   And on JS, where closing the gap is a `copyWithin` rather than a block move.

   The element still comes out *before* the gap closes, and for the same reason the native body gives:
   a move over a live value is bytes moving over one, which owes its own bookkeeping. So `doomed`
   is where it goes and a `->` binding owns what it holds, exactly as above - and the answer is a
   `Maybe(a)` on both targets because §13.3 is about who gets the element rather than about how the
   container is stored.

   **One body for both of §14's rows**, which is what `@host` on the count bought. `copyWithin` moves
   and does not shorten, so it leaves the last slot holding a duplicate of an element past the count;
   on the row whose elements are host objects that is a reference nothing would ever release. What
   releases it is the line after it. `self.length` on that row *is* `arr.length`, assigning it
   truncates, and truncating is what drops the duplicate - so recording the new count and freeing the
   slot are the same statement rather than two operations that have to agree.

   It was written as a `hostFixedCapacity` branch with `.splice` on one side, which is what that row
   needs when the count is stored somewhere else. Nothing needs it now: `.splice` shortens *and*
   renumbers, and the only thing this body wanted from it was the shortening.
-}
@platform(js) pub fn remove(&self: Array(a), index: Int) -> Maybe(a):
    let count = self.length :: Size
    if (bitcast(index) :: U32) >= (bitcast(count) :: U32) then return Nothing

    let ->doomed = hostRead(self.items, index :: Size)

    hostCopyWithin(self.items, index :: Size, (index :: Size) + 1, count)
    self.length = truncate(count - 1) :: Count

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
@platform(native) instance Reclaim(Array(a)):
    fn reclaim(->value: Array(a)) -> {}:
        let count = value.length :: Int
        let &i = 0 :: Int

        while i < count:
            let ->doomed = *(value.run.items + i)
            i = i + 1

        releaseRun(value.run)

{-
   The same traversal, and no release - Implementation-Containers.md §14's "release: nothing".

   Which is the whole of what a collector target changes, and it is worth saying that it changes
   *only* that: the elements still have to be walked, because whether an element's lifetime ends in
   something is a question about the element type and not about who owns the bytes. An
   `Array(Connection)` closes its connections here on both targets; an `Array(Int)` has nothing to run
   at each element, so the same body folds to nothing on both.
-}
@platform(js) instance Reclaim(Array(a)):
    fn reclaim(->value: Array(a)) -> {}:
        let count = value.length :: Int
        let &i = 0 :: Int

        while i < count:
            let ->doomed = hostRead(value.items, i :: Size)
            i = i + 1

{-
   ============================================================================
   Hashing - Implementation-Map.md §3 and §3.1.

   `Core` declares the class and the finalizer; what lives here is the *fold*, which is the half
   that has a target in it. Two folds and one seed, and every instance below is one line of one of
   them.
   ============================================================================
-}

{-
   The seed - Implementation-Map.md §3's last paragraph.

   The initial state of every fold, and most of what stops a map keyed by attacker-chosen strings
   from being a denial of service. It costs nothing: it is the initial state of a fold that is
   already happening.

   *Most*, not all. A seed only helps a hash whose output *difference* depends on it, which is
   exactly the property `crc32` lacks and §3.1 rejects it for - against a nonlinear fold the seed
   forces an attacker to search, against a linear one it cancels out of a collision set entirely.
   The fold below is the nonlinear one, which is what makes this worth having.

   **Per process and not per map.** Two maps of one type have to agree about a key's hash, or a
   rebuild could not move an entry without re-hashing it.

   A constant today, and the open question Implementation-Map.md §10 leaves is *not* whether it
   should be one - it should not - but what a debug build should do: fix it so a failure reproduces,
   or deliberately vary it to shake out programs that came to depend on an iteration order they were
   never promised. Randomizing it wants an entropy source at startup, which is Native.Linux's to
   supply and is not wired up here. What the language already guarantees is that nothing observable
   moves when it changes: iteration is insertion order, never hash order.
-}
pub let hashSeed = 2166136261 :: U32

{-
   Folding thirty-two bits of key into the state - the operation every instance below is written in
   terms of, and the one that differs between the targets.

   **Native: the 64x64->128 multiply-fold**, which is wyhash's and xxh3's core. The state and the
   word go into the two halves of one sixty-four-bit input, each half is xor'd with a constant, and
   the product's two halves are folded together - `lo ^ hi`, which is what makes it nonlinear and
   what a CRC is not. Implementation-Map.md §3.1 measures both: `crc32` is twice as fast and is
   disqualified, because the difference of two CRCs does not depend on the seed and a colliding key
   set can therefore be *solved for* once and reused against every process.

   One `mul`, one `mulhi`, and the folds around them. `mulHigh` is the only primitive the whole map
   asked the language for.
-}
@platform(native) pub fn hashWord(state: U32, word: U32) -> U32 =
    foldWide(((word :: U64) `shl` 32) `or` (state :: U64))

{-
   And the same fold on JS - Implementation-Map.md §3.1's second row, which is FNV-1a's step.

   Not the multiply-fold, and the reason is measured rather than assumed: its intermediates leave
   the Smi range, and `../../benchmark/map-js` §9 has the same cell measuring either 1.10 ns or 7.9
   ns depending on which way V8's type feedback fell. **A hash written for JS stays inside
   `Math.imul` and int32**, which this does - a 32-bit multiply is emitted as `Math.imul` and
   nothing here ever holds a value the engine cannot keep in a small integer.

   **The two targets are allowed to compute different functions**, and nothing had to be added to
   let them: no observable depends on a hash value, because the iteration order is insertion order
   and the seed is per process. Making JS compute the native function would cost 1.19x on a `U32`
   key, 1.50x on a string and 1.88x on a record, and buy nothing.
-}
@platform(js) pub fn hashWord(state: U32, word: U32) -> U32 = (state `xor` word) * 16777619

{-
   The multiply-fold itself, and the only thing in this file that needs a machine.

   `mum(a, b) = lo(a*b) ^ hi(a*b)` folded over one sixty-four-bit input and finished into
   thirty-two, which is what the class's `U32` state means (§3). The two constants are odd and
   well-mixed and nothing else about them matters; they are below 2^63 so that each is a literal
   this language reads.
-}
@platform(native) fn foldWide(value: U64) -> U32:
    let lhs = value `xor` 2685821657736338717
    let rhs = 6364136223846793005 :: U64
    let mixed = (lhs * rhs) `xor` mulHigh(lhs, rhs)

    return truncate(mixed `xor` (mixed `shr` 32)) :: U32

{-
   Folding a whole sixty-four-bit key.

   Natively this is one multiply-fold and the reason `foldWide` takes the width it does: a `Long`
   key costs exactly what a `U32` key costs. On JS a sixty-four-bit value is a `bigint`, which is
   off the arithmetic path entirely, so the two halves are folded separately at thirty-two bits -
   two `Math.imul`s and no `bigint` arithmetic beyond the shift that splits it.
-}
@platform(native) pub fn hashWide(state: U32, word: U64) -> U32 =
    foldWide(word `xor` ((state :: U64) `shl` 27))

@platform(js) pub fn hashWide(state: U32, word: U64) -> U32 =
    hashWord(hashWord(state, truncate(word) :: U32), truncate(word `shr` 32) :: U32)

{-
   The primitive instances - Implementation-Map.md §3's "a record's instance folds field by field".

   These are the leaves that fold ends at. Each is one call to one of the two folds above, and the
   conversion in front of it is what makes the key a machine word: a signed type reaches `U32` by
   `truncate`, which is the ordinary narrowing rung and not a reinterpretation of anything, and is
   injective over the source type's own range - which is the whole of what a hash needs from it.

   `Eq` is the superclass and the obligation these have to keep: two values `==` calls equal fold to
   the same state. Every one of them does so by construction, since the conversion in front of the
   fold is injective.
-}
instance Hash(U8):
    fn hash(value: U8, state: U32) -> U32 = hashWord(state, value :: U32)

instance Hash(U16):
    fn hash(value: U16, state: U32) -> U32 = hashWord(state, value :: U32)

instance Hash(U32):
    fn hash(value: U32, state: U32) -> U32 = hashWord(state, value)

instance Hash(I8):
    fn hash(value: I8, state: U32) -> U32 = hashWord(state, truncate(value) :: U32)

instance Hash(I16):
    fn hash(value: I16, state: U32) -> U32 = hashWord(state, truncate(value) :: U32)

instance Hash(I32):
    fn hash(value: I32, state: U32) -> U32 = hashWord(state, truncate(value) :: U32)

instance Hash(Int):
    fn hash(value: Int, state: U32) -> U32 = hashWord(state, truncate(value) :: U32)

instance Hash(U64):
    fn hash(value: U64, state: U32) -> U32 = hashWide(state, value)

instance Hash(I64):
    fn hash(value: I64, state: U32) -> U32 = hashWide(state, truncate(value) :: U64)

instance Hash(Long):
    fn hash(value: Long, state: U32) -> U32 = hashWide(state, truncate(value) :: U64)

{-
   `WideInt` is the one type whose instance is split by target, and it is split for the reason §3.1
   gives about JS arithmetic rather than for anything about the type: it is a plain `number` there,
   so converting it to a `U64` to reach `hashWide` would build the `bigint` that type exists to
   avoid. Fifty-three bits, folded as two words that are each an ordinary small integer.
-}
@platform(native) instance Hash(WideInt):
    fn hash(value: WideInt, state: U32) -> U32 = hashWide(state, truncate(value) :: U64)

@platform(js) instance Hash(WideInt):
    fn hash(value: WideInt, state: U32) -> U32 =
        hashWord(hashWord(state, truncate(value) :: U32), truncate(value `shr` 32) :: U32)

{-
   `Bool` folds as the number it is. One instruction on both targets, and it exists so that a record
   with a flag in it can derive a hash without the flag being a special case.
-}
instance Hash(Bool):
    fn hash(value: Bool, state: U32) -> U32 = hashWord(state, if value then 1 else 0)

{-
   The floats, and the one thing that is not a bit pattern.

   `0.0` and `-0.0` are two patterns that `==` calls equal, so hashing the pattern would break the
   superclass obligation for exactly one pair of values - and it is the pair a program is most
   likely to have. So a zero of either sign folds as the same word and everything else folds as its
   bits. `NaN` needs no such care in the other direction: `==` says a NaN is equal to nothing at all,
   including itself, so a map can never find one and what it hashed to was never asked.
-}
instance Hash(Float):
    fn hash(value: Float, state: U32) -> U32 =
        if value == 0.0 then hashWord(state, 0) else hashWord(state, bitcast(value) :: U32)

{-
   And `Double` natively only, which is a gap rather than a decision of this section's.

   `defineBitcastLadder` declines every 64-bit rung that crosses a float on JS, deliberately and for
   a stated reason: the integer on the far side is a `bigint` there, so the trip is a heap value and
   a `DataView` rather than a reinterpretation. A double's bits are therefore not reachable from
   source on that target at all, and a hash that folded something *other* than the bits - a
   truncation, a scaled residue - would be a quality loss nobody asked for and nothing would report.

   So `Map(Double, v)` compiles natively and does not on JS, which is a diagnostic at the call rather
   than a wrong answer anywhere. What closes it is a `Host` primitive handing back the two halves of
   a double as `U32`s, which is the scratch typed-array pair codegen/js/inst.cpp already emits for
   the 32-bit rungs, read twice instead of once. `Float` needs none of this and has its instance on
   both targets above.
-}
@platform(native) instance Hash(Double):
    fn hash(value: Double, state: U32) -> U32 =
        if value == 0.0 then hashWide(state, 0) else hashWide(state, bitcast(value) :: U64)

{-
   ============================================================================
   `Map(k, v)` - Implementation-Map.md.

   A dense run of entries in insertion order, plus a private index over it. The two corpora in
   `benchmark/map-native` and `benchmark/map-js` are what chose this shape and are the regression
   suites for it; §7 of the native findings is why there is **one** map type rather than a probing
   one for lookup and a dense one for iteration.

   The build order is that document's §8, and what is here is steps 1 to 4 and 6: the scan, `Hash`,
   the control-byte index with its probe, `bits`, and the subscript. **Step 5 - a predicate over a
   type argument on `@platform`, which is the one language extension the design asks for - is not
   built**, so the cached hash beside a structural entry is not here and JS gets a ported table for
   every key type rather than the host `Map` for the ones it could hold. Both are named where they
   would go.

   **The entries run is the same declaration on both targets and every operation over it is one
   body.** What differs is the index and nothing else: `Run(U8)` of control bytes natively, an
   `Array(I32)` of entry numbers on JS, behind six functions named `index...` below. That is a
   departure from §6, which splits the JS entries into parallel typed runs - and it is taken for a
   stated reason: iteration hands over `&Entry(k, v)`, so parallel runs would give `pairs` a
   different signature per target, and a `{&k, &v}` pair is a shape nothing else in the language
   builds. What it costs is §6.2's typed-array representation on JS, which is real and is written
   down in that section rather than argued with here.
   ============================================================================
-}

{-
   One entry: the key and the value, adjacent.

   One run rather than two - splitting it into a key run and a value run wins only where the two
   alignments differ, and costs an allocation (native findings §4). `pub` because it is what
   iteration hands over: a borrow of an entry is how a program reads a pair without either half
   being copied.
-}
pub data Entry(k, v) {key: k, value: v}

{-
   The three control-byte values - Implementation-Map.md §1.

   The encoding is chosen so that **the sign bit alone separates full from not-full**: a slot's tag
   is seven bits, so a full slot is 0..127 and both of the others are at or above 128. A group's
   free lanes are therefore one unsigned comparison against `ctrlEmpty` and no table, which is what
   `indexFreeSlot` below is written in terms of.
-}
pub let ctrlEmpty = 128 :: U8
pub let ctrlDeleted = 254 :: U8

-- The two JS index sentinels, which are the same two facts written as negative entry numbers: an
-- `Int32Array` has no spare bit pattern the way a control byte has a spare bit.
@platform(js) pub let slotEmpty = -1 :: I32
@platform(js) pub let slotDeleted = -2 :: I32

{-
   How many entries a map scans before an index is worth allocating - Implementation-Map.md §2 and
   §4.4, and the third of that section's three per-key-type rules.

   **A constant that folds**, which is the shape §2 says this rule can already have: 8 for a machine
   word, 32 for a `String`, 64 for anything structural, decided from the key's Repr at the call site
   and folded there. The other two rules in that table - `hostKeyFor` and `cachedHashFor` - select
   between *declarations* and are what needs the language extension; this one does not.

   Measured, not guessed: over the whole life of a one-entry map, scanning is 3.3x cheaper than an
   indexed map for `[U32: U32]` and 4.7x for `[Record: Record]`, and it stays ahead to about eight
   entries for a machine-word key, thirty-two for a string, and past a hundred for a record key,
   whose hash is a string walk plus three mixes.

   Takes the entries run rather than a key, for the reason `hostFixedCapacity` takes the array it
   asks about: what it needs is the *type*, a map with nothing in it has no key to hand over, and a
   pointer's pointee is exactly the question. The key is the entry's first field. Nothing reads the
   operand, and the same spelling works on both targets because both entries runs are `%Entry(k, v)`.
-}
pub fn scanLimitFor(entries: %Entry(k, v)) -> Size

{-
   And the same question asked of a run of *keys*, which is the shape the JS row has.

   Two names rather than one signature, because the two targets no longer store a key in the same
   place: native reaches it through the entry it is a field of, and JS has a run of nothing else. It
   is one intrinsic underneath - `emitScanLimit` unwraps an `Entry` where it finds one - and the two
   names are what keeps the unwrap unambiguous, since a *record key* is a record the way an entry is
   and asking one function to tell them apart would answer 8 for `Map(Point, Int)`.

   Both are private to `scanLimitOf` below, which is the one signature either target's callers see.
-}
pub fn scanLimitForKeys(keys: %k) -> Size

{-
   The map - Implementation-Map.md §1.

   Two allocations. The entries run is what a program sees: `length` live entries in insertion order
   with no holes, so iteration is a walk with no occupancy test at all. `Array(Entry(k, v))` is
   exactly §1's `entries: Run(Entry(k, v))` and `count: Count` - the same two words, with growth,
   bounds and teardown already written - which is why it is spelled as the container rather than as
   the run plus a count of this type's own.

   The index run is private and holds no value with a lifetime.

   **Natively it is one allocation with three parts:**

       byte 0                     slots        slots+16                       slots+16+4*slots
             | control bytes       | cloned group | entry numbers, one Count each |

   The **cloned group** is what makes an unaligned sixteen-byte load at any slot read sixteen bytes
   that are inside the allocation, so the probe never overreads and never wraps by hand. Every
   control write mirrors into it when the slot is below sixteen, which is one comparison against a
   constant.

   The **entry numbers** are `Count`, four bytes each. Native findings §9.1 measured the CPython
   trick - `u8`/`u16`/`i32` by table size - applied to exactly this run: it saves 3.3 B/entry and
   costs 7-14% of a lookup on machine-word keys, which is the key type where the saving is largest.
   **Do not narrow this run.**

   `mask` is the slot count less one, or **zero when there is no index at all** - which is §4.4's
   branch and the whole of the small-map path. `live` counts the index slots that are not empty,
   full and deleted together, since that is what the load factor is measured against.

   The two capacities are independent. The entries run doubles when it is full; the index doubles
   when `live + 1` would pass 7/8 of the slot count. Tying them would force one of the two into a
   shape it does not want - native findings §9.3 measured both fusions and neither is worth it.
-}
@platform(native) pub data Map(k, v)
    {entries: Array(Entry(k, v)), index: Run(U8), live: Count, mask: Count}

{-
   And the same map on JS, where the index is an `Int32Array` of entry numbers - map-js §5's layout,
   with §6's control-byte index deliberately left native-only: there is no `movemask` on that target
   and a sixteen-lane compare is sixteen loads, so the JS row keeps the plain index and this
   document does not guess.

   **The keys and the values are parallel runs here and one run of entries there**, which is §6's
   layout and the one departure from "every operation but the index is one body". `Array(a)` on this
   target is already a `TypedArray` wherever the element kind allows one
   (Implementation-Containers.md §14), so `Array(k)` and `Array(v)` *are* map-js §5's typed runs with
   nothing further to arrange - where a single `Array(Entry(k, v))` is a host array of one object per
   entry, which is the shape §4.2 and §4.4 between them rule out twice over.

   Measured against the parallel runs at `[U32: U32]` and a hundred thousand entries: iteration
   3.53 ns/entry against 0.37 and **70.40 bytes per entry against 13.25**, both flat in the entry
   count because they are per-object costs. Those are the two margins §6.2 has the ported table
   beating the host `Map` by, so the object row gave both of them back.

   What it costs is the six accessors below, which is where `entries` stops being a field two bodies
   share. Nothing above them changes: the probe, the rebuild, the insert and the removal are still
   one body each.

   One departure from §6 remains and it is step 5's absence rather than a disagreement with it. There
   is **no host-`Map` row**, so a `Map(Int, v)` is this table rather than `new Map()`; and there is
   **no stored hash**, so a rebuild re-hashes every key where §6's row would have read one out of a
   `Uint32Array`. Both need a predicate over a type argument to select between two field sets, which
   is the one thing Containers §14 records that nothing in the language does.
-}
@platform(js) pub data Map(k, v)
    {keys: Array(k), values: Array(v), index: Array(I32), live: Count, mask: Count}

-- A map with room for nothing, which allocates nothing. Neither run is touched until the first
-- insert, and the index not until the scan threshold is crossed.
@platform(native) pub fn emptyMap() -> Map(k, v) =
    Map {entries: emptyArray(), index: emptyRun(), live: 0, mask: 0}

@platform(js) pub fn emptyMap() -> Map(k, v) =
    Map {keys: emptyArray(), values: emptyArray(), index: emptyArray(), live: 0, mask: 0}

{-
   How many entries the map holds, as the class rather than as a plain `count` - and the reason is
   the one `Length`'s own note gives about `String`: this language has no ad-hoc overloading for
   plain functions, so a second `fn count` beside `Core`'s `count(mask)` is a duplicate declaration
   rather than an overload. `Length` is exactly the class that exists for that, and a map is exactly
   the kind of container it was declared over.
-}
instance Length(Map(k, v)):
    fn length(self: Map(k, v)) -> Size = entryCount(self)

-- How many slots the index has, which is one more than the mask. Never asked of a map with no
-- index, where the answer would be a slot count of one that does not exist.
fn indexSlots(self: Map(k, v)) -> Size = (self.mask :: Size) + 1

{-
   Which slot a hash starts its probe at - the low bits, masked.

   Written through `bitcast` and a widening rather than as `(h and mask) :: Size`, because `Size` is
   `I64` natively and `Int` on JS: the direct ascription widens on one target and narrows on the
   other, and there is no third spelling that is both. The mask is at most thirty bits (`Count`), so
   the value always fits the signed type it lands in and the reinterpretation loses nothing - which
   is exactly the case `bitcast` is for.
-}
fn slotFor(h: U32, mask: Count) -> Size = (bitcast(h `and` (mask :: U32)) :: I32) :: Size

-- The seven-bit tag, taken from the *top* of the finalized word - Implementation-Map.md §3. The slot
-- comes from the bottom, which is the opposite of abseil's split and is measured: a byte-at-a-time
-- hash mixes its low bits best, and `h >> 7` throws them away (findings §12.6).
fn tagOf(h: U32) -> U8 = truncate((h `shr` 25) `and` 127) :: U8

-- The finalized hash of a key. `mix32` is what makes a cheap instance merely slow instead of
-- quadratic: without it a weak `hash` would put every key of a table into one tag.
pub fn (Hash(k)) hashKey(key: k) -> U32 = mix32(hash(key, hashSeed))

{-
   ---------------------------------------------------------------------------
   The index, which is the only part of this map that differs between the targets.

   Six functions and one rule each, and everything below this block is written in terms of them:

     `indexFind`     which entry holds this key, or -1
     `indexSlotOf`   which *slot* holds this key, or -1 - what removal needs
     `indexPlace`    point a free slot at an entry
     `indexRebuild`  a fresh index of `slots` slots, with every live entry placed in it
     `indexReset`    every slot empty, the entries left alone - what `clear` needs
     `indexForget`   mark the slot a key occupies as deleted
   ---------------------------------------------------------------------------
-}

{-
   The probe, natively - Implementation-Map.md §4.1, and the whole of lookup.

   Three things in it are load-bearing:

   - **`step` grows by sixteen each time**, so the groups visited are `pos`, `pos+16`, `pos+48`, ... -
     triangular numbers times the group width. Over a power-of-two slot count that visits every group
     exactly once, which is what makes the loop terminate.
   - **The empty test is what ends a miss**, not the probe count: a group with any empty lane cannot
     be followed by a slot holding this key, because an insert would have taken that lane.
   - **`bits` turns the match into an integer**, so the walk over *several* matching lanes is
     `hits and (hits - 1)` - two instructions, against a `firstSet` and a lane-clearing mask
     operation per iteration.

   The bound on the outer loop is the slot count and is never reached: an empty lane exists at every
   load factor this map allows, so the `return` after it is unreachable in a map whose invariants
   hold. It is written rather than assumed because a loop with no exit condition is not something a
   reader should have to prove terminating.

   `indexSlotOf` below is this same walk answering the slot instead of the entry, which removal
   needs and every other caller would be discarding.
-}
@platform(native) fn (Hash(k)) indexFind(self: Map(k, v), key: k, h: U32) -> Size:
    let mask = self.mask :: Size
    let tag = splatGroup(tagOf(h))
    let empty = splatGroup(ctrlEmpty)

    let &pos = slotFor(h, self.mask)
    let &step = 0 :: Size
    let &visited = 0 :: Size

    while visited <= mask:
        let group = loadGroup(self.index.items + pos)
        let &hits = bits(group .== tag)

        while hits != 0:
            let slot = (pos + (trailingZeros(hits) :: Size)) `and` mask
            let at = entryNumber(self, slot)
            if keyEquals(self, at, key) then return at

            hits = hits `and` (hits - 1)

        if bits(group .== empty) != 0 then return 0 - 1

        step = step + 16
        pos = (pos + step) `and` mask
        visited = visited + 16

    return 0 - 1

@platform(native) fn (Hash(k)) indexSlotOf(self: Map(k, v), key: k, h: U32) -> Size:
    let mask = self.mask :: Size
    let tag = splatGroup(tagOf(h))
    let empty = splatGroup(ctrlEmpty)

    let &pos = slotFor(h, self.mask)
    let &step = 0 :: Size
    let &visited = 0 :: Size

    while visited <= mask:
        let group = loadGroup(self.index.items + pos)
        let &hits = bits(group .== tag)

        while hits != 0:
            let slot = (pos + (trailingZeros(hits) :: Size)) `and` mask
            if keyEquals(self, entryNumber(self, slot), key) then return slot

            hits = hits `and` (hits - 1)

        if bits(group .== empty) != 0 then return 0 - 1

        step = step + 16
        pos = (pos + step) `and` mask
        visited = visited + 16

    return 0 - 1

{-
   The first non-full slot of a key's probe sequence - the other half of §4.1.

   A **Deleted** slot counts as free and may be earlier than the Empty one that ends a search, which
   is what keeps a delete-heavy map from growing its probe lengths without bound. The test is one
   unsigned comparison for the reason the control encoding was chosen: full is 0..127 and both of the
   others are at or above `ctrlEmpty`.
-}
@platform(native) fn indexFreeSlot(self: Map(k, v), h: U32) -> Size:
    let mask = self.mask :: Size
    let empty = splatGroup(ctrlEmpty)

    let &pos = slotFor(h, self.mask)
    let &step = 0 :: Size
    let &visited = 0 :: Size

    while visited <= mask:
        let group = loadGroup(self.index.items + pos)
        let free = bits(group .>= empty)
        if free != 0 then return (pos + (trailingZeros(free) :: Size)) `and` mask

        step = step + 16
        pos = (pos + step) `and` mask
        visited = visited + 16

    return 0 - 1

{-
   The two accessors that know §1's byte layout, and the only things in this file that read the index
   run directly.

   `slots` is a power of two and at least sixteen, so `slots + 16` is a multiple of sixteen and the
   entry numbers need no padding to be four-byte aligned.
-}
@platform(native) fn entryNumbers(self: Map(k, v)) -> %Count =
    bitcast(self.index.items + (indexSlots(self) + 16)) :: %Count

@platform(native) fn entryNumber(self: Map(k, v), slot: Size) -> Size =
    (*(entryNumbers(self) + slot)) :: Size

{-
   The control byte for a slot, written into both copies.

   The branch is a comparison against a constant and it is the whole cost of the cloned group. It is
   here rather than at each of the three call sites so that "the clone and the original never
   disagree" is one line rather than an invariant three bodies have to keep.
-}
@platform(native) fn setControl(&self: Map(k, v), slot: Size, value: U8) -> {}:
    store(self.index.items + slot, value)
    if slot < 16 then store(self.index.items + (indexSlots(self) + slot), value)

{-
   Pointing one index slot at one entry.

   `live` counts up only where the slot taken was Empty - a Deleted one was already counted and
   giving it away does not change how full the table is.
-}
@platform(native) fn indexPlace(&self: Map(k, v), at: Size, h: U32) -> {}:
    let slot = indexFreeSlot(self, h)
    checkCondition(slot < 0)

    let taken = *(self.index.items + slot)
    setControl(self, slot, tagOf(h))
    store(entryNumbers(self) + slot, truncate(at) :: Count)

    if taken == ctrlEmpty then self.live = truncate((self.live :: Size) + 1) :: Count

@platform(native) fn (Hash(k)) indexForget(&self: Map(k, v), key: k, h: U32) -> {}:
    let slot = indexSlotOf(self, key, h)
    if slot >= 0 then setControl(self, slot, ctrlDeleted)

{-
   The slot holding entry number `from`, made to hold `to` instead - what a swap-remove needs.

   Found by **entry number and not by key**, which is what lets a removal re-point the moved entry's
   slot *before* the move rather than after it. After it the entry is no longer where the index says,
   so a key comparison would read past the end of the run - undefined on JS and stale memory
   natively, which is the same bug with only one of the two targets willing to say so.

   Entry numbers are unique, so the comparison is exact and no key is touched at all. `h` is the
   moved entry's own hash, so the walk is its own probe sequence and the slot is on it.
-}
@platform(native) fn indexRepoint(&self: Map(k, v), from: Size, to: Size, h: U32) -> {}:
    let mask = self.mask :: Size
    let tag = splatGroup(tagOf(h))

    let &pos = slotFor(h, self.mask)
    let &step = 0 :: Size
    let &visited = 0 :: Size

    while visited <= mask:
        let group = loadGroup(self.index.items + pos)
        let &hits = bits(group .== tag)

        while hits != 0:
            let slot = (pos + (trailingZeros(hits) :: Size)) `and` mask

            if entryNumber(self, slot) == from:
                store(entryNumbers(self) + slot, truncate(to) :: Count)
                return

            hits = hits `and` (hits - 1)

        step = step + 16
        pos = (pos + step) `and` mask
        visited = visited + 16

@platform(native) fn indexReset(&self: Map(k, v)) -> {}:
    setMemory(self.index.items, ctrlEmpty, indexSlots(self) + 16)
    self.live = 0

{-
   The rebuild - Implementation-Map.md §4.3.

   A sequential read of the entries run and a random write of the index, which is where the cached
   hash of §2 would pay: with one, this would touch no key at all. Without it every key is hashed
   again, which is the cost step 5 of §8 is what removes.

   `resize` rather than a fresh run and a release, so the growth path is `Run(a)`'s own and the
   shrink case - a rebuild at the same slot count, which is how tombstones are dropped - reuses the
   allocation it already has. Only the control bytes and their clone are initialized: an entry number
   in a slot whose control byte says Empty is never read.
-}
@platform(native) fn (Hash(k)) indexRebuild(&self: Map(k, v), slots: Size) -> {}:
    -- Held in a local rather than written inside the check, because an argument in `&` position is
    -- resolved as an ordinary expression when the call it is in is itself an argument - see the note
    -- in Reject.Exchange.yana, which is the same ordering seen from the other end.
    let grown = resize(self.index, slots + 16 + slots * 4)
    checkCondition(!grown)

    self.mask = truncate(slots - 1) :: Count
    indexReset(self)

    let count = entryCount(self)
    let &at = 0 :: Size

    while at < count:
        indexPlace(self, at, hashAt(self, at))
        at = at + 1

{-
   And the same six on JS - map-js §5's layout.

   An `Int32Array` of entry numbers with two negative sentinels, probed linearly. There is no group
   here and no tag: a control byte's seven hash bits are what a sixteen-lane compare filters on, and
   with no `movemask` the filter would cost more than the key compare it saves. What ends a miss is
   the same fact it is natively - an empty slot cannot be followed by one holding this key.

   The bound is the slot count for the reason the native probe's is, and is reached for the same
   never: the load factor keeps an empty slot in every table.
-}
@platform(js) fn (Hash(k)) indexFind(self: Map(k, v), key: k, h: U32) -> Size:
    let mask = self.mask :: Size
    let &slot = slotFor(h, self.mask)
    let &visited = 0 :: Size

    while visited <= mask:
        let at = self.index[slot] :: Size
        if at == (slotEmpty :: Size) then return 0 - 1
        if at >= 0 && keyEquals(self, at, key) then return at

        slot = (slot + 1) `and` mask
        visited = visited + 1

    return 0 - 1

@platform(js) fn (Hash(k)) indexSlotOf(self: Map(k, v), key: k, h: U32) -> Size:
    let mask = self.mask :: Size
    let &slot = slotFor(h, self.mask)
    let &visited = 0 :: Size

    while visited <= mask:
        let at = self.index[slot] :: Size
        if at == (slotEmpty :: Size) then return 0 - 1
        if at >= 0 && keyEquals(self, at, key) then return slot

        slot = (slot + 1) `and` mask
        visited = visited + 1

    return 0 - 1

@platform(js) fn indexPlace(&self: Map(k, v), at: Size, h: U32) -> {}:
    let mask = self.mask :: Size
    let &slot = slotFor(h, self.mask)
    let &visited = 0 :: Size

    while visited <= mask:
        let taken = self.index[slot] :: Size
        if taken < 0:
            self.index[slot] = at :: I32
            if taken == (slotEmpty :: Size) then self.live = truncate((self.live :: Size) + 1) :: Count
            return

        slot = (slot + 1) `and` mask
        visited = visited + 1

    checkCondition(True)

@platform(js) fn (Hash(k)) indexForget(&self: Map(k, v), key: k, h: U32) -> {}:
    let slot = indexSlotOf(self, key, h)
    if slot >= 0 then self.index[slot] = slotDeleted

@platform(js) fn indexRepoint(&self: Map(k, v), from: Size, to: Size, h: U32) -> {}:
    let mask = self.mask :: Size
    let &slot = slotFor(h, self.mask)
    let &visited = 0 :: Size

    while visited <= mask:
        if (self.index[slot] :: Size) == from:
            self.index[slot] = to :: I32
            return

        slot = (slot + 1) `and` mask
        visited = visited + 1

@platform(js) fn indexReset(&self: Map(k, v)) -> {}:
    let slots = length(self.index)
    let &slot = 0 :: Size

    while slot < slots:
        self.index[slot] = slotEmpty
        slot = slot + 1

    self.live = 0

{-
   The JS rebuild, which grows the typed array to exactly `slots` and then fills it.

   The length is assigned rather than pushed to, which is what `Array(a)`'s two rows on this target
   make possible: an `Int32Array` has a stored count, so `reserve` sets the capacity and the count is
   this map's to say. Nothing is read out of the old contents, so no copy of them is wanted.
-}
@platform(js) fn (Hash(k)) indexRebuild(&self: Map(k, v), slots: Size) -> {}:
    reserve(self.index, slots)
    self.index.length = truncate(slots) :: Count

    self.mask = truncate(slots - 1) :: Count
    indexReset(self)

    let count = entryCount(self)
    let &at = 0 :: Size

    while at < count:
        indexPlace(self, at, hashAt(self, at))
        at = at + 1

{-
   ---------------------------------------------------------------------------
   Everything below here is one body for both targets.
   ---------------------------------------------------------------------------
-}

{-
   The scan - Implementation-Map.md §4.4, and step 1 of §8 on its own.

   What a map below the threshold *is*: the dense entries run, walked. No index is allocated, no
   hash is computed, and the branch that chooses this costs one comparison on the lookup path -
   which predicts perfectly for the life of a map that never grows past it, and is the cheapest
   thing in the design.
-}
fn (Hash(k)) scanEntries(self: Map(k, v), key: k) -> Size:
    let count = entryCount(self)
    let &at = 0 :: Size

    while at < count:
        if keyEquals(self, at, key) then return at
        at = at + 1

    return 0 - 1

-- Which entry holds this key, or -1. The one branch §4.4 puts on the lookup path, and everything
-- else in this file is a variation on what is below it.
fn (Hash(k)) findEntry(self: Map(k, v), key: k, h: U32) -> Size:
    if self.mask == 0 then return scanEntries(self, key)
    return indexFind(self, key, h)

-- Room for `wanted` slots, rounded up to a power of two and never below sixteen. The load factor is
-- 7/8, so a map that will hold `n` entries wants `n * 8 / 7` slots before it stops growing.
fn indexSlotsFor(wanted: Size) -> Size:
    let &slots = 16 :: Size
    while slots - (slots `shr` 3) < wanted:
        slots = slots + slots

    return slots

{-
   Room for `wanted` entries, in both runs - Implementation-Map.md §4.5.

   Worth **1.8-2.2x** over the whole life of a hundred-entry map (native findings §5), which is the
   largest single small-map effect measured, and it is why a map literal and a sized construction
   reach this rather than the growth path.

   Not called `reserve`, for the reason `containsKey` is not called `contains`: `Array(a)`'s is
   already declared at this arity and this language has no ad-hoc overloading for plain functions.
-}
pub fn (Hash(k)) reserveMap(&self: Map(k, v), wanted: Size) -> {}:
    reserveEntries(self, wanted)

    if wanted > scanLimitOf(self):
        let slots = indexSlotsFor(wanted)
        if slots > indexSlots(self) || self.mask == 0 then indexRebuild(self, slots)

-- A map sized at construction, which is the one construction site where the count is known.
pub fn (Hash(k)) newMap(capacity: Size) -> Map(k, v):
    let &self = emptyMap() :: Map(k, v)
    reserveMap(self, capacity)

    return self

{-
   The threshold, asked of the map rather than of a run - and this is the shape every accessor below
   takes, for the reason the JS declaration gives: the two targets store an entry in different
   places, so what they share is the *question* and not the field it is answered from.
-}
@platform(native) fn scanLimitOf(self: Map(k, v)) -> Size = scanLimitFor(self.entries.run.items)
@platform(js) fn scanLimitOf(self: Map(k, v)) -> Size = scanLimitForKeys(self.keys.items)

-- How many entries there are, which is the length of whichever run holds them. The two JS runs are
-- the same length by construction: every write below appends to or removes from both.
@platform(native) fn entryCount(self: Map(k, v)) -> Size = length(self.entries)
@platform(js) fn entryCount(self: Map(k, v)) -> Size = length(self.keys)

-- Room for `wanted` entries in the run or runs that hold them.
@platform(native) fn reserveEntries(&self: Map(k, v), wanted: Size) -> {} =
    reserve(self.entries, wanted)

@platform(js) fn reserveEntries(&self: Map(k, v), wanted: Size) -> {}:
    reserve(self.keys, wanted)
    reserve(self.values, wanted)

-- One pair appended. Native writes one entry; JS writes each half into its own run, in that order,
-- so the two runs are never observed at different lengths by anything between them.
@platform(native) fn appendPair(&self: Map(k, v), ->key: k, ->value: v) -> {} =
    push(self.entries, Entry {key: key, value: value})

@platform(js) fn appendPair(&self: Map(k, v), ->key: k, ->value: v) -> {}:
    push(self.keys, key)
    push(self.values, value)

{-
   One key and one value, borrowed - and the reason these are functions rather than a subscript.

   Three of them rather than two, because which one a place produces is the caller's question and a
   subscript answers only the first: an argument is resolved as an ordinary expression before any
   callee is known, so `xs[i]` in a `&` position reaches `Index.get` and hands over a borrow that may
   not be written - see Reject.Exchange.yana, which is that ordering stated as a rejection. The value
   replacement in `insert` needs the mutable one.

   They also take the bounds check off the probe, which is the smaller of the two reasons and still a
   real one: every index reaching these came out of the index run or out of a walk below the count,
   so the test would be one this map has already made.

   `return self` is what roots the borrow in the map, and the pointer arithmetic underneath is
   unchecked by construction on the terms every other container in this module is written on.

   Written as the field of an entry borrow natively rather than as `borrow(addressOf(entry.key))`,
   and the difference is a target: taking an *address* of storage that is not a whole local is the
   gap codegen/js/inst.cpp reports, because the box it would have to make is a copy and a write
   through it would reach nobody. A *borrow* of the same place is not - it is the narrow reference
   that backend already builds, which names the slot rather than copying it.
-}
@platform(native) fn keyAt(return self: Map(k, v), at: Size) -> &k =
    borrow(self.entries.run.items + at).key

@platform(js) fn keyAt(return self: Map(k, v), at: Size) -> &k = hostAt(self.keys.items, at)

{-
   The key at one entry, hashed and compared - one body each, over the `keyAt` above.

   **`:: k` is what makes the hash resolve, and it is not decoration.** `keyAt` hands back a `&k`,
   and a borrow handed to a parameter declared `k` is inferred as an argument of *that* type rather
   than read through - so the generic body asked for `Hash(&k)` and was told there is no such
   instance. An ascription supplies the expected type and the read is then the ordinary one. `==`
   needs none, because an operator already reads both of its sides.

   Written as functions over `keyAt` rather than as two bodies each, so that where a key lives is
   the one thing either target has to answer, and so the unchecked read is the one `keyAt` already
   does: `self.keys[at]` here would be the subscript, and a subscript carries the bounds check this
   probe has already made.
-}
fn (Hash(k)) hashAt(self: Map(k, v), at: Size) -> U32 = hashKey(keyAt(self, at) :: k)
fn (Hash(k)) keyEquals(self: Map(k, v), at: Size, key: k) -> Bool = keyAt(self, at) == key

{-
   And the value inside one, borrowed - which is what `find` and the subscript hand back.

   Written as the field of the entry borrow rather than as `borrow(addressOf(entry.value))`, and the
   difference is a target: taking an *address* of storage that is not a whole local is the gap
   codegen/js/inst.cpp reports, because the box it would have to make is a copy and a write through
   it would reach nobody. A *borrow* of the same place is not - it is the narrow reference that
   backend already builds, which names the slot rather than copying it. So this is one body for both
   targets and the other spelling is one for neither.
-}
@platform(native) fn valueAt(return self: Map(k, v), at: Size) -> &v =
    borrow(self.entries.run.items + at).value

@platform(native) fn valueAtMut(return &self: Map(k, v), at: Size) -> &v =
    borrowMut(self.entries.run.items + at).value

@platform(js) fn valueAt(return self: Map(k, v), at: Size) -> &v = hostAt(self.values.items, at)
@platform(js) fn valueAtMut(return &self: Map(k, v), at: Size) -> &v = hostAtMut(self.values.items, at)

{-
   Appending an entry to the dense run, and pointing the index at it.

   Three things happen in order and the order matters: the entries run grows first, so the entry
   number this writes into the index is one the run can hold; the index is grown *before* the slot
   is chosen, since a rebuild moves every slot; and the threshold is crossed here rather than at the
   lookup, because it is a question about how many entries there are.
-}
fn (Hash(k)) appendEntry(&self: Map(k, v), ->key: k, ->value: v, h: U32) -> {}:
    let at = entryCount(self)
    appendPair(self, key, value)

    if self.mask == 0:
        -- Below the threshold there is no index, and crossing it is what builds the first one.
        if at + 1 > scanLimitOf(self):
            indexRebuild(self, indexSlotsFor(at + 1))

        return

    -- 7/8 is the load factor. A rebuild at *twice* the slot count where the entries genuinely fill
    -- it, and at the same count where they do not - which is how a table full of tombstones is
    -- cleaned without doubling the memory it never needed.
    let slots = indexSlots(self)
    if (self.live :: Size) + 1 > slots - (slots `shr` 3):
        indexRebuild(self, if (at + 1) * 2 > slots then slots + slots else slots)
        return

    indexPlace(self, at, h)

{-
   Insert - Implementation-Map.md §4.3.

   On a hit the value is replaced and the old one handed back, which is what makes the answer a
   `Maybe(v)`: a caller who wants the displaced value takes it, and one who does not writes
   `insert(m, k, v)` and lets the drop run at the call.

   The key is *not* replaced on a hit. Two keys that `==` calls equal are one key, so keeping the one
   already in the map is the answer that costs nothing; the one handed over is released at the end of
   this body by the ordinary rule for a `->` parameter nothing stored.
-}
pub fn (Hash(k)) insert(&self: Map(k, v), ->key: k, ->value: v) -> Maybe(v):
    let h = hashKey(key)
    let at = findEntry(self, key, h)

    if at >= 0:
        return Just(exchange(valueAtMut(self, at), value))

    appendEntry(self, key, value, h)
    return Nothing

{-
   Remove, which is a **swap-remove** - Implementation-Map.md §4.3 and §5.

   The whole reason the entries run has no holes: the last entry is moved into the gap and its own
   index slot is re-pointed, which is one extra probe per removal. The alternative - closing the gap
   and renumbering - is what native findings §6 prices at **155x** on churn at a thousand entries,
   and it is why the promise in §5 is insertion order *until something is removed* rather than
   insertion order unconditionally.

   The removed slot becomes **Deleted**, and `live` does not count down: a slot that was part of a
   full group has to keep saying "keep looking" or every probe that ran through it would stop one
   slot early. Abseil's test for when it may go back to Empty instead is not implemented here - what
   it costs is that a delete-heavy map rebuilds sooner, which the rebuild at the *same* slot count in
   `appendEntry` is what makes affordable.

   **The whole entry is handed back and not just the value**, which is a departure from §4's
   `Maybe(v)`. It is forced rather than chosen: `entry.value` alone is a move of *part* of a value,
   which the ownership pass refuses because the other part would be left with no owner, and the
   language has no record-destructuring pattern to take the two apart with. Handing over both costs
   nothing and loses nothing - a caller who wanted the value reads `.value` off it and lets the
   entry's own drop release the key.
-}
pub fn (Hash(k)) removeKey(&self: Map(k, v), key: k) -> Maybe(Entry(k, v)):
    let h = hashKey(key)
    let at = findEntry(self, key, h)
    if at < 0 then return Nothing

    if self.mask != 0 then indexForget(self, key, h)

    {-
       The moved entry's slot is re-pointed *before* the move, which is the ordering this has to keep:
       after it the entry is no longer at `last`, so anything that went looking for it by key would
       read past the end of the run. `indexRepoint` therefore looks for the entry *number*, and the
       hash it walks from is computed here, where the borrow of the key ends at the call rather than
       being held across a mutable one of the map.
    -}
    let last = entryCount(self) - 1

    if at != last && self.mask != 0:
        let moved = hashAt(self, last)
        indexRepoint(self, last, at, moved)

    return Just(takeEntry(self, at))

{-
   Taking one entry out and closing the hole with the last one.

   Two bodies, and they are the one place a map reads and writes its entries run without going
   through a bounds check - which is what a swap-remove is: two moves at indices the caller has
   already established are live. `Array.remove` cannot serve, because it closes the gap by *shifting*
   and that is the 155x this design exists to avoid.

   The length is assigned last, and on JS that is also what releases the duplicate: a plain host
   array's `length` is its occupancy and assigning it truncates, so recording the new count and
   freeing the slot are the same statement. It is the same line `Array.remove` relies on.
-}
@platform(native) fn takeEntry(&self: Map(k, v), at: Size) -> Entry(k, v):
    let last = entryCount(self) - 1
    let items = self.entries.run.items
    let ->doomed = *(items + at)

    if at != last:
        let ->moved = *(items + last)
        store(items + at, moved)

    self.entries.length = truncate(last) :: Count
    return doomed

@platform(js) fn takeEntry(&self: Map(k, v), at: Size) -> Entry(k, v):
    let last = entryCount(self) - 1
    let ->doomedKey = hostRead(self.keys.items, at)
    let ->doomedValue = hostRead(self.values.items, at)

    if at != last:
        let ->movedKey = hostRead(self.keys.items, last)
        let ->movedValue = hostRead(self.values.items, last)
        hostWrite(self.keys.items, at, movedKey)
        hostWrite(self.values.items, at, movedValue)

    self.keys.length = truncate(last) :: Count
    self.values.length = truncate(last) :: Count

    return Entry {key: doomedKey, value: doomedValue}

-- The last entry released and the run or runs shortened by one, which is what `clear` walks
-- backwards over. `Array.remove` at the last index is a swap-remove with nothing to swap.
@platform(native) fn removeLast(&self: Map(k, v)) -> {}:
    let dropped = remove(self.entries, truncate(entryCount(self) - 1) :: Int)

@platform(js) fn removeLast(&self: Map(k, v)) -> {}:
    let at = truncate(entryCount(self) - 1) :: Int
    let droppedKey = remove(self.keys, at)
    let droppedValue = remove(self.values, at)

-- Whether a key is in the map, which is the probe with its answer thrown away. Not `contains`, for
-- the reason `reserveMap` is not `reserve`.
pub fn (Hash(k)) containsKey(self: Map(k, v), key: k) -> Bool =
    findEntry(self, key, hashKey(key)) >= 0

{-
   Lookup - Implementation-Map.md §4.

   `Maybe(&v)` is a nullable borrow, and §10 leaves open whether it is one pointer: it is one exactly
   when Repr's niche computation folds `Nothing` into the null address, which is the same fold
   `Maybe(%a)` already gets. `MapRepr.yana` is where that is asserted as a number rather than assumed
   here.

   What a result of this shape needed from the compiler was liveness rather than layout: a borrow
   *inside* a returned value roots the map exactly as a bare `&v` would, and until analyze_effects
   learned to walk out of a direct aggregate into the call that produced it, the map was dropped at
   the call and the borrow read storage that had been handed back.
-}
pub fn (Hash(k)) find(return self: Map(k, v), key: k) -> Maybe(&v):
    let at = findEntry(self, key, hashKey(key))
    if at < 0 then return Nothing

    return Just(valueAt(self, at))

pub fn (Hash(k)) findMut(return &self: Map(k, v), key: k) -> Maybe(&v):
    let at = findEntry(self, key, hashKey(key))
    if at < 0 then return Nothing

    return Just(valueAtMut(self, at))

{-
   Emptying a map without giving its storage back.

   Every entry is released and both runs are kept, which is what a caller who is about to fill it
   again wants. The index is not rebuilt - it is filled with the empty sentinel, which is the same
   work a rebuild would start with and none of the work it would go on to do.
-}
pub fn clear(&self: Map(k, v)) -> {}:
    let &at = entryCount(self)

    while at > 0:
        at = at - 1
        removeLast(self)

    if self.mask != 0 then indexReset(self)

{-
   Iteration - Implementation-Map.md §5.

   **Every entry exactly once, in insertion order as long as nothing has been removed.** After a
   removal the order is unspecified: it is deterministic for a given target and sequence of
   operations and does not change between two iterations of an unmodified map, but it is not
   insertion order and it is not the same on both targets. That is the promise the layout can keep on
   both, and the stronger one costs 155x on churn.

   A walk over the dense prefix and nothing else - **iteration never touches the index**, which is
   what makes it 2.6-6.5x a Swiss table's and is the half of the two-type question this design wins
   outright.

   **`Entry(&k, &v)` - the entry record applied at two borrows**, which is what a target with the key
   and the value in separate runs can produce and a borrow of a whole entry is not. Reading is
   unchanged from the borrow this used to yield: `entry.key` and `entry.value` are both places, and
   neither half is copied. §4's `{k, &v}` is still not it, since a key read out by value would be a
   move out of storage the map owns.

   **`keys` and `values` are not declared, and `drain` is not either.** The first two are one line
   each over this iterator - `for entry in pairs(m)` and the field - and the name `values` is already
   `Contiguous`'s, which this language has no ad-hoc overloading to sit beside; the same rule that
   made `contains` into `containsKey`. `drain` needs an `iter fn` that consumes its source, which
   nothing in the language does yet.
-}
pub iter fn pairs(self: Map(k, v)) -> Entry(&k, &v):
    let count = entryCount(self)
    let &at = 0 :: Size

    while at < count:
        yield Entry {key: keyAt(self, at), value: valueAt(self, at)}
        at = at + 1

{-
   Subscripting a map - Implementation-Map.md §7.

   `m[key]` is a lookup and **traps on an absent key, in every build**. Unlike a bounds check this
   one cannot be release-unchecked: a missing key has no address to hand back, so "unchecked" would
   mean returning a borrow of whatever entry number happened to be there. The total forms are `find`
   and `containsKey`, and they are what a program that does not know should call.

   `m[key] = value` is `IndexInsert` below rather than this class's `getMut`, for the reason §7
   gives: there is nothing to borrow for an absent key and no zero value to invent for an arbitrary
   `v`.
-}
instance (Hash(k)) Index(Map(k, v), k, v):
    fn get(return self: Map(k, v), key: k) -> &v:
        let at = findEntry(self, key, hashKey(key))
        checkCondition(at < 0)

        return valueAt(self, at)

    fn getMut(return &self: Map(k, v), key: k) -> &v:
        let at = findEntry(self, key, hashKey(key))
        checkCondition(at < 0)

        return valueAtMut(self, at)

{-
   And the assignment form - Implementation-Map.md §7's second decision.

   `m[key] = value` **inserts**, which `getMut` cannot express: it has to answer a borrow, and an
   absent key has none. So the assignment form gets a class member of its own, which
   `resolveSubscript` prefers for `c[k] = v` wherever the container has an instance and falls back to
   `getMut` where it does not - so `Array`'s assignment is unchanged.

   It is the smaller of the two available answers. The other is making every container's `getMut`
   fallible, which changes `Array` too.

   **The key is a `->` and not a read**, which is the one place this member differs from `get` and
   `getMut` beside it. A lookup only *reads* the key it probes with, so those two take one by the
   borrow convention; an insert that misses **stores** it, and storage that outlives the call cannot
   be borrowed. Written as a read this was a diagnostic inside the library rather than at the
   assignment - `insert` declares `->key: k`, so the body handed borrowed storage to a sink - and it
   was invisible for every key a copy answers for: `m[1] = 2` compiled and `m["a"] = 2` did not.
-}
pub class IndexInsert(c -> k, v):
  fn insertAt(&self: c, ->index: k, ->value: v) -> {}

instance (Hash(k)) IndexInsert(Map(k, v), k, v):
    fn insertAt(&self: Map(k, v), ->index: k, ->value: v) -> {} = insert(self, index, value)
)COLLECTIONS";

/*
 * How many entries a map scans before it allocates an index - Implementation-Map.md §2's third row,
 * folded at the call site.
 *
 * The key comes out of the argument's *declared* type rather than out of any value: `%k` at this
 * call is a pointer to whatever `k` was substituted with, exactly as `hostFixedCapacity` reads its
 * element, and a map with nothing in it has no key to be handed one of.
 *
 * Two entry points, because the two targets no longer hold a key in the same place: `scanLimitFor`
 * is handed the entries run and takes the first field of the `Entry` it points at, `scanLimitForKeys`
 * is handed the key run and the pointee *is* the key. Which of the two is a caller's is decided by
 * `@platform` at `scanLimitOf`, and it has to be decided there rather than here: a record key is a
 * record exactly as an entry is, so one function reading `Entry` off the shape would answer 8 for a
 * `Map(Point, Int)` on the target whose run holds bare keys.
 *
 * Three answers and they are §2's: **8** for a machine word, whose hash is one multiply and a shift;
 * **32** for a `String`, whose hash is a walk over its units; and **64** for anything structural,
 * whose hash is that walk plus a fold per field. An erased generic body reaches none of them and
 * takes the largest, which is a slower small map rather than a wrong one - the threshold decides
 * where a scan stops paying, and both sides of it answer the same questions.
 */
template<bool unwrapEntry>
static ModulePtr<Value> emitScanLimit(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                      LocationId source, StringId name) {
    auto global = resolver.global;
    auto pointee = pointeeType(global, resolver.valueType(args[0]));
    TypePtr key = pointee;

    // `Entry(k, v)`'s first field, which is the key. One shape and no search: the argument is the
    // entries run, whose element this map declares two lines above the function that asks.
    if(unwrapEntry) {
        key = nullptr;

        if(pointee && global[pointee]->kind == Type::Record) {
            auto record = (RecordType*)global[pointee];

            if(record->constructors.size()) {
                auto content = record->constructors.get(global, 0).content;

                if(content && global[content]->kind == Type::Tup) {
                    auto& fields = ((TupType*)global[content])->fields;
                    if(fields.size()) key = fields.get(global, 0).type;
                }
            }
        }
    }

    auto kind = key ? global[key]->kind : Type::Error;
    auto limit = kind == Type::Int || kind == Type::Float ? 8 : (kind == Type::String ? 32 : 64);

    return resolver.makeInt(source, type, limit);
}

void defineCollections(Program& program) {
    auto& context = program.context;

    auto name = context.addQualifiedName("Collections", 11, 1);
    Lexer lexer(context, context.diagnostics, StringView { kCollectionsSource, stringLength(kCollectionsSource) }, name);
    Parser parser(context, lexer, name);

    // A declaration with no body, which this module has since the bulk operations landed: what a
    // call to one expands to is chosen by the compiler from the two implementations beside it. Core
    // and Native have said this since they were written, and for the same reason.
    parser.allowSignatures = true;

    auto ast = new ast::Module(parser.parseModule());

    auto module = program.addModule(ast->name, *ast->region);
    program.embeddedAsts.push(ast);

    resolveModuleDecls(*module, *ast, nullptr);

    /*
     * Before the bodies, not after them.
     *
     * `Program::arrayType` is what makes `Array(a)` recognizable *as* the growable array - it is
     * what `sliceOf` asks, so it is what decides whether the ordinary conversion to a slice exists.
     * Setting it afterwards meant this module alone could not use its own container: `elements`,
     * whose whole body is `slice(self, 0, self.length)`, saw `Array(a)` as an unrelated record and
     * reported that it does not fit `Flat(a)`.
     *
     * The declaration is what the pointer names, so it is available as soon as the declarations have
     * been read; nothing in this module's signatures writes `[a]`, which is the only thing that
     * would have needed it earlier still.
     */
    program.collections = module;
    auto array = module->namedTypes.get(context.addQualifiedName("Array", 5, 1));
    if(array) program.arrayType = (RecordType*)(*program.types)[array.unwrap()] - *program.types;

    // The map, on the same terms - Implementation-Map.md §7. One name for both platform rows: the
    // `@platform` selection has already run over the declarations, so whichever of the two `Map`
    // declarations this target kept is the one the literal instantiates.
    auto map = module->namedTypes.get(context.addQualifiedName("Map", 3, 1));
    if(map) program.mapType = (RecordType*)(*program.types)[map.unwrap()] - *program.types;

    // §5's two, looked up here for the reason Core's are looked up where they are declared: what
    // asks for them is the resolver rather than a name a program wrote. See CoreClasses.
    program.coreClasses.contiguous = classNamed(*module, "Contiguous"_v);
    program.coreClasses.chunked = classNamed(*module, "Chunked"_v);
    program.coreClasses.indexInsert = classNamed(*module, "IndexInsert"_v);

    // The map's one per-key-type constant - Implementation-Map.md §2 and §4.4. A rule in resolve
    // with every reader folded against it, which is the shape Containers §14 settled for and the
    // only one of that section's three rules that needs nothing new.
    attachIntrinsic(*module, "scanLimitFor"_v, emitScanLimit<true>);
    attachIntrinsic(*module, "scanLimitForKeys"_v, emitScanLimit<false>);

    /*
     * Recorded for the reason `allocateHeap` is: the compiler emits the call, so there is no name in
     * any program for resolution to start from.
     *
     * Before defineContainerInstances, because that is what makes a subscript check reachable: the
     * generated `get` bodies below are resolved against whatever this holds at the time.
     */
    if(context.settings.checks) {
        auto found = module->functions.get(context.addUnqualifiedName("checkCondition", 14));
        program.checkCondition = found ? found.unwrap() : nullptr;

        /*
         * And the arm it branches to, marked as one control does not come back out of.
         *
         * Here rather than in an attribute on the declaration because there is nothing in the source
         * to attach one to that would mean anything: `checkFailed` is `exitProcess(134)` and a
         * `return`, and what makes it final is the kernel rather than the shape of the body. Both
         * targets' spellings are equally final - a status on native, a thrown value on JS - so the
         * fact is about this function rather than about either platform's implementation of it.
         *
         * See `Function::noReturn` for what reads it. It is set whether or not `checkCondition` was
         * found, since a build with the checks off has no call to either.
         */
        auto failed = module->functions.get(context.addUnqualifiedName("checkFailed", 11));
        if(failed) (*module->arena)[failed.unwrap()]->noReturn = true;
    }

    // After `arrayType` above, and before this module's own bodies below - several of which
    // subscript, and would reach an instance that does not exist yet.
    defineContainerInstances(*module);

    // The bulk operations, whose declarations above have no body: which of the two implementations
    // beside each one a call takes is decided where the call is - see simd.cpp.
    defineBulkOperations(*module);

    resolveModuleBodies(*module);
}

/*
 * Text.
 *
 * `String`'s operations, split out of Collections - Implementation-Simplification.md §17.
 *
 * It is a module of its own for one reason, and the reason is a *cycle* rather than a division of
 * subject matter. What a native string is made of is Native's - a `Run(U8)` and a count - so the
 * reinterpretation that hands those two words out has to live behind an import that already means
 * "this is unsafe". But the run those words describe is a container, and the container's declaration
 * has to be implicitly visible because `[a]` is grammar. So the unsafe half sits *above* the
 * container it names and *below* the algorithms that use it, and one module cannot be on both sides
 * of that. See NativeText, which is the half in between.
 *
 * Implicitly imported, like Collections and for the same reason: a string literal is grammar, and
 * what `print` and `Show` mean has to be reachable without being asked for. That costs nothing in
 * safety, because an import is not transitive - see findInstances in name.cpp, and Program::native.
 */
static const char* kTextSource = R"TEXT(
import Native
import NativeText
import Host

{-
   ==========================================================================================
   `String` - Implementation-String.md parts 3 and 8, and Implementation-Storage.md part B's sink.
   ==========================================================================================

   Two implementations and one signature, exactly as `Array(a)` has. Nothing below this point is
   reachable from a program that has not written a string; nothing above it knows which target it is
   on.

   The guiding principle of that document is that the *default* path - length, indexing, append,
   concat - costs what the target's own string costs. That is why `length` is a field read rather
   than a walk, why `stringUnit` does no decoding, and why the whole Unicode tier (code points,
   grapheme clusters, boundary-safe slicing) is deliberately absent here rather than folded into
   these: it is parts 4 to 7, and every one of them is layered *on* this without changing it.
-}

{-
   How many native units the string holds - part 3's `.length`, O(1) on both targets.

   **This is not a portable number, on purpose.** It is UTF-8 bytes natively and UTF-16 code units on
   JS, so the same content can answer differently in the two builds - a trade part 3 makes explicitly
   in exchange for zero overhead, on the grounds that the *complexity class* is the thing that has to
   be uniform across targets and a constant factor is a documented platform fact. Anyone who needs
   the same number everywhere asks for a decoded count instead, which is part 6 and is O(n) on both.
-}
@platform(native) instance Length(String):
    -- Two steps rather than one: the stored count is a `@bits(30) U32` and `Size` is `I64` here, and
    -- a cast straight between them is not one the lower IR accepts. Through the unrefined width
    -- first, exactly as `resize` reads a run's capacity.
    fn length(self: String) -> Size:
        let units = stringData(self).bytes.length :: Int
        return units :: Size

@platform(js) instance Length(String):
    fn length(self: String) -> Size = hostStringLength(self)

{-
   One raw native unit - part 3's `String[i: Int]`.

   **A function and not an `Index` instance**, which is a real limitation rather than an omission and
   is worth stating where someone will look for the subscript. `Index` hands back a *borrow*, because
   `xs[i] = v` and `&xs[i]` are the operations that make a subscript worth having - and a string unit
   is not storage on both targets. On JS it is `charCodeAt`, a number computed from a host string
   that has no addressable units at all. Core's own note on the class says exactly this: it does not
   cover a container whose elements are computed rather than stored, and a weaker parent class that
   yields values is what such a container wants and is not written yet.

   So `s[i]` does not parse into this today; `stringUnit(s, i)` is the accessor. When the read-only
   parent class lands, this becomes an instance of it and the spelling arrives with no change here.

   No decode, no validation that what comes back is a whole code point. Reading one unit is always
   safe *as a read*; it just might be one byte of a multi-byte sequence, exactly as indexing one byte
   out of a `[U8]` promises nothing about the larger structure it belongs to. Bounds are unchecked,
   which is the same tier `get` on an array is on and goes the same way when `checkBounds`
   (Implementation-Containers.md §15) lands.
-}
@platform(native) pub fn stringUnit(self: String, index: Size) -> Int =
    (*(stringData(self).bytes.run.items + index)) :: Int

@platform(js) pub fn stringUnit(self: String, index: Size) -> Int = hostCharCodeAt(self, index)

{-
   Concatenation - part 8, which observes that this is *already* encoding-agnostic with no extra
   work: joining two strings never inspects their content, only their raw units, whatever those mean.

   The native body is the one allocation in the raw tier. It sizes the result exactly, so a `+` chain
   allocates once per `+` - which is what makes `Show`'s bound worth having and is exactly the cost
   Implementation-Storage.md part 8 exists to avoid for interpolation. A program building a string in
   a loop should reach for the sink below, not for this.

   The JS body is the host's `+`, which the engine implements with a rope and which therefore has the
   opposite cost profile. That difference is real and is not hidden; it is also the reason `Show`
   writes into a sink rather than returning a string, so that neither target's `+` is on the hot path.
-}
@platform(native) instance Append(String):
    fn +(lhs: String, rhs: String) -> String:
        let leftLength = length(lhs)
        let rightLength = length(rhs)

        let &fresh = newStringOfCapacity(leftLength + rightLength)
        appendUnits(fresh, stringData(lhs).bytes.run.items, leftLength)
        appendUnits(fresh, stringData(rhs).bytes.run.items, rightLength)

        return fresh

@platform(js) instance Append(String):
    fn +(lhs: String, rhs: String) -> String = hostConcat(lhs, rhs)

{-
   Equality, as a raw unit-wise comparison - part 3.

   *"Always correct regardless of encoding, since encoding is an injective function of content"*, so
   this is one of the few places the two targets provably cannot disagree: two strings are equal here
   exactly when they are equal there, whatever each is storing.

   The length test first is not just an optimization - it is what makes the loop's bound safe.
-}
@platform(native) instance Eq(String):
    fn ==(self: String, other: String) -> Bool:
        let count = length(self)
        if count != length(other) then return False

        let &i = 0 :: Size
        while i < count:
            if stringUnit(self, i) != stringUnit(other, i) then return False
            i = i + 1

        return True

    fn !=(self: String, other: String) -> Bool = !(self == other)

@platform(js) instance Eq(String):
    fn ==(self: String, other: String) -> Bool = hostStringEq(self, other)
    fn !=(self: String, other: String) -> Bool = !hostStringEq(self, other)

{-
   Ordering, raw unit-wise lexicographic - part 3's *default*, chosen for the same "no overhead"
   reason `.length` is raw.

   This one *can* diverge between targets, and part 3 says so rather than papering over it: on JS,
   UTF-16 code-unit order is not code-point order for supplementary-plane characters compared against
   certain BMP ones. Every ASCII comparison - which is the overwhelming majority of what anyone sorts
   - agrees on both. Code that needs guaranteed code-point order calls an explicit
   `compareByCodePoint`, which is part 6's and is not written yet.
-}
@platform(native) instance Ord(String):
    fn compare(self: String, other: String) -> Ordering:
        let leftLength = length(self)
        let rightLength = length(other)
        let &shared = leftLength
        if rightLength < shared then shared = rightLength

        let &i = 0 :: Size
        while i < shared:
            let left = stringUnit(self, i)
            let right = stringUnit(other, i)
            if left < right then return LT
            if left > right then return GT
            i = i + 1

        -- Every shared unit agreed, so the shorter one is the smaller - which is what makes this
        -- lexicographic rather than merely a prefix test.
        if leftLength < rightLength then return LT
        if leftLength > rightLength then return GT
        return EQ

@platform(js) instance Ord(String):
    fn compare(self: String, other: String) -> Ordering:
        if hostStringLt(self, other) then return LT
        if hostStringLt(other, self) then return GT
        return EQ

{-
   Hashing a string - Implementation-Map.md §3 and §3.1.

   **One body for both targets**, which is unusual in this module and is the honest state rather
   than a claim about the two: what a string hash costs is the walk over its units, and both targets
   walk. `stringUnit` is one load natively and one `charCodeAt` on JS, and `../../benchmark/map-js`
   §9.1 measures that walk as the *whole* cost there - reading the characters and merely adding them
   costs 30.9 ns per key where FNV-1a costs 24.0, so every candidate mixer lands within 12% of every
   other and the mixer is not what to spend on.

   The **length goes in first**, through the state fold, so that the two strings a prefix relation
   separates cannot fold to one value however the units happen to land - and so that the one
   expensive operation in this instance, the multiply-fold natively, happens once rather than per
   unit. What runs per unit is FNV-1a's step: one exclusive-or and one 32-bit multiply, which is
   `Math.imul` on JS and a `lea`-sized `imul` natively.

   **This is not §3.1's native row, and the gap is worth naming.** That row reads a string eight
   bytes at a time and folds each word with a 64x64 multiply, which is where its 1.97x on a string
   lookup comes from. Doing that here needs an *unaligned* sixty-four-bit load out of a run of bytes,
   and the language exposes no such load: `*p` at a `%U64` claims an alignment a string's bytes do
   not have, and the one overreading load there is (`vectorPast`) is a vector's and carries §8's
   tail-read guarantee with it. Closing it is a `Native` primitive and a fixture, not a change here.

   `mix32` in the map is what finalizes this, which is what lets the per-unit step stay this cheap:
   the seven-bit tag is taken from the *top* of the finalized word, so a fold whose entropy sits low
   is repaired once per lookup rather than once per unit.
-}
instance Hash(String):
    fn hash(value: String, state: U32) -> U32:
        let n = length(value)
        let &h = hashWord(state, truncate(n) :: U32)
        let &i = 0 :: Size

        while i < n:
            h = (h `xor` (truncate(stringUnit(value, i)) :: U32)) * 16777619
            i = i + 1

        return h

{-
   Releasing a string - Implementation-Containers.md §2's placement switch, and nothing else.

   An authored `Reclaim` rather than derived glue, because `String` is a primitive and has no members
   for a derived walk to recurse into. What it does is exactly `releaseRun`: hand the slots back if
   and only if they are the allocator's. A literal's run is `runBorrowed`, so this folds to nothing
   for one; a concatenated or formatted string's is `runFromHeap`, so it frees once.

   A `Reclaim` and not a `Drop`, which is what keeps a string region-placeable: there is no effect at
   last use, only storage, and storage is exactly the kind of thing a region discharges in bulk.

   No JS instance at all, deliberately. The collector owns every host string, so there is nothing to
   release and no traversal to run - a string has no elements whose lifetimes could end in something,
   which is the one thing §14 says does *not* fold away for a container. That asymmetry is the whole
   of the difference, and it is why this is `@platform`-split rather than a body with a branch in it.

   Implementation-Simplification.md §17 left open whether this should become `Array(U8)`'s derived
   glue now that the bytes *are* an array. It should not, and the reason is what that glue contains:
   a walk of every element, and then `releaseRun(value.run)`. At `U8` the walk provably does nothing
   - a byte's move is a copy and its teardown is empty - so the optimized build would fold it back to
   the line below, and the unoptimized build would run a loop over every byte of every string. The
   line below is what the glue reduces to, written directly, and `String.yana.lower.expect` shows it
   reaching the machine as the placement test and a `freeHeap` with no call left at all.
-}
@platform(native) instance Reclaim(String):
    fn reclaim(->value: String) -> {} = releaseRun(stringData(value).bytes.run)

{-
   ==========================================================================================
   The sink - Implementation-Storage.md part B, whose `Storage(StringUnit)` is this.
   ==========================================================================================

   That document's sink was a `Storage(a)`, the primitive Implementation-Containers.md §0 demoted;
   what replaced it is "the target's growable string", which is `String` itself. So formatting builds
   the string *in place* and finishing it is a reinterpretation rather than a copy, which is the
   property part 7 gives as the reason not to abstract over sinks in the first place.

   Three operations, and each has a native body that manages a run and a JS body that is one
   concatenation. The bound machinery is real natively and is inert on JS, where the host owns growth
   and there is no capacity to reserve - which is not a gap: `showBound` still decides *sizes*, and a
   host that ignores the reservation still gets the right answer, just without the allocation it
   saves.
-}

-- A string with room for `capacity` units and nothing in it yet. The run is the allocation; the
-- length is zero until something appends.
@platform(native) pub fn newStringOfCapacity(capacity: Size) -> String =
    stringFromData(StringData {bytes: Array {run: newRun(capacity), length: (0 :: Count)}})

@platform(js) pub fn newStringOfCapacity(capacity: Size) -> String = ""

{-
   Room for `wanted` more units, growing if there is not - the check-and-grow that
   Implementation-Storage.md part 8's strategy (c) appends through.

   Geometric, so that a format whose bound was `Nothing` still appends in amortized constant time
   rather than reallocating per unit. `resize` is what relocates, and a *borrowed* run - a literal
   being appended to - relocates by copying and freeing nothing, which is where copy-on-write happens.
-}
@platform(native) pub fn reserveString(&self: String, wanted: Size) -> {}:
    -- The container's own growth, which is what this used to be a second copy of. `reserve` takes
    -- the capacity to reach and this takes the amount to add, which is the only difference between
    -- the two and the only thing left here.
    let target = stringDataMut(self)
    reserve(target.bytes, (target.bytes.length :: Size) + wanted)

@platform(js) pub fn reserveString(&self: String, wanted: Size) -> {} = {}

{-
   One unit appended - what every `Show` instance ultimately writes through.

   Natively this is a store and a count bump, with the reservation hoisted out by whoever is about to
   write a known number of units. Part 7's contract is what makes that safe: an instance writes at
   most `showBound` units, the buffer was sized from the bounds, so the appends are provably in range
   and the reserve above them is the only check.
-}
@platform(native) pub fn pushUnit(&self: String, unit: Int) -> {}:
    -- `push` is the reserve, the bounds check, the store and the count bump, and it was all four of
    -- them written twice until the bytes became an `Array(U8)`.
    push(stringDataMut(self).bytes, truncate(unit) :: U8)

@platform(js) pub fn pushUnit(&self: String, unit: Int) -> {}:
    self = hostConcat(self, hostFromCharCode(unit))

-- A whole string appended, which is the common case and is a block copy rather than a loop.
@platform(native) pub fn pushString(&self: String, other: String) -> {}:
    appendUnits(self, stringData(other).bytes.run.items, length(other))

@platform(js) pub fn pushString(&self: String, other: String) -> {}:
    self = hostConcat(self, other)

{-
   The block copy both of the above are written in terms of. Private to this section: it takes a raw
   address, so it is exactly as unsafe as `copyMemory` and exactly as unreachable from a program.

   The check is the same one `push` makes and is here for a worse reason. `push` compared the count
   against the capacity and gave up, which lost an element; this copies a whole block and had no
   comparison at all, so a `reserveString` the allocator refused was followed by a `copyMemory` of
   `count` units into a buffer that had not grown to hold them. That is a write past the end of the
   run, not a lost append - and it was reachable by exactly the route the array's was, since both
   reserves end at the same `resize`.

   Read off `run.capacity` rather than off `reserveString`, which answers nothing: what has to hold
   before the copy is that the room is *there*, and that is one comparison against the field the copy
   is about to run past.
-}
@platform(native) pub fn appendUnits(&self: String, from: %U8, count: Size) -> {}:
    if count <= 0 then return {}
    reserveString(self, count)

    let target = stringDataMut(self)
    let wanted = (target.bytes.length :: Size) + count
    checkCondition((target.bytes.run.capacity :: Size) < wanted)

    copyMemory(target.bytes.run.items + (target.bytes.length :: I64), from, byteSpan(from, count))
    target.bytes.length = truncate(wanted) :: Count

{-
   ==========================================================================================
   `Show` - Implementation-Storage.md part 7's instances.
   ==========================================================================================

   The bounds below are the table that document gives, and each is exact rather than generous: the
   buffer is sized from them, so a bound larger than the truth wastes the difference and a bound
   smaller than the truth is the one thing an instance may not do.
-}

-- `"False"` is five units, which is the longer of the two.
instance Show(Bool):
    fn show(value: Bool, &to: String) -> {}:
        if value then pushString(to, "True")
        else pushString(to, "False")

    fn showBound(value: Bool) -> Maybe(Size) = Just(5)

{-
   The integers, written once over the widest signed type they all widen into.

   `showSigned` is the whole implementation and the instances below only say how wide the value can
   be. One body for both targets, because this is arithmetic and nothing else - `Long` is a machine
   word natively and a `bigint` on JS, and division and remainder mean the same thing on both.

   **The digits are produced in the negative domain**, which looks backwards and is the point. The
   most negative value of any width has no positive counterpart, so negating it first gives that same
   number back and a loop dividing it would emit wrong digits forever. Working downwards from zero
   makes `-9223372036854775808` an ordinary input rather than a case anyone has to remember. It needs
   truncating division and a remainder that follows the dividend's sign, which is what both targets
   do.

   The reversal buffer is a real cost - one temporary string per integer formatted - and it is the
   obvious thing to remove later: the digits are produced least-significant first and the sink can
   only be appended to. Writing them into a `[U8 *20]` instead would need no allocation at all, and
   is left until fixed arrays are reachable from a body this generic.

   Not `pub`: it is how the `Show` instances below are written and not something to call directly,
   since what a program wants of an integer is `show` and the instance decides the rest.
-}
fn showSigned(value: Long, &to: String) -> {}:
    if value == 0:
        pushUnit(to, 48)
        return {}

    -- Downwards from zero, so that every width's most negative value is representable throughout.
    let &remaining = value
    if remaining > 0 then remaining = 0 - remaining

    let &digits = newStringOfCapacity(20 :: Size)
    while remaining < 0:
        let digit = truncate(0 - (remaining `rem` 10)) :: Int
        pushUnit(digits, 48 + digit)
        remaining = remaining / 10

    if value < 0 then pushUnit(to, 45)

    -- Reversed, because the loop above produced them backwards.
    let &i = length(digits)
    while i > 0:
        i = i - 1
        pushUnit(to, stringUnit(digits, i :: Size))

-- `-2147483648` is eleven units, which is the widest an `Int` gets.
instance Show(Int):
    fn show(value: Int, &to: String) -> {} = showSigned(value :: Long, to)
    fn showBound(value: Int) -> Maybe(Size) = Just(11)

-- `-9223372036854775808` is twenty.
instance Show(Long):
    fn show(value: Long, &to: String) -> {} = showSigned(value, to)
    fn showBound(value: Long) -> Maybe(Size) = Just(20)

{-
   A string shows as itself, and its bound is a *runtime* value - which is the row that makes the
   point about `showBound`'s constant-ness not being in the type.

   `Just(11)` for `Int` folds to a literal at a concrete call site and this does not, and neither
   instance had to say which it was. That is the whole of what the `Maybe(Int)` shape buys, and it is
   why a format containing one string and three integers still sizes its buffer in one addition.
-}
instance Show(String):
    fn show(value: String, &to: String) -> {} = pushString(to, value)
    fn showBound(value: String) -> Maybe(Size) = Just(length(value))

{-
   What one hole contributes to a format's size - Implementation-Storage.md part 8, step 2.

   A plain function rather than something the compiler emits inline, because it is the same three
   lines at every hole and because this is where `Nothing` becomes a *number*. Strategy (c) - the
   unbounded case - is not a third code path in the formatter: it is this answering zero, so the
   buffer is seeded with the bounds that are known and the appends grow past it. That the three
   strategies are one shape is the whole of part 8's argument, and this is the line that makes it
   true.

   Zero and not one, and not a guess at a typical size. A seed is a promise about nothing: too small
   costs one growth, and inventing a number here would cost every bounded format the difference.
-}
fn formatBound(bound: Maybe(Size)) -> Size = match bound:
    Just(units) -> units
    Nothing -> 0

{-
   `print` - Implementation-Storage.md part 9.

   The point of the whole scheme, stated as one function: `print("x = {x}\n")` measures, sizes a
   buffer, formats into it and writes it once. Under strategy (a) the buffer never existed anywhere
   but the frame, so the first-day program's output path is one `write` and no allocation at all.

   Natively this is `writeFile` on the standard output descriptor, handed the run's base and the
   count - which is exactly what part 9 says, and is possible only because a growable string *is* a
   run of units rather than something that has to be flattened first. The result is discarded: what a
   short write or a closed pipe should do is a question about a `Result` type this does not have yet,
   and inventing an answer here would be the wrong place to make that decision.

   On JS it is `console.log`, which is not the same operation and is the honest mapping rather than a
   compromise: the host has no file descriptors, and its line-oriented console is what a program's
   output goes to. The trailing newline a caller writes is therefore doubled there - `console.log`
   adds its own - which is a real behavioural difference between the targets and is the reason this
   is `print` rather than `write`.
-}
@platform(native) pub fn print(text: String) -> {}:
    let bytes = stringData(text)
    let _ = writeStandardOutput(bytes.bytes.run.items, length(text))

@platform(js) pub fn print(text: String) -> {} = hostLog(text)

{-
   `Maybe(a)` is **not** an instance here, and the reason is a compiler limitation rather than a
   design choice - so it is written down rather than quietly left out.

   Implementation-Storage.md part 7's table gives it as the row that shows the bound *composing*:
   `Nothing` is seven units and `Just(x)` is `showBound(x)` plus six, so an option around a bounded
   type stays exactly bounded and one around an unbounded type stays unbounded. That is worth having,
   and the body is four lines.

   What blocks it is that a `&` parameter of a *constrained generic* instance method does not arrive
   as a place. Written out, `instance (Show(a)) Show(Maybe(a))` rejects its own body with

       a `&` argument must have exactly type String, but this is &mut String

   at every use of the sink - `pushString(to, "Nothing")` included, so it is not about the recursive
   dispatch. The same body as a concrete `instance Show(Maybe(Int))` compiles, which is what locates
   the fault: `bindFunctionArgs` binds a `Ref` argument to a place by `convention` alone and is
   right, so the reference is introduced somewhere on the constrained-instance path between the class
   signature and the body.

   The consequence is not confined to `Maybe`. It is exactly the shape every *container's* `Show`
   has - a generic instance constrained on its element's - so this is the thing to fix before more
   instances are written, rather than a wart on this one. What the composition would have
   demonstrated is separately visible in the two instances that do exist: `Show(Int)`'s bound is a
   constant that folds and `Show(String)`'s is a runtime value that does not, which is the property
   part 7 designed the `Maybe(Int)` shape for.
-}

{-
   ==========================================================================================
   The ASCII tier - Implementation-String.md part 5, Implementation-Vector.md §9 item 8.
   ==========================================================================================

   Scanning a string for one unit, at raw scan speed and with no decode step anywhere. It is correct
   without any encoding-awareness because an ASCII unit is **self-synchronizing in both UTF-8 and
   UTF-16**: a value below 0x80 never appears as part of the encoding of any other code point in
   either. That is a real property of both encodings rather than something being leaned on loosely,
   and it is what lets one signature over `CodeUnit` serve a byte scan natively and a UTF-16 scan on
   JS.

   **Natively these are the bulk operations over the string's own units**, which is the whole of what
   `instance Chunked(String, CodeUnit)` below buys: `indexOf` over a `Chunked` container is already
   the vector search - `firstSet` over a hit mask, sixteen units at a time - so the highest-value
   item in the vector plan is spelled here as three calls to machinery that was built for containers.
   A string is a container of code units, and saying so is what vectorizes it.

   **On JS they are the host's own**, which Implementation-String.md part 5 rules is "legitimate,
   encouraged" for exactly this family: searching for one fixed unit has no room for the cross-engine
   disagreement the decoding tier has to worry about, and `String.prototype.indexOf` is C++ that no
   `charCodeAt` loop this compiler emits will beat.

   The decoding tier is untouched by all of this and stays scalar.
-}
@platform(native) instance Chunked(String, CodeUnit):
    iter fn chunks(self: String) -> Flat(CodeUnit) = yield elements(stringData(self).bytes)

-- Where the first `unit` is, in units of this target's own encoding, or `Nothing`. The index is a
-- `Size` in that encoding and means what `stringUnit` means by one, which is the same number on both
-- targets for ASCII content and deliberately not for anything else.
@platform(native) pub fn findAscii(self: String, unit: CodeUnit) -> Maybe(Size) = indexOf(self, unit)

@platform(js) pub fn findAscii(self: String, unit: CodeUnit) -> Maybe(Size):
    let at = hostIndexOf(self, hostFromCharCode(unit :: Int), 0)
    if at < 0 then return Nothing
    return Just(at)

-- Whether it is there at all, which is the search with its answer thrown away - one function on both
-- targets, because the thing that differs is what `findAscii` is.
pub fn containsAscii(self: String, unit: CodeUnit) -> Bool = findAscii(self, unit) is Just(_)

-- How many there are. Natively one vector pass with a lane-wise compare and a mask count; on JS the
-- host search stepped along, which is the same C++ scan restarted at each hit.
@platform(native) pub fn countAscii(self: String, unit: CodeUnit) -> Int = occurrences(self, unit)

@platform(js) pub fn countAscii(self: String, unit: CodeUnit) -> Int:
    let needle = hostFromCharCode(unit :: Int)
    let &total = 0
    let &from = 0 :: Size

    while from >= 0:
        let at = hostIndexOf(self, needle, from)
        if at < 0 then return total

        total = total + 1
        from = at + 1

    return total
)TEXT";

void defineText(Program& program) {
    auto& context = program.context;

    auto name = context.addQualifiedName("Text", 4, 1);
    Lexer lexer(context, context.diagnostics, StringView { kTextSource, stringLength(kTextSource) }, name);
    Parser parser(context, lexer, name);
    auto ast = new ast::Module(parser.parseModule());

    auto module = program.addModule(ast->name, *ast->region);
    program.embeddedAsts.push(ast);

    resolveModuleDecls(*module, *ast, nullptr);
    program.text = module;

    /*
     * The three functions a format expression is built out of - Implementation-Storage.md part 8.
     *
     * Recorded for the reason `allocateHeap` is: `"a{x}b"` is resolved by the compiler, which has a
     * chunk list and a set of resolved holes and no call site for name resolution to start from.
     * Everything else about a format is an ordinary call to an ordinary function.
     */
    auto findText = [&](const char* text, Size length) -> ModulePtr<Function> {
        auto found = module->functions.get(context.addUnqualifiedName(text, length));
        return found ? found.unwrap() : nullptr;
    };

    program.newString = findText("newStringOfCapacity", 19);
    program.pushString = findText("pushString", 10);
    program.formatBound = findText("formatBound", 11);

    resolveModuleBodies(*module);
}

