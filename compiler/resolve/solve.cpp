#include "solve.h"
#include "generic.h"
#include "name.h"

/*
 * See solve.h for what a solve is and what is deliberately not in one. What follows is only how the
 * steps are performed.
 *
 * Nothing here reports a diagnostic, and that is a property rather than an omission: every overload
 * match runs a solve speculatively, so a solve that says no has to be free of consequences. What it
 * knows about the failure - which position, which variable - is in the answer, and the one caller
 * that is committed to this callee reads it back out and reports from there.
 */

Solver::Solver(ExprResolver& resolver, Solution& solution, GenEnv* declared):
    resolver(resolver), solution(solution),
    declared(declared ? declared - resolver.global : GlobalPtr<GenEnv>(nullptr)) {
    auto variables = declared ? declared->types.size() : 0;

    /*
     * The whole answer, and not only the bindings.
     *
     * A `Solution` is an output, so a solve starts from nothing whatever was in it - which used to
     * be true by construction, because every caller built one and handed it over once. It stopped
     * being true when a call site re-ran the solve over arguments it had learned more about (see
     * inferDeferredArguments): the bindings were cleared and filled correctly, and the stale
     * `Undecided` verdict from the first run stayed, because a settle only *sets* a verdict on a
     * solve that is still `Solved`.
     */
    solution.state = Solution::State::Solved;
    solution.position = 0;
    solution.undeclaredDependency = false;
    solution.instance = nullptr;
    solution.instanceArgs.clear();

    solution.types.clear();
    for(Size i = 0; i < variables; i++) solution.types.push(nullptr);
}

TypePtr Solver::declaredDefault(Size index) const {
    if(!declared) return nullptr;

    auto global = resolver.global;
    auto types = global[declared]->types;
    if(index >= types.size()) return nullptr;

    // The written form is spent on demand, and a signature reaching a settle is a demand: a caller
    // that omitted the argument is the first thing to ask what the declaration said it was.
    resolveGenDefaults(resolver.module, declared);
    return global[types.get(global, index)]->def;
}

/*
 * The two ways a *call's* positions may disagree about one variable and still mean one type.
 *
 * Neither is a weakening of what a variable means. A **literal** has not chosen a type yet, so a
 * position that wrote one is not a second opinion - it is the only opinion, and two literals merge
 * into one variable carrying both their classes, which is what leaves `1 + 2.5` a single question.
 * A **`@bits` refinement** and the type it refines are one type to everything that dispatches:
 * [repr.md](doc/spec/repr.md#bit-width-refinements) says *"`@bits(n)` is Repr-only: it never
 * participates in typeclass dispatch"*, and this is the half of that rule which is code. Without
 * it, `x == y` on a `@bits(2) U32` and a `U32` bound `a` twice to two types, found no common
 * `Widen` between them - widening a refinement to its base is structural rather than a conversion
 * anyone declared - and reported "no class function == accepts (@bits(2) U32, U32)" for an operator
 * the spec says is the plain `U32` one.
 *
 * The variable takes the **canonical** type, which is what makes the arguments converge: widening a
 * refinement to its base is free (see convertRefinement), and it is what a load of a packed field
 * already produces - repr.md's "load always widens". So the operation happens at the natural width,
 * as it does everywhere else, and only a store narrows again.
 *
 * **Not widening**, which is the line between this and the arm below it. `f(y: a, xs: [a])` handed
 * a `Long` and an array of `Int` is two positions that disagree about the element type of an array,
 * and there is no conversion that settles it - a whole argument may be widened into its parameter,
 * a type *inside* one may not. So this is the rule every position gets, at any depth, and widening
 * stays what only an outermost argument position does.
 */
static bool meetBinding(ExprResolver& resolver, TypePtr& bound, TypePtr concrete) {
    auto global = resolver.global;

    if(auto canonical = canonicalType(global, bound); canonical == canonicalType(global, concrete)) {
        bound = canonical;
        return true;
    }

    if(isLiteral(global, concrete)) {
        if(!isLiteral(global, bound)) return resolver.literalFits(concrete, bound);

        bound = resolver.mergeLiterals(bound, concrete);
        return true;
    }

    if(isLiteral(global, bound)) {
        if(!resolver.literalFits(bound, concrete)) return false;

        bound = concrete;
        return true;
    }

    return false;
}

