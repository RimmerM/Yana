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
| `resolve_stmt.cpp` | the things that do: statements, assignment, `while`, `return`, lens calls, bodies |

`resolveProgram`'s order is the one thing that has to be read as a whole and it is in `resolve.cpp`:
`declareTypes`, `declareClasses`, `synthesizeCoreClasses`, `declareRecordFields`, `foldSchemas`,
`declareClassMembers`, `declare`, `classifyTypes`, `resolveInitializers`, `resolveBodies`,
`freezeSchemas`, `verifyProgram`, `analyzeProgram`, `computeSummaries`.

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
comparison, `Bool`, locals, `if` as a value and as a statement, `while`, blocks, block arguments,
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

Three requirement forms are still refused by name - a **structural field** `a.field: b`, a
**constrained callable** `f: (a) -> b`, and a **const parameter** `n: Int` - and so is the `data` and
`class` head spelling of the last. All four want the same missing thing: a witness kind and a table
to load it out of, which is waypoint G5's erased ABI. Everything above them works because
specialization is the only lowering, so the *value* a body needs is always substituted rather than
passed. A **qualified alias** is refused for a different reason - it is a type rather than a
spelling, and needs conversions and `deriving` - and a **lens class member** for a third: a class
member is generic by construction, and a lens and a generic in one call is the refusal
`resolveLensCall` already argues.

