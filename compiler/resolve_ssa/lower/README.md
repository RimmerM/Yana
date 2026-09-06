# `resolve_ssa/lower` - what a value is in bytes, and the walk down to the shared lower IR

The native half of the back end of the rewrite. Everything in the parent directory is the SSA
representation and the transformations over it - a statement about the program rather than about a
machine. Everything here is a statement about **one** machine: `repr.cpp` answers what a logical type
is in addressable storage, `manifest.cpp` reads a contract on top of that to say how a value of it is
carried, and `gen.cpp` and its five companions turn a verified `ssa::Function` into the `LowerModule`
both resolve stages hand to the backends.

The directory is the boundary rather than a naming convention. [`Analysis-IR-JS.md`
](../../../doc/proposal/Analysis-IR-JS.md)'s JSIR lowering is built against the same SSA and answers
the same two questions with `JsRep` instead of `Repr`, so it arrives as a sibling of this directory
and not as a set of switches inside it. It is also what makes an accidental dependency visible: a
file in the parent that needs `lower/manifest.h` is asking a native question from the
target-independent half, and its include says so. Two do today - `convert.cpp` asks
`isReferenceValue`, `isAddressOnly` and `arrivesAsAddress` to choose a value's convention, and
`verify.cpp` asks `arrivesAsAddress` and `manifestNative` to check a call's operands against the
signature - and both are the same open question Analysis-IR.md §13 asks, of whether a convention is
decidable before a target is chosen.

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
arm and for every instantiation. `Repr::discriminantOffset` is the one place that says so and
`InstFieldAddress::kDiscriminant` is the projection that asks - a reserved field index rather than an
instruction of its own, because it *is* the same instruction and a second row would be a second place
that knows where a tag sits.

Where the payload leaves patterns it cannot produce there is no tag at all - see the folding section
below, which is the case `Repr::discriminant` exists to tell apart from this one.

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

**Which of the two `teardownHalves` builds is asked of the halves, and of both of them.** The pair
that decides it is the one `emitTeardown` computes - whether the *derived* walk runs for each half
being emitted - so it is computed the same way at both sites. Reading the drop half alone was wrong
in both directions and one of them hung: `Maybe(String)` owes nothing at last use and owes its
payload's storage, so its drop is `None` and its reclaim is `Derived`; the test failed, the sum went
through `emitTeardown` instead, that walk answered a sum with a call to the per-type function, and
the function it named was the one being built. `teardown(Maybe(String))` was one `call` to itself,
and every program holding one recursed until the stack ended. The authored halves are emitted before
the branch rather than left out of it, which is the other direction - `emitSumTeardown` recurses into
the members and nothing else, so a sum with an authored instance of either half used to lose that
call.

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

## A layout table, and the four things it does that a walk does not

`repr.cpp` is the native answer to `repr(logical type, target)`, and it is a **family** rather than a
parameterized walk. The shipping compiler computes both targets' layouts from one `Repr` with a
`ReprTarget` of numbers and flags, and the strain shows in its own struct: `NicheKind::Absent` marks
a niche whose offset, width and range all mean nothing, and a JS `ReprTarget` carries `pointerSize =
8` for a machine with no addresses. Those are two readings of one struct. So the answer here is per
*value model*: every number in `Repr` is a statement about addressable storage, and a host target
whose values are host values answers with `JsRep` (Analysis-IR-JS.md §3.2) instead - a different
type, not this one with different numbers.

What is deliberately **not** per family is the key (a whole `mir::Occurrence`, on every target), what
the language decides (`RecordType::pin`, `RecordType::layout`, the view table, the containment cut -
all on the logical type in `resolve_mir`), and the cost model. That last one is the trap this avoids
by name: the shipping compiler's `TargetFamily` lives *inside* `ReprTarget`, and its four readers are
`opt_inline`, `opt_closure`, `opt_scalar` and `opt_discharge` - none of them a layout question.
`NativeTarget` has no such field.

Four switches on `NativeTarget` change a layout rather than a number inside one. They are switches
because each is a claim the access path has to be able to serve, and the pairing has to be visible:

