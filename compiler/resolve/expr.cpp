#include "expr.h"
#include "name.h"

void ExprResolver::terminate(Inst* inst) {
    assertTrue(isTerminator(*inst));
    current = nullptr;
}

ModulePtr<Value> ExprResolver::find(StringId name) {
    for(Size i = bindings.size(); i > 0; i--) {
        if(bindings[i - 1].name == name) return bindings[i - 1].value;
    }

    return nullptr;
}

ModulePtr<Value> ExprResolver::makeInt(LocationId source, TypePtr type, U64 value) {
    return constant<ConstInt>(source, type, value);
}

ModulePtr<Value> ExprResolver::makeFloat(LocationId source, TypePtr type, F64 value) {
    if(type == module.scalar.float_) return constant<ConstFloat>(source, type, F32(value));
    return constant<ConstDouble>(source, type, value);
}

// The numeric types in widening order. This is not the conversion rule - Extend and Truncate
// are, and a user type may have instances of either - but it is the rule for which direction is
// implicit, and the tie-break that lets `1 + 2.5` reach one Num instance rather than none.
U8 ExprResolver::numericRank(TypePtr type) {
    if(type == module.scalar.int_) return 0;
    if(type == module.scalar.long_) return 1;
    if(type == module.scalar.float_) return 2;
    if(type == module.scalar.double_) return 3;
    return 0;
}

TypePtr ExprResolver::commonNumeric(TypePtr lhs, TypePtr rhs, LocationId source) {
    if(!isNumeric(global, lhs) || !isNumeric(global, rhs)) {
        context.diagnostics.error("operator requires numeric operands"_v, source);
        return module.scalar.error;
    }

    return numericRank(lhs) >= numericRank(rhs) ? lhs : rhs;
}

/*
 * Conversion is a class operation.
 *
 * `Extend(a, b)` widens and `Truncate(a, b)` narrows; an implicit conversion may only use the
 * first. Routing every conversion through instance selection rather than through a built-in
 * table of the five primitives is what will make a cast on a user-defined type work through the
 * same path, and what makes `x :: Long` on a type with a Truncate instance mean something.
 *
 * The primitive instances are intrinsics, so this still produces exactly one `cast` in the IR.
 */
ModulePtr<Value> ExprResolver::convert(ModulePtr<Value> value, TypePtr target, LocationId source, bool implicit) {
    if(!value || !target) return value;

    auto from = local[value]->type;
    if(sameType(from, target)) return value;

    if(global[from]->kind == Type::Error || global[target]->kind == Type::Error) return value;

    // A narrowing conversion has to be asked for. Checking the direction here rather than by the
    // absence of an Extend instance keeps the diagnostic about precision instead of about a
    // missing instance the user never mentioned.
    auto widening = !isNumeric(global, from) || !isNumeric(global, target) ||
                    numericRank(from) <= numericRank(target);

    if(implicit && !widening) {
        context.diagnostics.error("implicit conversion from %@ to %@ would lose precision"_v, source,
                                  describeType(context, global, from), describeType(context, global, target));
        return value;
    }

    auto name = widening ? context.addUnqualifiedName("extend", 6) : context.addUnqualifiedName("truncate", 8);
    Array<ClassFunRef> candidates;
    findClassFunctions(module, name, source, candidates);

    for(auto& candidate: candidates) {
        ModulePtr<Value> args[] = { value };

        ClassMatch match;
        if(!matchClassFun(candidate, { args, 1 }, target, match) || !match.instance) continue;

        auto implementation = local[match.instance]->functions.get(local, match.index);
        if(!implementation) continue;

        return emitDirectCall(implementation, { args, 1 }, source);
    }

    context.diagnostics.error("cannot convert %@ to %@"_v, source,
                              describeType(context, global, from), describeType(context, global, target));
    return value;
}

