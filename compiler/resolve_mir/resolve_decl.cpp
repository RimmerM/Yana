#include "resolve_internal.h"
#include "print.h"

namespace mir {

/*
 * Whether this declaration is part of *this* compilation - `doc/spec/declarations.md`'s platform
 * selection, and the whole of what `@platform` does.
 *
 * An excluded declaration is not read at all, so it contributes no name, no type, no instance and no
 * body. That is what makes `@platform` usable for alternative **implementations of one name** rather
 * than only for exclusion: a JS build that could still resolve the native `Storage.reserve` would
 * have two implementations of one name and would pick between them by accident.
 *
 * `targetSelector` is the one place a target name is decided, in `compiler/compiler/settings.h`, and
 * it is shared with the file-name selector and with the shipping stage. So an attribute, a file name
 * and `-target` can never come to disagree about what a machine is called, and this stage cannot
 * select a different set of declarations than the one beside it - which would be one source
 * compiling to two programs.
 *
 * Names inside one attribute read as **or** and the attribute written twice reads as **and**, which
 * is the spec's rule and is why `matched` is per attribute and `enabled` is across them.
 *
 * **Silent.** Every malformed spelling this could report is reported by `checkPlatform`, once, before
 * any pass runs - see `eachDeclaration`, which is what asks this and is asked eight times per
 * declaration. A predicate that reported would say the same sentence eight times, and the shipping
 * stage carries a `report` flag through its eight call sites for exactly that reason.
 */
bool platformEnabled(Context& context, ast::ParseBase parse, const ast::Decl& declaration) {
    // Copied rather than walked through the reference: a list's walk is non-const, and this
    // deliberately takes a `const` declaration because it decides nothing about one.
    auto attributes = declaration.attributes;
    if(attributes.isEmpty()) return true;

    auto enabled = true;

    for(auto attribute: attributes.contents(parse)) {
        if(findAttribute(stringView(context.findName(attribute.name))) != DeclAttribute::Platform) {
            continue;
        }

        // An attribute with no target, or one whose targets are all unreadable, restricts nothing.
        // `checkPlatform` is what says so; here it is the answer that keeps a typo from deleting a
        // declaration from every build while the diagnostic is still being read.
        if(attribute.args.isEmpty()) continue;

        auto matched = false;
        for(auto argument: attribute.args.contents(parse)) {
            if(argument.value.kind != ast::Expr::Var) continue;

            auto name = context.findName(argument.value.var);
            auto answer = targetSelector(context.settings, stringView(name));
            matched = matched || answer == TargetSelector::Matched;
        }

        enabled = enabled && matched;
    }

    return enabled;
}

/*
 * Every `@platform` in a file, judged once.
 *
 * A pass of its own, ahead of every other one, because the *selection* is asked once per declaration
 * per pass and the *spelling* has to be reported once. Splitting them is what lets `platformEnabled`
 * be a silent predicate that any walk may call as often as it likes.
 *
 * Ahead of everything rather than folded into the first pass, because the first pass already skips
 * excluded declarations - so an attribute on one of them would never be judged, and a typo that
 * excluded a declaration from every build would be the thing that hid its own diagnostic.
 *
 * An unknown name is reported rather than silently excluding, for that same reason from the other
 * side: `@platform(nativ)` is a declaration deleted from every target, and the compiler is the only
 * thing in a position to notice.
 */
void Resolver::checkPlatform(ast::Module& file) {
    // Every declaration, including the excluded ones - see the note above, which is why.
    for(auto declaration: file.decls.contents(parse)) {
        for(auto attribute: declaration.attributes.contents(parse)) {
            if(findAttribute(stringView(context.findName(attribute.name))) != DeclAttribute::Platform) {
                continue;
            }

            if(attribute.args.isEmpty()) {
                context.diagnostics.error("`@platform` needs at least one target - a platform (`js`, `native`), an operating system, an architecture, an x86-64 level or an instruction-set extension"_v,
                                          attribute.source);
                continue;
            }

            for(auto argument: attribute.args.contents(parse)) {
                if(argument.value.kind != ast::Expr::Var) {
                    context.diagnostics.error("a `@platform` target is a bare name - `js`, `native`, an operating system, an architecture, an x86-64 level or an instruction-set extension"_v,
                                              argument.value.source);
                    continue;
                }

                auto name = context.findName(argument.value.var);
                if(targetSelector(context.settings, stringView(name)) != TargetSelector::Unknown) {
                    continue;
                }

                context.diagnostics.error("unknown target %@ - expected `js`, `native`, an operating system (`linux`, `mac`, `win32`), an architecture (`x64`, `x86`, `arm`, `arm64`), an x86-64 level (`v2`, `v3`, `v4`) or an instruction-set extension (`sha`, `nosha`)"_v,
                                          argument.value.source, name);
            }
        }
    }
}

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

