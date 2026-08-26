#pragma once

/*
 * The type system, shared between the files it is split across.
 *
 * `type.h` is the interface - everything the rest of the resolver asks a type is declared there.
 * What is here is the seam between these seven translation units, which are one subsystem cut where
 * its questions differ rather than where a size limit fell:
 *
 *  - type.cpp         - the type universe. Interning, generic instantiation, substitution,
 *                       structural matching, and identity.
 *  - type_query.cpp   - classification. What a type *is*, asked without reference to how it is laid
 *                       out: the kind predicates, and the element a container hands back.
 *  - type_ast.cpp     - written types. Turning what the source says into a type, including the
 *                       attributes and refinements that only exist in written form.
 *  - type_schema.cpp  - generic environments. Which slot of a schema holds which type, witness,
 *                       property or function, and how one is grown as a requirement is discovered.
 *  - type_layout.cpp  - layout. Recursion through a type's members: breaking cycles, proving
 *                       acyclicity, and the widths and bit packing a scalar representation implies.
 *  - type_own.cpp     - ownership. What a type owes on the way out, folded from what its members
 *                       owe - which is the one classification that is neither structural nor
 *                       physical.
 *  - type_print.cpp   - describing a type in the words a diagnostic uses.
 *
 * Only what has a caller in another one of those files is declared here; everything else stays
 * static where it is defined, which is nearly all of it.
 */

#include "type.h"

// A reported failure, as a type. The error scalar is what resolution continues with, so that one
// bad written type is one diagnostic rather than a cascade - see resolveType, which returns this
// rather than null for exactly that reason.
TypePtr errorType(Module& module, LocationId source, StringView message);

// A written loan label, as a group index, introducing it into `env` if this is its first occurrence.
// `unlabelled` is what no label means here, which differs between a parameter marker and a type -
// see the definition.
LoanGroup resolveLoanGroup(Module& module, GenEnv* env, StringId label, LocationId source,
                           LoanGroup unlabelled = kNoLoan);

