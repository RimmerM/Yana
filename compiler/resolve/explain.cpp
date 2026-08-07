#include "explain.h"
#include "analyze.h"

/*
 * See explain.h. Nothing in this file decides anything: every answer is read out of
 * `FunctionSummary`, `OwnershipResults`, `Local::storage` or `Function::specializations`, all of
 * which the pipeline already produced and - the ownership header says so in as many words - keeps
 * "for printing rather than for any later stage".
 */

/*
 * The call-site index.
 *
 * One walk of every body in the program, counting the two kinds of call that name a callee.
 * `InstCallDyn` names none, so it is counted for nobody: a call through a function value is a call
 * the program cannot attribute, and attributing it to the function that happens to have been stored
 * would be worse than not counting it.
 */
static void countCall(HashMap<U32, U32>& into, ModulePtr<Function> callee) {
    if(!callee) return;

    auto entry = into.add(U32(callee));
    if(!entry.existed) *entry.value = 0;
    (*entry.value)++;
}

void CallSiteIndex::build(Program& program) {
    auto base = *program.arena;

    for(auto module: program.modules) {
        for(auto pointer: module->functionOrder.contents(base)) {
            auto function = base[pointer];

            for(auto blockPointer: function->blocks.contents(base)) {
                auto block = base[blockPointer];

                for(auto instPointer: block->instructions(base)) {
                    auto inst = base[instPointer];

                    if(inst->kind == Value::Call) {
                        countCall(direct, ((InstCall*)inst)->callee);
                    } else if(inst->kind == Value::GenCall) {
                        countCall(generic, ((InstGenCall*)inst)->callee);
                    }
                }
            }
        }
    }
}

U32 CallSiteIndex::directCalls(ModulePtr<Function> function) const {
    auto found = direct.get(U32(function));
    return found ? found.unwrap() : 0;
}

U32 CallSiteIndex::genericCalls(ModulePtr<Function> function) const {
    auto found = generic.get(U32(function));
    return found ? found.unwrap() : 0;
}

/*
 * The record.
 */

// The local's own entry in the ownership result, where there is one. The two are indexed alike -
// `OwnershipResult::locals` is built one per `Function::locals` - but a function the fixpoint never
// visited has no result at all, which is what `analyzed` reports.
static const TrackedLocal* trackedLocal(const OwnershipResult* result, U32 index) {
    if(!result || index >= result->locals.size()) return nullptr;
    return &result->locals[index];
}

