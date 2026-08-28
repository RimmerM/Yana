/*
 * The expression resolver's core: what a written expression dispatches to, and the values that have
 * no sub-expressions at all.
 *
 * `ExprResolver::resolve` is the one switch over `ast::Expr`, and everything it names is either
 * below it or in one of the files beside this one - see expr.h for the whole roster. What is kept
 * here with it is the leaves: a name lookup, an integer, a float, a string, and the bindings those
 * names resolve through.
 */

#include "expr.h"
#include "const.h"
#include "complete.h"
#include "generic.h"
#include "name.h"
#include "index.h"

void ExprResolver::terminate(Inst* inst) {
    assertTrue(isTerminator(*inst));
    current = nullptr;
}

/*
 * One inserted check - a subscript's bounds test, a `@bits` store's range test, a division by zero.
 *
 * Two shapes, and which one is emitted is a property of the build rather than of the site.
 * `checkCondition(failed)` is the default and carries nothing: what a program that stops on one says
 * is its exit status. `-check-locations` selects `checkConditionAt`, which takes the site's module,
 * line and column as three constants and prints them before it stops.
 *
 * The constants are built here rather than by a `@caller` fill, because there is no call site to
 * omit an argument at: this call *is* the compiler's, so what F2 does for a written call is done
 * directly. `resolveString` over the module name is the same string every check in a file names, so
 * it costs one constant global per module however many checks there are; the two numbers are
 * immediates.
 */
void ExprResolver::emitCheck(ModulePtr<Value> failed, LocationId source) {
    if(!failed || !checksEnabled()) return;

    /*
     * And the string constructor has to exist, which is not a formality: a native string literal is
     * built through `Core.stringLiteral`, and that is bound when `Native`'s intrinsics are attached.
     * A check emitted while the prelude itself is still being resolved is earlier than that, so those
     * few sites take the unlocated form rather than reporting an internal error - which is what the
     * first version of this did, twice, before any program had been compiled at all.
     */
    auto canName = isJsMode(context.settings) || module.program.stringLiteral;

    if(context.settings.checkLocations && module.program.checkConditionAt && canName) {
        auto node = context.diagnostics.sourceNode(source);
        auto where = node ? node->sourceModule : StringId();
        auto line = makeInt(source, module.scalar.int_, node ? node->sourceStart.line + 1 : 0);
        auto column = makeInt(source, module.scalar.int_, node ? node->sourceStart.column : 0);

        if(isJsMode(context.settings)) {
            // A host string is a value rather than a descriptor with an address, so the ordinary
            // literal is already the cheap form here.
            ResolvedArg located[] = { failed, resolveString(source, where), line, column };
            emitDirectCall(module.program.checkConditionAt, { located, 4 }, source);
            return;
        }

        /*
         * The bytes and their length as two scalars - see the note above `checkFailedAt`.
         *
         * A `String` in this position is a two-word descriptor passed by address, so a literal one
         * is sixteen bytes stored into a stack slot *at the call*, which is on the path a check that
         * holds takes. That measured at 34% of a loop whose bounds check cannot be folded away. The
         * blob is the same global `resolveString` would have pointed at; what is different is that
         * its address and its length travel as immediates the register allocator can leave in the
         * arm that stops.
         */
        auto text = context.findName(where);
        auto blob = stringLiteralBytes(module, where, source);
        if(!blob) return;

        auto pointerType = local[local[module.program.checkConditionAt]->args.get(local, 1)]->declaredType();
        auto address = ref(emit<InstSymbol>(source, StringId(), pointerType, nullptr,
                                            blob - *module.arena));

        ResolvedArg located[] = {
            failed,
            address,
            makeInt(source, module.scalar.int_, text.size()),
            line,
            column,
        };

        emitDirectCall(module.program.checkConditionAt, { located, 5 }, source);
        return;
    }

    ResolvedArg condition[] = { failed };
    emitDirectCall(module.program.checkCondition, { condition, 1 }, source);
}

Binding* ExprResolver::findBinding(StringId name, LocationId source) {
    Binding* found = nullptr;

    for(Size i = bindings.size(); i > 0; i--) {
        if(bindings[i - 1].name == name) {
            found = &bindings[i - 1];
            break;
        }
    }

    // A name a lambda body does not bind itself may still belong to an enclosing one, and naming it
    // is what makes it a capture. Nothing is a capture until it is used, which is Design-Memory
    // §8's "there is no capture list" made literal.
    if(!found) found = captureBinding(name, source);

    // §1.2's function-local choke point. A null source is a caller asking whether the name is bound
    // at all rather than reading it - resolveCall does exactly that to tell a call of a binding from
    // a call of a declaration - and an occurrence nobody wrote is not one to record.
    if(found && source != kNullLocation) recordBinding(*this, *found, source);

    return found;
}

/*
 * One binding, as the index records it.
 *
 * Which of the three local kinds it is comes off the binding rather than being carried on it: a
 * capture says so, and an ordinary binding whose value is the parameter itself is an argument. The
 * payload is the slot the kind addresses - the local index, the environment field, or the argument
 * index - which is only meaningful together with the enclosing function, and that is what `function`
 * is for.
 */
Symbol bindingSymbol(ExprResolver& resolver, const Binding& binding) {
    Symbol symbol;
    symbol.kind = Symbol::Kind::Local;
    symbol.module = &resolver.module;
    symbol.function = &resolver.function - resolver.local;
    symbol.name = binding.name;
    symbol.definition = binding.definition;
    symbol.payload = binding.local;

    if(binding.captured) {
        symbol.kind = Symbol::Kind::Capture;
        symbol.payload = binding.captureField;
    } else if(binding.value && resolver.local[binding.value]->kind == Value::Arg) {
        symbol.kind = Symbol::Kind::Arg;
        symbol.payload = ((Arg*)resolver.local[binding.value])->index;
    }

    return symbol;
}

void recordBinding(ExprResolver& resolver, const Binding& binding, LocationId source) {
    if(!resolver.context.index) return;
    recordReference(resolver.context, source, bindingSymbol(resolver, binding), bindingType(resolver, binding));
}

/*
 * The binding itself, as a definition rather than as a use - §1.2's `expr_pat.cpp` row.
 *
 * Recorded where the name is *introduced*, which the references cannot stand in for: a `let` whose
 * name is never read afterwards is recorded nowhere at all otherwise, and it is exactly the one an
 * editor is asked about while it is being written. It is also what makes find-references work from
 * the declaration rather than only from a use.
 */
void recordBindingDefinition(ExprResolver& resolver, const Binding& binding) {
    if(!resolver.context.index || binding.definition == kNullLocation) return;

    auto symbol = bindingSymbol(resolver, binding);
    recordDefinition(resolver.context, symbol);

    /*
     * And as an occurrence of itself, which is not redundant: a Symbol says which slot a name is
     * and a Reference says what type it had there, and the type of a local is not reachable from
     * the slot - an immutable binding names an SSA value rather than a frame slot at all. So a
     * declaration nothing reads afterwards - which is exactly the one being written - would
     * otherwise have no type recorded anywhere.
     *
     * The two surfaces that list occurrences leave it out where it would be a duplicate of the
     * declaration they already write; see lsp/feature.cpp.
     */
    recordReference(resolver.context, binding.definition, symbol, bindingType(resolver, binding));
}

