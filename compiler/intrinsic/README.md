# `intrinsic` - what the compiler and the source language agree about

Every place the compiler is fastened to a declaration, and every place a declaration is fastened to
the compiler, in one directory. The full argument is at the top of
[`role.h`](role.h); this is the shape of it.

## The five directions

A compiler that ships a standard library is coupled to it in five ways, and each is a table here.

| direction | table | example |
| --- | --- | --- |
| the compiler **creates** it and source names it | `primitive.def`, `constructor.def` | `I32`, `Size`; `Vec(a, n)`, `Atomic(a)` |
| source **declares** it and the compiler references it | `known.def` | `class Drop(a)`, `checkCondition` |
| source declares it and the compiler **implements** it | `op.def` | `pub fn *(it: %a) -> a` |
| source declares it and the compiler **fills** it | `supplied.def` | `@builtin(pageBytes) pub let pageBytes` |
| source declares the class and the compiler **supplies instances** | `provided.def` | `Num(I32)`, the conversion ladders |

## The link is written at the declaration

A declaration says which role it fills by writing `@builtin(role)`, and **the spelling the attribute
accepts is the enumerator stringified** - there is no name column anywhere for a table to drift from.
Adding a row makes `@builtin` accept its name; renaming one stops the old spelling being accepted, at
the declaration, rather than leaving a role quietly unfilled.

The alternative - a compiler-side table of library names - is that fact written a second time, and
the second copy is the one that goes stale. That failure is not hypothetical: renaming the library's
`commandLineCount` used to compile cleanly and produce a program whose arguments were silently zero,
on both backends, with no diagnostic anywhere. `supplied.def` was written to close it for globals,
and the other three tables close it for the rest of the surface.

Writing it at the declaration also makes the coupling visible where it binds. A reader of `lib/Core`
can see which declarations the compiler is fastened to, and therefore which ones are not free to
change; a table in another directory cannot tell them that.

`primitive.def` is the one exception, and necessarily: nothing claims a primitive, because there is
no declaration to write an attribute on. Its names are here for the same reason the rest are - they
are source-visible strings - but they are looked up as *types* and are not in the role namespace.

`constructor.def` is that same exception for the names that take **arguments** - `Vec`, `Mask` and
`Atomic`. The split between the two files is arity and nothing else: a stage answers a primitive by
handing back a type and answers one of these by *building* a type out of what was applied. Both pass
`primitive.def`'s test for belonging - a `data` head could not have stated either, because a vector's
natural lane count is the register's width divided by the lane's and an atomic is not `TrivialCopy`
while everything it holds is - and neither is in the role namespace, for the reason above.

`Bool` used to be a row there, and its two constructors a companion table beside it, on the argument
that a comparison has to answer something before there is a library to answer with. Giving the
ownership stage a library removed the argument: `Bool` is `known.def`'s `boolean` role now - an
ordinary two-constructor enum `lib/Core` declares - and the compiler holds an index into its
constructor list rather than either spelling. The test for belonging in `primitive.def` is that a `data` head *could not* have stated it,
and `I32`'s width and `Size`'s openness are the only things left that pass it.

The attribute's own spelling is `kBuiltinAttribute` in [`role.h`](role.h), for the same reason a
role's is: both resolve stages read it, and written out at each reader it would be the one thing
about this design that could still drift.

**One row's spelling is not its declaration's name**, and it is worth pointing at rather than leaving
for a reader to trip over: `known.def`'s `carrier` is `Try`, because `try` is a C++ keyword and
cannot be an enumerator. A spelling column for the one row that needs it would be exactly the second
name for one thing this directory exists to not have, so the *role* is named for what the class says
instead - which of a carrier's two paths a value is on, which is what `doc/spec/core.md` calls it.

## One namespace, three numberings

Three of the five directions are *claimable*: a declaration writes `@builtin(role)` and fills a slot.
`findRole` searches those three tables and `roleList()` prints all three, so `@builtin(copy)` means
one thing and a diagnostic naming what the compiler knows names everything it knows. Two rows sharing
a spelling would make which of them answered depend on search order, silently, so `verifyRoleTable`
asserts against it in assertion builds.

The other two are not in the role namespace and cannot be, which is `RoleKind`'s whole shape: a
primitive has no declaration to write an attribute on, and a `provided.def` row is keyed off the
`Known` role naming its class rather than claimed in its own right. So `Roles::claim` is total over
`RoleKind` - there is no arm that answers "nothing to claim", which is what a fourth kind would have
had to be.

Each consumer still indexes its own dense enum - `Known`, `Op`, `Builtin` - so a `switch` over roles
stays a jump table. `RoleRef` is the pair that spans them, and it exists for exactly one caller: the
attribute reader, which has a name and does not yet have a kind.

## What is not here

**No emitters.** What a role *expands to* is a question about one stage's IR - a MIR node, an SSA
value, a JavaScript expression - so each consumer implements the roles it cares about in its own
`intrinsic/` subdirectory, indexed by the enums here. `compiler/resolve_mir/intrinsic/` is the first.
Those implementations hold no names at all, which is the property this arrangement exists to buy.
The same holds for `provided.def`: a row names a class by its `Known` role, a family of types and
whether its heads are one type of that family or a *pair* of them, and the stage generates the
instances and writes their members' bodies from emitters indexed by the class's member *position* -
the one dependency on the library's order, stated in `known.def`.

## What is here that is not a table

