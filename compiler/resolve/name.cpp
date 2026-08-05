#include "name.h"
#include "index.h"

/*
 * The typed wrappers over search().
 *
 * Each one contributes only the rule that is specific to its kind of symbol - constructors can
 * be namespaced under their record, class functions can appear more than once - and leaves
 * module order, qualified prefixes, visibility and ambiguity to the traversal.
 */

bool Module::visible(const Import& import, StringId name) {
    if(import.include.isNotEmpty()) {
        auto included = false;
        for(auto symbol: import.include) {
            if(symbol == name) {
                included = true;
                break;
            }
        }

        if(!included) return false;
    }

    for(auto symbol: import.exclude) {
        if(symbol == name) return false;
    }

    return true;
}

TypePtr findType(Module& module, StringId name, LocationId source) {
    auto found = search<TypePtr>(module.context, module, name, source, [](Module& in, NameRef reference) -> TypePtr {
        if(reference.segments() != 1) return nullptr;

        auto type = in.namedTypes.get(reference.single());
        return type ? type.unwrap() : nullptr;
    });

    if(found) recordReference(module.context, source, typeSymbol(*found.module, *found, name), *found);
    return found ? *found : nullptr;
}

Maybe<TypeAlias*> findAlias(Module& module, StringId name, LocationId source) {
    auto found = search<TypeAlias*>(module.context, module, name, source, [](Module& in, NameRef reference) -> TypeAlias* {
        if(reference.segments() != 1) return nullptr;

        auto alias = in.aliases.get(reference.single());
        return alias ? &alias.unwrap() : nullptr;
    });

    if(found) recordReference(module.context, source, aliasSymbol(*found.module, **found));
    return found ? Just(*found) : Nothing();
}

// A constructor is reachable by its own name unless its record was declared `qualified`, and by
// `Record.Constructor` in either case.
Maybe<ConstructorRef> findConstructor(Module& module, StringId name, LocationId source) {
    auto global = *module.types;

    auto found = search<ConstructorRef>(module.context, module, name, source, [&](Module& in, NameRef reference) -> ConstructorRef {
        if(reference.segments() == 1) {
            auto constructor = in.constructors.get(reference.single());
            return constructor ? constructor.unwrap() : ConstructorRef {};
        }

        if(reference.segments() != 2) return {};

        auto type = in.namedTypes.get(reference.segment(0));
        if(!type || global[type.unwrap()]->kind != Type::Record) return {};

        auto record = (RecordType*)global[type.unwrap()];
        auto wanted = reference.segment(1);

        for(Size i = 0; i < record->constructors.size(); i++) {
            if(record->constructors.get(global, i).name == wanted) {
                return ConstructorRef { record->base(global), U16(i) };
            }
        }

        return {};
    });

    if(found && (*found).record) {
        recordReference(module.context, source, constructorSymbol(*found.module, *found));
    }

    return found && (*found).record ? Just(*found) : Nothing();
}

ModulePtr<Global> findGlobal(Module& module, StringId name, LocationId source) {
    auto found = search<ModulePtr<Global>>(module.context, module, name, source, [](Module& in, NameRef reference) -> ModulePtr<Global> {
        if(reference.segments() != 1) return nullptr;

        auto global_ = in.globals.get(reference.single());
        return global_ ? global_.unwrap() : nullptr;
    });

    if(found) recordReference(module.context, source, globalSymbol(module, *found),
                              (*module.arena)[*found]->type);
    return found ? *found : nullptr;
}

ModulePtr<Function> findFunction(Module& module, StringId name, LocationId source, LocationId occurrence) {
    auto found = search<ModulePtr<Function>>(module.context, module, name, source, [](Module& in, NameRef reference) -> ModulePtr<Function> {
        if(reference.segments() != 1) return nullptr;

        auto function = in.functions.get(reference.single());
        return function ? function.unwrap() : nullptr;
    });

    /*
     * Recorded here even though a call site may go on to select a class function instead - §1.2's
     * "record at the point of decision" cuts the other way for the *candidate*, not for the answer.
     * What makes it honest is that the selection records the reference it decided on at the same
     * location, and a later answer replaces an earlier one: see SemanticIndex::addReference.
     *
     * At `occurrence` rather than at `source`, because the two are not the same question. `source`
     * is where the lookup happens - what is visible, and where an ambiguity between two imports is
     * reported - and every lookup has one. An *occurrence* is a name someone wrote, and a
     * synthesized call has none: a pattern's `==` and a range subscript's `slice` are looked up at
     * the enclosing expression, which is a span the author wrote something else in.
     *
     * What that cost is a name in the index that is in no source file. `referenceAt` walks outwards
     * from the innermost node until something answers, so a cursor on the `1` of `xs[1..3]` - which
     * has no answer of its own - reached the subscript expression and was told `fn slice(self:
     * Flat(a), from: I64, to: I64)`, a signature the author never wrote and cannot go to. See
     * `sliced`/`inRange` in test/lsp/semantic, which asserts both halves.
     */
    if(found) recordReference(module.context, occurrence, functionSymbol(module, *found));
    return found ? *found : nullptr;
}