ModulePtr<Value> ExprResolver::find(StringId name) {
    auto binding = findBinding(name);
    return binding ? binding->value : nullptr;
}

/*
 * An integer constant, reduced to its type's normal form on the way in.
 *
 * A literal written where its type is already known is built at that type directly rather than
 * converted into it, so it reached none of the narrowing a conversion emits: `Box {small: 20}` on a
 * `@bits(4)` field stored 20, while `Box {small: v}` for a runtime `v` of 20 stored 4. Whether a
 * store narrowed came down to whether it could be folded, which is the one thing constant folding
 * must never decide.
 *
 * Every integer constant funnels through here, so this is the one place the rule has to be stated -
 * and the reason the *warning* below cannot live here: most callers hand this bits that are already
 * at the type's width.
 */
ModulePtr<Value> ExprResolver::makeInt(LocationId source, TypePtr type, U64 value) {
    if(type && global[type]->kind == Type::Int) value = reduceToWidth(*(IntType*)global[type], value);
    return constant<ConstInt>(source, type, value);
}

ModulePtr<Value> ExprResolver::countOf(TypePtr count, TypePtr type, LocationId source) {
    return ref(emit<InstTypeMetric>(source, StringId(), type, count, TypeMetricKind::Count));
}

ModulePtr<Value> ExprResolver::constParameterValue(StringId name, LocationId source) {
    auto env = functionGen(global, function);
    if(!env) return nullptr;

    // Looked up without creating, deliberately: a name nothing declared is an unknown name and not
    // a new const parameter. An open context introduces a variable from a *type* position, where
    // the position says what kind it is; an expression position says only "a value", which every
    // name is.
    auto found = findGenVariable(module, *env, name);
    if(!found || global[found]->kind != GenKind::Const) return nullptr;

    auto variable = (Type*)global[found] - global;
    recordReference(context, source, typeVarSymbol(module, found), variable);

    return countOf(variable, global[found]->constType, source);
}

/*
 * Reports a written literal that does not fit the type it is being built at.
 *
 * A warning rather than an error, because `makeInt` above gives the program a defined meaning either
 * way and because a full-width mask written at a signed type - `0xFFFFFFFF :: Int` - is a real idiom
 * rather than a mistake. What it catches is the case that has no other symptom at all: `Box {small:
 * 20}` on a `@bits(4)` field is 4, and nothing in the source says 4.
 *
 * Only where a *literal* is built. Every other caller of `makeInt` hands it bits that are already at
 * the type's width - a mask this file computed, a shift distance, the stored initializer of a global
 * - so checking there would report the compiler's own constants back at the author.
 *
 * Written literals are never negative: `-1` is `0 - 1`, two literals and an operator, so the range
 * that matters is one-sided and a signed type's is half an unsigned one's.
 */
void ExprResolver::checkLiteralRange(LocationId source, TypePtr type, U64 written, bool negative) {
    if(!type || global[type]->kind != Type::Int) return;

    auto& integer = *(IntType*)global[type];
    if(integerHolds(integer, written, negative)) return;

    auto reduced = reduceToWidth(integer, negative ? U64(0) - written : written);
    auto described = describeType(context, global, type);
    auto sign = negative ? "-"_v : ""_v;

    // Printed the way the type reads it, since the point of the message is what the program will do
    // and a signed truncation that comes out negative is exactly the surprising case.
    if(integer.isSigned) {
        context.diagnostics.warning("the literal %@%@ does not fit in %@ and is truncated to %@"_v,
                                    source, sign, written, described, I64(reduced));
    } else {
        context.diagnostics.warning("the literal %@%@ does not fit in %@ and is truncated to %@"_v,
                                    source, sign, written, described, reduced);
    }
}

ModulePtr<Value> ExprResolver::makeFloat(LocationId source, TypePtr type, F64 value) {
    if(type == module.scalar.float_) return constant<ConstFloat>(source, type, F32(value));
    return constant<ConstDouble>(source, type, value);
}

/*
 * Reading a module-level name.
 *
 * A `let &` global is storage, so reading one is a load of its place exactly as a mutable local's
 * is. A plain one is not. Nothing in the program can assign to it - resolvePlace reports on any
 * attempt - so its value is forever the constant declareGlobal recorded, and the read is that
 * constant rather than a load of the bytes it would have been emitted as. Nothing then reads the
 * storage at all, so the global is not marked used and is not emitted: an immutable global is a
 * name for a constant and occupies nothing, which is what `let regionSize = 4194304 :: I64` should
 * cost and what makes it worth writing in place of a function returning the same number.
 *
 * The fold itself is at the *place*, in `foldConstantRead` below, rather than here - because the
 * read this is asked about is not always the whole value. `origin.x` of a constant `Point` is a read
 * of a place with one projection on it, and it folds to `1` for exactly the reason a scalar global
 * does; asking here would have answered about `origin`, which is the one part of that expression
 * nothing wanted.
 */
ModulePtr<Value> ExprResolver::globalValue(ModulePtr<Global> global_, LocationId source) {
    if(!initializedGlobal(global_, source)) return nullptr;

    /*
     * A target question - `Target.byteOrder` - which is a constant whose value this stage does not
     * have. See `Global::targetMetric`: the read becomes the metric and the target folds it, which
     * is the same arrangement `sizeOf` on a concrete type is in.
     *
     * The global is never marked used, so nothing emits storage for it. That is not a special case
     * either: an immutable global is a name for a constant and occupies nothing, and this is one
     * whose constant is written down somewhere other than in the source.
     */
    auto& definition = *local[global_];
    if(definition.targetMetric) {
        return ref(emit<InstTypeMetric>(source, StringId(), definition.type, definition.type,
                                        definition.metric));
    }

    return load(Place::inGlobal(global_), source);
}

/*
 * Which part of a global's constant a place names, or null where the place does not name a part of
 * one at all.
 *
 * Two things stop the walk. A global **anything writes** - a `let &` the program assigns to, or a
 * `let` the entry sequence fills - has a constant that says what its storage *started* at and
 * nothing about what a read finds, which is one sentence for both and is the one `isWritten` states.
 * And a projection this cannot follow - a `Deref` through a box, a `Property` of a type the body
 * cannot see, an `Index` by something that is not a literal - answers null rather than guessing,
 * which leaves the read as the load it was.
 */
