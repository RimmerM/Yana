/*
 * Types and globals: what a declaration adds to the module.
 *
 * Split in two on purpose, and every type here goes through both halves. Declaring a record creates
 * it and its constructors without looking at what its fields hold; defining one fills them in. A
 * field whose type is a record declared later is the reason - the second half runs when every name
 * in the module exists, so it can resolve one, and the first half is what makes that true.
 */

#include "module_internal.h"
#include "analyze.h"
#include "const.h"
#include "core.h"
#include "expr.h"
#include "generic.h"
#include "host.h"
#include "index.h"
#include "name.h"
#include "native.h"
#include "verify.h"
#include "witness.h"
#include "../parse/ast.h"

/*
 * Generic contexts.
 */

/*
 * The arguments of one written class constraint - Implementation-Const-Generics.md §10.3.
 *
 * Each one goes through `resolveAppArg`, which is the same function a written type application uses,
 * so which positions are counts is read off the *class's* declared parameter list rather than
 * guessed from the argument's shape. That is the third place the same rule is needed; `Vec` and
 * `resolveApp` are the first two.
 *
 * A bare `a` needs no arm of its own. `resolveType`'s `Gen` case calls `genVariable`, which creates
 * the variable in an open context - so `fn (Num(a)) inc(x: a)` still introduces `a` here, exactly as
 * the list of bare identifiers this replaced did - and reports "unknown type variable" in a closed
 * one, which is the right message for a head naming something its parameter list does not.
 *
 * The arity is checked here because nothing downstream does. `superclassPath` bails on a length
 * mismatch and `hasClassRequirement` never matches one, so a constraint of the wrong arity is
 * decoration that quietly proves nothing - `fn (Num(a, b)) f(x: a)` was accepted in silence.
 */
static void resolveConstraintArgs(Module& module, ClassConstraint& entry, GenEnv* env) {
    auto global = *module.types;

    // Null when the class itself is unknown, which makes every position a type position - the right
    // reading when there is no declaration to say otherwise, and the arity check below is skipped
    // rather than reported against a class that was already reported.
    auto declared = entry.typeClass ? global[entry.typeClass]->gen : nullptr;
    Size index = 0;

    for(auto arg: entry.written.contents(module.parse)) {
        entry.args.push(module.types, resolveAppArg(module, declared, index, arg, env));
        index++;
    }

    if(!declared) return;

    auto arity = global[declared]->types.size();
    if(index != arity) {
        module.context.diagnostics.error("class %@ takes %@ arguments but was given %@"_v,
                                         entry.source, module.context.findName(entry.name),
                                         U32(arity), U32(index));
    }
}

/*
 * Builds the generic context of one declaration: its declared type variables, then the class
 * constraints written over them.
 *
 * `open` says whether a type variable the constraints mention introduces itself. A function and an
 * instance have no declared variable list, so it has to - and it has to be set *here* rather than
 * by the caller afterwards, because a constraint's own types are resolved below: `f: (a) -> Int`
 * mentions `a` before the signature that would otherwise have introduced it, and a context that
 * only opened after this returned would have rejected it.
 *
 * ## Two passes, and why only `Const` moves
 *
 * The constraint list is walked twice - Implementation-Const-Generics.md §10.3. The first pass takes
 * the `Const` entries and nothing else; the second takes the rest in written order.
 *
 * A const parameter has to be declared before anything *resolves a type mentioning it*, because
 * §1.5's inference in `resolveCountVariable` will otherwise create it: `fn (Num(Vec(I16, n)), n: Int)`
 * would have the class constraint invent `n` as a count of `Int`, and the annotation beside it would
 * then report §2.2's collision about a declaration that is perfectly correct. Splitting it out means
 * the two may be written in either order.
 *
 * Only `Const` moves. `Any` stays where it is, so the order in which a context introduces its
 * variables - and therefore every `GenType::index` in it - is byte-identical for a context with no
 * const parameter, which is every context that existed before this feature.
 */
