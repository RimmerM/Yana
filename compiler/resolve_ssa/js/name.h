#pragma once

#include "../../compiler/context.h"

/*
 * Identifiers on this target, and the one place the rules for one are written.
 *
 * Here rather than in `codegen/js/name.cpp`, where all of it used to live, because a *property name*
 * is a representation fact: `JsShape` records the final key, so the shape table has to be able to
 * produce one and it may not include the AST layer to do it (§8.1's include boundary). The AST layer
 * does include this one, and `codegen/js/name.cpp` is the identifier half - uniquing against the
 * names already handed out, which needs the file being emitted and is not a question about a shape.
 *
 * `+=(Int)` and `Num(Int).+` are real function names in this compiler, so sanitizing is the common
 * case rather than a corner one, and two names that sanitize alike have to end up as two
 * identifiers. That second half stays with the uniquing; what is here is the first.
 */
namespace jsir {

/*
 * The property keys that are the *target's* convention rather than the program's.
 *
 * `$tag` is a sum's discriminant, `$p` a constructor's non-tuple payload, `$v` the one property of
 * the box an escaping scalar is stored in, `$c`/`$e` the two words of a function value, `$h` the
 * closure header hung on a code word, and `$o`/`$k`/`$ke`/`$s` the parts of a reference.
 *
 * Here, in the table that records a `JsShape`'s final key, because the *representation* is what
 * decides them and two spellings of one key are two hidden classes for one record - a value built
 * by one generator and read by the other answers `undefined`, and there is no diagnostic for that.
 * Four places spelled them: `repr.cpp` interning shape properties, `codegen/js/name.cpp`'s
 * `nameSharedFields`, `gen.cpp`'s reference fields, and the two backends' argument flatteners.
 *
 * A `constexpr` table rather than an interned set, because the two consumers intern into different
 * things - a `StringId` in the shape table, a `Name` in the file being emitted - and what has to
 * agree is the text.
 */
namespace key {

constexpr StringView tag = "$tag"_v;
constexpr StringView payload = "$p"_v;
constexpr StringView box = "$v"_v;
constexpr StringView code = "$c"_v;
constexpr StringView env = "$e"_v;
constexpr StringView header = "$h"_v;
constexpr StringView owner = "$o"_v;
constexpr StringView index = "$k"_v;
constexpr StringView envIndex = "$ke"_v;
constexpr StringView scale = "$s"_v;

} // namespace key

// Whether a character may appear in a JavaScript identifier after the first position.
bool jsIdentifierPart(char c);

// Whether this text is a word an emitted identifier may not be. Includes the handful of globals the
// emitted code itself reaches for - `Math`, `BigInt`, `Array` - since shadowing one of those is the
// same failure as shadowing `return`.
bool jsReservedWord(StringView text);

/*
 * A source name as a JavaScript identifier, into `target`, answering the length written.
 *
 * Mapping every operator character to `_` would make `Num(Int).+` and `Num(Int).-` two names that
 * read alike. Spelling each operator out keeps the emitted code debuggable, which Analysis-JS.md
 * §3.6 asks for and is a large part of what this target is for; uniquing is what guarantees
 * distinctness regardless.
 */
Size jsSanitize(StringView text, char* target, Size capacity);

/*
 * Interning text the compiler built rather than text the source contained.
 *
 * `Context::addUnqualifiedName` keeps the pointer it is given instead of copying: every other caller
 * hands it a slice of source text, which outlives the compilation. Nothing built here does, so the
 * text is copied into the string arena first - and only the first time, since a field name is built
 * again at every projection that reads it.
 *
 * `interned` is the caller's record of what it has already copied. Two of them exist - the shape
 * table's and the emitted file's - and they may both hold an id without disagreeing, because what
 * the second copy would write is the same bytes.
 */
StringId jsInternText(Context& context, HashSet<StringId>& interned, StringView text);

} // namespace jsir