[`convert.h`](convert.h) is the exception and it is worth saying why it is not a `.def`. It states
**which conversion class relates a pair of numeric primitives** - `Widen` where nothing is lost,
`Narrow` where something is, `Bitcast` where the two are the same bits on every target - and that is
a rule rather than a roster: a table would be one row per ordered pair of the widths a target has,
which is generated data with the generator somewhere else.

It is here for the reason everything else is. It is a statement the *language* makes, it is stated
over `primitive.def`'s widths, and **both resolve stages read it**: a fixture is compiled by both and
the two answers compared, so two ladders that disagreed by one rung would be one source that compiles
under one stage and not the other. What is deliberately *not* here is anything a target decides - a
JavaScript build declines the `Bitcast` rungs crossing between a 64-bit integer and a `Double`, and
that judgement stays where the target is known.

**No signatures.** `op.def` states the *shape an emitter depends on* and not the declaration. The
declaration is the library's, it is generic and constrained and origin-qualified, the type checker
reads it and every call is checked against it - restating it here would be the disease rather than
the cure. [`shape.h`](shape.h) makes that argument in full, and names the bug the check exists to
prevent.

A shape says three things about a position's root - the type, how it is passed, where its storage
comes from - and, since 2026-09-06, one about a **pointer's pointee**: `rootedPointerAt` and
`rootedSameAs` are what `pointerAdd`, `pointerSub` and the two `pointerBorrow` rows now write.
That fourth axis exists because two libraries drifted in silence: while the ownership stage had a
standard library of its own it wrote `advance(it: %source'a, count: Size) -> %source'a` where
`lib/Native/Pointer.yana` wrote a plain `%a` at both ends, so an element of a window was a borrow
rooted in nothing and every signature that handed one back was refused three files away from the
mistake. There is one library now; the row is what keeps the declaration honest anyway.

## No operation is matched by name

`op.def` is the roster of operations the compiler implements, and it is now the whole roster. There
was a second mechanism beside it - `attachIntrinsic(module, "name"_v, emitter)`, a string looked up
in one module's function table - and every declaration the compiler depended on had its spelling
written out in C++ next to its emitter. It is gone, along with the hundred-odd names in it.

What that bought, beyond the rename safety the attribute exists for, is that **the conditions are
gone**. A name lookup that missed was reported as a compiler bug, correctly, so every question about
which declarations a build actually has had to be asked a second time at the attach site, in the same
vocabulary the source asks it in: `isJsMode` around the vector transfers, the syscalls and the whole
host file; a target and architecture test around `cloneThread`; three `targetSelector` calls around
the x86 groups; and a ternary choosing between `blockCopy` and `copyMemory`. A role nothing claimed
is a role nothing can call, so an emitter offered for one lands on nothing - and `@platform` is left
as the only place the question is asked, which is what it was always for.

Two declarations may claim one role from opposite sides of a platform filter, and that is how
`blockCopy` works: `Memory.native.yana` claims it for the `rep movsb` at the bottom of its ladder,
and on JS `copyMemory` claims it directly, so exactly one exists in any build.

## And the classes it references

The same change ran through `known.def`'s class rows, one direction over. `resolve_mir` bound them
from the start; `compiler/resolve`, the resolve stage deleted in J173, did not, and held
`classNamed(module, "Try"_v)` - one per field of `CoreClasses`, twenty-six in all, each asserting
its way to a handle. A library that renamed a class the language's own syntax dispatches through
tripped an assertion in a debug build and read a null in a release one, and the diagnostic in
neither case named the class or the rename. That stage came to read sixteen of the twenty-six out of
rows before it went, and found the rest by their spelling.

**No class is found by its spelling now.** Every class the compiler dispatches through is a row,
read out of `mir::Roles` from the attribute the declaration writes; a class the language's syntax is
written in terms of that has no row yet (`known.def`'s tail says which) is one nothing in the
compiler names, because a row is written the day something has to.

**The host is the one group whose roles are not one per operation**, and the reason is worth reading
in `op.def`'s host block: the member's *name* is a parameter there rather than part of the role, so
`hostCall1` serves every one-argument method the host has and the compiler knows none of their
spellings. That works because a role is claimed once - the alternative, an attribute naming the
member, would need a role per method - and it costs nothing because the wrappers are
`@inline(always)` and a string literal on that target is a constant with no storage.

Those rows had **never been checked against a declaration** until `resolve_mir` grew emitters for
them on 2026-09-21, and four of them did not fit the library that had been written against them:
`hostArray` stated `pointerAt()` at a *result* position, which no result can satisfy; `hostViewRead`
stated `integerAt()` for a signature whose width is the accessor's; and `hostAt`/`hostAtMut` stated a
call-bounded pointer for two declarations that publish an origin into it. That is what the warning at
the head of `resolve_mir/native.def` used to be about, and it is the argument for this directory
restated from the failure end: a shape is a claim about a declaration, and a claim nothing compares
is a claim that drifts. The rows are `rootedPointerAt()`, `addressAt()` and `anyAt()` now, and
`lib/Native/Host.js.yana` writes `self: %source'a` where it used to write `return self: %a`.

## Who links against it

`YanaIntrinsic` is a leaf beside `YanaCore` and depends on nothing else, because more than one stage
reads it: the resolver binds roles, `lower` carries the supplied globals across, and both native
backends fill them. It used to be `compiler/compiler/builtin.h`, which two code generators were
reading a role enum out of the resolver's neighbourhood.
