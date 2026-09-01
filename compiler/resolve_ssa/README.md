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
| whether an owned value is linear | `resolve_mir/type.cpp`, `isLinear` |
| whether a type has anything to run when it dies | `resolve_mir/type.cpp`, `hasTeardown` |
| what a place's address is | `convert.cpp`, `addressOf` |
| what a borrow depends on | `convert.cpp`, `attachLoan`, from the origin environment |
| what a value looks like on the target | `manifest.cpp` |
| how a constant becomes bytes | `lower.cpp`, `materialize` |
| where an object lives | `resolve_mir`'s `Object`, through `storageOf`, carried to `InstAlloc` |
| what a callee receives an argument as | `resolve_mir/inst.cpp`, `argumentMode` |
| whether a call may suspend | the callee's `Summary`, through `resolve_mir`'s `maySuspend` |

`conventionFor` asks `isLinear` and not `hasTeardown`, and the two rows above are separate for that
reason: they are different questions with the same answer for every type that exists, and the type
they differ on is the one an obligation is for. See `resolve_mir/README.md`'s ownership table.

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

### A sum is a tag and one shared payload

`layoutRecord` places a discriminant first where the declaration has several constructors, then lays
**each arm from one payload offset** and sizes the record by the widest. First is a decision rather
than an accident: what reads a tag is a match, and a match on a value it has proved nothing about may
only touch bytes every arm agrees are the tag, so putting it first makes that one address for every
arm and for every instantiation. `discriminantOffsetNative` is the one place that says so and
`InstFieldAddress::kDiscriminant` is the projection that asks - a reserved field index rather than an
instruction of its own, because it *is* the same instruction and a second row would be a second place
that knows where a tag sits.

The tag has a real `mir::IntType` (`tagTypeOf`), which is what keeps it an ordinary value everywhere
above the target: the place that names one carries it as its occurrence, the comparison a pattern
emits is an ordinary `Binary`, and the storage width follows from the type the way every other
integer's does. How wide and whether it is signed is `tagRangeOf`'s single answer over the
constructor list - `@value(n)` is what makes that a walk rather than a count, since a pinned value
may be sparse, may be negative and may not fit in thirty-two bits.

An **enum-layout** record - several arms, none carrying anything - is that tag and nothing else, so
`manifestNative` gives it a register form rather than an address. It is the one refinement no kind
can make and it is a *decision* rather than an observation, on the old stage's `isDirectType` terms:
resolve states it and the calling convention is bound by it. Nothing about matching changes, because
reading the tag is still a projection through the record's own storage and every local has some.
`Manifestation::isSigned` exists for the same row: whether a load of those bytes sign-extends is a
question about `@value(n)` rather than about `Type::Record`, and three sites used to derive it from
the logical type and would all have answered `false` for an enum counting up from below zero.

A sum's **derived teardown is a call**, to the per-type function `teardownHalves` builds. `emitTeardown`
is a walk with no control flow - it emits into a block its caller is in the middle of - and which arm
to release is a question about the value, so the branch goes inside a function that has blocks to put
it in. `emitSumTeardown` is that branch: one arm per constructor that owes anything, in declaration
order, tested against the tag.

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

## Lowering is four files

`lower.cpp` is the shape a function has before any of its code exists - globals, signatures, block
structure, phis, the driver - which is also everything that has to be decided before the first call
can be built, since a call names a signature another function declared. `lower_inst.cpp` is one
instruction at a time, plus `emitTeardown`, `fieldAddress`, `frameStorage` and `install`. The split is
where the *questions* differ rather than where the line count did.

`erase.cpp` is the third, and it is the ABI a generic body compiled **once** is called across.
`lower_fun.cpp` is the fourth, on the same terms: the two words a **function value** is, and the
thunk that makes a plain function callable across them.

## A function value is two words, and `code` always takes the environment first

`{code, env}`, sixteen bytes, address-only. A plain function referenced by name, a lambda that
captured nothing and a lambda that captured five things differ only in what the second word holds,
so a call site holding one never has to know which of the three it is - which is what lets a
function type be a type rather than a family of them.

That uniformity costs a **thunk** for the plain case, because a declaration has its own convention
and something has to absorb the environment argument. `funThunkFor` builds one per declaration, and
it is a forwarding call: unlike `erase.cpp`'s witness thunk it adapts no positions, since a function
type's positions are concrete by construction.

A **lifted lambda** needs no thunk at all, which is the same sentence read from the other end: it was
built with the environment as its first parameter, so it already has the shape a function value is
called across. The thunk exists to give a *declaration* that shape, not to give the pair one. That is
also why the environment parameter is emitted ahead of the hidden result pointer in `lower.cpp`: it
is a declared parameter of the MIR function, because the body reaches its captures by projecting it
and a projection needs a place, but it is not a parameter in the declared *order* - argument zero is
argument zero, and a closure whose result came back through storage would otherwise put the pointer
that describes the call ahead of the one that answers it.

