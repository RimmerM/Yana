# `resolve_ssa` - ownership SSA, and the way down to the shared lower IR

The back half of the rewritten resolve stage. `resolve_mir` produces checked MIR; this converts it to
ownership SSA, verifies that, and lowers it to the `LowerModule` both stages hand to the backends.

The design is [`doc/inflight/Analysis-IR.md`](../../doc/inflight/Analysis-IR.md) part 7, and part 4's
diagram is the order these files run in. What it calls OSSA is what `ssa::` is here - see
`resolve_mir/README.md` for why the code drops the `O`.

## The whole ownership model at this tier

Every value has one of five conventions and every use has one of three kinds. That is all of it:

- an `Owned` value is consumed exactly once on every reachable path, including by a drop;
- an `Affine` value is consumed **at most** once: it may not be duplicated, and it may be abandoned;
- a `Guaranteed` value may not outlive the loan it depends on;
- a `Trivial` value is unrestricted;
- an `Address` is not an owner at all.

**Five, where §7.1's table says four.** The table folds two independent questions - may this be
duplicated, must this be consumed - onto one axis, and `mir::Ownership` is the standing note that
they are not one axis. A type with an authored `Copy` and no teardown answers no to both: nothing
runs when a `Counter` dies, and making a second one is the author's function rather than the bytes.
The fold sent it to `Trivial`, whose contract is *freely duplicable*, so the one rule the instance
exists to state was the one rule this tier did not carry and an optimizer could give the value a
second use with the verifier's blessing. `Owned` is the other error and a louder one: it demands a
consumption the value does not owe and reports a leak at every exit it reaches. `Affine` shares
`Owned`'s entire dataflow - same lattice, same table, same transfer - and differs at exactly the two
comparisons where the word "exactly" appears.

That surface is small enough that an optimizer pass can be *required* to preserve it rather than
asked to, which is the reason for the tier. `verify.cpp` checks it after conversion, and `opt/`'s
`runPass` checkpoint runs it again after every pass in an assertion build - so a pass that breaks the
rule is named where it broke it rather than found later as a double free.

**The linear rule is checked as the CFG dataflow it is, not as a whole-body count.** That is not a
refinement, it is the difference between a check and a wrong answer: a count is not a weaker form of
the path rule but a different rule that fails in both directions. An owned value consumed once in
each arm of an `if` is correct and counts two, so a count rejects it; one consumed in a single arm is
a leak on the other path and counts one, so a count accepts it. There is therefore no version of the
count that could have served as an interim check until the first `Owned` value arrived, which is why
the dataflow - reverse postorder, dominance-scoped, over a three-state lattice - is here now.

**A `Guaranteed` value does not carry a loan identity, and that is a decision rather than a gap.**
It used to hold a `LoanId`, on the argument that "may not outlive the loan it depends on" is a rule
with no term to check against - which is true, and is the shape lenses, yielded borrows, values live
across a suspension and external iterator handles all need. What made the field unusable is §8.8: an
OMIR loan id is dense within one MIR body, and that region is released after conversion, so a value
holding one would name a table that no longer exists. When this tier needs loan identity it gets a
table of its own, minted by the conversion out of what the loan analysis already proved; until then
the field is absent rather than filled with an index nothing may follow. Analysis-IR.md §13.2 asks
whether a loan is an OMIR or an OSSA concept, and this is the answer: OMIR's, so far.

## Where each decision lives, exactly once

