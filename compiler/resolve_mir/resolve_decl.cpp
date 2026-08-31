#include "resolve_internal.h"
#include "print.h"

namespace mir {

/*
 * One declaration's attributes, read into the flags the stage has fields for.
 *
 * Two of them, and both are [removals](../../doc/spec/ownership.md#removing-a-capability): what a
 * removal takes away has to be closed and visible at the declaration, which is why they are
 * attributes on a `data` or an `instance` rather than instances of their own. An unrecognized one is
 * reported rather than ignored, because a silently dropped `@explicitDrop` is a linear type that is
 * not linear and nothing downstream would ever say so.
 *
 * A null destination is a *placement* error rather than an unknown name, and the two are reported
 * differently: `@local` on an `instance` and on a `data` mean the same thing and both have a field
 * here, while `@explicitDrop instance Drop(T)` is a statement about a type written on something that
 * is not one. An attribute this stage has no field for at all is neither, and is `unsupported`.
 */
void Resolver::readAttributes(ast::Decl& declaration, bool* explicitDrop, bool* local, StringView on) {
    for(auto attribute: declaration.attributes.contents(parse)) {
        auto which = findAttribute(stringView(context.findName(attribute.name)));
        if(which == DeclAttribute::Count) {
            unsupported(attribute.source, "this attribute"_v);
            continue;
        }

        // The one statement of which attribute lands where, and a switch rather than a conditional
        // so that a new enumerator is a compiler error here instead of an attribute that parses,
        // resolves and is quietly dropped on the floor.
        bool* target = nullptr;
        switch(which) {
            case DeclAttribute::ExplicitDrop: target = explicitDrop; break;
            case DeclAttribute::Local:        target = local; break;
            case DeclAttribute::Count:        break;
        }

        if(!target) {
            context.diagnostics.error("`@%@` has no meaning on %@"_v, attribute.source,
                                      attributeName(which), on);
            continue;
        }

        if(attribute.args.isNotEmpty()) {
            context.diagnostics.error("`@%@` takes no arguments"_v, attribute.source,
                                      attributeName(which));
            continue;
        }

        *target = true;
    }
}

/*
 * Pass one, part one: the record *names*, before any field type is resolved.
 *
 * Two sub-passes rather than one, because a `data` may hold a record declared below it and the field
 * type of the first would then resolve against a name table that does not have the second in it yet.
 * A shell is a name and an identity, which is all a nominal type needs to be referred to; the fields
 * arrive in `declareFields`.
 */
void Resolver::declareTypes(ast::Module& file) {
    for(auto declaration: file.decls.contents(parse)) {
        if(declaration.kind != ast::Decl::Data) continue;

        auto& written = declaration.data;
        auto push = [&](DeclPtr<RecordType> record) { writtenRecords.push(record); };

        // `data (Eq(a)) Set(a)` - the head's own constraints, read in `declareRecordFields` for the
        // reason the fields are: a constraint names a class, and no class exists yet.

        /*
         * One constructor, whose name is the type's - `data Pair {x: I32, y: I32}`.
         *
         * Several constructors are an enumeration: the value carries a discriminant, a projection
         * needs a downcast that a pattern proves, and the ownership schema differs per arm. That is
         * a representation with three parts this milestone does not have, and refusing it by name is
         * more useful than a record whose second constructor was silently dropped.
         */
        if(written.cons.size() != 1) {
            unsupported(declaration.source, "a data declaration with several constructors"_v);
            push(nullptr);
            continue;
        }

        auto constructor = written.cons.get(parse, 0);
        if(constructor.name != written.type.name) {
            unsupported(constructor.source, "a constructor whose name differs from its type"_v);
            push(nullptr);
            continue;
        }

        if(!constructor.content || parse[constructor.content]->kind != ast::Type::Tup) {
            unsupported(constructor.source, "a constructor without named fields"_v);
            push(nullptr);
            continue;
        }

        auto record = program.types.recordType(written.type.name, here(), declaration.source);
        auto pointer = DeclPtr<RecordType>(record - *program.decl);

        /*
         * The head's parameters - `data Boxed(a) {value: a}`.
         *
         * A **closed** context, which is the one thing a `data` head decides that a function's
         * signature does not: the parameters are written out, so a lowercase name in a field type
         * that is not one of them is a name nothing declared rather than a new parameter. Built here
         * with the shell rather than with the fields, because the fields are read in it.
         */
        if(written.type.kind.size()) {
            auto environment = new (program.decl) GenContext(true);
            record->gen = environment - *program.decl;

            for(auto parameter: written.type.kind.contents(parse)) {
                /*
                 * A const parameter - `data Buf(n: Int)`. A number rather than a type, and a
                 * *position* of this same list: what the annotation changes is what may be written
                 * opposite it and where the name may then be used, and nothing about arity,
                 * defaults or substitution - see `VariableType::constType`.
                 *
                 * Bound even where the annotation did not resolve, which is the difference between
                 * one diagnostic and three: a parameter the head wrote and this walk skipped is a
                 * name every field mentioning it reports as undeclared, and a record whose fields
                 * all failed that way reports again for having none.
                 */
                DeclPtr<Type> annotation;
                if(parameter.type) annotation = resolveConstParameterType(*parse[parameter.type]);

                if(findGenVariable(*program.decl, record->gen, parameter.name)) {
                    context.diagnostics.error("%@ already binds a type called %@"_v,
                                              parameter.source, context.findName(written.type.name),
                                              context.findName(parameter.name));
                    continue;
                }

                genVariable(program, record->gen, parameter.name, parameter.source, annotation);
            }

            closeContext(*program.decl, record->gen);
            declareDefaults(written.type.kind, record->gen);
        }

        // Read on the shell rather than in `declareFields`, because `classifyTypes` runs over the
        // records and not over the written declarations - a record whose fields were rejected still
        // has to carry what its declaration said about it.
        readAttributes(declaration, &record->writtenExplicitDrop, &record->local, "a type"_v);

        module.records.push(program.decl, pointer);
        push(pointer);

        if(!program.addRecordName(here(), record->name, pointer)) {
            context.diagnostics.error("%@ is already declared in this module"_v,
                                      declaration.source, context.findName(record->name));
        }
    }
}

/*
 * `alias Pair = Two(I32, I32)`, `alias Bytes(a) = Array(a)` - the shell and its head.
 *
 * Beside `declareTypes` and in the same pass for the same reason: a record's field may name an alias
 * and an alias's target may name a record, so both have to be *names* before either's contents are
 * read. What is read here is the name and the parameters; the target waits for
 * `resolveAliasTargets`.
 */
void Resolver::declareAliases(ast::Module& file) {
    // The declarations by address rather than by value, because the target is read later and an
    // `ast::Decl` walked by value is a copy this pass returns past. `ParseList` is non-embedding, so
    // the storage is the parse region's and outlives everything here.
    auto declarations = file.decls.pointer(parse);

    for(Size index = 0; index < file.decls.size(); index++) {
        auto& declaration = declarations[index];
        if(declaration.kind != ast::Decl::Alias) continue;

        auto& written = declaration.alias;

        /*
         * `alias qualified Flags = I64` - a **type** rather than a spelling.
         *
         * The transparent form below is free precisely because it is not a type: it has no identity
         * of its own, so nothing converts to or from it, no instance can be attached to it and the
         * ownership classification never sees one. A qualified alias is the opposite of all three -
         * it is a distinct type with `doc/spec`'s conversion rules to and from its target and with
         * `deriving` to attach instances - so it is a declaration this stage does not have rather
         * than a flag on the one it does.
         */
        if(declaration.qualified) {
            unsupported(declaration.source, "a qualified alias"_v);
            continue;
        }

        // A `deriving` clause needs no check of its own: the parser admits one only on a qualified
        // alias, which the line above has already refused, because a plain alias names the same type
        // as its target and instances belong to that type.

        auto alias = new (program.decl) Alias(written.type.name, here(), declaration.source);
        auto pointer = alias - *program.decl;

        if(written.type.kind.size()) {
            auto environment = new (program.decl) GenContext(true);
            alias->gen = environment - *program.decl;

            for(auto parameter: written.type.kind.contents(parse)) {
                DeclPtr<Type> annotation;
                if(parameter.type) annotation = resolveConstParameterType(*parse[parameter.type]);

                if(findGenVariable(decl(), alias->gen, parameter.name)) {
                    context.diagnostics.error("%@ already binds a type called %@"_v,
                                              parameter.source, context.findName(written.type.name),
                                              context.findName(parameter.name));
                    continue;
                }

                genVariable(program, alias->gen, parameter.name, parameter.source, annotation);
            }

            closeContext(decl(), alias->gen);
            declareDefaults(written.type.kind, alias->gen);
        }

        if(!program.addAliasName(here(), alias->name, pointer)) {
            context.diagnostics.error("%@ is already declared in this module"_v,
                                      declaration.source, context.findName(alias->name));
            continue;
        }

        alias->written = U32(writtenAliasDecls.size());
        program.aliases.push(pointer);
        writtenAliasDecls.push(&declaration);
    }
}

/*
 * One alias's target, read on demand - `Alias::state`.
 *
 * On demand rather than in declaration order, because an alias may name one declared below it and
 * both readings are legal. The three-colour walk is what resolves each exactly once and turns
 * `alias A = B` beside `alias B = A` into a diagnostic instead of a walk that does not terminate.
 */
/*
 * `Bytes(I32)` from `alias Bytes(a) = Array(a)` - the target with the head's parameters replaced.
 *
 * A substitution and not an instantiation, which is the whole difference between an alias and a
 * `data`: there is no record to intern, no fields to substitute and no ownership to classify,
 * because what comes back is a type that already exists under another name.
 */
DeclPtr<Type> Resolver::applyAlias(DeclPtr<Alias> pointer, DeclPtr<Type>* args, Size count,
                                   LocationId source) {
    auto target = aliasTarget(pointer);
    if(!target) return nullptr;

    auto& alias = *decl()[pointer];
    auto arity = alias.gen ? decl()[alias.gen]->variables.size() : 0;

    SmallArray<DeclPtr<Type>, 4> supplied;
    for(Size i = 0; i < count; i++) supplied.push(args[i]);

    if(!alias.gen ? count != 0 : !fillDefaults(context, program, alias.gen, supplied)) {
        context.diagnostics.error("%@ takes %@ type arguments and this names %@"_v, source,
                                  context.findName(alias.name), U64(arity), U64(count));
        return nullptr;
    }

    if(!alias.gen) return target;

    if(!checkArgumentKinds(context, decl(), alias.gen, alias.name, supplied.pointer(),
                           supplied.size(), source)) {
        return nullptr;
    }

    Substitution with { alias.gen, supplied.pointer(), supplied.size() };
    return substituteType(context, program, target, with);
}

DeclPtr<Type> Resolver::aliasTarget(DeclPtr<Alias> pointer) {
    auto& alias = *decl()[pointer];
    if(alias.state == Alias::State::Read) return alias.target;

    if(alias.state == Alias::State::Reading) {
        context.diagnostics.error("%@ is defined in terms of itself"_v, alias.source,
                                  context.findName(alias.name));
        alias.state = Alias::State::Read;
        return nullptr;
    }

    // Marked before the walk and not after it, which is what makes the cycle visible: the recursion
    // below reaches this alias again through whatever it names.
    alias.state = Alias::State::Reading;

    if(alias.written < writtenAliasDecls.size()) {
        GenScope generics(*this, alias.gen);
        alias.target = resolveType(writtenAliasDecls[alias.written]->alias.target);
    }

    alias.state = Alias::State::Read;
    return alias.target;
}

void Resolver::resolveAliasTargets() {
    for(auto pointer: program.aliases) aliasTarget(pointer);
}

/*
 * `data Boxed(a = I32)`, `class C(a, b = Bool)` - the head's defaults, once every parameter is bound.
 *
 * A second loop over the same list rather than a field filled beside each parameter, and the reason
 * is what a default is allowed to say: `data Pair(a, b = a)` names a parameter of the same head, so
 * every one of them has to exist before any default is read. Resolved against the **closed** context
 * for the same reason a field type is - a lowercase name in a default that the head did not bind is
 * a name nothing declared.
 *
 * A gap is filled with null, so `defaults` stays the same length as `variables` and `fillDefaults`
 * indexes one with the other. A head that defaults a parameter and then leaves a later one bare is
 * refused: every position after the first default has to have one, or an application supplying a
 * prefix could not say which parameter it meant.
 */
void Resolver::declareDefaults(ast::ParseList<ast::GenParam> parameters, DeclPtr<GenContext> gen) {
    if(!gen) return;

    GenScope generics(*this, gen);
    auto seen = false;

    for(auto parameter: parameters.contents(parse)) {
        auto position = decl()[gen]->defaults.size();

        if(!parameter.def) {
            if(seen) {
                context.diagnostics.error(
                    "%@ follows a parameter with a default and has none, so an application cannot leave that one out"_v,
                    parameter.source, context.findName(parameter.name));
            }

            decl()[gen]->defaults.push(program.decl, nullptr);
            continue;
        }

        seen = true;

        /*
         * Read as the *argument* it stands in for, at the position it fills.
         *
         * Which is what makes `data Buf(n: I32 = 4)` need no rule of its own: a default supplies an
         * argument, `resolveTypeArgument` is what reads one, and the number takes its width from the
         * parameter exactly as a written `Buf(4)` does. It is also what reports a type written where
         * a count belongs and a count written where a type belongs, in one place for both.
         */
        auto written = resolveTypeArgument(*parse[parameter.def], gen, position);

        if(written && decl()[written]->kind != Type::Const) {
            auto declared = decl()[gen]->variables.get(decl(), position);
            if(declared && ((VariableType*)decl()[declared])->isConst()) {
                context.diagnostics.error("%@ counts, so its default is a number"_v,
                                          parse[parameter.def]->source,
                                          context.findName(parameter.name));
                written = nullptr;
            }
        }

        decl()[gen]->defaults.push(program.decl, written);
    }

    // A head this walk gave up on part way still has to answer for every position, since
    // `fillDefaults` reads the two lists together.
    while(decl()[gen]->defaults.size() < decl()[gen]->variables.size()) {
        decl()[gen]->defaults.push(program.decl, nullptr);
    }
}

/*
 * `data (Eq(a)) Set(a)` - what every application of this head has to prove.
 *
 * A requirement of the record's own context, which is the same structure a signature's is - and it
 * is discharged at every *application* rather than at every call, which is the one thing that makes
 * this a second site rather than a second mechanism. `resolveApplication` is that site.
 *
 * The context is already closed, so a constraint may only name parameters the head wrote: a `data`
 * head lists its parameters, and a constraint introducing one would be a parameter no application
 * supplies an argument for.
 */
void Resolver::declareHeadConstraints(ast::Decl& declaration, RecordType& record) {
    for(auto constraint: declaration.data.constraints.contents(parse)) {
        if(!requireClassConstraint(constraint, "a data head constraint other than a class"_v)) {
            continue;
        }

        if(!record.gen) {
            context.diagnostics.error("%@ binds no parameters for a constraint to be about"_v,
                                      constraint.source, context.findName(record.name));
            continue;
        }

        GenRequirement requirement;
        if(!readClassRequirement(constraint, record.gen, false, requirement)) continue;

        decl()[record.gen]->classes.push(program.decl, requirement);
    }
}

/*
 * The fields of one record, once every record name exists.
 *
 * `next` indexes the *written* declarations including the rejected ones, for the reason
 * `writtenRecords` exists: the two walks have to advance in step or a later declaration is given an
 * earlier one's fields.
 */
void Resolver::declareFields(ast::Decl& declaration, Size& next) {
    if(next >= writtenRecords.size()) return;

    auto pointer = writtenRecords[next++];
    if(!pointer) return;

    auto record = decl()[pointer];

    // The shell exists, so `declareTypes` already established that there is exactly one constructor
    // and that its content is a written tuple - this is the same declaration read a second time
    // rather than a second set of conditions to keep in step.
    auto constructor = declaration.data.cons.get(parse, 0);
    auto& written = *parse[constructor.content];

    // The record's own ownership schema scope - §5.11. A `data` declaration is a scope exactly as a
    // signature is, and for the same reason: `data Editor {input: &'String, at: Size}` publishes one
    // origin, and what fills it is decided where an `Editor` is constructed.
    SlotScope slots;

    declareHeadConstraints(declaration, *decl()[pointer]);

    // The head's parameters are what the field types are written in terms of.
    GenScope generics(*this, record->gen);

    for(auto field: written.tup.fields.contents(parse)) {
        if(!field.name) {
            unsupported(record->source, "a positional field"_v);
            continue;
        }

        if(field.def) {
            unsupported(record->source, "a field default"_v);
            continue;
        }

        Occurrence occurrence;
        if(!resolveOccurrence(field.type, ast::BindType::Borrow, Position::Field, slots, occurrence)) {
            continue;
        }

        // A second field of one name would make `p.x` name whichever the lookup reached first, which
        // is a silent wrong answer rather than a missing feature.
        auto duplicate = false;
        for(auto existing: record->fields.contents(decl())) {
            if(existing.name != field.name) continue;

            context.diagnostics.error("%@ already has a field called %@"_v, record->source,
                                      context.findName(record->name), context.findName(field.name));
            duplicate = true;
            break;
        }

        if(duplicate) continue;

        RecordField declared;
        declared.name = field.name;
        declared.occurrence = occurrence;
        declared.source = record->source;
        declared.index = U16(record->fields.size());

        record->fields.push(program.decl, declared);
    }

    record->originSlots = slots.count();
    if(record->fields.size()) return;

    /*
     * A record with no fields has no layout question and no ownership question, and every value of
     * it is the same value - which is `{}`, spelled by a declaration. Refused rather than admitted
     * as a second unit type.
     *
     * Only where the *declaration* wrote none. A record every one of whose fields was rejected has
     * none either, and saying so would be reporting the consequence of a mistake beside the mistake
     * - which is what turns one bad field type into two messages.
     */
    if(written.tup.fields.size()) return;

    unsupported(record->source, "a record with no fields"_v);
}

void Resolver::declareGlobals(ast::Decl& declaration) {
    if(declaration.stmt.kind != ast::Expr::Decl) {
        unsupported(declaration.source, "a module-level statement"_v);
        return;
    }

    for(auto binding: declaration.stmt.decl.contents(parse)) {
        if(binding.pat.kind != ast::Pat::Var) {
            context.diagnostics.error("a global must be declared as a single name"_v, binding.pat.source);
            writtenGlobals.push(nullptr);
            continue;
        }

        if(!binding.content) {
            context.diagnostics.error("a global requires an initializer"_v, binding.pat.source);
            writtenGlobals.push(nullptr);
            continue;
        }

        auto global = new (program.decl) Global(binding.pat.var, owned(nullptr));
        global->source = binding.pat.source;
        global->writable = binding.bind == ast::BindType::Ref;

        auto pointer = global - *program.decl;
        module.globals.push(program.decl, pointer);
        writtenGlobals.push(pointer);

        if(!program.addGlobalName(here(), global->name, pointer)) {
            context.diagnostics.error("%@ is already declared in this module"_v,
                                      binding.pat.source, context.findName(global->name));
        }
    }
}
/*
 * Every record's fields, and nothing else.
 *
 * A pass of its own, ahead of `declare`, because `foldSchemas` runs between the two: a signature may
 * write `x: Reader(a')`, which binds `Reader`'s interior positions, and under one interleaved walk a
 * function declared above a `data` in the same file was resolved before that record had fields at
 * all. Splitting is what makes the precondition a phase order rather than a source order.
 */
void Resolver::declareRecordFields(ast::Module& file) {
    for(auto declaration: file.decls.contents(parse)) {
        if(declaration.kind == ast::Decl::Data) declareFields(declaration, nextRecordFields);
    }
}

// The class heads, beside the record shells: a constraint in a signature and an `instance` head both
// name a class, so every class exists before either is read - see `declareClass`.
void Resolver::declareClasses(ast::Module& file) {
    for(auto declaration: file.decls.contents(parse)) {
        if(declaration.kind == ast::Decl::Trait) declareClass(declaration);
    }
}

/*
 * Every class's members, ahead of every function and every instance.
 *
 * A phase of its own because an instance is checked against its class's *members*, and a class
 * declared below an `instance` - in the same file or in another one - would not have them yet. The
 * alternative is a walk that reads a member signature on demand, which is the same dependency
 * expressed as a recursion that has to detect its own cycles.
 *
 * Ahead of the functions rather than after them, so that what follows is read in **declaration
 * order**: a module's function list is what the dump prints and what lowering walks, and moving an
 * instance's members to the end of it would reorder every existing fixture's golden for no reason
 * anyone reading the fixture could see.
 */
void Resolver::declareClassMembers(ast::Module& file) {
    for(auto declaration: file.decls.contents(parse)) {
        if(declaration.kind == ast::Decl::Trait) declareClassMembers(declaration, nextClassMembers);
    }
}

void Resolver::declare(ast::Module& file) {
    for(auto declaration: file.decls.contents(parse)) {
        switch(declaration.kind) {
            case ast::Decl::Stmt:
                declareGlobals(declaration);
                break;
            case ast::Decl::Fun:
                declareFunction(declaration);
                break;
            case ast::Decl::Instance:
                declareInstance(declaration);
                break;
            // Read by their own passes, above - `declareTypes`, `declareAliases`, `declareClasses`.
            case ast::Decl::Data:
            case ast::Decl::Alias:
            case ast::Decl::Trait:
            case ast::Decl::Error:
                break;
            default:
                unsupported(declaration.source, "this declaration"_v);
                break;
        }
    }
}

} // namespace mir
