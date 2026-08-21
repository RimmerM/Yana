# Comparison against LLVM

Three corpora. **`programs/` is the one to use** — ten Yana programs compiled end to end by both
backends, which is the only one of the three that measures the compiler that ships. The two below it
measure the x64 backend in isolation; see "What each corpus can answer".

Two corpora, both written twice — once in this project's lower IR and once in LLVM IR, instruction
for instruction — and both put through **`llc`**, which is LLVM's backend alone, so no mid-level pass
runs on either side. What is compared is instruction selection, register allocation and frame layout.

**`bench.lower` / `bench.ll`** are three register-pressure shapes: eight pointers live across a call
with more reads each than there are preserved registers to hold them, the same inside a loop, and the
same with the call on an arm the IR says is taken one time in a thousand. Every load is `volatile` on
the LLVM side, so no common subexpression is removed that ours would still perform. Their loops do not
terminate — they were written so that emitted bytes could be counted, and bytes are all they report.

**`kernels.lower` / `kernels.ll`** are ten runnable shapes of ordinary code, each returning a value
from a bounded loop, so the same source compiled by both backends can be run against the same inputs,
checked for agreement and timed. Loads are plain rather than `volatile` — no kernel reads the same
address twice in an iteration, so there is nothing for the DAG combiner to remove that the other side
would still perform — except `loopCallN`, whose whole point is repeated reads.

**`programs/`** is ten ordinary Yana programs — a sieve, a matrix multiply, a quicksort, a hash
table, a CRC, a binary tree, string scanning, an adaptor chain, typeclass dispatch and a Mandelbrot
count — compiled by `-backend local` and by the same module put through `opt` and then `llc` at `-O3`
and at `-Os`, run against the same input, and timed once all three agree on the answer. This is the
corpus §9 of `findings.md` is built on and the one a change should be ranked from.

**Both LLVM columns have the vectorizers off**, which is what makes them a comparison of the scalar
code. `-Os` does *not* turn them off by itself — in LLVM's `PassBuilder` only `-Oz` sets
`VectorizeOnlyWhenForced`, and SLP needs a speedup level above one, which `-Os` has — so `Pipeline` at
`-Os` carries 66 vector operations and `Matrix` nine. `--force-vector-width=1 --slp-max-vf=1` removes
every one of them at both levels, and `run.py` passes both unless `--vectorize` is given. Every ratio
in §9 through §12 of `findings.md` was measured with them on and is not comparable with §13's or §14's.

Going through `opt` rather than through the driver's own in-process `-opt 3` is what makes that
possible: the two are the same pipeline, and only one of them lets a pass be turned off.

`findings.md` is the standing result and the ranked list of what closes the gap.

## What each corpus can answer

`X64Tester.cpp` parses a `.lower` file and calls codegen. **No pass in `compiler/opt` and none in
`compiler/lower` runs on `bench.lower` or `kernels.lower`** — not `promoteStackSlots`, not the
constant strength reduction, not the loop passes. Those two corpora measure instruction selection,
register allocation and frame layout, they measure them accurately, and they are blind to everything
between the front end and codegen.

`divideByConstant` is the standing demonstration: it still times 4.99x there, while `n / 8` in a real
program has compiled to `sar $0x3` since the strength reduction landed. A kernel can measure code the
shipping pipeline does not emit. Rank from `programs/`; use the kernels to attribute a *backend*
change once it is known to be one.

## The block-move corpora

Two, and neither is a comparison against LLVM.

`memmove/` is the **library**: `copyMemory` and `moveMemory` against glibc's `memcpy` and `memmove`
at forty-nine lengths — the same program written twice, once in Yana and once in C, so that what is
compared is one call each way. `memmove/findings.md` is its standing result and
`python3 memmove/run.py --pin 2` reproduces it.

