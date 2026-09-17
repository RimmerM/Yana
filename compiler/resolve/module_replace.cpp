#include "module_internal.h"
#include "expr.h"
#include "generic.h"
#include "name.h"
#include "../compiler/replace.h"
#include "../parse/ast.h"

/*
 * Replacements - Design-Replacements.md, the shipping stage's half.
 *
 * The same rules as `compiler/resolve_mir/resolve_replace.cpp`, and the part of them that could be
 * shared is: which name a selector is, what implies what and how a set is ordered are
 * `compiler/compiler/replace.h`, which both stages call. What is here is what this stage's
 * declarations look like - a replacement is an anonymous function nothing finds by name, linked to
 * its baseline once the module's signatures are in, and chosen where a call is made concrete: at a
 * concrete call site, and where a specialization clones a call a generic body deferred.
 */

// Where `decl` writes the attribute `name`, or `kNullLocation` where it does not.
static LocationId writtenAttribute(Module& module, ast::Decl& decl, StringId name) {
    for(auto attribute: decl.attributes.contents(module.parse)) {
        if(attribute.name == name) return attribute.source;
    }

    return kNullLocation;
}

static bool writesBuiltin(Module& module, ast::Decl& decl) {
    return writtenAttribute(module, decl, module.context.addUnqualifiedName(intrinsic::kBuiltinAttribute)) != kNullLocation;
}

bool writesReplaces(Module& module, ast::Decl& decl) {
    return writtenAttribute(module, decl, module.context.addUnqualifiedName("replaces", 8)) != kNullLocation;
}

void refuseMemberReplacement(Module& module, ast::Decl& member) {
    auto source = writtenAttribute(module, member, module.context.addUnqualifiedName("replaces", 8));
    if(source == kNullLocation) return;

    module.context.diagnostics.error("`@replaces` is for a plain function - a class or instance member is chosen by selection, so give it a body that calls a function, and replace that"_v,
                                     source);
}

/*
 * The name a replacement is emitted under - `replaces$total`, and `replaces$total$2` for a second.
 *
 * A name of its own because everything below the resolver keys a function by one, and a replacement
 * is compiled beside the baseline it replaces. It is never looked up: the function is anonymous.
 */
StringId replacementName(Module& module, StringId baseline) {
    U32 before = 0;
    for(auto& pending: module.pendingReplacements) {
        before += module.parse[pending.declaration]->fun.name == baseline ? 1 : 0;
    }

    StringBuilder text;
    text << "replaces$" << module.context.findName(baseline);

    if(before) {
        char digits[24];
        auto length = Tritium::format(toBuffer(digits), toString("$%@"_v), U64(before + 1));
        text.append(digits, length);
    }

    return module.context.addQualifiedName(text.pointer(), text.size(), 1);
}

void declareReplacement(Module& module, Function& function, ast::ParsePtr<ast::Decl> declaration) {
    module.pendingReplacements.push(Module::PendingReplacement {
        ModulePtr<Function>(&function - *module.arena), declaration, module.activeFile
    });
}

static StringView internedText(Context& context, StringId name) {
    auto& identifier = context.find(name);
    return StringView { identifier.text, identifier.textLength };
}

// The declaration's `@platform` and `@allocator` clauses and its file's segments - see the same
// function in the other stage.
static void declarationSelectors(Module& module, ast::Decl& decl, StringId fileSelectors,
                                 SelectorCondition& into) {
    auto& context = module.context;
    auto platform = context.addUnqualifiedName("platform", 8);
    auto allocator = context.addUnqualifiedName("allocator", 9);

    for(auto attribute: decl.attributes.contents(module.parse)) {
        if(attribute.name != platform && attribute.name != allocator) continue;
        if(attribute.args.isEmpty()) continue;

        auto clause = into.addClause();
        auto axis = attribute.name == platform ? SelectorAxis::Target : SelectorAxis::Allocator;

        for(auto argument: attribute.args.contents(module.parse)) {
            if(argument.value.kind != ast::Expr::Var) continue;
            into.add(clause, internedText(context, argument.value.var), axis);
        }
    }

    if(fileSelectors) appendFileSelectors(into, internedText(context, fileSelectors));
}

