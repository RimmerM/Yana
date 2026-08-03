# Register-allocation comparison against LLVM

Three shapes written twice — `bench.lower` in this project's lower IR and `bench.ll` in LLVM IR,
instruction for instruction: the same loads from the same pointers, the same accumulation order, the
same call, the same branch weights, System V on both sides.

Two things make it a comparison of register allocators rather than of compilers. Every load is
`volatile` on the LLVM side, so no common subexpression is removed that ours would still perform;
and the LLVM side goes through **`llc`**, which is LLVM's backend alone, so no mid-level pass runs.
What is left on both sides is instruction selection, register allocation and frame layout.

The shapes are the pressure splitting exists for: eight pointers live across a call with more reads
each than there are preserved registers to hold them, the same inside a loop, and the same with the
call on an arm the IR says is taken one time in a thousand.

## Running it

```sh
mkdir -p run/x64 && cp bench.lower run/x64/
(cd run && ../../../build/test/YanaX64Test generate)

sed -e 's/^define i64 @\(\w*\)(\(.*\)) {/define i64 @\1(\2) optsize {/' bench.ll > bench_os.ll
llc -O3 -filetype=obj -o o3.o bench.ll
llc -O2 -filetype=obj -o os.o bench_os.ll
```

`run/x64/bench.lower.expect` carries our bytes per instruction; `llvm-nm --print-size` gives LLVM's
per function.

## Result

Emitted bytes, at the revision that added §5.8 of `compiler/codegen/x64/README.md`:

| | without splitting | with | `llc -O3` | `llc` + `optsize` |
| --- | --- | --- | --- | --- |
| `acrossCall` | 297 | 238 | 142 | 142 |
| `loopCall` | 363 | 324 | 182 | 166 |
| `coldCall` | 247 | 216 | 158 | 153 |
| total | 907 | 778 | 482 | 461 |

Splitting closes a third of the gap. Most of what remains is instruction selection rather than
allocation — see §8 of the backend README for the breakdown.