| flag | what the layout does | what the access does |
| --- | --- | --- |
| `packFields` | a run of narrow fields shares one word - `placeArm` | `decodePacked` / `encodePacked` |
| `scalarizeRecords` | a whole record is one integer - `scalarizeRecord` | the same pair, one level up |
| `foldNiches` | a sum's tag is a pattern its payload cannot produce - `foldNiche` | `decodeFolded` / `encodeFolded` |
| `bitTagSums` | a sum's tag is bits above its payload - `scalarizeSum` | the first pair again, on the tag |

The last one is a smaller feature here than it is in the shipping compiler and a bigger one in a
different direction. There a tag word is four bytes, so putting the tag above the payload's whole
*storage* is a large win and the payload keeps its own layout; here a tag word is already one byte,
so that version never pays - `alignTo(1, payloadAlign) + payloadBytes` is never worse than the next
power of two above `payloadBytes * 8 + 1`. What pays instead is putting the tag above the bits the
arms actually **spend**, which needs every arm to be co-packed - and then the whole sum is a scalar,
so it co-packs into a parent, folds into a `Maybe` and is borrowed as a bit range like any other
narrow value. `data Pick = Left(Bool) | Right(Rank)` is one byte where the shipping stage's is two,
and a record of two of them is one byte where the shipping stage's is four. `BitTag.yana` carries
both dumps.

The niche **search** runs whichever way the last is set, because a niche is a fact about a type and
folding is a decision about a sum. That is what let the search be reviewed and asserted -
`Repr.yana`'s `.repr.expect` against its `.mir.repr.expect` - before the lowering that spends it
existed.

### A component with no address

A co-packed field and a folded tag are the same problem seen twice: `field.addr` produced a value the
tier above treats as an address, and there is nothing at that address to load. `VirtualPlace` is what
joins the two halves - the projection records what the component really is, and the `load`, `store`
or `copy` that follows spends it. It is the shape `PropertyAccess` already had, for the same reason:
only the second instruction knows which direction it is going.

Four things follow, and each of them is a case that would otherwise be silently wrong.

**The shifts compose.** `p.flags.optionA`, where `flags` shares a word with its neighbours, is *one*
load and one shift by the sum of the two placements - a scalarized record's fields all sit at offset
zero of it, so a field of a packed value is at the sum of the two bit offsets in the same word.
Nothing is dereferenced and no temporary is made. `PackedNested.yana` is what says so.

**A packed value whose type is an aggregate is read into storage of its own.** `Flags` has no
register form, so `decodeIntoStorage` puts its bits where they sit at shift zero and everything below
reads them the way it reads any record. That is a read-only copy and is sound because of where a
write goes: an assignment reaches a field through the projection, never through the value a load
produced.

**A `&` of one carries the shift beside the address** - Design.md's tier 2, `borrowNarrow` and
`unpackNarrowRef`. A callee is compiled once: `nudge(&x: Field)` reaches a whole local, an unpacked
field and a field sharing a word with three others, and the only thing it is missing about any of
them is *where* the value starts - the unit to load and the mask to apply both follow from the type
it was told. So the shift travels in the sixteen bits of a pointer no user address uses
(`NativeTarget::addressBits`), and folds away entirely where it is zero, which is every borrow of
anything the target left alone. Whether a pointee carries one is `Repr::isNarrow` and nothing else,
which is what makes it an ABI rather than a per-site decision.

The split against the callee's unit is where the straddle rule is spent, and it is the only thing
that rule is for: `packBits` guarantees a field lies inside one `storageBitsFor(bits)` block of its
word, and that composes through a nested packed record because a record of more than eight bits is
only ever placed at a multiple of its own storage unit - the fields go widest first, and a record of
`n > 8` bits has `n > storageBitsFor(n)/2`, so nothing narrower fits in front of it inside one unit.
The exception is a `C` pin, where the unit is the *declared* type's and a four-bit field may sit at
bit six of an `int`; a `&` of one is refused, which is what C does about the address of a bit-field
too.

