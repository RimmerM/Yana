#pragma once

#include "decl.h"
#include "Net/Stream.h"

namespace mir {

/*
 * `explain`: the inferred-property query (Analysis-Ambient.md §7.3, Implementation-Tooling.md §7).
 *
 * Yana infers properties that do not appear in source - suspension, placement, mutation, what an
 * argument escapes into, whether a call comes back - and each is invisible and viral: a value that
 * silently falls to the heap, an argument a callee keeps, a call site that quietly took the erased
 * path. This is the affordance that makes them visible, and it is a *printer over data the compiler
 * already has*. Every answer here is `Function::summary`, `storageOf` or a count of call nodes, and
 * nothing is computed that the ownership analysis and `computeSummaries` did not already decide.
 *
 * The record is built once and printed three ways, which is the whole shape of the design:
 *
 *   1. `printExplanation`      - the text form §7.3 specifies, for `yana explain <name>`.
 *   2. `printExplanationHover` - a markdown section for an editor tooltip, carrying only the parts
 *                                that are *surprising*. A tooltip that always says the same thing
 *                                stops being read.
 *   3. `explanationSummary`    - the one-line form an inlay hint beside a `fn` shows.
 *
 * All three go through `ExplainNote`, so the three surfaces cannot word one fact differently.
 *
 * It was written first against `compiler/resolve` and moved here with the editor surface: this stage
 * publishes the facts in one structure with a confidence on each, which is what lets a row say "not
 * known" rather than a confident "no" wherever a body was never summarized.
 */

/*
 * A property the compiler could not answer, as against one it answered "no" to.
 *
 * A function the summaries never reached - an unreachable one in a program that roots at its entry, a
 * body that was dropped because it did not resolve - has no answer, and reporting that as absent
 * would be a lie that becomes invisible the day the answer exists.
 */
enum class Inferred: U8 {
    No,
    Yes,
    Unknown,
};

// One parameter, as the callee's summary describes it.
struct ArgExplanation {
    StringId name {};
    Occurrence occurrence;
    LocationId source = kNullLocation;
    bool lazy = false;

    Inferred mutated = Inferred::Unknown;
    Inferred escapes = Inferred::Unknown;

    // Whether the position was written `&`. A write through one is what the author asked for; a
    // write through anything else is the surprise.
    bool unique() const { return occurrence.contract.access() == Access::Unique; }
};

// One named local whose placement is worth reporting - only the ones that are not the boring answer.
struct LocalExplanation {
    StringId name {};
    DeclPtr<Type> type = nullptr;
    StorageClass storage = StorageClass::Frame;
    LocationId source = kNullLocation;
};

/*
 * What `explain` knows about one function - the facts, with no text in them.
 *
 * Rendering is `explainNotes`, and the split is what lets §7.5's capability audit - "which functions
 * suspend, allocate, keep their arguments" - be a filter over these records rather than a second
 * printer.
 */
struct Explanation {
    DeclPtr<Function> function = nullptr;
    StringId name {};
    StringId module {};
    LocationId source = kNullLocation;

    // What leaves the function - its result, or what a traversal hands over.
    Occurrence result;

    Array<ArgExplanation> args;

    // Whether there is a body to have answers about, and whether the ownership analysis placed its
    // objects. The second is false for a body that did not survive resolution.
    bool bodyKnown = false;
    bool placed = false;

    // Whether the summaries reached this function at all - or, for a generic, any specialization of
    // it, whose facts are joined. Every per-fact `Inferred` below is `Unknown` where they did not.
    bool summarized = false;

    Inferred suspends = Inferred::Unknown;
    Inferred diverges = Inferred::Unknown;

    // The strongest placement anything reached from this activation needs - `Summary::placement`.
    StorageClass placement = StorageClass::Frame;

    Array<LocalExplanation> locals;

    // Whether this is a generic declaration, the form its body is compiled in, and how many
    // argument lists it has been specialized at.
    bool generic = false;
    GenForm form = GenForm::Auto;
    U32 specializations = 0;