Explanation explainFunction(Program& program, Function& function, const CallSiteIndex* calls) {
    auto base = *program.arena;
    auto& summary = function.summary;

    Explanation explanation;
    explanation.name = function.name;
    explanation.module = function.module ? function.module->name : StringId();
    explanation.source = function.source;
    explanation.returnType = function.returnType;

    explanation.declaredRoots = summary.declaredRoots;
    explanation.actualRoots = summary.actualRoots;
    explanation.invalidRoot = summary.invalidRoot;
    explanation.returnsBorrow = summary.returnsBorrow;
    explanation.mutableResult = summary.mutableResult;
    explanation.resultBound = summary.resultBound;
    explanation.opaque = summary.opaque || !summary.ready;

    explanation.generic = function.gen != nullptr;
    explanation.specializationOf = function.specializationOf;
    explanation.specializations = U32(function.specializations.size());

    U16 index = 0;
    for(auto argPointer: function.args.contents(base)) {
        auto arg = base[argPointer];

        ArgExplanation entry;
        entry.name = arg->name;
        entry.type = arg->declaredType();
        entry.convention = arg->convention;
        entry.source = arg->source;
        entry.lazy = arg->isLazy();

        // The summary is what a *caller* is checked against, so it is the authority on retention and
        // demand even where the declaration also says something. `returnRoot` is read from it for
        // the same reason: the declared marker and the accepted one are the same fact by the time a
        // summary exists, and reading one of them keeps a disagreement from being invisible here.
        if(index < summary.args.size()) {
            auto argSummary = summary.args.get(base, index);
            entry.requirements = argSummary.requirements;
            entry.retained = argSummary.retained;
            entry.returnRoot = argSummary.returnRoot;
        }

        explanation.args.push(entry);
        index++;
    }

    /*
     * The storage classes, from the ownership result rather than from the locals.
     *
     * `Local::storage` holds the same answer, but the result is what also knows whether the frame
     * *proved* the storage outlives it - and "heap because it escapes" and "heap because it could
     * not be sized" are different things to tell someone about.
     */
    const OwnershipResult* result = nullptr;
    if(program.ownership) {
        auto found = program.ownership->functions.get(U32(&function - base));
        if(found) result = &found.unwrap();
    }

    explanation.analyzed = result != nullptr;

    for(U32 local = 0; local < function.localCount(); local++) {
        auto slot = function.localAt(base, local);
        auto tracked = trackedLocal(result, local);

        auto storage = tracked ? tracked->storage : slot.storage;
        auto escapes = tracked && tracked->escapes;

        // Only the ones that are not the boring answer. A frame-placed local nothing escapes is what
        // every reader already assumed, and listing all of them would bury the one that is not.
        if(storage != StorageClass::Heap && !escapes && !slot.materialized) continue;

        LocalExplanation entry;
        entry.name = slot.name;
        entry.type = slot.type;
        entry.index = local;
        entry.storage = storage;
        entry.escapes = escapes;
        entry.materialized = slot.materialized;
        explanation.locals.push(entry);
    }

    /*
     * How this function is reached.
     *
     * A generic function's specialized call sites are calls to its *specializations*, so they are
     * counted there and summed here. That is what makes "specialized at 3 of 11 call sites" a
     * statement about the function the programmer wrote rather than about a clone they never saw.
     */
    if(calls) {
        auto pointer = &function - base;
        explanation.directCallSites = calls->directCalls(pointer);
        explanation.genericCallSites = calls->genericCalls(pointer);

        for(auto specialization: function.specializations.contents(base)) {
            explanation.directCallSites += calls->directCalls(specialization);
            explanation.genericCallSites += calls->genericCalls(specialization);
        }
    }

    // Left as they are: neither generators nor ambients exist, so the honest answer to both is that
    // the compiler does not know rather than that there are none. See Inferred.
    explanation.suspends = Inferred::Unknown;
    explanation.demandsAmbients = Inferred::Unknown;

    return explanation;
}

void findExplainTargets(Program& program, StringId module, StringId name, Array<Function*>& out) {
    auto base = *program.arena;

    for(auto candidate: program.modules) {
        if(module && candidate->name != module) continue;

        for(auto pointer: candidate->functionOrder.contents(base)) {
            auto function = base[pointer];
            if(function->name != name) continue;

            // A class signature has arguments and a return type and never a body, so there is
            // nothing about it to explain; the instances that implement it are the answers.
            if(function->signature) continue;

            // A specialization is a clone of source text that exists once, so explaining it would
            // answer a question about a function nobody wrote. The generic it came from carries the
            // specialization count instead.
            if(function->specializationOf) continue;

            out.push(function);
        }
    }
}

/*
 * Rendering.
 */

StringView explainTopicLabel(ExplainTopic topic) {
    switch(topic) {
        case ExplainTopic::Suspends: return "suspends"_v;
        case ExplainTopic::Ambients: return "ambients"_v;
        case ExplainTopic::Storage:  return "storage"_v;
        case ExplainTopic::Mutates:  return "mutates"_v;
        case ExplainTopic::Retains:  return "retains"_v;
        case ExplainTopic::Returns:  return "returns"_v;
        case ExplainTopic::Calls:    return "calls"_v;
    }

    return ""_v;
}

static void appendName(StringBuilder& target, Context& context, StringId name, StringView fallback) {
    if(name) target.append(context.findName(name));
    else target.append(fallback);
}