ModulePtr<ConstValue> ExprResolver::constantAt(const Place& place) {
    if(place.root != PlaceRoot::Global || !place.global) return nullptr;

    auto& definition = *local[place.global];
    if(definition.isWritten() || !definition.initial) return nullptr;

    auto at = definition.initial;
    auto path = place.projections;

    for(auto projection: path.contents(local)) {
        if(!at) return nullptr;

        auto value = *local[at];
        auto children = value.children.contents(local);

        /*
         * A string literal's node is its text *and* the aggregate its static form is, and a path
         * into one is a path into that aggregate: `computeString` gives a `String` its content's
         * size and alignment unchanged, so the two are one piece of storage at one offset. This is
         * the same step `ConstantWriter::write` takes for the same reason.
         *
         * Nothing underneath on JS, where a host string is one value - and there the walk stops,
         * since `children` is empty.
         */
        if(value.kind == ConstKind::String) {
            if(children.size() != 1) return nullptr;

            at = children[0];
            if(!at) return nullptr;

            value = *local[at];
            children = value.children.contents(local);
        }

        switch(projection.kind) {
            case ProjectionKind::Field: {
                // The fields of an aggregate, or of the content tuple a constructor carries - which
                // is why a `Construct` answers here as well as under its own `Downcast`.
                if(value.kind != ConstKind::Aggregate && value.kind != ConstKind::Construct) return nullptr;
                if(projection.index >= children.size()) return nullptr;

                at = children[projection.index];
                break;
            }
            case ProjectionKind::Index: {
                // Only a literal index. A computed one is a read of storage whichever element it
                // lands on, and this is not the pass that proves what a value is.
                if(value.kind != ConstKind::Aggregate || !projection.value) return nullptr;
                if(local[projection.value]->kind != Value::ConstInt) return nullptr;

                auto index = ((ConstInt*)local[projection.value])->value;
                if(index >= children.size()) return nullptr;

                at = children[Size(index)];
                break;
            }
            case ProjectionKind::Downcast: {
                // A downcast to the constructor this constant is not is unreachable code rather than
                // a value, so it is declined rather than answered.
                if(value.kind != ConstKind::Construct || projection.index != value.index) return nullptr;

                /*
                 * A tuple content is *this* node's children, so the step arrives where it started:
                 * the constructor's fields are what a `Field` after this one indexes, and a payload
                 * carried whole is the one case where the downcast reaches a value of its own. That
                 * is the same split `resolveConstruct` and `initializeFromConstant` make.
                 */
                auto record = global[value.type]->kind == Type::Record ? (RecordType*)global[value.type] : nullptr;
                if(!record || value.index >= record->constructors.size()) return nullptr;

                auto content = record->constructors.get(global, U16(value.index)).content;
                if(content && global[content]->kind == Type::Tup) break;

                if(children.size() != 1) return nullptr;
                at = children[0];
                break;
            }
            default:
                return nullptr;
        }
    }

    return at;
}

/*
 * The value a read of a constant place produces, or null where the read stays a load.
 *
 * **Only a direct-type leaf folds**, which is the same line the scalar case has always drawn and is
 * drawn here for a second reason as well. A memory-typed leaf is storage: producing it as a value
 * means *building* it, so a read of a whole constant record would construct one at every use, and a
 * read of a hundred-element constant array would write a hundred elements. The static form is
 * already exactly that value, and loading it is one read - so the aggregate keeps its storage, and
 * what folds is the scalar somewhere inside it that the expression actually asked for.
 */
ModulePtr<Value> ExprResolver::foldConstantRead(const Place& place, LocationId source) {
    auto constant = constantAt(place);
    if(!constant) return nullptr;

    auto& value = *local[constant];
    if(value.kind != ConstKind::Scalar || !isDirectType(global, value.type)) return nullptr;

    return constantBits(value.type, value.bits, source);
}

/*
 * Whether this global holds anything yet - the direct half of Analysis-Initialization.md §4.2.
 *
 * Asked only inside the entry sequence, which is the only body where the answer can be no: `main$`
 * is resolved before every other body, so by the time an ordinary function is looked at, every
 * global's initializer has run in the program and been resolved here.
 *
 * A global is in scope for the whole module from the first line, so a top-level statement naming one
 * declared further down is an ordinary use-before-init - the same mistake the ownership pass reports
 * for a local, reported here because a global has no state row in any frame to report it from.
 *
 * Two sequences reach this now, which is why the report names the order rather than the direction:
 * the program's start, where "further down" is the whole of it, and a test file's initializers,
 * where the global that has not run yet may be in another file entirely - see
 * resolveTestFileInitializers, whose pending list deliberately spans all of them.
 */
bool ExprResolver::initializedGlobal(ModulePtr<Global> global_, LocationId source) {
    if(!uninitialized || !uninitialized->containsValue(global_)) return true;

    context.diagnostics.error("%@ is read before its initializer runs - the program's start runs in written order and each test file's `let`s run after it in path order, and this global's initializer has not had its turn"_v,
                              source, context.findName(local[global_]->name));
    return false;
}

// The constant a declared-once value holds, from the bits its storage would have held at the width
// of its own type - the form both a global's initializer and a field default are recorded in.
ModulePtr<Value> ExprResolver::constantBits(TypePtr type, U64 bits, LocationId source) {
    if(isFloat(global, type)) return makeFloat(source, type, floatFromBits(global, type, bits));

    // The resolve IR has no pointer immediate on purpose, so a pointer constant is its address as
    // an integer reinterpreted - which is the same thing `null()` expands to.
    if(isPointer(global, type)) {
        auto address = makeInt(source, module.scalar.long_, bits);
        return ref(emit<InstUnary>(source, StringId(), type, Value::Cast, address));
    }

    return makeInt(source, type, bits);
}

/*
 * A constant, written into a place the caller already has.
 *
 * The whole of an aggregate constant's value form, and it is deliberately the same instructions the
 * construction the author could have written produces: a field is an `Init` of the field's place, a
 * sum writes its discriminant and then its payload through the `Downcast`, and a fixed array writes
 * one element per index. Nothing here reads a layout, which is what keeps a constant a *value* on
 * both targets rather than bytes that only one of them has.
 *
 * A string reaches `resolveString` instead of being walked, and that is the point of the text living
 * on the node: what a string literal is differs completely between the targets, and there is already
 * one place that knows it.
 */
void ExprResolver::initializeFromConstant(Place place, ModulePtr<ConstValue> constant, LocationId source) {
    // A unit field, which occupies nothing - the same silence `write` keeps for a unit place.
    if(!constant) return;

    auto& value = *local[constant];

    switch(value.kind) {
        case ConstKind::Scalar:
            initialize(place, constantBits(value.type, value.bits, source), source);
            return;
        case ConstKind::String:
            initialize(place, resolveString(source, value.text), source);
            return;
        case ConstKind::Address:
            initialize(place, constantValue(constant, source), source);
            return;
        case ConstKind::Aggregate: {
            auto children = value.children.contents(local);

            // A fixed array's components are elements, which are selected by a value rather than by
            // a field number - the same `Index` projection `buildAggregate` builds.
            if(global[value.type]->kind == Type::Array) {
                auto index = module.scalar.int_;
                for(Size i = 0; i < children.size(); i++) {
                    initializeFromConstant(project(place, ProjectionKind::Index, 0, makeInt(source, index, i)),
                                           children[i], source);
                }

                return;
            }

            for(Size i = 0; i < children.size(); i++) {
                initializeFromConstant(project(place, ProjectionKind::Field, U16(i)), children[i], source);
            }

            return;
        }
        case ConstKind::Construct: {
            auto record = global[value.type]->kind == Type::Record ? (RecordType*)global[value.type] : nullptr;

            // The discriminant, where the record has one. A `Single` record does not, and neither
            // does an enumeration - which never reaches here at all, being a scalar.
            if(record && record->layout == RecordType::Multi) {
                initialize(project(place, ProjectionKind::Discriminant, 0),
                           makeInt(source, module.scalar.int_, value.index), source);
            }

            auto payload = project(place, ProjectionKind::Downcast, U16(value.index));
            auto children = value.children.contents(local);

            // One child is a payload carried whole; several are the fields of a content tuple, and
            // the `Downcast` is what both of them are written through.
            if(children.size() == 1 && global[placeType(payload)]->kind != Type::Tup) {
                initializeFromConstant(payload, children[0], source);
                return;
            }

            for(Size i = 0; i < children.size(); i++) {
                initializeFromConstant(project(payload, ProjectionKind::Field, U16(i)), children[i], source);
            }

            return;
        }
    }
}

