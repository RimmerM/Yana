#include "resolve_internal.h"
#include "print.h"

#include "../compiler/replace.h"

namespace mir {

/*
 * Replacements - Design-Replacements.md, and `doc/spec/declarations.md`'s *Replacement* section.
 *
 * A baseline is a plain function. A replacement is another body of it, written `@replaces fn` with
 * the same name, arity and signature in the same module, chosen where its condition holds. The
 * condition is two things decided at two times: the replacement's **selectors**, which decided
 * whether it was read at all, and its **extra constraints**, which are asked at each call whose type
 * arguments are concrete - silently, because a constraint that fails there means the baseline runs
 * rather than that the program is wrong.
 *
 * What this file owns is the link (`linkReplacements`, once every signature is in) and the choice
 * (`chooseReplacement`, at a call). Nothing between them knows a replacement exists: its body is
 * resolved like any generic function's, and a body nothing chose is dropped by the reachability walk
 * like any function nothing calls.
 */

static bool writesAttribute(Context& context, ast::ParseBase parse, ast::Decl& declaration,
                            DeclAttribute which) {
    for(auto attribute: declaration.attributes.contents(parse)) {
        if(findAttribute(stringView(context.findName(attribute.name))) == which) return true;
    }

    return false;
}

// The interned text of a name, which lives as long as the compilation - what a `SelectorLiteral`
// holds, and why it is not `findName`'s temporary copy.
static StringView internedText(Context& context, StringId name) {
    auto& identifier = context.find(name);
    return StringView { identifier.text, identifier.textLength };
}

/*
 * A declaration's selectors as a formula - its `@platform` and `@allocator` attributes, one clause
 * each, and the segments of the file it is written in.
 *
 * The spellings were judged by `checkPlatform` before anything was read, so a name that is not a
 * bare variable is skipped here rather than reported a second time.
 */
static void declarationSelectors(Context& context, ast::ParseBase parse, ast::Decl& declaration,
                                 StringId fileSelectors, SelectorCondition& into) {
    for(auto attribute: declaration.attributes.contents(parse)) {
        auto which = findAttribute(stringView(context.findName(attribute.name)));
        if(which != DeclAttribute::Platform && which != DeclAttribute::Allocator) continue;
        if(attribute.args.isEmpty()) continue;

        auto clause = into.addClause();
        auto axis = which == DeclAttribute::Platform ? SelectorAxis::Target : SelectorAxis::Allocator;

        for(auto argument: attribute.args.contents(parse)) {
            if(argument.value.kind != ast::Expr::Var) continue;
            into.add(clause, internedText(context, argument.value.var), axis);
        }
    }

    if(fileSelectors) appendFileSelectors(into, internedText(context, fileSelectors));
}

// A condition's selectors as a reader writes them: clauses joined by `and`, alternatives by `or`.
static StringId selectorText(Context& context, const SelectorCondition& condition) {
    if(condition.isEmpty()) return StringId {};

    StringBuilder text;
    for(U16 clause = 0; clause < condition.clauses; clause++) {
        if(clause) text.append(" and "_v);

        auto first = true;
        for(auto& literal: condition.literals) {
            if(literal.clause != clause) continue;
            if(!first) text.append(" or "_v);

            if(literal.axis == SelectorAxis::Allocator) text.append("allocator "_v);
            text.append(literal.name);
            first = false;
        }
    }

    return context.addQualifiedName(text.pointer(), text.size(), 1);
}

ast::ModuleGroup* Resolver::groupOf(DeclPtr<Module> module) {
    for(Size i = 0; i < program.modules.size(); i++) {
        if(program.modules[i] == module) return groups[i];
    }

    assertTrue(false);
    return nullptr;
}

// By address: every declaration of a file is one contiguous run of the parse region.
U16 Resolver::fileHolding(DeclPtr<Module> module, const ast::Decl* declaration) {
    auto group = groupOf(module);

    for(U16 i = 0; i < U16(group->files.size()); i++) {
        auto& decls = group->files[i]->decls;
        auto first = decls.pointer(parse);

        if(declaration >= first && declaration < first + decls.size()) return i;
    }

    return 0;
}

void Resolver::declareReplacement(DeclPtr<Function> function, ast::Decl& declaration) {
    pendingReplacements.push(PendingReplacement {
        function, &declaration, here(), current->activeFile
    });
}

/*
 * One replacement's signature against its baseline's - §3.2.
 *
 * The variables are matched **by name**, and that is the whole of what `variables` records: a
 * replacement declares exactly the baseline's, and its numbering may differ because an extra
 * constraint written first introduces a variable earlier. Everything after that is read under the
 * substitution from the replacement's variables to the baseline's, so "the same type" is `sameType`
 * and nothing looser.
 *
 * `extra` receives the constraints the replacement states beyond the baseline's, in the baseline's
 * variables, which is what the ordering compares between two replacements.
 */
bool Resolver::checkReplacementSignature(Function& baseline, Function& replacement,
                                         ast::Decl& written, SmallArray<U8, 4>& variables,
                                         SmallArray<GenRequirement, 2>& extra) {
    auto source = written.source;
    auto name = context.findName(baseline.name);

    auto mismatch = [&](StringView what) {
        context.diagnostics.error("this replacement of %@ %@ - a replacement declares exactly its baseline's signature, and adds only constraints"_v,
                                  source, name, what);
        return false;
    };

    auto baseVariables = isGeneric(decl(), baseline.gen) ? decl()[baseline.gen]->variables.size() : 0;
    auto ownVariables = isGeneric(decl(), replacement.gen) ? decl()[replacement.gen]->variables.size() : 0;

    if(baseVariables != ownVariables) return mismatch("has different type variables"_v);

    SmallArray<DeclPtr<Type>, 4> mapped;

    for(U32 i = 0; i < ownVariables; i++) {
        auto own = decl()[replacement.gen]->variables.get(decl(), i);
        auto ownName = ((VariableType*)decl()[own])->name;
        auto found = false;

        for(U32 j = 0; j < baseVariables && !found; j++) {
            auto base = decl()[baseline.gen]->variables.get(decl(), j);
            if(((VariableType*)decl()[base])->name != ownName) continue;

            variables.push(U8(j));
            mapped.push(base);
            found = true;
        }

        if(!found) {
            context.diagnostics.error("this replacement of %@ declares the type variable %@, which its baseline does not - a replacement names the baseline's variables and adds none"_v,
                                      source, name, context.findName(ownName));
            return false;
        }
    }

    Substitution with { replacement.gen, mapped.pointer(), mapped.size() };
    auto same = [&](DeclPtr<Type> own, DeclPtr<Type> base) {
        return sameType(ownVariables ? substituteType(context, program, own, with) : own, base);
    };

    if(replacement.writtenArity() != baseline.writtenArity()) return mismatch("takes a different number of arguments"_v);

    for(U32 i = 0; i < baseline.writtenArity(); i++) {
        auto& own = *decl()[replacement.args.get(decl(), i)];
        auto& base = *decl()[baseline.args.get(decl(), i)];

        // By name, because a named argument at a call binds against the baseline's names.
        if(own.name != base.name) return mismatch("names its parameters differently"_v);
        if(!same(own.occurrence.type, base.occurrence.type)) return mismatch("has a parameter of a different type"_v);
        if(own.occurrence.contract != base.occurrence.contract || own.lazy != base.lazy) {
            return mismatch("passes a parameter differently"_v);
        }
    }

    // Defaults belong to the function rather than to a body, so they are written once - on the
    // baseline - and a replacement that wrote one would be a second answer for a call to disagree on.
    for(auto argument: written.fun.args.contents(parse)) {
        if(!argument.def && !argument.caller) continue;

        context.diagnostics.error("a replacement writes no defaults - what a call may leave out belongs to %@ itself, so it is written once, on the baseline"_v,
                                  argument.source, name);
        return false;
    }

    if(replacement.lens != baseline.lens || replacement.iterator != baseline.iterator) {
        return mismatch("is a different kind of function"_v);
    }

    if(!same(replacement.result.type, baseline.result.type) ||
       replacement.result.contract != baseline.result.contract) {
        return mismatch("has a different result"_v);
    }

    /*
     * The constraints: every one of the baseline's, and anything beyond them is the condition.
     *
     * Only *class* constraints may be extra. A field or a callable requirement is a promise about a
     * caller's argument rather than about a target, and a condition made of one would choose a body by
     * the shape of a record - which is overloading, and R2 refuses it.
     */
    auto& own = *decl()[replacement.gen];
    auto& base = *decl()[baseline.gen];

    if(own.properties.size() != base.properties.size() || own.callables.size() != base.callables.size()) {
        return mismatch("states different field or callable requirements"_v);
    }

    SmallArray<bool, 4> matched;
    for(U32 i = 0; i < base.classes.size(); i++) matched.push(false);

    for(auto requirement: own.classes.contents(decl())) {
        SmallArray<DeclPtr<Type>, 4> arguments;
        for(auto argument: requirement.args.contents(decl())) {
            arguments.push(ownVariables ? substituteType(context, program, argument, with) : argument);
        }

        auto found = false;
        for(U32 i = 0; i < base.classes.size() && !found; i++) {
            auto held = base.classes.get(decl(), i);
            if(matched[i] || held.klass != requirement.klass || held.args.size() != arguments.size()) continue;

            auto agrees = true;
            for(U32 k = 0; k < arguments.size(); k++) agrees = agrees && held.args.get(decl(), k) == arguments[k];

            if(agrees) {
                matched[i] = true;
                found = true;
            }
        }

        if(found) continue;

        auto copy = requirement;
        copy.args = {};
        for(auto argument: arguments) copy.args.push(program.decl, argument);
        extra.push(copy);
    }

    for(U32 i = 0; i < base.classes.size(); i++) {
        if(matched[i]) continue;

        auto held = base.classes.get(decl(), i);
        context.diagnostics.error("this replacement of %@ leaves out its baseline's constraint %@ - a replacement states every one of them, and adds to them"_v,
                                  source, name, context.findName(decl()[held.klass]->name));
        return false;
    }

    return true;
}

/*
 * Every replacement, attached to its baseline and ordered - §3.1, §3.2 and §3.5.
 *
 * After every signature is in, deferred headers included, because what is checked is signatures. The
 * replacements of one baseline are gathered in declaration order, checked one by one, and then
 * ordered together by `orderReplacements`, which is shared with the shipping stage.
 */
void Resolver::linkReplacements() {
    struct Linked {
        DeclPtr<Function> baseline;
        Size pending;
        SelectorCondition selectors;
        SmallArray<U8, 4> variables;
        SmallArray<GenRequirement, 2> extra;
    };

    Array<Linked> linked;

    for(Size index = 0; index < pendingReplacements.size(); index++) {
        auto& pending = pendingReplacements[index];
        auto& written = *pending.declaration;
        auto& replacement = *decl()[pending.function];
        auto name = context.findName(replacement.name);

        ModuleScope scope(*this, pending.module, pending.file);

        /*
         * What may not be one - §3.1. A replacement is reached only through its baseline, so it has no
         * name of its own to export, no role of its own to fill and no suite to join.
         */
        if(written.exported) {
            context.diagnostics.error("a replacement is not `pub` - it has no name of its own to export, and whether %@ is exported is its baseline's to say"_v,
                                      written.source, name);
            continue;
        }

        if(writesAttribute(context, parse, written, DeclAttribute::Builtin)) {
            context.diagnostics.error("a replacement fills no `@builtin` role - a role names one declaration, and a replacement is one body of another"_v,
                                      written.source);
            continue;
        }

        if(replacement.lens || replacement.iterator) {
            unsupported(written.source, "a replacement of a `lens fn` or an `iter fn`"_v);
            continue;
        }

        // Read off the declaration: `declareFunction` returns before it marks a replacement as
        // inferring, since a replacement is never one of the bodies whose result is waited for.
        if(written.fun.implicitReturn && !written.fun.ret) {
            context.diagnostics.error("a replacement states its result type - the signatures of %@'s bodies are compared before any body is read"_v,
                                      written.source, name);
            continue;
        }

        OverloadSet set;
        program.findOverloads(pending.module, replacement.name, replacement.writtenArity(), set);

        if(!set.plain) {
            context.diagnostics.error("%@ replaces nothing - a replacement is another body of the plain function of the same name taking %@ arguments in this module, and there is none"_v,
                                      written.source, name, U64(replacement.writtenArity()));
            continue;
        }

        auto& baseline = *decl()[set.plain];

        // A class member's R1 set holds no plain function, so reaching one here is a plain function
        // whose arity range happens to include this one's.
        if(baseline.writtenArity() != replacement.writtenArity()) {
            context.diagnostics.error("%@ replaces nothing - its baseline takes %@ arguments, and a replacement takes exactly as many"_v,
                                      written.source, name, U64(baseline.writtenArity()));
            continue;
        }

        if(baseline.intrinsic != intrinsic::Op::Count || baseline.inferResult) {
            context.diagnostics.error(baseline.inferResult
                ? "%@ cannot be replaced - its result is decided by its body, and a replacement's signature is compared with a stated one"_v
                : "%@ cannot be replaced - it fills a `@builtin` role, and a role's declaration has no body to replace"_v,
                written.source, name);
            continue;
        }

        auto& entry = *linked.push();
        entry.baseline = set.plain;
        entry.pending = index;

        if(!checkReplacementSignature(baseline, replacement, written, entry.variables, entry.extra)) {
            linked.pop();
            continue;
        }

        auto fileSelectors = groupOf(pending.module)->files[pending.file]->selectors;
        declarationSelectors(context, parse, written, fileSelectors, entry.selectors);
    }

    /*
     * One ordering per baseline, over the replacements this compilation read of it.
     *
     * The ambiguity check is over those, and that is narrower than §3.5's "for every target": a
     * replacement whose selectors failed was never declared, so its constraints were never resolved
     * and there is nothing to compare them with. Two replacements that are ambiguous are both read on
     * any build that satisfies both - which is the build on which the ambiguity would matter - and
     * that build reports it.
     */
    SmallArray<bool, 64> done;
    for(Size i = 0; i < linked.size(); i++) done.push(false);

    for(Size first = 0; first < linked.size(); first++) {
        if(done[first]) continue;

        SmallArray<Size, 4> members;
        for(Size i = first; i < linked.size(); i++) {
            if(linked[i].baseline != linked[first].baseline) continue;

            members.push(i);
            done[i] = true;
        }

        auto& baseline = *decl()[linked[first].baseline];

        SmallArray<SelectorCondition, 4> selectors;
        SmallArray<bool, 4> conditioned;
        for(auto member: members) {
            selectors.push(linked[member].selectors);
            conditioned.push(linked[member].extra.size() != 0);
        }

        // The baseline's own selectors, read off its written declaration - found by position, since the
        // two written lists are joined that way and nothing on a `Function` names its declaration.
        SelectorCondition baselineSelectors;
        for(Size i = 0; i < writtenFunctions.size(); i++) {
            if(writtenFunctions[i] != linked[first].baseline || !writtenFunctionDecls[i]) continue;

            auto& declaration = *writtenFunctionDecls[i];
            auto file = groupOf(baseline.module)->files[fileHolding(baseline.module, &declaration)];
            declarationSelectors(context, parse, declaration, file->selectors, baselineSelectors);
            break;
        }

        // Whether `i`'s extra constraints entail every one of `j`'s, through superclasses.
        auto superset = [&](Size i, Size j) {
            auto& have = linked[members[i]].extra;
            auto& want = linked[members[j]].extra;

            for(auto& wanted: want) {
                auto proved = false;

                for(auto& held: have) {
                    proved = proved || classProves(context, program, held.klass, held.args.pointer(decl()),
                                                   held.args.size(), wanted.klass,
                                                   wanted.args.pointer(decl()), wanted.args.size());
                }

                if(!proved) return false;
            }

            return true;
        };

        auto report = [&](ReplacementProblem problem, Size i, Size j) {
            auto& later = *pendingReplacements[linked[members[j]].pending].declaration;
            auto name = context.findName(baseline.name);

            switch(problem) {
                case ReplacementProblem::Unconditioned:
                    context.diagnostics.error("this replacement of %@ has no condition of its own - no selector beyond its baseline's and no constraint beyond its signature's - so the baseline could never run. Delete the baseline and write this without `@replaces`"_v,
                                              later.source, name);
                    break;
                case ReplacementProblem::Duplicate:
                    context.diagnostics.error("this replacement of %@ has the same condition as one declared before it, so one of the two could never be chosen"_v,
                                              later.source, name);
                    break;
                case ReplacementProblem::Ambiguous:
                    context.diagnostics.error("this replacement of %@ and one declared before it can both hold, and neither is more specific - add one's condition to the other's"_v,
                                              later.source, name);
                    break;
            }
        };

        ReplacementConditions conditions { selectors.pointer(), conditioned.pointer(), members.size() };
        SmallArray<U32, 4> order;
        for(Size i = 0; i < members.size(); i++) order.push(0);

        orderReplacements(conditions, baselineSelectors, superset, report, order.pointer());

        // A new entry's storage is not constructed by the map - `Program::addFunctionName` writes one
        // in for the same reason.
        auto added = program.replacements.add(U32(linked[first].baseline.offset));
        if(!added.existed) new (added.value) Program::ReplacementSet();

        auto& set = *added.value;
        for(auto at: order) {
            auto& member = linked[members[at]];
            auto& candidate = *set.candidates.push();

            candidate.body = pendingReplacements[member.pending].function;
            for(auto variable: member.variables) candidate.variables.push(variable);
            candidate.conditioned = member.extra.size() != 0;
            candidate.selectors = selectorText(context, member.selectors);
        }
    }

    pendingReplacements.clear();
}

/*
 * Which body one call reaches - §3.4, at the point `finishCall` has bound and checked the call against
 * the baseline's signature.
 *
 * The walk is the ordered candidate list. An unconditioned candidate holds wherever it was read, so
 * the first one ends the walk; a conditioned one holds where the call's type arguments are concrete
 * and its whole context discharges **silently** - a constraint over a type this target cannot form,
 * `Num(Vec(String))`, is a constraint that failed, which is what the silence reads back. A call whose
 * bindings are still open, in a body generic in them, takes only an unconditioned candidate: what it
 * reaches is the generic form, and a conditioned candidate cannot be asked without a type.
 *
 * Null where the baseline runs, so the caller carries on to the call it would have built anyway.
 */
Node* Resolver::chooseReplacement(Builder& builder, Function& callee, SmallArray<Node*, 8>& arguments,
                                  TypeArguments& typeArguments, LocationId source) {
    if(context.settings.noReplacements) return nullptr;

    auto set = program.replacementsOf(DeclPtr<Function>(&callee - *program.decl));
    if(!set) return nullptr;

    auto with = typeArguments.with();
    auto concrete = true;
    for(Size i = 0; i < with.count; i++) {
        concrete = concrete && with.arguments[i] && isConcreteType(decl(), with.arguments[i]);
    }

    for(auto& candidate: set->candidates) {
        if(candidate.conditioned && !concrete) continue;

        auto& body = *decl()[candidate.body];
        TypeArguments mapped(*this, body.gen);

        for(Size i = 0; i < candidate.variables.size(); i++) {
            mapped.bind(decl()[body.gen]->variables.get(decl(), U32(i)), with.arguments[candidate.variables[i]]);
        }

        if(candidate.conditioned) {
            Diagnostics::Silence quiet(context.diagnostics);
            if(!dischargeRequirements(body, mapped, source) || quiet.reported()) continue;
        }

        return builder.addCall(source, body, arguments.pointer(), arguments.size(), mapped.with());
    }

    return nullptr;
}

/*
 * What a function **value** names - the unconditioned winner, or the baseline.
 *
 * A value has no type arguments to ask a condition with, which is §3.4's second paragraph: only a
 * replacement decided per compilation can stand in for the baseline everywhere, including here.
 */
DeclPtr<Function> Resolver::replacementForValue(DeclPtr<Function> baseline) {
    if(context.settings.noReplacements) return baseline;

    auto set = program.replacementsOf(baseline);
    if(!set) return baseline;

    for(auto& candidate: set->candidates) {
        if(!candidate.conditioned) return candidate.body;
    }

    return baseline;
}

} // namespace mir
