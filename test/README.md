# Running the tests

Six drivers, each over its own fixture directory. They read their fixtures by paths relative to the
working directory, so they run from **this** directory and nowhere else.

```sh
cd test && ./run-tests.sh
```

That builds nothing — it runs what is already in `../build`. Name another tree as the first argument
(`./run-tests.sh ../build-assert`), cap the parallelism with `JOBS=n`, and anything after the build
directory is passed through to every driver.

| driver | fixtures | what it asserts |
| --- | --- | --- |
| `YanaResolveTest` | `resolve/` | the whole pipeline: resolve IR, ownership, lowered IR, the amd64 and JavaScript backends, and what `main` answers on both |
| `YanaParseTest` | `parser/` | the AST, the diagnostics, and that no prefix of a fixture hangs the parser |
| `YanaLowerTest` | `lower/` | the lower IR in isolation |
| `YanaX64Test` | `x64/` | instruction selection and register allocation from lower IR |
| `YanaLspTest` | `lsp/` | the editor-facing answers |
| `YanaLspProtocolTest` | `lsp/protocol.expect` | the lifecycle over the real message loop |

The fixture modes — which `.expect` files opt a fixture into what — are documented where they are
implemented, in the header comments of `ResolveTester.cpp`. `generate` as an argument rewrites the
expectation files rather than comparing against them; read `git status test/` afterwards, because
churn in a fixture you did not touch is a behaviour change and not noise.

## Sharding

`YanaResolveTest` and `YanaParseTest` take `shard:i/n` and run the fixtures whose position in the
directory listing is `i` modulo `n`. Fixtures are independent — each compiles its own source and
writes nothing outside itself — so the only thing that kept the suite serial was that a driver is one
process. `run-tests.sh` does this across cores; `shard.h` explains why the split is round-robin
rather than contiguous.

An argument that is neither `generate` nor a shard spec names one fixture by prefix, which is what to
use when something earlier in the run takes the whole process down:

```sh
../build/test/YanaResolveTest Subscript
```

## What it costs

Measured on a 32-core machine, at the revision that added `run-tests.sh`:

| | serial, `-O0` | serial, `-O2` | via `run-tests.sh` |
| --- | --- | --- | --- |
| whole suite | 12.5s | 5.1s | **1.8s** |
| `YanaResolveTest` alone | 8.7s | 2.1s | |
| `YanaParseTest` alone | 3.4s | 2.8s | |

`-O2` is worth 4x on the resolve suite, and it is not what a `Debug` tree gives you — see the note in
the root `CMakeLists.txt` about `CMAKE_CXX_FLAGS_DEBUG`, which silently discarded the flag for as
long as `build-assert` existed. Check `flags.make`, never the cache.

The parser driver barely moves, because almost all of it is `truncationTest` parsing every prefix of
every fixture: 38,215 parses over a 38KB corpus, quadratic in how long the fixtures are rather than
how many there are. Constructing the per-prefix `Context` is 0.11s of that and the parsing is the
rest, so there is nothing to hoist — the only thing that would make it cheaper is checking fewer cut
points, and every cut point is the property. Shard it instead.

Most of the resolve suite is the equivalence checks rather than the golden files: every runnable
fixture is compiled again with specialization declined and again with the IR optimizer off, and the
answers are compared with each other rather than with any file. That is roughly 590 full compiles for
159 fixtures, and it is the assertion in the suite that cannot be regenerated into agreeing with a
bad pass.

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

Fixtures with both `.js.expect` and `.run.expect` have their emitted JavaScript executed and held to
the same answer the amd64 backend gave, since the two targets agreeing is the property worth
asserting. That needs `node` on PATH; without it the driver says so once and skips.

`node-harness.js` is one Node process for the whole run, fed scripts over a pipe — starting Node ~170
times was most of the wall time of this half. Each script is still evaluated with
`vm.runInNewContext`, so it gets a fresh global and fresh intrinsics: the *process* is shared and no
fixture can observe that, but the *program* never is, and a program that answers correctly only
because of what the previous one left behind is what this suite exists to catch.
