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

void ExprResolver::emitCheck(ModulePtr<Value> failed, LocationId source) {
    if(!failed || !checksEnabled()) return;

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

        auto& value = *local[at];
        auto children = value.children.contents(local);

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
 */
bool ExprResolver::initializedGlobal(ModulePtr<Global> global_, LocationId source) {
    if(!uninitialized || !uninitialized->containsValue(global_)) return true;

    context.diagnostics.error("%@ is read before its initializer runs - a module's top level runs in the order it is written, and this global is declared further down"_v,
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
ModulePtr<Value> ExprResolver::resolveInteger(LocationId source, TypePtr target, U64 value) {
    if(target && isFloat(global, target)) return makeFloat(source, target, F64(value));

    if(target && isInteger(global, target)) {
        checkLiteralRange(source, target, value);
        return makeInt(source, target, value);
    }

    auto literal = constant<ConstInt>(source, literalVariable(module.coreClasses.fromInt), value);
    return target ? materializeLiteral(literal, target, source) : literal;
}

// Decimal syntax means FromDecimal, which no integer type has an instance of - that is what makes
// `1.5 :: Int` a missing instance rather than a lossy conversion. The parser keeps every decimal
// literal at F64 precision until a type is picked here.
ModulePtr<Value> ExprResolver::resolveDecimal(LocationId source, TypePtr target, F64 value) {
    if(target && isFloat(global, target)) return makeFloat(source, target, value);

    auto literal = constant<ConstDouble>(source, literalVariable(module.coreClasses.fromDecimal), value);
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
    if(isJsMode(context.settings.mode)) {
        return constant<ConstString>(source, module.scalar.string_, text);
    }

    auto content = context.findName(text);

    if(!module.program.stringLiteral) {
        context.diagnostics.error("internal: no string literal constructor for this target"_v, source);
        return nullptr;
    }

    /*
     * The bytes, as a global of their own.
     *
     * Named per literal rather than interned by content. Two identical literals therefore get two
     * globals, which costs the bytes twice and is deliberately left alone: deduplicating them is a
     * size optimization over a table keyed on content, and doing it here would mean a name that
     * depends on the bytes - so a literal containing a quote or a newline would have to be escaped
     * into an identifier, which is a decision better made once, later, in one place.
     */
    /*
     * The bytes, as a global of their own, named by position rather than by content.
     *
     * The counter is what makes two literals two globals. Interning them by content instead would
     * save the bytes of a repeated literal, and is deliberately not done here: the name would then
     * have to be derived from the content, so a literal containing a quote or a newline would need
     * escaping into an identifier - a decision worth making once, later, in one place, rather than
     * as a side effect of emitting the first one.
     */
    StringBuilder name;
    name << "string$";
    name.appendValue(module.stringLiteralCount++);

    auto size = content.size();
    auto bytes = module.addGlobal(builtName(context, name), source);
    bytes->type = module.scalar.string_;
    bytes->literalBytes = ByteBuffer((Byte*)module.arena.alloc(size), size);
    copy((const Byte*)content.text(), bytes->literalBytes.ptr, size);
    bytes->used = true;

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

    return ref(call);
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
ModulePtr<Value> ExprResolver::resolveFormat(const ast::Expr& expr) {
    auto& program = module.program;

    if(!program.newString || !program.pushString || !program.formatBound || !program.coreClasses.show) {
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
    auto total = makeInt(expr.source, module.scalar.int_, literalUnits);

    for(auto& hole: holes) {
        if(!hole.value) continue;

        auto bound = instanceMember(module, program.coreClasses.show, hole.type, 1, expr.source);
        if(!bound) {
            context.diagnostics.error("cannot format a value of type %@ - it has no instance of `Show`, so there is nothing that says what its text is"_v,
                                      expr.source, describeType(context, global, hole.type));
            return nullptr;
        }

        auto measure = create<InstCall>(expr.source, StringId(), (*module.arena)[bound]->returnType, bound);
        measure->args.push(module.arena, hole.value);
        append(measure);

        auto units = create<InstCall>(expr.source, StringId(), module.scalar.int_, program.formatBound);
        units->args.push(module.arena, ref(measure));
        append(units);
        (*module.arena)[program.formatBound]->used = true;

        total = ref(emit<InstBinary>(expr.source, StringId(), module.scalar.int_, Value::Add, total, ref(units)));
    }

    // The sink. One allocation, whose extent is whatever the sum turned out to be - see above.
    auto sizeType = (*module.arena)[(*module.arena)[program.newString]->args.get(*module.arena, 0)]->type;
    auto extent = convert(total, sizeType, expr.source);
    if(!extent) return nullptr;

    (*module.arena)[program.newString]->used = true;
    auto sink = create<InstCall>(expr.source, StringId(), module.scalar.string_, program.newString);
    sink->args.push(module.arena, extent);
    append(sink);

    sink->local = function.addLocal(module, sink->type, StringId(), ref(sink));

    /*
     * The sink's own storage, which is exactly what `let &sink = newStringOfCapacity(n)` compiles to
     * and is written out here for the same reason that line would have been.
     *
     * Two things need it, and borrowing the call's result directly satisfies neither. The appends
     * take a `&`, and a borrow is writable only where the place it names is - a call result's local
     * is not declared mutable. And on JS a `&` of a non-object is the `{$o, $k, $s}` triple
     * (Implementation-Containers.md §14.1), which needs a *box* to point into: a host string is a
     * primitive, so `sink[$k] = ...` against a bare one throws rather than writing. A `Ref`-convention
     * allocation is what makes the backend produce that box, and it is why this is an allocation and
     * an initialization rather than one instruction fewer.
     *
     * The copy is a temporary's, so the optimizer removes it wherever it can adopt the storage - the
     * same path an array literal's run takes.
     */
    auto storage = allocate(module.scalar.string_, expr.source, StringId(), ast::BindType::Ref);
    if(!storage) return nullptr;

    initialize(placeFor(storage, expr.source), ref(sink), expr.source);
    auto sinkValue = storage;

    /*
     * Appending, in written order. A `&` argument is a borrow of the sink's own storage, which is
     * what lets every one of these write into the buffer that was just sized for them.
     *
     * `sinkFirst` is not a detail: `pushString(&self: String, other: String)` takes the sink first
     * and `show(value: a, &to: String)` takes it second, and pushing the two in one order for both
     * produced a call whose arguments were swapped. The types differ, so it was caught - by the lower
     * IR validator rather than the resolver, because a `&` argument is an address at that level and
     * both positions are addresses at this one.
     */
    auto appendTo = [&](ModulePtr<Function> callee, ModulePtr<Value> argument, bool sinkFirst) {
        auto borrowed = borrowArgument(sinkValue, module.scalar.string_, expr.source, false);
        if(!borrowed) return false;

        (*module.arena)[callee]->used = true;
        auto call = create<InstCall>(expr.source, StringId(), module.scalar.unit, callee);

        if(sinkFirst) {
            call->args.push(module.arena, borrowed);
            call->args.push(module.arena, argument);
        } else {
            call->args.push(module.arena, argument);
            call->args.push(module.arena, borrowed);
        }

        append(call);
        return true;
    };

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
            auto writer = instanceMember(module, program.coreClasses.show, hole.type, 0, expr.source);
            if(!writer) {
                context.diagnostics.error("cannot format a value of type %@ - it has no instance of `Show`, so there is nothing that says what its text is"_v,
                                          expr.source, describeType(context, global, hole.type));
                return nullptr;
            }

            if(!appendTo(writer, hole.value, false)) return nullptr;
        }

        if(hole.hasText) {
            auto literal = resolveString(expr.source, hole.text);
            if(!literal || !appendTo(program.pushString, literal, true)) return nullptr;
        }
    }

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

            // Resolved against this function's own context, so that an ascription inside a generic
            // body may name the variables that body is written over - `cast(p) :: %a` is how a
            // generic function says which of the two pointer types a reinterpretation produces.
            auto type = resolveType(module, coerce.type, functionGen(global, function));

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