// The same question convert() answers, asked without answering it. Overload selection has to know
// whether a candidate accepts an argument before it commits to that candidate, and convert()
// cannot be used for that: reporting the mismatch is its job, and a candidate that does not fit is
// not an error while another member of the overload set may still serve the call.
bool ExprResolver::convertible(ModulePtr<Value> value, TypePtr target, LocationId source) {
    if(!value || !target) return false;

    auto from = valueType(value);
    if(sameType(from, target)) return true;

    // An error type has already been reported once, so it fits anything rather than producing a
    // second diagnostic about the same mistake.
    if(global[from]->kind == Type::Error || global[target]->kind == Type::Error) return true;

    // Only widening is implicit, so a narrowing pair does not fit even though convert() would
    // perform it when asked explicitly.
    if(isNumeric(global, from) && isNumeric(global, target)) return numericRank(from) <= numericRank(target);

    Array<ClassFunRef> candidates;
    findClassFunctions(module, context.addUnqualifiedName("extend", 6), source, candidates);

    for(auto& candidate: candidates) {
        ModulePtr<Value> args[] = { value };

        ClassMatch match;
        if(!matchClassFun(candidate, { args, 1 }, target, match) || !match.instance) continue;
        if(local[match.instance]->functions.get(local, match.index)) return true;
    }

    return false;
}

ModulePtr<Value> ExprResolver::finishBranches(Array<BranchArm>& arms, LocationId source, bool used) {
    // Every arm that diverged - returned, or broke out of a loop - left no block behind. If none
    // of them did leave one, the expression as a whole never completes and there is no join.
    if(arms.isEmpty()) {
        current = nullptr;
        return nullptr;
    }

    // An arm with no value is one that could not produce one (a missing `else`, or an error
    // already reported); it makes the whole expression valueless rather than the phi partial.
    auto values = used;
    TypePtr resultType = nullptr;

    for(auto& arm: arms) {
        if(!values) break;
        if(!arm.value) {
            values = false;
            break;
        }

        auto type = valueType(arm.value);
        if(!resultType) {
            resultType = type;
        } else if(!sameType(resultType, type)) {
            if(isNumeric(global, resultType) && isNumeric(global, type)) {
                resultType = commonNumeric(resultType, type, arm.source);
            } else {
                context.diagnostics.error("branches of this expression have different types"_v, arm.source);
                values = false;
            }
        }
    }

    auto join = addBlock();

    // Each arm's conversion goes at the end of that arm's own block: a phi input has to already
    // have the phi's type in the block it comes from, and the type to convert to is only known
    // once every arm has been seen.
    for(auto& arm: arms) {
        current = arm.end;
        if(values) arm.value = convert(arm.value, resultType, arm.source);
        terminate(emit<InstJmp>(arm.source, 0, module.scalar.unit, join));
    }

    current = join;
    if(!values) return nullptr;
    if(arms.size() == 1) return arms[0].value;

    auto phi = create<InstPhi>(source, 0, resultType);
    for(auto& arm: arms) phi->inputs.push(module.arena, PhiInput { arm.end, arm.value });
    append(phi);

    auto result = ref(phi);
    if(isMemoryType(global, resultType)) function.addLocal(module, resultType, 0, result);

    return result;
}

ModulePtr<Value> ExprResolver::resolveIf(const ast::Expr& expr, const ast::IfExpr& branch, TypePtr target, bool used) {
    auto cond = convert(resolve(branch.cond, module.scalar.bool_), module.scalar.bool_, branch.cond.source);
    if(!cond) return nullptr;

    auto thenBlock = addBlock();
    auto elseBlock = addBlock();
    terminate(emit<InstJe>(expr.source, 0, module.scalar.unit, cond, thenBlock, elseBlock));

    auto bindingCount = bindings.size();
    Array<BranchArm> arms;

    current = thenBlock;
    auto thenValue = resolve(branch.then, target, used);
    if(current) arms.push(BranchArm { current, thenValue, branch.then.source });
    bindings.resize(bindingCount);

    current = elseBlock;
    ModulePtr<Value> elseValue = nullptr;
    auto elseSource = expr.source;

    if(branch.otherwise) {
        elseValue = resolve(branch.otherwise.unwrap(), target, used);
        elseSource = branch.otherwise.unwrap().source;
    } else if(used) {
        context.diagnostics.error("value-producing if requires an else branch"_v, expr.source);
    }

    if(current) arms.push(BranchArm { current, elseValue, elseSource });
    bindings.resize(bindingCount);

    return finishBranches(arms, expr.source, used);
}