GlobalPtr<TypeClass> findClass(Module& module, StringId name, LocationId source) {
    auto found = search<GlobalPtr<TypeClass>>(module.context, module, name, source, [](Module& in, NameRef reference) -> GlobalPtr<TypeClass> {
        if(reference.segments() != 1) return nullptr;

        auto typeClass = in.classes.get(reference.single());
        return typeClass ? typeClass.unwrap() : nullptr;
    });

    if(found) recordReference(module.context, source, classSymbol(module, *found));
    return found ? *found : nullptr;
}

Maybe<U8> findPrecedence(Module& module, StringId name) {
    // Fixity is not a definition, so a missing one is not an error and two equal declarations do
    // not conflict; the traversal is used only to get the same module order as everything else.
    //
    // The precedence is carried one above what was declared, because search() reads a falsy result
    // as "not in this module" and 0 is a precedence a declaration may legitimately have - it is
    // where Core puts the compound assignments, below every other operator.
    auto found = search<U16>(module.context, module, name, kNullLocation, [](Module& in, NameRef reference) -> U16 {
        if(reference.segments() != 1) return 0;

        auto precedence = in.operatorPrecedence.get(reference.single());
        return precedence ? U16(precedence.unwrap()) + 1 : 0;
    });

    return found && *found ? Just(U8(*found - 1)) : Nothing();
}

// Unlike the lookups above, a class function name is deliberately allowed to be found more than
// once: two classes may both declare `show`, and only the argument and result types decide which
// was meant. Collecting all of them and letting selection reject the ones that do not fit is the
// difference between an ambiguity error and an overload.
void findClassFunctions(Module& module, StringId name, LocationId source, ClassFunList& target) {
    auto global = *module.types;

    auto collect = [&](Module& in, NameRef reference) {
        for(auto& candidate: in.classFunctions) {
            if(reference.segments() == 1) {
                if(candidate.name != reference.single()) continue;
            } else if(reference.segments() == 2) {
                // `Ord.compare` names one class's function explicitly.
                if(global[candidate.typeClass]->name != reference.segment(0)) continue;
                if(candidate.name != reference.segment(1)) continue;
            } else {
                continue;
            }

            auto duplicate = false;
            for(auto& existing: target) {
                if(existing.typeClass == candidate.typeClass && existing.index == candidate.index) {
                    duplicate = true;
                    break;
                }
            }

            if(!duplicate) target.push(candidate);
        }
    };

    collect(module, NameRef { &module.context.find(name), 0 });

    for(auto& import: module.imports) {
        NameRef reference { &module.context.find(name), 0 };
        auto alias = &module.context.find(import.localName);
        auto matchedAlias = reference.identifier->segmentCount > alias->segmentCount;

        if(matchedAlias) {
            for(U32 i = 0; i < alias->segmentCount; i++) {
                if(alias->getHash(i) != reference.identifier->getHash(i)) {
                    matchedAlias = false;
                    break;
                }
            }
        }

        if(matchedAlias) {
            reference.start = alias->segmentCount;
        } else if(import.qualified) {
            continue;
        }

        if(!Module::visible(import, reference.single())) continue;
        collect(*import.module, reference);
    }
}