**Design.md's tier 1 is still here, for the one boundary a shift cannot cross.** A body compiled once
reaches a value of a type parameter through a plain address and sizes it out of a descriptor, so a
narrow reference handed to one is copied into storage of its own and copied back when the call
returns - `materializeNarrow` and `commitPackedBorrow`, at the call argument rather than at the
borrow, because that is where the position's declared type is. A call with no declaration to read
(one through a function value) is flattened on the same terms, which is conservative in the one
direction that is safe: a temporary is a real address at shift zero, and a callee expecting either
form reads it correctly. The word is re-read at the write-back, which is what stops two co-packed
fields flattened across one call from losing an update.

**Fresh storage is zeroed where a word has spare bits.** A packed word is filled by read-modify-
writes that preserve everything outside the field, so the bits above the run are whatever the storage
held - and the niche the record publishes is made of exactly those bits. `hasPaddedWord` is the
question and `frameStorage` is the one place it is asked.

## `@box` is three operations, and the third one has to ask

`box.cpp` is the whole of `@box` below the MIR - `mir::RecordField::boxed`, and the parent
directory's README for why the indirection appears nowhere above this file. The three operations
are making a box, reaching through one, and releasing one, and only the last is interesting.

Making one is `createBox`: `allocateHeap` at the target's **stride**, the pointer stored into the
field's own slot, and the value installed through the address that came back. Reaching through one is
one load, in `fieldAddress`, which is why every consumer - a read, a write, a borrow, a teardown -
crosses the indirection without knowing it is there.

Releasing one is a **call** rather than an expansion, and there are two independent reasons, either of
which would be enough. It needs control flow that `emitTeardown` has none of; and a recursive type
reaches its own teardown through exactly this edge, so the memo on the generated function is what
makes `data List = Nil | Cons {tail: List}` terminate instead of inlining forever.

### Who knows where the storage came from

`BoxRelease` in `internal.h`. Handing an allocation back is only correct where the program made it,
and that is a question about **how the release site reached the value** rather than about the value's
type:

| | |
| --- | --- |
| `Owned` | the placement is visible - a value this frame owns, or one reached through a descriptor built for a placement. Unconditional `freeHeap`. |
| `Unknown` | the site was handed bytes and a type and nothing else, which is an overwrite through a `&` borrow. The caller may have placed the value in a region and its boxes with it. |
| `Foreign` | the placement is visible and is not the program's - a region discharging everything inside it in bulk. Nothing is emitted. |

`InstDrop::boxesVisible` is where the first two are decided, off the **root** of the place the
elaboration dropped: an owned local is this frame's, and a borrow, a global or a raw address is
somebody else's. It is on the instruction and not on the type because two drops of one type in one
function can differ in it.

The erased path is deliberately not the ambiguous one. A descriptor is built by its caller for a type
*and* a placement, so `teardownFor` reads the placement it was keyed on and answers `Owned` or
`Foreign` - which is the open question the note under `teardownFor` used to leave, answered.

### The two compilation modes

`Unknown` is the case with nothing to read, and `known.def`'s `isHeapAddress` is the two ways a build
answers it. `storesBoxProvenance` is the switch, and it is a property of the **program**: which
provider a build has is settled by `@allocator` and the file-name axis during declaration reading, so
it is a constant long before any layout is computed and nothing can memoize the wrong one.

- **A provider that answers `isHeapAddress`** recognizes its own allocations from the address - a
  reserved aligned window, or a page directory beside it. The compiler stores nothing: the release is
  `if isHeapAddress(p) then freeHeap(p)`, inside the generated function, and a record with a boxed
  field is exactly its fields.
- **A provider that does not** gets `Repr::boxFlagsOffset`: one bit per boxed field, in a word placed
  after every field, written by `createBox` and read by `emitBoxTeardown`. The bit lives in the
  *enclosing record* and the callee is handed only the target's address, so the conditional release
  takes it as a second argument - which is the only place the two modes differ in a signature.

Placing the word after the fields rather than among them is what makes the pair comparable: every
offset above it is the offset the record would have had with no word at all, so the two modes'
layouts differ by a suffix and by nothing else. `test/resolve/ownership/Box.Stored.yana` and
`Box.Classified.yana` are one program under each, and their goldens differ in the record's size, in a
store at construction, and in the shape of the release.

## Lowering is seven files

