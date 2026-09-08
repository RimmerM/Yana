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
   `resolve_ssa/lower/manifest.h`. Nothing in this directory may ask what a value looks like in a
   register.

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
| `resolve_internal.h` | the `Resolver` struct itself, `Operator`/`Chain`, `Held`/`Carry`, `TypeArguments` |
| `resolve.cpp` | the driver, written types, and constants |
| `resolve_decl.cpp` | declarations: types, aliases, fields, globals, and the walk that orders the passes |
| `resolve_sig.cpp` | one written occurrence, and the signature it is a position of |
| `resolve_class.cpp` | classes and instances - the heads, the members, and what each has to prove |
| `resolve_derive.cpp` | `deriving` on a qualified alias - one forwarding instance per class named, as source |
| `convert.cpp` | what a value does when the position it fills is a different type - `::` and every implicit widening, in one function |
| `resolve_try.cpp` | `?` and `?.` - the two operators that read a carrier, and where each one's other path goes |
| `intern.h` | the one probe behind every table in this stage that interns by content |
| `gen.h`/`gen.cpp` | type variables, generic contexts, class requirements, the schema, substitution, the one-way type matcher |
| `select.h`/`select.cpp` | which instance serves a class at an argument list - a match above the table |
| `specialize.h`/`specialize.cpp` | what a *call* to a generic becomes: one `Function` per argument list |
| `resolve_expr.cpp` | the things that answer: places, reads, calls, operators, `if` |
| `resolve_overload.cpp` | which member of an overload set a call reaches - `doc/spec/overloading.md`'s R2 through R5 |
| `resolve_stmt.cpp` | the things that do: statements, assignment, loops, `return`, lens calls, bodies |
| `resolve_lambda.cpp` | a lambda: the function it is lifted into, its captures, and the environment they live in |
| `resolve_caller.cpp` | `@caller` - what the marker may ask for, and the value a call that omitted the position passes |
| `resolve_test.cpp` | `@test` - the registry the attribute fills, and the entry a `-test` build runs |
| `resolve_intrinsic.cpp` | the intrinsics whose expansion *makes blocks* - `OpForm::Branch`, and why those are the resolver's rather than an emitter's |
| `name.h`/`name.cpp` | the one traversal every module-level lookup goes through, and what `pub` means to it |
| `import.cpp` | which modules the program is made of, and what each one can see |
| `export.cpp` | the declaring side of `pub` - a marker that means nothing, and an interface that leaks |
| `intrinsic/` | the compiler's roles over this IR - see below |

`resolveProgram`'s order is the one thing that has to be read as a whole and it is in `resolve.cpp`:
`discoverModules`, `linkImports`, `declareFixities`, `declareTypes`, `declareAliases`,
`declareClasses`, `resolveHeads`, `declareRecordFields`, `resolveAliasTargets`,
`foldSchemas`, `declareClassMembers`, `declare`, `checkExports`, `classifyTypes`,
`resolveInitializers`, `checkExportedGlobals`, `checkRoles`, `resolveProgramEntry` (or
`resolveTestEntry` under `-test`), `resolveBodies`, `freezeSchemas`, `verifyProgram`,
`analyzeProgram`, `computeSummaries`.

**Every phase runs over the whole program**, not over one module and then the next. That is the same
statement as "an import does not trigger the resolution of what it names": a pass that finished a
module before starting the next would be an ordering between them, and a cycle has no ordering to be
given. `Resolver::eachFile` is the one walk they are written against, and it is what sets the
position - which module, and which file of it - that a name lookup is relative to. It reads the
**library's modules before the program's**, whatever order discovery found them in: a `@builtin`
claim is first come, first served, so a program that claims a role the library fills has to be the
second claim and get the refusal on its own declaration. Only the declaration passes read in that
order - `Program::modules` keeps the root at index zero, and conversion, lowering and the dumps walk
that list, so a lowered module still opens with the program's own functions.

Seven declaration phases rather than two, and the split is a dependency rather than tidiness. A
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

A *generic head* is the one declaration read in two halves, and `resolveHeads` is the second of
them. The three shell passes bind each parameter as a **name** and nothing else; a const parameter's
annotation and a parameter's default are types, and a shell pass is the one place in the compiler
whose type namespace is deliberately incomplete - it is what builds it. Reading a type there meant
reading a different namespace from every other position: `data Buf(n: Int)` reported `Int` as an
unknown type where `n: I32` resolved, because `Int` is an alias and aliases are declared by the pass
beside that one; and `data Foo(a = Bar)` compiled or did not depending on whether `Bar` was written
above it. So the heads are queued and drained here, in one pass over all three declaration forms
rather than three copies of the walk - `data`, `alias` and `class` heads differ in nothing this
question can see.

**One dependency no ordering reaches**, and `completeInstantiations` is what closes it. A field's
type is resolved *inside* `declareRecordFields`, so `data Node {leaf: Maybe(Int)}` makes a `Maybe(I32)`
while the pass is still walking - and `instantiateRecord` copies the declaration's fields and
constructors at the moment it runs. Whether there was anything to copy came down to which `data` the
walk reached first: written before its generic, or in a module visited earlier, and the instantiation
came out empty. `Just(3)` at that type then asserted in the builder, and a projection through it
would have reported a field the author wrote. So an instantiation of a declaration nobody has read is
queued instead - `cons` is never empty once a record is read, which is what says which state it is in
- and filled once every `data` is in. The alias pass one line further on solves the same problem by
ordering; this is the half where both ends are inside one pass.

### Where a program starts, and what runs before `main`

`resolveProgramEntry` is the pass, and it sits **between the initializers and the bodies** because
that is the one place it can: a global whose initializer is not a constant gets its type from what
that initializer turned out to be, and every body that reads the name needs the answer.

The root module's top level is a program point - Analysis-Initialization.md stage B - and no other
module's is. A `let` there whose initializer folds to a constant is what a module-level `let` has
always been: it holds its value before the program starts and occupies read-only storage. One whose
initializer is an expression is **dynamic**: its storage starts at the zero of its type and is filled
where its declaration stands in the sequence. `resolveConstant`'s `notConstant` flag is how the two
are told apart - it suppresses the three "this is not a constant form" reports and answers the fact
instead, and everything else it rejects stays rejected, so `300 :: U8` is an error at the top level
too.

Where anything at all runs, the entry is a synthesized `main$` whose body is the top level in written
order followed by a call of `main`, and whose result is what `main` answered. Where nothing does -
which is every fixture that predates this - `main` is the entry itself and nothing is synthesized:
the degenerate case is the general one with an empty sequence rather than a case of its own.

Two rules fall out of the order being real. The declaring write is a `Destination::Init`, which is
the one `Init` whose root is not a local - a global's storage begins there, and `analyze_path`
already answers "not this function's to release" for a global root, so nothing is pre-dropped and no
move path is opened. And a read of a dynamic global whose statement has not run yet is reported
while the read is being **resolved**, out of a set the entry sequence holds and shrinks as it goes:
a global has no state row in any frame, so the initialization dataflow that answers this for a local
has nothing to look at, and what decides it is where in the sequence the read stands.

Root-only is the whole trick. There is exactly one module with executable top-level code, so there is
no cross-module order to define, no reference analysis to write and no import side effects to say
anything about. A statement in any other module is refused where it is written.

### `@caller`, and the entry a `-test` build runs

Design-Test.md §11's F1 and F2, both built here, and they are one story: an assertion has to be able
to say **where it was written**, and a suite has to exist without anybody writing it down twice.

**`@caller` is a position the resolver fills at the call that left it out** - `resolve_caller.cpp`.
`declareCallerFill` is the declaration half: `@caller at: Site` asks for the call's location and
`@caller(source: p) text: String` asks for the source text of whatever reached `p`. The location is
a record matched **by field name** against a fixed vocabulary - `file` and `function` are the type a
string literal has, `line` and `column` are integers - because the record lives in a library and this
compiler holds no handle to it; a framework wanting only the line declares a record with only `line`
in it and gets one. `callerFillValue` is the call half, reached from `fillDefaultArguments`: the
position is otherwise an *ordinary parameter*, so named arguments, arity and overload selection are
unchanged and `Arg::omittable` is the single predicate that learned there are two ways to fill a
position. A helper passing its own `at:` on explicitly is never filled, which is `#[track_caller]`
obtained for free.

The fill is **built the way the same expression written by hand would be**: `textValue` for a text,
`Builder::addAggregate` over four values for a location. A `Constant::Aggregate` was tried first and
is not available - a location holds a `String`, which is an owner, and a constant of a linear type is
a thing this IR has no way to say (`Editor::addConst` asserts it). It costs nothing anyway, since a
string literal owns nothing. Where the call site published nothing - a defaulted position, an
operator's operand, a call the compiler made - the text is `""`, which the library reads back and
reports as "check failed" with no expression.

Two things wait for the `stringLiteral` role rather than being asked at the declaration: which type a
text fill has to be. That role is bound while the declaration carrying it is *read*, so asking during
`declareArgs` answered "no such role" for every program whose `Native` module happened to come later.
`PendingCallerText` and `checkCallerTexts` are the deferral, beside `PendingTextDefault`, which is the
same ordering for a written string default.

**`@test` makes the entry the suite** - `resolve_test.cpp`. `readTestAttribute` runs where a plain
`fn` is declared and pushes a `TestDeclaration` onto `Program::tests`; `resolveTestEntry` replaces
`resolveProgramEntry` under `-test` and builds `main$` out of it. The registry has to be the
compiler's: there is no static initializer, no linker section and no reflection here for a `Test`
module to register from, and a written registration list means every test is written twice with the
second half invisible when it is missing.

Nothing in the synthesized body is a new mechanism, deliberately. A case is `addAggregate` over five
values; its `Site` is `siteValue`, the same one a `@caller` position is filled with; the array is
`buildGrowableArray`, the same two library roles `[1, 2, 3]` goes through; the runner is reached by
`callFunction`. What the library contributes is `Test.Case` and `Test.runMain`, found by **name** in
whichever module of the program is called `Test` - not by a role, because a role is a promise the
standard library makes to the compiler and a test framework is a library a program chooses. The five
fields of `Case` are read once, in `findTestLibrary`, which is the one place the contract is stated.

A `@test` answering `{}` is wrapped in a function answering `Test` - one call and one `Ok`, whose
arity is read off the constructor rather than assumed, since `Result(Failure, {})`'s carries a field
and a framework's own success constructor need not. Both entries begin with `resolveRootTopLevel`,
because §3.3's rule is about `main` and not about startup.

**A test file's own `let` is initialized by code** - Design-Test.md §11.2's F5, built 2026-09-07.
The rule was never about being the root module: it is about whether there is a program point at
which the initializer runs, and a test build has one. `activeTestFile` is `ast::Module::test` carried
by `eachFile` beside `activeFile` - it cannot be re-derived, since `mapFile` upper-cases every
segment of the identifier - and `declareGlobals` routes a test file's `let` into `testTopLevel`
whether or not the module is the root, exactly as `resolveGlobals` treats it as a program point.

`resolveTestFileInitializers` builds **one anonymous function per test file**, in that file's own
module and under that file's imports, and the entry emits a direct call. A function rather than
statements in the entry for two reasons that are the same reason: a test file's globals belong to
*its* module, and the entry lives in the root, which is usually neither; and a call is what the
reachability walk already understands. The order is stated rather than invented - modules as
`Program::modules` holds them, files in path order, statements as written - and one `uninitialized`
set spans every test file, so a read of a global whose file has not run yet is the use-before-init it
already was inside one top level. A bare **statement** stays refused there: a test file gets
initializers, not a second program start.

`test/lib/TestFileGlobals.yana` runs under both stages, and `test/lib/modules/GlobalInit.test.yana` beside
it is the case that says the initializer was resolved in *its* module.

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
name at several arities, an instance member whose signature is taken from the class, a transparent
`alias`, and a **qualified** one - `alias qualified Id = Int deriving (Eq, Ord)`, which is a record
with one constructor (`newtypeRecord`) plus one forwarding instance per class named
(`resolve_derive.cpp`).

A class variable bound **only by a functional dependency in the instance's own context** is admitted
- `instance (Magnitude(a, w), Show(w)) ShowWith(a)`, where nothing in the head says what `w` is.
`closeDecidedVariables` is the rule and it is read twice: at the declaration, where a variable
nothing decides is refused, and at selection, where `holdsContext` fills it from the instance that
answers the dependency. A **blanket** head - `instance (TrivialCopy(a)) Copy(a)` - is placed by the
ordinary orphan rule, which for a head naming no type means the module declaring the class and no
other; what keeps it from subtracting a capability is that the classification reads only keyed
heads, and `select.h` is where that is written down.

