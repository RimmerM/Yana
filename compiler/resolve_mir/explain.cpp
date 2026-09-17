#include "specialize.h"
#include "explain.h"
#include "index.h"

/*
 * See explain.h. Nothing in this file decides anything: every answer is read out of
 * `Function::summary`, `storageOf` or the call nodes of the bodies, all of which the pipeline had
 * already produced by the time a query runs.
 */

namespace mir {

/*
 * The call-site index.
 *
 * One walk of every body in the program, counting what names a declaration. A specialization is
 * skipped because it has no body of its own - it is the generic's body under a substitution, and
 * counting it would count one written call once per argument list.
 */
static void countSite(HashMap<U32, U32>& into, DeclPtr<Function> callee) {
    if(!callee) return;

    auto entry = into.add(callee.offset);
    if(!entry.existed) *entry.value = 0;
    (*entry.value)++;
}

void CallSiteIndex::build(Program& program) {
    auto decl = *program.decl;
    auto mir = *program.mir;

    for(auto module: program.modules) {
        for(auto pointer: decl[module]->functions.contents(decl)) {
            auto& function = *decl[pointer];
            if(function.specializes || !function.mir) continue;

            auto& body = *mir[function.mir];

            for(U32 i = 0; i < body.nodes.size(); i++) {
                auto node = body.node(mir, NodeId(i));

                switch(node->kind) {
                    case Node::Call:
                        countSite(calls, ((NodeCall*)node)->callee);
                        break;
                    case Node::LensCall:
                    case Node::LensTry:
                        countSite(calls, ((NodeLensCall*)node)->callee);
                        break;
                    case Node::FunValue:
                        countSite(values, ((NodeFunValue*)node)->callee);
                        break;
                    default:
                        break;
                }
            }
        }
    }
}

U32 CallSiteIndex::callsOf(DeclPtr<Function> function) const {
    auto found = calls.get(function.offset);
    return found ? found.unwrap() : 0;
}

U32 CallSiteIndex::valuesOf(DeclPtr<Function> function) const {
    auto found = values.get(function.offset);
    return found ? found.unwrap() : 0;
}

/*
 * The record.
 */

// A published fact about one position, as the three-valued answer a row prints. `Unknown` wherever the
// summary has no answer for the whole set, which is a different statement from the set not holding it.
static Inferred positionFact(Fact<ArgumentSet> fact, U32 position) {
    if(fact.knowledge == Knowledge::Unknown) return Inferred::Unknown;

    // The top of the lattice is what a call nothing can see into contributes - a body-less callee, a
    // call through a value - and every set it joins into becomes it. It is a safe answer for a pass
    // to act on and not an answer about this position, so a reader is told it is not known.
    if(fact.value == ArgumentSet::all()) return Inferred::Unknown;

    return fact.value.has(position) ? Inferred::Yes : Inferred::No;
}

static Inferred boolFact(Fact<bool> fact) {
    if(fact.knowledge == Knowledge::Unknown) return Inferred::Unknown;
    return fact.value ? Inferred::Yes : Inferred::No;
}

/*
 * Whether a value at this position could refer to storage - the only kind of value retaining one says
 * anything about.
 *
 * `Summary::escapes` is about *storage*: which positions reach a place that outlives the call, which is
 * what placement and containment need. An integer written back through a `&` reaches one too, and
 * keeping a copy of a number is not keeping anything. So a row a reader acts on is filtered to what can
 * hold an address - a type with one somewhere inside, or whose teardown releases something - and by the
 * type rather than the contract: the escape row cannot tell a borrow's address from the value read
 * through it, and an `&I64` has nothing but the value to keep.
 */
static bool containsAddress(DeclBase base, DeclPtr<Type> type, U32 depth) {
    if(!type || depth > 16) return true;

    switch(base[type]->kind) {
        case Type::Pointer:
        case Type::Fun:
        case Type::Variable:
            return true;
        case Type::Atomic:
            return containsAddress(base, ((AtomicType*)base[type])->content, depth + 1);
        case Type::Array:
            return containsAddress(base, ((ArrayType*)base[type])->element, depth + 1);
        case Type::Record:
            for(auto field: ((RecordType*)base[type])->fields.contents(base)) {
                if(field.boxed || containsAddress(base, field.occurrence.type, depth + 1)) return true;
            }

            return false;
        default:
            return false;
    }
}

static bool refersToStorage(DeclBase base, const Occurrence& occurrence) {
    return hasTeardownAt(base, occurrence) || containsAddress(base, occurrence.type, 0);
}

/*
 * The summaries a function's facts are read from.
 *
 * Its own, where the walk reached it. A **generic** is never reached - it has no code, and what runs is
 * a specialization - so its answer is the join of the specializations the walk did reach, which is what
 * the function the author wrote does at every argument list the program calls it at.
 */
static void summarySources(Program& program, DeclPtr<Function> pointer, Array<Function*>& into) {
    auto decl = *program.decl;
    auto& function = *decl[pointer];

    if(function.reachable) {
        into.push(&function);
        return;
    }

    if(!isGeneric(decl, function.gen)) return;

    for(auto module: program.modules) {
        for(auto candidate: decl[module]->functions.contents(decl)) {
            auto& clone = *decl[candidate];
            if(clone.specializes == pointer && clone.reachable) into.push(&clone);
        }
    }
}

static Inferred joinAny(Inferred joined, Inferred next) {
    if(joined == Inferred::Unknown || next == Inferred::Unknown) return Inferred::Unknown;
    return joined == Inferred::Yes || next == Inferred::Yes ? Inferred::Yes : Inferred::No;
}

static Inferred joinAll(Inferred joined, Inferred next) {
    if(joined == Inferred::Unknown || next == Inferred::Unknown) return Inferred::Unknown;
    return joined == Inferred::Yes && next == Inferred::Yes ? Inferred::Yes : Inferred::No;
}

Explanation explainFunction(Program& program, DeclPtr<Function> pointer, const CallSiteIndex* calls) {
    auto decl = *program.decl;
    auto& function = *decl[pointer];

    Explanation explanation;
    explanation.function = pointer;
    explanation.name = function.name;
    explanation.module = function.module ? decl[function.module]->name : StringId();
    explanation.source = function.source;
    explanation.result = function.lens ? function.handover : function.result;

    // Reached by the summaries, which is what every fact below is conditional on. A function the
    // walk did not reach has summaries in their initial state, and their initial state is a guess.
    Array<Function*> sources;
    summarySources(program, pointer, sources);
    explanation.summarized = sources.size() != 0;

    for(U32 i = 0; i < U32(function.args.size()); i++) {
        // Only the positions a call writes - see `describeSymbol`, which prints the same list.
        if(!function.writesArg(i)) continue;

        auto& arg = *decl[function.args.get(decl, i)];

        ArgExplanation entry;
        entry.name = arg.name;
        entry.occurrence = arg.occurrence;
        entry.source = arg.source;
        entry.lazy = arg.lazy;

        // A position with no type is one an editor's compile kept from a signature that did not
        // resolve - `declareSignature` - and nothing was summarized about it either way.
        auto retainable = arg.occurrence.type && refersToStorage(decl, arg.occurrence);

        for(Size s = 0; s < sources.size(); s++) {
            auto mutated = positionFact(sources[s]->summary.mutates(), i);
            auto escapes = retainable ? positionFact(sources[s]->summary.escapes(), i) : Inferred::No;

            entry.mutated = s ? joinAny(entry.mutated, mutated) : mutated;
            entry.escapes = s ? joinAny(entry.escapes, escapes) : escapes;
        }

        explanation.args.push(entry);
    }

    for(Size s = 0; s < sources.size(); s++) {
        auto& summary = sources[s]->summary;
        auto suspends = boolFact(summary.suspension());
        auto diverges = boolFact(summary.diverges());
        auto placement = summary.placement().get(StorageClass::Frame);

        explanation.suspends = s ? joinAny(explanation.suspends, suspends) : suspends;
        explanation.diverges = s ? joinAll(explanation.diverges, diverges) : diverges;
        if(U8(placement) > U8(explanation.placement)) explanation.placement = placement;
    }

    /*
     * Where the body's objects went - the placement the ownership analysis decided, asked of each
     * named local's lineage.
     *
     * Only the ones that are not the boring answer. A frame-placed local is what every reader already
     * assumed, and listing all of them would bury the one that is not. A body the analysis never saw
     * places nothing, so `placed` is what separates "all frame" from "not known".
     */
    explanation.bodyKnown = function.mir != nullptr;

    if(function.mir) {
        auto mir = *program.mir;
        auto& body = *mir[function.mir];
        explanation.placed = body.elaboration != nullptr;

        for(U32 i = 0; explanation.placed && i < body.locals.size(); i++) {
            auto local = body.local(mir, LocalId(i));
            if(local->name == StringId()) continue;

            auto storage = storageOf(mir, body, *local);
            if(storage == StorageClass::Frame) continue;

            LocalExplanation entry;
            entry.name = local->name;
            entry.type = local->occurrence.type;
            entry.storage = storage;
            entry.source = local->source;
            explanation.locals.push(entry);
        }
    }

    explanation.generic = isGeneric(decl, function.gen);
    explanation.form = function.form;
    explanation.specializations = function.specializations;

    if(calls) {
        explanation.callSites = calls->callsOf(pointer);
        explanation.valueSites = calls->valuesOf(pointer);
    }

    return explanation;
}

bool isExplainable(Context& context, Program& program, DeclPtr<Function> pointer) {
    auto decl = *program.decl;
    auto& function = *decl[pointer];

    if(function.specializes || function.environment) return false;
    if(function.provided != intrinsic::Provided::Count) return false;
    if(function.source == kNullLocation) return false;

    // A class member with no body is a signature, and what runs is an instance's.
    if(function.klass && !function.mir) return false;

    if(isCompilerName(context, function.name)) return false;

    // An instance's member is reached by selection rather than by name, and asking about it by name
    // would be asking about every instance of the class at once.
    for(auto module: program.modules) {
        for(auto instance: decl[module]->instances.contents(decl)) {
            for(auto member: decl[instance]->members.contents(decl)) {
                if(member == pointer) return false;
            }
        }
    }

    return true;
}

void findExplainTargets(Context& context, Program& program, StringId module, StringId name,
                        Array<DeclPtr<Function>>& out) {
    auto decl = *program.decl;

    for(auto candidate: program.modules) {
        if(module && decl[candidate]->name != module) continue;

        for(auto pointer: decl[candidate]->functions.contents(decl)) {
            if(decl[pointer]->name != name) continue;
            if(!isExplainable(context, program, pointer)) continue;

            out.push(pointer);
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
        case ExplainTopic::Object:   return "object"_v;
        case ExplainTopic::Calls:    return "calls"_v;
        case ExplainTopic::Bodies:   return "bodies"_v;
    }

    return ""_v;
}

static void appendName(StringBuilder& target, Context& context, StringId name, StringView fallback) {
    if(name) target.append(context.findName(name));
    else target.append(fallback);
}

static void appendText(StringBuilder& target, const String& text) {
    target.append(text.text(), text.size());
}

static void addNote(Array<ExplainNote>& notes, ExplainTopic topic, bool surprising, StringBuilder& text) {
    ExplainNote note;
    note.topic = topic;
    note.surprising = surprising;
    note.text = text.string();
    notes.push(note);
    text.clear();
}

static void plural(StringBuilder& text, U32 count, StringView one, StringView many) {
    show(count, text);
    text.append(count == 1 ? one : many);
}

// What a row says where the summaries never reached the function - once, in the same words everywhere.
static const StringView kUnsummarized = "unknown (no summary reached this function)"_v;

// And where they did, and what they found is the top of the lattice: something this calls could not be
// seen into, so every position is conservatively in the set.
static const StringView kOpaque = "unknown (it calls something whose effect cannot be seen)"_v;

static void explainSuspension(const Explanation& explanation, Array<ExplainNote>& notes) {
    StringBuilder text;

    switch(explanation.suspends) {
        case Inferred::No: text.append("no"_v); break;
        case Inferred::Yes: text.append("yes"_v); break;
        case Inferred::Unknown: text.append(kUnsummarized); break;
    }

    addNote(notes, ExplainTopic::Suspends, explanation.suspends == Inferred::Yes, text);
}

static void explainAmbients(const Explanation&, Array<ExplainNote>& notes) {
    // Not "none". Ambients are not implemented, and a confident negative here would silently become
    // wrong the day they are.
    StringBuilder text;
    text.append("unknown (not implemented)"_v);
    addNote(notes, ExplainTopic::Ambients, false, text);
}

/*
 * Where the result and the frame's own objects live.
 *
 * The result first because it is the one a caller is affected by: everything below it is a decision
 * inside this frame. What the frame could not answer is said before the locals rather than instead of
 * them, since the result's answer comes from the signature and is known either way.
 */
static void explainStorage(Context& context, Program& program, const Explanation& explanation,
                           Array<ExplainNote>& notes) {
    auto decl = *program.decl;
    StringBuilder text;
    auto said = false;

    if(explanation.result.contract.access() != Access::Owned) {
        text.append("the result's storage is the caller's, not this frame's"_v);
        addNote(notes, ExplainTopic::Storage, false, text);
        said = true;
    }

    if(!explanation.bodyKnown) {
        text.append("unknown for the locals (there is no body here to place them in)"_v);
        addNote(notes, ExplainTopic::Storage, false, text);
        said = true;
    } else if(!explanation.placed) {
        text.append("unknown for the locals (this body was not analyzed)"_v);
        addNote(notes, ExplainTopic::Storage, false, text);
        said = true;
    }

    for(auto& local: explanation.locals) {
        if(isCompilerName(context, local.name)) continue;

        appendName(text, context, local.name, "_"_v);
        text.append(": "_v);
        appendText(text, typeText(context, decl, local.type));
        text.append(local.storage == StorageClass::Heap ? " heap"_v : " region"_v);

        addNote(notes, ExplainTopic::Storage, true, text);
        said = true;
    }

    if(said) return;

    // Transitive, so it is the half of placement a frame-placed body can still owe: nothing here went
    // off the frame, and something this body reaches did.
    if(explanation.placement != StorageClass::Frame) {
        text.append("everything here is frame-placed, and something this calls allocates"_v);
    } else {
        text.append("everything is frame-placed"_v);
    }

    addNote(notes, ExplainTopic::Storage, false, text);
}

static bool isPointer(Program& program, const Occurrence& occurrence) {
    auto decl = *program.decl;
    return occurrence.type && decl[occurrence.type]->kind == Type::Pointer;
}

/*
 * What the body writes through. A `&` position is the author having said so, and a raw pointer is a
 * position whose pointee was never this signature's to promise about; the write that is worth a
 * reader's attention is through anything else, since nothing at either end of the call says it.
 */
static void explainMutation(Context& context, Program& program, const Explanation& explanation,
                            Array<ExplainNote>& notes) {
    StringBuilder text;

    if(!explanation.summarized) {
        text.append(kUnsummarized);
        addNote(notes, ExplainTopic::Mutates, false, text);
        return;
    }

    for(auto& arg: explanation.args) {
        if(arg.mutated != Inferred::Unknown) continue;

        text.append(kOpaque);
        addNote(notes, ExplainTopic::Mutates, false, text);
        return;
    }

    auto listed = 0;
    auto surprising = false;

    for(auto& arg: explanation.args) {
        if(arg.mutated != Inferred::Yes) continue;
        if(listed++) text.append(", "_v);

        appendName(text, context, arg.name, "_"_v);
        text.append(" writable"_v);

        if(!arg.unique() && !isPointer(program, arg.occurrence)) {
            text.append(" [not declared &]"_v);
            surprising = true;
        }
    }

    if(!listed) text.append("none"_v);
    addNote(notes, ExplainTopic::Mutates, surprising, text);
}

// What the body keeps of its arguments past the call - `Summary::escapes`. A loan of one of these cannot
// end at the call the way an ordinary argument's does, and nothing in the signature says so.
static void explainRetention(Context& context, const Explanation& explanation, Array<ExplainNote>& notes) {
    StringBuilder text;

    if(!explanation.summarized) {
        text.append(kUnsummarized);
        addNote(notes, ExplainTopic::Retains, false, text);
        return;
    }

    for(auto& arg: explanation.args) {
        if(arg.escapes != Inferred::Unknown) continue;

        text.append(kOpaque);
        addNote(notes, ExplainTopic::Retains, false, text);
        return;
    }

    auto listed = 0;

    for(auto& arg: explanation.args) {
        if(arg.escapes != Inferred::Yes) continue;
        if(listed++) text.append(", "_v);
        appendName(text, context, arg.name, "_"_v);
    }

    if(!listed) text.append("none"_v);
    addNote(notes, ExplainTopic::Retains, listed != 0, text);
}

static void explainResult(Context& context, const Explanation& explanation, Array<ExplainNote>& notes) {
    StringBuilder text;

    if(explanation.diverges == Inferred::Yes) {
        text.append("never - every path ends in a call that does not come back"_v);
        addNote(notes, ExplainTopic::Returns, false, text);
    }

    auto& contract = explanation.result.contract;
    if(contract.access() == Access::Owned) return;

    // Shared and exclusive rather than immutable and mutable - Analysis-Borrows.md §3.4. The modes are
    // properties of one loan rather than of the object, and the aliasing rule is what a reader needs.
    text.append(contract.access() == Access::Unique ? "an exclusive reference"_v : "a shared reference"_v);
    text.append(" rooted in "_v);

    auto slot = contract.originSlot();
    auto listed = 0;

    for(auto& arg: explanation.args) {
        if(slot == kNoOriginSlot || arg.occurrence.contract.originSlot() != slot) continue;
        if(listed++) text.append(", "_v);
        appendName(text, context, arg.name, "_"_v);
    }

    /*
     * Not surprising, including where no parameter lends it - which the shipping stage flagged. That
     * stage could return a borrow rooted in nothing a caller could name; this one refuses such a
     * signature at its declaration, so what reaches here with no parameter is a traversal lending its
     * own frame to the continuation, which is what every `iter fn` yielding a local does.
     */
    if(!listed) text.append("storage no parameter names"_v);
    addNote(notes, ExplainTopic::Returns, false, text);
}

/*
 * How the function is reached - §7.3's specialization line.
 *
 * An erased generic is the surprising one: every call reaches its body through a runtime environment
 * rather than a specialization, and that is exactly the cliff §7.3 says an invisible property needs to
 * be visible for.
 */
static void explainCalls(const Explanation& explanation, Array<ExplainNote>& notes) {
    StringBuilder text;
    auto surprising = false;

    if(explanation.generic && explanation.form == GenForm::Erased) {
        text.append("one erased body for "_v);
        plural(text, explanation.callSites, " call site"_v, " call sites"_v);
        surprising = explanation.callSites != 0;
    } else if(explanation.generic) {
        text.append("specialized at "_v);
        plural(text, explanation.callSites, " call site"_v, " call sites"_v);
        text.append(", "_v);
        plural(text, explanation.specializations, " specialization"_v, " specializations"_v);
    } else {
        text.append("called at "_v);
        plural(text, explanation.callSites, " site"_v, " sites"_v);
    }

    if(explanation.valueSites) {
        text.append(", named as a value at "_v);
        plural(text, explanation.valueSites, " site"_v, " sites"_v);
    }

    addNote(notes, ExplainTopic::Calls, surprising, text);
}

/*
 * The **state record** an `iter fn` compiles to where its result is kept -
 * Design-Iterator-Objects.md §7 stage 3's inspection output, and the same answer the `.ssa` dump
 * prints.
 *
 * Found by scanning the producer's module rather than carried on the function, because a site type
 * is minted by the *first position that builds one* and a producer nothing keeps the result of has
 * none at all - which is itself the answer worth printing.
 *
 * One row per member, and for a payload member the `yield`s it has to survive. That is the question
 * an author actually has about a record - "why is this in it" - and the `#` rows are what the
 * producer keeps across a hand-over without naming: a counted `for`'s own bound and step.
 */
static void explainObject(Context& context, Program& program, const Explanation& explanation,
                          Array<ExplainNote>& notes) {
    auto decl = *program.decl;
    auto& function = *decl[explanation.function];
    if(!function.iterator) return;

    RecordType* site = nullptr;

    for(auto recordPointer: decl[function.module]->records.contents(decl)) {
        auto record = decl[recordPointer];
        if(!record->producer) continue;
        if(&bodyOwnerOf(decl, *decl[record->producer]) != &function) continue;

        site = record;
        break;
    }

    StringBuilder text;

    if(!site) {
        text.append("no object of it is built, so it has no record"_v);
        addNote(notes, ExplainTopic::Object, false, text);
        return;
    }

    text.append(context.findName(site->name));
    text.append(" -"_v);

    for(auto field: site->fields.contents(decl)) {
        text.append(" "_v);
        appendName(text, context, field.name, "#"_v);

        // How a captured argument is held, where the name alone does not say it - the `.ssa` dump
        // prints the same three words beside the same three kinds of field.
        if(field.index >= 2 && field.index < site->payloadFirst) {
            auto kind = site->captureKinds.get(decl, field.index - 2);
            if(kind == RecordType::kCaptureAddress) text.append("(lent)"_v);
            else if(kind == 2) text.append("(window)"_v);
        }

        // `$held` comes after the payload and is not part of it - see `RecordType::heldField` - and
        // so do delegation's two words, which are notes about the chain rather than members.
        if(field.index < site->payloadFirst || field.index == site->heldField) continue;
        if(field.index == site->downField || field.index == site->innerField) continue;

        // Every entry in this field, which is one unless slot sharing put two members whose live
        // ranges never meet in it - `RecordType::payloadFields`. The field's own name says which
        // members those are (`i+j`) and the `across` list here is the union of theirs.
        for(U32 j = 0; j < site->payloadLocals.size(); j++) {
            if(site->payloadFields.get(decl, j) != field.index) continue;

            auto member = site->payloadLocals.get(decl, j);
            if(member & RecordType::kHeapMember) text.append("(placed)"_v);
            if(member & RecordType::kBorrowMember) text.append("(borrowed)"_v);
            if(member & RecordType::kNestMember) {
                /*
                 * `(boxed)` where the child is §6.3's recursion - the member holds the erased form
                 * rather than the child's own record, which is what the field's type says - and
                 * `(delegated)` beside it where the level is one a consumer's trampoline steps past,
                 * which the member's *link* type says.
                 */
                if(site->linkType && field.occurrence.type == site->linkType) {
                    text.append("(nested, boxed, delegated)"_v);
                } else {
                    text.append(decl[field.occurrence.type]->kind == Type::Iter ? "(nested, boxed)"_v
                                                                                 : "(nested)"_v);
                }
            }
        }

        text.append("(across"_v);

        for(U32 y = 0; y < site->handovers; y++) {
            auto keeps = false;

            for(U32 j = 0; j < site->payloadLocals.size() && !keeps; j++) {
                if(site->payloadFields.get(decl, j) != field.index) continue;
                keeps = site->keepsAcross(decl, j, y);
            }

            if(!keeps) continue;

            text.append(" "_v);
            appendText(text, Tritium::format("%@", U64(y)));
        }

        text.append(")"_v);
    }

    addNote(notes, ExplainTopic::Object, false, text);
}

/*
 * Which bodies a call of this function may run - Design-Replacements.md §7.
 *
 * On a baseline, one row per replacement this build read, in the order a call asks them, with the
 * condition that decides it: its selectors, which held or it would not have been read, and the
 * constraints it adds, which each call with concrete types asks. A replacement whose selectors do not
 * hold on this build was never read, so it cannot be listed - which is said, because a reader looking
 * for the AVX-512 body on a v3 build would otherwise read its absence as its nonexistence.
 *
 * On a replacement, the baseline it replaces. Nothing is printed for a function that is neither.
 *
 * An unconditioned replacement is the surprising row: it runs in place of the baseline everywhere, so
 * the body a reader is looking at is not the one that runs.
 */
static void explainBodies(Context& context, Program& program, const Explanation& explanation,
                          Array<ExplainNote>& notes) {
    auto decl = *program.decl;
    StringBuilder text;

    auto constraintsBeyond = [&](Function& replacement, Function& baseline) {
        Array<String> own;
        Array<String> base;

        auto render = [&](Function& function, Array<String>& into) {
            if(!isGeneric(decl, function.gen)) return;

            for(auto requirement: decl[function.gen]->classes.contents(decl)) {
                StringBuilder one;
                one.append(context.findName(decl[requirement.klass]->name));
                one.append("("_v);

                for(U32 i = 0; i < requirement.args.size(); i++) {
                    if(i) one.append(", "_v);
                    appendText(one, typeText(context, decl, requirement.args.get(decl, i)));
                }

                one.append(")"_v);
                into.push(one.string());
            }
        };

        render(replacement, own);
        render(baseline, base);

        auto listed = 0;
        for(auto& written: own) {
            auto inherited = false;
            for(auto& held: base) inherited = inherited || held == written;
            if(inherited) continue;

            text.append(listed++ ? ", "_v : " where "_v);
            appendText(text, written);
        }

        if(listed) text.append(" holds at a call's concrete types"_v);
    };

    if(auto set = program.replacementsOf(explanation.function)) {
        for(auto& candidate: set->candidates) {
            auto& body = *decl[candidate.body];

            text.append(context.findName(body.symbol));
            if(candidate.selectors) {
                text.append(" on "_v);
                text.append(context.findName(candidate.selectors));
            }

            if(candidate.conditioned) constraintsBeyond(body, *decl[explanation.function]);
            else text.append(", in place of this one wherever this build reads it"_v);

            addNote(notes, ExplainTopic::Bodies, !candidate.conditioned, text);
        }

        text.append("and this one otherwise, or under -no-replacements; a replacement whose selectors do not hold on this build is not read, so it is not listed"_v);
        addNote(notes, ExplainTopic::Bodies, false, text);
        return;
    }

    for(auto entry: program.replacements.entries()) {
        for(auto& candidate: entry.value.candidates) {
            if(candidate.body != explanation.function) continue;

            auto& baseline = *decl[DeclPtr<Function>(entry.key)];
            text.append("replaces "_v);
            text.append(context.findName(baseline.symbol));
            if(candidate.selectors) {
                text.append(" on "_v);
                text.append(context.findName(candidate.selectors));
            }

            if(candidate.conditioned) constraintsBeyond(*decl[explanation.function], baseline);
            addNote(notes, ExplainTopic::Bodies, false, text);
            return;
        }
    }
}

void explainNotes(Context& context, Program& program, const Explanation& explanation,
                  Array<ExplainNote>& notes) {
    // §7.3's row order, and it is deliberate: the property with no analysis behind it comes early so
    // that a reader is told what is *not* known before they read what is.
    explainSuspension(explanation, notes);
    explainAmbients(explanation, notes);
    explainStorage(context, program, explanation, notes);
    explainMutation(context, program, explanation, notes);
    explainRetention(context, explanation, notes);
    explainResult(context, explanation, notes);
    explainObject(context, program, explanation, notes);
    explainCalls(explanation, notes);
    explainBodies(context, program, explanation, notes);
}

/*
 * The three surfaces.
 */

void printExplanation(StringBuilder& target, Context& context, Program& program,
                      const Explanation& explanation) {
    describeSymbol(context, program, functionSymbol(*program.decl, explanation.function), nullptr,
                   target);
    target.append("\n"_v);

    // Said once, at the top, rather than qualifying every row below it.
    if(!explanation.summarized) {
        target.append("  (the summaries did not reach this function, so what it does to its callers is not known)\n"_v);
    }

    Array<ExplainNote> notes;
    explainNotes(context, program, explanation, notes);

    // The label is written once per topic and the rows under it are aligned to the same column, which
    // is what makes several heap locals read as one answer.
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

bool printExplanationHover(StringBuilder& target, Context& context, Program& program,
                           const Explanation& explanation) {
    Array<ExplainNote> notes;
    explainNotes(context, program, explanation, notes);

    auto surprising = 0;
    for(auto& note: notes) surprising += note.surprising ? 1 : 0;
    if(!surprising) return false;

    // A markdown list under a rule, so that it reads as a section of the hover rather than as more of
    // the signature above it. The label is bold because it is the thing a reader scans for.
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

void printExplanations(Net::Writer& writer, Context& context, Program& program) {
    auto decl = *program.decl;
    Size index = 0;

    CallSiteIndex calls;
    calls.build(program);

    for(auto module: program.modules) {
        if(decl[module]->fromLibrary) continue;

        for(auto pointer: decl[module]->functions.contents(decl)) {
            if(!isExplainable(context, program, pointer)) continue;
            if(index++) writer.writeByte('\n');

            auto explanation = explainFunction(program, pointer, &calls);

            StringBuilder target;
            printExplanation(target, context, program, explanation);

            auto hint = explanationSummary(context, program, explanation);
            if(hint.size()) {
                target.append("  hint:       "_v);
                target.append(hint);
                target.append("\n"_v);
            }

            // Written out as it stands rather than re-indented: what a fixture should pin is the exact
            // text the server hands the client.
            StringBuilder hover;
            if(printExplanationHover(hover, context, program, explanation)) {
                target.append("  hover:"_v);
                target.append(stringView(hover));
            }

            writer.writeString(stringView(target));
        }
    }
}

} // namespace mir