GlobalPtr<GenEnv> prepareGenEnv(Module& module, GenEnv::Kind kind,
                                       ast::ParseList<ast::GenParam> variables,
                                       ast::ConstraintList constraints, bool open) {
    auto env = new (module.types) GenEnv(kind);
    auto pointer = env - *module.types;
    env->open = open;
    env->module = &module;

    auto fresh = false;
    auto addVariable = [&](StringId variableName, LocationId source) -> GlobalPtr<GenType> {
        fresh = false;

        for(auto existing: env->types.contents(*module.types)) {
            if((*module.types)[existing]->name == variableName) return existing;
        }

        fresh = true;
        auto type = new (module.types) GenType(pointer, variableName, U16(env->types.size()));
        type->source = source;
        auto typePointer = type - *module.types;
        env->types.push(module.types, typePointer);
        return typePointer;
    };

    /*
     * `n: Int` - a const parameter, Implementation-Const-Generics.md §1.1 and §1.2.
     *
     * The declared kind, which is the case §1.5 requires in a head and allows in a signature. It is
     * set here rather than inferred because it is *written*: a variable whose annotation says what
     * it is has nothing left to work out, and the inference in type_ast.cpp is only for the variable
     * a private function leaves to the position that uses it.
     */
    auto declareConst = [&](GlobalPtr<GenType> variable, bool created,
                            ast::ParsePtr<ast::Type> annotation, LocationId source) {
        auto type = (*module.types)[variable];

        /*
         * A name this list already introduced, declared again as a const parameter - §2.2's
         * collision seen at declaration rather than at a use.
         *
         * Reported instead of being resolved by the second entry winning, because the two readings
         * of `fn (a, a: Int) f(x: a)` are a type and a number and neither is the obvious one.
         */
        if(!created) {
            module.context.diagnostics.error("%@ is already a parameter of this declaration - a name is a type parameter or a const parameter and not both"_v,
                                             source, module.context.findName(type->name));
            return;
        }

        auto declared = resolveType(module, *module.parse[annotation], (*module.types)[pointer]);

        // The kind is set even when the *type* was refused, so that one inadmissible annotation is
        // one diagnostic: leaving it a type parameter would make every count position that mentions
        // it report §2.2's collision as well, about a declaration that already knows it is wrong.
        type->kind = GenKind::Const;
        type->constType = admissibleConstType(module, declared, source) ? declared : module.scalar.int_;
    };

    /*
     * `a = Int`, `n: Int = 4` - recorded here and resolved on demand, see resolveWrittenDefaults.
     *
     * Recorded rather than resolved because a head is built in declaration order and a default may
     * name a type declared further down. What the index is taken from is the *variable*, not the
     * loop counter, so a head that named one parameter twice - which is already a diagnostic -
     * writes its default onto the parameter it names.
     */
    auto declareDefault = [&](GlobalPtr<GenType> variable, ast::ParsePtr<ast::Type> written,
                              LocationId source) {
        if(!written) return;

        if(env->writtenDefaults.isEmpty()) module.defaultedContexts.push(pointer);

        env->writtenDefaults.push(module.types, GenDefault {
            (*module.types)[variable]->index, written, source,
        });
    };

    for(auto variable: variables.contents(module.parse)) {
        auto added = addVariable(variable.name, variable.source);
        if(variable.type) declareConst(added, fresh, variable.type, variable.source);
        declareDefault(added, variable.def, variable.source);
    }

    // Pass one - see the header. Every `Const` entry, so that a type resolved below which mentions
    // one finds it declared rather than inferring a second reading of it.
    for(auto constraint: constraints.contents(module.parse)) {
        if(constraint.kind != ast::Constraint::Const) continue;

        // `fn (n: Int) lane(v: Vec(Float, n), i: Int)` - §1.2. A context declares a const parameter
        // in the same list it declares everything else in, so this is one arm and no second list.
        auto variable = addVariable(constraint.constant.name, constraint.source);
        declareConst(variable, fresh, constraint.constant.type, constraint.source);
        declareDefault(variable, constraint.constant.def, constraint.source);
    }

    for(auto constraint: constraints.contents(module.parse)) {
        switch(constraint.kind) {
            case ast::Constraint::Error:
            case ast::Constraint::Const:
                break;
            case ast::Constraint::Any:
                addVariable(constraint.name, constraint.source);
                break;
            case ast::Constraint::Class: {
                ClassConstraint entry;
                entry.name = constraint.klass.name;
                entry.source = constraint.source;
                entry.written = constraint.klass.args;

                /*
                 * Resolved here when the class is already declared, and deferred otherwise - §10.4.
                 *
                 * `findClass` cannot answer inside the pass that *declares* classes, which is where
                 * a `data` or `class` head is built. Those two contexts are closed, so a constraint
                 * of theirs introduces nothing and there is no ordering to lose by finishing them in
                 * resolveConstraintClasses.
                 *
                 * An *open* context - a function's or an instance's - is built two passes later, by
                 * which point every class exists. So a lookup that fails there is a name that does
                 * not exist rather than one not read yet, and the arguments are resolved anyway:
                 * that is what keeps `fn (Foo(a)) f(x: a)` to the one diagnostic naming `Foo`,
                 * instead of following it with an "unknown type variable a" about a variable the
                 * constraint would have introduced.
                 */
                entry.typeClass = findClass(module, entry.name, constraint.source);
                if(entry.typeClass || open) resolveConstraintArgs(module, entry, env);

                env->classes.push(module.types, entry);
                break;
            }
            case ast::Constraint::Field: {
                // `a.field: b` - a relation between two slots of *this* context rather than a fact
                // attached globally to `a`, which is what makes `Serialize`-shaped constraints
                // expressible at all. What satisfies it is a PropertyWitness for that one field.
                auto owner = addVariable(constraint.field.typeName, constraint.source);

                PropertyConstraint entry;
                entry.owner = (Type*)(*module.types)[owner] - *module.types;
                entry.field = constraint.field.fieldName;
                entry.source = constraint.source;
                entry.result = resolveType(module, *module.parse[constraint.field.type], env);

                env->properties.push(module.types, entry);
                break;
            }
            case ast::Constraint::Function: {
                // `f: (a) -> b`. The signature is a real function type, so the conventions and the
                // `return` group it promises survive into the requirement instead of being dropped
                // at the boundary - see FunArg.
                FunctionConstraint entry;
                entry.name = constraint.fun.name;
                entry.source = constraint.source;
                entry.signature = resolveType(module, *module.parse[constraint.fun.type], env);

                env->functions.push(module.types, entry);
                break;
            }
        }
    }

    return pointer;
}

