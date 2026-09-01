# `resolve_mir` - source to ownership MIR

The front half of the rewritten resolve stage: name resolution, logical types, ownership contracts,
MIR construction and the checking that runs over it. `resolve_ssa` is the back half.

The design is [`doc/inflight/Analysis-IR.md`](../../doc/inflight/Analysis-IR.md), whose part 5 is
this IR and whose part 11 is why this exists beside `compiler/resolve` rather than replacing it a
piece at a time. The source model it implements is
[`Analysis-Borrows-Revised.md`](../../doc/proposal/Analysis-Borrows-Revised.md).

**The document calls the two IRs OMIR and OSSA; the code calls them MIR and SSA.** One pair of names
that both start with `O` made `om::` and `os::` a keystroke apart in every diff, and MIR-then-SSA
says the order without knowing this compiler. The design record keeps its own spelling.

**This is a clean rewrite and it shares no code with `compiler/resolve`.** It is selected at run time
by `-resolver ownership`, the current stage is the default, and both hand a `LowerModule` to the same
backends. Part 11.5 is when the old one is deleted, and it is not on a schedule.

## The three things that are separate here and were not

1. **A logical type** (`type.h`) is what generics substitute and what instance selection sees. There
   is no borrow type, no view type and no owned/shared pair: `String` is one type however it is
   passed. Adding a `Type` kind for a *manner of passing* is the mistake this file exists to prevent.
2. **An ownership contract** (`contract.h`) is an occurrence's access, origin and access form. It
   decides legality and function compatibility and nothing about instance selection.
3. **A manifestation** is neither of those, is the target's answer, and lives in
   `resolve_ssa/manifest.h`. Nothing in this directory may ask what a value looks like in a register.

An `Occurrence` is a logical type, a `Contract` for the value's *root*, and an interned
`OwnershipSchema` for the positions *inside* it. All three, because the first two are not enough:
`a'Maybe(T)` is a shared occurrence of `Maybe(T)` through origin slot `a` with nothing borrowed
inside, and `Maybe(a'T)` is an owned occurrence of the same logical type with one interior slot at
the payload. With only a type and a root contract those two are indistinguishable, and the only way
to tell them apart is to put ownership back into `Type` - which is the mistake this file exists to
prevent. The schema is interned, so shape identity is handle identity, and the empty shape (every
type in the language subset built so far) is the null handle and costs nothing.

An occurrence travels together everywhere a bare type would have: locals, block arguments, node
results, signature positions, place records - and, across the phase boundary, `ssa::Value`.

## What is enforced where

| Invariant | Enforced by |
| --- | --- |
| every ownership-sensitive input has a use mode | `Operand` has no constructor from a bare ID |
| every node's operands are visible to generic walks, **with their roles** | `eachOperand` on the node struct; a kind without one does not compile |
| a node that has no result carries no result occurrence | `NodeResult` is a separate base, and `kMirResult` is what licenses the cast |
| a block has exactly one terminator, at the end | `Builder::append`, and `verifyBody` restates it |
| an edge's arguments match its successor's parameters, in type **and contract** | `Builder::addJump` for the arity, `verifyBody` for the occurrences |
| a read names a place; a write's destination is one it may write | `verifyBody`, over `OperandRole` |
| `CopyTrivial` has evidence, and `Take` names something owned | `verifyBody`, over `OperandRole` |
| a place's projection is well formed and interned, with **no truncated identity** | `Builder::internPlace`, whose key is asserted rather than masked |
| everything that needs the finished CFG | not attempted here - see below |

An `OperandRole` travels with every operand through `forEachOperand`, and that is what makes the
middle four rows expressible at all: a walk that hands over bare operands can check that a source
exists and nothing else, so "a read names a place" has no form to be written in. Analysis-IR.md
§12.3's mutation testing needs the roles for the same reason.

The split is Analysis-IR.md §2.1 against §2.2, and keeping it is the point. A builder cannot decide
whether a `Forward` is a move, where a loan ends, or whether a path initializes, and a verifier that
half-answered one of those would be a verifier nobody believes. `UseMode::Forward` is what records
the question the builder deliberately did not answer.

## Arenas

`region.h` declares all three - `DeclRegion`, `MirRegion`, `SsaRegion` - because a `mir::Function`
lives in the first and names a body in each of the other two. `DeclRegion` outlives the compilation;
`MirRegion` is released whole immediately after conversion, before the lower IR is built, which only
works because nothing below may hold a handle into it (§8.8). A body's literals are in the MIR region
for the same reason they are values rather than allocated nodes: a `7` inside a function is body data
and has no business surviving the body.

**Nothing placement-allocated in an arena may own heap memory.** A bump arena is released whole and
runs no destructor, so an `Array` or a `HashMap` held by an arena-allocated struct leaks its buffer
once per instance and once per compilation. `Types` and `Shapes` state the rule from the other side -
they are held by value on `Program` precisely because they need hash maps - and `Module` obeys it:
its dense lists are `RegionArray`, and its by-name indexes live on `Program`.

`RegionArray` grows in place where the arena allows it (`AlignedRegion::extendLast`). A bump
allocator cannot free, so a table that doubles by allocating a new buffer abandons every old one
inside the region; measured over a twelve-thousand-entry four-byte table that is 2.7x its live
contents, and 1.4x with the in-place growth. When something *was* allocated in between the fallback
is the old allocate-and-copy, which is the bump allocator's own bargain.

Allocations here are eight-aligned. `LinearArena::alloc` bumps by the raw size and
`compiler/util/README.md` records the misalignment that produces; the fix is held up in the old tree
by goldens with raw arena offsets in them, and there is no reason to acquire the same debt here.

## The files

`resolve.cpp` grew past the point where one file could be read, and is split the way
`compiler/resolve` is - by what a reader is looking for rather than by size:

| File | What is in it |
| --- | --- |
| `resolve_internal.h` | the `Resolver` struct itself, the operator table, `Held`/`Carry`, `TypeArguments` |
| `resolve.cpp` | the driver, written types, and constants |
| `resolve_decl.cpp` | declarations: types, aliases, fields, globals, and the walk that orders the passes |
| `resolve_sig.cpp` | one written occurrence, and the signature it is a position of |
| `resolve_class.cpp` | classes and instances - the heads, the members, and what each has to prove |
| `intern.h` | the one probe behind every table in this stage that interns by content |
| `gen.h`/`gen.cpp` | type variables, generic contexts, class requirements, the schema, substitution, the one-way type matcher |
| `select.h`/`select.cpp` | which instance serves a class at an argument list - a match above the table |
| `specialize.h`/`specialize.cpp` | what a *call* to a generic becomes: one `Function` per argument list |
| `resolve_expr.cpp` | the things that answer: places, reads, calls, operators, `if` |
| `resolve_stmt.cpp` | the things that do: statements, assignment, loops, `return`, lens calls, bodies |
| `resolve_lambda.cpp` | a lambda: the function it is lifted into, its captures, and the environment they live in |
| `name.h`/`name.cpp` | the one traversal every module-level lookup goes through, and what `pub` means to it |
| `import.cpp` | which modules the program is made of, and what each one can see |
| `export.cpp` | the declaring side of `pub` - a marker that means nothing, and an interface that leaks |
| `intrinsic/` | the compiler's roles over this IR - see below |

`resolveProgram`'s order is the one thing that has to be read as a whole and it is in `resolve.cpp`:
`discoverModules`, `linkImports`, `declareFixities`, `declareTypes`, `declareAliases`,
`declareClasses`, `synthesizeCoreClasses`, `declareRecordFields`, `resolveAliasTargets`,
`foldSchemas`, `declareClassMembers`, `declare`, `checkExports`, `classifyTypes`,
`resolveInitializers`, `checkExportedGlobals`, `checkRoles`, `resolveBodies`, `freezeSchemas`,
`verifyProgram`, `analyzeProgram`, `computeSummaries`.

**Every phase runs over the whole program**, not over one module and then the next. That is the same
statement as "an import does not trigger the resolution of what it names": a pass that finished a
module before starting the next would be an ordering between them, and a cycle has no ordering to be
given. `Resolver::eachFile` is the one walk they are written against, and it is what sets the
position - which module, and which file of it - that a name lookup is relative to.

Six declaration phases rather than two, and the split is a dependency rather than tidiness. A
signature may bind a record's interior positions - `x: Reader(a')` - so the record's schema has to
exist before signatures are read, and under one interleaved walk a function declared above a `data`
in the same file was resolved first. A *class head* has to exist before any signature, because a
constraint names one; a class's *members* have to exist before any instance, because an instance is
checked against them, and they are declared ahead of the ordinary functions so that what follows is
still read in declaration order - a module's function list is what the dump prints and what lowering
walks. A schema needs every field and nothing else; ownership needs the
whole instance set, which is why `classifyTypes` still waits and why the two are separate passes over
the same fields. There is **one error checkpoint**, after all four: a checkpoint between them turned
the reordering into a truncation, and a file with one bad field stopped reporting anything about its
functions.

## Milestone

Part 11.4's slices 1 to 6, through waypoint G4 and the parity work above it. A module of `let`
constants, `alias` and `data` declarations, `class` and `instance` declarations and functions, whose
bodies are
scalar computation, control flow, calls, records, ownership and loans: literals, arithmetic,
comparison, `Bool`, locals, `if` as a value and as a statement, `while`, the counted `for` in its
three spellings, `break` and `continue`, blocks, block arguments,
record construction and projection, moves, partial moves, the teardowns all of that implies, and
borrowing - `&` parameters and bindings, published origin slots, and record fields that refer to
storage the record does not own.

**Generic functions and types**, with every call specialized - see
[Generics, and what a specialization is](#generics-and-what-a-specialization-is) - and **classes,
instances, superclasses and defaults**, with every call selected: see
[Classes, and what selection is](#classes-and-what-selection-is). Classes take an argument list, an
instance head is a pattern matched against it, a `->` in a head is a functional dependency, and a
`data` head may constrain its own parameters. On the declaration side: head parameter defaults, one
name at several arities, an instance member whose signature is taken from the class, and a
transparent `alias`.

**Named arguments, default arguments and field defaults** - `Pair {x: 1}`, `open(path, mode: Read)`
and the positions either may leave out, `@lazy` among them. See
[Which position each argument fills](#which-position-each-argument-fills).

All five requirement forms a signature may write are built: a bare variable, a **class**
`Show(a)`, a **structural field** `a.field: b`, a **constrained callable** `f: (a) -> b`, and a
**const parameter** `n: I32`, which is also a `data`, `alias` and `class` head parameter. The last
three are one sentence made three ways - **a body names a requirement, never a number** - so a
property place names which field requirement, a `gen.value` names which count, and a call names
which callable, and the answer arrives with the substitution. Under specialization that is the
conversion; in the erased form it is a slot of the environment, and the numbering is `GenSchema`.

A **function type** and a **function value** are built - `(I32) -> I32` as a type, a named function
in a value position, and a call whose callee is a value rather than a name. A callable *requirement*
stays decomposed rather than becoming one, and the difference is the whole of why both exist: a
requirement is a shape to select a function by and is discharged where the call is written, and a
value is two words something holds, stores in a field and releases. `CallThrough` gained a fourth
row for it rather than the IR gaining a fifth call - where the callee comes from is what that enum
already answers - and the consequence is that "calling anything but a named function" is no longer
refused at all: anything that is not a name a declaration answers is a value.

A **lambda** is built, and is the same value: it is **lifted** into an ordinary function of the
module with one parameter in front of the ones the author wrote, and the expression becomes a
`fun.value` naming it. Its captures are found by the body walk rather than by a scan, because a
capture is a *place* projected out of that parameter and a place is interned rather than emitted - so
the environment record's fields are appended as the walk finds them. The conventions come off the
*enclosing binding*, which is what `doc/spec/functions.md` says and is the reading that has an answer
at the moment a field is appended. A capture is asked about everywhere a local is - in a place, in a
value position, and in callee position - so an enclosing binding shadows a declaration of the same
name exactly as a local does, and `g(x)` inside a lambda is a call through whatever `g` holds.

The environment is a **local of the enclosing frame**, and the closure is an owned pair with one
interior position naming it - `Shapes::closure`. That is the whole of the safety story, because it
puts a closure inside vocabulary that already existed: the value is rooted in the loan of the
environment, so `checkReturnConformance` refuses a `return` of one; a `&` capture holds an exclusive
loan for the closure's extent, so the enclosing frame writing what it lent is the ordinary overlap;
and a borrowed capture's drop is ordered after the closure's last use because the loan extent says
so. The root stays *owned* for a physical reason - a borrowed occurrence of an in-memory type
manifests as its address, so a closure described as a borrow would be one pointer where the language
has two words.

One check is new, and it is `own.cpp`'s rule applied to a value. A record already refuses a declared
field whose type refers to storage the record does not name; a function type is one type whether or
not the value of it captured anything, so the declaration cannot say and `checkCarriedOrigins` asks
the value instead. It fires at an **owned** position only - a field, a `->` parameter, an assignment
into storage that already exists - because a borrowing position is bounded by its call, which is what
passing a callback *is*. A `let` is accepted because it mints its storage from the value it binds.

What is refused, each by name: a nested lambda, a lambda in a generic body (argument zero is one
slot, and an erased body already spends it), a **consumed** capture, a merge of a capturing and a
non-capturing lambda, and every escape - in terms of the **heap-placed** environment it would need.
`StorageClass::Heap` is a rung nothing selects and `verifyBody` refuses it outright, the way it
refuses `Region` and for the same reason: there is no allocator, and a pass that selected it anyway
would reach a lowering that allocates a frame slot whatever the object says.

A **`@lazy` parameter** is the same lift with no written parameter list. `doc/spec/functions.md`'s
sentence - "the argument expression compiles to a nullary thunk over the caller's frame" - is the
implementation: `declareDeferred` rewrites the position's occurrence to a shared, call-bounded
`() -> T`, `resolveArguments` lifts the argument into a thunk instead of resolving it, and a read of
the name in the callee is a call of that thunk. So a deferred argument *is* a closure and inherits
every rule above with nothing added, including the two refusals - a thunk inside a thunk, and one in
a generic body. What it adds is one walk: **at most once on any path**, over the finished body and
looking for a read of the parameter's storage, which is what makes this call-by-name rather than
call-by-need - forcing twice is rejected, so there is no memoization slot, no flag and no allocation.
Handing the thunk on to another deferred position is that one read, and passes the thunk itself
rather than wrapping it again. The marker is refused on `&`, `->`, an external origin and a
traversal, each because the two would describe the position differently.

A **condition** is a *sequence of tests* rather than a value that is then tested, which is what makes
`if m is Just(v) && v > 0` bind `v` for the right operand and for the arm. The chain is flattened and
split at its loosest operators when those are all `&&`, so the split is the fixity table's answer and
not the parser's nesting - `a == 1 && b` splits and `a && b || c` does not - and each span's failure
edge is the whole condition's. `while` needed nothing of its own, since both go through one
`resolveCondition`. The shipping stage does not do this; `test/resolve/Conjunction.yana` is the
fixture, and five existing `&&` goldens shrank when the merged `Bool` stopped being built.

A **qualified alias** is refused for a different reason - it is a type rather than a spelling, and
needs conversions and `deriving`.

**Linear types**, which are this stage's alone: `@explicitDrop` withholds the compiler's permission
to insert a teardown, `->x` writes a disposal down, and `@local` says who may write one. See
[The Drop, Reclaim and Copy classes](#the-drop-reclaim-and-copy-classes) below and
`doc/proposal/Design-Linear-Types.md`, whose build order this implements the first four steps of.
The shipping stage refuses `->x` by name and accepts both attributes without acting on them, so a
fixture about any of this carries `# resolver: ownership`.
There is no prelude - the type names the language has of its own are builtin, and `Program::core` is
null so the implicit import of Core contributes nothing. Anything else in a source file is reported
by name as not implemented, because this stage is chosen by a flag over a corpus written for the
other one and meeting an unimplemented construction is an ordinary event.