ModulePtr<Value> ExprResolver::constantValue(ModulePtr<ConstValue> constant, LocationId source) {
    if(!constant) return nullptr;

    auto& value = *local[constant];

    switch(value.kind) {
        case ConstKind::Scalar:
            return constantBits(value.type, value.bits, source);
        case ConstKind::String:
            return resolveString(source, value.text);
        case ConstKind::Address:
            // The address of a global, which only a native string's static form contains - and that
            // form is never walked from here, since a string is built through `resolveString`. Kept
            // whole anyway, because a node that had no value form would be a hole in this switch.
            return ref(emit<InstSymbol>(source, StringId(), value.type, nullptr, value.global));
        case ConstKind::Aggregate:
        case ConstKind::Construct:
            break;
    }

    // Storage, built the way a construction of the same shape would build it. Fresh storage per use
    // rather than a copy out of one static value, for the reason expr.h gives: two uses of one
    // constant are two values, and one of them may be written through.
    auto storage = allocate(value.type, source);
    if(auto place = findPlace(storage)) initializeFromConstant(place.unwrap(), constant, source);

    return storage;
}

// An integer-syntax literal can resolve to either kind of number, so a floating target takes it
// as a float constant rather than as an Int that is then converted. Any other concrete target is
// an ordinary FromInt instance - which is how a literal reaches a user type - and no target at
// all leaves a literal variable behind for the surrounding expression to decide.
/*
 * `negative` is the sign of a number that was *written* with one, carried in rather than folded in -
 * which is the same shape resolvePat uses for a negative pivot, and for the same reason: the
 * magnitude and the sign answer questions that the two's complement of the two together cannot.
 * `checkLiteralRange` is the question - `-128` is a number `I8` holds and `128` is not, and the bits
 * are identical - so the sign has to survive to the point the type is known, which is here.
 */
ModulePtr<Value> ExprResolver::resolveInteger(LocationId source, TypePtr target, U64 value, bool negative) {
    if(target && isFloat(global, target)) {
        auto number = F64(value);
        return makeFloat(source, target, negative ? -number : number);
    }

    if(target && isInteger(global, target)) {
        checkLiteralRange(source, target, value, negative);
        return makeInt(source, target, negative ? U64(0) - value : value);
    }

    // Past this point the sign can only be folded in: an unpinned literal is a `ConstInt` and a
    // `Long` is what `fromInt` is handed, both of which read the number as a signed 64-bit one.
    auto written = negative ? U64(0) - value : value;
    auto literal = constant<ConstInt>(source, literalVariable(module.coreClasses.fromInt), written);
    return target ? materializeLiteral(literal, target, source) : literal;
}

// Decimal syntax means FromDecimal, which no integer type has an instance of - that is what makes
// `1.5 :: Int` a missing instance rather than a lossy conversion. The parser keeps every decimal
// literal at F64 precision until a type is picked here.
ModulePtr<Value> ExprResolver::resolveDecimal(LocationId source, TypePtr target, F64 value, bool negative) {
    // On the number rather than on its bits, which is what makes `-0.0` a value this can produce -
    // resolvePat says the same thing about a pattern, and the two have to agree for `-0.0` to be
    // matchable against itself.
    auto number = negative ? -value : value;
    if(target && isFloat(global, target)) return makeFloat(source, target, number);

    auto literal = constant<ConstDouble>(source, literalVariable(module.coreClasses.fromDecimal), number);
    return target ? materializeLiteral(literal, target, source) : literal;
}

/*
 * A string literal - Implementation-String.md part 9, which is the one point a `String` is authored
 * rather than built up through the API.
 *
 * The two targets diverge completely here and share nothing but the decoded bytes, which is the
 * honest shape of "one logical value, two Repr-driven encodings":
 *
 *  - **JS**: the literal is a host string, and the only thing that produces one is a constant in the
 *    emitted source. One value kind, no storage, no descriptor - see ConstString.
 *  - **native**: the bytes go into the module's data as an ordinary global, and the value is the two
 *    words describing them. `runBorrowed` is what makes that free: the run does not own its slots,
 *    so a literal costs no teardown, and `resize` relocates a borrowed run by copying rather than
 *    refusing - which is copy-on-write, reached through Implementation-Containers.md §2's existing
 *    answers rather than a fourth one.
 *
 * The lexer has already decoded every escape and interned the result as UTF-8, so there is no
 * encoding work left here on either side. On native that is the target's native unit already; on JS
 * the emitter re-encodes it into a source literal and the host owns the UTF-16 from there.
 */
ModulePtr<Value> ExprResolver::resolveString(LocationId source, StringId text) {
    if(isJsMode(context.settings)) {
        return constant<ConstString>(source, module.scalar.string_, text);
    }

    auto content = context.findName(text);

    if(!module.program.stringLiteral) {
        context.diagnostics.error("internal: no string literal constructor for this target"_v, source);
        return nullptr;
    }

    /*
     * The literal as a *constant*, in a global of its own, duplicated bitwise into the value.
     *
     * The two words are already what a literal is, and writing them as storage rather than building
     * them costs a block copy instead of a call. What that buys is not the call: it is that the
     * `ownsHeap` deciding whether the teardown does anything is now a field of an immutable
     * constant, so `inheritConstant` puts a `0` in front of the test without `stringLiteral` having
     * to be inlined first. Over 118 call sites past `policy.manyCallSites`, it never was.
     *
     * A bitwise duplicate is sound here and nowhere else for a non-`TrivialCopy` type, and the
     * reason is the constant itself: the run is `runBorrowed`, so both names own nothing, the
     * teardown of either is a test that fails, and a write to either relocates by copying rather
     * than sharing. That is Implementation-Containers.md §2's copy-on-write, reached through the
     * answers it already had.
     */
    if(module.scalar.stringContent) {
        if(auto constant = nativeStringConstant(module, source, text, "a string literal"_v)) {
            StringBuilder literalName;
            literalName << context.findName(module.name) << ".literal$";
            literalName.appendValue(module.stringLiteralCount++);

            auto literal = module.addGlobal(builtName(context, literalName), source);
            literal->type = module.scalar.string_;
            literal->initial = constant;
            literal->used = true;
            literal->anonymous = true;

            auto duplicate = create<InstCopy>(source, StringId(), module.scalar.string_,
                                              Place::inGlobal(literal - *module.arena));
            append(duplicate);

            auto copied = ref(duplicate);
            duplicate->local = function.addLocal(module, module.scalar.string_, StringId(), copied);
            return copied;
        }
    }

    // The bytes, as a global of their own - see stringLiteralBytes, which is the one place a
    // literal's blob is made and the one place its name is decided.
    auto size = content.size();
    auto bytes = stringLiteralBytes(module, text, source);

    auto constructor = module.program.stringLiteral;
    auto local = *module.arena;
    local[constructor]->used = true;

    // `stringLiteral` takes a `%U8`, and what it is handed is the address of a blob - so the
    // pointee type comes from the callee's own signature rather than being built here. That keeps
    // this correct if the unit ever stops being a byte, which is what part 2's table leaves open.
    auto byteType = local[local[constructor]->args.get(local, 0)]->type;
    auto address = ref(emit<InstSymbol>(source, StringId(), byteType, nullptr, bytes - local));
    auto length = makeInt(source, module.scalar.int_, size);

    auto call = create<InstCall>(source, StringId(), module.scalar.string_, constructor);
    call->args.push(module.arena, address);
    call->args.push(module.arena, length);
    append(call);

    auto result = ref(call);

    /*
     * The slot, on the same terms as any other call of memory type - `emitCall` writes this line
     * and this one did not, which is the whole of how a literal came to be untracked.
     *
     * A value with no slot is invisible to the ownership passes: `backingLocal` answers with the
     * slot a value fills, `findPlace` with the place it occupies, and both of them answer nothing
     * for a value that occupies none. So `let c = "text"` bound a name to a droppable `String` that
     * `sinkValue` could not move out of and `checkMoves` had no state for, and `consume(c) +
     * consume(c)` passed the same storage to two consumers with no diagnostic. The literal's run is
     * borrowed, so what it released twice was nothing - but the shape is general, and the same
     * omission on a producer that allocates is a double free. `verifyLocals` now refuses an untracked
     * droppable value outright, which is what keeps the next producer from re-introducing it.
     */
    if(isMemoryType(global, module.scalar.string_)) {
        call->local = function.addLocal(module, module.scalar.string_, StringId(), result);
    }

    return result;
}

