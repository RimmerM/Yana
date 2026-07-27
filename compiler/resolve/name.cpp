#include "name.h"

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

    return found ? *found : nullptr;
}

Maybe<TypeAlias*> findAlias(Module& module, StringId name, LocationId source) {
    auto found = search<TypeAlias*>(module.context, module, name, source, [](Module& in, NameRef reference) -> TypeAlias* {
        if(reference.segments() != 1) return nullptr;

        auto alias = in.aliases.get(reference.single());
        return alias ? &alias.unwrap() : nullptr;
    });

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

    return found && (*found).record ? Just(*found) : Nothing();
}

ModulePtr<Global> findGlobal(Module& module, StringId name, LocationId source) {
    auto found = search<ModulePtr<Global>>(module.context, module, name, source, [](Module& in, NameRef reference) -> ModulePtr<Global> {
        if(reference.segments() != 1) return nullptr;

        auto global_ = in.globals.get(reference.single());
        return global_ ? global_.unwrap() : nullptr;
    });

    return found ? *found : nullptr;
}

ModulePtr<Function> findFunction(Module& module, StringId name, LocationId source) {
    auto found = search<ModulePtr<Function>>(module.context, module, name, source, [](Module& in, NameRef reference) -> ModulePtr<Function> {
        if(reference.segments() != 1) return nullptr;

        auto function = in.functions.get(reference.single());
        return function ? function.unwrap() : nullptr;
    });

    return found ? *found : nullptr;
}

GlobalPtr<TypeClass> findClass(Module& module, StringId name, LocationId source) {
    auto found = search<GlobalPtr<TypeClass>>(module.context, module, name, source, [](Module& in, NameRef reference) -> GlobalPtr<TypeClass> {
        if(reference.segments() != 1) return nullptr;

        auto typeClass = in.classes.get(reference.single());
        return typeClass ? typeClass.unwrap() : nullptr;
    });

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
void findClassFunctions(Module& module, StringId name, LocationId source, Array<ClassFunRef>& target) {
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

    // Instances are not named, so they are neither shadowed nor ambiguous: every one that is
    // visible participates, and overlap between two of them is resolved by argument types.
    collect(module);
    for(auto& import: module.imports) collect(*import.module);
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
                            Array<TypePtr>& bindings, U32 depth);

static InstanceMatch matchInstanceAt(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                                     U32 depth) {
    auto local = *module.arena;
    Array<ModulePtr<ClassInstance>> candidates;
    findInstances(module, typeClass, candidates);

    InstanceMatch best;

    for(auto candidate: candidates) {
        Array<TypePtr> bindings;
        if(!instanceApplies(module, *local[candidate], args, bindings, depth)) continue;

        // Overlap between two heads is resolved by specificity: a candidate wins only when the one
        // held so far is general enough to cover it, which is what lets a hand-written `Eq(%U8)`
        // stand in front of a blanket `Eq(Ptr(a))`. Two heads that overlap without either covering
        // the other - `Eq(Pair(Int, a))` and `Eq(Pair(b, Bool))` - would make the answer depend on
        // declaration order; rejecting that pair is a coherence check the declaration pass does not
        // make yet, and until it does the first match stands.
        if(best && !instanceCovers(module, *local[best.instance], *local[candidate])) continue;

        best.instance = candidate;
        best.args = ::move(bindings);
    }

    return best;
}

static bool instanceApplies(Module& module, ClassInstance& instance, Buffer<TypePtr> args,
                            Array<TypePtr>& bindings, U32 depth) {
    auto global = *module.types;
    auto local = *module.arena;
    auto env = instance.gen ? global[instance.gen] : nullptr;

    if(!env) return sameTypes(instance.forTypes, local, args);
    if(instance.forTypes.size() != args.length) return false;

    for(Size i = 0; i < env->types.size(); i++) bindings.push(nullptr);

    Size index = 0;
    for(auto pattern: instance.forTypes.contents(local)) {
        if(!matchType(global, pattern, args[index++], { bindings.pointer(), bindings.size() })) return false;
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

        Array<TypePtr> concrete;
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

    Array<TypePtr> bindings;
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

InstanceMatch matchInstance(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args) {
    return matchInstanceAt(module, typeClass, args, kProofDepth);
}

ModulePtr<ClassInstance> findInstance(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args) {
    return matchInstance(module, typeClass, args).instance;
}