**Argument zero is one slot, not two.** It means "what the callee needs that its arguments do not
say": a generic environment for a body compiled once, a capture environment for a closure. One
function value can never want both, because a lambda over an erased body reaches it through a thunk
that supplies the generic environment out of its own captures - so `CallThrough::Value` fills the
same leading argument `CallThrough::Declaration` does for an erased callee, and the call lowering
has one shape for four call forms.

**A function value owes no teardown, and that is a statement about placement rather than about
function values.** An environment is a local of the frame that built the closure
(`resolve_mir/resolve_lambda.cpp`), it holds copies and borrows, and neither owes anything. The
capture that would make one own something is the spec's **consumed** capture - and a closure that
consumes a capture is precisely one that has to outlive its frame, which is the heap placement
`mir::verifyBody` refuses for want of an allocator. The two therefore arrive together, and a release
helper written before they do would read a header word nothing writes.

It is still not trivially copyable, and the two are different questions: a closure may hold an
exclusive loan of a capture, so duplicating the bytes would be a second exclusive capability over one
object. Its uses are moves, and nothing runs when it dies.

## The erased ABI lives at the bottom, in one file

A body compiled once cannot read a layout out of its types, because its types are parameters. What
it reads instead is an **environment**: a run of four-byte cells per slot of the caller's
`mir::GenSchema`, in that numbering, holding the answers a substitution would otherwise have
supplied. A descriptor pointer, a count, a class witness, a property witness, a function pointer -
the five slot kinds. Every one of them is one cell except a **count**, which is as many as the type
it counts in needs: `data Buf(n: I64)` is a legal declaration, and a slot narrower than the number it
carries is one the body reads a neighbour out of.

A cell that holds an **address** holds `target - &image$base` as a signed 32-bit number rather than a
pointer, which is what keeps it four bytes - `repr/table.h` argues the encoding against the two
alternatives, and the short version is that absolute cannot serve the JIT and self-relative cannot
serve an environment built on the frame. It costs one `lea` per body, which is pure and shared across
every cell it reads; it buys half the bytes and a table whose contents are final when the image is
assembled, so identical bytes serve an ELF executable and a JIT buffer.

Every number of that arrangement is in `erase.cpp`, because both ends of every one of them are:
the caller that fills a table and the body that loads out of one are two functions in that file
rather than two files that have to agree.

| what | where |
| --- | --- |
| a cell is four bytes, and an address cell is anchor-relative | `kSlotBytes` |
| a count fills as many cells as its type needs, and is read at the slot's width | `slotBytes`, `numberSlot` |
| a number is written in the target's byte order | `writeNumber`, `NativeTarget::byteOrder` |
| a descriptor is two cells: size with alignment above it, then teardown | `kDescriptorMetrics`, `kAlignShift` |
| a stride is `alignUp(size, align)` and not a cell | the note at `kDescriptorMetrics` |
| a descriptor is keyed by (type, **placement**), because a teardown is both | `descriptorFor`, `teardownFor` |
| a property slot is one of three shapes, chosen by the declared field type | `PropertyForm` |
| a field that fills its storage is a byte offset and an `add` | `readProperty`'s first branch |
| a **narrow** field carries its bit shift above the offset, and is read-modify-written | `kPropertyShiftShift`, `narrowAccess` |
| a field of an **unknown** type is a `{read, set}` pair on the `simple` convention | `propertyWitnessFor` |
| a field requirement decides its own result type | `fillFromProperty` in `resolve_mir` |
| the environment is argument zero | `Lowering::run`, and `lowerBody` reads the same offset |
| a dynamically sized allocation is 16-aligned | `kDynamicAlign` |

**A concrete caller adapts at every erased position, and the thunk undoes it.** A body compiled once
reaches a value of a type parameter through a pointer, so a caller holding an `I32` spills it to a
frame slot before the call and reads an in-memory result back out of one afterwards; `thunkFor` loads
where the call site stores and stores where it loads. Neither touches a position the declaration did
not erase. `generic/ErasedScalar.yana` is what says so - and the reason it had to be written is that
every other fixture here binds `a` to a record, which arrives as an address already.

**A property slot is a number wherever a number can answer, and a call only where one cannot.** The
requirement declares the field's *type*, so a body compiled once already knows the access width and
its signedness; what it is missing is where. An offset answers that for a field that fills its own
storage, an offset with a bit shift above it answers it for one that does not - `Bool` owns one bit
of a byte, and every `@bits(n)` the library spells owns fewer than its unit - and only a field whose
*type* the body cannot see needs the access handed over as a function. Which of the three a slot is
in is read off the declaration at both ends, never out of the data, which is what keeps a field
access from being a branch; `isNarrowRepr` in `repr/repr.h` is the old stage's version of the same
rule for a `&T`. A specialization spends the requirement entirely - `convert.cpp` turns the place
into a plain `field.addr` at the substituted subject - so a specialized access is the instructions a
hand-written one compiles to, with no slot in them.

