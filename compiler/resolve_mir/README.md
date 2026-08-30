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
| `resolve_internal.h` | the `Resolver` struct itself, and the operator table |
| `resolve.cpp` | the driver, written types, and constants |
| `resolve_decl.cpp` | declarations: types, fields, globals, signatures, instances |
| `resolve_expr.cpp` | the things that answer: places, reads, calls, operators, `if` |
| `resolve_stmt.cpp` | the things that do: statements, assignment, `while`, `return`, bodies |

`resolveProgram`'s order is the one thing that has to be read as a whole and it is in `resolve.cpp`:
`declareTypes`, `declare`, `classifyTypes`, `resolveInitializers`, `resolveBodies`, `verifyProgram`,
`analyzeProgram`, `computeSummaries`. Classification is between declarations and bodies because a
body asks what a type owes and every `instance` has to be known before it can be answered.

## Milestone

Part 11.4's slices 1 to 5. A module of `let` constants, `data` declarations, `instance Drop`/
`instance Reclaim` declarations and functions, whose bodies are scalar computation, control flow,
calls, records, ownership and loans: literals, arithmetic, comparison, `Bool`, locals, `if` as a
value and as a statement, `while`, blocks, block arguments, record construction and projection,
moves, partial moves, the teardowns all of that implies, and borrowing - `&` parameters and
bindings, published origin slots, and record fields that refer to storage the record does not own.
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
| `mustConsume` | must it reach a consumer the author wrote? |
| `drop` | is there an effect at the end of its life, and is it authored or derived? |
| `reclaim` | is there storage to release, and is it authored or derived? |

Five, because every pair of them agrees on every type that exists today and the first type where two
disagree should change one field rather than split a flag half the compiler is reading. `relocatable`
is not `!trivialCopy` - a copy makes a *second* object and a relocation makes the *same* object live
somewhere else - and `mustConsume` is not a `TeardownKind`, because it is the statement that there is
nothing to run and the value is still linear.

Three questions are asked *of* those fields and each has one home:

| Question | Function | Read by |
| --- | --- | --- |
| may a duplicate be made here? | `isTriviallyCopyable` | `readMode`, `verifyBody`'s `CopyTrivial` check |
| may the object travel? | `isRelocatable` | `verifyBody`'s `Take` check - the relocation sites |
| is this linear? | `isLinear` | `ssa::Conversion::conventionFor` |
| is its initialization followed? | `isTracked` | move paths, and whether a value gets an `Object` |

`conventionFor` used to ask `hasTeardown`, which is a fourth question that happens to have the same
answer for every type that exists - and the wrong one for the type the obligation is *for*. A
`@mustConsume` value has no teardown by construction, so keying the linear rule off the teardown made
exactly the values that must be consumed exactly once into `Trivial` ones a pass may duplicate and
discard. `OssaTester.cpp`'s "a type with an obligation and no teardown" is what says it does not.

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

### The `Drop` and `Reclaim` classes

`CoreClass` is two builtin classes and an `Instance` table keyed by `(class, concrete type)`. This is
milestone 6's instance selection with everything hard removed: no `class` declaration is parsed, no
generic head is accepted, no witness is passed. What it buys is the one thing milestone 4 cannot be
tested without - a teardown a fixture can *observe*, since a derived teardown of trivial members does
nothing a program can see. Milestone 6 generalizes this table rather than replacing it.

`Function::teardown` marks the member of such an instance, and it exists for a single reason: the
subject of a teardown is the one owned local in the language this frame does not owe a teardown for.
Without the flag, `instance Drop(Guard)`'s `->value: Guard` would be released by the ordinary rule -
which is a call to the function the body is in, on every path through it.

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

`originsOfOperand` is how all three passes ask - the environment, liveness and the extent walk - and
it answers for a *place* operand as well as a value one. `idOf(d)` names a value and `return d.n`
names a place, and a rule that saw only the first ended the loan behind `d` between them. It takes
the node the operand is read at, and that is not bookkeeping: storage answers differently at
different points, and a caller with no point to name would be asking for the union again.

**Storage is flow-sensitive, and a replacing write is where that is decided.** `replacedLocal` is the
one condition: a write to a whole local this frame *owns* discards what was there, so the forward
transfer replaces the local's origin set rather than adding to it, and the backward extent walk stops
counting that destination as a use. Refilling a reader from a second lender therefore ends the first
lender's loan at the assignment - `test/resolve/loans/Refill.yana`. A partial write and a write
*through* a loan are neither: `r.at = 3` reaches its value through `r`, and `d = box(2)` on a `&`
binding reaches `b` through `d`, so both remain uses and both union - `writeThroughKeepsLender` in
`Reject.Overlap.yana` is the half that says so.

The axis still collapsed is the other one: a record's origins are one set for the whole value rather
than one per schema slot, so reading one external field keeps the other's loan live. §5.11's model is
`program point x slot -> origin set` and this is one of the two halves; `separateLenders` in
`Reject.Overlap.yana` is where the approximation is written down.

**Liveness reads the environment too.** A use of a value is a use of the storage behind every loan it
may be rooted in, which is what keeps a lender alive across the call that borrowed it. Milestone 4's
interim rule - a use of a borrowing read's result is a use of that read's place - was correct for
exactly the one-instruction case and is gone.

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

The origin set of a record is **one set for the whole record** rather than one per schema slot.
§2.2 is explicit that grouping "does not claim that the members are one object", so the conservative
answer is the honest one until there is a reason for the precise one; the cost is that a write to one
lender is refused while a loan of another is live in the same record, which
`test/resolve/loans/Reject.Overlap.yana` asserts by name so that splitting the sets later has a
golden that says what changed.

Two things are refused by name rather than approximated, because the language cannot yet say them: an
*owned* field whose type has a schema, and an owned result that carries origins. Both need a loan
argument on a type application - `data Outer {inner: Reader(a')}` - which the parser has a kind for
and `resolveType` does not read.

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