**The builtin type names are `intrinsic/primitive.def`** and nothing else - `Types::findBuiltin` is
the one lookup, and it turns a row of that table into this stage's representation. The eight fixed
widths, the two floats and `Bool` are the obvious rows. Four more are not:

- **`WideInt`** is fifty-three signed bits - the widest integer that is still a machine primitive on
  every target, since a host with no integers holds fifty-three consecutive ones and nothing wider.
  A type of its own rather than `@bits(53) I64`, because a refinement dispatches to the instances of
  what it refines and would do its arithmetic at the width it is trying to avoid. It is the one
  width with no byte count, so `Types::intType` names its handle rather than indexing for it, and
  `manifestNative` rounds fifty-three bits up to eight bytes - a size taken by division is six,
  which is an alignment nothing can address.
- **`Size`, `USize` and `CodeUnit`** state no width at all. Which quantity answers is
  `intrinsic::TargetWidth`, and this stage carries that rather than a number: MIR does not depend on
  the target, and every stage that reads MIR already knows which target it is reading for. `minBits`
  and `maxBits` are what may still be said here - the bound is thirty-two to sixty-four for a word
  and eight to sixteen for a code unit - and `IntType::bits` holds the **maximum**, so an unaudited
  reader is wrong on a narrower target rather than laying out a value of no bytes.

  `resolve_ssa/manifest.cpp`'s `intBitsOn` is where the deferring stops, and it is the only place a
  `TargetWidth` becomes a number. What it reads is `NativeTarget::integers`, which is why the
  manifestation entry points take a `Layout` - the declaration region **and** the machine - rather
  than a region alone: a site holding only the first would silently get this compiler's answer for a
  width the program left open, which is the failure Analysis-Modules.md Move 2 removed from the old
  stage.

  Two rules that used to be written as a number are now written as the name. `BuiltinShape::Word`
  in `intrinsic/link.cpp` asks that a word-shaped builtin be declared `Size` or `USize`, where it
  used to compare the declared width against `targetIntWidths(settings).word` - a question this
  stage has no business asking, and one that would have accepted `I64` on a machine whose word is
  thirty-two bits while refusing the `Size` that is the word there. That was the last target query
  in `resolve_mir`. `erase.cpp`'s `slotBytes` is the same mistake one layer down: a const slot is
  sized at `intBitsOn` rather than at the maximum, because `numberSlot`'s access is sized from what
  the value manifests at and a slot eight bytes wide read by a thirty-two-bit load is what its
  assert exists to catch.

  What is **not** implemented is the conversion ladder between them. This stage has no integer
  widening at all yet, so `Int` does not reach a `Size` position and `integerRangeFits`' two-ended
  rule has nothing here to be the rule of.

**`Int` and `Long` are temporary builtins here**, spelled the way `lib/Core/Core.yana` spells them -
`alias Int = I32`, `alias Long = I64`. They exist for the fixture corpus, most of which writes its
integers that way, and they go when milestone 6 makes `alias` a declaration this stage resolves. An
alias is not a type of its own, so nothing downstream can tell which spelling was written and every
golden says `I32`.

**Arithmetic is not truncated to a declared width**, at any width narrower than the register it
arrives in. `add(200, 100)` at `U8` answers 300 here, and a `WideInt` sum passes 2^53 rather than
wrapping - one missing rule about every sub-register width, not something about those two types. It
is invisible wherever the result goes straight to storage, because the store is narrow. The shipping
stage's version is `wrapsAtDeclaredWidth`/`truncateToWidth` in `resolve/lower.cpp`, and the
invariant it rests on - a signed narrow value is held sign-extended - is what has to be established
here before the truncation is worth adding. `test/resolve/Widths.yana` is written to stay inside
what both stages agree on and says so.

### Modules, imports and `pub`

A program is a **root module and everything it imports**, and how it gets there is two passes with a
strict order between them - `import.cpp`. `discoverModules` walks the imports transitively, asking
the project's own files first and `lib/` second, and creates a `Module` for everything it reaches;
`linkImports` then fills in the edges. Two passes because a cycle has to have **one** meaning: an
import that triggered the resolution of what it named would give a two-module cycle two readings
depending on which was compiled first. `resolve/ImportCycle.yana` is the fixture, compiled by both
stages.

The project's files first and `lib/` second is the direction that has to work: a library is shared
and a program is not, so the one that can be changed to resolve a collision is the program - and it
should not have to be changed by *renaming* the module it wanted.

A module is a **directory of files** (Analysis-Modules.md §2.1), and an **import is scoped to the
file that wrote it** (§2.1.2). The second is one field and a guard rather than a file identity
threaded through every lookup: `Module::activeFile` plus `ModuleScope`, which restores rather than
clears, since resolution nests - an alias target is read on demand and may be another module's.
`resolve/module/Scope.yana` is the accepting half and `Reject.Scope.yana` the refusing one, and the
refusal probes five positions at once because a missing file scope is a **false accept**.

**Every module-level lookup goes through one traversal** - `mir::search` in `name.h`. Types,
aliases, constructors, functions, classes and operator fixity alike, so there is one answer to each
of the four questions the traversal decides: what a qualified name means, which modules a name is
looked for in and in what order, what an import's include/exclude lists hide, and what happens when
two imports offer the same name. A local definition shadows an import outright; two imports offering
one name are *ambiguous* rather than resolved by import order.

A name is a sequence of segments and so is a **declaration's** name, which is what makes `Maybe.Just`
and `Hmac.sha1` one lookup rather than two: `NameRef` (in `compiler/compiler/context.h`, since both
resolvers walk names this way) says how much of a name a step consumed, and the one place that
matters twice is an import whose local name is also a namespace of its own declarations - `File.open`
where `File` is both. The alias is consumed *before* the whole name is tried rather than instead of
it, so such a namespace is reachable rather than shadowed - and each reading applies **both halves of
visibility inside itself**, so a reading that finds a name the author may not have is "not here"
rather than an answer that masks the next reading. A module declaring a private `f` beside a
`pub M.f` offers the second to an importer writing `M.f`; `resolve/module/Import.yana` asserts it,
and the shipping stage had the same masking bug and has the same fix.

**`pub` has two sides and they are two files.** The reading side is one clause in the traversal: a
symbol found in an imported module is a hit only when it was exported, and a private name is *not
visible* rather than refused - it reads exactly as a name nothing declares, because "this exists but
you may not have it" is an interface a private declaration does not have. The declaring side is
`export.cpp`, and it is the two things that clause cannot say by itself: `pub` on an instance means
nothing and is reported, and a `pub` declaration may not hand out a type nobody outside can name. A
class carries the marker for its member signatures, since they are the interface it declares; a
constructor is exported with its record and never on its own.

The **orphan rule** is `resolve_class.cpp`: an instance belongs to the module declaring the class or
to one declaring a type its head reaches. A modularity guarantee rather than a soundness one -
coherence is program-wide and is the instance table refusing a second head of one shape - and it uses
the same "does this head reach a local type" walk the capability classes use, so the narrow rule and
the general one cannot disagree about a head.

**Fixity is a declaration.** `infixl`/`infixr` is read into the program by module and looked up
through the imports like any other name; `kOperators` in `resolve_expr.cpp` is the fallback for an
operator nothing in scope declares, standing in for `Core.yana`'s own lines until this stage reads
Core. What a declaration overrides is the *grouping* only - the form and the instruction stay the
table's, because an operator cannot dispatch to a class member here yet.

**`Program::core` is null and the implicit Core edge is dormant.** Nothing builds Core, so the edge
`linkImports` would add contributes nothing; the day something does, the mechanism is there and no
reader changes. `import Math` already finds `lib/Math.yana`, parses it, and reports honestly what it
cannot resolve.

### What a signature is here

An `Arg` is a name and an `Occurrence`, and the occurrence is the whole of the calling convention at
this level - `Analysis-Borrows-Revised.md` §2.1's table, read directly:

| Written | Contract | Built |
| --- | --- | --- |
| `x: T` | shared, call-bounded | yes |
| `->x: T` | owned, transferred to the callee | yes |
| `&x: T` | unique, call-bounded | yes |
| `x: a'T` | shared in published slot `a` | yes |
| `&x: a'T` | unique in published slot `a` | yes |

`x: &T` and `x: &a'T` are the same two rows written with the sigil on the type, and one function -
`resolveOccurrence` - reads all of them, for a parameter, a result and a record field alike. A
signature states an origin relationship on **both** ends: a result naming an origin no parameter
published is refused there rather than promoting the parameter to satisfy it, which is §2.2's rule
and what keeps a caller's argument from changing meaning because a result was edited.

A shared parameter of a trivially copyable type is still not a loan - the position copies, per
`doc/spec/ownership.md`, and a trivial value's loan and its copy are physically the same thing. That
it is nonetheless a different statement in the IR is what makes the two separable: `read
copy.trivial (x)` and `borrow.shared %v` are what the overlap check reads, and collapsing them would
have left it nothing to tell apart.

**A parameter is a `Local`, not a block argument** (`Local::parameter`). It is storage the body
projects, borrows from and - under `&` - writes through, so it is a place from the first statement.
The value that arrives on entry is an SSA question and is answered there, by one entry-block argument
per parameter and a store into the slot.

