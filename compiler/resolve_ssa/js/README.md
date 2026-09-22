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
passes through this directory** since 2026-09-21 (J69) - all 342 cases of `test/base`, which is the
same count the shipping backend's `js` row passes - **optimized and unoptimized** since J70, so
`ownership-js` and `ownership-js-unoptimized` are both rows of `test/run-base.sh`, and with the JSIR
optimizer switched off since J72, which is a third row.

## What each file owns

| file | what it decides |
| --- | --- |
| `module.h` + `inst.def` | the IR: representations, shapes, ABIs, values, places, instructions, blocks, bodies. `inst.def` is the only place an opcode is written down - the enum, the concrete-type cast, the mnemonic, the result form and the effects are generated from it |
| `repr.h/.cpp` | `jsRep(logical type)` and `jsManifest(occurrence)` - §3.2's two layers, and the shape table under them |
| `abi.h/.cpp` | one frozen `JsAbi` per emitted declaration, computed before any body |
| `name.h/.cpp` | what an identifier may be on this target - sanitizing, the reserved words, interning - and `jsir::key`, the compiler-owned property spellings (`$tag`, `$v`, `$c`, `$e`, `$o`, `$k`, ...) that **both** backends read |
| `build.h` | the typed builders. There is no universal `useValue`: a function's return type says which form came back |
| `convert_internal.h` | `Conversion` - the state the five conversion files share, and the three SSA predicates in front of it. Nothing outside this directory includes it |
| `convert.cpp` | the demand pass, places, and the instruction switch, the terminators, the edges, aggregates and calls |
| `constant.cpp` | `runConstant` and `constantLiteral` - §2.4's one constant lowering, which `codegen/js/assemble.cpp` asks for and spells |
| `teardown.cpp` | the two teardown *bodies* this tier synthesizes - a lambda environment's, and one per (type, halves) |
| `host.cpp` | the host rows (`resolve_mir/host.def`), the three vector transfers, and `.length`'s own range |
| `vector.cpp` | every operation over a multi-part value - asked **before** the scalar switch, on the representation rather than on the type |
| `cfg.h/.cpp` | `JsCfg` - reverse postorder, immediate dominators, loop membership and reachability, computed **once** per body and read by the verifier, the optimizer and the AST tier alike |
| `verify.cpp` | construction, finished-CFG and representation verification - on in every build |
| `opt.h` + `opt_alias.cpp` | the JSIR optimizer - §6.3's copy-to-alias elision and the forwarding and dead-code walks it needs to pay. Switchable off whole with `-no-jsir-opt` |
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
reference's. They were spelled in four places across the two backends until 2026-09-22 (J80), and
what makes them a representation fact rather than a spelling is that a `JsShape` records the final
key: two spellings of one key are two hidden classes for one record, and a value built by one
generator and read by the other answers `undefined` with no diagnostic anywhere. `jsir::key` in
`name.h` is the table and `codegen/js/shared.h` includes it.

**Every value has a `JsRepId`, and no lowering decision reads a `mir::Type`.** `JsRep::source` exists
for dumps and for the sentence a diagnostic names a construct with, and for nothing else. Several
logical types share one representation - `Int`, `Size` and a `@bits(30)` refinement are all one
`Number` - and one value may have a representation no single type node describes.

**Values and places are separate ID spaces.** Only `Load` and `Store` accept a place. A phi can never
denote a slot on one edge and its contents on another.

**An SSA value whose convention is `Address`, or whose occurrence is a loan that denotes storage,
becomes a place; everything else becomes a value.** `loanDenotesStorage` is §1.3.1's predicate -
`readMode`'s own decision procedure, with no layout in it. That one discriminator is what replaces
the shipping backend's scattered "is this a reference or a box" questions.

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
against the shipping backend's 166, and `Just:` disappeared from the file entirely.

