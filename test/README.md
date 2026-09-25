# Running the tests

Twelve drivers, each over its own fixture directory or none. They read their fixtures by paths relative to the
working directory, so they run from **this** directory and nowhere else.

```sh
cd test && ./run-tests.sh
```

That builds nothing — it runs what is already in `../build-assert`. Name another tree as the first
argument (`./run-tests.sh ../build-release`), cap the parallelism with `JOBS=n`, and anything after the build
directory is passed through to every driver.

| driver | fixtures | what it asserts |
| --- | --- | --- |
| `YanaResolveTest` | `resolve/` | the whole pipeline: resolve IR, ownership, lowered IR, the amd64 and JavaScript backends, and what `main` answers on both |
| `YanaLibTest` | `lib/` | what the standard library *computes*, on both targets, with no golden files at all - through the rewrite, and through the old stage as a second row |
| `YanaParseTest` | `parser/` | the AST, the diagnostics, and that no prefix of a fixture hangs the parser |
| `YanaLowerTest` | `lower/` | the lower IR in isolation |
| `YanaX64Test` | `x64/` | instruction selection and register allocation from lower IR |
| `YanaLlvmTest` | `llvm/` | the LLVM module built from lower IR, and that LLVM's own verifier accepts it |
| `YanaElfTest` | `resolve/` and its subdirectories, `lib/` | the same fixtures compiled by the rewrite to real ELF executables and run as processes |
| `YanaOssaTest` | none - it builds and breaks its own IR | that the OSSA verifier *rejects* what it is meant to |
| `YanaJsirTest` | none - it builds and breaks its own IR | that the JSIR verifier does the same: a number used as an object receiver, parts used as one value, an edge at the wrong representation, a missing definition, a store the place cannot hold, and a call that disagrees with its callee's plan |
| `YanaLspTest` | `lsp/` | the editor-facing answers |
| `YanaLspProtocolTest` | `lsp/protocol.expect` | the lifecycle over the real message loop |
| `YanaEditTest` | none - it builds its own IR by hand | the old stage's `IrEditor` (`compiler/resolve/edit.h`): edge shapes no source program produces, each step followed by `verifyFunction` |
| `../tools/check-layering.sh` | `compiler/`'s `#include` lines | the rewrite's directory boundaries: nothing in `resolve_ssa` outside `lower/` or `js/` reaches into either, and neither target's tier reaches into the other's |

The last row is a shell script rather than a driver, because what it checks is the *text of the
repository* and not the behaviour of a program - Analysis-IR-JS.md §8.1, and the guard §12.3's first
open question asked for when J75 closed it. It takes the same `generate` argument every job gets and
ignores it: there is nothing here to regenerate.

**Every driver reports its result in its exit status**, and `run-tests.sh` has nothing else to go on:
it runs each driver as a job, reads the status, and names the ones that were not zero. A failure is a
fixture that did not match, a fixture that could not be opened or parsed, and — because a driver that
verified nothing must not be mistaken for one that verified everything — a fixture directory that came
out empty, which is what running from anywhere but this directory looks like. `YanaX64Test` and
`YanaLowerTest` returned nothing at all until 2026-08-07, so twelve failing x64 goldens read as "all
green" for as long as that suite has existed; if you add a driver, this is the part to get right.

The fixture modes — which `.expect` files opt a fixture into what — are documented where they are
implemented, in the header comments of `ResolveTester.cpp`, and the library corpus' rules in
`LibTester.cpp`. `generate` as an argument rewrites the expectation files rather than comparing
against them; read `git status test/` afterwards, because churn in a fixture you did not touch is a
behaviour change and not noise.

Four of those modes belong to the **second resolve stage** rather than to the first — see the
"There are two resolve stages" section of `AGENTS.md`, and doc/inflight/Analysis-IR.md part 11.3.
`.mir.expect` opts a fixture into `compiler/resolve_mir`/`compiler/resolve_ssa` and is what adopts
the corpus one fixture at a time; `.ssa.expect` and `.mir.lower.expect` are separately optional
beside it. `.ssa.opt.expect` is the SSA after `compiler/resolve_ssa/opt` has run, and it is opt-in on
the stricter terms `.mir.repr.expect` has — `generate` regenerates one that exists and never creates
one — because a fixture that is not about the optimizer would otherwise carry a second copy of its IR
that moves whenever a pass changes. `.ssa.expect` stays the *conversion's* output, so the pair is the
before and after of the whole stage. `.mir.llvm.expect` is this stage's lowering handed to LLVM, on
the same opt-in terms as the shipping stage's `.llvm.expect` beside it.

