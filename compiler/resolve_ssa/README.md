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

A `Guaranteed` value carries a `LoanId`. Without one, "may not outlive the loan it depends on" is a
rule with no term to check against, which is the shape lenses, yielded borrows, values live across a
suspension and external iterator handles all need. Nothing mints a loan yet, so the verifier states
the check rather than requiring it.

## Where each decision lives, exactly once

| Question | Answered in |
| --- | --- |
| what a MIR occurrence's convention is | `convert.cpp`, `conventionFor` |
| whether a type has anything to run when it dies | `resolve_mir/type.cpp`, `hasTeardown` |
| what a place's address is | `convert.cpp`, `addressOf` |
| what a value looks like on the target | `manifest.cpp` |
| how a constant becomes bytes | `lower.cpp`, `materialize` |
| where an object lives | `resolve_mir`, and carried down to `InstAlloc` |

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

## Drop elaboration happens during conversion

Not as a pass that rewrites MIR first. §9.2's economy: a round that inserted instructions into MIR
only to copy them immediately is a round that can be skipped. Nothing in this milestone has a
teardown, so there is nothing to emit yet; the placement rules are stated where they will attach.

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