/*
 * `"a{x}b{y}c"` - Implementation-Storage.md part 8.
 *
 * The parser already produced the chunks; what happens here is the document's three steps, and the
 * design's whole trick is that they produce **one allocation whose extent is an ordinary value**
 * rather than three code paths:
 *
 *  1. the literal segments are known now, so their total `L` is a constant;
 *  2. each hole contributes `showBound(v)`, read through `formatBound` so that `Nothing` is zero;
 *  3. `newStringOfCapacity(L + Σ)` , then the literals and the holes appended in order.
 *
 * The three strategies are what the *existing* passes then make of that one allocation, which is why
 * none of them appears here:
 *
 *  **(a)** every bound is a constant `Just`, so the sum folds to a literal, the run's extent is a
 *  constant, and escape analysis puts a non-escaping format on the frame with no allocator call
 *  anywhere. This is the case the class's shape was designed for, and it needs the specializer to
 *  inline `showBound` and the folder to reduce what is left - both of which run.
 *
 *  **(b)** the sum is a runtime value. The allocation is the same instruction with a computed
 *  extent, and where it lives is `selectStorage`'s answer.
 *
 *  **(c)** some bound is `Nothing`. `formatBound` answers zero, so the seed covers the literals and
 *  the bounds that *are* known, and the appends grow past it through `reserveString`. A format that
 *  does not escape still starts on the frame and migrates only if it overflows.
 *
 * What is *not* here, and is part 8's own open question: the guarded `alloca`/heap pair strategy (b)
 * asks for, with the not-in-a-loop rule. `selectStorage` gives a computed extent the conservative
 * heap answer today - the same answer Implementation-Containers.md §12 records for every other
 * container - so (b) is correct and pays for the heap where it could sometimes have used the frame.
 * That is a placement decision shared with every container rather than something formatting can fix
 * on its own, which is why it is left where the rest of §12 is.
 */
