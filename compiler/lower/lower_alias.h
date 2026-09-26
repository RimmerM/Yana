#pragma once

#include "lower.h"

/*
 * A frame copy of a record that is only read, made a read of the record it was copied from - J178.
 *
 * The consumer half of an iterator object's hand-over (`doc/inflight/Analysis-Native-Parity.md`
 * gap 4). `let token = step(lx, ..)` over `if self.resume(c) is Just(t): return t` copies the item
 * out of the object's slot, because `step` answers a `Token` by value; by this tier that is eight
 * loads from `%it + 152`, a fresh 58-byte `alloca` the eight words are stored into, and a phi that
 * joins it with the `Nothing` arm's own local, which the loop then reads fields of and hands to
 * `advance`. Seventeen instructions a token, and nothing reads the copy that could not have read
 * the slot: the next write to the slot is the next step, and the copy is dead by then.
 *
 * The rewrite points every reader of the local at the source and drops the stores. It holds when:
 *
 *  - **the local is a whole copy**: stores that tile its bytes exactly, each of a load at the same
 *    offset from one source place, all in the local's block;
 *  - **every reader only reads** - a load, a comparison, a constant offset of those, a call argument
 *    at a parameter the callee neither writes through nor keeps (`paramEffects`), or one phi in a
 *    successor whose readers are all of those kinds and all in that block;
 *  - **the source is an allocation of this frame nothing keeps**, by the same walk, so what can
 *    write it is exactly what is handed a pointer into it;
 *  - **nothing from the first load to the last reader writes the source's bytes** - a store, a copy
 *    or a fill landing in them, or a call handed a pointer into the source at a parameter it may
 *    write through.
 *
 * **Why this tier and not `resolve_ssa/opt`.** At SSA the source is behind a `Maybe(&Token)` stored
 * in a frame slot and the copy crosses a block parameter; here promotion and the null-test threading
 * (J177) have already reduced it to one alloca and one phi, and the language facts the SSA tier
 * would read off the declarations - a `shared` parameter is read-only, a call-bounded borrow is not
 * kept - are recovered from the callees' lowered bodies instead.
 *
 * **On both backends, and measured on both**, because `test/bench/lexer/findings.md` §16 is a
 * rewrite of this neighbourhood that one backend paid for. It was built behind
 * `LowerModule::consumer` first and the LLVM column taken with the gate lifted: iter 320.9 -> 312.1
 * and idiom/iter 329.3 -> 319.5 instructions a token, 3-4% of cycles. LLVM does not make this one
 * itself - what it would need is that nothing between the slot's read and the last reader writes
 * the object, and the object's address has been handed to `lex$step` - so the gate came off.
 */
void aliasReadOnlyCopies(LowerBase base, LowerModule& module, LowerFunction& fun);
