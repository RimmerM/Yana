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

## Milestone

Part 11.4's slices 1 and 2. A module of `let` constants and functions whose bodies are scalar
computation and control flow: literals, arithmetic, comparison, `Bool`, locals, `if` as a value and
as a statement, `while`, blocks, and block arguments on every edge that carries one. There is no
prelude - the width names and `Bool` are builtin, and Core's `Int`/`Long` are aliases over them.
Anything else in a source file is reported by name as not implemented, because this stage is chosen
by a flag over a corpus written for the other one and meeting an unimplemented construction is an
ordinary event.

Two things it deliberately does not do, and says so rather than guessing:

- **`::` ascribes and does not convert.** `x :: I64` on an `I32` is a legal widening in the language
  and is reported here as unimplemented, not as a type error - `Convert` is a class, and conversions
  arrive with instance selection.
- **Operators are a table, not instance selection.** `resolve.cpp`'s `kOperators` is a stand-in for
  `Num`, `Eq`, `Ord` and `Truth`, carrying `Core.yana`'s own fixity numbers so that an expression
  groups here exactly as it does under the other stage. Milestone 6 deletes it.

The one thing arithmetic does *not* decide is signedness: op.def has no column for it, because
`I32` and `U32` are two logical types over one `div`, and `resolve_ssa/lower.cpp` is where the two
become `idiv` and `div` with the target in hand.
