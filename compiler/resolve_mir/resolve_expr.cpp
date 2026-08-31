#include "resolve_internal.h"
#include "print.h"

namespace mir {

static const Operator kOperators[] = {
    { "||"_v,  1, OperatorForm::ShortOr,  BinaryOp::Or  },
    { "&&"_v,  2, OperatorForm::ShortAnd, BinaryOp::And },
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

Place* Resolver::resolvePlace(Builder& builder, ast::Expr& expr, bool complain) {
    switch(expr.kind) {
        case ast::Expr::Nested:
            return resolvePlace(builder, *parse[expr.nested], complain);

        case ast::Expr::Var: {
            if(auto local = findLocal(expr.var)) return builder.localPlace(*local);

            auto found = program.findGlobal(here(), expr.var);
            if(!found) {
                // The one place an unknown name is reported, and the reason `complain` exists: the
                // target of a projection is asked whether it is a place *before* it is resolved as a
                // value, and a name that is neither has to be reported once rather than twice.
                if(complain) {
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
            auto target = resolvePlace(builder, written.target, false);
            if(!target && !context.diagnostics.errorCount()) {
                auto value = resolveValue(builder, written.target, nullptr, bindingRequest(Access::Shared));
                if(!value) return nullptr;

                target = hold(builder, *value, written.target.source);
            }

            if(!target) return nullptr;

            if(decl()[target->occurrence.type]->kind != Type::Record) {
                context.diagnostics.error("%@ has no fields"_v, written.target.source,
                                          typeText(context, decl(), target->occurrence.type));
                return nullptr;
            }

            if(written.field.kind != ast::Expr::Var) {
                unsupported(written.field.source, "a field selected by anything but a name"_v);
                return nullptr;
            }

            auto record = (RecordType*)decl()[target->occurrence.type];
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

    auto record = program.findRecord(here(), written.type.name);
    if(!record) {
        context.diagnostics.error("unknown constructor %@"_v, expr.source,
                                  context.findName(written.type.name));
        return nullptr;
    }

    auto declared = decl()[record];

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

    for(Size i = 0; i < declared->fields.size(); i++) {
        values.push(nullptr);
        named.push(false);
    }

    // The field values already in hand, for the reason a call's arguments are - and by index, so a
    // position still being resolved is the null the carry skips.
    HeldList<SmallArray<Node*, 8>> held(*this, values);

    auto ok = true;
    for(auto argument: written.args.contents(parse)) {
        if(!argument.name) {
            unsupported(argument.value.source, "a positional field in a construction"_v);
            ok = false;
            continue;
        }

        auto index = ~U32(0);
        for(auto field: declared->fields.contents(decl())) {
            if(field.name == argument.name) index = field.index;
        }

        if(index == ~U32(0)) {
            context.diagnostics.error("%@ has no field called %@"_v, argument.value.source,
                                      typeText(context, decl(), type),
                                      context.findName(argument.name));
            ok = false;
            continue;
        }

        if(named[index]) {
            context.diagnostics.error("%@ is given twice"_v, argument.value.source,
                                      context.findName(argument.name));
            ok = false;
            continue;
        }

        named[index] = true;

        auto field = declared->field(decl(), U16(index));
        auto declaredType = field.occurrence.type;

        auto value = resolveValue(builder, argument.value, typeArguments.hint(declaredType),
                                  requestOf(field.occurrence.contract));
        if(!value) { ok = false; continue; }

        typeArguments.bind(declaredType, valueType(*value));

        auto expected = typeArguments.expected(declaredType);
        if(!checkType(*value, expected, argument.value.source, "a field"_v)) { ok = false; continue; }

        values[index] = value;
    }

    for(auto field: declared->fields.contents(decl())) {
        if(named[field.index]) continue;

        context.diagnostics.error("%@ leaves out the field %@"_v, expr.source,
                                  typeText(context, decl(), type), context.findName(field.name));
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
                                      context.findName(declared->name));
            return nullptr;
        }

        type = instantiateRecord(context, program, record, typeArguments.arguments.pointer(),
                                 typeArguments.arguments.size(), expr.source);
        if(!type) return nullptr;
    }

    return builder.addAggregate(expr.source, type, values.pointer(), values.size());
}

Node* Resolver::resolveValue(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, Request wanted) {
    switch(expr.kind) {
        case ast::Expr::Nested:
            return resolveValue(builder, *parse[expr.nested], hint, wanted);

        case ast::Expr::Var:
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
        case ast::Expr::Multi:
            return resolveBlock(builder, expr, hint, true, wanted);

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

    if(written.callee.kind != ast::Expr::Var) {
        unsupported(written.callee.source, "calling anything but a named function"_v);
        return nullptr;
    }

    auto name = written.callee.var;

    // A local of the same name is a *value*, and calling one is an indirect call through a function
    // value - a different node with a different summary rule (Analysis-Sharing.md §3.5) and not
    // something to quietly resolve to the declaration it shadows.
    if(findLocal(name)) {
        unsupported(written.callee.source, "calling a value"_v);
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
    auto found = program.findFunction(here(), name, arity);
    if(!found) found = lookupCoreMember(name, arity);

    if(!found) {
        // Two messages, because they are two mistakes: a name nobody declared, and the right name
        // called with the wrong number of arguments. The second is what arity overloading makes
        // possible to get wrong, so it is the one that has to say what it saw.
        if(program.hasFunctionName(here(), name)) {
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
    if(!callee.summary.bodyKnown && !callee.klass) {
        unsupported(expr.source, "calling a function with no body"_v);
        return nullptr;
    }

    /*
     * A lens is not called; its body is resolved into the caller - Analysis-IR.md §10.1.
     *
     * So there is no call node to build here and nothing sensible to hand back: what a lens answers
     * is what its continuation answered, and the continuation is the block the call site did not
     * write. The one form that supplies one is `let x = f(...) in body`, and naming it is the whole
     * of this diagnostic.
     */
    if(callee.lens) {
        context.diagnostics.error(
            "%@ is a lens, so it needs a continuation - write `let x = %@(...) in ...`"_v,
            expr.source, context.findName(callee.name), context.findName(callee.name));
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

void Carry::save(SmallArray<Node*, 4>& into) const {
    for(Size i = 0; i < slots.size(); i++) into.push(value(U32(i)));
}

void Carry::edge(Builder& builder, SmallArray<Operand, 8>& into) {
    SmallArray<Node*, 4> now;
    save(now);
    edge(builder, now, into);
}

/*
 * What an edge supplies for the carried values.
 *
 * `argumentMode` and not a mode of this walk's own, which is the rule `closeArm` reads for the
 * merge's own parameter and the one every other position that fills a declared slot reads: an owned
 * parameter takes, a borrowed one does not, and a carry that decided for itself would be a second
 * answer to a question that already has one.
 */
void Carry::edge(Builder& builder, const SmallArray<Node*, 4>& state, SmallArray<Operand, 8>& into) {
    for(Size i = 0; i < slots.size(); i++) {
        into.push(builder.resultOf(*state[i], argumentMode(occurrences[i].contract,
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
    return isConcreteType(resolver.decl(), type) ? type : nullptr;
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

    return ok;
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

        auto value = resolveValue(builder, argument.value, typeArguments.hint(declared), wanted);
        if(!value) return false;

        typeArguments.bind(declared, valueType(*value));

        auto expected = typeArguments.expected(declared);
        if(!checkType(*value, expected, argument.value.source, "an argument"_v)) {
            return false;
        }

        /*
         * An owner handed to a parameter that only borrows.
         *
         * The callee does not take the value, so this frame still owes its teardown - and a value
         * that is only a node result has no place for one to name. Writing it into a temporary is
         * what makes the drop after the call an ordinary drop of an ordinary local, which is the
         * same rule a discarded statement follows and is stated once in `hold`.
         */
        if(wanted.access != Access::Owned && valueOccurrence(*value).contract.isOwned() &&
           hasTeardown(decl(), expected)) {
            auto place = hold(builder, *value, argument.value.source);
            value = readPlace(builder, argument.value.source, *place, wanted);
        }

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

    if(written.args.size() != callee.args.size()) {
        context.diagnostics.error("%@ takes %@ arguments but was given %@"_v, source,
                                  context.findName(callee.name), U64(callee.args.size()),
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

const Operator* Resolver::findOperator(StringId name) {
    auto text = stringView(context.findName(name));
    for(auto& entry: kOperators) {
        if(text == entry.text) return &entry;
    }

    return nullptr;
}

/*
 * An infix chain: flattened, then re-associated.
 *
 * The parser nests to the right without regard for precedence, so `a + b * c` arrives as
 * `a + (b * c)` only by accident of the tokens. Flattening the whole chain first and climbing it
 * afterwards is what makes the grouping the fixity table's answer rather than the parser's.
 */
Node* Resolver::resolveInfix(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint) {
    Chain chain;
    auto node = parse[expr.infix];

    while(true) {
        if(node->op.kind != ast::Expr::Var) {
            context.diagnostics.error("an infix operator must be a named operator"_v, node->op.source);
            return nullptr;
        }

        auto found = findOperator(node->op.var);
        if(!found) {
            unsupported(node->op.source, "this operator"_v);
            return nullptr;
        }

        chain.operands.push(&node->lhs);
        chain.operators.push(found);
        chain.sources.push(LocationId(node->op.source));

        if(node->rhs.kind != ast::Expr::Infix) {
            chain.operands.push(&node->rhs);
            break;
        }

        node = parse[node->rhs.infix];
    }

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
          chain.operators[operatorIndex]->precedence >= minimumPrecedence) {
        auto op = chain.operators[operatorIndex];
        auto source = chain.sources[operatorIndex];
        operatorIndex++;

        // One rung above this operator's own, which is what makes a left associative operator stop
        // before an equal one and take it as its own left operand instead.
        auto next = U8(op->precedence + 1);

        if(op->form != OperatorForm::Binary) {
            lhs = shortCircuit(builder, chain, operandIndex, operatorIndex, next, source, lhs, *op);
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

        lhs = applyBinary(builder, source, op->op, *lhs, *rhs);
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

    return branchValue(builder, source, *condition, program.types.boolean,
        [&](DeclPtr<Type>) -> Node* {
            // The arm that runs the right operand. `ShortAnd` reaches it when the left was True.
            if(op.form == OperatorForm::ShortAnd) {
                return climbBool(builder, chain, operandIndex, operatorIndex, next, source);
            }

            return constantBool(builder, source, shortValue);
        },
        [&](DeclPtr<Type>) -> Node* {
            if(op.form == OperatorForm::ShortAnd) return constantBool(builder, source, shortValue);
            return climbBool(builder, chain, operandIndex, operatorIndex, next, source);
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

// One arm's exit: the jump that hands its value over, or nothing where the arm already left.
void Resolver::closeArm(Builder& builder, Block& end, Block& join, Node* value, LocationId source,
                        Carry& carry, const SmallArray<Node*, 4>& carried) {
    builder.setBlock(end);
    if(builder.isTerminated()) return;

    if(!value) {
        // The arm produced no value and did not leave, which only happens after a diagnostic.
        // `unreachable` rather than a jump, because a merge it entered would need a parameter it has
        // nothing to supply.
        builder.addUnreachable(source);
        return;
    }

    /*
     * The merge's own parameter answers what filling it does, which is `argumentMode` and the same
     * rule a call argument and an aggregate component are given.
     *
     * `Take` for an owned merge: what is handed over is a node result, a value already in hand with
     * no other last use to weigh it against - `Forward` belongs to a bare *name* in sink position,
     * see UseMode. A merge of loans is not a handover at all, and saying `Take` there claimed to
     * consume storage the arm had only borrowed.
     */
    auto base = builder.base();
    auto& declared = valueOccurrence(*base[join.arguments.get(base, 0)]);

    SmallArray<Operand, 8> arguments;
    arguments.push(builder.resultOf(*value, argumentMode(declared.contract, SourceKind::Result)));

    // Then whatever crossed the branch, in the positions `branchValue` declared them in - and read
    // from where they stood at the *end of this arm*, which is why the snapshot is a parameter.
    carry.edge(builder, carried, arguments);

    builder.addJump(source, join, arguments.pointer(), arguments.size());
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

    auto condition = resolveValue(builder, written.cond, program.types.boolean, inspectRequest());
    if(!condition) return nullptr;
    if(!checkType(*condition, program.types.boolean, written.cond.source, "a condition"_v)) return nullptr;

    if(wantValue && !written.otherwise) {
        context.diagnostics.error("an `if` used as a value needs an `else`"_v, expr.source);
        return nullptr;
    }

    if(!wantValue) {
        // A statement `if` crosses values too: it is reached from inside the arm of a value one, so
        // the addition waiting on the far side of *that* branch is holding a value here as well.
        Carry carry(*this, expr.source);

        auto thenBlock = builder.addBlock(StringId());
        auto elseBlock = builder.addBlock(StringId());
        auto join = builder.addBlock(StringId());

        carry.declare(builder, *thenBlock);
        carry.declare(builder, *elseBlock);
        carry.declare(builder, *join);

        SmallArray<Operand, 8> entry;
        carry.edge(builder, entry);
        builder.addBranch(expr.source, builder.resultOf(*condition, UseMode::Inspect),
                          *thenBlock, *elseBlock, entry.pointer(), entry.size());

        builder.setBlock(*thenBlock);
        carry.arrive(builder, *thenBlock, 0);
        resolveStatement(builder, written.then);
        if(!builder.isTerminated()) {
            // Read where the arm ended rather than from a snapshot, which the value form needs and
            // this one does not: the jump is emitted here, so what the handles name is current.
            SmallArray<Operand, 8> arm;
            carry.edge(builder, arm);
            builder.addJump(expr.source, *join, arm.pointer(), arm.size());
        }

        builder.setBlock(*elseBlock);
        carry.arrive(builder, *elseBlock, 0);
        if(written.otherwise) resolveStatement(builder, written.otherwise.unwrap());
        if(!builder.isTerminated()) {
            SmallArray<Operand, 8> arm;
            carry.edge(builder, arm);
            builder.addJump(expr.source, *join, arm.pointer(), arm.size());
        }

        // A merge both arms left is unreachable and still needs a terminator. `finish` is what gives
        // it one, because whether anything entered it is a question about the finished edges rather
        // than about this statement.
        builder.setBlock(*join);
        carry.arrive(builder, *join, 0);
        return nullptr;
    }

    auto& otherwise = written.otherwise.unwrap();
    return branchValue(builder, expr.source, *condition, hint,
        [&](DeclPtr<Type> armHint) { return resolveValue(builder, written.then, armHint, wanted); },
        [&](DeclPtr<Type> armHint) { return resolveValue(builder, otherwise, armHint, wanted); });
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
    Node* result = nullptr;

    auto statements = expr.multi;
    auto count = statements.size();

    /*
     * Where this sequence has got to, kept where a suspended walk can read it - `Resolver::tails`.
     *
     * A `yield` in a lens body suspends this loop in the middle, resolves the caller's continuation,
     * and comes back; an exit out of that continuation has to run the statements this loop has not
     * reached yet, in a copy of its own. The index is therefore on the resolver rather than only in
     * this frame's C++ locals, and it costs a push and a pop per block.
     */
    auto step = U32(tails.size());
    tails.push(TailStep { statements, 0 });

    for(U32 index = 0; index < count; index++) {
        tails[step].next = index + 1;

        // A statement after control left is unreachable, and resolving it would append to a block
        // that already ends. Stopping is what the milestone needs; a warning about the code that was
        // dropped is a diagnostic question rather than a representation one.
        if(builder.isTerminated()) break;

        auto statement = statements.get(parse, index);

        if(wantValue && index + 1 == count) {
            result = resolveValue(builder, statement, hint, wanted);
            break;
        }

        resolveStatement(builder, statement);
    }

    tails.resize(step);
    bindings.resize(U32(scope));
    return result;
}

} // namespace mir