void findInstances(Module& module, GlobalPtr<TypeClass> typeClass, Array<ModulePtr<ClassInstance>>& target) {
    auto local = *module.arena;

    auto collect = [&](Module& in) {
        for(auto instance: in.instances) {
            if(local[instance]->typeClass != typeClass) continue;

            auto duplicate = false;
            for(auto existing: target) {
                if(existing == instance) {
                    duplicate = true;
                    break;
                }
            }

            if(!duplicate) target.push(instance);
        }
    };

    /*
     * Every instance in the program, not only the ones this module imported.
     *
     * Instances are not named, so they are neither shadowed nor ambiguous: overlap between two of
     * them is resolved by argument types, and there is nothing for an import to disambiguate. What
     * an import decides is which *names* are reachable, which is untouched by this - `freeHeap` is
     * still unavailable to a module that did not ask for Native.
     *
     * Coherence is not a nicety here, it is what the rest of the compiler already assumes.
     * `ownershipOf` caches its answer on the *type*, and teardown glue is interned per type
     * program-wide - so "does this type have a `Drop`" has to have one answer, and a module-relative
     * search made it whichever module asked first. Both halves of Implementation-Containers.md's
     * container work hit that: an array literal gives a module that never imported Native a `Run(a)`
     * to reclaim, and `Array(Buffer)` is instantiated from inside Collections, where the program's
     * own `instance Drop(Buffer)` was not visible and its buffers were therefore never released.
     */
    for(auto entry: module.program.modules) collect(*entry);
}

/*
 * Selecting an instance.
 *
 * See matchInstance()'s comment in name.h for the rules; what follows is only how they are
 * checked. A parametric head is matched with matchType(), which is the same inference the rest of
 * the resolver uses, so nothing here knows anything about the shape of a head that resolveType()
 * did not already decide.
 */

// Bounds the recursive proof of a head's own constraints. A hierarchy that needs more than this
// is a declaration that proves itself; the depth is what stops the search rather than a visited
// set, for the same reason impliesClass() bounds its walk over superclasses.
static const U32 kProofDepth = 8;

static bool instanceApplies(Module& module, ClassInstance& instance, Buffer<TypePtr> args,
                            TypeList& bindings, U32 depth);

/*
 * The two classes nobody writes an instance of.
 *
 * TrivialCopy and TrivialSink hold for a type exactly when they hold for every one of its members,
 * which is a question the compiler already answers structurally and long before typeclass dispatch
 * exists. Rather than making them a special case at every site that asks, one instance is interned
 * per type the structural answer says yes for, so requirement proving, witness construction and
 * printing all see an ordinary instance of an ordinary class.
 *
 * A generic type is deliberately never answered: Design-Memory §2.1 fixes a generic body's
 * behaviour by its own signature, so an unconstrained `a` must not acquire TrivialCopy from the
 * type one call site happened to substitute. The constraint has to be declared, and a declared one
 * is proved through the context rather than through here.
 */
static ModulePtr<ClassInstance> structuralInstance(Module& module, GlobalPtr<TypeClass> typeClass,
                                                   Buffer<TypePtr> args) {
    auto& classes = module.coreClasses;
    if(!typeClass || args.length != 1) return nullptr;

    auto wantsCopy = typeClass == classes.trivialCopy;
    if(!wantsCopy && typeClass != classes.trivialSink) return nullptr;

    auto type = args[0];
    if(!type || isGeneric(*module.types, type)) return nullptr;

    auto ownership = ownershipOf(module, type);
    if(wantsCopy ? !ownership.trivialCopy : !ownership.trivialSink) return nullptr;

    auto key = (U64(U32(typeClass)) << 32) | U64(U32(type));
    auto& interned = module.program.structuralInstances;
    if(auto found = interned.get(key)) return found.unwrap();

    // Built in Core, where the class is, because it says nothing about any other module: what makes
    // it true is the shape of the type, which every module that can see the type agrees on.
    auto& core = *module.program.core;
    auto instance = new (core.arena) ClassInstance(typeClass);
    instance->module = &core;
    instance->forTypes.push(core.arena, type);

    auto pointer = instance - *core.arena;
    *interned.add(key).value = pointer;
    return pointer;
}

static InstanceMatch matchInstanceAt(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                                     U32 depth) {
    auto local = *module.arena;

    if(auto structural = structuralInstance(module, typeClass, args)) {
        return InstanceMatch { structural, {} };
    }

    Scratch<Array<ModulePtr<ClassInstance>>> candidates(module.program.instanceCandidates);
    findInstances(module, typeClass, *candidates);

    InstanceMatch best;

    // One list for every candidate rather than one each: all but the winning candidate's bindings
    // are discarded, and the winner's are copied out. A TypeList, so the ordinary case where a class
    // has one or two variables never reaches the heap at all.
    TypeList bindings;

    for(auto candidate: *candidates) {
        bindings.clear();
        if(!instanceApplies(module, *local[candidate], args, bindings, depth)) continue;

        // Overlap between two heads is resolved by specificity: a candidate wins only when the one
        // held so far is general enough to cover it, which is what lets a hand-written `Eq(%U8)`
        // stand in front of a blanket `Eq(Ptr(a))`. Two heads that overlap without either covering
        // the other - `Eq(Pair(Int, a))` and `Eq(Pair(b, Bool))` - would make the answer depend on
        // declaration order; rejecting that pair is a coherence check the declaration pass does not
        // make yet, and until it does the first match stands.
        if(best && !instanceCovers(module, *local[best.instance], *local[candidate])) continue;

        best.instance = candidate;
        replaceContents(best.args, bindings);
    }

    return best;
}

