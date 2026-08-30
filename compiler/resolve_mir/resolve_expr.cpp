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
 */
Place* Resolver::hold(Builder& builder, Node& value, LocationId source) {
    auto local = builder.addLocal(StringId(), owned(valueType(value)), source, false);
    auto place = builder.localPlace(*local);

    builder.addWrite(source, *place, builder.resultOf(value, UseMode::Take), Destination::Init);
    return place;
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
                auto value = resolveValue(builder, written.target, nullptr, Access::Shared);
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
Node* Resolver::resolveField(Builder& builder, ast::Expr& expr, Access wanted) {
    auto place = resolvePlace(builder, expr, true);
    if(!place) return nullptr;

    return readPlace(builder, expr.source, *place, wanted);
}

Node* Resolver::readPlace(Builder& builder, LocationId source, Place& place, Access wanted) {
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
    if(wanted == Access::Owned && mode == UseMode::BorrowShared) {
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
    auto type = asBase<Type>(record);

    if(hint && !sameType(hint, type)) {
        context.diagnostics.error("this position asks for %@, and this builds %@"_v, expr.source,
                                  typeText(context, decl(), hint), typeText(context, decl(), type));
        return nullptr;
    }

    SmallArray<Node*, 8> values;
    for(Size i = 0; i < declared->fields.size(); i++) values.push(nullptr);

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

        if(values[index]) {
            context.diagnostics.error("%@ is given twice"_v, argument.value.source,
                                      context.findName(argument.name));
            ok = false;
            continue;
        }

        auto field = declared->field(decl(), U16(index));
        auto value = resolveValue(builder, argument.value, field.type, Access::Owned);
        if(!value) { ok = false; continue; }
        if(!checkType(*value, field.type, argument.value.source, "a field"_v)) { ok = false; continue; }

        values[index] = value;
    }

    for(auto field: declared->fields.contents(decl())) {
        if(values[field.index]) continue;

        context.diagnostics.error("%@ leaves out the field %@"_v, expr.source,
                                  typeText(context, decl(), type), context.findName(field.name));
        ok = false;
    }

    if(!ok) return nullptr;
    return builder.addAggregate(expr.source, type, values.pointer(), values.size());
}

Node* Resolver::resolveValue(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, Access wanted) {
    switch(expr.kind) {
        case ast::Expr::Nested:
            return resolveValue(builder, *parse[expr.nested], hint, wanted);

        case ast::Expr::Var:
        case ast::Expr::Field:
            return resolveField(builder, expr, wanted);

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

        case ast::Expr::Infix:
            return resolveInfix(builder, expr, hint);
        case ast::Expr::Prefix:
            return resolvePrefix(builder, expr, hint);
        case ast::Expr::App:
            return resolveCall(builder, expr);
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
Node* Resolver::resolveCall(Builder& builder, ast::Expr& expr) {
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

    auto found = program.findFunction(here(), name);
    if(!found) {
        context.diagnostics.error("unknown function %@"_v, written.callee.source,
                                  context.findName(name));
        return nullptr;
    }

    auto& callee = *decl()[found];

    // A declaration with no body is a call this stage cannot emit. It is separate from the summary's
    // own answer for one - `bodyKnown` false publishes "may suspend" and is the right fact - because
    // what stops here is code generation rather than analysis.
    if(!callee.summary.bodyKnown) {
        unsupported(expr.source, "calling a function with no body"_v);
        return nullptr;
    }

    if(!checkCallShape(written, callee, expr.source)) return nullptr;

    SmallArray<Node*, 8> arguments;
    Size index = 0;

    for(auto argument: written.args.contents(parse)) {
        auto& parameter = *decl()[callee.args.get(decl(), U32(index++))];
        auto wanted = parameter.occurrence.contract.access();

        auto value = resolveValue(builder, argument.value, parameter.occurrence.type, wanted);
        if(!value) return nullptr;

        if(!checkType(*value, parameter.occurrence.type, argument.value.source, "an argument"_v)) {
            return nullptr;
        }

        /*
         * An owner handed to a parameter that only borrows.
         *
         * The callee does not take the value, so this frame still owes its teardown - and a value
         * that is only a node result has no place for one to name. Writing it into a temporary is
         * what makes the drop after the call an ordinary drop of an ordinary local, which is the
         * same rule a discarded statement follows and is stated once in `hold`.
         */
        if(wanted != Access::Owned && valueOccurrence(*value).contract.isOwned() &&
           hasTeardown(decl(), parameter.occurrence.type)) {
            auto place = hold(builder, *value, argument.value.source);
            value = readPlace(builder, argument.value.source, *place, wanted);
        }

        arguments.push(value);
    }

    return builder.addCall(expr.source, callee, arguments.pointer(), arguments.size());
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
    auto value = resolveValue(builder, written.on, operandHint, Access::Shared);
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
    auto lhs = resolveValue(builder, *chain.operands[operandIndex++], hint, Access::Shared);

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

        auto rhs = climb(builder, chain, operandIndex, operatorIndex, next, valueType(*lhs));
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
void Resolver::closeArm(Builder& builder, Block& end, Block& join, Node* value, LocationId source) {
    builder.setBlock(end);
    if(builder.isTerminated()) return;

    if(!value) {
        // The arm produced no value and did not leave, which only happens after a diagnostic.
        // `unreachable` rather than a jump, because a merge it entered would need a parameter it has
        // nothing to supply.
        builder.addUnreachable(source);
        return;
    }

    // `Take` rather than a `Forward` candidate: what is being handed over is a node result, which is
    // a value already in hand and has no other last use to weigh it against. `Forward` belongs to a
    // bare *name* in sink position - see UseMode.
    auto argument = builder.resultOf(*value, UseMode::Take);
    builder.addJump(source, join, &argument, 1);
}

/*
 * `if c then a else b`, as a value or as a statement.
 *
 * One function for both, because the difference is only what the arms are resolved as and whether
 * the merge takes a parameter. A statement `if` with no `else` still branches to a merge from both
 * sides, so the block structure a reader sees does not change with the presence of a value.
 */
Node* Resolver::resolveIf(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue,
                          Access wanted) {
    auto& written = *parse[expr.singleIf];

    auto condition = resolveValue(builder, written.cond, program.types.boolean, Access::Shared);
    if(!condition) return nullptr;
    if(!checkType(*condition, program.types.boolean, written.cond.source, "a condition"_v)) return nullptr;

    if(wantValue && !written.otherwise) {
        context.diagnostics.error("an `if` used as a value needs an `else`"_v, expr.source);
        return nullptr;
    }

    if(!wantValue) {
        auto thenBlock = builder.addBlock(StringId());
        auto elseBlock = builder.addBlock(StringId());
        auto join = builder.addBlock(StringId());

        builder.addBranch(expr.source, builder.resultOf(*condition, UseMode::Inspect),
                          *thenBlock, *elseBlock);

        builder.setBlock(*thenBlock);
        resolveStatement(builder, written.then);
        if(!builder.isTerminated()) builder.addJump(expr.source, *join, nullptr, 0);

        builder.setBlock(*elseBlock);
        if(written.otherwise) resolveStatement(builder, written.otherwise.unwrap());
        if(!builder.isTerminated()) builder.addJump(expr.source, *join, nullptr, 0);

        // A merge both arms left is unreachable and still needs a terminator. `finish` is what gives
        // it one, because whether anything entered it is a question about the finished edges rather
        // than about this statement.
        builder.setBlock(*join);
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
                             Access wanted) {
    auto scope = bindings.size();
    Node* result = nullptr;

    auto statements = expr.multi;
    auto count = statements.size();
    Size index = 0;

    for(auto statement: statements.contents(parse)) {
        index++;

        // A statement after control left is unreachable, and resolving it would append to a block
        // that already ends. Stopping is what the milestone needs; a warning about the code that was
        // dropped is a diagnostic question rather than a representation one.
        if(builder.isTerminated()) break;

        if(wantValue && index == count) {
            result = resolveValue(builder, statement, hint, wanted);
            break;
        }

        resolveStatement(builder, statement);
    }

    bindings.resize(U32(scope));
    return result;
}

} // namespace mir