`blockcopy/` is the **backend**: a constant-size record assignment, which is what reaches
`expandBlockOperations` and what a compiler-generated aggregate copy is. It is a different question
from the one above and needs a different program to ask it — `copyMemory` is a body in the language
now, so a constant count folds into *that* ladder rather than into the backend's, and a benchmark
written over `copyMemory` would measure the wrong one. Six sizes chosen against the ragged-end
schemes rather than round; `blockcopy/findings.md` is the standing result.

## Running the program corpus

```sh
python3 programs/run.py                 # the scalar ten
python3 programs/run.py Matrix Text     # named ones
python3 programs/run.py --repeats 7     # more repeats; the minimum is reported
python3 programs/run.py --vectorize     # LLVM's vectorizers back on

python3 programs/run.py --simd --pin 2  # the three vector programs, at both tiers
python3 programs/run.py --all           # both sets
python3 programs/run.py VecString       # named, at both tiers

python3 programs/run.py --all --tier avx2  # every row at v3, the scalar ten included

python3 programs/run.py --inline speed --pin 2   # the local column at another -inline level

YANA_FUNC_ALIGN=512 python3 programs/run.py --repeats 9   # the only way to rank a change

for p in 0 8 16 24 32 40 48 56; do                        # and the only way to read a ratio
  YANA_FUNC_ALIGN=256 YANA_FUNC_PAD=$p python3 programs/run.py --pin 2 --repeats 5
done

python3 programs/census.py --all --insts --whole   # where a size difference is
```

**`--tier` is what reads the corpus at the other level.** Without it the scalar ten are compiled at
`DEFAULT_TIER` on both sides and only the vector three are read twice - and the ten are not exempt
from the level, because v3 is where `tzcnt`, `bzhi`, `shlx`, `andn` and the three-operand VEX
encodings of the SSE instructions live. §58.2 of `findings.md` is what the second reading is worth:
almost nothing to us (-16 bytes over the ten, all of it VEX re-encoding in `Float`) and -262 to
`llc`, sixty-four instructions of which are one cold `memset` widening from `xmm` to `ymm`.

**`--inline` reaches the local column only**, which is what makes it the right way to ask whether
compiler/opt's one size-for-speed knob is set well: the LLVM columns run `opt` over a module written
at `-opt 0`, where our own inliner has already had its say either way. §61 of `findings.md` is what
that sweep found — the level is worth taking (−2.3% over the ten, and 1.04x → 1.02x against `-O3`),
and raising its budget toward LLVM's is a 10.7% regression for reasons that are about register
allocation rather than about inlining.

**`census.py` is what says where a size difference is**, and its `--whole` mode is not optional after
§57.4. The LLVM module is internalized now, so `opt` inlines each program into its `main` and deletes
what it inlined: 37 functions across the sixteen rows against our 168, and a per-function comparison
has fifteen names to work with. `--whole` counts every function in each image instead. Neither side's
functions have partners, but both images compute the same answer.

**`YANA_FUNC_ALIGN` is not optional when attributing a change.** See §13.0 of `findings.md`: it pads
every function to the given boundary, so a function wider than any in the image pins each one to a
fixed address whatever the others do, and a change that alters one function's size cannot move
another. Without it a fold that removed two bounds checks from one function and touched no other byte
of the program measured **+28 ms** on the program and **-1 ms** on the function. Read the ordinary
build for what ships and the controlled one for what a change is worth.

**`YANA_FUNC_PAD` is the other half of that control, and it is what a *ratio* needs.** The boundary
above pins where each function starts; it cannot change where a loop inside one sits relative to the
32- and 64-byte lines the front end fetches in, because that distance is fixed by the code. The pad
displaces every entry point by the same number of bytes past its boundary, so sweeping it walks every
loop head in the image through the line together while leaving each function's internal layout
byte-identical. Take the minimum per program over a sweep of 0 to 56 in eights.

