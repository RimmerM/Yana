#include "expr.h"
#include "name.h"

/*
 * Patterns and `match`.
 *
 * Resolving a pattern emits the tests it needs into the current block and binds the names it
 * introduces, returning what it proved: an irrefutable pattern emits nothing, a refutable one
 * emits a branch to `onFail`. `match` chains those failure blocks together, so the alternatives
 * are tried in order and the last one - if the coverage tracking shows it must match - skips its
 * test entirely rather than branching to a trap.
 */

PatternResult ExprResolver::branchPattern(ModulePtr<Value> condition, ModulePtr<Block> onFail, LocationId source) {
    if(!condition) return PatternResult::Never;

    condition = convert(condition, module.scalar.bool_, source);
    auto success = addBlock();
    terminate(emit<InstJe>(source, 0, module.scalar.unit, condition, success, onFail));
    current = success;

    return PatternResult::Maybe;
}

// The value a range bound or a literal pattern compares against. A bound may be a literal, an
// existing binding, or `_` for an open end.
ModulePtr<Value> ExprResolver::patternBound(const ast::Pat& pattern, TypePtr target) {
    if(pattern.kind == ast::Pat::Any) return nullptr;

    if(pattern.kind == ast::Pat::Var) {
        auto value = find(pattern.var);
        if(!value) {
            context.diagnostics.error("unknown value in range pattern %@"_v, pattern.source, context.findName(pattern.var));
        }

        return value;
    }

    if(pattern.kind >= ast::Pat::Lit) {
        ast::Expr literal {
            .lit = pattern.lit,
            .source = pattern.source,
            .kind = ast::Expr::Kind(ast::Expr::Lit + (pattern.kind - ast::Pat::Lit)),
        };

        return resolve(literal, target);
    }

    context.diagnostics.error("range pattern bounds must be literals, existing values, or _"_v, pattern.source);
    return nullptr;
}

// Whether this pattern matches every value of `type`, which is what decides both that a `let`
// needs no alternatives and that a match alternative needs no test.
bool ExprResolver::irrefutable(const ast::Pat& pattern, TypePtr type) {
    if(pattern.kind == ast::Pat::Any || pattern.kind == ast::Pat::Var) return true;

    if(pattern.kind == ast::Pat::Tup) {
        if(!type || global[type]->kind != Type::Tup) return false;

        auto tuple = (TupType*)global[type];
        auto fieldList = pattern.tup;
        auto fields = fieldList.contents(parse);
        Size positional = 0;

        for(auto field: fields) {
            Size index = maxLimit<Size>;

            if(field.field) {
                for(Size i = 0; i < tuple->fields.size(); i++) {
                    if(tuple->fields.get(global, i).name == field.field) {
                        index = i;
                        break;
                    }
                }
            } else if(positional < tuple->fields.size()) {
                index = positional++;
            }

            if(index == maxLimit<Size> || !irrefutable(*parse[field.pat], tuple->fields.get(global, index).type)) return false;
        }

        return true;
    }

    if(pattern.kind == ast::Pat::Con) {
        auto found = findConstructor(module, pattern.con.name, pattern.source);
        if(!found || !type || global[type]->kind != Type::Record) return false;

        // The constructor names a declaration; the pivot has one of its instantiations. The
        // content type therefore comes from the pivot, where the type arguments are known.
        auto record = (RecordType*)global[type];
        if(record->base(global) != found.unwrap().record) return false;
        if(record->constructors.size() != 1) return false;
        if(!pattern.con.pats) return true;

        return irrefutable(*parse[pattern.con.pats], record->constructors.get(global, found.unwrap().index).content);
    }

    return false;
}