            // The two form attributes have no destination here and never will: what they choose is
            // three-valued and belongs to a signature, so `readGenForm` reads them and this walk
            // reports them as misplaced wherever it is the reader.
            case DeclAttribute::Specialize:
            case DeclAttribute::Generic:      break;

            // `@value(n)` belongs to a *constructor*, which is not a declaration this walk reads -
            // `readValueAttribute` is its one reader, and reaching here is the placement message.
            case DeclAttribute::Value:        break;

            // `@builtin(role)` is read by `bindRole`, which every declaration form calls with what
            // it is - so a misplaced one is reported there, against the role it named, rather than
            // here as an attribute with no meaning. Skipped rather than reported.
            case DeclAttribute::Builtin:      continue;

            // `@bits(n)` refines a type, and a declaration is not one - so it falls to the placement
            // message below, exactly as `@value` on a `data` head does.
            case DeclAttribute::Bits:         break;

            // `@platform` restricts any declaration, so there is no placement to report and this is
            // not its reader - `checkPlatform` judges the spelling and `platformEnabled` applies it,
            // both before this walk runs at all. Skipped rather than reported.
            case DeclAttribute::Platform:     continue;
            case DeclAttribute::Count:        break;
        }

        if(!target) {
            context.diagnostics.error("`@%@` has no meaning on %@"_v, attribute.source,
                                      attributeName(which), on);
            continue;
        }

        if(!takesNoArguments(attribute, which)) continue;

        *target = true;
    }
}

/*
 * A flag attribute given arguments - the one refusal every attribute that is not `@value` shares.
 *
 * A function rather than the four lines twice, because it is one rule with one message: `@inline(2)`
 * and `@specialize(a)` are the same mistake, and two copies of the sentence are two sentences that
 * can come to differ. Answers whether the attribute is well formed, so a caller reads it as a guard
 * rather than as a report it then has to remember happened.
 */
bool Resolver::takesNoArguments(ast::Attribute& attribute, DeclAttribute which) {
    if(!attribute.args.isNotEmpty()) return true;

    context.diagnostics.error("`@%@` takes no arguments"_v, attribute.source, attributeName(which));
    return false;
}

/*
 * `@value(n)` on a constructor - `doc/spec/declarations.md`'s pinned constructor values.
 *
 * The number a constructor *is*, and what it is for is a boundary somebody else designed: a `Whence`
 * handed to `lseek` is 0, 1 or 2 whatever the declaration order happens to be, and without the
 * attribute that is a fact the declaration cannot state.
 *
 * A reader of its own rather than a destination in `readAttributes`, because this is the one
 * attribute this stage reads that carries an argument - the destination is a number and not a flag,
 * and the argument has its own three refusals. Nothing means "absent" and is what makes the
 * unpinned rule expressible: an unpinned constructor continues from the one before it, which a
 * default of zero could not say.
 *
 * A literal and only a literal, negative or not. It is a number the ABI on the other side of the
 * boundary already fixed, so there is nothing here for an expression to compute - and an attribute
 * argument is read before anything in the module can be evaluated.
 */