static void appendLocalName(StringBuilder& target, Context& context, const LocalExplanation& local) {
    if(local.name) {
        target.append(context.findName(local.name));
    } else {
        target.append("local"_v);
        show(local.index, target);
    }
}

static void addNote(Array<ExplainNote>& notes, ExplainTopic topic, bool surprising,
                    StringBuilder& text, LocationId source = kNullLocation) {
    ExplainNote note;
    note.topic = topic;
    note.surprising = surprising;
    note.source = source;
    note.text = text.string();
    notes.push(note);
    text.clear();
}

static void explainSuspension(Context&, const Explanation& explanation, Array<ExplainNote>& notes) {
    StringBuilder text;

    switch(explanation.suspends) {
        case Inferred::No:
            text.append("no"_v);
            break;
        case Inferred::Yes:
            text.append("yes"_v);
            break;
        case Inferred::Unknown:
            // Not "no". Nothing infers suspension yet because nothing lowers a generator yet, and a
            // confident negative here would silently become wrong the day one does.
            text.append("unknown (not inferred yet)"_v);
            break;
    }

    addNote(notes, ExplainTopic::Suspends, explanation.suspends == Inferred::Yes, text,
            explanation.suspensionPoint ? explanation.suspensionPoint.unwrap() : kNullLocation);
}

static void explainAmbients(Context& context, Program& program, const Explanation& explanation,
                            Array<ExplainNote>& notes) {
    StringBuilder text;

    if(explanation.ambients.isEmpty()) {
        if(explanation.demandsAmbients == Inferred::Unknown) text.append("unknown (not implemented)"_v);
        else text.append("none"_v);

        addNote(notes, ExplainTopic::Ambients, false, text);
        return;
    }

    for(auto& ambient: explanation.ambients) {
        text.append("~"_v);
        appendName(text, context, ambient.name, "?"_v);
        text.append(": "_v);
        text.append(describeType(context, *program.types, ambient.type));

        if(ambient.requiredBy) {
            text.append(" (required by "_v);
            text.append(context.findName((*program.arena)[ambient.requiredBy]->name));
            text.append(")"_v);
        }

        if(ambient.defaulted) text.append(" [defaulted]"_v);
        addNote(notes, ExplainTopic::Ambients, true, text, ambient.source);
    }
}

/*
 * Where the result and the frame's own values had to live.
 *
 * After whatever this frame could not answer, the result bound comes before the locals because it is
 * the one a caller is affected by: everything below it is a decision inside this frame, and a result
 * that escapes is a decision the caller pays for.
 */
static void explainStorage(Context& context, Program& program, const Explanation& explanation,
                           Array<ExplainNote>& notes) {
    StringBuilder text;
    auto said = false;

    // Said before the rest rather than instead of it: the result bound comes from the summary and is
    // known either way, while the locals come from the ownership result and are the half that is
    // missing. A row that stayed quiet about which of the two a reader was looking at would be the
    // one mistake this report cannot afford.
    if(!explanation.analyzed) {
        text.append(explanation.generic
            ? "unknown for the locals (a generic body is analyzed through its specializations)"_v
            : "unknown for the locals (this body was not analyzed)"_v);
        addNote(notes, ExplainTopic::Storage, false, text);
        said = true;
    }

    switch(explanation.resultBound) {
        case StorageBound::Frame:
            break;
        case StorageBound::Arguments:
            // True of a returned borrow and of a result moved out of a parameter alike, which is
            // why the wording is about the storage rather than about a borrow: `Arguments` says the
            // storage is not this frame's, and does not say what shape the result has.
            text.append("the result's storage is the caller's, not this frame's"_v);
            addNote(notes, ExplainTopic::Storage, false, text);
            said = true;
            break;
        case StorageBound::Region:
            text.append("the result lives in the ambient region"_v);
            addNote(notes, ExplainTopic::Storage, true, text);
            said = true;
            break;
        case StorageBound::Escapes:
            text.append("the result escapes the frame -> heap"_v);
            addNote(notes, ExplainTopic::Storage, true, text);
            said = true;
            break;
    }

    for(auto& local: explanation.locals) {
        appendLocalName(text, context, local);
        text.append(": "_v);
        text.append(describeType(context, *program.types, local.type));

        if(local.storage == StorageClass::Heap) text.append(" heap"_v);
        if(local.escapes) text.append(" escapes"_v);

        // Design.md's tier 1: a mutable borrow of a packed field lives in a temporary of this
        // frame's, so a borrow of it may not outlive the frame. Worth saying because the source has
        // no name for the temporary and the restriction is on it rather than on the field.
        if(local.materialized) text.append(" materialized from a packed field"_v);

        addNote(notes, ExplainTopic::Storage, local.storage == StorageClass::Heap, text);
        said = true;
    }

    if(said) return;

    text.append("everything is frame-placed"_v);
    addNote(notes, ExplainTopic::Storage, false, text);
}