**Every fixture with a shipping-stage optimizer or LLVM golden has the rewrite's too** since
2026-09-24 (J133), where the rewrite compiles it: the sixteen `.llvm.expect` fixtures with a
`.mir.expect` have a `.mir.llvm.expect`, and the five `.opt.expect` ones a `.ssa.opt.expect`. The
two that could not - `Packed` and `ConstGenericVec`, J94 §3.4 gaps - have theirs since J142. A new
`.opt.expect` or `.llvm.expect` should arrive with its partner.
`.js.ssa.expect` is the fifth, and it is the same source compiled as a **JavaScript** program by
that same stage - Analysis-IR-JS.md milestone 3. It is additional rather than alternative: a fixture
acquires one by having the file beside it and none of its native goldens move, so `.mir.expect` and
`.ssa.expect` stay the native program's and this is the other target's. `.js.mir.expect` is
separately optional beside it. The pass stops after the optimizer, so what it asserts is exactly the
resolver's own answer for that target: the program resolves, converts, verifies and optimizes for it.
What stays here is the fixtures that are about the *resolver's* JavaScript answer, which is what
`resolve/HostOps.yana` and `resolve/HostText.yana` are; everything about what is emitted from it is
`.jsir.expect`'s, below.

`.jsir.expect` is the sixth, and it is the opt-in for the **JSIR pipeline** - Analysis-IR-JS.md
§10.6's fixture contract. A fixture is compiled by the new JavaScript backend because this file
exists beside it, on exactly the terms `.mir.expect` opts one into the native rewrite: a new one is
an empty file plus a `generate` run. `.jsir.js.expect` holds the JavaScript that pipeline emits,
beside the shipping stage's `.js.expect` rather than replacing it.

**It is the opt-in in the rewrite-only directories too** since 2026-09-21 (J66), where there is no
`.js.expect` for it to sit beside: `resolve/{ownership,loans,lens,generic,lazy,iter}/` hold the
programs the shipping stage never compiled, and a `.jsir.expect` there is what says the JavaScript
half of one runs. 117 of them have one, against 374 in the corpus proper (J165).

**And it does not follow `.js.expect`** since J70. The shipping stage's own opt-in is which fixtures
*that* backend was ever taught to compile, which is a different question from which programs have a
JavaScript meaning - so the rule here is the second one: a fixture with a `.run.expect` that the
rewrite compiles for this target and that answers the same number carries a `.jsir.expect`, whether
or not a `.js.expect` sits beside it. 241 of the 491 have no `.js.expect` at all. What is left out is
five fixtures whose JavaScript answer genuinely differs from their native one and which have no
`.js.run.expect` saying so - `NoChecks`, `Replaces`, `intrinsic/Supplied`, `VecLong.Avx512` and
`VecMaskUpTo.Avx2`, all five of which answer the same different number under the shipping backend, so
the difference is the fixture's rather than either pipeline's.

**Two goldens for the text and one for the answer**, and the asymmetry is the point. Two backends
emit different JavaScript for one program and there is no version of that which is a defect, so the
text needs two files; what they *compute* is the property worth asserting, and a second expected
value there would let exactly the disagreement this is built to catch pass silently. So both emitted
programs are run under Node and both are held to the one `.run.expect` the native build answers - or
to the `.js.run.expect` a fixture whose JavaScript answer differs on purpose names, which today is
`Platform.yana` and nothing else. The **unoptimized** JSIR build is run too and held to the optimized
one's answer, and a disagreement between those two is a failure in itself.

**Three runs since 2026-09-22 (J72)**, which is Analysis-IR-JS.md §10.2's third axis: the shared
optimizer on and off, and the **JSIR** optimizer on and off (`-no-jsir-opt`). The pair is what says
which tier a disagreement came from - a fixture that differs with `-no-ir-opt` and agrees with the
third run has a defect above the fork, and the other way round has one below it, which is where
`resolve_ssa/js/opt_alias.cpp`'s rewrites are. `test/run-base.sh`'s `ownership-js-no-jsir-opt` row is
the same axis asked of a whole program.

**Three runs and two compilations** since 2026-09-22 (J80). `-no-ir-opt` is a different program
above the fork and is a compilation of its own; the third configuration is the *same* SSA with the
JSIR optimizer off, which is one more `genJsirProgram` over the object the first one already built -
the setting is read at exactly one site, the per-function loop in `assemble.cpp`. That makes the
axis sharper rather than cheaper: the input is identical by construction instead of by a second
resolution being assumed to agree.

