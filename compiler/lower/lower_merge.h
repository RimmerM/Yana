#pragma once

#include "lower_inst.h"

/*
 * The same exit, written down several times.
 *
 * Inlining is what produces these. A bounds check's failure arm is a syscall and an `Unreachable` -
 * three machine instructions with no operands anything else can tell apart - and a body with four
 * checked subscripts copied into its caller brings four identical copies of it along. `main` in
 * test/resolve/Adaptor.yana held seven; `resize(Int)` holds three blocks that are nothing but
 * `ret 0`, one per way its allocation can fail.
 *
 * Nothing above here can collapse them. The resolve tier has already run - `endNonReturningBlocks`
 * cut the call that made these arms *reachable* as one function, which is what turned them into
 * bare syscalls in the first place - and by the time the caller holds four copies they are ordinary
 * blocks of ordinary instructions. So the question is asked where the copies are: two blocks that
 * compute the same thing out of the same values and leave the function the same way are one block,
 * whatever their instructions happen to be named.
 *
 * ## Why only a block control never leaves
 *
 * A block with no successors - one ending in `Ret` or `Unreachable` - is the case that needs no phi
 * surgery and no dominance question, and both of those are the whole difference between this and a
 * general tail merge.
 *
 * It dominates nothing, so no value it defines can be read anywhere else, so unifying two of them
 * cannot leave a reader holding a definition that no longer runs. And it appears in no phi's source
 * list, so retargeting the edges into it is the whole of the rewrite: nothing downstream has an
 * opinion about which of the two copies control arrived through.
 *
 * The general case - two blocks that both `jmp X` - is deliberately left alone. It needs the phis in
 * `X` to agree about the two edges *and* one of the two source entries removed from each of them,
 * which is a rebuild rather than an edit (a phi's alternatives are allocated with it). What it would
 * buy is one jump per merged pair, against the three-to-six instructions a duplicated exit is.
 *
 * ## Where it runs
 *
 * Last, after every fold, every strength reduction and the CSE. Two copies of an exit are only
 * identical once whatever they compute has been folded to the same shape in both, and this removes
 * blocks rather than instructions - so nothing below it gains from running behind it.
 *
 * The x64 backend has a rewind of its own (§7.2.1, `genFunction`) that compares emitted *bytes* and
 * merges a return tail into an earlier one. The two do not overlap: that one is confined to blocks
 * ending in `Ret` under a shared epilogue and cannot touch an abort arm, and it runs per target
 * where this runs once for every backend that reads this IR.
 */
void mergeIdenticalExits(LowerBase base, LowerModule& module, LowerFunction& fun);