// Mutation demand, per Design-Memory §2.2. Only the arguments that demand something are listed;
// what makes the row worth having is the *borrow* that turned out to need writable storage, since
// that is the one the caller pays for without either end saying so.
static void explainMutation(Context& context, const Explanation& explanation, Array<ExplainNote>& notes) {
    StringBuilder text;
    auto listed = 0;

    for(auto& arg: explanation.args) {
        if(arg.requirements.mutation == MutationDemand::ReadOnly && !arg.requirements.needsStableAddress &&
           !arg.requirements.mayResize) {
            continue;
        }

        if(listed++) text.append(", "_v);
        appendName(text, context, arg.name, "_"_v);

        switch(arg.requirements.mutation) {
            case MutationDemand::ReadOnly: break;
            case MutationDemand::Writable: text.append(" writable"_v); break;
            case MutationDemand::Unknown: text.append(" writable (unknown callee)"_v); break;
        }

        if(arg.requirements.needsStableAddress) text.append(" addressed"_v);
        if(arg.requirements.mayResize) text.append(" resizable"_v);

        // The cliff: written as a plain borrow, demanded writable anyway, so every root that reaches
        // this position is writable at the caller too.
        if(arg.demandsWritable()) text.append(" [borrow, so the demand reaches the caller]"_v);
    }

    if(!listed) text.append("none"_v);

    auto surprising = false;
    for(auto& arg: explanation.args) surprising = surprising || arg.demandsWritable();

    addNote(notes, ExplainTopic::Mutates, surprising, text);
}

// Argument retention, per Analysis-Sharing.md §4. A retained argument's loan cannot end at the call
// the way an ordinary borrow's does, and nothing in the signature says so.
static void explainRetention(Context& context, const Explanation& explanation, Array<ExplainNote>& notes) {
    StringBuilder text;
    auto listed = 0;

    for(auto& arg: explanation.args) {
        if(!arg.retained) continue;
        if(listed++) text.append(", "_v);
        appendName(text, context, arg.name, "_"_v);
    }

    if(!listed) text.append("none"_v);
    addNote(notes, ExplainTopic::Retains, listed != 0, text);
}