    // How this function is reached, counted over the whole program - see `CallSiteIndex`.
    U32 callSites = 0;
    U32 valueSites = 0;

    // The one property with no analysis behind it, held so that the record's shape is the one §7.3
    // specifies rather than the one today's compiler happens to fill in.
    Inferred demandsAmbients = Inferred::Unknown;
};

/*
 * Which functions are named where, counted in one walk of the program.
 *
 * Held apart from `Explanation` because the question is about the *program* and the record is about
 * one function: an editor asking for a hover per keystroke would otherwise walk every body each time.
 * Build one, keep it as long as the program, and hand it to every query.
 *
 * A call through a function value contributes nothing here, and that is the honest answer rather than
 * an omission: it names no callee, so no callee may count it. The *value* is counted instead, where it
 * was built.
 */
struct CallSiteIndex {
    void build(Program& program);

    // Keyed by the callee's declaration offset.
    HashMap<U32, U32> calls;
    HashMap<U32, U32> values;

    U32 callsOf(DeclPtr<Function> function) const;
    U32 valuesOf(DeclPtr<Function> function) const;
};

/*
 * The record for one function. `calls` may be null, in which case the site counts are zero - which is
 * what a caller that has not built an index gets, rather than a silent full-program walk per query.
 */
Explanation explainFunction(Program& program, DeclPtr<Function> function,
                            const CallSiteIndex* calls = nullptr);

/*
 * Whether a function is one an author wrote and could ask about.
 *
 * Not a specialization, a lifted lambda, an instance member or anything else the compiler made -
 * those are clones or desugarings of source that exists once, and a query about one would answer a
 * question about a function nobody wrote. A class member is explained where it has a default body.
 */
bool isExplainable(Context& context, Program& program, DeclPtr<Function> function);

// Every explainable function some module declares under `name`, in declaration order. `module` is the
// module name to restrict to, or zero for every module. A name is not a function - arities share one -
// so this answers with a list and the caller decides.
void findExplainTargets(Context& context, Program& program, StringId module, StringId name,
                        Array<DeclPtr<Function>>& out);

/*
 * What one explanation says, as lines.
 *
 * The topic is what the CLI prints as a row label and what an editor groups by; `surprising` is the
 * filter the hover applies.
 */
enum class ExplainTopic: U8 {
    Suspends,
    Ambients,
    Storage,
    Mutates,
    Retains,
    Returns,
    Object,
    Calls,
};

struct ExplainNote {
    ExplainTopic topic = ExplainTopic::Suspends;
    bool surprising = false;
    String text;
};

StringView explainTopicLabel(ExplainTopic topic);

// One explanation as notes, in the row order §7.3 specifies. Every surface goes through this.
void explainNotes(Context& context, Program& program, const Explanation& explanation,
                  Array<ExplainNote>& notes);

// §7.3's text form: the signature, then one aligned row per topic.
void printExplanation(StringBuilder& target, Context& context, Program& program,
                      const Explanation& explanation);

/*
 * The hover section, as markdown, holding only the surprising notes.
 *
 * Returns false and writes nothing when every answer is the boring one. That is the point rather than
 * an optimization: a section under every function is one nobody reads.
 */
bool printExplanationHover(StringBuilder& target, Context& context, Program& program,
                           const Explanation& explanation);

// The inlay-hint form: the surprising notes on one line, or an empty string when there are none.
String explanationSummary(Context& context, Program& program, const Explanation& explanation);

/*
 * Every explainable function of the program's own modules - the golden-file form. All three surfaces,
 * because the editor-facing two are a *filter* over the same notes, and a filter is exactly the kind
 * of thing that is wrong without anything failing. A library module is left out, as it is of every
 * other dump of this stage.
 */
void printExplanations(Net::Writer& writer, Context& context, Program& program);

} // namespace mir