// `meetBinding` as the hook `matchType` takes, so that every variable the structural walk reaches
// answers the same question the outermost one does - see MatchRebind.
static MatchRebind callRebind(ExprResolver& resolver) {
    return {
        [](void* context, TypePtr& bound, TypePtr concrete) {
            return meetBinding(*(ExprResolver*)context, bound, concrete);
        },
        &resolver,
    };
}

// One position, into a binding list that is not always the answer's - see bindResult.
static bool bindInto(ExprResolver& resolver, TypePtr pattern, TypePtr actual, TypeList& bindings, bool widen) {
    auto& module = resolver.module;
    auto global = resolver.global;

    if(!pattern || !actual) return false;

    // An owned container fits a `[T]` parameter, which is the conversion convertSlice performs -
    // so matching is against the slice it becomes, and a `Flat(a)` pattern binds `a` to the array's
    // element instead of failing outright. Argument direction only, for the reason the widening
    // rule below is: what a call *produces* has not decided anything by needing a conversion.
    if(widen && sliceElement(module, pattern)) {
        if(ownedElement(module, actual)) {
            actual = sliceOf(module, actual);
        } else if(auto element = contiguousElement(module, actual)) {
            /*
             * A container of the program's own, viewed as the slice its `Contiguous` instance
             * promises - Implementation-Containers.md §5. The conversion is a call to `elements` and
             * convert() emits it; what has to happen here is that `fn sum(xs: [a])` binds `a` at all,
             * since a generic parameter never reaches convert() without first having been matched.
             */
            actual = instantiateRecord(module, module.program.sliceType, { &element, 1 }, kNullLocation);
        }
    }

    if(global[pattern]->kind == Type::Gen) {
        auto index = ((GenType*)global[pattern])->index;
        if(index >= bindings.size()) return false;

        /*
         * A **borrow handed to a bare variable binds what it points at**, not the borrow.
         *
         * The same rule the concrete branch below already applies through `convertibleType`, said
         * here for the position that never reaches it: `fn f(x: String)` given a `'String` reads
         * through, and a signature must not mean something different because some other parameter
         * mentioned a variable. That is the sentence the paragraph below this one is about, and a
         * bare `a` was the one position still exempt from it.
         *
         * A reference is not a value of its own on this side of a call. Nothing takes one apart,
         * duplicates it or drops it - what those operations mean is what they mean for the storage
         * it names - so a variable bound to `'k` gives a callee a `k` it cannot do any of the three
         * to, and the failure is not at the binding. `copy(entry.key)` over a `pairs` yielding
         * `Entry('k, 'v)` bound `Copy`'s variable to `'k`, selected the blanket instance over
         * `TrivialCopy` because a reference is a pointer and a pointer is trivially copyable, and
         * duplicated the reference: `let ->duplicate = copy(k)` answered a second name for one
         * value. It was reported, when it was reported at all, as a borrow escaping the frame that
         * made it - which is true, and is a sentence about a line the program did not write.
         *
         * Argument direction only, on the same terms as everything else here. A *result* position
         * asks what a call produces, and `-> 'a` is a signature deliberately answering a reference -
         * `Map.find` and `unwrapOr` are both written that way. So is a variable reached at depth:
         * `Maybe('v)` is a carrier of references and a real type, and this is the outermost binding
         * rather than the structural walk (see matchType below, which nested positions go through).
         */
        if(widen && isBorrow(global, actual)) {
            if(auto to = ((BorrowType*)global[actual])->to) actual = to;
        }

        if(!bindings[index]) {
            bindings[index] = actual;
            return true;
        }

        if(bindings[index] == actual) return true;

        // The rules every position gets, outermost or not - see meetBinding, which is where the
        // reasoning for both of them lives.
        if(meetBinding(resolver, bindings[index], actual)) return true;

        /*
         * And the one an outermost argument position gets on top: a whole argument may be widened
         * into its parameter, because needing a conversion is part of fitting. `widen` is false for
         * a result, and false at every depth - see meetBinding's last paragraph.
         */
        if(!widen) return false;

        auto common = resolver.commonWiden(bindings[index], actual);
        if(!common) return false;

        bindings[index] = common;
        return true;
    }

    // A literal against a written type takes that type outright, since there is nothing below the
    // outermost type of a literal for matchType to walk into.
    if(isLiteral(global, actual)) return !isGeneric(global, pattern) && resolver.literalFits(actual, pattern);

    /*
     * A concrete position in an otherwise generic signature is judged the way a non-generic
     * function's is: there is no variable here to bind, so what matters is whether the argument
     * converts. Without this, `fn push(&self: Array(a), index: Int)` would reject the `Short` that
     * the identical non-generic signature accepts, and a signature would mean different things
     * depending on whether some *other* parameter mentioned a type variable.
     *
     * Literally the same question, through `convertibleType`, rather than a second approximation of
     * it: this one used to ask only for a `Widen` instance, so the same position rejected a borrow,
     * an `@bits` refinement and an error type that the non-generic path accepts - and a signature
     * still meant two different things depending on a variable somewhere else in it.
     *
     * Only in the argument direction. The result position asks what the call produces, and a
     * result that would need converting has not decided the type arguments by itself.
     */
    if(widen && !isGeneric(global, pattern) && !sameType(pattern, actual)) {
        return resolver.convertibleType(actual, pattern);
    }

    return matchType(global, pattern, actual, { bindings.pointer(), bindings.size() },
                     callRebind(resolver));
}