static bool instanceApplies(Module& module, ClassInstance& instance, Buffer<TypePtr> args,
                            TypeList& bindings, U32 depth) {
    auto global = *module.types;
    auto local = *module.arena;
    auto env = instance.gen ? global[instance.gen] : nullptr;

    /*
     * A head with no variables still cannot be compared by pointer equality, because a `@bits`
     * refinement is a distinct type that must dispatch as the type it refines: `instance Num(U64)`
     * has to answer `Num(Id)`. matchType() knows that, and the fast path here bypassed it - which
     * is exactly the kind of second, less careful copy of a rule that makes a feature work in one
     * place and not another.
     */
    if(!env) {
        if(instance.forTypes.size() != args.length) return false;

        Size index = 0;
        for(auto pattern: instance.forTypes.contents(local)) {
            auto arg = args[index++];
            if(arg && canonicalType(global, pattern) != canonicalType(global, arg)) return false;
        }

        return true;
    }

    if(instance.forTypes.size() != args.length) return false;

    for(Size i = 0; i < env->types.size(); i++) bindings.push(nullptr);

    Size index = 0;
    for(auto pattern: instance.forTypes.contents(local)) {
        /*
         * A null argument is a position the asker does not constrain, and it matches anything.
         *
         * That is `Try(m, a, e)`'s keying rule (Implementation-Semantics.md part 5) rather than a
         * convenience: `m` decides the other two, so a caller holding only `m` asks with the other
         * two empty and reads what the instance bound off the head afterwards. It stays sound
         * because the loop below still rejects an instance whose own variables the match left open,
         * so a position nothing constrained can only be one the *matched* positions already
         * determined - `instance Try(Maybe(a), a, {})` selected for `Maybe(Int)` has `a` decided by
         * its first argument, and one whose result type were free of its head would not apply here
         * at all.
         */
        auto arg = args[index++];
        if(!arg) continue;

        if(!matchType(global, pattern, arg, { bindings.pointer(), bindings.size() })) return false;
    }

    // A variable of the head that the match left open cannot be chosen by anything later, so the
    // instance does not apply. resolveInstance() rejects a head that can never bind one at all.
    for(auto binding: bindings) {
        if(!binding) return false;
    }

    if(env->classes.isEmpty()) return true;
    if(!depth) return false;

    for(auto constraint: env->classes.contents(global)) {
        if(!constraint.typeClass) continue;

        TypeList concrete;
        for(auto arg: constraint.args.contents(global)) {
            concrete.push(substituteType(module, arg, toBuffer(bindings), instance.source));
        }

        // A requirement over types that are still variables is nothing to look an instance up by.
        // It is left to the instantiation that makes them concrete, which asks this again.
        if(concrete.contains([&](TypePtr type) { return isGeneric(global, type); })) continue;

        // The instance's own head, for the types this match bound, is what is being decided here
        // rather than a further obligation - which is what lets one of its implementations use the
        // class it implements, as `!=` written in terms of `==` does.
        if(constraint.typeClass == instance.typeClass && sameTypes(toBuffer(concrete), args)) continue;

        if(!matchInstanceAt(module, constraint.typeClass, toBuffer(concrete), depth - 1)) return false;
    }

    return true;
}

bool instanceCovers(Module& module, ClassInstance& pattern, ClassInstance& other) {
    auto global = *module.types;
    auto local = *module.arena;
    if(pattern.forTypes.size() != other.forTypes.size()) return false;

    TypeList bindings;
    if(pattern.gen) {
        for(Size i = 0; i < global[pattern.gen]->types.size(); i++) bindings.push(nullptr);
    }

    Size index = 0;
    for(auto type: pattern.forTypes.contents(local)) {
        if(!matchType(global, type, other.forTypes.get(local, index++), { bindings.pointer(), bindings.size() })) {
            return false;
        }
    }

    return true;
}

