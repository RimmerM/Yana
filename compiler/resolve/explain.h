#pragma once

#include "module.h"
#include "Net/Stream.h"

/*
 * `explain`: the inferred-property query (Analysis-Ambient.md §7.3, Implementation-Tooling.md §7).
 *
 * Yana infers five viral properties that do not appear in source - suspension, storage class,
 * mutation demand, argument retention, and (once it exists) ambient demand. Each is invisible, each
 * is viral, and each has a cliff attached: a value that silently falls to the heap, a `&` that
 * forces a copy upstream, a call site that quietly took the erased path. This file is the affordance
 * that makes them visible, and it is a *printer over data the compiler already has* rather than an
 * analysis of its own - nothing here computes anything the ownership passes did not already decide.
 *
 * The record is built once and printed three ways, which is the whole shape of the design:
 *
 *   1. `printExplanation`      - the text form §7.3 specifies, for `yana explain <name>`.
 *   2. `printExplanationHover` - a markdown section for an editor tooltip, carrying only the parts
 *                                that are *surprising*. A tooltip that always says the same thing
 *                                stops being read.
 *   3. `explanationSummary`    - the one-line form an inlay hint above a `fn` shows.
 *
 * All three go through `ExplainNote`, so the three surfaces cannot word the same fact differently,
 * and every note carries the `LocationId` of its evidence so that a language server can turn it into
 * a range without re-deriving anything (Implementation-Tooling.md §2.1: the offset is authoritative,
 * and the server converts it against the document text it already holds).
 *
 * Everything is written against `StringBuilder` rather than `Net::Writer`, for the reason
 * Implementation-Tooling.md §6 gives: a server assembles a response in memory, and the golden-file
 * printers want the same text. The `Net::Writer` overloads are thin wrappers over the builder ones.
 */

/*
 * A property the compiler cannot yet answer, as against one it answered "no" to.
 *
 * Two of §7.3's five properties - suspension and ambient demand - have no analysis behind them
 * because the language features do not exist. Reporting them as absent would be a lie that becomes
 * invisible the day they land, so they are reported as unknown and the row says so.
 */
enum class Inferred: U8 {
    No,
    Yes,
    Unknown,
};

// One argument, as the callee's own summary describes it. Everything here is `FunctionSummary`'s
// except the name, the type and the convention, which come from the `Arg` it is about.
struct ArgExplanation {
    StringId name = 0;
    TypePtr type = nullptr;
    ast::BindType convention = ast::BindType::Borrow;
    LocationId source = kNullLocation;

    ReprRequirements requirements;
    bool retained = false;
    bool returnRoot = false;
    bool lazy = false;

    // True when the caller has to hand over storage it can write to even though the parameter was
    // written as a plain borrow. This is the cliff Analysis-Sharing.md names - a demand inferred in
    // the callee reaches back through every call site - and it is the one argument fact that is
    // invisible at both ends.
    bool demandsWritable() const {
        return convention == ast::BindType::Borrow &&
               requirements.mutation != MutationDemand::ReadOnly;
    }
};

// One local whose storage decision is worth reporting. Only the ones that are not the boring answer
// are collected: a frame-placed local that nothing escapes says nothing a reader did not assume.
struct LocalExplanation {
    StringId name = 0;
    TypePtr type = nullptr;
    U32 index = 0;
    StorageClass storage = StorageClass::Stack;
    bool escapes = false;
    bool materialized = false;
};

/*
 * One ambient a function turned out to demand - Analysis-Ambient.md part 2.
 *
 * Present in the record and never filled in, because ambients are not implemented. It is here
 * rather than added later so that the shape of the record does not move when they land: §7.3's
 * output is specified with this row in it, and a printer written without it would have to be
 * rewritten rather than extended.
 */
struct AmbientDemand {
    StringId name = 0;
    TypePtr type = nullptr;

    // The callee that required it and where, which is what makes an ambient failing three modules
    // away diagnosable at all.
    ModulePtr<Function> requiredBy = nullptr;
    LocationId source = kNullLocation;
    bool defaulted = false;
};

/*
 * What `explain` knows about one function.
 *
 * The facts, with no text in them. Rendering is `explainNotes` below, and the split is what lets
 * §7.5's capability audit - "which functions reach the filesystem, allocate, suspend, retain their
 * arguments" - be a filter over these records rather than a second printer.
 */
struct Explanation {
    StringId name = 0;
    StringId module = 0;
    LocationId source = kNullLocation;
    TypePtr returnType = nullptr;

    Array<ArgExplanation> args;

    // The return-root group, as declared and as every return path turned out to use it.
    U64 declaredRoots = 0;
    U64 actualRoots = 0;
    bool invalidRoot = false;
    bool returnsBorrow = false;
    bool mutableResult = false;
    StorageBound resultBound = StorageBound::Frame;

