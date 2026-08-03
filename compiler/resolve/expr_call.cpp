#include "expr.h"
#include "complete.h"
#include "witness.h"
#include "generic.h"
#include "name.h"
#include "index.h"

/*
 * Calls, operators, and typeclass instance selection.
 *
 * Every operator in the language is an ordinary call, and every arithmetic and comparison
 * operator is a class function: `+` is `Num.+`, `==` is `Eq.==`, and a conversion is
 * `Widen.widen` or `Narrow.narrow`. Selecting one is two steps - work out what the class's type
 * variables must be here, then find the instance for them - and both steps run against the class
 * signature, which is why a variable appearing only in the result type is inferable exactly like
 * one appearing in an argument. That is what makes `round(x) :: Long` pick an instance by its
 * return type.
 *
 * The rule for which position gets to decide is deliberately one-directional: arguments bind
 * first, and the expected result type only fills in variables the arguments left open. Design.md
 * asks for bottom-up, left-to-right inference with no backtracking, and this is that rule - it
 * keeps `1 + 2 :: Long` an Int addition widened afterwards rather than silently becoming a Long
 * addition, while still letting `widen(x) :: Long` work at all.
 *
 * A literal argument is the one thing that leaves a variable open without leaving it unbound. It
 * binds a literal variable, which is not a decision, so the expected result may still refine it -
 * `inc(1) :: Long` is a Long computation, while `inc(x) :: Long` on an Int `x` is not.
 *
 * A name may be declared by more than one class and by one class at more than one arity, so
 * emitCall selects out of an overload set rather than off a single signature. Design.md's
 * Overloading section states the five rules it implements: the set is keyed by (name, arity),
 * candidates are not ranked, a constraint declared by the enclosing function is what picks a class
 * in generic code, ambiguity is resolved by writing `Class.method` and never by a tiebreak, and a
 * plain function is one member of the set rather than a shadow over it.
 */

// The declared precedence of an operator that has one. Only reached for operators resolveBinary
// has already established a fixity for, so the fallback is unreachable rather than meaningful -
// precedence 0 is a real precedence, which is where an assignment operator belongs.
static U8 operatorPrecedence(Module& module, StringId op) {
    auto found = findPrecedence(module, op);
    return found ? found.unwrap() : 0;
}

// R4 resolves ambiguity by qualification and never by a tiebreak, so an ambiguity diagnostic has to
// say which qualified names there are to choose between - `Integral.and or Logic.and`. Leaving the
// author to go and find the classes themselves is the difference between a rule and a puzzle.
static String describeQualified(Context& context, GlobalBase global, StringId name,
                               Buffer<GlobalPtr<TypeClass>> classes) {
    StringBuilder text;

    for(Size i = 0; i < classes.length; i++) {
        if(i) text.append(i + 1 == classes.length ? " or "_v : ", "_v);
        text << context.findName(global[classes[i]]->name) << '.' << context.findName(name);
    }

    return text.string();
}

// Binds the class type variables that one position of a signature constrains. Widening applies at
// the top level only: unifying the positions bound to one class variable to their common Widen
// supertype is how `1 + 2.5` reaches Num(Float) without the class machinery having to know
// anything about numbers below the outermost type.
bool ExprResolver::bindPosition(TypePtr pattern, TypePtr actual, TypeList& bindings, bool widen) {
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

        if(!bindings[index]) {
            bindings[index] = actual;
            return true;
        }

        if(bindings[index] == actual) return true;

        /*
         * A `@bits` refinement and the type it refines are one type here.
         *
         * [repr.md](doc/spec/repr.md#bit-width-refinements) says it outright - *"`@bits(n)` is
         * Repr-only: it never participates in typeclass dispatch"*, and *"everything that dispatches
         * (instance selection, literal defaulting, overload resolution) canonicalizes first"* - and
         * `matchType` is the half that was built: an instance head canonicalizes both sides, so
         * `instance Num(U64)` answers `Num(@bits(53) U64)` and nobody writes an instance per width.
         *
         * This is the other half, and without it the rule held only while every position of a call
         * agreed on the *same* refinement. `x == y` on a `@bits(2) U32` and a `U32` bound `a` twice
         * to two types, found no common `Widen` between them - there is no instance for a refinement,
         * since widening one is structural rather than a conversion anyone declared - and reported
         * "no class function == accepts (@bits(2) U32, U32)" for an operator the spec says is the
         * plain `U32` one. Two refinements of one type failed the same way.
         *
         * The variable takes the *canonical* type, which is what makes the arguments converge:
         * widening a refinement to its base is free (see convertRefinement), and it is what a load of
         * a packed field already produces - repr.md's "load always widens". So the operation happens
         * at the natural width, as it does everywhere else, and only a store narrows again.
         */
        if(auto canonical = canonicalType(global, bindings[index]);
           canonical == canonicalType(global, actual)) {
            bindings[index] = canonical;
            return true;
        }

        // A literal has no type yet, so meeting one is not a conflict. It takes whatever the
        // other position decided if its class allows; two literals become one variable carrying
        // both their classes, which is what leaves `1 + 2.5` a single question to answer.
        if(isLiteral(global, actual)) {
            if(!isLiteral(global, bindings[index])) return literalFits(actual, bindings[index]);

            bindings[index] = mergeLiterals(bindings[index], actual);
            return true;
        }

        if(isLiteral(global, bindings[index])) {
            if(!literalFits(bindings[index], actual)) return false;

            bindings[index] = actual;
            return true;
        }

        if(!widen) return false;

        auto common = commonWiden(bindings[index], actual);
        if(!common) return false;

        bindings[index] = common;
        return true;
    }

    // A literal against a written type takes that type outright, since there is nothing below the
    // outermost type of a literal for matchType to walk into.
    if(isLiteral(global, actual)) return !isGeneric(global, pattern) && literalFits(actual, pattern);

    /*
     * A concrete position in an otherwise generic signature is judged the way a non-generic
     * function's is: there is no variable here to bind, so what matters is whether the argument
     * converts. Without this, `fn push(&self: Array(a), index: Int)` would reject the `Short` that
     * the identical non-generic signature accepts, and a signature would mean different things
     * depending on whether some *other* parameter mentioned a type variable.
     *
     * Only in the argument direction. The result position asks what the call produces, and a
     * result that would need converting has not decided the type arguments by itself.
     */
    if(widen && !isGeneric(global, pattern) && !sameType(pattern, actual)) {
        TypePtr pair[] = { actual, pattern };
        return findInstance(module, module.coreClasses.widen, { pair, 2 }) != nullptr;
    }

    return matchType(global, pattern, actual, { bindings.pointer(), bindings.size() });
}

/*
 * Which arguments of a call are deferred, decided from the name alone.
 *
 * The order this has to happen in is what makes it a question about a name. A `@lazy` argument must
 * not be evaluated, and selection needs the argument *types* to pick an overload, so the decision
 * comes before there is a callee to read it off - which leaves only what every candidate of one
 * (name, arity) has in common. Design.md's rule that strictness is fixed by the class signature
 * rather than by the instance is exactly this, stated from the other side.
 *
 * Two candidates that disagree are therefore a declaration error rather than a call-site one, but
 * it is only detectable where the two are visible together, which is here.
 */
static U32 lazyMaskOf(ModuleBase local, Function& function) {
    U32 mask = 0;
    Size index = 0;

    for(auto argPointer: function.args.contents(local)) {
        if(local[argPointer]->isLazy() && index < 32) mask |= U32(1) << index;
        index++;
    }

    return mask;
}

U32 ExprResolver::lazyArguments(StringId name, Size arity, LocationId source) {
    // The negative answer, which is every call in a program but a handful - see Program::lazyNames.
    if(!module.program.lazyNames.contains(name)) return 0;

    U32 mask = 0;
    auto seen = false;
    auto conflict = false;

    auto consider = [&](U32 candidate) {
        if(seen && candidate != mask) conflict = true;
        else mask = candidate;

        seen = true;
    };

    if(auto direct = findFunction(module, name, source)) {
        if(local[direct]->args.size() == arity) consider(lazyMaskOf(local, *local[direct]));
    }

    ClassFunList candidates;
    findClassFunctions(module, name, source, candidates);

    for(auto& candidate: candidates) {
        auto entry = global[candidate.typeClass]->functions.get(global, candidate.index);
        if(entry.arity != arity || !entry.fun) continue;

        consider(lazyMaskOf(local, *local[entry.fun]));
    }

    if(conflict) {
        context.diagnostics.error("the declarations of %@ disagree about which arguments are `@lazy`, so a call to it cannot tell what to evaluate - strictness is part of the signature and every overload of one name and arity has to declare the same one"_v,
                                  source, context.findName(name));
        return 0;
    }

    return mask;
}

// The instance of `typeClass` that serves `args`, and what selecting it bound its own type
// variables to.
ModulePtr<ClassInstance> ExprResolver::selectInstance(GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                                                      TypeList& instanceArgs) {
    auto match = matchInstance(module, typeClass, args);
    replaceContents(instanceArgs, match.args);
    return match.instance;
}

