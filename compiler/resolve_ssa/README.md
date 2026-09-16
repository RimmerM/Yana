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

**A terminator's edges are alternatives, not a sequence.** The rule is about paths and a branch runs
exactly one of its edges, so a value carried on both of them is consumed once on every path and not
twice on any - `"a" ++ (if c then "b" else "")`, where the left operand travels to the join over
whichever arm ran. Read as a run of uses those are two consumptions of one value and the check fired.
Each edge is therefore compared against the state as it stood before the first of them, and what an
earlier edge marked is rolled back for the next; what comes out is unchanged, because the block's out
state is one state for every successor and the meet in the join is where a disagreement between two
paths is actually decided.

**A `Guaranteed` value does not carry a loan identity, and that is a decision rather than a gap.**
It used to hold a `LoanId`, on the argument that "may not outlive the loan it depends on" is a rule
with no term to check against - which is true, and is the shape lenses, yielded borrows, values live
across a suspension and iterator objects all need. What made the field unusable is §8.8: an
OMIR loan id is dense within one MIR body, and that region is released after conversion, so a value
holding one would name a table that no longer exists. When this tier needs loan identity it gets a
table of its own, minted by the conversion out of what the loan analysis already proved; until then
the field is absent rather than filled with an index nothing may follow. Analysis-IR.md §13.2 asks
whether a loan is an OMIR or an OSSA concept, and this is the answer: OMIR's, so far.

## Where each decision lives, exactly once

| Question | Answered in |
| --- | --- |
| what a MIR occurrence's convention is | `convert.cpp`, `conventionFor` |
| whether a shared loan of it is its copy | `convert.cpp`, `valueOccurrenceOf` |
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
| how a constant names another global | `lower/gen.cpp`, `materialize`'s `Constant::Address` arm - a `LowerDataRelocation`, which is why `lowerGlobal` is a declare pass and a fill pass |
| whether a fixed array's components are one block fill | `lower/inst.cpp`, `uniformFill` - a `setpattern` where every component is one constant whose bytes are all equal |
| whether a record literal's zero fields are block fills | `lower/inst.cpp`, `zeroFieldRuns` - a `setpattern` per run of zero fields adjacent in offset order |
| what order a body's blocks are read in | `dominance.cpp`, `reversePostorder` |
| which passes run over a body, in what order, and how many times | `opt/opt.cpp` |
| whether a local becomes a value | `opt/promote.cpp`, `eligible` - and where its block arguments go |
| whether an edge may be moved, merged away or threaded | `opt/cfg.cpp` - one region, and never a region's anchor |
| whether a block argument says anything, and which conventions may be answered at all | `opt/cfg.cpp`, `removeTrivialArguments` - the one implementation `promote` and `cleanCfg` both call |
| whether a branch may become a `select`, and what one may carry | `opt/select.cpp`, `speculatable` and `selectableJoin` |
| whether a loop may be decided false, and what makes it terminate | `opt/loop.cpp`, `removableStep` and `stepsToTheBound` |
| whether a computation may leave a loop, and what may be speculated into a preheader | `opt/loop.cpp`, `hoist` - `movable` and `invariantRead` |
| whether two instructions compute the same thing, and whether either may be taken out | `opt/cse.cpp`, `sameComputation`/`sameOperand` and `isRemovable` - read by `simplify` too |
| which redundant computation is worth removing and which is worth keeping | `opt/cse.cpp`, `materializedAtUse` and `answerableAcrossBlocks` |
| whether two addresses name the same storage, and whether they may alias | `opt/memory.cpp`, `sameAddress` and `mayAlias` |
| whether a call is inlined | `opt/inline.cpp` - `clonableDeclaration`/`clonableBody` for what a copy can reproduce, `worthInlining` for what it is worth |
| which functions the lowering emits | `mir::Function::reachable`, corrected over SSA by `opt/reach.cpp` |
| whether a check is removed | `opt/check.cpp` - `-no-checks`, and the structural rule that says which branch is a check |
| whether an instruction may write an allocation's storage | `opt/analysis.cpp`, `mayWriteStorage`, over `Contained` |
| what width an integer folds at, and which types have one | `opt/fold.cpp`, `foldableInt` |
| what an operation over constants comes to, and which identities hold | `opt/fold.cpp` |
| the two's-complement arithmetic itself, at a width | `lower/fold_arith.h`, shared with the lower folder |
| what the shared lower passes do to a function, and in what order | `lower/lower_pipeline.cpp` |
| where an object lives | `resolve_mir`'s `Object`, through `storageOf`, carried to `InstAlloc` |
| what a callee receives an argument as | `resolve_mir/inst.cpp`, `argumentMode` |
| whether a call may suspend | the callee's `Summary`, through `resolve_mir`'s `maySuspend` |