ModulePtr<Value> ExprResolver::resolveMultiIf(const ast::Expr& expr, ast::ParseList<ast::IfCase> cases, TypePtr target, bool used) {
    auto contents = cases.contents(parse);
    if(contents.size() == 0) return nullptr;

    auto bindingCount = bindings.size();
    Array<BranchArm> arms;
    auto hasElse = false;

    for(Size i = 0; i < contents.size() && current; i++) {
        // The parser writes a trailing `_`/`else` case as a `True` literal condition, so an
        // always-taken final case is recognized here rather than being tested at runtime.
        auto isElse = i + 1 == contents.size() &&
                      ast::isLiteral(contents[i].cond) &&
                      ast::Literal::Kind(contents[i].cond.kind - ast::Expr::Lit) == ast::Literal::Bool &&
                      contents[i].cond.lit.b;

        ModulePtr<Block> nextBlock = nullptr;

        if(isElse) {
            hasElse = true;
        } else {
            auto cond = convert(resolve(contents[i].cond, module.scalar.bool_), module.scalar.bool_, contents[i].cond.source);
            if(!cond) return nullptr;

            auto thenBlock = addBlock();
            nextBlock = addBlock();
            terminate(emit<InstJe>(contents[i].cond.source, 0, module.scalar.unit, cond, thenBlock, nextBlock));
            current = thenBlock;
        }

        auto value = resolve(contents[i].then, target, used);
        if(current) arms.push(BranchArm { current, value, contents[i].then.source });
        bindings.resize(bindingCount);

        current = nextBlock;
    }

    // Without an else case, control can fall out of the last test having produced nothing.
    if(current) {
        if(used) context.diagnostics.error("value-producing multi-if requires an else case"_v, expr.source);
        arms.push(BranchArm { current, nullptr, expr.source });
    }

    return finishBranches(arms, expr.source, used && hasElse);
}

void ExprResolver::resolveWhile(const ast::WhileExpr& loop) {
    auto conditionBlock = addBlock();
    auto bodyBlock = addBlock();
    auto exitBlock = addBlock();

    terminate(emit<InstJmp>(loop.cond.source, 0, module.scalar.unit, conditionBlock));

    current = conditionBlock;
    auto cond = convert(resolve(loop.cond, module.scalar.bool_), module.scalar.bool_, loop.cond.source);
    terminate(emit<InstJe>(loop.cond.source, 0, module.scalar.unit, cond, bodyBlock, exitBlock));

    loops.push(LoopTarget { conditionBlock, exitBlock });
    current = bodyBlock;
    resolve(loop.body, nullptr, false);
    loops.pop();

    if(current) terminate(emit<InstJmp>(loop.body.source, 0, module.scalar.unit, conditionBlock));
    current = exitBlock;
}

void ExprResolver::resolveReturn(const ast::Expr& expr) {
    ModulePtr<Value> value = nullptr;
    if(expr.ret) value = resolve(*parse[expr.ret], function.returnType);

    if(isUnit(global, function.returnType)) {
        if(value) context.diagnostics.error("unit function cannot return a value"_v, expr.source);
        value = nullptr;
    } else if(!value) {
        context.diagnostics.error("non-unit function must return a value"_v, expr.source);
    } else {
        value = convert(value, function.returnType, expr.source);
    }

    terminate(emit<InstRet>(expr.source, 0, module.scalar.unit, value));
}

ModulePtr<Value> ExprResolver::resolveDecl(ast::ParseList<ast::VarDecl> declarations, TypePtr target, bool used) {
    ModulePtr<Value> result = nullptr;

    for(auto decl: declarations.contents(parse)) {
        if(!decl.content) {
            context.diagnostics.error("let requires an initializer"_v, decl.pat.source);
            continue;
        }

        if(decl.bind != ast::BindType::Borrow) {
            context.diagnostics.error("let binding conventions are deferred until the ownership resolver"_v, decl.pat.source);
        }

        auto checkpoint = bindings.size();
        auto value = resolve(*parse[decl.content]);
        if(!value) continue;

        if(!irrefutable(decl.pat, valueType(value))) {
            context.diagnostics.error(
                decl.alts.isNotEmpty()
                    ? "refutable declaration alternatives require ownership-aware initialization"_v
                    : "refutable let pattern requires alternatives"_v,
                decl.pat.source);

            continue;
        }

        // The pattern is irrefutable, so it emits no test: `current` stands in for a failure
        // block that is never branched to.
        resolvePattern(decl.pat, value, current, nullptr);

        if(decl.in) {
            result = resolve(*parse[decl.in], target, used);
            bindings.resize(checkpoint);
        } else {
            result = value;
        }
    }

    return result;
}