### The one rule a call has

`argumentMode(parameter contract, source kind)` in `inst.cpp` is where a signature becomes an
argument mode, and it is the only place. `Builder::addCall` takes the argument *values* rather than
operands, so a resolver cannot state a mode and therefore cannot state a wrong one; `verifyBody`
restates the same function over the built node, so the builder's assertion and a diagnostic in every
build are two halves of one statement. `resolve_ssa`'s conversion reads it a third time, to decide
what filling a parameter's storage does to the incoming value - which is how the two ends of one
handover cannot come to disagree about which it was.

### Which position each argument fills

`mapArguments` answers it, for a call and for a construction both, and the sharing is the feature
rather than a saving: positional then named, no position twice, and a position the site left out
taking its own default are one set of rules over two lists of names. The two callers differ in the
words their messages use - "parameter" against "field" - and in nothing else, which is what `ArgSite`
carries.

**A default is a `Constant` on the position**, `Arg::def` beside `RecordField::def`, and both go
through `declareDefault`: `doc/spec/functions.md` points a default argument at the field default's
rules rather than restating them, so the compiler has one function for the pair. It is a constant and
not an expression because there is no program point at which a declaration would run, and what a site
that omits the position builds is `addConst` - the same instruction a written literal reaches. Two
calls that both omit a position therefore hold two values, which is what makes an aggregate default
mean what it says when the constant resolver grows one.

**A declaration answers a *range* of call arities.** `Function::requiredArity` counts the positions
with no default and `writtenArity` is the whole list, and `Program::addFunctionName` files the
declaration under every count in between. So a call site still selects exactly one callee by a single
hash read - `doc/spec/overloading.md`'s one-function-per-name-and-arity is unchanged - and what a
second declaration collides with is a range instead of a point, which is what the conflicting arity
in that message is. `Function::acceptsArity` is the predicate every other "does this serve the call"
site asks, so a lookup and a match cannot come to disagree about a defaulted position.

**Evaluation follows the source and the operand list follows the declaration.** A named argument runs
where it is written and not where its parameter is (`doc/spec/expressions.md`), so `ArgMap` carries
both directions: `resolveArguments` walks the written arguments in order and drops each into its
parameter's slot. The defaults are built after all of them, which costs nothing to say - a constant
has no evaluation for an order to be observable in - and keeps the walk exactly the source order.

**A `@lazy` position takes a default too**, and what a call that omits one passes is not the constant
but a nullary function returning it - `defaultThunk`, cached on the parameter as `Arg::defThunk`. The
marker and the default say nothing about each other for the reason a default is a constant: there is
no expression, so there is nothing that could run at a moment the marker promised it would not, and
nothing to observe if the callee never forces it. Two things follow from the thunk being built rather
than lifted. The constant is made **inside** it, so nothing of the caller's frame is handed out of a
call; and it captures nothing, so the two refusals `liftFunction` makes - no lambda inside a lambda,
none in a generic body - do not reach it. That second one is the point rather than a detail:
`check(x)` inside a generic function is exactly what an assertion is, and a *written* `@lazy` argument
there is still refused.

What a deferred default is checked against is the type the position **promises**, read back out of
the `() -> T` that `declareDeferred` rewrote the occurrence into - which is the one step that a
position inherited from a class signature makes unavoidable, since it arrives already rewritten and
has no before to save.

One gap, stated where it is: a call through a function **value** takes no names, because `FunParam`
carries none and `doc/spec/expressions.md` makes them part of the type.

### Records, and what a type owes

A `data` declaration is a `RecordType`: a nominal type with a name, a module and an ordered field
list. Two declarations of one shape are two types, which is what nominal means and is why the
identity is the declaration rather than the field list.

A declaration with **several constructors** is the same record with the same field list, plus a
`RecordCon` per arm naming a *range* of it. That is the one decision sum types are built on: a field
index identifies its own constructor, so the ownership fold, the schema fold, the layout walk and
every projection keep working unchanged and a place path grows no downcast step whose only job is to
repeat what the index said. `RecordLayout` is what the three shapes are told apart by - `Single`,
`Enum` (several arms, none carrying anything, so the value is a number) and `Multi` - and it is a
property of the **declaration**, because a body written over `Outcome(a, e)` is projected before any
`a` is known and every instantiation has to agree with it.

Three rules follow, each in one place:

- the arms **share their storage**, so `fieldOffsetNative` lays each constructor out from one payload
  offset and the record is as wide as the widest of them;
- a take out of one arm's field **proves the discriminant** (`analyze_drop.cpp`), so at the same
  point every other arm's subtree is marked moved: what is left after `Kept(->g)` is the rest of
  `Kept` and nothing at all of any other arm. That is what lets the drop walk descend into a
  partially moved sum at all, and without it the surviving arms would be released as storage the
  take had just emptied. The one place it does *not* hold is inside an authored teardown for the
  sum, where nothing has proved which arm this is - so the walk does not descend there;
- two fields of a sum are **disjoint only inside one constructor** (`analyze_loan.cpp`), for the same
  reason read from the aliasing side.

And one is a language rule rather than a limit: a field of a sum is reached **by matching and by
nothing else**. `o.value` cannot be right for a value that turned out to be an `Exit`, so the
selector reports and a pattern is what proves the arm - which is also what makes the rules above
safe, since the only way to reach a payload is through a test of the tag.

`resolve_match.cpp` is the whole of the pattern half and is two things in one file. The **coverage
matrix** is Maranget's usefulness algorithm, and it answers three questions with one recursion: is
this alternative reachable, is the match complete once it is added, and if not which value is left
over. It runs before anything is emitted, which is what makes an exhaustive match need no wildcard -
the alternative that *completes* it carries no test, and a match completed by three nested patterns
is as complete as one completed by a `_`. The **emission** lays the tests out as control flow: each
branches to the next alternative when it does not hold and carries on in a fresh block when it does,
so an inner test runs only once the outer one has proved the payload is there. `armsValue` is the
block layout the alternatives share with `if` and with `&&`, and it is one construct rather than a
two-armed one used N-1 times precisely because a nested pattern's inner test has to reach the *next
alternative* and not the end of the outer one.

`test/resolve/Sum.yana` is the declaration half's fixture, `test/resolve/Patterns.yana` the pattern
half's, and `test/resolve/ConstructorMap.Pinned.yana` the corpus fixture `@value(n)` adopted.

What a record *owes* is folded once, by `own.cpp`, in a pass that runs between declarations and
bodies - after every `instance` is known and before any body can ask. `Ownership` is five
independent answers:

| Field | Question |
| --- | --- |
| `trivialCopy` | may a read of this duplicate it bit for bit? |
| `relocatable` | may the object be moved to a different address at all? |
| `explicitDrop` | may the compiler insert its teardown, or must the source write one? |
| `drop` | is there an effect at the end of its life, and is it authored or derived? |
| `reclaim` | is there storage to release, and is it authored or derived? |

Five, because every pair of them agrees on every type that exists today and the first type where two
disagree should change one field rather than split a flag half the compiler is reading. `relocatable`
is not `!trivialCopy` - a copy makes a *second* object and a relocation makes the *same* object live
somewhere else - and `explicitDrop` is not a `TeardownKind`, because it is orthogonal to both halves:
the type usually has a perfectly ordinary authored `Drop`, and what it withholds is permission to
call it unasked.

Three questions are asked *of* those fields and each has one home:

| Question | Function | Read by |
| --- | --- | --- |
| may a duplicate be made here? | `isTriviallyCopyable` | `readMode`, `verifyBody`'s `CopyTrivial` check |
| may the object travel? | `isRelocatable` | `verifyBody`'s `Take` check - the relocation sites |
| is this linear? | `isLinear` | `ssa::Conversion::conventionFor` |
| is its initialization followed? | `isTracked` | move paths, and whether a value gets an `Object` |

`conventionFor` used to ask `hasTeardown`, which is a fourth question that happens to have the same
answer for every type that does not use `@explicitDrop` - and the wrong one for the type the
obligation is *for*. An `@explicitDrop` type with nothing to run is linear and has no teardown, so
keying the linear rule off the teardown made exactly the values that must be consumed exactly once
into `Trivial` ones a pass may duplicate and discard. `OssaTester.cpp`'s "a type with an obligation
and no teardown" is what says it does not.

`drop` and `reclaim` are **two halves and stay two halves** all the way to `emitTeardown`. A record
with an authored `Drop` still has its members reclaimed, so an authored answer replaces the derived
walk *for its half alone*; folding them into one flag would release every member twice for the first
type that has both. Everything but a record answers a constant, and those constants live in
`ownershipOf` beside the kinds rather than in `own.cpp`, because "a raw pointer duplicates freely" is
a property of the kind and not of a fold.

**Which children a derived walk descends into is `tearsDownField`, and nowhere else.** A field in an
origin slot is a position the record reaches and does not own, so it takes no part in the teardown -
and three passes act on that answer: `classifyTypes` folds the ownership over the members,
`Lowering::emitTeardown` emits the walk, and `Summary`'s reachability collects the functions it
calls. Only the first read the contract, so `data Held {borrowed: s'Guard, owned: Guard}` classified
correctly and then ran the *caller's* `Drop` when it died. `ownsField` and `tearsDownField` in
`type.h` are the one answer the three now share; `test/resolve/loans/Teardown.yana` is what says they
still agree.

### Generics, and what a specialization is

Two structures and not one, which is the whole of what `gen.h` decides. A **`GenContext`** is what a
declaration binds while it is being read - its type variables, and, when later waypoints add them,
the requirements its signature declared and its body inferred. A **`GenSchema`** is the canonical
numbering derived from a *finished* context, and nothing may read a slot number before it exists.

The shipping stage keeps one structure and caches the numbering on it, discarding the cache whenever
a requirement arrives. That works only while nothing has already acted on a number: a slot number is
what emitted code *loads*, so a body that read slot 3 and then inferred a fourth requirement holds a
number that has silently come to mean something else. Splitting them makes that unrepresentable -
`freezeSchemas` is called once, from `resolveProgram`, after every body is in, and `slotOf` asserts
that it has been. What follows is that a body names a requirement and never a number.

**A type variable is a type kind.** `Type::Variable` is what substitution replaces and what instance
selection has to decline to answer for, and it belongs to exactly one context: two declarations that
both write `a` bind two variables, which is what makes a constraint relate two slots of *one* context
rather than being ambient metadata attached to a name.

**A generic record's instantiation is a record.** `Boxed(I32)` is a `RecordType` of its own with its
fields substituted, interned by `(declaration, arguments)`. What that keeps is every reader below it
written once against a plain record - the field walk, the schema fold, the ownership classification,
the native layout, projection, construction - rather than each learning to substitute while it walks.
It costs one record per argument list, which the interning bounds, and the shell is registered before
its fields are filled so that a recursive generic reaches a handle rather than substituting forever.

**A specialization is not a copy of the body.** `specialize.h`: it is a `Function` with a substituted
signature whose `mir` handle is the *generic's*, and the SSA conversion reads that one body through
`substitutionOf`. So there is one resolved body, one ownership analysis and one set of drop points,
and Implementation-Generics.md §1's invariant - *adding or removing a specialization may not change
which instance was selected, which conventions were chosen, or when a value is dropped* - is a
property of the representation rather than a rule a pass has to keep. `verifyProgram` and
`analyzeProgram` skip a specialization for exactly that reason: there is nothing of its own to check
or to place.

What a specialization *is* allowed to change is whether there is any code at a point. An
unconstrained `a` is classified as owing a teardown (`ownershipOf`, `Type::Variable`) because some
substitution does, so the generic body places a drop for it; `emitDrop` then emits nothing where the
substituted type has none. `test/resolve/generic/Records.yana` is the fixture: `discard(I32)` and
`discard(Guard)` are the same placement and different code, and `trace` is what says the second ran.

**A call names the generic, not a specialization.** Which one a call reaches depends on the
substitution the *enclosing* body is being compiled under - `middle`'s call to `smaller` is decided
by `middle`'s own specialization - so the node carries the callee and its type arguments and
`callTarget` is the one function that turns the pair into a callee. Reachability, the summary walk
and the conversion all ask there; three passes that worked it out separately would be three answers
that agree until one is changed. The specializations a program needs are therefore *discovered* by
the reachability walk, which is the walk that already had to know which functions the program calls.

