#include "resolve_internal.h"

namespace mir {

/*
 * `@caller` - Design-Test.md §11.1's F2, and the whole of it: what a declaration may ask for
 * (`declareCallerFill`) and what a call that left the position out passes (`callerFillValue`).
 *
 * **The fill happens where the position was omitted**, and everything else follows from that. A
 * helper that passes its own `at:` on explicitly is never filled here, so it reports the line that
 * called *it* - which is Rust's `#[track_caller]`, obtained because "a position no argument reached
 * is filled by its default" is already how a default works. Nothing about arity, named arguments,
 * overload selection or conversion needed a rule of its own: `Arg::omittable` is the one predicate
 * that had to learn there are two ways to fill a position, and it answers both the same.
 *
 * **The fill is built the way the same expression written by hand would be.** A text fill is
 * `textValue` - the one place a text becomes a value, which every string literal goes through - and
 * a location fill is `Builder::addAggregate` over four of those values, which is what a written
 * `Site {file: ..., line: ...}` is. There is no second code path for calls, for strings or for
 * records here.
 *
 * A `Constant::Aggregate` was tried first and is not available: a location holds a `String`, which
 * is an **owner** in this language, and `Editor::addConst` asserts that a constant's type is not
 * linear. A constant of an owning type would be static bytes with a teardown, which is the one
 * thing the ownership model has no way to say. What makes the built form cost nothing anyway is
 * that a string literal owns nothing - its teardown is a run over a constant global - so the
 * optimizer removes the record wherever it removes any other one.
 *
 * The compiler knows nothing about the library's `Site` record - it lives in `Test` and not in
 * `Core` - so what a declaration may ask for is matched by **field name** against a fixed
 * vocabulary rather than against a type this stage holds a handle to. A framework that wanted only
 * the line declares a record with only `line` in it and gets one.
 */

// The four names a location record's fields may be drawn from, and which kind of constant each is.
// A table rather than a chain of comparisons because the diagnostic below names the whole
// vocabulary, and a fifth field is then one row and not three edits.
enum class CallerField: U8 { File, Line, Column, Function, None };

static CallerField callerFieldOf(const String& name) {
    if(name == "file"_v) return CallerField::File;
    if(name == "line"_v) return CallerField::Line;
    if(name == "column"_v) return CallerField::Column;
    if(name == "function"_v) return CallerField::Function;
    return CallerField::None;
}

/*
 * Whether a field already matched to a vocabulary slot by `callerFieldOf` is declared the type
 * that slot needs - `Int` for `line`/`column`, and whatever a string literal answers for
 * `file`/`function`. The one rule both readers of the vocabulary check: `declareCallerFill` cannot
 * call this for its own `file`/`function` fields, because `text` is not certainly resolved at
 * declaration time and has to be deferred through `PendingCallerText` instead (see
 * `checkCallerTexts`), but its `line`/`column` check and every field `isCallerSiteRecord` asks about
 * - which runs once every declaration exists, so `text` is simply looked up - are the same question
 * asked the same way. `text` is null where the caller has none to offer, which only ever matches a
 * `None` field or fails a `File`/`Function` one, both already refused before it would be read.
 */
static bool callerFieldMatches(CallerField which, DeclPtr<Type> fieldType, DeclBase decl,
                               DeclPtr<Type> text) {
    switch(which) {
        case CallerField::File:
        case CallerField::Function: return text && sameType(fieldType, text);
        case CallerField::Line:
        case CallerField::Column: return decl[fieldType]->kind == Type::Int;
        case CallerField::None: return false;
    }

    return false;
}

/*
 * The marker on one parameter, checked against what the compiler is able to put there.
 *
 * The fill is a constant of the parameter's **declared type**, so the type is what says which fill
 * this is and whether it can be built at all. Two shapes and nothing else:
 *
 *  - `@caller(source: p) text: String` is the source text of whatever reached `p`. `String` - the
 *    type the library's own `stringLiteral` answers - and a parameter of this signature; anything
 *    else is reported here rather than becoming an empty string at every call site.
 *  - `@caller at: T` is the call's location, and `T` is a record whose fields are drawn from the
 *    vocabulary above.
 *
 * Called once every parameter exists, so that `@caller(source: p)` finds a `p` written after it -
 * see `declareArgs`, which collects the marked positions and comes back for them.
 *
 * **Declining the marker on failure** rather than leaving it half-set: what a `@caller` position
 * that cannot be filled must not become is a position every call site may leave out and nothing
 * fills.
 */
bool Resolver::declareCallerFill(Function& function, Arg& declared, const ast::Arg& argument) {
    if(argument.bind == ast::BindType::Ref) {
        context.diagnostics.error(
            "a `&` parameter cannot be `@caller` - the fill is a constant built at the call, and there is no storage of the caller's for it to name"_v,
            argument.source);
        return false;
    }

    if(declared.occurrence.contract.origin() == Origin::Slot) {
        context.diagnostics.error(
            "%@ publishes an origin, and a `@caller` fill is a constant of the *callee's* making with no caller root to bind that slot to"_v,
            argument.source, context.findName(argument.name));
        return false;
    }

    if(declared.def.isSet()) {
        context.diagnostics.error(
            "a `@caller` parameter cannot also have a written default - `@caller` *is* what fills it where a call site leaves it out"_v,
            argument.source);
        return false;
    }

    /*
     * `@lazy` with it, refused rather than built.
     *
     * A deferred position is filled by a nullary function over the constant, and `Arg::defThunk`
     * caches exactly one of those per parameter because a default *is* one constant. A caller fill
     * is a different constant at every call, so the pair would need a thunk per call site - a
     * function per assertion in the program, to defer a record of four constants that costs nothing
     * to build. The two markers are refused together until something wants that trade.
     */
    if(argument.lazy) {
        context.diagnostics.error(
            "`@lazy` and `@caller` on one parameter would need a thunk per call site - a `@caller` fill is a different constant at every call, and there is nothing in one to defer"_v,
            argument.source);
        return false;
    }

    auto type = declared.occurrence.type;
    if(!type) return false;

    /*
     * The **text** fill, which names another parameter of this same signature.
     *
     * By name and resolved to an index here, so that everything downstream reads a number: the call
     * site has the mapping in hand and no names, and a lookup deferred to it would be a second
     * search per call.
     */
    if(argument.callerSource) {
        auto found = false;

        for(auto pointer: function.args.contents(decl())) {
            auto& other = *decl()[pointer];
            if(other.name != argument.callerSource) continue;

            declared.caller.source = U16(other.index);
            found = true;
            break;
        }

        if(!found) {
            context.diagnostics.error(
                "%@ is not a parameter of this function, so there is no expression for `@caller(source: %@)` to be the text of"_v,
                argument.source, context.findName(argument.callerSource),
                context.findName(argument.callerSource));
            return false;
        }

        if(declared.caller.source == declared.index) {
            context.diagnostics.error(
                "`@caller(source: ...)` names this parameter itself - the text it would hold is the text of the position nothing was written at"_v,
                argument.source);
            return false;
        }

        // The type, checked where the role is certainly bound - see `PendingCallerText`. What is
        // decided *here* is that this is the text fill, which the marker's own spelling says.
        pendingCallerTexts.push(PendingCallerText { type, argument.source, StringId() });

        declared.caller.fill = CallerFill::Source;
        return true;
    }

    if(decl()[type]->kind != Type::Record) {
        context.diagnostics.error(
            "a `@caller` parameter is filled with the call's location, which is a record - declare it as one with fields drawn from `file: String`, `line`, `column` and `function: String`"_v,
            argument.source);
        return false;
    }

    auto& record = *(RecordType*)decl()[type];

    if(record.cons.size() != 1) {
        context.diagnostics.error(
            "a `@caller` parameter is filled with the call's location, which is a record of named fields rather than a choice between constructors"_v,
            argument.source);
        return false;
    }

    auto ok = true;

    for(auto field: record.fields.contents(decl())) {
        auto name = context.findName(field.name);
        auto which = callerFieldOf(name);

        if(which == CallerField::None) {
            context.diagnostics.error(
                "a `@caller` location has no %@ - what the compiler knows about a call site is `file: String`, `line`, `column` and `function: String`"_v,
                argument.source, name);
            ok = false;
            continue;
        }

        // A text field's type waits for the role, like the source fill's above; a number's is
        // answered here, since what an integer is is this stage's own.
        if(which == CallerField::File || which == CallerField::Function) {
            pendingCallerTexts.push(PendingCallerText {
                field.occurrence.type, argument.source, field.name
            });

            continue;
        }

        if(!callerFieldMatches(which, field.occurrence.type, decl(), nullptr)) {
            context.diagnostics.error(
                "field %@ of a `@caller` location is declared %@ - `line` and `column` are integers"_v,
                argument.source, name, typeText(context, decl(), field.occurrence.type));
            ok = false;
        }
    }

    if(ok) declared.caller.fill = CallerFill::Site;
    return ok;
}

/*
 * The text types the declarations above queued - `PendingCallerText`, where the ordering is argued.
 *
 * Asked once for the whole program rather than once per position, exactly as `checkTextDefaults`
 * asks it: the answer is the same for all of them, and a library declaring no `stringLiteral` is one
 * diagnostic and not one per assertion in it.
 *
 * And **drains**, on the same terms: the pass runs a second time for a program that deferred a
 * record's fields behind a declaration path, so an entry left behind would be reported twice.
 */
void Resolver::checkCallerTexts() {
    if(!pendingCallerTexts.size()) return;

    auto text = textType(pendingCallerTexts[0].source);
    if(!text) return;

    for(auto& pending: pendingCallerTexts) {
        if(sameType(pending.type, text)) continue;

        if(pending.field) {
            context.diagnostics.error(
                "field %@ of a `@caller` location is declared %@ - `file` and `function` are %@"_v,
                pending.source, context.findName(pending.field),
                typeText(context, decl(), pending.type), typeText(context, decl(), text));
        } else {
            context.diagnostics.error(
                "a `@caller(source: ...)` parameter is the *text* of an expression, so it has to be declared %@ - this is %@"_v,
                pending.source, typeText(context, decl(), text),
                typeText(context, decl(), pending.type));
        }
    }

    pendingCallerTexts.clear();
}

/*
 * What a `@caller` position the call site left out is filled with - one value, built from the
 * call's own syntax.
 *
 * `source` is the call as a whole and `map` is what each declared position was written at, so a
 * `Source` fill reads the span of the expression that reached the parameter it names - which is the
 * one thing `ArgMap::written` exists for.
 *
 * The **enclosing** function is the one being resolved, which is the right answer for a fill and
 * the only one there is: a `@caller` position is filled where the call is written, and that is
 * inside a body.
 */
Node* Resolver::callerFillValue(Builder& builder, const Arg& parameter, const ArgMap& map,
                                LocationId source) {
    auto type = parameter.occurrence.type;

    /*
     * The text of whatever reached the named parameter, or `""` where nothing did.
     *
     * Nothing reaching it is a real case rather than a failure: the position may itself have been
     * defaulted, the call may be an operator's or one the compiler synthesized, and neither has an
     * expression the author wrote. An empty string says exactly that and costs nothing - see
     * `claimOf` in `lib/Test/Check.yana`, which is the library reading it back.
     */
    if(parameter.caller.fill == CallerFill::Source) {
        auto written = map.writtenAt(parameter.caller.source);
        auto text = written != kNullLocation ? context.diagnostics.sourceText(written)
                                             : StringView { nullptr, 0 };

        return textValue(builder, context.addUnqualifiedName(text.ptr, U32(text.length)), source);
    }

    return siteValue(builder, type, source, StringId());
}

/*
 * A location record, field by field from one source node - the value both readers of the vocabulary
 * build.
 *
 * `addAggregate` takes one value per declared field, so the walk is over the *declaration* and the
 * vocabulary decides what goes in each slot rather than the other way round. That is what makes a
 * record with only `line` in it a legal `@caller` position and what makes the same function serve
 * the synthesized test entry, whose `Site` is the library's and whose fields it does not know
 * either.
 *
 * `containing` names the function the site is *inside*. Empty means the one being resolved, which
 * is the answer for a `@caller` fill and the wrong one for the test entry - whose sites belong to
 * the declarations it is building cases out of rather than to itself.
 */
Node* Resolver::siteValue(Builder& builder, DeclPtr<Type> type, LocationId at, StringId containing) {
    auto& record = *(RecordType*)decl()[type];
    auto node = context.diagnostics.sourceNode(at);

    SmallArray<Node*, 4> values;

    for(auto field: record.fields.contents(decl())) {
        auto fieldType = field.occurrence.type;
        Node* value = nullptr;

        switch(callerFieldOf(context.findName(field.name))) {
            case CallerField::File:
                value = textValue(builder, node ? node->sourceModule : StringId(), at);
                break;
            case CallerField::Function:
                value = textValue(builder, containing ? containing
                                         : currentFunction ? currentFunction->name : StringId(), at);
                break;
            case CallerField::Line:
                // One-based, which is what an editor and every diagnostic this compiler prints
                // count in. The column is not: `Loc::column` is a byte offset within the line with
                // tabs expanded, and the report is what says so.
                value = builder.addConst(at, intConstant(fieldType,
                                                         node ? U64(node->sourceStart.line) + 1 : 0));
                break;
            case CallerField::Column:
                value = builder.addConst(at, intConstant(fieldType,
                                                         node ? U64(node->sourceStart.column) : 0));
                break;
            case CallerField::None:
                // Not reachable from a `@caller` fill: `declareCallerFill` declined the marker for a
                // record with a field outside the vocabulary. `isCallerSiteRecord` is what says the
                // same thing about the library's `Site`.
                return nullptr;
        }

        if(!value) return nullptr;
        values.push(value);
    }

    return builder.addAggregate(at, type, 0, values.pointer(), values.size());
}

/*
 * Whether every field of this record is one the vocabulary names, of a type `siteValue` can
 * actually build a fill from - the same question `declareCallerFill` asks of a written `@caller`
 * position, asked of a type the *library* declared.
 *
 * Its own entry point because the test entry has no declaration to report against: what it holds is
 * `Test.Case`'s `site` field, and a `Site` the compiler cannot fill is a diagnostic about the
 * library rather than about a parameter somebody wrote. It used to check the name alone and leave
 * the type to `siteValue`, which has no diagnostic to give - a `Site {line: String}` reached it and
 * built an `Int`-tagged constant carrying a `String` type. `callerFieldMatches` is `declareCallerFill`'s
 * own check; unlike it, this runs once every declaration in the program exists (`resolveTestEntry` is
 * well after `checkCallerTexts`), so `text` is looked up directly instead of being deferred.
 */
bool Resolver::isCallerSiteRecord(DeclPtr<Type> type) {
    if(!type || decl()[type]->kind != Type::Record) return false;

    auto& record = *(RecordType*)decl()[type];
    if(record.cons.size() != 1) return false;

    DeclPtr<Type> text = nullptr;

    for(auto field: record.fields.contents(decl())) {
        auto which = callerFieldOf(context.findName(field.name));
        if(which == CallerField::None) return false;

        // Looked up on first need rather than unconditionally, so a `Site` with no `file`/
        // `function` field never asks a library that may not have declared `stringLiteral` at all.
        if((which == CallerField::File || which == CallerField::Function) && !text) {
            text = textType(kNullLocation);
            if(!text) return false;
        }

        if(!callerFieldMatches(which, field.occurrence.type, decl(), text)) return false;
    }

    return true;
}

} // namespace mir
