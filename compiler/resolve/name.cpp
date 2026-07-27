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
    auto found = search<U16>(module.context, module, name, kNullLocation, [](Module& in, NameRef reference) -> U16 {
        if(reference.segments() != 1) return 0;

        auto precedence = in.operatorPrecedence.get(reference.single());
        return precedence ? U16(precedence.unwrap()) : 0;
    });

    return found && *found ? Just(U8(*found)) : Nothing();
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

ModulePtr<ClassInstance> findInstance(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args) {
    auto local = *module.arena;
    Array<ModulePtr<ClassInstance>> candidates;
    findInstances(module, typeClass, candidates);

    for(auto candidate: candidates) {
        if(sameTypes(local[candidate]->forTypes, local, args)) return candidate;
    }

    return nullptr;
}