static void explainResult(Context& context, const Explanation& explanation, Array<ExplainNote>& notes) {
    if(!explanation.returnsBorrow && !explanation.invalidRoot) return;

    StringBuilder text;

    if(explanation.invalidRoot) {
        // A returned borrow rooted in something that can never be a group member. The ownership pass
        // has already reported this; it is repeated here because an explanation that stayed quiet
        // about it would be describing a program that does not compile as though it did.
        text.append("a borrow rooted in storage no caller can name"_v);
        addNote(notes, ExplainTopic::Returns, true, text);
        return;
    }

    text.append(explanation.mutableResult ? "a mutable borrow"_v : "a borrow"_v);
    text.append(" rooted in "_v);

    auto listed = 0;
    U32 index = 0;
    for(auto& arg: explanation.args) {
        if(explanation.actualRoots & (U64(1) << index++)) {
            if(listed++) text.append(", "_v);
            appendName(text, context, arg.name, "_"_v);
        }
    }

    if(!listed) text.append("nothing this frame owns"_v);

    // The declared group is the contract; the actual one is what the body used. A body using fewer
    // than it declared is legal and worth seeing, because the declaration is what every call site
    // was checked against.
    if(explanation.declaredRoots != explanation.actualRoots) {
        text.append(" (the signature declares a wider group)"_v);
    }

    addNote(notes, ExplainTopic::Returns, false, text);
}

/*
 * The specialization line - §7.3's "specialized at 3 of 11 call sites".
 *
 * A generic call site that took the erased path is the surprising one: the program has a call whose
 * callee is reached through a runtime environment rather than a specialization, and that is exactly
 * the cliff §7.3 says an invisible property needs to be visible for.
 */
static void explainCalls(const Explanation& explanation, Array<ExplainNote>& notes) {
    StringBuilder text;
    auto total = explanation.callSites();

    if(explanation.generic) {
        text.append("specialized at "_v);
        show(explanation.directCallSites, text);
        text.append(" of "_v);
        show(total, text);
        text.append(total == 1 ? " call site"_v : " call sites"_v);

        if(explanation.specializations) {
            text.append(", "_v);
            show(explanation.specializations, text);
            text.append(explanation.specializations == 1 ? " specialization"_v : " specializations"_v);
        }
    } else {
        text.append("called at "_v);
        show(total, text);
        text.append(total == 1 ? " site"_v : " sites"_v);
    }

    addNote(notes, ExplainTopic::Calls, explanation.generic && explanation.genericCallSites != 0, text);
}

void explainNotes(Context& context, Program& program, const Explanation& explanation, Array<ExplainNote>& notes) {
    // §7.3's row order, and it is deliberate: the two properties with no analysis behind them come
    // first so that a reader is told what is *not* known before they read what is.
    explainSuspension(context, explanation, notes);
    explainAmbients(context, program, explanation, notes);
    explainStorage(context, program, explanation, notes);
    explainMutation(context, explanation, notes);
    explainRetention(context, explanation, notes);
    explainResult(context, explanation, notes);
    explainCalls(explanation, notes);
}

/*
 * The three surfaces.
 */

void printExplainSignature(StringBuilder& target, Context& context, Program& program,
                           const Explanation& explanation) {
    target.append("fn "_v);
    appendName(target, context, explanation.name, "<anonymous>"_v);
    target.append("("_v);

    auto first = true;
    for(auto& arg: explanation.args) {
        if(!first) target.append(", "_v);
        first = false;

        // The convention is a property of the parameter rather than of its type, so it is written
        // where the source writes it rather than folded into the type.
        if(arg.convention == ast::BindType::Ref) target.append("&"_v);
        else if(arg.convention == ast::BindType::Sink) target.append("->"_v);

        if(arg.returnRoot) target.append("return "_v);
        if(arg.lazy) target.append("@lazy "_v);

        appendName(target, context, arg.name, "_"_v);
        target.append(": "_v);
        target.append(describeType(context, *program.types, arg.type));
    }

    target.append(") -> "_v);
    target.append(describeType(context, *program.types, explanation.returnType));
}

