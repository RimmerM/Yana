#include "resolve_internal.h"
#include "name.h"
#include "print.h"

namespace mir {

// The argument is in resolve_internal.h, beside the declaration; the spelling is `role.h`'s.
bool isNegation(Context& context, ast::ParseBase parse, ast::Expr& expr) {
    if(expr.kind != ast::Expr::Prefix) return false;

    auto& prefix = *parse[expr.prefix];
    if(prefix.op.kind != ast::Expr::Var || prefix.on.kind < ast::Expr::Lit) return false;

    return stringView(context.findName(prefix.op.var)) == intrinsic::kNegationOperator;
}

/*
 * An owner handed to a parameter that only borrows, given storage of its own.
 *
 * The callee does not take the value, so this frame still owes its teardown - and a value that is
 * only a node result has no place for one to name. Writing it into a temporary is what makes the
 * drop after the call an ordinary drop of an ordinary local, which is the same rule a discarded
 * statement follows.
 *
 * One function for the two kinds of call, and it had to become one: `resolveRequiredCall` is
 * `resolveArguments` with the declaration replaced by a promise and it did not carry this step, so
 * `grow(grow(x))` typed against `grow: (a) -> a` left the inner call's result owned, borrowed and
 * never released. Nothing said so under specialization at a trivial type, which is every fixture
 * that had one; at a type with a teardown the SSA verifier reports an owner reaching `return`
 * unconsumed, which is where this was found.
 *
 * **`isTracked` and not `hasTeardown`**, which is what the test used to be.
 *
 * The two agree on every type with something to run and disagree on the one that is followed without
 * having any: a **function value** owes no teardown and may not be duplicated, because a closure can
 * hold an exclusive loan of a capture and copying the pair would be a second exclusive capability
 * over one object. So its uses are moves, it is `Convention::Affine` one tier down, and a node
 * result of one named by an inspecting use is what `Editor::use` refuses by name - `pick(1)(21)`,
 * where the callee is a call result nothing had given a place. An authored `Copy` type is the same
 * cell of the same table.
 *
 * `isTracked` is the question the tier below asks about exactly this, and asking it here is what
 * makes the two agree - see `conventionFor` in `resolve_ssa/convert.cpp`, where the trio is argued.
 */
Node* Resolver::holdBorrowed(Builder& builder, Node& value, Request wanted, DeclPtr<Type> expected,
                             LocationId source) {
    if(wanted.access == Access::Owned) return &value;
    if(!valueOccurrence(value).contract.isOwned()) return &value;
    if(!isTracked(decl(), expected)) return &value;

    auto place = hold(builder, value, source);
    return readPlace(builder, source, *place, wanted);
}

/*
 * A value of a type with a teardown, given storage of its own - see the declaration.
 *
 * An unnamed local, so that a dump shows the storage without claiming the author wrote a binding.
 * The write is an `Init`, which is what establishes the place: the initialization dataflow starts
 * every local uninitialized, so a temporary that skipped it would be an owner nothing could drop.
 *
 * The value's **whole occurrence**, and not an owned one of its type. A call that answers a loan
 * hands back somebody else's storage, and a temporary that claimed to own it would be consuming the
 * loan to fill itself and releasing the lender's object at the end of the statement. What makes the
 * temporary writable is then the loan's own capability: an exclusive loan may be written through
 * wherever it is held, and an owned temporary is a value no other name can see - there is nothing
 * for a write to it to mean, which is what `false` said before loans existed and still says.
 */
Place* Resolver::hold(Builder& builder, Node& value, LocationId source) {
    auto& occurrence = valueOccurrence(value);
    auto writable = occurrence.contract.access() == Access::Unique;
    auto local = builder.addLocal(StringId(), occurrence, source, writable);
    auto place = builder.localPlace(*local);

    builder.addWrite(source, *place, value, Destination::Init);
    return place;
}

/*
 * `@local` on a `data` - `doc/spec/modules.md`'s local declarations, at the `data` placement.
 *
 * The type is exported and its *inside* is not, so what is restricted is the two positions that
 * reach it: building a value and projecting a field. Everything else about the type stays public -
 * naming it, holding one, passing one to a function the module exports - which is what makes it an
 * abstract type rather than a hidden one, and what a `Handle` or an `Index` newtype needs to enforce
 * an invariant at all.
 *
 * **It cannot fire yet, and the reason is the stage rather than the rule.** `resolveProgram` builds
 * one module for the whole program and every name table is keyed by `(module, name)`, so a record
 * declared elsewhere cannot be named here in the first place - `resolveType` reports it as unknown
 * long before this. Written here anyway, on `@pinned`'s terms: the two positions that reach inside a
 * record are enumerated *now*, so the milestone that adds a second module changes a name table and
 * does not go looking for them.
 */
bool Resolver::reachesInside(RecordType& record, LocationId source, StringView what) {
    if(!record.local || record.module == here()) return true;

    context.diagnostics.error("%@ is `@%@`, so %@ may only be used in the module that declares it"_v,
                              source, context.findName(record.name),
                              attributeName(DeclAttribute::Local), what);
    return false;
}

/*
 * Whether a local is a `@lazy` parameter of the function being resolved - `Arg::lazy`.
 *
 * Read off the declaration rather than mirrored onto the `Local`, because the declaration is where
 * the marker was written and a copy on the storage would be a second answer to maintain. A lambda's
 * lifted body swaps `currentFunction`, so a capture of an enclosing frame's deferred parameter is
 * asked about against the lambda's own signature and is correctly not one of these - what a capture
 * holds is the thunk, and calling it is an ordinary indirect call.
 */
const Arg* Resolver::deferredParameter(Local& local) {
    if(!local.isParameter() || !currentFunction) return nullptr;
    if(local.parameter >= currentFunction->args.size()) return nullptr;

    auto& argument = *decl()[currentFunction->args.get(decl(), local.parameter)];
    return argument.lazy ? &argument : nullptr;
}

/*
 * The thunk a `@lazy` parameter holds, in hand.
 *
 * **This read is the force**, and it is the whole of what `checkForced` looks for. Nothing else
 * marks one, which is why handing the thunk on to another deferred position - a read that calls
 * nothing here - spends the same one permission that running it would have, and why the walk over
 * the finished body asks about a read rather than about a call.
 */
Node* Resolver::readDeferred(Builder& builder, Local& local, LocationId source) {
    return readPlace(builder, source, *builder.localPlace(local), inspectRequest());
}

// Reading a `@lazy` parameter *by name*, which is what runs the argument - `doc/spec/functions.md`.
Node* Resolver::forceDeferred(Builder& builder, Local& local, LocationId source) {
    auto thunk = readDeferred(builder, local, source);
    if(!thunk) return nullptr;

    return builder.addCallValue(source, *thunk, valueType(*thunk), nullptr, 0);
}

Place* Resolver::resolvePlace(Builder& builder, ast::Expr& expr, bool complain, Access access) {
    switch(expr.kind) {
        case ast::Expr::Nested:
            return resolvePlace(builder, *parse[expr.nested], complain, access);

        case ast::Expr::Var: {
            if(isCursorSentinel(context, expr.var)) {
                captureCompletion(nullptr);
                return nullptr;
            }

            auto bound = false;

            if(auto local = findLocal(expr.var, &bound)) {
                recordLocal(expr.source, *local);

                /*
                 * A `@lazy` parameter is **not** storage of the type it was written with: what lives
                 * there is the thunk, and the value only exists once the thunk has run. So the name
                 * has no place, and the value walk is what makes one where a projection needs it -
                 * `p.x` asks for a place with `complain` off, gets none, and falls through to
                 * `resolveValue` and `hold` exactly as `f().x` does.
                 */
                if(auto deferred = deferredParameter(*local)) {
                    if(complain) {
                        context.diagnostics.error(
                            "%@ is `@lazy`, so it is a value produced when it is read rather than storage - there is nothing here to write to or borrow"_v,
                            expr.source, context.findName(deferred->name));
                    }

                    return nullptr;
                }

                return builder.localPlace(*local);
            }

            // A name this body bound and whose binding failed - see `findLocal`. Silent, and it
            // stops here: the spelling is the local's, whatever else in scope carries it.
            if(bound) return nullptr;

            /*
             * A name a **lambda** says that its enclosing function bound - a capture.
             *
             * Between the locals and the globals, which is where the enclosing scope would be if
             * `findLocal` had not stopped at the lambda's floor: an enclosing local shadows a global
             * of the same name exactly as a local does, and putting the lookup anywhere else would
             * make that depend on whether the name was captured.
             */
            if(auto captured = findCapture(builder, expr.var)) {
                recordCapture(expr.source, expr.var);
                return captured;
            }

            if(rawLambdaCaptureRejected()) return nullptr;

            auto found = findGlobal(context, program, here(), expr.var, expr.source);
            if(found) {
                recordReference(context, expr.source, globalSymbol(program, found),
                                decl()[found]->occurrence.type);
            }

            if(!found) {
                // The one place an unknown name is reported, and the reason `complain` exists: the
                // target of a projection is asked whether it is a place *before* it is resolved as a
                // value, and a name that is neither has to be reported once rather than twice.
                //
                // `bound` is the third case and is silent for the same reason: the body *did* bind
                // this name and the binding is what failed, so it has been reported already - see
                // `Binding`.
                if(complain && !bound) {
                    context.diagnostics.error("unknown name %@"_v, expr.source,
                                              context.findName(expr.var));
                }

                return nullptr;
            }

            /*
             * A dynamic global whose declaring statement has not run yet - stage B's item 2.
             *
             * The name is in scope from the first line of the module, which is what makes a global a
             * global, so a read above its declaration resolves to something that exists and holds
             * nothing. Only the *entry sequence* can be in that position - every other body runs
             * after the whole of it - which is why the set is empty everywhere else and why the
             * question is asked here rather than by the initialization dataflow, which has no state
             * row for a global to have.
             */
            for(auto pending: uninitialized) {
                if(pending != found) continue;

                if(complain) {
                    context.diagnostics.error(
                        "%@ is read before its initializer runs - the program's start runs in written order, and this global's initializer has not had its turn"_v,
                        expr.source, context.findName(expr.var));
                }

                return nullptr;
            }

            // Its initializer was rejected, so it has no type. Every path that produces this has
            // already reported; what this stops is the second failure, which is a read with nothing
            // to read.
            if(!decl()[found]->occurrence.type) return nullptr;

            return builder.globalPlace(found);
        }

        // `xs[i]` - an element, which is a place. See `resolveSubscript`.
        case ast::Expr::Sub:
            return resolveSubscript(builder, expr, complain, access);

        /*
         * `*p` - the object a raw pointer names, which is a place and not an operation.
         *
         * The *role* decides and not the spelling: `pointerLoad` is what `lib/Native/Pointer.yana`
         * claims on its `*`, and a program that declares a unary `*` of its own is an ordinary call
         * that answers a value. So this is silent wherever the sole callee is anything else, and
         * `resolvePrefix` resolves it as the call it is.
         */
        case ast::Expr::Prefix: {
            auto& written = *parse[expr.prefix];
            if(written.op.kind != ast::Expr::Var) return nullptr;

            OverloadSet set;
            findOverloads(context, program, here(), written.op.var, 1, written.op.source, set);

            auto found = set.sole();
            if(!found || decl()[found]->intrinsic != intrinsic::Op::pointerLoad) return nullptr;

            auto address = resolveValue(builder, written.on, nullptr, inspectRequest());
            if(!address) return nullptr;

            return pointeeAt(builder, *address, written.on.source, complain);
        }

        case ast::Expr::Field: {
            auto& written = *parse[expr.field];
            if(written.target.kind == ast::Expr::Con && written.field.kind == ast::Expr::Var &&
               isCursorSentinel(context, written.field.var)) {
                // `X86.` is a namespace selection, not a value projection. The namespace's
                // declarations are ordinary qualified names in the module, so collect the visible
                // set and let the LSP layer filter it to the `X86` segment.
                captureCompletion(nullptr);
                context.completion->qualified = true;
                return nullptr;
            }

            auto target = projectionTarget(builder, parse[expr.field]->target, access);
            if(!target) return nullptr;

            return fieldStep(builder, *target, expr);
        }

        default:
            return nullptr;
    }
}

/*
 * One `.f` of a projection, over a target that is **already resolved**.
 *
 * Split out so that `resolveAssign` can apply the same steps to a place it built itself: a target
 * written `xs[i].f` has its accessor chosen after the right-hand side has been read, and the field
 * steps above it are then the ones this walk would have made. Two spellings of a projection is
 * exactly the drift this stage is written to avoid, so there is one.
 */
Place* Resolver::fieldStep(Builder& builder, Place& target, ast::Expr& expr) {
    auto& written = *parse[expr.field];

    /*
     * `p.value` on a `%Node` is `(*p).value` - the projection reaches **through** a pointer.
     *
     * Here and not in `projectionTarget`, which a dot-*call* shares: `p.f(y)` is `f(p, y)` with the
     * pointer itself as argument zero - `lib/Native`'s own operators are written that way - so the
     * step belongs to the projections rather than to every receiver.
     */
    auto through = pointeeThrough(builder, target, written.target.source);
    if(!through) return nullptr;

    /*
     * `t.0` - selection by **position**, which is the other half of the grammar and the one an
     * anonymous field has.
     *
     * The literal is read as a whole because a field position takes an integer and nothing else:
     * there is no expression here to evaluate, and a spelling that is neither a name nor a number
     * is a parse the grammar admits and the language has no reading for.
     */
    if(written.field.kind == ast::Expr::Kind(ast::Expr::Lit + ast::Literal::Int)) {
        return positionalField(builder, *through, written.field);
    }

    return fieldOf(builder, *through, written.field, written.target.source);
}

/*
 * The type a run of named field steps selects, without building the places.
 *
 * The one caller is `resolveAssign`, and what it needs the answer for is a **hint**: an assignment
 * to `xs[i].f` reads its right-hand side before it takes the exclusive loan the target needs, and a
 * value resolved against no expected type at all is a different program (`xs[i].f = []` has nowhere
 * to get an element type from). The place is still built afterwards and is still what the write is
 * checked against, so this walk decides an order and never a meaning.
 *
 * Null wherever a step is anything but a named field of a single-constructor record read at its own
 * type - a positional field, a pointee, a view whose fields are its record's, a type variable's
 * requirement. Each of those is a step whose place walk emits or renames something, and the caller
 * simply builds the target first, which is what every assignment did before this.
 */
DeclPtr<Type> Resolver::fieldPathType(DeclPtr<Type> from, ast::Expr* const* steps, Size count) {
    for(Size i = count; i > 0; i--) {
        if(!from || decl()[from]->kind != Type::Record) return nullptr;

        auto& written = *parse[steps[i - 1]->field];
        if(written.field.kind != ast::Expr::Var) return nullptr;

        DeclPtr<Type> recordType = nullptr;
        auto index = fieldNamed(owned(from), written.field.var, recordType);

        if(!recordType || index == kNoField || recordType != from) return nullptr;

        from = ((RecordType*)decl()[recordType])->field(decl(), index).occurrence.type;
    }

    return from;
}

/*
 * Which field of an occurrence a name selects - **the one walk over a record's fields**.
 *
 * It reports nothing, and that is what makes it one walk rather than two. `fieldOf` turns a no into
 * a message and `resolveDotCall` turns the same no into a search for a function of the name, so a
 * predicate written for the second question would be a second reading of a record - and what it
 * decides is which of the two forms `x.f(y)` is, which is the last place two readings may drift.
 *
 * `record` comes back as the record the fields were read off, which is the **occurrence's** and not
 * the type's - `fieldRecordOf`, where a window's fields are decided to be its view's. Null there
 * means the occurrence has no fields at all; a record with several constructors answers `kNoField`
 * with `record` set, which is what lets the caller tell "reached by matching" from "no such field".
 */
U16 Resolver::fieldNamed(const Occurrence& occurrence, StringId name, DeclPtr<Type>& record) {
    record = fieldRecordOf(context, program, occurrence);
    if(!record) return kNoField;

    auto& declared = *(RecordType*)decl()[record];
    if(declared.layout != RecordLayout::Single) return kNoField;

    for(auto field: declared.fields.contents(decl())) {
        if(field.name == name) return field.index;
    }

    return kNoField;
}

/*
 * Whether the receiver's own type answers this name at all - a declared field, or one a signature
 * requires of a type variable.
 *
 * The **probe** half of the projection, and the only thing `resolveDotCall` needs to choose between
 * the two forms. A generic receiver is asked through `findProperty` rather than through the
 * requiring form for the reason the projection gives: a declared `a.name: T` selects the field and
 * its absence selects the function, and asking the form that reports would name a missing constraint
 * before the function half had been tried.
 */
bool Resolver::hasMemberNamed(const Occurrence& occurrence, StringId name) {
    if(!occurrence.type) return false;

    if(decl()[occurrence.type]->kind == Type::Variable) {
        return findProperty(occurrence.type, name) != kNoProperty;
    }

    DeclPtr<Type> record = nullptr;
    return fieldNamed(occurrence, name, record) != kNoField;
}

/*
 * `target.name` as a place, over a target that is **already resolved**.
 *
 * Split out of `resolvePlace` so that a dot-call reaches it with the receiver it has already got:
 * resolving is emission, so a form that decided between a field and a function by resolving the
 * receiver twice would leave the first walk's instructions behind in a stage that runs no pass to
 * remove them. The two callers hand it the same target and get the same place.
 */
Place* Resolver::fieldOf(Builder& builder, Place& target, ast::Expr& selector,
                         LocationId targetSource) {
    if(selector.kind != ast::Expr::Var) {
        unsupported(selector.source, "a field selected by anything but a name"_v);
        return nullptr;
    }

    return fieldNamedOf(builder, target, selector.var, selector.source, targetSource);
}

/*
 * The object a raw address names, as a place - `Place::Address`, and the one root outside the safe
 * model.
 *
 * **Always writable**, which is `Builder::addressPlace`'s own sentence said at the source level: the
 * memory a pointer names is mutable whatever binding the pointer itself was read out of, so
 * `n.value = 5` through a plain `let n = addressOf(node)` is a write and `n = m` on the same binding
 * is still refused. What a raw store does *not* do is release what was there - a store through an
 * address is `Destination::Assign` over a root with no move path, which `expandPointerStore` says at
 * length and is Native's to be unsafe about.
 *
 * The pointee's occurrence is `owned`, which is the same occurrence `pointerLoad`'s declared result
 * substitutes to: `fn *(it: %a) -> a` answers an owned `a`, and reading the place through a request
 * is what decides whether that is a copy or a move.
 */
Place* Resolver::pointeeAt(Builder& builder, Node& address, LocationId source, bool complain) {
    auto type = valueType(address);

    if(!type || decl()[type]->kind != Type::Pointer) {
        if(complain) {
            context.diagnostics.error("`*` names the object a pointer holds, and %@ is not one"_v,
                                      source, typeText(context, decl(), type));
        }

        return nullptr;
    }

    return builder.addressPlace(address, owned(((PointerType*)decl()[type])->to), true);
}

/*
 * A place that holds a pointer, read as the place its pointee is.
 *
 * `target` itself wherever it holds anything else, so the caller writes one line rather than a test:
 * a projection through a pointer is a projection with one extra step, and the step is either there
 * or it is not.
 */
Place* Resolver::pointeeThrough(Builder& builder, Place& target, LocationId source) {
    if(!target.occurrence.type || decl()[target.occurrence.type]->kind != Type::Pointer) {
        return &target;
    }

    auto address = readPlace(builder, source, target, inspectRequest());
    if(!address) return nullptr;

    return pointeeAt(builder, *address, source, true);
}

Place* Resolver::fieldNamedOf(Builder& builder, Place& target, StringId name,
                              LocationId nameSource, LocationId targetSource) {
    // The cursor after a `.` - §8.1's third kind, where both a read and an assignment reach a field.
    if(isCursorSentinel(context, name)) {
        captureMemberCompletion(target.occurrence);
        return nullptr;
    }

    /*
     * A field of a value whose type is a **variable** - `fn (a.size: I32) f(x: a)`.
     *
     * The signature is what says the field is there, so the requirement is looked up before the type
     * is reported as having no fields: an unconstrained `a` genuinely has none, and the difference
     * between the two is exactly what the constraint states.
     */
    if(decl()[target.occurrence.type]->kind == Type::Variable) {
        auto found = findProperty(target.occurrence.type, name);
        if(found == kNoProperty) {
            context.diagnostics.error(
                "%@ has no field called %@, and the signature does not require one"_v,
                nameSource, typeText(context, decl(), target.occurrence.type),
                context.findName(name));
            return nullptr;
        }

        auto property = decl()[currentFunction->gen]->properties.get(decl(), found);
        return builder.propertyPlace(target, U16(found), owned(property.result));
    }

    DeclPtr<Type> recordType = nullptr;
    auto index = fieldNamed(target.occurrence, name, recordType);

    if(!recordType) {
        context.diagnostics.error("%@ has no fields"_v, targetSource,
                                  typeText(context, decl(), target.occurrence.type));
        return nullptr;
    }

    auto& record = *(RecordType*)decl()[recordType];

    /*
     * A field of a **sum** is not reachable by name - Analysis-IR.md's G7d.
     *
     * Which fields a value has is what its discriminant says, and a selector says nothing about it:
     * `o.value` on an `Outcome` that turned out to be an `Exit` would read a type out of bytes
     * holding another one. What proves the arm is a pattern, so a pattern is the only thing that
     * projects one - which is also what lets the loan checker treat two arms' fields as one storage
     * and the drop pass leave them to the tag.
     */
    if(record.layout != RecordLayout::Single) {
        context.diagnostics.error(
            "%@ has several constructors, so %@ is reached by matching rather than by name"_v,
            nameSource, typeText(context, decl(), target.occurrence.type),
            context.findName(name));
        return nullptr;
    }

    if(index != kNoField) {
        recordReference(context, nameSource, fieldSymbol(decl(), recordType, index),
                        record.field(decl(), index).occurrence.type);

        if(!reachesInside(record, nameSource, "its fields"_v)) return nullptr;
        return builder.fieldPlace(target, index);
    }

    if(recordType != target.occurrence.type) {
        context.diagnostics.error(
            "a %@ view of %@ has no field called %@ - a view's fields are the ones its view record declares, not the owner's"_v,
            nameSource, accessName(target.occurrence.contract.access()),
            typeText(context, decl(), target.occurrence.type), context.findName(name));
        return nullptr;
    }

    context.diagnostics.error("%@ has no field called %@"_v, nameSource,
                              typeText(context, decl(), target.occurrence.type),
                              context.findName(name));
    return nullptr;
}

/*
 * `t.0` - field selection by position.
 *
 * **Anonymous fields only.** A named field is reached by its name and by nothing else: an index
 * beside a name would be a second spelling of the same slot that stops naming the same slot the
 * moment a field is inserted or moved, and the declaration that moved it says nothing about the call
 * sites that counted. What is left is exactly the fields with no other spelling, which today is a
 * positional tuple's - and, once a constructor written `Con(T)` is read here, the one unnamed field
 * it carries.
 *
 * Everything else about it is the ordinary field projection: the same single-constructor rule, the
 * same visibility check and the same `fieldPlace`. That is the point - a position is a way of naming
 * a field, not a way of reaching one.
 */
Place* Resolver::positionalField(Builder& builder, Place& target, ast::Expr& written) {
    auto type = target.occurrence.type;
    auto index = written.lit.i();

    /*
     * A receiver whose type is a variable has names and no positions.
     *
     * A field constraint *names* the field it promises, so nothing could have promised `a.0`: a
     * requirement numbered rather than named would be a promise no constraint can be written to
     * keep, since the numbering belongs to a declaration the signature has not seen.
     */
    if(decl()[type]->kind == Type::Variable) {
        context.diagnostics.error(
            "a value of a type parameter has no positional fields - a field constraint names the field it promises, so select it by that name"_v,
            written.source);
        return nullptr;
    }

    if(decl()[type]->kind != Type::Record) {
        context.diagnostics.error("%@ has no fields"_v, written.source,
                                  typeText(context, decl(), type));
        return nullptr;
    }

    auto record = (RecordType*)decl()[type];

    // The same rule a named selection is under, and G7d's reason for it: which fields a value has is
    // what its discriminant says, and a selector says nothing about it.
    if(record->layout != RecordLayout::Single) {
        context.diagnostics.error(
            "%@ has several constructors, so a field of it is reached by matching rather than by position"_v,
            written.source, typeText(context, decl(), type));
        return nullptr;
    }

    if(index >= record->fields.size()) {
        context.diagnostics.error("field index %@ is out of range - this value has %@ fields"_v,
                                  written.source, index, U64(record->fields.size()));
        return nullptr;
    }

    auto field = record->field(decl(), U16(index));

    // A field the declaration *named* - `RecordField::anonymous`, which is what tells a tuple's
    // fields and a one-payload constructor's from one somebody wrote a name for.
    if(!field.anonymous && field.name != StringId()) {
        context.diagnostics.error(
            "field %@ is named `%@`, so it is selected by that name - an index reaches an anonymous field only, since a named field moves when the declaration changes and its position does not say so"_v,
            written.source, index, context.findName(field.name));
        return nullptr;
    }

    if(!reachesInside(*record, written.source, "its fields"_v)) return nullptr;

    return builder.fieldPlace(target, field.index);
}

// A field access, as the read of the place it projects. The mode is `readMode`'s answer for the
// position, which is the whole of what makes `p.buffer` in a borrow position a loan and the same
// text in a sink position a move.
/*
 * `grow(x)` typed against `grow: (a) -> a`.
 *
 * The requirement is the signature, so the walk is `resolveArguments`' with the declaration replaced
 * by a promise: one value per declared position, each resolved against the type the requirement
 * stated and checked against it. What it does *not* do is bind type arguments - the requirement is
 * written in this signature's own variables and has none of its own - which is why there is no
 * `TypeArguments` here and why the callee is left to `callTarget`.
 */
Node* Resolver::resolveRequiredCall(Builder& builder, ast::AppExpr& written, U16 requirement,
                                    LocationId source) {
    auto declared = decl()[currentFunction->gen]->callables.get(decl(), requirement);

    /*
     * The declaration the name reaches, recorded so that every reader wanting "which function is
     * this about" keeps working - see `NodeCall::genCallable`. Absent is not an error here: what
     * has to exist is a `grow` at each *instantiation*, which `dischargeCallable` asks at the call
     * that supplies one.
     */
    auto found = findFunction(context, program, here(), declared.name, U32(declared.args.size()), source);

    SmallArray<Node*, 8> arguments;
    HeldList<SmallArray<Node*, 8>> held(*this, arguments);

    Size index = 0;
    for(auto argument: written.args.contents(parse)) {
        if(argument.name) {
            unsupported(argument.value.source, "a named argument"_v);
            return nullptr;
        }

        auto expected = declared.args.get(decl(), U32(index++));
        auto value = resolveValue(builder, argument.value, expected,
                                  bindingRequest(Access::Shared));
        if(!value) return nullptr;

        value = coerce(builder, *value, expected, argument.value.source, "an argument"_v);
        if(!value) return nullptr;

        // The same temporary an ordinary call's borrowing argument gets - see `holdBorrowed`. A
        // requirement writes no conventions, so every position here borrows and this is the one that
        // needed it: `grow(grow(x))` hands the inner call's owner to a borrowing position, and
        // without a place for it the frame that owes its teardown has nothing to name.
        arguments.push(holdBorrowed(builder, *value, bindingRequest(Access::Shared), expected,
                                    argument.value.source));
    }

    return builder.addRequiredCall(source, found, requirement, declared.result,
                                   arguments.pointer(), arguments.size());
}

U32 Resolver::findCallable(StringId name, U32 arity) {
    if(!currentFunction || !currentFunction->gen) return kNoProperty;

    auto& declared = *decl()[currentFunction->gen];
    for(U32 i = 0; i < declared.callables.size(); i++) {
        auto requirement = declared.callables.get(decl(), i);
        if(requirement.name == name && requirement.args.size() == arity) return i;
    }

    return kNoProperty;
}

U32 Resolver::findProperty(DeclPtr<Type> subject, StringId field) {
    if(!currentFunction || !currentFunction->gen) return kNoProperty;

    auto& declared = *decl()[currentFunction->gen];
    for(U32 i = 0; i < declared.properties.size(); i++) {
        auto requirement = declared.properties.get(decl(), i);
        if(requirement.subject == subject && requirement.field == field) return i;
    }

    return kNoProperty;
}

/*
 * The target of a projection - `p.x`, `xs[i]` - resolved as a place, or held as one.
 *
 * A place where there is one, so that `p.x` on a local is the local's own storage; a temporary where
 * there is not, so that `f().x` reads the field of a value the call produced. `hold` gives the value
 * a frame slot and answers its place, which is the same slot a borrowing argument already gets.
 */
Place* Resolver::projectionTarget(Builder& builder, ast::Expr& target, Access access) {
    ErrorMark mark(context);
    if(auto place = resolvePlace(builder, target, false, access)) return place;

    // Already reported: the target is a place and something about it is wrong, and a second message
    // naming the projection would be describing the consequence rather than the cause.
    if(mark.reported()) return nullptr;

    auto value = resolveValue(builder, target, nullptr, bindingRequest(Access::Shared));
    if(!value) return nullptr;

    return hold(builder, *value, target.source);
}

/*
 * `xs[i]` - one element, as a place.
 *
 * Two kinds of container and two answers. A **fixed array**'s element is a place of the compiler's:
 * `Place::Index` projects the array's own storage, the position decides what may be done through it
 * exactly as it does for a field, and the overlap test that refuses `f(&xs[0], &xs[0])` applies with
 * nothing added. Everything else - a window, a growable array, a string, a container of the
 * program's own - subscripts through the `Index` class, which is `doc/spec/collections.md`'s rule
 * and where the bounds check is: `get` where the position reads and `getMut` where it writes, each
 * an ordinary call whose result is a borrow of the element, held in a local so that it is a place.
 *
 * The compiler reads no field of a view here. What a window's base is and how far it reaches are
 * the instance's business, which is Analysis-Manifestations.md Part 8's rule for every operation on
 * a representation record.
 */
Place* Resolver::resolveSubscript(Builder& builder, ast::Expr& expr, bool complain, Access access) {
    Subscript subscript;
    if(!resolveSubscriptTarget(builder, expr, complain, access, subscript)) return nullptr;
    return elementAccess(builder, subscript, access, expr.source);
}

bool Resolver::resolveSubscriptTarget(Builder& builder, ast::Expr& expr, bool complain,
                                      Access access, Subscript& into) {
    auto& written = *parse[expr.sub];

    auto target = projectionTarget(builder, written.callee, access);
    if(!target) return false;

    if(written.args.size() != 1) {
        context.diagnostics.error("a subscript takes one index, and this writes %@"_v, expr.source,
                                  U64(written.args.size()));
        return false;
    }

    auto argument = written.args.get(parse, 0);
    if(argument.name) {
        context.diagnostics.error("a subscript's index has no name to be given"_v, expr.source);
        return false;
    }

    into.container = target;
    into.fixed = decl()[target->occurrence.type]->kind == Type::Array;
    into.element = elementTypeOf(decl(), program, target->occurrence.type);

    if(!into.fixed && !program.roles.klass(intrinsic::Known::index)) {
        if(complain) {
            context.diagnostics.error(
                "%@ is not a fixed array, and subscripting anything else needs the Index class"_v,
                written.callee.source, typeText(context, decl(), target->occurrence.type));
        }

        return false;
    }

    /*
     * The index, at the key type the container's own `Index` instance names.
     *
     * A **fixed array** has no instance and keys at the word width - `Size`, which `primitive.def`
     * argues is signed precisely because it is "the type an `Int` index widens *into*". The hint is
     * what makes `xs[n]` at an `Int` compile: every `I32` is a `Size` on every target, so `Widen`
     * relates them and the widening is the one the language performs at every other position.
     *
     * Everything else is asked, because the widening only runs one way. `m[900]` on a
     * `Map(Int, Int)` built the literal at `Size` and then had to narrow it into the `k` the
     * instance declares, which is the conversion the language refuses outright - sixteen sites in
     * the library's own suite. The class's functional dependency is what answers it: `Index(c -> k,
     * v)` at a known `c` decides both, so `selectInstance` with the two positions left open reads
     * `k` off the head that matched. A container whose instance cannot be found here - a generic
     * body's own variable - falls back to the word width and is coerced at the call as before.
     */
    auto key = program.types.words[1];

    if(!into.fixed) {
        DeclPtr<Type> query[3] = { target->occurrence.type, nullptr, nullptr };
        InstanceMatch found;

        if(selectInstance(context, program, program.roles.klass(intrinsic::Known::index), query, 3,
                          expr.source, false, found) &&
           found.arguments.size() == 3 && found.arguments[1]) {
            key = found.arguments[1];
        }
    }

    auto index = resolveValue(builder, argument.value, key, inspectRequest());
    if(!index) return false;

    if(into.fixed) {
        index = coerce(builder, *index, program.types.words[1], argument.value.source, "an index"_v);
        if(!index) return false;
    }

    into.index = index;
    return true;
}

/*
 * `m[k] = v` where the container declares `IndexInsert` - Implementation-Map.md §7's second
 * decision, and the one place an assignment is a call rather than a write through a place.
 *
 * True when this rule took the statement over, whether it emitted or reported; false when the
 * container has no such instance, which is `Array` and every fixed array and leaves the assignment
 * exactly where it was. The test is the *instance*, not the class: a library may declare
 * `IndexInsert` and a container may still be one that only `getMut` serves.
 */
bool Resolver::resolveIndexInsert(Builder& builder, const Subscript& subscript, Node& value,
                                  LocationId source) {
    auto klass = program.roles.klass(intrinsic::Known::indexInsert);
    if(!klass) return false;

    DeclPtr<Type> query[3] = { subscript.container->occurrence.type, nullptr, nullptr };
    InstanceMatch found;

    if(!selectInstance(context, program, klass, query, 3, source, false, found)) return false;

    auto self = readPlace(builder, source, *subscript.container, bindingRequest(Access::Unique));
    if(!self) return true;

    Node* operands[3] = { self, subscript.index, &value };
    applyMember(builder, intrinsic::Known::indexInsert, 0, operands, 3, source);
    return true;
}

/*
 * `xs[i]` on a `[T *n]` stops the program where `i` is not an element - `doc/spec/collections.md`.
 *
 * The only bounds test the *compiler* emits. Every other container is subscripted through a library
 * `Index` body, and the test is written there beside the length it reads; a fixed array has no such
 * body, because its element is a projection of the container's own place and there is nothing for a
 * call to hand back. So the one place a `[T *n]` element place is projected from a written index is
 * the one place this has to be built, and it goes through the same `checkFailed` role the library
 * reaches for - which is what lets `-no-checks` remove it with everything else (`opt/check.cpp`
 * recognizes a block that is one call to the role and a way out).
 *
 * **Unsigned**, exactly as `lib/Core/Array.yana`'s `Index` body writes it: an index is a signed
 * `Size`, and one comparison against the length catches a negative index as well, because a negative
 * number read unsigned is larger than any length.
 *
 * **A call to `checkCondition` and not the branch**, which is the shipping stage's own indirection
 * arrived at from the other end - `known.def`'s row says why it now has one. A subscript is resolved
 * inside whatever expression contains it, and this IR hands every value that crosses a block
 * boundary over as a block argument: a branch emitted here would have to thread everything the
 * enclosing expression has already computed through two new blocks, under a construct that has
 * already decided which blocks it owns. The branch inside the callee is the same test where nothing
 * is looking, and the inliner removes the call in an optimized build.
 *
 * **Skipped for a constant index that is in range**, which is why `[0] :: [U8 *64]` and every
 * `xs[0]` in the corpus cost nothing and change no golden. A constant that is *out* of range still
 * gets the test, and the optimizer folds it to the unconditional stop it is; refusing it at
 * compile time is a diagnostic this stage does not owe and the shipping stage does not make.
 */
void Resolver::checkFixedBound(Builder& builder, const Subscript& subscript, LocationId source) {
    auto array = (ArrayType*)decl()[subscript.container->occurrence.type];
    assertTrue(array->kind == Type::Array);

    // Silently, because the role is `Optional`: a library that declares no condition function is one
    // whose fixed arrays are unchecked, and saying so at every subscript would be a diagnostic about
    // the library repeated once per element access.
    if(!program.roles.function(intrinsic::Known::checkCondition)) return;

    auto unsignedWord = program.types.words[0];
    Node* bound = nullptr;

    if(array->hasFixedLength()) {
        // The index the source wrote, where it wrote a number. A `Const` at this point is the whole
        // of what is knowable: nothing here folds arithmetic, and what does is `fold.cpp`, above.
        if(subscript.index->kind == Node::Const) {
            auto written = builder.body.constant(builder.base(),
                                                 ((NodeConst*)subscript.index)->constant);
            if(written.kind == Constant::Int && written.asInt() < array->length) return;
        }

        bound = builder.addConst(source, intConstant(unsignedWord, array->length));
    } else {
        // A quantified count is a number of the const parameter's own declared type, which is an
        // `Int` where a signature wrote `n: Int` - so it is converted rather than reinterpreted.
        auto counted = builder.addGenValue(source, array->count);
        if(!counted) return;

        bound = builder.addConvert(source, *counted, unsignedWord);
    }

    if(!bound) return;

    auto index = builder.addBitcast(source, *subscript.index, unsignedWord);
    if(!index) return;

    // The mistake rather than the invariant - `index >= length` - which is what the library's own
    // comment on `checkCondition` asks for: the branch it holds is then predicted the way it goes.
    Node* past = builder.addBinary(source, BinaryOp::Ge, *index, *bound);
    callKnown(builder, intrinsic::Known::checkCondition, &past, 1, source);
}

Place* Resolver::elementAccess(Builder& builder, const Subscript& subscript, Access access,
                               LocationId source) {
    if(subscript.fixed) {
        checkFixedBound(builder, subscript, source);
        return builder.elementPlace(*subscript.container, *subscript.index);
    }

    /*
     * Which accessor, from what the position does: a write or a `&` argument reaches `getMut`, and
     * every other position `get`. A move out of an element is not an access an accessor can grant -
     * the element stays the container's - so it takes `get` and the read that follows refuses it,
     * naming `exchange`.
     */
    auto unique = access == Access::Unique;
    auto container = readPlace(builder, source, *subscript.container,
                               bindingRequest(unique ? Access::Unique : Access::Shared));
    if(!container) return nullptr;

    Node* operands[2] = { container, subscript.index };
    auto element = applyMember(builder, intrinsic::Known::index, unique ? 1 : 0, operands, 2,
                               source);
    if(!element) return nullptr;

    // What the loan checker's element exemption follows - `NodeCall::subscript`. A provided
    // instance expands to something else, and an element it hands back is a place already.
    if(element->kind == Node::Call) ((NodeCall*)element)->subscript = true;

    // The borrow the accessor handed back, as a place: a local holding a loan is the object it
    // names, exactly as a borrowed parameter is.
    return hold(builder, *element, source);
}

/*
 * `xs[a..b]` - part of a container, as a window.
 *
 * A call to `slice`, or to `sliceMut` where the position writes through it - the two functions
 * `known.def` names, whose bodies are where the clamping and the arithmetic are. The compiler's part
 * is to read the container at the access the position asks for and hand it to the one the access
 * names; a container that is not yet a window is adapted at the call by the ordinary rule.
 *
 * **Half-open**, matching `doc/spec/patterns.md`'s `a..b` and the counted `for`. `a..=b` is refused
 * rather than quietly read as the other one: what a subslice's bounds mean is exactly the thing a
 * reader has to be sure of.
 */
Node* Resolver::resolveSubwindow(Builder& builder, ast::Expr& expr, Request wanted) {
    auto& written = *parse[expr.sub];
    auto& range = *parse[written.args.get(parse, 0).value.range];

    if(range.reverse) {
        context.diagnostics.error("a window runs forwards, and this range does not"_v, expr.source);
        return nullptr;
    }

    auto unique = wanted.access == Access::Unique;

    auto target = projectionTarget(builder, written.callee, unique ? Access::Unique : Access::Shared);
    if(!target) return nullptr;

    // Borrowed rather than copied whatever its type says, because a window describes storage that
    // stays where it is - the position's own request, narrowed to the two accesses a window has.
    auto source = readPlace(builder, expr.source, *target,
                            bindingRequest(unique ? Access::Unique : Access::Shared));
    if(!source) return nullptr;

    auto at = resolveValue(builder, range.from, program.types.words[1], inspectRequest());
    auto to = resolveValue(builder, range.to, program.types.words[1], inspectRequest());
    if(!at || !to) return nullptr;

    Node* operands[3] = { source, at, to };
    return callKnown(builder, unique ? intrinsic::Known::sliceMut : intrinsic::Known::slice,
                     operands, 3, expr.source);
}

// See the declaration - one sentence, and the role name comes off the table rather than out of the
// caller's spelling of it.
void Resolver::reportMissingRole(intrinsic::Known role, StringView needs, LocationId source) {
    context.diagnostics.error(
        "the standard library does not declare anything with the role %@, which %@ needs"_v,
        source, intrinsic::knownDef(role).role, needs);
}

void Resolver::reportMissingRole(intrinsic::Op op, StringView needs, LocationId source) {
    context.diagnostics.error(
        "the standard library does not declare anything with the role %@, which %@ needs"_v,
        source, intrinsic::opDef(op).role, needs);
}

Node* Resolver::callKnown(Builder& builder, intrinsic::Known role, Node* const* operands,
                          Size count, LocationId source, DeclPtr<Type> hint) {
    auto declared = program.roles.function(role);
    if(!declared) {
        reportMissingRole(role, "this"_v, source);
        return nullptr;
    }

    return callFunction(builder, *decl()[declared], operands, count, source, hint);
}

// An argument list written in order with no names, mapped onto the callee's positions - see the
// declaration, where what it adds over `mapArguments` is argued.
void positionalMap(Function& callee, Size supplied, ArgMap& into) {
    for(U32 position = 0; position < callee.writtenArity(); position++) {
        into.sources.push(position < supplied ? position : ArgMap::kOmitted);
    }

    for(U32 argument = 0; argument < supplied; argument++) into.positions.push(argument);
}

/*
 * A call over values that are already resolved - the compiler's own calls, and an operator's.
 *
 * The same three steps `resolveCall` ends in, over a list that was not written as an argument list:
 * each value is prepared at the position it fills, the positions nothing filled take their own
 * defaults, and `finishCall` is the rest. `positionalMap` is what makes the second of those true
 * here - an operator reaches a declaration with a defaulted tail exactly as a written call does, and
 * a walk that handed `count` arguments to a callee declaring more asserted in the builder.
 *
 * `count` is what the name was found at rather than what the callee declares, so the assertion is
 * the arity *range* - `acceptsArity`, the same predicate the table filed the declaration under.
 */
/*
 * The callee's type arguments as the **resolved** arguments decide them, with nothing prepared.
 *
 * One pass ahead of the argument walk, for the one case that needs it: a position holding an
 * undecided literal takes its type from what the other arguments bound, and the argument walk binds
 * them in written order - so `48 + rest` would read an empty `a` at position zero and build the
 * number at its default, which is the state this rule exists to leave. Binding is structural and
 * writes into empty slots only, so the walk below binds the same things again and changes nothing.
 */
void Resolver::bindResolvedArguments(Function& callee, const ArgMap& map, Node* const* values,
                                     Size count, TypeArguments& into) {
    for(Size i = 0; i < count; i++) {
        if(!values[i]) continue;

        auto& parameter = *decl()[callee.args.get(decl(), map.position(U32(i)))];
        if(parameter.lazy) continue;

        into.bind(parameter.occurrence.type, valueType(*values[i]));
    }
}

Node* Resolver::callFunction(Builder& builder, Function& callee, Node* const* operands, Size count,
                             LocationId source, DeclPtr<Type> hint, ast::Expr* const* pending,
                             TypeArguments* carried) {
    assertTrue(callee.acceptsArity(decl(), U32(count)));

    ArgMap map;
    positionalMap(callee, count, map);

    /*
     * `carried` is `chooseCandidate`'s own trial - `selectOperatorCall`'s, the only caller that has
     * one - kept rather than rebuilt here: selection already bound and widened this signature to
     * decide the candidate fit, and a fresh `TypeArguments` would ask the same questions again and
     * could answer at least one of them differently, which is what let `x + y` at `(I32, I64)`
     * select through an overloaded `+` and then have `callFunction` refuse the very operand
     * selection had just widened for. Every other caller passes none and gets what it always did -
     * its own fresh solve, with nothing carried in because there was no earlier trial to disagree
     * with.
     */
    Maybe<TypeArguments> fresh;
    if(!carried) fresh.replace(*this, callee.gen);
    auto& typeArguments = carried ? *carried : fresh.unwrap();

    // What the resolved operands decide, before any of them is prepared - so that an operand left
    // for this call to type reads what the *others* bound rather than what the positions ahead of it
    // happened to. `48 + rest` is the case: the literal is operand zero and `rest` is what says what
    // it is. See `bindResolvedArguments`.
    if(pending) bindResolvedArguments(callee, map, operands, count, typeArguments);

    SmallArray<Node*, 8> arguments;
    for(U32 i = 0; i < map.count(); i++) arguments.push(nullptr);

    HeldList<SmallArray<Node*, 8>> held(*this, arguments);

    for(Size i = 0; i < count; i++) {
        auto position = map.position(U32(i));
        auto& parameter = *decl()[callee.args.get(decl(), position)];
        auto operand = operands[i];

        // An undecided literal, built at the position the winner declared - `undecidedLiteral`, and
        // the operator half of what `finishOverloadedCall` does for a written argument list. Through
        // the value walk, since what was left standing is the written expression - a negation and a
        // parenthesis are unwrapped there and nowhere else.
        if(!operand && pending && pending[i]) {
            operand = resolveValue(builder, *pending[i],
                                   typeArguments.hint(parameter.occurrence.type),
                                   requestOf(parameter.occurrence));
            if(!operand) return nullptr;
        }

        auto prepared = prepareArgument(builder, *operand, parameter, typeArguments, source);
        if(!prepared) return nullptr;

        arguments[position] = prepared;
    }

    // The widening a later position decided, applied to the ones already prepared - see
    // `widenPreparedArguments`. Harmless where `typeArguments` never unified anything, which is
    // every caller but `selectOperatorCall`'s: `widened` stays false and this returns at once.
    if(!widenPreparedArguments(builder, callee, map, source, arguments, typeArguments)) {
        return nullptr;
    }

    if(!fillDefaultArguments(builder, callee, map, source, arguments)) return nullptr;

    return finishCall(builder, callee, arguments, typeArguments, hint, source);
}

Node* Resolver::resolveField(Builder& builder, ast::Expr& expr, Request wanted) {
    auto place = resolvePlace(builder, expr, true, wanted.access);
    if(!place) return nullptr;

    return readPlace(builder, expr.source, *place, wanted);
}

/*
 * Whether an exclusive loan may be taken of this place although the name it goes through was never
 * declared `&`.
 *
 * `let &x = e` only - `Request::establishes`, where the pair of questions is argued - and only over
 * storage **this frame's own**. A place rooted in a local is that: a `let`, a pattern binding that
 * took a payload out, and a parameter alike, and what `doc/spec/ownership.md` grants over one is
 * exactly this - "`let &d = c` - an exclusive loan: writing through d writes c", with the `&` on the
 * new binding as the author's statement of intent.
 *
 * A **global** is deliberately not: the program declared whether it may be written, there is no
 * frame that owns it, and `writable` is the whole record of the answer. A raw address is the same
 * case, with the pointer's own mutability in that field.
 *
 * What this does not touch is the `&`-*argument* rule. `mutate(h)` for a plain `let h` is still
 * refused by the test below, which is the rule that makes `let &z = h` the way an author opts in.
 */
static bool bindsOwnStorage(Builder& builder, const Place& place, Request wanted) {
    if(!wanted.establishes) return false;

    auto base = builder.base();
    const Place* root = &place;
    while(root->parent) root = base[root->parent];

    return root->kind == Place::Local;
}

Node* Resolver::readPlace(Builder& builder, LocationId source, Place& place, Request wanted) {
    /*
     * A type with **one value** is that value, and never a read of the storage it was given.
     *
     * `{}` occupies no bytes, so there is nothing at the place to read: a load of it would state a
     * width of zero, which `makeMemoryFlags` has no encoding for and `lower_inst.cpp` asserts
     * against by name. What reaches this is a field of that type - `Try(Maybe(a), a, {})` exits
     * carrying one, and `Exit(->reason)` in a pattern binds one - so it is the projection that has
     * to answer rather than the load, and the answer a type with one value gives is a constant.
     *
     * Not for an **exclusive** request, which is the one position where the place itself is the
     * point: `&x: {}` promises writing through to the same storage, and a constant has none. Nothing
     * writes a `{}` through today; the guard is here so that the day something does, it is refused
     * by the ordinary rule rather than silently given a value of its own.
     */
    if(wanted.access != Access::Unique && decl()[place.occurrence.type]->kind == Type::Unit) {
        return builder.addConst(source, unitConstant(place.occurrence.type));
    }

    /*
     * An **immutable global whose initializer is a constant is that constant** - Analysis-
     * Initialization.md's stage A, said about a read rather than about a constant position.
     *
     * Nothing in the program can assign to it, so its value is forever what `resolveGlobals`
     * recorded and a read of the storage would load bytes that never change. What makes it worth
     * doing here rather than leaving to a later fold is that it is the *only* thing that makes the
     * compiler's supplied constants usable: `lib/Native/Memory.native.yana` writes
     * `let width = vectorBytes` and gates a `Vec(U8, 32)` load on `width > 32`, and the x64 backend
     * refuses that load outright below v3. Behind a load the comparison is opaque, the arm survives
     * and the *build fails*; folded, it is gone before any pass has run. `pageBytes`, `wordBits` and
     * `Target.byteOrder` are read the same way.
     *
     * The root only, and not a projection of one. `origin.x` of a constant record folds for exactly
     * the same reason and is the shipping stage's `constantAt` walk; nothing in `lib/` needs it yet,
     * so the walk is not written twice before there is a second reader.
     *
     * A **scalar** value only, which is the narrower half of that. An `Aggregate` or a `Text` global
     * has a *placement* - one run of bytes the program refers to - and a read of it is a read of
     * that storage rather than a description of what is in it; `resolve/NicheOwned.yana` answers a
     * different number when the byte image behind a string literal is handed to two readers as two
     * constants. What the supplied globals are is numbers, and numbers have no placement.
     */
    if(place.kind == Place::Global && !place.parent && wanted.access != Access::Unique) {
        auto& global = *decl()[DeclPtr<Global>(place.payload)];
        auto kind = global.initial.kind;
        auto scalar = kind == Constant::Int || kind == Constant::Float ||
                      kind == Constant::Enum || kind == Constant::Unit;

        if(!global.writable && !global.dynamic && scalar) {
            return builder.addConst(source, global.initial);
        }
    }

    /*
     * The same sentence about a **local**, and it is the one that makes the global rule above pay
     * off: `lib/Native/Memory.native.yana` writes `let width = vectorBytes` and gates a
     * `Vec(U8, 32)` load on `width > 32`, so folding the global read alone only moves the constant
     * into a frame slot the comparison then loads it back out of. Nothing promotes that slot on an
     * unoptimized build, the arm survives, and the x64 backend refuses the load outright below v3.
     *
     * `Local::initial` is set only for a binding nothing may assign to, so the value is permanent;
     * the storage is still there for a position that needs one.
     *
     * **Not for a published position**, which is where a local differs from a global. `'T` and `&T`
     * ask for a loan of *this* storage, and a constant has none to lend - the whole point of the
     * qualifier is the relationship to the place. A plain value position is the one that only wants
     * the number, and it is the one every arithmetic operand is.
     *
     * A **scalar** only, on the global rule's own terms: an aggregate or a text constant has a
     * placement, and a read of one is a read of that storage rather than a description of it.
     */
    if(place.kind == Place::Local && !place.parent && !wanted.published &&
       wanted.access != Access::Unique && wanted.form == AccessForm::WholeValue) {
        auto& local = *builder.body.local(builder.base(), LocalId(place.payload));
        auto kind = local.initial.kind;

        if(kind == Constant::Int || kind == Constant::Float || kind == Constant::Enum) {
            return builder.addConst(source, local.initial);
        }
    }

    auto mode = readMode(decl(), builder.base(), builder.body, place, wanted);

    /*
     * A sink position, and storage that has nothing to give it.
     *
     * `readMode` answered `BorrowShared` because the place is not this function's - a shared
     * parameter, a global, a projection of either - and a borrow is the only honest thing to
     * produce. But the position asked for an owner, and whatever consumes the value will act as
     * though it received one: `fn f(g: Guard) = eat(g)` handed the caller's guard to a `->`
     * parameter, and both frames then released it.
     *
     * Reported here and not caught anywhere later, deliberately. The ownership analysis sees a
     * borrowing read and a consuming call, both of which are individually well formed; nothing after
     * this point knows the position wanted something the place could not supply.
     */
    /*
     * Exclusive access, and storage that cannot grant it.
     *
     * `readMode` answers `BorrowUnique` for a position that asked for one whatever the place is,
     * because neither a copy nor a shared loan satisfies "writing through to the same storage". What
     * makes it legal is the place, and there are two ways it is not: the *storage* is somebody
     * else's and shared, or the *binding* is one the author did not declare `&`. They are different
     * mistakes with different fixes - see `Place::writable`, where the pair is kept apart - so they
     * get different messages.
     *
     * The second is about a **name**, so it does not apply to the one position that is establishing
     * one: `let &x = e` is where an author declares mutable storage, and asking the source to have
     * been declared `&` already would leave that with no spelling. `bindsOwnStorage` is that case
     * and nothing wider - the storage half above still refuses a shared borrow, and a global still
     * answers with the mutability the program gave it.
     */
    if(mode == UseMode::BorrowUnique) {
        if(place.occurrence.contract.access() == Access::Shared) {
            context.diagnostics.error(
                "%@ is borrowed as shared here, so it cannot be handed out for writing"_v,
                source, placeText(context, decl(), builder.base(), builder.body, place));
            return nullptr;
        }

        if(!place.writable && !bindsOwnStorage(builder, place, wanted)) {
            context.diagnostics.error(
                "%@ is not declared `&`, so it cannot be handed out for writing"_v,
                source, placeText(context, decl(), builder.base(), builder.body, place));
            return nullptr;
        }
    }

    if(wanted.access == Access::Owned && mode == UseMode::BorrowShared) {
        /*
         * A **capture**, handed to a position that consumes it - `doc/spec/functions.md`'s fourth
         * capture kind, refused where the general rule already refuses it.
         *
         * The general message would be right and would name a local the author never wrote: the
         * environment is synthesized, so "`f.lambda.env.h` cannot be handed over" describes the
         * compiler rather than the program. What the author asked for is a closure that owns what it
         * captured, and the spec pairs that with outliving the frame - which since D17 is what a
         * lambda written at an *owning* position gets. At a borrowing one the environment is a local
         * of this frame, so there is nothing in it to hand over and the position is what has to
         * change.
         */
        if(isCapture(builder, place)) {
            context.diagnostics.error(
                "a lambda cannot hand over what it captured, because this one is a callback - a closure owns its captures only where the position it fills owns the closure"_v,
                source);
            return nullptr;
        }

        /*
         * The same sentence from the other end: a closure the position *does* own, capturing
         * something this function only borrows.
         *
         * `buildEnvironment` reads each capture under the field's own contract, and at a heap-placed
         * environment that contract is owned - so a binding that is itself a loan of somebody else's
         * storage arrives here. What the author has to change is not the position but the value, and
         * the general message below would send them to the position.
         */
        if(lambdaFrames.size() && lambdaFrames[lambdaFrames.size() - 1].owns) {
            context.diagnostics.error(
                "this closure leaves the frame it was written in, so it owns what it captured - and %@ is storage this function borrows rather than owns"_v,
                source, placeText(context, decl(), builder.base(), builder.body, place));
            return nullptr;
        }

        context.diagnostics.error(
            "%@ cannot be handed over, because this function borrows it rather than owning it"_v,
            source, placeText(context, decl(), builder.base(), builder.body, place));

        // No value, rather than the loan it would have been. Every sink checks its argument for null
        // and reports nothing further for one, so this is what keeps a single mistake to a single
        // message: `return g` would otherwise be told twice, once here and once by the `return` that
        // found a borrow where an owner belongs.
        return nullptr;
    }

    /*
     * A `@bits(n)` place is read **at its own type**, and the widening happens at the use rather
     * than here.
     *
     * `doc/spec/repr.md`'s "load always widens" holds and is a statement about the machine: the
     * storage is thirty bits of a thirty-two bit cell, and what the register holds after the read
     * is that value zero- or sign-extended, which is `lower_width.cpp`'s normal form. What it is
     * *not* is a statement about the type this stage gives the read, and that is where the two came
     * apart: converting to the canonical here threw away the one fact the refinement carries.
     * `@bits(30) U32` reaches `Size` because thirty unsigned bits fit thirty-one signed ones, and a
     * value already read as `U32` does not - so `fn length(self: String) -> Size = self.length` was
     * refused, in the real `lib/Core/String.yana`, for a conversion that loses nothing.
     *
     * Nothing needed the eager form. Everything that *dispatches* canonicalizes on the way past -
     * `matchType` compares through `canonicalInt`, `TypeArguments::bind` binds the canonical, and
     * `commonWiden` unifies through it - so arithmetic still never lands at a refinement and `Num`
     * still needs one instance per family rather than one per width. What each of those does to the
     * value is the ordinary conversion, `convertValue`'s `refinesInteger` arm: one `convert` node,
     * at the use, to the type that use asked for. It is also one node fewer than the pair it
     * replaced, since a read widened to `U32` and then wanted at `U64` converted twice.
     *
     * The shipping stage reads a refinement the same way - `load %h@Header.length : @bits(30) U32`,
     * and a `cast` at each use - which is what makes the two stages agree about `lib/Core`.
     */
    return builder.addRead(source, place, mode, wanted);
}

/*
 * `Pair {x: 1, y: 2}` - Analysis-IR.md §5.7.
 *
 * One value per declared field and in field order, which is what `Builder::addAggregate` takes: the
 * written order is the author's and the field order is the type's, and resolving into the second
 * here is what lets the node be a plain operand list with nothing positional left to get wrong.
 *
 * Every field is resolved even after one has failed, because a missing field and a mistyped one are
 * different mistakes and an author fixing the first should not have to compile again to see the
 * second.
 */
/*
 * Which written argument fills each field of one constructor - the mapping half of a construction.
 *
 * Shared by the value form and the **constant** one, on `mapArguments`' own terms: a construction in
 * a body and one in a global's initializer are the same syntax naming the same fields, and two
 * copies of "which position does each argument fill" would agree until one was changed. What differs
 * between the two callers is what they then do with each position, which is where they part.
 */
bool Resolver::constructionArguments(ast::Expr& expr, RecordType& declared, const RecordCon& arm,
                                     ArgMap& into) {
    auto& written = *parse[expr.con];

    /*
     * `Cell(5)`, `Tagged{value}`, `Proceed(x)` - fields given by position, in declaration order.
     *
     * The same thing a call does with its arguments, and it is the same mapping below that does it:
     * a field is a declared position with a name, and writing one by position is leaving the name to
     * the declaration. `Proceed(x)` is not a special case of it either - its one field carries the
     * constructor's own name (`declareFields`), so filling position zero *is* naming that field.
     *
     * **A construction is all one or all the other**, which is where it parts from a call. A call
     * may write its positional arguments and then start naming; a construction's positions are the
     * record's *fields*, and `doc/spec/types.md` says of the shape they are closest to - the tuple -
     * that its fields are all positional or all named. The mixed form buys nothing here: a record's
     * fields have names, so a construction that has started using them has no reason to stop, and
     * one that never used them is reading the declaration in order. Reported here rather than left
     * to `mapArguments`' ordering rule, which would say something narrower and less useful about the
     * half of the mix it happens to see first.
     */
    auto named = false;
    auto positional = false;

    for(auto argument: written.args.contents(parse)) {
        if(argument.name) named = true; else positional = true;
    }

    if(named && positional) {
        context.diagnostics.error(
            "%@ is built with some fields named and some by position - write every one of them the same way"_v,
            expr.source, context.findName(arm.name));
        return false;
    }

    /*
     * The same mapping a call's arguments go through, and `mapArguments` is where the rules are. A
     * construction and a call differ in the words their messages use and in nothing else: both take
     * names, both fill by position until one is written, and both let a position the site left out
     * take its own default.
     */
    SmallArray<ArgPosition, 8> fields;
    for(U16 i = 0; i < arm.count; i++) {
        auto field = declared.field(decl(), U16(arm.first + i));
        fields.push(ArgPosition { field.name, field.def.isSet() });
    }

    ArgSite site;
    site.declared = fields.pointer();
    site.count = arm.count;
    site.subject = arm.name;
    site.one = "field"_v;
    site.many = "fields"_v;
    site.source = expr.source;

    return mapArguments(written.args, site, into);
}

Node* Resolver::resolveConstruction(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint,
                                    Request wanted) {
    auto destination = wanted.shape;
    auto& written = *parse[expr.con];

    if(written.type.kind != ast::Type::Con) {
        unsupported(expr.source, "this construction"_v);
        return nullptr;
    }

    auto found = findConstructor(context, program, here(), written.type.name, expr.source);
    if(found) recordReference(context, written.type.source, constructorSymbol(decl(), found));

    if(!found) {
        context.diagnostics.error("unknown constructor %@"_v, expr.source,
                                  context.findName(written.type.name));
        return nullptr;
    }

    auto record = found.record;
    auto declared = decl()[record];
    auto arm = declared->cons.get(decl(), found.index);

    if(!reachesInside(*declared, expr.source, "its constructor"_v)) return nullptr;

    /*
     * `Boxed {value: 33}` - which instantiation of a generic record this builds.
     *
     * The same question a generic call asks and answered the same way: each argument starts
     * undecided, the *expected type* seeds it where the position supplied one, and each field value
     * binds what stands opposite it. `bindTypeArguments` is the one matcher, so a constructor and a
     * call cannot come to disagree about what `Boxed(I32)` is inferred from.
     *
     * The declaration's own fields are what the walk below reads - the names and the indices are the
     * declaration's whatever the arguments turn out to be - and the *types* are substituted at each
     * comparison. The instantiation is made once, at the end, when there is something to make it
     * from.
     */
    TypeArguments typeArguments(*this, declared->gen);

    /*
     * A **view record** at a position that asks for its owner - `ArrayView {...}` at a `'[a]`
     * result, `StringView {...}` at a shared `String`. The hint is the owner's type, and it is the
     * right hint: the owner and its view have one argument list by `known.def`'s arity column, so
     * the owner's instantiation seeds the view's, and `adaptToView` at the position is what then
     * restates the record as the view. The only position a view record may be built for, and this
     * is where that is decided.
     */
    auto viewRow = viewRowOfRecord(decl(), program, asBase<Type>(record));
    auto ownerHint = hint && viewRow && viewRowOf(decl(), program, hint) == viewRow;

    if(typeArguments.any()) {
        if(hint && decl()[hint]->kind == Type::Record &&
           (((RecordType*)decl()[hint])->instanceOf == record || ownerHint)) {
            typeArguments.seed(*(RecordType*)decl()[hint]);
        } else if(hint) {
            context.diagnostics.error("this position asks for %@, and this builds a %@"_v,
                                      expr.source, typeText(context, decl(), hint),
                                      context.findName(declared->name));
            return nullptr;
        }
    } else if(hint && !ownerHint && !sameType(hint, asBase<Type>(record))) {
        context.diagnostics.error("this position asks for %@, and this builds %@"_v, expr.source,
                                  typeText(context, decl(), hint),
                                  typeText(context, decl(), asBase<Type>(record)));
        return nullptr;
    }

    auto type = asBase<Type>(record);

    ArgMap map;
    // The cursor among the fields being written - before they are mapped, since a half-typed name is
    // not a field and mapping it is what would report.
    if(captureFields(written.args, asBase<Type>(record), arm.first, arm.count)) return nullptr;

    if(!constructionArguments(expr, *declared, arm, map)) return nullptr;

    SmallArray<Node*, 8> values;
    for(U16 i = 0; i < arm.count; i++) values.push(nullptr);

    // The field values already in hand, for the reason a call's arguments are - and by index, so a
    // position still being resolved is the null the carry skips.
    HeldList<SmallArray<Node*, 8>> held(*this, values);

    /*
     * In **written** order, filling the field the mapping named - the rule a call's arguments follow
     * and for the same reason: `doc/spec/expressions.md` evaluates a named argument where it is
     * written, and what the aggregate is built from is in field order.
     *
     * Every field is resolved even after one has failed, because a mistyped field and a second
     * mistyped field are two mistakes and an author fixing the first should not have to compile
     * again to see the second.
     */
    auto ok = true;
    auto at = U32(0);
    for(auto argument: written.args.contents(parse)) {
        auto index = map.position(at++);
        auto field = declared->field(decl(), U16(arm.first + index));
        auto declaredType = field.occurrence.type;

        /*
         * What this position asks for is the **destination's** answer where it has one -
         * `Maybe('v)`'s payload, and the field's own everywhere else.
         *
         * Which is the whole of what a qualified type argument does at a construction: the field is
         * declared owned, and the position being filled says the record is only going to refer to
         * what goes in it. Reading the field's contract here instead would take a copy of a name the
         * signature promised to borrow - safe for a scalar and a duplicate for anything else - and
         * the result would then claim an origin it never took.
         */
        auto member = Occurrence { declaredType, fieldContract(decl(), destination, field),
                                   program.shapes.under(destination,
                                                        PathStep::field(field.index)) };

        /*
         * A **field initializer** keeps what an `iter fn` call answers - §2.1's third row, and the
         * position `data Parser(l) {lexer: l}` exists for: the field is at a type variable, the call
         * builds `lex.Iter`, and `bind` below is what makes the record's argument that type.
         */
        Node* value = nullptr;

        {
            ObjectScope object(*this, argument.value.source);
            value = resolveValue(builder, argument.value, typeArguments.hint(declaredType),
                                 requestOf(member));
        }

        if(!value) { ok = false; continue; }

        typeArguments.bind(declaredType, valueType(*value));

        auto expected = typeArguments.expected(declaredType);

        // A field that holds a view - `data Cursor {rest: '[U8]}` - takes one the way a parameter
        // does, and a container written there is adapted by the same member.
        value = adaptToView(builder, *value, Occurrence { expected, member.contract, member.shape },
                            argument.value.source);
        if(!value) { ok = false; continue; }

        value = coerce(builder, *value, expected, argument.value.source, "a field"_v,
                       member.contract.access());
        if(!value) { ok = false; continue; }

        // A closure into a field the record owns - `checkCarriedOrigins`, which is `own.cpp`'s
        // declaration-time refusal applied to a value whose interior shape the declaration cannot
        // see.
        if(!checkCarriedOrigins(*value, member, argument.value.source,
                                stringView(context.findName(declared->name)))) {
            ok = false;
            continue;
        }

        values[index] = value;
    }

    /*
     * The fields the construction left out, each built from the field's own constant.
     *
     * `doc/spec/declarations.md`'s "a default is *built* at each construction that leaves the field
     * out, by the same code the construction the author could have written would have used" - which
     * is this line: the same `addConst` a written literal reaches. So two constructions that both
     * omit a field hold two values, and a default that is one day an aggregate is an aggregate each
     * of them owns.
     */
    for(U16 i = 0; i < arm.count; i++) {
        if(!map.omitted(i)) continue;

        values[i] = constantValue(builder, declared->field(decl(), U16(arm.first + i)).def,
                                  expr.source);
        if(!values[i]) return nullptr;
    }

    if(!ok) return nullptr;

    if(typeArguments.any()) {
        // What the record's own head constraints decide, where a field left something open.
        typeArguments.fillDetermined(expr.source);

        // A record with a parameter no field mentions - `data Phantom(a) {n: I32}` - is what
        // reports here.
        if(typeArguments.reportUndecided(expr.source, "field"_v,
                                         Tritium::format("%@", context.findName(arm.name)))) {
            return nullptr;
        }

        type = instantiateRecord(context, program, record, typeArguments.arguments.pointer(),
                                 typeArguments.arguments.size(), expr.source);
        if(!type) return nullptr;
    }

    return builder.addAggregate(expr.source, type, found.index, values.pointer(), values.size(),
                                destination);
}

/*
 * `{4, 5}` and `{x: i, y: i * 2}` - the anonymous record, built.
 *
 * Its type is **the values'**, which is the difference between this and every other construction:
 * a `data` is a declaration a construction is checked against, and a tuple is a type that is made by
 * writing one. So the elements are resolved first and the type is interned from what they turned out
 * to be, with the written labels carried through.
 *
 * The **hint decides the elements**, where it is a tuple of this shape, and that is most of what it
 * does: it is what makes `{4, 5}` at a `{I64, I64}` position two `I64`s rather than two `I32`s that
 * then have to be widened, and it is the same push-down `::` performs one level up. A hint of
 * another shape is ignored rather than reported - the literal builds its own type, and a position
 * that cannot take it says so in the ordinary words at the position it is.
 *
 * Every field is an owner. There is no declaration on which a position's contract could have been
 * written, so a tuple holding a loan is a type the source has no spelling for - which is the same
 * sentence `resolveType` makes about the written form.
 */
Node* Resolver::resolveTupleLiteral(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint) {
    auto written = expr.tup;
    auto count = U32(written.size());

    // A tuple written where one is expected knows its own field names before any argument is
    // resolved - the only way a tuple's fields have names to complete against.
    if(hint && decl()[hint]->kind == Type::Record && ((RecordType*)decl()[hint])->tuple) {
        auto fields = U16(((RecordType*)decl()[hint])->fields.size());
        if(captureFields(written, hint, 0, fields)) return nullptr;
    }

    /*
     * The position's own type, where it is a tuple this literal could be one of.
     *
     * The arity **and the labels** have to agree, because they are the type's identity: `{x: I32}`
     * and `{I32}` are two types, so a hint whose labels differ is a hint about something else and
     * pushing its field types down would be filling positions that are not these.
     */
    auto expected = (RecordType*)nullptr;

    if(hint && decl()[hint]->kind == Type::Record) {
        auto record = (RecordType*)decl()[hint];

        if(record->tuple && record->fields.size() == count) {
            expected = record;

            auto index = U16(0);
            for(auto argument: written.contents(parse)) {
                if(record->field(decl(), index++).name != argument.name) expected = nullptr;
            }
        }
    }

    SmallArray<Node*, 8> values;
    for(U32 i = 0; i < count; i++) values.push(nullptr);

    // The values already in hand, for the reason a construction's fields are: a position still being
    // resolved is the null the carry skips, and everything before it is live across whatever control
    // flow the next one builds.
    HeldList<SmallArray<Node*, 8>> held(*this, values);

    auto ok = true;
    auto index = U32(0);

    /*
     * Every element resolved even after one has failed, which is `resolveConstruction`'s rule: a
     * mistyped field and a second mistyped field are two mistakes, and an author fixing the first
     * should not have to compile again to see the second.
     */
    for(auto argument: written.contents(parse)) {
        auto at = index++;
        auto declared = expected ? expected->field(decl(), U16(at)).occurrence.type : nullptr;

        auto value = resolveValue(builder, argument.value, declared, bindingRequest(Access::Owned));
        if(!value) { ok = false; continue; }

        if(declared) {
            value = coerce(builder, *value, declared, argument.value.source, "a field"_v,
                           Access::Owned);
            if(!value) { ok = false; continue; }
        }

        values[at] = value;
    }

    if(!ok) return nullptr;

    SmallArray<RecordField, 4> fields;
    index = 0;

    for(auto argument: written.contents(parse)) {
        auto at = index++;

        RecordField row;
        row.name = argument.name;
        row.occurrence = owned(valueType(*values[at]));
        row.source = argument.value.source;
        fields.push(row);
    }

    // The same rule the written form is under, asked in the same words - see `uniqueTupleLabels`,
    // which is where the two halves of one type are made to agree.
    if(!uniqueTupleLabels(fields.pointer(), fields.size())) return nullptr;

    auto type = tupleType(context, program, fields.pointer(), fields.size(), expr.source);
    if(!type) return nullptr;

    // The one constructor an anonymous record has - see `tupleType`, where it is made.
    return builder.addAggregate(expr.source, type, 0, values.pointer(), values.size());
}

/*
 * The source of a record update, into the copy's storage - a duplicate or a relocation.
 *
 * One function because the two forms differ in what a read of a place *means* and in nothing else,
 * and the difference is `readMode`'s: an owned request over a place either duplicates it, where the
 * type duplicates freely, or moves out of it - and moving out is exactly what `->` asks for. So the
 * copying form is that request plus one thing, which is that a type with no free duplicate has to
 * say what a duplicate of it is. `Copy` is the language's answer, and this is an ordinary call of
 * it: a type with no instance is told so in the words every other missing instance is told in.
 */
bool Resolver::initializeUpdate(Builder& builder, Place& root, Place* from, Node* produced,
                                bool sink, LocationId source) {
    auto value = produced;

    if(!value) {
        if(!sink && !isTriviallyCopyableAt(decl(), from->occurrence)) {
            /*
             * Asked before it is called, because the instance's absence is what the *author* has to
             * be told about and the ordinary "there is no instance of Copy(T)" does not say what to
             * write instead. An update is the one position where the answer is a spelling: `->`
             * relocates and needs no duplicate at all.
             */
            auto type = from->occurrence.type;
            InstanceMatch found;
            if(!selectInstance(context, program, program.roles.klass(intrinsic::Known::copy), &type,
                               1, source, false, found)) {
                context.diagnostics.error(
                    "%@ has no instance of `Copy`, so there is nothing here to copy it with - write `{->...}` to relocate the source into the update instead"_v,
                    source, typeText(context, decl(), type));
                return false;
            }

            auto shared = readPlace(builder, source, *from, bindingRequest(Access::Shared));
            if(!shared) return false;

            Node* of[1] = { shared };
            value = applyMember(builder, intrinsic::Known::copy, 0, of, 1, source);
        } else {
            value = readPlace(builder, source, *from, requestOf(ownedContract()));
        }
    }

    if(!value) return false;

    builder.addWrite(source, root, *value, Destination::Init);
    return true;
}

/*
 * `{p | y: 3}`, `{->p | y: 3}`, `{p | ~y}` and `{r | .a.b: 1, .a.c: 2}` - the record update,
 * `doc/spec/expressions.md`.
 *
 * **One copy and a run of writes into it**, which is the whole construct and is why it is not a
 * construction. A construction is a component per field handed to an aggregate; this starts from a
 * value that already has every field and replaces some of them, so what it needs is *storage* - and
 * once there is storage, a path is a place projection and nothing else. That is what makes
 * `{v | .a.b: 1, .a.c: 2}` set two fields of one `v.a` by construction rather than by a rule: both
 * paths are projections of the same local, so there is nothing for a second copy to discard.
 *
 * **What the copy is, is the ownership rules and not this expression's business.** The source is
 * read once, at the request the position it lands in makes - an owned one - and `readMode` answers
 * with the duplication a `TrivialCopy` type gets for free. Where the type has no such answer the
 * language's word for a duplicate is `Copy`, so what this emits is the ordinary `copy(v)` class
 * call - the one site in the compiler that *calls* that member rather than reading the capability an
 * authored instance subtracts. A type with no instance is the one refusal this expression words for
 * itself, because it is the one position where the answer is a spelling: `->` relocates.
 *
 * `{->v | ...}` is the same shape with a **relocation** in place of the copy - no duplication at
 * all, so a type that cannot be copied can still be updated, which is what the arrow is for. The
 * source is a place either way; a source that is *produced* - a call result, another update - is in
 * no storage for either to act on and is simply the value, which is why `->` on one is neither an
 * error nor a second path.
 *
 * The order is written rather than incidental. The replacements are resolved **before** the source
 * relocates, so `{->v | x: v.x + 1}` reads `v` while it is still there - and it is what a move
 * update is usually for. A copy is taken as early as it can be, because it leaves the source
 * readable and delaying it would only lengthen the value's live range.
 *
 * Every replacement is a `Destination::Assign` through the same `Builder::addWrite` a written
 * `v.f = x` reaches, and that is the one thing this construct owes: the copy put a live value in
 * every field, so a field being replaced has one to release. An `Init` here would leak it.
 */
Node* Resolver::resolveUpdate(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint) {
    auto& written = *parse[expr.tupUpdate];
    auto sink = written.bind == ast::BindType::Sink;

    /*
     * The source as a **place** where it names one, and as a value where it does not - `evaluatedPlace`'s
     * question, asked here for the same reason: what the source *is* decides what taking a copy of
     * it means, and only a place can be copied out of or relocated out of.
     */
    ErrorMark mark(context);
    auto from = resolvePlace(builder, written.value, false);
    Node* produced = nullptr;

    if(!from) {
        if(mark.reported()) return nullptr;

        produced = resolveValue(builder, written.value, hint, requestOf(ownedContract()));
        if(!produced) return nullptr;
    }

    auto type = from ? from->occurrence.type : valueType(*produced);

    /*
     * A single-constructor record, which is a `data T {..}` and an anonymous tuple both - the second
     * is the first with `tuple` set, so one test covers the two spellings the spec gives.
     *
     * A sum is refused for the reason a field of one is not reachable by name: which fields the
     * value has is what its discriminant says, and an update names fields.
     */
    auto record = decl()[type]->kind == Type::Record ? (RecordType*)decl()[type] : nullptr;

    if(!record || record->layout != RecordLayout::Single) {
        context.diagnostics.error(
            "%@ cannot be updated - an update replaces named fields, so its source has to be a record with one constructor or a tuple"_v,
            written.value.source, typeText(context, decl(), type));
        return nullptr;
    }

    /*
     * The copy's storage, declared before the source is read so that a path can be projected out of
     * it while the replacements are still being resolved.
     *
     * Writable, which `hold` cannot give it: that reads the permission off the value's own contract,
     * and an owned record's says nothing about whether the storage holding it may be written. Here
     * it may - the local is this frame's temporary and writing the replacements into it is the whole
     * reason it exists, exactly as a growable array literal's run is.
     */
    auto local = builder.addLocal(StringId(), owned(type), expr.source, true);
    auto root = builder.localPlace(*local);

    // The copy, as early as it can be - see the header. The relocation waits below.
    if(!sink && !initializeUpdate(builder, *root, from, produced, sink, written.value.source)) {
        return nullptr;
    }

    /*
     * One replacement per written argument, each into the place its path reaches.
     *
     * The values are held rather than written as they are resolved, because the `->` form has not
     * relocated yet: a write before that would land in storage the relocation is about to overwrite.
     * The copying form could write as it goes and does not, so that there is one order here rather
     * than one per form.
     */
    struct Replacement {
        Place* place = nullptr;
        Node* value = nullptr;
        LocationId source = kNullLocation;
    };

    SmallArray<Replacement, 4> replacements;

    SmallArray<Node*, 4> values;
    HeldList<SmallArray<Node*, 4>> held(*this, values);

    auto ok = true;

    for(auto argument: written.args.contents(parse)) {
        auto place = root;

        // The path, one field at a time, off whatever the segment before it reached - which is the
        // same answer a `.` gives, because an update path *is* field selection with a value on the
        // end of it.
        for(auto field: argument.path.contents(parse)) {
            if(isCursorSentinel(context, field)) {
                captureUpdateCompletion(place->occurrence);
                place = nullptr;
                break;
            }

            place = fieldNamedOf(builder, *place, field, argument.value.source,
                                 written.value.source);
            if(!place) break;
        }

        if(!place) { ok = false; continue; }

        auto value = resolveValue(builder, argument.value, place->occurrence.type,
                                  requestOf(place->occurrence));
        if(!value) { ok = false; continue; }

        value = coerce(builder, *value, place->occurrence.type, argument.value.source,
                       "a replaced field"_v, place->occurrence.contract.access());
        if(!value) { ok = false; continue; }

        // Storage that already exists says what may be in it - the same check an assignment makes,
        // because this is the same write.
        if(!checkCarriedOrigins(*value, place->occurrence, argument.value.source,
                                "this field"_v)) {
            ok = false;
            continue;
        }

        replacements.push(Replacement { place, value, argument.value.source });
        values.push(value);
    }

    // Every replacement resolved even after one has failed, which is `resolveConstruction`'s rule: a
    // mistyped field and a second mistyped field are two mistakes.
    if(!ok) return nullptr;

    // The relocation, after everything that still reads the source - see the header.
    if(sink && !initializeUpdate(builder, *root, from, produced, sink, written.value.source)) {
        return nullptr;
    }

    for(auto& replacement: replacements) {
        builder.addWrite(replacement.source, *replacement.place, *replacement.value,
                         Destination::Assign);
    }

    // The finished record, moved out of the local it was built in - the same read a growable array
    // literal ends with, and for the same reason: what the position receives is the temporary rather
    // than a borrow of a local that is about to end.
    return readPlace(builder, expr.source, *root, requestOf(ownedContract()));
}

/*
 * `"a{x}b{y}c"` - string interpolation, Implementation-Storage.md part 8.
 *
 * **One allocation and a run of appends**, and every one of the document's three strategies is what
 * the *existing* passes then make of that one shape rather than a path of its own:
 *
 *  1. the literal segments are known now, so their total is a constant;
 *  2. each hole contributes `formatBound(showBound(v))`, which is that hole's own promise about how
 *     much text it will write and is zero where it declines to make one;
 *  3. `String.ofCapacity(sum)`, then the literals and the holes appended in written order.
 *
 * So a format all of whose bounds are constants has a constant extent and folds to a buffer of a
 * known size; one whose bounds are values allocates that many units; and one with an unbounded hole
 * is seeded with the bounds that *are* known and grows past it through the library's own append.
 * Nothing here tells them apart, which is the argument the document makes.
 *
 * **Four library declarations and no compiler knowledge of what a string is.** `Show` is the class a
 * hole dispatches through, `showBound` and `show` are its two members, and `formatBound`,
 * `String.ofCapacity` and `String.push` are the three functions - all five named through
 * `known.def` rather than by spelling, so a library that declares none of them gets one message at
 * the literal instead of a compiler that cannot be built against it. That is what lets a library
 * without a formatter be a smaller library rather than a broken one.
 *
 * **The order is left to right and each hole is evaluated exactly once.** A hole is resolved into a
 * place (`evaluatedPlace`) before anything is measured, because it is read *twice* - once for its
 * bound and once for its text - and resolving it twice would run its expression twice. A hole may
 * hold any expression, braces included: the parser ends a hole at the `}` matching its own opening,
 * so `"{{v | .a.b: 1}.a}"` arrives here as one expression and is resolved as one.
 *
 * The hole comes before the text of its own chunk, and that is the parser's record rather than the
 * written order: `parseStringExpr` opens with the leading text and then pushes one chunk per hole
 * holding *that hole's expression and the text following it*. Appending a chunk's text first
 * renders `"n={7}!"` as `n=!7`, which is a wrong string of the right length.
 */
Node* Resolver::resolveFormat(Builder& builder, ast::Expr& expr) {
    auto klass = program.roles.klass(intrinsic::Known::show);
    auto sized = program.roles.function(intrinsic::Known::stringOfCapacity);
    auto pushed = program.roles.function(intrinsic::Known::stringPush);
    auto bounded = program.roles.function(intrinsic::Known::formatBound);

    /*
     * The library's four, checked here rather than at each use - a format needs all of them, and a
     * program written against a library with none should be told once which piece is missing rather
     * than at the first hole that happens to reach one.
     */
    if(!klass) {
        reportMissingRole(intrinsic::Known::show, "a string interpolation"_v, expr.source);
        return nullptr;
    }

    if(!sized || !pushed || !bounded) {
        reportMissingRole(!sized   ? intrinsic::Known::stringOfCapacity
                        : !pushed  ? intrinsic::Known::stringPush
                                   : intrinsic::Known::formatBound,
                          "a string interpolation"_v, expr.source);
        return nullptr;
    }

    auto& allocate = *decl()[sized];
    auto& append = *decl()[pushed];
    auto sizeType = decl()[allocate.args.get(decl(), 0)]->occurrence.type;

    /*
     * Every hole, in written order and each as a place - see the header, where evaluating one once
     * and reading it twice is argued.
     *
     * The literal units are counted on the same walk, because they are what the buffer is seeded
     * with and the walk already has the chunk in hand.
     */
    struct Hole {
        Place* place = nullptr;
        StringId text;
    };

    SmallArray<Hole, 8> holes;
    U64 literalUnits = 0;

    for(auto chunk: expr.format.contents(parse)) {
        Hole hole;
        hole.text = chunk.string;

        if(chunk.string) literalUnits += U64(context.findName(chunk.string).size());

        if(chunk.format) {
            hole.place = evaluatedPlace(builder, *parse[chunk.format]);
            if(!hole.place) return nullptr;
        }

        holes.push(hole);
    }

    // Step 1's constant, at the width the allocation declares rather than at the target word - the
    // role's signature is the one source of truth about what its argument is, which is the same rule
    // `textValue` follows for `stringLiteral`'s count.
    auto total = builder.addConst(expr.source, intConstant(sizeType, literalUnits));
    if(!total) return nullptr;

    // The running sum crosses whatever control flow a hole's own expression builds - `&&` in one,
    // a `?`, a `match` - so it is held the way every other value spanning a construct is.
    HeldValue heldTotal(*this, total);

    for(auto& hole: holes) {
        if(!hole.place) continue;

        /*
         * `formatBound(showBound(v))` - the hole's promise, reached the way an ordinary call to
         * either would be.
         *
         * The hole's type is this body's own variable wherever the format is written in a generic
         * function, which is where an interpolation is most worth writing: a constrained `show`
         * instance is one of these, and the alternative by hand is longer and one allocation per
         * piece. `applyMember` selects at the type or defers to the requirement exactly as a written
         * `showBound(v)` would.
         */
        auto value = readPlace(builder, expr.source, *hole.place, bindingRequest(Access::Shared));
        if(!value) return nullptr;

        Node* measured[1] = { value };
        auto bound = applyMember(builder, intrinsic::Known::show, 1, measured, 1, expr.source);
        if(!bound) return nullptr;

        Node* of[1] = { bound };
        auto units = callFunction(builder, *decl()[bounded], of, 1, expr.source);
        if(!units) return nullptr;

        units = coerce(builder, *units, sizeType, expr.source, "a hole's size"_v);
        if(!units) return nullptr;

        total = applyBinary(builder, expr.source, BinaryOp::Add, *total, *units);
        if(!total) return nullptr;
    }

    auto extent = coerce(builder, *total, sizeType, expr.source, "a format's size"_v);
    if(!extent) return nullptr;

    /*
     * The buffer, from the library's own allocator - and the bound is **not** a number here.
     *
     * Worth stating, because J46 §3.4 planned the opposite. The header's step 1 says the literal
     * segments are counted at resolve time, which they are; what it assumed on top of that is that
     * a hole's `formatBound(showBound(v))` is a number here too. It is not: `showBound` is a class
     * member whose body this stage does not run - the argument is a value rather than a constant -
     * so the sum is an expression at this point and becomes 54 in `resolve_ssa/opt/fold.cpp`, one
     * tier down. §2.4's `%132: Size trivial = const 54` is that fold, and it is where the number
     * first exists.
     *
     * So the buffer cannot be emitted here, and what asks the placement question instead is the
     * normalization in `place.cpp`: a `runSlots` whose count has folded to a constant becomes the
     * instruction a literal builds directly, in the frame the call was inlined into. That is the
     * same answer arrived at one tier later, and it is the tier that has the number.
     */
    Node* room[1] = { extent };
    auto sink = callFunction(builder, allocate, room, 1, expr.source);
    if(!sink) return nullptr;

    /*
     * The buffer's own storage, and a **writable** local - which is exactly what
     * `let &sink = String.ofCapacity(n)` compiles to and is written out here for the same reason
     * that line would have been. Every append takes a `&`, and a borrow is writable only where the
     * place it names is; `hold` reads the permission off the value's own contract and an owned
     * string's says nothing about the storage holding it.
     */
    auto local = builder.addLocal(StringId(), valueOccurrence(*sink), expr.source, true);
    auto into = builder.localPlace(*local);
    builder.addWrite(expr.source, *into, *sink, Destination::Init);

    /*
     * A fresh borrow per append rather than one held across all of them.
     *
     * The buffer is a local of this frame, so a borrow costs nothing, and taking one per call keeps
     * each loan's extent to the call that uses it - which is what lets a hole's own expression
     * borrow whatever it likes in between.
     */
    auto borrow = [&](Arg& parameter) {
        return readPlace(builder, expr.source, *into, requestOf(parameter.occurrence));
    };

    auto& writer = *decl()[decl()[klass]->members.get(decl(), 0)];

    for(auto& hole: holes) {
        if(hole.place) {
            auto value = readPlace(builder, expr.source, *hole.place, bindingRequest(Access::Shared));
            if(!value) return nullptr;

            auto to = borrow(*decl()[writer.args.get(decl(), 1)]);
            if(!to) return nullptr;

            // The sink is second here and first in `String.push` below, and that is not a detail:
            // `show(value: a, &to: String)` and `push(&self: String, other: String)` take them in
            // opposite orders, and one order for both is a call whose arguments are swapped.
            Node* written[2] = { value, to };
            if(!applyMember(builder, intrinsic::Known::show, 0, written, 2, expr.source)) {
                return nullptr;
            }
        }

        if(hole.text) {
            auto to = borrow(*decl()[append.args.get(decl(), 0)]);
            if(!to) return nullptr;

            auto literal = textValue(builder, hole.text, expr.source);
            if(!literal) return nullptr;

            Node* arguments[2] = { to, literal };
            if(!callFunction(builder, append, arguments, 2, expr.source)) return nullptr;
        }
    }

    // The finished string, moved out of the storage it was built in - the same read a growable array
    // literal ends with, and the same reason: what the position receives is the temporary rather
    // than a borrow of a local that is about to end.
    return readPlace(builder, expr.source, *into, requestOf(ownedContract()));
}

Node* Resolver::resolveValue(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, Request wanted) {
    /*
     * The top of a chain holding a `?.`, which is where the join those skip to has to be set up.
     *
     * Ahead of the switch because a `?.`'s extent is the rest of *its chain*, and a chain is a spine
     * of these four node kinds rather than any one of them - `a?.b.c(x)` tops out at a call and
     * `a?.b` at the `?.` itself. Entering here is what lets everything below resolve as the ordinary
     * chain it is, with the `?.` nodes finding the join through `optionalChain`.
     *
     * `onOptionalSpine` is what stops this re-entering the chain it is already resolving, and what
     * makes a chain written inside one's arguments its own.
     */
    switch(expr.kind) {
        case ast::Expr::Field:
        case ast::Expr::Unwrap:
        case ast::Expr::App:
        case ast::Expr::Sub:
            if(!onOptionalSpine(expr) && chainSkips(expr)) {
                return resolveOptionalChain(builder, expr, hint);
            }

            break;
        default:
            break;
    }

    switch(expr.kind) {
        case ast::Expr::Nested: {
            /*
             * Brackets are not a position - `(f(...))` keeps what `f(...)` answers, so the token
             * that says "this position holds an iterator object" has to follow the value through
             * them. See `ObjectScope`, whose token is a source position precisely so that it can be
             * pointed at the expression that is actually filling the position.
             */
            auto& inner = *parse[expr.nested];
            ObjectScope object(*this, objectCallSource == expr.source ? inner.source
                                                                      : objectCallSource);

            return resolveValue(builder, inner, hint, wanted);
        }

        /*
         * A name that is a **const parameter** of the enclosing signature - `n` in
         * `fn (n: I32) width() -> I32 = n`.
         *
         * Before the place walk rather than after it, because a const parameter is not storage: it
         * has no address, nothing may assign to it and `->n` means nothing. It is checked after
         * locals, so a parameter and a const parameter of one name are the local - which is a
         * shadowing rule this stage does not need, since `declareArgs` reports the collision at the
         * declaration where both spellings are visible.
         */
        case ast::Expr::Var: {
            /*
             * A `@lazy` parameter, which is a call rather than a read - `forceDeferred`.
             *
             * First, because it is the one name whose storage does not hold what the name means. The
             * place walk below would hand back the thunk's storage and every position after it would
             * be checked against `() -> T`, which is the type the author did not write.
             */
            // The cursor, in value position - Implementation-Tooling.md §8.2. The expected type is
            // what ranks the answer.
            if(isCursorSentinel(context, expr.var)) {
                captureCompletion(hint);
                return nullptr;
            }

            auto boundHere = false;
            auto local = findLocal(expr.var, &boundHere);
            if(local) recordLocal(expr.source, *local);

            if(local && deferredParameter(*local)) return forceDeferred(builder, *local, expr.source);

            // A binding of this body whose initializer failed - reported there, and the name is
            // taken here whatever else in scope carries it. See `findLocal`.
            if(!local && boundHere) return nullptr;

            if(!local && currentFunction) {
                auto found = findGenVariable(decl(), currentFunction->gen, expr.var);
                if(found && ((VariableType*)decl()[found])->isConst()) {
                    recordReference(context, expr.source, typeVarSymbol(decl(), here(), found),
                                    ((VariableType*)decl()[found])->constType);
                    return builder.addGenValue(expr.source, found);
                }
            }

            /*
             * A name that is a **function**, in a position that wants a value - G7's `fun.value`.
             *
             * After the place walk, deliberately: a local, a capture or a global of the name is
             * storage and a declaration of it is not, so the two never compete for the same
             * spelling - and a capture is asked about in the order `resolvePlace` asks, so an
             * enclosing binding shadows a declaration exactly as a local does. What decides
             * *which* declaration is the position's own type - the arity is part of R1's key, so
             * it is part of what a use selects, and a value position has only the hint to select
             * with. A name used as a value with no expected function type therefore
             * reports rather than guessing an arity, which is the same refusal a call with the wrong
             * number of arguments gets and for the same reason.
             */
            if(!local) {
                auto captured = findCapture(builder, expr.var);
                if(rawLambdaCaptureRejected()) return nullptr;

                if(!captured && !findGlobal(context, program, here(), expr.var, expr.source) &&
                   hasFunctionName(context, program, here(), expr.var)) {
                    return resolveFunValue(builder, expr.var, hint, expr.source);
                }
            }

            return resolveField(builder, expr, wanted);
        }

        case ast::Expr::Field:
            return resolveField(builder, expr, wanted);

        /*
         * `xs[i]` and `xs[a..b]` - one grammar and two things, told apart by what is inside the
         * brackets.
         *
         * An **element** is a place, read through the same `readPlace` a field is. A **range** is
         * not: what it names is a run rather than an object, so there is no storage for it to be a
         * place of and what comes out is a window - a description of part of the same borrow.
         */
        case ast::Expr::Sub: {
            auto& written = *parse[expr.sub];
            if(written.args.size() == 1 &&
               written.args.get(parse, 0).value.kind == ast::Expr::Range) {
                return resolveSubwindow(builder, expr, wanted);
            }

            return resolveField(builder, expr, wanted);
        }

        /*
         * `->x` in a position that wants a value - `doc/spec/expressions.md`: it is grammatically an
         * expression, of type `{}`, and it is written as a statement.
         *
         * The unit is what makes `fn unlock(->guard: Guard) = ->guard` typecheck, which is the whole
         * reason it is a value form at all: an implicit-return body *is* a value position, so a
         * disposal that produced nothing would have refused the one function the mutex example is
         * built out of. `f(->x)` and `let n: I32 = ->x` are still refused, by the ordinary type rule
         * and with the ordinary message.
         */
        case ast::Expr::Dispose: {
            if(!resolveDispose(builder, expr)) return nullptr;
            return builder.addConst(expr.source, unitConstant(program.types.unit));
        }

        /*
         * `yield e` in a position that wants a value - `let step = yield i`.
         *
         * Only a **resuming** iterator has one to give (Design-Iterator-Objects.md §2.5), so this is
         * two answers rather than one: the `yield` of a `-> (Y <- R)` is a value form and every other
         * traversal's is a statement. Here rather than in `resolveStatement` alone, because the two
         * are one walk - the branch the wrapper needs is the same branch either way, and the value is
         * what comes out of it.
         *
         * **Refused here rather than left to the position**, which is the correction J62 makes. This
         * used to hand straight to `resolveYield` on the reasoning that its null answer would be
         * reported by whatever wanted the value, "as it reports any expression that produced
         * nothing" - and nothing reports that. `let step = yield i` in an iterator with no resume
         * channel resolved to no value with no diagnostic, and the statement holding it was dropped:
         * `i = i + step` ceased to exist and the producer looped forever handing over the same
         * element. Which is the failure mode a written site type had once and §6 of
         * `test/bench/findings.md` called the dangerous kind - source that should be refused is
         * accepted, a statement vanishes, and nothing says so.
         *
         * Before the call rather than after it, because `resolveYield` has already built the
         * hand-over by the time it answers: the arm below is the one that knows the position wanted a
         * value, and it is the only one that can say so. The refusals `resolveYield` makes about
         * where a `yield` may appear at all are left to it - a `yield` outside a traversal is not a
         * question about the resume channel - which is what the guard tests for.
         */
        case ast::Expr::Yield: {
            auto traversal = currentFunction;

            if(traversal && traversal->lens && continuationLocal && !traversal->resume.type) {
                if(traversal->iterator) {
                    context.diagnostics.error(
                        "%@ declares no resume channel, so its `yield` hands a value over and answers nothing - write its result `-> (%@ <- R)` for a `yield` that evaluates to what the consumer sent back"_v,
                        expr.source, context.findName(traversal->name),
                        typeText(context, decl(), traversal->handover.type));
                } else {
                    context.diagnostics.error(
                        "a `lens fn` hands over once, so its `yield` is a statement and has no value - only an `iter fn` declared `-> (Y <- R)` has one"_v,
                        expr.source);
                }

                return nullptr;
            }

            return resolveYield(builder, expr, true);
        }

        case ast::Expr::Con: {
            // A nullary constructor of a concrete enum is a value rather than a construction - see
            // `resolveConstructorConstant`, which answers `Nothing` for everything else.
            auto constant = resolveConstructorConstant(expr, hint);
            if(!constant) return resolveConstruction(builder, expr, hint, wanted);
            if(!constant.unwrap().isSet()) return nullptr;

            return builder.addConst(expr.source, constant.unwrap());
        }

        case ast::Expr::Coerce: {
            /*
             * `e :: T` - an ascription, which is a hint and then a widening.
             *
             * **The hint first, and that is most of what `::` does.** Passing the type down makes
             * `1 :: I64` one constant rather than a widened `I32`, picks the instance for
             * `truncate(x) :: Int`, gives an array literal its element type and a lambda its
             * signature - none of which is a conversion at all. `doc/spec/overloading.md`: "`::`
             * selects a target type, and it may widen".
             *
             * What is left when the value still has another type is `convertValue`, at the *written*
             * end of its one rule: a lossless step happens, and a lossy one is reported naming
             * `truncate`. That is the same function every implicit position goes through, which is
             * what stops `::` being the one construct in the language that can quietly remove data -
             * the shipping stage let it narrow, and `length(xs) :: Int` cut a sixty-four-bit length
             * on one target while being free on the other.
             */
            auto& ascription = *parse[expr.coerce];

            /*
             * `@inline(n) @capacity(n) [T]` - read off here and the type resolved without it, the
             * way the `@bits` arm does one level down. This is one of the two positions that honour
             * a container refinement, and `resolveType` reports it anywhere else.
             */
            U64 capacity = 0;
            auto written = ascription.type;
            if(!readContainerRefinement(written, capacity)) return nullptr;
            if(capacity) written.attributes = nullptr;

            auto type = resolveAscribedType(written, ascription.target);
            if(!type) return nullptr;

            // An ascription on a name not written yet is the one thing saying what belongs there.
            if(ascription.target.kind == ast::Expr::Var && isCursorSentinel(context, ascription.target.var)) {
                captureCompletion(type);
                return nullptr;
            }

            /*
             * The request travels down as the surrounding position's, with one thing taken off it:
             * an ascription names a **type**, and a type is not a window.
             *
             * `total([1, 2, 3] :: [Int])` is the case. The parameter holds a window, so the literal
             * written bare builds a `[I32 *3]` and the parameter describes it; written with the
             * ascription it is a statement that this expression is an `Array(Int)`, and what the
             * call does with the array afterwards is the ordinary adaptation every owned container
             * reaching a window position goes through. Passing the form down would make `::` build
             * something that is not the type it names.
             */
            auto asked = wanted;
            asked.form = AccessForm::WholeValue;

            /*
             * The literal built with the bound, which is the one expression a refinement is about.
             * `resolveValue` would reach the same literal with the capacity dropped on the way, so
             * the array arm is called with it rather than the number travelling in a `Request`.
             */
            if(capacity) {
                if(ascription.target.kind != ast::Expr::Array) {
                    context.diagnostics.error(
                        "`@inline(n) @capacity(n)` says how much room an array literal starts with, "
                        "and this is not one"_v, ascription.target.source);
                    return nullptr;
                }

                auto made = resolveArrayLiteral(builder, ascription.target, type, asked, capacity);
                if(!made) return nullptr;

                return convertValue(builder, *made, type, expr.source, false,
                                    "an ascribed value"_v, asked.access, asked.establishes);
            }

            /*
             * `f(...) :: T` keeps what `f(...)` answers, exactly as brackets do - so the object
             * token follows the value through the ascription. `let &it = lex(s) :: iter(Token)` is
             * the spelling that needs it: what fills the `let` is the call, and what says which
             * form it takes is the type written beside it.
             */
            ObjectScope object(*this, objectCallSource == expr.source ? ascription.target.source
                                                                      : objectCallSource);

            auto value = resolveValue(builder, ascription.target, type, asked);
            if(!value) return nullptr;

            /*
             * The position's own convention travels with it, which is what decides where an adapted
             * value's storage goes - `iter(T)` and `(A) -> B` both read it. An ascription states a
             * type and never a position, so what it hands on is the *surrounding* position's
             * answer rather than the owned default.
             *
             * **Both halves of it**, and the second is what stops the first refusing ordinary code.
             * An exclusive position is one a conversion cannot fill, and a `let &x = e :: T` is an
             * exclusive position that is *declaring* its storage - `Request::establishes` - so the
             * converted value is what fills it and there is nothing behind it to write back to. With
             * the access alone, `let &total = n :: I64` was refused for a widening, and `::` is the
             * only way to write a type in a `let`. The array arm above passes the same pair, because
             * the two arms differing is how this was missed.
             */
            return convertValue(builder, *value, type, expr.source, false, "an ascribed value"_v,
                                asked.access, asked.establishes);
        }

        /*
         * `{}` and `{a, b}` - the empty tuple, which is the unit value, and the rest.
         *
         * The empty one is here rather than in `resolveTupleLiteral` because it is not a record at
         * all: a product of nothing has exactly one value, and `{}` is the name the language gives
         * that type - which is what makes `yield {}` in five of `lib/`'s seven lenses the unit it
         * reads as.
         */
        case ast::Expr::Tup: {
            if(expr.tup.size()) return resolveTupleLiteral(builder, expr, hint);
            return builder.addConst(expr.source, unitConstant(program.types.unit));
        }

        // `"a{x}b"` - see `resolveFormat`, which is where the whole of it is.
        case ast::Expr::Format:
            return resolveFormat(builder, expr);

        // `{p | y: 3}` - see `resolveUpdate`, which is where the whole of it is.
        case ast::Expr::TupUpdate:
            return resolveUpdate(builder, expr, hint);

        // `[1, 2, 3]` - see `resolveArrayLiteral`, which is where the whole of it is.
        case ast::Expr::Array:
            return resolveArrayLiteral(builder, expr, hint, wanted);

        case ast::Expr::Infix:
            return resolveInfix(builder, expr, hint, wanted);
        case ast::Expr::Prefix:
            return resolvePrefix(builder, expr, hint);
        case ast::Expr::App:
            return resolveCall(builder, expr, hint);
        case ast::Expr::If:
            return resolveIf(builder, expr, hint, true, wanted);
        case ast::Expr::MultiIf:
            return resolveMultiIf(builder, expr, hint, true, wanted);

        // A pattern in a value position - the test alone, since a name bound here would have no arm
        // to be in scope in. See `resolveIs`.
        case ast::Expr::Is:
            return resolveIs(builder, expr);

        // `x?` and `a?.b` - `resolve_try.cpp`, where both carrier operators are. The second is
        // reached through the chain above and only ever resolved from inside it.
        case ast::Expr::Try:
            return resolveTry(builder, expr, hint, wanted);
        case ast::Expr::Unwrap:
            return resolveUnwrap(builder, expr);

        case ast::Expr::Match:
            return resolveMatch(builder, expr, hint, true, wanted);
        case ast::Expr::Multi:
            return resolveBlock(builder, expr, hint, true, wanted);

        /*
         * `let x = e in body` where a value is wanted - `doc/spec/expressions.md`.
         *
         * The same statement `resolveScoped` builds, asked for what its body answered, and `in` is
         * what makes it a value form at all: a binding whose scope is not written has nothing inside
         * the expression after it, so there is nothing for the expression to be.
         *
         * **One binding.** A `where`-shaped `let a = 1, b = 2` declares two names and answers
         * nothing; it is a statement wherever it stands, and this is the only place that has to say
         * so - the statement form takes any number and reads each in turn.
         */
        case ast::Expr::Decl: {
            if(expr.decl.size() != 1) {
                context.diagnostics.error(
                    "a `let` binding several names is a statement, and this position needs a value"_v,
                    expr.source);
                return nullptr;
            }

            auto& binding = *expr.decl.pointer(parse);

            if(!binding.content) {
                context.diagnostics.error("a local requires an initializer"_v, binding.pat.source);
                return nullptr;
            }

            if(!binding.in) {
                context.diagnostics.error(
                    "a `let` is a statement unless the scope of its names is written - `let x = e in x` is the form that answers"_v,
                    expr.source);
                return nullptr;
            }

            return resolveScoped(builder, binding, hint, true, wanted);
        }

        // A lambda - `resolve_lambda.cpp`, where the whole of it is.
        case ast::Expr::Fun:
            return resolveLambda(builder, expr, hint, wanted);

        case ast::Expr::Map:
            return resolveMapLiteral(builder, expr, hint);

        // What the parser left where it reported - see `parseErrors`.
        case ast::Expr::Error:
            parseErrors++;
            return nullptr;

        default:
            // A literal is a call to `FromInt` - `literalValue`, which is where the fold to a
            // constant at a primitive is argued.
            if(expr.kind >= ast::Expr::Lit) return literalValue(builder, expr, hint, false);

            unsupported(expr.source, "this expression"_v);
            return nullptr;
    }
}

/*
 * A call through a function value - `f(x)` where `f` is a local.
 *
 * The value's *type* is the signature, so what is checked here is what a declaration would have
 * answered: that it is callable at all, that the arity is the one written, and that each argument
 * fits the position. There is nothing to look up and nothing to infer - a function type carries no
 * variables of its own - which is why this is short where `resolveCall` is not.
 */
Node* Resolver::resolveValueCall(Builder& builder, ast::AppExpr& written, Node& target,
                                 LocationId source) {
    auto type = valueType(target);

    /*
     * A **site type**, whose signature is the lambda's and whose call is a direct one -
     * Design-Iterator-Objects.md §4, built 2026-09-15.
     *
     * The same walk with one thing read differently: the shape comes from the record rather than
     * from the value's own type, because a site type *names* a signature where a function type *is*
     * one. Everything after that line - the arity, the positions, the conversions, the carried
     * origins - is the same statement about the same shape, which is what makes calling a closure
     * inline and calling a pair one construct rather than two.
     *
     * What the node records is the target's type, and `callTarget` is where the difference is spent:
     * a site type answers the lifted function and a function type answers "through the value". So a
     * body written over a type variable holds one node and both answers, which is what lets a record
     * generic in its callback be specialized at either.
     */
    auto site = siteTypeOf(type);
    auto signature = callableShapeOf(program, currentFunction ? currentFunction->gen : nullptr,
                                     type);

    if(!signature || decl()[signature]->kind != Type::Fun) {
        context.diagnostics.error("this is %@, which is not something that can be called"_v,
                                  written.callee.source, typeText(context, decl(), type));
        return nullptr;
    }

    /*
     * The callee, given a place where it is an owner this expression made.
     *
     * The fourth position that produces an owner and does not consume it - see `hold`, whose list
     * names the other three. `pick(1)(21)` calls a function value that is a *call result*: the call
     * reads its two words and hands nothing over, so the value is still this frame's afterwards and
     * still owes a teardown, and a node result has nowhere for one to be owed from. Without the
     * place the callee was an owned value named by an inspecting use, which `Editor::use` refuses by
     * name - a use with no obligation attached to a value that has one.
     *
     * A function value with nothing to release keeps its node, which is `holdBorrowed`'s own test
     * and the reason the common case costs nothing.
     *
     * At a **site type** the request is the environment parameter's rather than an inspection. The
     * pair is read and not held to - a call takes two words out of it - and a site type *is* the
     * storage the callee reads its captures out of, so it is handed over the way that parameter asks
     * for it, which for an owner this expression made means the same frame slot a borrowing argument
     * of any other record gets.
     */
    auto envParameter = site ? &decl()[decl()[site->lambda]->args.get(decl(), 0)]->occurrence
                             : nullptr;

    auto& callee = *holdBorrowed(builder, target,
                                 envParameter ? requestOf(*envParameter) : inspectRequest(),
                                 type, source);

    auto& shape = *(FunType*)decl()[signature];
    if(written.args.size() != shape.params.size()) {
        context.diagnostics.error("this takes %@ arguments and %@ were written"_v, source,
                                  U64(shape.params.size()), U64(written.args.size()));
        return nullptr;
    }

    SmallArray<Node*, 8> arguments;
    auto index = U32(0);

    for(auto argument: written.args.contents(parse)) {
        if(argument.name != StringId()) {
            unsupported(argument.value.source, "a named argument"_v);
            return nullptr;
        }

        auto declared = shape.params.get(decl(), index++).occurrence;
        auto value = resolveValue(builder, argument.value, declared.type, requestOf(declared));
        if(!value) return nullptr;
        value = coerce(builder, *value, declared.type, argument.value.source, "an argument"_v,
                       declared.contract.access());
        if(!value) return nullptr;

        if(!checkCarriedOrigins(*value, declared, argument.value.source, "this parameter"_v)) {
            return nullptr;
        }

        arguments.push(value);
    }

    return builder.addCallValue(source, callee, signature, arguments.pointer(), arguments.size());
}

/*
 * A named function as a value - `let f: (I32) -> I32 = add`.
 *
 * The arity comes from the position and from nowhere else, which is the note at the call site above.
 * Once it is known the declaration is a lookup like any other, and what is checked here is the one
 * thing a use site can get wrong that a call cannot: the *conventions*. A call binds each argument
 * against the parameter it fills, so a mismatch there is a message about that argument; a value has
 * to accept every call the position will make, so what is compared is the whole shape at once.
 */
Node* Resolver::resolveFunValue(Builder& builder, StringId name, DeclPtr<Type> hint,
                                LocationId source) {
    if(!hint || decl()[hint]->kind != Type::Fun) {
        context.diagnostics.error(
            "%@ is a function, and a value of one needs a position that says which arity - write the type"_v,
            source, context.findName(name));
        return nullptr;
    }

    auto& wanted = *(FunType*)decl()[hint];
    // The arity, which an open position still states - `isOpenFunPosition` leaves the *result*
    // open and nothing else, so a position may reach here undecided and still select a declaration.
    OverloadSet set;
    findOverloads(context, program, here(), name, U32(wanted.params.size()), source, set);

    /*
     * R5's order, and here it is the whole of selection: a class function is generic in its class's
     * own argument, so `funTypeOf` below declines every one of them - the plain half of a set is the
     * only member a value could ever point at. Whether it *fits* is the type check further down,
     * which is where a position wanting a different signature is told so.
     *
     * So this is not overloading on types at a value position. It is that of R1's set exactly one
     * member can be a value, and the rest are refused by the reason they are refused for a set of
     * one.
     */
    auto found = set.plain ? set.plain : set.sole();

    if(!found) {
        if(set.isEmpty()) {
            context.diagnostics.error("no %@ takes %@ arguments"_v, source, context.findName(name),
                                      U64(wanted.params.size()));
        } else {
            // Several class functions and no plain one. Not "no such function", which is what the
            // message above would have said: they exist, and none of them is a body to point at.
            context.diagnostics.error("%@ at %@ arguments is declared by %@ classes, and a function value names one body - a class function is chosen by the types at a call"_v,
                                      source, context.findName(name),
                                      U64(wanted.params.size()), U64(set.members.size()));
        }

        return nullptr;
    }

    auto& callee = *decl()[found];
    recordCallee(source, found);

    if(wanted.convention && (!callee.convention ||
                             callee.convention.unwrap() != wanted.convention.unwrap())) {
        context.diagnostics.error("%@ has calling convention %@, and this position takes %@"_v,
                                  source, context.findName(callee.name),
                                  nameForCallType(wanted.convention.unwrap()));
        return nullptr;
    }

    // What the signature says it answers, which for an `=` form with no result type is what its body
    // says - and `funTypeOf` below is about to read it. The other position that reads a signature
    // without calling it; `checkCallable` is the one every call goes through.
    if(!requireResult(callee, source)) return nullptr;

    if(!callee.summary.bodyKnown) {
        unsupported(source, "a value of a function with no body"_v);
        return nullptr;
    }

    auto type = funTypeOf(context, program, found);
    if(!type) {
        // A generic function, a lens and an `iter fn` are what `funTypeOf` declines, and they
        // decline for one reason: none of them names a single body a value could point at. An
        // iterator's value form is `iter(T)`, which is a type rather than a function type.
        unsupported(source, callee.lens     ? "a value of a lens"_v :
                            callee.iterator ? "a value of an `iter fn` - write `iter(T)`"_v
                                            : "a value of a generic function"_v);
        return nullptr;
    }

    auto wantedType = closeFunPosition(context, program, hint,
                                       ((FunType*)decl()[type])->result.type);

    if(type != wantedType) {
        context.diagnostics.error("%@ is %@, and this position takes %@"_v, source,
                                  context.findName(name), typeText(context, decl(), type),
                                  typeText(context, decl(), hint));
        return nullptr;
    }

    // No environment: a named function either becomes a direct raw code address or the ordinary
    // closure pair. The body is the one decided for the whole program - a value has no type
    // arguments to ask a replacement's constraints with.
    return builder.addFunValue(source, replacementForValue(found), type, nullptr);
}

/*
 * A value reaching a **view** position - the one adaptation site, Analysis-Manifestations.md
 * Part 8's `adaptToView`.
 *
 * Three things can arrive, and each is answered by something the view table names rather than by
 * this function knowing a layout:
 *
 *  - **A view already.** `f(w)` where `w` is a window is the same logical type and the same form,
 *    so the value passes through untouched - which is what makes a window-taking function callable
 *    from another one for free.
 *  - **A view record**, built by a body - `ArrayView {items: ..., length: ...}` at a `'[a]` result.
 *    The record is the two words the position holds, so it is *restated* as the view, without
 *    moving a byte: `Builder::addRepresent`, the only way a view is ever built out of its fields,
 *    and the only position a view record may fill.
 *  - **An owner**, or a fixed array, which is the case with a derivation: the class member the
 *    view table names for the position's access is called, and what comes back is the view rooted
 *    in the argument by that member's own signature. `Contiguous.elements` and
 *    `Writable.elementsMut` serve `Array(a)` and `[a *n]` alike, because the instances are
 *    ordinary instances; `Viewed.view` serves a type whose view has no spelling.
 *
 * Anything else falls through to `coerce`, whose message names the two types - which is the right
 * report for `f(3)` at a `[I32]` position.
 */
Node* Resolver::adaptToView(Builder& builder, Node& value, const Occurrence& position,
                            LocationId at) {
    if(position.contract.form() != AccessForm::View) return &value;

    auto held = valueOccurrence(value);
    auto access = position.contract.access();

    if(sameType(held.type, position.type) && held.contract.form() == AccessForm::View) {
        return &value;
    }

    if(viewRowOfRecord(decl(), program, held.type)) {
        if(viewRecordOf(context, program, position) != held.type) return &value;
        return builder.addRepresent(at, value, Occurrence { position.type, position.contract, nullptr });
    }

    auto row = viewRowOf(decl(), program, position.type);
    if(!row) return &value;

    if(decl()[held.type]->kind == Type::Array) {
        // A fixed array is a donor only where the elements agree - `[I64 *2]` at a `[I32]` position
        // is the ordinary mismatch, and the message that names both types is the one to give.
        auto& array = *(ArrayType*)decl()[held.type];
        if(!sameType(array.element, elementTypeOf(decl(), program, position.type))) return &value;
    } else if(!sameType(held.type, position.type)) {
        return &value;
    }

    auto adaptor = access == Access::Unique ? row->uniqueAdaptor : row->sharedAdaptor;
    auto member = access == Access::Unique ? row->uniqueMember : row->sharedMember;

    if(adaptor == intrinsic::Known::Count) {
        context.diagnostics.error("%@ has no %@ view, so nothing can be adapted to this position"_v,
                                  at, typeText(context, decl(), position.type), accessName(access));
        return nullptr;
    }

    /*
     * The derivation reaching **itself** - a body that asks for the view it is the definition of.
     *
     * `fn elements(self: source'Array(a)) -> source'[a] = unclampedSlice(self, 0, ...)` is that
     * program: the argument holds a window, the subject is the owner, and the only way an owner
     * becomes a window is this member - so the call this function would insert is a call to the
     * body it is inserting it into, and the answer at run time is a stack that never returns.
     *
     * Reported rather than accepted, because nothing further down can catch it: the recursion
     * type-checks, lowers and links, and the first sign of it is a segfault in a program that only
     * read an array. What the body has to do instead is *build* the view record - `ArrayView
     * {items: ..., length: ...}`, the `addRepresent` case above - which is what a derivation is.
     *
     * The same type and the same access, which is what makes this the recursion and not a
     * neighbour: a container whose `elements` derives a *different* container's view is an ordinary
     * call, and so is `elements` reaching the unique derivation, since that is the other member.
     *
     * **The question is asked of both ends since 2026-09-21 (J67)**, and until then it was asked of
     * one. The test compared the position against the member's *argument* type, which are the same
     * type only because a window and the container it is over are one logical type - so an
     * owner-subject instance was caught by accident and a fixed array's was not:
     * `Contiguous([a *n], a)` has `[a *n]` at the argument and `[a]` at the position, so `elements`
     * calling `unclampedSlice(self, ...)` on JavaScript slipped past and recursed until the stack
     * ran out (J66 §6). The value's type against the *argument* and the position against the
     * *result* is the statement the body is actually making, and it catches both.
     *
     * Turning it on needed `Container.ownership.js.yana`'s bodies first, which build the view record
     * out of `hostRow(self)` rather than asking for one - otherwise the report is the library
     * failing to compile for that target at all. The two changes are one change.
     */
    if(currentFunction && currentFunction->derivation && currentFunction->args.size() &&
       sameType(held.type, decl()[currentFunction->args.get(decl(), 0)]->occurrence.type) &&
       sameType(position.type, currentFunction->result.type) &&
       currentFunction->result.contract.access() == access) {
        context.diagnostics.error(
            "this is the body that makes %@'s view, so it cannot ask for one - build the view "
            "record out of the fields the owner holds"_v,
            at, typeText(context, decl(), position.type));
        return nullptr;
    }

    Node* operands[1] = { &value };
    auto derived = applyMember(builder, adaptor, member, operands, 1, at, position.type);

    // What the loan checker's element exemption follows through - `NodeCall::derivation`.
    if(derived && derived->kind == Node::Call) ((NodeCall*)derived)->derivation = true;
    return derived;
}

/*
 * What an array literal builds at a fixed-array hint - the element type, the length, and whether
 * the one element written fills the whole thing.
 *
 * Shared by the two readings of `[1, 2, 3]`, because the rules are the *language's* and not either
 * reading's: the count has to match or be one, a length that is a const parameter cannot be stated
 * by a literal, and a fill has to be duplicable. The value path builds an aggregate node out of the
 * answer and the constant path an aggregate constant, and a rule stated in each of them is a rule
 * the two can come to disagree about - which for the fill would be `n` owners of one object on one
 * path and a diagnostic on the other.
 *
 * The hint is required to be a fixed array **before** this is called, and that is the one question
 * left to the callers: a value has nowhere else to go and reports the missing ascription, and a
 * constant is simply not this form and lets the position say what it says about any other.
 */
Maybe<ArrayLiteralShape> Resolver::arrayLiteralShape(ast::Expr& expr, DeclPtr<Type> hint,
                                                     Size written) {
    auto& array = *(ArrayType*)decl()[hint];

    /*
     * A **fill** at a quantified count - `[0] :: [U8 *n]`, `lib/Digest/Digest.yana`'s digest buffer.
     *
     * Legal, and the refusal this replaces was right about the wrong half. *Stating* a length a
     * const parameter holds is what a literal cannot do - `[1, 2, 3] :: [U8 *n]` claims `n` is
     * three - but "every slot is this value" claims nothing about the count, so it is exactly as
     * well-formed here as at a written number and means the same thing. What it costs is that the
     * component list cannot be spread now: the number arrives with the specialization, which is
     * what `NodeAggregate::fill` carries it to.
     */
    if(!array.hasFixedLength()) {
        if(written != 1) {
            context.diagnostics.error(
                "this array's length is a const parameter, so a literal can only fill it - write one element, which every slot then holds"_v,
                expr.source);
            return Nothing();
        }

        if(!isTriviallyCopyable(decl(), array.element)) {
            context.diagnostics.error(
                "one element fills the whole array, and %@ cannot be duplicated - write every element, "
                "or give each its own value"_v,
                expr.source, typeText(context, decl(), array.element));
            return Nothing();
        }

        return Just(ArrayLiteralShape { array.element, 0, true, true });
    }

    auto length = array.length;
    auto fill = written == 1 && length != 1;

    if(!fill && written != length) {
        context.diagnostics.error(
            "this literal has %@ elements and %@ holds %@ - one element fills the whole array, and "
            "any other count has to be written out"_v,
            expr.source, U64(written), typeText(context, decl(), hint), length);
        return Nothing();
    }

    /*
     * The fill case, and the one rule that makes it legal.
     *
     * `[0] :: [Int *8]` is eight zeroes, and what makes that sound is that an `Int` may be
     * duplicated: `doc/spec/ownership.md`'s `TrivialCopy`. An element that owns something would be
     * `n` owners of one object - which is precisely what `newArray` takes a function for - so it is
     * refused here, naming the type rather than the literal.
     */
    if(fill && !isTriviallyCopyable(decl(), array.element)) {
        context.diagnostics.error(
            "one element fills the whole array, and %@ cannot be duplicated - write every element, "
            "or give each its own value"_v,
            expr.source, typeText(context, decl(), array.element));
        return Nothing();
    }

    return Just(ArrayLiteralShape { array.element, length, fill });
}

/*
 * `[1, 2, 3]` at a position that owns a **growable** array - `doc/spec/collections.md`'s `[a]`.
 *
 * The element type and the ordering are this function's; the container is `buildGrowableArray`'s,
 * below, which asks for the storage as one instruction and writes the two records over it. That
 * used to be two library calls - `arrayLiteral` for an array of `count` live slots and
 * `elementBase` for where those slots start - and `known.def`'s note above `runSlots` is where the
 * argument for removing them is.
 *
 * What both arrangements have over the shipping stage is that the *field list* comes from the
 * library's own records rather than being spelled at every literal: a library that laid a run out
 * differently fails to build here and miscompiles there. What the roles bought over that was one
 * fewer reader of the field order, and the read had to happen anyway - see `RunShape`.
 *
 * **The elements are resolved before the array is made**, which is the same ordering the map literal
 * has and is here for both of its reasons. The written expressions run left to right exactly once,
 * before anything of the container's own runs; and the first of them is what says what the array
 * holds when the position did not. The values stay live across the allocation and are handed over
 * one at a time below, which is what the temporaries are for.
 *
 * The writes are through a **raw pointer**, exactly as the library's own `push` writes through one.
 * That is what makes the hand-over a hand-over: a store through `%a` is outside the borrow model by
 * construction, so the frame that owned element `i` stops owing it a teardown on the path that
 * stored it, and the array's own `Reclaim` walk over `length` elements is what runs them afterwards.
 * The window in which the length promises values that are not there yet contains no program of the
 * author's: `buildGrowableArray` writes every slot immediately, in index order, with nothing of the
 * program's own running in between.
 */
Node* Resolver::resolveGrowableLiteral(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint,
                                       U64 capacity) {
    auto written = expr.arr.contents(parse);
    auto element = elementTypeOf(decl(), program, hint);

    SmallArray<Node*, 8> components;
    HeldList<SmallArray<Node*, 8>> held(*this, components);

    for(Size i = 0; i < written.size(); i++) {
        auto& item = *written.pointerAt(i);

        // Owned, which is what an element position is: an array's elements are its storage, so a
        // component fills one the way a field of a record does.
        auto value = resolveValue(builder, item, element, requestOf(ownedContract()));
        if(!value) return nullptr;

        /*
         * The first element decides where the position did not, and every one after it is coerced
         * to what it decided - `doc/spec/expressions.md`'s `[2, 3, 5, 7]` being `[Int]`.
         *
         * `coerce` and not a comparison, because that is what makes a mixed-width literal say what
         * is wrong with it: `[1, 2 :: I64]` is a widening the language performs everywhere else, and
         * `[1, "two"]` is the ordinary mismatch naming both types.
         */
        if(!element) {
            element = valueType(*value);
        } else {
            value = coerce(builder, *value, element, item.source, "an element"_v);
            if(!value) return nullptr;
        }

        components.push(value);
    }

    if(!element) {
        context.diagnostics.error(
            "cannot tell what this empty array holds - give it a type, as `[] :: [Int]`"_v,
            expr.source);
        return nullptr;
    }

    return buildGrowableArray(builder, element, components, expr.source, capacity);
}

/*
 * `["a": 1, "b": 2]` and `[:]` - `doc/spec/collections.md`'s `[K: V]`, and Implementation-Map.md §7.
 *
 * Not built in place the way an array literal is, and the difference is what makes this function
 * short: an array's elements *are* its representation, so the literal writes the run itself; a map's
 * are an insertion order, a probe and an index, none of which the compiler knows or should. So the
 * literal is `known.def`'s `mapLiteral` - `newMap(n)`, the one construction site where the entry
 * count is a compile-time constant - and one `IndexInsert.insertAt` per pair, left to right, which
 * is the same member `m[k] = v` reaches. No field of `Map` is read here, exactly as none of `Array`
 * is one function above.
 *
 * **The entries are resolved before the map is made**, which is the array literal's ordering and is
 * here for both of its reasons: the written expressions run left to right exactly once, and the
 * first pair is what says what the map holds when the position did not.
 *
 * `insertAt` and not `insert`, which is the shipping stage's argument said again: `insert` hands
 * back the value it replaced, so a literal built out of it would drop a `Maybe(v)` at every entry.
 * Two equal keys are therefore the assignment's rule and not a new one - the later entry wins.
 */
Node* Resolver::resolveMapLiteral(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint) {
    auto source = expr.source;

    auto declared = program.roles.record(intrinsic::Known::map);
    if(!declared) {
        reportMissingRole(intrinsic::Known::map, "a map literal"_v, source);
        return nullptr;
    }

    /*
     * What the position says the map holds, where it is a map at all.
     *
     * Both types together, because a map's two parameters are decided together: an ascription
     * naming `Map(String, Int)` settles the key and the value, and one naming anything else settles
     * neither and is left to the entries and then to the ordinary mismatch at the binding.
     */
    DeclPtr<Type> key = nullptr;
    DeclPtr<Type> value = nullptr;

    if(hint && decl()[hint]->kind == Type::Record) {
        auto& record = *(RecordType*)decl()[hint];

        if(record.instanceOf == declared && record.arguments.size() == 2) {
            key = record.arguments.get(decl(), 0);
            value = record.arguments.get(decl(), 1);
        }
    }

    struct Pair { Node* key; Node* value; };
    SmallArray<Pair, 8> pairs;

    // One list for the hold, since both halves of every pair have to stay live across the
    // construction call - `HeldList` is over one array and the pairs are two.
    SmallArray<Node*, 8> live;
    HeldList<SmallArray<Node*, 8>> held(*this, live);

    for(auto entry: expr.map.contents(parse)) {
        // Owned, which is what both positions are: an insert stores the key as well as the value,
        // and storage that outlives the call cannot be borrowed - `IndexInsert`'s own note.
        auto written = resolveValue(builder, entry.key, key, requestOf(ownedContract()));
        if(!written) return nullptr;

        if(!key) key = valueType(*written);
        else {
            written = coerce(builder, *written, key, entry.key.source, "a key"_v);
            if(!written) return nullptr;
        }

        auto made = resolveValue(builder, entry.value, value, requestOf(ownedContract()));
        if(!made) return nullptr;

        if(!value) value = valueType(*made);
        else {
            made = coerce(builder, *made, value, entry.value.source, "a value"_v);
            if(!made) return nullptr;
        }

        live.push(written);
        live.push(made);
        pairs.push(Pair { written, made });
    }

    if(!key || !value) {
        context.diagnostics.error(
            "cannot tell what this empty map holds - give it a type, as `[:] :: [String: Int]`"_v,
            source);
        return nullptr;
    }

    DeclPtr<Type> arguments[2] = { key, value };
    auto mapType = instantiateRecord(context, program, declared, arguments, 2, source);
    if(!mapType) return nullptr;

    /*
     * The map, at the literal's own count.
     *
     * `newMap` names neither `k` nor `v` in its arguments, so the result position is the only thing
     * that decides them - the same rule `Nothing :: Maybe(I32)` is selected by, and why the type is
     * built above rather than read back off the call.
     */
    auto count = builder.addConst(source, intConstant(program.types.words[1], U64(pairs.size())));
    if(!count) return nullptr;

    Node* made[1] = { count };
    auto map = callKnown(builder, intrinsic::Known::mapLiteral, made, 1, source, mapType);
    if(!map) return nullptr;

    /*
     * Held in a writable local, because `insertAt` takes a `&`: a call result is a value and a
     * `&` argument names storage. The same local the array literal builds its slots through, and
     * writable for the same reason - it is this frame's temporary and the literal is still filling
     * it.
     */
    auto local = builder.addLocal(StringId(), valueOccurrence(*map), source, true);
    auto place = builder.localPlace(*local);
    builder.addWrite(source, *place, *map, Destination::Init);

    for(auto& pair: pairs) {
        auto self = readPlace(builder, source, *place, bindingRequest(Access::Unique));
        if(!self) return nullptr;

        Node* operands[3] = { self, pair.key, pair.value };
        if(!applyMember(builder, intrinsic::Known::indexInsert, 0, operands, 3, source)) {
            return nullptr;
        }
    }

    // The map itself, moved out of the local - what makes the literal the owned temporary the
    // surrounding position receives rather than a borrow of a local that is about to end.
    return readPlace(builder, source, *place, requestOf(ownedContract()));
}

/*
 * The shapes an array literal builds, or nothing where the library declares neither record.
 *
 * Both instantiated at the element together, because a literal needs both and a stage that had one
 * of them could not build anything: the run is what the slots are described by and the array is what
 * the position receives.
 */
Maybe<RunShape> arrayShape(Resolver& self, DeclPtr<Type> element, LocationId source) {
    auto arrayRole = self.program.roles.record(intrinsic::Known::array);
    if(!arrayRole) {
        self.reportMissingRole(intrinsic::Known::array, "a growable array literal"_v, source);
        return Nothing();
    }

    auto runRole = self.program.roles.record(intrinsic::Known::run);
    if(!runRole) {
        self.reportMissingRole(intrinsic::Known::run, "a growable array literal"_v, source);
        return Nothing();
    }

    RunShape shape;
    shape.array = instantiateRecord(self.context, self.program, arrayRole, &element, 1, source);
    shape.run = instantiateRecord(self.context, self.program, runRole, &element, 1, source);
    if(!shape.array || !shape.run) return Nothing();

    /*
     * The field types, checked for arity here so that the aggregates below cannot be built against a
     * record with a different field list. A library free to declare a four-field `Run` is a library
     * this stage declines to build a literal for, which is a diagnostic rather than a miscompile.
     */
    auto& runRecord = *(RecordType*)self.decl()[shape.run];
    auto& arrayRecord = *(RecordType*)self.decl()[shape.array];

    if(runRecord.fields.size() != 3 || arrayRecord.fields.size() != 2) {
        self.context.diagnostics.error(
            "this library's `Run` and `Array` are not the two records an array literal is built out of"_v,
            source);
        return Nothing();
    }

    shape.capacity = runRecord.field(self.decl(), RunShape::kCapacity).occurrence.type;
    shape.flag = runRecord.field(self.decl(), RunShape::kOwnsHeap).occurrence.type;
    shape.length = arrayRecord.field(self.decl(), RunShape::kLength).occurrence.type;

    return Just(shape);
}

/*
 * `[1, 2, 3]` as an owned container - the run, the descriptor over it and the elements written into
 * it, all emitted here, over values that are already in hand.
 *
 * Split from `resolveGrowableLiteral` above because it has a second caller and only one of the two
 * has an expression: the synthesized test entry builds an `[Case]` out of cases it constructed, and
 * a suite whose array is assembled by a second piece of code spelling `Array`'s fields is exactly
 * what one builder in one place exists to prevent. Everything about *what* a literal is - the
 * element type, the ordering, the coercion of the second element to the first's type - stays above
 * this line, because none of it is a question the entry has.
 *
 * **Three library calls became one instruction**, which is J46 part 3. What this used to be was
 * `arrayLiteral(n)` - itself `newRun` and so `runSlots` and `runOver` - then `elementBase(&self)`
 * for somewhere to write, and `reserve` where a bound asked for more room than the literal filled.
 * Five bodies to resolve, instantiate, summarize and inline, at every literal in the program, to
 * arrive at an allocation and five stores. `NodeRunSlots` is the allocation, and the two aggregates
 * below are the stores, said once.
 *
 * What that buys is not only the compile time (§92.4's 4.7x, and `test/bench/findings.md` §94 for
 * the other half of the same cost). It is that `resolve_ssa/opt/place.cpp` no longer has to
 * *recognize* the request: the instruction is the request, so `runSlots` stops needing `@noinline`
 * to be findable and `newRun` stops needing `@inline(always)` to put the request in the right frame.
 *
 * **The capacity is `max(written, bound)` and the length is the written count.** `@capacity(n)`
 * used to be a `reserve` call after the fact, which relocates: the literal built a run of `n`
 * elements' worth and then grew it. Sizing the request once is the same guarantee with the
 * relocation gone, and it is available here because the request is an instruction whose size this
 * function chooses rather than a call whose argument is the written count.
 *
 * **The refusal is the same one the library made.** `runSlots` answers null where the allocator
 * says no, `runOver` turns that into a capacity of zero, and `literalArray`'s `checkCondition`
 * stops the program. Written out here it is one comparison against null, which folds to nothing
 * wherever the placement pass puts the slots in the frame - a frame allocation does not fail.
 */
/*
 * The same literal on JavaScript - `Array {items, length}` over the host's own array.
 *
 * A second builder rather than a branch inside the one below, because the two share nothing: there
 * is no run, no allocator refusal to test, no capacity field and no `Place::Index` over an address,
 * and what is left of the native body once those are removed is two lines. Analysis-Modules.md's
 * rule for the library is the one applied here - what varies between implementations is the
 * *owner*, and this is the owner being built.
 *
 * **The elements are operands of the `host.array` node**, not writes after it. `[a, b, c]` is one
 * expression in the emitted source and the shipping stage already emits it that way
 * (`codegen/js/inst.cpp`'s `HostArray` arm, which is variadic); building an empty array and writing
 * past its end would also be *wrong* on the typed row, where a `Uint8Array` of no elements accepts
 * no write at all. They arrive `Take` because the array holds them afterwards, which is the same
 * hand-over `hostWrite` declares and is what keeps this frame from releasing what the array owns.
 *
 * `@capacity(n)` is **ignored here**, and that is the honest answer rather than an omission: a host
 * array grows on its own, so there is no reservation to make - `Array.js.yana`'s own `reserve`
 * returns immediately for exactly that row - and on the typed row the literal's own count is the
 * capacity until `hostGrow` moves it.
 */
static Node* buildHostArray(Resolver& self, Builder& builder, DeclPtr<Type> element,
                            SmallArray<Node*, 8>& components, LocationId source) {
    auto arrayRole = self.program.roles.record(intrinsic::Known::array);
    if(!arrayRole) {
        self.reportMissingRole(intrinsic::Known::array, "a growable array literal"_v, source);
        return nullptr;
    }

    auto arrayType = instantiateRecord(self.context, self.program, arrayRole, &element, 1, source);
    if(!arrayType) return nullptr;

    auto& record = *(RecordType*)self.decl()[arrayType];
    if(record.fields.size() != 2) {
        self.context.diagnostics.error(
            "this library's `Array` is not the record an array literal is built out of"_v, source);
        return nullptr;
    }

    SmallArray<UseMode, 8> modes;
    for(Size i = 0; i < components.size(); i++) modes.push(UseMode::Take);

    auto handle = owned(self.program.types.pointerType(element));
    auto items = builder.addHost(source, HostOp::Array, StringId(), components.pointer(),
                                 modes.pointer(), components.size(), handle);
    if(!items) return nullptr;

    Node* parts[2];
    parts[RunShape::kItems] = items;
    parts[RunShape::kLength] = builder.addConst(
        source, intConstant(record.field(self.decl(), 1).occurrence.type, U64(components.size())));

    if(!parts[RunShape::kLength]) return nullptr;

    return builder.addAggregate(source, arrayType, 0, parts, 2);
}

Node* Resolver::buildGrowableArray(Builder& builder, DeclPtr<Type> element,
                                   SmallArray<Node*, 8>& components, LocationId source,
                                   U64 capacity) {
    if(isJsMode(context.settings)) return buildHostArray(*this, builder, element, components, source);

    auto shaped = arrayShape(*this, element, source);
    if(!shaped) return nullptr;

    auto& shape = shaped.unwrap();
    auto count = U64(components.size());
    auto room = capacity > count ? capacity : count;

    if(room > kMaxFixedArrayLength) {
        context.diagnostics.error("this array literal asks for more elements than a run can hold"_v,
                                  source);
        return nullptr;
    }

    /*
     * An empty literal asks for nothing and gets `emptyRun`'s answer written out: a null pointer and
     * a capacity of zero.
     *
     * Not an allocation of no bytes, which is what a `RunSlots` of zero would have to become - the
     * allocator would hand back a block the container then owes `freeHeap`, at every `[] :: [Int]`
     * in the program. `runSlots` refuses a capacity of zero for the same reason and answers the same
     * null; this is that arm, decided here because the count is known here.
     */
    Node* slots = nullptr;

    if(room) {
        slots = builder.addRunSlots(source, element, room);
    } else {
        auto zero = builder.addConst(source, intConstant(program.types.words[1], 0));
        if(!zero) return nullptr;

        slots = builder.addBitcast(source, *zero, program.types.pointerType(element));
    }

    if(!slots) return nullptr;

    if(!checkSlots(builder, *slots, room, source)) return nullptr;

    /*
     * `Run {items, capacity, ownsHeap}` and `Array {run, length}` - the two records, built as
     * ordinary aggregates.
     *
     * `runFromHeap` is what the request answered and what the descriptor therefore says, exactly as
     * `newRun` passed it: whoever decided where the bytes are is the only thing entitled to write
     * this field, and at this point that decision is "the allocator". `place.cpp` rewrites the
     * field where it moves the slots, which is the same edit it has always made.
     */
    Node* runParts[3];
    runParts[RunShape::kItems] = slots;
    runParts[RunShape::kCapacity] = builder.addConst(source, intConstant(shape.capacity, room));
    runParts[RunShape::kOwnsHeap] = builder.addConst(source,
                                                     intConstant(shape.flag, RunShape::kFromHeap));

    if(!runParts[RunShape::kCapacity] || !runParts[RunShape::kOwnsHeap]) return nullptr;

    auto run = builder.addAggregate(source, shape.run, 0, runParts, 3);
    if(!run) return nullptr;

    Node* arrayParts[2];
    arrayParts[RunShape::kRun] = run;
    arrayParts[RunShape::kLength] = builder.addConst(source, intConstant(shape.length, count));
    if(!arrayParts[RunShape::kLength]) return nullptr;

    auto array = builder.addAggregate(source, shape.array, 0, arrayParts, 2);
    if(!array) return nullptr;

    /*
     * Held in a local, because the elements are written through the slots and the array has to
     * outlive every one of those writes.
     *
     * Writable, which `hold` cannot give it: that function reads the permission off the value's own
     * contract, and an owned array's says nothing about whether the storage holding it may be
     * written. Here it may - the local is a temporary of this frame that the literal is still
     * building.
     */
    auto& occurrence = valueOccurrence(*array);
    auto local = builder.addLocal(StringId(), occurrence, source, true);
    auto place = builder.localPlace(*local);
    builder.addWrite(source, *place, *array, Destination::Init);

    /*
     * Element `i` at index `i` of the run the pointer names - the same `Place::Index` a fixed
     * array's element is, over an `Address` root instead of a local one.
     *
     * The place is a `[a *n]` of exactly the written length, which is what the storage *is*: `n`
     * elements at a stride, starting here. That is not the array's type and must not be - an
     * `Array(a)` is a record of two fields and these bytes are neither - and stating the run rather
     * than the container is what lets `elementPlace` project an element with nothing added.
     *
     * `Init` rather than `Assign`, and the difference is a teardown: the slots hold whatever the
     * allocator handed back, so an assignment would drop a value that was never there. It is also
     * what keeps the write out of the loan check - initializing fresh storage is not writing through
     * the borrow the address came from - and `Builder::addWrite` is where the raw-address root is
     * admitted for exactly this.
     *
     * Written through the request's own result rather than through the array, which is the one thing
     * that changed here: `elementBase(&self)` read the pointer back out of the container it had just
     * been put into, and the pointer is in hand.
     */
    if(count) {
        auto storage = program.types.arrayType(decl(), element, count);
        auto window = builder.addressPlace(*slots, Occurrence { storage, ownedContract(), nullptr },
                                           true);

        for(Size i = 0; i < components.size(); i++) {
            auto index = builder.addConst(source, intConstant(program.types.words[1], U64(i)));
            if(!index) return nullptr;

            auto slot = builder.elementPlace(*window, *index);
            builder.addWrite(source, *slot, *components[i], Destination::Init);
        }
    }

    // The array itself, moved out of the local it was held in. A read at an owning request, which is
    // what makes it the temporary the surrounding position receives rather than a borrow of a local
    // that is about to end.
    return readPlace(builder, source, *place, requestOf(ownedContract()));
}

/*
 * The allocator's refusal, tested - `literalArray`'s `checkCondition` written out.
 *
 * `isNull(slots)`, spelled the way `lib/Native/Pointer.yana` spells it: a pointer read as a word and
 * compared against zero. Skipped for a request of no slots at all, which is the one case where a
 * null answer is the right one - an empty literal's run has room for nothing and points at nothing,
 * and `emptyRun` is that value.
 *
 * Silent where the library declares no condition function, on `checkFixedBound`'s terms: the role is
 * `Optional`, and a library whose allocator failures are unchecked is a decision it made.
 */
bool Resolver::checkSlots(Builder& builder, Node& slots, U64 room, LocationId source) {
    if(!room) return true;
    if(!program.roles.function(intrinsic::Known::checkCondition)) return true;

    auto word = program.types.words[1];
    auto address = builder.addBitcast(source, slots, word);
    if(!address) return false;

    auto zero = builder.addConst(source, intConstant(word, 0));
    if(!zero) return false;

    auto refused = builder.addBinary(source, BinaryOp::Eq, *address, *zero);
    if(!refused) return false;

    return callKnown(builder, intrinsic::Known::checkCondition, &refused, 1, source) != nullptr;
}

/*
 * `[1, 2, 3]` - `doc/spec/expressions.md`'s array construction, and which of the two containers it
 * is.
 *
 * **A literal takes its type from context and never from itself**, which is what the spec means by
 * "there is no fixed-array literal - `[0, 1, 2]` is coerced to `[Int *3]` by context". Two elements
 * say nothing about whether they are two `I32`s or two `I64`s, and nothing about whether what is
 * being built is a fixed array or a growable one. What decides is the position, in the two readings
 * below; what a literal with no position at all builds is the spec's own answer, a growable array.
 *
 * `[x] :: [T *n]` **fills the whole array**, which is the spec's "a literal of one element at a
 * longer type fills the whole array" - `arrayLiteralShape`, where that rule and the count rule are
 * stated once for this and for the constant path both.
 */
Node* Resolver::resolveArrayLiteral(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint,
                                    Request wanted, U64 capacity) {
    auto written = expr.arr.contents(parse);

    /*
     * A literal at a **window** position takes its count from itself - `total([5, 5])`.
     *
     * `doc/spec/collections.md` says a count written `_` is read off the literal, and this is the
     * case where the position asks for one and states no number: what is built is a `[T *n]` of
     * exactly what was written, and `adaptToWindow` gives it a frame slot and a window of that. So
     * `total([5, 5])` and `total([5, 5] :: [I32 *2])` are one program, which is what a caller
     * writing a small array argument expects.
     *
     * A position that **holds** the sequence is the other reading of the same hint and is the
     * growable array - the elements are the container's own storage rather than a description of
     * somebody else's, so there is nothing to describe and the literal has to build one.
     *
     * `Request::form` is what tells the two apart, and it is the only thing that can: the expected
     * type of a window and of an owner is the same type, so the difference is in the contract the
     * position was read from and not in `Array(T)`. The access is deliberately *not* asked - `let
     * &xs = [1, 2, 3] :: [Int]` is a unique binding of a value the literal produces, which is
     * `resolveDeclaration`'s own rule that a produced value arrives owned whatever the sigil said.
     */
    if(hint && isSequenceType(decl(), program, hint) && wanted.form == AccessForm::View) {
        if(auto element = elementTypeOf(decl(), program, hint)) {
            hint = program.types.arrayType(decl(), element, U64(written.size()));
        }
    }

    /*
     * The growable array - `[2, 3, 5, 7]`, whose type `doc/spec/expressions.md` writes as `[Int]`.
     *
     * Two positions reach it and they are the two halves of "decided by the expected type and by
     * nothing else": a hint that *is* a growable array at a position that owns one, and **no hint
     * at all**, which is a literal standing on its own and is a growable array because that is what
     * the spec says an unascribed one is. A fixed array is never the answer where nothing asked for
     * one - its count is part of its type, and a literal has no type to put a count in.
     */
    if(!hint || isSequenceType(decl(), program, hint)) {
        return resolveGrowableLiteral(builder, expr, hint, capacity);
    }

    /*
     * A capacity written at a literal that is not a growable array - `[0] :: @inline(4) @capacity(4)
     * [Int *4]`. Reported rather than dropped, on `readContainerRefinement`'s own terms: a fixed
     * array's storage is its type and there is no run for a bound to be a bound of.
     */
    if(capacity) {
        context.diagnostics.error(
            "`@inline(n) @capacity(n)` refines a growable array - `[T]` - and this literal builds "
            "%@"_v, expr.source, typeText(context, decl(), hint));
        return nullptr;
    }

    if(decl()[hint]->kind != Type::Array) {
        context.diagnostics.error(
            "an array literal builds a fixed array or a growable one, and %@ is neither - write "
            "`:: [T *n]` for a fixed array, or `:: [T]` for a growable one"_v,
            expr.source, typeText(context, decl(), hint));
        return nullptr;
    }

    auto shaped = arrayLiteralShape(expr, hint, written.size());
    if(!shaped) return nullptr;

    auto element = shaped.unwrap().element;
    auto length = shaped.unwrap().length;

    SmallArray<Node*, 8> components;
    for(Size i = 0; i < written.size(); i++) {
        auto& item = *written.pointerAt(i);

        // Owned, which is what an element position is: an array's elements are its storage, so a
        // component fills one the way a field of a record does. `requestOf` on the same occurrence
        // `addAggregate` will read is what keeps the two from disagreeing.
        auto value = resolveValue(builder, item, element, requestOf(ownedContract()));
        if(!value) return nullptr;

        if(!sameType(valueType(*value), element)) {
            context.diagnostics.error("this element is %@ and %@ holds %@"_v, item.source,
                                      typeText(context, decl(), valueType(*value)),
                                      typeText(context, decl(), hint),
                                      typeText(context, decl(), element));
            return nullptr;
        }

        components.push(value);
    }

    // The fill, spread here rather than by the builder wherever there is a number to spread it to:
    // what `addAggregate` takes is one component per element, and a node that meant "this value, n
    // times" at a *countable* array would be a second aggregate shape for every consumer to learn.
    // A quantified count has no number here and is the one case the node carries - see `fill`.
    if(!shaped.unwrap().quantified) {
        while(components.size() < length) components.push(components[0]);
    }

    return builder.addAggregate(expr.source, hint, 0, components.pointer(), components.size(),
                                nullptr, shaped.unwrap().quantified);
}

/*
 * A call - Analysis-IR.md §5.6.
 *
 * Three steps and no more: find the callee, resolve one argument per declared position, and hand the
 * values to `Builder::addCall`, which is where the signature becomes argument modes. The resolver
 * deliberately never states a mode - see `argumentMode` - so what is left here is name resolution
 * and the arity and type diagnostics, which are the two failures a *program* has.
 *
 * The parameter's own access reaches the argument as `wanted`, and that is the one thing this does
 * with the signature beyond checking the type: a bare name filling `x: T` is a loan and the same
 * name filling `->x: T` is a move, and `readMode` is where the pair is answered.
 */
Node* Resolver::resolveCall(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint) {
    auto& written = *parse[expr.app];

    /*
     * Anything that is not a **name a declaration answers** is a value, and calling one is an
     * indirect call through it - G7.
     *
     * A local shadowing a function is the first case and is not something to quietly resolve to the
     * declaration it hides; a field holding a function value - `h.run(v)` - is the second, and is
     * the one a caller cannot see through even in principle. A **capture** is the third and is the
     * same case as the first read one frame out: a name the enclosing function bound is storage, and
     * a lambda that says it is calling a value whatever the module declares. All three go the same
     * way, because "where did the callee come from" is the only thing that differs and
     * `mir::CallThrough` is already the answer to it.
     *
     * The value is resolved before its arguments are, which is the order the operand walk publishes
     * and therefore the order every ownership pass reads: `f(g())` reads `f` and then runs `g`, and
     * a walk that had built the arguments first would place a loan of the callee after the loans of
     * what it is passed.
     *
     */
    /*
     * `x.f(y)` - the dot-call form, ahead of the indirect path because deciding between the two is
     * exactly what it does.
     *
     * A projection wins over a dot-call, which is the language's own rule and not an artifact:
     * `h.run(v)` where `run` is a field is a call through that field. `resolveDotCall` is where that
     * choice is made and it is the only place it is made.
     */
    if(written.callee.kind == ast::Expr::Field) {
        return resolveDotCall(builder, written, *parse[written.callee.field], hint, expr.source);
    }

    // `findCapture` appends the field on the first name it answers for, which is what makes it the
    // test as well as the lookup: where it says yes the walk below reads the name through the very
    // capture it just made, and where it says no it made nothing.
    auto globalCallee = written.callee.kind == ast::Expr::Var
        ? findGlobal(context, program, here(), written.callee.var, kNullLocation)
        : DeclPtr<Global>(nullptr);
    if(globalCallee && decl()[globalCallee]->occurrence.type &&
       decl()[decl()[globalCallee]->occurrence.type]->kind != Type::Fun) {
        globalCallee = nullptr;
    }
    auto storage = written.callee.kind == ast::Expr::Var &&
                   (findLocal(written.callee.var) || findCapture(builder, written.callee.var) ||
                    globalCallee);

    if(rawLambdaCaptureRejected()) return nullptr;

    if(written.callee.kind != ast::Expr::Var || storage) {
        auto target = resolveValue(builder, written.callee, nullptr, bindingRequest(Access::Shared));
        if(!target) return nullptr;

        return resolveValueCall(builder, written, *target, expr.source);
    }

    auto name = written.callee.var;

    // The cursor where a callee is written - where an editor asks most often, since a call is typed
    // name first.
    if(isCursorSentinel(context, name)) {
        captureCompletion(hint);
        return nullptr;
    }

    /*
     * By name **and arity** - `Program::addFunctionName`, which is where the rule is argued.
     *
     * `-(x)` and `-(x, y)` are two declarations of one name, so the number of arguments written is
     * part of what a call selects. It is not overloading on types: the arity picks exactly one
     * callee and everything after this line reads a single known signature.
     */
    auto arity = U32(written.args.size());

    /*
     * A **callable requirement** of the enclosing signature, before the module's own declarations.
     *
     * `fn (grow: (a) -> a) twice(x: a)` types `grow(x)` against the promise, and the promise is what
     * the body was written against: whatever `grow` is declared as, this signature said what it will
     * be at every instantiation, and a body checked against the declaration instead would refuse the
     * one call the constraint exists to permit.
     *
     * First rather than last, so that a requirement is not shadowed by a declaration it was written
     * to stand in for - which is exactly the case `fn grow(v: Vessel)` and `grow: (a) -> a` are.
     */
    if(currentFunction) {
        auto requirement = findCallable(name, arity);
        if(requirement != kNoProperty) {
            return resolveRequiredCall(builder, written, U16(requirement), expr.source);
        }
    }

    /*
     * R1's set, and the two paths out of it.
     *
     * A set of one is every call in the corpus and is resolved below, unchanged: its parameter types
     * are known before its arguments are and are pushed down as each argument's expected type.
     * Anything larger is `resolve_overload.cpp`, where R2 through R5 are - and where the arguments
     * are resolved with no expected type, because which parameter types apply is the question they
     * are being resolved to answer. R5's last paragraph is that division, stated by the spec itself.
     */
    OverloadSet set;
    findOverloads(context, program, here(), name, arity, written.callee.source, set);
    if(captureArguments(written.args, set, 0)) return nullptr;

    if(set.size() > 1) {
        return resolveOverloadedCall(builder, written, set, name, written.callee.source, hint,
                                     expr.source);
    }

    auto found = set.sole();

    if(!found) {
        // Two messages, because they are two mistakes: a name nobody declared, and the right name
        // called with the wrong number of arguments. The second is what arity overloading makes
        // possible to get wrong, so it is the one that has to say what it saw.
        if(hasFunctionName(context, program, here(), name)) {
            context.diagnostics.error("no %@ takes %@ arguments"_v, written.callee.source,
                                      context.findName(name), U64(arity));

            // The right name at the wrong arity is still an occurrence of that name, and a call
            // being typed is exactly the one an editor is asked about.
            if(recording(context)) {
                auto named = search<DeclPtr<Function>>(context, program, here(), name, kNullLocation,
                                                       [&](DeclPtr<Module> in, NameRef reference) {
                    return program.functionNamed(in, reference.rest());
                }, false);

                if(named) recordCallee(written.callee.source, *named);
            }
        } else {
            context.diagnostics.error("unknown function %@"_v, written.callee.source,
                                      context.findName(name));
        }

        // The arguments still name things an editor is asked about, and the body is dropped before
        // the analyses either way - see `Resolver::resolveBody`.
        if(recording(context)) {
            for(auto argument: written.args.contents(parse)) {
                resolveValue(builder, argument.value, nullptr, bindingRequest(Access::Shared));
            }
        }

        return nullptr;
    }

    auto& callee = *decl()[found];
    recordCallee(written.callee.source, found);

    if(!checkCallable(callee, expr.source)) return nullptr;

    ArgMap map;
    if(!checkCallShape(written, callee, expr.source, map)) return nullptr;

    /*
     * The callee's type arguments, decided by the argument walk rather than before it, because that
     * is where the information is: `id(7)` says `a` is `I32` by what it passes, and nothing at the
     * call site names a type.
     */
    TypeArguments typeArguments(*this, callee.gen);

    SmallArray<Node*, 8> arguments;
    if(!resolveArguments(builder, written, callee, map, expr.source, arguments, typeArguments,
                         hint)) {
        return nullptr;
    }

    auto call = finishCall(builder, callee, arguments, typeArguments, hint, expr.source);
    recordCallee(written.callee.source, found, &typeArguments);
    return call;
}

/*
 * The overload set a **dot-call** selects from - R1's set, with the receiver's own namespace
 * deciding the plain half.
 *
 * `String.push` is *the* plain function where it exists, rather than joining the set beside a bare
 * `push`: R1 admits at most one plain function and this is which one it is, which is what makes
 * `s.push(v)` reach `String`'s and `xs.push(v)` reach `Array`'s from one body. The class half is
 * gathered from the bare name alone, because a member is a name a class declared and no receiver
 * namespaces it.
 *
 * **The namespaced reading is asked first**, and that ordering is load-bearing rather than tidy:
 * `search` reports an ambiguity as it looks, so a bare lookup performed first would already have
 * said "visible through more than one import" by the time the type's own function could have
 * replaced its answer. `store` on an `Atomic` beside `Native`'s is exactly that shape.
 *
 * Reported normally rather than probed: this is the call that gets compiled if the namespaced form
 * is found at all, ambiguous or not, so an ambiguity here is a real mistake and not a candidate
 * about to be discarded in favour of the bare reading - `report` exists for the other kind of
 * lookup, the one only asking whether a name exists. `ErrorMark` is what keeps the bare half from
 * adding a second diagnostic about the same call, the same shape `projectionTarget` and
 * `resolveUpdate` use to try one form and fall back to a second without both reporting: the bare
 * lookup reports only where the namespaced one neither found a candidate nor already said something
 * about this name.
 */
void Resolver::findDotOverloads(DeclPtr<Type> receiver, StringId name, U32 arity, LocationId source,
                                OverloadSet& into) {
    OverloadSet namespaced;
    ErrorMark mark(context);

    if(auto qualified = namespacedName(context, program, receiver, name)) {
        findOverloads(context, program, here(), qualified, arity, source, namespaced);
    }

    OverloadSet bare;
    findOverloads(context, program, here(), name, arity, source, bare,
                  !namespaced.plain && !mark.reported());

    into.plain = namespaced.plain ? namespaced.plain : bare.plain;
    into.members.clear();
    for(auto member: bare.members) into.members.push(member);
}

/*
 * `x.f(y)` - `doc/spec/expressions.md`'s dot call, which is `f(x, y)` for every `f` a body can name.
 *
 * The receiver is resolved **once**, at the top, and handed to whichever half wins. That is the
 * whole shape of this function and the reason it exists rather than the two halves each doing their
 * own lookup: resolving is emission, so a probe that resolved the receiver and then gave up would
 * leave its instructions behind and the winning half would emit them again.
 *
 * It is resolved as a **place**, which is what makes the call form itself no different from a
 * written one. What a receiver is read as is `readMode`'s answer at the position it fills, and the
 * position is not known until the callee is - so a place is exactly what can be carried across the
 * selection, and `argumentMode` stays the one rule for `&self`, `->self` and a plain borrow alike.
 * There is no second call path: `resolveArguments` and `resolveOverloadedCall` take the receiver as
 * argument 0 of the same mapping every call uses.
 *
 * **The field wins.** A record whose field holds a function is called through that field, as it was
 * before this form existed, and the consequence is stated rather than hidden: adding a field to a
 * record shadows a dot-call on that type. It is the same shape as a local binding shadowing a
 * declaration and is decided the same way, by asking the nearer thing first.
 *
 * Universal, and that includes this module's private functions. There is no `impl` block and no
 * per-type namespace to *join* - `fn String.push` is R1's one plain function for `String` and
 * nothing more, and what a body can call a body can call with a dot.
 *
 * Two things the form does not reach, each grammar rather than rule. A receiver whose own resolution
 * depends on the access - `xs[i].bump()` where `bump` takes `&self` - is read shared, because the
 * accessor `Index.get` had to be emitted before a candidate could say it wanted `getMut`; the
 * shipping stage refuses the same program with the same reason. And the receiver takes no type
 * pushdown, since its type is what decided this was a dot-call at all.
 */
Node* Resolver::resolveDotCall(Builder& builder, ast::AppExpr& written, ast::FieldExpr& field,
                               DeclPtr<Type> hint, LocationId source) {
    /*
     * A selector that is not a plain name is a field position and nothing else: `t.0(x)` is a tuple
     * index holding a callable. It is not a name a function could have, so nothing is being chosen
     * between and the ordinary value walk resolves the whole callee expression.
     */
    if(field.field.kind != ast::Expr::Var) {
        auto target = resolveValue(builder, written.callee, nullptr, bindingRequest(Access::Shared));
        if(!target) return nullptr;

        return resolveValueCall(builder, written, *target, source);
    }

    if(field.target.kind == ast::Expr::Con && isCursorSentinel(context, field.field.var)) {
        // Keep the call form (`X86.foo(...)`) on the same namespace-completion path as the value
        // form. There is no receiver to resolve until a field name has been chosen.
        captureCompletion(hint);
        context.completion->qualified = true;
        return nullptr;
    }

    /*
     * Shared, which is the one thing the receiver cannot take from the selected signature - see the
     * note above. Everywhere but a subscript the place is the same under either access, so `&self`
     * on a name, a field or a global is decided by `readPlace` below exactly as a written `&`
     * argument is.
     */
    auto receiver = projectionTarget(builder, field.target, Access::Shared);
    if(!receiver) return nullptr;

    if(isCursorSentinel(context, field.field.var)) {
        captureMemberCompletion(receiver->occurrence);
        return nullptr;
    }

    /*
     * **The choice, and the only place it is made.**
     *
     * A binding of the name is deliberately not consulted: `f` in `x.f(y)` is a member position and
     * not a scope one, so a local called `map` does not become every value's `.map`. That is the one
     * place this form departs from `resolveCall`'s "a binding shadows a declaration", and it departs
     * from it because the two are not in the same namespace to begin with.
     */
    if(hasMemberNamed(receiver->occurrence, field.field.var)) {
        auto place = fieldOf(builder, *receiver, field.field, field.target.source);
        if(!place) return nullptr;

        // At the callee expression, which is where `resolveValue` would have read it: the two paths
        // differ in how the place was reached and in nothing a diagnostic should point at.
        auto target = readPlace(builder, written.callee.source, *place,
                                bindingRequest(Access::Shared));
        if(!target) return nullptr;

        return resolveValueCall(builder, written, *target, source);
    }

    auto name = field.field.var;

    // The receiver is argument 0, so it is part of the arity R1 keys the set by - `x.f(y)` selects
    // what `f(x, y)` selects and by the same lookup.
    auto arity = U32(written.args.size()) + 1;

    OverloadSet set;
    findDotOverloads(receiver->occurrence.type, name, arity, field.field.source, set);
    if(captureArguments(written.args, set, 1)) return nullptr;

    Receiver from { receiver, field.target.source };

    if(set.size() > 1) {
        return resolveOverloadedCall(builder, written, set, name, field.field.source, hint, source,
                                     from);
    }

    auto found = set.sole();

    if(!found) {
        /*
         * Nothing answered, and the message says what was looked for - both halves, because both
         * were looked in.
         *
         * The projection's own wording survives for the one case it is still true of: the name is
         * neither a field of the receiver nor a function anything in scope declares. Where a
         * function of the name does exist, the mistake is the count, and the receiver being the
         * first of them is the part an author of `x.f()` has to be told.
         */
        auto qualified = namespacedName(context, program, receiver->occurrence.type, name);
        auto declared = hasFunctionName(context, program, here(), name) ||
                        (qualified && hasFunctionName(context, program, here(), qualified));

        if(declared) {
            context.diagnostics.error(
                "no %@ takes %@ arguments - `x.%@(...)` is `%@(x, ...)`, so the receiver is the first of them"_v,
                field.field.source, context.findName(name), U64(arity), context.findName(name),
                context.findName(name));
        } else if(qualified) {
            context.diagnostics.error(
                "%@ has no field called %@, and nothing in scope declares %@ or a function %@"_v,
                field.field.source, typeText(context, decl(), receiver->occurrence.type),
                context.findName(name), context.findName(qualified), context.findName(name));
        } else {
            context.diagnostics.error(
                "%@ has no field called %@, and nothing in scope declares a function %@"_v,
                field.field.source, typeText(context, decl(), receiver->occurrence.type),
                context.findName(name), context.findName(name));
        }

        return nullptr;
    }

    auto& callee = *decl()[found];
    recordCallee(field.field.source, found);

    if(!checkCallable(callee, source)) return nullptr;

    ArgMap map;
    if(!checkCallShape(written, callee, source, map, true, 1, from.source)) return nullptr;

    TypeArguments typeArguments(*this, callee.gen);

    SmallArray<Node*, 8> arguments;
    if(!resolveArguments(builder, written, callee, map, source, arguments, typeArguments, hint,
                         from)) {
        return nullptr;
    }

    auto call = finishCall(builder, callee, arguments, typeArguments, hint, source);
    recordCallee(field.field.source, found, &typeArguments);
    return call;
}

Node* Resolver::prepareArgument(Builder& builder, Node& value, Arg& parameter,
                                TypeArguments& typeArguments, LocationId source) {
    auto declared = parameter.occurrence.type;
    auto held = valueType(value);

    /*
     * A donor that is not the position's type binds the position's *element*: `[I32 *3]` or an
     * `ArrayView(I32)` reaching `xs: [a]` says `a` is `I32`, and the ordinary binding - the whole
     * type against the whole type - would say nothing, leaving `a` open and the adaptation with no
     * element to compare. Bound before the expected type is read, so the adaptation sees `[I32]`.
     */
    if(parameter.occurrence.contract.form() == AccessForm::View) {
        auto wanted = elementTypeOf(decl(), program, declared);
        auto donor = wanted ? donorElementOf(decl(), program, held) : nullptr;
        if(donor) typeArguments.bind(wanted, donor);
    }

    /*
     * What a **functional dependency** already decides about this position - asked *before* the
     * argument offers its own answer, which is the whole of the ordering.
     *
     * `xs[i]` is `Index.get(self: c, index: k)`, and `k` is not something the index decides: `class
     * Index(c -> k, v)` promises that one container has one key type, so what the container bound
     * one position ago is the answer and an index written as an `Int` is a value to be converted to
     * it. Binding first would do what binding always does - see `k` open, offer `I32` - and the
     * requirement recorded would be `Index(Array(I32), I32, I32)`, an instance nobody wrote,
     * reported as missing where a widening was all that was wanted.
     *
     * Only where the position is still open, which is what keeps this from being a second answer to
     * anything: the fixpoint fills empty slots and never overwrites one, so asking here and asking
     * at the end of the walk give the same answer, and asking here is what puts it in reach of the
     * coercion below. The declared defaults are deliberately not run with it -
     * see `fillFromRequirements`.
     */
    if(mentionsContext(decl(), typeArguments.expected(declared), typeArguments.owner)) {
        typeArguments.fillFromRequirements(source);
    }

    typeArguments.bind(declared, held);

    auto expected = typeArguments.expected(declared);

    /*
     * The position as the callee will see it: the type bound, and the form the binding decides. A
     * shared `c` at a type with a view is that view - `substitutedContract`, applied here to the
     * one position the walk has in hand, so that the argument is adapted to what the specialized
     * callee's parameter manifests as rather than to what the generic one wrote.
     */
    auto position = Occurrence { expected,
                                 substitutedContract(decl(), program, declared, expected,
                                                     parameter.occurrence.contract),
                                 parameter.occurrence.shape };
    auto wanted = requestOf(position);

    // A container reaching a view position - the one implicit adaptation, and it happens before
    // the type check because what it produces is what the position asked for.
    auto adapted = adaptToView(builder, value, position, source);
    if(!adapted) return nullptr;

    adapted = coerce(builder, *adapted, expected, source, "an argument"_v,
                     position.contract.access());
    if(!adapted) return nullptr;

    // A closure into a position the callee *owns* - `checkCarriedOrigins`. After the type, as at
    // every other position that asks: what a value refers to is only worth a message once the
    // value is the kind the position wanted. A borrowing position is fine and is the whole point
    // of a callback - the loan lasts the call, which is what `Origin::CallBounded` says.
    if(!checkCarriedOrigins(*adapted, position, source, "this parameter"_v)) {
        return nullptr;
    }

    return holdBorrowed(builder, *adapted, wanted, expected, source);
}

void Resolver::recordCallee(LocationId nameSource, DeclPtr<Function> callee,
                            TypeArguments* typeArguments) {
    if(!recording(context)) return;

    auto& function = *decl()[callee];
    auto result = function.result.type;
    DeclPtr<Instance> instance = nullptr;

    if(typeArguments && typeArguments->any()) {
        // The result at this call rather than as declared - `Just(I32)` where the signature says
        // `Maybe(a)`. What the call left open stays the declaration's variable.
        result = typeArguments->expected(result);

        /*
         * Which instance served a class member - the same question `expandProvidedCall` asks, asked
         * quietly. Only where the call settled its arguments concretely: a call inside a generic body
         * is served by a different instance at each specialization, and the one an open head matches
         * here is not the one a more specific instance would displace it with there. A position the
         * class determines may still be null, which is selection's own protocol for filling it.
         */
        if(function.klass) {
            auto with = typeArguments->with();
            auto concrete = true;

            for(Size i = 0; i < with.count && concrete; i++) {
                if(with.arguments[i] && !isConcreteType(decl(), with.arguments[i])) concrete = false;
            }

            InstanceMatch found;
            if(concrete && selectInstance(context, program, function.klass, with.arguments,
                                          with.count, nameSource, false, found)) {
                instance = found.instance;
            }
        }
    }

    recordReference(context, nameSource, functionSymbol(decl(), callee), result, instance);
}

Node* Resolver::finishCall(Builder& builder, Function& callee, SmallArray<Node*, 8>& arguments,
                           TypeArguments& typeArguments, DeclPtr<Type> hint, LocationId source) {
    /*
     * `f(...)` over an `iter fn` at a position that **keeps** the result - the **state record**,
     * resolve_object.cpp, and §2.1's third row.
     *
     * Beside the handle above and for its reason: the arguments are what the two share, and what is
     * done with them is what differs. The source position is what separates the position's own call
     * from an iterator call written *inside* the expression filling it - `checkCallable` admits
     * both, because it has only the declaration, and this is where the five are told apart. See
     * `ObjectScope`, which is what sets the token.
     */
    if(callee.iterator) {
        if(source == objectCallSource) {
            return constructIterObject(builder, callee, arguments, typeArguments, source);
        }

        context.diagnostics.error(
            "%@ is an iterator, and what keeps an object of one is a `let`, a field, an argument, a `return` or an assignment - this call's result is not kept by any of them"_v,
            source, context.findName(callee.name));
        return nullptr;
    }

    /*
     * What the *result position* decides, where the arguments left something open.
     *
     * `fn empty() -> Maybe(a)` has no argument to read `a` off, and what settles it is the type the
     * call is being bound to - which is the same rule that picks an instance by a result type. The
     * hint is matched against the declared result exactly as an argument is matched against a
     * declared parameter, so `let x: Maybe(I32) = empty()` binds `a` and `empty()` in a position
     * that expects nothing still reports.
     *
     * **After** the arguments, deliberately. An argument is what the author wrote at this call and a
     * hint is what the surrounding position would like; binding the hint first would let a position
     * expecting `Maybe(I64)` silently decide a call whose argument said `I32`, and the mismatch
     * would then be reported against the argument rather than against the binding.
     *
     * `bindResult` and not `bind`, because *which* positions it filled is what the line below has to
     * know: a determined one is the dependency's to answer and the hint is only a wish about it.
     */
    if(hint) typeArguments.bindResult(callee.result.type, hint);

    // What the callee's own constraints decide, before anything is called undecided - a functional
    // dependency's whole purpose is that these positions have an answer no argument supplied, and
    // that its answer beats the one the result position wished for.
    typeArguments.fillDetermined(source);

    if(typeArguments.reportUndecided(source, "argument"_v,
                                     Tritium::format("call to %@", context.findName(callee.name)))) {
        return nullptr;
    }

    if(!dischargeRequirements(callee, typeArguments, source)) return nullptr;

    // `it.next()` on an object whose passes owe it a value - refused where the site type is written
    // out, and checked at run time through the class. See `refuseNextOnResuming`.
    if(!refuseNextOnResuming(callee, typeArguments.with(), source)) return nullptr;

    // Another body of this function, where one was read and its condition holds - bound and checked
    // against the baseline's signature above, which the replacement's is the same as. See
    // `chooseReplacement`.
    if(auto chosen = chooseReplacement(builder, callee, arguments, typeArguments, source)) {
        return chosen;
    }

    // A member of an instance the compiler generated, at a concrete call - the node it computes,
    // in place of the call. See `expandProvidedCall`.
    if(auto expanded = expandProvidedCall(builder, callee, arguments, typeArguments, source)) {
        return expanded;
    }

    /*
     * An **intrinsic**, which is this same call with a different last line.
     *
     * Everything above ran unchanged: the callee was found by name and arity, the arguments were
     * resolved against the declared parameters and prepared under the modes their contracts asked
     * for, the type arguments were bound and the constraints discharged. What the role decides is
     * only which node comes out - see `intrinsic/expand.h`, where the argument for keeping the two
     * paths joined this far is made.
     *
     * A `Flow` role is not expanded here: it is laid out by the operator walk, where its right
     * operand is a span of a chain rather than a value. Reached as a *written call* - `&&(a, b)`,
     * with a thunk already built for the second position - it is the ordinary call of the body
     * Core writes for it, which is what that body is for.
     */
    if(callee.intrinsic != intrinsic::Op::Count) {
        auto form = intrinsic::opDef(callee.intrinsic).form;

        // ...and a `Node` role this stage does not build a node for is an ordinary call of the body
        // the declaration carries - `opExpands`, where which those are and why is written down.
        if(form == intrinsic::OpForm::Node && opExpands(callee.intrinsic)) {
            return expandIntrinsic(builder, context, callee.intrinsic, callee, arguments.pointer(),
                                   arguments.size(), typeArguments.with(), source);
        }

        // A `Branch` role is the same last step laid out here instead, because what it expands to
        // makes blocks and a value this walk is holding has to travel their edges -
        // `resolve_intrinsic.cpp`, where that division is argued.
        if(form == intrinsic::OpForm::Branch) {
            return expandBranchingIntrinsic(builder, callee.intrinsic, callee, arguments.pointer(),
                                            typeArguments.with(), source);
        }
    }

    return builder.addCall(source, callee, arguments.pointer(), arguments.size(),
                           typeArguments.with());
}

/*
 * A concrete call of a class member whose selected instance the compiler generated.
 *
 * The rule: **a provided member expands where the call is concrete, and is called where it is
 * not.** `a + b` at two `I32`s selects `Num(I32)`, whose `+` is `provided.def`'s, and the call
 * becomes the `add` node the member computes; `x + y` in `fn (Num(a)) f(x: a, y: a)` names `Num.+`
 * with `a` open, is a call node, and `callTarget` finds the same member at the specialization -
 * which has a body the same emitter wrote, so nothing is lost but the inlining an optimizer does
 * anyway. The concrete case is the common one and is what makes an operator cost nothing without
 * an optimizer having run, which is the property the shipping stage's `Emit` hooks exist for.
 *
 * Selection here is the same `selectInstance` `callTarget` asks, and the two agree by construction:
 * the table is one table and the arguments are the same. `dischargeRequirements` has already
 * proved the instance exists, so a failed match is a compiler error and not a program's. A
 * generated instance is ground, so its members are never specialized and reading one off the
 * instance is reading the function itself - which is why this stays clear of `instanceMember`, a
 * specialization made here would be made before the generic's body exists.
 *
 * Null wherever the rule does not apply - a callee that is not a class member, an argument list
 * still open, an instance an author wrote, a member that is a body rather than a node - so that
 * the caller carries on to the call it would have built anyway.
 */
Node* Resolver::expandProvidedCall(Builder& builder, Function& callee,
                                   SmallArray<Node*, 8>& arguments, TypeArguments& typeArguments,
                                   LocationId source) {
    if(!callee.klass) return nullptr;

    auto with = typeArguments.with();
    for(Size i = 0; i < with.count; i++) {
        if(!with.arguments[i] || !isConcreteType(decl(), with.arguments[i])) return nullptr;
    }

    InstanceMatch found;
    if(!selectInstance(context, program, callee.klass, with.arguments, with.count, source, false,
                       found)) {
        return nullptr;
    }

    auto& instance = *decl()[found.instance];
    if(instance.provided == intrinsic::Provided::Count) return nullptr;

    auto member = instance.members.get(decl(), callee.classMember);
    if(!member) return nullptr;

    auto& function = *decl()[member];
    if(!providedExpands(function.provided, function.providedMember)) return nullptr;

    /*
     * The head is the **call's** argument list and not the instance's own.
     *
     * They are the same list for every row of `provided.def`, whose heads are ground. They are not
     * the same for a *parametric* generated instance - `Bitcast(%a, %b)`, and `Eq(%a)` beside it - and
     * there the instance's list is what it was declared at, which is a pair of variables. An emitter
     * reads the head for the type its node answers with, so a variable there is a `bitcast` whose
     * result type is `%b` and an ascription that then cannot be satisfied.
     *
     * Concrete by the check above, which is what makes this the honest source: `with.arguments` is
     * what selection was asked with and what it matched, so reading the answer off the question is
     * reading the one list both ends agree about.
     */
    return expandProvided(builder, function.provided, function.providedMember, with.arguments,
                          with.count, arguments.pointer(), arguments.size(), source);
}

Node* Resolver::applyMember(Builder& builder, intrinsic::Known klass, U16 member,
                            Node* const* operands, Size count, LocationId source,
                            DeclPtr<Type> hint, DeclPtr<Type> subject) {
    auto declared = program.roles.klass(klass);
    auto& callee = *decl()[decl()[declared]->members.get(decl(), member)];
    assertTrue(callee.writtenArity() == count);

    TypeArguments typeArguments(*this, callee.gen);

    /*
     * The class's **first argument**, where neither the call's own arguments nor its result decide
     * it - which is one member in the language and is `Rewrap`'s.
     *
     * `fn rewrap(->value: b) -> n` mentions `m` nowhere, exactly as `Try.fromExit` mentions `a`
     * nowhere: what selects it is a shape the compiler has already worked out - which carrier this
     * chain skips through - rather than anything an author could have written at a call site. So it
     * is supplied, and the ordinary binding then decides the rest.
     *
     * Bound by matching the class's own parameter, which is what makes it a *binding* and not an
     * assignment into a position: a contradiction with what an argument said is reported as the
     * ordinary mismatch, by the ordinary code.
     */
    if(subject) {
        typeArguments.bind(decl()[callee.gen]->variables.get(decl(), 0), subject);
    }

    SmallArray<Node*, 8> arguments;
    HeldList<SmallArray<Node*, 8>> held(*this, arguments);

    for(Size i = 0; i < count; i++) {
        auto prepared = prepareArgument(builder, *operands[i],
                                        *decl()[callee.args.get(decl(), U32(i))], typeArguments,
                                        source);
        if(!prepared) return nullptr;

        arguments.push(prepared);
    }

    return finishCall(builder, callee, arguments, typeArguments, hint, source);
}

Node* Resolver::truthValue(Builder& builder, Node& value, LocationId source) {
    Node* operands[1] = { &value };
    return applyMember(builder, intrinsic::Known::truth, 0, operands, 1, source);
}

Held::Held(Resolver& resolver): resolver(resolver), outer(resolver.held) {
    resolver.held = this;
}

Held::~Held() { resolver.held = outer; }

/*
 * Which values cross, read off the frames standing right now.
 *
 * Outermost first, so that the argument lists this produces are in a stable order whatever the
 * nesting - a block's parameters are positional, and two carries of one set that disagreed about
 * the order would fill each other's positions.
 */
Carry::Carry(Resolver& resolver, LocationId source): source(source) {
    SmallArray<Held*, 4> frames;
    for(auto frame = resolver.held; frame && frame != resolver.heldFloor; frame = frame->outer) {
        frames.push(frame);
    }

    for(Size i = frames.size(); i-- > 0;) {
        auto frame = frames[i];

        for(U32 index = 0; index < frame->count(); index++) {
            // A position whose value is being resolved at this very moment has no node yet, which is
            // exactly the position the construct being built is in.
            auto value = frame->at(index);
            if(!value) continue;

            slots.push(Carry::Slot { frame, index });
            occurrences.push(valueOccurrence(*value));
        }
    }
}

void Carry::declare(Builder& builder, Block& block) {
    for(Size i = 0; i < slots.size(); i++) {
        builder.addBlockArgument(block, occurrences[i], source);
    }
}

void Carry::arrive(Builder& builder, Block& block, U32 first) {
    auto base = builder.base();
    for(Size i = 0; i < slots.size(); i++) {
        value(U32(i)) = base[block.arguments.get(base, first + U32(i))];
    }
}

void Carry::edge(Builder& builder, SmallArray<Operand, 8>& into) {
    SmallArray<Node*, 4> now;
    save(now);
    edge(builder, now.pointer(), into);
}

/*
 * What an edge supplies for the carried values.
 *
 * `argumentMode` and not a mode of this walk's own, which is the rule `closeArm` reads for the
 * merge's own parameter and the one every other position that fills a declared slot reads: an owned
 * parameter takes, a borrowed one does not, and a carry that decided for itself would be a second
 * answer to a question that already has one.
 */
void Carry::edge(Builder& builder, const Node* const* state, SmallArray<Operand, 8>& into) {
    for(Size i = 0; i < slots.size(); i++) {
        into.push(builder.resultOf(*(Node*)state[i], argumentMode(occurrences[i].contract,
                                                                  SourceKind::Result)));
    }
}

TypeArguments::TypeArguments(Resolver& resolver, DeclPtr<GenContext> owner): resolver(resolver) {
    if(!isGeneric(resolver.decl(), owner)) return;

    this->owner = owner;
    for(Size i = 0; i < resolver.decl()[owner]->variables.size(); i++) arguments.push(nullptr);
}

DeclPtr<Type> TypeArguments::expected(DeclPtr<Type> declared) {
    return substituteType(resolver.context, resolver.program, declared, with());
}

bool TypeArguments::undecided(DeclPtr<Type> declared) {
    return mentionsContext(resolver.decl(), expected(declared), owner);
}

DeclPtr<Type> TypeArguments::hint(DeclPtr<Type> declared) {
    auto type = expected(declared);

    // Decided as far as this call is concerned: nothing of the callee's own context is left in it.
    // The enclosing signature's variables may be - `mentionsContext` is where that is argued - and
    // a hint that names one is what lets `x + 1` at `a` be `FromInt(a)`'s literal.
    if(!mentionsContext(resolver.decl(), type, owner)) return type;

    // The one position an open hint is still a hint - `isOpenFunPosition`, where the whole of the
    // rule is. What comes back binds the variable the position left open, through the same `bind`
    // every other argument goes through.
    return isOpenFunPosition(resolver.decl(), type) ? type : nullptr;
}

DeclPtr<Type> TypeArguments::resultHint(DeclPtr<Type> declared, DeclPtr<Type> result,
                                        DeclPtr<Type> wanted) {
    if(!any() || !result || !wanted) return nullptr;

    // A copy, because nothing here decides anything: the argument walk is what binds this context,
    // and what a position the walk has not reached yet is *hinted* with is not part of that answer.
    SmallArray<DeclPtr<Type>, 4> speculative;
    for(Size i = 0; i < arguments.size(); i++) speculative.push(arguments[i]);

    auto base = resolver.decl();
    if(!matchType(base, resolver.program.types, owner, speculative, result, wanted)) return nullptr;

    // The same last step `hint` takes, and for the same reason: a result that decided only some of
    // the context leaves this position open, and a variable is not something a value is made of.
    auto type = substituteType(resolver.context, resolver.program, declared,
                               Substitution { owner, speculative.pointer(), speculative.size() });
    return mentionsContext(base, type, owner) ? nullptr : type;
}

void TypeArguments::bind(DeclPtr<Type> declared, DeclPtr<Type> supplied) {
    if(!any()) return;

    /*
     * A `@bits(n)` refinement supplies the type it refines - `doc/spec/repr.md`'s "canonicalization
     * applies wherever a type variable is bound".
     *
     * Here rather than inside `matchType`, and the difference is depth: it is the *argument* that
     * widens, exactly as `readPlace` widens one read out of storage, so `x + 1` on a refinement is
     * the plain `U32` operator. A refinement standing inside a type argument is a different layout
     * and stays what it is - repr.md is explicit that `Box(@bits(20) U32)` does not convert to
     * `Box(U32)` - and binding a variable to the canonical *there* would select an instance
     * specialized for a record with different bytes.
     */
    auto base = resolver.decl();
    auto value = canonicalInt(base, supplied);

    /*
     * And **two arguments at one variable meet at their `Widen` upper bound** -
     * `doc/spec/overloading.md`'s "within a single call, the positions bound to the same class
     * variable are unified to their unique `Widen` upper bound where one exists", applied to a
     * function's own type arguments as well as to a class's.
     *
     * `checkAtMost(shown(x), bound(x))` at `(Size, Int)` and `valueOf(e) == want` at `(I32, I64)`
     * are the shape - one variable, two positions, and one of the two spellings the language calls
     * lossless. Without it the first argument bound the variable and every later one had to convert
     * *into* it, so the wider argument was a narrowing and the call was refused; six sites in the
     * library's suite carried an ascription for exactly that. `commonWiden` is the same function the
     * arms of an `if` and a `match` already meet through, so there is one rule and not two.
     *
     * **At the outermost position of an argument, and nowhere else**, which is the guardrail rather
     * than a shortcut: needing a conversion is part of an argument fitting its parameter, and
     * nothing else in the language converts. A variable reached *inside* a type - `Boxed(a)` given a
     * `Boxed(I32)` and a `Boxed(I64)` - is two different records and there is no conversion between
     * them; `bindResult` does not come here at all, because a result is what the call produces and a
     * result that needs converting has not decided anything. The shipping stage draws the line in
     * the same place, with `widen` false for a result and at every depth.
     */
    if(unifies && base[declared]->kind == Type::Variable) {
        auto& variable = *(VariableType*)base[declared];

        if(variable.owner == owner && variable.index < arguments.size() && variable.index < 64) {
            auto mask = U64(1) << variable.index;
            auto held = arguments[variable.index];

            /*
             * The meet, and it applies to a position **an earlier argument decided** and to no
             * other. `fromArgument` is what says which those are, and the two it excludes are the
             * two that are promises rather than offers: a functional dependency or a field
             * requirement filled through `fillFromRequirements` - `class Enum(a -> w)` says what
             * width an enum's value has, and an argument is in no position to widen it - and the
             * call's own result, which `bindResult` fills with `unifies` off for the same reason
             * the shipping stage's `widen` is false there.
             *
             * Through `canonicalInt` on the held side too, because a binding that already *accepts*
             * this argument is not two positions to unify: `Maybe(@bits(4) U32)` bound from the
             * destination and a `U32` argument are the same type to `matchType`, and meeting them
             * here would replace the refinement with the width it refines.
             */
            if(held && (fromArgument & mask) && canonicalInt(base, held) != value) {
                if(auto common = resolver.commonWiden(held, value, kNullLocation)) {
                    widened = widened || common != held;
                    arguments[variable.index] = common;
                    return;
                }
            }

            if(!held) {
                matchType(base, resolver.program.types, owner, arguments, declared, value);
                if(arguments[variable.index]) fromArgument |= mask;
                return;
            }
        }
    }

    matchType(base, resolver.program.types, owner, arguments, declared, value);
}

void TypeArguments::seed(RecordType& expected) {
    // The instantiation of the declaration this is for, so the two lists are the same length by
    // construction - `resolveConstruction` found the record through the same `instanceOf`. Asserted
    // rather than assumed, because reading past the end here would fill a slot with whatever the
    // next record's arguments happen to be and the result would still be a well-formed type.
    assertTrue(expected.arguments.size() == arguments.size());

    for(Size i = 0; i < arguments.size(); i++) arguments[i] = expected.arguments.get(resolver.decl(), i);
}

/*
 * What one clause of a context **promises** about a class, at a deciding prefix - the clause itself,
 * or a superclass of it expressed in the clause's own arguments.
 *
 * The closure is the point. `class (Num(a), FromDecimal(a), Lanewise(a, m), Bitwise(m)) Real(a -> m)`
 * is a class whose `m` no argument of it names: what says what `m` is is `Lanewise(a -> m)` in its
 * own context. So a body written `fn (Real(a, m)) magnitudeOf(value: a)` calling `value .<= 0.0`
 * asks `Lanewise(a, ?)`, and the clause it wrote is `Real` - a search for the class as written finds
 * nothing, and every comparison in `lib/Math` reported "no argument decides what m is", 24 of them.
 *
 * `classProves` already states this closure for the *discharge* half, which is why the two had to be
 * one rule rather than two: a requirement a signature can prove is a requirement it can be asked to
 * decide from. What is different here is the direction - a superclass's arguments come back and are
 * read - so the walk carries the answer rather than a yes.
 *
 * Bounded by a depth rather than by a visited set, on `superclassPath`'s reading: a superclass cycle
 * is a declaration error caught where classes are declared, and a real hierarchy stacks few.
 */
static constexpr U32 kSuperclassDepth = 8;

static bool heldPromises(Resolver& resolver, DeclPtr<Class> heldClass, const DeclPtr<Type>* heldArgs,
                         Size heldCount, DeclPtr<Class> klass, const DeclPtr<Type>* deciding,
                         U16 decidingCount, U32 depth, SmallArray<DeclPtr<Type>, 4>& into) {
    auto base = resolver.decl();
    if(!heldClass || !depth) return false;

    if(heldClass == klass && heldCount == base[klass]->arity(base)) {
        auto same = true;
        for(U16 i = 0; i < decidingCount && same; i++) same = sameType(heldArgs[i], deciding[i]);

        if(same) {
            into.clear();
            for(Size i = 0; i < heldCount; i++) into.push(heldArgs[i]);
            return true;
        }
    }

    auto gen = base[heldClass]->gen;
    if(!gen || base[gen]->variables.size() != heldCount) return false;

    Substitution with { gen, (DeclPtr<Type>*)heldArgs, heldCount };

    for(auto superclass: base[gen]->classes.contents(base)) {
        // The class's own self-requirement, which `declareClass` writes and which is the step this
        // walk just took. Following it is how a depth budget gets spent on one class.
        if(superclass.klass == heldClass) continue;

        SmallArray<DeclPtr<Type>, 4> expressed;
        for(auto argument: superclass.args.contents(base)) {
            expressed.push(substituteType(resolver.context, resolver.program, argument, with));
        }

        if(heldPromises(resolver, superclass.klass, expressed.pointer(), expressed.size(), klass,
                        deciding, decidingCount, depth - 1, into)) {
            return true;
        }
    }

    return false;
}

// The same, over every clause the enclosing signature wrote - which is the answer that does not
// depend on which instances happen to be declared.
static bool contextPromises(Resolver& resolver, DeclPtr<GenContext> gen, DeclPtr<Class> klass,
                            const DeclPtr<Type>* deciding, U16 decidingCount,
                            SmallArray<DeclPtr<Type>, 4>& into) {
    auto base = resolver.decl();
    if(!gen) return false;

    for(auto held: base[gen]->classes.contents(base)) {
        auto args = requirementArgs(base, held);

        if(heldPromises(resolver, held.klass, args.data, args.count, klass, deciding, decidingCount,
                        kSuperclassDepth, into)) {
            return true;
        }
    }

    return false;
}

/*
 * One requirement's determined positions, read off whatever proves its deciding ones.
 *
 * Answers whether anything moved, which is what the fixpoint above it needs. The write-back is
 * `matchType` rather than an assignment, because a determined position is a *type* rather than a
 * variable: `Contiguous(c, Array(e))` deciding the second position binds `e` inside it.
 */
static bool fillFromRequirement(Resolver& resolver, GenRequirement requirement,
                                TypeArguments& typeArguments, LocationId source) {
    auto base = resolver.decl();
    auto owner = typeArguments.owner;
    auto& into = typeArguments.arguments;
    auto& klass = *base[requirement.klass];
    auto arity = klass.arity(base);
    if(klass.determined >= arity) return false;

    Substitution with { owner, into.pointer(), into.size() };

    SmallArray<DeclPtr<Type>, 4> applied;
    auto wanted = false;

    for(U32 i = 0; i < arity; i++) {
        auto argument = substituteType(resolver.context, resolver.program,
                                       requirement.args.get(base, i), with);

        // A deciding position that is still open decides nothing, so there is no question to ask
        // yet - a later round of the fixpoint may bind it.
        if(!klass.isDetermined(i) && !argument) return false;

        wanted = wanted || (klass.isDetermined(i) && !isConcreteType(base, argument));
        applied.push(argument);
    }

    if(!wanted) return false;

    SmallArray<DeclPtr<Type>, 4> answer;

    /*
     * The enclosing signature's own requirement first, because it is the answer that does not depend
     * on which instances happen to be declared.
     *
     * Inside `fn (Contiguous(c, a)) first(xs: c)` the deciding position is the enclosing `c`, and
     * looking that up in the instance table would either find nothing or - worse - find an instance
     * for a type this body was never compiled against. The requirement the signature wrote is what
     * promised the relation, so it is what answers.
     */
    auto enclosing = resolver.currentFunction ? resolver.currentFunction->gen : nullptr;
    auto deciding = klass.determined;

    // Closed over the clauses' own superclasses - `heldPromises`, where the `Real(a -> m)` shape
    // that made the closure necessary is written down.
    if(enclosing) {
        contextPromises(resolver, enclosing, requirement.klass, applied.pointer(), U16(deciding),
                        answer);
    }

    if(!answer.size()) {
        /*
         * The instance table, asked with whatever the substitution left in the deciding positions -
         * `doc/progress/2026-09-06-inventory.md` D13.
         *
         * A **concrete** prefix is the ordinary question and `selectInstance` answers it. A prefix
         * that still carries this body's own variables is answerable too where its outermost
         * constructor is not one of them: `xs[i]` inside `fn (Ord(a)) firstBelow(xs: [a], ...)` asks
         * `Index(Array(a), ?, ?)`, and a head matching `Array(a)` with `a` free matches it at every
         * type a specialization binds. `selectOpenInstance` is where that is argued and where a bare
         * variable head is refused; the answer it gives fills `k` and `v` in, which is the whole of
         * what the dependency promised and is what the signature would otherwise have had to write
         * out as a second clause.
         *
         * Only the concrete question sets `missingClass`. An open one that finds nothing is not a
         * mistake about the class - the body may simply be one whose signature has to say what it
         * needs - so the position is left undecided and reported as that.
         */
        auto concrete = true;
        for(U32 i = 0; i < deciding; i++) concrete = concrete && isConcreteType(base, applied[i]);

        InstanceMatch found;
        for(U32 i = deciding; i < arity; i++) applied[i] = nullptr;

        if(!concrete) {
            /*
             * Not every open prefix is a question - `openHeadAnswerable`, asked here rather than
             * left to the selection so that a shape this may not be asked about costs no diagnostic.
             * `fillFromRequirements` runs once from `prepareArgument` before the argument that binds
             * the deciding position has been resolved, so the first sight of any requirement is a
             * variable of the *callee's* own context; a message there would name a type nobody wrote
             * and would spend the one report below on it.
             */
            auto enclosing = resolver.currentFunction ? resolver.currentFunction->gen : nullptr;

            if(!openHeadAnswerable(resolver.program, requirement.klass, applied.pointer(),
                                   applied.size(), enclosing)) {
                return false;
            }

            if(!selectOpenInstance(resolver.context, resolver.program, requirement.klass,
                                   applied.pointer(), applied.size(), enclosing, source,
                                   !typeArguments.reportedOpen, found)) {
                typeArguments.reportedOpen = true;
                return false;
            }

            for(auto argument: found.arguments) answer.push(argument);
        } else if(!selectInstance(resolver.context, resolver.program, requirement.klass,
                                  applied.pointer(), applied.size(), source, false, found)) {
            // A concrete question with no answer, which is what the position's openness means from
            // here on - `TypeArguments::reportUndecided`. The first such, since one is a reason.
            if(!typeArguments.missingClass) {
                typeArguments.missingClass = requirement.klass;
                for(U32 i = 0; i < deciding; i++) typeArguments.missingDeciding.push(applied[i]);
            }

            return false;
        } else {
            for(auto argument: found.arguments) answer.push(argument);
        }
    }

    auto moved = false;
    for(U32 i = deciding; i < arity; i++) {
        SmallArray<DeclPtr<Type>, 4> before;
        for(auto argument: into) before.push(argument);

        matchType(base, resolver.program.types, owner, into, requirement.args.get(base, i),
                  answer[i]);

        for(Size j = 0; j < into.size(); j++) moved = moved || into[j] != before[j];
    }

    return moved;
}

/*
 * One **field requirement's** result, read off whatever decides its subject.
 *
 * `a.field: b` is a functional dependency the syntax states outright: the subject is the deciding
 * position and the result is the determined one, so unlike a class there is nothing to count -
 * naming the field is what says which way the arrow points. Without this, `fn (a.mark: b) f(x: a)`
 * is a signature no call can satisfy, because nothing in the argument list mentions `b` and the
 * caller has no way to write it down; the requirement promises the relation and this is what spends
 * the promise.
 *
 * Two answers and no third, on `fillFromRequirement`'s terms and split the same way, except that
 * here the two cases are disjoint rather than ordered - a subject is either a record or a variable:
 *
 *  - a **record** answers with the field's own declared type, which is the whole of what the
 *    requirement was asking about;
 *  - a **variable** is answered by the enclosing signature having required the same field of the
 *    same subject. That is what makes `fn (a.mark: b) outer(x: a) = inner(x)` compile: `inner`'s own
 *    requirement is about `outer`'s `a`, and what promised its result is the constraint `outer`
 *    wrote rather than any record.
 *
 * The write-back is `matchType` and not an assignment, for the reason the class case gives: a
 * determined position is a *type* rather than a variable, so `a.items: Array(b)` against a concrete
 * `Array(I32)` binds `b` inside it.
 *
 * Nothing here checks. `dischargeProperty` does, from the same substitution, and it has to be the
 * one that does: a result the arguments had already decided differently is a *disagreement* rather
 * than a missing answer, and it is reported against the field it was wrong about.
 */
static bool fillFromProperty(Resolver& resolver, GenProperty property, DeclPtr<GenContext> owner,
                             SmallArray<DeclPtr<Type>, 4>& into) {
    auto base = resolver.decl();
    Substitution with { owner, into.pointer(), into.size() };

    // A subject nothing has decided yet decides nothing in turn - a later round of the fixpoint may
    // bind it, which is exactly the case `fn (a.mark: b, Peek(b)) g(x: a)` needs.
    auto subject = substituteType(resolver.context, resolver.program, property.subject, with);
    if(!subject) return false;

    auto result = substituteType(resolver.context, resolver.program, property.result, with);
    if(result && isConcreteType(base, result)) return false;

    DeclPtr<Type> answer = nullptr;

    if(base[subject]->kind == Type::Record) {
        for(auto field: ((RecordType*)base[subject])->fields.contents(base)) {
            if(field.name != property.field) continue;
            answer = field.occurrence.type;
            break;
        }
    } else {
        auto enclosing = resolver.currentFunction ? resolver.currentFunction->gen : nullptr;

        if(enclosing) {
            for(auto held: base[enclosing]->properties.contents(base)) {
                if(held.subject != subject || held.field != property.field) continue;
                answer = held.result;
                break;
            }
        }
    }

    // No such field, or a subject this signature promised nothing about. Both are reported by
    // `dischargeProperty` against what the author wrote, so there is nothing to say here.
    if(!answer) return false;

    SmallArray<DeclPtr<Type>, 4> before;
    for(auto argument: into) before.push(argument);

    matchType(base, resolver.program.types, owner, into, property.result, answer);

    for(Size i = 0; i < into.size(); i++) {
        if(into[i] != before[i]) return true;
    }

    return false;
}

/*
 * Which of the positions the **result hint** filled a functional dependency is the one entitled to
 * answer - the mask `fillDetermined` reopens before its fixpoint runs.
 *
 * A position is one of these when the variable standing in it is written *bare* in a determined
 * position of one of this context's own requirements and nowhere in that requirement's deciding
 * prefix. Bare, because a determined position naming a structure - `Contiguous(c, Array(e))` -
 * decides `e` inside it and reopening `e` would throw away what the argument bound one level down;
 * and not in the prefix, because a variable the dependency is asked *by* is not one it answers.
 */
static U64 resultDecidedDeterminedPositions(DeclBase base, DeclPtr<GenContext> owner, U64 fromResult) {
    if(!fromResult) return 0;

    U64 mask = 0;

    for(auto requirement: base[owner]->classes.contents(base)) {
        auto& klass = *base[requirement.klass];
        auto arity = klass.arity(base);
        if(klass.determined >= arity) continue;

        U64 deciding = 0;
        for(U16 i = 0; i < klass.determined; i++) {
            auto written = requirement.args.get(base, i);
            if(written && base[written]->kind == Type::Variable) {
                auto& variable = *(VariableType*)base[written];
                if(variable.owner == owner && variable.index < 64) deciding |= U64(1) << variable.index;
            }
        }

        for(U16 i = klass.determined; i < arity; i++) {
            auto written = requirement.args.get(base, i);
            if(!written || base[written]->kind != Type::Variable) continue;

            auto& variable = *(VariableType*)base[written];
            if(variable.owner != owner || variable.index >= 64) continue;

            auto bit = U64(1) << variable.index;
            if((fromResult & bit) && !(deciding & bit)) mask |= bit;
        }
    }

    return mask;
}

/*
 * The determined positions, and the one rule about who decides them: **a dependency beats the
 * target**.
 *
 * `class Enum(a -> w)` promises that one enum has one width, so in `fn seekBase(w: Whence) -> I64 =
 * valueOf(w)` the `I64` is not what `w` *is* - it is what the caller wants the answer converted to.
 * Binding the result first wrote `I64` into the position the dependency answers, and the lookup then
 * asked for `Enum(Whence, I64)` and reported a missing instance for what the author wrote as an
 * ordinary widening. `prepareArgument` already states the same sentence one position earlier - a
 * determined position is decided before the argument offers an answer - and this is the half of it
 * the result position needed.
 *
 * So a position the *result hint* filled and a requirement determines is put back to open, the
 * fixpoint runs, and the hint's answer is restored only where nothing answered - the wish stands
 * where there is no promise to beat it, and `coerce` at the call's result is then where the
 * conversion happens or is refused. A position an **argument** filled is never reopened: that one is
 * the type of a value that already exists rather than a wish about this call.
 */
void TypeArguments::fillDetermined(LocationId source) {
    auto reopen = resultDecidedDeterminedPositions(resolver.decl(), owner, fromResult);

    SmallArray<DeclPtr<Type>, 4> wished;

    if(reopen) {
        for(Size i = 0; i < arguments.size(); i++) {
            auto held = i < 64 && (reopen & (U64(1) << i)) ? arguments[i] : nullptr;
            wished.push(held);
            if(held) arguments[i] = nullptr;
        }
    }

    fillFromRequirements(source);

    for(Size i = 0; i < wished.size(); i++) {
        if(!arguments[i]) arguments[i] = wished[i];
    }

    fillDeclaredDefaults();
}

void TypeArguments::bindResult(DeclPtr<Type> declared, DeclPtr<Type> supplied) {
    if(!any()) return;

    SmallArray<bool, 4> before;
    for(auto argument: arguments) before.push(bool(argument));

    // With the meet off, which is the same line the shipping stage draws: a *result* that would need
    // converting has not decided the type arguments by itself, so it fills what nothing filled and
    // never moves what an argument did. See `bind`, where `fromArgument` is argued.
    auto meets = unifies;
    unifies = false;

    bind(declared, supplied);

    unifies = meets;

    for(Size i = 0; i < arguments.size() && i < 64; i++) {
        if(arguments[i] && !before[i]) fromResult |= U64(1) << i;
    }
}

void TypeArguments::fillFromRequirements(LocationId source) {
    if(!any() || !undecided()) return;

    auto base = resolver.decl();

    /*
     * A fixpoint, because one requirement's answer is another's question: `Contiguous(c, e)` beside
     * `Index(e, k, v)` decides `e` in the first round and `k` and `v` in the second, and which order
     * the two were written in is not something a caller should have to think about.
     *
     * Bounded by the number of arguments rather than by a number anybody picked. A round that moves
     * fills at least one position that was null - `matchType` only ever writes into an empty one -
     * so there are at most that many productive rounds, and the bound is the reason it terminates
     * rather than a guess about how many are enough.
     */
    for(Size round = 0; round <= arguments.size(); round++) {
        auto moved = false;

        for(auto requirement: base[owner]->classes.contents(base)) {
            moved = fillFromRequirement(resolver, requirement, *this, source) || moved;
        }

        // Beside the class requirements and in the same fixpoint, because the two decide each
        // other: `a.mark: b` answers `b` and a `Peek(b)` beside it may then answer something else,
        // and which order the author wrote them in is not a question a caller should have to have.
        for(auto property: base[owner]->properties.contents(base)) {
            moved = fillFromProperty(resolver, property, owner, arguments) || moved;
        }

        if(!moved || !undecided()) break;
    }
}

/*
 * What the *declaration* says a position is where nothing at the call decided it - `fn (n: Int = 0)
 * splat(value: a) -> Vec(a, n)`, whose `n` is the natural lane count wherever a caller wrote no
 * ascription.
 *
 * Last, after the requirements' fixpoint, and the order is the rule rather than an implementation
 * detail: a default is what a position means when nothing else says, so anything that *does* say -
 * an argument, the result hint, a functional dependency - has to have been asked first. Otherwise
 * `splat(x) :: Vec(F32, 2)` would be a two-lane request answered with the target's natural count.
 *
 * Substituted through what is already decided, exactly as a type application's defaults are, so a
 * head whose default mentions an earlier parameter reads what that parameter turned out to be.
 * `fillDefaults` is the same statement for an application; this is the call-site half, and the two
 * are separate because an application fills a *suffix* of a written list while a call fills whatever
 * inference left empty.
 */
void TypeArguments::fillDeclaredDefaults() {
    auto base = resolver.decl();
    auto& declared = *base[owner];

    for(Size i = 0; i < arguments.size(); i++) {
        if(arguments[i] || i >= declared.defaults.size()) continue;

        auto written = declared.defaults.get(base, U32(i));
        if(!written) continue;

        Substitution with { owner, arguments.pointer(), arguments.size() };
        arguments[i] = substituteType(resolver.context, resolver.program, written, with);
    }
}

DeclPtr<Type> TypeArguments::undecided() const {
    for(Size i = 0; i < arguments.size(); i++) {
        if(!arguments[i]) return resolver.decl()[owner]->variables.get(resolver.decl(), i);
    }

    return nullptr;
}

bool TypeArguments::reportUndecided(LocationId source, StringView decider, const String& where) {
    auto open = undecided();
    if(!open) return false;

    // A reason already given - `reportedOpen`. The position is open because two heads answered it
    // differently, which is what the author has to fix; "no argument decides this" said after it is
    // the same fact from the side that cannot be acted on.
    if(reportedOpen) return true;

    auto& context = resolver.context;
    auto base = resolver.decl();
    auto name = context.findName(((VariableType*)base[open])->name);

    if(missingClass) {
        String deciding;
        for(Size i = 0; i < missingDeciding.size(); i++) {
            deciding = Tritium::format("%@%@%@", deciding, i ? ", "_v : ""_v,
                                       typeText(context, base, missingDeciding[i]));
        }

        context.diagnostics.error(
            "there is no instance of %@ for %@, and that instance is what would decide %@ in this %@"_v,
            source, context.findName(base[missingClass]->name), deciding, name, where);
        return true;
    }

    context.diagnostics.error("no %@ decides what %@ is in this %@"_v, source, decider, name, where);
    return true;
}

/*
 * What this call has to prove about the types it chose - the one place a constraint is discharged.
 *
 * Two answers and no third, which is what keeps a generic body and its specializations agreeing. If
 * the argument is concrete, the instance has to exist *here*, at the call: `present(3)` needs
 * `Show(I32)`. If it is one of the enclosing declaration's variables, the enclosing declaration has
 * to require it: `fn (Show(b)) outer(y: b) = present(y)` passes the obligation on rather than
 * proving it, because at this point in the program there is nothing to prove it with.
 *
 * What is deliberately not a third answer is looking in the instance table for a *variable*. A body
 * that did would be answering a question about its own type parameter from the instances that
 * happen to be declared, which is exactly what makes a specialization mean something different from
 * the body it came from.
 *
 * A class member's own call is the same question and needs no clause of its own: a class requires
 * itself of its own parameter (`declareClass`), so `size(x)` is a call to a signature whose context
 * lists `Describe(a)`, and that is the requirement this loop discharges.
 */
bool Resolver::dischargeRequirements(Function& callee, TypeArguments& typeArguments,
                                     LocationId source) {
    if(!typeArguments.any()) return true;

    auto with = typeArguments.with();
    auto ok = true;

    for(auto requirement: decl()[callee.gen]->classes.contents(decl())) {
        ok = dischargeRequirement(requirement, with, source) && ok;
    }

    for(auto property: decl()[callee.gen]->properties.contents(decl())) {
        ok = dischargeProperty(property, with, source) && ok;
    }

    for(auto callable: decl()[callee.gen]->callables.contents(decl())) {
        ok = dischargeCallable(callee, callable, with, source) && ok;
    }

    return ok;
}

/*
 * `f: (a) -> b` proved for one argument list - the callable half of `dischargeRequirement`.
 *
 * The same shape as the other two and the same two answers. A signature that is **concrete** after
 * substituting names a function this module has or does not; one still mentioning a variable is
 * answered by the enclosing signature having promised the same thing, because a body generic in `a`
 * has no function to look up for it.
 *
 * The lookup and the signature check are **not here**. They are `matchCallable`, because the
 * specialization made from this same argument list has to reach the same function: two walks would
 * be a call admitted against one signature and lowered against another. What is left here is the
 * reporting, which is the half that only a call site can do.
 */
bool Resolver::dischargeCallable(Function& callee, GenCallable callable, const Substitution& with,
                                 LocationId source) {
    CallableWitness witness;

    // The generic's own module, and not this call's - `matchCallable` states why, and states that it
    // is a limit. What matters here is that it is the *same* module the specialization will ask, so
    // a call admitted by this check reaches the function the check looked at.
    if(!matchCallable(context, program, callee.module, callable, with, witness)) return true;

    /*
     * A signature still mentioning a variable, which names nothing to look up.
     *
     * What answers it is the enclosing declaration having promised the same thing - the same "pass
     * the obligation on" answer `dischargeRequirement` gives a class requirement, and for the same
     * reason: at this point in the program there is nothing to prove it with.
     */
    if(!witness.concrete) {
        auto enclosing = currentFunction ? currentFunction->gen : nullptr;

        if(enclosing) {
            for(auto held: decl()[enclosing]->callables.contents(decl())) {
                if(held.name != callable.name) continue;
                if(held.args.size() != witness.args.size()) continue;
                if(!sameType(held.result, witness.result)) continue;

                auto agrees = true;
                for(Size i = 0; i < witness.args.size(); i++) {
                    agrees = agrees && sameType(held.args.get(decl(), U32(i)), witness.args[i]);
                }

                if(agrees) return true;
            }
        }

        context.diagnostics.error(
            "this call needs %@ to be callable at these types, and the signature does not require it"_v,
            source, context.findName(callable.name));
        return false;
    }

    if(!witness.found) {
        context.diagnostics.error("this call needs a %@ taking %@ arguments, and there is none"_v,
                                  source, context.findName(callable.name),
                                  U64(witness.args.size()));
        return false;
    }

    if(!witness.agrees) {
        context.diagnostics.error("this call needs %@ to take %@, and it does not"_v, source,
                                  context.findName(callable.name),
                                  typeText(context, decl(),
                                           witness.args.size() ? witness.args[0] : witness.result));
        return false;
    }

    return true;
}

/*
 * `a.field: b` proved for one argument list - the property half of `dischargeRequirement`.
 *
 * The same two answers in the same order, for the same reason. A **concrete** subject has the field
 * or does not, and the record is right there to ask. A subject that is one of the *enclosing*
 * signature's variables has no fields to look at - `a` inside a body generic in `a` is not a record
 * - and what answers instead is the requirement that signature declared about the same variable.
 *
 * Reported here so that a mismatch names the call. The conversion checks it again when the index is
 * spent, and the second check is not a duplicate: this one is about the argument list a call wrote,
 * and that one is about every specialization the program eventually makes - including the ones a
 * *class member* reaches through an instance, which no call site here ever sees.
 */
bool Resolver::dischargeProperty(GenProperty property, const Substitution& with,
                                 LocationId source) {
    auto subject = substituteType(context, program, property.subject, with);
    auto result = substituteType(context, program, property.result, with);

    // A position the substitution has not filled, on `dischargeRequirement`'s terms: the caller has
    // already reported an argument nothing decides, and a second message about what it would have
    // had to have is noise.
    if(!subject || !result) return true;

    if(!isConcreteType(decl(), subject)) {
        auto enclosing = currentFunction ? currentFunction->gen : nullptr;

        if(enclosing) {
            for(auto held: decl()[enclosing]->properties.contents(decl())) {
                if(held.subject != subject || held.field != property.field) continue;
                if(!sameType(held.result, result)) continue;

                return true;
            }
        }

        context.diagnostics.error(
            "this call needs %@ to have a field %@ of type %@, and the signature does not require it"_v,
            source, typeText(context, decl(), subject), context.findName(property.field),
            typeText(context, decl(), result));
        return false;
    }

    if(decl()[subject]->kind == Type::Record) {
        auto& record = *(RecordType*)decl()[subject];
        for(auto field: record.fields.contents(decl())) {
            if(field.name != property.field) continue;

            if(!sameType(field.occurrence.type, result)) {
                context.diagnostics.error(
                    "%@ has a field %@ of type %@, and this call needs one of type %@"_v, source,
                    typeText(context, decl(), subject), context.findName(property.field),
                    typeText(context, decl(), field.occurrence.type),
                    typeText(context, decl(), result));
                return false;
            }

            return reachesInside(record, source, "its fields"_v);
        }
    }

    context.diagnostics.error("%@ has no field called %@, and this call needs one"_v, source,
                              typeText(context, decl(), subject),
                              context.findName(property.field));
    return false;
}

bool Resolver::dischargeRequirement(GenRequirement requirement, const Substitution& with,
                                    LocationId source, bool report) {
    SmallArray<DeclPtr<Type>, 4> arguments;
    auto concrete = true;

    for(auto argument: requirement.args.contents(decl())) {
        auto supplied = substituteType(context, program, argument, with);

        // A position the substitution has not filled, which every caller has already reported as
        // one no argument decides. Answering true is what keeps a call that could not infer its
        // arguments from reporting a second time about what those arguments would have had to prove.
        if(!supplied) return true;

        concrete = concrete && isConcreteType(decl(), supplied);
        arguments.push(supplied);
    }

    if(concrete) {
        InstanceMatch found;
        return selectInstance(context, program, requirement.klass, arguments.pointer(),
                              arguments.size(), source, report, found);
    }

    auto enclosing = currentFunction ? currentFunction->gen : nullptr;
    if(satisfiesRequirement(context, program, enclosing, requirement.klass, arguments.pointer(),
                            arguments.size())) {
        return true;
    }

    /*
     * And an instance whose head **matches with this body's variables still in it**, which is a
     * requirement no signature has to state.
     *
     * `fn null() -> %a = bitcast(0 :: Size)` needs `Bitcast(Size, %a)`, and `Bitcast(Size, %b)` is an
     * instance that holds for *every* `b` - so there is nothing for the signature to require and
     * demanding a `(Bitcast(Size, %a))` clause would be demanding a proof of something already
     * proved. The argument list here is not concrete, but it is not a bare variable either: what
     * decides the head is the pointer constructor around it.
     *
     * **Sound because a head is a pattern.** If it matches with `a` free, it matches with `a` at
     * every type a specialization binds - and the instance's own context is discharged at those
     * bindings, which is what makes this ordinary instance resolution rather than a hole.
     *
     * `selectOpenInstance` rather than `selectInstance` since D13, and it is the same statement made
     * twice: a *bare variable* head is refused here rather than looked up, a narrower head that
     * would answer a specialization differently is refused, and the instance's own context may be
     * discharged from what **this** declaration promised - `instance (Tag(a)) Sized(Pair(a), a)`
     * reached from `fn (Tag(a)) measured(p: Pair(a))` is the shape, and the table has nothing to say
     * about `Tag` of a body's own variable.
     *
     * Tried second, so a declared requirement always wins. It is the cheaper answer and the one an
     * author wrote down.
     */
    InstanceMatch matched;
    if(selectInstance(context, program, requirement.klass, arguments.pointer(), arguments.size(),
                      source, false, matched)) {
        return true;
    }

    /*
     * And the same table once more with the *enclosing* context carried in - D13, and additive:
     * everything above is unchanged, and what this reaches is only an instance whose own requirements
     * this declaration promised. `instance (Tag(a)) Sized(Pair(a), a)` reached from
     * `fn (Tag(a)) measured(p: Pair(a))` is the shape - the head matches, and `Tag` of a body's own
     * variable is a goal the table has nothing to say about, so the selection above stops at the
     * context while this one gets past it.
     */
    if(selectOpenInstance(context, program, requirement.klass, arguments.pointer(),
                          arguments.size(), enclosing, source, false, matched)) {
        return true;
    }

    if(report) {
        context.diagnostics.error("this call needs %@, and the signature does not require it"_v,
                                  source, appliedClassText(context, decl(), requirement.klass,
                                                           arguments.pointer(), arguments.size()));
    }

    return false;
}

/*
 * The expression written at a `@lazy` position - `doc/spec/functions.md`'s Deferred arguments.
 *
 * A thunk, except where the argument is *already* one. `f(flag, v)` inside a callee that declared
 * `@lazy v` hands the thunk it was given straight on rather than wrapping it in a second one: the
 * two would run the same expression through one more indirection, and the one permission to run it
 * travels with the value either way - which is why reading the name here is the same read
 * `checkForced` counts, and why using `v` again in that body is refused even though this body never
 * ran it.
 */
Node* Resolver::resolveDeferred(Builder& builder, ast::Expr& written, DeclPtr<Type> expected) {
    assertTrue(expected && decl()[expected]->kind == Type::Fun);

    auto expr = &written;
    while(expr->kind == ast::Expr::Nested) expr = parse[expr->nested];

    if(expr->kind == ast::Expr::Var) {
        if(auto local = findLocal(expr->var)) {
            if(deferredParameter(*local)) return readDeferred(builder, *local, expr->source);
        }
    }

    return resolveThunk(builder, written, expected, written.source);
}

// See the declaration. One diagnostic for both call paths, in the single string this used to be
// copied into: the single-callee path knows the parameter it is filling and the overloaded path
// knows only whether `agreedShape` found every candidate agreeing it is deferred, so `lazy` is
// asked for rather than a `Arg` handed in.
bool Resolver::checkDotReceiverNotLazy(LocationId source, StringId name, bool lazy) {
    if(!lazy) return true;

    context.diagnostics.error(
        "%@ defers its first argument, so it cannot be called with a dot - the receiver has to be evaluated to decide the form, and a `@lazy` position may not be. Write it as %@(...)"_v,
        source, context.findName(name), context.findName(name));
    return false;
}

bool Resolver::resolveArguments(Builder& builder, ast::AppExpr& written, Function& callee,
                                const ArgMap& map, LocationId source, SmallArray<Node*, 8>& into,
                                TypeArguments& typeArguments, DeclPtr<Type> hint,
                                Receiver receiver) {
    /*
     * One slot per declared position, filled in **written** order.
     *
     * The two orders are different and both matter. `doc/spec/expressions.md` puts a named argument's
     * evaluation where it is written and not where its parameter is, so the walk below is over the
     * source; what a call *hands over* is in declaration order, so the slot an argument lands in is
     * its parameter's. Pre-sized with nulls for the reason `resolveConstruction` pre-sizes its own -
     * a position still being resolved has no node yet, which is exactly what `Carry` skips.
     */
    for(U32 i = 0; i < map.count(); i++) into.push(nullptr);

    // Two positions at one variable meet here rather than the second being refused - see
    // `TypeArguments::unifies`, and `widenPreparedArguments` below, which is the other half of it.
    typeArguments.unifies = true;

    // The arguments already in hand, while the next one is resolved: an `if` in argument three puts
    // a branch between arguments one and two and the call that uses them.
    HeldList<SmallArray<Node*, 8>> held(*this, into);

    auto at = U32(0);

    /*
     * The receiver, first because it was written first - `Receiver`.
     *
     * It is an argument in every respect the loop below states, and it reaches the same
     * `prepareArgument`; what differs is only where the value comes from, since the expression was
     * resolved into a place before the callee was known. `readPlace` at the parameter's own request
     * is exactly what `resolveValue` would have spent on a bare name in this position, so `&self`
     * borrows the receiver's storage and `->self` moves out of it with no rule written twice.
     *
     * What it does not get is **pushdown**: its type is what decided this was a dot-call rather than
     * a field projection, so it was resolved before any candidate could say what it expected. Every
     * written argument keeps pushdown, which is what indexing the map from `at` rather than from
     * zero preserves.
     */
    if(receiver) {
        auto position = map.position(at++);
        auto& parameter = *decl()[callee.args.get(decl(), position)];

        if(!checkDotReceiverNotLazy(receiver.source, callee.name, parameter.lazy)) return false;

        auto value = readPlace(builder, receiver.source, *receiver.place,
                               requestOf(parameter.occurrence));
        if(!value) return false;

        into[position] = prepareArgument(builder, *value, parameter, typeArguments,
                                         receiver.source);
        if(!into[position]) return false;
    }

    /*
     * The written arguments, in **turns**: an argument whose type is the *position's* to decide is
     * resolved after the ones that decide it, and `turnOf` below is the whole of the order.
     *
     * `select(steep, 5.2359877559829887e-1, zeroLike(value))` in `lib/Math` is the case, at
     * `Lanewise(v -> m)`'s `select(mask: m, ifTrue: v, ifFalse: v)`: written left to right, the
     * decimal binds `v` to its own default width and the third argument - which knows what `v` is -
     * then arrives at a position already decided, and is reported against it. A literal has no type
     * of its own until a position gives it one, so it is not the thing that should be deciding one.
     *
     * The overloaded path has made this split for a literal since J8 (`undecidedLiteral`, where a
     * candidate's viability at a literal operand is read off its *other* parameters); this is that
     * rule at a call with one callee, which is where the whole of `lib/Math`'s kernels are.
     * `resolveOne` is shared rather than written per turn so the turns cannot drift.
     */
    auto resolveOne = [&](ast::TupArg& argument, U32 position) -> bool {
        auto& parameter = *decl()[callee.args.get(decl(), position)];
        auto declared = parameter.occurrence.type;
        auto wanted = requestOf(parameter.occurrence);

        /*
         * A `@lazy` position, which is the one place an argument is *not* resolved here - the
         * expression becomes a thunk and the thunk is what is passed.
         *
         * `expected` rather than `hint`, and that is the difference the deferred position makes to
         * inference: a hint is null while anything it mentions is open, and the thunk's result is
         * exactly the position that may still be open - `fn f(@lazy v: a)` has nothing but the
         * argument to decide `a` with. So the lift is handed the position with a variable still in
         * it and takes the G7c route a lambda in an open position already takes, and `bind` below
         * reads the answer back off what it built.
         */
        /*
         * What this position expects, from the arguments before it and then from the destination.
         *
         * The second is the fallback and never an override - `TypeArguments::resultHint`, where the
         * difference between hinting a position and deciding it is argued. A `@lazy` position takes
         * `expected` instead for the reason written below, and takes it open.
         */
        auto expectedType = typeArguments.hint(declared);
        if(!expectedType) {
            expectedType = typeArguments.resultHint(declared, callee.result.type, hint);
        }

        /*
         * An array literal at a fixed array whose count is the callee's own variable - `shuffle(v,
         * [3, 2, 1, 0])` against `pattern: [Int *m]`.
         *
         * The hint above is null while `m` is open, and a literal with no hint is a growable array,
         * which the parameter refuses. The count is the one part of the position the literal decides
         * for itself, so the position is hinted with its own element and the written count, and
         * `bind` reads `m` off what was built exactly as it does off an ascribed literal. An element
         * that is open as well is left to an ascription: there is no type to build the elements at.
         */
        if(!expectedType && argument.value.kind == ast::Expr::Array) {
            auto open = typeArguments.expected(declared);

            if(decl()[open]->kind == Type::Array) {
                auto& array = (ArrayType&)*decl()[open];
                if(!array.hasFixedLength() && !mentionsContext(decl(), array.element, typeArguments.owner)) {
                    auto count = U64(argument.value.arr.contents(parse).size());
                    expectedType = program.types.arrayType(decl(), array.element, count);
                }
            }
        }

        /*
         * An **argument** keeps what an `iter fn` call answers - §2.1's third row.
         *
         * The token is this argument's own source, so `f(lex(s))` builds an object and hands it over
         * while `f(g(lex(s)))` builds one for `g`'s parameter and refuses the outer position by
         * name. A `@lazy` position does not get one: what fills it is a thunk, and an object built
         * inside a thunk is one the deferred call builds when it runs.
         */
        Node* value = nullptr;

        if(parameter.lazy) {
            value = resolveDeferred(builder, argument.value, typeArguments.expected(declared));
        } else {
            ObjectScope object(*this, argument.value.source);
            value = resolveValue(builder, argument.value, expectedType, wanted);
        }

        if(!value) return false;

        into[position] = prepareArgument(builder, *value, parameter, typeArguments,
                                         argument.value.source);

        return into[position] != nullptr;
    };

    /*
     * Whether this argument is one of those - asked where the argument stands, so that what counts
     * as "the position has not been decided" is the state the walk is actually in.
     *
     * Narrow on purpose: a literal at a position the call has already decided - every concrete
     * parameter, and every variable an earlier argument bound - is resolved where it is written,
     * because there is nothing for the reorder to buy and the code a dump shows should be the code
     * the author wrote. `@lazy` is left out for the same reason: what fills one is a thunk, and its
     * result type is the position taken open by construction.
     */
    /*
     * Which turn an argument is resolved on, and the order is **how much the argument itself knows**.
     *
     * `0` is a value with a type of its own: it decides what it decides, and it decides it first.
     * `1` is an untyped number, which has a *default* and so can decide a position nothing else
     * does - `select(steep, 5.2e-1, zeroLike(v))` is the case, where reading it first bound the
     * class argument to the literal's own width and the third argument was then reported against a
     * position it had the answer to. `2` is a **lambda**, which knows nothing at all:
     * `apply((v) -> v + 1, 3)` at `fn (a, b) apply(f: (a) -> b, x: a)` takes `v`'s type from the
     * position, and the position is made of the callee's variables until `x` has bound them - so it
     * goes after the literal, which could still be what binds them.
     *
     * Only where the position is **undecided**, so an argument at a concrete parameter - and at a
     * variable an earlier argument already bound - is resolved where it was written. There is
     * nothing for the turn to buy there, and a dump should show the code the author wrote.
     */
    auto turnOf = [&](ast::TupArg& argument, U32 position) -> U32 {
        auto& parameter = *decl()[callee.args.get(decl(), position)];
        if(parameter.lazy) return 0;
        if(!typeArguments.any() || !typeArguments.undecided(parameter.occurrence.type)) return 0;

        auto written = &argument.value;
        while(written->kind == ast::Expr::Nested) written = parse[written->nested];
        if(written->kind == ast::Expr::Fun) return 2;

        intrinsic::Known role = intrinsic::Known::Count;
        return undecidedLiteral(argument.value, role) ? 1 : 0;
    };

    /*
     * Decided on the first turn and **remembered**, never asked twice: the question is about the
     * state the walk was in when the argument was passed over, and by the later turns the arguments
     * it was held back for are exactly the ones that have changed the answer. Asking again there
     * skipped the argument on every turn and handed the callee a null.
     *
     * **Evaluation order is unaffected**, which is what makes the reorder legal at all: what is held
     * back is a constant or a lambda, neither of which has anything to observe about when it was
     * built - the same reason `fillDefaultArguments` runs after every written argument rather than
     * interleaved with them.
     */
    SmallArray<U32, 8> turns;
    auto first = at;
    auto index = first;

    for(auto argument: written.args.contents(parse)) {
        auto position = map.position(index++);

        auto turn = turnOf(argument, position);
        turns.push(turn);
        if(turn) continue;

        if(!resolveOne(argument, position)) return false;
    }

    for(U32 turn = 1; turn <= 2; turn++) {
        /*
         * What the callee's own **requirements** decide, before the arguments that have nothing of
         * their own are resolved - the fixpoint `prepareArgument` already runs one position at a
         * time, asked once here for the positions no argument will ever answer.
         *
         * `sortBy(xs, (l: Int, r: Int) -> l < r)` at `fn (Writable(c, a)) sortBy(&xs: c, before:
         * (a, a) -> Bool)` is why: `a` is the *element*, which the dependency answers from `c` and
         * which no argument of this call supplies. Without it the lambda's position is still
         * `(a, a) -> Bool` when its turn comes, and a lambda at a position that states nothing is
         * the diagnostic - about a call that had the answer.
         *
         * The declared defaults are deliberately not run with it: `fillDeclaredDefaults` is what a
         * position means when *nothing* says, and the result hint has not been read yet.
         */
        if(turn == 2 && typeArguments.any()) typeArguments.fillFromRequirements(source);

        index = first;
        auto which = Size(0);

        for(auto argument: written.args.contents(parse)) {
            auto position = map.position(index++);
            if(turns[which++] != turn) continue;

            if(!resolveOne(argument, position)) return false;
        }
    }

    if(!widenPreparedArguments(builder, callee, map, source, into, typeArguments)) return false;

    return fillDefaultArguments(builder, callee, map, source, into);
}

/*
 * The widening a **later** argument decided, applied to the ones already prepared.
 *
 * `TypeArguments::bind` unifies two positions at one variable to their `Widen` upper bound, so
 * `checkAtMost(shown(x), bound(x))` at `(Size, Int)` binds `Size` from the second argument after the
 * first has already been coerced into `Int`, and `valueOf(e) == want` at `(I32, I64)` does the same
 * to an operator's left operand. The walk resolves one position at a time by design - a position is
 * decided before its argument offers an answer, which is what a functional dependency needs - so the
 * only place an earlier argument can be brought up to the answer is here, once the walk is over.
 *
 * **Integers only, and only where the two differ**, which is what keeps this from being a second
 * conversion pass: a widening between two integer widths is a `cast` with nothing to observe about
 * when it runs, and every other way a value's type can differ from its position's - a view adapted
 * from a container, a borrow of the declared type - was settled by `prepareArgument` and must not be
 * reconsidered here. A `@lazy` position holds a thunk and is left alone for the same reason.
 *
 * Called from every walk that prepares a call's positions - the written one above, the overloaded
 * one in `resolve_overload.cpp`, the operator chain, and `callFunction` - each immediately before
 * `fillDefaultArguments`, which is the point at which every written position is in hand.
 * `callFunction`'s own `typeArguments` is `unifies` only where a caller handed it one already solved
 * - `selectOperatorCall`'s - so calling this unconditionally there costs nothing at every other call:
 * `widened` never went true and the early return above is the whole of what runs.
 */
bool Resolver::widenPreparedArguments(Builder& builder, Function& callee, const ArgMap& map,
                                      LocationId source, SmallArray<Node*, 8>& into,
                                      TypeArguments& typeArguments) {
    if(!typeArguments.any() || !typeArguments.widened) return true;

    for(U32 position = 0; position < map.count(); position++) {
        if(!into[position]) continue;

        auto& parameter = *decl()[callee.args.get(decl(), position)];
        if(parameter.lazy) continue;

        auto held = valueType(*into[position]);
        auto expected = typeArguments.expected(parameter.occurrence.type);

        if(!held || !expected || sameType(held, expected)) continue;
        if(decl()[held]->kind != Type::Int || decl()[expected]->kind != Type::Int) continue;

        auto widened = coerce(builder, *into[position], expected, source, "an argument"_v,
                              parameter.occurrence.contract.access());
        if(!widened) return false;

        into[position] = widened;
    }

    return true;
}

/*
 * The positions the call left out, each built from the parameter's own constant.
 *
 * After every written argument rather than interleaved with them, and it costs nothing to say so: a
 * default is a constant, so there is no evaluation for an order to be observable in. What it buys is
 * that the argument walk is exactly the source order, with nothing spliced into it.
 *
 * Through `holdBorrowed` like any other argument, which is the whole of `doc/spec/functions.md`'s
 * "built at each call site that leaves the position out": a constant filling a borrowing position is
 * an rvalue and needs a frame slot to be borrowed from, and the code the author could have written
 * would have gone through the same line.
 *
 * A **deferred** position takes a thunk over its constant instead, for the reason it takes a thunk
 * over a written expression: what the callee holds is `() -> T`, and a value of `T` there would be
 * the one shape the marker promises does not arrive. `defaultThunk` is where the rest of that is
 * argued.
 *
 * A `@caller` position is the same walk over a value this call built rather than over a constant
 * the declaration wrote - see `callerFillValue`, and `Arg::omittable`, which is why the two arrive
 * here through one path at all.
 *
 * Its own function because both call paths end in it - the ordinary one above and the overloaded one
 * in `resolve_overload.cpp` - and what a position left out means is a property of the callee rather
 * than of how the callee was chosen.
 */
bool Resolver::fillDefaultArguments(Builder& builder, Function& callee, const ArgMap& map,
                                    LocationId source, SmallArray<Node*, 8>& into) {
    for(U32 position = 0; position < map.count(); position++) {
        if(!map.omitted(position)) continue;

        auto& parameter = *decl()[callee.args.get(decl(), position)];

        /*
         * A `@caller` position, whose value is built from *this* call's syntax rather than from a
         * constant the declaration wrote - `callerFillValue`, and the only difference between the
         * two ways a position gets filled. What follows is the line a written default goes through.
         */
        Node* value = nullptr;

        if(parameter.caller.isCallerFilled()) {
            value = callerFillValue(builder, parameter, map, source);

            /*
             * And the one adaptation a written argument gets and a constant does not need: what the
             * fill produces is an **owner** - `Site` holds two of them, and a text fill *is* one -
             * where a shared position of a viewed type manifests as the view. A `@caller` position
             * is concrete by construction (`declareCallerFill` admits a record or the text type and
             * nothing else), so the position it is adapted to is the declared occurrence itself and
             * there is no binding for it to take part in.
             *
             * The rest of `prepareArgument` is deliberately not run here. A default is a constant
             * that must not decide a generic position - see `fillFromRequirements` - and running
             * the whole of it for one of the two fills would make that rule depend on which.
             */
            if(value) value = adaptToView(builder, *value, parameter.occurrence, source);
        } else if(parameter.lazy) {
            value = defaultThunk(builder, callee, parameter, source);
        } else {
            value = constantValue(builder, parameter.def, source);
        }

        if(!value) return false;

        into[position] = holdBorrowed(builder, *value, requestOf(parameter.occurrence),
                                      parameter.occurrence.type, source);
    }

    return true;
}

bool Resolver::mapArguments(ast::ParseList<ast::TupArg> written, const ArgSite& site,
                            ArgMap& into) {
    // The leading positions are filled before the walk starts, because they were written before the
    // name - see `ArgSite::leading`. A site whose declared positions do not reach them is one the
    // arity lookup already excluded, and answering false is what keeps that a candidate filter
    // rather than an assertion.
    if(site.count < site.leading) return false;

    for(U32 i = 0; i < site.count; i++) into.sources.push(ArgMap::kOmitted);
    for(U32 i = 0; i < site.count; i++) into.written.push(kNullLocation);
    for(U32 i = 0; i < site.leading; i++) into.sources[i] = i;

    // The receiver of a dot-call, which is written before the list this walks - `ArgSite::leading`.
    // One position, since that is all a receiver ever is, and null where the site has none.
    for(U32 i = 0; i < site.leading; i++) into.written[i] = site.leadingSource;

    for(U32 i = 0; i < site.leading; i++) into.positions.push(i);
    for(Size i = 0; i < written.size(); i++) into.positions.push(ArgMap::kOmitted);

    auto ok = true;

    // The next position a *positional* argument fills, and whether a named one has been seen. The
    // two together are the whole of the ordering rule below.
    auto position = site.leading;
    auto anyNamed = false;

    auto index = site.leading;
    for(auto argument: written.contents(parse)) {
        auto at = index++;

        /*
         * **A positional argument may not follow a named one** - `doc/spec/expressions.md`.
         *
         * Nothing forces it. What "the next position" would count past is which ones the names
         * before it had already taken, and that is a question every *reader* of the call has to
         * answer before knowing what it does. A call that wants both writes the positional ones
         * first, which costs it a reordering and nothing else.
         */
        if(!argument.name) {
            if(anyNamed) {
                if(site.report) {
                    context.diagnostics.error(
                        "this is written by position and follows one written by name, so which %@ it fills would depend on the names before it - write the positional ones first"_v,
                        argument.value.source, site.one);
                }
                ok = false;
                continue;
            }

            /*
             * More written than there are positions. The whole site and not this argument, because
             * one argument too many is a statement about the count rather than about the value.
             *
             * Not reachable from a call, and that is the arity lookup rather than an assumption
             * here: a callee is found under the number of arguments written, so a call that got this
             * far wrote a count the signature answers. A construction has no such lookup - a
             * constructor is one declaration - and this is its count check.
             */
            if(position >= site.count) {
                if(site.report) {
                    context.diagnostics.error("%@ takes %@ %@ but was given %@"_v, site.source,
                                              context.findName(site.subject), U64(site.count),
                                              site.many, U64(site.leading + written.size()));
                }
                return false;
            }

            into.positions[at] = position;
            into.written[position] = argument.value.source;
            into.sources[position++] = at;
            continue;
        }

        anyNamed = true;

        // The first of that name, which is the only one: two positions sharing a name is refused
        // where they are declared, so a second match here cannot exist to be preferred.
        auto found = site.count;
        for(U32 i = 0; i < site.count; i++) {
            if(site.declared[i].name != argument.name) continue;

            found = i;
            break;
        }

        if(found == site.count) {
            if(site.report) {
                context.diagnostics.error("%@ has no %@ called %@"_v, argument.value.source,
                                          context.findName(site.subject), site.one,
                                          context.findName(argument.name));
            }
            ok = false;
            continue;
        }

        // Given twice, whichever way the two reached it: a name repeated, or a name naming what a
        // positional argument already filled. One rule, because the map has one slot per position.
        if(into.sources[found] != ArgMap::kOmitted) {
            if(site.report) {
                context.diagnostics.error("%@ is given twice"_v, argument.value.source,
                                          context.findName(argument.name));
            }
            ok = false;
            continue;
        }

        into.positions[at] = found;
        into.written[found] = argument.value.source;
        into.sources[found] = at;
    }

    /*
     * Every position the site left out that has nothing to fill it.
     *
     * All of them rather than the first, because a construction that forgot two fields is one
     * mistake in the author's head and two lines to write; and the message names the spelling that
     * fixes it, since a position reachable by name is reachable however the ones before it were
     * written.
     *
     * Only where every written argument landed somewhere, which is what `ok` is read for here: an
     * argument that was refused would have filled a position, and reporting that position as one the
     * site left out is a second message about the mistake one line above.
     */
    for(U32 i = 0; ok && i < site.count; i++) {
        if(!into.omitted(i) || site.declared[i].defaulted) continue;

        if(site.report) {
            context.diagnostics.error("%@ leaves out %@, which has no default - write it as `%@: value`"_v,
                                      site.source, context.findName(site.subject),
                                      context.findName(site.declared[i].name),
                                      context.findName(site.declared[i].name));
        }
        ok = false;
    }

    return ok;
}

/*
 * Whether a call may reach this declaration at all - see the declaration, where the four askers are.
 *
 * **A declaration with no body** is a call this stage cannot emit. It is separate from the summary's
 * own answer for one - `bodyKnown` false publishes "may suspend" and is the right fact - because
 * what stops here is code generation rather than analysis. A **class member** is not one of those,
 * and that is the whole of what a class call is: the declaration is a signature and what the call
 * reaches is the instance's body, which `callTarget` finds. Its summary is the selected member's for
 * the same reason - every pass that asks reads the target rather than the signature.
 *
 * **A lens in a value position** is the one place no call form reaches. What a lens answers is what
 * its continuation answered, and an expression has no continuation to give it: the two forms that
 * supply one are both statements (`resolveStatements` and `resolveScoped`), so a lens written
 * anywhere else has nothing to be. Naming both spellings is the whole of this diagnostic - the
 * scoped one, and the flat one whose extent is the rest of the statement's own block.
 */
bool Resolver::checkCallable(Function& callee, LocationId source, bool traversal) {
    // **An inferred result, demanded here.** A call reads the callee's result, so this is the point
    // at which "the body decides it" has to have happened - and it is the point at which a recursive
    // one is the call that closed the cycle. See `requireResult`.
    if(!requireResult(callee, source)) return false;

    if(!callee.summary.bodyKnown && !callee.klass && callee.intrinsic == intrinsic::Op::Count) {
        unsupported(source, "calling a function with no body"_v);
        return false;
    }

    // ... and not where the *position* is the continuation, which is the one place a lens is exactly
    // what was wanted: a `for` selects among the iterators of a name (`CandidateKind::Iterator`) and
    // then hands the winner the loop body. Everything above it is asked there too.
    if(callee.lens && !traversal) {
        /*
         * ... and not where the result is being **kept**, which is the object form -
         * `doc/inflight/Design-Iterator-Objects.md` §2.1's second row.
         *
         * The token is set around a `let`'s initializer alone, so what it admits is an iterator call
         * written anywhere inside one - and which of those is the `let`'s own call is `finishCall`'s
         * question, asked by source position, because that is where the answer is needed. An
         * iterator nested inside the initializer is refused there, by the message that names the
         * positions stage 4 adds.
         */
        if(callee.iterator && objectCallSource != kNullLocation) return true;

        // An iterator's continuation is a loop body and nothing else, so the lens spellings are the
        // wrong advice for one. Every lens call form sends an iterator here - see `lensCallee` - and
        // the words are the shipping stage's, so both stages refuse it alike.
        if(callee.iterator) {
            context.diagnostics.error(
                "%@ is an iterator, so this call has no body to hand its values to - write it as the "
                "source of a `for` loop, which is the only thing that supplies one"_v,
                source, context.findName(callee.name));
            return false;
        }

        context.diagnostics.error(
            "%@ is a lens, so it needs a continuation - write `%@(...):` with a block under it, or "
            "`let x = %@(...)` where what it hands over is used"_v,
            source, context.findName(callee.name), context.findName(callee.name),
            context.findName(callee.name));
        return false;
    }

    return true;
}

/*
 * Whether the call's shape matches the signature at all, before any argument is resolved.
 *
 * The mapping is about the call as a whole and does not leave a position to carry on from: which
 * parameter each argument fills is what every later step reads, so resolving the arguments first and
 * reporting afterwards would produce one true diagnostic and a run of consequences of it.
 *
 * The **written** parameters and not all of them, which is what `writtenArity` is for: a lens's
 * continuation is the desugaring's own parameter and no call site fills it.
 */
bool Resolver::checkCallShape(ast::AppExpr& written, Function& callee, LocationId source,
                              ArgMap& into, bool report, U32 leading, LocationId leadingSource) {
    SmallArray<ArgPosition, 8> parameters;
    for(U32 i = 0; i < callee.writtenArity(); i++) {
        auto& parameter = *decl()[callee.args.get(decl(), i)];
        parameters.push(ArgPosition { parameter.name, parameter.omittable() });
    }

    ArgSite site;
    site.declared = parameters.pointer();
    site.count = U32(parameters.size());
    site.subject = callee.name;
    site.one = "parameter"_v;
    site.many = "arguments"_v;
    site.source = source;
    site.report = report;
    site.leading = leading;
    site.leadingSource = leadingSource;

    return mapArguments(written.args, site, into);
}

/*
 * `-x` and `!x` - a call of the prefix operator at one operand.
 *
 * A negated literal is folded into the constant rather than emitted as a negation - see
 * `resolveConstant`, which does the same for a global's initializer and says why it matters, and
 * `role.h`'s `kNegationOperator`, which is the one spelling this involves.
 *
 * `!` is `op.def`'s `truthNot` - the truth test of the operand and one `not` - laid out here rather
 * than expanded from a prepared value, because the truth test is instance selection and the
 * expansion of a `Node` role is handed values already selected. Everything else is `Num.-` or
 * whatever the name reached, called at one operand.
 */
Node* Resolver::resolvePrefix(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint) {
    auto& written = *parse[expr.prefix];

    if(written.op.kind != ast::Expr::Var) {
        context.diagnostics.error("a prefix operator must be named"_v, written.op.source);
        return nullptr;
    }

    if(isNegation(context, parse, expr)) return literalValue(builder, written.on, hint, true);

    /*
     * R1's set at one operand, and the same two paths the infix form takes.
     *
     * A prefix operator is a call of one argument, so a name that reaches several declarations is
     * selected between exactly as `climb` selects: the operand is resolved on its own under what the
     * set agreed on, and `selectOperatorCall` is the choice. `Signed.-` beside a plain `-` over a
     * type of a program's own is the pair this admits.
     */
    OverloadSet set;
    findOverloads(context, program, here(), written.op.var, 1, written.op.source, set);

    if(set.isEmpty()) {
        reportMissingOperator(written.op.var, 1, written.op.source);
        return nullptr;
    }

    auto found = set.sole();

    if(!found) {
        Request request;
        if(!operatorRequests(set, written.op.var, 1, written.op.source, &request)) return nullptr;

        auto value = resolveValue(builder, written.on, nullptr, request);
        if(!value) return nullptr;

        Node* values[1] = { value };
        return selectOperatorCall(builder, set, written.op.var, expr.source, values, 1, hint);
    }

    auto& callee = *decl()[found];

    if(callee.intrinsic == intrinsic::Op::truthNot) {
        auto value = resolveValue(builder, written.on, nullptr, inspectRequest());
        if(!value) return nullptr;

        auto truth = truthValue(builder, *value, expr.source);
        if(!truth) return nullptr;

        return builder.addUnary(expr.source, UnaryOp::Not, *truth);
    }

    if(!checkCallable(callee, expr.source)) return nullptr;

    /*
     * What the operand is expected to be, asked as `resolveArguments` asks it of a written
     * argument - the two questions in that order, and neither of them a decision.
     *
     * The signature first, which for an operand of a sole callee is the declared parameter with
     * whatever earlier arguments bound; there are no earlier arguments here, so it answers wherever
     * the parameter is closed. Then the **result**, which is where a position's own type reaches an
     * operand at all: `-(value: a) -> a` at a position wanting `I64` reads its operand at `I64`.
     *
     * `resultHint` is speculative by construction - it matches into a copy and binds nothing - and
     * that is what the operator `*` needs: `(*p) :: I64` over a `%U8` would otherwise have `a`
     * decided as `I64` before the operand was read, and the pointer it was handed reported as the
     * wrong type. What the call is actually built with is bound below from the value that arrived.
     */
    auto& parameter = *decl()[callee.args.get(decl(), 0)];

    TypeArguments hinting(*this, callee.gen);
    auto expected = hinting.hint(parameter.occurrence.type);
    if(!expected) expected = hinting.resultHint(parameter.occurrence.type, callee.result.type, hint);

    auto value = resolveValue(builder, written.on, expected, requestOf(parameter.occurrence));
    if(!value) return nullptr;

    return callFunction(builder, callee, &value, 1, expr.source, hint);
}

/*
 * An operator no declaration in scope answers at this arity.
 *
 * Two messages, because they are two mistakes and `resolveCall` tells them apart the same way: a
 * name nobody declared, and a name declared at another arity - `-` at three operands is the second,
 * and so is an operator a program's own module shadowed with a plain function of one argument.
 */
void Resolver::reportMissingOperator(StringId name, U32 arity, LocationId source) {
    if(hasFunctionName(context, program, here(), name)) {
        context.diagnostics.error("no %@ takes %@ %@"_v, source, context.findName(name),
                                  U64(arity), arity == 1 ? "operand"_v : "operands"_v);
    } else {
        context.diagnostics.error("unknown operator %@"_v, source, context.findName(name));
    }
}

/*
 * The two halves of an operator, put together - the note on `Operator` in resolve_internal.h.
 *
 * The name says what it does and the *declaration* says how it groups, and there is no fallback for
 * the second: `Nothing` here means either that nothing in scope declares a function of that name at
 * two arguments or that nothing declared a fixity for it, and both are "this is not an operator I
 * can resolve". Each is reported once, naming what is missing.
 */
Maybe<Operator> Resolver::findOperator(StringId name, LocationId source, bool report) {
    /*
     * R1's whole set, not one declaration - `doc/spec/overloading.md`, and the reason it is the set
     * is `lib/Native`'s `+`: a pointer's offset and `Num`'s addition are one name at two operands,
     * and a stage that read only a sole candidate resolved neither in any file that could see both.
     *
     * The **fixity** is what flattening needs and it is a lookup of its own, so a set of several
     * costs the chain walk nothing: where the operators are is decided before which declaration each
     * one is, and `climb` is what asks the second question - by then it has operand types to ask it
     * with.
     */
    OverloadSet set;
    findOverloads(context, program, here(), name, 2, source, set);

    if(set.isEmpty()) {
        if(report) reportMissingOperator(name, 2, source);
        return Nothing();
    }

    auto callee = set.sole();

    // The one declaration this operator is, where there is one - `checkCallable`, asked here so that
    // a lens named as an operator is reported at the operator rather than as a continuation type
    // nothing decided. A set with something to choose between is asked the same question of its
    // winner, in `chooseCandidate`.
    if(callee && !report && decl()[callee]->lens) return Nothing();
    if(callee && report && !checkCallable(*decl()[callee], source)) return Nothing();

    /*
     * A fixity is a declaration and there is no fallback for it - see the note above.
     *
     * Reachable in one situation, and it is worth naming rather than reporting as a mystery: a
     * library declaring a two-argument function whose name is an operator and no `infixl` for it.
     * `doc/spec/declarations.md` is explicit that this is a mistake and not a default.
     */
    auto declared = mir::findFixity(context, program, here(), name);
    if(!declared) {
        if(report) {
            context.diagnostics.error(
                "no module in scope declares how `%@` groups, so this expression cannot be read - the library states that with `infixl`"_v,
                source, context.findName(name));
        }

        return Nothing();
    }

    /*
     * Which three are control flow is the callee's role and not its spelling - `op.def`'s `Flow`
     * rows, which is where the argument for them being roles at all is written.
     *
     * A set with something to choose between is a `Call`, because there is no callee yet to read a
     * role off. That is not a gap: laying an operator out as a branch is what a `Flow` role *is*, so
     * a name reaching both a `Flow` role and something else is a name whose operands one candidate
     * would evaluate and another would not - which `operatorRequests` refuses in those words.
     */
    auto form = OperatorForm::Call;

    if(callee) {
        auto& function = *decl()[callee];
        form = function.intrinsic == intrinsic::Op::truthAnd ? OperatorForm::ShortAnd
             : function.intrinsic == intrinsic::Op::truthOr  ? OperatorForm::ShortOr
                                                             : OperatorForm::Call;
    }

    return Just(Operator { set, name, callee, U8(declared.properties.precedence), form,
                           declared.properties.associativity });
}

/*
 * An infix chain, flattened.
 *
 * The parser nests to the right without regard for precedence, so `a + b * c` arrives as
 * `a + (b * c)` only by accident of the tokens. Flattening the whole chain first and climbing it
 * afterwards is what makes the grouping the fixity table's answer rather than the parser's.
 *
 * Separate from the climb because the *condition* walk has to look at the whole chain before it
 * climbs any of it - a conjunction is split into tests rather than folded into a value - and both
 * walks have to get the same answer about where the operators are.
 */
bool Resolver::flattenChain(ast::Expr& expr, Chain& chain, bool report) {
    auto node = parse[expr.infix];

    while(true) {
        if(node->op.kind != ast::Expr::Var) {
            if(report) {
                context.diagnostics.error("an infix operator must be a named operator"_v,
                                          node->op.source);
            }

            return false;
        }

        // Reports for itself - the two ways it can fail are two sentences.
        auto found = findOperator(node->op.var, node->op.source, report);
        if(!found) return false;

        chain.operands.push(&node->lhs);
        chain.operators.push(found.unwrap());
        chain.sources.push(LocationId(node->op.source));

        if(node->rhs.kind != ast::Expr::Infix) {
            chain.operands.push(&node->rhs);
            break;
        }

        node = parse[node->rhs.infix];
    }

    return true;
}

/*
 * Whether a flattened chain is a **conjunction** - `a && b`, at the top level of its own grouping.
 *
 * The chain's loosest operators are the ones the re-association makes outermost, so the question is
 * whether every one of them is `&&`. `a == 1 && b` splits (`==` binds tighter), `a && b || c` does
 * not (the `||` is looser and is what the whole expression is), and `a && b && c` splits twice.
 *
 * Asked of the flattened chain rather than of the written tree because the parser nests to the right
 * whatever the fixity: `a == 1 && b` arrives as `a == (1 && b)`, whose top node is the `==`. Reading
 * the grouping off the tree would therefore have answered about a different expression.
 */
bool Resolver::splitsAtConjunction(Chain& chain) {
    auto loosest = U8(0xff);
    for(Size i = 0; i < chain.operators.size(); i++) {
        if(chain.operators[i].precedence < loosest) loosest = chain.operators[i].precedence;
    }

    auto found = false;

    for(Size i = 0; i < chain.operators.size(); i++) {
        if(chain.operators[i].precedence != loosest) continue;
        if(chain.operators[i].form != OperatorForm::ShortAnd) return false;

        found = true;
    }

    return found;
}

/*
 * Whether a value this candidate answers could fill the position - the **result** half of R2.
 *
 * Two questions and neither is the other: `matchType` asks whether the declared result could *be*
 * the position under some binding of the candidate's own variables, which is what admits `Num.+`'s
 * `a` at an `Int`; `acceptsType` asks whether a value of a decided result converts into it, which is
 * what admits an `I32` answer at an `I64` position. `Native`'s `+(p: %a, n: Size) -> %a` answers
 * neither - no binding of `a` makes `%a` an integer - which is the whole of what this rules out.
 *
 * Reported nothing and binds nothing: the array is scratch, so a candidate this admits still solves
 * its signature from the arguments in the ordinary way.
 */
static bool resultCouldFill(Resolver& resolver, DeclPtr<Function> candidate, DeclPtr<Type> wanted,
                            LocationId source) {
    auto base = resolver.decl();
    auto& declared = *base[candidate];

    SmallArray<DeclPtr<Type>, 4> scratch;
    if(declared.gen) {
        for(Size i = 0; i < base[declared.gen]->variables.size(); i++) scratch.push(nullptr);
    }

    if(matchType(base, resolver.program.types, declared.gen, scratch, declared.result.type,
                 wanted)) {
        return true;
    }

    return resolver.acceptsType(declared.result.type, wanted, source);
}

Node* Resolver::resolveInfix(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint,
                             Request wanted) {
    Chain chain;
    if(!flattenChain(expr, chain)) return nullptr;

    /*
     * One operator whose set has something to choose between, narrowed by what the **position**
     * asks for - `doc/progress/2026-09-07-J21-retire-prelude.md`'s third finding.
     *
     * `import Native` puts the pointer `+` in the same set as `Num`'s, and
     * `fn add(a: I64, b: I64) -> Int = truncate(a) + truncate(b)` then reported "no argument decides
     * what `b` is": with something to choose between, both operands are resolved on their own, and
     * `truncate`'s result is exactly what has nothing of its own to be. A candidate whose result
     * cannot fill the position was never a candidate, and removing it leaves one - which is R5's
     * "only where the plain function is the whole overload set" reached by subtraction rather than
     * by ranking.
     *
     * **One operator, and that is the bound rather than a first step.** The hint is what the
     * *chain's* value fills, and only a chain of one has an operator whose result is that value:
     * `p + n * 4` at a `%U8` says nothing about what `*` answers, and a heterogeneous operator is
     * precisely the kind this narrowing is about. Where the chain is longer the loop below is what
     * decides whether the hint travels at all, unchanged.
     */
    if(hint && chain.operators.size() == 1 && !chain.operators[0].callee) {
        auto& op = chain.operators[0];

        DeclPtr<Function> only = nullptr;
        auto viable = Size(0);

        for(Size i = 0; i < op.set.size(); i++) {
            if(!resultCouldFill(*this, op.set.at(i), hint, expr.source)) continue;

            only = op.set.at(i);
            viable++;
        }

        // Nothing viable is left alone: the ordinary path reports what it reports, and a narrowing
        // that removed every candidate has said nothing an author could act on.
        if(viable == 1) {
            auto& function = *decl()[only];
            op.callee = only;
            op.form = function.intrinsic == intrinsic::Op::truthAnd ? OperatorForm::ShortAnd
                    : function.intrinsic == intrinsic::Op::truthOr  ? OperatorForm::ShortOr
                                                                    : OperatorForm::Call;
        }
    }

    /*
     * Whether the position's own type says anything about the chain's **leftmost operand**, which
     * is the one operand a hint can reach directly: every other one is reached through a callee,
     * and takes what that callee's signature relates it to.
     *
     * It does exactly when every operator in the chain answers with its own left operand's declared
     * type. `x + 1 * 2` at an `I64` position reads `x` at `I64`, because each of `+` and `*` says
     * the result is the operand; `countBits(v) == 13` at a `Bool` position reads neither operand at
     * `Bool`, because `==` answers `Bool` whatever it compared.
     *
     * Asked of the **whole chain** and not of the operator the climb happens to reach first, which
     * is the shape the bug had: `1 + 2 == 3` in a `Bool` position climbs into `+` first, and `+`
     * does relate its result to its operand - but what the position is being asked for is `==`'s
     * answer, so the `Bool` would have travelled down through the `+` and built `1` at it. There is
     * no `FromInt(Bool)` in any library, so that is a diagnostic on an expression nobody wrote
     * wrongly.
     *
     * An operator whose callee is not yet chosen - a set with more than one candidate - stops it
     * too, which is R5's last paragraph one syntax out: which signature applies is the question the
     * operands are being resolved to answer.
     */
    for(auto& op: chain.operators) {
        if(op.form == OperatorForm::Call && op.callee) {
            auto& callee = *decl()[op.callee];
            auto& left = *decl()[callee.args.get(decl(), 0)];

            if(sameType(callee.result.type, left.occurrence.type)) continue;
        }

        hint = nullptr;
        break;
    }

    Size operandIndex = 0;
    Size operatorIndex = 0;
    return climb(builder, chain, operandIndex, operatorIndex, 0, hint, wanted);
}

/*
 * Precedence climbing over a flattened chain, where each operator is a call.
 *
 * `hint` reaches the left operand, and the right one takes what the callee's second parameter is
 * once the first has bound it - `a` for `+(lhs: a, rhs: a)`, so `x + 1` types the literal from `x`,
 * and `1 + x` types `x` from a literal that defaulted, which is a real limit rather than an
 * oversight: local inference is what the language specifies and what is here. Read off the
 * signature through `TypeArguments`, so a callee whose two parameters are unrelated hands the right
 * operand nothing, which is what `&&`'s declaration says about its own.
 *
 * The left operand is prepared before the right is climbed, and held while it is - `HeldList`. This
 * is the position the whole mechanism was found by: `one + (if c then 2 else 0)` puts a branch
 * between a value and its use, and nothing about the addition says so.
 */
Node* Resolver::climb(Builder& builder, Chain& chain, Size& operandIndex, Size& operatorIndex,
                      U8 minimumPrecedence, DeclPtr<Type> hint, Request outer) {
    /*
     * What this operand is asked for, which is the request of the **parameter it will fill**.
     *
     * An operator is a call, so its operands are arguments and are subject to the rule
     * `resolveArguments` states: a position is resolved under `requestOf` its own contract, because
     * that is what decides whether a place is read as a copy or borrowed. `+=(&target: a, amount: a)`
     * is the case that made the difference visible - a shared read of `x` produces the *value*, and
     * `x += 1` then incremented a temporary and left `x` alone.
     *
     * Which position it is falls out of the loop below rather than being a second decision: this
     * operand is the *left* one of `operators[operatorIndex]` exactly when that operator is consumed
     * here, which is the same condition the `while` tests. Where it is not, the operand is the right
     * one of whatever called in - so the request travels down as `outer`, and the top-level caller's
     * own is what it starts as. `resolveInfix` always enters with a chain of at least one operator
     * and a minimum of zero, so the top level always takes the first branch.
     *
     * A short-circuit operator is not a call and has no parameter to read: its operands are truth
     * values, which `climbBool` asks for with the shared request every condition uses.
     */
    auto wanted = outer;

    // What an overloaded operator asks is the *set's* answer and not one declaration's - see
    // `operatorRequests`, where an irreconcilable disagreement is refused rather than picked
    // between. Held for the right operand too, which is asked the same question one rung down.
    Request operands[2] = { outer, outer };
    auto overloaded = false;

    // Whether the sole callee's left parameter is a position only the other operand can decide -
    // the second half of the deferral rule below.
    auto openLeft = false;

    if(operatorIndex < chain.operators.size() &&
       chain.operators[operatorIndex].precedence >= minimumPrecedence &&
       chain.operators[operatorIndex].form == OperatorForm::Call) {
        auto& op = chain.operators[operatorIndex];

        if(op.callee) {
            auto& callee = *decl()[op.callee];
            auto& left = *decl()[callee.args.get(decl(), 0)];

            wanted = requestOf(left.occurrence);
            openLeft = !isConcreteType(decl(), left.occurrence.type);
        } else {
            if(!operatorRequests(op.set, op.name, 2, chain.sources[operatorIndex], operands)) {
                return nullptr;
            }

            overloaded = true;
            wanted = operands[0];
        }
    }

    /*
     * **No hint** where the operator is overloaded - R5's last paragraph, one syntax out.
     *
     * A hint is the callee's parameter type read through the result, and which parameter that is is
     * exactly what selection has not decided yet. `resolveOverloadedCall` states the same rule for
     * the written form and for the same reason.
     *
     * An **undecided literal** operand is then not built at all - `undecidedLiteral`, where the rule
     * is. `48 + rest` with no hint is built at `I32` and fits neither `Native`'s pointer `+` nor
     * `Num.+`; left standing, it is what the *other* operand decides, and `selectOperatorCall`
     * builds it at the position the winner declared. Only where the operator is overloaded: where
     * the set has one callee its parameter type is known before the operand is, which is the
     * ordinary push-down and needs none of this.
     *
     * A **sole callee whose left parameter is open** defers for the same reason and by the same
     * mechanism - `0.5 + r` inside `fn (Real(a, m)) expSeries(r: a)`, and `1.0 / value` where
     * `value` is a `Double`. There is one signature, so nothing about *selection* is waiting; what
     * is waiting is the one thing that decides the position, which is the other operand. Built at
     * the default it reported `there is no instance of FromDecimal(Bool)` and `an argument must be
     * F32, and this is F64` against types the author never wrote, where `r + 0.5` beside it worked
     * because the right operand takes the hint the left one bound.
     *
     * Sound because a number has no effects: when it is built is not observable, only what it is
     * built at. What it costs is that the constant is emitted *after* the other operand in every
     * body that writes one, which is the golden reordering
     * `doc/progress/2026-09-06-J8-inference.md` measured and declined to pay before there was a
     * library site that needed it.
     *
     * Only where nothing else decided it. A `hint` reaching this operand is the position's own type
     * and is a better answer than either operand's; a concrete left parameter is the push-down that
     * already works.
     */
    ast::Expr* pending[2] = { nullptr, nullptr };
    intrinsic::Known literals[2] = { intrinsic::Known::Count, intrinsic::Known::Count };

    auto& leftOperand = *chain.operands[operandIndex++];
    Node* lhs = nullptr;

    auto defers = overloaded || (openLeft && !hint);

    if(defers && undecidedLiteral(leftOperand, literals[0])) {
        pending[0] = &leftOperand;
    } else {
        lhs = resolveValue(builder, leftOperand, overloaded ? nullptr : hint, wanted);
        if(!lhs) return nullptr;
    }

    while(operatorIndex < chain.operators.size() &&
          chain.operators[operatorIndex].precedence >= minimumPrecedence) {
        auto op = chain.operators[operatorIndex];
        auto source = chain.sources[operatorIndex];
        operatorIndex++;

        // One rung above this operator's own where it is left associative, which is what makes it
        // stop before an equal one and take that as its own left operand instead; its own rung where
        // it is right associative, which lets the equal one climb and become the right operand.
        auto next = U8(op.assoc == Assoc::Right ? op.precedence : op.precedence + 1);

        if(op.form != OperatorForm::Call) {
            lhs = shortCircuit(builder, chain, operandIndex, operatorIndex, next, source, lhs, op);
            if(!lhs) return nullptr;
            continue;
        }

        if(!lhs && !pending[0]) return nullptr;

        /*
         * The overloaded path: both operands resolved on their own, then one choice.
         *
         * `operands` was filled by the peek above, which is the same peek that decided the left
         * operand's request - so the right one is asked for what the set agreed on, and the two
         * halves of `resolve_overload.cpp`'s operator pair meet here with the values between them.
         */
        if(!op.callee) {
            HeldValue heldLeft(*this, lhs);

            /*
             * The right operand, which is a *span* of the chain rather than one expression - so it
             * is left for the selection to type only where the span is exactly one written literal.
             * That is the case whenever the next operator binds looser than this rung, which is the
             * same test the climb below would have used to stop.
             */
            auto loneRight = operandIndex < chain.operands.size() &&
                             (operatorIndex >= chain.operators.size() ||
                              chain.operators[operatorIndex].precedence < next);

            Node* operand = nullptr;

            if(loneRight && undecidedLiteral(*chain.operands[operandIndex], literals[1])) {
                pending[1] = chain.operands[operandIndex++];
            } else {
                operand = climb(builder, chain, operandIndex, operatorIndex, next, nullptr,
                                operands[1]);
                if(!operand) return nullptr;
            }

            Node* values[2] = { lhs, operand };

            lhs = selectOperatorCall(builder, op.set, op.name, source, values, 2, hint, pending,
                                     literals);
            if(!lhs) return nullptr;

            // What this operator left for the selection is spent. The next rung's left operand is
            // the call above, which is a value.
            pending[0] = pending[1] = nullptr;
            literals[0] = literals[1] = intrinsic::Known::Count;
            continue;
        }

        auto& callee = *decl()[op.callee];

        /*
         * The sole callee's own path, which is `callFunction` with the right operand resolved in
         * the middle of it - the one thing an operator does that a written call does not.
         *
         * It has to be in the middle: the right operand's expected type is what the *left* one
         * bound, which is `resolveArguments`' rule for the second argument of any call, and the
         * left operand has to be prepared before the climb so that a branch the right operand
         * builds - `one + (if c then 2 else 0)` - has the value already travelling its edges.
         * `positionalMap` and `fillDefaultArguments` are the same two steps `callFunction` takes
         * around them, and are what let an operator reach a declaration with a defaulted tail.
         */
        ArgMap map;
        positionalMap(callee, 2, map);

        auto& left = *decl()[callee.args.get(decl(), map.position(0))];
        auto& right = *decl()[callee.args.get(decl(), map.position(1))];

        TypeArguments typeArguments(*this, callee.gen);
        typeArguments.unifies = true;

        SmallArray<Node*, 8> arguments;
        for(U32 i = 0; i < map.count(); i++) arguments.push(nullptr);

        HeldList<SmallArray<Node*, 8>> held(*this, arguments);

        if(lhs) {
            auto prepared = prepareArgument(builder, *lhs, left, typeArguments, source);
            if(!prepared) return nullptr;

            arguments[map.position(0)] = prepared;
        }

        /*
         * A **`@lazy` right operand** - `m ?? fallback`, which is the one infix declaration in
         * `lib/Core` that defers a position and is not `&&` or `||`.
         *
         * The two are not one case: `&&` and `||` are `Flow` roles whose laziness *is* the branch
         * `shortCircuit` builds, so they never arrive here. An ordinary operator with a deferred
         * position wants what a written call's does - a thunk - and climbing the operand as a value
         * hands a `T` to a position typed `() -> T`, which is what the diagnostic said.
         *
         * **Only where the operand is one written expression**, because that is the only place there
         * is an AST node to lift: the right operand of a rung is a *span* of the chain, and a span
         * that re-associates into a call has no expression anybody can point at. The parenthesis is
         * the spelling that makes one, and the message says so - it is a real form and not a
         * workaround, since `(a + b)` is a single operand of this chain by construction.
         */
        Node* rhs = nullptr;

        if(right.lazy) {
            // The same test the climb below stops on: the next operator binds looser, so this rung's
            // right operand is exactly the operand standing here.
            auto lone = operandIndex < chain.operands.size() &&
                        (operatorIndex >= chain.operators.size() ||
                         chain.operators[operatorIndex].precedence < next);

            if(!lone) {
                context.diagnostics.error(
                    "%@ defers its right operand, and this writes an operator chain there - a deferred operand has to be one expression, so put it in brackets"_v,
                    source, context.findName(callee.name));
                return nullptr;
            }

            rhs = resolveDeferred(builder, *chain.operands[operandIndex++],
                                  typeArguments.expected(right.occurrence.type));
        } else {
            rhs = climb(builder, chain, operandIndex, operatorIndex, next,
                        typeArguments.hint(right.occurrence.type),
                        requestOf(right.occurrence));
        }

        if(!rhs) return nullptr;

        auto preparedRight = prepareArgument(builder, *rhs, right, typeArguments, source);
        if(!preparedRight) return nullptr;

        arguments[map.position(1)] = preparedRight;

        /*
         * And the left operand the peek left standing, now that the right one has bound its
         * position - the deferral above, spent.
         *
         * `typeArguments.hint` and not the declared type: what the literal is built at is what the
         * *other* operand decided the parameter to be, which is the whole point of waiting.
         */
        if(pending[0]) {
            auto built = resolveValue(builder, *pending[0],
                                      typeArguments.hint(left.occurrence.type), wanted);
            if(!built) return nullptr;

            auto preparedLeft = prepareArgument(builder, *built, left, typeArguments, source);
            if(!preparedLeft) return nullptr;

            arguments[map.position(0)] = preparedLeft;

            // Spent: the next rung's left operand is this call, which is a value.
            pending[0] = nullptr;
            literals[0] = intrinsic::Known::Count;
        }

        if(!widenPreparedArguments(builder, callee, map, source, arguments, typeArguments)) {
            return nullptr;
        }

        if(!fillDefaultArguments(builder, callee, map, source, arguments)) return nullptr;

        lhs = finishCall(builder, callee, arguments, typeArguments, hint, source);
        if(!lhs) return nullptr;
    }

    return lhs;
}

/*
 * The compiler's own arithmetic, said as the member it is.
 *
 * A counted `for` compares its counter and steps it, and a range pattern compares its pivot; none
 * of those has an operator written, and what each *is* is a member of `Ord` or `Num` at the value's
 * type - which is how `for i in a .. b` counts over anything with both instances and not only over
 * the widths. The map from the node an emitter would build to the member that builds it is the
 * one table here, and it is the same positions `known.def` states.
 */
static bool memberFor(BinaryOp op, intrinsic::Known& klass, U16& member) {
    using intrinsic::Known;

    switch(op) {
        case BinaryOp::Eq:  klass = Known::eq;  member = 0; return true;
        case BinaryOp::Ne:  klass = Known::eq;  member = 1; return true;
        case BinaryOp::Lt:  klass = Known::ord; member = 0; return true;
        case BinaryOp::Le:  klass = Known::ord; member = 1; return true;
        case BinaryOp::Gt:  klass = Known::ord; member = 2; return true;
        case BinaryOp::Ge:  klass = Known::ord; member = 3; return true;
        case BinaryOp::Add: klass = Known::num; member = 0; return true;
        case BinaryOp::Sub: klass = Known::num; member = 1; return true;
        case BinaryOp::Mul: klass = Known::num; member = 2; return true;
        case BinaryOp::Div: klass = Known::num; member = 3; return true;
        default: return false;
    }
}

Node* Resolver::applyBinary(Builder& builder, LocationId source, BinaryOp op, Node& lhs, Node& rhs) {
    intrinsic::Known klass;
    U16 member;
    auto named = memberFor(op, klass, member);
    assertTrue(named);

    Node* operands[2] = { &lhs, &rhs };
    return applyMember(builder, klass, member, operands, 2, source);
}

/*
 * `&&` and `||`, which are control flow rather than operations - `op.def`'s `Flow` roles.
 *
 * `a && b` is `if truthy(a) then truthy(b) else False` and `a || b` is `if truthy(a) then True else
 * truthy(b)`, which is what Core's own definitions say they are - the difference being that Core
 * writes them as functions with a `@lazy` second argument, and here the laziness is the branch
 * itself. Building them out of the same merge every `if` uses is the point: the right operand is a
 * span of the chain rather than an AST node anyone can point at, and climbing it *inside* the block
 * it belongs to is what makes it run only when it should. Each operand is asked its own `Truth`,
 * so the two need not share a type, which is what the declaration's two variables say.
 */
Node* Resolver::shortCircuit(Builder& builder, Chain& chain, Size& operandIndex, Size& operatorIndex,
                             U8 next, LocationId source, Node* condition, const Operator& op) {
    if(!condition) return nullptr;

    auto test = truthValue(builder, *condition, source);
    if(!test) return nullptr;

    auto shortValue = op.form == OperatorForm::ShortAnd ? false : true;

    /*
     * Two alternatives, the first guarded by the left operand - which is what `armsValue` is, and
     * why this needs no branch construct of its own.
     *
     * `ShortAnd` reaches its first alternative when the left operand was True, and that is the one
     * that runs the right one; `ShortOr` reaches it when the left was True and answers the constant
     * there, running the right operand in the second. Either way the guard is the same value and
     * only what the two alternatives contain differs.
     */
    return armsValue(builder, source, 2, booleanType(program), true,
        [&](U32 index, Block* onFail, DeclPtr<Type>, Carry& carry) -> Node* {
            if(onFail) failIfNot(builder, *test, *onFail, carry, source);

            auto climbs = (op.form == OperatorForm::ShortAnd) == (index == 0);
            if(climbs) return climbBool(builder, chain, operandIndex, operatorIndex, next, source);

            return constantBool(builder, source, shortValue);
        });
}

// The right operand of a short circuit, as the truth value it answers - climbed with no hint,
// because what a `Truth` instance is asked about is the operand's own type.
Node* Resolver::climbBool(Builder& builder, Chain& chain, Size& operandIndex, Size& operatorIndex,
                          U8 next, LocationId source) {
    auto value = climb(builder, chain, operandIndex, operatorIndex, next, nullptr,
                       inspectRequest());
    if(!value) return nullptr;

    return truthValue(builder, *value, source);
}

Node* Resolver::constantBool(Builder& builder, LocationId source, bool value) {
    return builder.addConst(source, truthConstant(program, value));
}

// See the declaration, where the difference between `merged` and a null `value` is argued.
bool Resolver::closeArm(Builder& builder, Block& end, Block& join, Node* value, bool merged,
                        LocationId source, Carry& carry, const Node* const* carried) {
    builder.setBlock(end);
    if(builder.isTerminated()) return false;

    if(merged && !value) {
        // The alternative produced no value and did not leave, which only happens after a
        // diagnostic. `unreachable` rather than a jump, because a merge it entered would need a
        // parameter it has nothing to supply.
        builder.addUnreachable(source);
        return false;
    }

    SmallArray<Operand, 8> arguments;

    if(merged) {
        /*
         * The merge's own parameter answers what filling it does, which is `argumentMode` and the
         * same rule a call argument and an aggregate component are given.
         *
         * `Take` for an owned merge: what is handed over is a node result, a value already in hand
         * with no other last use to weigh it against - `Forward` belongs to a bare *name* in sink
         * position, see UseMode. A merge of loans is not a handover at all, and saying `Take` there
         * claimed to consume storage the arm had only borrowed.
         */
        auto base = builder.base();
        auto& declared = valueOccurrence(*base[join.arguments.get(base, 0)]);
        arguments.push(builder.resultOf(*value, argumentMode(declared.contract, SourceKind::Result)));
    }

    // Then whatever crossed the branch, in the positions `armsValue` declared them in - and read
    // from where they stood at the *end of this alternative*, which is why the snapshot is a
    // parameter rather than the state now.
    carry.edge(builder, carried, arguments);

    builder.addJump(source, join, arguments.pointer(), arguments.size());
    return true;
}

// See the declaration. Everything here is a statement about *one* occurrence being the merge's, and
// the alternatives that do not have it are reported and dropped.
/*
 * Whether an expression's whole job is to **leave** - `return`, `break`, `continue`.
 *
 * `resolveStatement` has a case for each, and none of them produces a value: a position holding one
 * answers nothing, and everything after it on that path is unreachable. A parenthesis is transparent
 * to the question for the reason it is transparent to every other one - it is not a form, it is how
 * the source grouped the form inside it.
 *
 * A **block** leaves where its last statement does, which is the same question asked one level in:
 * the statements before it are the ones that run, and what stands at the end is what the position
 * would have answered with. Reading only the outer node made these two arms different shapes -
 *
 *     Filled(h): return h.value
 *     Filled(h):
 *         let v = h.value
 *         return v
 *
 * - and the second of them asked `resolveValue` for a `return`, which reaches nothing and was
 * reported as an expression this stage does not implement.
 */
bool Resolver::leavesBlock(ast::Expr& expr) {
    auto written = &expr;
    while(written->kind == ast::Expr::Nested) written = parse[written->nested];

    switch(written->kind) {
        case ast::Expr::Ret:
        case ast::Expr::Break:
        case ast::Expr::Continue:
            return true;
        case ast::Expr::Multi: {
            auto statements = written->multi;
            if(!statements.size()) return false;

            auto last = statements.get(parse, U32(statements.size() - 1));
            return leavesBlock(last);
        }
        default:
            return false;
    }
}

/*
 * One alternative's body - the arm of an `if`, an `if:` or a `match`.
 *
 * Three constructs and one rule, which is the whole reason this is a function rather than the two
 * lines it replaces in each of them: an arm is asked for a value where the construct is a value and
 * for nothing where it is a statement, and in **either** case it may leave instead of answering.
 *
 * `if c then return 7 else a` is what the third clause is for. `return e` is not an expression of
 * some bottom type that a merge then has to eliminate - it is the statement that ends the function,
 * so it is resolved as one, and what the arm answers is nothing. `armsValue` reads that off a null
 * value with a terminated block, which is the same shape an arm whose *block* ended in a `return`
 * already had; this is the bare form of it.
 */
Node* Resolver::resolveArm(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue,
                           Request wanted) {
    if(!wantValue || leavesBlock(expr)) {
        resolveStatement(builder, expr);
        return nullptr;
    }

    return resolveValue(builder, expr, hint, wanted);
}

/*
 * Analysis-Borrows.md §2.5's arm join, in the one shape it is built for - see the declaration.
 *
 * An **owned** alternative beside a call-bounded loan is the pair `lib/Test/Harness.native.yana`
 * writes, and it is the one a reader would not expect to be a question:
 *
 *     let directory = match variable("TMPDIR"):
 *         Just(->named) -> named
 *         Nothing -> "/tmp"
 *
 * `named` names storage that already exists and the literal produces some, which is
 * `doc/spec/ownership.md`'s two initializer kinds arriving at one binding. The join is the
 * **weaker** of them: the frame keeps the produced value in a temporary and the merge borrows it,
 * so what `directory` is worth is a loan over the union of both arms' origins and neither arm has
 * claimed anything it did not have. Weakening upward instead - copying the loan so the merge owns
 * one - is the allocation this language makes a program say out loud (`Copy`, `++`), and inserting
 * it at a merge would be the one place it happened silently.
 *
 * `holdBorrowed` is the whole of the edge, because it is already the rule for the same situation
 * one construct down: a produced value filling a borrowing position gets a frame slot and the
 * position reads a borrow of it. Emitted in the alternative's **own** block for `mergeArms`'
 * reason - the jump into the merge supplies the merge's occurrence, so anything that changes it has
 * to stand before the jump.
 *
 * What is deliberately *not* joined is two loans that disagree. A published origin weakened to a
 * call-bounded one, or an exclusive loan weakened to a shared one, changes what the merge is
 * carrying rather than where it came from - there is no `Read` of a value, only of a place - and the
 * refusal below is a better report than a merge quietly holding one arm's contract. A `TrivialCopy`
 * type never reaches either case: `promisesTrivialCopy` has already made every arm owned.
 */
void Resolver::joinArmLoans(Builder& builder, SmallArray<Block*, 8>& ends,
                            SmallArray<Node*, 8>& values, DeclPtr<Type> merged,
                            LocationId source) {
    Contract loan;
    auto lends = false;
    auto owns = false;

    for(Size i = 0; i < values.size(); i++) {
        if(!values[i]) continue;

        auto contract = valueOccurrence(*values[i]).contract;
        if(contract.isOwned()) { owns = true; continue; }

        // Only where every lending alternative already agrees, and only a loan the call bounds.
        // Both are the "not joined" case above, and reporting them is `mergeArms`'.
        if(contract.origin() != Origin::CallBounded) return;
        if(lends && contract != loan) return;

        loan = contract;
        lends = true;
    }

    if(!owns || !lends) return;

    auto standing = builder.currentBlock();

    Request wanted;
    wanted.access = loan.access();
    wanted.form = loan.form();

    for(Size i = 0; i < values.size(); i++) {
        if(!values[i] || !valueOccurrence(*values[i]).contract.isOwned()) continue;

        builder.setBlock(*ends[i]);
        values[i] = holdBorrowed(builder, *values[i], wanted, merged, source);
        ends[i] = builder.currentBlock();
    }

    if(standing) builder.setBlock(*standing);
}

/*
 * Two arms answering two **site types**, which is one closure written twice -
 * Design-Iterator-Objects.md §2.3.
 *
 * `let f = if c then (x: Int) -> x + a else (x: Int) -> x * b` is two lambdas, so it is two records,
 * and a merge has one occurrence that every edge supplies. There is no join over two layouts and
 * there should not be one: what the author wrote is a value that is *either* closure, which is what
 * the erased pair is for. So a site type crossing a merge that does not agree with the other arms is
 * adapted here, in the arm's own block, exactly as the widenings below are.
 *
 * Only where they disagree. Two arms answering the *same* site type are one closure reached two
 * ways - which today is only a lambda bound before the branch - and erasing them would spend an
 * allocation and an indirect call on a merge that already has one type.
 *
 * This is the one adaptation no position asks for, and the reason it is not one is that a merge has
 * no position: the alternatives are judged against each other. `mergeArms` reports what is left over
 * as an ordinary type mismatch, which is what a lambda and a number in two arms still is.
 */
void Resolver::eraseSiteArms(Builder& builder, SmallArray<Block*, 8>& ends,
                             SmallArray<Node*, 8>& values, LocationId source) {
    DeclPtr<Type> first = nullptr;
    auto sites = 0u;
    auto differ = false;

    for(Size i = 0; i < values.size(); i++) {
        if(!values[i]) continue;

        auto answered = valueType(*values[i]);
        if(siteTypeOf(answered)) sites++;

        if(!first) first = answered;
        else differ = differ || !sameType(first, answered);
    }

    if(!sites || !differ) return;

    auto standing = builder.currentBlock();

    for(Size i = 0; i < values.size(); i++) {
        if(!values[i]) continue;

        auto site = siteTypeOf(valueType(*values[i]));
        if(!site) continue;

        builder.setBlock(*ends[i]);
        values[i] = adaptSiteValue(builder, *values[i], *site, source);
        ends[i] = builder.currentBlock();
    }

    if(standing) builder.setBlock(*standing);
}

Node* Resolver::mergeArms(Builder& builder, Block& join, SmallArray<Block*, 8>& ends,
                          SmallArray<Node*, 8>& values, LocationId source) {
    /*
     * The type the merge answers, folded out of the alternatives: their unique `Widen` upper bound
     * where they do not already agree - `commonWiden`, which is where the rule is argued.
     *
     * Order-independent for the ladder the language has, because a widening is a total order over
     * the pairs that are related at all: whichever alternative is seen first, the fold ends at the
     * widest of them. An alternative that reaches neither the running answer nor the other way is
     * the one that is reported, and it is nulled rather than allowed to move the answer - so a
     * third alternative is still judged against what the ones before it agreed on.
     */
    eraseSiteArms(builder, ends, values, source);

    DeclPtr<Type> merged = nullptr;

    for(Size i = 0; i < values.size(); i++) {
        if(!values[i]) continue;

        auto answered = valueType(*values[i]);
        if(!merged) { merged = answered; continue; }

        auto common = commonWiden(merged, answered, source);
        if(!common) {
            context.diagnostics.error("the arms answer %@ and %@, which are different types"_v,
                                      source, typeText(context, decl(), merged),
                                      typeText(context, decl(), answered));
            values[i] = nullptr;
            continue;
        }

        merged = common;
    }

    if(!merged) return nullptr;

    /*
     * The widenings, each emitted at the end of the alternative that produced the value.
     *
     * An edge into the merge supplies the merge's own type, so the conversion has to stand in the
     * block the jump leaves from - and `closeArm` has not built that jump yet, which is what makes
     * this the one point where both the type and an unterminated arm block are in hand. The walk's
     * own position is put back afterwards, because nothing here is a statement of the construct.
     */
    auto standing = builder.currentBlock();

    for(Size i = 0; i < values.size(); i++) {
        if(!values[i] || sameType(valueType(*values[i]), merged)) continue;

        builder.setBlock(*ends[i]);
        values[i] = coerce(builder, *values[i], merged, source, "an arm"_v);

        // Where the arm leaves from, which the conversion may have moved: a `Widen` an author wrote
        // is an ordinary call, and the standing block after one is not always the block it started
        // in. The caller builds the jump out of this, so it is the block the last instruction of
        // the arm is in and never the block the arm began in.
        ends[i] = builder.currentBlock();
    }

    if(standing) builder.setBlock(*standing);

    // §2.5's join, in the one shape it is built for - see the definition.
    joinArmLoans(builder, ends, values, merged, source);

    Node* first = nullptr;

    for(Size i = 0; i < values.size(); i++) {
        if(!values[i]) continue;
        if(!first) { first = values[i]; continue; }

        auto& answered = valueOccurrence(*values[i]);
        auto& expected = valueOccurrence(*first);

        /*
         * And the same *ownership*, exactly.
         *
         * A merge parameter has one occurrence and every edge into it supplies that one - which the
         * verifier states as an equality and this is the source-level half of. Every alternative was
         * resolved against the same `Request`, so they agree on the interesting half already: a
         * position that copies copies everywhere and a position that borrows borrows everywhere.
         * What they can still disagree about is *whose* storage they borrowed - one arm a published
         * parameter and another a local - and §2.5's answer to that is the join, a loan weakened to
         * what every side can promise.
         *
         * The join is deliberately not built. Weakening a contract can change how the value is
         * carried - a shared call-bounded loan of a trivially copyable type is a register where a
         * published one is an address - so an arm whose contract differs needs a reborrow *on its
         * edge*, which is a node this milestone has no reason to have anywhere else. Refusing is the
         * honest report until it does; accepting and handing the merge one arm's contract is the
         * silent version of the same gap.
         *
         * The three `lib/Core` bodies that reported it were **not** it. Each had one arm holding a
         * borrowed parameter of a `TrivialCopy`-constrained variable and the other an arithmetic
         * result, and the copy upgrade - `promisesTrivialCopy` in type.cpp - makes both arms owned,
         * so there is nothing left to weaken. What still reaches here is an owned arm beside a
         * genuine loan and two loans of different origins, and nothing in `lib/` writes either;
         * whichever caller arrives first is what the reborrow should be designed against.
         */
        if(answered.contract != expected.contract) {
            context.diagnostics.error(
                "the arms borrow under different terms, and one value that is either is not implemented"_v,
                source);
            values[i] = nullptr;
            continue;
        }

        /*
         * And the same interior **shape**, which is the third part of the occurrence and the same
         * statement about it.
         *
         * The pair that reaches this is a lambda that captured something and one that did not: a
         * function type is one type either way, so the arms agree about the type and the contract
         * and disagree about whether there is a position inside naming this frame's storage. The
         * join is the shaped one - a value that may refer to something is what a merge of the two is
         * - and it is not built here for the reason the contract join is not.
         */
        if(shapeOf(decl(), answered) != shapeOf(decl(), expected)) {
            context.diagnostics.error(
                "the arms differ in what they refer to inside the value, and one value that is either is not implemented - a lambda that captures and one that does not are the pair that reaches this"_v,
                source);
            values[i] = nullptr;
        }
    }

    if(!first) return nullptr;

    /*
     * The merge's own parameter first, then the carried ones - the order `closeArm` reads.
     *
     * The alternative's whole occurrence rather than an owned one of its type. A merge of two loans
     * is a loan, and saying otherwise here is what made `if c then p.left else p.right` claim to own
     * storage the arms only borrowed.
     */
    return builder.addBlockArgument(join, valueOccurrence(*first), source);
}

/*
 * `if c then a else b`, as a value or as a statement.
 *
 * One function for both, because the difference is only what the arms are resolved as and whether
 * the merge takes a parameter. A statement `if` with no `else` still branches to a merge from both
 * sides, so the block structure a reader sees does not change with the presence of a value.
 */
Node* Resolver::resolveIf(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue,
                          Request wanted) {
    auto& written = *parse[expr.singleIf];

    /*
     * The condition, and the names a pattern in it binds - `resolve_match.cpp`.
     *
     * `if o is Proceed(v)` binds `v` for the *then* arm and for nothing else, which is why the
     * condition is not resolved as a value here: a value has nowhere to put a name, and the two arms
     * are what this has.
     */
    Chain chain;
    auto condition = resolveCondition(builder, written.cond, chain);
    if(!condition) return nullptr;

    if(wantValue && !written.otherwise) {
        context.diagnostics.error("an `if` used as a value needs an `else`"_v, expr.source);
        return nullptr;
    }

    /*
     * Two alternatives: the one the condition selects, and whatever is left.
     *
     * The same `armsValue` a `match` of two constructors builds, which is what makes `if x is p`
     * carry a nested pattern without a second mechanism: the pattern's tests all branch to the
     * second alternative's entry, however many of them there are.
     */
    return armsValue(builder, expr.source, 2, hint, wantValue,
        [&](U32 index, Block* onFail, DeclPtr<Type> armHint, Carry& carry) -> Node* {
            if(index == 1) {
                if(!written.otherwise) return nullptr;

                auto& otherwise = written.otherwise.unwrap();
                if(!wantValue) { resolveStatement(builder, otherwise); return nullptr; }

                return resolveValue(builder, otherwise, armHint, wanted);
            }

            // The names the condition bound, in scope for exactly this alternative. Recorded before
            // the tests, because a conjunction binds *between* them - `resolveTests`.
            auto scope = U32(bindings.size());

            SmallArray<PatternBinding, 4> made;
            if(!resolveTests(builder, condition, onFail, carry, made)) {
                bindings.resize(scope);
                return nullptr;
            }

            auto value = resolveArm(builder, written.then, armHint, wantValue, wanted);

            bindings.resize(scope);
            return value;
        });
}

/*
 * Whether this case is the `else` marker - the parser's encoding, read back once.
 *
 * `parseIfExpr` records `else` as the condition that always holds, so that an `if:` case list is one
 * production rather than two. Reading it here is what turns the marker back into a *shape* of the
 * construct - the one arm with no test - instead of a condition that happens to be constant, and
 * this is the only place below the parser that knows the encoding.
 *
 * The **last** case and no other. An `else` written above one is an ordinary always-true condition
 * and the cases under it are unreachable, which is what the program says and is not this function's
 * to rule on; what the position decides is whether anything can fall out of the list at all.
 */
static bool isElseCase(const ast::Expr& cond) {
    if(!ast::isLiteral(cond)) return false;
    if(ast::Literal::Kind(cond.kind - ast::Expr::Lit) != ast::Literal::Bool) return false;

    return cond.lit.b;
}

/*
 * `if:` with a case list - `doc/spec/expressions.md`'s multi-way if.
 *
 * The two-armed form written out to n arms, and `armsValue` is exactly that construct: each case's
 * tests branch to the *next* case's entry, so a case runs only once every case above it has failed.
 * Nesting two-armed branches instead would give each test a merge of its own to fail into, and the
 * cases after it would then be reached along a different path per test - which is the argument
 * `armsValue` makes and the reason this is a call of it rather than a loop around `resolveIf`.
 *
 * The condition is resolved **inside its own arm**, unlike the two-armed form's, and that is the
 * whole of what the case list means: case two's condition is code that runs after case one's failed,
 * so it belongs in the block that failure reaches. `if x is Just(v)` still binds `v` for its own arm
 * and for nothing else, since the tests and the body are in one arm here as they are there.
 */
Node* Resolver::resolveMultiIf(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue,
                               Request wanted) {
    auto cases = expr.multiIf;
    auto count = U32(cases.size());

    // The parser requires at least one case and reports where there is none, so this is a parse that
    // has already been told. Kept rather than merged with the empty-alternative rule in
    // `resolveMatch`, which is the same decision about the same shape.
    if(!count) return nullptr;

    auto hasElse = isElseCase(cases.get(parse, count - 1).cond);

    // The same sentence the two-armed form makes, and for the same reason: a construct whose last
    // test can fail answers nothing on that path, and a value position has nowhere to put nothing.
    if(wantValue && !hasElse) {
        context.diagnostics.error("an `if` used as a value needs an `else` case"_v, expr.source);
        return nullptr;
    }

    // One alternative per case, and one more where the last case can fail - which is the path that
    // falls out of the whole construct having run none of it.
    return armsValue(builder, expr.source, hasElse ? count : count + 1, hint, wantValue,
        [&](U32 index, Block* onFail, DeclPtr<Type> armHint, Carry& carry) -> Node* {
            // Falling out of the last test: nothing ran, and there is nothing to answer. Only ever
            // reached in statement position, since the value form was refused above.
            if(index >= count) return nullptr;

            auto written = cases.get(parse, index);
            auto scope = U32(bindings.size());

            /*
             * The tests, except in the `else` arm - which is the alternative `armsValue` asks for
             * no test from, exactly as a `match`'s exhaustive last pattern is.
             *
             * The chain is declared here so that it outlives `resolveTests`, which resolves a
             * conjunction's later operands inside the blocks its earlier ones proved.
             */
            if(!hasElse || index + 1 < count) {
                Chain chain;
                auto condition = resolveCondition(builder, written.cond, chain);
                if(!condition) return nullptr;

                SmallArray<PatternBinding, 4> made;
                if(!resolveTests(builder, condition, onFail, carry, made)) {
                    bindings.resize(scope);
                    return nullptr;
                }
            }

            auto value = resolveArm(builder, written.then, armHint, wantValue, wanted);

            bindings.resize(scope);
            return value;
        });
}

/*
 * A block, as a sequence of statements whose last may answer.
 *
 * `wantValue` is the position's question. A `= expr` body's block answers with its last expression;
 * a `:` body's answers with a `return`, and asking a statement block for a value would be asking the
 * wrong question of it.
 */
Node* Resolver::resolveBlock(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue,
                             Request wanted) {
    auto scope = bindings.size();

    // A lens call written inside a statement, lifted in front of it - `hoistBlockLenses`. Before the
    // list is read, so that everything below sees the rewritten block and nothing has to know a
    // rewrite happened; the extent scan included, since an extent is a slice of this same list.
    hoistBlockLenses(expr);

    auto statements = expr.multi;

    // The walk itself is `resolveStatements`, because a block is not the only run of statements
    // there is: a flat lens call's extent is a *slice* of one (Analysis-Lens.md §11.2), and it has
    // to be walked by whatever knows that a statement inside it may open an extent of its own.
    auto result = resolveStatements(builder, statements, 0, U32(statements.size()), hint, wantValue,
                                    wanted);

    bindings.resize(U32(scope));
    return result;
}

} // namespace mir