| Question | Answered in |
| --- | --- |
| what a MIR occurrence's convention is | `convert.cpp`, `conventionFor` |
| how a body is built, and how one is rewritten | `build.cpp`, `Editor` |
| whether an owned value is linear | `resolve_mir/type.cpp`, `isLinear` |
| whether a type has anything to run when it dies | `resolve_mir/type.cpp`, `hasTeardown` |
| what a place's address is | `convert.cpp`, `addressOf` |
| what a *value's* address is | `mir::Node::Address` one tier up, a `bitcast` here |
| any of `TypeTraits(a)`'s five measurements | `Inst::Metric`, over `mir`'s `metric.def` |
| what a system call or a block move lowers to | `Inst::Native`, over `mir`'s `native.def` |
| what a borrow depends on | `convert.cpp`, `attachLoan`, from the origin environment |
| how wide one **type** is, and where its fields sit | `lower/repr.cpp`, `reprOf` |
| what a value looks like on the target | `lower/manifest.cpp` |
| whether a component has an address at all | `lower/repr.h`, `FieldRepr::isPacked` and `Repr::discriminant` |
| how a constant becomes bytes | `lower/gen.cpp`, `materialize` |
| what order a body's blocks are read in | `dominance.cpp`, `reversePostorder` |
| which passes run over a body, in what order, and how many times | `opt/opt.cpp` |
| whether an edge may be moved, merged away or threaded | `opt/cfg.cpp` - one region, and never a region's anchor |
| whether a branch may become a `select`, and what one may carry | `opt/select.cpp`, `speculatable` and `selectableJoin` |
| whether a loop may be decided false, and what makes it terminate | `opt/loop.cpp`, `removableStep` and `stepsToTheBound` |
| whether two addresses name the same storage, and whether they may alias | `opt/memory.cpp`, `sameAddress` and `mayAlias` |
| whether a call is inlined | `opt/inline.cpp` - `clonableDeclaration`/`clonableBody` for what a copy can reproduce, `worthInlining` for what it is worth |
| which functions the lowering emits | `mir::Function::reachable`, corrected over SSA by `opt/reach.cpp` |
| whether an instruction may write an allocation's storage | `opt/analysis.cpp`, `mayWriteStorage`, over `Contained` |
| what width an integer folds at, and which types have one | `opt/fold.cpp`, `foldableInt` |
| what an operation over constants comes to, and which identities hold | `opt/fold.cpp` |
| the two's-complement arithmetic itself, at a width | `lower/fold_arith.h`, shared with the lower folder |
| what the shared lower passes do to a function, and in what order | `lower/lower_pipeline.cpp` |
| where an object lives | `resolve_mir`'s `Object`, through `storageOf`, carried to `InstAlloc` |
| what a callee receives an argument as | `resolve_mir/inst.cpp`, `argumentMode` |
| whether a call may suspend | the callee's `Summary`, through `resolve_mir`'s `maySuspend` |

`conventionFor` asks `isLinear` and not `hasTeardown`, and the two rows above are separate for that
reason: they are different questions with the same answer for every type that exists, and the type
they differ on is the one an obligation is for. See `resolve_mir/README.md`'s ownership table.

`lower/repr.cpp` and `lower/manifest.cpp` are the two halves of
[`Analysis-Borrows-Revised.md`](../../doc/proposal/Analysis-Borrows-Revised.md) §1.3's third layer,
and the split is what each of them is a function of:

```text
repr(logical type, target)     -> the bytes
manifest(occurrence, target)   -> which repr, and how the value is carried
```

`lower/manifest.cpp` is §1.3's `manifest(logical type, contract, target)`. It takes the **whole
occurrence** - type, root contract and interned interior shape - rather than reading anything off the
type: an owned `String` and a shared window into one are the same logical type and different
representations, and that is the case this table exists for.

Taking a contract *parameter* was not enough on its own, and the way that failed is worth keeping. An
`ssa::Value` used to carry only its logical type and its convention, so every caller in
`lower/gen.cpp` had nothing to pass and passed `ownedContract()` - which would have manifested a
shared window as its owner the moment one existed. `Value::occurrence` is what closes it: the tier
retains what the table needs, and the signature asks for exactly that.

## Parameters arrive as entry-block arguments and become storage

A MIR parameter is a `Local` - storage the body projects and borrows from - and what the call
actually hands over is a *value*. Both are true, and this tier is where they meet: one entry-block
argument per declared parameter, then the `Alloc` every local gets, then a store of the incoming
value into its slot.