**Type arguments are inferred and never written.** `matchType` matches a declared type against what
stands opposite it - one-way, structural, and shared by a call's arguments, a constructor's fields
and an *instance head*, so no two of them can come to disagree about what `Boxed(I32)` is inferred
from. Three things decide a variable, in this order: what the arguments bound, what the **result
position** expects (`fn empty() -> Maybe(a)` called where a `Maybe(I32)` is wanted), and what the
signature's own constraints determine. A variable none of them decides is reported rather than
guessed.

**A functional dependency is what makes a constrained signature usable at all** - `fillDetermined`.
`fn (Contiguous(c, a)) first(xs: c) -> a` binds `c` from its argument and has *nothing* to bind `a`
with, so without `class Contiguous(c -> a)`'s promise that one `c` has one `a`, the call reports
"cannot infer `a`" and every such signature is unwritable. Two things can answer, and they are the
same two `dischargeRequirement` splits on: a concrete deciding prefix selects an instance and the
determined arguments are read off its head; a prefix that is one of the *enclosing* declaration's
variables has no instance to read anything off, and what answers instead is the requirement that
signature declared. Repeated until nothing moves, so a chain of dependencies resolves whatever order
the constraints were written in.

### Classes, and what selection is

A class is an **argument list** and an instance head is a **pattern** - `select.h`. Selection is a
match above a table rather than a lookup into one, and the three things that keeps bounded are worth
naming, because each is what a solver would otherwise have been:

 - **The head's outer constructors are the key.** `instance (Eq(a)) Eq(Array(a))` is filed under
   `(Eq, Array)`, so a lookup narrows to one candidate before anything is matched - which is what
   keeps coherence a property of a hash table, since two heads of one shape collide at the
   declaration.
 - **An open head is tried second and there is at most one per class.** `instance (TrivialCopy(a))
   Copy(a)` names no constructor and matches everything; a keyed head is strictly more specific, so
   the order *is* the specificity order rather than a preference anyone wrote.
 - **The head's own context is a goal, not a filter.** A matched head whose `(Eq(a))` does not hold
   is reported rather than passed over - passing over it turns one missing instance into "no instance
   of `Eq(Array(Guard))`", which names a type nobody wrote.

**A parametric instance's member is an ordinary generic function over the instance's own context.**
That is why a head becoming a pattern needed no second mechanism below this line: what runs for
`Eq(Array(I32))` is that member specialized at what the head bound, which is the same sentence a
class default already obeyed with the class's context. Nothing is cached on the `Instance` any more -
one instance serves every argument list its head matches, so a member slot on it could only have held
the first of them, and `specializeFunction` interns at the granularity the answer actually has.

**The classifier asks a different question from selection** - `declaredInstanceFor`. `Copy`, `Drop`
and `Reclaim` each *subtract* a capability, and `doc/spec/ownership.md` places a removal in the module
that declares what it is removing from. A keyed head does that; an open head is a statement about
every type there is, including ones already compiled against the derived answer, so it may not be
what takes `trivialCopy` away from one of them. Selection is therefore the wrong question there and
"what did somebody declare *about this type*" is the right one.

**A class member is a generic function whose context is the class's.** That one decision is what
makes a class call and a generic call one mechanism rather than two. `size(x)` finds the *class's*
declaration of `size` by name, `resolveArguments` binds the class's `a` from what was passed exactly
as it binds a callee's, and `callTarget` turns the pair into a callee - by selecting an instance
where a plain generic would be specialized. Reachability, the summaries and the SSA conversion all
ask that one function, so no pass can come to a different answer about which body a call reaches.

**A class member may be a `lens fn`, an `iter fn`, or take a `@lazy` parameter**, and reaching one is
the ordinary call above with nothing added. Three things had to be said once for that:

- **what the continuation answers.** A module-level lens leaves it a variable its call site binds; a
  class member has no context to bind one in, so it is fixed to the exit signal - which is what every
  call site was binding it to anyway, and is the one thing a witness slot could not have held, since
  a result the *caller* chose is not a shape a table row has. `declareContinuation` states the three
  reasons a traversal fixes it in one place.
- **who states the protocol.** An instance member takes the hand-over and the failure channel from
  the class and is *compared* against them; what it does not do is re-derive what the function
  answers, because `declareOutcome` synthesizes a record per skipping traversal and two derivations
  of one protocol would be two records with the same fields - one the call site is typed against and
  one the callee returns. The kind itself is compared before the signature is declared, since a
  declaration of the wrong kind has no protocol to inherit.
- **the member key.** A lens's continuation is a parameter no source writes, so a member is filed and
  looked up under its `writtenArity()`.

`@lazy` needed none of that - the marker is spent on the position at the class - beyond travelling
with an inherited one: a member that wrote no type for a parameter takes the class's, and the marker
is part of what that is. What it does *not* reach is a thunk built inside a generic body, which is
the closure refusal above; handing a deferred parameter on through a dispatch is fine and spends the
same one permission. Fixtures: `lens/Class.yana`, `lazy/Class.yana`, and the two `Reject.Class.yana`.

**A class requires itself of its own parameter.** Written into the context by `declareClass`, and it
is not a tautology: a body inside the class - a default - is a generic body, and what it may dispatch
on is what its environment says. With it, `fn (Describe(a)) present(x: a)` and a default of
`Describe` are discharged by one rule; without it, the rule would have needed a clause about class
members. Superclasses go in the same list for the same reason, which is why a default may call a
superclass's member on the same subject.

**A requirement has two answers and deliberately no third** - `dischargeRequirements`. If the type
the call chose is concrete, the instance has to exist *at the call*. If it is one of the enclosing
declaration's variables, the enclosing declaration has to require it, and the obligation is passed on
rather than proved. What is not a third answer is looking in the instance table for a *variable*: a
body that did would be answering a question about its own type parameter from whichever instances
happen to be declared, which is exactly what makes a specialization mean something different from the
body it came from.

**A default is a generic body, and an instance that leaves the member out gets it specialized.** So
a default and a hand-written implementation are the same kind of thing by the time anything calls
one: one function, one signature, one body. `instanceMember` is where the specialization is made, on
demand, because at declaration time the default's body does not exist yet and a specialization made
then would share a null.

**Defaults have to bottom out**, which is `checkClassDefaults` and the one thing a class declaration
cannot be asked until its defaults have bodies. `one` written as `two` and `two` written as `one` is
a class every instance satisfies without writing either, and selection then finds a body at every
step of a program that recurses until the stack ends - accepted silently until the walk was added.
The question is whether the "this default calls that member" graph is acyclic once the signatures
with no default are removed, since those are where every chain stops, and a three-colour walk answers
it. The current compiler computes a rank per member and compares; the two answer the same question,
and this one has nothing to store on the declaration afterwards.

### `intrinsic/` - the compiler's roles over this IR

Everything the source language and the compiler have to agree about is declared in
[`compiler/intrinsic`](../intrinsic/README.md) - a leaf beside `YanaCore` that more than one stage
reads. What is *here* is this stage's half of it, and it holds no names:

| File | What is in it |
| --- | --- |
| `intrinsic/roles.h` | the claim table `Program` holds - a role to the declaration that filled it |
| `intrinsic/link.cpp` | the one reader of `@builtin(role)`, and every check made of a claimant |
| `intrinsic/expand.cpp` | the roster: one arm per `op.def` row |
| `intrinsic/expand_*.cpp` | the emitters, grouped by family |

**An intrinsic call is an ordinary call up to the last step.** Name resolution, arity, inference,
constraint discharge and *argument preparation* all run exactly as they do for a callee with a body;
the role decides only which node `resolveCall` builds instead of `addCall`. So an emitter receives
values already read under the mode `argumentMode` answered for the declared contract, and cannot
apply a convention by hand because there is none left to apply.

That is not a tidiness argument. The current resolver's `expandIntrinsic` applies none of the
declared conventions and lets each emitter decide, which is how `Atomic.intoValue(->self: Atomic(a))`
came to consume nothing: three emitters applied the handover by hand with a comment saying so, and
the fourth did not.

The other half is `op.def`'s shape, checked by `checkRoles` before any body is resolved. A signature
with no body *is* the signature - nothing duplicates it - but an emitter necessarily assumes things
about it, and those assumptions were unwritten. `store`'s `->value` is the live example: the pointee
holds the value afterwards, so a declaration that wrote a plain parameter there would leave the
teardown obligation with the caller and the value with the pointee, with nothing to say so.

### The `Drop`, `Reclaim` and `Copy` classes

These three are **declarations too**, and that is what G3 changed about them. `class Drop(a)` is
synthesized where nothing declares it - this stage does not build Core - so `instance Drop(T)` is
checked against a class by the one rule every instance is checked by, and the hand-written shape test
per enumerator that used to do it is gone. A `Known` role survives as a *tag* on the declaration,
which is what the ownership classifier keys off; when this stage reads Core, the declarations there
claim the roles with `@builtin` and nothing is synthesized.

They were recognized by *name* until the role tables existed - a declared class's name compared
against three spellings written beside the enum. That is the coupling `compiler/intrinsic` exists to
remove, and these three were its first rows: the compiler now holds no library names at all, and a
class says which role it fills where it is written. `Presence::Synthesized` on those rows is what
keeps a program with no standard library compilable, and the two names a stand-in is built from - the
class's and its member's - are columns in `known.def`, because they are what a program *writes*
against the stand-in.

A synthesized class belongs to no module, which is what makes it visible from every one: `Drop` is
written in the module that declares the type, and there may be several. `lookupClass` and
`lookupCoreMember` are the two places that say so, and both are standing in for an ordinary import.

`Drop` and `Reclaim` differ from `Copy` in exactly one way that is not about ownership: their members
are **not** callable by name. Which of the three that is is the role's own `teardown` column and not
a comparison against a list of enumerators, which is what it replaced - `drop || reclaim` written out
in four places, each of which would have had to be found again the day a third ownership class
existed. A teardown runs because a value died, so there is no call to write, and
registering `drop` would both put it in the module namespace once per class that has one and let
source trigger a disposal the ownership analysis is answerable for placing. `copy(x)` is an ordinary
class call and dispatches like any other.

### Linear types, and where each rule lives

Three mechanisms and a rule each, and none of them is a construct the IR knows about:

| Mechanism | Where it is decided | Where it acts |
| --- | --- | --- |
| `@explicitDrop` | `readAttributes`, folded by `classifyTypes` | `analyze_drop.cpp`, at every point a drop would have been placed |
| `->x` | `resolveDispose` | nowhere - it is a read into a temporary, and the ordinary placement does the rest |
| `@local` | `readAttributes` | `reachesInside` for a `data`, `disposalPermitted` for an `instance` |

**Through a type variable, both are asked at the instantiation instead**, which is waypoint G4 and
`doc/spec/ownership.md`'s own rule: "a question with a universal fallback is answered once in the
body, and a question with no fallback has to be asked at the instantiation". `@explicitDrop` and
`@local` withhold something and supply no replacement, so there is no conservative answer a generic
body could be compiled against - `a` has no attribute and no instance, and both checks above pass
whatever a caller later substitutes. `analyze_drop.cpp` therefore records a `Disposal` on the
function wherever it chose the end of a value whose type mentions a variable, and
`specializeFunction` substitutes the type and asks the two questions there, once per argument list.

Without it a generic function laundered both: `fn discard(->x: a): return` called at an
`@explicitDrop` type is exactly the implicit disposal the attribute exists to refuse, and neither
stage reported anything. `test/resolve/generic/Ownership.yana` is the half that passes -
`->x` written in the generic body, so the source chose the point and any type may travel through it -
and `Reject.Removal.yana` the half that does not.

`->x` having no representation is the point rather than an economy. It reads the value under
`bindingRequest(Access::Owned)` and writes it into a temporary that is dead from the next position
on, so "the teardown runs here" is the sentence the drop placement already says - which is why a
disposal runs an authored `Drop`, a derived walk and nothing at all without knowing which of the
three it is. `Local::disposal` is the one bit that survives the desugaring, and it exists because the
diagnostic has to tell "the compiler chose this point" from "the source did".

**The subject of a teardown has move paths and is never dropped.** `Analysis::teardownSubject` is
that pair. It used to have no path at all, which cost a partial move out of the subject - the shape
`fn drop(->value: Pool): ->value.held` needs - and left the rule an authored `Drop` takes on by
existing unenforceable: its subject's linear members are the body's to dispose of, and a body that
forgets one would otherwise leak it in silence. `nameLinearMembers` gives those members places at
resolve time, since a member the body never mentions interns no place and a path exists only for a
place that does.

`Function::teardown` marks the member of a *teardown* instance, and it exists for a single reason:
the subject of a teardown is the one owned local in the language this frame does not owe a teardown
for. Without the flag, `instance Drop(Guard)`'s `->value: Guard` would be released by the ordinary
rule - which is a call to the function the body is in, on every path through it.