That is not a refinement, and §55.2 is the demonstration: this backend emitted no loop-head alignment
at all where `llc` emits one on substantially every loop — two of our 165 loop heads were sixteen-byte
aligned against 196 of its 215 — so a single reading compared our alignment luck with their alignment
policy. Swept, the scalar ten went from 1.065x against `-Os` to **1.026x**, and `Text` alone moved 30%.
§57.1 is the fix and took most of that back; §59.2 is the rest of it, choosing the boundary from the
loop's extent rather than fixing it at sixteen, which is what took `Text`'s spread from 29.4% to
2.6%. The sweep is still what a *ratio* has to be read from: a loop longer than
`kLoopAlignMaxWindows` is deliberately not padded at all - `Tree` and `Matrix` still move 12 to 16% -
and every function's position within the image is still whatever the code above it made it. Both
variables default off and the shipping build is unchanged to the byte.

It needs `build-release/compiler/Yana`, plus `opt` and `llc` for the `-Os` column. Each program reads
its seed from **stdin** — that is what stops either backend from folding it away, and it is why the
`test/resolve` fixtures cannot be used for this: `main` there calls everything with literals and
`llc -O3` reduces most of them to a `mov` and a `ret`. Each writes its answer as eight raw bytes,
compared across all three configurations before anything is timed.

Sizes are the sum of the compiler's own `FUNC` symbols rather than the `.text` section, because a
linked executable carries the C startup code and the local backend's does not.

**`main` and `yana.entry` are counted, and before §57 of `findings.md` they were not.** That
exclusion held only while both backends put the same code in the same functions, and they no longer
do: the LLVM module is internalized, so `opt` inlines the whole program into `main` and deletes what
it inlined - a measure that skipped `main` reported the LLVM column as **zero bytes**. Nothing is
excluded now but the C runtime, which neither compiler wrote. Every size number in `findings.md`
before §57 was taken the old way, and the local column is about a fifth smaller under it; §57.4
re-baselines both sides.

## The SIMD programs

`VecInt`, `VecFloat` and `VecString` are explicitly vectorized and are run by `--simd`, each **once
per level** — `sse` is `-enable-inst v2` against `llc -mcpu=x86-64-v2`, `avx2` is `-enable-inst v3`
against `-mcpu=x86-64-v3`. Two rows rather than one, because the lane count is a property of the
*type*: `Vec(Int)` is four lanes at v2 and eight at v3, so the two are two different programs.

**`--pin` is not optional for these six rows, and §47.3 is why.** They run for 3 to 6 ms each, and
this machine is a 13900K: `cpu16-31` are E-cores, so a run that lands on one is measuring the
scheduler. It shows up as rows that move by tens of percent between runs — `VecFloat/sse` reported
0.85x and then 0.62x against the same binaries — and, worse, as a result that is not about either
compiler: the same pair of functions times **0.89x on a P-core and 1.00x on an E-core**. The scalar
ten run for 130 to 250 ms and are far less exposed, which is why this is a flag and not the default.
`--pin 2` is a P-core here; check `/sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq` on another
machine, where the higher number is the P-cores.

**The level is named rather than spelled out, and that is §41.0's correction.** The `avx2` row used
to pass `-mattr=+avx2,+bmi,+fma,+f16c,+lzcnt,+movbe`, which is v3 written from memory — and a list
written from memory is a list with something missing from it. BMI2 was, which stopped being harmless
the moment this backend began emitting `bzhi`. A level cannot be short a member.

**Every build here names its level, including the scalar ten**, and that half was missing outright:
`-mattr` was passed only for a *named* tier, so the ten were compiled by this compiler at v2 and by
`llc` at the bare `x86-64` baseline — no SSE4.2, no POPCNT, no CMPXCHG16B. Every ratio the ten
produced from §9 to §40 was against an LLVM given less to work with than we had, and the columns from
§41 on are not comparable with them. A build that names no level is compiled for the machine that ran
it (see `TargetExtensions`), and a corpus whose numbers depend on which machine ran it measures
nothing.

`-enable-inst` goes on the **front-end** run of the LLVM build as well as on `llc`. The width is
decided when the module is written, so a module written at v2 is a 128-bit program however `llc` is
then invoked.