bool Solver::bind(TypePtr pattern, TypePtr actual, bool widen) {
    return bindInto(resolver, pattern, actual, solution.types, widen);
}

/*
 * What a deferred position produces, where that is already known - the result type of the thunk it
 * holds, and null for a promise that is still an unresolved expression.
 *
 * A `value` promise is deliberately not read: it is a position that was evaluated before anything
 * knew it was lazy, and by the time one exists the callee has been chosen and there is no solve
 * left to inform.
 */
static TypePtr deferredResult(ExprResolver& resolver, const Deferred& promise) {
    if(!promise.thunk) return nullptr;

    auto global = resolver.global;
    auto type = resolver.valueType(promise.thunk);
    if(global[type]->kind != Type::Fun) return nullptr;

    return ((FunType*)global[type])->result;
}

void Solver::bindArguments(ModulePtr<Function> signature, Buffer<ResolvedArg> args, Unresolved unresolved) {
    auto local = resolver.local;
    auto declaration = local[signature];

    for(Size i = 0; i < args.length && i < declaration->args.size(); i++) {
        /*
         * A deferred position has no type to match with: the argument has not been resolved, and
         * resolving it to find out would evaluate it. It is therefore skipped, and the parameter
         * type the other positions decided is what it is later resolved *against* - which is the
         * same one-directional rule the rest of selection follows, applied to an argument that
         * binds nothing rather than to one that binds a literal.
         *
         * Unless the thunk already exists, which is the one way a deferred position has a type
         * without anything having been evaluated: the closure was built, so what the argument
         * produces is written on it, and it is the parameter's *promised* type - `declaredType()`,
         * not the thunk the parameter actually receives - that the closure's result answers. Only
         * `emitGenericCall` ever arrives here that way, and only for a variable no strict position
         * mentions, so no overload match sees this and nothing about selection changes. See
         * deferredOnlyVariable.
         */
        if(args[i].isDeferred()) {
            auto promised = deferredResult(resolver, args[i].promise);
            if(!promised) continue;

            auto declared = local[declaration->args.get(local, i)]->declaredType();
            if(bind(declared, promised, true)) continue;

            if(solution.state == Solution::State::Solved) {
                solution.state = Solution::State::Argument;
                solution.position = i;
            }

            if(unresolved != Unresolved::Skips) return;
            continue;
        }

        /*
         * A defaulted position binds nothing, for a reason of its own: what fills it is a constant
         * of the parameter's *declared* type, so matching it against that type is an identity - and
         * a parameter declared as a type variable cannot have a default at all, which is the rule
         * `resolveArgumentDefault` states from the declaration's side.
         */
        if(args[i].isDefault()) continue;

        if(args[i].isFailed() && unresolved != Unresolved::Binds) {
            if(unresolved == Unresolved::Skips) continue;

            solution.state = Solution::State::Argument;
            solution.position = i;
            return;
        }

        auto declared = local[declaration->args.get(local, i)]->declaredType();
        if(bind(declared, resolver.valueType(args[i].value), true)) continue;

        // The *first* position that did not fit is the one worth naming, so a mode that keeps
        // binding past it does not overwrite what it found.
        if(solution.state == Solution::State::Solved) {
            solution.state = Solution::State::Argument;
            solution.position = i;
        }

        // A caller that rejects the whole solve has no use for the positions after this one; the
        // one that is inferring a shape from whatever fit does. See Unresolved.
        if(unresolved != Unresolved::Skips) return;
    }
}

