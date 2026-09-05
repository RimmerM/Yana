#include "link.h"
#include "../../intrinsic/primitive.h"
#include "../../parse/ast.h"
#include "../select.h"

namespace mir {

using namespace intrinsic;

/*
 * The claim table.
 *
 * Three arrays rather than one, because the three enums are three independent numberings and the
 * one thing a reader ever does with a role is subscript the table its kind names. `RoleRef` is what
 * spans them, and it exists for exactly one caller - the attribute reader, which has a name and not
 * yet a kind.
 *
 * Total, and it has to be: every `RoleKind` is a kind a declaration can claim, which is what makes
 * `RoleKind` those three and not the five directions. A `provided.def` row is keyed off the `Known`
 * role naming its class rather than claimed in its own right, so there is no fourth arm to write.
 */
RoleClaim& Roles::claim(RoleRef role) {
    switch(role.kind) {
        case RoleKind::Op:       return op[role.index];
        case RoleKind::Supplied: return supplied[role.index];
        case RoleKind::Known:    break;
    }

    return known[role.index];
}

/*
 * The typed readings of a claim.
 *
 * Each asserts the row's entity before casting, which is what makes storing one offset safe: the
 * table says what kind of declaration may claim the role and `bindRole` refused anything else, so a
 * caller asking for the wrong reading is a compiler bug rather than a program's.
 */
DeclPtr<Class> Roles::klass(Known role) const {
    assertTrue(knownDef(role).entity == Entity::Class);
    return DeclPtr<Class>(known[Size(role)].declaration);
}

DeclPtr<Function> Roles::function(Known role) const {
    assertTrue(knownDef(role).entity == Entity::Function);
    return DeclPtr<Function>(known[Size(role)].declaration);
}

DeclPtr<RecordType> Roles::record(Known role) const {
    assertTrue(knownDef(role).entity == Entity::Record);
    return DeclPtr<RecordType>(known[Size(role)].declaration);
}

DeclPtr<Global> Roles::global(Known role) const {
    assertTrue(knownDef(role).entity == Entity::Global);
    return DeclPtr<Global>(known[Size(role)].declaration);
}

DeclPtr<Function> Roles::operation(Op role) const {
    return DeclPtr<Function>(op[Size(role)].declaration);
}

DeclPtr<Global> Roles::suppliedGlobal(Builtin role) const {
    return DeclPtr<Global>(supplied[Size(role)].declaration);
}

// What a role wants to be written on. `Op` and `Supplied` have no entity column because there is
// only one answer for each: an operation is a signature and a supplied role is storage.
static Entity entityFor(RoleRef role) {
    switch(role.kind) {
        case RoleKind::Op:       return Entity::Function;
        case RoleKind::Supplied: return Entity::Global;
        case RoleKind::Known:    break;
    }

    return knownDef(Known(role.index)).entity;
}

Maybe<RoleRef> bindRole(Context& context, Program& program, ast::ParseBase parse,
                        ast::Decl& declaration, Maybe<Entity> entity, U32 declaration_) {
    if(declaration.attributes.isEmpty()) return Nothing();

    auto attributeName = context.addUnqualifiedName(kBuiltinAttribute);
    Maybe<RoleRef> bound = Nothing();

    for(auto attribute: declaration.attributes.contents(parse)) {
        if(attribute.name != attributeName) continue;

        if(attribute.args.size() != 1) {
            context.diagnostics.error(
                "`@builtin` takes one role - `@builtin(drop)`. This compiler knows: %@"_v,
                attribute.source, roleList());
            continue;
        }

        auto argument = attribute.args.get(parse, 0).value;
        if(argument.kind != ast::Expr::Var) {
            context.diagnostics.error("a `@builtin` role is a bare name. This compiler knows: %@"_v,
                                      argument.source, roleList());
            continue;
        }

        auto written = context.findName(argument.var);
        auto found = findRole(StringView { written.text(), written.size() });

        if(!found) {
            context.diagnostics.error("%@ is not a role this compiler has - it knows: %@"_v,
                                      argument.source, written, roleList());
            continue;
        }

        auto role = found.unwrap();

        /*
         * A declaration that can never claim one.
         *
         * Every declaration form calls this, including the forms with no role to fill, which is what
         * makes a misplaced attribute a diagnostic at all: an `instance` carrying `@builtin(drop)`
         * would otherwise bind nothing and say nothing, and the author would be looking for a role
         * they believe they filled.
         */
        if(!entity) {
            context.diagnostics.error("`@builtin` has no meaning here - %@ is %@"_v,
                                      attribute.source, written, entityName(entityFor(role)));
            continue;
        }

        if(entity.unwrap() != entityFor(role)) {
            context.diagnostics.error("%@ is %@, and this is %@"_v, attribute.source, written,
                                      entityName(entityFor(role)), entityName(entity.unwrap()));
            continue;
        }

        // One role per declaration. A second is refused rather than taken, because there is one
        // field to record it in and taking the last would make which one applied depend on the
        // order the attributes were written.
        if(bound) {
            context.diagnostics.error("a declaration fills one role - %@ is a second"_v,
                                      attribute.source, written);
            continue;
        }

        auto& slot = program.roles.claim(role);

        if(slot.filled()) {
            context.diagnostics.error(
                "the role %@ is already filled - one declaration fills a role, or the compiler would reach whichever was read last"_v,
                argument.source, written);
            context.diagnostics.error("this is the declaration that filled it"_v, slot.source);
            continue;
        }

        slot.declaration = declaration_;
        slot.source = attribute.source;
        bound = Just(role);
    }

    return bound;
}

/*
 * Whether a type is what a shape term asks for.
 *
 * `positions` is the whole signature - the result at index zero is deliberately *not* in it, because
 * every relative term names a **parameter**: a result that referred to itself would have no fixed
 * point, and a parameter that referred to the result would be a signature read backwards.
 */
static bool termMatches(Program& program, const PositionShape& shape, DeclPtr<Type> type,
                        bool deferred, const DeclPtr<Type>* parameters, Size count) {
    auto base = *program.decl;

    if(!type) return false;

    auto relative = [&]() -> DeclPtr<Type> {
        return shape.of < count ? parameters[shape.of] : nullptr;
    };

    switch(shape.term) {
        case ShapeTerm::Any: return true;
        case ShapeTerm::Var: return base[type]->kind == Type::Variable;

        case ShapeTerm::Pointer: return base[type]->kind == Type::Pointer;

        case ShapeTerm::PointeeOf: {
            auto other = relative();
            if(!other || base[other]->kind != Type::Pointer) return false;

            return ((PointerType*)base[other])->to == type;
        }

        case ShapeTerm::PointerTo: {
            auto other = relative();
            if(!other || base[type]->kind != Type::Pointer) return false;

            return ((PointerType*)base[type])->to == other;
        }

        case ShapeTerm::SameAs: return relative() == type;

        case ShapeTerm::Integer: return base[type]->kind == Type::Int;
        case ShapeTerm::Bool:    return type == booleanType(program);
        case ShapeTerm::Unit:    return base[type]->kind == Type::Unit;
        case ShapeTerm::Function: return base[type]->kind == Type::Fun;

        /*
         * The string whose constant the emitter reads - and this stage cannot name that type yet.
         *
         * `String` in the ownership stage is one of the things the real `Core` is still blocked on,
         * so a structural test here would be a term no reader could answer, which is worse than no
         * term - the same ruling the vector positions in `op.def` are under. The day `known.def`
         * names the string, this is the row that says so.
         *
         * What the term is actually *for* is checked either way, and not here: whether an argument
         * is a literal is a question about a call, and `host.cpp`'s `nameAt` is what reports one
         * that is not. A shape says what a declaration owes its emitter, and "this position is the
         * text type" is all of that a declaration can carry.
         */
        case ShapeTerm::Text: return true;

        // The declaration's own marker and not its type: `declareDeferred` has already rewritten the
        // position to `() -> T`, and a signature that wrote that type by hand is a strict parameter
        // holding a function, which is exactly what the expansion must not be handed.
        case ShapeTerm::Deferred: return deferred;
    }

    return false;
}

/*
 * Whether a declared position is passed the way the emitter depends on it being passed.
 *
 * Two questions and not one, because they are two axes of the declaration: `Any` on either is the
 * statement that the emitter does not care, which leaves the library free to write whichever
 * spelling reads best. `borrow` and `borrowMut` differ in the first and agree in the second, which
 * is what made a single enum of the combinations the wrong shape for this.
 */
static bool accessMatches(ShapeAccess wanted, Contract declared) {
    switch(wanted) {
        case ShapeAccess::Any:    return true;
        case ShapeAccess::Shared: return declared.access() == Access::Shared;
        case ShapeAccess::Owned:  return declared.access() == Access::Owned;
        case ShapeAccess::Unique: return declared.access() == Access::Unique;
    }

    return false;
}

static bool originMatches(ShapeOrigin wanted, Contract declared) {
    switch(wanted) {
        case ShapeOrigin::Any:         return true;
        case ShapeOrigin::CallBounded: return declared.origin() == Origin::CallBounded;
        case ShapeOrigin::Published:   return declared.origin() == Origin::Slot;
    }

    return false;
}

/*
 * One intrinsic's declaration against the shape its emitter depends on.
 *
 * Reported at the *declaration*, naming the position and what was wanted there, because that is
 * where the mistake is: a library that changed `store`'s second parameter from `->value` to `value`
 * has written something the compiler will not honour, and the call sites are all innocent.
 */
static bool checkOpShape(Context& context, Program& program, Op role, Function& function,
                         LocationId source) {
    auto base = *program.decl;
    auto& shape = opDef(role);
    auto ok = true;

    auto arity = function.writtenArity();
    if(arity != shape.count) {
        context.diagnostics.error("the role %@ takes %@ %@, and this declaration takes %@"_v, source,
                                  shape.role, U64(shape.count),
                                  shape.count == 1 ? "argument"_v : "arguments"_v, U64(arity));
        return false;
    }

    // The parameter types, gathered first: a relative term names a parameter by index, and the
    // result's term may name one that comes after the position being checked.
    SmallArray<DeclPtr<Type>, 8> parameters;
    for(auto argument: function.args.contents(base)) {
        parameters.push(base[argument]->occurrence.type);
    }

    Size index = 0;
    for(auto argument: function.args.contents(base)) {
        if(index >= shape.count) break;

        auto& declared = *base[argument];
        auto& wanted = shape.params[index];

        if(!termMatches(program, wanted, declared.occurrence.type, declared.lazy,
                        parameters.pointer(), parameters.size())) {
            context.diagnostics.error("for the role %@, %@ must be %@"_v, declared.source,
                                      shape.role, context.findName(declared.name),
                                      shapeTermName(wanted.term));
            ok = false;
        }

        if(!accessMatches(wanted.access, declared.occurrence.contract)) {
            context.diagnostics.error("for the role %@, %@ must be %@"_v, declared.source,
                                      shape.role, context.findName(declared.name),
                                      shapeAccessName(wanted.access));
            ok = false;
        }

        if(!originMatches(wanted.origin, declared.occurrence.contract)) {
            context.diagnostics.error("for the role %@, %@ must be %@"_v, declared.source,
                                      shape.role, context.findName(declared.name),
                                      shapeOriginName(wanted.origin));
            ok = false;
        }

        index++;
    }

    if(!termMatches(program, shape.result, function.result.type, false, parameters.pointer(),
                    parameters.size())) {
        context.diagnostics.error("for the role %@, the result must be %@"_v, source, shape.role,
                                  shapeTermName(shape.result.term));
        ok = false;
    }

    if(!accessMatches(shape.result.access, function.result.contract)) {
        context.diagnostics.error("for the role %@, the result must be %@"_v, source, shape.role,
                                  shapeAccessName(shape.result.access));
        ok = false;
    }

    if(!originMatches(shape.result.origin, function.result.contract)) {
        context.diagnostics.error("for the role %@, the result must be %@"_v, source, shape.role,
                                  shapeOriginName(shape.result.origin));
        ok = false;
    }

    return ok;
}

/*
 * A supplied global's declaration, and the value the target writes into it.
 *
 * The five refusals are the ones `Analysis-Builtins.md` argues for, restated over this stage's
 * declarations: written storage must be `&` and supplied storage must not be, the type has to be
 * what a backend is about to store a register into, a supplied initializer has to be the zero of
 * its type, and a target with no answer for a supplied role has to say so rather than leave a zero.
 */
static bool checkSuppliedGlobal(Context& context, Program& program, Builtin role, Global& global,
                                LocationId source) {
    auto base = *program.decl;
    auto& definition = builtinDef(role);

    if(definition.kind == BuiltinKind::Written && !global.writable) {
        context.diagnostics.error(
            "%@ is storage the compiler writes before the program starts - declare it `&`"_v,
            source, definition.role);
        return false;
    }

    if(definition.kind == BuiltinKind::Supplied && global.writable) {
        context.diagnostics.error(
            "%@ is a constant the compiler supplies, so nothing may write it - drop the `&`"_v,
            source, definition.role);
        return false;
    }

    auto type = global.occurrence.type;
    auto shaped = false;

    switch(definition.shape) {
        case BuiltinShape::Word:
            /*
             * The target's *word* and not any integer: what a backend stores here is a whole
             * register, and a `U8` would be one byte of storage taking eight bytes of it.
             *
             * Asked as `TargetWidth::Word` rather than by comparing a declared width against the
             * machine's, and that is the layering rather than a preference: this stage does not
             * know which machine it is resolving for. A rule written as a number reads `I64` as a
             * word here and would go on reading it as one where a word is thirty-two bits - while
             * refusing the `Size` that is the word there - so the only spelling that stays true is
             * the name that defers.
             */
            shaped = type && base[type]->kind == Type::Int &&
                     ((IntType*)base[type])->target == intrinsic::TargetWidth::Word;
            break;

        case BuiltinShape::Address:
            shaped = type && base[type]->kind == Type::Pointer;
            break;

        /*
         * An enumeration of the library's - a record whose constructors all carry nothing, which is
         * what a number can be stored into.
         *
         * Which number each constructor is is `@value`'s to say and is not checked here: a rule
         * about that would be the library's declaration restated in the compiler, which is what
         * this whole directory exists to remove.
         */
        case BuiltinShape::Enum: {
            if(type && base[type]->kind == Type::Record) {
                auto& record = *(RecordType*)base[type];
                shaped = true;

                // `cons` is never empty, so what is asked is only that nothing is carried - a
                // constructor with fields is one a stored number could not stand for.
                for(auto constructor: record.cons.contents(base)) {
                    if(constructor.count) shaped = false;
                }
            }

            break;
        }
    }

    if(!shaped) {
        context.diagnostics.error("%@ has to be %@"_v, source, definition.role,
                                  definition.shape == BuiltinShape::Word
                                      ? "the target's word - `Size` or `USize`"_v
                                  : definition.shape == BuiltinShape::Address
                                      ? "a pointer"_v
                                      : "an enumeration - constructors that carry nothing"_v);
        return false;
    }

    if(definition.kind != BuiltinKind::Supplied) return true;

    /*
     * The initializer has to be the zero of its type.
     *
     * A written number the compiler then overwrites is a line of source that lies to whoever reads
     * it - and the reader has no way to find out, because the number the program actually runs with
     * is a target's answer that appears nowhere in the file.
     */
    auto enumerated = definition.shape == BuiltinShape::Enum;

    auto zeroed = [&] {
        if(!enumerated) return global.initial.kind == Constant::Int && global.initial.asInt() == 0;
        if(global.initial.kind != Constant::Enum) return false;

        auto& record = *(RecordType*)base[type];
        return record.cons.get(base, global.initial.asConstructor()).value == 0;
    };

    if(!zeroed()) {
        context.diagnostics.error(
            "%@ is supplied by the compiler, so its initializer says nothing - write %@"_v, source,
            definition.role, enumerated ? "the constructor `@value(0)` names"_v : "`0`"_v);
        return false;
    }

    auto value = builtinValue(context.settings, role);
    if(!value) {
        context.diagnostics.error("this target has no answer for %@"_v, source, definition.role);
        return false;
    }

    if(!enumerated) {
        global.initial = intConstant(type, value.unwrap());
        return true;
    }

    /*
     * The constructor whose `@value` is the number, and not the number itself.
     *
     * `builtinValue` answers what is *stored*, which is the enumeration's own numbering - so what
     * this looks up is the declaration's answer to that number rather than assuming a constructor's
     * position is its value. A library that renumbered its constructors and left one without the
     * value this role means is what the refusal is for, and it says which number went missing.
     */
    auto& record = *(RecordType*)base[type];

    for(auto constructor: record.cons.contents(base)) {
        if(U64(constructor.value) != value.unwrap()) continue;

        global.initial = Constant { type, Constant::Enum, constructor.index };
        return true;
    }

    context.diagnostics.error("this target answers %@ with %@, and %@ has no constructor of that value"_v,
                              source, definition.role, value.unwrap(),
                              typeText(context, base, type));
    return false;
}

/*
 * What a **view record** has to be - Analysis-Manifestations.md Part 8's three conditions, checked
 * rather than assumed.
 *
 * A view is an ordinary `data` that the view table pairs with an owner, and what makes it a
 * representation rather than a type is that nothing can reach it as one: it is `@local`, so the
 * name cannot be written outside the module that declares the owner and `privateTypeIn` refuses it
 * in any `pub` signature; it has one constructor, so a projection through it never needs a match;
 * and it owns nothing, so no field of it owes a teardown - a window describes storage somebody else
 * releases. Nothing here is about the field list, deliberately: the compiler reads no field of a
 * view, and `known.def`'s row says so.
 *
 * An instance on a view is the fourth condition and is refused where instances are declared - a head
 * naming a `@local` type from another module is already the orphan rule's.
 */
static bool checkViewRecord(Context& context, DeclBase base, Known role, RecordType& record,
                            LocationId source) {
    // Which roles are views is the view table's column and not a list here - `viewRowForView`, so
    // that a row added to `type.cpp` is checked by this without an edit.
    if(!viewRowForView(role)) return true;

    auto& definition = knownDef(role);

    if(!record.local) {
        context.diagnostics.error(
            "the role %@ describes a representation, not a type - write `@local` on it so that nothing outside this module can name it"_v,
            source, definition.role);
        return false;
    }

    if(record.cons.size() != 1) {
        context.diagnostics.error(
            "the role %@ describes a representation, which has one constructor - this has %@"_v,
            source, definition.role, U64(record.cons.size()));
        return false;
    }

    for(auto field: record.fields.contents(base)) {
        if(!hasTeardown(base, field.occurrence.type)) continue;

        context.diagnostics.error(
            "the role %@ describes a view of storage somebody else owns, so no field of it may own anything - %@ does"_v,
            source, definition.role, context.findName(field.name));
        return false;
    }

    return true;
}

bool checkDeclaredRoles(Context& context, Program& program) {
    auto base = *program.decl;
    auto ok = true;

    for(Size i = 0; i < Size(Known::Count); i++) {
        auto role = Known(i);
        auto& definition = knownDef(role);
        auto& claim = program.roles.known[i];

        // A function's claim is filed when its signature is declared, which is a later pass than
        // this check runs in - `checkKnownFunctions` is where those rows are judged.
        if(definition.entity == Entity::Function) continue;

        if(!claim.filled()) {
            /*
             * A `Required` role nothing filled, which is a broken library rather than a broken
             * program - so it names the role and not a source position, because there is none.
             *
             * `Optional` is silent: it means the compiler copes, which is the ordinary state of a
             * program that does not import the module the role lives in.
             */
            if(definition.presence == Presence::Required) {
                context.diagnostics.error(
                    "the standard library does not declare anything with the role %@, which this compiler needs"_v,
                    kNullLocation, definition.role);
                ok = false;
            }

            continue;
        }

        /*
         * A record whose constructors the compiler *numbers* - `known.def`'s `members` read at the
         * other entity it applies to.
         *
         * Three things checked together because they are one dependency: the compiler holds an
         * index into `cons` and turns it into a stored tag, so the count has to be what it expects,
         * each constructor has to carry the payload the row states, and each has to be numbered by
         * its position. A `@value(7)` on the true constructor would make every comparison this
         * compiler emits answer a value the language does not have.
         */
        if(definition.entity == Entity::Record) {
            auto& record = *base[DeclPtr<RecordType>(claim.declaration)];

            if(definition.arity && record.gen && base[record.gen]->variables.size() != definition.arity) {
                context.diagnostics.error("the role %@ is a type of %@ parameters, and this has %@"_v,
                                          claim.source, definition.role, U64(definition.arity),
                                          U64(base[record.gen]->variables.size()));
                ok = false;
                continue;
            }

            if(!definition.members) continue;

            auto constructors = record.cons.contents(base);

            if(constructors.size() != definition.members) {
                context.diagnostics.error(
                    "the role %@ is a type of %@ constructors, and this has %@"_v, claim.source,
                    definition.role, U64(definition.members), U64(constructors.size()));
                ok = false;
                continue;
            }

            for(auto constructor: constructors) {
                if(constructor.count == definition.payload && constructor.value == constructor.index) {
                    continue;
                }

                context.diagnostics.error(
                    "the role %@ numbers its constructors from zero and gives each %@, and %@ is not one of those"_v,
                    claim.source, definition.role,
                    definition.payload == 0 ? "no fields"_v
                                            : definition.payload == 1 ? "one field"_v : "several fields"_v,
                    context.findName(constructor.name));
                ok = false;
            }

            if(!checkViewRecord(context, base, role, record, claim.source)) ok = false;

            continue;
        }

        if(definition.entity != Entity::Class) continue;

        auto& klass = *base[DeclPtr<Class>(claim.declaration)];

        // The head's arity, which is the shape every reader indexes it at: a three-parameter
        // `Drop(a, b, c)` claiming the role would have the ownership walk reading a head at an arity
        // nobody wrote.
        if(definition.arity && klass.arity(base) != definition.arity) {
            context.diagnostics.error("the role %@ is a class of %@ parameters, and this has %@"_v,
                                      claim.source, definition.role, U64(definition.arity),
                                      U64(klass.arity(base)));
            ok = false;
        }

        // And its member count, which is the other half of the same dependency: the compiler reaches
        // a member by position, so a class with a different number of them is one it cannot index.
        if(definition.members && klass.members.size() != definition.members) {
            context.diagnostics.error("the role %@ is a class with %@ members, and this has %@"_v,
                                      claim.source, definition.role, U64(definition.members),
                                      U64(klass.members.size()));
            ok = false;
        }

        /*
         * A **structural** class declares no members, and that is what licenses selection answering
         * one with no instance behind it: the answer is a proof and there is nothing to dispatch
         * through. A member on one would be a call with no body anywhere, found at every type the
         * shape admits.
         *
         * Checked here rather than by a `members` of zero in the table, because zero there means
         * "the library's business" - the column states a count the compiler indexes by, and this
         * states that there is nothing to index.
         */
        if(isStructuralClass(role) && klass.members.size()) {
            context.diagnostics.error("the role %@ is a classification the compiler makes from the type, so it declares no members and this has %@"_v,
                                      claim.source, definition.role, U64(klass.members.size()));
            ok = false;
        }
    }

    return ok;
}

/*
 * The functions the compiler inserts calls to - `stringLiteral`, `slice` - judged once every
 * signature is declared, which is later than `checkDeclaredRoles` runs.
 *
 * The arity is the whole of the dependency: the call is built with that many arguments, and the
 * types are checked at the call exactly as a written one's are. A required one nothing declared is
 * reported the way a missing class is - against the role, since there is no position.
 */
static bool checkKnownFunctions(Context& context, Program& program) {
    auto base = *program.decl;
    auto ok = true;

    for(Size i = 0; i < Size(Known::Count); i++) {
        auto role = Known(i);
        auto& definition = knownDef(role);
        if(definition.entity != Entity::Function) continue;

        auto& claim = program.roles.known[i];
        if(!claim.filled()) {
            if(definition.presence == Presence::Required) {
                context.diagnostics.error(
                    "the standard library does not declare anything with the role %@, which this compiler needs"_v,
                    kNullLocation, definition.role);
                ok = false;
            }

            continue;
        }

        auto& function = *base[DeclPtr<Function>(claim.declaration)];

        if(definition.arity && function.writtenArity() != definition.arity) {
            context.diagnostics.error("the role %@ is a function of %@ arguments, and this takes %@"_v,
                                      claim.source, definition.role, U64(definition.arity),
                                      U64(function.writtenArity()));
            ok = false;
        }
    }

    return ok;
}

bool checkRoles(Context& context, Program& program) {
    auto base = *program.decl;
    auto ok = checkKnownFunctions(context, program);

    for(Size i = 0; i < Size(Op::Count); i++) {
        auto& claim = program.roles.op[i];
        if(!claim.filled()) continue;

        auto function = DeclPtr<Function>(claim.declaration);
        if(!checkOpShape(context, program, Op(i), *base[function], claim.source)) ok = false;
    }

    for(Size i = 0; i < kBuiltinCount; i++) {
        auto& claim = program.roles.supplied[i];
        if(!claim.filled()) continue;

        auto global = DeclPtr<Global>(claim.declaration);
        if(!checkSuppliedGlobal(context, program, Builtin(i), *base[global], claim.source)) {
            ok = false;
        }
    }

    return ok;
}

} // namespace mir