/*
 * Settling a call's type arguments, with the constraints' dependencies given the last word.
 *
 * Three steps rather than one, and the middle one is the whole reason this is a function. A
 * binding a *literal* made is not an answer, it is a default waiting to be overridden:
 * `fn (Index(c, k, v)) at(xs: c, i: k) -> v` called as `at(xs, 0)` binds `k` from the `0`, and
 * settling that gives `Int` - so by the time the dependency is asked, `k` looks decided and the
 * instance is never consulted. Which is backwards. `c` decides `k`, and a container whose key is
 * `Size` should take the literal at `Size` rather than have no instance at `Int`.
 *
 * So the dependency is asked with those positions cleared, and what it answers wins. A position no
 * instance decides keeps the default the settle already gave it, which is why the answer is merged
 * back rather than assigned: a class that determines nothing is unaffected, and so is every call
 * whose arguments were not literals.
 *
 * The settle still comes first for the *deciding* positions, and only what is already decided can
 * be settled - the dependency is answered by looking an instance up, and `sum([1, 2, 3])` binds the
 * container to a literal type until something defaults it. A deciding position is never one of the
 * cleared ones: `Array(<literal>)` is a record and not itself a literal, which is what keeps the
 * lookup answerable.
 */
void ExprResolver::settleWithDependencies(GenEnv& env, TypeList& bindings, LocationId source) {
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

    SmallArray<bool, 8> defaulted;
    for(Size i = 0; i < bindings.size(); i++) {
        auto overridable = i < 64 && (answerable & (U64(1) << i)) && !(decides & (U64(1) << i));
        defaulted.push(overridable && bindings[i] && isLiteral(global, bindings[i]));
    }

    for(Size i = 0; i < bindings.size(); i++) {
        if(bindings[i]) bindings[i] = settleType(bindings[i]);
    }

    TypeList asked = bindings;
    for(Size i = 0; i < asked.size(); i++) {
        if(defaulted[i]) asked[i] = nullptr;
    }

    fillDetermined(module, env, asked, source);

    for(Size i = 0; i < bindings.size(); i++) {
        if(asked[i]) bindings[i] = asked[i];
    }
}

ModulePtr<Value> ExprResolver::emitInstanceCall(Module& site, ModulePtr<ClassInstance> instance,
                                                Buffer<TypePtr> instanceArgs, U16 index,
                                                Buffer<ModulePtr<Value>> args, LocationId source,
                                                TypePtr target, StringId resultName,
                                                Buffer<Deferred> deferred) {
    auto implementation = local[instance]->functions.get(local, index);
    if(!implementation) return nullptr;

    // A default the instance did not override is generic over the *class's* type variables rather
    // than over the head's, so what specializes it is what the head resolves to - `Ptr(Int)`, for
    // `Eq(Ptr(a))` selected at `a = Int` - and not the head's own bindings. Reading the class types
    // back off the head is what makes a concrete and a parametric instance the same case here.
    if(implementation == global[local[instance]->typeClass]->functions.get(global, index).defaultFun) {
        TypeList classArgs;
        for(auto type: local[instance]->forTypes.contents(local)) {
            classArgs.push(substituteType(module, type, instanceArgs, source));
        }

        auto specialized = instantiateFunction(site, implementation, toBuffer(classArgs), source);
        if(!specialized) return nullptr;

        return emitDirectCall(specialized, args, source, target, resultName, deferred);
    }

    // A concrete instance's implementation is a function like any other.
    if(!local[instance]->gen) return emitDirectCall(implementation, args, source, target, resultName, deferred);

    // A parametric one's is written against the head's variables, so the types the head bound are
    // what makes it a function about something. An intrinsic has no body to specialize and is
    // generated here for those types, exactly as a generic intrinsic is at an ordinary call site.
    ValueList converted;
    for(Size i = 0; i < args.length; i++) {
        // A deferred argument has no conversion to apply yet - it is not a value. What it converts
        // to is decided where it is forced, against the parameter type the callee declares.
        if(isDeferred(deferred, i)) {
            converted.push(nullptr);
            continue;
        }

        auto declared = local[local[implementation]->args.get(local, i)]->declaredType();
        converted.push(convert(args[i], substituteType(module, declared, instanceArgs, source), source));
    }

    if(local[implementation]->intrinsic || local[implementation]->deferredIntrinsic) {
        return expandIntrinsic(implementation, instanceArgs, toBuffer(converted), source, resultName, deferred);
    }

    auto specialized = instantiateFunction(site, implementation, instanceArgs, source);
    if(!specialized) return nullptr;

    return emitDirectCall(specialized, toBuffer(converted), source, target, resultName, deferred);
}

// Works out whether one class function can serve this call, and if so which instance it selects.
// Returns false when the call does not fit the signature at all; a fitting signature with no
// instance is reported through `resolved` so the caller can tell "wrong function" from "no
// instance for these types", which are very different diagnostics.
bool ExprResolver::matchClassFun(const ClassFunRef& reference, Buffer<ModulePtr<Value>> args, TypePtr target,
                                 ClassMatch& resolved, Buffer<Deferred> deferred) {
    auto typeClass = global[reference.typeClass];
    auto signature = local[typeClass->functions.get(global, reference.index).fun];
    if(!signature || signature->args.size() != args.length) return false;

    auto env = global[typeClass->gen];
    TypeList bindings;
    for(Size i = 0; i < env->types.size(); i++) bindings.push(nullptr);

    for(Size i = 0; i < args.length; i++) {
        if(isDeferred(deferred, i)) continue;
        if(!args[i]) return false;

        auto declared = local[signature->args.get(local, i)]->declaredType();
        if(!bindPosition(declared, valueType(args[i]), bindings, true)) return false;
    }

    // The expected result only fills in what the arguments left open, so an ascription can pick
    // an instance but cannot re-pick one the arguments already determined. A literal argument
    // determines nothing, so a binding it made is still open in this sense.
    if(target) {
        TypeList withTarget = bindings;

        if(bindPosition(signature->returnType, target, withTarget, false)) {
            for(Size i = 0; i < bindings.size(); i++) {
                if(!bindings[i] || isLiteral(global, bindings[i])) bindings[i] = withTarget[i];
            }
        }
    }

    /*
     * A class's type argument has to be a real type before an instance can be looked for, so a
     * literal variable that no position decided takes its class's default here. The end of the
     * statement is the outer boundary for that; a call that needs an instance is the inner one,
     * and it is the one that comes first.
     *
     * The determining positions of a functional dependency settle *before* the rest, because the
     * instance they select is what decides the rest. Settling everything first would ask the table
     * for a literal's type rather than its default and find nothing; filling first would leave the
     * determined positions holding whatever a literal determiner happened to point at.
     */
    auto determined = typeClass->determines() ? Size(typeClass->determined) : bindings.size();

    for(Size i = 0; i < determined; i++) {
        bindings[i] = settleType(bindings[i]);
        if(!bindings[i]) return false;
    }

    // `c` decides `a`, so a call that bound only `c` reads `a` off the instance rather than failing
    // to infer it. The selected instance is kept: looking it up again below with the now-complete
    // arguments would find the same one, and this way the search happens once.
    ModulePtr<ClassInstance> fromDependency = nullptr;
    auto open = false;

    for(Size i = determined; i < bindings.size(); i++) {
        if(!bindings[i]) open = true;
    }

    if(open) {
        if(auto match = resolveDetermined(module, reference.typeClass, bindings)) {
            fromDependency = match.instance;
            replaceContents(resolved.instanceArgs, match.args);
        } else if(auto env = functionGen(global, function)) {
            /*
             * Inside a generic body there is no instance to read the determined positions off:
             * `c` is this function's own type variable and which container it will be is the
             * caller's business. What answers instead is the requirement the signature declared,
             * which already gave the determined position a name - the `a` of `fn (Contiguous(c,
             * a)) first(self: c)`.
             *
             * Undeclared is deliberately not inferred. Recording `Contiguous(c, ?)` would mean
             * inventing a variable for the body, which is one more thing every caller has to
             * satisfy without the author having written it; the constraint has to be declared, and
             * the diagnostic below says so.
             */
            TypeList declared;

            if(findClassRequirement(module, *env, reference.typeClass, toBuffer(bindings), declared)) {
                for(Size i = determined; i < bindings.size() && i < declared.size(); i++) {
                    if(!bindings[i]) bindings[i] = declared[i];
                }
            } else if(typeClass->determines()) {
                resolved.undeclaredDependency = true;
            }
        }
    }

    for(Size i = determined; i < bindings.size(); i++) {
        bindings[i] = settleType(bindings[i]);
        if(!bindings[i]) return false;
    }

    resolved.typeClass = reference.typeClass;
    resolved.index = reference.index;
    resolved.instance = fromDependency
        ? fromDependency
        : selectInstance(reference.typeClass, toBuffer(bindings), resolved.instanceArgs);

    replaceContents(resolved.args, bindings);

    return true;
}