The accessor pair takes the **`simple`** convention, and that is what makes the one case that needs a
call affordable. `simple` passes in three GPRs and clobbers those plus `rax` and `xmm0-2`, leaving
every other allocatable register callee-saved; `complex` takes eight argument registers and clobbers
nine plus all sixteen vector ones, so a call through one forces the caller to spill everything it was
holding. Two pointers and no result is what `simple` was cut for. Both ends state it, because the
call is indirect through a cell and a callee's convention is not something an indirect site can look
up - see `codegen/x64/constraint.cpp`, where the tables are.

**Which teardown a descriptor names is decided by how the value was allocated, not by its type
alone.** A value in a region has had its reclaim half discharged in bulk and must not run it again; a
boxed one has storage to hand back after its obligations are met; a frame one has neither. So the
descriptor is interned on the pair and only its second cell differs - the old stage reached the same
conclusion at `NativeTypeDesc::kTeardown` and called it "a second descriptor" rather than a second
slot. `Frame` is the only placement anything selects today, and the release a `Heap` one needs is the
single piece with no allocator behind it.

**The tiers above stay symbolic.** MIR names a requirement and never a number, and so does SSA: an
`InstCall` carries `mir::CallThrough` and which requirement, an `InstFieldAddress` carries which
*property* and never a position, and `InstGenValue` names the parameter. Nothing above this file holds a slot index, which
is what the split of `GenContext` from `GenSchema` exists to make possible - a body that held one
would be a body whose meaning changed if a requirement were inferred after it was written. The
lowering is the numbering's one reader.

**A type parameter is address-only with a size that is not a number.** `manifestNative` answers
`inMemory` and `dynamic` for one, and `dynamic` is the flag no consumer may ignore: a size of zero
is a real answer that `{}` gives, and reading `manifest.size` on a dynamic manifestation allocates
one byte for an object of any size. `Lowering::sizeValue` is what asks instead, and it answers with a
*value* in both cases - a literal where the layout is known and a load out of a descriptor where it
is not.

**The environment is a table in the image, and that is a limit as well as a choice.** Every slot of
it is a link-time constant because this milestone builds one only where the argument list at the call
is concrete. A call inside an erased body that would have to *forward* slots out of its own
environment is refused in `callTarget` rather than reaching here with nothing to write; what it needs
is the same walk building the environment on the frame, which is a generalization of
`environmentFor` and not a different mechanism.

**Entry thunks are built here rather than in MIR**, and G5's own note guessed otherwise. A thunk has
no ownership content at all: it moves each argument from the form the table promises into the form
the callee declares and hands the result back the other way. There is no value whose life it decides,
no place it borrows and no teardown it could place. Built in MIR it would be a body the loan
analysis, the drop elaboration and the SSA verifier each had to have an opinion about, over storage
named by a raw address - `Place::Address`, the construct that exists so that nothing has to produce
one. What it costs is that a thunk does not appear in the MIR or SSA dumps, which is the right end of
the trade: what a golden should assert about one is its calling convention, and that is exactly what
those two tiers deliberately do not carry.

**A thunk is identified by the pair** - which signature is promised, and which function stands
behind it - and `thunkFor` builds the key out of both halves itself so that no caller can supply one
of them. Keyed on the promise alone, two instances of one class share one thunk and the second
instance's witness table points at the first instance's member: invisible in a dump, because a table
entry is a relocation, and invisible in a run wherever the two types share a layout. Witness tables
are interned on the same pair, for the reason descriptors are - two environments naming one instance
should name one table.

**A descriptor's teardown pointer is never null.** A type with nothing to run points at the module's
one empty function, so a drop in an erased body is one indirect call rather than a load, a test and a
branch - and there is no second rule about what a slot may hold. `generic/Erased.yana` is what
asserts it end to end, and the assertion is the *answer*: `trace` accumulates the identity of every
value that dies, so a teardown that ran through the wrong descriptor, ran twice or did not run is a
different number and is invisible in every golden. Verified by pointing the descriptor at the empty
teardown while still emitting the real one - the IR text matched and the answer was 26 against 39.

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
  per pair the linear rule compares.
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

The linear rule's own dataflow is a worklist over reverse postorder with the loan half as a bit
table, on the same terms as the MIR analyses - [their README](../resolve_mir/README.md#what-the-analysis-costs)
says why, and it is the same reason.

`manifestNative` memoizes a record's size and alignment on the record (`RecordType::nativeAlign`),
which is the one cache on a type in this stage. What it removes is the nesting: laying out a record
asks for each member's manifestation to place it, so a type three deep re-laid out everything below
it at every query. A field's *offset* is still a prefix walk, and stays one - the offsets are the
sizes and alignments of the members, so the walk is the arithmetic that produces them and a table
beside it would be a second answer to keep in step.

## Native only

JavaScript is deliberately not a consumer of `lower.cpp`.
[`Analysis-IR-JS.md`](../../doc/proposal/Analysis-IR-JS.md)'s JSIR is built against SSA from the
start rather than by teaching this walk a second target, which is how the current JS backend acquired
representation questions of its own. `checkSettings` refuses `-resolver ownership` on a JS build
rather than quietly producing a program by the other stage.
