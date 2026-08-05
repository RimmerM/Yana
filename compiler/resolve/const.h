#pragma once

#include "module.h"

/*
 * Constant evaluation.
 *
 * Two declaration forms start something at a value without running any code - a module-level `let`
 * and a field default - and neither has a program point at which an expression could be evaluated.
 * What they accept is therefore a *constant*: a value that the declaration reduces to and that
 * nothing downstream has to run.
 *
 * The two used to answer that separately, and the divergence was a bug rather than a style
 * complaint: `declareGlobal` took the type from `:: T` and never asked whether the literal in front
 * of it was a thing a `T` could be, so `let &slot = 0 :: Pair` declared a record's worth of zeroed
 * static storage that every later assignment would pre-drop as though it held a constructed value.
 * Half of that was closed by giving both the same "what bits does this literal have at this type"
 * helper; this is the other half - the *forms* a constant may be written in, which the two still
 * disagreed about. A field default accepted a nullary constructor and a global did not, so `False`
 * was a constant of `Bool` in one position and a syntax error in the other, for no reason either
 * could state.
 *
 * So there is one evaluator, and a position is a noun it words its diagnostics with. What differs
 * between the two callers afterwards is only where the answer is recorded: a `FieldDefault` on the
 * constructor, or the type and initial bits of a `Global`.
 */

/*
 * A constant: a value of a known type, as the bits its storage holds.
 *
 * Bits rather than a number for the reason `floatBits` gives - the conversion is at the type's own
 * width, so a `Float` constant is four bytes of single precision and not a truncated double, and
 * nothing downstream has to convert again. An enumeration's constant is its constructor index,
 * which is what a value of an enum record *is* (see `resolveConstruct`).
 *
 * There is deliberately no third case for aggregates. A record's constant would have to be bytes,
 * and bytes are not a thing resolve knows: what a field's offset is, how wide an address is and
 * which end of it comes first are the emitting target's answers, which is why a compiler-built
 * table is a list of `TableSlot`s here and becomes bytes only in `lowerProgram`. A source-level
 * aggregate constant is that same machinery pointed at a source type, and it is a feature rather
 * than a refactor - see Analysis-Status.md point 9.
 */
struct Constant {
    TypePtr type = nullptr;
    U64 bits = 0;

    explicit operator bool() const { return type != nullptr; }
};

/*
 * The constant `expr` was written as, or a null one where it is not a constant of the type this
 * position requires - reported here, since the reason is the case analysis below and restating it
 * at a call site is what this exists to stop.
 *
 * `expected` is the type the position already has, and null where the position has none: a field
 * knows its type and a global is told one by its `:: T`, or takes the literal's own default. A
 * position that has one still accepts an ascription, and the two then have to agree.
 *
 * `what` names the position - "a global's initializer", "a field default" - and is the only thing
 * the diagnostics differ by.
 */
Constant evaluateConstant(Module& module, const ast::Expr& expr, TypePtr expected, StringView what);