void resolveConstraintClasses(Module& module, GenEnv& env) {
    for(Size i = 0; i < env.classes.size(); i++) {
        auto constraint = env.classes.get(*module.types, i);
        if(constraint.typeClass || !constraint.name) continue;

        constraint.typeClass = findClass(module, constraint.name, constraint.source);
        if(!constraint.typeClass) {
            module.context.diagnostics.error("unknown class %@"_v, constraint.source,
                                             module.context.findName(constraint.name));
        } else {
            // The half prepareGenEnv could not do, for the two closed contexts whose constraints may
            // name a class declared after them - §10.4.
            resolveConstraintArgs(module, constraint, &env);
        }

        env.classes.set(*module.types, i, constraint);
    }
}

/*
 * Data, alias and class declarations.
 */

/*
 * Records the defaults written in a record's constructors.
 *
 * A pass of its own, after every record has been defined, because a default may name a nullary
 * constructor of a record declared further down and the layout that makes one a constant is
 * decided by defineRecord. Instantiations inherit their declaration's constructors whole, so a
 * generic record's defaults reach `Maybe(Int)` with nothing to do here.
 */
void declareRecordDefaults(Module& module, ast::Decl& decl) {
    auto global = *module.types;
    auto found = module.namedTypes.get(decl.data.type.name);
    if(!found || global[found.unwrap()]->kind != Type::Record) return;

    auto record = (RecordType*)global[found.unwrap()];
    Size at = 0;

    for(auto con: decl.data.cons.contents(module.parse)) {
        if(at >= record->constructors.size()) break;

        auto index = at++;
        auto constructor = record->constructors.get(global, index);
        auto content = constructor.content;
        if(!con.content) continue;

        // Only a tuple content has named fields to write a default on; a constructor wrapping one
        // unnamed type has nothing to leave out.
        auto& astContent = *module.parse[con.content];
        if(astContent.kind != ast::Type::Tup || !content || global[content]->kind != Type::Tup) continue;

        auto tuple = (TupType*)global[content];
        auto astFields = astContent.tup.fields;
        U16 field = 0;
        auto added = false;

        for(auto astField: astFields.contents(module.parse)) {
            if(astField.def && field < tuple->fields.size()) {
                auto type = tuple->fields.get(global, field).type;

                // A field whose own type did not resolve has had that reported already - as the
                // error type, which is what `resolveType` returns once it has said so. Evaluating a
                // default against it would report the recovery rather than the mistake, and against
                // *nothing* would silently answer with the literal's own default type.
                if(!type || global[type]->kind == Type::Error) {
                    field++;
                    continue;
                }

                auto constant = evaluateConstant(module, *module.parse[astField.def], type, "a field default"_v,
                                                 false);

                if(constant) {
                    constructor.defaults.push(module.types, FieldDefault { field, constant });
                    added = true;
                }
            }

            field++;
        }

        if(added) record->constructors.set(global, index, constructor);
    }
}

// The record a declaration introduces, with its own type variables but no constructors yet. Shared
// by `data` and by the newtype an `alias qualified` declares, which differ only in what they then
// put in it.
static RecordType* declareRecordType(Module& module, ast::SimpleType& type, ast::ConstraintList constraints,
                                     bool qualified, bool exported, LocationId source) {
    auto found = module.namedTypes.add(type.name);
    if(found.existed) {
        module.context.diagnostics.error("duplicate type %@"_v, source, module.context.findName(type.name));
        auto existing = *found.value;

        return existing && (*module.types)[existing]->kind == Type::Record
            ? (RecordType*)(*module.types)[existing]
            : nullptr;
    }

    auto record = new (module.types) RecordType(type.name);
    record->source = source;
    record->qualified = qualified;
    record->exported = exported;
    *found.value = (Type*)record - *module.types;

    auto variables = type.kind;
    if(variables.isNotEmpty() || constraints.isNotEmpty()) {
        record->gen = prepareGenEnv(module, GenEnv::Record, variables, constraints);
        record->generic = (*module.types)[record->gen]->types.isNotEmpty();
    } else {
        /*
         * A record with no type variables still needs somewhere to number its loan slots -
         * Analysis-Borrows.md §4.3, where a field names its own origin and the labels are collected
         * from the field types.
         *
         * An empty environment rather than a null one, so that `data Parser {input: src'Node}` has
         * the same place to introduce `src` into that a generic declaration does. `generic` stays
         * false, because a slot is not a type parameter: it has no runtime existence, takes no
         * descriptor and is not part of what an instantiation is made of - which is §8.2's "keep
         * group identity out of runtime layout and ordinary nominal dispatch", and is why one is
         * not written after the type's name.
         */
        record->gen = prepareGenEnv(module, GenEnv::Record, variables, constraints);
    }

    return record;
}