// Which of the module's files a declaration was written in - every file's declarations are one run
// of the parse region.
static U16 fileHolding(Module& module, ast::ParsePtr<ast::Decl> declaration) {
    auto pointer = module.parse[declaration];

    for(U16 i = 0; i < U16(module.files.size()); i++) {
        auto& decls = module.files[i]->decls;
        auto first = decls.pointer(module.parse);

        if(pointer >= first && pointer < first + decls.size()) return i;
    }

    return 0;
}

/*
 * One replacement's signature against its baseline's - §3.2, on the terms the other stage states.
 */
static bool checkSignature(Module& module, Function& baseline, Function& replacement, ast::Decl& written,
                           SmallArray<U8, 4>& variables, SmallArray<ClassConstraint, 2>& extra) {
    auto& context = module.context;
    auto global = *module.types;
    auto local = *module.arena;
    auto source = written.source;
    auto name = context.findName(baseline.name);

    auto mismatch = [&](StringView what) {
        context.diagnostics.error("this replacement of %@ %@ - a replacement declares exactly its baseline's signature, and adds only constraints"_v,
                                  source, name, what);
        return false;
    };

    auto baseEnv = functionGen(global, baseline);
    auto ownEnv = functionGen(global, replacement);

    auto baseVariables = baseEnv ? baseEnv->types.size() : 0;
    auto ownVariables = ownEnv ? ownEnv->types.size() : 0;
    if(baseVariables != ownVariables) return mismatch("has different type variables"_v);

    TypeList mapped;

    for(Size i = 0; i < ownVariables; i++) {
        auto own = global[ownEnv->types.get(global, i)];
        auto found = false;

        for(Size j = 0; j < baseVariables && !found; j++) {
            auto base = baseEnv->types.get(global, j);
            if(global[base]->name != own->name) continue;

            variables.push(U8(j));
            mapped.push(TypePtr(base));
            found = true;
        }

        if(!found) {
            context.diagnostics.error("this replacement of %@ declares the type variable %@, which its baseline does not - a replacement names the baseline's variables and adds none"_v,
                                      source, name, context.findName(own->name));
            return false;
        }
    }

    auto substituted = [&](TypePtr type) {
        return ownVariables ? substituteType(module, type, toBuffer(mapped), source) : type;
    };

    if(replacement.args.size() != baseline.args.size()) return mismatch("takes a different number of arguments"_v);

    for(Size i = 0; i < baseline.args.size(); i++) {
        auto own = local[replacement.args.get(local, i)];
        auto base = local[baseline.args.get(local, i)];

        if(own->name != base->name) return mismatch("names its parameters differently"_v);
        if(!sameType(substituted(own->declaredType()), base->declaredType())) {
            return mismatch("has a parameter of a different type"_v);
        }

        if(own->convention != base->convention || own->loan != base->loan || own->isLazy() != base->isLazy()) {
            return mismatch("passes a parameter differently"_v);
        }
    }

    for(auto argument: written.fun.args.contents(module.parse)) {
        if(!argument.def && !argument.caller) continue;

        context.diagnostics.error("a replacement writes no defaults - what a call may leave out belongs to %@ itself, so it is written once, on the baseline"_v,
                                  argument.source, name);
        return false;
    }

    if(replacement.funKind != baseline.funKind) return mismatch("is a different kind of function"_v);

    if(!sameType(substituted(replacement.returnType), baseline.returnType)) {
        return mismatch("has a different result"_v);
    }

    if(!ownVariables) return true;

    if(ownEnv->properties.size() != baseEnv->properties.size() ||
       ownEnv->functions.size() != baseEnv->functions.size()) {
        return mismatch("states different field or callable requirements"_v);
    }

    SmallArray<bool, 4> matched;
    for(Size i = 0; i < baseEnv->classes.size(); i++) matched.push(false);

    for(auto constraint: ownEnv->classes.contents(global)) {
        if(!constraint.typeClass) continue;

        TypeList arguments;
        for(auto argument: constraint.args.contents(global)) arguments.push(substituted(argument));

        auto found = false;
        for(Size i = 0; i < baseEnv->classes.size() && !found; i++) {
            auto held = baseEnv->classes.get(global, i);
            if(matched[i] || held.typeClass != constraint.typeClass) continue;
            if(!sameTypes(held.args, global, toBuffer(arguments))) continue;

            matched[i] = true;
            found = true;
        }

        if(found) continue;

        ClassConstraint copy;
        copy.typeClass = constraint.typeClass;
        copy.source = constraint.source;
        for(auto argument: arguments) copy.args.push(module.types, argument);
        extra.push(copy);
    }

    for(Size i = 0; i < baseEnv->classes.size(); i++) {
        auto held = baseEnv->classes.get(global, i);
        if(matched[i] || !held.typeClass) continue;

        context.diagnostics.error("this replacement of %@ leaves out its baseline's constraint %@ - a replacement states every one of them, and adds to them"_v,
                                  source, name, context.findName(global[held.typeClass]->name));
        return false;
    }

    return true;
}

