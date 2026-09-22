#pragma once

#include "build.h"

namespace jsir {

/*
 * One optimized SSA body as JSIR - Analysis-IR-JS.md §5.2 and §5.3.
 *
 * A demand pass first, then one linear walk. The order is the whole point: whether a local needs a
 * cell and whether a value needs materializing are facts about its *later* uses, and guessing them
 * in one source-order pass and repairing afterwards is the failure mode §5.4 names.
 *
 * Answers false where something was reported. The body may be partly built when it does - a sweep
 * wants every construct a program reaches named rather than the first, so conversion continues past
 * a report and the caller is what stops.
 */
bool convertFunction(JsTarget& target, DeclPtr<mir::Function> function, JsBody& body);

/*
 * Whether an address is handed to anybody else, following the reborrows - §5.2's demand question,
 * asked of one value.
 *
 * The per-body pass asks it of every `Alloc` and `planBoxedGlobals` asks it of every
 * `GlobalAddress`, and the two must be the same predicate or a local and a global that escape the
 * same way get two different answers. The scratch is the caller's, because the per-body pass asks
 * this once per allocation and a set allocated inside would be one allocation per local.
 */
bool jsAddressEscapes(JsTarget& target, ssa::Body& from, SsaPtr<ssa::Value> address,
                      Array<SsaPtr<ssa::Value>>& pending, IndexSet& seen);

/*
 * One module-level constant as a body of its own - the whole of this target's constant lowering.
 *
 * `constant` is null for a global with no initializer, which is the representation's zero;
 * `boxed` is `JsTarget::boxedGlobals`. The body is one block whose `return` carries the value, and
 * `codegen/js/structure.cpp`'s `structureConstant` is what spells it as the `var`'s initializer.
 *
 * It exists because a second lowering did, in `assemble.cpp`, and disagreed with this one twice -
 * see `Conversion::runConstant` for what each disagreement answered.
 */
bool convertConstant(JsTarget& target, const mir::Constant* constant, JsRepId rep, bool boxed,
                     LocationId where, JsBody& body);

/*
 * One lambda's **environment teardown**, as a body of its own - §3.2.1's header, this side of it.
 *
 * The one JSIR body with no SSA behind it, and the reason it exists rather than being emitted at
 * the drop: which lambda a function value holds is a run-time fact, so what a drop can reach is a
 * function hung on the code word, and that function has to be a function. It takes the environment
 * record and runs `emitTeardown` over it - the same walk a field of that type would get, which is
 * what keeps the two from disagreeing about what a capture owes.
 *
 * Only asked for where `jsOwesTeardown` says the environment runs something. A lambda that captured
 * an `Int` gets no body, no header and no assignment, and a drop of one of its values finds nothing
 * - which is the same three-way agreement the native stage's zero cell states.
 */
bool convertEnvironmentTeardown(JsTarget& target, DeclPtr<mir::Function> lambda, JsBody& body);

/*
 * One **sum's** teardown, as a body of its own - `JsInstTypeDrop`'s other end.
 *
 * The one release a site cannot write inline, because which arm a value holds is a run-time fact
 * and the walk therefore branches. `teardownHalves` is the native counterpart, keyed the same way:
 * the type and which of the two halves the caller still owes.
 */
bool convertTypeTeardown(JsTarget& target, DeclPtr<mir::Type> type, bool drop, bool reclaim,
                         LocationId where, JsBody& body);

} // namespace jsir
