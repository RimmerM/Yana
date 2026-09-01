#include "resolve_internal.h"
#include "name.h"
#include "print.h"

namespace mir {

static const Operator kOperators[] = {
    { "||"_v,  1, OperatorForm::ShortOr,  BinaryOp::Or  },
    { "&&"_v,  kConjunctionPrecedence, OperatorForm::ShortAnd, BinaryOp::And },
    { "=="_v,  3, OperatorForm::Binary,   BinaryOp::Eq  },
    { "!="_v,  3, OperatorForm::Binary,   BinaryOp::Ne  },
    { ">"_v,   3, OperatorForm::Binary,   BinaryOp::Gt  },
    { ">="_v,  3, OperatorForm::Binary,   BinaryOp::Ge  },
    { "<"_v,   3, OperatorForm::Binary,   BinaryOp::Lt  },
    { "<="_v,  3, OperatorForm::Binary,   BinaryOp::Le  },
    { "xor"_v, 5, OperatorForm::Binary,   BinaryOp::Xor },
    { "or"_v,  5, OperatorForm::Binary,   BinaryOp::Or  },
    { "and"_v, 6, OperatorForm::Binary,   BinaryOp::And },
    { "shl"_v, 7, OperatorForm::Binary,   BinaryOp::Shl },
    { "shr"_v, 7, OperatorForm::Binary,   BinaryOp::Shr },
    { "sar"_v, 7, OperatorForm::Binary,   BinaryOp::Sar },
    { "+"_v,   8, OperatorForm::Binary,   BinaryOp::Add },
    { "-"_v,   8, OperatorForm::Binary,   BinaryOp::Sub },
    { "*"_v,   9, OperatorForm::Binary,   BinaryOp::Mul },
    { "/"_v,   9, OperatorForm::Binary,   BinaryOp::Div },
    { "%"_v,   9, OperatorForm::Binary,   BinaryOp::Rem },
};

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
 */
Node* Resolver::holdBorrowed(Builder& builder, Node& value, Request wanted, DeclPtr<Type> expected,
                             LocationId source) {
    if(wanted.access == Access::Owned) return &value;
    if(!valueOccurrence(value).contract.isOwned()) return &value;
    if(!hasTeardown(decl(), expected)) return &value;

    auto place = hold(builder, value, source);
    return readPlace(builder, source, *place, wanted);
}

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

    return builder.addCallValue(source, *thunk, nullptr, 0);
}

