#include "gen.h"
#include "own.h"

namespace mir {

StringView genSlotKindName(GenSlotKind kind) {
    switch(kind) {
        case GenSlotKind::Type:     return "type"_v;
        case GenSlotKind::Const:    return "const"_v;
        case GenSlotKind::Class:    return "class"_v;
        case GenSlotKind::Property: return "property"_v;
        case GenSlotKind::Function: return "function"_v;
        case GenSlotKind::Count: break;
    }

    return "?"_v;
}

// Whether two applied argument lists are the same list. Handle equality per position, which is what
// type equality is here: the type table interns, so two spellings of one type are one handle.
static bool sameArguments(const DeclPtr<Type>* left, Size leftCount, const DeclPtr<Type>* right,
                          Size rightCount) {
    if(leftCount != rightCount) return false;
    for(Size i = 0; i < leftCount; i++) {
        if(left[i] != right[i]) return false;
    }

    return true;
}

/*
 * The superclass walk, which is where the substitution happens - see `classProves`.
 *
 * A walk rather than a precomputed closure, because the sets are two or three entries and a cached
 * closure is a second answer that has to be invalidated. A cycle cannot be built - `declareClass`
 * refuses a class as its own superclass and a superclass has to be declared before the class that
 * names it - so the recursion terminates on the declaration order.
 */
bool classProves(Context& context, Program& program, DeclPtr<Class> have,
                 const DeclPtr<Type>* haveArgs, Size haveCount, DeclPtr<Class> want,
                 const DeclPtr<Type>* wantArgs, Size wantCount) {
    if(!have || !want) return false;

    auto base = *program.decl;

    if(have == want && sameArguments(haveArgs, haveCount, wantArgs, wantCount)) return true;

    /*
     * What `have`'s own head says its superclasses are applied to, read through this application.
     *
     * `class (Eq(a)) Ord(a)` holds `Eq` applied to `Ord`'s variable, so holding `Ord(T)` is holding
     * `Eq(T)` - and the substitution is what turns the first into the second. Written once here
     * because it is the only place a superclass is *used*; an instance head checking that its
     * superclasses are present asks this same function.
     */
    Substitution with { base[have]->gen, (DeclPtr<Type>*)haveArgs, haveCount };

    for(auto super: base[have]->superclasses.contents(base)) {
        SmallArray<DeclPtr<Type>, 4> arguments;
        for(auto argument: super.args.contents(base)) {
            arguments.push(substituteType(context, program, argument, with));
        }

        if(classProves(context, program, super.klass, arguments.pointer(), arguments.size(), want,
                       wantArgs, wantCount)) {
            return true;
        }
    }

    return false;
}

bool satisfiesRequirement(Context& context, Program& program, DeclPtr<GenContext> gen,
                          DeclPtr<Class> klass, const DeclPtr<Type>* args, Size count) {
    if(!gen) return false;

    auto base = *program.decl;

    for(auto requirement: base[gen]->classes.contents(base)) {
        auto held = requirementArgs(base, requirement);
        if(classProves(context, program, requirement.klass, held.data, held.count, klass, args,
                       count)) {
            return true;
        }
    }

    return false;
}

DeclPtr<Type> findGenVariable(DeclBase base, DeclPtr<GenContext> context, StringId name) {
    if(!context) return nullptr;

    for(auto variable: base[context]->variables.contents(base)) {
        if(((VariableType*)base[variable])->name == name) return variable;
    }

    return nullptr;
}

DeclPtr<Type> genVariable(Program& program, DeclPtr<GenContext> context, StringId name,
                          LocationId source, DeclPtr<Type> constType) {
    auto base = *program.decl;
    if(auto found = findGenVariable(base, context, name)) return found;
    if(!context || !base[context]->open) return nullptr;

    auto& into = *base[context];
    auto index = U16(into.variables.size());
    auto variable = new (program.decl) VariableType(context, index, name, source);
    variable->constType = constType;

    into.variables.push(program.decl, asBase<Type>(variable - base));
    return asBase<Type>(variable - base);
}

void closeContext(DeclBase base, DeclPtr<GenContext> context) {
    if(context) base[context]->open = false;
}

/*
 * The canonical numbering, derived once per context.
 *
 * Five groups, in Implementation-Generics.md part 2's order: the declared **type** variables, the
 * declared **const** variables, then the **class**, **property** and **callable** requirements, each
 * in the order it was written. The two fixed-width groups are first and `typeCount`/`constCount` are
 * their lengths, so a caller filling an environment writes the prefix it can compute without
 * building anything before it reaches the first witness.
 *
 * Five loops rather than a copy of five lists, because what is being stated is an *order*: a slot
 * number is shared between a caller that writes it and a callee that reads it, and the one thing
 * that must not vary is which group a row lands in.
 */
static void freezeSchema(Program& program, DeclPtr<GenContext> context) {
    auto base = *program.decl;
    auto& into = *base[context];

    /*
     * Already numbered, which is reached rather than an error: a class's context is the context of
     * every one of its members, so the walk below finds it once per member and once for the class.
     *
     * The invariant is one numbering *per context*, and an early return is exactly that. What it is
     * not is a second numbering, which is what the split of `GenContext` from `GenSchema` exists to
     * make unrepresentable.
     */
    if(into.schema) return;

    auto schema = new (program.decl) GenSchema();

    // The parameter list is one list holding both kinds - see `VariableType::constType` - so the two
    // groups are two passes over it rather than two lists. A pass that filtered once and tagged
    // would be the same walk with the group decided by the row instead of by the numbering.
    for(auto variable: into.variables.contents(base)) {
        if(((VariableType*)base[variable])->isConst()) continue;

        GenSlot slot;
        slot.kind = GenSlotKind::Type;
        slot.type = variable;
        schema->slots.push(program.decl, slot);
    }

    schema->typeCount = U16(schema->slots.size());

    for(auto variable: into.variables.contents(base)) {
        if(!((VariableType*)base[variable])->isConst()) continue;

        GenSlot slot;
        slot.kind = GenSlotKind::Const;
        slot.type = variable;
        schema->slots.push(program.decl, slot);
    }

    schema->constCount = U16(schema->slots.size() - schema->typeCount);

    // The witnesses, after the fixed-width prefix: one per declared requirement, in source order.
    // Superclasses are deliberately *not* slots of their own - a witness references the ones it
    // implies rather than the environment carrying each twice.
    schema->classStart = U16(schema->slots.size());

    for(auto requirement: into.classes.contents(base)) {
        GenSlot slot;
        slot.kind = GenSlotKind::Class;
        slot.klass = requirement.klass;
        for(auto argument: requirement.args.contents(base)) {
            slot.args.push(program.decl, argument);
        }

        schema->slots.push(program.decl, slot);
    }

    schema->propertyStart = U16(schema->slots.size());

    for(auto property: into.properties.contents(base)) {
        GenSlot slot;
        slot.kind = GenSlotKind::Property;
        slot.type = property.subject;
        slot.name = property.field;
        schema->slots.push(program.decl, slot);
    }

    schema->functionStart = U16(schema->slots.size());

    for(auto callable: into.callables.contents(base)) {
        GenSlot slot;
        slot.kind = GenSlotKind::Function;
        slot.name = callable.name;
        slot.type = callable.result;
        for(auto argument: callable.args.contents(base)) slot.args.push(program.decl, argument);
        schema->slots.push(program.decl, slot);
    }

    into.schema = schema - base;
}

void freezeSchemas(Program& program) {
    auto base = *program.decl;

    // The classes first, though the order does not matter: a class's context is reached from its
    // members too, and freezing is by context rather than by owner.
    for(auto klass: program.classes) {
        if(base[klass]->gen) freezeSchema(program, base[klass]->gen);
    }

    for(auto modulePointer: program.modules) {
        auto module = base[modulePointer];

        for(auto recordPointer: module->records.contents(base)) {
            auto& record = *base[recordPointer];

            // An instantiation has no context of its own: its parameters are bound, and what it
            // holds instead is the argument list that bound them.
            if(record.gen) freezeSchema(program, record.gen);
        }

        for(auto functionPointer: module->functions.contents(base)) {
            auto& function = *base[functionPointer];
            if(function.gen) freezeSchema(program, function.gen);
        }
    }
}

bool isConcreteType(DeclBase base, DeclPtr<Type> type) {
    if(!type) return true;

    switch(base[type]->kind) {
        case Type::Variable:
            return false;
        case Type::Pointer:
            return isConcreteType(base, ((PointerType*)base[type])->to);
        case Type::Fun: {
            // Every position, because a function type is its positions: `(a) -> I32` is no more
            // concrete than `a` is, and it is the one kind whose variables are all *inside* it.
            auto& fun = *(FunType*)base[type];
            for(auto param: fun.params.contents(base)) {
                if(!isConcreteType(base, param.occurrence.type)) return false;
            }

            return isConcreteType(base, fun.result.type);
        }
        case Type::Record: {
            // The *arguments*, not the fields. An instantiation's fields are what its arguments
            // substituted into, so a walk over them would be the same question asked one level
            // deeper and would not terminate on a recursive generic.
            auto& record = *(RecordType*)base[type];
            for(auto argument: record.arguments.contents(base)) {
                if(!isConcreteType(base, argument)) return false;
            }

            // A generic *declaration* is not a type a value has: its parameters are unbound, which
            // is the same answer a bare variable gives for the same reason.
            return record.gen == nullptr;
        }
        default:
            return true;
    }
}

/*
 * A signature read as a type - the note in gen.h.
 *
 * The parameters' occurrences in order and the result's, handed to `Types::funType` unchanged: what
 * a declared position asks of what fills it is exactly what a value of the type asks, and a reading
 * that normalized anything here would be a value that accepts calls the declaration does not.
 *
 * A **lens** is refused along with a generic, and for a related reason: a lens has no standalone
 * body to point at - `Function::lens` says its body is resolved into its caller - so there is no
 * code word for a value of it to hold. Milestone 7's CPS lowering is what gives it one.
 */
DeclPtr<Type> funTypeOf(Context& context, Program& program, DeclPtr<Function> function) {
    auto base = *program.decl;
    auto& declared = *base[function];

    if(isGeneric(base, declared.gen) || declared.lens) return nullptr;

    /*
     * A lifted lambda's **environment** is argument zero and is not one of the type's positions.
     *
     * `Function::environment` is what says the position is there, and skipping it here is the only
     * thing any reader of a closure's signature has to do differently: the value's type is the one
     * the lambda was written with, and the environment is what the pair's second word supplies at
     * every call across it.
     */
    SmallArray<FunParam, 4> params;
    auto first = declared.environment ? U32(1) : U32(0);

    for(U32 i = first; i < declared.args.size(); i++) {
        params.push(FunParam { base[declared.args.get(base, i)]->occurrence });
    }

    return program.types.funType(base, params.pointer(), params.size(), declared.result);
}

DeclPtr<Type> countType(DeclBase base, DeclPtr<Type> type) {
    if(!type) return nullptr;

    auto& written = *base[type];
    if(written.kind == Type::Const) return ((ConstType&)written).type;
    if(written.kind == Type::Variable) return ((VariableType&)written).constType;
    return nullptr;
}

bool isCountType(DeclBase base, DeclPtr<Type> type) { return countType(base, type) != nullptr; }

/*
 * Every argument of one application against the parameter it fills - a count where the head declared
 * one, a type where it did not.
 *
 * One function with four callers, which is every position that applies a head by *writing* an
 * argument list: a record, an alias, a class requirement and an instance head. `matchType` asks the
 * same question from the other end, where a pattern meets a concrete type, and the two are separate
 * for a reason - this one has a written position to name in the message, and that one has none.
 *
 * The arity is not checked here. It is one message per application and the applier already has it,
 * so a count here would be the same mistake reported twice.
 */
bool checkArgumentKinds(Context& context, DeclBase base, DeclPtr<GenContext> owner, StringId name,
                        const DeclPtr<Type>* args, Size count, LocationId source) {
    if(!owner) return true;

    auto& head = *base[owner];
    auto arity = head.variables.size();
    auto ok = true;

    for(Size i = 0; i < count && i < arity; i++) {
        auto& parameter = *(VariableType*)base[head.variables.get(base, i)];
        if(parameter.isConst() == isCountType(base, args[i])) continue;

        if(parameter.isConst()) {
            context.diagnostics.error("%@ of %@ counts, and this names a type"_v, source,
                                      context.findName(parameter.name), context.findName(name));
        } else {
            context.diagnostics.error("%@ of %@ is a type, and this names a number"_v, source,
                                      context.findName(parameter.name), context.findName(name));
        }

        ok = false;
    }

    return ok;
}

bool fillDefaults(Context& context, Program& program, DeclPtr<GenContext> owner,
                  SmallArray<DeclPtr<Type>, 4>& arguments) {
    auto base = *program.decl;
    auto& into = *base[owner];
    auto arity = into.variables.size();

    if(arguments.size() >= arity) return arguments.size() == arity;

    while(arguments.size() < arity) {
        auto declared = into.defaults.get(base, arguments.size());
        if(!declared) return false;

        // Substituted through the arguments *so far*, which is what makes `Pair(a, b = a)` fill `b`
        // with what `a` turned out to be. The substitution is partial by construction - the
        // positions after this one are still empty - and `substituteType` leaves those alone.
        Substitution with { owner, arguments.pointer(), arguments.size() };
        arguments.push(substituteType(context, program, declared, with));
    }

    return true;
}

void markVariables(DeclBase base, DeclPtr<GenContext> owner, DeclPtr<Type> type,
                   SmallArray<bool, 4>& into) {
    if(!type) return;

    auto& written = *base[type];

    switch(written.kind) {
        case Type::Variable: {
            auto& variable = (VariableType&)written;
            if(variable.owner == owner && variable.index < into.size()) into[variable.index] = true;
            return;
        }
        case Type::Pointer:
            markVariables(base, owner, ((PointerType&)written).to, into);
            return;
        case Type::Fun: {
            auto& fun = (FunType&)written;
            for(auto param: fun.params.contents(base)) {
                markVariables(base, owner, param.occurrence.type, into);
            }

            markVariables(base, owner, fun.result.type, into);
            return;
        }
        case Type::Record:
            for(auto argument: ((RecordType&)written).arguments.contents(base)) {
                markVariables(base, owner, argument, into);
            }

            return;
        default:
            return;
    }
}

// One position matched against what stood opposite it - `matchType` in `gen.h`.
bool matchType(DeclBase base, DeclPtr<GenContext> owner, SmallArray<DeclPtr<Type>, 4>& into,
               DeclPtr<Type> pattern, DeclPtr<Type> concrete) {
    if(!pattern || !concrete) return false;

    auto& written = *base[pattern];

    if(written.kind == Type::Variable && ((VariableType&)written).owner == owner) {
        auto& variable = (VariableType&)written;

        /*
         * A const parameter binds a *number* and a type parameter binds a type, and neither binds
         * the other.
         *
         * Checked here rather than at each position that supplies an argument, because this is the
         * one place a parameter meets what stood opposite it: an application writes its arguments
         * in order, an instance head is matched against a call's, and a call infers from what was
         * passed - three positions, one question. The number's own type has to agree as well,
         * which is the half `sameType` on the argument alone would not catch: `Buf(4 :: I64)` at
         * `data Buf(n: I32)` is a number of the wrong kind, not a number of the wrong value.
         */
        if(variable.isConst() != isCountType(base, concrete)) return false;
        if(variable.isConst() && countType(base, concrete) != variable.constType) return false;

        auto index = variable.index;
        if(!into[index]) into[index] = concrete;
        return sameType(into[index], concrete);
    }

    if(sameType(pattern, concrete)) return true;

    auto& supplied = *base[concrete];
    if(written.kind != supplied.kind) return false;

    switch(written.kind) {
        case Type::Pointer:
            return matchType(base, owner, into, ((PointerType&)written).to,
                             ((PointerType&)supplied).to);
        case Type::Fun: {
            /*
             * Position by position, and the **contracts have to agree exactly**.
             *
             * The types are matched because that is what binds a variable, and the contracts are
             * compared because they are not patterns: a parameter that owns what it is handed and
             * one that borrows it accept different calls, so a pattern that ignored the difference
             * would bind `a` from a shape the value does not have. `sameType` above already let an
             * identical pair through, so what reaches here differs in at least one type.
             */
            auto& expected = (FunType&)written;
            auto& actual = (FunType&)supplied;

            if(expected.params.size() != actual.params.size()) return false;
            if(expected.result.contract != actual.result.contract) return false;
            if(expected.result.shape != actual.result.shape) return false;

            for(Size i = 0; i < expected.params.size(); i++) {
                auto pattern = expected.params.get(base, i).occurrence;
                auto against = actual.params.get(base, i).occurrence;

                if(pattern.contract != against.contract || pattern.shape != against.shape) return false;
                if(!matchType(base, owner, into, pattern.type, against.type)) return false;
            }

            return matchType(base, owner, into, expected.result.type, actual.result.type);
        }
        case Type::Record: {
            // Two instantiations of one declaration match argument by argument. Two of *different*
            // declarations do not match at all, whatever their fields look like - which is what
            // nominal identity means and is why the fields are not walked.
            auto& expected = (RecordType&)written;
            auto& actual = (RecordType&)supplied;

            if(!expected.instanceOf || expected.instanceOf != actual.instanceOf) return false;
            if(expected.arguments.size() != actual.arguments.size()) return false;

            for(Size i = 0; i < expected.arguments.size(); i++) {
                if(!matchType(base, owner, into, expected.arguments.get(base, i),
                              actual.arguments.get(base, i))) {
                    return false;
                }
            }

            return true;
        }
        default:
            return false;
    }
}

DeclPtr<Type> substituteType(Context& context, Program& program, DeclPtr<Type> type,
                             const Substitution& with) {
    if(!type || with.isEmpty()) return type;

    auto base = *program.decl;

    switch(base[type]->kind) {
        case Type::Variable: {
            auto& variable = *(VariableType*)base[type];

            // A variable of an enclosing declaration passes through. That is what keeps a
            // specialization of one function from renumbering another's environment, and it is the
            // whole of why a substitution names the context it is for.
            if(variable.owner != with.owner) return type;

            assertTrue(variable.index < with.count);

            /*
             * A **partial** substitution leaves what it has not decided alone.
             *
             * That is what lets one function serve the call site's inference - where the arguments
             * are bound one parameter at a time and a later position is substituted with whatever
             * the earlier ones settled - and the finished substitution, where nothing is null. A
             * second "substitute what is known" function would be the same walk with one case
             * different, which is how two of them come to disagree about the rest.
             */
            auto argument = with.arguments[variable.index];
            return argument ? argument : type;
        }
        case Type::Pointer: {
            auto to = ((PointerType*)base[type])->to;
            auto substituted = substituteType(context, program, to, with);
            return substituted == to ? type : program.types.pointerType(substituted);
        }
        case Type::Fun: {
            // The positions substituted and the contracts carried through unchanged: what a
            // substitution supplies is types, and a position's convention was decided where the
            // shape was written.
            auto& fun = *(FunType*)base[type];

            SmallArray<FunParam, 4> params;
            auto moved = false;

            for(auto param: fun.params.contents(base)) {
                auto substituted = substituteType(context, program, param.occurrence.type, with);
                moved |= substituted != param.occurrence.type;
                params.push(FunParam { Occurrence { substituted, param.occurrence.contract,
                                                    param.occurrence.shape } });
            }

            auto result = substituteType(context, program, fun.result.type, with);
            if(!moved && result == fun.result.type) return type;

            return program.types.funType(base, params.pointer(), params.size(),
                                         Occurrence { result, fun.result.contract, fun.result.shape });
        }
        case Type::Record: {
            auto& record = *(RecordType*)base[type];

            // A concrete record has nothing to substitute, and neither does a generic declaration
            // reached as a type - the second cannot occur, since a declaration is only ever named
            // through an application.
            if(!record.instanceOf) return type;

            SmallArray<DeclPtr<Type>, 4> arguments;
            auto moved = false;

            for(auto argument: record.arguments.contents(base)) {
                auto substituted = substituteType(context, program, argument, with);
                moved = moved || substituted != argument;
                arguments.push(substituted);
            }

            if(!moved) return type;

            return instantiateRecord(context, program, record.instanceOf, arguments.pointer(),
                                     arguments.size(), record.source);
        }
        default:
            return type;
    }
}

Occurrence substituteOccurrence(Context& context, Program& program, const Occurrence& occurrence,
                                const Substitution& with) {
    auto substituted = substituteType(context, program, occurrence.type, with);
    if(substituted == occurrence.type) return occurrence;

    return Occurrence { substituted, occurrence.contract, occurrence.shape };
}

DeclPtr<Type> instantiateRecord(Context& context, Program& program, DeclPtr<RecordType> declaration,
                                DeclPtr<Type>* args, Size count, LocationId source) {
    auto base = *program.decl;
    auto& written = *base[declaration];

    /*
     * A declaration with no parameters is its own type, applied to nothing.
     *
     * `Reader(a')` names origins and no types, and it has to come back as `Reader` itself rather than
     * as an instantiation with an empty argument list - the two would be different handles for one
     * declaration, and `sameType` is a handle comparison.
     */
    if(!written.gen) {
        if(count) {
            context.diagnostics.error("%@ takes no type arguments and this names %@"_v, source,
                                      context.findName(written.name), U64(count));
            return nullptr;
        }

        return asBase<Type>(declaration);
    }

    auto parameters = base[written.gen]->variables.size();

    /*
     * The head's own defaults, filled in where an application left a trailing position out.
     *
     * Here rather than at each caller, because this is the one place a record is applied: the
     * written type walk reaches it, a substitution rebuilding an instantiation reaches it, and both
     * would otherwise have to know that `Boxed` and `Boxed(I32)` are the same type. A rebuilt
     * instantiation always carries the full list already, so this is a no-op for it.
     */
    SmallArray<DeclPtr<Type>, 4> supplied;
    for(Size i = 0; i < count; i++) supplied.push(args[i]);

    if(!fillDefaults(context, program, written.gen, supplied)) {
        context.diagnostics.error("%@ takes %@ type arguments and this names %@"_v, source,
                                  context.findName(written.name), U64(parameters), U64(count));
        return nullptr;
    }

    args = supplied.pointer();
    count = supplied.size();

    // A count where a type belongs is not a mistake `instantiate` can find: the two arrive as
    // handles and interning is happy to key on either. Reported before the record exists, so that a
    // head applied wrongly leaves no instantiation behind for the classifier to fold.
    if(!checkArgumentKinds(context, base, written.gen, written.name, args, count, source)) {
        return nullptr;
    }

    auto created = false;
    auto record = program.types.instantiate(base, declaration, args, count, created);
    auto pointer = record - base;

    if(!created) return asBase<Type>(pointer);

    /*
     * Registered in the declaring module, so that the pass that classifies types reaches it exactly
     * as it reaches a written `data`.
     *
     * The declaration's module and not the use site's, which is the orphan rule read from the type
     * side: `Boxed(I32)` is `Boxed`, made concrete, and belongs where `Boxed` was written whoever
     * wrote the application.
     */
    base[written.module]->records.push(program.decl, pointer);

    Substitution with { written.gen, args, count };

    for(auto field: written.fields.contents(base)) {
        RecordField substituted = field;
        substituted.occurrence = substituteOccurrence(context, program, field.occurrence, with);
        record->fields.push(program.decl, substituted);
    }

    record->originSlots = written.originSlots;

    /*
     * The schema, then the classification, and only the second waits.
     *
     * A schema is a property of the field declarations alone, so it is folded here whenever the
     * record is made. What a type *owes* depends on the whole instance set, which is why the pass
     * exists at all - so an instantiation made while the declarations are still being read is left
     * for the pass, and one made afterwards answers for itself. `own.h` is where the distinction is
     * argued; this is the one place it is spent.
     */
    foldRecordSchema(context, program, *record);
    if(program.typesClassified) classifyRecord(context, program, *record);

    return asBase<Type>(pointer);
}

} // namespace mir