ModulePtr<Value> ExprResolver::resolveLiteral(const ast::Expr& expr, TypePtr target) {
    switch(ast::Literal::Kind(expr.kind - ast::Expr::Lit)) {
        case ast::Literal::Int: {
            // An integer-syntax literal can resolve to either kind, so a floating target takes
            // it as a float constant rather than as an Int that is then converted.
            if(target && isFloat(global, target)) return makeFloat(expr.source, target, F64(expr.lit.i()));
            return makeInt(expr.source, target && isInteger(global, target) ? target : module.scalar.int_, expr.lit.i());
        }
        case ast::Literal::Float:
            return makeFloat(expr.source, target && isFloat(global, target) ? target : module.scalar.float_, F64(expr.lit.f));
        case ast::Literal::Double:
            // Decimal syntax defaults to Float and can only resolve to a floating type. The
            // parser keeps every decimal literal at F64 precision until one is picked here.
            return makeFloat(expr.source, target && isFloat(global, target) ? target : module.scalar.float_, expr.lit.d());
        case ast::Literal::Bool:
            return makeInt(expr.source, module.scalar.bool_, expr.lit.b ? 1 : 0);
        default:
            context.diagnostics.error("literal is not available in the aggregate resolver"_v, expr.source);
            return nullptr;
    }
}

ModulePtr<Value> ExprResolver::resolve(const ast::Expr& expr, TypePtr target, bool used) {
    if(!current) return nullptr;
    if(ast::isLiteral(expr)) return resolveLiteral(expr, target);

    switch(expr.kind) {
        case ast::Expr::Error:
            return nullptr;
        case ast::Expr::Nested:
            return resolve(*parse[expr.nested], target, used);
        case ast::Expr::Multi: {
            ModulePtr<Value> result = nullptr;
            auto expressions = expr.multi;
            auto values = expressions.contents(parse);

            for(Size i = 0; i < values.size() && current; i++) {
                auto last = i + 1 == values.size();
                result = resolve(values[i], last ? target : nullptr, used && last);
            }

            return result;
        }
        case ast::Expr::Var: {
            auto value = find(expr.var);
            if(!value) {
                context.diagnostics.error("unknown scalar value %@"_v, expr.source, context.findName(expr.var));
                return nullptr;
            }

            return target ? convert(value, target, expr.source) : value;
        }
        case ast::Expr::Con:
            return resolveConstruct(expr, *parse[expr.con], target);
        case ast::Expr::App:
            return resolveCall(expr, *parse[expr.app], target);
        case ast::Expr::Infix:
            return resolveBinary(expr, *parse[expr.infix], target);
        case ast::Expr::Prefix:
            return resolvePrefix(expr, *parse[expr.prefix], target);
        case ast::Expr::If:
            return resolveIf(expr, *parse[expr.singleIf], target, used);
        case ast::Expr::MultiIf:
            return resolveMultiIf(expr, expr.multiIf, target, used);
        case ast::Expr::Match:
            return resolveMatch(expr, *parse[expr.match], target, used);
        case ast::Expr::Decl:
            return resolveDecl(expr.decl, target, used);
        case ast::Expr::While:
            resolveWhile(*parse[expr.whileLoop]);
            return nullptr;
        case ast::Expr::Coerce: {
            auto& coerce = *parse[expr.coerce];
            auto type = resolveType(module, coerce.type);

            // `::` is what supplies the expected type where nothing else does, so it is pushed
            // down into a literal (which has no type of its own) and into a call (whose class
            // instance may be decided by its result type - `truncate(x) :: Int`). The call keeps
            // its own result unconverted, because the ascription that selected the instance is
            // also the explicit conversion, and an explicit one may narrow.
            if(ast::isLiteral(coerce.target)) {
                return convert(resolve(coerce.target, type), type, expr.source, false);
            }

            if(coerce.target.kind == ast::Expr::App) {
                auto value = resolveCall(coerce.target, *parse[coerce.target.app], type, false);
                return convert(value, type, expr.source, false);
            }

            if(coerce.target.kind == ast::Expr::Prefix) {
                auto value = resolvePrefix(coerce.target, *parse[coerce.target.prefix], type, false);
                return convert(value, type, expr.source, false);
            }

            return convert(resolve(coerce.target), type, expr.source, false);
        }
        case ast::Expr::Ret:
            resolveReturn(expr);
            return nullptr;
        case ast::Expr::Break:
        case ast::Expr::Continue: {
            if(loops.isEmpty()) {
                context.diagnostics.error(expr.kind == ast::Expr::Break ? "break outside a loop"_v : "continue outside a loop"_v, expr.source);
                return nullptr;
            }

            if(expr.kind == ast::Expr::Break && expr.breakValue) {
                context.diagnostics.error("scalar while loops do not produce values"_v, expr.source);
            }

            auto& loop = loops[loops.size() - 1];
            auto targetBlock = expr.kind == ast::Expr::Break ? loop.breakBlock : loop.continueBlock;
            terminate(emit<InstJmp>(expr.source, 0, module.scalar.unit, targetBlock));

            return nullptr;
        }
        case ast::Expr::Tup:
            return resolveTuple(expr, expr.tup, target);
        case ast::Expr::Field:
            return resolveField(expr, *parse[expr.field]);
        case ast::Expr::Assign:
            context.diagnostics.error("assignment requires the ownership resolver's places and mutable bindings"_v, expr.source);
            return nullptr;
        default:
            context.diagnostics.error("expression is not available in the aggregate resolver"_v, expr.source);
            return nullptr;
    }
}

