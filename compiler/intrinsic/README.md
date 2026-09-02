# `intrinsic` - what the compiler and the source language agree about

Every place the compiler is fastened to a declaration, and every place a declaration is fastened to
the compiler, in one directory. The full argument is at the top of
[`role.h`](role.h); this is the shape of it.

## The five directions

A compiler that ships a standard library is coupled to it in five ways, and each is a table here.

| direction | table | example |
| --- | --- | --- |
| the compiler **creates** it and source names it | `primitive.def` | `I32`, `Size` |
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

`Bool` used to be a row there, and its two constructors a companion table beside it, on the argument
that a comparison has to answer something before there is a library to answer with. `prelude/`
removed the argument: `Bool` is `known.def`'s `boolean` role now - an ordinary two-constructor enum
the library declares - and the compiler holds an index into its constructor list rather than either
spelling. The test for belonging in `primitive.def` is that a `data` head *could not* have stated it,
and `I32`'s width and `Size`'s openness are the only things left that pass it.

The attribute's own spelling is `kBuiltinAttribute` in [`role.h`](role.h), for the same reason a
role's is: both resolve stages read it, and written out at each reader it would be the one thing
about this design that could still drift.

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

**No signatures.** `op.def` states the *shape an emitter depends on* and not the declaration. The
declaration is the library's, it is generic and constrained and origin-qualified, the type checker
reads it and every call is checked against it - restating it here would be the disease rather than
the cure. [`shape.h`](shape.h) makes that argument in full, and names the bug the check exists to
prevent.

## Who links against it

`YanaIntrinsic` is a leaf beside `YanaCore` and depends on nothing else, because more than one stage
reads it: both resolvers bind roles, `lower` carries the supplied globals across, and both native
backends fill them. It used to be `compiler/compiler/builtin.h`, which two code generators were
reading a role enum out of the resolver's neighbourhood.