// Registers one constructor of `record`, and makes it reachable by its own name. A qualified
// record's constructors are addressed only as `Record.Constructor`, so they are not added to the
// module's flat constructor table.
static void declareConstructor(Module& module, RecordType& record, StringId name, U32 index,
                               bool qualified, LocationId source, I64 value = 0, bool pinned = false) {
    Constructor constructor { name, nullptr, index };
    constructor.source = source;
    constructor.value = value;
    constructor.pinnedValue = pinned;
    record.constructors.push(module.types, constructor);

    recordDefinition(module.context, constructorSymbol(module, ConstructorRef { &record - *module.types, U16(index) }));
    if(qualified) return;

    auto inserted = module.constructors.add(name);
    if(inserted.existed) {
        module.context.diagnostics.error("duplicate constructor %@"_v, source, module.context.findName(name));
    } else {
        *inserted.value = ConstructorRef { &record - *module.types, U16(index) };
    }
}

/*
 * `@value(n)` on a constructor - Analysis-Language.md §5.1.
 *
 * The number a constructor *is*. What it is for is the boundary an enum has to cross: a `Whence`
 * handed to `lseek` is 0, 1 or 2 whatever the declaration order happens to be, and until this
 * existed the only way to say so was a `match` producing the identity function one compare at a
 * time.
 *
 * A literal and only a literal, negative or not. It is a number the ABI on the other side of the
 * boundary already fixed, so there is nothing here for an expression to compute - and an attribute
 * argument is read before anything in the module can be evaluated, which is the same restriction a
 * field default lives under.
 */
static Maybe<I64> readValueAttribute(Module& module, ast::Con& con) {
    auto& context = module.context;
    auto value = context.addUnqualifiedName("value", 5);
    Maybe<I64> result;

    for(auto attribute: con.attributes.contents(module.parse)) {
        if(attribute.name != value) continue;

        if(attribute.args.size() != 1) {
            context.diagnostics.error("`@value` takes one number - `@value(2)`"_v, attribute.source);
            continue;
        }

        auto written = attribute.args.get(module.parse, 0).value;
        auto argument = &written;
        auto negative = false;

        // A negated literal arrives as a prefix applied to one rather than as a signed literal,
        // because what a written number *is* is a question about the type it lands in - the same
        // reason `ast::Pat` carries the sign beside the magnitude.
        if(argument->kind == ast::Expr::Prefix) {
            auto& prefix = *module.parse[argument->prefix];
            if(prefix.op.kind != ast::Expr::Var || prefix.op.var != Context::nameHash("-"_v)) {
                context.diagnostics.error("`@value` takes a whole number written as a literal"_v,
                                          argument->source);
                continue;
            }

            argument = &prefix.on;
            negative = true;
        }

        if(!ast::isLiteral(*argument) ||
           ast::Literal::Kind(argument->kind - ast::Expr::Lit) != ast::Literal::Int) {
            context.diagnostics.error("`@value` takes a whole number written as a literal"_v,
                                      argument->source);
            continue;
        }

        auto number = I64(argument->lit.i());
        result = Just(negative ? -number : number);
    }

    return result;
}

void declareRecord(Module& module, ast::Decl& decl) {
    auto record = declareRecordType(module, decl.data.type, decl.data.constraints, decl.qualified, decl.exported, decl.source);
    if(!record) return;

    recordDefinition(module.context, typeSymbol(module, (Type*)record - *module.types));

    U32 index = 0;

    // The number the next unpinned constructor takes. Declaration order is what this counts when
    // nothing pins anything, which is what makes an enum that writes no attribute the enum it was.
    I64 next = 0;

    for(auto con: decl.data.cons.contents(module.parse)) {
        auto written = readValueAttribute(module, con);
        auto value = written ? written.unwrap() : next;

        declareConstructor(module, *record, con.name, index++, decl.qualified, con.source,
                           value, written.isJust());

        next = value + 1;
    }

    /*
     * Distinct, which is the one rule pinning can break.
     *
     * Quadratic over the constructor list, and deliberately: what it compares is a declaration's own
     * constructors, so the list is the length somebody typed. Reported at the *later* of the two,
     * because that is the one whose number can be changed without renumbering everything after it.
     */
    auto constructors = record->constructors.contents(*module.types);
    for(Size i = 1; i < constructors.size(); i++) {
        for(Size j = 0; j < i; j++) {
            if(constructors[i].value != constructors[j].value) continue;

            module.context.diagnostics.error("%@ and %@ are both %@ - a constructor's value has to be its own"_v,
                                             constructors[i].source,
                                             module.context.findName(constructors[j].name),
                                             module.context.findName(constructors[i].name),
                                             constructors[i].value);
            break;
        }
    }
}

/*
 * `alias qualified Id = Int` - a newtype.
 *
 * A distinct type wrapping one other type, under a constructor of its own name. There is no way to
 * spell a different constructor name, and no ambiguity to resolve: what follows `=` in an `alias`
 * is a type, whereas under `data` the same words would be a constructor list.
 *
 * Everything past the declaration is an ordinary single-constructor record, so `Id(5)`, `Id(v)` in
 * a pattern, and every class instance over `Id` work with nothing further to teach them.
 */
void declareNewtype(Module& module, ast::Decl& decl) {
    auto record = declareRecordType(module, decl.alias.type, {}, false, decl.exported, decl.source);
    if(!record) return;

    declareConstructor(module, *record, decl.alias.type.name, 0, false, decl.source);
}