// The plain-function half of an overload set. Design.md's R1 keys the set by (name, arity) and
// admits at most one plain function, so this is arity plus "do the arguments fit", and the answer
// has to be reached without reporting anything - see ExprResolver::convertible.
bool ExprResolver::matchFunction(ModulePtr<Function> callee, Buffer<ModulePtr<Value>> args, TypePtr target,
                                LocationId source, Buffer<Deferred> deferred) {
    auto callable = local[callee];
    if(callable->args.size() != args.length) return false;

    // A generic function fits when its type arguments can all be inferred here, by the same
    // one-directional rule the classes use.
    if(auto env = functionGen(global, *callable)) {
        TypeList bindings;
        for(Size i = 0; i < env->types.size(); i++) bindings.push(nullptr);

        for(Size i = 0; i < args.length; i++) {
            if(isDeferred(deferred, i)) continue;

            auto declared = local[callable->args.get(local, i)]->declaredType();
            if(!bindPosition(declared, valueType(args[i]), bindings, true)) return false;
        }

        if(target) {
            TypeList withTarget = bindings;

            if(bindPosition(callable->returnType, target, withTarget, false)) {
                for(Size i = 0; i < bindings.size(); i++) {
                    if(!bindings[i] || isLiteral(global, bindings[i])) bindings[i] = withTarget[i];
                }
            }
        }

        /*
         * A variable this signature's constraints determine is inferred from them rather than from
         * the call, so `fn (Contiguous(c, a)) sum(xs: c) -> a` fits a call that mentions only `c`.
         *
         * The settle has to come first, and only what is already decided can be settled: the
         * dependency is answered by looking an instance up, and `sum([1, 2, 3])` binds the
         * container to a *literal* type until something defaults it. There is no instance for one
         * of those, so asking before settling finds nothing and infers nothing.
         */
        settleWithDependencies(*env, bindings, source);

        for(Size i = 0; i < bindings.size(); i++) {
            if(!settleType(bindings[i])) return false;
        }

        return true;
    }

    for(Size i = 0; i < args.length; i++) {
        if(isDeferred(deferred, i)) continue;
        if(!convertible(args[i], local[callable->args.get(local, i)]->declaredType(), source)) return false;
    }

    return true;
}

// Precedence climbing over the flattened operand/operator lists. The parser cannot do this
// itself: fixity declarations are module-level, so an operator's precedence is only known once
// the whole module has been read.
/*
 * `target` is the type the whole chain is expected to produce, and it is passed to each operator's
 * own selection rather than to the operands.
 *
 * What it buys is the case where nothing else says: `1 \`or\` 2` at a `WideInt` result used to
 * settle both literals to `Int`, select `Integral(Int)`, compute at 32 bits and widen the *answer* -
 * so `34359738368 \`or\` 255` was 255, having lost the operand before the operator ran.
 * `matchClassFun` already knows what to do with an expected result; it was simply never handed one
 * from here.
 *
 * To the operator and not to the operands, because the operands are where this would stop being
 * safe. `matchClassFun` lets the expected result fill only what the arguments left *open* - a
 * literal binds nothing, a concrete operand binds its own type and wins - so `x + 1` on an `Int` x
 * still computes at `Int` however the result is used. Resolving the operands against `target`
 * instead would convert `x` first and silently change that to `WideInt` arithmetic.
 *
 * The same reason bounds what it fixes: a *parenthesized* sub-expression is resolved through
 * `resolve` below with no expected type, so `(1 \`or\` 2) \`and\` 3` still settles the inner call
 * to `Int` before the outer one is matched. Carrying an expected type down into operand position
 * is bidirectional checking, which this resolver deliberately does not do (it binds one-way and
 * positionally), and it is the same wall the property-constraint inference hit.
 */
// Advances past exactly the sub-chain resolvePrecedence would have consumed, without resolving any
// of it. What a deferred right operand needs: the chain still has to be walked to find where this
// operator's argument ends and the next one begins, but the expression itself belongs in whatever
// block the callee decides to run it in.
static void skipPrecedence(Module& module, SmallArray<StringId, 8>& operators, Size& operandIndex,
                           Size& operatorIndex, U8 minimumPrecedence) {
    operandIndex++;

    while(operatorIndex < operators.size() &&
          operatorPrecedence(module, operators[operatorIndex]) >= minimumPrecedence) {
        auto precedence = operatorPrecedence(module, operators[operatorIndex++]);
        skipPrecedence(module, operators, operandIndex, operatorIndex, precedence + 1);
    }
}

ModulePtr<Value> ExprResolver::resolvePrecedence(SmallArray<const ast::Expr*, 8>& operands, SmallArray<StringId, 8>& operators, SmallArray<LocationId, 8>& operatorSources, Size& operandIndex, Size& operatorIndex, U8 minimumPrecedence, TypePtr target) {
    auto lhsExpr = operands[operandIndex++];
    auto lhs = resolve(*lhsExpr);

    while(operatorIndex < operators.size() && operatorPrecedence(module, operators[operatorIndex]) >= minimumPrecedence) {
        auto opSource = operatorSources[operatorIndex];
        auto op = operators[operatorIndex++];
        auto precedence = operatorPrecedence(module, op);

        /*
         * The right operand of a short-circuiting operator is not resolved here.
         *
         * Only the right one: the left is already a value by the time the operator is read, and an
         * operator whose *first* argument was `@lazy` would need the chain to be walked backwards
         * to find it. Nothing declares one, and Design.md's uses are all second-position, so it is
         * reported rather than supported.
         */
        auto lazy = lazyArguments(op, 2, opSource);

        if(lazy & 1) {
            context.diagnostics.error("the left operand of %@ is declared `@lazy`, which an infix operator cannot be - it is evaluated before the operator is read"_v,
                                      lhsExpr->source, context.findName(op));
            return nullptr;
        }

        DeferredChain chain;
        Deferred deferred[2];
        ModulePtr<Value> rhs = nullptr;

        if(lazy & 2) {
            chain.operands = &operands;
            chain.operators = &operators;
            chain.operatorSources = &operatorSources;
            chain.operandIndex = operandIndex;
            chain.operatorIndex = operatorIndex;
            chain.minimumPrecedence = U8(precedence + 1);
            deferred[1].chain = &chain;

            skipPrecedence(module, operators, operandIndex, operatorIndex, U8(precedence + 1));
        } else {
            rhs = resolvePrecedence(operands, operators, operatorSources, operandIndex, operatorIndex, precedence + 1);
            if(!rhs) return nullptr;
        }

        if(!lhs) return nullptr;

        ModulePtr<Value> args[] = { lhs, rhs };
        lhs = emitCall(op, { args, 2 }, lhsExpr->source, target, 0,
                       lazy ? Buffer<Deferred>{ deferred, 2 } : Buffer<Deferred>{}, 0, opSource);
    }

    return lhs;
}

ModulePtr<Value> ExprResolver::resolveBinary(const ast::Expr& expr, const ast::InfixExpr& binary, TypePtr target, bool convertResult) {
    SmallArray<const ast::Expr*, 8> operands;
    SmallArray<StringId, 8> operators;
    SmallArray<LocationId, 8> operatorSources;
    auto node = &binary;

    // The parser nests infix expressions to the right without regard for precedence, so the
    // chain is flattened first and then re-associated by resolvePrecedence.
    while(true) {
        if(node->op.kind != ast::Expr::Var) {
            context.diagnostics.error("an infix operator must be a named operator"_v, node->op.source);
            return nullptr;
        }

        if(!findPrecedence(module, node->op.var)) {
            context.diagnostics.error("operator has no declared fixity %@"_v, node->op.source, context.findName(node->op.var));
            return nullptr;
        }

        operands.push(&node->lhs);
        operators.push(node->op.var);
        operatorSources.push(node->op.source);

        if(node->rhs.kind != ast::Expr::Infix) {
            operands.push(&node->rhs);
            break;
        }

        node = parse[node->rhs.infix];
    }

    Size operandIndex = 0;
    Size operatorIndex = 0;

    // Climbing starts at 0 rather than 1 because 0 is a declarable precedence - it is where Core
    // puts the compound assignments. Starting a rung above it would drop such an operator out of
    // the loop and quietly yield its left operand instead of applying it.
    // The target goes into the chain as well as being applied to its result. Where the operators
    // could honour it the conversion afterwards is then the identity; where they could not - a
    // concrete operand decided the instance - it is the conversion that was always emitted.
    auto result = resolvePrecedence(operands, operators, operatorSources, operandIndex, operatorIndex, 0, target);
    if(result && target) result = convert(result, target, expr.source, convertResult);
    return result;
}

ModulePtr<Value> ExprResolver::resolvePrefix(const ast::Expr& expr, const ast::PrefixExpr& prefix, TypePtr target,
                                             bool convertResult) {
    if(prefix.op.kind != ast::Expr::Var) {
        context.diagnostics.error("a prefix operator must be named"_v, prefix.op.source);
        return nullptr;
    }

    auto lazy = lazyArguments(prefix.op.var, 1, prefix.op.source);

    Deferred deferred[1];
    ModulePtr<Value> value = nullptr;

    if(lazy & 1) {
        deferred[0].expr = &prefix.on;
    } else {
        // The operand is resolved with no expected type of its own. What a prefix operator's
        // argument should be is its selected overload's parameter type, which is not known until
        // the operand has one - and pushing the *result* type down is only right when the two
        // coincide, as they do for `-` and not for a dereference, whose operand is a pointer to
        // its result.
        value = resolve(prefix.on);
        if(!value) return nullptr;
    }

    ModulePtr<Value> args[] = { value };
    auto result = emitCall(prefix.op.var, { args, 1 }, expr.source, target, 0,
                           lazy ? Buffer<Deferred>{ deferred, 1 } : Buffer<Deferred>{}, 0, prefix.op.source);

    return convertResult && target ? convert(result, target, expr.source) : result;
}