The store's `UseKind` is `argumentMode`'s answer put through `useKindFor` - the same pair the call
site read to build its operand. An owned parameter installs an owner and a borrowed one installs a
loan, and asking the shared function rather than switching here is what stops the two ends of one
handover from coming to disagree about which it was.

The load-and-store traffic that leaves behind for a scalar nothing borrows is `promoteStackSlots`'s
to remove, on exactly the terms every other local gets - see below.

## A `{}` is materialized, never transferred

The MIR call node always has a result occurrence, because a signature always declares one. A `{}`
manifests as no bytes, so there is no register for a value to be in and nothing below could map one -
so the conversion gives such a call no `ssa::Value` at all. That is the loud choice rather than the
quiet one: an operand that tries to name it fails here, with a source position, instead of lowering
to a null that the machine walk silently drops.

Nothing can name it, and that is the other half of the rule rather than a hope. `mir::Builder::resultOf`
answers the **constant** for a node whose result is a `{}`, so a use of one arrives as a constant
operand and is minted at the position that wants it - which is what a value carrying no information
costs nothing to do. The MIR `Const` node of a `{}` therefore produces no `ssa::Value` either: with
no operand able to name it, producing one would leave a dead `imm` in every body that mentions `{}`,
and this stage runs no pass that would remove it.

A **store** of one is not an instruction for the same reason a load of one is not: `Resolver::readPlace`
answers a `{}` place with the constant rather than a load, because there are no bytes at the place to
read, and a store into it writes those same no bytes. The one position where a `{}` still travels is
a **call argument**, because the callee declared a parameter for it - a zero-byte manifestation that
the shared IR still passes as a lane.

## A constant is a tree of values, and this tier says where it lives

`mir::Constant` carries a scalar's bits inline and an aggregate's parts as an `AggregateConstant` -
which constructor, and one constant per field. It says **what the value is** and nothing about where
it goes, because where a field sits and how wide an address is are the target's answers and MIR has
none of them.

`materialize` is the one walk that turns one into bytes, and it reads the same `Repr` every other
value of that type is laid out with - the field positions, the discriminant's form, and for a
co-packed field the word and the bit range it merges into. The destination arrives zeroed, so padding
and the payload an arm does not use stay zero without anything saying so.

Where those bytes go is this tier's decision and it is deliberately one function each way.
`lowerGlobal` writes them into the global's own storage. `placeConstant` puts a constant reached
inside a *body* - a field or parameter default whose value is a record - into read-only data,
interned per constant, and hands back its address; a record has no register form, so a constant of
one is storage and something has to place it. A constant that would be cheaper as a pair of
immediates, or on a target that would rather have neither, is a refinement of that function rather
than a different representation - which is the whole reason resolve hands down a tree and no
placement at all.

## Drop elaboration happens during conversion

Not as a pass that rewrites MIR first. §9.2's economy: a round that inserted instructions into MIR
only to copy them immediately is a round that can be skipped. What the conversion reads is
`Body::elaboration` - the drop and flag steps the ownership analysis placed, both lists sorted by
`(block, position)` so the walk carries one cursor each rather than searching per node.

`InstDrop` says a life ends and nothing more. What it *expands into* is a target question and is
`lower/inst.cpp`'s `emitTeardown`: the derived walk over the members, an authored instance call, or
both for different halves of one type - see `resolve_mir/README.md` on why `Drop` and `Reclaim` stay
separate all the way down.

A conditional drop is a branch on a flag slot, built as two blocks and a merge exactly as an `if` in
the source would be: a teardown behind a condition is control flow, and giving it a shape of its own
here would be a second thing for every later pass to understand. The address is computed *before* the
branch so that it dominates both arms. Nothing in the current language subset produces one - the MIR
README says why - so this path is written and untested.

## Bytes are a directory below

Everything from here to the `LowerModule` - what a record is in storage, what a value is in a
register, and the walk that emits it - is [`lower/`](lower/README.md) and is native. The split is a
wall rather than a filing decision: this directory is the ownership graph and the checks over it, and
a second target's lowering arrives as a sibling of that directory rather than as a switch inside it.