/*
 * `@layout(c)` and `@layout(js)` on a type declaration - Design.md's "the existing `@layout`
 * attribute pins it and opts a type out of Repr optimization", and Design-Memory §11's declared pin.
 *
 * `@layout(auto)` is the default and is accepted so that writing it means something; an argument that
 * is none of the three is reported rather than ignored, because a misspelled pin would silently
 * produce a layout an FFI declaration is relying on not to move.
 *
 * The two pins do not compose - a type has one representation, and `c` and `js` name different ones -
 * so writing both is an error rather than a precedence rule to remember.
 *
 * The pin is per declaration and does not reach inside one. A `@layout(c)` record containing a field
 * of an `@layout(auto)` record type gets a C struct's *offsets* for that field, but the nested record
 * is still laid out by its own rules - exactly as C lays out a nested struct by its own. A type whose
 * whole graph has to match C therefore has to say so at each level, which is the same thing C requires
 * of a header. `@layout(js)` is the same: a pinned record's field of an unpinned record type gets a
 * real property, but what sits in that property may well be a `number`.
 */
/*
 * `@inline` and `@noinline`, read off a declaration onto the function it declares.
 *
 * The two are deliberately not one attribute with a direction, because they are not the same kind of
 * statement. `@noinline` is a *directive* the compiler can always carry out - declining to inline is
 * always possible - so compiler/opt honours it with no exception and no diagnostic. `@inline` is a
 * *hint*: it raises the budget the callee is weighed against, and a callee compiler/opt declines
 * structurally is still declined. Design.md documents that asymmetry, and the reason it is written
 * down rather than left to be discovered is that the alternative reading - "@inline means it will be
 * inlined" - is one the pass cannot honour today for a callee with control flow in it.
 *
 * `@inline(always)` is the spelling that *would* be the directive, and it is reported rather than
 * accepted: the pass that could keep that promise is the one that grafts a callee's blocks into its
 * caller, and until it exists, accepting the word `always` would be the attribute lying. Reserving
 * it here is what keeps the bare form free to mean "hint" for good.
 *
 * Both on one declaration is a contradiction rather than a precedence question, so it is reported
 * and neither applies.
 */
void readInlineAttribute(Module& module, const ast::Decl& decl, Function& function) {
    auto attributes = decl.attributes;
    if(attributes.isEmpty()) return;

    auto& context = module.context;
    auto inlineName = context.addUnqualifiedName("inline", 6);
    auto noInlineName = context.addUnqualifiedName("noinline", 8);
    auto always = context.addUnqualifiedName("always", 6);

    for(auto attribute: attributes.contents(module.parse)) {
        if(attribute.name == noInlineName) {
            if(attribute.args.isNotEmpty()) {
                context.diagnostics.error("`@noinline` takes no arguments"_v, attribute.source);
                continue;
            }

            function.noInline = true;
            continue;
        }

        if(attribute.name != inlineName) continue;

        if(attribute.args.isEmpty()) {
            function.inlineHint = true;
            continue;
        }

        auto argument = attribute.args.get(module.parse, 0).value;

        if(attribute.args.size() == 1 && argument.kind == ast::Expr::Var && argument.var == always) {
            context.diagnostics.error("`@inline(always)` is not implemented yet - `@inline` on its own asks the optimizer to try harder, and is a hint rather than a guarantee"_v,
                                      attribute.source);
            continue;
        }

        context.diagnostics.error("`@inline` takes no arguments"_v, attribute.source);
    }

    if(function.inlineHint && function.noInline) {
        context.diagnostics.error("`@inline` and `@noinline` say opposite things about the same function"_v,
                                  decl.source);
        function.inlineHint = false;
        function.noInline = false;
    }
}

/*
 * `@convention(name)` - the calling convention a function is entered under.
 *
 * The name table is `lower/convention.h`'s, shared with the lower-IR parser and printer so that the
 * three spellings of a convention cannot drift apart - see the note there.
 *
 * **Not every convention in that table may be written**, and `conventionWritableInSource` is where
 * the set is argued. What a program may name is `clobber`, whose promise is semantic rather than an
 * implementation detail, and the two foreign ABIs a `foreign` declaration will need. `simple` and
 * `complex` are the backend's own choice between two internal shapes and are reported here, because
 * pinning one would write down as an interface something the backend is entitled to change.
 *
 * This does *not* imply `@noinline`, and the omission is deliberate. A convention describes a call
 * boundary, so a function that is inlined has none - but that is a fact about one call site, and an
 * attribute whose whole point is to be general has no business carrying an inlining decision for
 * every future user of it. A declaration that depends on the boundary surviving writes `@noinline`
 * beside this and says why; see `sha256CompressBlocks` in lib/Digest/Hardware.sha.yana.
 */