`valueOccurrenceOf` is `readMode`'s first clause re-asked with the substitution and the layout in
hand, and it is the **one decision a specialization makes for itself** - `specialize.h`'s "a
specialization decides nothing" is about instance selection, use modes and drop points, all of which
are still the generic body's and are read unchanged. A generic body reads its parameter
`borrow.shared` because `a` owes a teardown as far as the MIR builder knows; at `I32` it does not,
and `manifest.cpp` already states that a plain shared loan of a register-valued trivially copyable
type *is* its copy - "no loan of a trivial scalar is made at all for this table to represent". So
this restores an invariant the manifestation table assumes rather than adding one, and
`lower/gen.cpp` builds a signature from the **declared** occurrence, so nothing about the ABI moves.

What it is worth is the *slot*. A borrow is a second name for storage, so a parameter read through
one keeps its frame slot: `promote` declines a slot whose store installs a `Guaranteed` value and
whose loads read back `Trivial`, a convention change being no substitution, and the inliner's
`sizeOf` then counts a store and a load per parameter that is never emitted. `fn f(n: Int) = n + n`
was an `alloc`, a `store` and a `load` in front of the add and is now `add %0, %0`; `g(10)` at a
generic `x + x` reached the machine as `imm 10 ; add %v,%v` and is now `20`. Over the thirteen bench
programs it is **1.0 G instructions** (0.9615 -> 0.9512 of the shipping stage, all of it `Hash`), and
it costs bytes rather than saving them: the bodies are honestly smaller, so the inliner buys more
with the same budget - J43 §2.

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

The load-and-store traffic that leaves behind is `opt/promote.cpp`'s to remove, on exactly the terms
every other local gets - see below. A **borrowed** parameter's slot is the one it declines, and the
reason is the store above: what it installs is a `Guaranteed` value of a borrowed occurrence, where
the loads read back a `Trivial` one of the owned occurrence, and a substitution across either of those
is not a substitution. (That is the slot's *contents* being a borrow, which is a different thing from
a borrow *of* the slot - see below, which the pass follows.) A `->` parameter of a trivially copyable type is an owned occurrence at both
ends and promotes with every other local, and so does every parameter slot of an inlined body.

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

## Locals are storage, and the *conversion* leaves them that way

A MIR local becomes one `Alloc` in the entry block and the reads and writes of it become `Load` and
`Store`. Nothing in `convert.cpp` tries to hold a scalar in a value instead, and that half is
unchanged: doing it during conversion would make the SSA dump depend on an optimization, which is the
one thing a fixture about ownership should not have to read around, and `.ssa.expect` is that dump.

**Where it stops being storage is `opt/promote.cpp`, at this tier**, and that reversed the original
reading. The lower IR's own `promoteStackSlots` does run over these on the way to machine code -
`lower/lower_pipeline.h`, the whole shared run - so a local that is never borrowed did cost nothing in
the emitted bytes; what it cost was everything above them. Phase 10 of `opt/README.md` has the
numbers: `range` could never fire because no named value at this tier was read in two blocks,
`convertSelects` needed an exception for the `load` of a parameter slot, `threadBranches` could not
see through a `let`, and the inliner counted a store and a load per parameter towards a budget of
twenty. It is also what `Analysis-IR-JS.md` Part 6.1 requires, since JSIR consumes this IR and never
sees the lower one.

What promotion takes is an allocation in `StorageClass::Frame` whose held occurrence owes nothing,
all of whose uses are whole-allocation loads and stores agreeing on one `Trivial` occurrence, plus
drops that lower to nothing - and, since J37, the same again through a **borrow of it**: a `load`
whose result is `Guaranteed` and the `represent`s over it are second *names* for the storage rather
than uses of what is in it, so the walk follows one and takes the borrow away with the slot. That is
what a grafted `iter fn` continuation leaves behind and it is most of what the pass is worth. What it
leaves is everything else - a record reached through a `field.addr`, a slot a call was handed the
address of - and `promoteStackSlots` is still below it for the half that only matters to the machine.

`StorageClass` states three rungs and reaches two. That is Analysis-IR.md §10.4's requirement rather
than an unfinished switch: the current compiler's equivalent reserves the region enumerator and
never selects it, which left the decision shaped like a two-way choice, and stating three from the
start means the escape analysis ranks them instead of learning a third answer. `Region` is the one
still out of reach, and `mir::verifyBody` refuses it - there is no ambient witness for a region-placed
object to be placed in.

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

`free` is deliberately **not** a fifth row. It ends the storage as thoroughly as a `drop` does, and
it is emitted only immediately before a terminator that ends the activation - so there is no
instruction after it in its block for a stale borrow to be used by, and a row here would be a rule
with nothing to catch. The day a release is placed anywhere else, this is the line to revisit.

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

## The editor is the only thing that writes, and its destructive half has ten rules

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
- **A block argument added to a finished body is two operations, and the pair has a window.**
  `addBlockArgument` is the builder's and refuses a block that already holds instructions;
  `declareBlockArgument` and `addEdgeArgument` are what a *pass* uses, and between them the body is in
  a state `verifyBody` rejects - an edge supplying fewer operands than its target declares. They
  cannot be one call, because a loop header's incoming value is a value the header itself defines.
  `opt/promote.cpp` is the only caller and should stay so.
