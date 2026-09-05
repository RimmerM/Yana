#include "resolve_internal.h"
#include "name.h"
#include "print.h"

namespace mir {

/*
 * `deriving` on a qualified alias - Analysis-Derive.md §3's `newtype` shape.
 *
 * The smallest slice of that document's mechanism, and deliberately the one that needs none of it. A
 * template exists to *iterate* - fields, constructors - and a newtype has one content and nothing to
 * iterate, so what a forwarding derivation needs is a rewrite rather than a repetition form: read
 * the content out of every argument the class typed as its own variable, call the class function on
 * what comes out, and put the name back on the result where the class variable is what comes out.
 *
 * **The expansion is source**, which is the property to keep: what is produced here is an
 * `ast::Decl` that goes through `declareInstance` and `resolveInstanceBody` unchanged, so a derived
 * instance is selected, specialized, inlined, printed and reported against by the passes that
 * already do all of that for a hand-written one. There is no second, IR-level path to keep honest
 * against the source one. `core.cpp`'s principle in the shipping stage says it best: everything the
 * compiler derives is something a programmer could have written, and what it derives *is* that
 * program.
 *
 * The whole of the difference from the shipping stage's `module_derive.cpp` is the unwrap. That one
 * matches - `match lhs: N(lhs$) -> ...` - and pays for a binding name per argument and a rule about
 * what a pattern may hand out; this one writes `lhs.0`, which is what
 * `doc/spec/types.md#newtypes` says the content is reached by. A projection is a place, so a `&`
 * position forwards as one and needs no exception.
 */

namespace {

/*
 * The AST this file writes, in the region every declaration of the program is addressed through -
 * `Context::parseRegion`. A derived instance is an AST written here rather than in a file.
 *
 * One `source` for all of it, and it is the *class name in the clause*: every diagnostic a
 * derivation produces is about one of the classes in the list, so a list of four reports at four
 * carets rather than four times at the same one.
 */
struct Expansion {
    Region<ast::ParseRegion>& arena;
    LocationId source;

    template<class T>
    ast::ParsePtr<T> heap(const T& value) { return new (arena) T(value) - *arena; }

    ast::Expr var(StringId name) {
        return ast::Expr { .var = name, .source = source, .kind = ast::Expr::Var };
    }

    ast::Type conType(StringId name) {
        return ast::Type { .name = name, .attributes = nullptr, .source = source,
                           .kind = ast::Type::Con };
    }

    // `f(a, b)` where `f` is an ordinary name - which every class function is, operators included:
    // `==` is a name the overload set holds like any other, so nothing here has to know that the
    // call it is building will be written infix by whoever reads it back.
    ast::Expr call(StringId name, Buffer<ast::Expr> args) {
        ast::ParseList<ast::TupArg> list;
        for(auto& argument: args) list.push(arena, ast::TupArg { StringId(), argument });

        return ast::Expr { .app = heap(ast::AppExpr { var(name), list }), .source = source,
                           .kind = ast::Expr::App };
    }

    // `x.0` - the content, as the anonymous field it is. The index is a literal because a field
    // position is one, which is `positionalField` in resolve_expr.cpp.
    ast::Expr content(StringId name) {
        ast::Literal zero { .s = StringId() };
        zero.i(0);

        auto index = ast::Expr {
            .lit = zero,
            .source = source,
            .kind = ast::Expr::Kind(ast::Expr::Lit + ast::Literal::Int),
        };

        return ast::Expr { .field = heap(ast::FieldExpr { var(name), index }), .source = source,
                           .kind = ast::Expr::Field };
    }