void readConventionAttribute(Module& module, const ast::Decl& decl, Function& function) {
    auto attributes = decl.attributes;
    if(attributes.isEmpty()) return;

    auto& context = module.context;
    auto conventionName = context.addUnqualifiedName("convention", 10);

    for(auto attribute: attributes.contents(module.parse)) {
        if(attribute.name != conventionName) continue;

        if(attribute.args.size() != 1) {
            context.diagnostics.error("`@convention` takes one argument - the name of a calling convention, as in `@convention(clobber)`"_v,
                                      attribute.source);
            continue;
        }

        auto argument = attribute.args.get(module.parse, 0).value;
        if(argument.kind != ast::Expr::Var) {
            context.diagnostics.error("`@convention` takes the name of a calling convention, written as an identifier"_v,
                                      attribute.source);
            continue;
        }

        auto found = callTypeForName(argument.var);
        if(!found) {
            context.diagnostics.error("unknown calling convention %@ - the ones a declaration may name are `clobber`, `sysv` and `win64`"_v,
                                      attribute.source, context.findName(argument.var));
            continue;
        }

        if(!conventionWritableInSource(found.unwrap())) {
            context.diagnostics.error("%@ is not a convention a declaration may name - `simple` and `complex` are the compiler's own choice between two internal shapes, and `system` describes what a syscall leaves alone rather than how anything is entered. Write `clobber`, `sysv` or `win64`"_v,
                                      attribute.source, context.findName(argument.var));
            continue;
        }

        if(function.convention) {
            context.diagnostics.error("this declaration already names a calling convention"_v, attribute.source);
            continue;
        }

        function.convention = found;
    }
}

/*
 * `@x86_legacy_sse` - see the note on `Function::legacySse`.
 *
 * No arguments, and deliberately no second form saying "and clear the vector state on entry". That
 * is a separate fact about a separate set of functions - the two entry points and not the twelve
 * helpers below them - and deriving which is which from a signature is what would silently stop
 * being right the first time an entry point gained a vector parameter. The state change is
 * `X86.vzeroupper()`, an ordinary call the author places. See Analysis-Warts.md §7.
 */
void readLegacySseAttribute(Module& module, const ast::Decl& decl, Function& function) {
    auto attributes = decl.attributes;
    if(attributes.isEmpty()) return;

    auto& context = module.context;
    auto legacyName = Context::nameHash(nameForLegacySse());

    for(auto attribute: attributes.contents(module.parse)) {
        if(attribute.name != legacyName) continue;

        if(attribute.args.isNotEmpty()) {
            context.diagnostics.error("`@x86_legacy_sse` takes no arguments - it says how this function's vector instructions are encoded, and nothing else. The vector state is cleared by calling `X86.vzeroupper()`"_v,
                                      attribute.source);
            continue;
        }

        function.legacySse = true;
    }
}

static TypeLayout readLayoutAttribute(Module& module, const ast::Decl& decl) {
    auto attributes = decl.attributes;
    if(attributes.isEmpty()) return TypeLayout::Auto;

    auto& context = module.context;
    auto layout = context.addUnqualifiedName("layout", 6);
    auto c = context.addUnqualifiedName("c", 1);
    auto js = context.addUnqualifiedName("js", 2);
    auto automatic = context.addUnqualifiedName("auto", 4);
    auto result = TypeLayout::Auto;
    auto pinned = false;

    for(auto attribute: attributes.contents(module.parse)) {
        if(attribute.name != layout) continue;

        if(attribute.args.size() != 1) {
            context.diagnostics.error("`@layout` takes one argument - `@layout(c)`, `@layout(js)` or `@layout(auto)`"_v,
                                      attribute.source);
            continue;
        }

        auto argument = attribute.args.get(module.parse, 0).value;
        if(argument.kind != ast::Expr::Var ||
           (argument.var != c && argument.var != js && argument.var != automatic)) {
            context.diagnostics.error("`@layout` takes `c`, `js` or `auto`"_v, argument.source);
            continue;
        }

        auto requested = argument.var == c ? TypeLayout::C
                       : argument.var == js ? TypeLayout::Js
                       : TypeLayout::Auto;

        if(pinned && requested != TypeLayout::Auto && requested != result) {
            context.diagnostics.error("`@layout(c)` and `@layout(js)` name different representations and cannot both apply"_v,
                                      argument.source);
            continue;
        }

        if(requested != TypeLayout::Auto) {
            result = requested;
            pinned = true;
        }
    }

    return result;
}

// Fills in a record's constructor contents, once every type name in the module is registered so
// that one may name a type declared further down.
static void defineRecordContent(Module& module, RecordType& record, LocationId source,
                                Buffer<const ast::Type*> contents, TypeLayout layout) {
    auto env = record.gen ? (*module.types)[record.gen] : nullptr;
    record.pinned = layout != TypeLayout::Auto;

    for(Size index = 0; index < contents.length && index < record.constructors.size(); index++) {
        auto stored = record.constructors.get(*module.types, index);
        stored.content = contents[index] ? resolveType(module, *contents[index], env) : module.scalar.unit;

        /*
         * The pin lives on the content tuple rather than only on the record, because content tuples
         * are interned structurally and the Repr cache is keyed on the type - so two records of
         * identical fields would otherwise share one layout and one of them would get the wrong
         * answer. Re-interning here is what makes the pinned one a distinct type. See TypeLayout.
         */
        if(stored.content && layout != TypeLayout::Auto &&
           (*module.types)[stored.content]->kind == Type::Tup) {
            auto tuple = (TupType*)(*module.types)[stored.content];
            Array<Field> fields;
            for(auto field: tuple->fields.contents(*module.types)) fields.push(field);

            stored.content = (Type*)resolveTupleType(module, toBuffer(fields), source, layout) - *module.types;
        }

        record.constructors.set(*module.types, index, stored);
    }

    computeRecordLayout(*module.types, record);

    /*
     * `@value` on a constructor that carries something - Analysis-Language.md §5.1.
     *
     * Reported here rather than where the attribute is read, because what makes it wrong is the
     * *content*, and a constructor's content is resolved a phase later than its name. What it would
     * have meant is nothing: a sum with a payload carries a discriminant beside that payload, and a
     * discriminant numbers the constructors rather than standing for them, so a pinned value would
     * have been written down and never read.
     */
    if(record.layout != RecordType::Enum) {
        for(auto constructor: record.constructors.contents(*module.types)) {
            if(!constructor.pinnedValue) continue;

            module.context.diagnostics.error("`@value` needs a constructor that carries nothing - %@ has a payload, so what the value would name is its tag rather than the value itself"_v,
                                             constructor.source, module.context.findName(constructor.name));
        }
    }

    record.definitionReady = true;
    (void)source;
}