- **A branch's two edges may carry different operands, and only the builder ever assumed otherwise.**
  `makeBranch` pushes one list onto both, which is what a conversion emits - a value live before a
  branch is live on both sides of it. `makeBranchEdges` states each edge's own, and is what a pass
  rebuilding a branch that already exists takes: since promotion places a block argument where one
  slot's writes meet, the two arms of a branch routinely declare different ones.
  **The same is true of a `switch`**, which took until 2026-09-16 (J56) to find: `makeSwitch` pushes
  one list onto every arm and `promote` pushes an operand onto only the arms that reach the block,
  so `makeSwitchEdges` is what `cfg.cpp`'s merge and `inline.cpp`'s clone take. Both read the
  *fallback's* list and gave it to every case until an iterator object's step dispatch - the first
  three-armed `switch` here over a body that writes frame slots - asserted inside the builder.
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

**Except in a step**, since J53: an iterator object's step **embeds** every region the producer opens
rather than outlining it - a `for x in g(...)` and, since J56, a lens over part of the producer's own
body - because the outer producer is suspended rather than running and there is no frame for the
callee to hand control back into. The region's blocks become the step's own, so a step
is one scope over the producer's whole body - `Conversion::inScope` answers true for every block
there - and what the region's three exits mean is mapped where `convertLensCall` maps them, with the
destinations being blocks of this body rather than a discriminant the caller tests. That is still not
a third region *kind*: the regions are `mir::Continuation`'s and the rule below is what says the
mapping is total.

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
what the callee declared, control entering at `entry` and coming back at `destination` is the whole
of the relation, and since 2026-09-15 `resume` and `answer` are the other two halves - what a pass
hands back and what the continuation answers with. So the multi-resume form `Y <- R` is *there*, and
it is one more fact on the region rather than a second kind of region: the region is re-entered at
its entry the way a `Many` one always was, and what the resume value changed is the answer's type.

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

**`<- R` is where the signal stops being a bare number**, and it is the one change to this lowering
Design-Iterator-Objects.md §2.5 needed. Where the region's callee declared a resume channel, the
outlined function answers `Outcome(R, exit)` instead of the `I32`: `emitExit` wraps the row it was
going to return in `Exit(...)` and `mir::Node::Resume` - `continue v` - answers `Proceed(v)`, which
is the only exit that carries anything. `Planner::plan` reads the type off `Continuation::answer`
rather than rebuilding it, because interning a record is not something this tier does.

Two things about that are worth stating. **The caller's side does not move**: what a consumer's
`lens.call` reads back is the *iterator's* result, which is still the signal or the `| E` wrapper, so
every test in `convertLensCall` is the one it was. And **the record is address-only**, so what the
lifted function really returns is the hidden result pointer the lowering gives any record result -
see `lower/README.md`. A resuming pass therefore costs one store and one load of a discriminant per
step over a non-resuming one, and `test/resolve/lens/Resume.yana.ssa.opt.expect` is where that is
visible: the flattening below folds the call away entirely and leaves one loop, and the wrapper's
store-then-load round trip survives it, because promotion does not promote an address-only value.

The environment is `resolve_lambda.cpp`'s shape one tier down and for the same reason: a continuation
*is* a closure over the caller's frame. One record of borrowed fields holding the addresses of the
locals the region reads, an environment parameter in front of the ones the position declares, and a
`fun.value` pairing the two. What differs is only that nothing in the source wrote it - and that a
continuation's environment is always the frame's, so it carries `Shapes::closure` and owes nothing.
(`fun.value` stopped being `kSsaPure` with D17: a value whose environment is heap-placed *takes* that
environment, so the operand's lineage ends at the instruction exactly as an aggregate's components
do, and the flags are per kind.)

**And `opt/inline.cpp` takes it back apart**, which is what keeps one lowering enough:
`flattenContinuations` copies the iterator's body into its consumer, resolves the call through the
pair to the declaration the pair names, and copies the continuation back in - to a fixed point, so a
three-stage chain comes out as one loop. What that means for a reader of this section is that the
shape described here is what the *conversion* emits and rarely what the backend receives; it survives
a recursive consumer, a spent budget and `-inline none`, and nothing else.

The lifted declaration therefore has to carry the facts a consumer reads off a `mir::Function`, and
one of them is not free: `computeSummaries` runs over the MIR before this declaration exists and
never runs again, so every `Fact` on `Function::summary` is `Unknown` unless the outlining fills it.
`setSuspension` is filled from the lifter, exactly - a continuation's blocks *are* the lifter's
blocks - and an unfilled fact reads as whatever conservative default its consumer names. That cost a
session: the inliner's default is "may suspend", so it declined to copy a body that is the caller's
own code.

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

## An iterator object's step, which is the same body read differently

`doc/inflight/Design-Iterator-Objects.md` §3.2, built 2026-09-15. `StepPlan` and the `stepping` field
on `Conversion` are the fourth shape a conversion takes, beside a function of its own, an outlined
continuation and a graft - and the only one that changes what the *body means* rather than where it
is emitted.

    fn f$step(&self: f.Iter, $resume: A, $cancel: Bool) -> Bool