void printExplanation(StringBuilder& target, Context& context, Program& program,
                      const Explanation& explanation) {
    printExplainSignature(target, context, program, explanation);
    target.append("\n"_v);

    if(explanation.opaque) {
        // Said once, at the top, rather than qualifying every row below it: an opaque callee's
        // answers are all the conservative one, and a reader who knows that reads the rest right.
        target.append("  (no body was summarized, so every answer below is the conservative one)\n"_v);
    }

    Array<ExplainNote> notes;
    explainNotes(context, program, explanation, notes);

    // The label is written once per topic and the rows under it are aligned to the same column,
    // which is what makes several ambients or several heap locals read as one answer.
    auto previous = Maybe<ExplainTopic>();

    for(auto& note: notes) {
        auto label = explainTopicLabel(note.topic);
        auto repeated = previous && previous.unwrap() == note.topic;

        target.append("  "_v);

        if(repeated) {
            for(Size i = 0; i < 12; i++) target.append(" "_v);
        } else {
            target.append(label);
            target.append(":"_v);
            for(Size i = label.length + 1; i < 12; i++) target.append(" "_v);
        }

        target.append(note.text);
        target.append("\n"_v);
        previous = Just(note.topic);
    }
}

void printExplanation(Net::Writer& writer, Context& context, Program& program,
                      const Explanation& explanation) {
    StringBuilder target;
    printExplanation(target, context, program, explanation);
    writer.writeString(stringView(target));
}

bool printExplanationHover(StringBuilder& target, Context& context, Program& program,
                           const Explanation& explanation) {
    Array<ExplainNote> notes;
    explainNotes(context, program, explanation, notes);

    auto surprising = 0;
    for(auto& note: notes) surprising += note.surprising ? 1 : 0;
    if(!surprising) return false;

    // A markdown list under a rule, so that it reads as a section of the hover rather than as more
    // of the signature above it. The label is bold because it is the thing a reader scans for.
    target.append("\n---\n"_v);

    for(auto& note: notes) {
        if(!note.surprising) continue;

        target.append("- **"_v);
        target.append(explainTopicLabel(note.topic));
        target.append("**: "_v);
        target.append(note.text);
        target.append("\n"_v);
    }

    return true;
}

String explanationSummary(Context& context, Program& program, const Explanation& explanation) {
    Array<ExplainNote> notes;
    explainNotes(context, program, explanation, notes);

    StringBuilder target;
    auto listed = 0;

    for(auto& note: notes) {
        if(!note.surprising) continue;
        if(listed++) target.append("; "_v);
        target.append(explainTopicLabel(note.topic));
        target.append(" "_v);
        target.append(note.text);
    }

    return listed ? target.string() : String();
}

/*
 * The whole-program dump.
 *
 * Exactly printOwnership's filter, because the two dumps answer about the same set of functions and
 * a fixture that had to reconcile two different sets would be asserting the filters rather than the
 * answers.
 *
 * All three surfaces, not only the CLI one. The editor-facing two are a *filter* over the same
 * notes, and a filter is exactly the kind of thing that is wrong without anything failing - a
 * fixture asserting only the CLI form would keep passing while every hover in the editor said
 * nothing or said everything. They appear only where there is something surprising to say, which is
 * itself the property worth asserting.
 */
void printExplanations(Net::Writer& writer, Context& context, Program& program) {
    auto base = *program.arena;
    Size index = 0;

    CallSiteIndex calls;
    calls.build(program);

    for(auto module: program.modules) {
        for(auto pointer: module->functionOrder.contents(base)) {
            auto function = base[pointer];
            if(!module->root && !function->used) continue;
            if(function->signature) continue;
            if(function->specializationOf) continue;

            if(index++) writer.writeByte('\n');

            auto explanation = explainFunction(program, *function, &calls);

            StringBuilder target;
            printExplanation(target, context, program, explanation);

            auto hint = explanationSummary(context, program, explanation);
            if(hint.size()) {
                target.append("  hint:       "_v);
                target.append(hint);
                target.append("\n"_v);
            }

            // Written out as it stands rather than re-indented: what a fixture should pin is the
            // exact text the server hands the client, and reformatting it here would assert this
            // printer instead.
            StringBuilder hover;
            if(printExplanationHover(hover, context, program, explanation)) {
                target.append("  hover:"_v);
                target.append(stringView(hover));
            }

            writer.writeString(stringView(target));
        }
    }
}