ModulePtr<Value> ExprResolver::resolveFormat(const ast::Expr& expr, ModulePtr<Value> into) {
    auto& program = module.program;

    if(!program.newString || !program.pushString || !program.formatBound || !program.coreClasses.show ||
       (into && !program.reserveString)) {
        context.diagnostics.error("internal: string formatting is unavailable in this build"_v, expr.source);
        return nullptr;
    }

    struct Hole {
        ModulePtr<Value> value = nullptr;
        TypePtr type = nullptr;
        StringId text = StringId();
        bool hasText = false;
    };

    SmallArray<Hole, 8> holes;
    U64 literalUnits = 0;

    /*
     * Every hole resolved before anything is measured, and that ordering is the contract rather than
     * convenience: the arguments run left to right exactly once, and both `showBound` and `show` then
     * read the same value. Resolving a hole twice would run its expression twice.
     */
    auto chunks = expr.format;
    for(auto chunk: chunks.contents(parse)) {
        Hole hole;

        if(chunk.string) {
            hole.text = chunk.string;
            hole.hasText = true;
            literalUnits += context.findName(chunk.string).size();
        }

        if(chunk.format) {
            hole.value = resolve(*parse[chunk.format], nullptr, true);
            if(!hole.value) return nullptr;

            hole.value = settle(hole.value, expr.source);
            if(!hole.value) return nullptr;

            hole.type = valueType(hole.value);
        }

        holes.push(hole);
    }

    // Step 3's constant half. Runtime bounds are added to it below, and where there are none this is
    // the whole extent and folds straight into the allocation.
    auto total = makeInt(expr.source, module.scalar.size, literalUnits);

    for(auto& hole: holes) {
        if(!hole.value) continue;

        /*
         * `showBound`, reached the way an ordinary call to it would be - see emitClassMember, and
         * Design-Test.md §11.1's P1 for what asking for the implementation instead cost.
         *
         * The hole's type is this body's own variable whenever the format is written in a generic
         * function, which is where `Show` is *most* worth writing an interpolation in: the whole of
         * a constrained `show` instance is one of these, and the alternative spelling by hand is
         * both longer and one allocation per piece.
         */
        ResolvedArg measured[] = { ResolvedArg(hole.value) };

        auto missing = false;
        auto bound = emitClassMember(program.coreClasses.show, 1, hole.type, toBuffer(measured),
                                     expr.source, &missing);
        if(!bound) {
            if(missing) {
                context.diagnostics.error("cannot format a value of type %@ - it has no instance of `Show`, so there is nothing that says what its text is"_v,
                                          expr.source, describeType(context, global, hole.type));
            }

            return nullptr;
        }

        auto units = create<InstCall>(expr.source, StringId(), module.scalar.size, program.formatBound);
        units->args.push(module.arena, bound);
        append(units);
        (*module.arena)[program.formatBound]->used = true;

        total = ref(emit<InstBinary>(expr.source, StringId(), module.scalar.size, Value::Add, total, ref(units)));
    }

    /*
     * The sink's extent, which is the sum and nothing else - and what happens to it is the only
     * thing the two forms of a format differ in.
     *
     * Without a sink of somebody else's it is an allocation of exactly that size. With one it is a
     * *reservation* on the string that already exists, which is the same number spent on the same
     * buffer growth and skips both the allocation and the temporary that would have been copied out
     * of. See P2 in Design-Test.md §11.1.
     */
    auto sinkSignature = into ? program.reserveString : program.newString;
    auto sizeType = (*module.arena)[(*module.arena)[sinkSignature]->args.get(*module.arena, into ? 1 : 0)]->type;
    auto extent = convert(total, sizeType, expr.source);
    if(!extent) return nullptr;

    ModulePtr<Value> sinkValue = nullptr;

    /*
     * The borrow every append writes through.
     *
     * Taken once for a sink that was handed in, and once per append for one this expression made.
     * The difference is what the two are borrowing: an existing string is reached through whatever
     * property path the caller wrote, and `++=` exists to walk that path once - a `@host` field or a
     * packed one is a read and a write-back per borrow rather than an address. The allocation below
     * is a local of this frame, where a borrow is free and a fresh one per append keeps each loan's
     * extent to the call that uses it.
     */
    ModulePtr<Value> heldBorrow = nullptr;

    auto borrowSink = [&]() {
        if(!into) return borrowArgument(sinkValue, module.scalar.string_, expr.source, false);
        return heldBorrow;
    };

    if(into) {
        heldBorrow = borrowArgument(into, module.scalar.string_, expr.source, false);
        if(!heldBorrow) return nullptr;

        sinkValue = into;

        (*module.arena)[program.reserveString]->used = true;
        auto reserve = create<InstCall>(expr.source, StringId(), module.scalar.unit, program.reserveString);
        reserve->args.push(module.arena, heldBorrow);
        reserve->args.push(module.arena, extent);
        append(reserve);
    } else {
        (*module.arena)[program.newString]->used = true;
        auto sink = create<InstCall>(expr.source, StringId(), module.scalar.string_, program.newString);
        sink->args.push(module.arena, extent);
        append(sink);

        sink->local = function.addLocal(module, sink->type, StringId(), ref(sink));

        /*
         * The sink's own storage, which is exactly what `let &sink = newStringOfCapacity(n)` compiles
         * to and is written out here for the same reason that line would have been.
         *
         * Two things need it, and borrowing the call's result directly satisfies neither. The appends
         * take a `&`, and a borrow is writable only where the place it names is - a call result's
         * local is not declared mutable. And on JS a `&` of a non-object is the `{$o, $k, $s}` triple
         * (Implementation-Containers.md §14.1), which needs a *box* to point into: a host string is a
         * primitive, so `sink[$k] = ...` against a bare one throws rather than writing. A
         * `Ref`-convention allocation is what makes the backend produce that box, and it is why this
         * is an allocation and an initialization rather than one instruction fewer.
         *
         * The copy is a temporary's, so the optimizer removes it wherever it can adopt the storage -
         * the same path an array literal's run takes.
         */
        auto storage = allocate(module.scalar.string_, expr.source, StringId(), ast::BindType::Ref);
        if(!storage) return nullptr;

        initialize(placeFor(storage, expr.source), ref(sink), expr.source);
        sinkValue = storage;
    }

    /*
     * The hole first and the literal second, which is the order the parser records rather than the
     * order the two are written in.
     *
     * `parseStringExpr` opens with `{leading text, no expression}` and then pushes one chunk per
     * hole holding *that hole's expression and the text following it*. So a chunk is "this value,
     * then this text", and appending a chunk's text before its value renders `"n={7}!"` as `n=!7` -
     * which is a wrong string of the right length, so a fixture that checked only `length` would
     * have passed. `Format.yana` reads the units back for exactly this reason.
     */
    for(auto& hole: holes) {
        if(hole.value) {
            /*
             * `show`, at the same type and by the same route as the bound above.
             *
             * The sink is second here and first in `pushString` below, and that is not a detail:
             * `pushString(&self: String, other: String)` takes it first and `show(value: a, &to:
             * String)` takes it second, and pushing the two in one order for both produced a call
             * whose arguments were swapped. The types differ, so it was caught - by the lower IR
             * validator rather than the resolver, because a `&` argument is an address at that level
             * and both positions are addresses at this one.
             */
            auto borrowed = borrowSink();
            if(!borrowed) return nullptr;

            ResolvedArg written[] = { ResolvedArg(hole.value), ResolvedArg(borrowed) };

            auto missing = false;
            auto call = emitClassMember(program.coreClasses.show, 0, hole.type, toBuffer(written),
                                        expr.source, &missing);
            if(!call) {
                if(missing) {
                    context.diagnostics.error("cannot format a value of type %@ - it has no instance of `Show`, so there is nothing that says what its text is"_v,
                                              expr.source, describeType(context, global, hole.type));
                }

                return nullptr;
            }
        }

        if(hole.hasText) {
            auto literal = resolveString(expr.source, hole.text);
            if(!literal) return nullptr;

            auto borrowed = borrowSink();
            if(!borrowed) return nullptr;

            (*module.arena)[program.pushString]->used = true;
            auto push = create<InstCall>(expr.source, StringId(), module.scalar.unit, program.pushString);
            push->args.push(module.arena, borrowed);
            push->args.push(module.arena, literal);
            append(push);
        }
    }

    /*
     * A format written into somebody else's string is a *statement*: the string is theirs, the
     * appends have already happened, and there is nothing here to hand back. `++=` answers unit, so
     * this is that same answer with no call in front of it.
     */
    if(into) return allocate(module.scalar.unit, expr.source);

    /*
     * The finished string, read out of the storage it was built in.
     *
     * A *load* and not the allocation, and the difference is the whole of what a format expression
     * produces: the sink is storage this frame owns and the format's value is the string in it. On
     * JS that distinction is visible in the emitted source - the sink is a box, so handing the
     * allocation on passes `{$v: ...}` where every reader wants `.$v` - and natively it is the
     * difference between the address and the two words at it.
     */
    return load(placeFor(sinkValue, expr.source), expr.source);
}

ModulePtr<Value> ExprResolver::resolveLiteral(const ast::Expr& expr, TypePtr target) {
    switch(ast::Literal::Kind(expr.kind - ast::Expr::Lit)) {
        case ast::Literal::Int:
            return resolveInteger(expr.source, target, expr.lit.i());
        case ast::Literal::Float:
            return resolveDecimal(expr.source, target, F64(expr.lit.f));
        case ast::Literal::Double:
            return resolveDecimal(expr.source, target, expr.lit.d());
        case ast::Literal::String:
            return resolveString(expr.source, expr.lit.s);
        case ast::Literal::Bool:
            return makeInt(expr.source, module.scalar.bool_, expr.lit.b ? 1 : 0);
        default:
            context.diagnostics.error("literal is not available in the aggregate resolver"_v, expr.source);
            return nullptr;
    }
}