**`Copy` is in the table for what it subtracts rather than for what it calls.** An authored
`instance Copy(T)` sets `trivialCopy` false, which is `doc/spec/ownership.md`'s "an authored `Copy`
subtracts `TrivialCopy`" and the same subtraction `Drop` and `Reclaim` already make through
`needsTeardown()`. Without it the instance is dead: every default and `->` position keeps duplicating
the bytes and the authored body runs only where somebody writes `copy(x)` by hand, so whatever
invariant it maintains is broken by copies the author cannot see.

Two consequences are worth naming because nothing else in the stage has produced them before.

- **The member is checked and not called.** `copy` is dispatched by a class call this stage has no
  solver for, so `resolve_class.cpp` verifies the signature - `fn copy(from: T) -> T`, the subject
  shared and the result owned - where it is written rather than at the milestone that finally reaches
  it. `Function::teardown` stays false for it: it borrows its subject and the caller's original
  outlives the call.
- **The head must be a record.** All three of these instances take a capability away, so the head
  decides *whose*. The orphan rule places `instance Copy(MyType)` in `MyType`'s own module, which is
  where a removal has to be visible; a structural head is orphan-legal by what is inside it and would
  subtract from a type somebody else declared, so it is refused.

The type this produces is the first in the stage that is **tracked without being linear**: no
teardown, so `analyze_drop` emits nothing for it, and its uses are still moves so that a use after
one is a diagnostic. That is what `isTracked`'s `|| !trivialCopy` term is for and what nothing
reached before. `test/resolve/ownership/AuthoredCopy.yana` is the accepted half - `Counter` and
`Plain` differ only by the instance, so the goldens are the assertion - and
`Reject.AuthoredCopy.yana` and `Reject.CopyInstance.yana` are the refused ones, split because
resolution stops before the analysis runs.

**The tier below has a convention for exactly this cell**, and did not at first. `conventionFor`
asked `isLinear` and sent everything else to `ssa::Convention::Trivial`, whose contract is *freely
duplicable* - so the rule the instance exists to state was carried by MIR and dropped one tier down,
and an optimizer could make a second `Counter` with the verifier's blessing. `Affine` is that cell:
consumed at most once, abandoned freely, never duplicated. See `resolve_ssa/README.md`.

### Move paths, and the analysis

`analyze.h` is one entry point over three readings of one structure, which is the point of putting
them behind one:

- `analyze_path.cpp` builds the move-path tree: the place tree restricted to field projections, with
  an index or a dereference collapsed to its container. A path exists because the body *names* it, so
  a record used only as a whole has one path however many fields it has.
- `analyze_loan.cpp` is the origin environment and everything read off it - see below.
- `analyze_live.cpp` is backward liveness, and with it resolves every `UseMode::Forward` into the
  `Take` it was or the diagnostic it is. This is the one rewrite a sealed body permits, and it is why
  `Forward` exists (§5.4).
- `analyze_drop.cpp` is the forward initialization dataflow and the drop placement, which are one
  walk because they are one question asked at each position: what is owned here, and is it still
  wanted?

Drop placement is **one sentence** - *an object is released at the first point at which it is owned
and no longer live* - and the four cases a reader expects to find written out are that sentence
meeting four control-flow shapes. `test/resolve/ownership/DropOrder.yana` asserts all of them by
counting. A pass with a rule per shape is how the current compiler's drop placement grew to four
files.

Two rules are the inverse of each other and both have to exist. Moving a field marks every ancestor
`Moved`, because a record with a hole in it is not a record; writing a field back marks an ancestor
`Owned` again once *every* tracked child is, because a record put back together is one. The second
was missing at first, and a record could be taken apart and reassembled and still not be handed over.

`analyze_drop.cpp` is also where the two things the walk rewrites in a sealed body happen: `Forward`
above, and `Destination::Assign` becoming `Reinit` where the destination holds nothing. Whether the
old contents are there is a dataflow answer - the same `a = e` inside a loop is a refill on the first
iteration and a replacement on the second - so it cannot be decided when the node is built.

### Objects, and what a lineage is for

`ObjectId` sat in `region.h` unused while placement was a `StorageClass` on a storage slot. That is
one defect seen from two sides: the identity existed and the thing it was the identity *of* did not,
so the relation it was supposed to carry - which producer's value ended up in which local - would
have been reconstructed from producer/local pairs by whichever pass first needed it.

An `Object` is one thing whose teardown is counted and whose placement is decided, and two rules make
it an identity rather than a label:

- **moving preserves a lineage.** `let b = a` does not make a second object; it makes one object
  reachable through a second name.
- **copying starts a lineage.** A duplicate is a second object with its own answers.

The representation is a union-find over `Body::objects`, which is what makes "preserves" a *merge*
rather than a relation somebody has to keep transitive. `Builder::mintObject` is called by the
producers - an aggregate, an owned call result, an owned block argument - and `uniteObjects` by the
two places a move happens: a write that takes its value, and an edge argument that hands an owner to
a successor's parameter. A local then has exactly one object however many moves reached it, and
`storageOf` is the single question "where does this live", asked of the lineage. The MIR dump prints
`objectN` on a local only where two or more share one, because that is the part a reader cannot get
from the code - `rebound()` in `Baseline.Drop.yana` is `let a = open(7); let b = a`, and both locals
print `object0`.

What a component of an aggregate contributes is deliberately still open: taking a field into a record
is the same object at a new address, and whether that is a merge or a containment is §10.4's interior
placement question. `addAggregate` is where it says so, which is where it will be answered.

### Loans, and the one structure behind them

A `Loan` (`loan.h`) is a place and a capability, minted **only** by `Builder::addRead` and named by
the read that made it. Everything §6.4 asks for is read off one table:

`Analysis::computeOrigins` builds §5.11's origin environment - for every value, and for every local
**at every program point**, which loans it may be rooted in - as a forward fixpoint. A borrow
inherits its lender's origins; a record inherits its components'; a call result in a published slot
inherits whatever filled that slot; a block parameter inherits the union over its edges; and storage
holds what was last written into it. The other four answers are all this table:

- **An extent is a live range**, not a scope: `computeLoanExtents` is the same table read backwards,
  so a loan lasts exactly as long as some value rooted in it is still wanted. A loan bound to a name
  therefore lasts to the last use of the *name*, and one made in a loop body is dead at the back
  edge, with no rule for either shape.
- **Overlap** is `placesOverlap` - walk both places to their roots, disjoint if the roots differ or a
  field index does - against the loans live *after* the access, which is what makes a loan not
  conflict with itself.
- **A reborrow** is a loan whose origin set contains the one it reborrows, so §2.5 is a `skip`
  argument to the overlap check rather than a rule beside it.
- **Conformance** is `checkReturnConformance`: walk each returned loan to its place's root and ask
  whether the signature published it. A global satisfies any slot, a parameter in the declared slot
  is what was promised, and the three ways it can be neither get three different messages, because
  they have three different fixes.

`forEachOperandOrigin` is how all four passes ask - the environment, liveness, the extent walk and
the overlap check - and it answers for a *place* operand as well as a value one. `idOf(d)` names a
value and `return d.n` names a place, and a rule that saw only the first ended the loan behind `d`
between them. It takes the point the operand is read at, and that is not bookkeeping: storage answers
differently at different points, and a caller with no point to name would be asking for the union
again.

**Storage is flow-sensitive, and a replacing write is where that is decided.** `replacedLocal` is the
one condition: a write to a whole local this frame *owns* discards what was there, so the forward
transfer replaces the local's origin set rather than adding to it, and the backward extent walk stops
counting that destination as a use. Refilling a reader from a second lender therefore ends the first
lender's loan at the assignment - `test/resolve/loans/Refill.yana`. A partial write and a write
*through* a loan are neither: `r.at = 3` reaches its value through `r`, and `d = box(2)` on a `&`
binding reaches `b` through `d`, so both remain uses and both union - `writeThroughKeepsLender` in
`Reject.Overlap.yana` is the half that says so.

**The environment is indexed by position, not by value** - §5.11's model is
`program point x slot -> origin set` and both axes are there. A value or a local has one origin set
for its root and one for each interior slot its type's `OwnershipSchema` names, so
`data Two {left: x'Guard, right: y'Guard}` keeps two relationships and reading `t.right` says nothing
about `t.left`. `originPositions` is how many a given occurrence has and `selectPositions` is the
mapping a projection makes: the root of the local always reaches, a slot the projection walks *into*
lands on the root of the value that comes out, and a slot it stops above survives as a slot with the
projection taken off the front of its path.

The root is a position even where the occurrence is owned, and is then always empty. It costs a row
per origin-carrying local and it buys the property every one of those mappings relies on: a place and
the value read out of it have the same position count, so a whole read moves position `i` to position
`i` and only a projection renumbers.

This is the axis a generic borrowing record, a yielded value and an external iterator handle all
need, because distinct result slots have to keep distinct lifetime relationships - and it was a
documented false rejection until it existed: `separateLenders` is an accepting case in `Schemas.yana`
now and was a golden error in `Reject.Overlap.yana`.

It was *not* the reason the old state table was 819 MB, which is what this used to say. A dense
bitmap costs what its dimensions are and not what its contents are: splitting one set into one per
slot changes which loans are in a set and cannot remove a row, and the measured table held **one**
loan in every non-empty cell. So the representation below came first and made room for it - the slot
axis doubles the local state's rows on a body of readers, exactly as predicted. What was *not*
predicted is that it shrinks `valueOrigins` by far more: that table was a row per **node**, and most
nodes - a constant, an add, a branch - cannot be rooted in anything and now have an empty range
rather than a row of zeros. See `test/bench/findings.md` §74 for both numbers.

**Liveness reads the environment too.** A use of a value is a use of the storage behind every loan it
may be rooted in, which is what keeps a lender alive across the call that borrowed it. Milestone 4's
interim rule - a use of a borrowing read's result is a use of that read's place - was correct for
exactly the one-instruction case and is gone.

### What the analysis costs

The four fixpoints here are over the same graph and are shaped the same way, deliberately, because
they are the same walk asked four questions. The shape is worth stating because the first version of
it was not: it was rounds over every block until nothing moved, over tables of one byte per column,
and both halves are visible on a body a compiler will meet.

- **Every table is bits** - `BitMatrix`, `util/container.h`. A row is the loans one value may be
  rooted in, or the paths live at one block, and every operation over one is a word at a time.
- **Every fixpoint is a worklist over reverse postorder** - `sweepForward` and `sweepBackward`. A
  block is visited when something it reads has moved and not otherwise, and a sweep in the right
  order picks up everything woken ahead of the cursor within the same sweep, so an acyclic region
  settles in one and a loop in one per level of nesting.
- **The local origin state is not tabulated at all** - it is `localIn` per block and an undo log
  between, walked by an `OriginCursor`. `stepOriginsForward` replaces one row and leaves the rest of
  the state alone, so consecutive points differ in one row of `loanWords` words; storing the whole
  state at each of them stored the same rows over and over. A forward walk needs no log, because
  stepping forward *is* the transfer; a backward one puts back the row the log kept. Every walk that
  reads the state is per block and in one direction, which is what makes a cursor the same thing as
  the table was.
- **The overlap walk expands each loan once per state.** What a loan reaches through the origin
  environment is a transitive closure, it is the same closure for every comparison made at one point,
  and `checkLoans` rebuilt it per comparison. It is now built once and every comparison is two
  word-at-a-time tests against it - and once per *state* rather than once per node, keyed on
  `OriginCursor::revision`, because the closure is taken from the state and from nothing else.
  Keying that cache on the node while the state under it moved a few dozen times in the body is what
  §72 found.
- **The overlap set is computed for the loans that can be asked about**, which is the live ones and
  what they reach - not for every loan in the body. `placesOverlap` walks two projection chains, and
  the loan count is not what a point holds but what the *body* mints: every argument handed to a
  borrowing parameter mints one, and they are dead a node later.

What licenses the worklist is `checkValueScope` in `verify.cpp`: an operand names a value of its own
block, so the only ways one block's answer reaches another are its exit state and its successors'
parameters - which are the two things `computeOrigins` wakes on. The representation permits more
(`checkOperand` holds an operand to naming an *earlier* node, which is an ordering statement), so it
is checked rather than assumed.