The same pass produces the **resolver's** three goldens for this target as well, off the same
compilation: `.js.mir.expect` and `.js.explain.expect` before the conversion (which releases the MIR
bodies their placements are read off) and `.js.ssa.expect` before the optimizer. That was a second
pass with its own front end until J80, so a fixture carrying both was compiled twice for one target.
A fixture may carry the three and not the two - `resolve/HostOps.yana` is one, since this tier has
no form for the program it describes.

A `.jsir.expect` golden carries one **`optimizer`** line saying what that pass did to the fixture -
clones seen, clones aliased, fields forwarded, instructions removed - so a fixture's own diff says
whether a rewrite reached it. That is why every one of them moved when the pass landed, and it is
why one will move again whenever a later slice adds a counter.

`.jsir.errors.expect` is the seventh and is the other opt-in, added 2026-09-22 (J71): a fixture whose
JavaScript answer is a **deliberate target gap** carries the sentence this target reports instead of
a dump. It is `.errors.expect` one tier down - the file's existence is the opt-in, an empty one plus
a `generate` run creates it, and what is compared is the recorded diagnostics of a JSIR compilation
that is expected to report.

It exists because a sweep is not a golden. `lib/Native`'s `allocateHeap`, `heapNext`, `sizeOf` and
`mapMemory` are `@platform(native)` declarations a JavaScript program has no answer for; a bitcast
between vectors of *different lane widths* has none either, a lane being a number here rather than a
run of bits; and nine `X86.*` rows are one machine's. All of those are gaps this target is supposed
to have, and until J71 the only thing standing over them was a count in a progress record - so a gap
that silently became a *different* gap, or a wrong answer, changed nothing anybody would see. 68
fixtures carry one. A file that records a *resolver* diagnostic rather than a JSIR one says so in its
own text, which is what makes it answer "which of the two is this gap" rather than only "is there
one".

Mutually exclusive with `.jsir.expect` by construction: a fixture that converts has a dump and one
that reports has none. A fixture that starts converting therefore loses this file and gains the other
two, which is exactly the change a slice that closes a gap should show in its diff. **The one
exception is a gap only the unoptimized build has** (J165): with both files, the optimized build is
held to the dump and the answer, and the `-no-ir-opt` compilation - which keeps a construct the
optimizer removed - is the one expected to report this file's sentence, in place of being run.
`Instance.yana` is the case: JSIR refuses a reference to storage written into a pointer field, and
only the optimized program forwards that store away.

`.js.ssa.expect` and `.js.mir.expect` stay as they are and stay separately optional: a fixture may
have either, both or neither. The first is about what the *resolver* made of a JavaScript program and
the second about what MIR did, and neither is subsumed by a golden of the tier below them - the same
relation `.ssa.expect` has to `.mir.lower.expect` natively.

`.jsir.js.expect` is a migration file and says so: when the shipping stage's JavaScript path is
deleted, every one of them is renamed over its `.js.expect` in one change and the suffix goes.
Keeping the new text under a distinct name until then is what makes "did the new pipeline's output
move?" answerable from `git diff` while the old one is still regenerating its own goldens beside it.

A fixture that is a JavaScript program and nothing else writes `# platform: js` beside
`# resolver: ownership` - `resolve/HostOps.yana` and `resolve/Reject.HostMember.yana` are the two,
and they have to be, because every declaration they reach is `lib/Native/Host.js.yana`'s and no other
target reads it. Such a fixture takes only the JavaScript path: there is no machine to run it on, no
native golden beside it, and every driver that compiles for one skips it.

There is deliberately **no second run expectation**: `.run.expect` is what both stages are checked
against, because two implementations of one language answering differently is exactly what this is
here to catch. What a `.run.expect` does buy the second stage is a second *run*: the fixture is
compiled again with `-no-ir-opt` and the two answers compared, which is the same equivalence check
the first stage has and is the guard an optimizer golden cannot be.

## `resolve/` and `lib/`

**Which of the two a fixture belongs in is decided by what it asserts, not by what it is about.**

A `resolve/` fixture asserts a decision the *compiler* made - which calls a literal desugars to,
where a drop was placed, what a type's layout is, which instruction an instance selected - and pins
that decision in golden IR text. A `lib/` fixture asserts what a library function *computes*, and
has no golden files at all.

