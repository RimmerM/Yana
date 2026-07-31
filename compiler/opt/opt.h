#pragma once

#include "../resolve/module.h"
#include "../repr/repr.h"

/*
 * The optimizer - see Analysis-Optimization.md, which is the argument for this directory existing
 * at all rather than for the passes inside it.
 *
 * It runs over the *resolve* IR, and it is on the emitting side of the layout wall: it constructs
 * its own ReprTable from the target it was handed, exactly as `lowerProgram` and `js::genProgram`
 * do, and for the same reason - a target is *chosen* by whoever emits rather than inferred from a
 * compile mode. `compiler/resolve` still does not know what a layout is, and nothing in it includes
 * this.
 *
 * ## Why the resolve IR rather than either target's own
 *
 * Because the alternative is writing everything twice. The read-modify-write sequences a packed
 * field lowers to are built below the fork, once in `resolve/lower.cpp` and once in
 * `codegen/js/place.cpp`, so a peephole under either one sees half the program. Putting the fork
 * *below* this stage instead means one constant folder serves the arithmetic the source wrote and
 * the arithmetic the packing invented.
 *
 * ## Why this is the low-risk place to rewrite ownership-bearing IR
 *
 * Every ownership question has been decided and spent before this runs. `runProgramOwnership` has
 * settled its fixpoint, reported every diagnostic, inserted every `Drop` and chosen every
 * `StorageClass`, so what is left is an IR in which each of those decisions is an explicit
 * instruction. A pass that neither creates nor deletes `Init`, `Assign`, `Move`, `Copy`, `Drop`,
 * `Swap`, `Exchange` or `Borrow` cannot change what a program owns - and whether a pass does that
 * is a property of its code rather than a claim about it.
 *
 * What is left of the analyses afterwards is per-*local* - `requirements`, `escapes`, `storage`,
 * `droppable`, which is all `lowerProgram` reads. The instruction numbering and the live ranges are
 * read only by `analyze_print.cpp`, so the ownership dump is printed before this stage rather than
 * after it, and rewriting instructions invalidates no decision.
 */
void optimizeProgram(Context& context, Program& program, const ReprTarget& target);