/*
 * A call whose callee is a value rather than a name.
 *
 * Two shapes reach here and they are the same question asked twice: `f(x)` where `f` is a local of
 * function type, and `(expr)(x)` where the callee is any expression at all. The first has to be
 * checked before the name is looked up as a function, because a binding shadows a module-level
 * declaration exactly as it does everywhere else.
 *
 * Null when this call is not an indirect one, which leaves it for the ordinary path.
 */
ModulePtr<Value> ExprResolver::resolveIndirectCall(const ast::Expr& expr, const ast::AppExpr& call,
                                                   TypePtr target) {
    auto& callee = unwrapNested(call.callee);
    ModulePtr<Value> callable = nullptr;

    if(callee.kind == ast::Expr::Var) {
        auto binding = findBinding(callee.var, callee.source);
        if(!binding) return nullptr;

        if(binding->lazy) {
            // The name holds the thunk, so calling what it stands for is two calls: force it, and
            // then call the function value the argument produced.
            Deferred deferred;
            deferred.thunk = binding->value;
            callable = force(deferred, nullptr, callee.source);
        } else {
            callable = binding->isPlace() ? load(placeOf(*binding, callee.source), callee.source)
                                          : binding->value;
        }

        if(!isFunction(global, valueType(callable))) {
            context.diagnostics.error("%@ is not callable - it is %@"_v, callee.source,
                                      context.findName(callee.var),
                                      describeType(context, global, valueType(callable)));
            return nullptr;
        }
    } else {
        callable = resolve(callee);

        // Already broken, and said so - see resolveField. Calling an error is not a second fact.
        if(callable && global[valueType(callable)]->kind == Type::Error) return callable;

        if(!callable || !isFunction(global, valueType(callable))) {
            if(callable) {
                context.diagnostics.error("this expression is not callable - it is %@"_v, callee.source,
                                          describeType(context, global, valueType(callable)));
            }

            return nullptr;
        }
    }

    auto signature = (FunType*)global[valueType(callable)];
    ValueList values;
    Size index = 0;

    // A function value's parameter types are known before its arguments are resolved, exactly as a
    // plain function's are, so they are pushed down the same way - which is what lets `f(Nothing)`
    // through a `(Maybe(Int)) -> Bool` know which `Maybe` it is building.
    auto callArgs = call.args;

    for(auto arg: callArgs.contents(parse)) {
        if(arg.name) {
            context.diagnostics.error("named call arguments are not available yet"_v, arg.value.source);
        }

        auto expected = index < signature->args.size()
            ? signature->args.get(global, index).type : TypePtr(nullptr);

        values.push(resolve(arg.value, expected));
        index++;
    }

    auto result = emitDynamicCall(callable, toBuffer(values), expr.source, 0);
    return target ? convert(result, target, expr.source) : result;
}

ModulePtr<Value> ExprResolver::resolveCall(const ast::Expr& expr, const ast::AppExpr& call, TypePtr target, bool convertResult) {
    // A binding of function type shadows a declaration of the same name, and an arbitrary callee
    // expression was never a name at all. Both are the indirect path.
    auto& calleeExpr = unwrapNested(call.callee);

    /*
     * The cursor sentinel in callee position - `f|(x)` - which is where an editor asks most often,
     * since typing a name and then its arguments is the order a call is written in.
     *
     * The call's expected result type is what ranks the answer: a candidate that returns what this
     * position wants is the one being reached for. Ahead of everything, because the arguments are
     * resolved against a callee that does not exist and would each report.
     */
    if(calleeExpr.kind == ast::Expr::Var && isCursorSentinel(context, calleeExpr.var)) {
        captureCompletion(*this, target, nullptr, false);
        return nullptr;
    }

    auto named = calleeExpr.kind == ast::Expr::Var && !findBinding(calleeExpr.var);

    if(!named) return resolveIndirectCall(expr, call, convertResult ? target : nullptr);

    // A plain function's parameter types are known before its arguments are resolved, so they
    // are pushed down as the expected type of each one. That is what lets `f(Nothing)` know
    // which `Maybe` it is building - neither a class function nor a generic function can do the
    // same, because which types their parameters have is exactly what the arguments are being
    // resolved to decide.
    //
    // Only when the plain function is the whole overload set, though: R5 lets the class half serve
    // a call the plain function does not fit, and pushing its parameter types into the arguments
    // would report the mismatch before selection ever got the chance to look elsewhere.
    /*
     * Looked up at the *callee's* location rather than the call's.
     *
     * It is where the name is written, so it is both the better place for an ambiguity diagnostic
     * to point and the only location the index should hold this answer against - see
     * resolve/index.h. Recording it against the whole call would make find-references report the
     * call and the name as two hits on the same name.
     */
    auto direct = findFunction(module, calleeExpr.var, calleeExpr.source);
    auto callArgs = call.args;
    auto declared = direct && !local[direct]->gen && local[direct]->args.size() == callArgs.size();

    if(declared) {
        ClassFunList overloads;
        findClassFunctions(module, calleeExpr.var, calleeExpr.source, overloads);
        declared = overloads.isEmpty();
    }

    /*
     * A lens or iterator call that reaches here left its continuation out and is in a position that
     * does not supply one.
     *
     * Nothing splits at this position - Analysis-Lens.md's V1 restriction is that a lens call is the
     * whole right-hand side of a `let` or a statement of its own, and an iterator's is the source of
     * a `for` - so the arity is genuinely one short, and saying that is more use than "takes 3
     * arguments but was given 2".
     */
    if(direct && local[direct]->funKind != ast::FunKind::Plain &&
       local[direct]->args.size() == callArgs.size() + 1) {
        context.diagnostics.error(local[direct]->funKind == ast::FunKind::Iter
            ? "%@ is an iterator, so this call has no body to hand its values to - write it as the source of a `for` loop, which is the only thing that supplies one"_v
            : "%@ is a lens, so this call needs the rest of a block to hand its values to - write it as a statement of its own or as the whole right-hand side of a `let`, or pass the continuation as a final argument"_v,
            expr.source, context.findName(calleeExpr.var));
        return nullptr;
    }

    auto lazy = lazyArguments(calleeExpr.var, callArgs.size(), calleeExpr.source);

    ValueList values;

    /*
     * Parallel to `values`, and built only where there is a `@lazy` parameter to fill.
     *
     * Both halves of that matter. Inline, because a call has as many of these as it has arguments
     * and that is two or three; and skipped entirely when `lazy` is zero, because `pending` below
     * is empty in that case - so what this used to be was a list built for every call in the
     * program and then not passed on. `@lazy` is rare enough that the ordinary call is the one
     * worth being right about.
     */
    SmallArray<Deferred, 8> deferred;

    // The positions that resolved to a value carrying nothing, rather than to no value - see the
    // guard in emitCall. A bitmask on the same terms as `lazy`, and cut off at the same 32: an
    // argument past that is left to the older reading, which is what it had before.
    U32 nothing = 0;

    auto written = callArgs.contents(parse);

    for(Size index = 0; index < written.size(); index++) {
        // By address rather than through the iterator: a deferred argument is resolved long after
        // this loop has ended, so what is remembered has to be the node in the parse arena.
        auto arg = written.pointerAt(index);

        if(arg->name) {
            context.diagnostics.error("named call arguments are not available yet"_v, arg->value.source);
        }

        if(lazy) deferred.push(Deferred());

        // A `@lazy` argument is left as written. Not even the expected type is pushed into it here:
        // it is resolved against the parameter type once the callee is known, which is where the
        // force happens and therefore the only place that can convert it.
        if(index < 32 && (lazy & (U32(1) << index))) {
            deferred[index].expr = &arg->value;
            values.push(nullptr);
            continue;
        }

        auto parameter = declared ? local[local[direct]->args.get(local, index)] : nullptr;

        // A `&` parameter's type is deliberately not pushed down. What the argument has to produce
        // is storage to borrow, not a value of the parameter's type - so converting here would build
        // a temporary and then borrowArgument would be asked for a mutable borrow of something this
        // expression owns rather than of what was written.
        auto expected = parameter && !parameter->isMutableBorrow() ? parameter->declaredType()
                                                                   : TypePtr(nullptr);

        // An argument that resolved to nothing did so for one of two reasons, and the difference is
        // only visible from here: either the resolver reported on it, or the expression genuinely
        // produces no value - `{}`, or a call whose result type is `{}`. The same before-and-after
        // count `resolvePattern`'s callers and `witness.cpp` use to ask exactly this question.
        auto errors = context.diagnostics.errorCount();
        auto value = resolve(arg->value, expected);

        if(!value && index < 32 && errors == context.diagnostics.errorCount()) {
            nothing |= U32(1) << index;
        }

        values.push(value);
    }

    auto pending = lazy ? toBuffer(deferred) : Buffer<Deferred>{};

    if(declared) {
        // The whole overload set was one plain function, so the decision was made above rather than
        // in emitCall - and §1.2 says to record where the decision is.
        recordReference(context, calleeExpr.source, functionSymbol(module, direct));
    }

    auto result = declared ? emitDirectCall(direct, toBuffer(values), expr.source, target, 0, pending)
                           : emitCall(calleeExpr.var, toBuffer(values), expr.source, target, 0, pending, nothing,
                                      calleeExpr.source);

    return convertResult && target ? convert(result, target, expr.source) : result;
}