What is left below is what a *value* is here, which is not a representation question.

## Locals are storage, and promotion is somebody else's job

A MIR local becomes one `Alloc` in the entry block and the reads and writes of it become `Load` and
`Store`. Nothing here tries to hold a scalar in a value instead: the lower IR's own
`promoteStackSlots` runs over these on the way to machine code - `lower/lower_pipeline.h`, which is
the whole shared run and which this stage called one member of until phase 1 of `opt/README.md` - so
a local that is never borrowed costs nothing in the end, and doing it during conversion would make
the SSA dump depend on an optimization, which is the one thing a fixture about ownership should not
have to read around.

`StorageClass` states three rungs and reaches one. That is Analysis-IR.md §10.4's requirement rather
than an unfinished switch: the current compiler's equivalent reserves the region enumerator and
never selects it, which left the decision shaped like a two-way choice, and stating three from the
start means the escape analysis ranks them instead of learning a third answer.

It is `mir::StorageClass` and is decided in MIR, **of the object rather than of the slot**. Frame,
region and heap placement is what ownership analysis answers - whether a local escapes its frame,
whether it outlives the call that made it, whether an ambient region witness is in scope - and all
three are questions about the MIR the escape analysis runs over. Two locals a move connects are one
`Object`, so asking each slot separately is how one object comes to have two allocations that
disagree; `storageOf` is the join over the lineage, in one place. This tier carries the answer to
`InstAlloc`; it does not make one, and a second enumeration here would be a second place to add a
rung to.

## Loans are an identity here, minted by the conversion

Milestone 5 said `Value` deliberately carried no loan handle, and gave the right reason: an
`mir::LoanId` is dense in one MIR body and that region is released the moment conversion finishes, so
carrying one down would be an index into a table that no longer exists. What it also said was that
the day a pass moves a guaranteed value, the thing to add is an **OSSA-local** loan table minted by
the conversion. That is `ssa::Loan`, and this is that day - not because a pass exists, but because
the verifier now has two rules that need the term.

A loan is a set of storage values and a capability. A set, because a borrow can be rooted in more
than one place - `if flag then borrow(a) else borrow(b)` is one value with two lenders - and it ends
when *any* member is released, which is the only sound direction for a may-set. An empty set is the
loan a **caller** granted: a borrowed parameter arrives already borrowed and nothing here can end it.