Place* Resolver::resolvePlace(Builder& builder, ast::Expr& expr, bool complain) {
    switch(expr.kind) {
        case ast::Expr::Nested:
            return resolvePlace(builder, *parse[expr.nested], complain);

        case ast::Expr::Var: {
            auto bound = false;

            if(auto local = findLocal(expr.var, &bound)) {
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

            /*
             * A name a **lambda** says that its enclosing function bound - a capture.
             *
             * Between the locals and the globals, which is where the enclosing scope would be if
             * `findLocal` had not stopped at the lambda's floor: an enclosing local shadows a global
             * of the same name exactly as a local does, and putting the lookup anywhere else would
             * make that depend on whether the name was captured.
             */
            if(auto captured = findCapture(builder, expr.var)) return captured;

            auto found = findGlobal(context, program, here(), expr.var, expr.source);
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

            // Its initializer was rejected, so it has no type. Every path that produces this has
            // already reported; what this stops is the second failure, which is a read with nothing
            // to read.
            if(!decl()[found]->occurrence.type) return nullptr;

            return builder.globalPlace(found);
        }

        case ast::Expr::Field: {
            auto& written = *parse[expr.field];

            /*
             * The target of a projection, as a place where it is one and as storage where it is not.
             *
             * `f().x` names a field of a value nobody has storage for, and a field *is* a projection
             * of storage - so the value is written into a local first and the projection is of that.
             * The temporary is then an ordinary owner with an ordinary drop, which is what makes
             * `f().x` and `let t = f(); t.x` the same program rather than two.
             */
            ErrorMark mark(context);
            auto target = resolvePlace(builder, written.target, false);
            if(!target && !mark.reported()) {
                auto value = resolveValue(builder, written.target, nullptr, bindingRequest(Access::Shared));
                if(!value) return nullptr;

                target = hold(builder, *value, written.target.source);
            }

            if(!target) return nullptr;

            if(written.field.kind != ast::Expr::Var) {
                unsupported(written.field.source, "a field selected by anything but a name"_v);
                return nullptr;
            }

            /*
             * A field of a value whose type is a **variable** - `fn (a.size: I32) f(x: a)`.
             *
             * The signature is what says the field is there, so the requirement is looked up before
             * the type is reported as having no fields: an unconstrained `a` genuinely has none, and
             * the difference between the two is exactly what the constraint states.
             */
            if(decl()[target->occurrence.type]->kind == Type::Variable) {
                auto found = findProperty(target->occurrence.type, written.field.var);
                if(found == kNoProperty) {
                    context.diagnostics.error(
                        "%@ has no field called %@, and the signature does not require one"_v,
                        written.field.source, typeText(context, decl(), target->occurrence.type),
                        context.findName(written.field.var));
                    return nullptr;
                }

                auto property = decl()[currentFunction->gen]->properties.get(decl(), found);
                return builder.propertyPlace(*target, U16(found), owned(property.result));
            }

            if(decl()[target->occurrence.type]->kind != Type::Record) {
                context.diagnostics.error("%@ has no fields"_v, written.target.source,
                                          typeText(context, decl(), target->occurrence.type));
                return nullptr;
            }

            auto record = (RecordType*)decl()[target->occurrence.type];

            /*
             * A field of a **sum** is not reachable by name - Analysis-IR.md's G7d.
             *
             * Which fields a value has is what its discriminant says, and a selector says nothing
             * about it: `o.value` on an `Outcome` that turned out to be an `Exit` would read a type
             * out of bytes holding another one. What proves the arm is a pattern, so a pattern is
             * the only thing that projects one - which is also what lets the loan checker treat two
             * arms' fields as one storage and the drop pass leave them to the tag.
             */
            if(record->layout != RecordLayout::Single) {
                context.diagnostics.error(
                    "%@ has several constructors, so %@ is reached by matching rather than by name"_v,
                    written.field.source, typeText(context, decl(), target->occurrence.type),
                    context.findName(written.field.var));
                return nullptr;
            }

            for(auto field: record->fields.contents(decl())) {
                if(field.name != written.field.var) continue;
                if(!reachesInside(*record, written.field.source, "its fields"_v)) return nullptr;

                return builder.fieldPlace(*target, field.index);
            }

            context.diagnostics.error("%@ has no field called %@"_v, written.field.source,
                                      typeText(context, decl(), target->occurrence.type),
                                      context.findName(written.field.var));
            return nullptr;
        }

        default:
            return nullptr;
    }
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

        if(!checkType(*value, expected, argument.value.source, "an argument"_v)) return nullptr;

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

Node* Resolver::resolveField(Builder& builder, ast::Expr& expr, Request wanted) {
    auto place = resolvePlace(builder, expr, true);
    if(!place) return nullptr;

    return readPlace(builder, expr.source, *place, wanted);
}

Node* Resolver::readPlace(Builder& builder, LocationId source, Place& place, Request wanted) {
    auto mode = readMode(decl(), place, wanted);

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
     */
    if(mode == UseMode::BorrowUnique) {
        if(place.occurrence.contract.access() == Access::Shared) {
            context.diagnostics.error(
                "%@ is borrowed as shared here, so it cannot be handed out for writing"_v,
                source, placeText(context, decl(), builder.base(), builder.body, place));
            return nullptr;
        }

        if(!place.writable) {
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
         * captured, and the spec pairs that with outliving the frame - which is the heap placement
         * nothing here selects.
         */
        if(isCapture(builder, place)) {
            context.diagnostics.error(
                "a lambda cannot hand over what it captured - a closure that owns its captures is one that outlives its frame, which needs a heap-placed environment the ownership resolver does not implement yet"_v,
                source);
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

    return builder.addRead(source, place, mode);
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
Node* Resolver::resolveConstruction(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint) {
    auto& written = *parse[expr.con];

    if(written.type.kind != ast::Type::Con) {
        unsupported(expr.source, "this construction"_v);
        return nullptr;
    }

    auto found = findConstructor(context, program, here(), written.type.name, expr.source);
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

    if(typeArguments.any()) {
        if(hint && decl()[hint]->kind == Type::Record &&
           ((RecordType*)decl()[hint])->instanceOf == record) {
            typeArguments.seed(*(RecordType*)decl()[hint]);
        } else if(hint) {
            context.diagnostics.error("this position asks for %@, and this builds a %@"_v,
                                      expr.source, typeText(context, decl(), hint),
                                      context.findName(declared->name));
            return nullptr;
        }
    } else if(hint && !sameType(hint, asBase<Type>(record))) {
        context.diagnostics.error("this position asks for %@, and this builds %@"_v, expr.source,
                                  typeText(context, decl(), hint),
                                  typeText(context, decl(), asBase<Type>(record)));
        return nullptr;
    }

    auto type = asBase<Type>(record);

    SmallArray<Node*, 8> values;

    /*
     * Which fields the source *named*, beside the values it produced for them.
     *
     * Two arrays because they answer different questions and a field can be one without the other: a
     * field whose value failed to resolve has a name written and no value, and reporting it below as
     * one the constructor left out would be a second message about a mistake that has already been
     * reported one line up.
     */
    SmallArray<bool, 8> named;

    for(U16 i = 0; i < arm.count; i++) {
        values.push(nullptr);
        named.push(false);
    }

    // The field values already in hand, for the reason a call's arguments are - and by index, so a
    // position still being resolved is the null the carry skips.
    HeldList<SmallArray<Node*, 8>> held(*this, values);

    /*
     * `Proceed(x)` - the one-payload spelling, which writes no field name because there is one field.
     *
     * Read as a name here rather than as a second walk: the field carries the constructor's own name
     * (see `declareFields`), so a positional argument in a one-payload constructor *is* an argument
     * for that name and everything below is the named walk unchanged. Any other positional list is
     * refused, because a constructor of several unnamed fields is a tuple type and this stage has
     * none.
     */
    auto payload = arm.count == 1 &&
                   declared->field(decl(), arm.first).name == arm.name;

    auto ok = true;
    auto position = U32(0);
    for(auto argument: written.args.contents(parse)) {
        auto wrote = argument.name;

        if(!wrote && payload && position == 0) {
            wrote = arm.name;
        } else if(!wrote) {
            unsupported(argument.value.source, "a positional field in a construction"_v);
            ok = false;
            position++;
            continue;
        }

        position++;

        auto index = ~U32(0);
        for(U16 i = 0; i < arm.count; i++) {
            if(declared->field(decl(), U16(arm.first + i)).name == wrote) index = i;
        }

        if(index == ~U32(0)) {
            context.diagnostics.error("%@ has no field called %@"_v, argument.value.source,
                                      context.findName(arm.name), context.findName(wrote));
            ok = false;
            continue;
        }

        if(named[index]) {
            context.diagnostics.error("%@ is given twice"_v, argument.value.source,
                                      context.findName(wrote));
            ok = false;
            continue;
        }

        named[index] = true;

        auto field = declared->field(decl(), U16(arm.first + index));
        auto declaredType = field.occurrence.type;

        auto value = resolveValue(builder, argument.value, typeArguments.hint(declaredType),
                                  requestOf(field.occurrence.contract));
        if(!value) { ok = false; continue; }

        typeArguments.bind(declaredType, valueType(*value));

        auto expected = typeArguments.expected(declaredType);
        if(!checkType(*value, expected, argument.value.source, "a field"_v)) { ok = false; continue; }

        // A closure into a field the record owns - `checkCarriedOrigins`, which is `own.cpp`'s
        // declaration-time refusal applied to a value whose interior shape the declaration cannot
        // see.
        if(!checkCarriedOrigins(*value, field.occurrence, argument.value.source,
                                stringView(context.findName(declared->name)))) {
            ok = false;
            continue;
        }

        values[index] = value;
    }

    for(U16 i = 0; i < arm.count; i++) {
        if(named[i]) continue;

        context.diagnostics.error("%@ leaves out the field %@"_v, expr.source,
                                  context.findName(arm.name),
                                  context.findName(declared->field(decl(), U16(arm.first + i)).name));
        ok = false;
    }

    if(!ok) return nullptr;

    if(typeArguments.any()) {
        // A record with a parameter no field mentions - `data Phantom(a) {n: I32}` - is what reports
        // here.
        // What the record's own head constraints decide, where a field left something open.
    typeArguments.fillDetermined(expr.source);

    if(auto open = typeArguments.undecided()) {
            context.diagnostics.error("no field decides what %@ is in this %@"_v, expr.source,
                                      context.findName(((VariableType*)decl()[open])->name),
                                      context.findName(arm.name));
            return nullptr;
        }

        type = instantiateRecord(context, program, record, typeArguments.arguments.pointer(),
                                 typeArguments.arguments.size(), expr.source);
        if(!type) return nullptr;
    }

    return builder.addAggregate(expr.source, type, found.index, values.pointer(), values.size());
}

Node* Resolver::resolveValue(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, Request wanted) {
    switch(expr.kind) {
        case ast::Expr::Nested:
            return resolveValue(builder, *parse[expr.nested], hint, wanted);

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
            auto local = findLocal(expr.var);
            if(local && deferredParameter(*local)) return forceDeferred(builder, *local, expr.source);

            if(!local && currentFunction) {
                auto found = findGenVariable(decl(), currentFunction->gen, expr.var);
                if(found && ((VariableType*)decl()[found])->isConst()) {
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
             * *which* declaration is the position's own type - "one plain function per name, any
             * arity" makes the arity part of what a use selects, and a value position has only the
             * hint to select with. A name used as a value with no expected function type therefore
             * reports rather than guessing an arity, which is the same refusal a call with the wrong
             * number of arguments gets and for the same reason.
             */
            if(!local && !findCapture(builder, expr.var) &&
               !findGlobal(context, program, here(), expr.var, expr.source) &&
               hasFunctionName(context, program, here(), expr.var)) {
                return resolveFunValue(builder, expr.var, hint, expr.source);
            }

            return resolveField(builder, expr, wanted);
        }

        case ast::Expr::Field:
            return resolveField(builder, expr, wanted);

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

        case ast::Expr::Con: {
            if(!isBooleanConstructor(expr)) return resolveConstruction(builder, expr, hint);

            auto constant = resolveBoolean(expr, hint);
            return constant.isSet() ? builder.addConst(expr.source, constant) : nullptr;
        }

        case ast::Expr::Coerce: {
            /*
             * `e :: T` - an ascription, which is a hint and then a check.
             *
             * Not a conversion: `::` says what a value is and `convert` is what changes one. Passing
             * the type down first is what makes `1 :: I64` one instruction rather than a widened
             * `I32`, and checking afterwards is what makes `x :: I64` on an `I32` local a diagnostic
             * rather than a silent extension.
             */
            auto& coerce = *parse[expr.coerce];
            auto type = resolveType(coerce.type);
            if(!type) return nullptr;

            auto value = resolveValue(builder, coerce.target, type, wanted);
            if(!value) return nullptr;

            /*
             * `::` widens as well as ascribes, and only the ascribing half is here.
             *
             * `x :: I64` on an `I32` is a legal widening in the language, so a plain "wrong type"
             * would be describing the program wrongly rather than describing this stage.
             * Conversions are a class - `Convert` - and arrive with instance selection; what this
             * milestone can do honestly is name the gap.
             */
            if(!sameType(valueType(*value), type)) {
                auto text = Tritium::format("converting %@ to %@ with `::`",
                                            typeText(context, decl(), valueType(*value)),
                                            typeText(context, decl(), type));
                unsupported(expr.source, stringView(text));
                return nullptr;
            }

            return value;
        }

        /*
         * `{}` - the one tuple this stage has, and it is the unit value.
         *
         * Here because a lens that hands over nothing writes `yield {}`, which is what five of the
         * seven lenses in `lib/` are. A tuple with fields is a different feature - it needs an
         * anonymous record type and a layout - and is refused as one rather than folded in here.
         */
        case ast::Expr::Tup: {
            if(expr.tup.size()) {
                unsupported(expr.source, "a tuple literal"_v);
                return nullptr;
            }

            return builder.addConst(expr.source, unitConstant(program.types.unit));
        }

        case ast::Expr::Infix:
            return resolveInfix(builder, expr, hint);
        case ast::Expr::Prefix:
            return resolvePrefix(builder, expr, hint);
        case ast::Expr::App:
            return resolveCall(builder, expr, hint);
        case ast::Expr::If:
            return resolveIf(builder, expr, hint, true, wanted);

        // A pattern in a value position - the test alone, since a name bound here would have no arm
        // to be in scope in. See `resolveIs`.
        case ast::Expr::Is:
            return resolveIs(builder, expr);

        case ast::Expr::Match:
            return resolveMatch(builder, expr, hint, true, wanted);
        case ast::Expr::Multi:
            return resolveBlock(builder, expr, hint, true, wanted);

        // A lambda - `resolve_lambda.cpp`, where the whole of it is.
        case ast::Expr::Fun:
            return resolveLambda(builder, expr, hint);

        default:
            if(expr.kind >= ast::Expr::Lit) {
                auto constant = resolveLiteral(expr, hint, false);
                return constant.isSet() ? builder.addConst(expr.source, constant) : nullptr;
            }

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
    if(!type || decl()[type]->kind != Type::Fun) {
        context.diagnostics.error("this is %@, which is not something that can be called"_v,
                                  written.callee.source, typeText(context, decl(), type));
        return nullptr;
    }

    auto& shape = *(FunType*)decl()[type];
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
        auto value = resolveValue(builder, argument.value, declared.type, requestOf(declared.contract));
        if(!value) return nullptr;
        if(!checkType(*value, declared.type, argument.value.source, "an argument"_v)) return nullptr;

        if(!checkCarriedOrigins(*value, declared, argument.value.source, "this parameter"_v)) {
            return nullptr;
        }

        arguments.push(value);
    }

    return builder.addCallValue(source, target, arguments.pointer(), arguments.size());
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
    auto found = findFunction(context, program, here(), name, U32(wanted.params.size()), source);

    if(!found) {
        context.diagnostics.error("no %@ takes %@ arguments"_v, source, context.findName(name),
                                  U64(wanted.params.size()));
        return nullptr;
    }

    auto& callee = *decl()[found];
    if(!callee.summary.bodyKnown) {
        unsupported(source, "a value of a function with no body"_v);
        return nullptr;
    }

    auto type = funTypeOf(context, program, found);
    if(!type) {
        // A generic function and a lens are the two `funTypeOf` declines, and they decline for one
        // reason: neither names a single body a value could point at.
        unsupported(source, callee.lens ? "a value of a lens"_v : "a value of a generic function"_v);
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

    // No environment: a named function captures nothing, so the pair's second word is null and the
    // value refers to no storage at all.
    return builder.addFunValue(source, found, type, nullptr);
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
     * Note that a projection wins over a **dot-call** here, which is the language's own rule and not
     * an artifact: `h.run(v)` where `run` is a field is a call through that field. This stage does
     * not implement dot-call at all, so nothing is being chosen between yet.
     */
    // `findCapture` appends the field on the first name it answers for, which is what makes it the
    // test as well as the lookup: where it says yes the walk below reads the name through the very
    // capture it just made, and where it says no it made nothing.
    auto storage = written.callee.kind == ast::Expr::Var &&
                   (findLocal(written.callee.var) || findCapture(builder, written.callee.var));

    if(written.callee.kind != ast::Expr::Var || storage) {
        auto target = resolveValue(builder, written.callee, nullptr, bindingRequest(Access::Shared));
        if(!target) return nullptr;

        return resolveValueCall(builder, written, *target, expr.source);
    }

    auto name = written.callee.var;

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

    auto found = findFunction(context, program, here(), name, arity, written.callee.source);
    if(!found) found = lookupCoreMember(name, arity);

    if(!found) {
        // Two messages, because they are two mistakes: a name nobody declared, and the right name
        // called with the wrong number of arguments. The second is what arity overloading makes
        // possible to get wrong, so it is the one that has to say what it saw.
        if(hasFunctionName(context, program, here(), name)) {
            context.diagnostics.error("no %@ takes %@ arguments"_v, written.callee.source,
                                      context.findName(name), U64(arity));
        } else {
            context.diagnostics.error("unknown function %@"_v, written.callee.source,
                                      context.findName(name));
        }

        return nullptr;
    }

    auto& callee = *decl()[found];

    /*
     * A declaration with no body is a call this stage cannot emit. It is separate from the summary's
     * own answer for one - `bodyKnown` false publishes "may suspend" and is the right fact - because
     * what stops here is code generation rather than analysis.
     *
     * A **class member** is not one of those, and that is the whole of what a class call is: the
     * declaration is a signature and what the call reaches is the instance's body, which `callTarget`
     * finds. Its summary is the selected member's for the same reason - every pass that asks reads
     * the target rather than the signature.
     */
    if(!callee.summary.bodyKnown && !callee.klass &&
       callee.intrinsic == intrinsic::Op::Count) {
        unsupported(expr.source, "calling a function with no body"_v);
        return nullptr;
    }

    /*
     * A lens in a *value* position, which is the one place neither call form reaches.
     *
     * What a lens answers is what its continuation answered, and an expression has no continuation
     * to give it: the two forms that supply one are both statements (`resolveStatements` and
     * `resolveScoped`), so a lens written anywhere else has nothing to be. Naming both spellings is
     * the whole of this diagnostic - the scoped one, and the flat one whose extent is the rest of
     * the statement's own block.
     */
    if(callee.lens) {
        context.diagnostics.error(
            "%@ is a lens, so it needs a continuation - write `%@(...):` with a block under it, or "
            "`let x = %@(...)` where what it hands over is used"_v,
            expr.source, context.findName(callee.name), context.findName(callee.name),
            context.findName(callee.name));
        return nullptr;
    }

    if(!checkCallShape(written, callee, expr.source)) return nullptr;

    /*
     * The callee's type arguments, decided by the argument walk rather than before it, because that
     * is where the information is: `id(7)` says `a` is `I32` by what it passes, and nothing at the
     * call site names a type.
     */
    TypeArguments typeArguments(*this, callee.gen);

    SmallArray<Node*, 8> arguments;
    if(!resolveArguments(builder, written, callee, arguments, typeArguments)) return nullptr;

    /*
     * A variable no parameter decided.
     *
     * `fn empty() -> Maybe(a)` is decided by the *expected* type at the call site rather than by an
     * argument, which is the same rule that picks a class instance by its result type. That needs
     * the position's hint to reach a call, which this stage does not thread yet, so it is refused by
     * name: a specialization chosen for an unbound variable is a specialization chosen at random.
     */
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
     */
    if(hint) typeArguments.bind(callee.result.type, hint);

    // What the callee's own constraints decide, before anything is called undecided - a functional
    // dependency's whole purpose is that these positions have an answer no argument supplied.
    typeArguments.fillDetermined(expr.source);

    if(auto open = typeArguments.undecided()) {
        context.diagnostics.error("no argument decides what %@ is in this call to %@"_v, expr.source,
                                  context.findName(((VariableType*)decl()[open])->name),
                                  context.findName(callee.name));
        return nullptr;
    }

    if(!dischargeRequirements(callee, typeArguments, expr.source)) return nullptr;

    /*
     * An **intrinsic**, which is this same call with a different last line.
     *
     * Everything above ran unchanged: the callee was found by name and arity, the arguments were
     * resolved against the declared parameters and prepared under the modes their contracts asked
     * for, the type arguments were bound and the constraints discharged. What the role decides is
     * only which node comes out - see `intrinsic/expand.h`, where the argument for keeping the two
     * paths joined this far is made.
     */
    if(callee.intrinsic != intrinsic::Op::Count) {
        return expandIntrinsic(builder, context, callee.intrinsic, callee, arguments.pointer(),
                               arguments.size(), typeArguments.with(), expr.source);
    }

    return builder.addCall(expr.source, callee, arguments.pointer(), arguments.size(),
                           typeArguments.with());
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
    for(auto frame = resolver.held; frame; frame = frame->outer) frames.push(frame);

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

DeclPtr<Type> TypeArguments::hint(DeclPtr<Type> declared) {
    auto type = expected(declared);
    if(isConcreteType(resolver.decl(), type)) return type;

    // The one position an open hint is still a hint - `isOpenFunPosition`, where the whole of the
    // rule is. What comes back binds the variable the position left open, through the same `bind`
    // every other argument goes through.
    return isOpenFunPosition(resolver.decl(), type) ? type : nullptr;
}

void TypeArguments::bind(DeclPtr<Type> declared, DeclPtr<Type> supplied) {
    if(!any()) return;
    matchType(resolver.decl(), owner, arguments, declared, supplied);
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
 * One requirement's determined positions, read off whatever proves its deciding ones.
 *
 * Answers whether anything moved, which is what the fixpoint above it needs. The write-back is
 * `matchType` rather than an assignment, because a determined position is a *type* rather than a
 * variable: `Contiguous(c, Array(e))` deciding the second position binds `e` inside it.
 */
static bool fillFromRequirement(Resolver& resolver, GenRequirement requirement,
                                DeclPtr<GenContext> owner, SmallArray<DeclPtr<Type>, 4>& into,
                                LocationId source) {
    auto base = resolver.decl();
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

    if(enclosing) {
        for(auto held: base[enclosing]->classes.contents(base)) {
            if(held.klass != requirement.klass) continue;

            auto same = true;
            for(U32 i = 0; i < deciding && same; i++) {
                same = sameType(held.args.get(base, i), applied[i]);
            }

            if(!same) continue;

            for(U32 i = 0; i < arity; i++) answer.push(held.args.get(base, i));
            break;
        }
    }

    if(!answer.size()) {
        auto concrete = true;
        for(U32 i = 0; i < deciding; i++) concrete = concrete && isConcreteType(base, applied[i]);
        if(!concrete) return false;

        InstanceMatch found;
        for(U32 i = deciding; i < arity; i++) applied[i] = nullptr;

        if(!selectInstance(resolver.context, resolver.program, requirement.klass, applied.pointer(),
                           applied.size(), source, false, found)) {
            return false;
        }

        for(auto argument: found.arguments) answer.push(argument);
    }

    auto moved = false;
    for(U32 i = deciding; i < arity; i++) {
        SmallArray<DeclPtr<Type>, 4> before;
        for(auto argument: into) before.push(argument);

        matchType(base, owner, into, requirement.args.get(base, i), answer[i]);

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

    matchType(base, owner, into, property.result, answer);

    for(Size i = 0; i < into.size(); i++) {
        if(into[i] != before[i]) return true;
    }

    return false;
}

void TypeArguments::fillDetermined(LocationId source) {
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
            moved = fillFromRequirement(resolver, requirement, owner, arguments, source) || moved;
        }

        // Beside the class requirements and in the same fixpoint, because the two decide each
        // other: `a.mark: b` answers `b` and a `Peek(b)` beside it may then answer something else,
        // and which order the author wrote them in is not a question a caller should have to have.
        for(auto property: base[owner]->properties.contents(base)) {
            moved = fillFromProperty(resolver, property, owner, arguments) || moved;
        }

        if(!moved || !undecided()) return;
    }
}

DeclPtr<Type> TypeArguments::undecided() const {
    for(Size i = 0; i < arguments.size(); i++) {
        if(!arguments[i]) return resolver.decl()[owner]->variables.get(resolver.decl(), i);
    }

    return nullptr;
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
                                    LocationId source) {
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
                              arguments.size(), source, true, found);
    }

    auto enclosing = currentFunction ? currentFunction->gen : nullptr;
    if(satisfiesRequirement(context, program, enclosing, requirement.klass, arguments.pointer(),
                            arguments.size())) {
        return true;
    }

    context.diagnostics.error("this call needs %@, and the signature does not require it"_v, source,
                              appliedClassText(context, decl(), requirement.klass,
                                               arguments.pointer(), arguments.size()));
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

bool Resolver::resolveArguments(Builder& builder, ast::AppExpr& written, Function& callee,
                                SmallArray<Node*, 8>& into, TypeArguments& typeArguments) {
    Size index = 0;

    // The arguments already in hand, while the next one is resolved: an `if` in argument three puts
    // a branch between arguments one and two and the call that uses them.
    HeldList<SmallArray<Node*, 8>> held(*this, into);

    for(auto argument: written.args.contents(parse)) {
        auto& parameter = *decl()[callee.args.get(decl(), U32(index++))];
        auto declared = parameter.occurrence.type;
        auto wanted = requestOf(parameter.occurrence.contract);

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
        auto value = parameter.lazy
            ? resolveDeferred(builder, argument.value, typeArguments.expected(declared))
            : resolveValue(builder, argument.value, typeArguments.hint(declared), wanted);

        if(!value) return false;

        typeArguments.bind(declared, valueType(*value));

        auto expected = typeArguments.expected(declared);

        if(!checkType(*value, expected, argument.value.source, "an argument"_v)) {
            return false;
        }

        // A closure into a position the callee *owns* - `checkCarriedOrigins`. After the type, as at
        // every other position that asks: what a value refers to is only worth a message once the
        // value is the kind the position wanted. A borrowing position is fine and is the whole point
        // of a callback - the loan lasts the call, which is what `Origin::CallBounded` says.
        if(!checkCarriedOrigins(*value, parameter.occurrence, argument.value.source,
                                "this parameter"_v)) {
            return false;
        }

        value = holdBorrowed(builder, *value, wanted, expected, argument.value.source);
        into.push(value);
    }

    return true;
}

/*
 * Whether the call's shape matches the signature at all, before any argument is resolved.
 *
 * Both questions are about the call as a whole and neither leaves a position to carry on from: a
 * named argument changes which parameter every later position fills, and a wrong count means every
 * position after the first missing one is checked against the wrong type. Resolving the arguments
 * first and reporting afterwards would produce one true diagnostic and a run of consequences of it.
 */
bool Resolver::checkCallShape(ast::AppExpr& written, Function& callee, LocationId source) {
    for(auto argument: written.args.contents(parse)) {
        if(argument.name) {
            unsupported(argument.value.source, "a named argument"_v);
            return false;
        }
    }

    if(written.args.size() != callee.writtenArity()) {
        context.diagnostics.error("%@ takes %@ arguments but was given %@"_v, source,
                                  context.findName(callee.name), U64(callee.writtenArity()),
                                  U64(written.args.size()));
        return false;
    }

    return true;
}

/*
 * `-x` and `!x`.
 *
 * A negated literal is folded into the constant rather than emitted as a negation - see
 * `resolveConstant`, which does the same for a global's initializer and says why it matters.
 */
Node* Resolver::resolvePrefix(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint) {
    auto& written = *parse[expr.prefix];

    if(written.op.kind != ast::Expr::Var) {
        context.diagnostics.error("a prefix operator must be named"_v, written.op.source);
        return nullptr;
    }

    auto text = stringView(context.findName(written.op.var));

    if(text == "-"_v && written.on.kind >= ast::Expr::Lit) {
        auto constant = resolveLiteral(written.on, hint, true);
        return constant.isSet() ? builder.addConst(expr.source, constant) : nullptr;
    }

    UnaryOp op;
    if(text == "-"_v) op = UnaryOp::Neg;
    else if(text == "!"_v) op = UnaryOp::Not;
    else {
        unsupported(written.op.source, "this prefix operator"_v);
        return nullptr;
    }

    // `!` asks for a `Bool` and `-` passes the position's hint through, since negation answers what
    // it was given.
    auto operandHint = op == UnaryOp::Not ? program.types.boolean : hint;
    auto value = resolveValue(builder, written.on, operandHint, inspectRequest());
    if(!value) return nullptr;

    if(!operandTypeAllowed(decl(), unaryCategory(op), valueType(*value))) {
        context.diagnostics.error("%@ cannot be applied to %@"_v, expr.source, text,
                                  typeText(context, decl(), valueType(*value)));
        return nullptr;
    }

    return builder.addUnary(expr.source, op, *value);
}

Maybe<Operator> Resolver::findOperator(StringId name) {
    auto text = stringView(context.findName(name));

    for(auto& entry: kOperators) {
        if(text != entry.text) continue;

        /*
         * A declared fixity wins over the table's, and only over its grouping half.
         *
         * `kOperators` stands in for `Core.yana`'s own `infixl` lines until this stage reads Core,
         * so a module that declares one is saying something the stand-in was guessing at. What it
         * cannot override is the *form* and the instruction: those are what an operator does, and
         * until an operator can dispatch to a class member there is nowhere else for them to come
         * from.
         */
        auto declared = mir::findFixity(context, program, here(), name);
        if(!declared) return Just(Operator(entry));

        auto found = entry;
        found.precedence = U8(declared.properties.precedence);
        found.assoc = declared.properties.associativity;
        return Just(found);
    }

    return Nothing();
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
bool Resolver::flattenChain(ast::Expr& expr, Chain& chain) {
    auto node = parse[expr.infix];

    while(true) {
        if(node->op.kind != ast::Expr::Var) {
            context.diagnostics.error("an infix operator must be a named operator"_v, node->op.source);
            return false;
        }

        auto found = findOperator(node->op.var);
        if(!found) {
            unsupported(node->op.source, "this operator"_v);
            return false;
        }

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

Node* Resolver::resolveInfix(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint) {
    Chain chain;
    if(!flattenChain(expr, chain)) return nullptr;

    Size operandIndex = 0;
    Size operatorIndex = 0;
    return climb(builder, chain, operandIndex, operatorIndex, 0, hint);
}

/*
 * Precedence climbing over a flattened chain.
 *
 * `hint` reaches the left operand and, for the operations that answer their operand type, the right
 * one takes the left one's answer instead. That is where this stage's inference stops: `x + 1` types
 * the literal from `x`, and `1 + x` types `x` from a literal that defaulted, which is a real limit
 * rather than an oversight - local inference is what the language specifies and what is here.
 */
Node* Resolver::climb(Builder& builder, Chain& chain, Size& operandIndex, Size& operatorIndex,
                      U8 minimumPrecedence, DeclPtr<Type> hint) {
    auto lhs = resolveValue(builder, *chain.operands[operandIndex++], hint, inspectRequest());

    while(operatorIndex < chain.operators.size() &&
          chain.operators[operatorIndex].precedence >= minimumPrecedence) {
        auto op = chain.operators[operatorIndex];
        auto source = chain.sources[operatorIndex];
        operatorIndex++;

        // One rung above this operator's own where it is left associative, which is what makes it
        // stop before an equal one and take that as its own left operand instead; its own rung where
        // it is right associative, which lets the equal one climb and become the right operand.
        auto next = U8(op.assoc == Assoc::Right ? op.precedence : op.precedence + 1);

        if(op.form != OperatorForm::Binary) {
            lhs = shortCircuit(builder, chain, operandIndex, operatorIndex, next, source, lhs, op);
            if(!lhs) return nullptr;
            continue;
        }

        if(!lhs) return nullptr;

        /*
         * The left operand, held while the right one is resolved - `Held`.
         *
         * This is the position the whole mechanism was found by: `one + (if c then 2 else 0)` puts
         * a branch between a value and its use, and nothing about the addition says so. The frame
         * ends before `applyBinary`, which reads `lhs` as it stands after whatever the right operand
         * built.
         */
        Node* rhs = nullptr;
        {
            HeldValue held(*this, lhs);
            rhs = climb(builder, chain, operandIndex, operatorIndex, next, valueType(*lhs));
        }

        if(!rhs) return nullptr;

        lhs = applyBinary(builder, source, op.op, *lhs, *rhs);
        if(!lhs) return nullptr;
    }

    return lhs;
}

Node* Resolver::applyBinary(Builder& builder, LocationId source, BinaryOp op, Node& lhs, Node& rhs) {
    auto type = valueType(lhs);

    if(!sameType(type, valueType(rhs))) {
        context.diagnostics.error("%@ compares %@ with %@, which are different types"_v, source,
                                  binaryOpName(op), typeText(context, decl(), type),
                                  typeText(context, decl(), valueType(rhs)));
        return nullptr;
    }

    if(!operandTypeAllowed(decl(), binaryCategory(op), type)) {
        context.diagnostics.error("%@ cannot be applied to %@"_v, source, binaryOpName(op),
                                  typeText(context, decl(), type));
        return nullptr;
    }

    return builder.addBinary(source, op, lhs, rhs);
}

/*
 * `&&` and `||`, which are control flow rather than operations.
 *
 * `a && b` is `if a then b else False` and `a || b` is `if a then True else b`, which is what Core's
 * own definitions say they are - the difference being that Core writes them as functions over
 * `Truth` with a `@lazy` second argument, and here the laziness is the branch itself. Building them
 * out of the same merge every `if` uses is the point: the right operand is a span of the chain
 * rather than an AST node anyone can point at, and climbing it *inside* the block it belongs to is
 * what makes it run only when it should.
 */
Node* Resolver::shortCircuit(Builder& builder, Chain& chain, Size& operandIndex, Size& operatorIndex,
                             U8 next, LocationId source, Node* condition, const Operator& op) {
    if(!condition) return nullptr;
    if(!checkType(*condition, program.types.boolean, source, "a short-circuit operand"_v)) return nullptr;

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
    return armsValue(builder, source, 2, program.types.boolean, true,
        [&](U32 index, Block* onFail, DeclPtr<Type>, Carry& carry) -> Node* {
            if(onFail) failIfNot(builder, *condition, *onFail, carry, source);

            auto climbs = (op.form == OperatorForm::ShortAnd) == (index == 0);
            if(climbs) return climbBool(builder, chain, operandIndex, operatorIndex, next, source);

            return constantBool(builder, source, shortValue);
        });
}

Node* Resolver::climbBool(Builder& builder, Chain& chain, Size& operandIndex, Size& operatorIndex,
                          U8 next, LocationId source) {
    auto value = climb(builder, chain, operandIndex, operatorIndex, next, program.types.boolean);
    if(!value) return nullptr;
    if(!checkType(*value, program.types.boolean, source, "a short-circuit operand"_v)) return nullptr;

    return value;
}

Node* Resolver::constantBool(Builder& builder, LocationId source, bool value) {
    return builder.addConst(source, boolConstant(program.types.boolean, value));
}

// See the declaration, where the difference between `merged` and a null `value` is argued.
void Resolver::closeArm(Builder& builder, Block& end, Block& join, Node* value, bool merged,
                        LocationId source, Carry& carry, const Node* const* carried) {
    builder.setBlock(end);
    if(builder.isTerminated()) return;

    if(merged && !value) {
        // The alternative produced no value and did not leave, which only happens after a
        // diagnostic. `unreachable` rather than a jump, because a merge it entered would need a
        // parameter it has nothing to supply.
        builder.addUnreachable(source);
        return;
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
}

// See the declaration. Everything here is a statement about *one* occurrence being the merge's, and
// the alternatives that do not have it are reported and dropped.
Node* Resolver::mergeArms(Builder& builder, Block& join, SmallArray<Node*, 8>& values,
                          LocationId source) {
    Node* first = nullptr;

    for(Size i = 0; i < values.size(); i++) {
        if(!values[i]) continue;
        if(!first) { first = values[i]; continue; }

        auto& answered = valueOccurrence(*values[i]);
        auto& expected = valueOccurrence(*first);

        if(!sameType(answered.type, expected.type)) {
            context.diagnostics.error("the arms answer %@ and %@, which are different types"_v,
                                      source, typeText(context, decl(), expected.type),
                                      typeText(context, decl(), answered.type));
            values[i] = nullptr;
            continue;
        }

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

            auto value = wantValue ? resolveValue(builder, written.then, armHint, wanted) : nullptr;
            if(!wantValue) resolveStatement(builder, written.then);

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