**Linear types**, which are this stage's alone: `@explicitDrop` withholds the compiler's permission
to insert a teardown, `->x` writes a disposal down, and `@local` says who may write one. See
[The Drop, Reclaim and Copy classes](#the-drop-reclaim-and-copy-classes) below and
`doc/proposal/Design-Linear-Types.md`, whose build order this implements the first four steps of.
The shipping stage refuses `->x` by name and accepts both attributes without acting on them, so a
fixture about any of this carries `# resolver: ownership`.
There is no prelude - the width names and `Bool` are builtin. Anything else in a source file is reported by name as not implemented, because this stage is
chosen by a flag over a corpus written for the other one and meeting an unimplemented construction is
an ordinary event.

**`Int` and `Long` are temporary builtins here** (`Types::findBuiltin`), spelled the way
`lib/Core/Core.yana` spells them - `alias Int = I32`, `alias Long = I64`. They exist for the fixture
corpus, most of which writes its integers that way, and they go when milestone 6 makes `alias` a
declaration this stage resolves. An alias is not a type of its own, so nothing downstream can tell
which spelling was written and every golden says `I32`.

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

### Records, and what a type owes

A `data` declaration is a `RecordType`: a nominal type with a name, a module and an ordered field
list. Two declarations of one shape are two types, which is what nominal means and is why the
identity is the declaration rather than the field list.

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

### The `Drop`, `Reclaim` and `Copy` classes

These three are **declarations too**, and that is what G3 changed about them. `class Drop(a)` is
synthesized where nothing declares it - this stage does not build Core - so `instance Drop(T)` is
checked against a class by the one rule every instance is checked by, and the hand-written shape test
per enumerator that used to do it is gone. `CoreClass` survives as a *tag* on the declaration, which
is what the ownership classifier keys off; when this stage reads Core, the loop that recognizes the
three by name finds real declarations and synthesizes nothing.

A synthesized class belongs to no module, which is what makes it visible from every one: `Drop` is
written in the module that declares the type, and there may be several. `lookupClass` and
`lookupCoreMember` are the two places that say so, and both are standing in for an ordinary import.

`Drop` and `Reclaim` differ from `Copy` in exactly one way that is not about ownership: their members
are **not** callable by name. A teardown runs because a value died, so there is no call to write, and
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

### Continuations, and what flattening buys

Part 11.4's slice 7, in its first shape: a transparent `lens fn` in the `yield` form, called in the
scoped form (`let x = f(...) in: ...`). `resolveLensCall` resolves the callee's body *into* the
caller, so what the stage builds is the inlined lowering and only that.

**That is a choice this stage currently makes unconditionally, and `doc/spec/lenses.md` says it is a
choice.** The spec's two lowerings are the inlined one and the CPS one - the callee compiled once,
taking the continuation as a final argument - and which a call gets is per call site, under the
ordinary inlining budget, because inlining copies the body into every site. §10.1's requirement is
weaker than the flattening: a continuation's exits have to be **edges in the CFG** the loan,
liveness and drop passes walk, and the continuation is the *caller's* code either way, so its region
and its exits are in the caller under both lowerings. What flattening adds is that the callee's
frame storage becomes visible too, which is why the entry below about drop placement is a defect
rather than a feature. The CPS form is W7's, it is not built, and until it is, a lens is refused
wherever it cannot be flattened.

What the CFG then holds is one function with two named scopes inside it, which the graph cannot
state on its own - `mir::Continuation`, mirroring `ssa::Continuation` and now its producer:

| Region | Whose blocks | Entered at | Rejoins at |
| --- | --- | --- | --- |
| `Body` | the function's own | the entry | nothing; it returns |
| `Frame` | the flattened callee's | the block after the arguments are bound | the join after the call |
| `Handover` | the caller's continuation | the `yield`'s successor | the cleanup after the `yield` |

Nesting is what makes a lens inside another's continuation work with no rule of its own: the inner
call's `Frame` is opened inside the outer call's `Handover`, so the cleanups run in the order the
regions close. The nesting rule itself - an edge stays in a region, enters a *direct child at its
entry*, or leaves for the destination that child declared - is checked in `verify.cpp` and again in
`ssa::verifyBody`, and both were written for `Cleanup` before there was a second kind.

Three things fall out of flattening rather than being implemented:

- **A lens's frame storage is ordinary storage, released when its frame is left.**
  `test/resolve/lens/Frame.yana`'s guard is a local of the caller's frame here, so the ordinary drop
  elaboration places it - but *not* at its own last use, which is the one thing flattening may not
  decide. `Analysis::keepFrameLocals` holds it live through the frame's region and lets it die at
  the instruction that leaves, on every way out, which is where the CPS lowering would release it:
  after everything the callee wrote, including the cleanup after its `yield`. The two lowerings have
  to be observationally identical (`doc/spec/lenses.md`), an authored `Drop` is what would otherwise
  tell them apart, and `Frame.yana`'s `ordering` counts the one observation that can - a multiplying
  cleanup, since every other reading in that file adds. Analysis-Lens.md §11's complaint that a lens
  frame outlives its reason is answered by the *extent*, not by releasing the storage early.
- **A borrow handed over is checked against the storage it is really rooted in.** The declared
  hand-over contract - `Contract`'s `Origin::CallBounded`, which is what an unqualified `-> T` on a
  `lens fn` means - is kept on the region rather than used for checking, because it is what an
  *opaque* boundary would have instead of the storage.
- **There is one argument loop.** `resolveArguments` is shared by the ordinary call and the lens
  call, which is the thing Analysis-Lens.md §12.1's sixth item asks for by name: the shipping stage
  has two, and rules added to one of them are missing from the other.

The costs are real and are not hidden. A lens body is resolved once per call site, so a diagnostic
inside one is reported once per call site; and a lens nothing calls is never checked at all, which is
why `test/resolve/lens/Reject.Scoped.yana` calls every lens it is about. Both end at the opaque
boundary, which is what gives a lens a body of its own.

`checkFrameYield` is the transparent rule (Analysis-Lens.md §7.1) as a forward walk over the frame's
blocks rather than a count of `yield` statements, because both halves of it are about paths: handing
over on one arm of an `if` and not the other is a *skipping* lens, which needs the `Try` instance
milestone 6 has, and handing over twice would run one continuation twice inside one frame.

### Leaving a continuation, and why each exit carries its own cleanup

§10.1 gives a continuation four exits - falling out, `return`, `break` and `continue` - and two of
them are built. Falling out resumes at the region's destination; a `return` written inside a
continuation leaves the enclosing *function*, past every open frame, running each one's cleanup on
the way. That is the whole reason a scoped-acquisition API is correct on an early return with nothing
written at either site, and with the lens flattened it is one function and one `return`, so what the
exit needs beyond an ordinary one is only the cleanups.

**Each exit resolves its own copy of them** - `runCleanup`, from the AST, at the exit. The
alternative is one shared copy ending in a branch on which exit arrived, and it fails for a reason
worth stating: the returned value would have to survive the cleanup in storage that is initialized on
one incoming edge and read on the merge of both, and the correlation that makes that safe is
invisible to every pass in this stage. That is §1.2's failure mode exactly - a fact the
representation knows and the checker cannot see - so the duplication is the cheaper of the two. What
it costs is code and diagnostics proportional to the exits the continuation actually writes, on the
same terms flattening already pays them; the opaque boundary takes the other trade, running one
cleanup and propagating an exit code, which is what Analysis-Lens.md §5.1's `Outcome` is for.

The cleanup copy is emitted in the region the exit is written in rather than in the frame's, and that
is what keeps the nesting rule unchanged: a `return` has no successors, so nothing leaves a region by
an edge and `destination` stays the single fall-out answer it was. `Resolver::tails` is what makes a
copy possible at all - `resolveBlock` records where each statement sequence has got to, so a `yield`
suspends a walk that an exit can resume from.

Which frames an exit unwinds is `LensFrame::Stage`, and it is a fact about the *path* rather than
about how far the walk has gone. A frame owes its cleanup only while the continuation is being
resolved, so the stage is set for the length of that and put back afterwards: latching it would make
`if c: yield 1 else: yield 2` - one hand-over on every path - look like a second hand-over in the
second arm. Two consequences worth having in fixtures, and both are: a `return` written in the lens's
own cleanup skips the rest of it exactly as a `return` skips the rest of any block, and a `return`
written inside a cleanup *copy* leaves past the frames outside it rather than past those the path has
already entered.

The **escape rule** needed nothing built. A lens's frame storage is the caller's storage once the
call is flattened, so a hand-over that leaves through a `return` is an ordinary borrow of a local
escaping the frame it is rooted in, and `analyze_loan.cpp` reports it at the return in the words it
already has - `test/resolve/lens/Reject.Escape.yana`, whose message names the *callee's* own local,
because that is where the storage really is. Analysis-Lens.md §12.3(c)'s boundary refusal is the
other half of this and is not here: it is about an unqualified `%T` at a *signature*, and this stage
has no pointer operations yet.

`Builder::seal` gained `pruneUnreachable` for the same waypoint, and it is not tidying. A
continuation every path of which leaves is a hand-over nothing resumes from, so the frame's resume
and the caller's join become unreachable at exactly that call site - which the old resolver never
produced, and which nothing downstream is prepared for: the x64 block ordering asserts that its
postorder covers the function, and an unreachable block is a hole in every dense per-block analysis.

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

Three constructs make blocks and all three carry: the value `if` and the short-circuit operators
(which share `branchValue`), the statement `if`, and `while`. A loop needs it in one more place than
a branch does - the header is entered twice, so the back edge refills the positions the entry
filled.

Only the first of the three can have anything to carry *today*, because a statement cannot stand in
a value position in this stage yet - `let ... in` as an expression is refused, and there is no other
way to put a statement `if` or a loop between a value and its use. The other two carry anyway,
because the day one of them can be reached is not the day to discover that two of the three sites
that build blocks were written to a different rule. `BranchCarry.yana` is the fixture, and it is
about the reachable one.

Two things follow that are worth stating. **A branch's two edges take one argument list**, because a
value live before a branch is live on both sides of it; what differs between the arms is what they
*produce*, and that travels the other way. And **an owned value carried across a branch is handed
over on the edges**, under `argumentMode` like every other position that fills a declared slot - so
the frame that had it is the frame that releases it, once, and an arm that leaves without reaching
the merge drops it where it stands.

What still does not carry is a lens call: flattening a callee's body into its caller opens blocks in
several places, and `resolveLensCall` refuses a generic for a related reason. A value held across one
is caught by `checkValueScope` as an internal error rather than miscompiled.

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
- **Leaving a lens through `return` is refused by name.** Falling out is one of §10.1's four exits
  and the other three - `return`, `break`, `continue` - each need their own edge out of the region
  and drop elaboration on it. A `return` that skipped the frames' cleanups would release nothing and
  say nothing, so it is reported rather than approximated.
- **A lens call carries no sigil.** `let &x = f(...) in:` is refused naming the declaration, because
  what the continuation receives under is the callee's `-> &T` or `-> ->T` - part of its published
  interface, per Analysis-Language.md §3a - and a second spelling at the call site could disagree
  with it.
- **A requirement on a generic declaration is refused by name, one refusal per form that has no
  witness.** `a.field: b`, `f: (a) -> b` and a const parameter each need a slot kind to be numbered
  as and a table to be discharged against; a constraint that parsed and was dropped would be a body
  dispatching on nothing and a caller proving nothing. A **class** constraint has both as of G3 and
  compiles. So is refused a *constrained* `data` head, which is a second discharge site, and a **lens
  and a generic in one call**, in either direction: flattening
  resolves the callee's body inside the caller's walk and `Resolver::genContext` says which
  declaration a written type is read in, so a generic callee would need a second substitution
  mechanism beside `specializeFunction` and a generic caller would leave its own variables in scope
  while the callee's body is read. The milestone-7 rebuild ends both by making a lens an ordinary
  call, which has one context at each end.
- **A class is one parameter, an instance is one logical type, and a member binds nothing of its
  own.** Selection is a lookup keyed by `(class, type)`, so a second class parameter needs a match
  above the table, an instance head mentioning a variable would be an entry no lookup could find, and
  a member with a type variable the class did not bind would be a signature no instance could supply.
  Each is refused by name.
- **An instance member's signature is not inferred from the class.** `fn size(value) = value.value`
  inside `instance Describe(Feet)` is what the class already fixed, and taking it from there is a
  real feature this stage does not have yet - `Class.yana` in the shared corpus uses it, which is one
  of the two reasons that fixture cannot be adopted. The other is `Int` and `Eq`, which come from
  Core.
- **A type argument no argument decides is refused rather than guessed.** `fn empty() -> Maybe(a)`
  is decided by the expected type at the call site, which is the same rule that picks a class
  instance by its result type and needs the position's hint to reach a call.
- **Specialization that does not terminate is reported where it is visible.** A call chain that
  specializes at `a`, then `Boxed(a)`, then `Boxed(Boxed(a))` has no fixed set of instantiations, so
  `callTarget` bounds the argument nesting depth and reports. It is a diagnostic about the *choice of
  lowering* rather than about the program - the erased ABI is what compiles it - which is why the
  message says what it says.

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