Where the lender comes from is the part worth stating. It is `Elaboration::rootedIn`, §5.11's origin
environment, and **not** the operands: a call's result is rooted in whichever argument its signature
put the result's slot in, so `middle(a, b, c) -> q'Guard` is rooted in `b` with nothing in the operand
order to say so. A version of this that guessed from the first address operand reported on correct
IR, which is worse than reporting on none. `Editor::addResult` gives every borrow the caller's loan -
the permissive default - and `Conversion::attachLoan` refines it where the environment knows.

The rules `verifyBody` states with it: a borrow names a loan and no other convention does; the loan's
storage is defined before the value that depends on it; and **a value on a loan is not used after the
loan ended**. Only the third is not implied by dominance - the endpoint and the use are both
dominated by the allocation, so every structural check passes while the program reads storage it
handed back.

A loan ends at four events, all read off the operands rather than stated by a marker instruction - an
`end_loan` in the IR would be a second thing every pass has to move in step with the thing it names:

| event | what it is |
| --- | --- |
| `drop` of the storage | the release |
| a consuming use of the storage itself | an owner leaving the frame takes its borrows with it |
| a `load` out of the storage whose result is tracked | the **handover**: a destructive read empties the slot |
| a `store` to the storage | the **replacement**: a different object where the borrowed one was |

The last two were missing, and they are how nearly every handover in a converted body is actually
written: an owner leaves by being loaded out of its slot and consumed *as a loaded value*, so the
loan names the allocation and no operand of the consuming instruction names it at all. Nothing
related the two, and a pass that sank a borrow past the load was accepted. A `store` through a field
address or through a loan is neither, and gets that answer for free - `storageOfLoan` roots every
loan at the allocation it was taken from, so a `field.addr` result and a `Guaranteed` value lend
nothing and the reverse index finds no row for them. That is `analyze_loan.cpp`'s `replacedLocal`
rule - a partial write and a write *through* a loan are uses, not replacements - arrived at from the
operands rather than restated.

`OssaTester.cpp` reaches all three shapes by hand: it swaps a `drop` with the call above it, hoists a
handover `load` over a borrow of the slot it empties, and sinks a borrow's use past the `store` that
overwrites it.

**A clone mints new loans**, which is `opt/inline.cpp`'s. A `LoanId` is dense per body, so a callee's
loan cannot travel; one whose storage set names the callee's own allocations is re-minted naming the
clones of them, and one whose set is **empty** - the caller's, which is what every borrow the
conversion did not refine carries - stays empty. It does not become the loan the borrowing argument
holds, and the note in that file is the argument: the empty loan is "rooted outside" rather than
"rooted in parameter 2", and the caller's loan is also the one substitution `replaceAllUses` admits
when a returned borrow answers the call's result.

The optimizer reads the same four events from the other side. `mayWriteStorage` in `opt/analysis.cpp`
calls the handover `load` and the `store` **writes** of the storage, which is what stops
`forwardMemory` from carrying a fact across either; and a borrow is what an allocation's containment
turns on, because there is no `Borrow` instruction here - `&x` handed to a callee is a `load` of the
slot whose result is `Guaranteed`, so an allocation stays contained only while such a result does not
escape either.

## Two addresses, and the two questions every memory rewrite asks about them

An address at this tier is an `Alloc`, a global, or a pointer, plus a chain of `FieldAddress`/
`ElementAddress` - so both questions are **structural**, which is the whole reason `opt/memory.cpp`
exists at this tier rather than below the fork. They are two questions and not one, and a rewrite
that used either for the other's job would be wrong in a different direction each time.

**The same storage** - what a read may be answered with, and which store an overwrite kills. Two
addresses are the same when they are the same value, or when they are chains on the same base
agreeing step for step: a `FieldAddress` on the same record, field, interior shape and property, an
`ElementAddress` on the same container whose indices are one value or two constants that agree.
`simplify`'s block-local CSE has usually unified the two spellings already, so value identity is the
common answer and the chain walk is what reaches the pair it could not.

**May alias** - what a write ends. Two addresses may not alias when they are rooted in two different
`Alloc`s of this body, or in one `Alloc` on paths that separate: a different field of a
**single-constructor** record, or two constant element indices that differ. Everything else may
alias. Three refusals are load-bearing. An address with no allocation root of this body's - a global,
a pointer loaded out of storage, a borrowed parameter - may name anything. A **prefix** is not a
separation, so a whole-record store kills every field of it. And a **sum**'s fields never separate:
`resolve_mir` keeps one flat field list with a range per constructor, so two constructors' fields are
different indices over the same payload storage, and a niche-folded discriminant is a pattern of that
payload - both of which are `lower/repr.cpp`'s answers, a target's, and `optimizeProgram` runs once
per program.

## The editor is the only thing that writes, and its destructive half has eight rules

`ssa::Editor` builds a body and rewrites one. Four lists are private with reading accessors and the
editor is their only friend - `Block::instructions`, `Block::incoming`, `Block::arguments` and
`Value::uses` - because each of them is half of a statement the IR makes twice, and a pass that could
push into one directly is a pass that could state half of it. The old stage's experience is
unambiguous about which half of that pays: the passes quietly maintaining half a structure were not
found by review, they were the compile errors left after the members went private.

The rules the destructive half keeps, each stated once beside the operation in `build.h`:

- **An erase is an unlink, never a compaction.** `Body::insts`, `Body::values` and `Body::blocks` are
  indexed by their ids and so is every side table over them, so `eraseInst` leaves the table entry
  standing and `deleteBlock` sets `Block::deleted` rather than removing anything. The verifier and
  the lowering skip a deleted block; the verifier skips a value that is defined nowhere and read
  nowhere, which is what an erased instruction's results become.
- **Nothing live is erased.** `eraseInst` refuses an instruction whose results still have readers,
  which is the assertion every caller that removes a live value has already satisfied by pointing its
  readers somewhere with `replaceAllUses`.
- **A substitution preserves the use kind, and two conventions.** A consuming use of the old value is
  a consuming use of the new one; `isTracked` must agree, and a borrow may only be replaced by a
  borrow **on the same loan or on the caller's** - the loan is what the extent rule is measured
  against, and the caller's is the one substitution that cannot move an endpoint past a use.
- **A loan is repaired with the value it names.** `Loan::storage` names values, so `eraseInst` drops
  an entry naming a result it removed and `replaceAllUses` repoints one. A loan still naming a value
  nothing defines is a loan whose endpoints are never found, and the endpoint rule then passes on IR
  that reads storage it handed back.
- **A move is an unlink and a relink.** Never a remove followed by an add: the operands already name
  the instruction, and re-adding registers every use a second time. The symptom is quiet, because an
  inflated use count reads as "more than one reader" and every single-reader rewrite then declines.
- **`Block::incoming` holds one entry per edge, not per block.** `branch %c, X, X` is legal, so `X`
  names its predecessor twice and every edit moves exactly one entry. That is why `redirectEdge`
  takes an **ordinal** rather than a target, and why `removeBlockArgument` drops one operand per edge
  rather than one per predecessor.
- **A terminator replacement compares the two successor lists as multisets.** An edge present in both
  is left exactly as it was, its arguments included: folding `branch %c, then, else` into
  `jump then` is a fold whose site has no idea what `then` was being handed, and removing and
  re-adding the edge would take those arguments and put none back.
- **A pass places an instruction in front of another, never at the end of a block.** `setBlock` is
  the building form and appends; every block a *pass* sees already has a terminator, so `append`
  would refuse all of them. `setInsertBefore` is the cursor, and what it is for is the fold: a
  constant made for an instruction has to stand above the instruction that reads it, since a value
  defined below its use is the first thing `checkUse` reports. The cursor advances behind each
  instruction placed through it, so two added in a row arrive in the order they were written.

`addJump` and `addBranch` create an edge and record **no** predecessor entry - a conversion emits
every block before it seals any of them, and `Editor::seal` fills the lists from the edges once. A
pass has no such point, so a pass gives a block its terminator through `replaceTerminator`.
`verifyBody` counts the two records of every edge against each other, which is what makes that a
reported mistake rather than a carried one.

`Body::version` is two counters, `values` and `blocks`, bumped by every operation that writes. It is
on the body rather than handed to the editor because there is one body per function and the editor is
constructed per rewrite: a counter the constructor took would be one a caller could leave out, and an
analysis stamped against a counter the editor was never given is a cache that never goes stale.
`bool* changed` is a constructor argument for the opposite reason - whether a pass changed anything
is the pass's question, and nothing about the body answers it.

`OssaTester.cpp` builds each of those rules a case: the duplicate-arm redirect in both directions,
the fold that keeps the surviving edge's arguments, a move that must not inflate a use count, an
erase whose result a loan named, a block argument removed from every edge that fed it, a consuming
use preserved across a substitution, a split whose halves are in one region, and a block deleted
without its table entry moving. Every one of them was confirmed to fail against a deliberately
half-updated version of the operation it is about.

## Blocks say which continuation region owns them

`ssa::Continuation` is a region of blocks with a nesting rule, and every block names one. That is a
statement the CFG cannot make: a diamond is a diamond whether its right arm is a cleanup, an `else`
or the second half of a short-circuit, so a pass recovering "these blocks are the cleanup arm of that
point" from the graph would be inferring an intent the builder already knew.

`Body` is the region almost every block is in, and `Cleanup` is the arm of a conditional teardown,
which `emitDrop` synthesizes around a drop flag. Those are the two kinds that exist, and a lens call
is deliberately not a third: `mir::Continuation` has a `Handover`, because §10.1 requires the split
to be CFG at that tier and every ownership pass walks it there, and the conversion **outlines** each
one into a function of its own - so what arrives here is an ordinary call and an ordinary test of
what it answered. A row here is what the *embed* lowering will need, and it is a row rather than a
mechanism because the rule below already holds of it.

The rule: a block may be entered only from its own region or, **at that region's entry**, from its
parent, and may leave only to its own region or to an ancestor. `Continuation::entry` is the first
block made inside the region and is recorded when it is made; without it, "entered from its parent"
accepts an edge into any block of the region, which is the reroute a pass makes when it threads a
branch past the flag test a cleanup arm begins with. `Editor::enterRegion`/`leaveRegion` is the API
that opens one - a span of blocks, not one block, which is why it is a pair and not an argument to
`addBlock`.

**What `opt/` may do to one.** Two files touch regions and they touch them in opposite directions.
`opt/inline.cpp` **makes** them: a clone re-creates each of the callee's regions under the region the
call site's block is in, keeping its kind and multiplicity, so a cleanup arm inside a callee becomes
a cleanup arm nested wherever the call was - and the callee's own root maps to the call site's
region rather than to a new one, since the copied body is not a continuation of anything.
`opt/cfg.cpp` is the only thing that rewrites edges, and it reads the rule as one sentence: a rewrite
that moves or removes an edge is confined to one region. A merge
folds a block into a predecessor in the same region, a splice redirects a predecessor to a target in
the same region, a thread sends a predecessor to an arm in the same region; anything crossing a
boundary is declined rather than reasoned about. `entry` and `destination` are **read-only** there -
no block either field names is ever merged or spliced away - so a region can never come to name a
block that is not one. The one thing that does delete such a block is the unreachable sweep, and it
cannot produce that state: a region is entered at its `entry` and nowhere else, so a region whose
entry is unreachable is unreachable entirely.

**What a continuation still is not.** The result contract and the yield/resume relation both live one
tier up, on `mir::Continuation`, where a lens call is still a region: what a continuation receives is
what the callee declared, and control entering at `entry` and coming back at `destination` is the
whole of the relation. What is missing is the multi-resume form - `Y <- R`, where a yield evaluates to
what the consumer handed back and the region is re-entered where it left off - which arrives with
`iter fn`.

## Outlining a continuation

`Outline` and `Planner` in `convert.cpp` are the CPS lowering, and it is the only one: a `Handover`
region becomes an ordinary function taking the caller's frame by reference, the call site passes it
as the callee's last argument, and how the continuation *left* comes back as the value that function
answers with.

What licenses the outlining being mechanical is a property checked for other reasons.
`checkValueScope` says an operand names a value of its own block, so the only things that cross the
region boundary are the entry's block arguments - which become parameters - and the *storage* the
region reads, which becomes the environment. Nothing else can, which is why this is a transcription
and not an analysis.

The **exit signal is a compiler ABI and not a source type**, which is Analysis-Lens.md §12.1's fifth
item: a continuation answers with a small discriminant and what an exit carries travels in a
caller-frame slot the environment already reaches. One slot for the whole nest, so a `return` written
three continuations deep fills the storage the outermost frame reads and every frame between
propagates a discriminant. A region with **one** exit needs no test at all, which is every
`withLock`-shaped call.

Three rows, and only the middle one reaches the caller:

| Row | Written | Who acts on it |
| --- | --- | --- |
| `kExitFallOut` | falling out; `continue` in a `for` body | the caller resumes at the destination |
| `kExitLeave` | `return` | the caller propagates, and the outermost frame returns from the slot |
| `kExitBreak` | `break` in a `for` over an `iter fn` | the **callee**: an iterator stops asking |

So the call site's test is `== kExitLeave` rather than `!= kExitFallOut`. `break` and running out both
mean the loop is over and where it was over is the same block, so a row the callee alone acts on
costs the caller nothing - and adding a row below is a change to the callee alone. `mir::Node::Break`
is what emits the third: the same edge to the same block as falling out, under a kind that says it is
the last one.

The environment is `resolve_lambda.cpp`'s shape one tier down and for the same reason: a continuation
*is* a closure over the caller's frame. One record of borrowed fields holding the addresses of the
locals the region reads, an environment parameter in front of the ones the position declares, and a
`fun.value` pairing the two. What differs is only that nothing in the source wrote it.

Three things about it are worth knowing before changing one:

- **The plan is made before anything is emitted**, because the call site is converted first and names
  both the function and the record. Innermost region first, which is the reverse of the creation
  order: a child's environment is built by its *parent's* blocks, so a local the child captures and
  the parent does not declare has to reach the parent first.
- **One conversion is one scope.** `Conversion::scope` is the region whose blocks it emits, and a
  body with lens calls in it becomes several functions reading one MIR body. `function` is still the
  *owner* - whose substitution it is read under and whose declaration `callTarget` resolves against -
  and `emitted` is the one being filled.
- **A load out of the environment is given an owned pointer occurrence deliberately.** A load whose
  result is a loan is no instruction, because a value that lives in storage *is* its address; a field
  that *holds* a reference is the one place that is not true, and the occurrence is what tells the
  lowering which of the two this is.

## What the verifier costs

It runs after every ownership-sensitive pass in an assertion build (§12.1), so its own bound matters
in a way a once-per-compilation check's does not. Three things shape it, and each replaces a walk
that was rebuilding an answer it already had:

- **Dominance is two comparisons.** `Dominators` stamps every block with the interval its subtree of
  the dominator tree spans, so `a dominates b` is `enter[a] <= enter[b] && leave[b] <= leave[a]`
  rather than a climb up `b`'s idom chain - which was asked once per operand of the body and once
  per pair the linear rule compares. It lives in `dominance.h`/`.cpp` rather than in this file,
  because the lowering wants the reverse postorder underneath it and `opt/` caches the tree; the
  verifier still builds its own, uncached, since a cache shared between a check and the thing it
  checks is not a check.
- **The use lists are checked against a CSR, not by rescanning.** `operandUses` already counts, per
  value, how many operand slots name it, so the forward relation is a prefix sum away; each value
  then stamps its own users and every entry in its use list is one lookup. The walk this replaces
  re-read *every operand of a user* for every entry naming it, which an instruction naming one value
  many times paid the square of. The stamp carries the **multiplicity** of the pair and each entry
  spends one unit of it: membership plus a row total is a strictly weaker statement that a real
  editor bug passes - two instructions naming a value once each, against a use list holding the first
  of them twice, is stamped at both entries and totals two either way.
- **`Loan::storage` is indexed the other way.** `endLoansOn` fires at every consuming use and every
  drop; it used to scan every loan's storage list for the value, which is the loan count multiplied
  into the operand count for an answer that is the same at every point in the body.

- **The two records of an edge are compared by count.** An edge is written down in the predecessor's
  terminator and in the successor's `incoming`, and `checkPredecessors` compares the multiplicity of
  each pair rather than asking whether the block appears at all - which is the same argument the
  def-use check makes, and the same shape of bug: a fold of one arm of `branch %c, X, X` owes exactly
  one removal, and a membership test passes on removing both and on removing neither.

The linear rule's own dataflow is a worklist over reverse postorder with the loan half as a bit
table, on the same terms as the MIR analyses - [their README](../resolve_mir/README.md#what-the-analysis-costs)
says why, and it is the same reason.

`reprOf` memoizes a record's whole layout on the record (`RecordType::nativeRepr`, a bare offset
into the layout table), which is the one cache on a type in this stage. What it removes is the
nesting: laying out a record asks for each member's layout to place it, so a type three deep re-laid
out everything below it at every query. The field offsets are part of what is memoized rather than a
prefix walk beside it, because a packed field's position is not a running sum of the sizes before it
- `packOrder` may place it anywhere in the word - so a second walk would be a second answer.
