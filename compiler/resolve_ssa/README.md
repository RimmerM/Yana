# `resolve_ssa` - ownership SSA, and the way down to the shared lower IR

The back half of the rewritten resolve stage. `resolve_mir` produces checked MIR; this converts it to
ownership SSA, verifies that, and lowers it to the `LowerModule` both stages hand to the backends.

The design is [`doc/inflight/Analysis-IR.md`](../../doc/inflight/Analysis-IR.md) part 7, and part 4's
diagram is the order these files run in. What it calls OSSA is what `ssa::` is here - see
`resolve_mir/README.md` for why the code drops the `O`.

## The whole ownership model at this tier

Every value has one of four conventions and every use has one of three kinds. That is all of it:

- an `Owned` value is consumed exactly once on every reachable path, including by a drop;
- a `Guaranteed` value may not outlive the loan it depends on;
- a `Trivial` value is unrestricted;
- an `Address` is not an owner at all.

That surface is small enough that an optimizer pass can be *required* to preserve it rather than
asked to, which is the reason for the tier. `verify.cpp` checks it after conversion and is meant to
run after every ownership-sensitive pass in an assertion build.

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
| whether a type has anything to run when it dies | `resolve_mir/type.cpp`, `hasTeardown` |
| what a place's address is | `convert.cpp`, `addressOf` |
| what a value looks like on the target | `manifest.cpp` |
| how a constant becomes bytes | `lower.cpp`, `materialize` |
| where an object lives | `resolve_mir`, and carried down to `InstAlloc` |
| what a callee receives an argument as | `resolve_mir/inst.cpp`, `argumentMode` |
| whether a call may suspend | the callee's `Summary`, through `resolve_mir`'s `maySuspend` |

`manifest.cpp` is [`Analysis-Borrows-Revised.md`](../../doc/proposal/Analysis-Borrows-Revised.md)
§1.3's `manifest(logical type, contract, target)`. It takes the **whole occurrence** - type, root
contract and interned interior shape - rather than reading anything off the type: an owned `String`
and a shared window into one are the same logical type and different representations, and that is the
case this table exists for.

Taking a contract *parameter* was not enough on its own, and the way that failed is worth keeping. An
`ssa::Value` used to carry only its logical type and its convention, so every caller in `lower.cpp`
had nothing to pass and passed `ownedContract()` - which would have manifested a shared window as its
owner the moment one existed. `Value::occurrence` is what closes it: the tier retains what the table
needs, and the signature asks for exactly that.

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

## A `{}` call result is absent, not present and unmapped

The MIR call node always has a result occurrence, because a signature always declares one. A `{}`
manifests as no bytes, so there is no register for a value to be in and nothing below could map one -
so the conversion gives such a call no `ssa::Value` at all. That is the loud choice rather than the
quiet one: an operand that tries to name it fails here, with a source position, instead of lowering
to a null that the machine walk silently drops.

## Drop elaboration happens during conversion

Not as a pass that rewrites MIR first. §9.2's economy: a round that inserted instructions into MIR
only to copy them immediately is a round that can be skipped. What the conversion reads is
`Body::elaboration` - the drop and flag steps the ownership analysis placed, both lists sorted by
`(block, position)` so the walk carries one cursor each rather than searching per node.

`InstDrop` says a life ends and nothing more. What it *expands into* is a target question and is
`lower_inst.cpp`'s `emitTeardown`: the derived walk over the members, an authored instance call, or
both for different halves of one type - see `resolve_mir/README.md` on why `Drop` and `Reclaim` stay
separate all the way down.

A conditional drop is a branch on a flag slot, built as two blocks and a merge exactly as an `if` in
the source would be: a teardown behind a condition is control flow, and giving it a shape of its own
here would be a second thing for every later pass to understand. The address is computed *before* the
branch so that it dominates both arms. Nothing in the current language subset produces one - the MIR
README says why - so this path is written and untested.

## Records are address-only, and that is the whole calling convention