The rule is on the **logical type** and not on the name `Maybe` - `nicheFoldOf` in `repr.cpp`. Two
constructors, one carrying nothing and the other carrying exactly one thing whose representation
cannot already be `null`; `Nothing` is `null` and `Just(x)` is `x`. `Result(E, T)` has two payloads
and no second pattern to tell them apart by, so it declines here exactly as `Niche::fits` declines it
in the shipping stage, and a `@layout(js)` pin declines because that is the declaration saying what
the host sees. **Nesting is refused** - a `Maybe(Maybe(T))` keeps its `$tag` over a folded inner one -
which is §3.2's rule and `jsAdmitsAbsent`'s single statement of it, read by `jsRep` to decide a fold
and by `verify.cpp` to refuse one taken anyway.

**A number payload folds**, and that was measured rather than read off the shipping backend: with
number payloads left tagged the suite runs in **2.420 s** against **2.058 s** with them folded,
interleaved best of nine. Folding only the object payloads is *slower than folding nothing*, which
nothing predicted - a `Maybe(Int)` left as an object beside a folded `Maybe(Person)` makes the sites
that handle both polymorphic.

Three operations and no fourth, which is what makes a later encoding - a packed tag, a second niche -
a change to three functions rather than to every site a sum is reached from:

- **the tag read** is `v === null ? <absent> : <present>`, at `HostBool` so that the comparison the
  consumer writes folds against it (`foldTernaryCompare` in `codegen/js/opt.cpp`, which already
  existed for the shipping backend's own folded tags). The projection of
  `InstFieldAddress::kDiscriminant` answers that *value* rather than a place, which is the state
  `placeOf` already knows;
- **the payload place** is the base's place read at the payload's representation -
  `JsBuilder::addNichePayload`, a copy of the place with `JsPlace::nichePayload` set, which is the
  one licence `checkPlace` grants against a shape's property representation. `Downcast` being the
  identity is the same sentence said in the shipping backend;
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
an array: 981 of them in `Base.js` against the shipping backend's 137, which J69 §6 measured as the
largest single line of the size and time gap. The **record's own representation** is the `Parts`, not
the occurrence's, which is the half that took a measurement to see: `slice` builds an owned
`ArrayView` value and a `represent` relabels it as the window, so a rule keyed on `AccessForm::View`
alone would have built the object at the aggregate and split it one instruction later. `jsStoredRep`
is the object again, so a *stored* window - a field, a local the demand pass kept, an erased argument
- is exactly what it was.

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

**A reference crosses flat** - `(owner, key)`, §3.2's narrow-reference row minus the scale this
target has no packed places for. A descriptor is already a pair of host values, so the object around
it buys nothing at a position that takes two arguments and costs one allocation at every mutable
borrow crossing a call: 867 in `Base.js` against 223, and 819 of those were `{$o: box, $k: "$v"}` - a
reference to a whole boxed local. It also takes a level off every read *inside* the callee, which is
an `Element` place over the two halves: `owner[key]` where it was `x.$o[x.$k]`. This is the shipping
backend's own arrangement, whose `showWith(v699, ..., v11, "$v", 1)` is its flattened `{$o, $k, $s}`.

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

**The discriminator is the representation, not the type.** The shipping backend asks `isVectorType`
before its scalar switch; here the question is whether the value is multi-part, which is the stronger
form of the same test and the one this tier is allowed to make. A comparison is the one operation
asked of its *operands*, because its result is the mask. `vectorLanes`, `VectorType::isMask` and
`isVectorType` appear nowhere below `repr.cpp`.

**A lane is an ordinary value, so the arithmetic is the scalar arithmetic.** `structure.cpp` spells a
lane's multiply as `Math.imul` and its division as `$div` by reading the result's representation,
exactly as it does for a scalar - so none of the thirty spellings the shipping backend's `laneBinary`
restates is restated here. What is left in `laneBinary` is the four things a lane genuinely differs
in: the mask's bitwise operators are the logical ones, the two `Core.Bits` families are declared at
widths the 32-bit helpers do not have and are corrected, the saturating pair has to be exact before
it clamps, and a reduction is a tree.

**The materialized form is a host row**, typed where the lane has a typed row, and it exists only for
the positions one value is the only thing that fits: a field, an element, a `return`, an erased
argument. A **block argument is deliberately not one of them**, which is what closes the shipping
backend's known gap 10 by construction - a vector carried around a loop is `lanes` block arguments
and `lanes` edge operands, so an accumulator in a vector loop allocates nothing per iteration.
`resolve/VecValue.yana`'s two goldens are the pin: the `.js.expect` rebuilds an array each time round
and the `.jsir.js.expect` assigns four variables.

The refusals are the shipping backend's, by name: a vector of a 33-to-53-bit lane has no JavaScript
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
- **`compiler/repr`.** That is the shipping stage's table, which computes both targets from one
  struct - §3.2 quotes `lower/repr.h` on why that is two readings of one type. The rewrite reads it
  nowhere and JSIR must not be what brings it back.
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

## The optimizer - one pass, five rewrites

`opt_alias.cpp` since 2026-09-22 (J72), run between the conversion and the AST builder and
switchable off whole with **`-no-jsir-opt`** - Analysis-IR-JS.md §10.2's third configuration, which
`test/run-base.sh`'s `ownership-js-no-jsir-opt` row and every `.jsir.expect` fixture's third Node run
now exercise. The verifier runs on what the pass *returns* in every build and, in an assertion
build, on what it was given as well, so a body this pass broke is named by the run before it.

**What it removes is the structural copy at a borrow.** `readMode`'s first clause turns a shared,
call-bounded loan of a trivially copyable value into a `copy` (§1.3.1's category 2). Natively that is
a few words on the stack and `promote` takes it back; here `cloneOf` writes the object out property
by property and **recursively**, so a two-field record of two records is eighteen property reads at
every borrow. 2,954 of them in `Base.js`, 262 KB of object literals against the shipping backend's
5.5 KB - a quarter of the whole size gap, and the shipping backend never copies at a borrow at all:
its `keptValue` duplicates only at a *store* or a `return` whose source still names live storage.

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
`md5Answers` in `test/base` is the shape, five times over: two allocations and two sixty-four element
literals where one of each is owed. The place is taken from the value that was stored - the
allocation goes and the store becomes `zero = alias stored`, in the store's own position - on three
conditions: **every use of the zero comes after the store** (checked as "in this block, ahead of it,
with nothing naming it in between", which is enough because a block is straight-line), **what is
stored is a fresh allocation this store is the only use of**, and the representations agree. 133
sites over `Base.js`, and the field-wise copy count went 679 to 372 against the shipping backend's
375. The first attempt asked instead that every use of the zero be a *place* rooted at it and fired
ten times: the uses it refused are the ones that make a `Value` place what it is, since a call taking
the local as `&self` takes the object itself.

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

`normalizeForm` allocated **1,581 MB** over `test/base` against the shipping backend's **67 MB**,
and the shipping backend's difference is not an optimization: it writes the same objects *inside*
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

`JsTarget::optimizer` counts what fired - clones seen, clones aliased, fields forwarded, instructions
removed, storages adopted, allocations sunk - and `-mode jsir` prints one `optimizer` line per program, so "how much
did this fire" is a `grep` over a dump. Every `.jsir.expect` golden carries that line, which is what makes a fixture's
own diff say whether a rewrite reached it.

## The helper families, by tag

`JsInst::Helper` is one opcode for the whole roster and `JsHelperOp` is which - the float
reinterpretation pair, the byte swap, the three bit counts, the two rotations, the three `Core.Bits`
operations and a function value's release. One row rather than eleven because what distinguishes them
here is a number: each is a call to a function out of `JsShared`'s slot table and nothing between the
conversion and the emitter branches on which one it is.

The **name is never the recognizer** (§4.4) - `structure.cpp` looks the family up by tag and the
tables are `codegen/js/inst.cpp`'s, shared with the shipping backend and keyed on a `BitFamily` that
neither tier's IR owns. The **effects are the tag's**, through `effectsOf(const JsInst&)` on `Host`'s
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
