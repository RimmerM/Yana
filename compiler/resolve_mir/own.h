#pragma once

#include "decl.h"

namespace mir {

/*
 * Ownership classification - `doc/spec/ownership.md`'s Teardown, and the one thing about a type that
 * a *later declaration* can change.
 *
 * Every other property of a logical type is decided where the type is made: its kind, its width, its
 * fields. Whether it releases something is decided by the whole program, because an
 * `instance Drop(T)` written at the bottom of a file is a statement about a type declared at the
 * top. So it is a pass rather than a query, run once between the declaration walk and the bodies,
 * and everything afterwards reads `ownershipOf` and gets the settled answer.
 *
 * Running it as a query instead is the mistake this shape avoids. A cached query answers differently
 * before and after the instance that changes it, and nothing says which callers ran first - the
 * current compiler needs a fixpoint with an assumption stack for exactly that reason, and it needs
 * one because its classification is asked from inside type construction.
 *
 * Returns false and reports where the member graph has a cycle: a record that contains itself has no
 * layout, and `@box` - the written indirection that breaks one - is not implemented here yet.
 */
bool classifyTypes(Context& context, Program& program);

/*
 * The same fold for one record, for a type that did not exist when the pass ran.
 *
 * A generic instantiation is made while a *body* is being resolved - `Boxed(I32)` is written at a
 * use site - so there is a record after the pass and it still has to be classified before anything
 * reads what it owes. That is not the query the header above argues against: the pass's precondition
 * is that every `instance` is in, and every declaration is in long before any body is walked, so the
 * answer this gives is the one the pass would have given.
 *
 * `Program::typesClassified` is what says which of the two applies, and `instantiateRecord` is the
 * one caller.
 */
bool classifyRecord(Context& context, Program& program, RecordType& record);

/*
 * Where every record's external positions sit - Analysis-IR.md §5.11's ownership schema.
 *
 * A separate pass from `classifyTypes`, and *before* signatures rather than beside ownership,
 * because the two answer different questions from different inputs. A schema is a property of the
 * field declarations alone and nothing later can change it; ownership is a property of the whole
 * instance set, which is why it waits.
 *
 * The order is what a signature needs. `x: Reader(a')` binds `Reader`'s interior positions to this
 * signature's origin slots, so the interned shape has to exist by the time a signature is read - and
 * under the old single walk a function declared above a `data` in the same file was resolved first.
 */
bool foldSchemas(Context& context, Program& program);

/*
 * Whether a body in `from` may trigger the teardown of a value of this type - `@local instance
 * Drop(T)`, `doc/spec/modules.md`'s local declarations.
 *
 * Asked by the two places that dispose of a value, which is why it is a named question rather than
 * four lines in each: the drop walk asks it of a type it can see, and `specializeFunction` asks it
 * of one a substitution has just decided. A generic body that asked neither would launder the
 * restriction - the disposal is placed in the generic's module and runs at whatever type a caller
 * supplies, which is precisely the permission `@local` withholds.
 *
 * True where there is no instance and where the instance is not `@local`, so a caller reads it as
 * permission rather than as the presence of a restriction.
 */
bool disposalPermitted(Context& context, Program& program, DeclPtr<Type> type,
                       DeclPtr<Module> from);

// One record's schema, for the same reason `classifyRecord` exists: an instantiation is a record
// made after the pass, and a signature that binds its interior positions needs the schema to be
// there. Reports and answers false where a field's own type refers to storage the record cannot
// name - the diagnostic the pass gives, given once per record wherever the record was made.
bool foldRecordSchema(Context& context, Program& program, RecordType& record);

/*
 * The one rule a **qualified type argument** has to obey - `Maybe('v)` against `Pair('k, v)`.
 *
 * A qualified argument takes a member away: the payload of a `Maybe('v)` is somebody else's, so what
 * dies with the value and what a copy of it duplicates are not what the *type* answers. The type is
 * where both are answered - a derived teardown is generated per type and descends into the fields
 * the declaration owns - so a value that still owed something after the removal would be released by
 * a walk that could not be told which field to skip.
 *
 * Rather than key a teardown on a shape, the qualification is admitted only where the residue is
 * **nothing**: no authored teardown or copy on the type itself, and nothing left in the fields the
 * shape did not remove. That is every use the language has today - a `Maybe`, an `Entry`, any
 * wrapper whose members are exactly its parameters - and it is what makes `ownershipOfOccurrence` a
 * constant rather than a second fold.
 *
 * A pass rather than a check inside `resolveApplication`, because the answer is `classifyTypes`'s and
 * that runs after every signature is read: an application is resolved long before the program knows
 * whether the type it names releases anything.
 */
bool checkQualifiedShapes(Context& context, Program& program);

} // namespace mir
