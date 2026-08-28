# Running the tests

Nine drivers, each over its own fixture directory. They read their fixtures by paths relative to the
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
| `YanaLibTest` | `lib/` | what the standard library *computes*, on both targets, with no golden files at all |
| `YanaParseTest` | `parser/` | the AST, the diagnostics, and that no prefix of a fixture hangs the parser |
| `YanaLowerTest` | `lower/` | the lower IR in isolation |
| `YanaX64Test` | `x64/` | instruction selection and register allocation from lower IR |
| `YanaLlvmTest` | `llvm/` | the LLVM module built from lower IR, and that LLVM's own verifier accepts it |
| `YanaElfTest` | `resolve/`, `lib/` | the same fixtures compiled to real ELF executables and run as processes |
| `YanaLspTest` | `lsp/` | the editor-facing answers |
| `YanaLspProtocolTest` | `lsp/protocol.expect` | the lifecycle over the real message loop |

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
the end are things this corpus now *runs* rather than describes; `lib/Aborts.yana` is that fixture.
It needs a real process to re-execute, so `YanaLibTest`, which JITs, skips those cases and
`YanaElfTest`, where the kernel starts the program, runs them.

Every fixture is compiled and run three times - specialized, forced generic, and with the optimizer
off - and a fourth and fifth time on JavaScript where a `<fixture>.yana.js` marker file exists. Those
are the assertions that cannot be regenerated into agreeing with a bad pass. `generate` is accepted
and ignored, because there is nothing here to generate.

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

An argument that is neither `generate` nor a shard spec names one fixture by prefix, which is what to
use when something earlier in the run takes the whole process down:

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

`node-harness.js` is one Node process for the whole run, fed scripts over a pipe — starting Node ~170
times was most of the wall time of this half. Each script is still evaluated with
`vm.runInNewContext`, so it gets a fresh global and fresh intrinsics: the *process* is shared and no
fixture can observe that, but the *program* never is, and a program that answers correctly only
because of what the previous one left behind is what this suite exists to catch.