// Emits a call to a known function, converting each argument to its declared type. An intrinsic
// produces its result directly instead: the primitives are real functions with real bodies, but
// an ordinary call to one expands to the instruction it contains rather than to a call the
// backend would have to inline again later.
ModulePtr<Value> ExprResolver::emitDirectCall(ModulePtr<Function> callee, Buffer<ModulePtr<Value>> args,
                                              LocationId source, TypePtr, StringId resultName,
                                              Buffer<Deferred> deferred) {
    auto function_ = local[callee];

    // An `=` callee whose result type its body decides has to have decided it before this call can
    // be given a type. Ordinarily it already has - resolveModuleBodies() settles them up front -
    // but one inferring function calling another declared after it arrives here first.
    requireReturnType(module, *function_, source);

    /*
     * The deferred arguments, in the callee's own terms.
     *
     * This is the point every `@lazy` argument has been travelling towards: the callee is known, so
     * the parameter type it was declared at is known, and the choice between emitting the argument
     * where it is used and wrapping it in a closure can finally be made. An intrinsic that declares
     * one takes the whole list unresolved and decides for itself where each one runs, which is what
     * makes `a && b` a branch; anything else gets the thunk.
     */
    SmallArray<Deferred, 4> pending;
    auto anyDeferred = false;

    for(Size i = 0; i < args.length; i++) {
        auto declared = local[function_->args.get(local, i)];
        Deferred entry = i < deferred.length ? deferred[i] : Deferred();

        if(!declared->isLazy()) {
            pending.push(Deferred());
            continue;
        }

        // Not deferred by the call site: the argument was resolved before anything knew this
        // position was lazy, which is what a forwarded value and a synthesized call look like.
        if(!entry.isSet()) entry = deferredValue(args[i], declared->lazyType);

        entry.type = declared->lazyType;
        pending.push(entry);
        anyDeferred = true;
    }

    /*
     * Where this call's own packed-field write-backs start.
     *
     * By mark rather than wholesale, because the arguments were resolved before this was reached
     * and a nested call among them has already committed its own: `f(&h.a, g(&h.b))` commits `b`
     * after `g` and `a` after `f`, rather than committing `a` twice or `b` too late.
     */
    auto packed = packedMark();

    if(anyDeferred && function_->deferredIntrinsic) {
        auto expanded = function_->deferredIntrinsic(*this, args, toBuffer(pending), function_->returnType,
                                                     source, resultName);
        flushPackedBorrows(packed);
        return expanded;
    }

    // Every argument's convention is applied here, which is the one place a call knows both what
    // the callee asked for and what the caller produced. A `&` becomes a mutable borrow of the
    // argument's storage; everything else is the ordinary value path.
    ValueList converted;
    for(Size i = 0; i < args.length; i++) {
        auto declared = local[function_->args.get(local, i)];

        // The callee cannot see the argument, so what it is handed is the closure that runs it.
        if(declared->isLazy()) {
            converted.push(makeThunk(pending[i], declared->lazyType, source));
            continue;
        }

        if(declared->isMutableBorrow()) {
            converted.push(borrowArgument(args[i], declared->type, source, declared->returnRoot));
            continue;
        }

        auto value = convert(args[i], declared->type, source);

        // A `->` parameter consumes what it is given, so the argument is moved out of its storage
        // - or copied, for a TrivialCopy type. The conversion comes first deliberately: a
        // converted argument is a temporary of the callee's type, and moving out of a temporary is
        // the no-op sinkValue() already reports it as.
        if(declared->convention == ast::BindType::Sink) value = sinkValue(value, source);

        /*
         * A `return` argument is loaned rather than merely read.
         *
         * The marker says a borrow in the result may be rooted here, so the loan has to outlive the
         * call: nothing may write this storage while the result is still live. Making that an
         * explicit InstBorrow is what puts the extent in front of the borrow checker, which
         * otherwise sees only a value passed and returns the storage to general use at the call.
         *
         * The mutable case already has one - `&` created it above - and this is deliberately the
         * immutable one only.
         *
         * And only for a parameter the callee receives by reference. A borrow and the thing
         * borrowed are the same machine value for a memory type - both are an address - so
         * substituting one for the other is free, which is what made this work without anyone
         * having to say so. A scalar is passed by value, so the substitution would hand the callee
         * the address of the caller's variable where it declared the value: `fn get(return self:
         * %a, index: k)` was reached with `&%Int` and added the index to the wrong pointer. There
         * is nothing to protect in that case either - the callee got a copy, and a borrow rooted in
         * a copy is what the return-root check calls invalid - so no loan is the right answer as
         * well as the working one.
         */
        if(declared->returnRoot && value && isMemoryType(global, declared->type)) {
            if(auto place = findPlace(value)) {
                value = borrowPlace(place.unwrap(), resolveBorrowType(module, declared->type, false),
                                    source, true);
            }
        }

        converted.push(value);
    }

    if(function_->intrinsic) {
        auto expanded = function_->intrinsic(*this, toBuffer(converted), function_->returnType, source, resultName);
        flushPackedBorrows(packed);
        return expanded;
    }

    function_->used = true;
    auto call = create<InstCall>(source, resultName, function_->returnType, callee);

    /*
     * The argument list stays positional, whatever any one argument turned out to be.
     *
     * Lowering pairs this list with the callee's parameters by index and decides there which
     * positions survive - a declared unit is left out, a declared *variable* that is unit here is
     * not, since the erased body it was compiled from still reads a position for it. Both of those
     * need the entry to be here to be counted, so a hole punched at argument `i` does not drop
     * argument `i`: it drops argument `i + 1`, and every one after it.
     *
     * A value carrying nothing is spelled as no value at all, so `f(2, {}, 3)` is exactly that hole.
     * It gets storage instead - which is what lowering makes for the erased case anyway, of the zero
     * bytes the type occupies - so that the position exists and carries its type. `unitValue()` in
     * the same argument reaches here as an ordinary value and always did, which is why only the
     * literal ever shifted a list.
     */
    for(Size i = 0; i < converted.size(); i++) {
        auto value = converted[i];

        if(!value && i < function_->args.size() &&
           isUnit(global, local[function_->args.get(local, i)]->declaredType())) {
            value = allocate(module.scalar.unit, source);
        }

        if(value) call->args.push(module.arena, value);
    }

    append(call);
    auto result = ref(call);

    // An aggregate result is returned through storage the caller provides, so it needs a local
    // for the same reason a constructed value does - see resolve/lower.cpp's Call case.
    if(isMemoryType(global, call->type)) {
        call->local = function.addLocal(module, call->type, resultName, result);
    }

    // The loan every `&` argument created ends with the call, so this is where a packed field is
    // told what the callee wrote - Design.md's tier 1.
    flushPackedBorrows(packed);
    return result;
}

/*
 * The selected class function, and the instance that served it.
 *
 * §1.2's second rule made concrete: this is the point of *decision*, so the reference recorded here
 * is the one an editor shows. `findClassFunctions` collected four candidates and recorded none of
 * them, because a call site showing all four would be showing something the program does not mean.
 *
 * The instance is the answer §1.2 calls the one hover most wants - which `Ord` served this
 * `compare` - and it is null exactly when the types that would decide are still variables here.
 */
void recordClassFunReference(ExprResolver& resolver, LocationId source, ClassMatch& match,
                             ModulePtr<ClassInstance> instance) {
    if(!resolver.context.index || source == kNullLocation || !match.typeClass) return;

    auto symbol = classFunSymbol(resolver.module, match.typeClass, match.index);

    // The result type at this occurrence, in the caller's terms rather than the class's.
    TypePtr type = nullptr;
    auto entry = resolver.global[match.typeClass]->functions.get(resolver.global, match.index);

    if(entry.fun) {
        type = substituteType(resolver.module, resolver.local[entry.fun]->returnType,
                              toBuffer(match.args), source);
    }

    recordReference(resolver.context, source, symbol, type, instance);
}