Three things are read differently and **nothing else is**. Everything between two hand-overs is the
producer's own code, converted exactly as the continuation-passing form converts it, reading the same
drop points and the same loans.

| In the MIR body | In the step |
| --- | --- |
| a parameter's local | a field of `self` - the captured argument, or the pointer in it |
| a local live across some `yield` | a field of `self` - the payload |
| every other local | an ordinary `alloc`, in the step's own frame |
| `read ($body)` | nothing at all; a `yield` is not a call here |
| the call through it - the `yield` | write the slot, write the index, `return True` |
| ... and the value that call produced | the step's `$resume` argument, in the resume block |
| `return` | write the *finished* index, `return False` |
| the entry block | reached from the dispatch, which switches on the index |
| a `lens.call` - a `for` over a traversal, or a lens | initialize the member, then step it - see below |

**The step's argument is the continuation's answer, unchanged.** J49 made an iterator's continuation
answer the exit signal, or `Outcome(R, exit)` under `<- R`; that value is what a step is handed, at
the same type, and it is what lets one function serve three callers - `next()` passes "fall out",
`resume(v)` passes `Proceed(v)`, and the **teardown** passes "leave". So the producer's own departure
path is the record's teardown with nothing added to it: `resolveYield`'s test reads the argument,
`emitDeparture` walks out of the loops, the statements below them run, and drop elaboration releases
every local that is still live. §3.3's "the teardown is shared across indices" is not something built
here - it is what converting one body once already does.

`$cancel` is the one thing the answer cannot carry. A record at index zero has run *none* of the
producer's body (§2.6), so entering it to cancel has to release the captured arguments and stop
rather than start; it is a parameter rather than a reserved index because the index says where the
producer is and this says what is being asked of it.

**A caller that sends a *row* has to send it at the type the producer declared**, and `exitRow` is
the one place that is written. Three of the generated callers choose a row rather than passing a
consumer's value - the teardown's "leave", a nested member's "fall out", and the cancellation of a
suspended child - and each of them wrote the bare exit signal, which is right for a plain producer
and is the wrong type where `<- R` makes the step's parameter `Outcome(R, exit)`. The teardown is the
one that mattered, because owning anything is what gives a record a teardown at all: every resuming
producer with a growable array or a `String` in it failed to compile with `internal: call to g$step
supplies argument 1 of type I32, and it declares Outcome(Cmd, I32)`.
`../../doc/progress/2026-09-16-J57-object-defects.md` §2 is the account and
`test/resolve/iter/ResumeOwned.yana` is the fixture.

**Values crossing a hand-over are spilled.** A counted `for` hands its bound and its step along its
own back edge as block arguments, so the block a `yield` sits in has values live across it that no
local names - and splitting that block leaves a resume block naming a value nothing on the way in
defines. `RecordType::payloadLocals` carries them as node indices with the high bit set, and
`RecordType::keepsAcross` says which hand-overs each has to survive; the store is at the `yield` and
the reload replaces what the conversion holds for the value from the resume block on. Two limits are
held at the *mint* rather than met here: such a value must be trivially copyable, because the drop
elaboration the producer's body carries knows nothing about a field this transform invented; and
every block naming it has to be converted after the `yield`'s own, because the conversion emits
blocks in index order - a spill *none* of whose hand-overs the conversion reaches before the value
exists is left out of the record entirely, which is where J51's "a member that is never written"
went.

