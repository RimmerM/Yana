#pragma once

#include "shared.h"
#include "../../resolve_ssa/js/name.h"
#include "../../resolve_ssa/js/convert.h"
#include "../../resolve_ssa/js/abi.h"
#include "../../resolve_ssa/js/verify.h"
#include "../../resolve_ssa/js/cfg.h"
#include "../../resolve_ssa/js/opt_coalesce.h"
#include "../../resolve_ssa/js/print.h"
#include "../../resolve_mir/specialize.h"
#include "Net/Stream.h"

/*
 * The JSIR pipeline's half of this directory - Analysis-IR-JS.md §8.1.
 *
 * `structure.cpp` turns one verified JSIR body into statements: the CFG becomes `if`, `for(;;)` and
 * labelled `break`/`continue`, block arguments become `var`s and edge assignments, and a place
 * becomes a property or an index. `assemble.cpp` is the file those functions go into - the
 * declarations, the globals, the helper families, the entry call, and the two passes that were
 * always here (`optimizeFile`, `formatFile`).
 *
 * **This is where representation stops being a question.** §8.1's rule is that "ordinary JS AST
 * builders do not receive `TypePtr` as a reason to choose representation", and the stronger form
 * holds here: nothing below this line sees a `mir::Type` at all except to spell a *name*. What a
 * value is, what shape an object has and what a place's protocol is are all read off the JSIR node.
 */
namespace js {

/*
 * What the two files share, on top of `JsShared`.
 *
 * Deliberately small: everything a JSIR body needs is on the body, so what is left is the
 * file-level naming and the one-function scratch. The scratch is reset per function rather than
 * rebuilt, which is `compiler/util/README.md`'s rule applied where it costs one allocation per
 * compilation instead of one per function.
 */
// One per-type teardown this file has been asked for - see `Assembly::typeDropNames`.
struct TypeDrop {
    Name name;
    DeclPtr<mir::Type> type;
    bool drop;
    bool reclaim;
    LocationId source;
};

struct Assembly: JsShared {
    jsir::JsTarget& target;

    // Module-level names, by declaration handle. Handed out once, before any body is emitted, so
    // that a call can name a callee this file has not reached yet.
    HashMap<U32, Name> functionNames;
    HashMap<U32, Name> globalNames;

    /*
     * The environment-taking shims a function value needs, by callee.
     *
     * A call through a pair is `code(env, ...)` unconditionally - that is what makes one convention
     * serve both halves - and a named function that does not already begin with an environment
     * cannot be that code word. The shim is `function f$fn($e, a) { return f(a); }`, emitted once per
     * callee that is ever taken as a value.
     */
    HashMap<U32, Name> thunkNames;
    Array<DeclPtr<mir::Function>> thunkOrder;

    /*
     * The per-type teardowns a `TypeDrop` names, by (type, halves) - `JsInstTypeDrop`.
     *
     * Named on demand and emitted after the bodies, because one of them may ask for another: a sum
     * holding a sum is a call from inside the first function's arm. So the order is a worklist
     * rather than a list, and `typeDropOrder` is walked by index while it grows.
     */
    HashMap<U64, Name> typeDropNames;
    Array<TypeDrop> typeDropOrder;

    /*
     * The erased half - one module-level table per generic environment, per type descriptor and per
     * class witness, named on demand and emitted after the bodies.
     *
     * Three registries rather than one, because the three are emitted in **this order** and that is
     * not cosmetic: a `var` initializer runs where it is written, so a table naming a descriptor
     * that is declared below it captures `undefined`. Descriptors name only functions, which hoist;
     * witnesses name only functions; an environment names both. So the dependency is a line rather
     * than a graph and the emission is three loops.
     */
    HashMap<U32, Name> genTableNames;
    Array<DeclPtr<mir::GenEnvironment>> genTableOrder;

    HashMap<U64, Name> descriptorNames;
    Array<DeclPtr<mir::Type>> descriptorOrder;

    HashMap<U64, Name> witnessNames;
    Array<mir::GenWitness> witnessOrder;
    Array<DeclPtr<mir::Class>> witnessClasses;

    /*
     * `function $nd(x) {}` - the teardown a descriptor of a type that owes nothing points at.
     *
     * A descriptor's teardown cell is **never null**, so that a drop in a body compiled once is one
     * indirect call rather than a test and a branch in the middle of a block the caller is filling -
     * the same rule `lower/erase.cpp` states for the native cell, and the same shared empty body.
     */
    Name noDropHelper;

    /*
     * The sentences a `throw` carries, interned - `var $t0 = "yana: a runtime check failed";`.
     *
     * One message per *text* rather than per site, because a check is the library's own
     * `if ... then checkFailed()` grafted by `@inline(always)` at every subscript: there are 936 of
     * them in `Base.js` and they all throw the same twenty-eight characters. The generator deleted
     * in J173 had no equivalent because its resolver emitted a `checkCondition` **call** at a
     * subscript and the sentence lived once, inside that body - so this is what inlining the check
     * would otherwise cost, measured at 31 KB.
     */
    Array<Name> throwOrder;
    Array<StringId> throwTexts;

    /*
     * The constant tables a `match` over literals reads - `var $m = [1, 4, 0, 2, 5];` - interned by
     * content, so two maps answering the same row share one, and named `$m`, `$m$1`, ... by `uniqueName`. `mapCells` is every table's slots end
     * to end and `mapStart` is where each begins; `mapStart[i + 1]` (or the end) is where it stops.
     * Emitted beside the thrown sentences and for their reason. See `JsInst::Map` in
     * `structure.cpp` for which maps become one.
     */
    Array<Name> mapNames;
    Array<U32> mapStart;
    Array<jsir::JsConstant> mapCells;