[§71 of the benchmark record](../../test/bench/findings.md) has the numbers, and the shape that makes
them visible: a body with three hundred simultaneously live borrows took 1.28 s and 1.6 GB before
this and 0.03 s and 10 MB after. §72 is the same walk along the three axes §71 did not grow -
`test/bench/ownership-scaling.py` generates all four - and it found the last two entries above: a
chain of sixteen `if`s over 320 borrowed values took 45.5 s and takes 2.0 s.

§73 is the bound those two left open, and it was the state table itself: `rows x origin locals x
ceil(loans/64)` words is a product of three terms that all grow with the program, and a `let` over a
record with an external field is one of each at once. 640 origin-carrying locals written four times
each was 819 MB of it - cubic in the locals, and past what a `RegionPtr` can address at about 2600
of them. The cursor is what closed it: the same body is 1.3 MB of log, and shape A's peak went from
842 MB to 62 MB with nothing in the corpus moving.

`testOriginStateBeyondARegion` in `test/OssaTester.cpp` is the case that keeps it closed. It
generates 2800 readers, recomputes what the old table's three factors would have come to - 2.76 GB -
and asserts that past `LinearArena::kReserve`, which is the 2 GB a region can address at all. A
bound stated as a number somebody picked is a bound that drifts; this one is stated as the
representation's own ceiling. It also asserts that the body's origin *positions* outnumber its
origin-carrying locals, which is what says the case exercises the slot axis rather than agreeing with
the old shape by accident.

§74 is the slot axis measured, and it is the section that reverses one of §73's expectations. The
precision costs the factor §73 predicted - the local state's rows double on a body of readers - and
removes a larger one nobody was looking at: `valueOrigins` was a row per node and is now a range per
value, so the shape whose `checkLoans` §72 fixed went from 254 MB to 122 MB on top of everything
above.

### What a position asks for

`Request` (`contract.h`) is what a position wants of the value that arrives in it: an access, and
whether it published an origin. Deliberately **not** a `Contract`, because a contract describes an
occurrence that exists and `let x = c` has no contract until the initializer says what `c` is.

`readMode(base, place, Request)` is the four answers, and it is `doc/spec/ownership.md` read from the
destination:

| The position | The place | The answer |
| --- | --- | --- |
| unpublished, and the type is `TrivialCopy` | anything | `CopyTrivial` |
| exclusive | writable, and not shared storage | `BorrowUnique` |
| shared or published | storage this frame does not own | `BorrowShared` |
| owned | storage this frame owns | `Forward` |

That one function is what makes `let x` a shared loan of what it names, `let &x` an exclusive one and
`let ->x` a handover, without any of the three being a case: the two axes of a binding are a
`Request`, and what the binding *becomes* is the initializer's answer.

### Records that hold a loan

A field declared `'T` or `&name'T` makes the record's interned `OwnershipSchema` non-empty - `own.cpp`
builds it, and `RecordType::schema` is where it lives. `Builder::fieldPlace` reaches through such a
field with a `Place::Deref`, and `resolve_ssa` lays it out as a pointer rather than as the object.

A record's slots are numbered within its own field list and a function's within its signature, so
they are **separate scopes**: the `a` in `data Reader {held: a'Guard, ...}` and the `a` in
`fn f(g: a'Guard)` are unrelated names that happen to be spelled alike. What crosses the boundary is
the origin set, not the name - which is what makes handing a schema slot on to a function's slot, or
into a second record, an ordinary reborrow.

**A use site can bind those slots, and that is `Reader(a')`** - Analysis-Borrows-Revised.md §2.3's
loan argument on a type application. It fills a slot rather than a type parameter, so `Reader(a')`
*is* the type `Reader` with the same interning, layout and instances, and what the argument supplies
is which of the *signature's* origin sets fills each of the record's interior positions.

Which makes the two scopes above a renumbering, and that is the whole implementation.
`resolveQualified` copies `Reader`'s schema slot for slot - same path, same capability - replacing
each record-scope slot number with the one the argument named, and interns the result as
`Occurrence::shape`. Both shapes live in one table, because they are one structure read in two
scopes. `shapeOf` is where anything asks what is inside a value and prefers the occurrence's answer
to the type's; `originSlotAt` is where the difference between them is spent, and it has three cases:

| Position | Answer |
| --- | --- |
| the root | its own contract's slot |
| an interior position the occurrence **bound** | that slot's contract, in this scope |
| an interior position nothing bound | the root's answer |

The third is conservative on purpose rather than absent. `x: a'Reader` says the reader is in `a` and
says nothing about the guard inside it, so everything reachable through `x` is treated as `a`'s too -
the alternative credits a result derived from `x` with the reader's extent and not the guard's, and
drops the guard while a borrow of it is live. `kNoOriginSlot` is therefore reached only where the
*root* names nothing either, which is an owned occurrence whose interior the signature declined to
describe, and is exactly what `checkReturnConformance` refuses.

`test/resolve/loans/Applied.yana` is the grid: an owned result carrying an origin, the same
relationship forwarded through a second signature that spells its origin differently, and two slots
filled crosswise so the mapping is a permutation rather than the identity it is with one.

Still refused by name: an *owned field* whose type has a schema. `data Outer {inner: Reader(a')}`
would give `Outer`'s own schema `Reader`'s position at a deeper path, and `foldSchemas` walks the
records flat rather than on demand, so the fold order is what is missing rather than the spelling.

### Where places come from, and why all of them

`Builder::fieldPlace` interns **every** field of a record that owes a teardown, not just the one
asked for. A drop is placed on a path, a path exists because a place names the storage, and a body
that moves `p.left` and never mentions `p.right` therefore had nowhere to put the survivor's
teardown - and leaked it silently. Interning the set in the builder rather than materializing the
missing ones in the analysis keeps places something only the builder makes, and keeps the sealed body
the analysis reads exactly the body the verifier checked.

### Drop flags are built and dormant

`Elaboration::flags`, `FlagStep`, the `Maybe` state and the conditional arm in `emitDrop` are all
implemented per §6.7, and **nothing in this language subset reaches them**. The argument is short: a
drop sinks to the earliest point on each path at which the value is dead, so a value moved on one arm
of an `if` is released on the *other* arm, before the join, and is never `Maybe` at a drop point; and
a value that is still live after a conditional move is already a diagnostic. A `drop.maybe` line in a
MIR golden is therefore news. It is built now rather than later because the machinery is what makes
the `Maybe` state mean something, and a lattice with an unreachable element is easier to keep honest
than one that has to be added back.

### Continuations, and what a lens call is

Part 11.4's slice 7: a transparent `lens fn` in the `yield` form, called in either of the two forms -
scoped (`let x = f(...) in: ...`) or flat (`let x = f(...)`, with the extent derived; see below) - and
an `iter fn`, whose continuation is a `for` body and which is the same construct with one flag on it
(see below). A lens is an ordinary function here - `declareContinuation` is the whole of the
desugaring -

    lens fn f(a) -> P        declares      fn ($r) f(a, body: (P) -> $r) -> $r

- so its body is resolved once, its diagnostics are reported once, a lens nothing calls is still
checked, and a lens may call itself and may be generic. `yield e` is a call of that parameter, and
what makes the continuation safe to hand over is what a shared `Origin::CallBounded` parameter
already means to every pass here: `checkCarriedOrigins` refuses it at every owned position, so the
callee cannot store it, return it, or hand it to anything that outlives the call.

**`$r` is a variable and not the exit code it is currently always instantiated at.** What a
continuation answers with is a manifestation choice, so the *call site* binds it; making it a
variable is what keeps this declaration and milestone 6's explicit-callback form one shape, and it is
where a skipping lens's `Try` result and the flat form's block value go. An `iter fn` is the one that
fixes it at the declaration, and for a reason rather than for convenience - see below.

The **call site** is where §10.1 is met, and it is met here rather than in the conversion:

    ...arguments...                  the caller's own region
    lens.call f(...) -> bb(entry)
  entry:  %h = argument              what the callee hands over
    ...the extent...
    jump bb(join)                    falling out, which is one of the region's exits
  join:  the caller, again

The continuation is the caller's code under *either* lowering, so its four exits - falling out,
`return`, `break`, `continue` - are ordinary CFG edges that liveness, the loan extents and drop
elaboration walk. What the CFG cannot say is which blocks are the continuation and where control
comes back, and `mir::Continuation` is what says it:

| Region | Whose blocks | Entered at | Rejoins at |
| --- | --- | --- | --- |
| `Body` | the function's own | the entry | nothing; it returns |
| `Handover` | the caller's continuation | the `lens.call`'s successor | the block the call returns to |

Nesting is what makes a lens inside another's continuation work with no rule of its own, and the
nesting rule itself - an edge stays in a region, enters a *direct child at its entry*, or leaves for
the destination that child declared - is checked in `verify.cpp` and again in `ssa::verifyBody`.

**Which lowering a call gets is `resolve_ssa/convert.cpp`'s choice and is invisible here.** Today
there is one: the region is outlined into a function and the callee is called with it. See that
directory's README for the exit signal and the environment.

Three things follow from the callee being **opaque**, and each is a rule that was not written:

- **A lens's frame storage is ordinary storage of an ordinary function**, released at the first point
  at which it is owned and no longer live - milestone 4's one sentence, with nothing added. Where the
  hand-over really has to outlive the call, the **loan** says so: the continuation reads through a
  borrow of it and a borrow's extent covers the call it was granted for. `test/resolve/lens/Frame.yana`
  counts both halves, and its `ordering` is the case that separates them - it hands over a *copy* of a
  field, so nothing borrows the guard and it dies before the continuation runs.
- **A borrow handed over is checked against what the signature declared.** `Continuation::handover` is
  that declaration, and it is what the region's entry parameter carries, because it is what a caller
  has instead of the callee's storage.
- **There is one argument loop.** `resolveArguments` is shared by the ordinary call and the lens call,
  which is the thing Analysis-Lens.md §12.1's sixth item asks for by name: the shipping stage has two,
  and rules added to one of them went missing from the other twice.

`checkHandover` is the transparent rule (Analysis-Lens.md §7.1) as a forward walk over the callee's
own blocks, **once per lens** rather than once per call site: handing over on one arm of an `if` and
not the other is a *skipping* lens, which needs the `Try` instance milestone 6 has, and handing over
twice would run one continuation twice.

That walk is `walkOnce`, and it is shared with `@lazy`'s at-most-once rule. Two bits per block - a
path reached it before the event, a path reached it after - and the callers differ only in which node
*is* the event: a write into `$yielded` for the hand-over, a read of the parameter's storage for the
force. "Exactly once on every path" and "at most once on any path" are two readings of one answer,
and a count of the events in the text is not either of them.

### `iter fn`, which is the same construct with two things settled

`Function::iterator` is set beside `Function::lens`, and everything above is written once: the
declaration, the hand-over position, the continuation parameter, `resolveArguments`, the region, the
binding, the rejoin. `handoverCall` is that shared half and `inside(region, join)` is the only thing
a lens call and a `for` disagree about - a written or derived extent, against a loop body.

What Analysis-Lens.md §7.3 says an iterator specializes is two things, and both earn their place.

**It fixes what its continuation answers.** A lens hands its continuation's result straight back
without looking at it, so `$r` can be a variable; an iterator has to know whether to ask for the next
element, so it has to *compare* the answer - and a variable can be handed on but not compared. So
`declareContinuation` gives an iterator the exit code and `resolveYield` emits the test: falling out
is the loop asking for the next element and is the fall-through, and anything else is returned
unexamined. Which of `break` and `return` it was belongs to the consumer, and every frame in between
passes it along by an ordinary return, so a `break` through three adaptors stops all three.

That is also why an `iter fn` has no `return` and no exactly-once rule. Falling off the end is *ran
out*, which is the fall-out row emitted by `finish`, and an iterator that yields nothing is a filter
that accepted nothing.

### Where an iterator goes once its consumer has left

`doc/spec/lenses.md` says an iterator's own cleanup runs when a loop breaks out of it, and
`emitDeparture` is the whole of that: **a departure resumes after the outermost loop of the region
the `yield` was written in.**

Outermost and not innermost, which is what separates it from `break` - a loop whose hand-over has
nowhere to go has nothing left to do, so a departure ends every loop it was inside at once. What is
below them runs, and it is the *same blocks* the fall-out path reaches: there is no second copy of
the cleanup and no second exit, because `finish` answers with `$yielded` either way and the only
difference between "ran out" and "was left" is what is in it.

Three nestings and no case analysis, because "the outermost loop of the region" names all three:

| The `yield` is in | It leaves by | And then |
| --- | --- | --- |
| an ordinary loop | the edge that loop's own `break` takes, carry and all | the statements below it |
| a `for` over another iterator | `Break` - the row that stops it | `resumeDeparture` at the rejoin |
| a lens's continuation, or nothing | falling out to the region's destination | `resumeDeparture` at the rejoin |