`gen.cpp` is the shape a function has before any of its code exists - globals, signatures, block
structure, phis, the driver - which is also everything that has to be decided before the first call
can be built, since a call names a signature another function declared. `inst.cpp` is one
instruction at a time, plus `emitTeardown`, `fieldAddress`, `frameStorage` and `install`. The split
is where the *questions* differ rather than where the line count did.

Two things about the driver are worth knowing before changing one. **The block walk takes the shared
reverse postorder** (`../dominance.h`) rather than the table order: a `BlockId` is handed out at
creation and nothing keeps that in step with the graph, so a block appended after the block that
reads its values fails a walk that maps values as it goes. The blocks the entry cannot reach are
lowered last rather than skipped, because their lower blocks exist and each owes a terminator and
whatever phi alternative its edges promised. And **the declared ranges are hinted in one place**,
`hintDeclaredRanges`, over the finished value mapping: `LowerType` is a register class, so an
unsigned type's width and a payload-free sum's tag range are facts the lower passes would otherwise
have to reconstruct from instructions - and cannot, for an argument or a phi.

Behind both of them is `lower/lower_pipeline.h`, the run of shared lower passes both resolve stages
owe their output. This stage called one member of it - `makeDivisionTotal` - until phase 1 of
[`../opt/README.md`](../opt/README.md).

`erase.cpp` is the third, and it is the ABI a generic body compiled **once** is called across.
`fun.cpp` is the fourth, on the same terms: the two words a **function value** is, and the thunk
that makes a plain function callable across them.

`width.cpp` is the fifth and is the whole of what a value narrower than its register costs.
`pack.cpp` is the sixth and is the whole of what a component with **no address** costs - see the
next section. `box.cpp` is the seventh and is the whole of what a component with an address **of its
own** costs, which is the section above.

## A narrow value is held in normal form

A `U8` is one byte of storage and a *thirty-two bit register*, and those are two numbers rather than
one. There is no eight-bit register in the lower IR and there should not be - a machine adds in its
whole register whatever the program declared - so every operation on a narrow value happens at 32 or
64 bits, and something has to say what the bits above the width hold.

**Sign-extended where the type is signed, zero-extended where it is not.** Every producer leaves a
value that way and every consumer may assume it, and `width.cpp` is the first half of that
sentence. What rests on the second half is worth naming, because none of those readers has a mask of
its own and none of them should: a **comparison** at a `U8` is a 32-bit comparison and is right
exactly because both registers hold the byte and nothing above it; a **widening** is a `cast` that
extends whatever the register held; a **store** writes one byte and discards the rest, which is why
the gap was invisible for so long - `(200 :: U8) + 100` read back out of a local is 44 and compared
in a register is 300.