    // Only the locals with something to say - see LocalExplanation.
    Array<LocalExplanation> locals;

    // What the ownership passes could not say anything about: a class signature, or a body the
    // fixpoint never reached. Everything below is the conservative answer for one of these, and a
    // reader has to be told that rather than shown a confident "no".
    bool opaque = false;

    // Set for a function the ownership passes never ran over, so there is no result to read the
    // locals and storage classes out of. A generic body is the ordinary case: it is analyzed
    // through its specializations.
    bool analyzed = false;

    bool generic = false;
    ModulePtr<Function> specializationOf = nullptr;

    // How this function is reached, counted over the whole program - see CallSiteIndex. `direct`
    // counts the specialized call sites of a generic function too, through its specializations.
    U32 specializations = 0;
    U32 directCallSites = 0;
    U32 genericCallSites = 0;

    U32 callSites() const { return directCallSites + genericCallSites; }

    // The two properties whose analyses do not exist yet, held so that the record's shape is the one
    // §7.3 specifies rather than the one today's compiler happens to fill in.
    Inferred suspends = Inferred::Unknown;
    Maybe<LocationId> suspensionPoint;
    Inferred demandsAmbients = Inferred::Unknown;
    Array<AmbientDemand> ambients;
};

/*
 * Which functions call which, counted in one walk of the program.
 *
 * Held apart from `Explanation` because the question is about the *program* and the record is about
 * one function: an editor asking for a hover per keystroke would otherwise walk every body in the
 * program each time. Build one, keep it as long as the program, and hand it to every query.
 *
 * Calls through a function value contribute to nothing here, and that is the honest answer rather
 * than an omission: a dynamic call names no callee, so no callee may count it.
 */
struct CallSiteIndex {
    void build(Program& program);

    // Keyed by the callee's arena offset, on the same terms as OwnershipResults::functions.
    HashMap<U32, U32> direct;
    HashMap<U32, U32> generic;

    U32 directCalls(ModulePtr<Function> function) const;
    U32 genericCalls(ModulePtr<Function> function) const;
};

/*
 * The record for one function. `calls` may be null, in which case the call-site counts are zero and
 * the specialization row says only how many specializations exist - which is what a caller that has
 * not built an index gets, rather than a silent full-program walk per query.
 */
Explanation explainFunction(Program& program, Function& function, const CallSiteIndex* calls = nullptr);

// Every function some module declares under `name`, in declaration order. `module` is the module
// name to restrict to, or 0 for every module. Overloads and class instances mean a name is not a
// function, so this answers with a list and the caller decides.
void findExplainTargets(Program& program, StringId module, StringId name, Array<Function*>& out);

/*
 * What one explanation says, as lines.
 *
 * The topic is what the CLI prints as a row label and what an editor groups by; `surprising` is the
 * filter the hover section applies. A note whose evidence has a source location carries it, so a
 * server can attach the note to a range instead of to the whole declaration.
 */
enum class ExplainTopic: U8 {
    Suspends,
    Ambients,
    Storage,
    Mutates,
    Retains,
    Returns,
    Calls,
};

struct ExplainNote {
    ExplainTopic topic = ExplainTopic::Suspends;
    bool surprising = false;
    LocationId source = kNullLocation;
    String text;
};

StringView explainTopicLabel(ExplainTopic topic);

// Renders one explanation into notes, in the row order §7.3 specifies. Every surface goes through
// this, which is what keeps the three of them from wording one fact three ways.
void explainNotes(Context& context, Program& program, const Explanation& explanation, Array<ExplainNote>& notes);

// `fn handle(req: Request) -> Response`, with each parameter's convention as it was written.
void printExplainSignature(StringBuilder& target, Context& context, Program& program, const Explanation& explanation);

// §7.3's text form: the signature, then one aligned row per topic.
void printExplanation(StringBuilder& target, Context& context, Program& program, const Explanation& explanation);
void printExplanation(Net::Writer& writer, Context& context, Program& program, const Explanation& explanation);

/*
 * The hover section, as markdown, holding only the surprising notes.
 *
 * Returns false and writes nothing when every answer is the boring one. That is the point rather
 * than an optimization: a tooltip section that appears under every function is one nobody reads,
 * and the properties this exists for are precisely the ones that are usually unremarkable and
 * occasionally a cliff.
 */
bool printExplanationHover(StringBuilder& target, Context& context, Program& program, const Explanation& explanation);

// The inlay-hint form: the surprising notes on one line, or an empty string when there are none.
String explanationSummary(Context& context, Program& program, const Explanation& explanation);

/*
 * Every function of the program, explained - the golden-file form, on the same terms as
 * printOwnership: a fixture that asserts these is asserting the answers an editor would show and
 * the answers §7.5's capability audit would filter.
 */
void printExplanations(Net::Writer& writer, Context& context, Program& program);