They are deliberately **not** in `PROGRAMS`. The scalar ten are what every ratio from §9 of
`findings.md` onward is stated against, and adding a row to that set would change what "the corpus"
means. `--all` runs both.

**The two columns used to disagree here by 2.5x and no longer do.** On the vector programs they were
1.23x and 3.25x, because `-O3` inlined the continuation the iteration protocol hands the loop body
and `-Os` did not — so `-Os` had the same shape as this backend and its ratio was the quality of the
vector code, while the `-O3` ratio was the cost of the shape. §35 of `findings.md` closed that: the
chain is flattened in the shared tier, so all three columns compile one loop and the ratios are
1.13x and 1.17x. §33 is what the gap was and §35 is what it took.

One constraint the programs are still written around, recorded in §9.6 of `findings.md`: `push` past
about 131000 elements fails silently. `push` itself is now a capacity test and a call to `@noinline
growArray` rather than one body — see §14.3, and note that `reserve` no longer appears in a compiled
program at all. `Text.yana`'s size was bounded by a second one — a string
temporary leaking 64 bytes against a 4 MiB heap — until that was fixed, and its loop counts are now
chosen rather than forced.

## Running the kernels

Sizes, both corpora:

```sh
mkdir -p run/x64 && cp bench.lower kernels.lower run/x64/
(cd run && ../../../build/test/YanaX64Test generate)

cd run
sed -e 's/^define i64 @\(\w*\)(\(.*\)) {/define i64 @\1(\2) optsize {/' ../bench.ll > bench_os.ll
sed -e 's/^define \(i64\|i32\) @\(\w*\)(\(.*\)) {/define \1 @\2(\3) optsize minsize {/' ../kernels.ll > kernels_os.ll
llc -O3 -filetype=obj -o o3.o ../bench.ll   && llc -O2 -filetype=obj -o os.o bench_os.ll
llc -O3 -filetype=obj -o k_o3.o ../kernels.ll && llc -O2 -filetype=obj -o k_os.o kernels_os.ll
```

`run/x64/*.lower.expect` carries our bytes per instruction; `llvm-nm --print-size` gives LLVM's per
function.

Times. The `.expect` file's hex is extracted into a linkable object — the tester emits every function
into one buffer with relocations resolved, so concatenating them in order reproduces the image
exactly, and `objdump` confirms each `call` lands on its callee:

```sh
python3 extract.py run/x64/kernels.lower.expect kernels   # -> kernels.bin, kernels.sym
python3 mkasm.py kernels yana_ kernels.s
gcc -c -o kernels.o kernels.s
gcc -O2 -fno-pie -no-pie -o harness harness.c kernels.o run/k_o3.o && ./harness
```

`harness.c` calls both sides, **compares the answers before timing anything**, runs them alternately
so a frequency step lands on both, and reports the minimum over the repeats.

Attribution. `variants.s` holds the backend's own output with exactly one optimization applied by
hand, so the difference against the baseline is that one change and nothing else:

```sh
gcc -c -o variants.o variants.s
gcc -O2 -fno-pie -no-pie -o attribute attribute.c kernels.o run/k_o3.o variants.o && ./attribute
```

`chunkloop.s` and `chunkloop.c` are the same idea one level down, and §58.5 is why they exist: two
loops that are seven instructions on both sides and 1.66x apart, where reading the disassembly says
nothing and the displacement sweep says it is not placement. Transcribe both out of the linked
binaries, add one variant per suspected cause, and let the machine rank them.

```sh
gcc -O2 -march=x86-64-v2 -o chunkloop chunkloop.c chunkloop.s && taskset -c 2 ./chunkloop
```

It checks that every case returns the same number before it reports a time — the first transcription
seeded the accumulator from the wrong argument and would otherwise have been timed happily.

## Result

Ten real programs and three vector ones, both backends, end to end — the measure that ranks a change.
**`findings.md` §59 is the current writeup** and §58 holds the per-program tables at both levels.
**Vectorizers off on both LLVM columns**, so the scalar ten are scalar code against scalar code.

