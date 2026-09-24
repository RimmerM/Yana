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
        auto which = findAttribute(stringView(context.findName(attribute.name)));

        // The two selecting attributes, read by one walk. They are different *axes* - a machine and
        // a policy - and the same *question*: is this declaration part of this compilation. Each
        // restricts on its own, so a declaration carrying both has to satisfy both, which is what
        // `enabled` being a running `and` across every attribute already says.
        if(which != DeclAttribute::Platform && which != DeclAttribute::Allocator) continue;

        // An attribute with no target, or one whose targets are all unreadable, restricts nothing.
        // `checkPlatform` is what says so; here it is the answer that keeps a typo from deleting a
        // declaration from every build while the diagnostic is still being read.
        if(attribute.args.isEmpty()) continue;

        auto matched = false;
        for(auto argument: attribute.args.contents(parse)) {
            if(argument.value.kind != ast::Expr::Var) continue;

            auto name = context.findName(argument.value.var);
            auto answer = which == DeclAttribute::Platform
                ? targetSelector(context.settings, stringView(name))
                : allocatorSelector(context.settings, stringView(name));

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
            auto which = findAttribute(stringView(context.findName(attribute.name)));

            /*
             * `@allocator` has no spelling to judge, and that is the axis rather than an omission.
             * A target name is one of a closed set the compiler knows, so a name outside it is a
             * typo the compiler can see; a provider name is whatever the library declared, and
             * every non-empty one is legal to write. What catches a mistyped provider is the roles:
             * after selection nothing claims `allocate`, and `checkDeclaredRoles` says so once
             * about the program rather than once per declaration that named it.
             */
            if(which == DeclAttribute::Allocator) {
                if(!attribute.args.isEmpty()) continue;

                context.diagnostics.error("`@allocator` needs the name of a provider - the one this program is built against, which `-allocator` selects"_v,
                                          attribute.source);
                continue;
            }

            if(which != DeclAttribute::Platform) continue;

            if(attribute.args.isEmpty()) {
                context.diagnostics.error("`@platform` needs at least one target - a platform (`js`, `native`), a system environment (`linux`, `mac`, `win32`, `bare`), an architecture, an x86-64 level or an instruction-set extension"_v,
                                          attribute.source);
                continue;
            }

            for(auto argument: attribute.args.contents(parse)) {
                if(argument.value.kind != ast::Expr::Var) {
                    context.diagnostics.error("a `@platform` target is a bare name - `js`, `native`, a system environment (`linux`, `mac`, `win32`, `bare`), an architecture, an x86-64 level or an instruction-set extension"_v,
                                              argument.value.source);
                    continue;
                }

                auto name = context.findName(argument.value.var);
                if(targetSelector(context.settings, stringView(name)) != TargetSelector::Unknown) {
                    continue;
                }

                context.diagnostics.error("unknown target %@ - expected `js`, `native`, a system environment (`linux`, `mac`, `win32`, `bare`), an architecture (`x64`, `x86`, `arm`, `arm64`), an x86-64 level (`sse4`, `avx2`, `avx512`) or an instruction-set extension (`aes`, `sha`, `gfni`, ...)"_v,
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
void Resolver::readAttributes(ast::Decl& declaration, bool* explicitDrop, bool* local, StringView on,
                              bool pinned) {
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

            // `@inline` and `@noinline` belong to a function and `readInlineAttribute` reads them
            // there; here they fall to the placement message, as `@value` on a `data` head does.
            case DeclAttribute::Inline:
            case DeclAttribute::NoInline:     break;

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
            // message below, exactly as `@value` on a `data` head does. `@box` is the same statement
            // one axis over: it belongs to a *field*, `declareFields` reads it there, and a
            // declaration is not a field either.
            case DeclAttribute::Bits:
            case DeclAttribute::Box:          break;

            // `@platform` restricts any declaration, so there is no placement to report and this is
            // not its reader - `checkPlatform` judges the spelling and `platformEnabled` applies it,
            // both before this walk runs at all. Skipped rather than reported. `@allocator` is the
            // same statement on the policy axis and is skipped for the same reason.
            case DeclAttribute::Platform:
            case DeclAttribute::Allocator:    continue;

            // `@layout` belongs to a `data` and `readLayoutAttribute` reads it there. `pinned` is
            // non-null exactly where that reader ran, so this walk skips it there and reports the
            // placement message everywhere else - which is what `@inline` does one axis over.
            case DeclAttribute::Layout:       if(pinned) continue; break;

            // `@x86_legacy_sse` belongs to a function and `readSignatureAttributes` reads it there,
            // beside `@inline` and for the same reason: the destination is a field of `Function`
            // and this walk has none. Here it falls to the placement message. `@convention` is the
            // same sentence with a word for an argument, which is why it has a reader of its own.
            case DeclAttribute::LegacySse:
            case DeclAttribute::Convention:   break;

            // `@host` belongs to a **field**, exactly as `@box` does, and `declareFields` reads it
            // there. A declaration is not a field, so this is the placement message.
            case DeclAttribute::Host:         break;

            /*
             * `@capacity(n)` refines a *type* beside `@inline(n)` and `readContainerRefinement`
             * reads the pair there; `@heap` is written in front of a *statement* and
             * `readBindingAttributes` reads it there. Neither has a destination in a declaration's
             * attribute list, so both fall to the placement message - which is the same sentence
             * `@bits` and `@box` get one line up.
             */
            case DeclAttribute::Capacity:
            case DeclAttribute::Heap:         break;

            // `@cold` and `@hot` belong to a function and `readSignatureAttributes` reads them
            // there, beside `@inline` and for its reason: the destination is a field of `Function`
            // and this walk has none.
            case DeclAttribute::Cold:
            case DeclAttribute::Hot:          break;

            // `@replaces` belongs to a function and `declareFunction` reads it there, for the same
            // reason: a type or an instance has no body to be another one of.
            case DeclAttribute::Replaces:     break;

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
 * `@cold` and `@hot` - what a function says about how often it runs.
 *
 * A reader of its own rather than two more arms of `readInlineAttribute`, because the question is a
 * different one: that walk answers "expand this body", and this one answers "how often is this
 * reached". The two meet in the inliner and nowhere else, and a declaration may sensibly write
 * `@noinline @cold` or `@inline @hot` - neither pairing is a contradiction, so neither belongs in
 * the other's exclusion test.
 *
 * Both on one declaration is a contradiction rather than a precedence question, so it is reported
 * and neither applies - `@inline(always) @noinline`'s rule, for its reason: a declaration that says
 * two opposite things has stated no intent, and taking the one written first would make the answer
 * depend on the order.
 */
void Resolver::readFrequencyAttributes(ast::Decl& declaration, bool& cold, bool& hot) {
    cold = false;
    hot = false;

    for(auto attribute: declaration.attributes.contents(parse)) {
        auto which = findAttribute(stringView(context.findName(attribute.name)));
        if(which != DeclAttribute::Cold && which != DeclAttribute::Hot) continue;
        if(!takesNoArguments(attribute, which)) continue;

        if(which == DeclAttribute::Cold) cold = true;
        else hot = true;
    }

    if(cold && hot) {
        context.diagnostics.error("`@cold` and `@hot` say opposite things about the same function"_v,
                                  declaration.source);
        cold = false;
        hot = false;
    }
}

/*
 * What a function says about being expanded at its call sites - `Function::alwaysInline`,
 * `Function::inlineHint` and `Function::noInline`.
 *
 * Three spellings and three answers, which is a correction: a bare `@inline` used to be read and
 * dropped here, on the argument that what an optimizer does with a hint is its own business. It is,
 * and that is an argument about the *number* rather than about the flag - the number is stated once,
 * in `resolve_ssa/opt/inline.cpp`'s `Budget`, and what this stage owes it is the fact that the
 * author wrote the attribute at all. `@inline(always)` is a promise the *resolver* keeps, by
 * grafting the body where it is called. `@noinline` is the promise in the other direction, and is
 * the one of the three that can never be refused: declining to expand a body is always possible.
 *
 * One reader for both attributes rather than one each, because what they say is one thing and the
 * contradiction is only visible to something that has read both. `@inline(always) @noinline` is
 * that contradiction, and it is reported here with neither answer taken - a declaration that says
 * two opposite things has stated no intent for the compiler to follow, and picking the one written
 * first would make the answer depend on the order.
 *
 * The one argument `@inline` takes is the word `always`; a number or a second word is reported,
 * because an attribute that is silently misread is a declaration whose author believes something
 * the compiler does not. `@noinline` takes none at all.
 */
void Resolver::readInlineAttribute(ast::Decl& declaration, bool& always, bool& hint, bool& never,
                                   bool& legacySse) {
    always = false;
    hint = false;
    never = false;
    legacySse = false;

    for(auto attribute: declaration.attributes.contents(parse)) {
        auto which = findAttribute(stringView(context.findName(attribute.name)));

        /*
         * `@x86_legacy_sse`, read on the same walk as the inline pair rather than on one of its own.
         *
         * Not because they are related - one is about expansion and the other about encoding - but
         * because both are flags on a *signature* whose destination is a field of `Function`, and
         * this is the walk that has one in hand. A reader of its own would be the same loop over the
         * same list with one arm in it.
         */
        if(which == DeclAttribute::LegacySse) {
            if(!takesNoArguments(attribute, which)) continue;

            legacySse = true;
            continue;
        }

        if(which == DeclAttribute::NoInline) {
            if(!attribute.args.isEmpty()) {
                context.diagnostics.error("`@noinline` takes no arguments"_v, attribute.source);
                continue;
            }

            never = true;
            continue;
        }

        if(which != DeclAttribute::Inline) continue;

        // The bare spelling, which is the hint. `always` is the only argument the attribute takes,
        // so no arguments is the whole of what distinguishes the two.
        if(attribute.args.isEmpty()) {
            hint = true;
            continue;
        }

        auto argument = attribute.args.get(parse, 0).value;
        if(attribute.args.size() != 1 || argument.kind != ast::Expr::Var ||
           stringView(context.findName(argument.var)) != "always"_v) {
            context.diagnostics.error("`@inline` takes no argument, or the one word `always`"_v,
                                      attribute.source);
            continue;
        }

        always = true;
    }

    if(always && never) {
        context.diagnostics.error(
            "`@inline(always)` and `@noinline` say opposite things about the same function"_v,
            declaration.source);

        always = false;
        never = false;
    }

    // A hint beside the directive that refuses it is the same contradiction one step weaker, and it
    // is answered rather than reported: `@noinline` is what the compiler can always carry out, so a
    // hint under it is a term that would be weighed and then thrown away.
    if(never) hint = false;
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
 * `alias qualified Id = Int` read as the `data` it is.
 *
 * `doc/spec/types.md`: "past the declaration a newtype is an ordinary single-constructor record
 * whose one constructor has the type's own name", and that sentence is this function. Every pass
 * below sees a record, so nothing after this point knows the form exists - which is what keeps a
 * newtype's construction, its pattern, its `.0`, its instances and its layout from each being a
 * second rule about a second kind of declaration.
 *
 * The **one-payload** constructor and not a named field: `Id(5)` is how one is made and `id.0` is
 * how the content is read, which is exactly what `Proceed(a)` already is.
 *
 * `qualified` does **not** travel with it, and the record below is built unqualified on purpose.
 * The word means two different things in the two places it is written: on an `alias` it selects the
 * newtype form, and on a `data` it keeps the constructors out of the module namespace. A newtype's
 * constructor is the type's own name and `doc/spec/types.md` says "there is no way to spell a
 * different constructor name", so `StringId(4)` is the only spelling there is and has to be in the
 * namespace to be written at all. The shipping stage says the same thing by passing
 * `qualified = false` from `declareNewtype`.
 *
 * Built where it is read and never written back into the file. The parse tree is the *source*: the
 * other resolve stage reads the same declarations and an editor reads them again, and a declaration
 * that changed kind underneath them would be one program with two readings. Two small allocations
 * in the parse region per newtype per pass is what that costs.
 */
ast::Decl& Resolver::newtypeRecord(ast::Decl& declaration) {
    auto& written = declaration.alias;

    ast::ParseList<ast::Con> cons;
    cons.push(context.parseRegion, ast::Con {
        written.type.name,
        new (context.parseRegion) ast::Type(written.target) - *context.parseRegion,
        ast::AttrList {},
        declaration.source,
    });

    auto record = new (context.parseRegion) ast::Decl {
        .data = { cons, written.type, ast::ConstraintList {} },
        .attributes = declaration.attributes,
        .source = declaration.source,
        .kind = ast::Decl::Data,
        .exported = declaration.exported,
        .qualified = false,
    };

    return *record;
}

// Whether this declaration is one `newtypeRecord` answers for - the one test, so that the two passes
// that walk records agree about which declarations are records.
static bool isNewtype(ast::Decl& declaration) {
    return declaration.kind == ast::Decl::Alias && declaration.qualified;
}

/*
 * `@layout(c)` and `@layout(packed)` on a `data` - `doc/spec/repr.md`'s pinned layouts.
 *
 * A reader of its own for `@value`'s reason: the argument is a word rather than nothing, so
 * `readAttributes` - which fills flags - is not where it can land. What it produces is a
 * `TypeLayout` on the record, and what that is *for* is stated there: the pin travels with the
 * logical type so that every family's layout table reads the same statement.
 *
 * `auto` is admitted and means the default, which is not the same as writing nothing: it is how a
 * declaration says out loud that its layout is the compiler's, and it composes with a second
 * attribute the way an unwritten one does. Two pins that disagree are refused rather than resolved
 * by order - two different `@layout` names name two representations and neither is more written than
 * the other.
 */
TypeLayout Resolver::readLayoutAttribute(ast::Decl& declaration) {
    auto pin = TypeLayout::Auto;

    for(auto attribute: declaration.attributes.contents(parse)) {
        if(findAttribute(stringView(context.findName(attribute.name))) != DeclAttribute::Layout) {
            continue;
        }

        if(attribute.args.size() != 1) {
            context.diagnostics.error("`@layout` takes one name - `@layout(c)`, `@layout(js)`, `@layout(packed)` or `@layout(auto)`"_v,
                                      attribute.source);
            continue;
        }

        auto argument = attribute.args.get(parse, 0).value;
        auto named = argument.kind == ast::Expr::Var ? stringView(context.findName(argument.var))
                                                     : StringView();

        auto requested = named == "c"_v ? TypeLayout::C
                       : named == "js"_v ? TypeLayout::Js
                       : named == "packed"_v ? TypeLayout::Packed
                       : named == "auto"_v ? TypeLayout::Auto
                       : TypeLayout::Unknown;

        if(requested == TypeLayout::Unknown) {
            context.diagnostics.error("`@layout` takes `c`, `js`, `packed` or `auto` - a representation this declaration pins, or the compiler's own"_v,
                                      argument.source);
            continue;
        }

        if(requested == TypeLayout::Auto) continue;

        if(pin != TypeLayout::Auto && pin != requested) {
            context.diagnostics.error("`@layout` names different representations and cannot apply more than one pin to a declaration"_v,
                                      argument.source);
            continue;
        }

        pin = requested;
    }

    return pin;
}

/*
 * `@convention(clobber)` on a signature - the calling convention this function is entered under.
 *
 * The name table is `lower/convention.h`'s, shared with the lower IR's parser and printer, because
 * the three spellings of one convention have to agree and a fixture round-trips through all three.
 *
 * **Not every convention in that table may be written**, and `conventionWritableInSource` is where
 * the set is argued: `simple` and `complex` are the backend's choice between two internal shapes and
 * `system` describes what a syscall leaves alone, so naming one writes an implementation detail down
 * as an interface. The messages are the shipping stage's word for word - one language, one
 * diagnostic - and `Reject.VectorReset.yana` is what holds the two to it.
 *
 * Read here rather than only on a plain `fn`, which is where this stage and the shipping one differ:
 * `readInlineAttribute` already reads `@x86_legacy_sse` off every signature, and one walk that reads
 * a signature's backend facts is worth more than a placement rule the two attributes would then have
 * to state separately. A class member that names a convention says something a call through the
 * class cannot honour, and that is worth a diagnostic of its own the day one is written.
 */
void Resolver::readConventionAttribute(ast::Decl& declaration, Function& function) {
    for(auto attribute: declaration.attributes.contents(parse)) {
        if(findAttribute(stringView(context.findName(attribute.name))) !=
           DeclAttribute::Convention) {
            continue;
        }

        if(attribute.args.size() != 1) {
            context.diagnostics.error("`@convention` takes one argument - the name of a calling convention, as in `@convention(clobber)`"_v,
                                      attribute.source);
            continue;
        }

        auto argument = attribute.args.get(parse, 0).value;
        if(argument.kind != ast::Expr::Var) {
            context.diagnostics.error("`@convention` takes the name of a calling convention, written as an identifier"_v,
                                      attribute.source);
            continue;
        }

        auto found = callTypeForName(argument.var);
        if(!found) {
            context.diagnostics.error("unknown calling convention %@ - the ones a declaration may name are `clobber`, `sysv`, `win64` and `x86_interrupt`"_v,
                                      attribute.source, context.findName(argument.var));
            continue;
        }

        if(!conventionWritableInSource(found.unwrap())) {
            context.diagnostics.error("%@ is not a convention a declaration may name - `simple` and `complex` are the compiler's own choice between two internal shapes, and `system` describes what a syscall leaves alone rather than how anything is entered. Write `clobber`, `sysv`, `win64` or `x86_interrupt`"_v,
                                      attribute.source, context.findName(argument.var));
            continue;
        }

        if(function.convention) {
            context.diagnostics.error("this declaration already names a calling convention"_v,
                                      attribute.source);
            continue;
        }

        function.convention = found;
    }
}

/*
 * Where a record's `@host` fields may sit, which is after the one field that stores the value.
 *
 * The elision makes the record *be* its remaining field, exactly as a one-field record already is on
 * a target with no layout - so there has to be exactly one such field and the properties hang off it
 * rather than the other way round. Pinning it to field zero costs nothing: `{items: %a, @host
 * length: Count}` is the declaration and is also the only order a reader would write, and every walk
 * that already answers "a transparent record is its first field" keeps answering it.
 *
 * A **name** is required, because the name *is* the property: `length` is what the host is asked
 * for, and a positional field has nothing to ask for.
 *
 * What this cannot check is whether the claim is *true* - that is a question about a host value
 * rather than about a type, and the JS pipeline is where it is answered.
 */
void Resolver::checkHostFields(RecordType& record, U16 first, LocationId source) {
    auto count = U16(record.fields.size());

    auto elided = false;
    for(U16 i = first; i < count; i++) elided = elided || record.field(decl(), i).host;
    if(!elided) return;

    if(count - first < 2 || record.field(decl(), first).host) {
        context.diagnostics.error("`@host` names a property of the value the record holds, so it has to follow the one field that holds it - field zero of the record, which may not itself be `@host`"_v,
                                  source);
        return;
    }

    for(U16 i = U16(first + 1); i < count; i++) {
        if(record.field(decl(), i).host) continue;

        context.diagnostics.error("a record with a `@host` field has exactly one stored field, and this one has more - every field after the first has to be a host property of it"_v,
                                  source);
        return;
    }

    for(U16 i = U16(first + 1); i < count; i++) {
        if(record.field(decl(), i).name) continue;

        context.diagnostics.error("a `@host` field is reached by its own name and so has to have one"_v,
                                  source);
        return;
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
    for(auto& each: eachDeclaration(file)) {
        if(each.kind != ast::Decl::Data && !isNewtype(each)) continue;

        auto& declaration = isNewtype(each) ? newtypeRecord(each) : each;
        auto& written = declaration.data;
        auto push = [&](DeclPtr<RecordType> record) { writtenRecords.push(record); };

        // `data (Eq(a)) Set(a)` - the head's own constraints, read in `declareRecordFields` for the
        // reason the fields are: a constraint names a class, and no class exists yet.

        /*
         * The constructors - one for `data Pair {x: I32}`, several for
         * `data Outcome(a, e) = Proceed(a) | Exit(e)`.
         *
         * **A constructor's name has nothing to do with its type's**, however many there are.
         * `doc/spec/declarations.md` states it as the rule that `data Direction = North` declares a
         * nullary constructor `North` and "never a type named `North`", and the two tables here say
         * the same thing from the other side: `Program::recordsByName` and
         * `Program::constructorsByName` answer different questions, so `data RandomError =
         * NoEntropySource` files one name in each and neither is a second spelling of the other.
         * The braced form is the case where the two names coincide, and the parser is what makes
         * them coincide - it synthesizes a constructor called after the type. Nothing here has to
         * know that happened.
         */

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
        readAttributes(declaration, &record->writtenExplicitDrop, &record->local, "a type"_v, true);

        // The declared pin - see `RecordType::pin`. Read on the shell for the same reason the two
        // flags above are: it is a fact about the declaration, and a record whose fields were
        // rejected still carries what its declaration said about its bytes.
        record->pin = readLayoutAttribute(declaration);

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

        recordDefinition(context, typeSymbol(decl(), asBase<Type>(pointer)));
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
         * `alias qualified Flags = I64` - a **type** rather than a spelling, and therefore not this
         * pass's.
         *
         * The transparent form below is free precisely because it is not a type: it has no identity
         * of its own, so nothing converts to or from it, no instance can be attached to it and the
         * ownership classification never sees one. A newtype is the opposite of all three, so it is
         * declared where records are - `newtypeRecord`, which is where the whole of the difference
         * between the two forms is written down.
         */
        if(declaration.qualified) continue;

        // A `deriving` clause needs no check of its own: the parser admits one only on a qualified
        // alias, which the line above has already left, because a plain alias names the same type
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
        recordDefinition(context, aliasSymbol(decl(), pointer));
        writtenAliasDecls.push(&declaration);
    }
}

bool Resolver::withinPacked(Size count, LocationId source, StringView what) {
    if(count < kMaxPackedPositions) return true;

    context.diagnostics.error("a declaration may have at most %@ %@, and this one has more"_v,
                              source, U64(kMaxPackedPositions), what);
    return false;
}

bool Resolver::roomForVariable(DeclPtr<GenContext> gen, StringId name, LocationId source) {
    if(!gen || findGenVariable(decl(), gen, name)) return true;
    return withinPacked(decl()[gen]->variables.size(), source, "type parameters"_v);
}

bool Resolver::roomForSlot(SlotScope& slots, StringId name, LocationId source) {
    if(!slots.full() || slots.find(name, false) != SlotScope::kNone) return true;
    return withinPacked(slots.names.size(), source, "origin slots"_v);
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

        if(!roomForVariable(gen, parameter.name, parameter.source)) break;
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

namespace {

/*
 * The logical bits a packed field spends, or zero when the field owns whole bytes.
 *
 * This is deliberately the resolver's small structural check rather than a second native layout
 * walk. `@bits` has a fixed width by the time fields are declared, and a payload-free enum's values
 * are known from its constructors. The comparison is against the refined integer's *base* storage,
 * not the logical width: `@bits(8) U64` is an eight-bit packed field, not an eight-byte ordinary
 * field. Everything else is already byte-sized at this layer; the native representation remains
 * responsible for the exact byte count and the access word it uses.
 */
static U32 packedNarrowBits(DeclBase base, DeclPtr<Type> type) {
    if(!type) return 0;

    auto declared = base[type];
    if(declared->kind == Type::Int && isRefinedInt(base, type)) {
        auto bits = U32(((IntType*)declared)->bits);
        auto canonical = canonicalInt(base, type);
        auto natural = U32(storageBitsFor(U8(((IntType*)base[canonical])->bits)));
        return bits < natural ? bits : 0;
    }

    if(declared->kind != Type::Record) return 0;

    auto& record = *(RecordType*)declared;
    if(record.layout != RecordLayout::Enum) return 0;

    auto range = tagRangeOf(base, record);
    if(range.isSigned || range.highest < 0) return 0;

    U32 bits = 1;
    while(bits < 64 && (U64(1) << bits) <= U64(range.highest)) bits++;
    return bits < U32(range.bits) ? bits : 0;
}

static void checkPackedFields(Context& context, Program& program, RecordType& record) {
    auto base = *program.decl;
    U32 bits = 0;

    for(U16 i = 0; i < record.fields.size(); i++) {
        auto field = record.fields.get(base, i);
        auto narrow = packedNarrowBits(base, field.occurrence.type);
        if(narrow) {
            bits += narrow;
            continue;
        }

        if(bits % 8) {
            context.diagnostics.error(
                "a `@layout(packed)` record cannot leave a partial byte before an ordinary field"_v,
                field.source);
            return;
        }

        // The exact number of whole bytes is a native representation question. For this check only
        // the byte boundary matters, and an ordinary field always leaves the cursor on one.
        bits = 0;
    }

    if(bits % 8) {
        context.diagnostics.error(
            "a `@layout(packed)` record must fill its final byte with bit-fields"_v, record.source);
    }
}

} // namespace

void Resolver::declareFields(ast::Decl& declaration, Size& next) {
    if(next >= writtenRecords.size()) return;

    auto pointer = writtenRecords[next++];
    if(!pointer) return;

    // A `data` naming `f.Iter` in one of its fields waits for every declaration - `deferFields`,
    // where the reason is. The slot in `writtenRecords` is consumed either way, so the pairing this
    // pass is joined to the next by is unaffected.
    if(deferFields(declaration, pointer)) return;

    declareFieldsOf(declaration, pointer);
}

/*
 * One `data`'s constructors and fields, into the record shell `declareTypes` made.
 *
 * Split from the walk above so that the drain has an entry point that is not driven by
 * `writtenRecords`' cursor - see `Resolver::pendingSiteFields`.
 */
void Resolver::declareFieldsOf(ast::Decl& declaration, DeclPtr<RecordType> pointer) {
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

    // The number the next unpinned constructor takes. Declaration order is what this counts when
    // nothing pins anything, which is what makes a declaration that writes no attribute the
    // declaration order it always had - `doc/spec/declarations.md`, and C's rule.
    I64 following = 0;

    for(auto constructor: declaration.data.cons.contents(parse)) {
        if(!withinPacked(record->cons.size(), constructor.source, "constructors"_v)) break;

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

        auto field = [&](StringId name, ast::Type& type, ast::Expr* def, LocationId source,
                         bool anonymous = false) {
            /*
             * The one position `@box` is legal in - `Resolver::readingFieldType`, which
             * `resolveType` clears as soon as it has read the attribute off this type.
             */
            Occurrence occurrence;
            readingFieldType = true;
            auto resolved = resolveOccurrence(type, ast::BindType::Borrow, Position::Field, slots,
                                              occurrence);
            readingFieldType = false;

            // Reported already; what is left is to stop its uses reporting it again - see
            // `RecordType::broken`.
            if(!resolved) {
                record->broken = true;
                return;
            }
            if(!withinPacked(record->fields.size(), source, "fields"_v)) return;

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
            built.anonymous = anonymous;

            /*
             * `@box` - the field's storage is a pointer to its value rather than the value.
             *
             * Read here rather than in `resolveOccurrence`, and that is the whole difference from
             * `@bits`: this one says nothing about the type. `cfg.cold` is a `Diagnostics` whether
             * or not the attribute is written, so the occurrence above is already the answer and
             * what is left is a fact about the *field*.
             */
            built.boxed = readBoxAttribute(type);
            if(built.boxed) program.usesBoxes = true;

            // `@host`, read in the same position and spent the same way - `RecordField::host`, where
            // the reason this stage records a fact it never reads is written.
            built.host = readHostAttribute(type);

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
            field(constructor.name, *content, nullptr, content->source, true);
        }

        declared.count = U16(U16(record->fields.size()) - declared.first);

        // And where the constructor's `@host` fields may sit, now that there is a field list to ask
        // about. Per constructor rather than per record: two arms of a sum are two shapes over one
        // storage, so which field holds the value is each arm's own question.
        checkHostFields(*record, declared.first, content ? content->source : constructor.source);

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

            record->valuesCollide = true;
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
         *
         * **`data qualified` keeps its constructors out of this table**, which is the whole of what
         * the keyword does: `doc/spec/declarations.md`'s "either way `Record.Constructor` works, so
         * `qualified` removes a spelling rather than adding one - it is for types whose constructor
         * names are too generic to occupy the module namespace". `lib/Date/Format.yana` is exactly
         * that case, with `TimeUnit.Micros` beside `TimeDetail`'s bare `Micros`.
         *
         * No exception for a constructor spelled like its type: `data qualified Wrapped =
         * Wrapped(Int)` is written `Wrapped.Wrapped(7)`, which reads oddly and is what the spec
         * says. A **newtype** is not that case - `newtypeRecord` builds it unqualified, and the
         * paragraph there is where the two meanings of the word are told apart.
         */
        auto reference = ConstructorRef { pointer, declared.index };
        auto reachable = declaration.qualified
            || program.addConstructorName(here(), declared.name, reference);

        if(!reachable) {
            context.diagnostics.error("%@ is already a constructor in this module"_v,
                                      declared.source, context.findName(declared.name));
        }

        record->cons.push(program.decl, declared);
        recordDefinition(context, constructorSymbol(decl(), reference));
    }

    record->layout = recordLayoutOf(decl(), *record);
    record->originSlots = slots.count();

    if(record->pin == TypeLayout::Packed && record->layout == RecordLayout::Multi) {
        context.diagnostics.error(
            "`@layout(packed)` cannot be used on a sum type whose constructors carry data - the tag and payload have no padding-free representation"_v,
            declaration.source);
    } else if(record->pin == TypeLayout::Packed) {
        checkPackedFields(context, program, *record);
    }

    /*
     * There is no refusal here any more, and what used to be one is worth keeping written down.
     *
     * A single-constructor record with no fields was reported as "a record with no fields", on the
     * argument that every value of it is the same value and `{}` already spells that. The shape it
     * actually caught was `data Marker = Marker` - a **nullary constructor**, which
     * `doc/spec/declarations.md` declares legal in as many words, and which is what
     * `data RandomError = NoEntropySource` is once a constructor's name is free of its type's.
     * `data Unit {}` was never it: the braced form of an empty content parses as `{}` the *type*,
     * so that declaration has one field carrying a unit and is a different shape entirely.
     *
     * The type is zero bytes with one value, which is a thing a program passes, matches and names
     * in a signature - `Result(RandomError, {})` is the library's own use of it.
     */
}

void Resolver::declareGlobals(ast::Decl& declaration) {
    // Where this statement's own globals begin - see `TopLevelStmt`, which is what the slice is for.
    auto first = U32(writtenGlobals.size());
    auto root = here() == program.root();

    /*
     * A test file's `let` may be initialized by code, on the same terms the root module's is -
     * Design-Test.md §11.2's F5.
     *
     * The rule was never about being the root. It is about whether there is a program point at which
     * the initializer runs, and a test build has one: `resolveTestEntry` runs the top level ahead of
     * the cases exactly as the ordinary entry runs it ahead of `main`. What the root has that a
     * library module does not is *an entry at all*, and under `-test` every module with a test file
     * in it does too.
     *
     * Two things keep that from widening into library semantics. A test file is selected in and out
     * as a **unit**, so in a build without `-test` the global does not exist and neither does the
     * code that would have filled it - "a dynamic global exists only where something runs its
     * initializer" then holds by construction rather than by argument. And only a `let` is admitted,
     * never a bare statement: the program's start stays one file's statements in written order.
     */
    auto testFile = context.settings.test && activeTestFile;
    auto startup = atProgramPoint();

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
        /*
         * A test file gets initializers, not a second program start - so a bare statement is refused
         * there even when the module around it is the root. Two files' statement sequences would
         * need an order between them, and the only order this compiler states is the one written
         * inside a single file.
         */
        if(testFile) {
            context.diagnostics.error(
                "only a `let` declaration can appear at a test file's module level - its declarations are initialized before the cases run, but a program's start is one file's statements in written order and a test file is not that file"_v,
                declaration.source);
            return;
        }

        if(!root) {
            context.diagnostics.error(
                "only a `let` declaration can appear at module level - a statement has to run at some point in a program's startup, and only the module being compiled has one"_v,
                declaration.source);
            return;
        }

        topLevel.push(TopLevelStmt { declaration.stmt, declaration.source, here(),
                                     current->activeFile, first, 0 });
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

        recordDefinition(context, globalSymbol(program, pointer));

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
     * The statement, in the sequence that will run it - the root module's own, or the one the test
     * entry runs ahead of the cases.
     *
     * A `let` is in it too, and not because it always runs something: an initializer that folds to a
     * constant runs nothing at all. What the sequence needs from it is the **order** - a statement
     * above a `let` sees the global as its declaration left it, and a `let` below one sees what the
     * statement wrote - so a `let` is a position in the sequence whether or not it fills one.
     */
    if(startup) {
        auto& into = testFile ? testTopLevel : topLevel;
        into.push(TopLevelStmt { declaration.stmt, declaration.source, here(),
                                 current->activeFile, first,
                                 U16(writtenGlobals.size() - first) });
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
        // The same two kinds `declareTypes` walked and in the same order, which is what
        // `writtenRecords` requires of the pair - see `declareFields`.
        if(declaration.kind == ast::Decl::Data) declareFields(declaration, nextRecordFields);
        else if(isNewtype(declaration)) declareFields(newtypeRecord(declaration), nextRecordFields);
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
            /*
             * `deriving (Bitwise, Truth)` - one instance per class named, expanded here because this
             * is where instances are declared and the expansion produces one.
             *
             * The expansions are kept rather than rebuilt, in the order they were made:
             * `resolveBodies` walks the same declarations and needs the same ASTs to resolve the
             * member bodies out of. A null is a class that could not be derived and has reported
             * why, and it is kept too so that the two walks stay in step.
             */
            case ast::Decl::Alias: {
                if(!declaration.qualified) break;

                for(auto derive: declaration.alias.derives.contents(parse)) {
                    auto expanded = deriveInstance(declaration, derive);
                    derivedInstances.push(expanded);

                    if(expanded) declareInstance(*expanded);
                }

                break;
            }
            // Read by their own passes, above - `declareTypes`, `declareAliases`, `declareClasses`.
            case ast::Decl::Data:
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