// The record a data or qualified-alias declaration introduced, or null when the name turned out to
// be something else (a duplicate that lost, or a plain alias).
RecordType* declaredRecord(Module& module, StringId name) {
    auto found = module.namedTypes.get(name);
    if(!found || (*module.types)[found.unwrap()]->kind != Type::Record) return nullptr;

    return (RecordType*)(*module.types)[found.unwrap()];
}

void defineRecord(Module& module, ast::Decl& decl) {
    auto record = declaredRecord(module, decl.data.type.name);
    if(!record) return;

    /*
     * The record head's own constraints, which nothing resolved until now - Implementation-Const-
     * Generics.md §10.4.
     *
     * `declareRecord` runs in the pass that declares classes, so a head naming one written further
     * down the file cannot look it up there; and this was the only one of the four contexts with no
     * later pass to finish it in, so `data (NoSuchClass(a)) A(a)` was accepted in silence. Here is
     * the first point at which every class in the module exists.
     */
    if(record->gen) resolveConstraintClasses(module, *(*module.types)[record->gen]);

    // Inline: a record's constructors are one for a struct and a handful for a sum, and one of
    // these is built for every `data` declaration in every module the compile touches.
    SmallArray<const ast::Type*, 8> contents;
    for(auto con: decl.data.cons.contents(module.parse)) {
        contents.push(con.content ? module.parse[con.content] : nullptr);
    }

    defineRecordContent(module, *record, decl.source, toBuffer(contents), readLayoutAttribute(module, decl));
}

void defineNewtype(Module& module, ast::Decl& decl) {
    auto record = declaredRecord(module, decl.alias.type.name);
    if(!record) return;

    const ast::Type* content = &decl.alias.target;
    defineRecordContent(module, *record, decl.source, { &content, 1 }, readLayoutAttribute(module, decl));
}

void declareAlias(Module& module, ast::Decl& decl, ast::ParsePtr<ast::Decl> pointer) {
    auto found = module.aliases.add(decl.alias.type.name);
    if(found.existed || module.namedTypes.get(decl.alias.type.name)) {
        module.context.diagnostics.error("duplicate type %@"_v, decl.source,
                                         module.context.findName(decl.alias.type.name));
        return;
    }

    TypeAlias alias;
    alias.name = decl.alias.type.name;
    alias.module = &module;
    alias.ast = pointer;
    alias.source = decl.source;
    alias.exported = decl.exported;

    auto variables = decl.alias.type.kind;
    if(variables.isNotEmpty()) alias.gen = prepareGenEnv(module, GenEnv::Alias, variables, {});

    *found.value = alias;
    recordDefinition(module.context, aliasSymbol(module, alias));
}

/*
 * A statement at module level: a `let` that declares a global, or - in the root module - anything
 * else, which is a statement the program runs on the way in.
 *
 * The two halves of that sentence are Analysis-Initialization.md stage B. A library module still has
 * no program point at which its own code would run, so its `let` is a *constant* and nothing else is
 * admitted at all; the root module *is* the program, so its top level is an ordinary statement
 * sequence and its `let` may be initialized by one. Which of the two a `let` turns out to be is not
 * a syntactic distinction: a constant initializer keeps every property it had - it folds at each
 * read, occupies nothing and needs no code - and only an initializer that is not one gets storage
 * and a place in the entry sequence.
 *
 * `evaluateConstant` is asked rather than a shape being matched here, and the `notConstant` flag is
 * what makes that safe: `let &x = 300 :: U8` is the right form with the wrong contents, so it stays
 * the error it was instead of quietly becoming a runtime initializer that truncates.
 */
/*
 * A global whose type promises a teardown that will never happen.
 *
 * A global lives for the whole program and is never torn down - Analysis-Initialization.md §4.1's
 * ruling, and the reason it is one: drop points are last-use-based and a global has no last use.
 * Reclamation of its memory is the operating system's at exit and the host collector's on
 * JavaScript, which is the same observable behaviour on both targets; an authored `Drop` is the half
 * that was promised to *run*, so a global holding one silently skips an effect the program wrote.
 *
 * A warning rather than a rejection: the global is legal and its meaning is defined, and a
 * program-lifetime resource is a real thing to want. Authored rather than the whole member graph,
 * deliberately - `Type::Fun` is classified `Derived` because a closure's captures are not visible in
 * its type, so testing `drop != None` would warn about every global holding a function value and say
 * something untrue about most of them.
 *
 * Over every global at once, after the bodies are resolved, rather than beside each initializer.
 * That is where it used to be, and it therefore only ever saw the *dynamic* ones - so a constant
 * that carries a `Drop` said nothing at all, which is what `let &held = Handle {id: 4}` became the
 * day a construction stopped needing an initializer to run.
 */