void Solver::bindResult(TypePtr declared, TypePtr target) {
    if(!target || !declared) return;

    TypeList withTarget = solution.types;
    if(!bindInto(resolver, declared, target, withTarget, false)) return;

    for(Size i = 0; i < solution.types.size(); i++) {
        if(!solution.types[i] || isLiteral(resolver.global, solution.types[i])) {
            solution.types[i] = withTarget[i];
        }
    }
}

bool Solver::settle(Size from, Size limit) {
    for(Size i = from; i < limit && i < solution.types.size(); i++) {
        solution.types[i] = resolver.settleType(solution.types[i]);
        if(solution.types[i]) continue;

        // Nothing decided this position and no literal default answered it either, so the
        // declaration's own default is the last thing asked - see declaredDefault.
        solution.types[i] = declaredDefault(i);
        if(solution.types[i]) continue;

        if(solution.state == Solution::State::Solved) {
            solution.state = Solution::State::Undecided;
            solution.position = i;
        }

        return false;
    }

    return true;
}

U64 Solver::settleOpen(GenEnv& env) {
    auto global = resolver.global;
    U64 open = 0;

    for(Size i = 0; i < solution.types.size(); i++) {
        if(solution.types[i]) {
            solution.types[i] = resolver.settleType(solution.types[i]);
            continue;
        }

        if(i < 64) open |= U64(1) << i;
        solution.types[i] = (Type*)global[env.types.get(global, i)] - global;
    }

    return open;
}

bool Solver::anyOpen(Size from) const {
    for(Size i = from; i < solution.types.size(); i++) {
        if(!solution.types[i]) return true;
    }

    return false;
}

void Solver::settleDependencies(GenEnv& env, LocationId source) {
    auto global = resolver.global;

    /*
     * Which variables a dependency could answer, and which one has to be decided before it can.
     *
     * A variable in a *determined* position is one the instance answers; a variable in a *deciding*
     * position is one the lookup needs in hand. A variable in both - which a constraint like
     * `Elem(c, Wrap(c))` would produce - is treated as deciding, because a lookup that cannot
     * happen answers nothing at all.
     */
    U64 answerable = 0;
    U64 decides = 0;

    for(auto constraint: env.classes.contents(global)) {
        auto typeClass = constraint.typeClass;
        if(!typeClass || !global[typeClass]->determines()) continue;

        Size index = 0;
        for(auto arg: constraint.args.contents(global)) {
            genVariablesIn(global, arg, index < global[typeClass]->determined ? decides : answerable);
            index++;
        }
    }

    auto& bindings = solution.types;

    SmallArray<bool, 8> defaulted;
    for(Size i = 0; i < bindings.size(); i++) {
        auto overridable = i < 64 && (answerable & (U64(1) << i)) && !(decides & (U64(1) << i));
        defaulted.push(overridable && bindings[i] && isLiteral(global, bindings[i]));
    }

    for(Size i = 0; i < bindings.size(); i++) {
        if(bindings[i]) bindings[i] = resolver.settleType(bindings[i]);
    }

    TypeList asked = bindings;
    for(Size i = 0; i < asked.size(); i++) {
        if(defaulted[i]) asked[i] = nullptr;
    }

    fillDetermined(resolver.module, env, asked, source, functionGen(global, resolver.function));

    for(Size i = 0; i < bindings.size(); i++) {
        if(asked[i]) bindings[i] = asked[i];
    }
}