    // `N(e)` - the newtype's constructor applied to one thing, which is the only way in.
    ast::Expr construct(StringId name, ast::Expr inner) {
        ast::ParseList<ast::TupArg> list;
        list.push(arena, ast::TupArg { StringId(), inner });

        return ast::Expr {
            .con = heap(ast::ConExpr { .type = conType(name), .args = list }),
            .source = source,
            .kind = ast::Expr::Con,
        };
    }
};

} // namespace

/*
 * Whether the class variable is the *whole* of this type, appears inside it, or not at all.
 *
 * The condition rule 1 is well defined under, and Analysis-Derive.md §2's sixth point is why it is
 * checked and named rather than left implicit: it is Haskell's `GeneralizedNewtypeDeriving`
 * restriction, and lifting it needs a representational-coercion concept - roles - that this language
 * does not have and should not acquire by accident. `fromValue(n: I64) -> Maybe(a)` fails it, and
 * the reason is not a missing case here: there is no cast from `Maybe(OpenFlags)` to `Maybe(I64)`
 * that this file could emit, because nothing says the two have one representation.
 */
Resolver::Forwarding Resolver::forwardingOf(DeclPtr<Type> type, DeclPtr<Type> variable) {
    if(type == variable) return Forwarding::Bare;
    if(!type || !variable) return Forwarding::Absent;

    return mentionsVariable(decl(), type, variable) ? Forwarding::Nested : Forwarding::Absent;
}

/*
 * One class of a `deriving (...)` clause, as the instance it stands for - or null after reporting
 * why it does not stand for one.
 *
 * Every refusal is checked over the whole class before a single member is produced, because a class
 * that cannot be derived usually cannot be derived for more than one reason, and reporting them one
 * build at a time is the shape of diagnostic that makes a feature feel broken.
 *
 * What is deliberately *not* here is a pre-check that the content answers each call. The body is
 * ordinary source and reports where the clause is written, so the failure is legible without this
 * file duplicating overload resolution to say it one message sooner.
 */
ast::Decl* Resolver::deriveInstance(ast::Decl& declaration, const ast::Derive& derive) {
    auto& alias = declaration.alias;
    auto klass = lookupClass(derive.name, derive.source);
    if(!klass) return nullptr;

    auto& declared = *decl()[klass];

    /*
     * A **generic** newtype - `alias qualified Boxed(a) = Maybe(a) deriving (Eq)`.
     *
     * The head this wants is `instance (Eq(Maybe(a))) Eq(Boxed(a))`, whose context is *inferred*
     * from the content. Analysis-Extensibility.md takes that inference deliberately and notes that
     * it cuts against Design.md's rule that a public declaration writes its constraints explicitly;
     * taking it is a decision worth making on its own rather than acquiring here. Refused with the
     * head it would have needed, so what is missing is legible rather than a silent nothing.
     */
    if(alias.type.kind.size()) {
        context.diagnostics.error("%@ takes type parameters, and deriving for it would need the instance's context worked out from its content - which is a decision this stage has not taken"_v,
                                  derive.source, context.findName(alias.type.name));
        return nullptr;
    }

    /*
     * A class the compiler answers from the shape of the type. Refused for the reason
     * `declareInstance` refuses a written one: whether a value may be duplicated or relocated
     * bitwise is computed from its members, and a newtype's one member has already decided it. A
     * derivation would be a claim the compiler has contradicted.
     */
    if(isStructuralClass(declared.role)) {
        context.diagnostics.error("%@ is decided structurally and cannot be derived - a newtype is already whatever its content is"_v,
                                  derive.source, context.findName(declared.name));
        return nullptr;
    }

    /*
     * One type variable. Analysis-Extensibility.md restricts deriving to single-variable classes and
     * the restriction is not arbitrary: `Widen(a, b)` has no single "the content" to forward to, so
     * there is nothing for the rewrite to be about.
     */
    auto arity = declared.arity(decl());
    if(arity != 1) {
        context.diagnostics.error("%@ takes %@ type variables, and only a class with one can be derived - there is no single content for a newtype to forward to"_v,
                                  derive.source, context.findName(declared.name), U64(arity));
        return nullptr;
    }

    auto variable = decl()[declared.gen]->variables.get(decl(), 0);

    Expansion expansion { context.parseRegion, derive.source };
    ast::DeclList members;
    auto derivable = true;

    for(auto pointer: declared.members.contents(decl())) {
        auto& member = *decl()[pointer];

        // Only the obligations. A default is already written, in terms of the members below it, and
        // producing one here would replace a body the class author wrote with a forwarding call to
        // that same body one level down - which is what keeps a derived `Eq` to `==` alone.
        if(member.summary.bodyKnown) continue;

        auto name = context.findName(member.name);

        // A `lens fn` or an `iter fn`. Its result is a hand-over to a continuation rather than a
        // value, so "wrap the result" has nothing to name.
        if(member.lens || member.iterator) {
            context.diagnostics.error("%@ cannot be derived - its member %@ hands over to a continuation rather than answering a value, so there is nothing for the newtype to be put back on"_v,
                                      derive.source, context.findName(declared.name), name);
            derivable = false;
            continue;
        }

        for(auto argument: member.args.contents(decl())) {
            auto& position = *decl()[argument];
            auto forwarding = forwardingOf(position.occurrence.type, variable);

            if(forwarding == Forwarding::Nested) {
                context.diagnostics.error("%@ cannot be derived - its member %@ takes %@ as %@, where the content would have to be substituted inside another type; only a bare occurrence can be unwrapped"_v,
                                          derive.source, context.findName(declared.name), name,
                                          context.findName(position.name),
                                          typeText(context, decl(), position.occurrence.type));
                derivable = false;
                continue;
            }

            // A `@lazy` argument of the class variable's own type. What arrives is a thunk over the
            // caller's frame, so there is no value here to read the content out of - and forcing it
            // would evaluate an argument the class promised not to.
            if(forwarding == Forwarding::Bare && position.lazy) {
                context.diagnostics.error("%@ cannot be derived - its member %@ takes %@ `@lazy`, so what arrives is a thunk rather than a value to read the content out of"_v,
                                          derive.source, context.findName(declared.name), name,
                                          context.findName(position.name));
                derivable = false;
            }
        }

        auto result = forwardingOf(member.result.type, variable);

        if(result == Forwarding::Nested) {
            context.diagnostics.error("%@ cannot be derived - its member %@ answers %@, where the newtype would have to be substituted inside another type; only a bare occurrence can be wrapped"_v,
                                      derive.source, context.findName(declared.name), name,
                                      typeText(context, decl(), member.result.type));
            derivable = false;
            continue;
        }

        /*
         * A result that **borrows**. Putting the name back on one would hand back a borrow of the
         * content under a name for the whole, and the extent it was rooted in is about the argument
         * the caller passed rather than about the value this body read out of it.
         */
        if(result == Forwarding::Bare && member.result.contract.access() != Access::Owned) {
            context.diagnostics.error("%@ cannot be derived - its member %@ answers a borrow, and what a forwarding body would put the newtype's name on is storage the caller already owns"_v,
                                      derive.source, context.findName(declared.name), name);
            derivable = false;
            continue;
        }

        /*
         * The member itself, and the argument types are left **unwritten**: an instance member with
         * no type on a position takes the class's, substituted at this head (`declareArgs`), so
         * writing them here would repeat what the class already fixed and would be a second place
         * for the two to disagree. Only `@lazy` is written, because it is part of the position and
         * an implementation may not add or drop one.
         */
        ast::ParseList<ast::Arg> args;
        Array<ast::Expr> forwarded;

        for(auto argument: member.args.contents(decl())) {
            auto& position = *decl()[argument];

            args.push(expansion.arena, ast::Arg {
                .source = derive.source,
                .name = position.name,
                .type = nullptr,
                .def = nullptr,
                .bind = ast::BindType::Borrow,
                .returnRoot = false,
                .lazy = position.lazy,
            });

            // A position the class variable does not reach is handed on exactly as it arrived -
            // `Show.show`'s `&to: String` is the same buffer at both levels and not a copy of one.
            forwarded.push(forwardingOf(position.occurrence.type, variable) == Forwarding::Bare
                           ? expansion.content(position.name) : expansion.var(position.name));
        }

        auto body = expansion.call(member.name, toBuffer(forwarded));

        // The name is put back only where the class variable is what comes out. `Truth.truthy`
        // answers a `Bool` and forwards it untouched; `Bitwise.and` answers the class variable, and
        // what its content answered has to be given the name back.
        if(result == Forwarding::Bare) body = expansion.construct(alias.type.name, body);

        members.push(expansion.arena, ast::Decl {
            .fun = {
                .name = member.name,
                .constraints = {},
                .args = args,
                .ret = nullptr,
                .retBind = ast::BindType::Borrow,
                .protocol = ast::noProtocol(),
                .body = expansion.heap(body),
                .implicitReturn = true,
                .kind = ast::FunKind::Plain,
            },
            .attributes = {},
            .source = derive.source,
            .kind = ast::Decl::Fun,
            .exported = false,
        });
    }

    if(!derivable) return nullptr;

    ast::ParseList<ast::Type> headArgs;
    headArgs.push(expansion.arena, expansion.conType(alias.type.name));

    auto head = ast::Type {
        .app = expansion.heap(ast::AppType { expansion.conType(derive.name), headArgs }),
        .attributes = nullptr,
        .source = derive.source,
        .kind = ast::Type::App,
    };

    return new (context.parseRegion) ast::Decl {
        .instance = { head, {}, members },
        .attributes = {},
        .source = derive.source,
        .kind = ast::Decl::Instance,
        .exported = declaration.exported,
    };
}

} // namespace mir