PatternResult ExprResolver::resolvePattern(const ast::Pat& pattern, ModulePtr<Value> pivot, ModulePtr<Block> onFail, RecordCoverage* coverage) {
    if(pattern.asVar) {
        if(find(pattern.asVar)) {
            context.diagnostics.error("pattern name is already bound %@"_v, pattern.source, context.findName(pattern.asVar));
        } else {
            bindings.push(Binding { pattern.asVar, pivot });
        }
    }

    switch(pattern.kind) {
        case ast::Pat::Error:
            return PatternResult::Never;
        case ast::Pat::Any:
            return PatternResult::Always;
        case ast::Pat::Var: {
            // A name that is already bound is a test against that value rather than a new
            // binding, which is what makes `match x: y -> ...` mean "equal to y".
            if(auto existing = find(pattern.var)) {
                ModulePtr<Value> args[] = { pivot, existing };
                return branchPattern(emitCall(Context::nameHash("==", 2), { args, 2 }, pattern.source, module.scalar.bool_), onFail, pattern.source);
            }

            if(local[pivot]->name == 0) local[pivot]->name = pattern.var;
            bindings.push(Binding { pattern.var, pivot });
            return PatternResult::Always;
        }
        case ast::Pat::Tup: {
            auto type = valueType(pivot);
            if(global[type]->kind != Type::Tup) {
                context.diagnostics.error("tuple pattern used on a non-tuple value"_v, pattern.source);
                return PatternResult::Never;
            }

            auto tuple = (TupType*)global[type];
            auto root = placeFor(pivot, pattern.source);
            auto fieldList = pattern.tup;
            auto fields = fieldList.contents(parse);
            Size positional = 0;
            auto overall = PatternResult::Always;

            for(auto fieldPattern: fields) {
                Size index = maxLimit<Size>;

                if(fieldPattern.field) {
                    for(Size i = 0; i < tuple->fields.size(); i++) {
                        if(tuple->fields.get(global, i).name == fieldPattern.field) {
                            index = i;
                            break;
                        }
                    }
                } else if(positional < tuple->fields.size()) {
                    index = positional++;
                }

                if(index == maxLimit<Size>) {
                    context.diagnostics.error("tuple pattern refers to a missing field"_v, parse[fieldPattern.pat]->source);
                    return PatternResult::Never;
                }

                auto child = load(project(root, ProjectionKind::Field, U16(index)), parse[fieldPattern.pat]->source);
                auto result = resolvePattern(*parse[fieldPattern.pat], child, onFail, nullptr);

                if(result == PatternResult::Never) return result;
                if(result == PatternResult::Maybe) overall = result;
            }

            return overall;
        }
        case ast::Pat::Con: {
            auto found = findConstructor(module, pattern.con.name, pattern.source);
            auto pivotType = valueType(pivot);

            if(!found || global[pivotType]->kind != Type::Record) {
                context.diagnostics.error("constructor pattern is incompatible with the pivot"_v, pattern.source);
                return PatternResult::Never;
            }

            auto reference = found.unwrap();
            auto record = (RecordType*)global[pivotType];
            auto recordType = pivotType;

            // A constructor belongs to a declaration, so `Just` matches any `Maybe(a)`; the
            // content it exposes is the pivot's own, with that pivot's type arguments in it.
            if(record->base(global) != reference.record) {
                context.diagnostics.error("constructor %@ does not belong to %@"_v, pattern.source,
                                          context.findName(pattern.con.name),
                                          describeType(context, global, pivotType));
                return PatternResult::Never;
            }

            auto constructor = record->constructors.get(global, reference.index);
            auto childAlways = !pattern.con.pats || irrefutable(*parse[pattern.con.pats], constructor.content);
            auto lastConstructor = record->constructors.size() == 1;

            // Once every other constructor has been tested and failed, this one is the only
            // possibility left, so it needs no test of its own.
            if(coverage && coverage->type == recordType && childAlways) {
                auto already = (coverage->checked & (U64(1) << reference.index)) != 0;
                lastConstructor = coverage->checkedCount + (already ? 0 : 1) == record->constructors.size();

                if(!already) {
                    coverage->checked |= U64(1) << reference.index;
                    coverage->checkedCount++;
                }
            }

            auto testedConstructor = false;

            if(record->constructors.size() > 1 && !lastConstructor) {
                ModulePtr<Value> discriminant = pivot;

                if(record->layout == RecordType::Enum) {
                    discriminant = ref(emit<InstUnary>(pattern.source, 0, module.scalar.int_, Value::Cast, pivot));
                } else {
                    discriminant = load(project(placeFor(pivot, pattern.source), ProjectionKind::Discriminant, 0), pattern.source);
                }

                ModulePtr<Value> args[] = {
                    discriminant,
                    makeInt(pattern.source, module.scalar.int_, reference.index),
                };

                branchPattern(emitCall(Context::nameHash("==", 2), { args, 2 }, pattern.source, module.scalar.bool_), onFail, pattern.source);
                testedConstructor = true;
            }

            auto childResult = PatternResult::Always;

            if(pattern.con.pats && !isUnit(global, constructor.content)) {
                if(record->layout == RecordType::Enum) {
                    context.diagnostics.error("nullary constructor pattern cannot contain a child pattern"_v, pattern.source);
                    return PatternResult::Never;
                }

                auto content = load(project(placeFor(pivot, pattern.source), ProjectionKind::Downcast, reference.index), pattern.source);
                childResult = resolvePattern(*parse[pattern.con.pats], content, onFail, nullptr);
            }

            if(childResult == PatternResult::Never) return childResult;
            return testedConstructor ? PatternResult::Maybe : childResult;
        }
        case ast::Pat::Range: {
            auto from = patternBound(*parse[pattern.range.from], valueType(pivot));
            auto to = patternBound(*parse[pattern.range.to], valueType(pivot));
            if(!from && !to) return PatternResult::Always;

            ModulePtr<Value> condition = nullptr;

            if(from) {
                ModulePtr<Value> args[] = { pivot, from };
                condition = emitCall(Context::nameHash(">=", 2), { args, 2 }, pattern.source, module.scalar.bool_);
            }

            if(to) {
                ModulePtr<Value> args[] = { pivot, to };
                auto upper = emitCall(Context::nameHash("<=", 2), { args, 2 }, pattern.source, module.scalar.bool_);

                if(condition) {
                    ModulePtr<Value> both[] = { condition, upper };
                    condition = emitCall(Context::nameHash("and", 3), { both, 2 }, pattern.source, module.scalar.bool_);
                } else {
                    condition = upper;
                }
            }

            return branchPattern(condition, onFail, pattern.source);
        }
        case ast::Pat::Arr:
        case ast::Pat::Rest:
            context.diagnostics.error("array patterns are deferred until arrays are represented in resolve IR"_v, pattern.source);
            return PatternResult::Never;
        default:
            break;
    }

    if(pattern.kind >= ast::Pat::Lit) {
        ModulePtr<Value> args[] = { pivot, patternBound(pattern, valueType(pivot)) };
        return branchPattern(emitCall(Context::nameHash("==", 2), { args, 2 }, pattern.source, module.scalar.bool_), onFail, pattern.source);
    }

    return PatternResult::Never;
}