So `Map` is in both, and neither copy is a duplicate of the other. `resolve/MapLiteral.yana` is the
literal `[k: v]` becoming `newMap` and n inserts, with the IR to prove it; `lib/Map.yana` is three
hundred entries inserted, found, removed and iterated. `resolve/String.yana` is what a string literal
*is* on each target; `lib/String.yana` is concatenation, appending, equality and order. Splitting
them removed close to 2MB of `.resolve.expect` and `.lower.expect` from beside fixtures that said nothing
about the IR they printed and were regenerated whenever any pass changed anything.

**A library fixture is a suite rather than a program** - Design-Test.md's phases 0-3, built, and
phase 4's library half beside them. It has
no `main`: it has `@test` declarations, each of which states its expectations with `check`,
`checkEqual`, `require…` and the rest of `lib/Test/`, and the entry that runs them is synthesized by
the compiler under `-test` - the flag all three drivers that read this corpus set. The suite answers
**0** when every claim held. There is no `.run.expect`: a sentinel return - `return 0 - 7` for the
seventh check - made the corpus its own decoder ring, since the number in the golden had to be worked
out by adding up what every check contributed and adding a case anywhere renumbered it.

What a fixture gains from being a suite is where a failure is:

```
Fail (lib/Show.yana): the amd64 build stopped with status 134 - a check or an assertion failed, while running Lib.Show.boolText.
  fail  0 Lib.Show:53:4  expected 3, got 2
```

Both lines come from the **report stream** the runner writes to descriptor 3, which the driver opens
for it - `test/report.h` is the reader, shared by `YanaLibTest` and `YanaElfTest`. The second is the
claim with its source location and its values, which `assert` could never say. The first is the case
that was open when the stream stopped: a `begin` with no `end` names the case that died, with no
second process and no debugger, which is the absence the section below describes.

The module a fixture compiles as is `Lib.<name>` rather than its own name - qualified, because half
this corpus is named after the module it is about and `Atomic.yana` compiled as `Atomic` imports
itself.

A failed check still stops the process with status 134 natively and throws on JS, so `YanaLibTest`
still runs the program in a **forked child**: a driver that dies is what the section below says the
machine's own instability looks like, and a test failure must not be indistinguishable from that.

**`@test(aborts)` is the case that could not be written before.** Its verdict is inverted - the case
passes when the process stops and fails when it returns - so a failed `assert` and a subscript past
the end are things this corpus now *runs* rather than describes. It needs a real process to
re-execute, so `YanaLibTest`, which JITs, skips those cases and `YanaElfTest`, where the kernel starts
the program, runs them. The library's own such cases moved to `lib/Native/Aborts.test.native.yana`
and run in `run-base.sh`'s native rows; in this driver's corpora the one left is
`resolve/TestEntry.yana`'s, against the minimal runner.

Every fixture is compiled and run three times - specialized, forced generic, and with the optimizer
off - and a fourth and fifth time on JavaScript where a `<fixture>.yana.js` marker file exists. Those
are the assertions that cannot be regenerated into agreeing with a bad pass. `generate` is accepted
and ignored, because there is nothing here to generate.

Those five are the **shipping stage's**, and since 2026-09-24 (J133) they are asked for with
`resolver:legacy`: the driver's default is the **rewritten stage's** builds - one native and, where
the same `<fixture>.yana.js` marker exists, one JavaScript build through JSIR. Two rather than five,
because the switches the other three vary are the shipping optimizer's and the shipping specializer's
and the rewrite's own are asserted by `YanaResolveTest`, which runs every `.jsir.expect` fixture in
the optimized and unoptimized form and compares the answers. What these two add is the one thing that
corpus cannot say - that the *library's* own suite passes when the rewrite compiled it.
`run-tests.sh` runs both, the old stage as the `lib-legacy.*` jobs, because five fixtures cost a
second or two; `resolver:ownership` is still accepted and means the default.

**Two of the five carry the marker** since 2026-09-21 (J68) - `MapLiteral` and
`StringLiteralStorage`, which are exactly the two the shipping stage's own JavaScript rows compile.
The other three are native by nature: two walk a file system and read addresses, and the third needs
a module directory. So the marker set is "what this target can run at all" rather than "what the
rewrite can run yet", and the rewrite passes all of it. Before vectors landed it passed none of it
(`doc/progress/2026-09-21-J68-jsir-parts-vectors.md` §3).