The lens row is the one worth reading twice. A departure inside `withLock(m):` **falls out**, so the
lens runs its own cleanup on the way past exactly as a `return` through it would - which is why the
spec tells authors to write acquisition as a lens, and it is now true rather than a workaround. The
same row covers a `yield` in a region with no loop at all, so there is no fourth case.

`resumeDeparture` is why the table's second and third rows both end there: a departure leaves the
region it is written in and no more, because the loops outside it are outside the blocks it could
name. So the iterator asks again where the region rejoins, and a departure through three adaptors
leaves three frames one rejoin at a time with no rule per frame.

**A departed iterator must not re-enter its consumer**, and that is a flag rather than a `Try`. The
storage is `$yielded` - the same local a lens parks its continuation's answer in, which is what made
it the iterator's answer too - and the guard is a read of it before the call. It is emitted only
where a `yield` can be reached after a departure: control resumes *below* the loops, so the only one
that can be is a `yield` written after them, and a body with one `yield` has none. `resolveBody`
counts them, which is a syntactic answer to a CFG question about a body that does not exist yet, and
over-approximating costs one branch. The test *after* a `yield` is skipped on the same terms wherever
the departure would fall through, which is every straight-line iterator. `upTo` in `Iterator.yana` is
the shape to read: its `yield` is a call, a comparison and the loop's own exit edge.

**Its continuation runs any number of times**, which is `Continuation::Many` - the one value of that
field that changes what an analysis walks rather than only what it may assume. The caller's blocks
show one pass, because the loop is in the callee and the callee is opaque, so the **back edge is a
fact about the region rather than an edge**: `Analysis::forEachSuccessor` gives a block that falls out
of a `Many` region that region's entry as a successor too, and `Analysis::forEachPredecessor` is its
mirror. Both, and not one - a backward fixpoint settles by waking what reads a block's answer, and an
edge one direction knows about and the other does not is a fixpoint that stops one round early. That
was this waypoint's own bug: a value moved in a loop body looked dead afterwards.

Only a `Jump` gets the back edge. A `break` leaves the same block for the same target and does not
come back, which is the whole of what `Node::Break` is for - see below.

### Traversals that can fail, and the second lens-call kind

`lens fn f(...) -> (T | E)` and `iter fn f(...) -> (T | E)` declare a **failure channel**
(Analysis-External-Iterators.md §2.1), and that is the whole of what a *skipping* lens is: one that
may finish without ever having handed over. `Function::failure` is the type, null where there is
none, and every rule below reads it rather than a flag.

Three pieces, and each is in one place:

- **What the traversal answers.** `Function::continuationResult` is what the *continuation* answers -
  the exit signal for an iterator or a skipping lens, a free `$r` for a transparent one - and
  `Function::result` is what the *function* answers. They are the same until `| E`, where the second
  becomes `declareOutcome`'s `Proceed(exit) | Exit(reason)`: one synthesized two-constructor record
  per skipping declaration, which no program can name. `declareContinuation` is the only place the
  two are related, so `finish`, the call site and the conversion never decide it again.
- **How a body fails.** `return e` in `resolveReturn`, which needed no new word because a `return`
  in a lens body was refused before this. `checkHandover` then treats the failure exit as satisfying
  the hand-over rule rather than breaking it - a traversal that failed is *why* it did not hand over -
  and refuses the opposite order for a lens: see below.
- **Where the skip goes.** `Node::LensTry`, sharing `NodeLensCall` the way `Break` shares `NodeJump`,
  with a second edge. A callee that never handed over returns to a caller that never entered the
  region, so the caller needs a successor for it; what it failed with arrives there as that block's
  one parameter, exactly as the hand-over arrives at the region's entry. **So the terminator still
  has no result**, which is what keeps `kMirResult` and `kMirTerminator` disjoint. The region is
  `AtMostOnce`, its first user.

**Only an iterator may fail late.** A lens's alternative runs *instead of* its continuation - the
block below the call is the same block on both paths, so the two arms join at the statement rather
than at a value, which is what makes the arm writable when the continuation's result is a type the
call site chose. A lens that hands over and then fails would break that, and the caller's CFG cannot
even show it, because the skip edge leaves the *call*. `checkHandover` refuses it, using the same two
bits it already had. An `iter fn` may, because its consumer's loop ending is an ordinary thing - and
its body has run by then, which is a path in the callee that no edge here shows, so it is a derived
successor beside the back edge above and for the same reason.

**The failure is owned, and that is where the wrapper costs something.** A reason is produced on the
path the continuation never ran, so nothing could be borrowing it - which makes `declareOutcome`'s
record linear whenever `E` is, and a linear value returned by a call owes a consumption on every path
out of the call site. Moving a field out of an SSA *value* is not one, so `convertSkip` parks the
wrapper in a frame slot (`Scratch::outcomes`, allocated in the prologue beside `resultCarrier` and for
the same reason: a `lens.try` inside a loop must not allocate per iteration) and each arm reads its
own field out of storage. The shell then owes nothing, because the field the other arm would have read
was never written.

**Where the alternative is written** is `parseAlternatives`' and no concern of this stage: an arm is
`pat -> e` or `pat: block` with no keyword in front of it, and a block arm may stand after the
continuation as well as before it. Both positions read into one `ParseList<Alt>`, so `checkAlternatives`
and `resolveAlternative` see a list and never ask where it came from.

`Skipping.yana`, `Skipping.Types.yana`, `Skipping.Flat.yana`, `Skipping.Syntax.yana`,
`Skipping.Nested.yana`, `Reject.Skipping.yana` and `Reject.Protocol.yana` are the fixtures - `Skipping.Types.yana` is the conventions: an owned
hand-over, an exclusive one, an owned reason both bound and discarded. Not implemented: `<- R`, the
resume channel, which needs the wrapper turned round; and a failure type the signature is generic in,
since `declareOutcome` builds a concrete record.

### `break` and `continue`, in the two places they mean different things

Within one region they are edges and nothing else, which is Analysis-Lens.md §5.1's "the loop-shaped
instance of one mechanism". `Loop` is a stack of stack frames like `Held`, holding the two blocks and
the **carry** - a `break` is an edge into a block some enclosing construct declared parameters on, so
it supplies exactly what the loop's own edges do.

The loop that *is* a continuation region - a `for` over an `iter fn` - is where the two come apart.
Both exits go to the block the caller carries on at, and what tells them apart is the node:

| Written | Node | Row |
| --- | --- | --- |
| the body's end, and `continue` | `Jump` | fall-out - ask the iterator for the next element |
| `break` | `Break` | stop the iterator; the caller treats it as the loop being over |
| `return` | `Return` | leave, which every frame in between propagates |

`Node::Break` shares `NodeJump`'s struct because it is one node with two meanings, and the row is all
it adds. The call site's test is therefore `== kExitLeave` rather than `!= kExitFallOut`: `break` is a
third row the *callee* acts on and the caller does not, so adding a row below is a change to the
callee alone.

A `break` or `continue` whose loop is outside the continuation it is written in is refused by name.
That is a fourth row plus a target, and the two rows there are today are falling out and `return`.

### How much of the caller the extent is

The two call forms differ in this and in nothing else, which is why `Extent` is a struct rather than
two paths through `resolveLensCall`: what the call does, what it hands over, which region it opens
and how it rejoins are the same in both.

- **Scoped** - `let x = f(...) in: ...`, and `f(...):` which the parser desugars to it with `_` as
  the pattern. The extent is one statement, and it is what the author wrote.
- **Flat** - `let x = f(...)`, with the extent taken from the statements after it in the same block.
  `resolveStatements` is the walk that can do this, which is why a block is not resolved one
  `resolveStatement` at a time: the meaning of a flat call is not local to its own statement.

The derived rule is **the last use of the hand-over, and of everything bound while it is live**
(`flatExtent`), and the second half is not a refinement of the first. A name bound inside the extent
is a local of the *continuation*: under the CPS lowering that is a function and its locals are its
own frame, so reading one after the extent would be a read of storage that is gone - and under the
embed lowering it would happen to work, which is the one thing the two-lowerings requirement forbids.

Analysis-Lens.md §11.4's nesting falls out of that rather than being checked separately. A lens
called inside the extent binds a name, so the name is tracked, so this extent covers the inner one's
last use - which is exactly *`max(its own last use, the end of every extent opened within it)`*,
reached without a second pass over lens calls. It is a fixpoint because the set of names and the end
of the extent grow together: a statement first skipped as outside is inside once a later statement
extends the end, and what it binds is tracked from then on.

The scan is **syntactic**, and §11.1 is why it can be: the extent is settled at the call site, so
what settles it is what the call site wrote. Nothing else is available - the region has to exist
before the statements inside it are built, so there is no liveness to ask. Over-approximating is
safe and under-approximating is not, so a name counted where it is shadowed only makes the extent
longer, which is the extent every lens had before this.

What the rule does **not** follow is a value derived from the hand-over into a place of the caller's:
`let n = strlen(cstr)` is covered because `n` is bound here, and `out = view(cstr)` writing into a
local declared before the call is not. That case is a borrow that outlives its lender, which the
ownership passes report in the words they already have.

**An extent that covers nothing is refused** - §11.3, `reportEmptyExtent`. A hand-over with no live
use is destroyed at the call, and the author meant the scoped form or meant a plain function. Stated
over the extent rather than over the result type, so one rule covers `withLock(m)` written flat, a
`let` of a value nobody reads, and a body that is one statement with nothing after it at all. §7.2's
skipping lens called for its alternatives alone is the one legitimate empty extent, and it needs the
`Try` milestone 6 supplies.

Two shapes are refused by name rather than given an answer. A statement that binds **several** names
- a `where` block, the only production that makes one - has no extent to give one of them, because an
extent belongs to the statement and there would be two of them. And a block whose **own value** falls
inside an extent has nowhere to put it: that value is `$r`, which today carries the exit discriminant
alone, and it arrives with a skipping lens's `Try`.

### Leaving a continuation

§10.1 gives a continuation four exits - falling out, `return`, `break` and `continue` - and all four
are built where the loop they name is the region itself. Falling out resumes at the region's
destination; a `return` written inside a continuation is a `return` of the enclosing function,
because the continuation is a region of that function's own blocks; `continue` *is* falling out, and
`break` is the third row above. None needs a word about lenses: drop elaboration on the way out is
the ordinary rule on an ordinary edge, which is what makes a scoped-acquisition API correct on an
early return with nothing written at either site.

What is not built is `break` or `continue` naming a loop **further out than the continuation** - a
fourth row plus a target, where the two rows there are today are falling out and `return`.

A **`return` in the lens's own body** is the one narrowing rather than an addition. A lens answers
what its continuation answered, so it has no value of its own to write: `return e` is refused, and a
bare `return` answers with what the `yield` was handed - which is what makes a `return` in the
cleanup skip the rest of it, exactly as a `return` skips the rest of any block.

### A value that crosses a branch

`10 + (if flag then 2 else 0)` was the shape that did not compile, and the interesting thing about
it is that nothing in the source says a value crosses a block. The author wrote an addition.
Evaluation is left to right, so the left operand is produced before the branch and consumed after
it, and §5.9's rule - a value that crosses a block boundary is handed over as a block argument - is
therefore about the *walk's* state rather than about anything the program said.

So the walk says it. `Held` is a frame a resolver function registers around the values its own C++
locals are holding - an operator's left operand, the call arguments already resolved, the record
fields already filled - and `Carry` is what a construct that makes blocks does with them: one block
argument per carried value on each block it creates, the same list on both edges of a branch, and
the handles pointed at what came out. The frames nest exactly as the walk does, so a branch inside
an arm carries what the outer branch already carried and hands the result back out.

Three constructs make blocks and all three carry: `armsValue` - which `if`, `match`, `&&` and `||`
are all instances of - `while`, and the counted `for`. A loop needs it in one more place than a chain
of arms does: the header is entered twice, so the back edge refills the positions the entry filled. A
counted `for` needs it in two more, because its bounds are values of the block it was written in and
are read inside it, and because its **advance** is a block of its own that `continue` enters.

A `for` over an `iter fn` carries nothing, for the same reason a lens call does not: an edge that
leaves a continuation region carries nothing, because the block on the far side is another function's
under the CPS lowering.