Which operations need it is not decided here. `resolve_mir/op.def` has a `width` column -
`WidthEffect` - because "can the result leave the range" is arithmetic and true on every machine;
what this tier adds is the target's half, whether the register is wider than the range at all. Four
treatments, one per operation: nothing, wrap the result, mask the operand *then* wrap the result
(`shr`, the one operation that reads a narrow value's storage rather than its value), and the shift
pair a `rol` at a narrow width has to be - the machine's rotate instruction turns within its whole
register, so the definition is emitted instead of the instruction.

**`@bits(n)` arrives here as an ordinary narrow value**, and that is the whole of what this tier
does about it. `resolve_mir` decides where a refinement is widened and where it is narrowed -
`doc/spec/repr.md`'s "load always widens, store always narrows" - and hands over a `convert` whose
*result* type declares fewer bits than its register; putting that back in range is the same mask, or
the same `shl`/`sar` pair, a `U8` addition gets. So a refinement needs no rule of its own here, and
the one place it would need one is packing: a field sharing a word is a shift and a mask against a
*cell* rather than a value in a register, which is `propertyPlace` in `erase.cpp` and `decodePacked`
in `pack.cpp`. Those are one piece of arithmetic asked with the shift from three different
places - the field's own `FieldRepr`, a property slot in an erased body, and a narrow reference -
and `VirtualPlace::shift` is what makes them one: a shift that is a constant and a shift that is a
value are the same place, so the decode and the encode are written once and the three sources differ
only in where the number came from.

It is also not a fold: the mask on a value already known to be in range is removed by the builders'
own constant folding and by the backends, not by a test here.

**`bitcast` needs the normal form re-established and `op.def` cannot say so**, which is the one place
the width column and this tier's question come apart. The column asks whether the arithmetic can
leave the type's range, and a bitcast's result is the same bits under a type of the same width - so
`Closed` is right there. What changes is which extension the register is *required* to carry, and a
bitcast between the two readings of one narrow width is exactly where that flips. `inst.cpp`'s unary
arm therefore puts a bitcast in range beside the column's answer. It was invisible for as long as it
was because a store discards the register above the byte, so writing the value to a local repaired
it; `BitcastNarrow.yana` is the fixture, and the shipping stage carries the same correction with the
wrong answers it produced.

## A vector is a register value, and an atomic is a location

Two of `resolve_mir`'s type kinds reach the manifestation table with an answer neither a record nor a
scalar gives.

A **vector** manifests as the lower IR's own `LowerType` - the lane kind and the count, carried
across the seam unchanged - and is *not* `inMemory`. That last part is a contract rather than an
observation: Design-Vector §2.5 states that a vector is carried in a register and binds every calling
convention to it, so a backend with no register that wide has to refuse the type loudly rather than
quietly spill one, and an `inMemory` here would be that refusal made silently at every call. Its
alignment is its width, because a vector move is one instruction against a naturally aligned address
on every target that has one. `resolve_mir` has already spent the natural lane count, so what arrives
is a number and the translation is a lookup.

An **atomic** is its content's bytes at its content's width and **is** `inMemory`, which is the one
place this table departs from what the content alone would answer: an `Atomic(I32)` is four bytes
like an `I32` and is reached the way a record is, because it is a *location* rather than a value and
a register copy of one is the second location the type spends itself refusing. That is what lets
`InstAtomic`'s first operand be the atomic itself rather than an address - an SSA value of an
in-memory type already is one - and it is why `atomic(x)` lowers to frame storage and an ordinary
store while every operation between takes the address it already has.

Two things follow in `inst.cpp` and are easy to get wrong. **Signedness is the lane's**, not
the operand's: a division of two `Vec(I32)` is a signed division per lane and a comparison of them is
a signed comparison per lane, which `arithmeticLane` is the one place that asks - a vector answering
"not signed" made `v / 3` unsigned and every lanewise ordering wrong. And a **comparison of two
vectors states its result type**, because it answers a mask where a scalar comparison answers a
`Bool`; the lower IR's validator refuses one that does not, which is how the omission was found.

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
also why the environment parameter is emitted ahead of the hidden result pointer in `gen.cpp`: it
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
| a descriptor is two cells: all five metrics in one, then teardown | `kDescriptorMetrics`, `kAlignShift` |
| a metrics cell is a size, two flags, and `align - 1` on top | `kAlignShift`, `kDescriptorTrivialCopy` |
| a stride is `alignUp(size, align)` and not a cell | `metricValue`, and the note at `kDescriptorMetrics` |
| a size is **not** rounded up to the alignment - the padding is the container's | `manifest.cpp`, `layoutRecord`, and `strideOf` beside it |
| every metric is asked in one place, immediate or descriptor read | `metricValue`; `sizeValue` is it with the metric written in |
| a descriptor is keyed by (type, **placement**), because a teardown is both | `descriptorFor`, `teardownFor` |
| a property slot is one of three shapes, chosen by the declared field type | `PropertyForm` |
| a field that fills its storage is a byte offset and an `add` | `readProperty`'s first branch |
| a **narrow** field carries its bit shift above the offset, and is read-modify-written | `kPropertyShiftShift`, `propertyPlace` |
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


## Native only

JavaScript is deliberately not a consumer of `gen.cpp`.
[`Analysis-IR-JS.md`](../../doc/proposal/Analysis-IR-JS.md)'s JSIR is built against SSA from the
start rather than by teaching this walk a second target, which is how the current JS backend acquired
representation questions of its own. `checkSettings` refuses `-resolver ownership` on a JS build
rather than quietly producing a program by the other stage.