`YanaElfTest` shares both corpora and their goldens rather than having any of
its own, and asserts a different thing about them: the resolve driver maps the generated bytes and
calls into them, and this one writes the executable file that `yana -mode exe` writes on this platform
and lets the kernel start it. What only this driver can see is everything between the last byte
generated and the first instruction executed — segment layout, the entry point, which page the data
landed on, the addresses written into constant data — and all of those fail as a crash rather than as
a wrong answer, which is why it reports a signal as an outcome of its own. It compares the **low
byte** of the expected value, since that is all a process exit status carries; the whole number is
still asserted, in-process, by `YanaResolveTest`. A `lib/` fixture names no number at all, so the
expected status is zero: a 1 is a suite that reported a failure and a 134 is one that stopped where
it stood, both arriving as themselves. It is also the only driver that runs a `@test(aborts)` case,
for the reason above. It is skipped entirely off amd64 Linux.

**It builds with the rewrite** since 2026-09-24 (J133), and `resolver:legacy` is the old stage by hand.
It had built with the old stage alone, which left the driver that asserts a program runs as a *file*
saying nothing about the compiler `yana` builds files with. Which `resolve/` fixtures it runs is the
corpus's word, not a trial: the ones carrying `# resolver: ownership` or a `.mir.expect`, which is how
`YanaResolveTest` adopts one - and since J143 there is **no** top-level `.run.expect` fixture with
neither: `InlineCapacity`, J94 §3.4's last gap, has its `.mir.expect` (fifteen at J133; J139-J141
adopted eleven, J142 `Lens`, `NamedArgument.Value` and `Packed`, with `ConstGenericVec` beside them
on JavaScript and LLVM, and J143 the last). It walks `resolve/`'s subdirectories too since then, one level as `YanaResolveTest` does - they
are almost all the rewrite's, which is why it never had - and its module provider reads the grouped
`resolve/modules/<Name>/` form for them. It names the module `ResolveTest` as the in-process driver
does, because a program may compute with its module's name and the two share `.run.expect`:
`RecordFields` adds it into its answer, and read four less as `ElfTest`. The old stage is not a row of
`run-tests.sh`: what it computes is `YanaResolveTest`'s to assert, and everything this driver is
about is below the `LowerModule` the two share.

### `# resolver: ownership` - the rewritten stage only

The rewrite in `compiler/resolve_mir` already accepts programs the shipping stage refuses; a partial
move out of a record is the first of them. Such a fixture says so in its own source, wherever it is
filed - `resolve/ownership/`, `loans/`, `lens/`, `generic/`, `lazy/` and `iter/` hold most of them,
and the top of `resolve/` holds respellings the shipping stage refuses and every rejection fixture
whose rule the rewrite enforces:

```
   # resolver: ownership
```

The directive is what makes it run that way - see `directives.h` - and the directory is filing. A
fixture anywhere carrying that line is compiled only by the rewritten stage; `YanaResolveTest` skips
the legacy half and its eight goldens for it, `YanaBench -legacy` skips it (the default run, on the
rewrite since J150, includes it), and `YanaElfTest` - which
builds with the rewrite since J133 - runs it, and skips it only under `resolver:legacy`.

**The corpus's stage is not the command line's.** `-resolver` defaults to the rewrite since
2026-09-22 (J81), and `applyResolverDirective` pins `ResolverStage::Legacy` before it looks for the
line above - so the drivers read a fixture exactly as they did before the flip, and this directive
stays the one thing that raises one out of it. That is deliberate and not an oversight: a corpus that
inherited the flag's default would have had nine hundred goldens written by one stage quietly
reclassified as the other's, and the `.mir.expect` oracle would have been asserting that the rewrite
agrees with itself. The passes that *are* the rewrite's set the stage themselves, after the
directives run. `resolve/` has no `# resolver: legacy` and needs none: legacy is what a fixture with
no directive already is.

**A `Reject.X.Legacy.yana` is the part of `Reject.X.yana` only the shipping stage refuses.** When
the rewrite enforces a rejection fixture's rule, the fixture carries the directive above; a line
it does not refuse yet, or refuses only as "does not implement ... yet", is split into the
remainder, whose header says the rule and what the rewrite does instead. When the rewrite enforces
it, the line goes back and the remainder is deleted. **One is left, by decision rather than as
pending work**: `Reject.GenericDefault.Legacy.yana` records the shipping stage's refusal of a generic
default naming an earlier parameter, which is language since J144 and which the rewrite accepts, so it
goes when that stage does. A rejection fixture the rewrite resolves *clean* is then converted,
optimized and lowered, because a few refusals are the lowering's - `Reject.PackedAddress.yana` is
asserted that way.