**Named arguments, default arguments and field defaults** - `Pair {x: 1}`, `open(path, mode: Read)`
and the positions either may leave out, `@lazy` among them. A **string** is a constant here like any
other: `mir::Constant::Text` holds the text and `Resolver::textValue` is the one place that says how
a text becomes a value, which is what keeps the shape of a `String` the library's. See
[Which position each argument fills](#which-position-each-argument-fills).

**A record update, a string interpolation and a pattern on a `for` binding** arrived 2026-09-06 and
are three readings of one idea - a construct whose whole content is storage and a run of writes
through it, or a place read more than once. `{p | y: 3}` is a copy of its source and one
`Destination::Assign` per replacement, with `->` relocating instead and `Copy` called where the type
does not duplicate for free; `"a{x}b"` is `String.ofCapacity` and a run of appends over `Show`'s two
members, all five declarations named through `known.def` and all five `Presence::Optional` so that a
library without a formatter reports at the literal; and `for {a, b} in pairs(m)` is the `let`-pattern
path over the hand-over's own local. See
[A record update is one copy and a run of writes](#a-record-update-is-one-copy-and-a-run-of-writes),
[A format is one allocation and a run of appends](#a-format-is-one-allocation-and-a-run-of-appends)
and [What a `for` binds, and how a pattern reaches it](#what-a-for-binds-and-how-a-pattern-reaches-it).

**Fixed arrays and windows** - `[T *n]` as a logical type, `[T]` in a binding position as an
ownership contract over `Array(T)`, element places, array literals and `xs[a..b]`. See
[Containers, and why there is no slice type](#containers-and-why-there-is-no-slice-type).

**Vectors, masks and atomics** - `Vec(a, n)`, `Mask(a, n)` and `Atomic(a)` as logical types, the
portable vector set of `lib/Core/Vector.yana` and every operation of `lib/Atomic`. See
[The vector, the mask and the atomic](#the-vector-the-mask-and-the-atomic) and the three sections
after it. `Native.atomicAt` is the one declaration of either family that is not built, and it is
blocked on a spelling the library itself is waiting for.

All five requirement forms a signature may write are built: a bare variable, a **class**
`Show(a)`, a **structural field** `a.field: b`, a **constrained callable** `f: (a) -> b`, and a
**const parameter** `n: I32`, which is also a `data`, `alias` and `class` head parameter. A class
head's counted parameter reaches a *signature* through the constraint that names it:
`fn (Digest(d, n)) Hmac.of(...) -> [U8 *n]` introduces `n` as a const parameter of this signature,
because `class Digest(d -> n: Int)` says the position counts and `resolveTypeArgument` reads the
head's own annotation for a bare name exactly as it already read it for a literal. There is nowhere
else to say it - the constraint list is read left to right and `fn (n: Int, Digest(d, n))` would
declare the parameter before the class has said what it is. The last three requirement forms are one
sentence made three ways - **a body names a requirement, never a number** - so a
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
`fun.value` naming it. Its **body is its value** whichever arrow was written: `(x) -> e` and `(x): e`
are one grammar production apart and mean different things for a `fn` - `doc/spec/functions.md`'s
rule that a `:` body returns `{}` - and the same thing here, because that rule is written on a
declaration and a lambda has none. `resolveLiftedBody` states it with `resolveArm`'s rule (2026-09-07,
J30): the body answers unless its job is to **leave**, and a `{}` result wants no value at all and
takes the statement path every `:` lambda written for its effect was already on. What its parameters and its result are is the **position**'s to say, with one
rule for the position that says nothing (2026-09-07): a lambda that writes a type on *every* one of
its parameters is a signature with no name, so it stands at its own header and its body decides the
result - exactly as `fn double(x: Int) = x * 2` does. `writtenLambdaPosition` builds that header and
leaves the result `Types::undecided`, which is the state G7c's open result already had a walk for. A
lambda that leaves a parameter open still has nowhere to take it from and is refused; one rule and
not a list of the syntactic places a lambda may be found in, because a list of those would have to
agree with itself. Its captures are found by the body walk rather than by a scan, because a
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

### A closure that leaves its frame - D17, built 2026-09-07

The environment above is one of two, and **which one a lambda gets is decided by the position it is
written at**. A borrowing position - a callback parameter, a `let` - is bounded by its call, so the
environment stays a local of the enclosing frame and everything in the paragraphs above applies. An
*owning* position - a `return`, a field, a `->` parameter, an assignment into storage that already
exists - is exactly where `checkCarriedOrigins` fires, and there the environment is a **heap-placed
block the value owns**.

A block is `[teardown][captures]`: one pointer, then the environment record at whatever alignment it
wants. The teardown releases the captures and hands the block back through the allocator roles;
`resolve_ssa/lower/fun.cpp` builds all of it, beside the `{code, env}` pair, which is `@box`'s
arrangement - the ownership is decided here and the allocation lives one tier down. The captures
themselves become the spec's fourth kind, a **consumed** capture (`CaptureKind::Owned`), so the
enclosing binding is moved out of and the environment owns what it holds. Deciding this from the
position rather than from the ownership analysis is forced: the analysis knows more and runs after
the body is built, and the capture conventions are settled *while* it is built.

**The second word carries a tag bit**: bit zero set means a heap block. That is D17's one open
question, answered in J26 from the tree rather than from a benchmark - the alternative was
`known.def`'s `isHeapAddress`, which is `Presence::Optional` and which the *default* provider does
not fill (`lib/Native/Heap.native.yana` is `@allocator(bootstrap)` and declares `allocateHeap` and
`freeHeap` only; the one declaration of the third is `lib/Native/HeapWindow.native.yana`, under
`@allocator(window)`). A closure teardown built on that role would work in an opt-in build and be
unwritable in the default one - the asymmetry `storesBoxProvenance` absorbs for boxes - and absorbing
it here would mean two representations of a function value in one language.

**What the tag costs, measured on the emitted code.** The *call* pays nothing at the site: `code` is
called with `env` exactly as the pair holds it, and a heap-placed lambda's own entry block adds
`headerBytes - 1` to its environment parameter once (`funEnvironmentBias`), which the backend folds
into the displacement of the first capture it reads - `ClosureTeardown.yana`'s `escaping.lambda` is
one `add` and a `loads`. The *drop* is one call to `teardown(fun)`, which is one function for the
whole program: load the word, `and $1`, branch, and on the taken edge load the header and call it.

**A frame closure's drop is not folded, it is not emitted.** `ownershipOf` answers `Derived` for
`Fun` as a *type*, because a value of one may own a block; what says otherwise is the **occurrence**,
and `ownershipOfOccurrence` is the one place it is read. A value carrying `Shapes::closure` holds
somebody else's environment and owes nothing, which is the same sentence `qualifiesInterior` makes
about a record whose member a qualified argument replaced. A value carrying `Shapes::noEnvironment`
has a null word and owes nothing either - that second mark exists because without it `let f = twice`
would be dropped like a closure, which stops a call through the pair being devirtualized and puts a
call on every `@lazy` thunk over nothing. The two marks differ at exactly one reader,
`checkCarriedOrigins`: the first may not leave the frame and the second may go anywhere.

`ClosureTeardown.yana` is the pair as emitted code - `direct` and `either` are frame closures and
have no teardown anywhere in them, `escaping` allocates and carries the tag - and
`ownership/ClosureEscape.yana` is the four owning positions run, with the allocator's bump pointer
read back so that a block that was never handed back fails.

**Corpus cost**, `*.mir.lower.expect` over `test/resolve/`: 84637 -> 85065 instructions and
16065 -> 16151 blocks, +0.5%, on the fixtures that existed before. It is where a `Fun` is a *field*:
`Step {run: (Int) -> Int}` now owes a derived teardown, so the record is materialized and the call
through it stays indirect (`FunFieldBorrow.yana`). Six adopted fixtures add 1274 more.

What is still refused, each by name: a **nested** lambda and a lambda in a **generic** body - neither
is a placement question, one being a chain of environments and the other argument zero already spent;
a **consumed** capture asked for at a *borrowing* position, which is a callback being asked to own
something; a `&` capture at an *owning* one, which is the one refusal D17 added rather than removed -
the block outlives the frame and a loan into it would dangle, and nothing downstream catches it
because a value that owns its environment carries no origin; a capture of a binding this function
only borrows; a merge of a capturing and a non-capturing lambda; and a closure written at a borrowing
position and *then* sent to an owning one (`let f = ...; return f`), which is the environment placed
on the frame before anything asked it to leave. `generic/Reject.Closure.yana` and
`generic/Reject.ClosureEscape.yana` are the two lists, split by which pass answers.

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
split at its loosest operators when those are all `&&`, so the split is the *declared* fixity's answer
and not the parser's nesting - `a == 1 && b` splits and `a && b || c` does not - and each span's failure
edge is the whole condition's. `while` needed nothing of its own, since both go through one
`resolveCondition`. The shipping stage does not do this; `test/resolve/Conjunction.yana` is the
fixture, and five existing `&&` goldens shrank when the merged `Bool` stopped being built.

**Conversions** - `Widen`, `Narrow` and `Bitcast`, the ladders over every ordered pair of numbers,
`::` as the selection that may widen, and the implicit widening every declared position performs. See
[Conversions, and where `::` stops](#conversions-and-where--stops).

A **qualified alias** is refused for a different reason - it is a type rather than a spelling, and
needs `deriving` as well as the conversions it now has.

**Linear types**, which are this stage's alone: `@explicitDrop` withholds the compiler's permission
to insert a teardown, `->x` writes a disposal down, and `@local` says who may write one. See
[The Drop, Reclaim and Copy classes](#the-drop-reclaim-and-copy-classes) below and
`doc/proposal/Design-Linear-Types.md`, whose build order this implements the first four steps of.
The shipping stage refuses `->x` by name and accepts both attributes without acting on them, so a
fixture about any of this carries `# resolver: ownership`.
**The standard library is `lib/`, and it is the only one.** `LibrarySource::directory` answers the
same directory for both stages since 2026-09-07 (J21), and `-lib` overrides it for both the same way.

*There used to be a second one, `prelude/`.* It was a directory laid out exactly like `lib/`, found
by the same walk, judged by the same `Core/Core.yana` test, holding a *subset* of `lib/Core`'s heads
and signatures - the smallest Core this stage could compile while it was being built - and every
declaration in it claimed its role with `@builtin` exactly as `lib/Core`'s does, which is what made
pointing the stage at the real library a dropped default rather than a refactor. Its whole reason was
that a declaration nothing could reach is a declaration with no meaning: the stage reached the real
library one construct at a time and the stand-in was what it compiled in the meantime. It is gone,
and nothing in the compiler moved when it went.

What *that* replaced was a set of declarations **synthesized in C++**: `class Drop(a)`, `Reclaim` and
`Copy` were built by `synthesizeCoreClasses` into no module at all, so that they could be found from
every one, and `Presence::Synthesized`, `KnownDef::standIn`, `KnownDef::member`, `lookupClass`'s
fallback and `lookupCoreMember` all existed to serve them. Every one of those is gone. A class
written in Yana is checked by the same front end that checks every other declaration, so a signature
that would not typecheck is a build failure rather than a mismatch found at milestone 8.

Anything a source file names that the library does not declare is reported by name as not
implemented, because this stage is chosen by a flag over a corpus written for the other one and
meeting an unimplemented construction is an ordinary event.

**The builtin type names are `intrinsic/primitive.def`** and nothing else - `Types::findBuiltin` is
the one lookup, and it turns a row of that table into this stage's representation. The eight fixed
widths and the two floats are the obvious rows. Four more are not:

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

  `resolve_ssa/lower/manifest.cpp`'s `intBitsOn` is where the deferring stops, and it is the only
  place a `TargetWidth` becomes a number. What it reads is `NativeTarget::integers`, which is why
  the manifestation entry points take a `Layout` - the declaration region **and** the machine -
  rather than a region alone: a site holding only the first would silently get this compiler's
  answer for a width the program left open, which is the failure Analysis-Modules.md Move 2 removed
  from the old stage.

  Two rules that used to be written as a number are now written as the name. `BuiltinShape::Word`
  in `intrinsic/link.cpp` asks that a word-shaped builtin be declared `Size` or `USize`, where it
  used to compare the declared width against `targetIntWidths(settings).word` - a question this
  stage has no business asking, and one that would have accepted `I64` on a machine whose word is
  thirty-two bits while refusing the `Size` that is the word there. That was the last target query
  in `resolve_mir`. `erase.cpp`'s `slotBytes` is the same mistake one layer down: a const slot is
  sized at `intBitsOn` rather than at the maximum, because `numberSlot`'s access is sized from what
  the value manifests at and a slot eight bytes wide read by a thirty-two-bit load is what its
  assert exists to catch.

  The conversion ladder between them **is** implemented, and it is where the two ends of that bound
  are actually read: `integerRangeFits` in `intrinsic/convert.h` takes the source's maximum and the
  destination's minimum, so `Int` reaches a `Size` position on every target and a `U32` reaches
  `USize` and not `Size`. `explainAbstractWidth` is the note that follows the second refusal, because
  it is correct and surprising in the same breath - on this machine a `Size` holds a `U32` twice
  over, and on a host whose word is thirty-two bits it does not.

**`Int` and `Long` are temporary builtins here**, spelled the way `lib/Core/Core.yana` spells them -
`alias Int = I32`, `alias Long = I64`. They exist for the fixture corpus, most of which writes its
integers that way, and they go when milestone 6 makes `alias` a declaration this stage resolves. An
alias is not a type of its own, so nothing downstream can tell which spelling was written and every
golden says `I32`.

**Arithmetic is truncated to a declared width** at every width narrower than the register it arrives
in, and the rule is split across the two stages rather than stated here: `op.def`'s width column says
which operations can leave the range, and `resolve_ssa/lower/width.cpp` says whether the register is
wider than the range on this target. See
[The width seam, and the division guard](#the-width-seam-and-the-division-guard), which is where the
invariant it rests on - a narrow value is held in normal form - is written out.

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

**Fixity is a declaration, and there is no fallback for it.** `infixl`/`infixr` is read into the
program by module and looked up through the imports like any other name, and `lib/Core/Core.yana`
declares one for every operator this stage computes. An operator with no fixity in scope is a
diagnostic, which is
`doc/spec/declarations.md`'s rule verbatim: "there is no default precedence".

`kOperators` carried a precedence column until 2026-09-02 and it *had already diverged from Core*:
`shl` sat one rung above `and` there and beside it in Core, so `a and b shl c` was `a and (b shl c)`
here and `(a and b) shl c` under the shipping stage - one source, two programs, nothing to say so.
The table is gone entirely now - see "Operators are calls" below.

A module's **own** declaration wins over any import's, because `name.h`'s `search` reads the module
before it reads an import - so a file may group `*` however it likes and Core's is what everyone
else gets. `test/resolve/module/Fixity.yana` asserts both halves.

**`Program::core` is the library's Core, and the implicit edge is live.** `discoverModules` seeds it
before it walks a single import, because a program whose source names nothing still needs the three
ownership classes and `[a]` to mean something - so the edge `linkImports` had written and left
dormant went live with no change to `linkImports` at all, which is what the dormant version was for.
The module it seeds is `lib/Core`, found by `LibrarySource::directory` - the same answer the
shipping stage gets, since J21 retired the stand-in library this stage used to read.

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

**A result the declaration did not write is decided by the body.** `fn double(x: I32) = x * 2` has
no `-> T`, and what a signature holds until the body says is `{}` - a placeholder, marked by
`Function::inferResult`, because unit is an answer that type-checks and a body left at it would have
its value computed and dropped at the `ret`. What makes the answer independent of declaration order
is that a caller **demands** it: `checkCallable` calls `requireResult`, which resolves the callee's
body at the call if the walk has not reached it yet - the same shape as `aliasTarget`, and for the
same reason, since neither is a dependency an order could be given to. `resolveFunValue` and
`resolveProgramEntry` ask too, being the two positions that read a signature without calling it.
`resolveBody` is therefore re-entrant: it saves what the walk carries about *which function it is
in* - the names, the held temporaries, the open loops, the lens pair, the lambda frames - exactly as
`resolveLiftedBody` does one level down, and `Function::bodyState` is the three-colour record that
keeps one declaration from being built twice.

**Recursion cannot infer**, and the call that closes the cycle is the one place that can say so:
the callee's body is on the stack, so the type being asked for is the type it is about to produce.
It is reported once, there, and the callee stops being callable rather than being recorded as unit -
`Function::resultUnknown`, which is the poison this stage has instead of an error type. What is
inferred is an **owned** result and only that: a borrowing one would have to name an origin slot no
parameter published, and promoting a parameter's group to satisfy a body is exactly what §2.2
refuses at a written signature. An `=` body that produces nothing warns and points at the `:` form,
suppressed where `-> {}` was written and where the body already reported. Only a plain free function
infers - `declareFunction` is the one path such a declaration takes, and a class member's result is
its class signature's, an instance member's is inherited from it and a lens's is its continuation's.
`test/resolve/ownership/ImplicitReturn.yana` and its `Reject.` half are the fixtures.

One thing the demand does not reach: `checkExports` runs before any body, so a `pub fn f() = Secret
{...}` leaks its result type without being told. That is where the shipping stage is too.

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

A dot-call's **receiver is position 0** of the same mapping - `ArgSite::leading`, which is a count
rather than a synthetic entry in the written list so that the walk stays exactly the source the
author wrote. `x.f(self: y)` is then the ordinary "given twice", reported by the line that reports it
for `f(y, self: y)`, and a named argument, a default and a `&` parameter each mean at a dot-call what
they mean anywhere.

**A default is a `Constant` on the position**, `Arg::def` beside `RecordField::def`, and both go
through `declareDefault`: `doc/spec/functions.md` points a default argument at the field default's
rules rather than restating them, so the compiler has one function for the pair. It is a constant and
not an expression because there is no program point at which a declaration would run, and what a site
that omits the position builds is `addConst` - the same instruction a written literal reaches. Two
calls that both omit a position therefore hold two values, which is what makes an aggregate default
mean what it says when the constant resolver grows one.

**A declaration answers a *range* of call arities.** `Function::requiredArity` counts the positions
with no default and `writtenArity` is the whole list, and `Program::addFunctionName` files the
declaration under every count in between. So a call site still reaches its overload set by a single
hash read, and what a second *plain* declaration collides with is a range instead of a point, which
is what the conflicting arity in that message is. `Function::acceptsArity` is the predicate every other "does this serve the call"
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

**A construction may write its fields by position**, in declaration order - `Cell(5)`, `Tagged{value}`,
`Proceed(x)` - which is the same mapping with the names left to the declaration. `Proceed(x)` is not
a special case of it either: its one field carries the constructor's own name (`declareFields`), so
filling position zero *is* naming that field. Where a construction parts from a call is that it is
**all one or all the other**: a call may write its positional arguments and then start naming, and a
construction's positions are the record's fields, which have names - one that has started using them
has no reason to stop, and one that never used them is reading the declaration in order. That is the
rule `doc/spec/types.md` already states of the shape a record is closest to, the tuple.

One gap, stated where it is: a call through a function **value** takes no names, because `FunParam`
carries none and `doc/spec/expressions.md` makes them part of the type.

### `x.f(y)` is `f(x, y)`, and where the choice between the two forms is made

Built 2026-09-06. There is no `impl` block and no per-type method table: what a body can call, a body
can call with a dot, this module's private functions included. `resolveDotCall` is the whole of it
and it is one function because the receiver is resolved **once** - resolving is emission, so a form
that decided between a field projection and a function by resolving the receiver twice would leave
the first walk's instructions behind in a stage that runs no pass to remove them.

It is resolved as a **place**, and that is what makes the form no different from a written call
rather than a second call path. What a receiver is *read* as - a copy, a move, a shared loan, an
exclusive one - is `readMode`'s answer at the position it fills, and the position is not known until
the callee is; a place is exactly what can be carried across selection. So `&self` borrows the
receiver's storage, `->self` moves out of it, and `argumentMode` stays the one rule. A receiver that
is not storage is the temporary `projectionTarget` already gives `f().x`.

**The field wins**, and the consequence is stated rather than hidden: adding a field to a record
shadows a dot-call on that type. It is the same shape as a local binding shadowing a declaration and
is decided the same way, by asking the nearer thing first. `fieldNamed` is the one walk over a
record's fields and it reports nothing - `fieldOf` turns its no into a message and `resolveDotCall`
turns the same no into a search for a function - because a second predicate written for the second
question is the last place two readings of a record may drift, and what it decides is which of the
two forms this is. On a **generic** receiver the probe is `findProperty`, never the requiring form:
a declared `a.name: T` selects the field and its absence selects the function, and the requiring form
would report a missing constraint before the function half had been tried. A binding of the name is
deliberately not consulted at all - `f` in `x.f(y)` is a member position and not a scope one, so a
local called `map` does not become every value's `.map`.

**A function written in a type's namespace is R1's one plain function for that type.**
`namespacedName` composes `String.push` from the receiver's record declaration and looks it up like
any other name, so `pub`, the import lists and `hiding` decide the dot form exactly as they decide
the written spelling. It is the plain half rather than a candidate joining the set, which keeps "at
most one plain function" true where two of them could otherwise reach one call. The namespaced
reading is asked **first**: `search` reports an ambiguity as it looks, so a bare lookup performed
first would already have said "visible through more than one import" by the time the type's own
function could have replaced its answer. Composed rather than registered by a declaration pass - the
shipping stage keys a hash map on (declaring type, member) to avoid the concatenation, and here that
would be a second table to keep in step with the declarations for a question `findOverloads` already
answers.

Class members come along free, R4's `Integral.and` included, because the set is `findOverloads`' and
the receiver is one more argument to select on. A `lens fn` and an `iter fn` are reached through
`namedCallee`, which asks the **name before the receiver** and the order is load-bearing:
`upTo(n).twice()` names nothing, and resolving its receiver to find that out would emit it and then
report "upTo is an iterator, write it as the source of a `for` loop" - which is what the author did
write. `handoverCall` resolves the receiver afterwards, in the caller's block and before the
arguments, which is where a written first argument would have been.

Three things the form does not reach, each grammar rather than rule. A `@lazy` first parameter, since
deciding the form needs the receiver evaluated and a deferred position is the one that must not be.
Type pushdown onto the receiver, for the same reason. And a receiver whose own resolution depends on
the access - `xs[i].bump()` where `bump` takes `&self` - is read shared, because `Index.get` had to
be emitted before a candidate could say it wanted `getMut`; the shipping stage refuses that program
with the same reason. `test/resolve/DotCall.yana` asserts every shape by a number and both stages run
it against one `.run.expect`; `AtomicOwnership.yana` is the class half at `Atomic(a)`.

### Records, and what a type owes

A `data` declaration is a `RecordType`: a nominal type with a name, a module and an ordered field
list. Two declarations of one shape are two types, which is what nominal means and is why the
identity is the declaration rather than the field list.

An **anonymous record** - `{I32, I32}`, `{x: I32, y: I32}` - is the same `RecordType` with
`tuple` set, and the two facts that follow are the whole difference. It has no declaration to be
nominal about, so it is *interned on its field list* (`Types::tupleType`); and it belongs to no
module, so nothing looks it up by name and `Program::tuples` is what the classification pass reaches
it through. Interning is what makes it structural without weakening anything: two spellings of one
field list are one handle, so `sameType` stays a handle comparison as it is for every other kind, and
`data Pair {x: I32, y: I32}` is still a different type from `{x: I32, y: I32}` because a declaration
is not a field list. A kind of its own in type.def would have been a parallel case in the layout, the
ownership fold, the schema, the projection, the construction and the matching - six readers, for a
shape that differs in two facts.

Its fields are **owners**, and there is no spelling for anything else: what a position holds is
written on the declaration a tuple has not got. Its one constructor is what `RecordType::cons` says a
product has - a reader asks the constructor list rather than testing whether a record "is a sum".

Two things read a field list as syntax, and they are one function. `constructorFields` takes the
braced list written after a constructor name and the one written alone, because they are the same
syntax over the same fields, and the coverage matrix and the emission both go through it - an answer
that differed between them would be a `match` checked as one program and compiled as another.

**A tuple written as a match pivot is never built** (`decomposeMatch`). `match {lhs, rhs}` pairs two
scrutinees so that one match can look at both, and building the tuple anyway is a second owner of
each element: a tuple is contiguous storage, so its fields are copied in, and a copy of a value with
a teardown gets a teardown of its own. The elements are already values, and matching against them
directly is the same tests with no storage in between. Every alternative has to be a positional field
list of the right arity or `_`; a pattern that binds the pivot whole wants the tuple to exist, and a
named field refers to labels an unlabelled literal has not got.

`e.0` selects a field by **position**, and reaches an anonymous field only (`positionalField`). A
named field is reached by its name and by nothing else: an index beside a name would be a second
spelling of the same slot that stops naming the same slot the moment a field is inserted, and the
declaration that moved it says nothing about the call sites that counted. Everything else about it is
the ordinary projection - the same single-constructor rule, the same visibility check, the same
`fieldPlace` - because a position is a way of *naming* a field and not a way of reaching one.

**A constructor's name has nothing to do with its type's**, however many there are.
`doc/spec/declarations.md` states it as `data Direction = North` declaring a nullary constructor
`North` and "never a type named `North`", and the two tables here say it from the other side:
`recordsByName` and `constructorsByName` answer different questions, so `data RandomError =
NoEntropySource` files one name in each and neither spelling reaches the other. The braced form is
where the two coincide, and it coincides because the parser synthesizes a constructor named after
the type - nothing below requires it to. A **nullary** constructor is the other half of the same
sentence: `NoEntropySource` is a value of `RandomError`, zero bytes wide, and a thing a program
passes, matches and names in a signature.

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

### A record update is one copy and a run of writes

`{p | y: 3}` is not a construction and `resolveUpdate` is not near `resolveConstruction`. A
construction is a component per field handed to an aggregate; an update starts from a value that
already has every field and replaces some of them, so what it needs is **storage** - and once there
is storage a path is a place projection and nothing else. That is what makes `{v | .a.b: 1, .a.c: 2}`
set two fields of one `v.a` *by construction* rather than by a rule: both paths are projections of
the same local, so there is nothing for a second copy to discard, and the source expression is named
once however many paths there are.

Every replacement is a `Destination::Assign` through the same `Builder::addWrite` a written
`v.f = x` reaches, which is the one thing this construct owes and the one thing the shipping stage
leaves undone: the copy put a live value in every field, so a field being replaced has one to
release. `Update.Owned.yana` counts them.

**What the copy is, is the ownership rules and not this expression's business.** The source is read
once, at an owned request, and `readMode` answers with the duplication a `TrivialCopy` type gets for
free. Where the type has no such answer the language's word for a duplicate is `Copy`, so what is
emitted is the ordinary `copy(v)` class call - the first place in the compiler that *calls* that
member rather than reading the capability it subtracts. `{->v | ...}` is the same shape with a
**relocation** instead, which is why a type with no `Copy` can still be updated and is what the
refusal names. A source that is *produced* - a call result, another update - is in no storage for
either to act on and is simply the value, so `->` on one is neither an error nor a second path.

The order is written rather than incidental: the replacements are resolved **before** the source
relocates, so `{->v | x: v.x + 1}` reads `v` while it is still there, and a copy is taken as early as
it can be because it leaves the source readable. `Record.yana` is the shared fixture,
`ownership/Update.Copy.yana` the authored-`Copy` half the shipping stage cannot compile, and
`ownership/Reject.Update.yana` the refusals.

### Containers, and why there is no slice type

Two spellings, and **one of them is not a type**. `doc/spec/collections.md` and
Analysis-Borrows-Revised.md §3.1 both say it: a container type names a capability and the position
says which one.

| Written | Position | Logical type | Contract |
| --- | --- | --- | --- |
| `[T *n]` | any | `ArrayType` - the count is part of it | the position's own |
| `xs: [T]` | parameter | `Array(T)` | shared, `View` |
| `&xs: [T]` | parameter | `Array(T)` | unique, `View` |
| `xs: src'[T]` | parameter | `Array(T)` | shared through slot `src`, `View` |
| `xs: Array(T)` | parameter | `Array(T)` | shared, `WholeValue` |
| `->xs: [T]`, `-> [T]`, a field | owning | `Array(T)` | refused - there is no allocator |

`Array(T)` is a **declaration**, not a kind: `@builtin(array)` claims it, `lib/Core/Array.native.yana`
writes it and `lib/Core/Array.native.yana` writes the same attribute on the real one, so `[T]` and
`Array(T)` intern to one handle exactly as the spec says they are one type. What the compiler decides
about it is the *window*, whose two words are `manifest.cpp`'s answer and appear in no type.

**This is the whole of what the rewrite is for, and the shipping stage is the argument.** There,
`&[T]` and `l'[T]` are third and fourth `TypePtr`s - `sliceCapabilityType` mints a sibling
`RecordType` carrying `canonical`, `sliceMut` and `sliceLoan` - and the cost is on the record:
`substituteType` dropped the capability, so a class member declaring `-> &Flat(a)` specialized to a
*shared* window and every instance was compared against the wrong one; `bindingType` has to fold the
sibling back into a `BindType`; `sliceOf` throws the count away, so `'[Int *4]` written as a type
loses it and such a function cannot declare its own result; and ~58 sites have to ask "or a sibling
of one". Here the form is four bits of `Contract`, `sameType` is untouched, and `substituteType` has
nothing extra to preserve.

**An element is a place.** `Place::Index` is a projection of the container's own place, whose payload
is the node producing the index. The shipping stage reaches one as `borrowMut(self.run.items + index)`
instead, and both consequences were real: a `[T *n]` local's teardown was placed *nowhere*, because
`rootLocal` of an address is nothing and the local never became owned; and `f(&xs[i], &xs[j])` was
accepted with no check of any kind, `xs[0], xs[0]` included, reachable in safe code as a
use-after-free. Rooted at the parent, `place.h`'s own rule - "`Index` steps of one parent are not
[disjoint] unless a separately justified rule proves it" - applies with nothing added, and
`ownership/Reject.ArrayBorrow.yana` is the fixture.

**The adaptation is a node over a borrow.** A `[T *n]` argument reaching a `[T]` position is a
`window` node whose operand is the value the `read` produced - so the loan is the read's, the window
carries it (`analyze_loan.cpp` unions the source's origins into the result's root), and every question
about how long a window may live is a question about that loan. The shipping stage builds a two-word
*record* here, which is a fresh owned value whose relationship to the container then has to be
recovered afterwards through `Local::viewOf` and `resolveViewRoots` - and while one of those was
missing, `sum(slice(elements(xs), 1, 4))` released `xs` before reading through the window.

`requestOf` grew a third `published` case for the same reason: a window describes the *caller's*
storage, so a trivial copy cannot satisfy the position. Without it a `[U8 *4096]` argument is copied
at every call and the callee gets a window into its own frame.

**A literal takes its type from context and never from itself**, which is what makes `[0, 1, 2]` a
`[Int *3]` in one position and a growable `[Int]` in another. Two spellings say what the count is:
`[0] :: [U8 *64]` fills the whole array, and `[3, 1, 4] :: [U8 *_]` has the count read off it.
`ArrInferred` is the second, and `resolveAscribedType` is its one reader - the `::` of an expression
and the `::` of a constant both go through it, because they are one construct and `_` cannot mean
two things. Every other type position calls `resolveType`, whose arm for `ArrInferred` is the report:
a parameter, a field and a return type reach it by having no literal to count, exactly as
`doc/spec/expressions.md` says. The two spellings are complementary rather than composable, and that
falls out rather than being a case - `[0] :: [U8 *_]` is a one-element array, because the count a
fill would spread into is the one being read off the literal.

`xs[a..b]` is `slice`, `length(xs)` is `Length`'s member and `xs[i]` on anything but a fixed
array is `Index`'s - library bodies over the view's own fields, grafted where they are called by
`@inline(always)`, so `length` on a fixed array is `Length([a *n])`'s `n :: Size` and a constant
with no window built. `doc/inflight/Analysis-Manifestations.md` Part 9 is the record; the window
and length nodes this paragraph used to describe are gone.

**What is not built**: the growable owner (no allocator), `[k:v]`, `Contiguous`/`Chunked`/`Writable`,
and the **bounds check** - `doc/spec/collections.md` makes an out-of-range subscript a trap through a
library function and there is none to call, so a subscript here is unchecked and the fixtures say so.
A move out of an element is refused: a move path names one object and `xs[i]` is a run whose length
may be a const parameter, so "which element is gone" is not a state a path can hold.

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
   `(Eq, Array)`, so a lookup narrows to a handful of candidates before anything is matched - which
   is what keeps coherence a property of a hash table, since two heads of one shape collide at the
   declaration.
 - **A deciding position the head leaves open is a wildcard in that key**, one position at a time,
   and the candidates are tried **most specific first** - fewest wildcards. `instance (TrivialCopy(a))
   Copy(a)` names no constructor and is the all-wildcard entry, so it loses to
   `instance (Copy(a)) Copy(Array(a))` for an array by the ordering rather than by either of them
   saying so. This used to be two states - keyed, or wholly open, at most one of the second per class
   - and `Rewrap(m, b -> n)` is what could not be said in them: its instances name a constructor in
   `m` and a variable in `b`, so all three were filed as "matches everything" and the second and third
   were reported as duplicates of the first. See `instanceKey`, where the enumeration is bounded.
 - **The head's own context is a goal, not a filter.** A matched head whose `(Eq(a))` does not hold
   is reported rather than passed over - passing over it turns one missing instance into "no instance
   of `Eq(Array(Guard))`", which names a type nobody wrote. Asked from inside a generic body it has a
   second source of answers, the enclosing signature's own clauses - see the requirement paragraph
   below.

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
"what did somebody declare *about this type*" is the right one - and of what a keyed head declares,
a `Copy` whose own context asks for `Copy` of the head's variables declares nothing the member fold
has not: `inheritedCopy`, and the paragraph on the three ownership classes below.

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

**An instance's superclasses are checked in a pass of their own** - `checkInstanceSuperclasses`,
between `declare` and `checkExports`. It was a line in `declareInstance`, which made the answer
"whichever instances have been read so far": `instance Contiguous(Array(a), a)` was refused for a
`Chunked` written below it and accepted for the same one written above. Whether a program compiles
is not allowed to depend on the order its declarations are written in, and there is nothing about
one table lookup that needs the table to still be filling - the same statement `resolveProgram`'s
phase order makes about `classifyTypes` waiting for the last `instance`. A parametric head still
proves its superclasses from its own context rather than from the table, which is the second of the
two answers below. `test/resolve/SuperclassOrder.yana` is the fixture, and it is in the shared
corpus rather than in `ownership/` because the shipping stage answers this too - a rule two stages
hold is tested against both.

**A requirement has three answers and deliberately no fourth** - `dischargeRequirements`. If the type
the call chose is concrete, the instance has to exist *at the call*. If it is one of the enclosing
declaration's variables, the enclosing declaration has to require it, and the obligation is passed on
rather than proved. And - inventory D13, 2026-09-06 - if its **outermost constructor is concrete and
the body's own variables are inside it**, the table answers: `xs[i]` in
`fn (Ord(a)) firstBelow(xs: [a], i: Size, j: Size) -> Bool` asks `Index(Array(a), ?, ?)`, and a head
matching `Array(a)` with `a` free matches it at every type a specialization binds, because matching
is one-way. `selectOpenInstance` is that answer and it is where the three conditions on it are
argued: a deciding position that is a **bare variable** is still refused, for the reason the rule was
written - a body answering a question about its own type parameter from whichever instances happen to
be declared is what makes a specialization mean something other than the body it came from; every
variable in a deciding position has to be one of the *enclosing declaration's*, since an inference
variable of a half-resolved call site is not a settled shape; and a **narrower head that answers the
determined positions differently** is reported naming both, because that is the one way the query's
answer and a specialization's could come apart. What is recorded is the requirement rather than the
instance - it travels with the body under the substitution and is discharged again at every
instantiation, exactly as a written clause is.

The same relaxation is what lets a matched head's own context be discharged from the enclosing
signature: `instance (Tag(a)) Sized(Pair(a), a)` reached from `fn (Tag(a)) measured(p: Pair(a))`
needs `Tag` of the body's own variable, which no table holds. `Selector::enclosing` carries it and
`holdsContext` asks it first, which makes the derivation recursive through the same rule rather than
only its first step. Fixtures: `generic/OpenHead.yana`, `generic/Reject.OpenHead.yana`.

**A structural class is answered wherever a requirement is discharged**, not only where a caller
entered. `TrivialCopy` and its three neighbours have no table row and never will, and
`structuralAnswer` used to be asked at `selectInstance`'s door - so a structural goal standing inside
*another instance's* context reached the table, found nothing and was reported as a missing instance.
`instance (Magnitude(a, w), ..., TrivialCopy(w), ...) ReadWith(NumberStyle, a)` at `readHexInt` is
the shape: `Magnitude(a -> w)` decides `w` to `U32` one requirement earlier and the next one was
"there is no instance of TrivialCopy(U32)", four times in `lib/Core/Format/Number.yana`. The question
now sits at the top of `Selector::select`, ahead of the memo and the cycle check because it is a fold
over the type rather than a search. `StructuralContext.yana`.

**A requirement a signature can *prove* is one it can be asked to *decide* from**, and the two halves
had to close over superclasses alike. `classProves` already walked them for the proof;
`fillFromRequirement` searched the enclosing signature's clauses for the class *as written*, so
`class (Num(a), FromDecimal(a), Lanewise(a, m), Bitwise(m)) Real(a -> m)` - whose `m` no member of
`Real` mentions - left every `value .<= 0.0` in a body constrained on `Real` reporting "no argument
decides what m is", 24 times across `lib/Math`. `heldPromises` is the closure: a clause, or a
superclass of one expressed in that clause's arguments, bounded by a depth for `superclassPath`'s
reason. `SuperclassDecides.yana`, which asserts the one level `lib/Math` writes - the shipping stage
looks exactly one superclass deep and refuses a second, which is a difference to settle before it is
written into a shared golden.

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

### Operators are calls, and the primitive instances are the compiler's

**There is no operator table.** `doc/spec/expressions.md` says an operator is "a member of an
overload set keyed by `(name, arity)`", and that is how one is read here: `a + b` finds `+` at arity
two through the same `findFunction` a written call uses, reaches `Num`'s member, and `finishCall` -
the one tail every call goes through - selects the instance at the operands' type. `a == b` on a
`Money` with a declared `Eq(Money)` runs the `==` the program wrote; `!`, `&&` and `||` are Core's
three plain functions over `Truth`, read through `op.def`'s `Flow` roles and laid out as the branch
they are. What the resolver holds about operators is `Operator` - the callee, the declared fixity,
and which of the three forms it is - and `role.h`'s `kNegationOperator`, which is a lexical fact
about `-1` rather than a role.

**The instances at the widths are generated** - `compiler/intrinsic/provided.def`'s direction 5,
and `intrinsic/provide.cpp` is where it happens. A row names a class by its `Known` role, a *family*
of types (every number, every integer, the two floats, `Bool`, `Ordering`) and what its heads are
made of - one type of that family, or a *pair* of them, which is the column the conversion ladders
needed. `provideInstances` declares one ordinary `Instance` per head with one ordinary `Function` per
member: a real signature substituted from the class's, a symbol like `I32.+`, and a body written by
`provideBodies` from the same emitter a call site reaches. Nothing below the resolver can tell one
from an instance an author wrote. What is special is one rule in `finishCall`: **a provided member
expands where the call is concrete and is called where it is not.** So `a + b` at two `I32`s is the
`add` node it always was, with no call anywhere; `x + y` in `fn (Num(a)) f(x: a, y: a)` is a call
node, and the specialization reaches the member's body through `callTarget` exactly as it reaches
any other instance member - which is also what the erased witness table holds a pointer to. One
statement of what `Num(I32).+` computes, two placements.

Three of the compiler's own constructs reach members with no operator written, each by the position
`known.def` states: a **condition** is `truthValue`, which is `Truth.truthy` of the tested value
resolved at its *own* type with no hint - `Bool`'s provided instance is the identity and builds
nothing, an integer's is the test against zero; a **literal pattern** compares with `Eq.==` and a
range pattern with `Ord`'s four, so a literal against a `Money` pivot runs the program's own
equality; and a **counted `for`** tests its bound with `Ord` and steps with `Num`, so
`for m in (2 :: Money) .. 5` counts over anything with both instances - `applyBinary` is the map
from the node an emitter would build to the member it is.

An **operator pattern** is the one that is none of those. `| >0` names an operator the *program*
wrote, so it is an overload set and a selection like any other call of that name rather than a
member the compiler picked - `sectionPattern`, which reads the pivot at the request
`operatorRequests` says the left operand is in and puts the answer through `truthValue`, so a
pattern and an `if` agree about what a condition is.

**A literal is a call to `FromInt`** - `doc/spec/core.md`'s rule, in three cases that are one path.
At a width the instance is the compiler's and its `fromInt` of a constant *folds*, so `1 :: I32` is
`const 1` and no `Long` stands above it (`foldLiteral`, shared with the expansion). At a type with a
declared instance, `5 :: Money` is `fromInt(5)`. At a variable of the enclosing signature, `x + 1`
inside `fn (Num(a)) f(x: a)` is a call discharged against `FromInt(a)`, which `Num` requires of every
instance - and for that the hint a call hands its arguments is now "decided as far as this call is
concerned" (`mentionsContext`) rather than "concrete", which is what lets an enclosing variable
reach a literal. A constant position - a field default, a global's initializer - asks the family
rather than the instance table, because it is resolved before the instances exist, and refuses a
type whose literal is a call. The generated `fromInt` bodies are where the one `convert` unary op
comes from: `Long` to the instance's own type, lowered to `cast`.

`Outcome` and `Ordering` are `Known` record roles for the same reason `Bool` is: the compiler
builds values of both - `compare` at a width answers an `Ordering`, and a skipping lens answers its
continuation's result inside an `Outcome` - so it has to name the declaration and number the
constructors, and `declareOutcome` now instantiates Core's `Outcome(a, e)` at the exit signal and
the failure type rather than synthesizing a record per lens with `Proceed` and `Exit` spelled in
C++.

**An operand is an argument, and is asked for what its parameter declared.** `climb` resolves each
operand under `requestOf` the contract of the position it will fill, which is the rule
`resolveArguments` states for a written call - and which position that is falls out of the precedence
climb rather than being decided a second time: the operand at hand is the *left* one of the operator
being consumed exactly when the loop consumes it, and otherwise it is the right one of whatever
called in, so the request travels down. Reading every operand as a shared inspection instead was a
miscompile rather than a missing feature: `+=(&target: a, amount: a)` is an ordinary call, a shared
read of `x` produces the *value* rather than a loan of the place, and `x += 1` then incremented a
temporary and left `x` alone. `SharedStorage.yana`, `Borrow.yana` and `DropFlag.yana` are the three
corpus fixtures that were held back by it.

**Two rows are generated over something other than a family of widths.** `provided.def`'s
`Family::Shapes` is the marker for a row whose heads come from a *generator*, because the set is not
enumerable from the target's primitives: `Lanewise(Vec(a, n), Mask(a, n))` is one parametric head,
and `Enum(a -> w)` is one head per `data` in the program. What such a row is still there for is the
emitters, which are reached by `(role, member position)`.

`provideEnumInstances` is the second of those, and it is the one pass here that runs **after** the
written instances rather than before them - `resolve.cpp`'s order, right after `declare`. That
position is what makes "a declared instance wins" a condition instead of an ordering: the walk skips
a head the table already holds, where the shipping stage gets the same rule from consulting its
hooks only on a lookup that found nothing. Each payload-free sum gets three:

- **`Enum`**, which nobody may write - `valueOf` is the tag read at the width `enumValueTypeOf`
  determined (`Int`, or `I64` where a `@value` needs it), and `fromValue` is a range test where the
  numbers run consecutively and one comparison per constructor where they do not, then the tag
  written. `nameOf` and `fromName` are the same pair in the other currency and are generated too:
  `nameOf` is a comparison chain choosing one string constant, built by `textValue` so that a
  generated word and a written literal are one placed global, and the last constructor is the
  fall-through rather than a tested arm because the tag *is* one of them. `fromName` is that chain
  the other way round over **`Eq`'s own member** at the name's type - what comparing two strings
  costs is the library's, and the only thing the compiler contributes is the list of words, so the
  class's "compare lengths first" is `Eq(String)`'s body and not a second string equality living
  here. Neither is a multi-way form: inventory D3 says a jump-table shape is dedicated IR carried
  through to the backend, not something the front end reconstructs. `EnumNames.yana`.
- **`Eq`** and **`Ord`**, which replace the one line every enum used to write -
  `valueOf(lhs) == valueOf(rhs)`. A generated instance rather than a `structuralAnswer` beside
  `TrivialCopy`, and the choice is forced: a structural answer names no instance, which is sound only
  for a class with no members, and an erased witness needs a real function in the slot. `Bool` and
  `Ordering` are left out, because `provided.def`'s `Comparable` and `Numbers` rows already ruled on
  both.
- **`Show`** (2026-09-07), whose whole body is `nameOf` handed to the string sink: a constructor with
  no payload has exactly one text form, which is its name, and every enum in this tree that wrote a
  `Show` wrote that. Two calls and no graph of its own, deliberately - repeating `nameOf`'s chain
  here would be a second place for the two to disagree - and `showBound` answers the longest name as
  a constant, which is what the class's exactness contract asks for and what lets an interpolation
  of an enum size its buffer. `Known::show` and `stringPush` are both `Optional` and both asked, so
  a library that declares none of them generates none of these and a format hole still reports the
  missing role by name.
  This one is generated for `Ordering` as well: those two exclusions are `Comparable`'s and
  `Numbers`' rulings about *order*, and neither says anything about text. `Bool` keeps the `Show`
  `lib/Core/Show.yana` writes, by the same `unclaimed` rule as anything else. It is 55 of the
  library suite's 109 diagnostics, over eleven types.

An enum reaches the *comparisons* as its tag - `Eq` compares two of them as they stand, since
`operandTypeAllowed` admits a record to an equality, and `Ord` bitcasts first, since an ordering is
arithmetic. `UnaryCategory::Raw` admits an enum for that reason and for the two crossings `Enum`
itself is made of.

`ByteSwap` is an ordinary row beside `Bits`, over `Family::Swappable` - 16, 32, 64 and the target's
word, which is `hasByteOrder` and is `hasMachineWidth` plus sixteen: a count reads a value and needs
a width the machine counts in, a reversal reads its *bytes*. The 16-bit case is the one width the
lower IR has no scalar for, so `narrowByteSwap` spends it as the shift pair there, beside
`narrowRotate`.

`BitPermute` joined them 2026-09-07 - `gatherBits` and `scatterBits`, x86's `pext` and `pdep`, over
`Family::Machine` because a gather reads the *value's* bits at the mask's positions and a narrow
value would be answering about the register's padding. Two `op.def` rows (`GatherBits`,
`ScatterBits`, `Closed` because the library is explicit that both are total) crossing to
`lower/inst.def`'s own pair as themselves. `BitPermute.yana` and `BitPermute.Bmi2.yana`.

Fixtures: `Operators.yana`, compiled by both stages against one run expectation,
`ownership/CountedFor.Class.yana` for the loop the shipping stage refuses, and `EnumInstances.yana`
and `ByteSwap.yana`, both shared.

### Conversions, and where `::` stops

**`::` selects a target type, and it may widen.** That is the whole of it, and most of what an
ascription does is not a conversion at all: passing the type down makes `1 :: I64` one constant
rather than a widened `I32`, picks the instance in `truncate(x) :: Int`, gives an array literal its
element type and a lambda its signature. What is left when the value still has another type is one
call to `convertValue`, which is the same function every implicit position goes through -
`convert.cpp`, and the file is the argument for there being one.

**Which class relates a pair is `compiler/intrinsic/convert.h`**, and it is read by *both* resolve
stages. That is not tidiness: a fixture is compiled by both and the two answers compared, so two
ladders that disagreed by one rung would be one source that compiles under one stage and not the
other - the same failure the fixity table had before `lib/Core` took it over. `resolve/core.cpp`'s
`defineRung` and this stage's `eachHead` are two walks over one rule.

The rule, in three parts. `Widen` where every value of the source is a value of the destination -
`integerRangeFits`, which reads the source's **maximum** and the destination's **minimum**, so a
target-answered width answers conservatively at both ends and the pair is decided once for every
machine. `Narrow` where it is not, plus the identity pair, which is what makes `truncate` total:
`Size` is `I64` on one target and `Int` on another, so `truncate(length(xs)) :: Int` has to compile
on both and on one of them nothing is cut. `Bitcast` over the pairs that are the same bits on every
target, which is about twenty rungs - a width the target picks pairs with the same target quantity
and with nothing else, and `WideInt` has no partner at all.

**A conversion is an ordinary call.** `convertValue` selects `Widen(from, to)` and then goes through
`applyMember`, so the arguments are prepared, the constraint discharged and the instance expanded by
the same `finishCall` a written `widen(x)` reaches. At a width that expansion is the one `convert`
node it is; at a type a program declared an instance for it is the author's body. Which is why
`instance Widen(Money, Cents)` in a program reaches every position below with nothing added here.
`applyMember` gained a result **hint** for it, and that is what makes a conversion selectable at all:
nothing about `widen(x)`'s argument says which rung was meant, so `Widen`'s second parameter is bound
from the type being converted to.

**A sigil in front of an ascribed type names the type under it and nothing else.**
`let view = Native.atomicAt(p) :: 'Atomic(U32)` is what writes one, and what the tick says there is
what the expression already *is* - the call hands back a view of somebody's storage, and the
ascription is there to say which `a` the view is of. An ascription is a hint and a widening, so it
names a type and never a contract: nothing here could make an owned value into a loan, and a `let`
reads its own ownership off the initializer the way it always does. A *label* is refused rather than
dropped, because it would be naming an origin set and an expression is not a signature.

Two refusals are the file's own. A **narrowing** is reported naming `truncate` rather than as a
missing instance, in either direction - an implicit position says what type it wanted, an ascription
says that `::` does not narrow - and `explainAbstractWidth` follows it where one side's width is the
target's, because `let n: Size = someU32` is correct to refuse and surprising to have refused. And a
position the callee **writes through** may not take a converted value at all: widening an `I32` local
into an `&I64` parameter would hand the callee eight fresh bytes and the local would never see what
was written, which is `conversionFillsPosition` and the one place the access of the position matters.

What is *not* here is the **view adaptation** - a `[T *n]` or a container reaching a `[T]` position,
a `String` reaching a shared one. That changes the logical type without changing who owns the
storage, so it is a call to the derivation the view table names - `Contiguous.elements`,
`Writable.elementsMut`, `Viewed.view` - and `adaptToView` runs ahead of the coercion at the positions
that have one. A conversion builds a new value; an adapted view is the caller's bytes described by
a record the library declared. A fixed-array literal at an *owning* `[T]` position is still reported
by `reportOwnedSequence` rather than as a `Widen` instance nobody would write.

**A derivation may not ask for its own view.** `fn elements(self: source'Array(a)) -> source'[a] =
unclampedSlice(self, 0, self.length)` reads as the right statement and is not one: `unclampedSlice`
takes a *window*, so the argument position asks an owner to become one, and the only thing that
turns an owner into a window is this member. The shipping stage's conversion is structural there and
the recursion never appears; here the adaptation is the call `known.def` says it is, so `elements`
called itself on every path and `let xs = [1, 2, 3]; xs[0]` compiled against the real `lib/`
segfaulted with no diagnostic anywhere. `Function::derivation` records the member and `adaptToView`
refuses it - same type, same access, which is what makes it the recursion and not a neighbour, since
a container whose `elements` derives a *different* container's view is an ordinary call and so is
`elements` reaching the unique derivation. What a derivation has to do instead is *build* the view
record out of the owner's fields, which is the `addRepresent` case beside it and what
the stand-in library always did; `lib/Core/Container.yana` now writes the same four bodies,
platform-split because the view record is a representation and the two targets do not share one.

Fixtures: `Convert.yana`, compiled by both stages against one run expectation, and `Types.yana`,
`Literal.yana`, `Promote.yana`, `Unsigned.yana` and `BitcastNarrow.yana` among the sixteen the
ladder adopted. `ownership/Reject.Convert.yana` is the other side - the two refusals, the
target-width note, and the position a converted value may not fill.

### One branch construct, and the two forms that are not it

`armsValue` is the shape every choice takes: one block per alternative, one join, and a failing test
jumps to *the next alternative's entry*. Nesting two-armed branches instead gives each test a merge
of its own to fail into, and the alternatives after it are then reached along a different path per
test - which is why `match`, `if`, `if:`, `&&`, `||` and `?` are all one call of it rather than a
two-armed form used n-1 times.

`if:` with a case list is that written out. Its conditions are resolved **inside their own arms**,
unlike the two-armed form's, and that is the whole of what a case list means: case two's condition is
code that runs after case one's failed, so it belongs in the block that failure reaches. `else` is
not a condition - the parser writes it as the condition that always holds, so that a case list stays
one production, and `isElseCase` is the only place below the parser that knows the encoding. It reads
back as the arm with no test, which is what makes the construct answer on every path; a value
position without one is refused in the words the two-armed form uses.

`let x = e in body` is a value wherever `body` is one, and `in` is what makes it one at all: a
binding whose scope is not written has nothing inside the expression after it. `resolveScoped` is the
statement and the value form together, with one tail, because the difference between them is what the
body is asked for and nothing else.

**An arm may leave instead of answering**, and `resolveArm` is where the three constructs that have
arms written in the source say so once. `if c then return 7 else a` is the shape: `return e` is not
an expression of some bottom type for a merge to eliminate afterwards, it is the statement that ends
the function, so it is resolved as one and the arm answers nothing. `armsValue` already reads that
off a null value with a terminated block, because an arm whose *block* ended in a `return` had the
same shape; this is the bare form of it, and the merge then has one predecessor.

**The arms unify to their unique `Widen` upper bound**, which is `commonWiden` and is the one place
in the stage a conversion decides a type rather than filling one -
`doc/spec/overloading.md` states the exception for the positions of a single call bound to one class
variable, and a set of arms is the same question about the same ladder. The fold runs after every
alternative has been resolved, because until then there is nothing to fold; and the widening it
decides is then emitted **in the block that produced the value**, since an edge into a merge supplies
the merge's own type and `closeArm` has not built the jump yet. That ordering is why `mergeArms`
takes the arms' end blocks: `let v = if c then a else b` over an `Int` and a `Double` is one `convert`
in the `Int` arm, and nowhere else.

### A `match` over constants

A `match` whose every arm is compile-time data states a **mapping**, and a mapping is a value rather
than a shape of control flow. `matchAsMap` in resolve_match.cpp recognizes one and `mir::NodeMap`
carries it - inventory D3, and `compiler/resolve/map.h` is the shipping stage's version of the same
recognition, whose reasoning this inherits.

Two domains reach it. A **record**'s own constructors are a closed domain - a well-typed value holds
one of them and nothing else - so there is no out-of-range case to guard and the node carries no
fallback. An **integer** pivot is open, needs a wildcard arm, and what that arm answers is the
fallback; that half is new, since `recognizeConstructorMap` requires a record, and
`lib/Date/Format.yana`'s `monthName` is what it is for. The integer domain stops at `Type::Int`
because a machine integer's equality is the compiler's own operation, where a type with an authored
`Eq` runs a body the program wrote.

What a pattern may do is the eligibility rule read from the other side: name a constructor, with `_`
for its payload, or be `_` itself. An `as` binding, a payload pattern that tests anything, and a
`&->` that takes the payload out are each a reason to decline, because none of them is work a lookup
performs. Every refusal is a **fall-through to `armsValue`** and reports nothing: `PatternSpace` has
already said whatever there is to say about a bad match, and a second opinion would be a worse
message about the same line. That is why recognition runs after the warnings and after the
exhaustiveness check, and why it reads every pattern before it evaluates any arm - a pass that
evaluated as it walked built the first arm's string literal and then found the binding in the second.

**Nothing about the target is decided here.** The node always exists, and which of six shapes it
becomes - the constant, two comparisons, a bit mask, an affine step, a packed immediate, or a run of
slots in read-only data - is answered once, at lowering, in `resolve_ssa/lower/map.cpp`. The shipping
stage has to consult its size rule twice, once to promise and once to emit, because its recognizer
owes its caller an emission; here the promise does not exist. The one thing recognition still bounds
is `mapSpanAdmits`, and that is a statement about what a table may *be* rather than about whether one
is worth having: five of the six shapes read no memory, but the sixth has to exist for every mapping
the node admits, since there is no comparison chain left below to fall back to.

A **string** result is the one exemption to "the result duplicates freely and owes nothing", and it
is the shipping stage's: a literal's run is borrowed, so a slot holds two words describing bytes
nobody owns and its teardown is the nothing a written literal's is. What a slot holds is what
`stringLiteral` *returns*, built by running it - so nothing here spells a field of a `String`. The
building is deferred to `resolveTextMaps`, beside `resolveTextGlobals` and for its reason: which
bodies are resolved when a mapping is recognized depends on the order the files were walked in, and
that is not something a recognition rule may depend on. Within one mapping there is one blob per
distinct *text*, which a written literal does not get - constant equality is what decides the uniform
form, so two arms answering `"colour"` have to be one constant or a mapping with one answer becomes
a table with five.

A generated `Enum.nameOf` is one of these (`intrinsic/provide.cpp`), and so is `Enum.fromValue` where
the constructors are not one interval. The comparison chain is still written for the declaration a
table cannot serve - `@value(n)` may pin constructors too far apart to place.

### A `match` that dispatches, and the one node that is a terminator

A `match` whose alternatives each name a distinct constructor and test nothing below the tag is a
**multi-way branch**, and `mir::NodeSwitch` is that shape said once instead of as a chain of
comparisons. `matchCases` decides it and `armsValue`'s `multiway` form emits it: the entry blocks are
the case targets, the discriminant is read once in the block the `match` was written in, and every
alternative is then resolved with **no failure edge at all** - which is exactly what `armsValue`
already gives the alternative exhaustiveness proved.

Two nodes and not one, which is the same argument the roster makes everywhere else: a `switch` names
*blocks* and its arms may do anything, a `map` names *constants* and its arms may do nothing, and one
node whose arms were either would carry a flag set on half its instances.

The rules are narrow because the failure edge is what is being given up. `irrefutablePattern` is the
whole of it: a name and `_` bind, a braced list of them takes the value apart, and everything else -
a nested constructor, a literal, a range, a section - is a test that would have had nowhere to fail
to. Answering "irrefutable" wrongly is a dropped test, so the conservative direction is the one that
declines. The **last** alternative is the fallback and has to be the one exhaustiveness proved, since
that is the arm with no test; and below three cases it declines, because two comparisons is a chain
and two spellings of one shape is what the roster refuses.

**The fallback is mandatory and there is no exhaustiveness invariant on the node.** An IR that
restated exhaustiveness would oblige every pass that deletes a case to re-prove that the remainder
still covers the domain. A fallback costs one edge, and where the source was exhaustive it is an arm
that already exists.

The shape now reaches the bottom: `resolve_ssa/lower/inst.cpp` hands a `switch` across as
`LowerInst::Switch`, which is what D3's slice 3 widened `LowerBlock::outgoing` into a list for. What
it is *for* is the choice below that, taken per backend - `codegen/x64/transform_switch.cpp` spends
one as a comparison chain or a jump table, and the LLVM backend hands it to `CreateSwitch`. See the
reports for D3.

### The guard `let`, and what an alternative has to do

`let Just(v) = m | return 0` is the refutable declaration: the pattern may fail, the alternatives say
what happens when it does, and **every alternative has to leave the block**. That last rule is what
lets the names the pattern bound stay in scope for the rest of the block with no nesting level of
their own - there is exactly one way to reach the code after the declaration, and on it the pattern
matched.

So it is *not* an `armsValue`: there is no merge. The pattern's tests branch to a failure block, the
walk carries on in the success block, and the alternatives are built in the failure block and are
each required to leave. "Leave" is read off the control flow rather than the syntax, which is what
`departed` is for - a terminator is one way out, and a block nothing branches to is the other, since
a `match` whose every arm returned leaves an empty join behind and the walk standing in it.

The pattern and its alternatives are **one space**. `let Just(Red) = m` leaves three cases and the
alternatives have to cover them, so none of them needs a wildcard; and an always-matching pattern
with a `|` after it is an *error* rather than dead code, because `|` is a reserved operator and
`let flags = a | b` bound `flags` to `a` and dropped `b` on the floor.

What the pattern bound comes back to the caller rather than going into scope, because the
alternatives are resolved in between and the names are not theirs. Who binds them is also what
decides how long they live: the rest of the block for a flat declaration, and the `in` body for a
scoped one.

### `?` and `?.`, and why they are two operators

Both ask one question of one class - `Try(m -> a, e)`, "which of this value's two paths is it on" -
and they differ in **where the other path goes**, which is the whole of why one operator could not
have served. `resolve_try.cpp` is where both are, for the same reason `convert.cpp` holds one rule:
what they share is the selection and the branch, and what parts them is two lines.

`x?` **leaves the enclosing function.** It is the value where the carrier proceeded and a `return`
where it did not, and what it returns is that function's own result type rebuilt from the reason -
`Try.fromExit`, selected for the **signature** rather than for the operand. That asymmetry is the
entire reason the operator is compiler-known rather than a suffix somebody could declare: every other
skipping construct in the language has its destination written at the call site, and this one's is
the enclosing return edge, typed by a signature the expression cannot see.

`a?.b` **skips to the end of the chain it is written in.** `row?.name.trim()` is a `Maybe(String)`
and nothing departs, which is what every language with this spelling means by it. What the chain
comes back in is `Rewrap(m, b -> n)` - the one direction `Try` cannot state, since it reads a carrier
to what is inside it and this needs the wrapper around a *different* payload.

Almost all of the implementation is shared with the rest of the resolver, deliberately. The branch is
`armsValue`, the same construct `if` and `match` are; the calls are `applyMember`, the same one an
operator goes through; the payload is a *place* projected out of the outcome, which is what a
constructor pattern does. Four things are the file's own:

- **The shape is one `selectInstance` with holes.** `Try` is keyed on the carrier alone, so what it
  proceeds and exits with are read off whichever head matched - the dependency turning "which types
  are these" into a lookup rather than an inference problem. A carrier that is the body's *own*
  variable has no instance to read anything off, and what answers instead is the requirement the
  enclosing signature declared: `carrierShape` splits on exactly that.

  A carrier *applied to* one - `Result(ReadError, a)` in `lib/Core/Format.yana`'s `readAll` - is
  neither, and is inventory D13's open head: `Result` names its own constructor, so the instance that
  serves it is decided for every `a` a specialization binds. A requirement stating it would have to
  be written on every signature in the library that returns a result. `selectOpenInstance` is asked
  after the requirements and before the refusal, and a *bare* variable still falls through to the
  refusal, which is the line `openHeadAnswerable` draws. `Try.OpenCarrier.yana`.
- **`?.`'s join is built by hand** where `?`'s is `armsValue`. The alternatives of a chain are not
  written side by side and cannot be - one of them *is* the chain and the others branch out of the
  middle of it - so the layout is spelled out, with every rule about the merge kept: one occurrence,
  every edge supplying it, and the carried values snapshotted per arm and replayed on the edge. Each
  skip's reason is projected **in its own block** and not before the branch, because a payload read on
  the path where its constructor is not the live one is a move out of storage holding something else.
- **A chain is identified by node address** (`OptionalChain::spine`), not by a depth counter: in
  `a?.b(c?.d)` the inner `?.` is its own chain, and a counter would join it to the outer wrapper.
  Parentheses end a chain, because the spine walk deliberately does not go through `Nested` - so
  `(a?.b).c` is a field read of the carrier, and `(x?).field` is the escape hatch the `?.`-produces-a-
  wrapper diagnostic points at.
- **`rewrap` is called with its subject supplied.** `fn rewrap(->value: b) -> n` mentions `m`
  nowhere, exactly as `Try.fromExit` mentions `a` nowhere; neither is callable from source for that
  reason, and what selects them is a shape the compiler already worked out. `applyMember` takes a
  `subject` for it - the class's first argument, bound by matching the class's own parameter, so a
  contradiction with what an argument said is still the ordinary mismatch.

`lib/Core/Core.yana` declares both classes and the three instances each, verbatim from
`lib/Core`; `known.def`'s rows are `carrier` and `rewrap`, and the first is the one row here whose
spelling is not its class's name - `try` is a C++ keyword, and a spelling column would be the second
name for one thing that `intrinsic/` exists to not have.

Fixtures: `Try.Carrier.yana` and `OptChain.yana`, both compiled by **both stages** against one run
expectation, and `ownership/Reject.Carrier.yana` for the twelve refusals. `Try.yana` is the shipping
stage's own and cannot be shared: it asserts `?` inside a skipping lens written as `-> Maybe(a)`,
where this stage declares one with `| E`.

### `intrinsic/` - the compiler's roles over this IR

Everything the source language and the compiler have to agree about is declared in
[`compiler/intrinsic`](../intrinsic/README.md) - a leaf beside `YanaCore` that more than one stage
reads. What is *here* is this stage's half of it, and it holds no names:

| File | What is in it |
| --- | --- |
| `intrinsic/roles.h` | the claim table `Program` holds - a role to the declaration that filled it |
| `intrinsic/link.cpp` | the one reader of `@builtin(role)`, and every check made of a claimant |
| `intrinsic/expand.cpp` | the roster: one arm per `op.def` row |
| `intrinsic/expand_*.cpp` | the emitters, grouped by family - `expand_provided.cpp` is every member of every instance the compiler generates, `expand_atomic.cpp` and `expand_vector.cpp` the two families of milestone 8, `expand_container.cpp` the numbers a container asks about its own key type |
| `intrinsic/provide.cpp` | the instances the compiler generates from `provided.def`: their declarations, and their bodies |

**A role is not a promise that the compiler does the work.** It says the compiler is entitled to,
and `opExpands` is where this stage declines two of them. `newRun` is placed by an escape analysis
this stage has not got; the library writes what the answer is when the compiler does not choose
(`lib/Native/Run.native.yana`'s heap run), the declaration carries it beside the role -
`roleMayWriteBody` in `compiler/intrinsic/op.h`, which is `Flow`'s existing rule widened - and this
stage calls it. What is lost is a placement decision rather than an operation.

**The seven bulk operations were on that list and are not any more** (J34). Their expansion is a
*selection between two library bodies* - `lib/Core/Reduce.yana` writes each of them twice, and which
one runs is a question about a lane's stride and the target's register width rather than about
anything a constraint could carry. What kept them on the list was that this stage had no handle on
the body it would be choosing; it has one now, a `known.def` row per vector half
(`sumVectors` and its six neighbours), which is the row-per-body answer that note named as a way out.
`Resolver::selectBulkBody` in `resolve_expr.cpp` is the choice and `bulkVectorRole` in `expand.cpp`
is the pairing; the declaration's own body is still what a call reaches where the element has no
vector, so "no vector on this target" and "no compiler that picks" remain the same answer. The
element is found by asking which of the call's *bound type arguments* has a natural vector - a bulk
declaration binds a container and an element, and a container is a record - which is what lets the
rule be stated without the compiler having to name `Chunked`. `VecString` went 9.00x of the shipping
stage to 1.377x on the clock for it (`test/bench/findings.md` §81).

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

**The raw pointer is the family that is built**, and it is the one place where what the declaration
says decides more than a convention. `addressOf` is declared `fn addressOf(it: source'a) ->
%source'a`: the parameter publishes an origin and the result names it, so the argument is read as a
*loan of a place* rather than copied, and the pointer is an ordinary member of that origin set. Every
question about how long the storage lives is then the loan's, and §12.3(c)'s refusal of an
unqualified frame-rooted `%T` has a relationship to compare against - which since D15 it actually
makes, `lib/Native/Pointer.yana` having been respelled to this on 2026-09-07. Written `it: a`, which
is what it said before and what `doc/spec/native.md` never said, `readMode` answers `CopyTrivial` at
every scalar and what arrives is a value rather than a loan; `fn leaks() -> %I32` over this frame's
own storage was accepted in silence. The shipping stage reads the published signature at its plain
positions - it supplies the definition, `takeRolePositionsPlain` in `resolve/module_decl.cpp` - so
`test/resolve/Global.yana`'s answer there is what it always was.

**Which the emitter has to have an answer for, because `addressOf` is what makes storage exist.**
`expandAddressOf` asks two questions of an operand that arrived owned, in the order the shipping
stage's `materialize` asks them.

First, **the storage it was copied out of, where the copy is one this position made**: a `Read` of a
*writable* place is the case, and the address is that place's - the same `findPlace` step
`expr_construct.cpp` takes, and the reason both stages say one thing about
`waitProcess(self.id, addressOf(status), 0)` followed by a read of `status`. Under the unpublished
spelling the address of a copy would be a child's exit code written into a temporary nobody looks at. A plain
`let x` is not storage anything may write through and keeps the copy, which is the meaning
`test/resolve/Global.yana` records for the address of a value that cannot be assigned to.

Otherwise the operand really is only a value: an unnamed local of the frame, written once, and the
address is that slot's. The read of the slot is the declared position's request with `published`
forced on **and the access forced to `Unique`**, and the second half is not decoration: a shared,
call-bounded loan of a trivially copyable scalar *is* the scalar in a register
(`isReferenceValue`), so under the unpublished spelling `addressOf(9)` read the slot back as a
number and the SSA conversion reported the address of a value that is not reached through storage.
An exclusive loan of a slot this function just made is reached through it, and it is the same
statement the capability makes from the other end - the pointer handed back is a `%a` and a store
through one reaches this storage. `Builder::addAddressOf` therefore still asserts that what it is
handed is a borrow - an owned node result is nowhere, and there would be nothing for the address to
be the address *of* - rather than testing it.

`test/resolve/AddressTemporary.yana` is the fixture, in the shared corpus because both stages
materialize and both have to answer the same number; the *first* question is reachable only under
`lib`'s spelling, so `test/lib/PointerAddress.yana` is where it is stated.

Two nodes carry it. `Node::Address` reshapes a borrow the way `window` does - `Inspect`, taking no
loan of its own - and `Node::Metric` measures a type. The second is a node and not a number because
at a **type parameter** it is not one: an erased body reads its metrics out of the caller's
descriptor, so folding a literal in the conversion would be this stage answering a layout question it
cannot see. Arithmetic on an address is written as a reinterpretation, an integer displacement and a
reinterpretation back, so `add` stays homogeneous; `UnaryCategory::Raw` is what admits a pointer to a
`bitcast` and nothing else does.

`Node::Metric`'s five rows are `metric.def`, and they are `TypeTraits(a)`'s five fields in that
order - `expandTypeTraits` holds the two lists to each other by count, so a sixth field in the
library is reported rather than built out of whatever the fifth left behind. One node with a column
rather than five nodes: what they share is the shape, which is an unusual one - no operands at all,
and an answer that depends on a table the caller supplied.

**The two flags are the only rows that are not measurements**, and they are the two bits the erased
descriptor's metrics cell gives up an alignment for. An alignment is a power of two and sixty-four is
the widest any target asks for, so six bits holding `align - 1` covers every layout there is; a size
has a use for its whole range, since a framebuffer held as a fixed array is megabytes. `kAlignShift`
in `resolve_ssa/lower/erase.cpp` is where that is argued, and storing the bias rather than the
alignment makes a stride one instruction shorter than it was.

Implementing `size` is also what found the new stage laying out records **differently from the
shipping one**: `layoutRecord` rounded a record's size up to its alignment, so `data Pad {wide: I64,
narrow: I8}` was sixteen bytes here and nine there, and a `data Nest {inner: Pad, tag: I8}` put its
tag at sixteen rather than at nine. A size is not a stride - `strideOf` is the rounding, applied by
the two readers that want the other number - and nothing had noticed because the size was only ever
used to allocate and to copy, where the padding is harmless. `test/resolve/intrinsic/Traits.yana`
carries no resolver directive for that reason: both stages compile it and both are held to the same
number.

### The operations a machine has and the IR does not

`Node::Native` and `native.def` - a system call, and a block of memory moved or filled. **Not a
call**: there is no function, no signature to check arguments against and no body for a summary to
walk, so what the node holds is a column and a flat operand list, and the backend is what knows what
any of them means. The declarations that reach it are `@builtin` in `Native` and their signatures are
what checked the arguments before the node was built.

Sixteen rows and not the shipping stage's whole `NativeOp`. The host operations belong to a target
this stage does not compile for, and a row with no producer is a description checked against nothing.
The seven `syscallN` roles are **one** row, because an arity is a declaration's - a call's argument
count decides which registers the kernel reads - and one row per arity would be six rows that lower
identically.

Twelve of the sixteen are the **x86 family** - `lib/Native/Intrinsic/X86.yana`'s `crc32`,
`vzeroupper` and the seven SHA steps. Each is an instruction the lower IR already has
(`LowerInst::Crc32`, `ShaBinary`, `Sha256Rounds`, `VZeroUpper`), so the lowering arm is a mapping
with no expansion and no width to mask, and each declaration is `@platform`-gated - a build below the
level that has one never resolves the name, which is why a family this target-specific is rows here
rather than a tier of its own. Two shapes are worth stating. `sha1rnds4`'s round group is **four
rows** rather than a third operand, because it is the instruction's trailing byte and a computed one
is not an instruction: `LowerSha` already says so with four enumerators, and the resolver holds the
argument to a literal at the call, which is the only place the argument is. `sha256rnds2`'s third
operand is `xmm0` on the machine, which is a register-allocation constraint the x64 form carries and
nothing above the backend has to know, so here it is an ordinary third argument.

`verifyBody`'s `checkNative` is the rule over all of them and it checks the one thing a `Native` node
has to be checked for: the **arity**. native.def deliberately carries no arity column - a system call
has seven - so the numbers are stated once in the verifier, and a row whose count is a declaration's
answers "any". The lowering indexes its operands positionally, so a `sha256rnds2` that reached it
with two would read past the list rather than report.

`CloneThread` is the fourth, and the one whose operands are not what MIR wrote. It lowers to
`LowerIntrinsic::CloneThread`, a backend pseudo whose encoder is `clone` plus the child's entry
sequence, and the child needs both words of the entry - the code to jump to and the environment to
hand it. The declaration takes an ordinary function of the language, closure included, so what MIR
carries is the `{code, env}` value the author wrote and **the pair is split at the lowering**:
`resolve_ssa/lower/inst.cpp` already reads exactly that pair with `wordAt` at `funCodeOffset` and
`funEnvOffset` for a call through a function value, so the one place that knows a function value is
two words at two offsets stays one place. The shipping stage splits it in the resolver instead and
hands the backend six operands; both ends agree on the backend's order,
`(flags, stackTop, code, argument, threadId, env)`.

**What is *not* here is the whole-vector transfer**, and that is the decision worth recording.
`vectorAt`, `vectorPast` and `setVectorAt` are `*p` and `store(p, v)` at a vector type: the address
is the caller's element pointer named at the vector's type, which is a `bitcast` and no arithmetic
and no alignment claim, so what they expand to is the `Read` and the `Write` through an address root
that `expandPointerLoad` and `expandPointerStore` already build. A `vector.load` node beside those
would be a second spelling of one operation, and every pass that reasons about places - the loan
checker, the drop elaboration, the conversion below - would need an arm for it that said the same
thing. So `vectorAt(p, i)` reads through a pointer loan exactly as `pointerLoad` does and
`setVectorAt` writes through a `%` under `pointerStore`'s rule, because they *are* those two.

`vectorPast` is the one bit of difference and it is a bit: `NodeRead::overread`, carried unchanged
to `InstLoad::overread` and to `LowerInstLoad::isOverread`, where the three passes that must not read
"deliberately past the end" as a bug read it. `Native.vectorPast` is the only thing in the language
that sets it, so the reasoning about when the tail-read guarantee may be spent is in one place, and
no write ever carries it - a store past the end of an object is not a read of unspecified bytes but
the destruction of whatever follows. The dump prints `read.past` and `load.past`;
`test/resolve/VectorTransfer.yana` is the fixture, and its `loadx` in both stages' lower dumps is
what says the bit survived the seam.

The movemask is the fourth of that family and is a **reduction**: `ReduceOp::Bits`, added to
`op.def`'s roster where its own note used to say a backend writes the kind for itself. That was true
while nothing above one could reach `Native.bits`; the declaration is `@platform(native)` and sits
beside the pointer operations precisely because `any`, `all`, `count` and `firstSet` are the portable
readings and a movemask has no portable answer.

### The float unaries, and where their check moved

`sqrt`, `fma` and the four roundings are `op.def` roles over an **unconstrained** `a` - there is no
class in this language that says "this is a float", so `expandVecFloatUnary` holds the argument to
one by looking at the type at the call. In a generic body there is no type to look at, and refusing
there kept `lib/Math` out of this stage entirely: `modulusOf`, `logOf`, `hypotenuseOf` and the
trigonometric bodies all round or take a root of their own `Real(a, m)` parameter, twelve sites.

So `operandTypeAllowed(UnaryCategory::Float, ...)` admits a **type variable**, the way it has always
admitted a *vector* whose lane is one, and the check moves to `resolve_ssa/convert.cpp`, which is the
point the type is known: a specialization is the generic's body read at concrete types, so the
substituted occurrence is exactly what the emitter would have seen had the call been concrete, and
the node's own source is the call the author wrote, so the message lands where it did.

`Arith` and `Bits` stay strict, and the split is the point - `neg` at an open `a` is a body assuming
something about `a` that `Num` could have promised and did not.

### The pointer instances, which `provided.def` cannot describe

`Eq(%a)`, `Ord(%a)` and the reinterpretation rungs are generated by `providePointerInstances` and not
by a row of the table, for the reason that table has the shape it has: a row says which *family of
types* a class is generated over, and there is one pointer head and it is a type variable. The
shipping stage draws the same line in the same place, in `native.cpp`.

Three things had to give way for a **parametric** generated instance, and each is a place that had
assumed a ground one:

- **`headKey` files every pointer under one key**, `kPointerHead`, which is the same statement the
  `Record` arm makes about `Array(I32)` and `Array(a)`. Without it `%a` and `%I32` are two handles
  and the instance is filed where no lookup goes.
- **`expandProvidedCall` expands at the call's head and not the instance's.** They are the same list
  for every ground row; for `Bitcast(%a, %b)` the instance's list is a pair of variables, and an
  emitter reads the head for the type its node answers with - so a `bitcast` came out typed `%b`.
- **`dischargeRequirement` accepts an instance that matches with the body's variables still in it.**
  `fn null() -> %a = bitcast(0 :: Size)` needs `Bitcast(Size, %a)`, and `Bitcast(Size, %b)` holds for
  every `b`; there is nothing for a signature to require. Sound because a head is a pattern - if it
  matches with `a` free it matches at every type a specialization binds - and the selection's own
  `holdsContext` has already discharged the instance's requirements at those bindings, so an instance
  needing `Copy(a)` of this body's `a` still does not resolve here.

`Order` admits a pointer alongside `Equality`, because a region's bump pointer against its limit is
the question the allocator asks. Arithmetic does not: `p + n` scales by the pointee's stride and is
`pointerAdd`, which no `Num` instance could express.

### The vector, the mask and the atomic

Three type constructors the compiler creates rather than a library declares -
`compiler/intrinsic/constructor.def`, which is `primitive.def`'s companion for the names that take
arguments. The test for belonging there is the one `primitive.def` states: **a `data` head could not
have said it.**

- `Vec(a, n)` and `Mask(a, n)` hold `n` lanes **in a register**, which Design-Vector §2.5 makes a
  contract every calling convention is bound by, and the natural form's count is the register's
  width divided by the lane's stride - a number no declaration could write and one a target would
  contradict. One `Type::Vector` kind for both, with a flag, so a switch over the kinds gains one arm
  rather than two and the places that genuinely differ read it.
- `Atomic(a)` **inverts** its content's ownership: every type it may hold is trivially copyable, and
  a copied atomic is a second location. No fold over members produces that, which is the whole of why
  it is a kind.

Three states of a vector's count, and each is reachable from source: a number, a const variable, and
neither - the natural form deferred, which is `Vec(a)` inside a body generic in `a`. The natural form
is spent during **resolution** rather than left to the manifestation, which is sound because
resolution is already per target (`@platform` selects declarations the same way), and what it buys is
that nothing below this stage reasons about a count's absence. `makeVectorType` is the one place all
of it happens, and the printed form goes back the other way: a count equal to the target's natural
one is not printed, so a golden that mentions a vector is not a golden per feature level.

**A natural count is also a number a call can hand over**, and that is `ConstType`'s second state -
`lanesOf`, the one type-level *expression* this stage has. `let acc = zero() :: Vec(a)` handed to
`fn (n: Int) horizontalSum(v: Vec(a, n))` has to bind `n`, and there is no number until the
specialization binds `a`; so `matchType` binds `Types::naturalLanesType(a)`, which `substituteType`
turns into the number by making the vector it is the count of and reading the count back - one
definition of "the natural count of `a`" rather than two. It is an expression rather than a default
filled at the substitution because `n` is a *type argument of that call*: it travels in the argument
list, is numbered by the callee's schema and is substituted at every instantiation, and a default
would have had to be re-derived by the specializer's const slot, by the conversion's `GenValue` and
by the erased witness, each having to know that this particular number came from a vector.
`makeVectorType` collapses `Vec(T, lanes(T))` back to the natural form, so the two spellings are one
handle and a call needs no conversion. `lanes(v)` in a generic body is then a `GenValue` naming that
expression rather than a constant - it used to fold to the natural form's zero, silently, which is
what `lib/Core/Container.yana`'s chunk width was getting. `VecNatural.yana` is the fixture.

**Masks are not normalized.** `Mask(F32)` and `Mask(I32)` are two types related by `Bitcast` at one
shape. The shipping stage interned them as one and the bill was a non-injective type constructor -
substituting `a := Int` into `{Vec(a), Mask(a)}` gave `{Vec(Int), Mask(U32)}`, which then failed to
bind `a` back - in a language where every generic mechanism assumes an application binds its
arguments.

### What a vector needed of the IR, which is five nodes and a select

Most of what a vector does is what this IR already did. An add of two of them is `binary`, a lanewise
comparison is `binary` whose result type `binaryResult` answers as a mask, a lane-count-preserving
conversion is `convert` or `bitcast`, and every pass that walks one of those walks these. What was
missing is four shapes none of them have - `vec.splat`, the lane pair, `vec.shuffle` and
`vec.reduce` - and `select`, which is a node because a lanewise choice has an answer *per lane* and
so has no block structure that expresses it.

`vec.iota` is the fifth, and it is there for the reason `expandVecLanes`' `GenValue` is. At a
concrete vector `iota` is not a node at all - the emitter writes lane `i` with `i` over a zero splat
and the folder answers the whole chain - but a body **compiled once** has no lane count to make that
chain as long as, and the natural form's count is zero. What came out was a splat of zero with no
diagnostic, so `maskUpTo` answered a mask with no lane set and `lib/Core/Container.yana`'s `vectors`
dropped its tail chunk: summing `[1..11]` gave 36 rather than 66, at both optimization levels and
with nothing in the dump to point at. The node carries the question to the SSA conversion, which is
the first point the count is a number, and the chain built there is the same chain. `maskUpTo` at an
unmeasurable lane also keeps the lane's **own** type for its indices rather than the float-to-integer
mapping, because that mapping is a float's and asking it of a variable decides the domain before the
substitution that settles which one it is.

Six unary rows arrived with them and are not vector-specific: `abs`, `sqrt` and the four roundings
are one instruction on every target and were the thing a scalar wanted all along. `op.def`'s new
`UnaryCategory::Float` is what admits a float and refuses an integer, once, rather than at each
emitter.

**The lane index and the shuffle pattern are fields rather than operands**, which is the boundary
Design-Vector §3.3 draws: each is part of the instruction on every target that has one, so a computed
one is not an instruction at all. `expand_vector.cpp` is where a call is held to that, at the call,
because that is the only place the argument is; nothing below has a dynamic case to handle.

### The vector instances, which `provided.def` cannot describe either

`providePointerInstances`' argument, one constructor over: a row of that table says which *family of
types* a class is generated over, and "every vector" is not an enumerable set - a lane is any machine
primitive and a count is the target's. So `provideVectorInstances` generates one **parametric** head
per class and the head's own context is what makes it correct: `Num(Vec(a, n))` requires `Num(a)`,
`Integral(Vec(a, n))` requires `Integral(a)`, and `holdsContext` discharges it at whatever the head
bound. A vector of a lane with no arithmetic therefore has none, and there is no such vector anyway.

Four heads are not that shape and each says why in `provide.cpp`: `Bitwise(Mask(a, n))` has **no**
context, since a mask's four operations are defined on its own two patterns rather than on the lane
it was made from; `Lanewise(Vec(a, n), Mask(a, n))` has none for the same reason; and the two
`Bitcast` heads are open in all four positions because what relates them is an arithmetic relation
between two lane widths and two counts, which no requirement can state - `reinterpretableShapes` is
where a pair is held to it, at the call, where the two shapes are concrete and can be named.

`Eq` and `Ord` are deliberately absent and must stay absent: a comparison of two vectors answers a
mask, so neither class describes what comparing them is, and generating one would typecheck and emit
a comparison the lower IR's validator refuses. `Lanewise` is what describes it, and it is the first
`known.def` class role this stage binds that the shipping stage still finds by name.

Two things `lib/Core/Vector.yana` declares that the stand-in library could not: the free `min` and `max` over
vectors. Design.md's R1 admits one plain function per `(name, arity)` *beside* any number of class
functions, and this stage files both in one table - so they collide with `Ord`'s members. The
emitters are written and the roles stay unclaimed until the overload rule is, which is what
`@builtin` being a claim rather than a lookup makes safe.

### The atomics, and the form that made the branch the resolver's

`lib/Atomic/Atomic.native.yana` is the whole written surface and `expand_atomic.cpp` is what stands
behind it. Two of `op.def`'s rows are not atomic instructions and are handled as what they are:
**constructing** one is frame storage and an ordinary store, because nothing can be sharing a
location still being built, and **consuming** one is an ordinary load, because owning it exclusively
proves nobody is. Everything between them is an ordering edge, and the order is a *field* of the
node for the reason the lower IR gives - what a pass has to be able to ask is which edge this
establishes, and an opaque operation that reads, writes and orders is the answer that stops every
question.

An **ordering argument must be a constant**, which is §3.3's rule and is enforced at the call: an
order changes which instructions are generated and which motions the optimizer may perform, so it is
not useful run-time state. The four library ordering *types* exist so that `store(x, LoadAcquire)`
does not typecheck; they collapse to one `AtomicOrder` on the way in, and `expand_atomic.cpp`'s four
tables are the constructor lists read by position.

`compareExchange` is the one thing here that is not an emitter, and `intrinsic::OpForm::Branch` is
the row that says so. The instruction answers two things - what was there, and whether the store
happened - and the library answers one, so the expansion is the branch that joins them into
`Exchanged` or `Unchanged(previous)`. A branch is blocks, and **a value the surrounding walk is
holding has to travel their edges**: `Held`/`Carry` is the walk's own state and nothing a `Builder`
can see, so the layout belongs to the resolver. That is the same division the `Flow` roles are under
and `resolve_intrinsic.cpp` is where it lands; the third form was needed because `Flow` also means
"the declaration carries a body" and this one has none.

`AtomicValue(a)` and `AtomicInteger(a)` are the first **structural** classes here - answered from the
shape of the type rather than from a declaration, which `selectInstance` asks before the table and
`isStructuralClass` is the one statement of which classes those are. A structural class declares no
members (`checkRoles` holds a claimant to it), which is what licenses an answer with no instance
behind it, and no program may write an instance of one: the family is what a target performs
indivisibly, and a program saying otherwise about its own type would be claiming an instruction that
does not exist.

`Native.atomicAt` is **not** implemented, and it is the library's spelling rather than the stage that
blocks it: the declaration is the one signature in `lib/` still written with the `return` marker, and
the note above it records that the `'` spelling does not express what it needs. `test/resolve/Atomic.yana`
stays the shipping stage's alone for that reason alone now - the dot form it reaches the operations
with arrived 2026-09-06 and `AtomicOwnership.yana` runs here on it.

### `Bool` is an ordinary enum, and so is what a bare literal settles to

Three things the compiler used to state itself are the library's now, and two of them had already
started to disagree with it.

**`Bool` is `data Bool = False | True`,** claiming `@builtin(boolean)`. There is no `Type::Bool` kind
and no `BoolType`: a comparison answers whatever record claimed the role, `if` reads one, and
`manifest.cpp` gives a two-constructor enum one byte holding a tag - which is the answer the builtin
kind gave, reached by the row that was already there for every other enum. The argument for the
builtin was that a comparison has to answer *something* and this stage might have no declarations to
answer with; giving this stage a library is what removed it.

The circularity that looks like it should block this does not exist. Matching a constructor emits a
comparison, and a comparison answers a `Bool` - but nothing then compares *that*, because `branch`
consumes it directly: `Bool`'s tag is its whole value, so the regress stops at the first step.

What the role requires is two constructors numbered from zero, and `checkRoles` enforces it - the
comparisons this compiler emits answer 0 and 1, so which constructor is false is not something the
library may move with `@value(n)`. Which *words* they are is entirely the library's; nothing in
`compiler/intrinsic` spells `True`.

**A nullary constructor of a concrete enum is a constant,** `Constant::Enum`, holding the index into
`RecordType::cons`. That is what `True` in a field default needs, and it is general rather than
`Bool`'s special case: `data Holder {f: Flag = Off}` works for the same reason, and `Off` in a value
position is `const Off` rather than an `aggregate` instruction. The index and not the stored number,
so that a dump prints the constructor's name and nothing above the manifestation layer learns that
`True` is a one - `constantBits` in `resolve_ssa/lower/internal.h` is the single place that
conversion happens, and it is one function because two callers used to do it and one of them read
`Constant::bits` directly.

**A record built out of constants is a constant too** - `Constant::Aggregate`, whose payload is an
`AggregateConstant` in the declaration region: which constructor, and one `Constant` per field of its
range. **A tree of values and never a block of bytes**, which is the whole of the layering: where a
field sits, how wide an address is and which end of it comes first are the target's answers, and
`resolve_ssa`'s `materialize` lays one out with the same `Repr` - field offsets, discriminant and
all - that every other value of that type is laid out with. An evaluator that
produced bytes would be a second record layout, and the way that fails is silent.

There is deliberately **no global-versus-local split in what a constant is**. `let &shape = Circle(7)`
at module level and a field default of the same shape reduce to one value, and where that value is
*put* is a placement question the first tier that knows the target answers: `lowerGlobal` writes it
into the global's own storage, and `placeConstant` puts one reached inside a body in read-only data
and hands back its address. A constant that would be cheaper as a pair of immediates is a refinement
of that one function rather than a different representation.

**An array literal is a constant too**, and it is the one that made the difference to the library:
`lib/Core/Float/Ryu.native.yana` writes two 684-entry `[I64 *n]` tables at module level. A fixed
array's elements are the value's own storage, so there is no run to allocate and nothing to point
at - which is exactly why a *growable* `[T]` has no constant form, and why the recognition below
turns on the position's hint being a fixed array. `arrayLiteralShape` states the literal's rules -
the count matches or is one, a fill has to be duplicable - and both readings of `[1, 2, 3]` ask it,
because a rule written twice is a rule the value path and the constant path can come to disagree
about.

**A literal at a `@bits(n)` refinement is a constant**, which is `canonicalInt`'s rule and not an
exception: `0 :: HeapFlag` dispatches as the `U32` it refines, so the instance is the compiler's own
and the value is that fold put into the refinement's normal form. Asking the family about the
refinement itself answered "no such instance", which is true and is not the question - what it
produced was a global with a startup initializer calling something nobody declared.

The **recognition** is where the split still is, and it is a gap rather than a design.
`resolveAggregateConstant` is asked where a constant is what the position is for - a global's
initializer, a field default, a parameter default - and a value position asks only for the enum form.
Folding a body's `Point {x: 1, y: 2}` costs two things this stage has not got: the walk is
*speculative* in a value position, so an inner report leaks - the widening a field's value is
entitled to happens in `coerce` and not in the constant evaluator, so `Point {x: 1 :: I8}` would be
refused rather than widened - and the operand nodes the construction has already appended would be
left dead, which is a `Const` nothing names in a stage that runs no pass to remove one. Both are the
same missing piece from two sides: the two positions have to reduce by one rule, and that rule has to
run where the value is made.

### Arithmetic is a constant, and what decides is which instance serves the operator

`4 * 1024 * 1024` folds in a global's initializer, a parameter or field default, and a fixed array's
length. What makes it foldable is **not** that the operands are numbers: an operator is a call, so
`resolveInfixConstant` decides the same thing a call decides - which declaration the name reaches,
and whether the instance serving it *at these operands* is one `provided.def` generated. A program's
own `instance Num(Money)` is a body, and a declaration pass has no body to run; the compiler's
`Num(I32)` is one node, and `fold.h`'s `providedOperation` is the table that says which. That table
is `expandProvided`'s too, so a member that builds an `Add` in a body cannot fold as something else
here.

The arithmetic itself is `lower/fold_arith.h`, which is now shared by three folders rather than two -
this tier, ownership SSA and the lower IR. What is *not* shared is which types may be folded at all,
and that is `fold.h`'s one rule: a **target-answered width folds only where the answer cannot depend
on the target**. `Size` is 32 bits on one machine and 64 on another, so `1 shl 40 :: Size` is two
numbers and neither is this stage's to pick; `wordBits - 1` is one number on every machine, because
its operands and its result fit the width the language guarantees. `IntType::minBits` is that width
and `holdsAtWidth` is the test. A `@bits(n)` refinement is refused outright, which is
`doc/spec/repr.md`'s rule rather than a limit here - a load widens and a store narrows, so the
addition is the `U32` one and folding at four bits would be the one answer the machine does not give.

Two things travel with it. A **global whose initializer is a constant** is readable in a constant
position, which stage A calls "purely syntactic, no analysis" - and both adjectives in "an immutable
global declared earlier" are load-bearing: a `let &` is storage a startup statement may have assigned
to since, and a later `let` has no constant yet because initializers resolve in source order. And a
**supplied global** holds the target's answer from the moment its own declaration settles rather than
after every initializer has run - `bindSuppliedGlobal`, moved out of `checkRoles` for exactly that -
so `let pageMask = pageBytes - 1` is 4095 and not -1.

**Field defaults moved with it.** They used to be drained beside the record fields, two passes before
any class or instance existed, so a field default could not fold anything at all while a parameter
default - resolved inside `declare` - could. They are drained after `declare` now and before
`checkTextDefaults`, which is what a string default of a field is queued for. What a field default
still cannot read is a module-level `let`: a global's initializer may omit a defaulted field and a
field default may name a global, so one of the two has to be second, and the constants are.

What is **not** foldable is a `TypeTraits` field - `sizeOf`, `alignOf`, the stride. That is the
MIR/manifestation split rather than a gap: `metric.def` says none of its rows folds here, because a
body compiled once for every `a` has no layout to fold against and this directory may not ask what a
value looks like in a register. A constant `sizeOf(T)` would need the layout to be answerable above
`resolve_ssa`, which is an Analysis-IR decision and not a table entry.

**And a read of an immutable global is that constant, wherever it stands** - `readPlace`'s first
arm, which is stage A said about a read rather than about a constant position. Nothing may assign to
such a global, so its value is forever what `resolveGlobals` recorded and loading the bytes would
load something that never changes. What makes it worth doing here rather than leaving to a later fold
is that it is the *only* thing that makes the supplied constants usable:
`lib/Native/Memory.native.yana` writes `let width = vectorBytes` and gates a `Vec(U8, 32)` load on
`width > 32`, which the x64 backend refuses outright below v3 - behind a load the comparison is
opaque, the arm survives and the build fails. Folded, the arm is gone before any pass has run, which
is what that file's own comment already claimed.

**A scalar value only.** An `Aggregate` or a `Text` global has a *placement* - one run of bytes the
program refers to - and a read of it is a read of that storage rather than a description of what is
in it; `resolve/NicheOwned.yana` answers a different number when the byte image behind a string
literal is handed to two readers as two constants. What the supplied globals are is numbers, and a
number has no placement.

**And the same sentence one scope down, about a `let`** (`Local::initial`, J21). Folding the global
alone only moved the constant into a frame slot the comparison then loaded it back out of: this stage
gives every local a slot at the MIR-to-SSA conversion and `opt`'s promotion is what removes it, so at
`-no-ir-opt` `width > 32` was still opaque and the arm still survived - the x64 backend refused
nineteen fixtures of the corpus on it the day `lib/` became the library. So a `let` that is **not
writable** and whose initializer resolved to a scalar constant records it, and `readPlace` answers a
read of that local with the constant. Nothing may assign to such a binding, which is the whole of
what makes it sound; the storage still exists and is still written, so `addressOf(x)` and a borrow of
it name a place as they always did. **Not at a published position** - `'T` and `&T` ask for a loan of
*this* storage and a constant has none to lend - which is the one way a local differs from a global
here.

**A constructor's head comes from the position** (2026-09-07). A generic `data Opt(a) = None |
Some(a)` has no type for `None` until something binds `a`, and what would bind it from the *inside* is
the inference a construction runs over its field values - a `TypeArguments` walk a constant position
has no builder to make one with. What binds it from the outside is written down wherever a constant is
what a position is for: `state: State(Int) = On` and `limit: Maybe(Int) = Just(4) :: Maybe(Int)` are
the two shapes, and `Types::instantiate` has already substituted that head's fields, so
`constantHeadFor` hands the instantiation over and the field walk reads them without substituting
anything itself. A position that says nothing, or one that names another declaration, is still "not
this form".

**A literal at a vector is the lane repeated** - `let &sink = 0 :: Vec(I32, 4)` is an aggregate
constant of `lanes` values, which is the same answer `FromInt`'s vector instance computes in a value
position said as a value rather than as a node. So the bytes a global holds before anything runs are
the lane's bytes repeated, and the two paths agree by construction. A vector aggregate is written by
the same three lines a fixed array's is - one type laid end to end at its own stride, with nothing
over it - which is why `materialize` has one arm for the pair.

### A string is a constant, and the library is what says what one is

`Constant::Text` says what a string literal *is* - the interned text - and leaves how it is built to
the library: `stringLiteral` takes the address of bytes the compiler places and a count, and
describes a run that owns nothing. In a value position `textValue` calls it, and **the count is built
at the width the role's own signature declares** - the stand-in library wrote `length: Size` and `lib/` writes
`length: Int`, and the declaration is the only thing that says which. Building it at the target word
instead was the single largest diagnostic in the real library (31 in the Core closure alone:
"an argument must be I32, and this is Size" at every literal). In a **global's
initializer** there is no call to make: the value is bytes written before anything runs, and a
library module has no program point at which a declaration's own code would run at all. So
`lib/Core/Show.yana`'s digit table used to be a diagnostic.

The answer is to **run the library's function**, which is `evaluate.cpp`: a body of one block ending
in a `return`, over constants, with reads of places, conversions, aggregates and nested calls. The
MIR tier rather than the written form, because name resolution, overload selection, substitution and
`expandIntrinsic` have already happened - `truncate(length) :: Count` is a `convert` by then - so
there is nothing here to re-decide. What it will not run is refused whole rather than interpreted
along one path; the moment a branch is followed this is a machine with a program counter, and what
it would need next is loop bounds and a termination argument.

The alternative was to spell the fields of `String` here, which is what `resolve/const.cpp` does in
the shipping stage and is why that file has a "this target's string layout is not the one this
compiler writes" diagnostic to report. Running the library's own function is the same answer with no
second description of a type the library owns.

Two things make it fit. **`Constant::Address`** is the address of a global, which is the one leaf
whose value is unknown until the image is placed - `materialize` writes zero and records a
`LowerDataRelocation`, and `lowerGlobal` is split into a declare pass and a fill pass so that an
initializer can name a global declared after it (the bytes of a literal are minted while the
initializer that points at them is being resolved, so they always are). And **the evaluation is
deferred**: `PendingTextGlobal` is drained after every body, because that is the first point at
which the library's builder has one. The global is a constant either way, which is the part the
passes in between rely on - its type is settled where the initializer is read, it is not `dynamic`,
and nothing puts it in the entry sequence.

What is deferred is the **value** and not the text, because a literal may be a *leaf* of it:
`let names = ["a", "b", "c"] :: [String *3]` and a fixed array of records with a string field are the
same three byte globals and the same three relocations that three separate `let`s would be. A
deferral that tested the root alone left `Constant::Text` inside the aggregate for the byte writer,
which has no bytes for one and asserted; `holdsText` is the test and `textsResolved` is the walk that
rebuilds the value with each leaf built. `test/resolve/TextAggregate.yana` is both shapes.

**What an unadorned `1` or `1.5` is comes from `FromInt` and `FromDecimal`'s head defaults**, and
what a literal at a type *is* is that class's instance - see "Operators are calls" above.
`class FromDecimal(a = Float)` is the language stating that a bare decimal literal is an `F32`; the
compiler wrote `floatType(64)` instead and **the two disagreed**, so `let x = 1.5` was an `F64` here
and an `F32` under the stage that reads the real library. It is the fixity divergence again, one tier
down, and found the same way - by asking what in this stage is a spelling of something the library
already says.

**A field default is resolved in a pass of its own** because of that: `= True` names a constructor,
and during `declareRecordFields` the constructor table is half built, with which half depending on
the order the walk reaches the modules. `Resolver::PendingDefault` records the position - module,
file, record, field, expression - and `resolveFieldDefaults` drains it once every record in the
program has its constructors. The hole it closes was there before and unreachable: nothing a default
could name was a declaration until one of them was.

### A format is one allocation and a run of appends

`"a{x}b"` is the same arrangement one type over: `resolveFormat` is one function, it produces
ordinary calls, and the compiler reads no field of a `String`. Implementation-Storage.md part 8's
three steps are the whole of it - the literal segments are known now so their total is a constant,
each hole contributes `formatBound(showBound(v))`, and `String.ofCapacity(sum)` is followed by the
literals and the holes appended in order. The document's three *strategies* are what the existing
passes then make of that one shape: a format all of whose bounds are constants folds to a buffer of a
known size, one whose bounds are values allocates that many units, and one with an unbounded hole is
seeded with the bounds that are known and grows through the library's own append. Nothing here tells
them apart, which is the argument the document makes.

Five library declarations and five `known.def` rows: `Show` with `show` at member zero and
`showBound` at member one, and `formatBound`, `String.ofCapacity` and `String.push` beside it. All
five are `Presence::Optional`, and that is the decision the rows carry - a library without a
formatter is a smaller library and not a broken one, so a library without one gets a `"{x}"`
written against it is told at the literal which role is missing. `String.reserve` is deliberately not
a row: the shipping stage reads it for the `to ++= "a{x}b"` rewrite, which is an optimization of a
*written* operator rather than part of what a format is, so this stage builds the temporary and
appends it.

**Left to right, each hole exactly once.** A hole is resolved into a place before anything is
measured, because it is read *twice* - once for its bound and once for its text - and resolving it
twice would run its expression twice while one node used twice would hand one owner to two positions.
`evaluatedPlace` is that function and is the same one a `match` pivot goes through. The hole is
appended before the text of its own chunk, which is the parser's record rather than the written
order: a chunk holds a hole's expression and the text *following* it, so appending the text first
renders `"n={7}!"` as `n=!7` - a wrong string of the right length, which is why
`ownership/Format.Roles.yana` reads the units back.

### A growable array literal is two library calls and a run of stores

`[1, 2, 3]` builds a `[T *3]` at a window position and an **`Array(T)`** everywhere else, and
`doc/spec/expressions.md` is explicit that which one is "decided by the expected type and by nothing
else". The expected *type* of a window and of an owner is the same type, so the thing that actually
decides is `Request::form` - the `AccessForm` of the contract the position was read from, carried
down beside the access it asks for. A literal with no position at all is the growable one, because
that is what the spec says an unascribed `[2, 3, 5, 7]` is, and because a fixed array's count is part
of a type a literal standing on its own has not got.

`::` takes the form **off** the request on the way down. An ascription names a type and a type is not
a window, so `total([1, 2, 3] :: [Int])` builds the array the ascription names and the call adapts it
the way it adapts any owned container reaching a window position - where passing the form down would
make `::` build something that is not the type it wrote.

**The compiler builds no container.** Two library bodies do, and `known.def` names them:
`arrayLiteral(count)` answers an array of `count` live slots, and `elementBase(&self: '[a])` says
where those slots start. What is left here is the part a literal actually is - `count` values, each
written into slot `i` in the order they were written down - and none of it reads a field of `Array`
or of its view. That is the difference from the shipping stage, where the run, the capacity and the
placement tag are spelled out at every literal and `buildRunInto` has to be kept in step with three
library records by hand.

The elements are resolved **before** the array is made, which is the map literal's ordering and is
here for both of its reasons: the written expressions run left to right exactly once before anything
of the container's own runs, and the first of them says what the array holds when the position did
not. The writes go into `Place::Index` over a `Place::Address` root at `Destination::Init` - fresh
storage, so an `Assign` would drop a value that was never there - and `Builder::addWrite` admits a
raw-address root for exactly this, on the terms `analyze_path` already answers for one: storage
reached through a pointer is never this frame's to release, so no move path is opened and no drop is
placed.

Two roles rather than one, and they are two because the pair cannot be a function: something
answering both an owner and a pointer into it would be publishing a loan of a value it is handing
over in the same breath, and the language has no way to say that. Both are `Presence::Optional` - a
library that declares neither is one whose arrays do not grow, which is a smaller library rather
than a broken one.

**`elementBase` takes a unique window**, and that is not a detail: `%source'a` says which storage it
points into and nothing about what may be done there, so what decides whether a store through it is
legal is the loan the window was read under. A shared one is what every element accessor takes; the
literal fills through the other, and `lib/Core/Array.yana` declares both over one field read.

### A map literal is one library call and a run of inserts

`["a": 1, "b": 2]` and `[:]`, on the array literal's terms one type over - `resolveMapLiteral`, and
`known.def`'s `map`, `mapLiteral` and `indexInsert` rows. Not built in place the way an array literal
is, and the difference is what makes the function short: an array's elements *are* its
representation, so the literal writes the run itself; a map's are an insertion order, a probe and an
index, none of which the compiler knows or should. So the literal is `newMap(n)` - the one
construction site where the entry count is a compile-time constant, which §4.5 measures at 1.8-2.2x
over a small map's life - and one `IndexInsert.insertAt` per pair, left to right.

`insertAt` and not `insert`: the second hands back the value it replaced, so a literal built out of
it would drop a `Maybe(v)` at every entry. Two equal keys are therefore the assignment's rule applied
`n` times and not a rule of its own - the later entry wins.

**`m[k] = v` reaches the same member**, which is what `IndexInsert` exists for: `getMut` has to answer
a borrow and an absent key has none, so an assignment written through it traps on exactly the key the
author is inserting. `resolveIndexInsert` prefers it wherever the container *has* an instance and
falls through to `getMut` where it does not, so `Array`'s assignment is unchanged. `[K: V]` is the
same declaration written as a type.

`test/lib/MapLiteral.yana` is the fixture and runs under both stages; it was written there because
`test/resolve` compiled this stage against a library with no `Map` in it, and it stays there because
that is where a fixture about the real containers belongs. `test/resolve/MapLiteral.yana` beside it
is the shipping stage's.

### A determined position is decided before the argument offers an answer

`xs[i]` is `Index.get(self: c, index: k)`, and `class Index(c -> k, v)` promises that one container
has one key type. So `k` is not something the index decides: what the container bound one position
ago is the answer, and an index written as an `Int` is a value to be **converted** to it.

`prepareArgument` therefore asks `TypeArguments::fillFromRequirements` the moment the position it is
about to coerce into is still open, *before* binding the argument's own type into it. Binding first
did what binding always does - saw `k` open, offered `I32` - and the requirement recorded was
`Index(Array(I32), I32, I32)`, an instance nobody wrote, reported as missing where a widening was all
that was wanted.

**The written index is resolved at that key type too**, and not at the word width -
`resolveSubscriptTarget` asks `selectInstance` for the container's `Index` head with `k` and `v` left
open and reads `k` off the head that matched. A fixed array has no instance and keys at `Size`, where
the hint is what makes `xs[n]` at an `Int` compile: every `I32` is a `Size` on every target. Anything
else is asked, because the widening only runs one way - `m[900]` on a `Map(Int, Int)` built the
literal at `Size` and then had to narrow it into the `k` the instance declares, which is the
conversion the language refuses outright, and which was sixteen of the library suite's diagnostics.

Only the requirement fixpoint runs there. The declared defaults do not: a default is what a position
means when *nothing* says, and the result hint has not been read at that point - so
`fillDetermined` stays the pair and `fillFromRequirements` is the half that may run early.

The same rule reaches a **field** requirement, and that changed a diagnostic:
`fn (a.mark: b) twiceDecided(x: a, other: b)` given a `Marked` and an `I64` now reports the ordinary
argument mismatch against the `I64` the author wrote, where it used to report that `Marked`'s field
was the wrong type - a complaint about the subject, for a mistake in the operand beside it.

**And the result position is the other half of it: a dependency beats the target.** `class Enum(a ->
w)` promises that one enum has one width, so in `fn seekBase(w: Whence) -> I64 = valueOf(w)` the
`I64` is not what `w` *is* - it is what the caller wants the answer converted to. `finishCall` binds
the hint through `TypeArguments::bindResult`, which records *which* positions it filled;
`fillDetermined` then reopens the ones a requirement determines, runs the fixpoint, and puts the
hint's answer back only where nothing answered. Binding it and leaving it wrote `I64` into the
position the dependency answers, and the lookup asked for `Enum(Whence, I64)` - a missing instance
reported for what the author wrote as an ordinary widening. A position an **argument** filled is
never reopened: that one is the type of a value that already exists rather than a wish about this
call. `EnumValue.yana` is the fixture, and the shipping stage states the same rule in `solve.cpp`.

### An argument that takes its type from the position is resolved last

`resolveArguments` walks the written arguments in three turns, and the order is **how much the
argument itself knows**. A value with a type of its own goes first and decides what it decides. An
untyped number goes second: it has a *default*, so it can still decide a position nothing else does,
but it must not be the thing that decides one somebody else knows - `select(steep, 5.2e-1,
zeroLike(v))` at `Lanewise(v -> m)`'s `select(mask: m, ifTrue: v, ifFalse: v)` bound `v` to the
literal's own width and then reported the third argument against a position it had the answer to. A
**lambda** goes third, because it knows nothing at all: `apply((v) -> v + 1, 3)` at
`fn (a, b) apply(f: (a) -> b, x: a)` takes `v`'s type from the position, and the position is made of
the callee's variables until `x` has bound them.

Only where the position is still **undecided** (`TypeArguments::undecided`), so an argument at a
concrete parameter - and at a variable an earlier argument already bound - is resolved where it was
written: there is nothing for the turn to buy, and a dump should show the code the author wrote. The
turn is decided on the first pass and remembered, never asked again, because by the later turns the
arguments it was held back for are exactly the ones that have changed the answer.

The requirement fixpoint runs once **before the lambda turn**, which is the same question
`prepareArgument` asks one position at a time: `sortBy(xs, (l: Int, r: Int) -> l < r)` at
`fn (Writable(c, a)) sortBy(&xs: c, before: (a, a) -> Bool)` has `a` answered by the dependency from
`c` and by no argument of the call, so without it the lambda's position is still `(a, a) -> Bool`
when its turn comes. The declared defaults are deliberately not run with it, for the reason above.

Evaluation order is unaffected, which is what makes the reorder legal: what is held back is a
constant or a lambda, neither of which has anything to observe about when it was built - the same
reason `fillDefaultArguments` runs after every written argument rather than interleaved with them.
The overloaded path has made the literal half of this split since J8 (`undecidedLiteral`, where a
candidate's viability at a literal operand is read off its *other* parameters); this is that rule at
a call with one callee, which is where the whole of `lib/Math`'s kernels are.

### Two arguments at one variable meet at their `Widen` upper bound

`doc/spec/overloading.md` already says it - "within a single call, the positions bound to the same
class variable are unified to their unique `Widen` upper bound where one exists" - and until J23 this
stage applied it to the arms of an `if` and a `match` and not to a call's own type arguments. The
first argument bound the variable and every later one had to convert *into* it, so
`checkAtMost(shown(x), bound(x))` at `(Size, Int)` and `valueOf(e) == want` at `(I32, I64)` were
narrowings and were refused; six sites in `lib/`'s own suite carried an ascription for it.

`TypeArguments::bind` performs the meet through `commonWiden`, which is the same function the arms
use, so there is one rule. Three things bound it:

- **Only a walk that can fix up afterwards asks for it** - `TypeArguments::unifies`, set by the
  written call, the overloaded call and the operator chain, and by nothing else. The meet moves a
  binding an earlier argument was already coerced against, so `widenPreparedArguments` brings those
  positions up once the walk is over: integers only, and only where the two differ, since every other
  way a value can differ from its position was settled by `prepareArgument`. A `TypeArguments` filled
  from values the compiler itself built - `callFunction`, `applyMember`, a construction - has no such
  pass, and a binding that moved there would leave them at the width they were built at.
- **Only a position an argument decided** - `fromArgument`. A functional dependency or a field
  requirement filled through `fillFromRequirements` is a *promise*, and an argument is in no position
  to widen it: `class Enum(a -> w)` says what width an enum's value has, and `fn (a.mark: b) f(x: a,
  other: b)` says `b` is the field's.
- **Never from the result.** `bindResult` runs with the meet off, which is the line the shipping
  stage draws too: a result that would need converting has not decided the type arguments by itself.

### `@noinline`, and the one direction a declaration can insist on

`@inline` is a **hint** - `Function::inlineHint` since 2026-09-07 (J30), where this stage used to
read the attribute and drop it. The old reasoning was that what an optimizer does with a hint is its
own business, which is an argument about the *number* and not about the flag: the number is stated
once, in `resolve_ssa/opt/inline.cpp`'s `Budget`, and what this stage owes it is that the author
wrote the attribute at all. `@inline(always)` is a promise the *resolver* keeps by grafting the body
at the call, and `@noinline` is the promise in the other direction - the one of the three that can
never be refused, because declining to expand a body is always possible.
`readInlineAttribute` reads all three in one walk, since the contradiction between them is only
visible to something that has read both; a declaration saying two opposite things has stated no
intent, so neither answer is taken and the pair is reported. A hint written beside `@noinline` is
the same contradiction one step weaker and is answered rather than reported - the directive is what
the compiler can always carry out, so the hint under it would be a term weighed and thrown away.

`resolve_ssa/opt/inline.cpp` is the one reader, and it asks before the numbers rather than after:
this is a directive and not a weight. Nineteen fixtures in the corpus depend on the boundary
surviving - a measurement that would otherwise fold into its caller, `ArrayFill`'s opaque index, the
inliner's own subject - and every one of them was quietly being inlined here.

### `@convention` and `@host` - the two attributes that were parsed and thrown away

Both are read since 2026-09-07 (J26), and neither is a representation this stage implements.

`@convention(clobber)` is the second **backend** fact a resolver carries, beside `@x86_legacy_sse`
and for its reason: what a program may name is a semantic promise about a call boundary, or one of
the two foreign ABIs, and a stage that dropped it emitted a body entered under the wrong convention
with nothing saying so. `readConventionAttribute` is the reader - a word for an argument, so
`readAttributes` cannot be it - `Function::convention` is a `Maybe` so that "wrote nothing" and
"wrote the default" stay different statements, and `lower/gen.cpp` carries it to
`LowerFunction::callType` beside `legacyVectors`. The name table is `lower/convention.h`'s, shared
with the lower IR's parser and printer, and `conventionWritableInSource` is what refuses `simple`,
`complex` and `system`. The messages are the shipping stage's word for word. One difference from it
is deliberate: it reads the attribute on a plain `fn` only, and this stage reads it on every
signature, because `readInlineAttribute` already reads the encoding marker off every signature and
one walk for a signature's backend facts is worth more than a placement rule stated twice.

`@host` on a field is the opposite kind of fact - a **JS-visible source fact with no native
semantics**. `RecordField::host` records it and nothing here reads it: a native record with one lays
the field out exactly as it would without it, and what elides the slot is the JS pipeline, later.
What this stage owes is the refusals, and they are the shipping stage's: the attribute follows the
one field that stores the value and may not be that field, a record with one has exactly one stored
field, the field has to be named (the name *is* the property), and `@bits`/`@box` cannot combine with
it - an elided field has no word to narrow and no storage to point at. `checkHostFields` is per
**constructor** rather than per record, because two arms of a sum are two shapes over one storage.

`test/resolve/ownership/Reject.Convention.yana` is both attributes' refusals through this stage.
A fixture of its own rather than a directive on `Reject.VectorReset.yana` and `Reject.Host.yana`,
because a rejection fixture names *one* stage and those two are the shipping stage's.

### What containers still cannot do, measured

Four defects were found by pointing the corpus at the container path once the literal existed. None
is about the literal. Three are fixed - see the section below, J15's load, and J18's owner-to-view
derivation - and the last is recorded rather than worked around.

A fifth and a sixth arrived with `lib/` and are **library** statements rather than compiler ones, and
both are J15's `Array(a)` recursion one type over. `Contiguous([a *n], a).elements` wrote
`unclampedSlice(self, 0, n)`, whose argument is a *window* - so the only thing that turns a fixed
array into one is that member, and it called itself on every path. And `Chunked([a], a).chunks` wrote
`yield elements(self)` where `self` **is** the chunk: a class member's subject for a container is the
container's window, so reaching for `elements` there asks a window to become one and the callee then
read the owner's field offsets out of a view. `lib/Core/Container.yana` writes both out now - the
native fixed-array pair as the view record over `bitcast(addressOf(self))`, which is what
`lib/Core/Array.yana` writes, and `chunks` as `yield self`. Both stages got shorter
IR out of it.


  - ~~**A shared `String` element of a container is the owner's address.**~~ Fixed 2026-09-07 (J18).
    The derivation is emitted at the one place the substitution is known - the MIR-to-SSA conversion
    of a specialized body - and `resolve/SinkCallResult.yana` is adopted. See
    "An owner reaching a view position under substitution" in `../resolve_ssa/README.md`.
  - ~~**A class member reached through a constraint miscompiles a published borrow of a scalar.**~~
    Fixed 2026-09-07 (J15). `fn (Index(c, k, v), Num(v)) at2(xs: c, a: k) -> v = xs[a] + xs[a]`
    answered `2 * (8 + 4 * a)` because a published shared loan of a trivially copyable scalar
    manifests as an *address* and the arithmetic read it as the number. The load that was missing is
    `valueUseOf` in `resolve_ssa/convert.cpp` - see "A loan of a scalar in a value position is a
    load" below. `resolve/Bounds.yana` is adopted.
  - **`lib` declares two plain `capacity`s at arity one** - `Run(a)`'s and `Array(a)`'s -
    and `doc/spec/overloading.md` R1 lets an overload set hold one. This stage reports the
    ambiguity, so `lib/Core/Array.yana` deliberately declares neither and reads the field.
    `resolve/TypedArray.yana` waits on the library sorting the name out.
  - ~~**A `[T *n]` element is reached with no bound.**~~ Fixed 2026-09-07 (J20), and it was the last
    subscript in the language that was not checked. Every other container is subscripted through an
    `Index` body and the test is written there beside the length it reads; a fixed array has no such
    body, because its element is a projection of the container's own place and there is nothing for a
    call to hand back. `checkFixedBound` in `resolve_expr.cpp` emits it at the one place a `[T *n]`
    element place is projected from a written index, unsigned against the length so that a negative
    index is the same comparison, and skipped for a constant index that is in range.

    What it emits is a **call** to `known.def`'s new `checkCondition` role rather than the branch,
    and that is not a preference: a subscript is resolved inside whatever expression contains it, and
    this IR hands every value that crosses a block boundary over as a block argument, so a branch
    here would have to thread the enclosing expression through two new blocks under a construct that
    has already decided which blocks it owns. The shipping stage's indirection has exactly that
    reason, and `known.def`'s note used to say a condition function was a declaration this stage
    would never reach for. `-no-checks` discharges it with everything else, one inlining later.
    `resolve/FixedBound.yana` is the fixture; the written-count and quantified-count bounds are both
    in its `.mir.lower.expect`.

### `@inline(n) @capacity(n)`, and the half of it this stage has

`readContainerRefinement` reads the pair off a type and checks it exactly as the shipping stage does -
both written, equal, and not zero, because the rows that spill need a discriminant neither stage
builds. What it then honours is the **capacity** and not the placement: the literal is built by
`arrayLiteral` at the number of elements it wrote and then reserved to the bound, through
`known.def`'s `arrayReserve` (`lib/Core/Array.native.yana`'s own `reserve`). Sizing the run from the
bound instead would make `length` the bound as well, which is not what the annotation says.

`@inline(n)` asks for those slots in the **frame**, and this stage still does not give them - but the
reason is not the placement ladder, which J24 built. It is a **layout**: the shipping stage's
`repr.cpp::computeInlineContainer` replaces the container's `Run(a)` field with `n` slots of the
element's stride, so there is no run pointer and no capacity word, and the tag the construction
writes is `runFixed`. Nothing in `resolve_ssa/lower/repr.cpp` has that rung, and `resolveType` refuses
the refinement anywhere but the two `::` positions - so `data Buffer {items: @inline(4) @capacity(4)
[Int]}` does not resolve here at all. `newRun` being a library call rather than a placed allocation
is the second half of the same gap: expanding it would need an allocation with an **extent**, which
`Inst::Alloc` does not carry. So a refined array costs an allocation and a free the shipping stage
does not pay and means the same thing; `withPathCString` and every path-taking `File` call run
correctly and not for free.

The two positions that honour a refinement are the `::` of an array literal in a body and in a
constant, and both read it off and resolve the type without the attributes the way the `@bits` arm
does. Anywhere else - a field, a parameter, a result - `resolveType` **reports** it: an ABI
annotation silently ignored is a representation its author believes in.

### A hand-over is a parameter, and a lending one needs the same temporary

`iter fn lent(n) -> Held: yield make()` left the temporary owned, borrowed by the consumer, and
released by nobody - reported one tier down as an owner reaching `return` unconsumed, which is where
it was found.

`yield e` is a call of `$body` and the hand-over is that call's one parameter, so a lending
convention leaves the *iterator's* frame owing the value's teardown. A value that is only a node
result has no place for a teardown to name, which is exactly what `holdBorrowed` exists for and what
every written argument already goes through; `resolveYield` did the two steps before it -
`adaptToView` and `coerce` - and stopped one short. It calls the same function now, so what a
borrowing position does to an owner is one answer and not two.

The other three conventions pass through untouched, and by `holdBorrowed`'s own first lines rather
than by a test here: `-> ->T` hands the value over, so the consumer owns what it received and this
frame stops owing anything; a hand-over of a type with nothing to run needs no place; a `{}` needs
neither.

`resolve/IterHandover.yana` is adopted, and answers 23 on both stages.

### The `Drop`, `Reclaim` and `Copy` classes

These three are **declarations**, and that is what G3 changed about them: `instance Drop(T)` is
checked against a class by the one rule every instance is checked by, and the hand-written shape test
per enumerator that used to do it is gone. A `Known` role survives as a *tag* on the declaration,
which is what the ownership classifier keys off.

They were *synthesized* in C++ until this stage had a library at all. `lib/Core/Core.yana` declares
all three with `@builtin(reclaim)`, `@builtin(drop)` and `@builtin(copy)` on them, so there is no
second implementation of a class declaration and no class belonging to no module.

**An authored `Copy` subtracts `TrivialCopy` at exactly that head** (`own.cpp`,
`doc/spec/ownership.md`, inventory D16). A concrete head - `instance Copy(String)` - is the author
saying the structural answer is wrong for that type, and it subtracts. A **parametric** head whose
own context asks for the same class - `instance (Copy(a)) Copy(Maybe(a))` - says the carrier copies
as far as its payload does, which is the structural answer restated: where the payload is trivial the
`Copy(a)` it requires is the blanket instance, which subtracts from nothing, and where it is not the
member fold has already said so. So `Maybe(Int)` is `TrivialCopy` and `Maybe(String)` is not,
`lib/Core/Core.yana`'s own comment beside those three instances is right as written, and the shipping
stage agrees. `inheritedCopy` is the one predicate, and reading it literally - every declared `Copy`
subtracting - is what made `let b = x` over a `Maybe(Int)` a double consume and stopped
`node(n)?.leaf?.value` handing a `Maybe` field out of a record with an authored teardown
(`test/resolve/OptChain.yana`, which is back to its original form).

They were recognized by *name* until the role tables existed - a declared class's name compared
against three spellings written beside the enum. That is the coupling `compiler/intrinsic` exists to
remove, and these three were its first rows: the compiler now holds no library names at all, and a
class says which role it fills where it is written. `Presence::Required` is what they are, and the
report for one nothing claimed names the role rather than a source position, because there is none -
a library that declares no `Drop` is a broken installation and not a broken program.

`Drop` is written in the module that declares the type and there may be several, so the class has to
be visible from every one - which is what the **implicit import of Core** does (`linkImports`), and
is an ordinary import rather than a rule of its own. `lookupClass` had a fallback over the role table
for this and no longer needs one; `lookupCoreMember`, which found `copy` the same way, is gone.

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

### Where an object lives, and the one function that says so

`analyze_escape.cpp` is the whole of placement. It is deliberately two things with a seam between
them - the **findings** (`Placement` in `place.h`) and the **ranking** (`selectStorage`) - and that
split is decision D6: `@heap`, the escape analysis and a future region witness each establish one
field, and none of them decides a rung. Implementation-Regions.md §4's ladder is then one line in one
function rather than a second placement rule somewhere that has to agree with the first.

`Frame` and `Heap` are both produced. `Region` still is not, and `verifyBody` refuses it, because
what it means is "in the region the ambient witness names" and a `GenContext` has no slot kind for
one.

**What selects `Heap` today is `@heap` on a binding** - `doc/Design.md`'s per-binding override, read
by `readBindingAttributes`. It is the only thing in the language that names a rung directly, and it
is one-directional: it moves an object off the frame and never onto it.

**The escape half is not a dataflow here, and that is a statement about the representation rather
than a milestone.** In an ownership MIR nothing escapes by being a value: a returned record is copied
into storage the caller passed in, a `->` argument is moved into the callee's own slot, a write into
a global fills the global's storage. In each of those the object travels and its storage stays
behind. What is left is storage reached through a **reference**, and every reference here is a loan -
so the escape seeds are exactly `analyze_loan.cpp`'s conformance failures, which already compute the
answer per position.

The one construct in the language that reaches them is a **closure**, and since D17 it is placed
rather than refused: `resolve_lambda.cpp` sets `Placement::outlivesFrame` from the position the
lambda fills - which is `checkCarriedOrigins`' own firing condition, an owning position - and asks
`selectStorage`. That is the second caller of the ranking and the first that is not an attribute.
What it does *not* go through is `Object::storage`: a closure's environment block is not the frame's
storage at all, so it is neither allocated by `buildSlots` nor freed by `releaseSlots`. It is
allocated where the pair is built and released by the pair's own teardown - see
`resolve_ssa/lower/fun.cpp`, and the closure section above for why the two are different animals.

**The allocation and its release live below the MIR**, which is the arrangement `@box` already has:
placement changes no move path, no initialization state and no drop step, because what a value owes
when it dies does not depend on where it was sitting. `Conversion::buildSlots` allocates a heap slot
where it would have made a frame one and `Conversion::releaseSlots` hands it back at each of the
scope's exits - one allocation per activation, one release per path out of it. That pairing is why a
heap-placed value handed over by a `->` argument, which owes no drop at all, still hands its block
back; `test/resolve/HeapRelease.yana` and `DropFlagHeap.yana` measure both against the allocator's
own bump pointer.

`Summary::placement` is the published half: the strongest rung any object an activation needs,
transitive over the call graph, joined by `computePlacement` on the fixpoint suspension already uses.

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
  conflict with itself. A **raw address** overlaps everything, *unless* its type says where it
  points: `%source'a`'s pointee origin set is exactly the loans the address may reach, so a write
  through a buffer of this frame's own no longer conflicts with every live borrow in the body. It
  says which storage and not whether anyone else writes through it, so two pointers into one source
  still conflict and `doc/spec/native.md`'s "proves no exclusivity" is untouched. An **unqualified**
  address read out of storage still reaches anywhere, and what it reborrows is what the read went
  through: `unionPlaceOrigins` walks back from the address to the place it was loaded from, so
  `n.next.value = ...` over a `data Node {next: %Node}` does not conflict with the `addressOf(first)`
  that produced `n` - a linked list is unwritable otherwise, and a field's own type has nowhere to
  name an origin.
- **A reborrow** is a loan whose origin set contains the one it reborrows, so §2.5 is a `skip`
  argument to the overlap check rather than a rule beside it.
- **A unique loan taken for an argument is a reservation until something uses it** - two-phase
  borrowing, `reserved` in `checkLoans`. `doc/spec/expressions.md` evaluates arguments left to right
  and the callee runs after all of them, so `pushFill(to, value.fill, value.width - (length(to) -
  start))` reads `to` at a point where nothing can yet have written through the `&to` beside it.
  Between the read that mints such a loan and the first operand naming that read's result it excludes
  a write and a move, and it excludes **a loan only where that loan is still live at the activation** -
  so a nested call taking the same storage exclusively and releasing it before the outer call is
  admitted (`readColor(options, takeValue(options, ...))`, `lib/Test/Options.yana`, eight times) and
  `twice(b, b)` is not. The liveness is asked at the *take*, which is what keeps the blame on the
  argument an author can change; a **shared** take is excused outright and reported at the activation
  if it survives, since a read cannot be the mistake. The activation is checked as the take would have
  been, against the loans live *before* that node - `loans/TwoPhase.yana` and `Reject.TwoPhase.yana`
  are the pair, and `ownership/Reject.Exchange.yana`'s `swap(x, x)` is the whole-place case that is
  refused where it is written.
- **A subscript written to takes its accessor after the right-hand side.** `xs[i] = xs[i] + 1` reads
  through `get` and writes through `getMut`, and the exclusive loan the second takes has to start
  after the first has ended - so `resolveAssign` resolves the container and the index, then the
  value, then the accessor. A **path of fields over the subscript** is the same statement one
  projection out (`xs[i].f = xs[i].f + n`, `lib/Test/Console.yana`'s tally), and it needs the field
  path's *type* before the place exists: `fieldPathType` answers that for a run of named fields of
  single-constructor records and nothing else, and where it cannot the target is built first exactly
  as it was. `fieldStep` is the one walk both this and `resolvePlace` apply.
- **The one exemption** is `exchangeOnlyLoan`: two exclusive loans of elements of one container may
  be live at once when each is used by nothing but a `swap` or an `exchange` - `doc/spec/core.md`'s
  Exchange, and `swap(xs[i], xs[j])` is what a sort is written with. The walk is structural, from the
  read that minted the loan through the subscript and derivation calls the compiler made
  (`NodeCall::subscript`, `NodeCall::derivation`), the local `hold` parked the element in, and the
  `address.exchange` the relocation reaches the storage through; anything else is a use the argument
  does not cover. It is a pair that is excused, so a swap's loan meeting an ordinary borrow of the
  container is refused as any other pair is - `ownership/Reject.Exchange.yana`.
- **Conformance** is `checkReturnConformance`: walk each returned loan to its place's root and ask
  whether the signature published it. A global satisfies any slot, a parameter in the declared slot
  is what was promised, and the three ways it can be neither get three different messages, because
  they have three different fixes. A root that is a **local holding a borrow of a published origin**
  is a reborrow rather than this frame's storage - `xs[i]` is a call whose result is parked in a
  frame slot so that a projection has a place to name, and what is *in* the slot was named by the
  producing signature as somebody else's. What it is really rooted in is in the same origin set, so
  the loop checks that instead. `Origin::Slot` and not merely "borrowed": a hand-over is
  `Origin::CallBounded` and a binding that holds one is exactly the escape
  `lens/Reject.Escape.yana` refuses.

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

`Request::establishes` is the one row of that table with an exception, and the exception is a *name*
rather than storage. "Writable" there is `Place::writable`, which is what the author declared -
`mutate(h)` for a plain `let h` is refused because a `&` argument must name mutable storage, and
`let &z = h` is how an author says otherwise. So the initializer of a `let &` is asked the storage
half and not the name half: over a place rooted in a **local** - a `let`, a pattern binding that took
a payload out, a parameter - it takes the exclusive loan `doc/spec/ownership.md` describes ("`let &d
= c` - an exclusive loan: writing through d writes c"). A **global** keeps the whole test, because
the program declared whether it may be written and no frame owns it, and a shared borrow is refused
by the row above whatever the binding says. `bindsOwnStorage` is the whole of it, and
`test/resolve/BindAxes.yana` is the fixture both stages now compile.

That one function is what makes `let x` a shared loan of what it names, `let &x` an exclusive one and
`let ->x` a handover, without any of the three being a case: the two axes of a binding are a
`Request`, and what the binding *becomes* is the initializer's answer.

**A type with one value is that value.** `{}` occupies no bytes, so there is nothing at a place of
that type to read: `readPlace` answers the constant rather than a load, and `Builder::resultOf`
answers the constant for a *node* whose result is one. Both halves are needed and they are the same
sentence - a call that answers `{}` returns no register, so an operand naming its result would name
nothing, and `resolve_ssa` refuses that loudly rather than lowering it to a null. What is left is a
`{}` reaching a call argument, which is the one position that still carries one, because the callee
declared a parameter for it. An **exclusive** request is the exception: `&x: {}` promises writing
through to the same storage and a constant has none, so it is refused by the ordinary rule.

### Records that hold a loan

A field declared `'T` or `&name'T` makes the record's interned `OwnershipSchema` non-empty - `own.cpp`
builds it, and `RecordType::schema` is where it lives. `Builder::fieldPlace` reaches through such a
field with a `Place::Deref`, and `resolve_ssa` lays it out as a pointer rather than as the object.

**The pointee's writability is the reference's access and not the record's.** Re-pointing the slot
writes the record, so the field place itself keeps the parent's answer; writing *through* the
reference touches no part of the record, so an exclusive `&'T` field of a record held in a plain
`let` still writes the storage it names. `findMut` answering `Maybe(&'v)` is the program - `Just(v)
-> v = v + 1` writes the map's own entry through a `Maybe` nobody declared `&` - and the narrowing
`fieldPlace` already applies is what removes the capability where the record was reached through a
shared borrow. It is the one projection exempt from the verifier's "a projection of a place nothing
may write is a place nothing may write", for that reason.

**A pattern binds a payload at the capability the payload carries**, and `&` on a pattern is not what
says so - `bindPattern` reads the position's own contract, exactly as `hold` reads a value's. A
payload whose position is `&m'Guard` binds as a `&` local and is read at `requestOf` rather than at a
plain binding request, since an unpublished binding of a `TrivialCopy` scalar is a copy and a copy is
not what `v = v + 1` was written against. `resolve/loans/Argument.yana` is the golden, and its
`exclusive()` used to record the gap in its own comment.

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

**A result writes no argument at all, and that is elision rule 1** - Analysis-Borrows.md §4.8,
`Resolver::elideResultOrigins`. A result whose *type* carries an interior position nobody labelled
takes the signature's one anonymous group, exactly as `-> 'v` at the root does: `padded(value: 'a,
width: Size) -> Aligned(a)` over `data Aligned(a) {value: 'a, ...}` is the shape every named
formatter in `lib/Core/Format` has, eighteen of them, and the argument could only ever name the one
group §4.8 measured the library to have. The shape is built where the result occurrence is derived -
the one place, so a written signature and an inherited one are elided by the same line - by
renumbering the record's own schema into this scope, which is `Reader(a')`'s mapping with the
argument supplied rather than written. A **pointer** result is the second kind: it has one interior
position and no field to name it, so what is built there is `%source'a`'s slot exactly.

Only at an **owned** root, which is where the silence is: `originSlotAt`'s third case already
answers an unbound interior position with the root's slot, so a result that borrows says something
about everything inside it. `loans/ResultElision.yana` is the fixture, and both stages compile it.

**And where the scope has no group at all, one is promoted onto the signature's candidates** - rules
5 and 6, `Resolver::promoteLoanCandidates`, in the form §4.8's own supersession block settled on.
Rule 5 was written as a *count* - "exactly one parameter could root this" - and a count is a property
of the whole signature that changes when something unrelated is added to it, so a rule that *picks*
by one turns that into silence: the signature keeps compiling, means something else, and every caller
is re-checked against a contract nobody wrote. So nothing is picked. **Every** candidate joins the one
anonymous group, which is rule 1 and is what a conservative group has always been, and a label is
what splits it where the coarse answer rejects a program worth accepting; rule 6 needs no clause of
its own, since `self` is a candidate like any other.

A **candidate** is an unlabelled parameter that refers to the caller's *storage* rather than to a
value it could equally have been handed a copy of: a unique (`&`) borrow, or a shared borrow whose
form is a view - `xs: [T]`, `s: String`. A plain `p: Pair` is not one, which is what keeps
`loans/Reject.Signature.yana`'s refusals refusals - it did not move - and an `Array(a)` owner
receiver and a raw pointer are not either, which §4.8 lists as types that cannot yet carry a group
rather than as an argument for guessing. Only the **anonymous** group elides: `-> a'Guard` in a
signature that published no `a` is naming a relationship the author has not written, and promoting a
parameter into `a` would be inventing their second one. `loans/ResultCandidate.yana` is the fixture,
and `iter fn each(xs: [String]) -> 'String` in `lib/Core/BorrowUpdate.test.native.yana` is why it
exists: it is the one library signature that had no spelling both stages accepted, because the
shipping stage promotes the same candidate (`settleLoanCandidates`) and refuses the explicit
`source'String`.

Still refused by name: an *owned field* whose type has a schema. `data Outer {inner: Reader(a')}`
would give `Outer`'s own schema `Reader`'s position at a deeper path, and `foldSchemas` walks the
records flat rather than on demand, so the fold order is what is missing rather than the spelling.

### A pointee is a place, and both spellings reach it

`*p = v`, `(*p).value = v` and `p.value = v` are one construct: the object a raw address names is a
place - `Place::Address`, the one root outside the safe model - and everything a place composes with
composes with it. `resolvePlace` reaches it two ways and they meet at `pointeeAt`: a prefix whose sole
callee carries the `pointerLoad` **role** (the role and not the spelling, so a program's own unary `*`
is an ordinary call), and a **projection** through a place that holds a pointer, which is
`pointeeThrough` and is what makes `p.value` mean `(*p).value`. The step is on the projections rather
than in `projectionTarget`, which a dot-*call* shares: `p.f(y)` is `f(p, y)` with the pointer itself
as argument zero, which is how `lib/Native`'s own operators are written.

The place is **always writable**, which is `lib/Native/Pointer.yana`'s own sentence - "the memory a
pointer names is always mutable, the pointer itself is not" - so `n.value = 5` through a plain
`let n = addressOf(node)` is a write and `n = m` on the same binding is still refused. A store through
it releases nothing, for the reason `expandPointerStore` gives at length: an address root has no move
path, so what the pointee held before is neither dropped nor tracked, and that is Native's to be
unsafe about.

`addressOf` answers the **storage** rather than a copy of it wherever the place it read has an access
that is not shared - an owned local, a `&` parameter. Whether the *binding* may be assigned was the
test until 2026-09-07 and it refused a plain `let node = Node {...}`, so `copyMemory(addressOf(node),
...)` wrote a temporary; a place this frame only shares has no exclusive loan to give and keeps the
copy, which is also the only reading available for storage this frame does not own. What
`test/resolve/Global.yana` records for `addressOf` of a *constant* is untouched and was never the
writability's answer: an immutable global whose initializer is a scalar constant reads **as** that
constant, so there is no place there at all.

### A pointer restated as a pointer keeps its pointee

`Builder::addRetype` carries the operand's `Occurrence::shape` across a pointer-to-pointer `bitcast`,
which is the third of the four `PositionShape` axes read at a *node* rather than at a signature.
`%source'a` says whose storage the address may be in, and reading the same bytes at another element
type does not change the answer; dropping it made the result a bare `%U8` - a pointer publishing
nothing - which `analyze_loan.cpp` reads as naming nobody's storage and its overlap rule then treats
as everybody's. `Core/Text.native.yana`'s two case conversions write through `bitcast(target + i)`
and were the last two `lib/Core` diagnostics because of it.

`unionPlaceOrigins` is the other half and had the symmetric hole: a place rooted in an **address**
reaches what the pointer's type says, and only `reachFrom` knew that. So a write *through* a
qualified pointer reached the loan it was writing through and reborrowed nothing -
`store(base + count, 0)` in `withPathCString` conflicting with the `valuesMut(buffer)` loan that
produced `base`. Both callers ask one function now.

### A borrow inside a type argument

`Maybe('v)` and `Entry('k, 'v)` are the other half - Analysis-Borrows-Revised.md §2.4's structural
qualification, and the difference from `Reader(a')` above is one sentence. A loan argument
**renumbers** a position the record already declared; a qualified type argument names a position the
record does *not* have, because the declaration wrote a member there.

The logical type is unchanged and that is the whole design: `Maybe('v)` binds `a := v`, interns as
`Maybe(v)`, and selects `v`'s instances - "constraints and class instances are selected for `String`,
never for a view type". What the tick adds is an `Occurrence::shape` naming the payload, which is
`type.def`'s opening paragraph paying off: a borrow is a contract over a logical type, so a type that
holds one in a *type argument* position needs no new kind, no sibling record and no substitution rule.

What it does change is the bytes. `Maybe('v)`'s payload is a reference where `Maybe(v)`'s is the
value, so one logical type is laid out two ways in one program and the shape has to travel to
everything that places or projects:

| Reader | What it does with the shape |
| --- | --- |
| `Resolver::resolveArgument` | reads the sigils on one type argument into an `Occurrence` |
| `Resolver::bindArgumentPositions` | walks the *declaration*'s fields to the parameter, as paths |
| `Resolver::prefixArgumentShape` | moves the argument's own positions under the field holding it |
| `Request::shape` | carries what the destination asked for *down* into the expression filling it |
| `Resolver::resolveConstruction` | fills each field under `fieldContract`'s answer, not the field's |
| `Builder::addAggregate` | takes each component under it, and mints the value carrying it |
| `Builder::fieldPlace` | reaches a qualified position with a `Place::Deref`, as an external field |
| `layoutRecord` | manifests each field under it, and does **not** memoize on the type |
| `InstFieldAddress::shape` | carries the subject's, because the base's own type is a pointer |

`qualifiesInterior` is the one test that tells this apart from a statement about *provenance* - a
field the record **owns**, held at this occurrence as a borrow, which is exactly what `membersOf`
lays out differently and exactly what a derived teardown would otherwise release out of somebody
else's storage. Restricted to records, because a closure's environment word and a pointer's pointee
are positions those kinds have whether or not anybody said whose storage they name; and to a path of
one field step, because a deeper one reaches *through* the member rather than standing in for it.
Three readers rest on it and they have to be the same test - `ownershipOfOccurrence` for what the
value owes, `checkQualifiedOccurrence` for what a declaration may say, `checkCarriedOrigins` for
where such a value may go. It was a slot **count** against the type's own schema until 2026-09-05,
which is an approximation wrong in both directions: `Maybe(%s'U8)` adds a slot and takes no member,
so a value that still owed a teardown was answered trivial.

**The shape travels on the `Request`**, and that is a decision rather than a convenience. A
construction is the only expression that *fills* an interior position, and what it was filling cannot
be recovered afterwards: reading a bare name under an owning request copies it, and nothing about the
copy says which name it came from. So the destination is pushed down - and because it rides on the
request, it reaches a construction in an `if` arm, a `match` arm, a block's tail and an argument
position by the same route that already carries the access the position asks for. A parallel
parameter reached only what was written directly after `return`.

**What the value questions are asked of.** `hasTeardownAt`, `isTrackedAt`, `isTriviallyCopyableAt`
and `isLinearAt` are `ownershipOf`'s answers read off the *occurrence*, and the rule is about the
site: anything holding an occurrence asks the occurrence. They move together because they are read
against each other - `readMode` answers `CopyTrivial` from one and `verifyBody` checks it with the
same one - and a site that asked the type while its neighbour asked the occurrence is how a value
that copies freely got a move path and a diagnostic saying it could not be copied. `captureKindOf`
is the deliberate exception: a capture that copied a value carrying somebody's borrow would put the
borrow where no loan records it, so it asks the type and takes the shared rung.

Four things it does not do, each refused where it is written:

- **A field may not qualify one.** The positions are found by walking the applied declaration's
  fields, and a field's own type is resolved inside the pass that reads them - so
  `data Node {leaf: Maybe('v)}` would find `Maybe`'s fields or not depending on which `data` the walk
  reached first. What a field writes instead is an origin on its own type.
- **One field step, and it comes first.** That is the whole of the depth rule, and both halves of it
  follow from `fieldPosition`: a path of exactly one field step is what `layoutRecord` reads, so a
  second field step would ask the *inner* record to be laid out differently and nothing tells it.
  A **pointee** step costs nothing, because it is an indirection rather than a place in the bytes -
  so `Maybe(%s'U8)` composes out of two readers and gives `[.Just, *]`, the same path shape an
  external field has been interned as since milestone 3, while `Maybe(Boxed('v))` is refused.
- **The residue must be nothing.** A derived teardown is generated per type and descends into the
  fields the declaration owns, with no way to be told to skip a borrowed one - so the qualification is
  admitted only where the bytes the member left behind owe nothing, to release or to duplicate.
  `checkQualifiedShapes` is that pass, and it runs after `classifyTypes` because that is the first
  point the answer exists. `ownershipOfOccurrence` is where it is spent: a qualified occurrence is
  trivial, which is a constant rather than a second fold. A declared **`Copy`** is deliberately not
  counted, and the division is between what the compiler inserts and what a use site asks for: a
  partial teardown has no site to blame and has to be refused where it is written, while an authored
  `copy` is a call, and the call is a position like any other. That is the next bullet.
- **A qualified value may only fill a position qualified the same way**, at every convention.
  `checkCarriedOrigins` is that rule and it is one rule: the extent half beside it in the same
  function exempts a borrowed position, because a borrow is bounded by the call it was granted for
  and a callback holding this frame open for the duration is the feature. A **layout** is bounded by
  nothing, and neither is what the callee's type offers to do with the value whole - `copy` of a
  `Cell('v)` selects `Copy(v)` and duplicates what the borrow points at, because a borrow handed to a
  bare type variable binds what it points at. Both directions, since the lie is symmetric.
  `Reject.Shape.yana` is the fixture; before this rule the borrowed case compiled and answered zero.
- **An instance head may not carry a capability.** `instance Chunked('[a], a)` names `Array(a)`,
  which is the same handle `[a]` and `Array(a)` intern to, so it is the unqualified head written
  twice. That is a real difference from the shipping stage, where `Flat(a)` is a sibling record and
  the two are two types - it is `type.def`'s "no slice sibling" met by a library written against the
  other reading, and it is the library's line to change. **No such head is left in `lib/`.**
  `Contiguous('[a], a)` and `Writable('[a], a)` went on 2026-09-04, generated by the shipping stage
  rather than written, because the identity is a fact about the representation - `emitWindowItself`
  there. `Chunked('[a], a)`, `Eq('[a])` and `Show('[a])` went on 2026-09-05, when the shipping stage
  learned that a window selects the container's instance wherever the class only reads the container
  - `ClassInstance::windowArgs`, and the `Array(a)` heads beside them went too, since one instance
  now answers both forms. `ShowWith(Base64Style, '[U8])` and `ShowWith(HexBytesStyle, '[U8])` went
  the same day and by a different rule, because their subject reaches the class only through a
  *field*: a field declared as a borrow of something that turns out to be a container stores that
  container's window (`foldStoredWindow`), so `Formatted(HexBytesStyle, [U8])` is one record a window
  and an owner both fill. See Analysis-IR.md §11.4.1.

`test/resolve/loans/Argument.yana` is the accepting grid - an optional borrow built, returned and
matched; the generic form one body serves; two parameters qualified at once; the exclusive spelling;
a value filled from an `if` arm, a `match` arm and an argument position; a pointee composed under a
field; and the same value consumed twice, which is what the trivial-copy answer buys - with a
per-`Guard` release count, so a payload that had been copied in would be released twice. `Reject.Argument.yana` and `Reject.Residue.yana` are the refusals, and they are two files
because the residue is checked after `classifyTypes`, which a program with any declaration error
never reaches.

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

**And the optimizer takes the outlining back apart, which is why one lowering is enough.**
`flattenContinuations` in `resolve_ssa/opt/inline.cpp` copies the iterator's body into its consumer -
which brings the call through the pair into the frame that *built* the pair - resolves that call to
the declaration the pair names, and copies the continuation back in, to a fixed point. A three-stage
chain is six of those steps and comes out as one loop with the bodies in it. So the *embed* lowering
`ssa::Continuation`'s note describes is a question about what the conversion emits rather than about
what the backend receives: an adaptor chain is already flattened by the time anything is lowered, and
what the outlining costs is paid only where the optimizer declines - a recursive consumer, a budget
spent, or `-inline none`. `test/resolve/FlattenIter.yana` is the worked example and asserts both
halves, including the recursive one that must stay a call.

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

### What a `for` binds, and how a pattern reaches it

The hand-over lands in one local of the continuation whatever the loop wrote, so a pattern on a `for`
is the same construct a `let` over any other place is: `bindIrrefutable` takes that local apart by
reading through it, and what each name binds - a shared borrow, or the owned part where the iterator
declared `-> ->Item` - is the pattern's own rule read at the place, not a second rule written for
loops. `handoverCall` hands the local out for that reason rather than only naming it; a `for` binding
a plain name has already been given it by name and ignores the extra.

A **refutable** pattern is refused, and by the language rather than by this stage: a `for`'s `|`
introduces the *iterator's* failure channel, so there is no position in the grammar for the arms a
failing pattern would need. The report is the coverage question a `let` with no alternative asks,
over the same `PatternSpace`, and it names what is left over. It is emitted **after** the pattern is
built rather than before, which is the one place this differs from `resolveBinding`'s order: a
pattern the type has no reading for at all - `{a, b}` over an `I32` - leaves a gap because it matches
nothing, and reporting the gap first would say "this can fail" about a shape that is simply wrong.

The counted `for` goes the same way over its own counter local, so there is one answer and not two.
`ForPattern.yana` and `ownership/Reject.ForPattern.yana`.

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

### What the continuation's parameter is rooted in

A lens call's edge supplies no argument - what arrives at the region's entry is produced *inside* the
call - so the origin walk's edge rule finds nothing to union into the entry's parameter, and the
parameter came back rooted in nothing. That is fine for a hand-over the callee built, and wrong for
every one that is a **window into an argument**.

`for x in items(xs)` is the shape. The traversal borrows `xs`, hands a window into its storage to the
body, and nothing related the two: `xs`'s last use was the call, so `placeDrops` put its teardown at
the top of the loop body and released the run the window points into before the first element was
read. The elements then read whatever the allocator's free list had written over them, which is two
words of zeroes at the front of the block and everything after it intact - a wrong answer rather than
a crash.

So `analyze_loan.cpp` maps a `LensCall`'s arguments onto the entry parameter exactly as it maps a
call's onto its result, with one difference: the slot is read off `Continuation::handover` and not off
`callee.result`, because an `iter fn`'s declared result is the continuation's *answer* and what is
handed over is the continuation parameter's type.

The slot is what the *published* half is matched on, and an **unqualified** hand-over publishes
nothing - so the walk keyed on it related nothing at all for `pub iter fn (Chunked(c, a)) items(self:
c) -> a`, which is the spelling `lib/Core/Container.yana` settled on (a `-> 'a` there needs `self:
'c`, which the shipping stage then reads as a reference *to* the container). An unqualified hand-over
therefore takes **every** argument's loans rather than none: `-> a` is a loan bounded by the
continuation, so what is behind it is anything the call was lent and the callee is still running.
Conservative in the only direction that is safe - it keeps storage alive rather than releasing it -
and exact wherever the signature published a slot. `test/resolve/lens/Reject.Escape.yana`'s third case
is where the over-approximation is visible: the message now names the argument the value could be
rooted in instead of the binding, and it is still a refusal.

The other half is the library's. `class Chunked(c -> a)` may write `iter fn chunks(self: source'c) ->
source'[a]` where the relation is worth stating exactly; the compiler no longer depends on it.

### A place the expansion writes through is the owner

`op.def`'s `placeAt()` and `placeSameAs(n)` - `swap`'s two parameters and `exchange`'s first - say the
position names *storage of exactly this type*, so `declareSignature` gives it `AccessForm::Owner`
rather than `WholeValue`. The difference is what a substitution does to it: `WholeValue` at a type
variable becomes the **view** wherever the argument has one, which is right for a position a callee
reads through and exactly wrong for one the expansion writes. At `a := Array(I32)` the argument was
adapted by `elementsMut` into a descriptor in a temporary, and `swap` then exchanged two temporaries
while the caller's containers stood still - seven cases of the library's suite saying "expected 8, got
3". Read off the shape column rather than named here, so a row added with a place parameter needs no
second edit, and set at the declaration rather than at the expansion, because by the time `expandSwap`
has its operands the derivation is already a node in the body.

An **exclusive result** at a bare type variable is the same sentence one level down.
`Index.getMut(&self: source'c, index: k) -> &source'v` wants opposite answers at its two variable
positions: the *subject* takes the window, because a caller holds either an owner or a window and the
window is the form both adapt to (which is what lets `sortRange(&xs: [a])` write `xs[i]`), and the
*result* is the element's own storage - `xs[i] = ys`, `push(xs[i], 5)` and `swap(xs[0], xs[1])` each
write the whole value. So `resolveOccurrence` answers `Owner` for every exclusive result, at a
concrete type as well as at a variable, which is what keeps a class member's `-> &v` and an instance's
`-> &CodeUnit` one signature. `expandPointerBorrow` carries the declared result's *form* onto the
place it reads, for the same reason: built at `WholeValue` the loan became a view under substitution
and `borrowMut(valuesMut(self) + index)` handed back a descriptor.

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
- **Bare** - `f(...)` written as a statement, which is the flat form with nothing bound
  (`resolveBareLensCall`). `flatExtent` bounds a `let` by the last use of what it bound, and a call
  that binds nothing has no last use - so the extent is *the rest of the block*, which is what the
  statement means: `cell.withLock()` holds while the code below it runs, and a shorter extent would
  be a lock released at a line nothing marks. Every lock in `lib/` is written this way.

A lens call written **inside** a statement is lifted in front of it rather than given a fourth form
- `hoistBlockLenses`, run once over a block before any statement of it is walked. The call moves to
a `let $lensN = ...` inserted before the statement and the position it stood in is left naming that
binding, so what is resolved is a program the flat form already compiles; `test/resolve/LensHoist.yana`
asserts that the lifted form means what the hand-written one does. The walk is in evaluation order
and stops at the first call it would move, and it declines three things by name: an argument of a
callee with a `@lazy` position (`defersArguments` - `&&`'s right operand runs under a branch, so a
lens lifted out of it would run where the program does not), the arms of an `if`, a `match` and a
lambda body (no case in the walk, for the same reason), and a statement that binds **several** names,
which is refused below rather than rewritten.

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
extent belongs to the statement and there would be two of them; the hoist declines it for the same
reason, since lifting a call out of one would give it an extent covering the other binding. And a
block whose **own value** falls inside an extent has nowhere to put it: that value is `$r`, which
today carries the exit discriminant alone, and it arrives with a skipping lens's `Try`.

### What a hand-over may be rooted in, and `yield'`

A hand-over is a *binding* position, so its default is `Origin::CallBounded` - "cannot outlive the
call or continuation it was granted for" - which is exactly what lets `withLock` yield a view of its
own frame with nothing written. That covers the **root** and it is why five of the seven lenses in
`lib/` need no qualifier at all.

What has no default is a position **inside** the handed-over value, and the one that matters is a raw
pointer's pointee. `-> %U8` says nothing about whose storage the address is in; a pointer is
trivially copyable, so a continuation may copy the address into something that outlives the call and
the copy is an owned scalar rooted in nothing. `yield'` is the spelling for that position, and it is
`Origin::CallBounded` at a path step rather than a mechanism of its own - which is
Analysis-Lens.md §12.2's whole claim, that an interior position is an ordinary position.

`Resolver::tickContract` is the one reader of a written tick, shared by the root and the pointee. It
is legal in a hand-over and refused elsewhere naming the alternative: on a field a named origin the
record publishes is wanted, and on a plain result there is no continuation for the bound to be about.
The parser reads `Token::kwYield` followed by the marker, so no `originlabel` can collide with it -
which is why the origin is a keyword and not a reserved name.

`checkHandoverConformance` is the body half. A hand-over whose value is rooted in **this frame's own
storage** at a position the declaration did not describe at all is reported, naming the spelling; the
same statement about a `return` is `checkReturnConformance`, which needed nothing added. A pointer
manufactured from an integer, an allocator or foreign code is rooted in no loan, so a bare `%T` there
stays what it is - which is what keeps this a rule about values the model can follow rather than a
migration of every signature that names a pointer.

**Both halves follow the *pointee position* of a value, and a value has one only where its type names
an origin - which is a hole, found when `prelude/` went (J21).** The stand-in library wrote
`addressOf(it: source'a) -> %source'a`, so `addressOf(v)` of a local produced a value with a pointee
position carrying the loan, and the rule fired. `lib/Native/Pointer.yana` writes `addressOf(it: a) ->
%a`, deliberately (`test/resolve/Global.yana` pins what that means for a constant), so the same call
produces a `%I32` with **one** position: `expandAddressOf` materializes or borrows and the loan is in
the body, while the type says nothing, and `fn leaks() -> %I32: let &v = 7; return addressOf(v)` is
*accepted*. `test/resolve/ownership/Reject.Pointer.yana` is written through
`values(self: source'[a]) -> %source'a` for exactly that reason and says so.

Closing it means reading the loan off the producing **node** where the type published nothing, rather
than off the value's positions - which is a second way of asking the same question and wants
designing before it is written.

**The shipping stage now reads `yield'` as no group** (`compiler/resolve/type_ast.cpp`'s
`resolveLoanGroup`), which is what a plain `%U8` already meant there - `kNoLoan`, read by every
consumer as "every group". It used to refuse it, on the argument that a group numbered beside the
author's own labels would be a lie; the lie that mattered was the other one, because refusing left
`lib/Native/CString.native.yana`'s `withPathCString` with no spelling both stages accept. D1 says a
`lib/` change stays accepted by the shipping stage, so the two ends had to agree on one signature and
this is the end where admitting it costs no check.

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

**The frames themselves stop at a region**, which is `Resolver::heldFloor` and was the half of this
that had been left to hope. A lens call carrying nothing says what the *call* does; what an enclosing
construct is holding is a different question, and a counted `for` whose body opens a continuation
region was threading its bound and its stride into that region's blocks and reading them back out on
the far side. `for round in 0..n:` around `for e in entries(root):` was exactly that, and what it
produced passed the MIR verifier and reported at the SSA conversion - an operand naming a node with
no value, twice, once at each end of the boundary. So the frames standing when a `Handover` region is
entered do not cross it: their handles are untouched while it is built, and the block after the call
is dominated by the block before it, so what they name is still in scope when the walk comes back
out. A value a program genuinely needed inside a continuation would need storage rather than an edge,
which is what `bindingFloor` says about a lambda one tier over.

### Two ways a borrowed place fills a position that consumes

A plain parameter is a shared borrow, so "hand this over" and "this is somebody else's" meet at every
`fn f(x: T) = eat(x)`. `readMode` answers `BorrowShared` there and `Resolver::readPlace` reports it,
and that is the rule. Two things are allowed past it, and they are different mechanisms with
different obligations - which is why they are two and not one relaxation of the refusal.

**The copy upgrade** is Design-Memory §2.1, and it is a fact about the *type*. A value that
duplicates by its bytes can fill a sink by duplicating, and the copy is what is handed over; nothing
is owed afterwards. `readMode`'s first arm has always done this for a concrete type. What was missing
is the same answer inside a generic body: `ownershipOf` treats a type variable as non-`TrivialCopy`,
which is Implementation-Generics.md §1's invariant and right - a body has one meaning for every
substitution - but the invariant says *unconstrained*, and a signature that declared `TrivialCopy(a)`
has supplied the answer. `promisesTrivialCopy` in `type.cpp` is that reading, and because it is
answered in `ownershipOf` rather than at the read, every consumer moves with it at once: the mode,
the verifier's `CopyTrivial` evidence, whether the value gets a move path, whether a teardown is
placed, and `conventionFor`'s rung one tier down. Answering it at `readPlace` instead would have been
one site agreeing with the constraint and five sites still reading the type.

It is the **declared** requirements only and not their superclass closure, which is `classProves` and
substitutes into an argument list this cannot build. The direction that costs is the safe one: a body
under `(AtomicValue(a))` is refused where it could have copied. Five `lib/Core` sites are this rule -
`Core.yana`'s blanket `Copy`, `Format.yana`'s `with`, and the three folds and readers that write
`let &acc = ifEmpty` - and `ownership/CopyUpgrade.yana` and `Reject.CopyUpgrade.yana` are the pair
that differ only by the constraint.

**Taking the contents out and putting them back** is `doc/spec/ownership.md`'s other sentence, and it
is a fact about the *place*. `pub fn (Append(a)) ++=(&target: a, ->amount: a): target = target ++
amount` has no other body: `++` consumes both operands, and `target` is storage the caller owns. The
storage is emptied, something is written back, and only then does the frame let go of the reference -
so at every point the caller can observe, the storage holds a value. Two properties of the language
make the window unobservable and both are properties rather than assumptions: an exclusive loan is
exclusive while it is live, so no second reader exists to look into the hole, and nothing unwinds, so
no teardown runs over one on the way out.

`lendsItsContents` is the whole of what is admitted, and it is narrower than "an exclusive loan"
deliberately: a **`&` parameter** and nothing else. `let &d = c` re-lends storage this frame is also
tracking under `c`'s own move path, and a take through the alias would empty one place while the
other still said it held a value - `ownership/Reject.BorrowAlias.yana`. A parameter's lender is the
caller, whose own analysis holds the argument exclusively for the length of the call, so there is no
second path here to disagree with.

What carries it is the machinery that already exists rather than a rule beside it. `buildPaths` gives
such a parameter a move path - for the *hole*, since nothing here is ever released - `readMode`
answers `Forward` so that liveness decides the take exactly as it does for an owned name, and the
assignment that follows is normalized to `Reinit` by the same dataflow that normalizes every other
one, because a place holding nothing releases nothing. Three things then say the value is not this
frame's: the drop walk skips a lending path before it looks at teardowns, `verifyBody` admits the
`Take` out of a place the function does not own for these places and no others, and the obligation is
checked at every `Return` - the point the caller may look into its own storage again. `Maybe` is
refused there with the same message as `Moved`: a refill on one path and not on another leaves the
caller holding a value only a run-time bit knows about, and a drop flag is not an answer, because a
flag says whether to release and what is missing is a value. `ownership/BorrowRefill.yana` and
`Reject.BorrowRefill.yana`.

What is **not** built is the same statement about a continuation's environment - `acc` inside a `for`
body is a field of a heap environment reached through a pointer, which is the second tier
`lib/Core/BorrowUpdate.test.native.yana` asserts in the shipping stage. It needs a lending *path*
rooted in something other than a local, which is the same shape `Place::Address` is refused as today.

### What is refused, and why

Three of these were found by `test/resolve/ownership/` and the rest by `test/resolve/loans/`, which
is what those directories are for:

- **A field may not be handed out of a value with an authored teardown.** An authored teardown runs
  on the whole value, and after a partial move there is no whole value for it to run on - so the
  authored body is silently *not run*, which is the one outcome a teardown may not have. A *derived*
  teardown is unaffected and must be: releasing the members one at a time is exactly what it does.
- **Storage this function borrows may not fill a sink**, with the two exceptions below. `readMode`
  answers `BorrowShared` for a place the frame does not own, whatever the position asked, and that is
  the right mode; the position is what makes it wrong, so `Resolver::readPlace` is where the pair is
  judged. Before it existed, `fn f(g: Guard) = eat(g)` compiled and the guard was released twice.
- **A diagnostic names the path, not the binding.** `placeText` in `print.cpp` is the one spelling,
  shared by the dump, the resolver and the analysis. A message that called `p.left` "p" could not
  tell "the record is gone" from "one of its fields is", which is the distinction partial ownership
  exists to make.
- **A position asking to write is refused for two different reasons.** The storage is somebody
  else's and shared, or the binding is one the author did not write `&` on. `Resolver::readPlace`
  keeps them apart because telling an author to add `&` where the storage was never theirs is advice
  that cannot be taken. The first is a question about the *storage* and not about the binding, so
  `let &acc = ifEmpty` over a `TrivialCopy`-constrained parameter never reaches it: what fills the
  binding is a copy, and the copy is storage the frame owns.
- **Two arms of a value `if` must agree on ownership, and §2.5's join is built for one pair of them.**
  A merge parameter has one occurrence and every edge supplies that one, so an arm whose contract
  differs has to be brought to the merge's before the jump. `joinArmLoans` does that for the pair a
  `let` over a `match` arrives at - an **owned** arm beside a **call-bounded loan**, which is
  `let directory = match variable("TMPDIR"): Just(->named) -> named; Nothing -> "/tmp"` in
  `lib/Test/Harness.native.yana`, one alternative naming storage that exists and the other producing
  some. The join is the *weaker*: the produced value gets a frame slot and its edge supplies a
  borrow of it (`holdBorrowed`, which is already the rule for a produced value filling a borrowing
  position one construct down), so the merge is a loan over the union of the alternatives' origins
  and no arm has claimed anything it did not have. Weakening upward instead - copying the loan so
  the merge owns one - is the allocation this language makes a program say out loud, and a merge is
  the one place it would happen silently.

  Two loans that **disagree** are still refused, and by name: a published origin weakened to a
  call-bounded one, or an exclusive loan weakened to a shared one, changes what the merge is
  carrying rather than where it came from, and there is no `Read` of a value to re-mint it with. A
  `TrivialCopy` type reaches neither case - `promisesTrivialCopy` has already made every arm owned,
  which is what the three `lib/Core` bodies that used to report here (`Bits.yana`'s `readSigned`,
  `Read.yana`'s `limit`, `Format/Bytes.yana`'s rounding) turned out to be.
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
- **A closure may not carry a loan out of the frame it was written in**, and that is the whole of
  what is left of the escape refusals after D17. A lambda written *at* a position that owns it gets a
  heap-placed environment and may go anywhere; what stays refused is a `&` capture at such a position
  (a pointer into a frame the value outlives, which nothing downstream catches because a value owning
  its environment carries no origin), a capture of a binding this function only borrows, and a lambda
  written at a *borrowing* position that is then sent to an owning one - the environment was placed
  on the frame before anything asked it to leave, so the message names the position rather than the
  value. See the closure section above.
- **A requirement is spent by a specialization or read out of an environment, and which is the
  declaration's `form`.** `selectGenForms` in specialize.cpp decides one per generic: `@specialize`
  and `@generic` write it and everything else measures it. Both halves exist - what a specialization
  does is here, and what the erased form is belongs to `resolve_ssa/lower/erase.cpp`, which is where
  every number of the ABI lives.

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
  Every **call** as well, through `erasableCall`: the one shape an environment cannot make is
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
- **A witness table is flattened, which is how a superclass is reached.** A class's own members come
  first and each superclass's table follows, in declaration order and recursively -
  `classWitnessCount` states the layout, `classWitnessAt` reads it back and `classWitnessPath` is
  `classProves` carrying the offset the proof landed at. So a member reached through a *superclass*
  of a declared requirement is an **offset** into the witness the caller already passed rather than a
  second load, `freezeSchema`'s "superclasses are not slots of their own" reads as "they are ranges",
  and a range of a flattened table is itself a valid table - which is what makes handing one on
  possible at all. The alternative is a pointer per superclass, which is what the shipping stage
  writes: it buys a shared grandparent being one table and costs a load per level at every dispatch,
  and a hierarchy here is two or three deep with three or four cells per class.
  `generic/Erased.Superclass.yana` asserts it three levels deep, by arithmetic, because a wrong
  offset reaches the wrong member of the right table and returns rather than crashing. Built J27.
- **What an erased body still refuses is one shape: a generic called at this body's own type
  parameters.** An environment is a table in the image built by a caller that knows every argument;
  what this needs is one built on the **frame**, filled by copying the caller's own cells - which is
  possible now in a way it was not, because a class slot can be forwarded by adding a constant to an
  anchor-relative cell. What it is waiting for is a decision rather than a mechanism: whether a
  forwarding that can only *copy* (so a call at a composed argument like `Pair(a)`, whose descriptor
  the caller does not hold, stays refused) is the feature, or whether the caller should be able to
  build a descriptor on the frame as well. `Reject.Erased.yana` is the fixture.
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
`yield` is milestone 9. The **lattices** were built ahead of their passes, and that was the point:
`boundedStack: bool` could not publish the byte count an external iterator has to allocate, and
`retainsArguments: bool` lost the only thing a caller wanted to know - *which* argument - so a caller
with one frame-placed argument had to treat all of them as retained. A fact nobody can act on is how
a consumer ends up recomputing its own. They are `StackBound` and `ArgumentSet`, with `consumes`
beside them for linear consumption. The instantiation set is deliberately still absent, and `Summary`
says what it is waiting for: every fact there is a lattice element that fits in a word, and a set of
targets needs storage the summary would have to own, keyed by an identity milestone 6's substitution
decides.

Each fact is **read through an accessor and written through a named setter**, and the confidences for
all nine live in one `U32`. `Fact<T>` is still the vocabulary - a fact cannot be read without its
confidence, and `get(fallback)` still makes the caller name what it falls back to - but stored as a
pair it cost seven bytes of padding per fact whose value is a `U64`, and four of them are: eighty
bytes carrying thirty-two of payload and six two-bit numbers. Forty after that, and fifty-six since
J37: `escapes` is the ninth fact and the fifth `U64`, so it added the eight bytes a lattice value
that does not fit in the tail always would, and the ninth confidence took `states` from two bytes to
four, which pushed the five single-byte values out of the padding they were sharing. A tenth row of
either cheap shape is free again - the layout note on `Summary` is where that is worked out.

#### The six rows the fixpoint fills beside suspension

**`retains`, `escapes`, `mutates` and `consumes`** are one walk, because they are the same question
about four verbs. What makes them answerable here rather than by the eight hundred lines the shipping stage
spends is `analyze_escape.cpp`'s own opening observation: nothing in an ownership MIR reaches a
caller's storage by *being a value* - a record is copied, an owned argument is moved into the
callee's slot, a returned value is written into storage the caller passed in. The two ways back are a
**loan**, which the loan checker has already resolved per value and per interior position
(`Elaboration::rootedIn`), and a **raw address**, which takes no loan and is followed structurally.
So a body's own answer is one sweep over four node kinds - a write mutates what its destination
reaches and retains what it stored where that destination outlives the frame, a return retains what
it hands back, and an atomic or a native operation reaches storage through an address this tier
cannot read further into - and the interprocedural half is a join per call site over the callee's
rows, on the same worklist.

Each call site's arguments are resolved to the caller's parameters **once**, before the fixpoint, so
a callee whose row grows costs a join per site rather than a re-analysis of every caller. A call this
pass cannot attribute to a position - an indirect call, a witness slot, a generic with no
specialization - starts its caller's rows at the top of the lattice, which is the same fallback
`bodyKnown == false` gets one level out.

`consumes` is the odd row and says so: `checkLinear` refuses a body that does not discharge an owned
parameter on every path, so the owned positions *are* the answer and the join contributes nothing.

**`escapes` is `retains` asked about the address**, and J37 built it because the difference is what
`ssa::Contained` needs. `retains` is a reach set over *values*: `growArray(&self, n)` reads a run
pointer out of `self` and stores it back into `self`, so parameter 0 is in the written value's reach
and reads as retained - and no address of `self`'s storage went anywhere at all. Containment is
asking the second question and was being answered the first, which is why no container's frame slot
was ever contained (`test/bench/findings.md` §83.2 is where that was found and §84 is the number).

The narrower walk follows only chains that never pass through a **load of the caller's bytes**: an
`addressOf` of the parameter's place or of a projection of it, the incoming pointer of a `%T`
parameter, one `Deref` of *that* pointer, and a place handed on at another call's argument position.
A `Deref` whose parent is not the root ends the chain, because what it names is wherever the pointer
it read pointed. What makes that sound is `Contained`'s own premise rather than an argument of its
own: an allocation containment admits is one whose address is never stored, so no pointer the callee
loads out of memory can be it. A `return` is read on the result's contract - an owned result hands
back a value, so `elementBase(&self) -> %a` returns the run pointer and not the header's address,
and a borrowed result hands back a place whose address does outlive the call.

**`stack`** publishes `isBounded()` and zero bytes, which is Analysis-External-Iterators §4.5's own
split rather than a gap - "the resolver can validate that the call graph is bounded, but it cannot
know the exact bytes", because spill slots and the outgoing argument area are decided after register
allocation. It is a *peel* and not a join: nothing in a recursive cycle ever becomes unbounded by a
callee changing its answer, so Kahn's algorithm over the callee graph finalizes exactly the functions
that reach no cycle and what is left over is the answer.

**`diverges`** is the divergence fact - whether calling this ever comes back - and it is the shipping
stage's declared `Function::noReturn` inferred instead. The seed is a role and not a walk:
`lib/Core/Check.yana`'s `checkFailed` is `abortProcess()`, which is `exitProcess(134)`, which is a
`syscall1` followed by an ordinary `return`, so no CFG in the program says the process stopped.
Above the seed everything is a CFG answer - a body diverges when its entry cannot reach a `return`
with the block of every diverging call cut off at that call - and that is why this one row recomputes
its base case on the worklist rather than joining a lattice. `resolve_ssa/opt`'s
`endNonReturningBlocks` is the reader.

`while True` **is** caught since 2026-09-07 (J30), and the note here used to say it was not: the
branch is still a branch at this tier, but nothing said the backward walk had to follow both of its
edges. `forEachLiveEdge` takes the one edge a **decided** condition names - a `Const` operand at a
`Bool` or an integer - so a loop the program never leaves stops reaching a `return` through the arm
it never takes. The constant is *read* rather than folded into MIR, which is the decision: folding it
is a CFG edit that would move every golden that holds one, for the sake of a fact this walk is the
only reader of. What is still not caught is a condition that has to be *computed* to be decided -
`while 1 < 2` is `test/resolve/InlineEndless.yana`'s `endless`, and that comparison is the SSA
folder's one tier later.

The consumers, so that no row here is a fact nobody acts on: `mutates` is read by
`ssa::mayWriteStorage`, where a call through a `&` parameter nothing writes stops being a barrier for
a forwarded load; `escapes` by `ssa::Contained`, which admits a call argument at a position whose
address the callee does not keep; `retains` by the loan checker and by the inliner's `rootLender`;
`diverges` by `endNonReturningBlocks`. `stack` is the one row with no consumer
yet, and it is §4.4's eligibility test waiting for the construct it gates.

Three things it deliberately does not do, and says so rather than guessing:

- **One plain function per name.** `doc/spec/overloading.md` keys an overload set by `(name, arity)`
  and admits one plain function per key beside any number of class functions; selecting among those
  is milestone 6. A second declaration of one name is refused at its declaration, because the name
  table would otherwise keep the first and every call would silently reach it.
- **An `=` body does not infer its result.** `fn double(x: Int) = x * 2` gets `{}` and reports at its
  first caller, where the shipping stage reads the result off the body. Nothing about `?` needed it -
  the operator's own refusal names `{}` having no `Try` instance, which is true - but it is the one
  place a reader might expect the third arm the other stage's `canLeaveWithCarrier` equivalent has.

The one thing arithmetic does *not* decide is signedness: op.def has no column for it, because
`I32` and `U32` are two logical types over one `div`, and `resolve_ssa/lower/gen.cpp` is where the
two become `idiv` and `div` with the target in hand.

### The width seam, `@bits(n)`, and the division guard

All three are written now, and all three were on the list above until they were.

**A value narrower than its register is held in normal form** - sign-extended where the type is
signed, zero-extended where it is not - and `resolve_ssa/lower/width.cpp` is the one file that
establishes it. `op.def`'s `width` column says which operations can leave the declared range
(`WidthEffect`), and the seam says whether that matters on this target; the split is the same one the
signedness note above makes, from the other side. `(200 :: U8) + 100 == 44` and
`((0 :: WideInt) - 1) shr 1 == 2^52-1` are the two shortest statements of what it buys, and `rol` at
a narrow width is the third - the machine has no rotate instruction there at all, so the shift pair
that is the *definition* of a rotation is emitted instead.

`NarrowNot.yana`, `Rotate.yana`, `WideInt.yana` and `Truncate.Wide.yana` were the fixtures that
measured the gap and are adopted now.

**`@bits(n)` is the other narrow thing, and it is a different one.** A refinement declares how many
bits a value owns of its *storage*, where the seam above is about how many bits a type owns of its
*register* - and `doc/spec/repr.md` states the pair that keeps them apart: **a load always widens and
a store always narrows**, so arithmetic never operates on a narrowed value at all. `addSmall(12)` on
a `@bits(4) U32` is 24, because the addition is the `U32` one.

Four pieces, and each is the one place its half is decided:

- `IntType::canonical` is the whole representation - a distinct type, because the storage differs,
  with a link to what it refines. `canonicalInt` is its only reader, `Types::refinedInt` interns one
  and `Resolver::resolveBitsType` is where what may be refined and by how much is checked.
- **Reading** a refined place is `readPlace`'s last act: what comes out of the storage is the
  canonical integer, so a type variable binds the natural type and `Num` keeps one instance per
  family. `TypeArguments::bind` canonicalizes what an argument supplies for the same reason, and
  `matchType` compares two integer positions through their canonicals so that `instance Num(U64)`
  answers `Num(@bits(53) U64)`. `headKey` files a refinement under what it refines, or the instance
  table would never offer the row for `matchType` to match.
- **Writing** is `convertValue`'s refinement arm - `refinesInteger`, which is a conversion the ladder
  cannot name because a refinement is a type this stage makes. Both directions are implicit; the
  narrowing one is a `convert` node whose result type is the refinement, and the *seam* is what makes
  that mean something: a value put back into its declared width is a mask, or a `shl`/`sar` pair
  where the type is signed.
- A **written literal** is folded in its type's normal form (`foldWritten`, `reducedToWidth`) and
  warned about where it does not fit, which is the one case with no other symptom: nothing in
  `Box {small: 20}` says 4.

Nothing about **packing** is here, and nothing needs to be: two `@bits(20)` fields occupy four bytes
each rather than sharing a word, and every value in `WidePack.yana` still comes out right, because
what the answers depend on is the narrowing at the store and the extension at the load rather than
where the bits sit. That is the property a later co-packing pass has to preserve rather than
establish. `NativeWidth.yana` and `WidePack.yana` are adopted, and they were the last two fixtures on
the width side of the corpus that were not.

**Integer division is total**, and `lower/lower_divide.h`'s `makeDivisionTotal` is what makes it so
on a machine that raises. It is called from `lowerProgram`, per function and immediately behind the
body, and it is a *correctness* pass rather than an optimization: `doc/spec/types.md` says `x / 0`
is 0 and `minimum / -1` is `minimum`, and neither native backend provides either. `Divide.yana` is
the fixture and it is adopted.

One thing the seam decides that the shipping stage's does not: **a narrow signed division wraps**.
`(-128 :: I8) / d` at `d == -1` is `128` in the register the guard's negation leaves it in, and `I8`
answers `-128`. `op.def` gives `Div` the `Wraps` effect for that one pair, which costs a mask on a
narrow *unsigned* division that both backends fold away.

## An overload set is a set

`doc/spec/overloading.md`'s R1 keys an overload set by `(name, arity)` and says what is in it: every
class function of that name and arity, **plus at most one plain function**. This stage held one
`Function` per key and refused everything after it - which reads as a stricter reading of the same
rule and is not one. R1's set is what a *call* selects from, so a table with one entry per key is not
a stricter language; it is a table that cannot express the language at all.

It showed up in `lib/Core`. `Core/Vector.yana` writes `min`, `max` and `abs` as plain functions over
`Vec(a, n)`, beside `Ord`'s and `Signed`'s members of the same names and arities - because a
comparison of two vectors answers a *mask* rather than a `Bool`, so `Ord` is not what describes
ordering them. The same line refused two *classes* declaring one member name, which nothing in `lib/`
happens to do and any two libraries would.

**What the table refuses now is a second *plain* function at a key**, which is the one part of R1
that is a count. What it stopped stating moved to where it is actually visible: one member per
`(name, arity)` within one class, checked in `declareClassMembers`, because the key is what an
instance's members are matched against and a class with two would have matched the first twice and
left the second unimplementable.

**Selection is `resolve_overload.cpp`, and it runs only where a name and arity find more than one
candidate.** A set of one is the ordinary call path, unchanged, and that division is R5's own last
paragraph rather than an optimization: a sole plain function's parameter types are known before its
arguments are and are pushed down as each argument's expected type - which is what lets `f(Nothing)`
know which `Maybe` it is building - and "that happens **only** where the plain function is the whole
overload set". Where a name has something to choose between there is no parameter type to push down,
because which parameter types apply is the question the arguments are being resolved to answer.

So the arguments are resolved **once**, in written order, with no expected type, and the candidates
are matched against what came back. Nothing is resolved twice and nothing is rolled back - a
speculative walk that emitted nodes and then discarded them would leave them in the block it was
building, in a stage that runs no pass to remove a value nothing names.

**What that cost, and how much of it is left.** A set of several is selected from what the
*arguments* answered, so a position's own expected type reaches no candidate - and a call in an
argument whose result the candidate would have decided is then undecidable. `import Native` makes `+`
such a set, because `lib/Native/Pointer.yana` writes a pointer `+` beside `Num`'s;
`truncate(a) + truncate(b)` at a result of `Int` reported "no argument decides what `b` is in this
call to `truncate`", and both stages did.

**The result narrows the set** since 2026-09-07, and by *subtraction* rather than by ranking: a
candidate whose result could not fill the position was never a candidate. `resultCouldFill` asks two
questions and neither is the other - `matchType`, whether the declared result could *be* the position
under some binding of the candidate's own variables, which admits `Num.+`'s `a` at an `Int`; and
`acceptsType`, whether a value of a decided result converts into it, which admits an `I32` answer at
an `I64` position. `Native`'s `+(p: %a, n: Size) -> %a` answers neither, so one candidate is left and
`resolveInfix` takes the sole-callee path - where the hint travels down and decides `truncate`'s own
result. Where the subtraction leaves nothing, or more than one, the set is unchanged.

**One operator, and that is the bound rather than a first step.** The hint is what the *chain's*
value fills, and only a chain of one has an operator whose result is that value: `p + n * 4` at a
`%U8` says nothing about what `*` answers, and a heterogeneous operator is precisely the kind this is
about. `TargetWidth.yana`, `WideInt.yana`, `PackTop.yana` and `ArrayRemove.yana` moved for it -
`0 - 1` written at a `Size` position is now subtracted at `Size` rather than at `I32` and converted,
which is the same answer through one fewer node.

Two things the rules do not state and this had to decide.

**The convention is spent before selection runs.** `resolveValue` takes the position's `Request`, and
`readPlace` reads a name differently under each - a shared read of a local is a borrow and an owning
one is a copy or a move - so the request has to be the *set's* rather than the winner's. A `@lazy`
position is irreconcilable and is refused: it needs the expression unevaluated where every other
position needs it evaluated, and taking one candidate's answer would do neither - the operand would
be produced *and* thunked. Owning over borrowing is not, and that is exactly what
`Ord.min(->lhs, ->rhs)` beside a plain `min(lhs, rhs)` needs - an owning read serves both, because a
borrowing position accepts a value the caller owns and the reverse does not hold. The cost is stated
where it is paid: where they disagree and a borrowing candidate wins, a non-trivial argument has been
moved for nothing and using it afterwards is refused, which is a false rejection with a clear message
rather than a call that quietly hands over what it only borrowed.

**Publishing is reconcilable exactly where R5 is**, and that took a second look. A `&` position needs
storage where an unpublished one needs a value and no reading is both - but where the set holds a
plain function the spec has already said which candidate wins, so its answer is the set's one step
before matching can be asked. Refusing the disagreement outright made declaring a name Core also
declares a mistake, which is the one thing R5 exists to say it is not: `fn widen(&p: Point)` written
beside Core's `Widen.widen` is the shape, and three fixtures in the corpus are it. Between two
*classes* there is no such rule and the disagreement stays refused - R4 answers an ambiguity by
qualifying the call, and no qualified spelling changes how an argument had already been read.

**R4's escape had to be built.** `Bitwise.and(x, y)` is how the spec says an ambiguity is resolved,
and the ambiguity diagnostic hands the author exactly that text - so a class function had to be
reachable as `Class.member`, which it was not. It is split inside the name traversal the way
`findConstructor` splits `Record.Constructor`, by the same `NameRef::range`, and it names a single
candidate by construction.

**The two halves of a set are gathered by two different rules**, and the second is the one that was
wrong. The **plain function** shadows like every other name: the nearest reading that has one is the
set's, a local declaration beats an import, and two imports offering one are ambiguous. The **class
functions** join - every visible reading, deduplicated - because R5's "a plain function joins the
overload set rather than shadowing it" has to hold in both directions. A set taken from the first
reading that answered anything made a declaration in *this* module hide what an import offered, and
`lib/Core` is where that showed: `Num.+` is declared there and `%p + n` is `lib/Native`'s plain `+`,
so every file in Core that imports Native could reach only one of them and `units(text) + at` was
reported as a type error on the argument. `visitReadings` beside `search` in `name.h` is the split -
one walk, two policies about what to do with several answers.

**Operators go through selection**, which they did not. `findOperator` used to need a single callee
before an operand was resolved, and reported a set of several as something this stage did not
implement. What made that unnecessary is that the two questions are separate: the **fixity** is a
lookup of its own, so a chain flattens before which declaration each operator is has been decided,
and `climb` asks the second question with the operand types in hand. `operatorRequests` answers what
the operands are asked for - `agreedShape` at an operator's shape - and `selectOperatorCall` runs
`chooseCandidate`, which is the *same* R2-through-R5 a written call runs, extracted so that
`advance(p, n)` and `p + n` cannot select differently. The one refusal left is an operator whose set
holds a `@lazy` position: the right operand of an infix operator is a span of the chain that
precedence climbing discovers, so there is no AST node to build a thunk over, and the message says to
write it as a call.

**A `for` loop's traversal goes through selection too** (2026-09-07, J26). `namedCallee` answers a
declaration and answers none where the set has several, which is R1's rule and right for every other
reader of it - and a `for` is the position where "several" is the question the arguments answer.
`iteratorCallee` is the second attempt: it narrows the set to the **iterators**
(`CandidateKind::Iterator`, the filter `selectOverload` takes) and, where more than one is left, puts
it through the same `chooseCandidate` a written call and an operator go through. So a plain `iter fn`
declared beside a class member is a *member* of the set rather than a shadow over it, which is what
`Iter.Overload.yana`, `Iter.Shape.yana` and `Iter.Strictness.yana` are each one half of.

The seam that made it one rule rather than two is a **split**: `selectOverload` is
`resolveOverloadedCall`'s first four steps and carries the values it resolved out in an
`OverloadChoice`, and `prepareResolvedArguments` is `finishOverloadedCall`'s preparation loop. A
`for` runs the first and then hands the winner *and its arguments* to `handoverCall`, which opens a
continuation region instead of emitting a call. Resolving the arguments a second time there would
evaluate them twice, which is why the choice carries them rather than only the declaration.

Two things about it are deliberately narrow. `checkCallable` refuses a lens in a value position and
is asked by `chooseCandidate`, so it takes a `traversal` flag: at a `for`, a lens is exactly what was
wanted. And the **dot** spelling of a set with several iterators is refused rather than selected - a
receiver is argument zero of the mapping and is resolved before a candidate is known, so selecting
one needs the receiver resolved in `iteratorCallee` and `handoverCall` then not resolving it again,
which is a second seam through the same walk for a spelling no candidate needs today. `xs.chunks()`
reaches the one-iterator path unchanged.

**A literal operand is not built until a candidate has typed it.** R5 pushes a position's type down
only where a plain function is the whole set, so an untyped number standing in a set of several has
nothing to take - and built at its default it fits nothing: `48 + rest` and `values(buffer) + at` in
`lib/Core` offered `I32` to `Native`'s pointer `+` and to `Num.+` alike and were reported as "no `+`
accepts these arguments", eight times. A literal *is* a `FromInt` call at a type to be decided, so
what decides it is the position it fills. `undecidedLiteral` marks such an operand, the walk leaves
it unresolved exactly as it leaves a `@lazy` one, and `candidateAccepts` asks about it in a second
pass once every other argument has bound: a candidate is viable when the position is fixed and a
conversion instance exists there, or when it is fixed at a variable of the *enclosing* signature
whose context requires the conversion class - `0.5 + r` inside a body constrained on `Real(a)` builds
the half at `a`. A position nothing at all decided takes the literal's own default, which is what
`1 - 1023 - 52 - 2` needs. Exactly one viable candidate selects and several is the ambiguity R2
already reports, so there is no rule of its own here. The winner's position is where the number is
finally built - through `resolveValue`, since what was left standing is the written expression and a
negation folds its sign one level down. `OperatorLiteral.yana`.

What this does **not** yet reach is a literal left operand of a *sole* callee whose first parameter
is open: `0.5 + r` where `+` names only `Num.+` still builds the half at the decimal default, because
`climb` sends the outer hint to the left operand and resolves it before the right one. Deferring that
one the same way is sound and closes eight `lib/Math` sites, and it moves the constant after the
other operand in sixty existing goldens - `doc/progress/2026-09-06-J8-inference.md` has the
measurement.

A hint stops at an overloaded operator for R5's reason one syntax out, and stops at a **comparison**
for one of its own: the position's type reaches a chain's leftmost operand only where every operator
in it answers with its own left operand's declared type. `1 + 2 == 3` in a `Bool` position climbs
into `+` first and `+` does relate its result to its operand - but what the position asks for is
`==`'s answer, so the `Bool` travelled down and built `1` at it, and there is no `FromInt(Bool)` in
any library.

`test/resolve/Overload.yana` asserts each rule by a **number**: every call in it reaches one of two
bodies that answer different values, so a selection rule that broke would change what the program
computes rather than whether it compiles, and both resolvers run it against one `.run.expect`.
`Reject.Overload.yana` is the refusing half - R2's "an error rather than a contest", R3's own example
from the spec, and the one set that cannot be resolved whichever candidate would win, which is two
classes disagreeing about publishing with no plain function to decide it. `module/Overload.yana`
asserts the set crossing a module boundary in both directions and is the traversal note above - it
fails against a set recorded rather than gathered - and `OperatorOverload.yana` asserts the same five
rules at an infix and a prefix operator.

**Selecting the same way is not the whole of being the same call**, and the rest of it is the two
steps a written call takes around selection:

- *The normalized call.* `Program::addFunctionName` files a declaration under every arity its
  defaults admit, so a name found at two operands may declare three positions - and the third is
  filled from the parameter's own constant, which is what `positionalMap` and `fillDefaultArguments`
  do for an operator. `mapArguments` is the same answer for a *written* list; what it adds is names,
  which an operator does not write, so an operator's map is the identity and its candidates need no
  shape check at all. `callFunction` is that whole tail - prepare, default, finish - and is what a
  prefix operator and an overloaded one call rather than filling positions themselves. An infix
  operator is the one caller that cannot: its right operand is resolved *between* the two prepares,
  because what the right operand is expected to be is what the left one bound.
- *Whether the callee is one a call may reach.* `checkCallable` - a declaration with no body, and a
  lens, which needs a continuation no expression has. Asked where a set settles on a declaration
  rather than at each caller: `resolveCall` for a name that reached one, `chooseCandidate` for a
  winner, `findOperator` and `resolvePrefix` for an operator's. Three of those four used to reach a
  lens and find out much later - as a continuation type nothing decided, or as an argument count the
  builder asserted on. It is also where an **inferred result** is demanded - see below.

## `@layout(c)`, and where a layout is answered

`readLayoutAttribute` reads the pin, `RecordType::pin` holds it, and `Types::recordType` copies it
into every instantiation. Beyond that this directory has nothing to say about bytes: a pin is a
statement about the *type*, and what a pinned record's bytes are is a target's question.

That question is answered in `compiler/resolve_ssa/lower/repr.h`, per family of machine rather than
per target - one table for a machine whose values are bytes, and a different type (`JsRep`) for one
whose values are host values. The three things that stay above every such table live here, and this
is why:

- **the key** is `mir::Occurrence`, so that no family can select a manifestation from less than the
  logical type, the contract and the interior shape;
- **what the language decides** is on the logical type - `RecordType::pin`, `RecordType::layout`, the
  view table, the containment cut - so that every family reads one statement rather than each
  re-deriving one;
- **what a value costs** is not a layout question and has no field on a layout target.

What a pin *does* is decline all four of the things `Auto` licenses: reordering, co-packing,
whole-record scalarization and niche folding. Each of those is one test of `RecordType::pinned()` in
`resolve_ssa/lower/repr.cpp`, and they read the pin rather than the two names apart because what `C`
and `Js` have in common is exactly "the compiler does not choose". Where they differ is the one
place a pinned record still packs: under `C` a written `@bits(n)` refinement *is* a bit-field,
allocated in its declared type's unit and sharing that unit only with a bit-field written next to
it, which is what a C compiler does with `int x: 4`.

Analysis-Manifestations.md part 10 is the argument, and `test/resolve/LayoutPin.yana` is the
assertion: its two `.repr.expect` goldens - the shipping stage's and this one's - are byte-identical,
and so are `Repr.yana`'s and `PackedNested.yana`'s.

## `@box`, and where the indirection is

`RecordField::boxed` is the flag, and two things set it: `@box` written on a field, and
`breakLayoutCycles` in `own.cpp` at the back edge of every inline-containment cycle. `doc/spec/repr.md`
keeps those apart on purpose and this directory does too - one is a declaration, the other is what
makes a recursive `data` have a layout at all - but they mean the same thing about the field and
nothing below reads them apart.

**The indirection is not in the MIR.** `fieldPlace` hands back the same owned projection an inline
field gets, `buildPaths` builds a move path over it, `placeDrops` places a drop on it and the loan
checker sees a field of a record. This is the one thing the stage does differently from the shipping
one, which appends a `Deref` to the place path and carries the boxed edge through every place walk in
the compiler - seven of them, each of which has to agree. Here the edge is a **layout** fact and
`compiler/resolve_ssa/lower/box.cpp` is the whole of it: three operations, one file.

What that buys is that partial ownership needed no work. `p.cold` has a state of its own, moving one
field out leaves the others owed, and none of it knows there is a pointer in the way.

What it costs is that the two things a box is - the value, and the allocation holding it - are one
place in this representation. Which is why:

**Handing the value of a boxed field over on its own is refused.** `let ->g = c.cold` moves the `G`
and leaves `c` holding an allocation whose contents are gone, and a path is `Moved` or it is not - a
moved one places no drop, so the allocation leaks with nothing anywhere saying so. `analyze_drop.cpp`
reports it at the take. The fix is a `DropStep` that releases storage without running a teardown,
placed on the field whose value left; that is a real feature and this is the message that stands in
for it. A **copy** out of a boxed field is unaffected and is the ordinary case.

**A constant cannot hold one.** `materialize` writes bytes and no relocations, so a box in static
data would put the target's bytes where the layout says a pointer is. `constantNeedsAllocation` is
the question and a global whose initializer answers it becomes `dynamic` - initialized by the entry
sequence, exactly as `Constant::Text` and `let m = Map()` already are.

**A `write.init` into a boxed field place is not built.** Every `Destination::Init` this stage emits
targets a local or a pattern binding, so a construction reaches a box through `Inst::Aggregate` and
nowhere else. If one ever does, `box.cpp`'s `createBox` is what it wants.

**Where the value inside a box is built is the lowering's, not this stage's.** `Inst::Aggregate` is a
value-producing instruction here and carries no storage, which is the tier boundary working: a
component whose producer could write straight into the box is a question about addresses, and
`resolve_ssa/lower`'s `storageFor` is where it is asked. What this stage owes it is the shape it
already has - one consumer per value, and the consume written on the operand.

**Whether a boxed field carries a provenance bit is settled here**, at the end of `resolveProgram`.
`settleBoxProvenance` asks whether any *reachable* body releases a boxed value through a borrow, a
global or a raw address - the one kind of site that reads the bit - and `Program::asksBoxProvenance`
carries the answer to `ssa::storesBoxProvenance`. It is here and not in the lowering because a `Repr`
is memoized on the record it describes, so the answer has to be a constant before the first record is
laid out; and it is about the reachable bodies rather than the declarations because every library
holds a function that overwrites through a `&`. `boxesVisibleAt` in `place.h` is the one question,
asked by this pass over the whole program and by the conversion over one drop.

### Which types the cut is looked for in

`breakLayoutCycles` walks in two sweeps, and the second one is not an optimization. The first roots
at the **written declarations** in declaration order, which is what decides where a mutually
recursive pair's pointer goes and what makes `Maybe(Tree)`'s cut land at the payload rather than at
`Branch.left`. The second roots at every **instantiation the first did not reach**.

There has to be a second, because an instantiation is reached by the first sweep only if some type
*contains* it - and `List(I32)` in `fn count(s: List(I32)) -> Int` is contained by nothing. It exists
because a signature named it. A stage with only the first sweep has no generic recursive types at
all: `data List(a)` compiles, and the first function that takes one reports that `List(I32)` contains
itself.

The order between the sweeps is what makes the second one safe to write as "everything left over":
by the time it runs, everything a declaration reaches is already `layoutBroken` and stops at its
first line, so what it cuts is exactly what nothing else could have. The shipping stage arranges the
same thing differently - `completeInstance` breaks each instantiation as it is filled - and pays for
it with a walk that has to notice it ran while a declaration was still incomplete.

**What is left after both sweeps is still a diagnostic**, and it is now a narrow one: a cycle whose
every edge is one no pointer can go in. A fixed array is the way to write one - `[T *n]` is `n` of
the type at a stride and has no slot an address could live in - and `ownership/Reject.Recursive.yana`
is the fixture. `Classifier::ownership` descends through a fixed array's element for that reason: a
`[T *n]` member's own answer is folded from `T`'s by `ownershipOf`, which has no stack to notice that
`T` is the record the fold is already inside.