`manifestNative` answers `inMemory` for a record: `LowerType::Pointer`, with the size and alignment
`layoutRecord` folds out of one walk over the fields. Everything else follows from that one bit:

- a local of such a type is an `alloc` and a use of it is its address;
- an argument of such a type is passed as an address;
- a *result* of such a type comes back through storage the caller supplies, so a memory-returning
  function takes a hidden pointer as its **leading** argument. It is leading because that is what the
  shared IR's other producer does, and two producers that disagreed about the position would build
  calls no callee could read. `Lowering::returnPlaces` holds it per function rather than recovering
  it from the argument list, so "does this function have one" and "is its result address-only" cannot
  come to differ.

`InstLoad` and `InstCopy` differ in *meaning* rather than in machine form: a load is "the object at
this address" and a copy is "a second one". Which instructions that becomes is the manifestation's
answer, asked once - a load of an in-memory type is the address itself, and a copy of one is an
`alloca` and a block copy. A representation-shaped encoding would have put that question in the IR.

### `inMemory` and "is an address" are two questions

`isAddressOnly` asks whether the value's own bytes live at the address it carries; `isReferenceValue`
asks whether the value *is* an address. An owned record answers yes to both. A **loan** answers yes
to the second and no to the first, and one flag answering both is a real defect this cost: a record
field holding a shared loan of a four-byte `Cell` was installed by block-copying the lender's object
into a pointer-sized slot.

A loan manifests as a pointer when the source published it - exclusive access, whose whole contract
is writing through to the same storage, or an external-origin qualifier, which "must now retain an
address" - and otherwise exactly when the object itself is one. A plain shared loan of a trivially
copyable type is not among them, because the spec copies at that position and `readMode` never
produces a loan there for this table to represent.

A loan **local** gets no slot of its own: its `Init` produces the address, and the local aliases that
value. Giving it a slot and storing into it is how `let d = c` came to allocate a second `Guard` and
read the address back as an object.

A `Copy` whose only use is a `Store` currently copies twice: once into its own slot and once into the
destination. Destination passing is §5.6's later optimization and is deliberately not attempted here,
because the conversion would have to know what a value's single use is - which is an analysis, not a
translation.

## Lowering is two files

`lower.cpp` is the shape a function has before any of its code exists - globals, signatures, block
structure, phis, the driver - which is also everything that has to be decided before the first call
can be built, since a call names a signature another function declared. `lower_inst.cpp` is one
instruction at a time, plus `emitTeardown`, `fieldAddress`, `frameStorage` and `install`. The split is
where the *questions* differ rather than where the line count did.

## Locals are storage, and promotion is somebody else's job

A MIR local becomes one `Alloc` in the entry block and the reads and writes of it become `Load` and
`Store`. Nothing here tries to hold a scalar in a value instead: the lower IR's own
`promoteStackSlots` runs over these on the way to machine code, so a local that is never borrowed
costs nothing in the end, and doing it during conversion would make the SSA dump depend on an
optimization - which is the one thing a fixture about ownership should not have to read around.

`StorageClass` states three rungs and reaches one. That is Analysis-IR.md §10.4's requirement rather
than an unfinished switch: the current compiler's equivalent reserves the region enumerator and
never selects it, which left the decision shaped like a two-way choice, and stating three from the
start means the escape analysis ranks them instead of learning a third answer.

It is `mir::StorageClass` and is decided in MIR. Frame, region and heap placement is what ownership
analysis answers - whether a local escapes its frame, whether it outlives the call that made it,
whether an ambient region witness is in scope - and all three are questions about the MIR the escape
analysis runs over. This tier carries the answer to `InstAlloc`; it does not make one, and a second
enumeration here would be a second place to add a rung to.

## Native only

JavaScript is deliberately not a consumer of `lower.cpp`.
[`Analysis-IR-JS.md`](../../doc/proposal/Analysis-IR-JS.md)'s JSIR is built against SSA from the
start rather than by teaching this walk a second target, which is how the current JS backend acquired
representation questions of its own. `checkSettings` refuses `-resolver ownership` on a JS build
rather than quietly producing a program by the other stage.
