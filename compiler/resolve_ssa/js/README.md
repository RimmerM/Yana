# JSIR - the JavaScript half of the fork

This is `resolve_ssa/lower`'s sibling. Everything in `resolve_ssa` above these two directories is a
statement about the *program*; everything in `lower` is a statement about one machine, and everything
here is a statement about one host. Both are built against the same optimized SSA and both answer the
same two questions - what a value is, and how an occurrence of one is carried - with `JsRep` where
the other has `Repr`.

The design is [`doc/inflight/Analysis-IR-JS.md`](../../../doc/inflight/Analysis-IR-JS.md). Parts 3,
4, 5 and 8 are what this directory implements; part 9's milestone 2 is the slice that built it and
milestone 4 is the one that made the rest of the non-vector corpus run
(`doc/progress/2026-09-21-J66-jsir-breadth.md`, then
`doc/progress/2026-09-21-J67-jsir-coverage.md`). **The library's own test suite compiles, runs and
passes through this directory** since 2026-09-21 (J69) - all 342 cases of `test/base`, which was
the count the old JavaScript backend (deleted with `compiler/resolve` in J173) passed - **optimized
and unoptimized** since J70, so `js` and `js-unoptimized` are both rows of `test/run-base.sh`, and
with the JSIR optimizer switched off since J72, which is a third row, `js-no-jsir-opt`.

## What each file owns

| file | what it decides |
| --- | --- |
| `module.h` + `inst.def` | the IR: representations, shapes, ABIs, values, places, instructions, blocks, bodies. `inst.def` is the only place an opcode is written down - the enum, the concrete-type cast, the mnemonic, the result form and the effects are generated from it |
| `repr.h/.cpp` | `jsRep(logical type)` and `jsManifest(occurrence)` - §3.2's two layers, and the shape table under them |
| `abi.h/.cpp` | one frozen `JsAbi` per emitted declaration, computed before any body |
| `name.h/.cpp` | what an identifier may be on this target - sanitizing, the reserved words, interning - and `jsir::key`, the compiler-owned property spellings (`$tag`, `$v`, `$c`, `$e`, `$o`, `$k`, ...) that `codegen/js` reads too |
| `build.h` | the typed builders. There is no universal `useValue`: a function's return type says which form came back |
| `convert_internal.h` | `Conversion` - the state the five conversion files share, and the three SSA predicates in front of it. Nothing outside this directory includes it |
| `convert.cpp` | the demand pass, places, and the instruction switch, the terminators, the edges, aggregates and calls |
| `constant.cpp` | `runConstant` and `constantLiteral` - §2.4's one constant lowering, which `codegen/js/assemble.cpp` asks for and spells |
| `teardown.cpp` | the two teardown *bodies* this tier synthesizes - a lambda environment's, and one per (type, halves) |
| `host.cpp` | the host rows (`resolve_mir/host.def`), the three vector transfers, and `.length`'s own range |
| `vector.cpp` | every operation over a multi-part value - asked **before** the scalar switch, on the representation rather than on the type |
| `cfg.h/.cpp` | `JsCfg` - reverse postorder, immediate dominators, loop membership and reachability, computed **once** per body and read by the verifier, the optimizer and the AST tier alike |
| `verify.cpp` | construction, finished-CFG and representation verification - on in every build |
| `opt.h`, `opt_internal.h` + `opt_*.cpp` | the JSIR optimizer, one `Optimizer` split by the question each pass answers (J93): `opt_analysis` the indexes (uses, dominance, positions, variable touches) and the editing helpers, `opt_alias` §6.3's copy-to-alias elision and the literal forwarding beside it, `opt_move` a copy at the last use, `opt_number` value numbering, constant folding (J136) and decided branches, `opt_objects` where an object is built and whether it needs to be one, `opt_variables` scalar replacement and dead stores, `opt_driver` the order. Switchable off whole with `-no-jsir-opt` |
| `opt_coalesce.h/.cpp` | §6.8's block-argument coalescing - *which* arguments may share a variable. What one is called is still `codegen/js/structure.cpp`'s |
| `print.cpp` | `-mode jsir` - the bodies, then the representation and shape declarations, which are only complete once every body has asked for one |

`codegen/js/structure.cpp` turns a verified body into statements and `codegen/js/assemble.cpp` is the
file they go into. Those are the AST's side of §2.3's boundary and are not here.

**The five conversion files are one pass split by subject**, 2026-09-22 (J80), and `convert.cpp` was
5,414 lines before it. The struct stays one object rather than becoming several passes: what the
split is of is the *text*, and the state - the builder, the per-function scratch, the SSA body being
read - is one thing the whole conversion shares and would only be threaded back through parameters.
§8.1's `demand.cpp` and `place.cpp` are not among them yet, and the reason is mechanical rather than
a decision: both are in-class definitions on `Conversion`, and the `FieldAddress` and `Represent`
arms `place.cpp` is meant to hold are arms of `convertInst`'s switch rather than methods at all.

## The invariants

**One constant lowering, and it is this directory's.** A global's initializer is the one value on
this target that no body builds, and `codegen/js/assemble.cpp` used to write it with a second walk
over `mir::` types. `jsir::convertConstant` builds a throwaway body instead and `structureConstant`
spells it, so a record a global builds and a record a body builds go through the same arms - see
J79 for the two ways the second walk disagreed, one of which was a `Maybe(I64)` global that threw
under Node.

**The compiler's own property keys are one table and it is this directory's.** `$tag` is a sum's
discriminant, `$v` the one property of a box, `$c`/`$e` a function value's two words, `$o`/`$k` a
reference's. They were spelled in four places across two JavaScript backends until 2026-09-22 (J80),
and what makes them a representation fact rather than a spelling is that a `JsShape` records the
final key: two spellings of one key are two hidden classes for one record, and a value built by one
piece of code and read by another answers `undefined` with no diagnostic anywhere. `jsir::key` in
`name.h` is the table and `codegen/js/shared.h` includes it.

**Every value has a `JsRepId`, and no lowering decision reads a `mir::Type`.** `JsRep::source` exists
for dumps and for the sentence a diagnostic names a construct with, and for nothing else. Several
logical types share one representation - `Int`, `Size` and a `@bits(30)` refinement are all one
`Number` - and one value may have a representation no single type node describes.

**Values and places are separate ID spaces.** Only `Load` and `Store` accept a place. A phi can never
denote a slot on one edge and its contents on another.

**An SSA value whose convention is `Address`, or whose occurrence is a loan that denotes storage,
becomes a place; everything else becomes a value.** `loanDenotesStorage` is §1.3.1's predicate -
`readMode`'s own decision procedure, with no layout in it. That one discriminator is what replaced
the old JavaScript backend's scattered "is this a reference or a box" questions.

