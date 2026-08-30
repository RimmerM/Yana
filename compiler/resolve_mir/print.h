#pragma once

#include "decl.h"
#include "Net/Stream.h"

namespace mir {

/*
 * The MIR dump - `-mode mir`, and the `.mir.expect` fixture golden.
 *
 * What it prints is exactly what a pass may read: every operand's use mode, every place's path,
 * every block's parameters and every edge's arguments. That is the point of asserting it at all -
 * the failures Analysis-IR.md part 1 collects are all a relation the IR did not state, and a dump
 * that hid the modes would be a golden that could not have caught any of them.
 *
 * Deliberately not a superset of what the old resolver prints: the two representations have nothing
 * to compare line by line, so its goldens are separate files and a fixture opts into this one by
 * having a `.mir.expect` beside it (Part 11.3).
 */
void printProgram(Net::Writer& writer, Context& context, Program& program);

// One function, for a diagnostic or a debugger. `describeFunction` in the other stages is the same
// affordance.
void printFunction(Net::Writer& writer, Context& context, Program& program, Function& function);

/*
 * A place, spelled the way the author wrote it - `p.left`, `counter`, `global live`.
 *
 * For diagnostics rather than for the dump, and the dump's own place printer is a `Writer` walk that
 * this shares nothing with beyond agreeing on the spelling. It exists because two passes need to
 * name a place in a message - the resolver, when a position asks for ownership storage cannot give,
 * and the ownership analysis, when a path was moved or is still live - and a message that named the
 * root binding for a projection cannot say which of a record's fields it means.
 *
 * The bytes are the returned `String`'s, not the name table's: a diagnostic formats it and drops it,
 * and interning one name per projection would grow a table that lives for the whole compilation for
 * text nothing looks up.
 */
String placeText(Context& context, DeclBase base, MirBase mir, Body& body,
                 const Place& place);

// A type and its contract, as the dump writes them. Shared with the SSA printer so the two stages
// spell one occurrence one way.
void printOccurrence(Net::Writer& writer, Context& context, DeclBase base, const Occurrence& occurrence);
void printType(Net::Writer& writer, Context& context, DeclBase base, DeclPtr<Type> type);
void printConstant(Net::Writer& writer, DeclBase base, const Constant& constant);

} // namespace mir