ModulePtr<Value> ExprResolver::emitCall(StringId callName, Buffer<ModulePtr<Value>> args, LocationId source, TypePtr target, StringId resultName, Buffer<Deferred> deferred, U32 nothing, LocationId nameSource) {
    /*
     * Three things are spelled as a null argument, and only one of them is a reason to stop.
     *
     * A failed argument is: something has reported on it already, and matching an overload set
     * against a type nobody worked out would report a second, worse diagnostic about a call the
     * author may not have got wrong. A deferred position is null on purpose - it is not a value yet.
     * And a value of unit type is null because that is how this resolver spells a value that carries
     * nothing, which `valueType` already answers `{}` for and which every overload rule below
     * therefore handles without knowing it was null.
     *
     * The third used to be caught here as the first, which made `f({})` resolve to nothing at all
     * for any generic `f` - silently, since the whole point of this guard is that the diagnostic was
     * already written. Which positions are that third kind is the call site's knowledge, not this
     * one's, so it arrives in `nothing`.
     */
    for(Size i = 0; i < args.length; i++) {
        if(args[i] || isDeferred(deferred, i)) continue;
        if(i < 32 && (nothing & (U32(1) << i))) continue;

        return nullptr;
    }

    // The name's own location where the call site knew one, so that what the index records lands
    // on the name - see resolveCall. A synthesized call has none, and then the enclosing
    // expression is the only location there is.
    auto lookupSource = nameSource != kNullLocation ? nameSource : source;

    ClassFunList candidates;
    findClassFunctions(module, callName, lookupSource, candidates);

    auto direct = findFunction(module, callName, lookupSource);

    /*
     * A borrow is transparent for reading, and this is where that has to be said.
     *
     * `convert` already reads one through wherever a type is expected - `p.a :: Int` for a
     * `&Int` field is a load and always was - so a borrow reaches an argument position untouched
     * only when nothing there asked for a particular type. Dispatch is exactly that position: `p.a
     * + p.b` binds `Num`'s variable to `&Int` and then looks for an instance of it, and nobody
     * writes `instance Num(&Int)`.
     *
     * Written as a fallback rather than as a rule in the matcher, and the difference is what it
     * cannot break. A parameter declared `&T` still takes a borrow, because the arguments are only
     * rewritten when *nothing at all* accepts them as they stand - so every call that resolves
     * today resolves the same way, and this only turns a diagnostic into a call. The alternative,
     * teaching the matcher that `&T` also matches `T`, makes the two candidates overlap and needs a
     * rule saying which wins.
     *
     * Reading through is a load, so what it produces is a value of the borrowed type: for a scalar
     * a register, for an aggregate the address it already was. Whether the result may then be
     * *stored* is not decided here - checkTransfer answers that, and answers it the same way for a
     * borrow reached like this as for one written out.
     */
    SmallArray<ModulePtr<Value>, 8> readThrough;

    auto borrowed = false;
    for(auto arg: args) borrowed = borrowed || (arg && isBorrow(global, valueType(arg)));

    if(borrowed) {
        auto accepted = direct && matchFunction(direct, args, target, source, deferred);

        /*
         * Matching is not enough: it is what fails *after* it that this is for.
         *
         * `Num`'s signature fits `+` with its variable bound to `&Int` - a class function is
         * declared over a variable, and a variable accepts anything - and the call dies at instance
         * selection, because nobody writes `instance Num(&Int)`. So the test has to be the one the
         * loop below makes: an instance was found, or the types are still this body's own variables
         * and the instance is decided later.
         */
        for(auto& candidate: candidates) {
            if(accepted) break;

            ClassMatch attempt;
            if(!matchClassFun(candidate, args, target, attempt, deferred)) continue;

            accepted = attempt.instance ||
                       attempt.args.contains([&](TypePtr argument) { return isGeneric(global, argument); });
        }

        if(!accepted) {
            for(Size i = 0; i < args.length; i++) {
                auto arg = args[i];
                auto type = arg ? valueType(arg) : nullptr;

                readThrough.push(type && isBorrow(global, type)
                    ? convert(arg, ((BorrowType*)global[type])->to, source)
                    : arg);
            }

            args = toBuffer(readThrough);
        }
    }

    // Committing to the plain function, once it is the candidate the call is being served by. Its
    // arity is checked here rather than in matchFunction because a mismatch has to be reported as
    // itself: "takes two arguments" says more than the list of types the class half accepts.
    auto emitPlain = [&]() -> ModulePtr<Value> {
        recordReference(context, nameSource, functionSymbol(module, direct));

        if(local[direct]->args.size() != args.length) {
            context.diagnostics.error("%@ takes %@ arguments but was given %@"_v, source, context.findName(callName),
                                      U32(local[direct]->args.size()), U32(args.length));
            return nullptr;
        }

        return local[direct]->gen ? emitGenericCall(direct, args, source, target, resultName, deferred)
                                  : emitDirectCall(direct, args, source, target, resultName, deferred);
    };

    // R5: a plain function is an ordinary member of the overload set, not a shadow over it. It wins
    // when it fits, which keeps "my definition beats the imported one" for the case that really
    // overlaps; when it doesn't fit, the class candidates are still reachable. Shadowing outright
    // meant that a module-level `fn and(a: Permissions, b: Permissions)` silently disabled
    // `Integral.and` for every Int in the module, reported as an argument-type error on a call the
    // author never touched.
    if(direct && (candidates.isEmpty() || matchFunction(direct, args, target, source, deferred))) {
        return emitPlain();
    }

    if(candidates.isEmpty()) {
        context.diagnostics.error("unknown function %@"_v, source, context.findName(callName));
        return nullptr;
    }

    ClassMatch selected;
    ClassMatch withoutInstance;
    auto selectedCount = 0;
    auto withoutInstanceCount = 0;

    // Matches on this function's own type variables. Which class a call is, and with which type
    // arguments, is still decided here and once; only the instance has to wait until the types
    // become concrete.
    SmallArray<ClassMatch, 4> undecided;

    // Every class that turned out to apply, kept only so an ambiguity can name them all.
    SmallArray<GlobalPtr<TypeClass>, 4> applicable;

    // A candidate the signature fit and a functional dependency did not, kept so the failure can be
    // reported as the missing constraint it is rather than as a call nothing accepts.
    GlobalPtr<TypeClass> undeclared = nullptr;

    for(auto& candidate: candidates) {
        ClassMatch match;

        if(!matchClassFun(candidate, args, target, match, deferred)) {
            if(match.undeclaredDependency && !undeclared) undeclared = candidate.typeClass;
            continue;
        }

        auto isUndecided = match.args.contains([&](TypePtr argument) { return isGeneric(global, argument); });

        if(isUndecided) {
            applicable.push(match.typeClass);
            undecided.push(::move(match));
        } else if(match.instance) {
            applicable.push(match.typeClass);
            if(!selectedCount) adopt(selected, match);
            selectedCount++;
        } else {
            if(!withoutInstanceCount) adopt(withoutInstance, match);
            withoutInstanceCount++;
        }
    }

    if(!selectedCount && undecided.isNotEmpty()) {
        // A requirement the signature already declared wins over one that would have to be
        // inferred, so writing the constraint out is also how an overloaded name is settled.
        auto env = functionGen(global, function);
        Size chosen = 0;
        Size declaredCount = 0;

        for(Size i = 0; env && i < undecided.size(); i++) {
            if(!hasClassRequirement(global, *env, undecided[i].typeClass, toBuffer(undecided[i].args))) continue;

            chosen = i;
            declaredCount++;
        }

        if(declaredCount > 1 || (!declaredCount && undecided.size() > 1)) {
            context.diagnostics.error(
                "ambiguous call to %@ - more than one class applies, and the types that would decide are not known here. Name one class here (%@), or declare which one this function requires"_v,
                source, context.findName(callName),
                describeQualified(context, global, callName, toBuffer(applicable)));
            return nullptr;
        }

        // Selected, but not yet decided *which instance* - the types are still this body's own
        // variables. The class function is the answer either way, and the instance is left null,
        // which is what §1.3 means by recording the generic answer.
        recordClassFunReference(*this, nameSource, undecided[chosen], nullptr);

        return emitGenericDispatch(undecided[chosen], args, source, resultName, deferred);
    }

    if(selectedCount > 1) {
        context.diagnostics.error("ambiguous call to %@ - more than one class instance applies. Name one class here (%@)"_v,
                                  source, context.findName(callName),
                                  describeQualified(context, global, callName, toBuffer(applicable)));
        return nullptr;
    }

    if(!selectedCount) {
        // Nothing in the class half of the overload set fits. A plain function of this name is then
        // the only candidate left, and its own diagnostic - which argument did not fit, and what it
        // was declared as - says more than the list of types the classes would not take.
        if(direct) return emitPlain();

        StringBuilder types;

        // The signature fit and the dependency had nothing to answer with. Naming the constraint is
        // the whole diagnostic: the call is right, and what is missing is the promise that gives
        // the determined parameter a name in this body.
        if(undeclared) {
            context.diagnostics.error("%@ needs to know what %@ determines here, and this function does not require it - declare the constraint, as `fn (%@(...)) %@(...)`"_v,
                                      source, context.findName(callName),
                                      context.findName(global[undeclared]->name),
                                      context.findName(global[undeclared]->name),
                                      context.findName(function.name));
            return nullptr;
        }

        if(withoutInstanceCount) {
            describeTypes(context, global, toBuffer(withoutInstance.args), types);

            context.diagnostics.error("no instance of %@ for (%@), required by %@"_v, source,
                                      context.findName(global[withoutInstance.typeClass]->name),
                                      types.view(), context.findName(callName));
        } else {
            TypeList given;
            auto broken = false;
            for(auto arg: args) {
                auto type = valueType(arg);
                if(global[type]->kind == Type::Error) broken = true;
                given.push(type);
            }

            // An argument that is already an error has had its diagnostic. Reporting that no
            // instance accepts it names the failure a second time, in terms of a type the author
            // never wrote - see the `<error>` in "no class function * accepts (Int, <error>)".
            if(broken) return nullptr;

            describeTypes(context, global, toBuffer(given), types);

            context.diagnostics.error("no class function %@ accepts (%@)"_v, source, context.findName(callName),
                                      types.view());
        }

        return nullptr;
    }

    if(!local[selected.instance]->functions.get(local, selected.index)) {
        context.diagnostics.error("instance of %@ does not implement %@"_v, source,
                                  context.findName(global[selected.typeClass]->name), context.findName(callName));
        return nullptr;
    }

    /*
     * A class member declared `iter fn` or `lens fn` is run rather than called.
     *
     * The signature is not desugared (see resolveSignature), so it fits an argument list with no
     * continuation in it and would otherwise reach an implementation that has one - which is an
     * arity mismatch reported against a parameter the author never wrote. What runs one is a `for`
     * loop for an iterator and a call site with a block under it for a lens, and both reach the
     * instance by their own route.
     */
    auto signature = local[global[selected.typeClass]->functions.get(global, selected.index).fun];

    if(signature && signature->funKind == ast::FunKind::Iter) {
        context.diagnostics.error("%@ is an `iter fn` of class %@, so it is run by a `for` loop rather than called - write `for x in %@(...)`"_v,
                                  source, context.findName(callName),
                                  context.findName(global[selected.typeClass]->name), context.findName(callName));
        return nullptr;
    }

    if(signature && signature->funKind == ast::FunKind::Lens) {
        context.diagnostics.error("%@ is a `lens fn` of class %@, and a class member declared as one has no call site yet - a lens call reaches its implementation by name, which a class function is not"_v,
                                  source, context.findName(callName),
                                  context.findName(global[selected.typeClass]->name));
        return nullptr;
    }

    recordClassFunReference(*this, nameSource, selected, selected.instance);

    return emitInstanceCall(module, selected.instance, toBuffer(selected.instanceArgs), selected.index,
                            args, source, target, resultName, deferred);
}

