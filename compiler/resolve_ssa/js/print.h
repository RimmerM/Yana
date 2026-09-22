#pragma once

#include "repr.h"
#include "Net/Stream.h"

namespace jsir {

/*
 * `-mode jsir` - Analysis-IR-JS.md §8.4.
 *
 * The tier has a golden from its first fixture, which is the argument `-mode mir` and `-mode ssa`
 * already won for the native rewrite: the alternative - an internal dump behind a debug switch - is
 * how a representation acquires a second reader before it has one golden.
 *
 * **No arena offsets and no generated C++ IDs**, so a fixture does not depend on allocation order.
 * Everything printed is a dense body-local number or a name the program contains.
 */

// The representation and shape declarations, once, at the top of the dump.
void printTables(Net::Writer& writer, JsTarget& target);

// One function: its ABI, its blocks with arguments and edge operands, its places, and its values
// with their representations.
void printBody(Net::Writer& writer, JsTarget& target, JsBody& body);

// One representation, as the dump spells it - shared with the diagnostics, so a report about a
// representation names it the way the dump does.
void printRep(Net::Writer& writer, JsTarget& target, JsRepId rep);

} // namespace jsir