`# recover: yes` is `CompileSettings::recover`: the rewritten stage goes on past a checkpoint that
reported. Without it a compile stops at the first failing checkpoint, so a declaration error hides
every body error after it. A rejection fixture that asserts both kinds, or several body errors in
functions whose checks run at different checkpoints, needs it; one whose golden is the same without
it should not carry it.

`# test: yes` is `-test`, and it is the one directive that changes what the *entry* is. A fixture
carrying it has no `main`: it has `@test` declarations, and what runs is the entry the compiler
synthesizes out of them, over `resolve/modules/Test.yana` - a minimal runner both resolve stages
compile, so the harness itself is something the shared corpus can assert rather than only the library
suite. `resolve/TestEntry.yana` is that fixture; its `.run.expect` is a number `runMain` packs out of
the cases the compiler built, so a case that went missing or a `Site` that was not filled moves it.

**The directive is not how a fixture opts into the new stage.** That is the `.mir.expect` file beside
the source, and it is how the shared corpus is adopted one fixture at a time: a fixture with the
golden and no directive is compiled by *both* stages and asserts that they agree on the answer, which
is the most valuable thing either of them can say. The directive costs that comparison, so it is
worth writing only for a program the old stage genuinely cannot compile - which every fixture in that
directory is, and which is worth re-checking before adding one.

`resolve/loans/`, `resolve/lens/`, `resolve/iter/` and `resolve/generic/` are filing of the same
kind, one family of the rewrite's fixtures each - the borrow grid, the continuation cases, the
iterator objects, and the generics. A fixture in
one of them carries the directive only when it needs to, and a fixture both stages compile stays in
the shared corpus instead - `resolve/Identity.yana` is the generics one, filed apart from
`resolve/generic/` because agreeing about the answer is worth more than being filed together.

`resolve/modules/` is the other subdirectory and is not fixtures at all: it holds the modules an
`import` in a fixture resolves to. `kModuleDirectory` in the driver names it once, for the provider
that reads it and for the listing that must not run it.

## What a fixture looks like run by hand

A `lib/` fixture is a program, and running the one a driver built is often faster than reading a
driver's log. Under a driver it says nothing at all — descriptor 3 is open, which is how a runner
knows something is reading its report, and a process whose report has a channel of its own prints
nothing else. Run at a terminal it has neither, and prints for a person instead:

```
Show  5 tests

     test                  time  result
  -------------------------------------
  ✓  Show.boolText       445 ns  passed
  ✗  Show.integerText    407 ns  failed
  ✓  Show.stringText     160 ns  passed

  ✗  Show.integerText
      Lib.Show:53:4  expected 3, got 2

  ✗  1 failed, 4 passed in 22.66 µs
```

While it runs, one line rewrites itself with the case that is running — so a fixture that crashes or
hangs leaves the name of the case that did it on the screen, which is the same thing a `begin` with
no `end` buys a driver. A redirected run gets the same text in ASCII with no colour and no progress
line; `--color`, `--ascii` and `--no-progress` override the detection. `Design-Test.md` §5.5 is the
design and `lib/Terminal/` is what draws it.

Every switch the runner takes is in `lib/Test/Options.yana` — `--filter`, `--only`, `--shard i/n`,
`--isolate`, `--jobs n`, `--timeout ms`, `--list`, `-v`, `-q` — and `--list` on its own is the
quickest way to see what a fixture holds.

## Sharding

`YanaResolveTest`, `YanaParseTest`, `YanaLibTest` and `YanaElfTest` take `shard:i/n` and run the fixtures whose position in the
directory listing is `i` modulo `n`. Fixtures are independent — each compiles its own source and
writes nothing outside itself — so the only thing that kept the suite serial was that a driver is one
process. `run-tests.sh` does this across cores; `shard.h` explains why the split is round-robin
rather than contiguous.

The same four take `jobs:n`, which is the shards run **by the driver itself**: it forks `n` children,
each takes one shard, and the parent replays their output in shard order and exits with the worst of
their statuses. `generate` works the same way, since two shards never write the same file.

**This is the default, and does not have to be asked for.** A driver invoked on its own runs one
shard per core; `chooseJobs` in `shard.h` is where that is decided, and it defers in two directions —
to an explicit `jobs:n`, and to `1` when the caller already passed a `shard:i/n`, which is
`run-tests.sh`, whose whole job is to have done the splitting. `jobs:1` is how a run under a debugger
or a profiler asks for a single process.