// Class signatures, generated functions and specializations have no AST and are already complete.
bool resolveFunctionBody(Module& module, Function& function) {
    auto& context = module.context;
    if(!function.ast || function.resolving) return true;

    auto& decl = *module.parse[function.ast];
    if(!decl.fun.body) {
        context.diagnostics.error("function %@ requires a body"_v, decl.source, context.findName(function.name));
        return false;
    }

    function.resolving = true;

    ExprResolver resolver(context, module, function);
    for(auto argPointer: function.args.contents(*module.arena)) {
        auto arg = (*module.arena)[argPointer];
        auto value = (ModulePtr<Value>)argPointer;

        if(isMemoryType(*module.types, arg->type)) {
            function.addLocal(module, arg->type, arg->name, value);
        }

        resolver.bindings.push(Binding { arg->name, value });
    }

    auto errors = context.diagnostics.errorCount();

    if(decl.fun.implicitReturn) {
        auto result = resolver.resolve(*module.parse[decl.fun.body], function.returnType, true);

        if(resolver.current) {
            result = isUnit(*module.types, function.returnType)
                ? nullptr
                : resolver.convert(result, function.returnType, decl.source);

            resolver.terminate(resolver.emit<InstRet>(decl.source, 0, module.scalar.unit, result));
        }
    } else {
        resolver.resolve(*module.parse[decl.fun.body], nullptr, false);

        if(resolver.current) {
            if(isUnit(*module.types, function.returnType)) {
                resolver.terminate(resolver.emit<InstRet>(decl.source, 0, module.scalar.unit, nullptr));
            } else {
                context.diagnostics.error("not all paths return a value"_v, decl.source);
            }
        }
    }

    function.ast = nullptr;
    function.resolving = false;
    return errors == context.diagnostics.errorCount();
}

bool resolveModuleBodies(Module& module) {
    auto success = true;
    auto local = *module.arena;

    // Resolving one body adds specialized functions to the module, so the list is walked by index
    // rather than by iterator: a specialization created while resolving function 3 is reached
    // when the loop gets to it.
    for(Size i = 0; i < module.functionOrder.size(); i++) {
        success = resolveFunctionBody(module, *local[module.functionOrder.get(local, i)]) && success;
    }

    return success;
}