void linkReplacements(Module& module) {
    auto& context = module.context;
    auto global = *module.types;
    auto local = *module.arena;

    struct Linked {
        ModulePtr<Function> baseline;
        Size pending;
        SelectorCondition selectors;
        SmallArray<U8, 4> variables;
        SmallArray<ClassConstraint, 2> extra;
    };

    Array<Linked> linked;

    for(Size index = 0; index < module.pendingReplacements.size(); index++) {
        auto& pending = module.pendingReplacements[index];
        auto& written = *module.parse[pending.declaration];
        auto& replacement = *local[pending.function];
        auto name = context.findName(written.fun.name);

        FileScope scope(module, pending.file);

        if(written.exported) {
            context.diagnostics.error("a replacement is not `pub` - it has no name of its own to export, and whether %@ is exported is its baseline's to say"_v,
                                      written.source, name);
            continue;
        }

        if(writesBuiltin(module, written)) {
            context.diagnostics.error("a replacement fills no `@builtin` role - a role names one declaration, and a replacement is one body of another"_v,
                                      written.source);
            continue;
        }

        if(replacement.funKind != ast::FunKind::Plain) {
            context.diagnostics.error("a replacement of a `lens fn` or an `iter fn` is not supported"_v, written.source);
            continue;
        }

        if(replacement.inferReturn) {
            context.diagnostics.error("a replacement states its result type - the signatures of %@'s bodies are compared before any body is read"_v,
                                      written.source, name);
            continue;
        }

        auto found = module.functions.get(written.fun.name);
        if(!found) {
            context.diagnostics.error("%@ replaces nothing - a replacement is another body of the plain function of the same name taking %@ arguments in this module, and there is none"_v,
                                      written.source, name, U64(replacement.args.size()));
            continue;
        }

        auto& baseline = *local[found.unwrap()];
        auto& baselineWritten = *module.parse[baseline.ast];

        if(baseline.args.size() != replacement.args.size()) {
            context.diagnostics.error("%@ replaces nothing - its baseline takes %@ arguments, and a replacement takes exactly as many"_v,
                                      written.source, name, U64(baseline.args.size()));
            continue;
        }

        if(baseline.inferReturn || writesBuiltin(module, baselineWritten)) {
            context.diagnostics.error(baseline.inferReturn
                ? "%@ cannot be replaced - its result is decided by its body, and a replacement's signature is compared with a stated one"_v
                : "%@ cannot be replaced - it fills a `@builtin` role, and a role's declaration has no body to replace"_v,
                written.source, name);
            continue;
        }

        auto& entry = *linked.push();
        entry.baseline = found.unwrap();
        entry.pending = index;

        if(!checkSignature(module, baseline, replacement, written, entry.variables, entry.extra)) {
            linked.pop();
            continue;
        }

        declarationSelectors(module, written, module.files[pending.file]->selectors, entry.selectors);
    }

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

        auto& baseline = *local[linked[first].baseline];

        SmallArray<SelectorCondition, 4> selectors;
        SmallArray<bool, 4> conditioned;
        for(auto member: members) {
            selectors.push(linked[member].selectors);
            conditioned.push(linked[member].extra.size() != 0);
        }

        SelectorCondition baselineSelectors;
        declarationSelectors(module, *module.parse[baseline.ast],
                             module.files[fileHolding(module, baseline.ast)]->selectors, baselineSelectors);

        auto superset = [&](Size i, Size j) {
            for(auto& wanted: linked[members[j]].extra) {
                auto proved = false;

                for(auto& held: linked[members[i]].extra) {
                    TypeList have, want;
                    for(auto argument: held.args.contents(global)) have.push(argument);
                    for(auto argument: wanted.args.contents(global)) want.push(argument);

                    proved = proved || classEntails(module, held.typeClass, toBuffer(have), wanted.typeClass,
                                                    toBuffer(want));
                }

                if(!proved) return false;
            }

            return true;
        };

        auto report = [&](ReplacementProblem problem, Size, Size j) {
            auto& later = *module.parse[module.pendingReplacements[linked[members[j]].pending].declaration];
            auto name = context.findName(later.fun.name);

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

        auto added = module.program.replacements.add(U32(linked[first].baseline));
        if(!added.existed) new (added.value) Program::ReplacementSet();

        for(auto at: order) {
            auto& member = linked[members[at]];
            auto& candidate = *added.value->candidates.push();

            candidate.body = module.pendingReplacements[member.pending].function;
            for(auto variable: member.variables) candidate.variables.push(variable);
            candidate.conditioned = member.extra.size() != 0;
        }
    }

    module.pendingReplacements.clear();
}