Maybe<I64> Resolver::readValueAttribute(ast::Con& constructor) {
    Maybe<I64> pinned;

    for(auto attribute: constructor.attributes.contents(parse)) {
        auto which = findAttribute(stringView(context.findName(attribute.name)));

        if(which != DeclAttribute::Value) {
            // Every other attribute is one this stage does not read here, and a constructor is not a
            // declaration with a placement table to report against - so it is refused by name, on
            // `readAttributes`' own reasoning about a dropped attribute.
            unsupported(attribute.source, "an attribute on a constructor"_v);
            continue;
        }

        if(attribute.args.size() != 1) {
            context.diagnostics.error("`@value` takes one number - `@value(2)`"_v, attribute.source);
            continue;
        }

        auto written = attribute.args.get(parse, 0).value;
        auto argument = &written;
        auto negative = false;

        /*
         * A negated literal arrives as a prefix applied to one rather than as a signed literal.
         *
         * The lexer produces only the magnitude, so what a written number *is* is a question about
         * where it lands - which is the same reason `ast::Pat` carries the sign beside the
         * magnitude rather than folding it in.
         */
        if(argument->kind == ast::Expr::Prefix) {
            auto& prefix = *parse[argument->prefix];

            if(prefix.op.kind == ast::Expr::Var && prefix.op.var == Context::nameHash("-"_v)) {
                argument = &prefix.on;
                negative = true;
            }
        }

        if(!ast::isLiteral(*argument) ||
           ast::Literal::Kind(argument->kind - ast::Expr::Lit) != ast::Literal::Int) {
            context.diagnostics.error("`@value` takes a whole number written as a literal"_v,
                                      argument->source);
            continue;
        }

        auto number = I64(argument->lit.i());
        pinned = Just(negative ? -number : number);
    }

    return pinned;
}

/*
 * `@specialize` and `@generic` on a signature - which form its body is compiled in, `GenForm`.
 *
 * Separate from `readAttributes` because the two differ in what an *unrecognized* name means. A
 * `data` head takes a closed set and anything else is a mistake this stage has to report, since
 * everything it reads there is a removal and a dropped removal is unsound. A signature carries
 * attributes this stage does not read at all - `@inline`, `@noinline` and `@convention` are the
 * backend's - so a name with no row here is not the resolver's to refuse.
 *
 * Whether the declaration is generic at all is deliberately not asked. A function's context is open
 * and fills as the signature is read, so nothing at this point knows; `selectGenForms` is where a
 * written form meets a finished context, and it is where writing one on a plain `fn` is reported.
 */
GenForm Resolver::readGenForm(ast::Decl& declaration) {
    auto form = GenForm::Auto;

    for(auto attribute: declaration.attributes.contents(parse)) {
        auto which = findAttribute(stringView(context.findName(attribute.name)));

        auto wanted = GenForm::Auto;
        if(which == DeclAttribute::Specialize) wanted = GenForm::Specialized;
        else if(which == DeclAttribute::Generic) wanted = GenForm::Erased;
        else continue;

        if(!takesNoArguments(attribute, which)) continue;

        // Both written on one declaration. One field with two names that may write it, so the
        // conflict is caught where the second name lands rather than by comparing two flags
        // afterwards - which is the shape that admits a third attribute nobody compared against.
        if(form != GenForm::Auto && form != wanted) {
            context.diagnostics.error(
                "a generic body is compiled once or once per argument list, and this asks for both"_v,
                attribute.source);
            continue;
        }

        form = wanted;
    }

    return form;
}

/*
 * `infixl 8 +` - what a module says an operator's grouping is.
 *
 * A pass of its own and the first of them, because fixity is read by `resolveInfix` while a *body*
 * is resolved and by nothing before that - so what it has to be ahead of is every body, and being
 * ahead of every declaration costs nothing and is one less order to remember. It is looked up
 * through the imports like any other name, which is what makes an operator declared in Core group
 * the same way wherever Core is imported.
 *
 * A second declaration of one operator in one module is reported. Two *modules* declaring one is
 * not: a fixity is not a definition, so the traversal takes the first it reaches rather than calling
 * them ambiguous - see `mir::findFixity`.
 */