void checkGlobalTeardown(Module& module) {
    auto& context = module.context;
    auto global = *module.types;
    auto local = *module.arena;

    for(auto pointer: module.globalOrder.contents(local)) {
        auto definition = local[pointer];

        // Every compiler-built table and blob, none of which has a source type to ask about.
        if(definition->anonymous || !definition->type) continue;
        if(global[definition->type]->kind == Type::Error) continue;

        if(ownershipOf(module, definition->type).drop != TeardownKind::Authored) continue;

        context.diagnostics.warning("%@ has type %@, whose `Drop` will not run - a global lives for the whole program and is never torn down. Hold the value in `main` instead if the teardown has to happen"_v,
                                    definition->source, context.findName(definition->name),
                                    describeType(context, global, definition->type));
    }
}

void declareGlobal(Module& module, ast::Decl& decl, ast::ParsePtr<ast::Decl> pointer) {
    auto parse = module.parse;
    auto& context = module.context;

    if(decl.stmt.kind != ast::Expr::Decl) {
        if(!module.root) {
            context.diagnostics.error("only a `let` declaration can appear at module level - a statement has to run at some point in a program's startup, and only the module being compiled has one"_v,
                                      decl.source);
            return;
        }

        module.topLevel.push(module.arena, TopLevelStmt { pointer });
        return;
    }

    TopLevelStmt statement { pointer };

    /*
     * One entry per written name, including the ones rejected below, because the entry sequence
     * walks this list beside the same declarations - see resolveEntryBody. A hole would silently
     * pair a name with the initializer of the one after it.
     */
    auto declared = [&](ModulePtr<Global> global_) { statement.globals.push(module.arena, global_); };

    for(auto declaration: decl.stmt.decl.contents(parse)) {
        if(declaration.pat.kind != ast::Pat::Var) {
            context.diagnostics.error("a global must be declared as a single name"_v, declaration.pat.source);
            declared(nullptr);
            continue;
        }

        if(declaration.bind == ast::BindType::Sink || declaration.sink) {
            context.diagnostics.error("a global is either plain or `&` mutable"_v, declaration.pat.source);
            declared(nullptr);
            continue;
        }

        if(!declaration.content) {
            context.diagnostics.error("a global requires an initializer"_v, declaration.pat.source);
            declared(nullptr);
            continue;
        }

        // `let x = e in body` names something for the length of `body`, and a module level has no
        // body: what follows the declaration is the rest of the file, which is what the global's own
        // scope already is. Reported rather than ignored, since ignoring it drops written code.
        if(declaration.in) {
            context.diagnostics.error("a module-level `let` has no `in` - the name it declares is in scope for the whole module"_v,
                                      decl.source);
            declared(nullptr);
            continue;
        }

        /*
         * The value, and with it the type.
         *
         * A global has no other way to say what it is - a `let` pattern carries no type annotation -
         * so the `:: T` of `let &heapNext = 0 :: %U8` is read by the evaluator as the position's
         * type rather than checked afterwards, and a global written without one takes the literal's
         * own default. Everything else about what may be written here is const.cpp's.
         *
         * A dynamic global has no type yet: what it holds is whatever its initializer produces, and
         * that is resolved by the entry sequence. Nothing may read one before then, which is why the
         * entry body is the first body resolved - see resolveProgram.
         */
        auto notConstant = false;
        auto constant = evaluateConstant(module, *parse[declaration.content], nullptr,
                                         "a global's initializer"_v, true,
                                         module.root ? &notConstant : nullptr);

        if(!constant && !notConstant) {
            declared(nullptr);
            continue;
        }

        /*
         * A global's constant becomes storage, and one shape has no static form - see
         * `constantHasStaticForm`. It is the *right* form with contents this position cannot place,
         * so it is a report rather than a fall-back where there is nowhere to fall back to, and an
         * ordinary startup initializer in the root module, where there is: `Node(Leaf)` is a value
         * the entry sequence can build perfectly well.
         */
        if(constant && !constantHasStaticForm(*module.types, *module.arena, constant)) {
            if(!module.root) {
                context.diagnostics.error("a global's initializer holds a value reached through an indirection - a recursive type keeps its payload behind an owning pointer, and static storage has nothing for one to point at"_v,
                                          declaration.content ? parse[declaration.content]->source
                                                              : declaration.pat.source);
                declared(nullptr);
                continue;
            }

            constant = nullptr;
            notConstant = true;
        }

        auto global_ = module.addGlobal(declaration.pat.var, declaration.pat.source);
        recordDefinition(module.context, globalSymbol(module, global_ - *module.arena));
        global_->type = constantType(*module.arena, constant);
        global_->initial = constant;
        global_->mut = declaration.bind == ast::BindType::Ref;
        global_->dynamic = notConstant;
        global_->exported = decl.exported;
        declared(global_ - *module.arena);
    }

    // By value, not by move: what the list holds is a region pointer and two counts, so the copy is
    // the same eight bytes either way and the elements it names do not move.
    if(module.root) module.topLevel.push(module.arena, statement);
}