ModulePtr<Function> unconditionedReplacement(Module& module, ModulePtr<Function> baseline) {
    if(module.context.settings.noReplacements) return baseline;

    auto found = module.program.replacements.get(U32(baseline));
    if(!found) return baseline;

    for(auto& candidate: found.unwrap().candidates) {
        if(!candidate.conditioned) return candidate.body;
    }

    return baseline;
}

/*
 * Which body a concrete call reaches, and the bindings to reach it with - §3.4.
 *
 * `bindings` are the baseline's, all concrete; on a replacement they are rewritten into its own
 * variables. A conditioned candidate's body is resolved first and out loud - it is a body like any
 * other, and its mistakes are the program's - and only then is its context proved with the reports
 * swallowed, over every requirement the body collected as well as the ones its signature wrote. A
 * requirement over a type this target cannot form fails there by reporting, which is what the silence
 * reads back.
 */
ModulePtr<Function> chooseReplacement(Module& from, ModulePtr<Function> baseline, TypeList& bindings,
                                      LocationId source) {
    if(from.context.settings.noReplacements) return baseline;

    auto found = from.program.replacements.get(U32(baseline));
    if(!found) return baseline;

    auto global = *from.types;
    auto local = *from.arena;

    for(auto& candidate: found.unwrap().candidates) {
        TypeList mapped;
        for(auto variable: candidate.variables) mapped.push(bindings[variable]);

        if(candidate.conditioned) {
            auto& body = *local[candidate.body];
            if(!resolveFunctionBody(*body.module, body)) continue;

            auto env = functionGen(global, body);
            if(!env) continue;

            Diagnostics::Silence quiet(from.context.diagnostics);
            auto holds = true;

            for(auto constraint: env->classes.contents(global)) {
                if(!constraint.typeClass || !holds) continue;

                TypeList concrete;
                for(auto argument: constraint.args.contents(global)) {
                    concrete.push(substituteType(from, argument, toBuffer(mapped), source));
                }

                holds = !quiet.reported() && findInstance(from, constraint.typeClass, toBuffer(concrete));
            }

            if(!holds || quiet.reported()) continue;
        }

        bindings.clear();
        for(auto binding: mapped) bindings.push(binding);
        return candidate.body;
    }

    return baseline;
}