Only a *value* form can have anything to carry today, because a statement cannot stand in a value
position in this stage yet - `let ... in` as an expression is refused, and there is no other way to
put a statement `if`, a statement `match` or a loop between a value and its use. The others carry
anyway, because the day one of them can be reached is not the day to discover that the sites that
build blocks were written to a different rule. `BranchCarry.yana` is the fixture, and it is about
the reachable one.

Two things follow that are worth stating. **A branch's two edges take one argument list**, because a
value live before a branch is live on both sides of it; what differs between the arms is what they
*produce*, and that travels the other way. And **an owned value carried across a branch is handed
over on the edges**, under `argumentMode` like every other position that fills a declared slot - so
the frame that had it is the frame that releases it, once, and an arm that leaves without reaching
the merge drops it where it stands.

A lens call carries nothing across either, and that is checked rather than hoped for: a `lens.call`
is a terminator, so a value live across one would be a value named from a block that is not its own,
which `checkValueScope` reports. It is also what licenses the conversion outlining the region at all.

### What is refused, and why

Three of these were found by `test/resolve/ownership/` and the rest by `test/resolve/loans/`, which
is what those directories are for:

- **A field may not be handed out of a value with an authored teardown.** An authored teardown runs
  on the whole value, and after a partial move there is no whole value for it to run on - so the
  authored body is silently *not run*, which is the one outcome a teardown may not have. A *derived*
  teardown is unaffected and must be: releasing the members one at a time is exactly what it does.
- **Storage this function borrows may not fill a sink.** `readMode` answers `BorrowShared` for a
  place the frame does not own, whatever the position asked, and that is the right mode; the position
  is what makes it wrong, so `Resolver::readPlace` is where the pair is judged. Before it existed,
  `fn f(g: Guard) = eat(g)` compiled and the guard was released twice.
- **A diagnostic names the path, not the binding.** `placeText` in `print.cpp` is the one spelling,
  shared by the dump, the resolver and the analysis. A message that called `p.left` "p" could not
  tell "the record is gone" from "one of its fields is", which is the distinction partial ownership
  exists to make.
- **A position asking to write is refused for two different reasons.** The storage is somebody
  else's and shared, or the binding is one the author did not write `&` on. `Resolver::readPlace`
  keeps them apart because telling an author to add `&` where the storage was never theirs is advice
  that cannot be taken.
- **Two arms of a value `if` must agree on ownership exactly.** A merge parameter has one occurrence
  and every edge supplies that one. §2.5's join - a loan weakened to what both sides can promise - is
  not built, because weakening can change how the value is carried and an arm would then need a
  reborrow on its edge; refusing is the honest report until there is one.
- **`break` and `continue` naming a loop outside the continuation they are written in are refused by
  name.** The loop they leave is the region itself in a `for` over an `iter fn`, and there they are a
  row of the exit signal and an ordinary fall-out; the loop being *further out* needs a fourth row
  and a target to go with it, where there are two rows today. An exit that skipped the frames in
  between would release nothing and say nothing, so it is reported rather than approximated.
- **A loop is a statement, so `break` carries no value.** What would hold one is the loop's own
  result, which arrives with the `iter(Y <- R | E)` result type rather than with the loop; a value
  evaluated and dropped is the one reading nobody wrote it for.
- **A `for` reaches its iterator by name.** Nothing derives one from a type: a container has several
  traversals and naming the one meant is the point. Which of the three ways it went wrong is told
  apart - not a named call at all, a plain `fn`, or a `lens fn`, which hands over once and has no
  second element to ask for - because one is a traversal to write and the others are a declaration to
  fix.
- **A counted `for` counts over whole numbers**, asked once where the loop is built rather than
  arriving as `sub cannot be applied to String` from a subtraction the author did not write. The two
  bounds must agree on a type, and a written *literal* takes the other's rather than defaulting, so
  `for i in 0 .. n` counts at `n`'s type.
- **A lens call carries no sigil.** `let &x = f(...) in:` is refused naming the declaration, because
  what the continuation receives under is the callee's `-> &T` or `-> ->T` - part of its published
  interface, per Analysis-Language.md §3a - and a second spelling at the call site could disagree
  with it.
- **A requirement is spent by a specialization or read out of an environment, and which is the
  declaration's `form`.** `selectGenForms` in specialize.cpp decides one per generic: `@specialize`
  and `@generic` write it and everything else measures it. Both halves exist - what a specialization
  does is here, and what the erased form is belongs to `resolve_ssa/erase.cpp`, which is where every
  number of the ABI lives.

  The measurement is a **share of the body** - `Summary::genCost`, published as a fact and read back
  here - and never a count of call sites, which is the metric this would be wrong with: a generic
  over integers that lowers to two instructions is worth specializing however few callers it has,
  and a large body that takes a type parameter, does nothing with it and drops it at the end is
  worth erasing however many. Named requirements are one site each, a value whose type is not
  concrete is a site because its size, its copy and its teardown come out of a descriptor, and the
  teardowns the analysis placed count - which is why the pass runs after `analyzeProgram` rather
  than beside `freezeSchemas`. It is deliberately unweighted by how often a site runs, and by which
  *kind* of environment work a site is: loop depth could be had, call frequency is the term that
  would matter and cannot be, and half a cost model reads as one.

  A chosen erased form is then held to `checkErasable`, which is a different question from the
  measurement and is asked second: it is about *possibility* where the share is about cost. A
  descriptor states a size, an alignment and a teardown, which is everything a value of a
  *parameter* needs and not enough for a record with a field at one - `Boxed(a)`'s layout would have
  to arrive as a table of offsets, which is a second layout algorithm beside `fieldOffsetNative`.
  Every **call** as well, through `erasableCall`: the two shapes an environment cannot make are
  refused where the form is chosen and not where the call would be built, so that a body nobody
  wrote an attribute on cannot be measured erased and then told the compiler does not implement it.
  A written `@generic` that cannot be honoured is an error; a measured one falls back to
  specializing in silence, because nothing asked and the other form is a correct answer to the same
  question. `Reject.Erasure.yana`, `Reject.Erased.yana`, and `Erasure.yana`'s `carried` for the
  silent half.

  Three kinds of generic have no choice and are answered silently: a **selected member** - a class
  member or an instance's - is reached from the instance table and specialized there, a **teardown**
  is called by the compiler at a fixed signature, and a **lens** has no standalone body. Silently
  because neither attribute has a placement inside a `class` or an `instance`, so no author can have
  asked.
- **What an erased body still refuses is two shapes the environment cannot express.** A class member
  reached through a *superclass* of a declared requirement: the promise holds, and the slot carries
  the subclass's witness with no path from it to the superclass's members - `freezeSchema` says
  superclasses are not slots of their own, and a witness that carried the ones it implies is what
  that needs. And a generic called at this body's own type parameters: an environment is a table in
  the image built by a caller that knows every argument, and forwarding slots out of one environment
  into another is an environment built on the frame instead. Both are `Reject.Erased.yana`.
- **A lens and a generic in one call are refused by name, in either direction.** Flattening resolves
  the callee's body inside the caller's walk and `Resolver::genContext` says which declaration a
  written type is read in, so a generic callee would need a second substitution mechanism beside
  `specializeFunction` and a generic caller would leave its own variables in scope while the
  callee's body is read. The milestone-7 rebuild ends both by making a lens an ordinary call, which
  has one context at each end.
- **A specialization is held to being the generic under a substitution.** `checkSpecializations`
  runs last - after the analysis and the reachability walk, which are what make one - and checks the
  claim every other pass rests on: the argument list matches the generic's arity and is concrete, the
  body handle is shared, and every parameter occurrence and the result are `substituteOccurrence` of
  the generic's. The *whole* occurrence, so "substituting `Int` for unconstrained `a` does not turn a
  borrow into a copy" is checked rather than intended. A class default's clone is the one field where
  the two must *differ*: it sheds `klass`, because `callTarget` recognizes a class member by it and a
  clone that kept it would be dispatched again at the answer dispatch already reached.
- **A call declines to specialize past a budget.** `Function::specializations` counts the argument
  lists one generic has been specialized at; past 32 the call hands the generic back and reports,
  naming `@generic` as the answer. At a call and nowhere else - `teardownMember` has no second form
  to fall back to, and a null there would read as "nothing to run". Still a diagnostic rather than a
  fallback now that the erased form exists, and that is a pass-order fact: the form is chosen before
  any specialization is made and `checkErasable` ran against that choice, so switching here would
  name a body nothing had checked. Deciding the form after a first specialization round is what
  would close it. The program-wide half of the budget is deferred for its own reason: a growth ratio
  measures 9.1x on the smallest fixture against 0.73x elsewhere, and a flat count needs a program
  larger than a fixture to pick a number from.
- **Specialization that does not terminate is reported where it is visible.** A call chain that
  specializes at `a`, then `Boxed(a)`, then `Boxed(Boxed(a))` has no fixed set of instantiations, so
  `callTarget` bounds the argument nesting depth and reports. It is a diagnostic about the *choice of
  lowering* rather than about the program - a body compiled once does not grow with the argument
  lists that reach it - so the message names `@generic`, which is now a fix an author can write.

### The summary

`summary.cpp` is the one interprocedural pass: reachability from the roots, and `Summary::suspension`
by a fixpoint over the call graph. Both were previously stand-ins - everything was marked reachable
and suspension was `Known` false at declaration time - and both were only correct while nothing could
call anything.

The fixpoint runs **in the summaries themselves**, at `Knowledge::Assumed`, which is what that state
is for: the value has one home, and `isKnown` is what stops a provisional one being published or
cached. A parallel table of provisional answers would have been a second place the question is asked,
and the two would answer differently for a callee that is mid-fixpoint.

The *schedule* is a worklist over the **reversed call graph**, which reverses an earlier decision not
to store the edges. The argument for not storing them was that they are the `Call` nodes and a copy
is a second thing to keep in step; what it cost was a fixpoint that re-scanned every reachable body
on every round, so a call chain of length n cost n rounds over n bodies - quadratic in the size of
the program, in a pass every compilation runs, to save one array. The objection does not apply,
because the array is not a cache: it is built by the walk that establishes reachability, from bodies
nothing may rewrite while summaries are computed, and it is discarded when the pass ends. The
teardowns a body places are edges in it too, and were not before: a `Drop` instance is reached by an
object dying rather than by a call, so a graph holding only the `Call` nodes says a body that
releases a suspending guard does not suspend.

Nothing suspends yet - `suspendsAt` is an exhaustive switch over the node kinds answering false, and
`yield` is milestone 9. The other facts stay `Unknown` until their passes exist, which is what `Fact`
is for; what this milestone establishes is the pass they join. Their **lattices** are real, though,
and that is not the same statement: `boundedStack: bool` could not publish the byte count an external
iterator has to allocate, and `retainsArguments: bool` lost the only thing a caller wanted to know -
*which* argument - so a caller with one frame-placed argument had to treat all of them as retained.
A fact nobody can act on is how a consumer ends up recomputing its own. They are `StackBound` and
`ArgumentSet` now, with `consumes` beside them for linear consumption. The instantiation set is
deliberately still absent, and `Summary` says what it is waiting for: every fact there is a lattice
element that fits in a word, and a set of targets needs storage the summary would have to own, keyed
by an identity milestone 6's substitution decides.

Each fact is **read through an accessor and written through a named setter**, and the confidences for
all six live in one `U16`. `Fact<T>` is still the vocabulary - a fact cannot be read without its
confidence, and `get(fallback)` still makes the caller name what it falls back to - but stored as a
pair it cost seven bytes of padding per fact whose value is a `U64`, and four of the six are: eighty
bytes carrying thirty-two of payload and six two-bit numbers. Forty now.

Three things it deliberately does not do, and says so rather than guessing:

- **One plain function per name.** `doc/spec/overloading.md` keys an overload set by `(name, arity)`
  and admits one plain function per key beside any number of class functions; selecting among those
  is milestone 6. A second declaration of one name is refused at its declaration, because the name
  table would otherwise keep the first and every call would silently reach it.
- **`::` ascribes and does not convert.** `x :: I64` on an `I32` is a legal widening in the language
  and is reported here as unimplemented, not as a type error - `Convert` is a class, and conversions
  arrive with instance selection.
- **Operators are a table, not instance selection.** `resolve.cpp`'s `kOperators` is a stand-in for
  `Num`, `Eq`, `Ord` and `Truth`, carrying `Core.yana`'s own fixity numbers so that an expression
  groups here exactly as it does under the other stage. Milestone 6 deletes it.

The one thing arithmetic does *not* decide is signedness: op.def has no column for it, because
`I32` and `U32` are two logical types over one `div`, and `resolve_ssa/lower.cpp` is where the two
become `idiv` and `div` with the target in hand.