/*
 * Generic calls.
 */

ModulePtr<Value> ExprResolver::emitGenericDispatch(ClassMatch& match, Buffer<ModulePtr<Value>> args,
                                                   LocationId source, StringId resultName,
                                                   Buffer<Deferred> deferred) {
    auto env = functionGen(global, function);
    if(!env) {
        // Nothing outside a generic body has a type variable to be undecided about.
        context.diagnostics.error("internal: a class call was deferred outside a generic function"_v, source);
        return nullptr;
    }

    requireClass(module, function, match.typeClass, toBuffer(match.args), source);

    // Whatever the declared constraints imply, the dispatch itself needs a witness, and a witness
    // needs a slot. See GenEnv::dispatched.
    requireClassSlot(module, *env, match.typeClass, toBuffer(match.args), source);

    auto typeClass = global[match.typeClass];
    auto entry = typeClass->functions.get(global, match.index);
    auto signature = local[entry.fun];
    auto resultType = substituteType(module, signature->returnType, toBuffer(match.args), source);

    auto call = create<InstGenCall>(source, resultName, resultType, entry.fun, match.typeClass, match.index);
    for(auto argument: match.args) call->typeArgs.push(module.arena, argument);

    for(Size i = 0; i < args.length; i++) {
        auto parameter = local[signature->args.get(local, i)];

        // The instance is not known here, so there is nothing that can see through the argument:
        // a deferred one becomes the thunk whichever implementation is selected will call.
        if(parameter->isLazy()) {
            auto type = substituteType(module, parameter->lazyType, toBuffer(match.args), source);
            Deferred entry = i < deferred.length ? deferred[i] : Deferred();
            if(!entry.isSet()) entry = deferredValue(args[i], type);

            call->args.push(module.arena, makeThunk(entry, type, source));
            continue;
        }

        auto expected = substituteType(module, parameter->type, toBuffer(match.args), source);
        auto value = convert(args[i], expected, source);

        /*
         * A value carrying nothing is spelled as no value at all, and this list is positional - so
         * it gets storage rather than a hole, exactly as emitDirectCall gives one. The parameter is
         * declared at a *variable* of the class's context, which the erased body reads a position
         * for whatever that variable turned out to be here, so leaving the entry out would shift
         * every argument after it and hand the callee the wrong ones.
         *
         * Reached by `Try.fromExit` on a carrier whose skip carries nothing - `Maybe`'s `e` is `{}`
         * - which is the first call in the language to pass a unit through a class function's
         * generic position. The crash it caused was in the ownership walk, which reads every
         * argument's type and had no null to read.
         */
        if(!value && expected && isUnit(global, expected)) value = allocate(expected, source);

        if(value) call->args.push(module.arena, value);
    }

    append(call);
    auto result = ref(call);
    if(isMemoryType(global, resultType)) call->local = function.addLocal(module, resultType, resultName, result);

    return result;
}

/*
 * The erased call.
 *
 * Everything the callee needs to know about the types it was instantiated for travels as one
 * constant environment, built for exactly this argument list and interned. What the call itself
 * looks like is unchanged - the same arguments in the same order - because the environment goes in
 * a hidden leading position that only lowering ever names.
 *
 * Null when the environment could not be built, which today means the callee has a requirement no
 * witness exists for yet. The caller then specializes instead, which is always available for a
 * concrete argument list and is what keeps this a staged optimization rather than a cliff.
 */
ModulePtr<Value> ExprResolver::emitErasedCall(ModulePtr<Function> callee, Buffer<TypePtr> typeArgs,
                                              Buffer<ModulePtr<Value>> args, LocationId source,
                                              StringId resultName, Buffer<Deferred> deferred) {
    auto generic = local[callee];
    auto resultType = substituteType(module, generic->returnType, typeArgs, source);

    generic->used = true;
    generic->genericallyUsed = true;

    /*
     * Every argument's convention still applies.
     *
     * An erased call is a different *representation* of the same call, so the callee's conventions
     * mean exactly what they meant - a `&` parameter is still a mutable borrow of the caller's
     * storage, a `->` still consumes, and a `return` argument is still loaned for the result's
     * lifetime. Reading the value instead would hand a `&` parameter a copy, and the writes it made
     * would land somewhere the caller never looks.
     */
    auto packed = packedMark();

    ValueList converted;
    for(Size i = 0; i < args.length; i++) {
        auto declared = local[generic->args.get(local, i)];
        auto expected = substituteType(module, declared->type, typeArgs, source);

        if(declared->isLazy()) {
            auto lazyType = substituteType(module, declared->lazyType, typeArgs, source);
            Deferred entry = i < deferred.length ? deferred[i] : Deferred();
            if(!entry.isSet()) entry = deferredValue(args[i], lazyType);

            converted.push(makeThunk(entry, lazyType, source));
            continue;
        }

        if(declared->isMutableBorrow()) {
            converted.push(borrowArgument(args[i], expected, source, declared->returnRoot));
            continue;
        }

        auto value = convert(args[i], expected, source);
        if(declared->convention == ast::BindType::Sink) value = sinkValue(value, source);

        if(declared->returnRoot && value) {
            if(auto place = findPlace(value)) {
                value = borrowPlace(place.unwrap(), resolveBorrowType(module, expected, false), source, true);
            }
        }

        converted.push(value);
    }

    for(auto value: converted) {
        if(!value) return nullptr;
    }

    // The environment itself is filled in by prepareGenericCalls, once the whole program has been
    // resolved. It cannot be built here: a slot number comes from a finished context, and the
    // callee's context is still collecting requirements while its body is being resolved.
    auto call = create<InstGenCall>(source, resultName, resultType, callee, nullptr, 0);

    for(auto argument: typeArgs) call->typeArgs.push(module.arena, argument);
    for(auto value: converted) call->args.push(module.arena, value);

    append(call);
    auto result = ref(call);
    if(isMemoryType(global, resultType)) call->local = function.addLocal(module, resultType, resultName, result);

    flushPackedBorrows(packed);

    return result;
}

/*
 * Generating a generic intrinsic at the call site.
 *
 * A concrete intrinsic - Core's `Num(Int).+` - is a real function whose body an ordinary call
 * expands instead of calling. A generic one has no body at all: `fn (a) *(it: %a) -> a` is not
 * one operation but one per element type, so there is nothing to write down until the call says
 * which. The type arguments are therefore handed to the intrinsic through the substituted result
 * type, which is all any of them needs.
 */