J65 wrote it out here, in `convert.cpp`, rather than reaching for `lower/manifest.h` - "the first
consumer of that restatement", since the shared tier was still asking the native table the same
question through a `Layout`. J75 closed that (§12.3's first open question), so the predicate now
lives in `../build.h` as `ssa::loanDenotesStorage` and `loanDenotesStorageAt` here is the shim that
names it. The window row below is J71's and went up with it, unchanged: **one** definition, read by
both targets, which is what the restatement was for.

**Nothing falls through to `null`.** Every arm of every switch either converts or reports by name:
"JSIR does not convert X yet", at the offending instruction. A sweep over the corpus is then a
worklist rather than a list of fixtures that answer the wrong number.

**An instruction's operands are computed before the instruction exists.** Reading a place may emit a
`load`, and one emitted after the node that uses it - or after a terminator - is an instruction in a
block that has already ended. Every terminator here collects first and attaches second.

**One reusable arena, released after each function.** The stable tables live for the compilation;
`JsBody` does not survive the loop that emits it, which is why `-mode jsir` writes its dump inside
that loop rather than afterwards. `JsTarget::beginBody` is the one call that begins a body - the
arena back to empty and the cached `JsCfg` thrown away - because the arena is not the only thing
that stops describing the last one.

**The per-function side tables are the target's, not the pass's.** `JsConvertScratch`,
`JsOptScratch`, `JsCoalesceScratch` and `JsCfg` are §7.4's dense scratch, reset per function on
storage the widest body so far already grew. Each of the passes that reads one is constructed *per
body*, so a list declared as a member of one is an allocation per function per list - which is what
they were, and which J79 measured at a third of this tier's whole allocation count.

## The representation vocabulary, as built

`Void`, `Number{Bool01 | Signed(bits) | Unsigned(bits) | Float32 | Float64}`, `HostBool`, `BigInt`,
`HostString`, `GcObject{shape}`, `HostArray{element, typed-array kind, length}`,
`Parts{parts, materialized?}`, `Cell{content}` and `Erased`.

There is no packed kind. A record packed whole is a plain `Number{Unsigned, span}` and a `$pN` word
is a property of that representation (`JsPropertyRole::StorageUnit`); what says which bits are which
field is the `JsPacking` row the record names, read by the one place that decodes it - a
`JsPlaceKind::BitRange`. On by default since J85; see "Packed places" below.

Seven of them are decisions rather than transcriptions of the plan:

- **`Parts` is N values and never one.** A logical value whose manifestation is `Parts` is N JSIR
  values of the part representations; no JSIR value ever *has* a `Parts` representation, and the
  verifier says so. `materialize` and `split` are the only two nodes that name one, and
  `jsStoredRep` is the rule that keeps it out of every position holding a single host value - a
  place's content, a shape property, a host array's element, a global, a returned value. §3.6 asked
  for the conversions to be explicit; this is what makes "explicit" checkable rather than a habit.
- **`HostBool` exists because a mask lane is cheapest as one.** `Bool` is the number 0 or 1 here and
  a mask lane is the host boolean, for the reason LLVM's `<N x i1>` is not `<N x i8>`: a mask lane is
  produced by a comparison, combined by `and`/`or`/`not` and consumed by a select or a reduction, and
  a boolean is what all four are cheapest over. It is reachable from no logical type.

- **`HostString` is the whole of a `String`** in every contract but unique mutable (§3.2.2). There is
  no view record on this target - `known.def`'s `stringView` row is unclaimed - so a shared string is
  the host string.
- **A transparent record is its one stored field.** One stored field and the rest `@host`, which is
  what makes `Array(Handle)` the bare host row and `Array(Int)` the `{items, length}` object: the
  `@host` elision is live exactly where the value already has that property (§14's two rows). A
  **lambda environment is the exception** and keeps its object, because what a closure and the frame
  that built it share is the record's *identity* - see `JsTarget::environmentRecords`.
- **A `Cell` is a `{$o, $k}` descriptor**, not a `{$v}` box. The box is the *storage* an escaping
  scalar local gets and the descriptor names a property of it, so one reference form serves a local,
  a field and an element alike. Since J71 the object exists only where one value is required: a
  reference crosses a call and an entry block as its two halves - see "Windows and references".
- **A shape property is the field's *occurrence*, not its type** - `fieldRepOf`, which is
  `carriedRepOf`'s counterpart at a call and is the same `needsCell`. A `&` field holds the
  descriptor; reading the type gave it the pointee's representation and a write through a captured
  mutable local landed in a copy.
- **An erased function value is one `GcObject` of the `{$c, $e}` shape**, and `Type::Iter` is the
  same pair (§3.2.1). `Parts` is where §3.2 puts it and is milestone 5's; the note above `jsFunRep`
  is where that trade is argued.

## The niche fold - `Maybe(T)` is `T | null`

`JsRepKind::Nullable` is produced since 2026-09-22 (J74) and is the one representation here that
removes an **allocation** rather than text: `{ $tag: ... }` literals in `Base.js` went 1,243 to 206
against the old JavaScript backend's 166, and `Just:` disappeared from the file entirely.

The rule is on the **logical type** and not on the name `Maybe` - `nicheFoldOf` in `repr.cpp`. Two
constructors, one carrying nothing and the other carrying exactly one thing whose representation
cannot already be `null`; `Nothing` is `null` and `Just(x)` is `x`. `Result(E, T)` has two payloads
and no second pattern to tell them apart by, so it declines here exactly as the native niche rule
does, and a `@layout(js)` pin declines because that is the declaration saying what
the host sees. **Nesting is refused** - a `Maybe(Maybe(T))` keeps its `$tag` over a folded inner one -
which is §3.2's rule and `jsAdmitsAbsent`'s single statement of it, read by `jsRep` to decide a fold
and by `verify.cpp` to refuse one taken anyway.

**A number payload folds**, and that was measured rather than read off the old backend: with
number payloads left tagged the suite runs in **2.420 s** against **2.058 s** with them folded,
interleaved best of nine. Folding only the object payloads is *slower than folding nothing*, which
nothing predicted - a `Maybe(Int)` left as an object beside a folded `Maybe(Person)` makes the sites
that handle both polymorphic.

Three operations and no fourth, which is what makes a later encoding - a packed tag, a second niche -
a change to three functions rather than to every site a sum is reached from:

- **the tag read** is `v === null ? <absent> : <present>`, at `HostBool` so that the comparison the
  consumer writes folds against it (`foldTernaryCompare` in `codegen/js/opt.cpp`, which already
  existed for the old backend's own folded tags). The projection of
  `InstFieldAddress::kDiscriminant` answers that *value* rather than a place, which is the state
  `placeOf` already knows - and it is read when the projection is converted, so it is one of the
  projections J91 converts again at a use (below);
- **the payload place** is the base's place read at the payload's representation -
  `JsBuilder::addNichePayload`, a copy of the place with `JsPlace::nichePayload` set, which is the
  one licence `checkPlace` grants against a shape's property representation. `Downcast` is the
  identity;
- **the construction** is an `Alias` of the payload or a `null` - `convertAggregate` and
  `aggregateConstant`, so a constant `Just(4)` is the number four relabelled.

A `match`, an `is`, a `?`, a refutable `let` and a `Select` over constructors all arrive at those
three. A **mutable borrow** of a folded option is `Cell(Nullable(T))` even when `T` is an object, and
that falls out rather than being written: a `Nullable` is not a `GcObject`, so `jsManifest`'s
existing `needsCell` rule answers it. The **teardown** is one guarded arm of the per-type function a
sum already had, so an erased position holding a folded value releases it through the descriptor's
own cell with no erased body knowing anything about the encoding.

What is refused by name: a discriminant *written* into a folded value from something that is not a
literal, which nothing in `lib/` or the corpus does.

## Scalarized records - a small `data` as its fields

Since 2026-09-22 (J74), and it is the window's arrangement (`Parts`) asked of an ordinary `data`:
three or four fields, every one of them a primitive, one constructor, no `@layout` pin, not a lambda
environment, and nothing to release or to run at a copy. `jsStoredRep` is what keeps the object at
every position holding one host value, so a field, a stored local and an erased argument are
unchanged and what becomes flat is an argument, a result, an edge and the aggregate that would
otherwise allocate.

Three of those conditions have a program behind them rather than an argument:

| condition removed | what happens |
| --- | --- |
| a part may hold an object | `resolve/ErasedCopy.yana` answers **25** where the unoptimized build answers -7 |
| nothing to release | `resolve/ScalarRelease.yana` answers **36003** against 36000 - a leak, because parts are not storage and the teardown walk finds nothing to walk |
| the materialization is memoized per value | the verifier: "an operand whose definition does not dominate its use" - a `return` in two arms materializes in whichever block asked first |

The last of those is `JsRep::freshEachUse`, and the memo is kept **per block** instead - sound
because a block is straight-line, and worth keeping because without any memo a record read twice in
one statement is two allocations where the tagged form was one, which measured as a 4% regression.

The band is measured and both ends of it are: a **two**-field record costs more flat than as an
object, which nothing predicted. `kScalarizeLeast` is where the numbers are.

## Windows and references - the flat values

Two families are `Parts` since 2026-09-22 (J71), and neither is lanes of one value.

**An array window is its three fields.** A shared or unique `[a]` in a binding position is the
library's `ArrayView(a)` record - `{items, length, offset}` - and one was built at *every* borrow of
an array: 981 of them in `Base.js` against the old JavaScript backend's 137, which J69 §6 measured
as the largest single line of the size and time gap. The **record's own representation** is the
`Parts`, not the occurrence's, which is the half that took a measurement to see: `slice` builds an
owned `ArrayView` value and a `represent` relabels it as the window, so a rule keyed on
`AccessForm::View` alone would have built the object at the aggregate and split it one instruction
later. `jsStoredRep` is the object again, so a *stored* window - a field, a local the demand pass
kept, an erased argument -
is exactly what it was.

Two rules follow and both are `manifestNative`'s, read here rather than invented:

- **a window is a value whatever the access says.** The native view row sits *above* the
  reference-value row and hands the record back at `mir::owned(view)`: what `&xs: [Int]` writes
  through is the elements, and the descriptor itself is never rebound - `sliceMut` returns a fresh
  one. So `jsManifest` returns before `needsCell`, and `loanDenotesStorage` answers **false** for a
  view occurrence before it asks about the access or the origin. Answering yes kept every window one
  value, which is the form this removes. Since J75 that row is in the shared predicate, so it is the
  native conversion's answer too - and the native tier's own "is this reached as an address" question
  is `denotesStorage`, which keeps the view row on the other side because a *descriptor* does live in
  storage there;
- **the one position a window still materializes** is a `represent` from a window value to a
  `%Array(a)` *address* - a reborrow of a window taken as storage, which `joinWith`'s
  `copy(separator)` and `Test.runMain`'s `cases` both reach. The parts have no address, so `placeOf`
  builds the view object once and names a `Value` place over it, which is the place the window had
  before it was flat. The arm asks the **representation** - `partsRepOf(operand).valid()` - and not
  whether the parts table happens to hold the operand, which is J78's correction. A window is in
  that table *usually* and is one host value whenever something upstream already asked for one, and
  with the table as the test the node fell past both arms into the scalar `Represent`, which
  relabelled the view object as the `%Array(a)` row: the place built over it then held a bare host
  array and the `split` that read the window back out of it is what the verifier reported. It was
  reachable only once an inlining decision left the call below it out of line, which is why it
  survived every sweep until one did.

**A reference crosses flat** - `(owner, key)`, §3.2's narrow-reference row; J84 added the scale
that makes it reach a field packed into a word. A descriptor is already a pair of host values, so the object around
it buys nothing at a position that takes two arguments and costs one allocation at every mutable
borrow crossing a call: 867 in `Base.js` against 223, and 819 of those were `{$o: box, $k: "$v"}` - a
reference to a whole boxed local. It also takes a level off every read *inside* the callee, which is
an `Element` place over the two halves: `owner[key]` where it was `x.$o[x.$k]`. This was the old
JavaScript backend's own arrangement, whose `showWith(v699, ..., v11, "$v", 1)` was its flattened
`{$o, $k, $s}`.

**An erased position keeps the object**, which is J70's rule read one step on: a body compiled once
reads a `Cell` argument as `x.$o[x.$k]` whatever is in it, and flattening there would need the erased
body's parameter list to change with its instantiation. `JsPassing::erased` is read off the
declaration, so the two ends agree without either looking at the other.

The parts of a flat reference are both **`Erased`**, because an owner is any host object and a key is
a string or a number. So the per-part representation check waives them - an `Erased` part accepts
anything, the same waiver an `Erased` shape property gets - and the **arity** is what stands over the
family. `JsirTester::testFlatReferenceIsTwoPositions` is that as a mutation, and
`emitThunks` was the one place it was already wrong: a thunk written per *parameter* rather than per
*position* dropped every argument after the first of a multi-part one, which `NarrowRef.yana`'s
`apply(raise, panel.trail)` answered wrongly under `-no-ir-opt` and which a vector through a function
value would have too.

`JsRep::lanewise` is the bit that tells the two `Parts` producers apart, and it exists because
`convertVectorInst` runs in front of the scalar switch: a `represent` between two window occurrences
is not a lane-wise conversion of a host array and two numbers. Every ask inside that switch goes
through `lanesRepOf` rather than `partsRepOf`.

## Vectors and masks - the first `Parts` consumer

`Vec(a, n)` is `n` values of the lane's representation and `Mask(a, n)` is `n` `HostBool`s
(Implementation-Vector.md §7, which makes this the representation at *any* lane count rather than an
optimization over a boxed one). What that buys is that **no operation over a vector builds
anything**: an add is `lanes` additions over `lanes` variables, a splat is one value named `lanes`
times, and a lane read, a lane write and a shuffle emit no instruction at all.

Three things about it are worth knowing before reading `convertVectorInst`.

**The discriminator is the representation, not the type.** The old JavaScript backend asked
`isVectorType` before its scalar switch; here the question is whether the value is multi-part, which
is the stronger form of the same test and the one this tier is allowed to make. A comparison is the
one operation asked of its *operands*, because its result is the mask. `vectorLanes`,
`VectorType::isMask` and `isVectorType` appear nowhere below `repr.cpp`.

**A lane is an ordinary value, so the arithmetic is the scalar arithmetic.** `structure.cpp` spells a
lane's multiply as `Math.imul` and its division as `$div` by reading the result's representation,
exactly as it does for a scalar - so none of the thirty spellings the old backend's `laneBinary`
restated is restated here. What is left in `laneBinary` is the four things a lane genuinely differs
in: the mask's bitwise operators are the logical ones, the two `Core.Bits` families are declared at
widths the 32-bit helpers do not have and are corrected, the saturating pair has to be exact before
it clamps, and a reduction is a tree.

**The materialized form is a plain host row** - not a typed array, since J136: a typed row defeated
V8's escape analysis (see the `Vector` arm of `jsRep`) - and it exists only for
the positions one value is the only thing that fits: a field, an element, a `return`, an erased
argument. A **block argument is deliberately not one of them**, which closed the old JavaScript
backend's known gap 10 by construction - a vector carried around a loop is `lanes` block arguments
and `lanes` edge operands, so an accumulator in a vector loop allocates nothing per iteration.
`resolve/VecValue.yana`'s `.jsir.js.expect` is the pin: it assigns four variables, where the old
backend's `.js.expect` rebuilt an array each time round.

The refusals are the old backend's, by name: a vector of a 33-to-53-bit lane has no JavaScript
form, and a bitcast between vectors of different lane widths has none either - a lane is a number
here rather than a run of bits.

## The erased ABI - one host row, argument zero

A body compiled once cannot read a layout out of its types, because its types are parameters. What it
reads instead is an **environment**: one cell per slot of the caller's `mir::GenSchema`, in that
numbering. `lower/erase.cpp` is the native half and answers the same questions; what is different
here is that almost every number in that file is gone - a cell is an element of a host row, so a slot
number is already an index and there is no anchor, no four-byte relative address and no packed
metrics word.

```js
function twiceMark($g, x) { return $g[1][0](x) + $g[1][0](x) | 0; }

var $d$I32 = [1, 0, $nd, 0];            // a type descriptor
function $nd(x) {}                      // what a type that owes nothing points at
var $w$Marked = [I32_mark];             // a class witness
var $ge$twiceMark = [$d$I32, $w$Marked];
```

- **A descriptor is four cells** - `[trivialCopy, hasTeardown, teardown, zero]`, `JsDescriptorCell` -
  and three of the native five are gone: there are no bytes to move here, so the three measurements
  have no answer and the two facts about *ownership* are what a body still asks. The teardown cell is
  never null, which is the native rule read for this target's reason: a test and a branch in the
  middle of a block the caller is filling is a CFG change.
- **A class witness is a row of functions**, in `mir::classWitnessCount`'s order with the
  superclasses flattened in after them. A **const** slot is the number, at the representation its
  parameter counts in. A **property** witness is the field's own **name** since J70, and the read
  through it is `owner[key]` - an `Element` place whose index happens to be a string. The three
  shapes `lower/erase.cpp` gives the native slot (a byte offset, an offset with a bit shift, a
  `{read, set}` pair) are all answers to "how wide is it and where does it start", and a host
  object's property has neither; what it costs instead is that a subject with no host object of its
  own - a transparent record, a host row - has no name to hand over and is reported by name. The
  content is the requirement's **declared result type**, which is the same rule the native slot
  states: `a.size: I32` is a promise the signature made, so both ends know the representation before
  either has seen the other.
- **An erased `Cell` position wants a real descriptor.** The waiver `JsPassing::erased` grants is
  about a position's *content* and not about its convention: a body compiled once reads one as
  `x.$o[x.$k]` whatever is in it, so the host object a `Value` place answers when it is asked for its
  own reference is a descriptor that indexes into nothing. The demand pass therefore asks a second
  question - `useWantsDescriptor`, whether an address reaches such a position - and a `GcObject`
  local that does gets a box rather than a `Value` place. Every other reader of it is unchanged,
  since a `One` position loads `box.$v` and gets the same object back.
- **The environment is a module-level table** rather than a row built at the site: every cell is a
  compile-time constant, because one is built only where the argument list at the call is concrete.
  `JsInstGenTable` names it and `assemble.cpp` fills it.
- **There are no argument thunks.** Natively a concrete caller adapts at every erased position - a
  pointer where it holds a register - and `thunkFor` undoes it. Here a host value of any type is one
  value, so the two ends agree at every position they spell the same way; `witnessSlotsAgree` is that
  as a *question*, asked before a cell is filled, and a disagreement is a report rather than a cell
  that drops or duplicates an argument.
- **`JsPassing::erased`** is what the call-site representation check waives, read off the
  declaration (`lower/erase.cpp`'s `erasedPosition`). It replaces a waiver keyed on what a
  representation turned out to be, which let two genuinely different concrete reps past whenever one
  of them was a type variable's.

## What is deliberately not here

- **`lower/`.** This directory may include `resolve_ssa` and `resolve_mir` and may not include the
  native manifestation table, which is not a fallback for a question this one has not answered yet.
  Nothing in `resolve_ssa` outside this directory includes `js/`.
- **One layout table for both targets.** `compiler/repr`, deleted with `compiler/resolve` in J173,
  computed both targets from one struct - §3.2 quotes `lower/repr.h` on why that is two readings of
  one type. JSIR must not be what brings it back.
- **`exchange` and `swap`.** §3.5 keeps them as place operations until one late legalizer, and SSA
  has neither instruction: a swap is a pair of loads and a pair of stores by the time it reaches this
  tier. What sits between them is an `addressOf`, which `resolve_ssa/convert.cpp` writes as a
  `Bitcast` of a value that arrived as an address - and on this target such a value **is a place**,
  so the node is the identity on it. Reading it as a value instead is what made every `swap` and
  `exchange` in `lib/` a bitcast between a host reference and a number. The day a legalizer arrives
  it is §5.5's and it goes in `build.h`.
- **A second ownership model.** JSIR consumes verified ownership and does not reinterpret it. A
  derived reclaim emits nothing; an authored teardown is an ordinary call in OSSA order. The three
  releases that are *not* one call at the site say why beside themselves: a **function value** is one
  shared helper reading the header off the code word, and nothing at all where no lambda in the
  program owes an authored teardown (`JsTarget::closureOwesTeardown`); a **sum** is one `TypeDrop`
  naming a per-(type, halves) function, because its walk branches on the tag and a branch is a CFG
  change in the middle of a block the caller is filling; and a **fixed array** is unrolled, for the
  same reason read the other way.
- **Most of the optimization.** §6's cell promotion, materialization sinking and range propagation
  are still milestone 6's. What the conversion does is build useful form directly (§5.4); the one
  pass that exists is `opt_alias.cpp`, below, and the rest is still the AST peepholes that were
  always there.
- **The *naming* of a coalesced block argument**, which is `codegen/js/structure.cpp`'s. A block
  argument is an argument right up to `emitEdge`, which is the one place it becomes an assignment,
  so what a shared variable is *called* belongs where the variables are named. The **analysis** -
  which arguments may share one at all - is `opt_coalesce.cpp`'s since 2026-09-22 (J79) and was up
  there too until then, which put two hundred lines of liveness dataflow in a file whose rule is
  that it switches on nothing but a `JsRep`, a `JsShape` and a `JsPlace`. It needs liveness over the
  JSIR CFG and nothing else about JSIR, and the CFG is unchanged across the boundary - which is what
  `JsCfg` being computed once and read on both sides of it now says out loud.

## The optimizer - one pass, many rewrites

`opt_alias.cpp` since 2026-09-22 (J72), run between the conversion and the AST builder and
switchable off whole with **`-no-jsir-opt`** - Analysis-IR-JS.md §10.2's third configuration, which
`test/run-base.sh`'s `js-no-jsir-opt` row and every `.jsir.expect` fixture's third Node run
now exercise. The verifier runs on what the pass *returns* in every build and, in an assertion
build, on what it was given as well, so a body this pass broke is named by the run before it.

**What it removes is the structural copy at a borrow.** `readMode`'s first clause turns a shared,
call-bounded loan of a trivially copyable value into a `copy` (§1.3.1's category 2). Natively that is
a few words on the stack and `promote` takes it back; here `cloneOf` writes the object out property
by property and **recursively**, so a two-field record of two records is eighteen property reads at
every borrow. 2,954 of them in `Base.js`, 262 KB of object literals against the old JavaScript
backend's 5.5 KB - a quarter of the whole size gap, and that backend never copied at a borrow at
all: its `keptValue` duplicated only at a *store* or a `return` whose source still named live
storage.

The elision fires when four conditions hold, and the second is what makes the first one cheap:

1. **the copy is never written and never kept** - every use of it, through `Alias`, is a property
   read, another clone, or an argument at a position the callee's frozen `JsAbi` marks `shared`;
2. **the source names storage nothing outside this body can reach** - a `Var` slot, which is exactly
   the demand pass's statement that the address never escaped, or a parameter at a `shared` position,
   which is the caller's loan and holds for the length of the call, or an allocation made here;
3. **the object in that storage is never written** - the same read-only walk over the source, over
   every other value read out of the same slot, and over every value stored into it;
4. **what a `Var` holds was built here**, so the object is not one somebody else also names.

Condition 2 is why condition 1's call case does not have to look at the call's *other* arguments.
`f(p, &p)` is legal for a trivially copyable `p` precisely because the copy holds no loan - and it is
also the program in which `&p` makes the local's address escape, so the demand pass gives `p` a
`Value` place rather than a `Var` and condition 2 refuses before the call is reached.
`test/resolve/AliasWrite.yana` is that program.

**The one hole this had was a property read.** `load property o.f` does not write `o`, so it reads
as a read - but where `f` holds an object, what comes back is a second name for *that* object and a
write through it lands inside `o`. `AggregateAlias.yana`'s second case caught it as a wrong answer
under Node within one regenerate: a `[Int *4]` in a record field, written through the copy and read
back through the source. The walk now recurses into a loaded value whose content representation is
not a primitive, which costs 80 bytes over `Base.js` and is the reason `cloneOf` recurses at all.

**Two rewrites beside it, and they are what make it pay.** A clone whose result goes into an object
literal is refused by condition 1, and that literal is very often one the next instruction takes
apart again - `Failure {site, claim, detail}` built to hand its three fields to `recordAt`. So
`forwardFields` answers a property read from the literal that built it where the literal never
escapes and is never written, `dropDead` then removes the literal, and the clones that were kept in
it become clones of clones the second round aliases. Neither is general CSE nor general store
forwarding; both are §6.6's first and fifth bullets at the shape the measurement named. Aliased went
from 703 to **2,487** when those two landed and the alias chain was followed to its root.

Every rewrite here replaces one instruction with an **`Alias` of a value already in scope**, which is
why none of them rewrites an operand list: the result value keeps its identity and its users never
learn anything changed. `Alias` emits nothing at all in `structure.cpp`.

**A projection of storage that is not the object is converted again at a use** that follows a write
or a call (J91, `reprojectIfStale` in `convert_internal.h`). A field or element address of a
variable, a property or a cell *loads* the object out of that storage where the projection is
converted. Natively an address is a pure function of its base, so the shared optimizer computes one
once and reuses it across a whole store to the base - `acc.count` before `acc = {acc | ...}` and
after it - and here that store rebinds the storage to a new object, which the old load does not see.
So the row remembers where the owner was loaded (`JsTargetValue::projectedIn`), and a use in another
block, or after anything in the same block that writes or calls, converts the projection again. The
alternative was to refuse the reuse in the shared optimizer, which is a native cost for a JavaScript
representation. `Base.js` moved by a few hundred bytes.

**Two use counts, and they answer different questions.** `JsValue::uses` is the construction count
`codegen/js/opt.cpp` reads to decide whether an expression may be written at its single use, and it
is an over-count by design - a place built by `placeOf` and never read counted its owner, and a
`Value` place's load counted the object twice. The pass maintains it exactly, which means a place's
owner is discounted with the *last* instruction naming that place rather than with each of them,
because `convert.cpp` caches one `JsPlace` per SSA value. Dead code is decided on the pass's own
index instead: an over-count there is not conservative but fatal, since an object literal whose three
property reads have all been forwarded would never reach zero and would survive with nothing reading
it.

**The fourth rewrite is `adoptInitialStores`** (2026-09-22, J73), §6.6's fifth bullet at a second
shape. A local whose address escapes becomes a `Value` place - the storage *is* the host object - so
the conversion has to build one before anything can name it, and where the first thing that happens
to it is a whole-value store that object is written property by property and thrown away.
`md5Answers` in `test/base` is the shape, five times over: two allocations and two sixty-four
element literals where one of each is owed. The place is taken from the value that was stored - the
allocation goes and the store becomes `zero = alias stored`, in the store's own position - on three
conditions: **every use of the zero comes after the store** (checked as "in this block, ahead of it,
with nothing naming it in between", which is enough because a block is straight-line), **what is
stored is a fresh allocation this store is the only use of**, and the representations agree. 133
sites over `Base.js`, and the field-wise copy count went 679 to 372 against the old JavaScript
backend's 375. The first attempt asked instead that every use of the zero be a *place* rooted at it
and fired ten times: the uses it refused are the ones that make a `Value` place what it is, since a
call taking the local as `&self` takes the object itself.

Its two conditions are **mutation tests** rather than fixtures, for the reason `JsPassing::shared` is
one: the conversion emits the zero and the store together, so no `.yana` program varies either
without varying everything else. `JsirTester::testAdoptionNeedsTheStoreFirst` and
`testAdoptionNeedsTheStoredValueUnshared` are them.

**Condition 2's `Value` arm is unreachable**, which J73 found while closing J72 §7's first zero row:
a whole-value read of a `Value` place is the owner value itself and not a `Load`, so `sourceIsStable`
never reaches the kind test with one. What that test is actually asked about is a `Property`, an
`Element`, a `Cell` and a `Global`, and `test/resolve/AliasFieldRoot.yana` is the fixture for them -
`both(h.p, h)` answers 102 rather than 3 when a `Property` root is accepted as stable. Condition 4 is
still distinguished by nothing: every route the language has into a `Var` slot puts a fresh object
there, so it is insurance against a conversion that does not exist rather than a rule the corpus
needs.

**The fifth rewrite is `sinkAllocations`** (2026-09-22, J77), §6.5's first and third bullets, and it
is the one rewrite here that removes allocations a program *makes* rather than text it spells.

A local whose address escapes is a `Value` place, so the storage *is* the host object and the
conversion builds one where the frame slot is declared - the entry block. Every path that leaves
before the local is touched pays for it, and in the library's own Unicode cases that is most paths:

```js
function normalizeForm(text, compose, compatible) {   // before
  var v8 = { $v: "" };
  var v11 = { items: new Uint32Array([]), length: 0 };
  var s0 = { f0: 0, f1: 0 };
  var v35 = { items: new Uint32Array([]), length: 0 };
  ...
  if (stablePrefix(text, compose, compatible) === text.length) return text;
```

`normalizeForm` allocated **1,581 MB** over `test/base` against the old JavaScript backend's
**67 MB**, and that backend's difference was not an optimization: it wrote the same objects *inside*
the `else`. The whole program allocated 12.5 GB against 11.1 GB.

An `Object`, an `Array` or a `Materialize` moves to the lowest common ancestor of its uses in the
dominator tree. Three conditions, and each has a reason rather than a range:

- **an allocation is unobservable where nobody names it**, so moving one changes what runs only by
  not running it. `Clone` is deliberately not in the set - what it copies is whatever its source
  holds at the moment it runs;
- **it may not land in a loop the definition was not in** (`loopMask`), because the object is then
  rebuilt per iteration and a local's storage is re-zeroed with it. `test/resolve/SinkLoop.yana` is
  the program: an accumulator used only inside its loop answers **77**, and **45** with the rule
  removed;
- **only across a block boundary.** Moving one down inside its own block skips nothing - a block is
  straight-line - and it separates the allocation from operands nothing else reads, which
  `codegen/js/opt.cpp` then has to name. `VecMove.yana` measures that at +592 bytes at three
  goldens for no movement in the time.

`Alias` is sunk with them and is not an allocation: the conversion writes one at every local whose
storage is the object, in the block the local was declared in, and that alias is the object's only
use. Candidates are taken **backwards** so a producer follows the consumer that has already moved.

The CFG analysis it needs - reverse postorder, immediate dominators and one bit per natural loop -
is computed once per body and only for a body that has a candidate. An **irreducible** graph is
refused rather than approximated: a retreating edge whose target does not dominate its source names
a cycle no natural loop describes, and `test/base` contains no such body.

**The sixth and seventh are the live-range half of §6.3** (2026-09-22, J82), J72 §9's fifth item.
The four conditions above are asked of the whole body, so a copy whose source was written *before*
it, or whose copy is returned, survived even where nothing named the source again - `return p` from
a `let &p` was `return { a: s0.a, b: s0.b }`. Two narrower rules take those shapes without a
liveness analysis:

- **`moveLastUses`: a clone at the last use of a local is a move.** Every name for the slot's object
  - each load of the slot, each alias of one, each object-valued property read out of one - is used
  only before the clone, in its own block or in one dominating it. Outside a loop that is an order
  by dominance; **inside one** (J86) the block must also be in exactly the clone's loops, which makes
  it "earlier in this iteration", and every load of the slot must be preceded in its own iteration by
  a store to it (`refilledFirst`), so what the next iteration reads is never the moved object -
  `xs[i] = {xs[i] | ...}` in a loop is one clone rather than two. None of those names was ever kept, and every object
  the slot ever holds, nested fields included, is owned outright: a fresh allocation used once or a
  clone that is still one, **never an `Alias`**. That last clause is what the move adds - aliasing
  the copy hands out its nested objects too, and one of them may be a field `aliasCopies` shared.
  After it, the copy may be written, returned or kept like any owned value.
- **`readThroughCopies`: a clone read only for numbers is answered from its source** when every read
  is in the clone's block and nothing between the clone and the last read writes or calls. A store
  into a `Var` is the one write let through, and the copy may pass through one variable on the way -
  `let i = o.inner` is a store and a load back. The source may be any root, a `Property` included.

Both run after `aliasCopies` has been applied and the index rebuilt, so a clone that pass made an
alias is seen as the alias it now is; and `aliasCopies` in the next round follows a moved copy through
its `alias` like any other name. Both count under `aliased`. `test/resolve/AliasLastUse.yana` is the
fixture - records with an `F64` in them, so no packing slice will turn the clones into numbers - and
`JsirTester`'s shared-position test gained a third variant, since its body was a clone at the last
use of its local and had quietly become a move. `doc/progress/2026-09-22-J82-jsir-alias-last-use.md`
is the account.

**The eighth is `numberValues`** (2026-09-22, J86): value numbering over the dominator tree, for the
redundancy an `@inline(always)` graft leaves once per copy. `xs[i] = {xs[i] | f: xs[i].f + 1}` is four
grafted subscripts, each with its own bounds check and element read, and OSSA's `cse` removes none of
them: a window's length is a field of a view record it roots in no allocation, and it declines a
comparison across blocks on purpose. Here the window is flat values (J71), so the four lengths are
already one value and what was missing was a pass saying the four checks are one:

- a `binary`, `unary`, `coerce` or `convert` over the same canonical operands as one in scope
  becomes an alias of it - except a comparison in another block (numbered, not rewritten, for
  `cse.cpp`'s materialization reason) and a computation over constants alone, which is spelled at
  its use (`Scalars.yana` grew `var`s the one time it was not excluded). `0 + i` at an integer
  representation is `i`;
- a read of a property, an element or a range of either is answered by an identical read in scope,
  where scope ends at a block with more than one way in and at any write that is not a local or any
  call; a range of an element whose whole word a read already produced becomes a range of that value
  (a `BitRange` over a `Value` place, the form a packed record that arrived as a value already has);
- a branch on a condition a dominating arm decided - the arm's block entered only from that arm -
  becomes a jump, and every block the entry no longer reaches is deleted - by reachability from the
  entry since J91, because a loop in the dropped arm keeps a predecessor through its own back edge and
  deleting only the blocks left with none kept it, with a `var` for its argument and a phantom
  predecessor on the join. The cached `JsCfg` is dropped when that happens, and the argument
  coalescing and `structure.cpp` skip a deleted block's arguments.

**`dropWriteOnlyObjects`** (J86) sits in front of `dropDead`: an object every use of which is the
owner of a property store is removed with its stores, which is the `{ $v: 0 }` box an inlined `for`
over `items(xs)` wrote on every iteration and nobody opened. `test/resolve/LoopCleanup.yana` is the
fixture for all three, with `stamp` as the loop-move negative.
`doc/progress/2026-09-22-J86-jsir-loop-cleanup.md` is the account.

**Three rewrites for boxes and updates** (J87), in `opt_number.cpp` and `opt_objects.cpp`:

- **`foldConstantBranches`** answers `hostFixedCapacity` in JSIR with `structure.cpp`'s rule (a typed
  row has a capacity, anything else does not) and decides every branch whose condition is a constant
  or `eq`/`ne` of two integral ones. Before this the guarded `growJsArray(box, "$v", n)` call was
  still in the body when the optimizer ran, and it was the one use keeping an inlined `push`'s array
  local boxed.
- **`unboxLocals`** turns a one-property box whose every use is a read or write of that property into
  a variable: `{ $v: [] }` and `v.$v` become `var s = []` and `s`. Any other name for the box (a
  descriptor, a call argument, a bit range, a niche view) refuses. Growth on the plain row never
  replaces the array, but `xs = [...]` through a `&` does, so the box stays wherever the descriptor
  still leaves the body - a non-inlined `push_String(box, "$v", x)` included. Removing *those* needs
  an interprocedural "never rebinds" fact per parameter and a different ABI for it; not built.
- **`updatesAsLiterals`** builds `{v | a: x}` of an object as one literal: the clone into a variable,
  the stores into its fields and the read that uses it become an object built from the source's
  unreplaced fields and the replacements, with a packed `$pN` word seeded from the source and run
  through the updates' bit stores in one variable. The clone and that read must be joined by a
  straight path (single-predecessor `jump`s) on which nothing but a variable or the copy is written
  and nothing is called, so reading the source at the end sees what the clone saw. An unreplaced
  field that is not a number, a boolean, a `BigInt` or a string refuses, since a clone copies an
  object-valued field and a literal would share it. `test/resolve/ArrayBoxUpdate.yana` is the
  fixture; `doc/progress/2026-09-22-J87-jsir-array-boxes-update.md` is the account.

**A `match` over literal constants** (J88) is still one `JsInstMap` in JSIR - the semantic tag §5.6
asks to survive - and `mapExpr` in `codegen/js/structure.cpp` spells it, in the order of what each
costs to run: one literal where every arm agrees; a range test where two answers are all there are and
one of them is a run (of the domain when the map is closed, of the integers when it is open); an affine
step `s * k + b` (closed, or open over a dense run tested first, and only where every product, the
bias and every answer are exact integers of a double - J91); a module-level table `$m[s - lo]`
from four arms up where the span is at most four times the count and 256; and otherwise the `===`
chain. An open table reads `T[i] ?? fallback` unless an arm is `null`, when it tests the range. Tables
are interned by content (`Assembly::mapNames`) and the read carries the row's narrowest integer type,
so the AST range walk still drops the `| 0` the chain made unnecessary. `JsInstMap::closed` and
`closedFrom` keep the last arm's selector, which only the chain could do without. A niche-folded
answer (`Nothing`/`Just(k)`) is a literal (`constantLiteral`), and an object arm stays a `Select`
chain - never a table, for J69's reason - that skips any arm answering the fallback's record
(`ssa::sameConstant`, moved up from `lower/map.cpp`). `test/resolve/ConstantMapTables.yana` is the
fixture; `doc/progress/2026-09-22-J88-jsir-closure-ref-map-tables.md` is the account.

**A record whose field is a unique loan is never transparent** (J88, `transparentContentOf`). A
lambda's environment is the one that has such a field, and `environmentRecords` keeps it an object
only while the lambda is reachable - once inlined everywhere, the environment's construction and the
reads of a captured `&` survived and the record became the *value* it captured, so a callee that
rebound the storage wrote into a box around a copy. `test/resolve/ClosureRefCapture.yana`.

**Scalar replacement of records** (J89), three rewrites at the end of each round plus a wider
`forwardFields` - the fold of forwarding a whole-record store into a field load, which native gets
from `splitAggregateSlots` in the lower tier this target never runs:

- **`splitRecordVariables`** makes a record variable one variable per property where every store
  into it is a literal nothing else names (an `object`, or a `materialize` building one host object,
  each value at its property's representation) and every load of it is only ever - through
  `alias`es - the owner of a property read, a property write or a bit range over one property.
  Anything else - a clone of it, an argument, an edge, a `Value` or `Cell` place, a niche payload, a
  bit range over the variable itself - refuses the slot. So does an access **after a whole store**
  (J91): a load observes the object in the slot when it ran, so every access through its names must
  be reached from it with no whole store of the slot on the way - after it and before the next store
  in its block, or in a block `floodClean` reaches cleanly. `aliasCopies` makes the other shape out
  of `h(p, p)` with `&r` rebinding `p`, and the split answered the new record's fields for `q`. A record built on two paths needs nothing
  more: each path stores its fields, and a variable is already what a join reads.
- **`forwardVariables`** answers a variable's loads from its one store where that store dominates
  them. **`dropDeadStores`** removes a whole store no path reads before the next one, by liveness over
  that slot - asked of a slot nothing loads and of a slot some store fills with an allocation or a
  constant, which covers the zero the conversion writes where a record local is declared.
- **`storeLiteralFields`** writes a literal stored whole into a `Value` place as its values, property
  by property, which is how `structure.cpp` spelled that store anyway.
- **`forwardFields`** now follows `alias`es, whole reads of a `Value` place and clones nothing writes
  to the literal, answers a `split` of a `materialize` with its part, and runs a second time at the
  end of the round. Through a clone it forwards only a primitive, because a clone copies an
  object-valued field - `ErasedCopy.yana` answers -7 instead of 25 without that rule.

`structure.cpp` no longer declares a variable no instruction names. `test/resolve/RecordFields.yana`
is the fixture; `doc/progress/2026-09-22-J89-jsir-scalar-replacement.md` is the account.

**Text the per-body tiers could not remove** (J90), three mechanisms in three places:

- **Calls to empty functions, and functions nothing reaches** - `sweepFunctions` in
  `codegen/js/opt.cpp`, run by `genJsirProgram` over the whole file before the peephole and once
  after it. A teardown's call is minted by `teardown.cpp` after the shared inliner ran, so a
  `Reclaim` body that folded to nothing was still called (`Array_a_reclaim_I32(v9);`, 367 in
  `Base.js`), and this tier's rewrites take the last use of helpers minted per use (`growJsArray_T`,
  a devirtualized lambda) after `resolve_ssa/opt/reach.cpp` decided reachability. A call statement to
  an empty function becomes what its arguments write, in order; a function no non-function
  top-level statement reaches goes, with its `$h` header. To a fixed point. It is the only
  whole-program pass on this target, and it is on the tree because bodies are converted, optimized
  and structured one at a time on one reused arena.
- **An edge value computed into its argument** - `coalesceEdgeValues` in `opt_coalesce.cpp`, read by
  `defineBound` in `structure.cpp` through `JsCoalesced::into`. A result whose one use is an edge copy
  into a block argument, defined in the block whose terminator copies it, is assigned to the
  argument's variable where it is computed - `p5 = p5 + 1 | 0` rather than `var v673 = ...; p5 =
  v673;` - where nothing after the definition reads the argument's class (no instruction, not the
  terminator's operand, no other copy on any of its edges, following `alias`es), the class is live
  out along no edge, and no other value of the block claimed the class. `emitEdge` now writes the
  edge as the **parallel** copy it is: ordered so that no copy overwrites what a later one reads, and
  a cycle broken through one `var t`. Before J90 a swap of two plain arguments came out as
  `p1 = p2; p2 = p1;` - no fixture and nothing in `test/base` had one.
- **Short-circuit chains** - four peephole rules in `codegen/js/opt.cpp`, gated on `jsirPipeline`:
  nested `if`s without `else` are `&&`, two adjacent `if`s taking one jump are `||`, a labelled
  region's guard becomes `if/else` where nothing else names the label, and an `if/else` assigning
  one variable is a conditional (with `c ? (d ? x : y) : y` as `c && d ? x : y`). JavaScript's `&&`
  is lazy, so none of this is speculation, and it runs after one-use bindings are folded into
  conditions, which is what keeps `xs[i]` behind `i < length(xs)`. Not in `resolve_ssa/opt/select.cpp`:
  that pass would have to prove a later condition safe to evaluate eagerly, and native does not
  need the text.

`test/resolve/TextCleanup.yana` is the fixture; `doc/progress/2026-09-22-J90-jsir-text-cleanup.md`
is the account.

`JsTarget::optimizer` counts what fired - clones seen, clones aliased, fields forwarded, instructions
removed, storages adopted, allocations sunk, values numbered, branches decided, boxes unboxed, updates
built as literals, records split, variable loads promoted - and `-mode jsir` prints one `optimizer` line per program, so "how much
did this fire" is a `grep` over a dump. Every `.jsir.expect` golden carries that line, which is what makes a fixture's
own diff say whether a rewrite reached it.

## The helper families, by tag

`JsInst::Helper` is one opcode for the whole roster and `JsHelperOp` is which - the float
reinterpretation pair, the byte swap, the three bit counts, the two rotations, the three `Core.Bits`
operations and a function value's release. One row rather than eleven because what distinguishes them
here is a number: each is a call to a function out of `JsShared`'s slot table and nothing between the
conversion and the emitter branches on which one it is.

The **name is never the recognizer** (§4.4) - `structure.cpp` looks the family up by tag and the
tables are `codegen/js/helpers.cpp`'s, keyed on a `BitFamily` (`codegen/js/shared.h`) that neither
tier's IR owns. The **effects are the tag's**, through `effectsOf(const JsInst&)` on `Host`'s
terms: eight are pure, the float pair writes one buffer the whole program shares, and `FunDrop` runs
an authored teardown.

## Globals, and the two things a `var` is not

**A parameter whose address leaves the body is stored boxed too** - J70, and it is the same rule read
one position over. A parameter arrives in memory natively, so `%r = load %param` at a loan that
denotes storage is the address it already had and nothing has to exist; here a parameter the ABI
passes as one value is a host value with nothing behind it, so the same reborrow has nowhere to
point. The question is asked of the **reborrow** and not of the parameter, which is the difference
from a local and is not a detail: `walkAddress` answers "did this storage leave", and every ordinary
use of a *value* is a use that does not keep an address, so a parameter run through it directly is
always an escape and every scalar parameter in the corpus would get a box it never reads.

**And a value with no storage at all is boxed where a descriptor is demanded** - J78, and it is the
same sentence one position further out. A `Value` place is a recipe for reading one host value and
`placeOver` builds one over whatever is in hand: a constant, a call result, a reborrow of either. For
a host object that place *is* the reference, which is `jsIsItsOwnStorage` and is the rule `jsManifest`
states from the occurrence's side; for a host string or a number it is not, and answering the owner
handed the value out where the position wanted `{$o, $k}`. A continuation's environment holds a
unique loan of everything it captures, so `countIn(names, "bob")` supplied the string where the
field wanted the descriptor - `test/resolve/BorrowNoStorage.yana`. `referenceTo` builds the box
there, freshly and with no memo: what can reach that arm is a value no other name reaches, so two
descriptors over it are two temporaries and no write can be observed across them, which is what a
native temporary in a frame slot is.

**A global whose address is handed anywhere is stored boxed** - `var counter = {$v: 0}`, and every
read of it is one property deeper. A reference on this target is the `{$o, $k}` descriptor naming a
property of something and a module-level binding is not a property of anything, so the box is the
object there is to name. `JsTarget::boxedGlobals` is the set and `planBoxedGlobals` computes it in
`planProgram`, over the whole program: a global is borrowed in one function and read in another and
the two never see each other, so a shape they disagree about is a value written one way and read the
other. The predicate is the per-body demand pass's, factored out as `jsAddressEscapes` so that a
local and a global that escape the same way cannot get two different answers. A global whose value
already *is* a host object is not boxed, because writing through a reference to one writes its
properties.

**A `var` holding a record is built at its zero where anything writes it field-wise.** `var s2;` is
`undefined`, and a record that is only ever filled one property at a time - which is what an inlined
`f$new` does to an iterator's state record - never becomes an object at all. `Conversion::projects`
is the demand pass's second bit and the store goes in at the allocation; the place stays a `Var`, so
a whole-value store is still the rebinding it should be. The escaping case is the one above it and
makes the object a `Value` place for a different reason - identity rather than existence.

## What the tier reports rather than converts, as of this slice

Atomics, a **property witness whose subject has no host object of its own** (a transparent record or
a host row, which is where the native slot's `{read, set}` pair would go), a **function value** of a
body compiled once, a
call through a structural `Call` witness, a teardown through a boxed field, a bitcast between a host
reference and a number *that reaches no place and denotes no storage* (J70 made the `addressOf` arm
ask `placeOf` where the operand's own occurrence says it is a reference - which is what
`exchange(valueAtMut(m, at), v)` under `-no-ir-opt` needed, where the `&` argument is a call result
rather than a place the caller wrote), a
reference to storage written into a pointer field, a `RunSlots`, a size or an alignment of a type
this target cannot see, a native operation that is one machine's rather than one of the five portable
vector rows, a mutable borrow of a global (which now means a global the boxing pass did not reach),
and a mutable borrow of a `Var` whose address the demand pass did not see. Each is a named diagnostic at the instruction, which is what makes the sweep in
`doc/progress/2026-09-21-J68-jsir-parts-vectors.md` a worklist rather than a list of fixtures that
answer the wrong number.

That list grew at J68 rather than shrinking, and the reason is worth keeping: a type this tier had no
representation for made `planProgram` fail, so **no body was converted at all** and everything behind
the first such type was invisible. Vectors were that type for the whole of `lib/`, and the three
diagnostics J67 measured the library's JavaScript half at were the only three it ever reached.

A **`Value` place is its own reference** since J67 and is no longer among them: the storage is the
host object, so handing it out is handing the object out, which is the same sentence `jsManifest`
says when it answers `needsCell = false` for a unique loan of a `GcObject`.

A `Value` place has a second half that J69 added and it is the one rule about this tier that is not
symmetric. A whole value **read** out of one at an *owning* occurrence is a one-level copy of the
object, because a whole value **written** into one writes its properties: both are right alone and
wrong together, and `exchange` is where they meet - it reads the old value out, writes the new one
in, and hands back an object the write has already overwritten. One level is what a native relocation
out of storage is, which is the difference from `Clone`, a second owner that recurses.
`resolve/ExchangeStorage.yana` is the fixture, over the three storages that differ.

**Packed places** (J83, on by default since J85). `JsTarget::scalarizeRecords` makes a record all of
whose fields are narrow numbers one `Number{Unsigned, span}`, and `packFields` puts the narrow
fields of a record that stays an object into `$pN` properties (`JsPropertyRole::StorageUnit`, with
`JsShape::packedFields` as the per-shape field-to-(slot, range) map - `slotOfField` answers
`kNoField` for a packed field on purpose). Which fields are candidates is read off the `JsRep` -
narrow, a `number`, trivially copyable, not `@host`/`@box`, and never in a pinned record or a lambda
environment - and where they land is `resolve_ssa/packing.h`, the *rule* the native layout also
uses. The two targets share the rule and not the layout: natively the budget is 64 bits and on this
target 53, and candidacy differs too (a `bigint` field, a witnessed field, J86's exactly-full whole
record), so one record may be one word natively and two here. A field of either is a
`JsPlaceKind::BitRange` over the word's place, a construction is a `Pack` that the builder folds
when every operand is a constant, and `codegen/js/structure.cpp` decodes and read-modify-writes
through `codegen/js/bits.cpp`'s bit helpers. A record whose run exactly fills 8, 16 or 32 bits is
one number too since J86, unlike native, because what packing saves here is the object rather than
bits. Whole-record scalarization is asked first and co-packing only of a record that cannot be one
number (J84), which was the old JavaScript backend's order. A field a **property witness** names
never packs (`JsTarget::witnessedFields`, J85): a body compiled once reads it as `owner[key]`, so it
keeps a property of its own while its neighbours still pack, and a record holding one is never one
number. The old backend handed such a body a `{read, set}` accessor pair instead; here the answer is
the layout, because the accessor would turn every erased field read into two calls to serve a layout
this target is free not to pick. `generic/PackedProperty.yana` is the fixture.

**The narrow reference** (J84, on with the same switches). Where something packs, a reference whose
content is narrow (`jsRefCarriesScale`, which is `packableRep` of the content) crosses as three
positions, `(owner, key, scale)`, and the callee's place is a `BitRange` with a `bitScale` over
`owner[key]`, read and written through the borrowed `$bNx$get/set` helpers. The caller answers the
scale: `2**offset` for a range of a word, times the incoming scale for a range of a reference, one
for plain unsigned storage, and zero ("no range") for plain signed storage (`JsPlace::bitWhole`).
Where such a reference has to be **one value** - an erased argument, a return, a phi, a capture - it
is `Materialize`d into `$bitCell`'s accessor box, `{$o: box, $k: "$v"}`, so every `Cell` still reads
and writes as `$o[$k]`. There is no arity guard; `abi.cpp` says why. A property witness on a packed
field cannot arise (above); `assemble.cpp` still refuses one by name, as an invariant.