Layout-controlled (`YANA_FUNC_ALIGN=256`, `YANA_FUNC_PAD` swept 0 to 56, minimum per program), which
is the only reading a ratio may be quoted from:

| | local | `opt -Os` | ratio |
| --- | ---: | ---: | ---: |
| scalar ten, time, v2 | 1612.2 ms | 1568.0 ms | **1.03x** |
| vector three, time, v2 | 11.2 ms | 11.6 ms | **0.97x** |
| vector three, time, v3 | 7.1 ms | 7.2 ms | **0.99x** |
| all sixteen rows, bytes, v2 | 23373 | 22792 | **1.02x** |
| thirteen rows, bytes, v3 | 18227 | 17495 | **1.04x** |

**The scalar half is a tie and the vector half is now under parity at both levels.** It was 1.29x and
1.12x through §58, and what closed it was two instructions in one loop: the chunk index in
`vectors`/`vectorsMasked` was an `Int` sign-extended into every address, and the test recomputed the
next index instead of comparing against a limit hoisted out. §58.5 is the attribution, `chunkloop.s`
is the measurement, and §59.1 is the change — five instructions an iteration where `llc -Os` emits
seven.

**Every ratio before §58 was read at one level (v2) and every size number before §57 was read with
`main` excluded.** Neither is comparable with what is above; §57.4 re-based the size measure and
§58.0 is the first reading at both levels.

Everything from here down is the history, and the size figures in it are the pre-§57 measure.

Eight rows were at or within a few percent of `-Os` on time as of §32 — `Pipeline` **0.90x**, `Bits` 1.00x,
`Sort` 1.01x, `Hash` 1.02x, `Float` 1.03x, `Matrix` 1.05x, `Dispatch` 1.06x, `Text` 1.07x — and the
size column is *under* parity outright. `Pipeline` is the first row faster than both LLVM columns, and
§28 is why: a bounds check a counted loop's own test has already made. The `-Os` column moves whenever
a change lands in a tier both backends read, and §25 is by far the largest instance: it took 166 ms
off `-Os` as well as 198 off ours, so read the first column against its own history and not only the
ratio. See the note below.

**And read `test/resolve` beside it.** Two of §20's three items are worth nothing on these ten
programs and a third of a percent over the 149 `test/resolve` fixtures that build to executables,
which is 149 whole programs against ten. Sum the `t`/`T` symbol sizes of each one compiled with
`-mode exe -backend local`; §20.1 is the worked example of a change that column ranks correctly and
this one nearly missed; §21 and §22 are two more, the first at 46 sites there against five functions
here.

§32 is the current round, and it is the one this corpus cannot see at all: its image is byte-identical
here, and what it is worth is −495 bytes over the 155 `test/resolve` executables — an early return's
teardown, written down once instead of once per way out. Its other two items are an induction
extension that reaches nothing on either corpus and one that was built, measured and taken back out;
§32.3 records what would unblock the second, and it is one block boundary in the x64 backend.

§30 is three rounds back, and it is the one that landed almost nothing and found the most. Two of its
four items were measured out — the loop-rotation relaxations §29 asked for do reach `Hash`'s probe
loop, do rotate it into `llc -Os`'s instructions exactly, and are worth **no measurable time**, since
that loop is bound by a random-access miss and not by a branch; and `Matrix`'s inner bounds check is
not one addition away from §28's proof but two facts nothing local has. What the round is worth is
§30.5: **a quarter of `Tree` is a traversal of the whole tree that does nothing at all**, because
`ownershipOf` answers a recursive type from the pessimistic end of its own cycle and gives every one
of them a `Drop` it does not need. Measured at **204.7 → 155.5 ms**, 1.12x → 0.97x, and it is the head
of the nineteenth list.

