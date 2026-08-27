#include "caller.h"
#include "expr.h"
#include "type_internal.h"

/*
 * The declaration half - see caller.h. Runs while the signature is being resolved, and the only
 * thing it writes is `Arg::caller`.
 *
 * The fill is a constant of the parameter's declared type, so the type is what says which fill this
 * is and whether it can be built at all. Two shapes and nothing else:
 *
 *  - `@caller(source: p) text: String` is the source text of whatever reached `p`. `String` and a
 *    parameter of this signature; anything else is reported here rather than becoming an empty
 *    string at every call site.
 *  - `@caller at: T` is the call's location, and `T` is a **record whose fields are drawn from a
 *    fixed vocabulary**: `file` and `function` are text, `line` and `column` are numbers. Matching
 *    by field name is what lets a declaration ask for the three things it wants and leave out the
 *    ones it does not. A field with any other name is what this reports, and it names the four that
 *    exist.
 *
 * Declining the marker on failure rather than leaving it half-set: what a `@caller` position that
 * cannot be filled must not become is a position every call site may leave out and nothing fills.
 */
void resolveCallerFill(Module& module, Function& function, Arg& declared, StringId sourceName,
                       LocationId source) {
    auto global = *module.types;
    auto local = *module.arena;

    if(declared.isMutableBorrow() || declared.returnRoot()) {
        module.context.diagnostics.error("a `&` or `return` parameter cannot be `@caller` - the fill is a constant built at the call, and there is no storage of the caller's for it to name"_v,
                                         source);
        return;
    }

    if(declared.hasDefault()) {
        module.context.diagnostics.error("a `@caller` parameter cannot also have a written default - `@caller` *is* what fills it where a call site leaves it out"_v,
                                         source);
        return;
    }

    auto type = declared.declaredType();
    if(!type || global[type]->kind == Type::Error) return;

    if(sourceName) {
        U16 position = 0;
        auto found = false;

        for(auto argPointer: function.args.contents(local)) {
            if(local[argPointer]->name == sourceName) {
                declared.caller.source = position;
                found = true;
                break;
            }

            position++;
        }

        if(!found) {
            module.context.diagnostics.error("%@ is not a parameter of this function, so there is no expression for `@caller(source: %@)` to be the text of"_v,
                                             source, module.context.findName(sourceName),
                                             module.context.findName(sourceName));
            return;
        }

        if(declared.caller.source == declared.index) {
            module.context.diagnostics.error("`@caller(source: ...)` names this parameter itself - the text it would hold is the text of the position nothing was written at"_v,
                                             source);
            return;
        }

        if(!sameType(type, module.scalar.string_)) {
            module.context.diagnostics.error("a `@caller(source: ...)` parameter is the *text* of an expression, so it has to be declared `String`"_v,
                                             source);
            return;
        }

        declared.caller.fill = CallerFill::Source;
        return;
    }

    if(global[type]->kind != Type::Record) {
        module.context.diagnostics.error("a `@caller` parameter is filled with the call's location, which is a record - declare it as one with fields drawn from `file: String`, `line`, `column` and `function: String`"_v,
                                         source);
        return;
    }

    auto record = (RecordType*)global[type];
    auto content = record->constructors.size() == 1
        ? record->constructors.get(global, 0).content : TypePtr(nullptr);

    if(!content || global[content]->kind != Type::Tup) {
        module.context.diagnostics.error("a `@caller` parameter is filled with the call's location, which is a record of named fields - declare it as one with fields drawn from `file: String`, `line`, `column` and `function: String`"_v,
                                         source);
        return;
    }

    auto ok = true;

    for(auto field: ((TupType*)global[content])->fields.contents(global)) {
        auto name = module.context.findName(field.name);
        auto text = name == "file"_v || name == "function"_v;
        auto number = name == "line"_v || name == "column"_v;

        if(!text && !number) {
            module.context.diagnostics.error("a `@caller` location has no %@ - what the compiler knows about a call site is `file: String`, `line`, `column` and `function: String`"_v,
                                             source, name);
            ok = false;
            continue;
        }

        auto fits = text ? sameType(field.type, module.scalar.string_) : isInteger(global, field.type);

        if(!fits) {
            module.context.diagnostics.error("field %@ of a `@caller` location is declared %@ - `file` and `function` are `String`, and `line` and `column` are integers"_v,
                                             source, name, describeType(module.context, global, field.type));
            ok = false;
        }
    }

    if(ok) declared.caller.fill = CallerFill::Site;
}

ModulePtr<Value> buildCallerSite(ExprResolver& resolver, TypePtr type, LocationId at, StringId containing) {
    auto& context = resolver.context;
    auto global = resolver.global;

    auto storage = resolver.allocate(type, at);
    if(!storage) return nullptr;

    auto storagePlace = resolver.findPlace(storage);
    if(!storagePlace) return nullptr;

    // Down to the single constructor before the fields, which is what a field place *is* - a record
    // is a sum of one, and `x.f` is a downcast to its only constructor followed by an ordinary field
    // projection. Without it the projection lands on the record rather than on its content, which
    // natively is the same address and on JavaScript is a property read against the wrong object.
    auto place = resolver.project(storagePlace.unwrap(), ProjectionKind::Downcast, 0);

    auto node = context.diagnostics.sourceNode(at);
    auto record = (RecordType*)global[type];
    auto content = record->constructors.get(global, 0).content;

    U16 index = 0;

    for(auto field: ((TupType*)global[content])->fields.contents(global)) {
        auto name = context.findName(field.name);
        ModulePtr<Value> value = nullptr;

        if(name == "file"_v) {
            value = resolver.resolveString(at, node ? node->sourceModule : StringId());
        } else if(name == "function"_v) {
            value = resolver.resolveString(at, containing ? containing : resolver.function.name);
        } else if(name == "line"_v) {
            value = resolver.makeInt(at, field.type, node ? node->sourceStart.line + 1 : 0);
        } else {
            value = resolver.makeInt(at, field.type, node ? node->sourceStart.column : 0);
        }

        if(!value) return nullptr;
        resolver.initialize(resolver.project(place, ProjectionKind::Field, index), value, at);
        index++;
    }

    return storage;
}

ModulePtr<Value> callerFillValue(ExprResolver& resolver, const Arg& parameter, LocationId at,
                                 Buffer<ResolvedArg> args) {
    if(parameter.caller.fill == CallerFill::Site) {
        return buildCallerSite(resolver, parameter.declaredType(), at);
    }

    /*
     * The text of whatever reached the named parameter, or `""` where nothing did.
     *
     * Nothing reaching it is a real case rather than a failure: the position may itself have been
     * defaulted, or the call may be one the compiler synthesized, and neither has an expression the
     * author wrote. An empty string says exactly that and costs nothing.
     */
    auto written = parameter.caller.source < args.size() ? args[parameter.caller.source].written
                                                         : kNullLocation;

    auto text = written != kNullLocation ? resolver.context.diagnostics.sourceText(written)
                                         : StringView { nullptr, 0 };

    return resolver.resolveString(at, resolver.context.addUnqualifiedName(text.ptr, U32(text.length)));
}