void Resolver::declareFixities(ast::Module& file) {
    for(auto fixity: file.ops.contents(parse)) {
        auto properties = OpProperties {
            U16(fixity.precedence),
            fixity.kind == ast::Fixity::Right ? Assoc::Right : Assoc::Left,
        };

        if(!program.addFixity(here(), fixity.op, properties)) {
            context.diagnostics.error("%@ already has a fixity in this module"_v, fixity.source,
                                      context.findName(fixity.op));
        }
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
    for(auto& declaration: eachDeclaration(file)) {
        if(declaration.kind != ast::Decl::Data) continue;

        auto& written = declaration.data;
        auto push = [&](DeclPtr<RecordType> record) { writtenRecords.push(record); };

        // `data (Eq(a)) Set(a)` - the head's own constraints, read in `declareRecordFields` for the
        // reason the fields are: a constraint names a class, and no class exists yet.

        /*
         * The constructors - one for `data Pair {x: I32}`, several for
         * `data Outcome(a, e) = Proceed(a) | Exit(e)`.
         *
         * A **single** constructor still has to be the type's own name, because there is nothing to
         * tell it from: `data Pair = Other {x: I32}` would give one type two spellings, one of which
         * names something that is not a type. Several constructors are each a name of their own and
         * that restriction lifts, which is what makes them constructors rather than a second syntax
         * for the same declaration.
         */
        if(written.cons.size() == 1 && written.cons.get(parse, 0).name != written.type.name) {
            unsupported(written.cons.get(parse, 0).source,
                        "a constructor whose name differs from its type"_v);
            push(nullptr);
            continue;
        }

        auto shaped = true;
        for(auto constructor: written.cons.contents(parse)) {
            /*
             * What a constructor may carry - named fields, one unnamed payload, or nothing.
             *
             * `Proceed(a)` is the one-payload form and is the shape `Outcome` and `Maybe` are
             * written in; it becomes a constructor with a single field, so that everything below is
             * about a field list and the spelling is not a second representation. `Proceed(a, b)` -
             * a positional list of several - is a *tuple type* as the content, which this stage has
             * no type for at all, and is refused where the tuple would be.
             */
            if(!constructor.content) continue;

            auto& content = *parse[constructor.content];
            if(content.kind == ast::Type::Tup) {
                if(!content.tup.fields.size()) continue;
                if(content.tup.fields.get(parse, 0).name) continue;

                unsupported(constructor.source, "a constructor with positional fields"_v);
                shaped = false;
            }
        }

        if(!shaped) {
            push(nullptr);
            continue;
        }

        auto record = program.types.recordType(written.type.name, here(), declaration.source);
        auto pointer = DeclPtr<RecordType>(record - *program.decl);

        // `pub`, on the declaration and never on an instantiation - see `exportedSymbol`. A type's
        // constructors are exported with it, so nothing here is written per constructor.
        record->exported = declaration.exported;

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
            declareHead(written.type.kind, record->gen, written.type.name);
        }

        // Read on the shell rather than in `declareFields`, because `classifyTypes` runs over the
        // records and not over the written declarations - a record whose fields were rejected still
        // has to carry what its declaration said about it.
        readAttributes(declaration, &record->writtenExplicitDrop, &record->local, "a type"_v);

        // `@builtin(role)` - a record the compiler names. No row claims one yet; the call is here so
        // that the day one does, a `data` is already a declaration form that can fill it - and so
        // that a misplaced role on one is reported rather than silently ignored.
        bindRole(context, program, parse, declaration, Just(intrinsic::Entity::Record),
                 pointer.offset);

        current->records.push(program.decl, pointer);
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
        alias->exported = declaration.exported;
        auto pointer = alias - *program.decl;

        if(written.type.kind.size()) {
            auto environment = new (program.decl) GenContext(true);
            alias->gen = environment - *program.decl;
            declareHead(written.type.kind, alias->gen, written.type.name);
        }

        // An `alias` fills no compiler role - it is a spelling rather than a declaration, so there
        // is nothing for a role to name. Stated so that a misplaced one is reported; see the note
        // on the same call for an `instance`.
        bindRole(context, program, parse, declaration, Nothing(), 0);

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
        /*
         * Read where it was *written*, which is not where it was asked for.
         *
         * A target is resolved on demand, so an alias of one module may name an alias of another and
         * reach here from over there - and what its own target means is what its own module and file
         * could see. This is also what puts a position on the pass below, which runs between the
         * per-file walks and has none of its own.
         */
        ModuleScope scope(*this, alias.module,
                          decl()[alias.module]->fileOf(decl(), context, alias.source));

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
 * The parameters of one generic head - `data Boxed(a, n: Int = 4)`, and the same for an `alias` and
 * a `class`.
 *
 * **The names now, every written type later.** A head is read in the shell pass, and the shell pass
 * is the one pass whose type namespace is deliberately incomplete: it is what *builds* that
 * namespace, so no alias exists yet and only the records above this one do. A position that resolved
 * a type there would be reading a different namespace from every other position in the language -
 * which is not a hypothetical, it is what `data Buf(n: Int)` did. `Int` is an alias, aliases are
 * declared by the pass beside this one, and `findBuiltin` answered for `I32` while nothing answered
 * for `Int`; a head default naming a record was worse still, since whether `data Foo(a = Bar)`
 * compiled depended on whether `Bar` was written above or below it.
 *
 * So what happens here is what needs no namespace - binding each parameter as a name - and the rest
 * is queued for `resolveHeads`. A field type mentioning a parameter needs only the name, which is
 * why the split can be made at all.
 *
 * The context is closed here rather than there, because closing it is a statement about the
 * *parameter list* and the list is complete at this point: a name written in a default or an
 * annotation is one the head did not bind, and that is true whenever it is read.
 */
void Resolver::declareHead(ast::ParseList<ast::GenParam> parameters, DeclPtr<GenContext> gen,
                           StringId owner) {
    if(!gen) return;

    for(auto parameter: parameters.contents(parse)) {
        /*
         * Bound even where its annotation has not been read, which is the difference between one
         * diagnostic and three: a parameter the head wrote and this walk skipped is a name every
         * field mentioning it reports as undeclared, and a record whose fields all failed that way
         * reports again for having none.
         *
         * A const parameter - `data Buf(n: Int)` - is a *position* of this same list, and what the
         * annotation changes is what may be written opposite it and where the name may then be used
         * (`VariableType::constType`). Nothing about arity, defaults or substitution reads it, which
         * is what lets it arrive after the name does.
         */
        if(findGenVariable(decl(), gen, parameter.name)) {
            context.diagnostics.error("%@ already binds a type called %@"_v, parameter.source,
                                      context.findName(owner), context.findName(parameter.name));
            continue;
        }

        genVariable(program, gen, parameter.name, parameter.source);
    }

    closeContext(decl(), gen);
    pendingHeads.push(PendingHead { here(), current->activeFile, gen, parameters });
}

/*
 * Every head's annotations and then every head's defaults - `PendingHead`, where the ordering is
 * argued.
 *
 * Two sweeps over the queue and not one pass over each head in turn, because a default is read
 * against the annotations: `declareDefaults` asks whether the position it is filling counts, and
 * that is what `n: Int = I32` is reported by. One sweep would make the answer depend on where in the
 * queue the head sat, which is the source order this pass exists to stop mattering.
 */
void Resolver::resolveHeads() {
    for(auto& pending: pendingHeads) {
        ModuleScope scope(*this, pending.module, pending.file);

        // No variables in scope, stated rather than inherited. A const parameter counts, so its
        // annotation names one of the integer types and never a parameter of the head being read -
        // a head whose own variables were in scope here would take `n: a` as a use of `a` rather
        // than as the mistake it is, and one that inherited an outer scope would take it as a use
        // of something else again.
        GenScope none(*this, nullptr);

        auto position = Size(0);
        for(auto parameter: pending.parameters.contents(parse)) {
            auto index = position++;
            if(!parameter.type) continue;

            auto annotation = resolveConstParameterType(*parse[parameter.type]);
            if(!annotation) continue;

            /*
             * The **written** position, which is what `declareDefaults` and `resolveTypeArgument`
             * index by as well - the three have to agree, and agreeing on the same wrong answer for
             * a head that binds one name twice is better than one of them being right.
             *
             * The two lists are the same list except on that head, `declareHead` has already
             * reported it, and the checkpoint after the declaration phases is what stops anything
             * downstream from reading either.
             */
            auto declared = decl()[pending.gen]->variables.get(decl(), index);
            if(declared) ((VariableType*)decl()[declared])->constType = annotation;
        }
    }

    for(auto& pending: pendingHeads) {
        ModuleScope scope(*this, pending.module, pending.file);
        declareDefaults(pending.parameters, pending.gen);
    }

    pendingHeads.clear();
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
/*
 * Every field default the declaration walk queued - the argument is on `PendingDefault`.
 *
 * Run once, after every record in the program has its fields and its constructors, so that what a
 * default may name is the whole program rather than the part of it this pass had reached. The
 * position is restored from the entry: the module and file decide what a name reaches, and the
 * record's own head is what a generic field's type was written against.
 *
 * A failure is reported by `declareDefault` and leaves `Constant::None` behind, which is what a
 * field with no default holds. The field is still declared - a construction that gives it a value
 * is not a construction the mistake is about.
 */
void Resolver::resolveFieldDefaults() {
    for(auto& pending: pendingDefaults) {
        ModuleScope scope(*this, pending.module, pending.file);

        auto& record = *decl()[pending.record];
        GenScope gen(*this, record.gen);

        // The storage rather than a copy, because this is the one pass that writes to a field after
        // it was pushed - `field(base, i)` answers by value and is every other reader's.
        auto& field = record.fields.pointer(decl())[pending.field];
        field.def = declareDefault(*pending.expr, field.occurrence.type, field.name, "field"_v,
                                   "construction"_v);
    }

    pendingDefaults.clear();
}

void Resolver::declareFields(ast::Decl& declaration, Size& next) {
    if(next >= writtenRecords.size()) return;

    auto pointer = writtenRecords[next++];
    if(!pointer) return;

    auto record = decl()[pointer];

    // The record's own ownership schema scope - §5.11. A `data` declaration is a scope exactly as a
    // signature is, and for the same reason: `data Editor {input: &'String, at: Size}` publishes one
    // origin, and what fills it is decided where an `Editor` is constructed. One scope for the whole
    // declaration and not one per constructor: an origin a `data` publishes is the *type's*, so two
    // arms naming the same borrowed thing name one slot.
    SlotScope slots;

    declareHeadConstraints(declaration, *decl()[pointer]);

    // The head's parameters are what the field types are written in terms of.
    GenScope generics(*this, record->gen);

    auto anyWritten = false;

    // The number the next unpinned constructor takes. Declaration order is what this counts when
    // nothing pins anything, which is what makes a declaration that writes no attribute the
    // declaration order it always had - `doc/spec/declarations.md`, and C's rule.
    I64 following = 0;

    for(auto constructor: declaration.data.cons.contents(parse)) {
        auto pinned = readValueAttribute(constructor);

        RecordCon declared;
        declared.name = constructor.name;
        declared.source = constructor.source;
        declared.index = U16(record->cons.size());
        declared.value = pinned ? pinned.unwrap() : following;
        declared.pinned = pinned.isJust();
        declared.first = U16(record->fields.size());

        following = declared.value + 1;

        /*
         * The one-payload form - `Proceed(a)` - as a constructor with a single field.
         *
         * The field carries the **constructor's own name**, which is a `ConID` and therefore not
         * something a field selector can spell: the payload is reached by matching and by nothing
         * else, and a name source cannot write is how that is stated rather than by a flag every
         * projection would have to test. A dump still prints something a reader recognises.
         */
        auto content = constructor.content ? parse[constructor.content] : nullptr;
        auto named = content && content->kind == ast::Type::Tup;

        auto field = [&](StringId name, ast::Type& type, ast::Expr* def, LocationId source) {
            anyWritten = true;

            Occurrence occurrence;
            if(!resolveOccurrence(type, ast::BindType::Borrow, Position::Field, slots, occurrence)) {
                return;
            }

            /*
             * A second field of one name in *one constructor*, which is what would make `p.x` name
             * whichever the lookup reached first.
             *
             * Per constructor and not per record, because two arms of a sum are two shapes over one
             * storage: `Proceed {value: a} | Exit {value: e}` names one thing in each, and which one
             * a projection means is settled by the match that reached it.
             */
            for(U16 i = declared.first; i < U16(record->fields.size()); i++) {
                if(record->fields.get(decl(), i).name != name) continue;

                context.diagnostics.error("%@ already has a field called %@"_v, source,
                                          context.findName(declared.name), context.findName(name));
                return;
            }

            RecordField built;
            built.name = name;
            built.occurrence = occurrence;
            built.source = source;
            built.index = U16(record->fields.size());

            record->fields.push(program.decl, built);

            /*
             * The default, **deferred** - see `PendingDefault`, where the ordering is argued.
             *
             * Noted here rather than resolved here, and after the duplicate check so that nothing is
             * queued for a field that is not going to exist. A default may name a constructor of a
             * record this pass has not reached yet - `{f: Flag = Off}` above `Flag`, or `Bool`'s
             * `True` from another module - and reading it now would be reading a constructor table
             * still being built.
             */
            if(def) {
                pendingDefaults.push(PendingDefault { here(), current->activeFile, pointer,
                                                      built.index, def });
            }
        };

        if(named) {
            for(auto written: content->tup.fields.contents(parse)) {
                if(!written.name) {
                    unsupported(constructor.source, "a positional field"_v);
                    continue;
                }

                // The field's *own* type position, so that two fields of one constructor report at
                // two carets rather than twice at the constructor's name.
                field(written.name, written.type, written.def ? parse[written.def] : nullptr,
                      written.type.source);
            }
        } else if(content) {
            /*
             * The one-payload spelling, which carries no default and can carry none: what a
             * construction would leave out is the payload itself, and `Proceed()` is then a
             * constructor written with no arguments rather than one taking its default. Only a
             * *named* field may default, which is `doc/spec/declarations.md`'s rule and the reason
             * the parser has nowhere to put one here.
             */
            field(constructor.name, *content, nullptr, content->source);
        }

        declared.count = U16(U16(record->fields.size()) - declared.first);

        /*
         * And what the attribute needs to have been true of the constructor it was written on.
         *
         * Checked here rather than in `readValueAttribute` because it is a fact about the *fields*,
         * which are only in by now. A sum with a payload carries a discriminant beside the payload,
         * and a discriminant numbers the constructors rather than standing for them - so there is
         * nothing for the number to pin.
         */
        if(declared.pinned && declared.count) {
            context.diagnostics.error(
                "`@value` needs a constructor that carries nothing - %@ has a payload, so what the value would name is its tag rather than the value itself"_v,
                declared.source, context.findName(declared.name));
        }

        /*
         * Distinct, which is the one rule pinning can break.
         *
         * Quadratic over the constructor list and deliberately: what it compares is one
         * declaration's own constructors, so the list is the length somebody typed. Reported at the
         * *later* of the two, because that is the one whose number can be changed without
         * renumbering everything after it.
         */
        for(auto existing: record->cons.contents(decl())) {
            if(existing.value != declared.value) continue;

            context.diagnostics.error("%@ and %@ are both %@ - a constructor's value has to be its own"_v,
                                      declared.source, context.findName(existing.name),
                                      context.findName(declared.name), declared.value);
            break;
        }

        /*
         * The name, in the constructor table - which is what a construction and a pattern look up.
         *
         * A record with one constructor registers the type's own name here as well as in the record
         * table, so `Pair {x: 1}` and `Proceed(v)` take one path. A collision is two constructors of
         * one name, which is a name that builds two different types - reported here because this is
         * where both are visible.
         */
        auto reference = ConstructorRef { pointer, declared.index };
        if(!program.addConstructorName(here(), declared.name, reference)) {
            context.diagnostics.error("%@ is already a constructor in this module"_v,
                                      declared.source, context.findName(declared.name));
        }

        record->cons.push(program.decl, declared);
    }

    record->layout = recordLayoutOf(decl(), *record);
    record->originSlots = slots.count();

    /*
     * A record with no fields *and* nothing to tell its values apart has no layout question and no
     * ownership question, and every value of it is the same value - which is `{}`, spelled by a
     * declaration. Refused rather than admitted as a second unit type.
     *
     * `data Colour = Red | Green | Blue` has no fields either and is not that: its values differ,
     * which is exactly what the discriminant is. So the test is the layout rather than the field
     * count, which is the one place the three shapes are told apart.
     *
     * Only where the *declaration* wrote none. A record every one of whose fields was rejected has
     * none either, and saying so would be reporting the consequence of a mistake beside the mistake
     * - which is what turns one bad field type into two messages.
     */
    if(record->fields.size() || record->layout != RecordLayout::Single || anyWritten) return;

    unsupported(record->source, "a record with no fields"_v);
}

void Resolver::declareGlobals(ast::Decl& declaration) {
    // Where this statement's own globals begin - see `TopLevelStmt`, which is what the slice is for.
    auto first = U32(writtenGlobals.size());
    auto root = here() == program.root();

    /*
     * A statement rather than a declaration - Analysis-Initialization.md stage B.
     *
     * The **root** module has a program point at which one runs, and no other module has: the
     * synthesized entry is its top level, and defining a point in a program's startup for a library
     * module's is the cross-module half stage B deliberately leaves out. Root-only is the whole
     * trick - there is exactly one module with executable top-level code, so there is no order
     * between modules to define and no import side effects to say anything about.
     */
    if(declaration.stmt.kind != ast::Expr::Decl) {
        if(!root) {
            context.diagnostics.error(
                "only a `let` declaration can appear at module level - a statement has to run at some point in a program's startup, and only the module being compiled has one"_v,
                declaration.source);
            return;
        }

        topLevel.push(TopLevelStmt { declaration.stmt, declaration.source, current->activeFile,
                                     first, 0 });
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

        /*
         * `let x = e in body` at module level, which is a scope written where there is nothing to
         * scope: what follows a module-level `let` is the rest of the file, and that is already the
         * whole scope of the name it declares.
         *
         * Reported and then declared anyway, so that the rest of the module still reads against the
         * name and one mistake stays one message.
         */
        if(binding.in) {
            context.diagnostics.error(
                "a module-level `let` has no `in` - the name it declares is in scope for the whole module"_v,
                declaration.source);
        }

        auto global = new (program.decl) Global(binding.pat.var, owned(nullptr));
        global->source = binding.pat.source;
        global->writable = binding.bind == ast::BindType::Ref;
        global->exported = declaration.exported;

        auto pointer = global - *program.decl;
        current->globals.push(program.decl, pointer);
        writtenGlobals.push(pointer);

        if(!program.addGlobalName(here(), global->name, pointer)) {
            context.diagnostics.error("%@ is already declared in this module"_v,
                                      binding.pat.source, context.findName(global->name));
        }

        /*
         * `@builtin(role)` - a global the compiler fills or supplies.
         *
         * The attribute is written before the whole `let`, so a statement declaring several names
         * has no way to say which one it meant. Refused rather than applied to the first: the
         * failure it would produce is a role bound to a declaration the author did not intend, which
         * is exactly what this mechanism exists to make impossible.
         *
         * Everything else about the claim - the mutability, the type, the initializer - is
         * `checkRoles`, which runs after initializers because that is when a global has a type at
         * all.
         */
        if(declaration.stmt.decl.size() > 1) {
            if(bindRole(context, program, parse, declaration, Nothing(), 0)) {
                context.diagnostics.error(
                    "`@builtin` is written before the whole `let` and names one declaration - give it a `let` of its own"_v,
                    declaration.source);
            }

            continue;
        }

        bindRole(context, program, parse, declaration, Just(intrinsic::Entity::Global),
                 pointer.offset);
    }

    /*
     * The statement, in the root module's own sequence.
     *
     * A `let` is in it too, and not because it always runs something: an initializer that folds to a
     * constant runs nothing at all. What the entry sequence needs from it is the **order** - a
     * statement above a `let` sees the global as its declaration left it, and a `let` below one sees
     * what the statement wrote - so a `let` is a position in the sequence whether or not it fills
     * one.
     */
    if(root) {
        topLevel.push(TopLevelStmt { declaration.stmt, declaration.source, current->activeFile,
                                     first, U16(writtenGlobals.size() - first) });
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
    for(auto& declaration: eachDeclaration(file)) {
        if(declaration.kind == ast::Decl::Data) declareFields(declaration, nextRecordFields);
    }
}

// The class heads, beside the record shells: a constraint in a signature and an `instance` head both
// name a class, so every class exists before either is read - see `declareClass`.
void Resolver::declareClasses(ast::Module& file) {
    for(auto& declaration: eachDeclaration(file)) {
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
    for(auto& declaration: eachDeclaration(file)) {
        if(declaration.kind == ast::Decl::Trait) declareClassMembers(declaration, nextClassMembers);
    }
}

void Resolver::declare(ast::Module& file) {
    for(auto& declaration: eachDeclaration(file)) {
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
