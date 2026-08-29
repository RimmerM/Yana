#pragma once

#include "decl.h"

namespace mir {

/*
 * The construction verifier - Analysis-IR.md §6.1.
 *
 * It asks structural questions only: does every operand name something that exists, does every
 * ownership-bearing operand have a mode, does each block end in exactly one terminator, do a
 * predecessor's edge operands match its successor's parameters, is every place well-formed. It does
 * not ask whether a `Forward` should have been a `Take`, where a loan ends or whether a path
 * initializes - those need the dataflow passes and their absence here is deliberate, because a
 * verifier that half-answers a dataflow question is a verifier nobody believes.
 *
 * The difference from the current resolver's verifier is what a *newly added* node does. There, an
 * unknown ownership effect defaults to absence and passes; here, a kind that has not declared its
 * operand schema does not compile at all (see `forEachOperand`), and one whose stored structure
 * disagrees with what it declared is reported by name.
 *
 * Returns false and reports through `context.diagnostics` as an internal error. Called at every seal
 * in an assertion build and once before conversion in every build.
 */
bool verifyBody(Context& context, Program& program, Function& function);

// Every reachable function's body. The entry point a driver calls once resolution has finished.
bool verifyProgram(Context& context, Program& program);

} // namespace mir