ModulePtr<Value> ExprResolver::expandIntrinsic(ModulePtr<Function> callee, Buffer<TypePtr> typeArgs,
                                               Buffer<ModulePtr<Value>> args, LocationId source,
                                               StringId resultName, Buffer<Deferred> deferred) {
    auto generic = local[callee];
    auto resultType = substituteType(module, generic->returnType, typeArgs, source);

    // An intrinsic that takes a `&` parameter makes the borrow itself - see exchangedPlace in
    // core.cpp - so the loan it creates ends here, where the operation it was made for has been
    // emitted. `swap(&h.a, &h.b)` on two co-packed fields commits both, in order.
    auto mark = packedMark();
    ModulePtr<Value> result = nullptr;

    if(generic->deferredIntrinsic) {
        // The declared type of each deferred parameter, at the types this call decided. It is what
        // the argument is resolved and converted against when the expansion runs it.
        SmallArray<Deferred, 4> pending;

        for(Size i = 0; i < args.length; i++) {
            auto declared = local[generic->args.get(local, i)];
            Deferred entry = i < deferred.length ? deferred[i] : Deferred();

            if(!declared->isLazy()) {
                pending.push(Deferred());
                continue;
            }

            auto lazyType = substituteType(module, declared->lazyType, typeArgs, source);
            if(!entry.isSet()) entry = deferredValue(args[i], lazyType);
            entry.type = lazyType;
            pending.push(entry);
        }

        result = generic->deferredIntrinsic(*this, args, toBuffer(pending), resultType, source, resultName);
    } else {
        result = generic->intrinsic(*this, args, resultType, source, resultName);
    }

    flushPackedBorrows(mark);
    return result;
}

ModulePtr<Value> ExprResolver::emitGenericCall(ModulePtr<Function> callee, Buffer<ModulePtr<Value>> args,
                                               LocationId source, TypePtr target, StringId resultName,
                                               Buffer<Deferred> deferred) {
    auto generic = local[callee];
    auto calleeEnv = functionGen(global, *generic);

    if(!calleeEnv || generic->args.size() != args.length) {
        return emitDirectCall(callee, args, source, target, resultName, deferred);
    }

    TypeList bindings;
    for(Size i = 0; i < calleeEnv->types.size(); i++) bindings.push(nullptr);

    // The same one-directional rule the classes use: the arguments decide, and the expected
    // result only fills in what they left open.
    for(Size i = 0; i < args.length; i++) {
        if(isDeferred(deferred, i)) continue;

        auto declared = local[generic->args.get(local, i)]->declaredType();

        if(!bindPosition(declared, valueType(args[i]), bindings, true)) {
            /*
             * A fixed array where a growable one was asked for - Implementation-Containers.md §6's
             * "it is never a growable argument. The diagnostic says so directly: a fixed array
             * cannot be pushed to."
             *
             * Said here rather than left to the general message because the general message is
             * true and useless: `[Int *4]` does not fit `Array(a)` for a reason that is the whole
             * design - growth is nominal, so the operations that grow name the growable type - and
             * a reader who has just watched `[Int *4]` pass to five other `[Int]` functions needs
             * to be told which capability this one wanted instead of which types failed to unify.
             */
            if(fixedElement(module, valueType(args[i])) && isGrowableArray(module, declared)) {
                context.diagnostics.error("%@ cannot be passed to %@, which asks for a growable array - a fixed array holds exactly the elements its type names and cannot grow. Only the operations that grow say `Array`; everything that reads says `[T]` and accepts this"_v,
                                          source, describeType(context, global, valueType(args[i])),
                                          context.findName(generic->name));
                return nullptr;
            }

            /*
             * A `Chunked` container where a `[T]` was asked for - Implementation-Containers.md §5.
             *
             * Said here as well as in convert(), because the two positions fail in different places:
             * a concrete `[Int]` parameter reaches the conversion and reports there, while a `[a]`
             * one fails at the binding above and never has a slice type to convert to. The message
             * is the same one because the mistake is, and what fixes it is the parameter.
             */
            if(sliceElement(module, declared) && chunkedElement(module, valueType(args[i]))) {
                context.diagnostics.error("%@ is `Chunked` and not `Contiguous`, so it cannot be passed to %@, which asks for a slice - its elements are not one buffer, and flattening them would be a copy this position does not say it makes. A function that only reads elements should take `fn (Chunked(c, a)) f(xs: c)` instead, which this container satisfies"_v,
                                          source, describeType(context, global, valueType(args[i])),
                                          context.findName(generic->name));
                return nullptr;
            }

            context.diagnostics.error("argument %@ of %@ is %@, which does not fit %@"_v, source, U32(i + 1),
                                      context.findName(generic->name),
                                      describeType(context, global, valueType(args[i])),
                                      describeType(context, global, declared));
            return nullptr;
        }
    }

    if(target) {
        TypeList withTarget = bindings;

        if(bindPosition(generic->returnType, target, withTarget, false)) {
            for(Size i = 0; i < bindings.size(); i++) {
                if(!bindings[i] || isLiteral(global, bindings[i])) bindings[i] = withTarget[i];
            }
        }
    }

    /*
     * After the target, because a functional dependency is a promise about the instances and the
     * expected type is a wish about this one call. Where the two disagree, the instance wins and
     * the conversion the target wanted is reported where it fails, rather than silently selecting
     * an instance the dependency says does not serve these types.
     *
     * And after the settle of what is already decided, for the reason matchFunction gives: an
     * instance is looked up by these types, and a literal has none until it takes its default.
     */
    settleWithDependencies(*calleeEnv, bindings, source);

    for(Size i = 0; i < bindings.size(); i++) {
        // A specialization is made for concrete types, so a literal variable the call left open
        // settles to its default before it becomes one of them.
        bindings[i] = settleType(bindings[i]);
        if(bindings[i]) continue;

        context.diagnostics.error("cannot infer type argument %@ of %@ here - give the expected type"_v, source,
                                  context.findName(global[calleeEnv->types.get(global, i)]->name),
                                  context.findName(generic->name));
        return nullptr;
    }

    ValueList converted;
    for(Size i = 0; i < args.length; i++) {
        if(isDeferred(deferred, i)) {
            converted.push(nullptr);
            continue;
        }

        auto argument = local[generic->args.get(local, i)];
        auto wanted = substituteType(module, argument->declaredType(), toBuffer(bindings), source);

        /*
         * A `&` parameter's argument is left exactly as it was written.
         *
         * What it is handed is a *borrow* of the caller's storage, and creating that is
         * borrowArgument's - which happens once, in whichever call form this turns into. Converting
         * first would build a read-only temporary and then ask for a mutable borrow of it, which is
         * how `sort(&xs)` on an owned array reported that a `let &` binding was not mutable.
         */
        auto value = argument->isMutableBorrow() ? args[i] : convert(args[i], wanted, source);

        /*
         * The same positional rule emitDirectCall keeps, and the erased form needs it for a second
         * reason on top of the shift: lowering reads the *concrete* type off this argument to size
         * the storage it hands over, so an argument that is not here has no type to read. A
         * position declared as a type variable exists whatever it was substituted with - `{}`
         * included - which is exactly the case that arrives with nothing to put in it.
         */
        if(!value && isUnit(global, wanted)) value = allocate(module.scalar.unit, source);

        converted.push(value);
    }

    auto undecided = bindings.contains([&](TypePtr binding) { return isGeneric(global, binding); });

    if(!undecided) {
        // A generic intrinsic has nothing to specialize: what it means is generated here from the
        // types the call decided, so there is no body to clone and no function to call. This is
        // what keeps a pointer dereference one load rather than a call per element access.
        if(generic->intrinsic || generic->deferredIntrinsic) {
            return expandIntrinsic(callee, toBuffer(bindings), toBuffer(converted), source, resultName,
                                   deferred);
        }

        // Both forms are first-class outputs, and which one a concrete call site takes is a choice
        // rather than a property of the callee - see Program::Specialization. Taking the erased path
        // needs the body first, since the body is what collects the requirements the environment has
        // to supply.
        if(module.program.specialization == Program::Specialization::Generic &&
           resolveFunctionBody(*generic->module, *generic) &&
           genericBodyLowerable(module, callee)) {
            if(auto call = emitErasedCall(callee, toBuffer(bindings), toBuffer(converted), source, resultName,
                                          deferred)) {
                return call;
            }
        }

        auto specialized = instantiateFunction(module, callee, toBuffer(bindings), source);
        if(!specialized) return nullptr;

        return emitDirectCall(specialized, toBuffer(converted), source, target, resultName, deferred);
    }

    auto env = functionGen(global, function);
    if(!env) {
        context.diagnostics.error("internal: a generic call was deferred outside a generic function"_v, source);
        return nullptr;
    }

    // The callee's requirements become this function's, expressed in this function's variables:
    // whoever instantiates this one has to prove them, because nobody else can. Its body is
    // resolved first, since that is what collects the ones its signature did not declare - a
    // forward reference would otherwise inherit a shorter list than the callee really has.
    resolveFunctionBody(*generic->module, *generic);

    for(auto constraint: calleeEnv->classes.contents(global)) {
        if(!constraint.typeClass) continue;

        TypeList forwarded;
        for(auto argument: constraint.args.contents(global)) {
            forwarded.push(substituteType(module, argument, toBuffer(bindings), source));
        }

        requireClass(module, function, constraint.typeClass, toBuffer(forwarded), source);
    }

    auto resultType = substituteType(module, generic->returnType, toBuffer(bindings), source);
    auto call = create<InstGenCall>(source, resultName, resultType, callee, nullptr, 0);

    for(auto binding: bindings) call->typeArgs.push(module.arena, binding);
    for(auto value: converted) call->args.push(module.arena, value);

    append(call);
    auto result = ref(call);
    if(isMemoryType(global, resultType)) call->local = function.addLocal(module, resultType, resultName, result);

    return result;
}