Determined fillDependency(Module& module, Function& function, GlobalPtr<TypeClass> typeClass,
                          TypeList& args, InstanceMatch& instance, bool bindGeneric) {
    auto global = *module.types;
    if(!typeClass) return Determined::Nothing;

    auto declaration = global[typeClass];
    auto determined = declaration->determines() ? Size(declaration->determined) : args.size();

    /*
     * Whether there is anything to look an instance up by.
     *
     * A *bare* type variable in a deciding position selects nothing, and this is the rule rather
     * than an optimization: a blanket `instance Elem(x -> x)` would otherwise answer for the `c` of
     * `fn (Elem(c, a)) f(self: c)` and commit the body to it, ignoring the instance the caller's
     * actual type has. `bindGeneric` does not lift it - a caller asking what an instance looks like
     * is asking about `Maybe(a)`, whose head is `Maybe` and whose answer is the same whatever `a`
     * turns out to be. resolveDetermined states the same rule from the other side; it is repeated
     * here because the shape-asking caller is exactly the one that switches it off there.
     */
    auto decidable = determined <= args.size();

    for(Size i = 0; i < determined && decidable; i++) {
        if(!args[i] || global[args[i]]->kind == Type::Gen) decidable = false;
    }

    if(decidable) {
        if(auto match = resolveDetermined(module, typeClass, args, bindGeneric)) {
            instance.instance = match.instance;
            replaceContents(instance.args, match.args);
            return Determined::Instance;
        }
    }

    /*
     * Inside a generic body there is no instance to read the determined positions off: `c` is this
     * function's own type variable and which container it will be is the caller's business. What
     * answers instead is the requirement the signature declared, which already gave the determined
     * position a name - the `a` of `fn (Contiguous(c, a)) first(self: c)`.
     *
     * Only the positions this caller left open are filled. The deciding ones are what the
     * requirement was matched on and are already what it says, and a determined one the caller
     * decided is an ascription that has to keep meaning what it said.
     */
    auto env = functionGen(global, function);
    if(!env) return Determined::Nothing;

    TypeList declared;

    if(findClassRequirement(module, *env, typeClass, toBuffer(args), declared)) {
        for(Size i = determined; i < args.size() && i < declared.size(); i++) {
            if(!args[i]) args[i] = declared[i];
        }

        return Determined::Requirement;
    }

    /*
     * Undeclared is deliberately not inferred. Recording `Contiguous(c, ?)` would mean inventing a
     * variable for the body, which is one more thing every caller has to satisfy without the author
     * having written it; the constraint has to be declared, and the caller's diagnostic says so.
     */
    return declaration->determines() ? Determined::Undeclared : Determined::Nothing;
}

void solveSignature(ExprResolver& resolver, ModulePtr<Function> callee, Buffer<ResolvedArg> args,
                    TypePtr target, LocationId source, Unresolved unresolved, Solution& out) {
    auto declaration = resolver.local[callee];
    auto env = functionGen(resolver.global, *declaration);

    Solver solver(resolver, out, env);
    if(!env) return;

    solver.bindArguments(callee, args, unresolved);
    if(!out.fits()) return;

    solver.bindResult(declaration->returnType, target);

    /*
     * A variable this signature's constraints determine is inferred from them rather than from the
     * call, so `fn (Contiguous(c, a)) sum(xs: c) -> a` fits a call that mentions only `c`.
     *
     * After the target, because a functional dependency is a promise about the instances and the
     * expected type is a wish about this one call. Where the two disagree, the instance wins and the
     * conversion the target wanted is reported where it fails, rather than silently selecting an
     * instance the dependency says does not serve these types.
     */
    solver.settleDependencies(*env, source);

    // A specialization is made for concrete types, so a literal variable the call left open settles
    // to its default before it becomes one of them.
    solver.settle();
}

void solveClassFun(ExprResolver& resolver, GlobalPtr<TypeClass> typeClass, ModulePtr<Function> signature,
                   Buffer<ResolvedArg> args, TypePtr target, Solution& out) {
    auto global = resolver.global;
    auto declaration = global[typeClass];
    auto env = global[declaration->gen];

    Solver solver(resolver, out, env);

    solver.bindArguments(signature, args, Unresolved::Rejects);
    if(!out.fits()) return;

    // The expected result only fills in what the arguments left open, so an ascription can pick an
    // instance but cannot re-pick one the arguments already determined.
    solver.bindResult(resolver.local[signature]->returnType, target);

    /*
     * A class's type argument has to be a real type before an instance can be looked for, so a
     * literal variable that no position decided takes its class's default here. The end of the
     * statement is the outer boundary for that; a call that needs an instance is the inner one, and
     * it is the one that comes first.
     */
    auto determined = declaration->determines() ? Size(declaration->determined) : out.types.size();
    if(!solver.settle(0, determined)) return;

    /*
     * `c` decides `a`, so a call that bound only `c` reads `a` off the instance rather than failing
     * to infer it. The instance that answered is kept: looking one up again below with the
     * now-complete arguments would find the same one, and this way the search happens once.
     */
    if(solver.anyOpen(determined)) {
        InstanceMatch match;

        switch(fillDependency(resolver.module, resolver.function, typeClass, out.types, match)) {
            case Determined::Instance:
                out.instance = match.instance;
                replaceContents(out.instanceArgs, match.args);
                break;
            case Determined::Undeclared:
                out.undeclaredDependency = true;
                break;
            case Determined::Nothing:
            case Determined::Requirement:
                break;
        }
    }

    if(!solver.settle(determined, out.types.size())) return;

    if(!out.instance) {
        out.instance = resolver.selectInstance(typeClass, toBuffer(out.types), out.instanceArgs);
    }
}