§27, §28 and §29 are the round before it, and it is the pattern the three before them had: two of the
three items that landed are in the shared lower tier and the backend one measured its way out. §27
lets the forwarding move the *allocator call* above the writes it is redirecting — the one callee a
lower pass may know anything about, because the compiler wrote the call — so `Tree.build` builds each
child straight into its heap node (**214.6 → 205.1 ms**, 1.18x → 1.13x). §28 proves a counted loop's
bounds check redundant against the loop's own test rather than unswitching it, which is `Pipeline`
**53.2 → 44.3 ms** and the row that is now under both LLVM columns. §29 is the one that did not land:
`Hash`'s insert phase is one unconditional jump per probe, the loop rotation that would remove it was
generalized three ways and built, and it cost 147 bytes over `test/resolve` for no time — and does not
reach that loop anyway, whose preheader is the function's entry block.

§26 is the round before them and it is not a backend one either. The whole of `Hash`'s gap was one call
the inliner refused, on a size three times the body it was measuring: a callee nothing was inlined
*into* was being judged in the form the resolver wrote it, where a mutable local is an `alloc`, a load
per read and a store per write and none of that survives — so `mix` sized at 24 where a backend emits
8. **237.7 → 220.9 ms**, 1.10x → **1.02x**, and 78187 → 77249 bytes over the 152 `test/resolve`
executables, which makes it the first round on this list that is smaller *and* faster. §26.4 is why
two penalties had to move with it.

§25 is the round before it and it is not a backend one at all: the adaptor chain in `Pipeline.yana` is
inlined flat, so `chained` compiles to the loop the hand-written `direct` beside it is. **252.0 →
54.1 ms**, 5.04x against `-O3` → 1.04x, and 123,776 → 116,728 bytes over the `test/resolve`
executables — a 5.7% size move where every round before it was worth tens to a few hundred bytes.
Five things were in the way, and three of them were flags outside `compiler/opt` that say who frees a
closure's environment being read as saying what can reach it.

§24 is the round before it, and it is the two oldest items on the list: accumulator recursion turned
into a loop (`Tree` 1.19x → 1.15x) and a narrow loop counter carried at the width its addresses are
computed in (`Text` 1.33x → **1.10x**). Both were costed against the resolve IR and both are small
transforms over the lower one — see the note there on why that is now the third time running.

§23 is the round before it: a register segment around a cluster of reads, which is what `loopCallN`
was made of (256 → 232 bytes) and what two `test/resolve` binaries and `Matrix` each lose a repeated
`lea` to. It also sizes what is left of the item — see the table there.

§15, §16 and §17 are the three rounds ranked on the size column rather than the time one. §15 spent
25 ms for 1790 bytes and the 25 ms is layout rather than instructions (§15.1); §16 took 933 bytes back
and gave the 25 ms back with them; §17 took a further 974 and cost nothing at all. One caution the
census turned up: **`-Os` spends 593 of its 13409 bytes on alignment padding and this backend spends
none**, so the size ratio flatters us by about 4%. Against LLVM's real code the number is 1.04x.

We are *smaller* than `-O3`; `Bits` is 0.99x against `-Os` and `Matrix` 1.01x. **Read the `-Os` column
when ranking a fix** — `-O3` turns on unrolling and more aggressive loop deletion, which are passes
rather than gaps, and on `Pipeline` it closed-forms the whole adaptor chain. That is *not* the
vectorizer: with both vectorizers off it still reports 5.0x there.

**And read the first column against its own history, not only the ratio.** §11 is the round that made
this matter: five of its six items are in tiers *both* backends read, so the abort-arm change worth
0.95x to us was worth 0.93x to `-Os` as well and the ratio came out unchanged. A shared-IR improvement
moves the denominator too; only a backend-only one shows up in the ratio alone.

One thing the corpus can now rule out rather than suggest: **bounds checks are not the gap.** Compiling
both sides with and without them puts our whole check cost at 216 ms of 2362 and LLVM's at 316 ms of
1866, so removing every check we emit would still leave us 1.38x behind `-Os` with its checks off. What
is left is per-iteration instruction count in the hot loop, from the six causes §10 ranks. Those
numbers predate §11, whose largest item is about the *shape* a check compiles to rather than its cost;
re-run `programs/checkcost.py` before quoting them again.