ModulePtr<Value> ExprResolver::resolve(const ast::Expr& expr, TypePtr target, bool used, bool implicit) {
    if(!current) return nullptr;
    if(ast::isLiteral(expr)) return resolveLiteral(expr, target);

    /*
     * The top of a chain containing a `?.`, which is where the skip those need has to be set up.
     *
     * Ahead of the switch because the extent of a `?.`'s skip is the rest of *its chain*, and a
     * chain is a spine of these four node kinds rather than one of them - `a?.b.c(x)` tops out at a
     * call and `a?.b` at the `?.` itself. Entering here is what lets everything below resolve as the
     * ordinary chain it is, with the `?.` nodes finding the join through `optionalChain`.
     *
     * `onOptionalSpine` is what stops this re-entering the chain it is already resolving, and what
     * makes a chain written inside one's *arguments* its own - see OptionalChain.
     */
    switch(expr.kind) {
        case ast::Expr::Field:
        case ast::Expr::Unwrap:
        case ast::Expr::App:
        case ast::Expr::Sub:
            if(!onOptionalSpine(expr) && chainSkips(expr)) {
                return resolveOptionalChain(expr, target, used, implicit);
            }

            break;
        default:
            break;
    }

    switch(expr.kind) {
        case ast::Expr::Error:
            sawParseError = true;
            return nullptr;
        case ast::Expr::Nested:
            return resolve(*parse[expr.nested], target, used, implicit);
        case ast::Expr::Multi: {
            ModulePtr<Value> result = nullptr;
            auto expressions = expr.multi;
            auto values = expressions.contents(parse);

            for(Size i = 0; i < values.size() && current; i++) {
                auto last = i + 1 == values.size();

                /*
                 * A lens call consumes the rest of this block, so it is the last thing the loop
                 * does whatever position it was written in - see expr_lens.cpp. The value it
                 * produces is the block's, because the statements after it are what produced it.
                 */
                ModulePtr<Value> lens = nullptr;
                if(resolveLensStatement(expressions, i, used, lens)) {
                    if(lens && target && current) lens = convert(lens, target, values[i].source, implicit);
                    return lens;
                }

                result = resolve(values[i], last ? target : nullptr, used && last, last && implicit);

                // Each element of a block is a statement of its own, which is the boundary a
                // literal variable that nothing decided has to be settled at.
                if(!last) result = settle(result, values[i].source);
            }

            return result;
        }
        case ast::Expr::Var: {
            /*
             * The cursor sentinel in ordinary value position - Implementation-Tooling.md §8.2.
             *
             * Everything completion needs is already here: the scope stack, and the type this
             * position was asked for. Ahead of the lookup because the sentinel names nothing, so
             * the lookup's only possible outcome is the "unknown scalar value" report below.
             */
            if(isCursorSentinel(context, expr.var)) {
                captureCompletion(*this, target, nullptr, false);
                return nullptr;
            }

            auto binding = findBinding(expr.var, expr.source);
            if(!binding) {
                if(auto found = findGlobal(module, expr.var, expr.source)) {
                    auto value = globalValue(found, expr.source);
                    return value && target ? convert(value, target, expr.source, implicit) : value;
                }

                // A function's name in value position is the function value that reaches it. This
                // is the last thing tried rather than the first, so a binding and a global still
                // shadow a declaration exactly as they did before function values existed.
                if(auto callee = findFunction(module, expr.var, expr.source)) {
                    auto value = functionValue(callee, expr.source);
                    return value && target ? convert(value, target, expr.source, implicit) : value;
                }

                /*
                 * A const parameter read as a value - Implementation-Const-Generics.md §1.6.
                 *
                 * `fn (n: Int) stride(v: Vec(Float, n)) -> Int = n * 4`. This needs no new
                 * expression form: the parameter has a *type*, so every expression rule already
                 * applies to it, and what is here is only the arm that finds it before the lookup
                 * gives up. That it lands after the binding, the global and the function is what
                 * keeps a local called `n` shadowing it exactly as one shadows anything else.
                 */
                if(auto value = constParameterValue(expr.var, expr.source)) {
                    return target ? convert(value, target, expr.source, implicit) : value;
                }

                context.diagnostics.error("unknown scalar value %@"_v, expr.source, context.findName(expr.var));
                return nullptr;
            }

            // Reading a `@lazy` parameter is what runs the argument the caller wrote, so this one
            // name is an effect rather than a value that was already there. Once, on any path -
            // checked over the whole body by checkLazyForcing below.
            if(binding->lazy) {
                Deferred deferred;
                deferred.thunk = binding->value;

                auto forced = force(deferred, nullptr, expr.source);
                return forced && target ? convert(forced, target, expr.source, implicit) : forced;
            }

            // A mutable binding names storage, so what its name produces is whatever is in that
            // storage now rather than what was put there when it was declared. The name stays on
            // the place, and each read of it is its own value.
            auto value = binding->isPlace() ? load(placeOf(*binding, expr.source), expr.source)
                                            : binding->value;

            return value && target ? convert(value, target, expr.source, implicit) : value;
        }
        case ast::Expr::Con:
            return resolveConstruct(expr, *parse[expr.con], target);
        case ast::Expr::App:
            return resolveCall(expr, *parse[expr.app], target, implicit);
        case ast::Expr::Infix:
            return resolveBinary(expr, *parse[expr.infix], target, implicit);
        case ast::Expr::Prefix:
            return resolvePrefix(expr, *parse[expr.prefix], target, implicit);
        case ast::Expr::If:
            return resolveIf(expr, *parse[expr.singleIf], target, used, implicit);
        case ast::Expr::MultiIf:
            return resolveMultiIf(expr, expr.multiIf, target, used, implicit);
        case ast::Expr::Is:
            return resolveIs(expr, *parse[expr.is], used);
        case ast::Expr::Try:
            return resolveTry(expr, target, used, implicit);
        case ast::Expr::Match:
            return resolveMatch(expr, *parse[expr.match], target, used, implicit);
        case ast::Expr::Decl:
            return resolveDecl(expr.decl, target, used);
        case ast::Expr::While:
            resolveWhile(*parse[expr.whileLoop]);
            return nullptr;
        case ast::Expr::For:
            resolveFor(expr, *parse[expr.forLoop]);
            return nullptr;
        case ast::Expr::Coerce: {
            auto& coerce = *parse[expr.coerce];

            /*
             * Resolved against this function's own context, so that an ascription inside a generic
             * body may name the variables that body is written over - `cast(p) :: %a` is how a
             * generic function says which of the two pointer types a reinterpretation produces.
             *
             * `[T *_]` is the one type whose count is not in the type at all: it is the number of
             * elements the literal on the other side of the `::` has, and `resolveType` is handed
             * the type and never the value. See inferredArrayType.
             */
            auto env = functionGen(global, function);
            auto type = coerce.type.kind == ast::Type::ArrInferred
                ? inferredArrayType(module, coerce.type, coerce.target, env)
                : resolveType(module, coerce.type, env);

            /*
             * The cursor sentinel takes the ascription as what this position asked for.
             *
             * Here rather than in the Var case below, because the fallback at the end of this
             * function deliberately does *not* push the type into a plain name - `x :: U8` converts
             * explicitly afterwards rather than resolving `x` against `U8`. An ascription on a name
             * that has not been written yet is the one thing saying what belongs there, so it is
             * exactly the type completion should rank by.
             */
            if(coerce.target.kind == ast::Expr::Var && isCursorSentinel(context, coerce.target.var)) {
                captureCompletion(*this, type, nullptr, false);
                return nullptr;
            }

            // `::` is what supplies the expected type where nothing else does, so it is pushed
            // down into a literal (which has no type of its own), into a call (whose class
            // instance may be decided by its result type - `truncate(x) :: Int`) and into a
            // constructor (whose record's type arguments may be - `Nothing :: Maybe(%U8)`, which
            // nothing else in the expression says). The call keeps its own result unconverted,
            // because the ascription that selected the instance is also the explicit conversion,
            // and an explicit one may narrow.
            if(ast::isLiteral(coerce.target)) {
                return convert(resolve(coerce.target, type), type, expr.source, false);
            }

            if(coerce.target.kind == ast::Expr::Con) {
                return resolveConstruct(coerce.target, *parse[coerce.target.con], type);
            }

            // An array literal, for the same reason - Implementation-Containers.md §8's "a literal
            // reaches `[T]` and `[T *n]` by ordinary context typing". Which of the two it builds is
            // decided by the expected type and by nothing else, so an ascription that arrived after
            // the fact would have built the wrong container and then found no conversion between
            // them - there is deliberately none, since fixed-owner to growable-owner allocates and
            // copies. The result is still converted, because `[1, 2] :: [Int]` in an argument
            // position may go on to become a slice.
            if(coerce.target.kind == ast::Expr::Array) {
                return convert(resolveArray(coerce.target, coerce.target.arr, type), type,
                               expr.source, false);
            }

            // A map literal, for the same reason and with one more of it: `[:]` says nothing at all
            // about what it holds, so an ascription that arrived after the fact would have had
            // nothing to build. See resolveMap.
            if(coerce.target.kind == ast::Expr::Map) {
                return convert(resolveMap(coerce.target, coerce.target.map, type), type,
                               expr.source, false);
            }

            // A lambda has no type of its own either: its argument types and its result are read
            // off the position it appears in, and `::` is what supplies one where nothing else
            // does. Through the parentheses, because `::` binds looser than the lambda arrow and
            // `((x) -> x * 3) :: (Int) -> Int` is how one is written.
            auto& ascribed = unwrapNested(coerce.target);
            if(ascribed.kind == ast::Expr::Fun) {
                return resolveFun(ascribed, *parse[ascribed.fun], type);
            }

            if(coerce.target.kind == ast::Expr::App) {
                auto value = resolveCall(coerce.target, *parse[coerce.target.app], type, false);
                return convert(value, type, expr.source, false);
            }

            if(coerce.target.kind == ast::Expr::Prefix) {
                auto value = resolvePrefix(coerce.target, *parse[coerce.target.prefix], type, false);
                return convert(value, type, expr.source, false);
            }

            /*
             * A form with no type of its own - a parenthesis, a block, the arms of an `if` or a
             * `match` - is a pass-through, so the ascription belongs to each leaf rather than to the
             * value they join. Without this the target stopped at the parenthesis: `(a `or` b) :: T`
             * resolved the operator chain against nothing, its literals defaulted to `Int`, and the
             * truncated result was converted afterwards.
             *
             * Pushed down as an *explicit* conversion, which is the whole reason `implicit` exists
             * as a parameter. `(x) :: U8` has to keep meaning what `x :: U8` means, and an implicit
             * conversion to a narrower type is an error about precision rather than a narrowing.
             *
             * The conversion below is then a no-op in the ordinary case and the fallback in the one
             * where a leaf produced something else - a branch whose arms unified to a common type
             * that still has to reach the ascribed one.
             */
            if(isPassThrough(coerce.target)) {
                return convert(resolve(coerce.target, type, true, false), type, expr.source, false);
            }

            return convert(resolve(coerce.target), type, expr.source, false);
        }
        case ast::Expr::Ret:
            resolveReturn(expr);
            return nullptr;
        case ast::Expr::Yield:
            return resolveYield(expr);
        case ast::Expr::Break:
        case ast::Expr::Continue: {
            /*
             * A `for` loop's body is lifted, so the loop these leave is not one this function has a
             * block for: it is the call in the enclosing frame, and leaving it is a value returned
             * to the iterator - Analysis-Lens.md §7.3's step signal. Which value depends on what
             * the rest of this body does, so the block is left open the way a `return` here is and
             * finished once that is known.
             *
             * Before the `inContinuation` case below, because a `for` body is both: what makes a
             * `break` here mean this loop rather than one further out is that a `for` *is* the
             * nearest enclosing loop of anything written in it.
             */
            if(loops.isEmpty() && inLoopBody) {
                if(expr.kind == ast::Expr::Break && expr.breakValue) {
                    context.diagnostics.error("a `for` loop does not produce a value in this version, so `break` cannot carry one - the loop's own value is what the iterator's result would have to hold, which is Analysis-Lens.md's V3"_v,
                                              expr.source);
                }

                loopExits.push(ContinuationLoopExit { current, expr.kind == ast::Expr::Break, expr.source });
                current = nullptr;
                return nullptr;
            }

            if(loops.isEmpty() && inContinuation) {
                // The loop is in the function this continuation was split out of, so leaving it is
                // the exit signal carrying a `break` rather than a `return` - Analysis-Lens.md
                // §5.1's "break/continue are the loop-shaped instance of one mechanism". A `for`
                // body is the case that mechanism now covers; a lens continuation is not, since the
                // lens between here and the loop has no step signal to report the departure in.
                context.diagnostics.error("`break` and `continue` cannot cross a lens call yet - the loop is in the function this block was split out of, and only `return` carries the exit signal past a lens"_v,
                                          expr.source);
                return nullptr;
            }

            if(loops.isEmpty()) {
                context.diagnostics.error(expr.kind == ast::Expr::Break ? "break outside a loop"_v : "continue outside a loop"_v, expr.source);
                return nullptr;
            }

            if(expr.kind == ast::Expr::Break && expr.breakValue) {
                context.diagnostics.error("scalar while loops do not produce values"_v, expr.source);
            }

            auto& loop = loops[loops.size() - 1];

            // A counted `for` has not built the block this leaves to yet - see LoopTarget. The
            // block is left open and resolveCountedFor comes back for it.
            if(auto deferred = expr.kind == ast::Expr::Break ? loop.deferredBreak : loop.deferredContinue) {
                deferred->push(current);
                current = nullptr;
                return nullptr;
            }

            auto targetBlock = expr.kind == ast::Expr::Break ? loop.breakBlock : loop.continueBlock;
            terminate(emit<InstJmp>(expr.source, StringId(), module.scalar.unit, targetBlock));

            return nullptr;
        }
        case ast::Expr::Array:
            return resolveArray(expr, expr.arr, target);
        case ast::Expr::Map:
            return resolveMap(expr, expr.map, target);
        case ast::Expr::Format:
            return resolveFormat(expr);
        case ast::Expr::Sub: {
            // A subscript read produces a borrow of the element, which the position it appears in
            // then reads through - so the caller writes `xs[0] + 1` and never names the borrow.
            auto borrowed = resolveSubscript(expr, *parse[expr.sub], false);
            if(!borrowed || !isBorrow(global, valueType(borrowed))) return borrowed;

            return convert(borrowed, ((BorrowType*)global[valueType(borrowed)])->to, expr.source);
        }
        case ast::Expr::Tup:
            return resolveTuple(expr, expr.tup, target);
        case ast::Expr::TupUpdate:
            return resolveTupUpdate(expr, *parse[expr.tupUpdate], target);
        case ast::Expr::Field:
            return resolveField(expr, *parse[expr.field]);
        case ast::Expr::Unwrap:
            return resolveUnwrap(expr);
        case ast::Expr::Assign:
            return resolveAssign(expr, *parse[expr.assign]);
        case ast::Expr::Fun:
            return resolveFun(expr, *parse[expr.fun], target);
        default:
            context.diagnostics.error("expression is not available in the aggregate resolver"_v, expr.source);
            return nullptr;
    }
}