InstanceMatch resolveDetermined(Module& module, GlobalPtr<TypeClass> typeClass, TypeList& args,
                                bool bindGeneric) {
    auto global = *module.types;
    auto local = *module.arena;
    if(!typeClass || !global[typeClass]->determines()) return {};

    auto determined = global[typeClass]->determined;
    if(determined >= args.size()) return {};

    for(U16 i = 0; i < determined; i++) {
        if(!args[i]) return {};

        /*
         * A determining position that is still a type variable does not select an instance for a
         * body being resolved, because a body's meaning is fixed by its own signature
         * (Design-Memory §2.1): a blanket `instance Elem(x -> x)` would otherwise answer for the
         * `c` of `fn (Elem(c, a)) f(self: c)` and commit the body to it, ignoring the instance a
         * caller's actual type has. What answers there is the declared constraint.
         *
         * A caller checking the *shape* of the instance rather than picking an implementation asks
         * with `bindGeneric` - which is a lens's declaration, where `Try(Maybe(a), ...)` has to
         * match while `a` is still the lens's own variable, and the same instance is selected again
         * per call site once it is not.
         *
         * The rule is about a *bare* variable, and only about one. `Array(a)` is generic and is
         * nonetheless a container the author named: its head is `Array`, one instance answers for
         * it, and what that instance determines - `Size` and `a` - is the same answer whatever `a`
         * turns out to be. Refusing it would mean `fn (a) first(xs: Array(a)) -> a = xs[0]` had to
         * declare `Index(Array(a), Size, a)` to index a type it wrote out in full, which is a
         * constraint about nothing the signature left open. The blanket instance the paragraph
         * above is guarding against still cannot answer here, because a bare `c` still cannot
         * select.
         */
        if(!bindGeneric && global[args[i]]->kind == Type::Gen) return {};
    }

    TypeList asked;
    for(Size i = 0; i < args.size(); i++) asked.push(i < determined ? args[i] : nullptr);

    auto match = matchInstanceAt(module, typeClass, toBuffer(asked), kProofDepth);
    if(!match) return {};

    // What the head put in each determined position, under the bindings selecting it made -
    // `instance Contiguous(Array(a) -> a)` selected for `Array(Int)` answers `Int` rather than `a`.
    auto instance = local[match.instance];
    if(instance->forTypes.size() != args.size()) return {};

    for(Size i = determined; i < args.size(); i++) {
        args[i] = substituteType(module, instance->forTypes.get(local, i), toBuffer(match.args), instance->source);
    }

    return match;
}

// One ordering of the dependency check: whether `pattern`'s determining positions cover `other`'s
// while the two disagree about what they determine.
static bool dependencyConflict(Module& module, ClassInstance& pattern, ClassInstance& other, U16 determined) {
    auto global = *module.types;
    auto local = *module.arena;

    TypeList bindings;
    if(pattern.gen) {
        for(Size i = 0; i < global[pattern.gen]->types.size(); i++) bindings.push(nullptr);
    }

    for(U16 i = 0; i < determined; i++) {
        if(!matchType(global, pattern.forTypes.get(local, i), other.forTypes.get(local, i),
                      { bindings.pointer(), bindings.size() })) {
            return false;
        }
    }

    // The determining halves describe the same types, so the determined halves have to as well -
    // under the bindings that match made, since `C([a] -> a)` promises its element is whatever the
    // container's is rather than any fixed type.
    for(Size i = determined; i < pattern.forTypes.size(); i++) {
        auto promised = substituteType(module, pattern.forTypes.get(local, i), toBuffer(bindings), pattern.source);
        if(!sameType(promised, other.forTypes.get(local, i))) return true;
    }

    return false;
}

bool breaksDependency(Module& module, ClassInstance& pattern, ClassInstance& other) {
    auto typeClass = (*module.types)[pattern.typeClass];
    if(!typeClass->determines()) return false;

    auto determined = typeClass->determined;
    if(pattern.forTypes.size() != other.forTypes.size()) return false;
    if(determined >= pattern.forTypes.size()) return false;

    return dependencyConflict(module, pattern, other, determined)
        || dependencyConflict(module, other, pattern, determined);
}

InstanceMatch matchInstance(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args) {
    return matchInstanceAt(module, typeClass, args, kProofDepth);
}

ModulePtr<ClassInstance> findInstance(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args) {
    return matchInstance(module, typeClass, args).instance;
}