    /*
     * Per function.
     */

    // The body being translated. Not named `body`: `JsShared::body` is where statements go, and two
    // fields of one name in one struct is the shape a reader gets wrong exactly once.
    jsir::JsBody* current = nullptr;

    // `JsValueId` -> the expression that value is. A value with no entry has not been emitted, which
    // the structurer reports rather than writing `undefined`.
    Array<JsPtr<Expr>> values;

    /*
     * And the `var` each computed value is bound to, kept so that a value named twice keeps one name.
     *
     * The structurer emits a block **more than once** wherever the dispatch lowering duplicates one,
     * and `uniqueName` hands out a fresh identifier every time it is asked - so the second emission
     * of a block bound its results to `v23$1` while the uses already written said `v23`, and nothing
     * declared that one. A name is a property of the value rather than of the emission, which is
     * what this says.
     */
    Array<Name> valueNames;

    // A `Var` place's name, and a block argument's - the only two things that become a `var` here.
    Array<Name> variables;
    Array<Name> arguments;

    // Which `Var` slots an instruction still names. A slot the JSIR optimizer emptied - a record
    // `splitRecordVariables` replaced by its fields, a field nothing read - is declared by nobody.
    Array<U8> variableNamed;

    // One name per coalescing class, by the value id of the class's representative - see
    // `jsir::JsCoalesced`, which is what says which arguments share one.
    Array<Name> classNames;

    /*
     * What the structuring adds to the CFG, and it is the whole of what it adds.
     *
     * The graph itself - reverse postorder, immediate dominators, loop membership, reachability -
     * is `jsir::JsCfg`'s since 2026-09-22 and is computed once per body, above this boundary: it is
     * the same graph the verifier and the JSIR optimizer walk, and there were three answers to it.
     * There is no post-dominance in any of them and that is what changed at J73: the structuring is
     * over the *dominator tree*, and the question post-dominance used to answer - which of two
     * joins is the outer one - is the reverse postorder number instead.
     *
     * Blocks that own a label in their immediate dominator's scope - every block with two ways
     * forward into it, and every block a loop leaves to.
     */
    IndexSet labelled;
    IndexSet emitted;
    Array<Exit> exits;
};

/*
 * A compiler-minted module-level name: a prefix and a sanitized spelling of what it names.
 *
 * `$d$Person`, `$w$Show`, `$td$Maybe_I32`, `$ge$map` - five sites in these two files wrote the same
 * six lines with a different three or four characters in the middle. The sanitizing is not optional
 * and is why they are not a `format`: the text is a type's or a declaration's own spelling, and a
 * `(`, a `.` or a space in one is not an identifier here. The cap is the buffer rather than a guess -
 * a longer name is truncated and `uniqueName` is what keeps two truncations apart.
 */
inline Name mintPrefixed(JsShared& g, StringView prefix, StringView text) {
    char buffer[256];
    constexpr Size room = sizeof(buffer) - 8;

    auto length = text.length < room ? text.length : room;
    copy(prefix.ptr, buffer, prefix.length);

    auto sanitized = jsir::jsSanitize(StringView { text.ptr, U32(length) }, buffer + prefix.length,
                                      room);

    return uniqueName(g, StringView { buffer, U32(sanitized + prefix.length) }, false);
}

/*
 * The name of one type's teardown, reserved on first ask - `Assembly::typeDropNames`.
 *
 * Keyed on the type *and the two halves*, which is `teardownHalves`' own key: a drop-only request
 * and a full one are different functions over one type, because an authored `Drop` replaces the
 * derived walk for its own half only.
 *
 * **One key encoding, and it had to be.** `structure.cpp` names one for a `TypeDrop` in a body and
 * `assemble.cpp` names one for a descriptor's teardown cell, and the two wrote that packed key out
 * separately - so a spelling that drifted would have emitted the same type's teardown twice, under
 * two names, with a descriptor pointing at whichever one asked first.
 */
Name typeDropNameFor(Assembly& a, DeclPtr<mir::Type> type, bool drop, bool reclaim,
                     LocationId source);

// One body as the statements of a function. Reports through the target and answers an empty list if
// it could not, which is what the caller turns into a failed compilation.
StmtList structureBody(Assembly& a, jsir::JsBody& body, JsList<Name, false>& parameters);

/*
 * One **constant** body as the expression it is - `jsir::convertConstant`'s other end.
 *
 * A global's initializer, a descriptor's zero cell. The body is a straight-line tree of
 * constructors ending in a `return`, so this spells it through the same arms every instruction goes
 * through and nests each result into its one reader rather than binding it - see `runConstant`.
 */
JsPtr<Expr> structureConstant(Assembly& a, jsir::JsBody& body);

// One literal a constant `match` answers, as the expression it is spelled with - shared by the map's
// own emission and the tables `Assembly::mapNames` declares.
JsPtr<Expr> mapLiteral(Assembly& a, const jsir::JsConstant& value);

/*
 * The whole program through JSIR - the entry `-mode exe -platform js` reaches.
 *
 * `dump` is `-mode jsir`: the tables once, then each function's JSIR as it is converted. It is
 * written inside the per-function loop rather than afterwards because the bodies do not survive the
 * loop - §7.1's "one reusable body arena", which is the whole memory argument for this tier.
 */
Ptr<File> genJsirProgram(Context& context, mir::Program& program, ssa::Converted& converted,
                         Net::Writer* dump);

} // namespace js