ModulePtr<Value> ExprResolver::resolveMatch(const ast::Expr& expr, const ast::MatchExpr& match, TypePtr target, bool used) {
    auto pivot = resolve(match.pivot);
    if(!pivot) return nullptr;

    auto alternativeList = match.alts;
    auto alternatives = alternativeList.contents(parse);
    if(alternatives.size() == 0) {
        context.diagnostics.error("match requires at least one alternative"_v, expr.source);
        return nullptr;
    }

    RecordCoverage coverage;
    if(global[valueType(pivot)]->kind == Type::Record) {
        coverage.type = valueType(pivot);

        if(((RecordType*)global[coverage.type])->constructors.size() > 64) {
            context.diagnostics.error("temporary exhaustiveness tracking supports at most 64 constructors"_v, expr.source);
        }
    }

    auto bindingCount = bindings.size();
    Array<BranchArm> arms;
    auto exhaustive = false;

    for(auto alternative: alternatives) {
        // An alternative that must match needs no failure block, and getting that wrong in
        // either direction is a bug rather than a diagnostic - hence the cross-check below.
        auto expectedAlways = irrefutable(alternative.pat, valueType(pivot));

        if(!expectedAlways && coverage.type && alternative.pat.kind == ast::Pat::Con) {
            auto found = findConstructor(module, alternative.pat.con.name, alternative.pat.source);
            auto record = (RecordType*)global[coverage.type];

            // A constructor from another type leaves this false; resolvePattern reports it.
            if(found && record->base(global) == found.unwrap().record) {
                auto reference = found.unwrap();
                auto constructor = record->constructors.get(global, reference.index);
                auto childAlways = !alternative.pat.con.pats || irrefutable(*parse[alternative.pat.con.pats], constructor.content);
                auto already = (coverage.checked & (U64(1) << reference.index)) != 0;

                expectedAlways = childAlways && coverage.checkedCount + (already ? 0 : 1) == record->constructors.size();
            }
        }

        auto failure = expectedAlways ? ModulePtr<Block>(nullptr) : addBlock();
        auto patternResult = resolvePattern(alternative.pat, pivot, failure, coverage.type ? &coverage : nullptr);

        if(patternResult == PatternResult::Never) {
            bindings.resize(bindingCount);
            if(!failure) return nullptr;

            current = failure;
            continue;
        }

        auto value = resolve(alternative.expr, target, used);
        if(current) arms.push(BranchArm { current, value, alternative.expr.source });
        bindings.resize(bindingCount);

        if(patternResult == PatternResult::Always) {
            exhaustive = true;
            current = nullptr;
            break;
        }

        if(!failure) {
            context.diagnostics.error("internal pattern exhaustiveness mismatch"_v, alternative.pat.source);
            return nullptr;
        }

        current = failure;
    }

    if(!exhaustive) {
        context.diagnostics.error("match is not exhaustive"_v, expr.source);
        return nullptr;
    }

    return finishBranches(arms, expr.source, used);
}