Bytes on the kernels corpus, from the revision that added §5.8 of `compiler/codegen/x64/README.md` to
the load fold (§5 of `findings.md`):

| | without splitting | with | after §5 | `llc -O3` | `llc` + `optsize` |
| --- | --- | --- | --- | --- | --- |
| `acrossCall` | 297 | 238 | 169 | 142 | 142 |
| `loopCall` | 363 | 324 | 255 | 182 | 166 |
| `coldCall` | 247 | 216 | 195 | 158 | 153 |
| total | 907 | 778 | 619 | 482 | 461 |

Splitting closed a third of the gap and the load fold closed more than half of what that left. These
shapes read each of eight pointers more times than there are registers to hold them, so nearly every
load in them has exactly one reader directly below it, which is the case the fold is for. Rotation
(§6 of `findings.md`) then put 25 bytes back — 623 to 648 — which it is meant to: these are the
shapes under the most pressure, so the merge it adds at each loop's exit is a pair of stack moves
rather than a coalesce.

Ordinary code, both measures, and what `findings.md` has moved so far:

| | yana | after §2 | after §4 | after §5 | after §6 | after §7 | `llc -O3` | ratio |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| bytes | 800 | 778 | 771 | 714 | 772 | 732 | 587 | 1.36x → 1.25x |
| time | 269056 µs | 238452 µs | 222600 µs | 221202 µs | 207600 µs | 207670 µs | ~187000 µs | 1.46x → 1.11x |

§7's time column is the alignment-controlled measurement, not the one the plain image reports. The
plain image says 213600 µs, all of which is `sumMatrix` landing differently for a change of three
bytes in its entry block; see the caution below and §7 of `findings.md`.

The `llc` column is not one number across those runs — it is the same object file measured in five
images, and it has drifted 3% between them for the reason the caution below gives. The ratios are each
against the `llc` measurement from their own run. Bytes went back up under §6 and were meant to: it is
the one item on the list that spends bytes to remove a branch.

`padasm.py` builds two images with every function padded to a common length, which is the control the
caution below describes, done for both images at once rather than by hand:

```sh
python3 extract.py base/x64/kernels.lower.expect kbase   # the image being compared against
python3 extract.py run/x64/kernels.lower.expect kernels
python3 padasm.py kbase kernels kbase_p.s kernels_p.s
```

**It cannot be used on an image containing a call**, which today means it cannot be used on the
kernels: the padding moves `sink` relative to `loopCallN`, and the `call` between them is the resolved
rel32 the caution below names — so the padded image runs off into the padding and segfaults, rather
than measuring anything. It is a control for `bench.lower`, whose shapes call nothing, and for any
per-case image built the way §7 of `findings.md` builds one, where the padding goes *before* the
function being controlled rather than between two that reference each other.

A caution about timing this corpus. The functions are concatenated into one image at the offsets the
tester emitted them at, and `mkasm.py` cannot pad between them — the internal `call` is a resolved
rel32 — so changing the size of one function moves every one below it, and a hot loop that lands
across a 32-byte boundary differently is worth up to 40% on its own. Both columns move: `llc`'s own
unchanged code has measured 12% apart between two such runs. To attribute a change to the change,
rebuild the image with the affected functions padded back to their original lengths with `0x90` after
the `ret`, so that every other function keeps its address and measures as its own control.

Padding cannot control for a function whose *own body* changed length, since its loop then spans a
different set of 32-byte lines whatever address the function starts at. For that, sweep: pad the
function above it by 0 to 31 bytes and time the kernel at each shift, on both images. `sumMatrix`
under §5 of `findings.md` is the worked example — its measurements span 21.6k to 31.4k µs across one
sweep, and the two images' best cases are 8% apart, which is the difference that is actually real.

The two disagree in sign on `divideByConstant`, which is *smaller* than `llc -O3` and five times
slower, so neither measure stands in for the other. See `findings.md` for the per-kernel breakdown,
the attribution of the gap to six specific optimizations, and the order to do them in.