**Which field a payload entry lives in is read and not derived.** Slot sharing (§3.2's third step)
puts two members whose live ranges never meet in one field, so there are more entries than fields and
`RecordType::payloadFields` is the map - `StepPlan::localField` and `spillField` are filled from it
rather than from `payloadFirst + j`. Nothing else in the step changes: the storage walk asks which
field a local is in and gets an answer, and two locals that share one are two locals whose lifetimes
say the answer may be the same.

**The dispatch is emitted last**, because the blocks a `yield` resumes at are made while the body is
converted - so the switch that names them cannot be written before it. Below three cases it is a
chain of comparisons rather than a `switch`, which is the IR's own rule and is what a producer with
one `yield` gets.

**`$held` is the slot's own note**, where the hand-over owns something with a teardown: the step sets
it as it writes and releases the previous contents first, and its exits - the producer's `return` and
the cancellation of a record at index zero - release whatever is still there. Every consumer that
*takes* clears it: `f$take` for a `for`, and nothing else, because the class borrows.

**A region is embedded, not outlined** - §3.2's first step, since J53 for a traversal and J56 for a
lens, and the fifth shape this conversion takes:

      %child = g$new(...)              -- the member, initialized where the call was
      store %child -> self.$g
      jump advance

    advance:
      %more = g$step(&self.$g, fallOut, False)
      branch %more -> obtain -> <the region's destination>

    obtain:
      %item = <the child's slot, under g's own convention>
      jump <the region's entry>(%item)

`convertStepNest` is the call site and `convertEdge` is the rest of it. Falling out of the region is
the pass ending, which here asks the member for its next element rather than answering a
discriminant, so the edge goes to `advance`; `break` - which is also what a **departure** out of the
outer object emits, since `emitDeparture` leaves the loops the `yield` was written in - cancels the
child and resumes at the region's destination; and a `return` or a departure propagates outward
unchanged. A `yield` written inside the region needs nothing of its own: it writes the *outer*
record's slot and index exactly as one written anywhere else does, and the child stays suspended in
its field.

**A lens region is that picture unchanged**, which is what §6.6 asked and did not settle. J53 read a
`Once` region as a different mapping of the same three exits, because a lens's continuation runs once
and an iterator's runs per element. It is the same mapping. A lens hands over exactly once on every
path (`checkHandover`), so the member's step answers `True` at its `yield` and, stepped again, runs
the code after it - the cleanup - and answers `False`. `advance` entered a second time is therefore
exactly "run the cleanup", and the `False` arm it already has is exactly "carry on at the region's
destination". The block that loops for an iterator is the block that releases for a lens, and nothing
in `convertStepNest` or `convertEdge` distinguishes the two multiplicities.

A **departure** takes the same edge: a `yield` inside a lens region whose consumer has left jumps to
the region's destination (`emitDeparture`), which is a plain `Jump` and therefore the fall-out arm, so
the lens releases on the way past exactly as it does in the continuation-passing form. A `break`
written *inside* a lens region is refused one tier up, so the `break` arm belongs to a `Many` region.

**A lens region whose continuation contains no `yield` is embedded too**, which is §6.6's second
question answered by choosing. Such a region is an ordinary outlined call in the CPS form and a step
cannot plan one - the planner runs for a body this stage builds and a step is a second reading of
somebody else's, which is why `convertProgram` hands a generated body an empty outline list. Running
the planner for a step's yield-free regions was the alternative; embedding them is one shape for one
construct, and it is what keeps a site type's layout the same whether or not a `yield` happens to be
written inside the region.

**A boxed child is built here and released here** - §6.3's recursion, built 2026-09-16 (J59). Where
the member is an erased `iter(Y)` rather than the child's record (`RecordType::nestBoxed`), the three
lines above become four: `g$new` into a local of *this* step, one `fun.value` over it - which the
lowering relocates into a heap block with the child's teardown in front of it, exactly as it places a
lambda's environment - and the pair stored into the member. `advance` then steps the pair rather than
the record: `CallThrough::Value` against the signature `IterType::step` carries, whose answer is
**where the item is** rather than a `Bool` and a slot, so the loop test is the one value an address
cannot have. That indirect call, once per level between the root and the leaf, was the O(depth) cost
§6.3 states until delegation was built; the allocation is one per level *entered*, which for a
producer with two regions means a block even for a child that yields nothing.

**A delegating child is not stepped here at all** - §6.3's `yield from`, built 2026-09-16 (J60), and
where the member's pass is nothing but `yield x` over a child in this producer's own cycle. The four
lines above become: `g$new` into a local, the pair into the member's **link**
(`RecordType::linkType`, which is the pair beside one word of chain), *which member it is* into
`$down`, and the level suspended at an index of its own. There is no `advance`, and the region's
blocks are not converted at all (`inScope`) - they are a hand-over of the item and nothing else, and
the consumer is about to take that item from the child directly. `delegateResume` is where the level
comes back once the child has run out: it writes what the consumer answered into `guardYields`' own
local - which nothing else would, since the `yield` that normally writes it is a block this level no
longer runs - and then releases the box and carries on where the region rejoins.

**The guard also decides whether the level delegates at all.** A departure runs the producer's code
below its loops, and that code may open the very region it is leaving: `marked`'s walk of its right
subtree is written after the `yield`s, so a consumer's `break` five elements in still enters it. A
level that delegated there would suspend with nobody left to step it, so where `$yielded` is set the
region builds its child with the **ordinary** shim instead of the raw one, steps it once and releases
it - which is exactly what the region's own blocks do when the guard is set, and is what makes the
delegating and non-delegating forms release the same levels in the same order.

The trampoline itself is `f$walk` (`emitIterWalk`), which is what every consumer of a delegating
object steps through: it loads `$inner`, steps that level's pair directly, pushes when the answer is a
tagged link, pops through the link's chain word when the answer is null, and steps this record's own
`f$step` when `$inner` is zero. Nothing in it owns anything - a level is still released by the level
that built it - so a teardown is still one drop of the root record recursing on the machine's stack.

**A chain word is resolved and not simply dereferenced** - `iterLinkAddress`, built 2026-09-16 (J61).
With bit zero clear it is the address of a link inside a heap block; with bit zero set it is
`field << 1 | 1`, one of the reader's own link members, and the address is derived from the `self`
the step was entered with. That is what lets a suspended delegating object be **moved**: every level
below the root is boxed and a block never moves, so the root's own link is the one address in the
chain a relocation could invalidate, and naming it as a member rather than an address is what makes a
`let moved = it`, a `->` argument, a `return` and a store into a field all keep the walk. Only
`$self.$down` ever carries the tag, so a tagged word always names a field of the root;
`emitIterErasedRaw` resolves before it answers, because that address crosses to another record's
trampoline.

A boxed level is ended by **dropping the pair**, which is one instruction that runs everything: the
tag bit is tested, the block's header is the child record's own `Drop`, that is its step entered with
a cancel request, and the block is handed back - so the recursion down the chain is the machine's.
The **empty pair** `f$new` writes in its place is what makes it idempotent, and it is the box's answer
to the finished index below: a function value whose environment word is null has the tag bit clear, so
a drop that meets it does nothing, and the code word is the real shim so the value is never one that
could be stepped into nothing. The same pair is written back after every release, because a box is
released where its region runs out, where a `break` leaves it, and again at the producer's `return`,
and a second free is not the no-op a second cancel is.

**`f$obj` is the witness's "make an object" entry** (`emitIterObject`), built 2026-09-16 (J61) -
Design-Iterator-Objects.md §6.4, and the one generated body that is not part of a step. It is `g$new`
into a frame local and one `fun.value` over it, which `emitFunValue` relocates into a heap block with
the record's teardown in front of it - `adaptIterValue`'s own pair at an owning position, made inside
a generated body because it is declared after the drop elaboration has run. Its parameters are the
*class member's*, so a call built against the class's declaration reaches it directly where the
subject is concrete, and the two lines that adapt each one to the constructor's position are
`convertStepNest`'s own.

**Cancelling a child is its own step entered with a cancel request**, and it is idempotent because
`f$new` writes the **finished** index into every nested member: a child that was never entered, one
that ran out and one that was already cancelled are all a call that does nothing. So the outer
object's teardown cancels a suspended child exactly once on every path without a flag beside the
member - `stepCancelNests` runs at the producer's `return`, which is exhaustion and the departure a
cancellation takes, and `convertEdge` runs one at the `break` that leaves a region early. For a lens
member that call *is* the release: the step entered with "leave" is what its `yield` reads as the
consumer being gone, so `emitDeparture` walks out of whatever loops the `yield` was in and the
statements below them are the cleanup. `stepCancelNests` walks the fields in reverse, which is body
order reversed, so a region opened inside another is ended inside it.

**What the child handed over is read under the child's own convention** - `g$item` where it lends,
`g$take` where it carries a `$held` flag, and a copy of the slot otherwise - and then adapted to the
occurrence the region's entry declared: a **window** is derived from the owner by the same member
`emitIterStepper` calls, and anything else is a `represent`, which moves no bytes. Two adaptations a
call boundary would have made silently are made by hand here, because a store is not a call: the
child's constructor takes its captures by the record's conventions rather than the lens call's, and a
`yield` whose value is a reference - `xs[i]` inside a *generic* producer answers `source'a` - is
loaded into a slot that holds bytes.

A **boxed** child answers an address and nothing else, so its item is two rows rather than three
(`stepBoxItem`): a position that is itself a reference takes the address as it stands, and one that
takes the value loads through it. A window hand-over is the one the box has no row for and is refused
at the mint. A child held **inline** whose own record delegates answers an address too - the region
steps it through `f$walk` rather than through `f$step`, because a delegating record's step may answer
"I entered a child" and only the trampoline knows what to do with that - so the two arms differ in how
the step is reached and read the item the same way.

**A `@heap`-placed payload member lives in a run of its own** (§6.1) and the field holds its pointer:
`f$new` allocates one per placed member for the object's whole life, the step reaches the value
through the pointer, and the step's exits free it. That is what makes the self-borrow refusal's
advice work - a borrow into a run does not care that the record moved - and it is why such a record
owes a teardown whatever its members are.

Four more generated bodies sit beside it in `convertProgram`, none of which reads a MIR body:
`f$new` allocates the record, stores the index at zero and stores the captured arguments (a record
with an untouched payload is a shape MIR has no write for, which is why the body exists at all - and
it is an ordinary inlinable body since J52, the `@noinline` J51 gave it having been a mitigation for
`opt/place.cpp`'s carrier walk rather than for anything the memory passes do); `f$drop` is one call
of the step with `$cancel` and `Exit(kExitLeave)` at whichever of the two types the step declares,
storing its subject into a
slot of its own the way every authored teardown does - and it is the record's *whole* release, which
is why `resolve_mir`'s classification gives a site record no derived `Reclaim` beside it; `f$item` loads the lent value's address out of
the slot and `f$take` moves the value out of it. `f$next` and `f$resume` are `Iterate`'s two members -
a step call, the item **borrowed** out of the slot, and the `Maybe` around it. The borrow is where
the work is: the payload position is a reference, so what is carried is the slot's address said as
the loan it stands for, or - where the item has a window, as a `String` does - two words derived from
it through the same member `deriveView` calls, kept reachable by `summary.cpp` because a generated
body has no node for that walk to find. `f$next` on a **resuming** producer also checks the index:
`next()` means "I have nothing to send", which at a suspend index is a pass that answered nothing and
is a `checkCondition` rather than a silent end.

**A load of a field that holds a reference has to be an instruction**, and both `f$item` and the
by-address capture are the places that bit again: a value that lives in storage *is* its address, so
a load whose result is *declared* a loan lowers to nothing at all and what comes back is the field's
address rather than what is in it. Both load at the field's own `%T` and hand the result on under the
occurrence that says what it means - which is the same note this file already makes about a
continuation's environment.

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

## A loan of a scalar in a value position is a load, and a value at a reference position is a slot

The conversion is the first tier that has a **manifestation**, and it is therefore the first tier
that can see the two places where a MIR occurrence and the position it fills disagree about what a
value physically is. `'I32` with a published origin manifests as an *address* (`isReferenceValue`);
a call-bounded shared loan of the same scalar manifests as the number. MIR is right about both and
says nothing about either, so the adaptation belongs here.

`valueUseOf` is one direction. `firstOf(xs) + 1` at `fn firstOf(xs: [I32]) -> 'I32`, and `xs[a] +
xs[b]` through an `Index` constraint, put a loan straight into an arithmetic operand - a pointer
where the type says `I32`, which reached the machine as an `add` of a `Ptr` and an `I32`. It is
spent at every operand of a `Binary`, a `Unary` and a `Native` op, and at a call argument whose
declared position is not reached through an address.

`referenceUseOf` is the other. `hold(9)` at `fn hold(value: 'Int)` has a number in a register and a
position that is the address of one, so the temporary that address names has to exist. The
*lowering* already made that slot at a call it still saw (`frameStorage` in `lower/inst.cpp`) - and
that was enough only while every call reached the machine as a call. The inliner and `graftCall`
**erase** the call, and what they leave behind is the callee's body reading a parameter that has
become a register value: `test/resolve/BorrowTemporary.yana` segfaulted at every optimization level
above none, and was correct under `-no-ir-opt`. The slot is made here, where the call is still one,
so by the time anything splices a callee in the two sides already agree.

Concrete positions of the argument's own type only, in both directions. An **erased** position takes
an address whatever it was bound to and its declared type is a variable this body cannot allocate,
so it stays the lowering's - which is where the descriptor that sizes it is read.

**A call through a *value* is a boundary with positions too.** `yield e` is a call of the
continuation, and `items`' `yield chunk[i]` supplies a subscript's published loan - an address - to a
position declared `a`, which at a scalar holds the number. `verify.cpp`'s `checkValueCall` compares
exactly those two conventions, so the conversion reads the positions off the target's own `FunType`
where there is no declaration to read them from and spends the same two adaptations. Without it every
`for x in items(xs)` over `lib/`'s containers stopped in the verifier.

## A relocation out of a raw address is a copy

A value that lives in storage *is* its address at this tier, so a `Load` of one is no instruction and
a move out of it re-points nothing: the object stays where it is, and what the move produced is the
right to reach it. That is exactly right for a move out of a **local**, because the move is what says
nothing may read the source again.

It is not right for a place rooted in a raw address. The storage is outside the ownership model, so a
`store` through the same pointer afterwards is an ordinary operation and the "moved" value follows
it. `lib/Core/Map.native.yana`'s swap-remove is the shape - `let ->doomed = *(items + at)` and then
`store(items + at, moved)` - and read as an alias it answers the entry that was moved *into* the gap,
which is what `Core.mapCases` reported once `Maybe(Int)` and `Entry(Int, Int)` became `TrivialCopy`
(inventory D16). So a relocation whose place is rooted in a `Place::Address` emits the same `InstCopy`
a `CopyTrivial` of an address-only value already emits: fresh storage with the bytes in it.

At a **trivially copyable** result and no other, which is what an `InstCopy` means - a value that owes
a teardown may not be duplicated, and a relocation of one out of a raw address is a hand-over whose
destination write is what moves the bytes. The shipping stage has always copied here: an `InstMove`
out of a pointer place is bytes.

## A read of a type with one value is that value, at a specialization too

`Resolver::readPlace` answers a `{}` place with a constant rather than a load, because a type with no
bytes has nothing at the place to read and `makeMemoryFlags` has no encoding for a width of zero. It
asks that of the **written** type, which is all it has; a generic body reads a payload of type `a`
and only a specialization at `a := {}` makes it this. `toOutcome` at `Result(e, {})` - `?` on a
function that answers nothing - is the one in the library, and what it produced was a load of zero
bytes that `lower/inst.cpp` asserts against by name. The conversion asks the same question of the
*substituted* occurrence, which is the first place the answer is available.

## An owner reaching a view position under substitution

`fn (Hash(k)) probe(->key: k) -> U32 = hashKey(key)` at `k := String` segfaulted, and `Map(String, v)`
did not run because `insert(&self, ->key: k, ->value: v)` is that shape. The parameter's storage holds
the **owner** - three words - and `hashKey`'s shared position of `k` is the **view** under
`substitutedContract`, which is two words in `StringView`'s layout. `Resolver::adaptToView` derives one
from the other wherever the type is concrete; inside the generic it cannot, because `k` is open where
the body is resolved and a specialization has no body of its own to adapt in.

So the derivation is emitted **here**, which is the one place a substitution is known:
`Conversion::deriveView` calls the same member the resolver calls - `mir::viewMember` off the
`ViewRow` - and never a second one, because `mir::needsViewDerivation` is the one predicate and a
value that is already the view fails it. Every position kind goes through it: a `Read`, a call
argument, a `return`, a block-argument edge, and a `yield`, which is a call through a value and
therefore the argument path. A field read out of a record holding the owner is the `Read` again, one
projection down.

**A place's occurrence is not a position's**, and that is the trap this cost a round on.
`Conversion::storageAt` is the distinction: a place's contract is the *parent's* qualification
carried down, so a field or an element declared at a type variable reaches `substitutedContract`
looking exactly like a borrowed position and is answered as the view - right for a parameter, wrong
for storage. `data W(a) = W {v: a}` at `a := String` holds an owner in `v` however the record was
reached, and `copy(Just("abc"))` aborted until the two were told apart. A **local** is left alone: a
parameter's occurrence *is* a position.

The reverse direction is not a derivation. Two words describe somebody else's storage and nothing
reconstructs the run, the capacity and the count out of them, so `refuseViewForOwner` reports it -
`exchangeSize(xs: Array(Int))` given `m[1]`, which is that element's window. The one exception is a
**derivation applied to the view it makes**, which is that view: `fn (Writable(c, a)) sort(&xs: c)`
writes `elementsMut(xs)` because at `c := [a *n]` the parameter is the storage, and at `c := Array(a)`
it already is the window. That is folded rather than called, on `Function::derivation` - carried onto
a specialization for the same reason `teardown` is.

**Reachability is what keeps the derivation**, and this walk may not mint one - `convertProgram`
asserts that the function list does not grow. `summary.cpp`'s `rootViewDerivations` roots
`viewMember` for every substituted type argument of a specialization that has a view, which is
deliberately blunter than the test above, on `opt/reach.cpp`'s terms for teardowns: which positions of
a body need the adaptation is a walk under this substitution, and reproducing it in the MIR walk would
be a second statement of a rule that has one.

An **erased** body neither asks for a view nor could answer one: `substitutedContract` needs a
concrete substitution to turn a position into the view at all, and a descriptor carries no per-access
entry (Analysis-Manifestations.md §5.3). So an open type never reaches `deriveView`, and one that did
would be refused by `viewMember` answering null rather than lowered to a derivation of something with
no layout.

## A stored borrow of a container is the descriptor, not a pointer to one

`data Formatted(s, a) {value: 'a, style: s}` at `a := Array(U8)` holds a base and a length **inline**:
`manifestNative` puts the view row above the reference-value row, so the field's own address already
names the object. `Builder::fieldPlace` builds every non-owned field as a `Field` step followed by a
`Deref`, which is right for a field that holds a reference and reads the first element's bytes as an
address here - twenty cases of the library's suite segfaulted in `pushHex`. The test is made at the
`Deref`, in `addressOf`, because a generic body is where the field is projected and `a` is open there:
the substitution is known at this tier and nowhere above it, which is the same argument `deriveView`
rests on.

The shape's own half of that is one tier up. An occurrence carries a root contract *and* an interior
shape, and `substituteOccurrence` moved only the first - so `fn (ShowWith(s, a)) with(value: 'a, style:
's)` at `a := Array(U8)` produced an instantiation whose *field* was the descriptor and whose *shape
slot* still said whole value, and `fieldContract` prefers the shape. `substituteShape` in `gen.cpp` is
the missing half, and it applies to a **field** step only: a slot ending at a *pointee* -
`valuesMut(&self: source'[a]) -> %source'a` - names storage the pointer reaches, and storage of a
container is the owner however the position that hands it out is written.

## The globals the compiler fills

`Lowering::declareGlobal` carries `supplied.def`'s `BuiltinKind::Written` rows across to
`LowerModule::builtins`, which is the three `commandLine*` cells the process entry stub stores argc,
argv and envp into - `genCommandLine` in `codegen/x64/emit.cpp`, shared with the shipping stage. The
lowered global and not the name, for the reason the shipping stage's own lowering gives: what a
backend needs is the global, the name it happens to have is the library's, and this is the one place
both halves are in hand. A `BuiltinKind::Supplied` row is not one of these - its value went into the
initializer the moment the declaration existed. Only a reachable global gets a row, which is the
intended answer: a program that never asks about its arguments leaves them null and the stub emits
nothing. This is what makes the library suite's `--only`, `--isolate` and `--list` work under this
stage, and therefore what makes a failure sweep case by case possible at all.

## A promoted slot may declare less than an arm hands it

`opt/promote.cpp` empties the interior **shape** where a slot's loads and stores disagree about one,
and its own note is the argument: two definitions of one slot may have been rooted differently -
`at = at + 1` beside a global's own pointee origin - and the empty shape is what a slot whose ends
disagree can honestly say. A shape changes no manifestation at this tier, so the block argument the
promotion declares accepts an arm carrying one. Declaring a shape and being handed none stays an
error: that is a promise the argument does not keep. Before the excusal, `Environment.variable`'s
`while` loop was rejected by the verifier after `promote` and every pass after it.