The default was `1` and the reason to change it is that the serial run was never the useful one:
`run-tests.sh` already shards across cores, so a driver invoked on its own was the *only* way to run
a corpus serially, and that is exactly the invocation an edit loop uses. On this machine the resolve
corpus is 62 s serially and 4.8 s by default.

```sh
../build-assert/test/YanaResolveTest              # every core
../build-assert/test/YanaResolveTest generate
../build-assert/test/YanaResolveTest jobs:1       # one process, for a debugger
```

The other eight drivers take neither argument, and that is a measurement rather than an omission:
`YanaLowerTest`, `YanaX64Test`, `YanaLlvmTest`, `YanaEditTest` and `YanaJsirTest` are each under a
tenth of a second (`YanaJsirTest` about a hundredth, even in `build-assert`),
and `YanaOssaTest`, `YanaLspTest` and `YanaLspProtocolTest` are each under half of one — less than a
fork costs.

An argument that is neither `generate`, a shard spec nor a jobs spec names one fixture by prefix,
which is what to use when something earlier in the run takes the whole process down:

```sh
../build-assert/test/YanaResolveTest Subscript
```

## What it costs

Measured on a 32-core machine, at the revision that added `run-tests.sh`:

| | serial, `-O0` | serial, `-O2` | via `run-tests.sh` |
| --- | --- | --- | --- |
| whole suite | 12.5s | 5.1s | **1.8s** |
| `YanaResolveTest` alone | 8.7s | 2.1s | |
| `YanaParseTest` alone | 3.4s | 2.8s | |

Those numbers predate the library matrix, which is the critical path now: it builds the whole
library once per row, and one of those builds compiles more source than the entire fixture corpus.
On the same machine the whole suite is **33 s**, of which the matrix is 28 s and everything else is
over in the first five. `run-base.sh` builds its rows at once rather than in sequence, which is what
took it from 112 s; what is left is one library build, and that is a compiler-speed number rather
than a scheduling one.

`-O2` is worth 4x on the resolve suite, and it is not what a `Debug` tree gives you by itself — see
the note in the root `CMakeLists.txt` about `CMAKE_CXX_FLAGS_DEBUG`, which silently discarded the
flag for as long as `build-assert` existed. Check `flags.make`, never the cache. The `-O0` column is
kept as a measurement; there is no longer a tree configured that way, and the note in `AGENTS.md`
says why.

The parser driver barely moves, because almost all of it is `truncationTest` parsing every prefix of
every fixture: 38,215 parses over a 38KB corpus, quadratic in how long the fixtures are rather than
how many there are. Constructing the per-prefix `Context` is 0.11s of that and the parsing is the
rest, so there is nothing to hoist — the only thing that would make it cheaper is checking fewer cut
points, and every cut point is the property. Shard it instead.

Most of the resolve suite is the equivalence checks rather than the golden files: every runnable
fixture is compiled again with specialization declined and again with the IR optimizer off, and the
answers are compared with each other rather than with any file. That is roughly 590 full compiles for
159 fixtures, and it is the assertion in the suite that cannot be regenerated into agreeing with a
bad pass. The library suite is nothing *but* that: forty-seven fixtures, three or five builds each,
and no file on either side of any of them.

## A red run that says nothing is probably the CPU

**A sharded run fails intermittently on this machine, roughly once per one to two thousand driver
processes, and it is not a compiler bug.** Before investigating a failure, look at whether the
failing shard's log contains an actual message. A fixture regression always says what it expected —
`main returned`, `optimizer changed`, `resolver produced N diagnostics`. This says nothing at all:
the log ends after a passing fixture and the process is simply gone. Re-run before believing it.

Two cores were captured, in unrelated subsystems — a null dereference in `insertBlockDrops`
(`analyze_drop.cpp`) and a wild pointer in `HashMap::lookup` under the register allocator
(`place.cpp`). Both had coherent stacks: canary, saved frame pointer, return address and every
enclosing frame's locals intact. The decisive one is a three-instruction sequence in
`SmallList::push`:

```asm
mov -0x30(%rbp),%rax     ; arena
mov %rax,%rdi
call Region<ModuleRegion>::operator*
```

The stack slot at `rbp-0x30` **contains** a valid pointer in the core; the callee's stored `this` is
zero. A load returned something the memory it read does not hold, on an otherwise intact stack. No
software defect produces that.

The machine is a 13th Gen i9-13900K running microcode **0x11D** at 5.5GHz — before any of Intel's
mitigations for the Raptor Lake Vmin Shift Instability, whose documented symptom is exactly this:
random faults under heavy parallel compile-shaped load. The fix is a BIOS update carrying microcode
**0x12B** or later. Affected parts also carry an extended warranty, so if it survives the update the
part has likely already degraded.

Ruled out first, so nobody repeats it: the Node harness (it reproduces with `node` off PATH), arena
movement (`LinearArena::alloc` only bumps, and `Function::blocks` holds pointers, so neither a
`Block*` nor a raw arena pointer can dangle), one bad fixture (2000 runs of the fixture from the core
were clean) and one bad shard (600 runs clean). **gdb, ASan, `MALLOC_CHECK_`, `stdbuf` and `taskset`
all suppress it**, between 256 and 960 runs each — so it will not be caught under a tool, and a clean
run under one is not evidence of anything.

## The JavaScript half

Fixtures with both `.js.expect` and `.run.expect` - or, under `lib/`, with a `.js` marker - have their emitted JavaScript executed and held to
the same answer the amd64 backend gave, since the two targets agreeing is the property worth
asserting. That needs `node` on PATH; without it the driver says so once and skips.

**Both files, and the `.js.expect` is the one that decides.** `runJsPass` is called only where one
exists, so a fixture with a run expectation and no golden has its JavaScript neither emitted nor
executed - the JS backend never sees it at all. 89 of the 237 fixtures with a `.run.expect` were in
that position, and one of them - `SpecializedSink.yana` - had been storing `null` into an array on
JS for as long as it had a golden that agreed with it.

So **a new fixture that is not about a native-only property should be given a `.js.expect`**, which
means creating the file empty and running `generate`. 44 of those 89 were given one; the seven that
were not are named below, and the rest already had one. Making the run expectation alone sufficient
is not the answer, because being native-only is a real and common thing for a fixture to be:

- `Pointer.yana`, `LazyPointer.yana`, `Record.yana`, `RunExtentClone.yana` and `Box.Storage.yana`
  are about raw pointers and addresses, which a host value does not have;
- `TailPad.yana` asserts sizes and strides in bytes, and `FoldedAddress.yana` an x86 addressing mode.

Every one of the seven says so in its own header. `OptChain.Index.yana` was an eighth until the
defect it was held back for was fixed - `prepareLocals` boxed a local because the local's *own* type
was not a host object, where what a reference needs is an object at the end of the path, so
`match maybeArr(n): Just(xs) -> xs[1]` read `.$v` off the bare array. It runs on both targets now.

### The rewrite's JavaScript rows

`run-base.sh` has a third JavaScript row since 2026-09-21 (J69): **`ownership-js`**, which is the same
`test/base` suite built with `-mode js -resolver ownership` and therefore through
`compiler/resolve_ssa/js`. It passes all **342** cases, which is exactly what the `legacy-js` and
`legacy-js-unoptimized` rows pass, so the two pipelines are held to the same run rather than to two
goldens.

**Every row of that file names its stage and writes `-resolver` out**, since 2026-09-22 (J81) - the
shipping rows are `legacy`, `legacy-generic`, `legacy-unoptimized`, `legacy-no-replacements`, the
four `legacy-avx2`/`legacy-sha`/`legacy-avx512`/`legacy-avx512-256` rows and the two `legacy-js` ones, and the rewrite's are the native
`ownership` and `ownership-unoptimized` rows, the three `ownership-js` ones and - since 2026-09-24
(J133) - `ownership-no-replacements` and the same `ownership-avx*`/`ownership-sha` ladder, started by
one `start_levels` function per stage so that the two cannot disagree about which rows this machine
can run. `legacy-generic` is the one row with no partner: the rewrite refuses `-specialize never`
(J98). The default moved that day; a matrix whose rows
inherit a default is a matrix that silently changes which compiler it is about when the default moves
again, which is the one thing a comparison suite may not do.

The rewrite's rows arrived as each went green and not before - a row that always fails is a row
everybody learns to ignore. `ownership-js-unoptimized` waited on J67 §5's occurrence defect, which
J70 closed, and the native `ownership` row on the suite passing every case in both trees.

`node-harness.js` is one Node process for the whole run, fed scripts over a pipe — starting Node ~170
times was most of the wall time of this half. Each script is still evaluated with
`vm.runInNewContext`, so it gets a fresh global and fresh intrinsics: the *process* is shared and no
fixture can observe that, but the *program* never is, and a program that answers correctly only
because of what the previous one left behind is what this suite exists to catch.
