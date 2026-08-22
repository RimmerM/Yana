#include "module_internal.h"
#include "name.h"

/*
 * `deriving` on a qualified alias - Analysis-Derive.md §3's `newtype` shape.
 *
 * The smallest slice of that document's mechanism, and deliberately the one that needs none of it.
 * A template exists to *iterate* - fields, constructors - and a newtype has one content and nothing
 * to iterate, so what a forwarding derivation needs is a rewrite rather than a repetition form:
 * unwrap every argument whose type is the class variable, call the class function on the content,
 * and wrap the result back up where the class variable is what comes out.
 *
 * Everything below is therefore the *front* of that design rather than a rule beside it. Section
 * 11's first recommendation is the shape this follows exactly: expansion happens at the declaration,
 * it produces an `ast::Decl`, and that declaration goes through `resolveInstance` unchanged. There
 * is no second, IR-level path that has to be kept honest against the source one - which is the
 * property Analysis-Extensibility.md calls load-bearing about rule 1, and the reason `core.cpp`'s
 * principle survives here: everything the compiler derives is something a programmer could have
 * written, and what it derives *is* that program.
 *
 * What that buys, concretely, is that the expansion lowers, prints, breakpoints, specializes and
 * inlines like any other instance, and that every diagnostic about it comes out of the passes that
 * already check hand-written instances. The whole of this file is the rewrite and the conditions
 * under which it is well-defined; nothing here knows what any class means.
 */

namespace {

/*
 * The name an unwrapped binding gets.
 *
 * `$` is not in the identifier grammar, so it can never collide with an argument name the class
 * wrote - which matters because the two are in one scope: `fn and(lhs, rhs)` binds `lhs$` beside
 * `lhs` and the forwarding body reads both.
 *
 * Through `builtName` and not `addUnqualifiedName`: the latter interns the pointer it is handed
 * rather than the bytes, so it is for static text only and a builder's buffer handed to it is a
 * name that reads as whatever the stack holds afterwards.
 */
StringId contentName(Context& context, StringId argument) {
    StringBuilder text;
    text << context.findName(argument) << '$';
    return builtName(context, text);
}

/*
 * Whether the class variable occurs *bare* in a written type - as the whole of it, and not inside
 * anything.
 *
 * This is the condition rule 1 is well-defined under, and Analysis-Derive.md §2's sixth point is why
 * it is checked and named rather than left implicit: it is Haskell's `GeneralizedNewtypeDeriving`
 * restriction, and lifting it needs a representational-coercion concept (roles) that this language
 * does not have and should not acquire by accident. `fromValue(n: I64) -> Maybe(a)` fails it, and
 * the reason it fails is not a missing case here - there is no cast from `Maybe(OpenFlags)` to
 * `Maybe(I64)` that this file could emit, because nothing says the two have one representation.
 */
enum class Occurrence {
    Absent,  // The class variable does not appear; the position is passed through untouched.
    Bare,    // The position *is* the class variable; it is unwrapped, or wrapped.
    Nested,  // It appears inside something else, which is what makes the derivation ill-defined.
};

Occurrence occurrenceOf(GlobalBase global, TypePtr type, U16 index) {
    if(!type) return Occurrence::Absent;

    if(global[type]->kind == Type::Gen) {
        return ((GenType*)global[type])->index == index ? Occurrence::Bare : Occurrence::Absent;
    }

    return mentionsVariable(global, type, index) ? Occurrence::Nested : Occurrence::Absent;
}

/*
 * The AST a forwarding implementation is.
 *
 * Built into the parse region the module was read from rather than into a region of its own, because
 * a `ParsePtr` is an offset from one base and a module has exactly one - `Function::ast` points into
 * it, and the body pass reads through `module.parse`. The region is a bump allocator over a reserved
 * range, so appending to it after parsing moves nothing that is already there.
 *
 * Every node carries the location of the class name in the `deriving` clause. That is the honest
 * answer to "where was this written": a diagnostic from inside an expansion is about the derivation
 * having been asked for, and the clause entry is the only text there is to point at.
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
        return ast::Type { .name = name, .attributes = nullptr, .source = source, .kind = ast::Type::Con };
    }

    // `f(a, b)` where `f` is an ordinary name - which every class function is, operators included:
    // `==` is a name the overload set holds like any other, so nothing here has to know that the
    // call it is building will be written infix by whoever reads it back.
    ast::Expr call(StringId name, Buffer<ast::Expr> args) {
        ast::ParseList<ast::TupArg> list;
        for(auto& arg: args) list.push(arena, ast::TupArg { StringId(), arg });

        return ast::Expr {
            .app = heap(ast::AppExpr { var(name), list }),
            .source = source,
            .kind = ast::Expr::App,
        };
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

    /*
     * `match value: N(bound) -> body` - the only way *out*.
     *
     * One arm, which is exhaustive because a newtype has exactly one constructor, and a match rather
     * than a field access because a newtype's content is not a named field: positional content is
     * reachable only by pattern matching, which is Analysis-Derive.md §1's fourth probe and is still
     * true. This is `openBits` in `Native/Linux.yana`, written once per argument instead of once per
     * newtype.
     *
     * `bind` is the convention the class declared for the argument being unwrapped, carried onto the
     * pattern so that a `->` parameter's content is taken out rather than borrowed out of storage
     * the frame no longer owns.
     */
    ast::Expr unwrap(StringId typeName, StringId value, StringId bound, ast::BindType bind, ast::Expr body) {
        auto inner = ast::Pat { .var = bound, .asVar = StringId(), .bind = bind, .source = source, .kind = ast::Pat::Var };

        ast::Pat pattern {};
        pattern.con = { typeName, heap(inner) };
        pattern.asVar = StringId();
        pattern.source = source;
        pattern.kind = ast::Pat::Con;

        ast::ParseList<ast::Alt> alts;
        alts.push(arena, ast::Alt { pattern, body });

        return ast::Expr {
            .match = heap(ast::MatchExpr { var(value), alts }),
            .source = source,
            .kind = ast::Expr::Match,
        };
    }
};

}

/*
 * One class of one `deriving` clause.
 *
 * Reports and returns on anything it cannot derive, and never half-produces: an instance that
 * implements some of a class's obligations is worse than none, because the missing ones are then
 * reported at whichever call site reaches them rather than here.
 */
static void deriveNewtypeInstance(Module& module, ast::Module& ast, ast::Decl& decl,
                                  RecordType& record, ast::Derive derive) {
    auto& context = module.context;
    auto global = *module.types;
    auto local = *module.arena;
    auto typeName = decl.alias.type.name;

    auto classPointer = findClass(module, derive.name, derive.source);
    if(!classPointer) {
        context.diagnostics.error("unknown class %@"_v, derive.source, context.findName(derive.name));
        return;
    }

    // The two implicit classes, refused here for the reason resolveInstance refuses them: whether a
    // type may be duplicated or relocated bitwise is computed from its members, and a newtype's one
    // member has already decided it. A derivation would be a claim the compiler has contradicted.
    if(classPointer == module.coreClasses.trivialCopy || classPointer == module.coreClasses.trivialSink) {
        context.diagnostics.error("%@ is decided structurally and cannot be derived - a newtype is already whatever its content is"_v,
                                  derive.source, context.findName(derive.name));
        return;
    }

    auto typeClass = global[classPointer];
    resolveClassSignatures(*typeClass->module, *typeClass);

    // Analysis-Extensibility.md restricts deriving to single-variable classes, and the restriction is
    // not arbitrary: `Widen(a, b)` has no single "the content" to forward to, so there is nothing for
    // the rewrite below to be about.
    auto variables = global[typeClass->gen]->types.size();
    if(variables != 1) {
        context.diagnostics.error("%@ takes %@ type variables and only a class with one can be derived - there is no single content for a newtype to forward to"_v,
                                  derive.source, context.findName(derive.name), U32(variables));
        return;
    }

    auto functions = typeClass->functions;

    /*
     * The conditions, checked over the whole class before a single line is produced.
     *
     * All of them at once rather than at the first failure, because a class that cannot be derived
     * usually cannot be derived for more than one reason, and reporting them one build at a time is
     * the shape of diagnostic that makes a feature feel broken.
     */
    auto derivable = true;

    for(Size i = 0; i < functions.size(); i++) {
        auto entry = functions.get(global, i);

        // Only the obligations. A default is already written, in terms of the ones below it, and
        // producing one here would replace a body the class author wrote with a forwarding call to
        // the same body one level down - Analysis-Derive.md §4's note that a derived `Eq` writes only
        // `==` because `!=` is a default, which is what keeps every derivation this short.
        if(entry.defaultFun) continue;

        auto signature = local[entry.fun];
        if(!signature) { derivable = false; continue; }

        auto name = context.findName(entry.name);

        // A `lens fn` or `iter fn` member. Its result is a step signal over a synthesized
        // continuation rather than the written type, so "wrap the result" has nothing to name.
        if(signature->funKind != ast::FunKind::Plain) {
            context.diagnostics.error("%@ cannot be derived - its member %@ is a %@, whose result is a continuation rather than a value to wrap"_v,
                                      derive.source, context.findName(derive.name), name,
                                      signature->funKind == ast::FunKind::Iter ? "`iter fn`"_v : "`lens fn`"_v);
            derivable = false;
            continue;
        }

        // A borrow in the result rooted in an argument. Wrapping such a result would hand back a
        // borrow of the content under a name for the whole, and the extent the root established is
        // about the argument the caller passed rather than the temporary this body unwrapped.
        if(signature->returnRoots) {
            context.diagnostics.error("%@ cannot be derived - its member %@ returns a borrow rooted in an argument, and a forwarding body would root it in the unwrapped content instead"_v,
                                      derive.source, context.findName(derive.name), name);
            derivable = false;
            continue;
        }

        for(Size argIndex = 0; argIndex < signature->args.size(); argIndex++) {
            auto arg = local[signature->args.get(local, argIndex)];
            auto occurrence = occurrenceOf(global, arg->declaredType(), 0);

            if(occurrence == Occurrence::Nested) {
                context.diagnostics.error("%@ cannot be derived - its member %@ takes %@ as %@, where a newtype's content would have to be substituted inside another type; only a bare occurrence can be unwrapped"_v,
                                          derive.source, context.findName(derive.name), name,
                                          context.findName(arg->name),
                                          describeType(context, global, arg->declaredType()));
                derivable = false;
                continue;
            }

            if(occurrence == Occurrence::Absent) continue;

            // A `@lazy` argument of the class variable's own type. What arrives is a thunk over the
            // caller's frame, so there is no value here to unwrap - forcing it to unwrap it would
            // evaluate an argument the class promised not to.
            if(arg->isLazy()) {
                context.diagnostics.error("%@ cannot be derived - its member %@ takes %@ `@lazy`, so what arrives is a thunk rather than a value to unwrap"_v,
                                          derive.source, context.findName(derive.name), name,
                                          context.findName(arg->name));
                derivable = false;
                continue;
            }

            // A mutable borrow of the whole. Matching does not establish exclusive access, so a
            // pattern cannot produce a writable binding to the content of storage somebody else
            // owns - which is exactly what `&` on this position would need.
            if(arg->convention == ast::BindType::Ref) {
                context.diagnostics.error("%@ cannot be derived - its member %@ takes %@ as a mutable borrow, and matching cannot hand out writable access to the content of a borrowed value"_v,
                                          derive.source, context.findName(derive.name), name,
                                          context.findName(arg->name));
                derivable = false;
                continue;
            }
        }

        if(occurrenceOf(global, signature->returnType, 0) == Occurrence::Nested) {
            context.diagnostics.error("%@ cannot be derived - its member %@ returns %@, where the newtype would have to be substituted inside another type; only a bare occurrence can be wrapped"_v,
                                      derive.source, context.findName(derive.name), name,
                                      describeType(context, global, signature->returnType));
            derivable = false;
        }
    }

    if(!derivable) return;

    /*
     * The header constraint - Analysis-Derive.md §3's first mitigation for the one honest cost of
     * expansion, which is that a mistake surfaces from inside generated code.
     *
     * Asked **per obligation and not per class**, and the difference between those two is the case
     * the feature was built for. Analysis-Extensibility.md states rule 1 as "call `Class`'s instance
     * for the content"; that is not what the derived body does, and it is not what the hand-written
     * instance this replaces did either. `Logic(OpenFlags)` forwards `and` to an `I64`, and `I64` is
     * not in `Logic` at all - `and`, `or`, `xor` and `not` are `Integral`'s there, and `Logic` is
     * instanced for `Bool` alone. The body resolved that call the way any body resolves a call, over
     * every class the name belongs to, and found `Integral(I64).and`. A class-level check would
     * state something narrower than the expansion does and would reject the motivating derivation.
     *
     * So the question asked here is the one the expansion will ask: is there a class function of
     * this name whose class the content is in. That accepts the `Integral` route, rejects `and` at a
     * `String` with one diagnostic instead of the four-plus-cascade the expansion produces, and names
     * the member and the content rather than the class alone.
     *
     * It is a *pre*-check and not a proof: arity and the non-class argument positions are still the
     * expansion's to settle. What it buys is that the common failure - the content cannot do this at
     * all - never reports from inside a body nobody wrote.
     */
    auto content = record.constructors.get(global, 0).content;

    for(Size i = 0; i < functions.size(); i++) {
        auto entry = functions.get(global, i);
        if(entry.defaultFun) continue;

        ClassFunList candidates;
        findClassFunctions(module, entry.name, derive.source, candidates);

        auto answered = false;

        for(auto& candidate: candidates) {
            // Single-variable classes only. A multi-variable one may well answer the call - `Widen`
            // does - but which of its positions the content would bind is a question this cannot ask
            // without doing overload resolution, and guessing wrong here would reject a derivation
            // the expansion would have accepted. Left to the expansion, which is the safe direction.
            auto other = global[candidate.typeClass];
            if(global[other->gen]->types.size() != 1) { answered = true; break; }

            if(matchInstance(module, candidate.typeClass, { &content, 1 })) { answered = true; break; }
        }

        // And a *plain* function of the name, which Design.md's R1 admits one of beside any number
        // of class functions. A class whose obligation is answered at the content by a plain function
        // rather than by another class is a shape nothing in this tree has, and rejecting it here
        // would be this check being narrower than the expansion again - the mistake the comment above
        // is about. `kNullLocation` for the occurrence so the lookup records nothing in the index:
        // there is no name in any source file at this point.
        if(!answered && findFunction(module, entry.name, derive.source, kNullLocation)) answered = true;

        if(answered) continue;

        context.diagnostics.error("%@(%@) cannot be derived - its member %@ would forward to %@ on %@, and nothing in scope answers that"_v,
                                  derive.source, context.findName(derive.name), context.findName(typeName),
                                  context.findName(entry.name), context.findName(entry.name),
                                  describeType(context, global, content));
        return;
    }

    /*
     * The expansion.
     *
     * `instance C(N)` with one function per obligation, each of them the same three-step rewrite.
     * The argument types and the result type are left unwritten: `resolveInstance` substitutes the
     * instance's types into the class signature and takes the conventions off it, so writing them
     * here would be repeating what the class already fixed - and would be a second place for the two
     * to disagree.
     */
    Expansion expansion { ast.region, derive.source };
    ast::DeclList members;

    for(Size i = 0; i < functions.size(); i++) {
        auto entry = functions.get(global, i);
        if(entry.defaultFun) continue;

        auto signature = local[entry.fun];
        ast::ParseList<ast::Arg> args;
        Array<ast::Expr> forwarded;

        struct Unwrapped { StringId value; StringId bound; ast::BindType bind; };
        Array<Unwrapped> unwrapped;

        for(Size argIndex = 0; argIndex < signature->args.size(); argIndex++) {
            auto arg = local[signature->args.get(local, argIndex)];
            auto argName = arg->name;

            args.push(expansion.arena, ast::Arg {
                .source = derive.source,
                .name = argName,
                .type = nullptr,
                .def = nullptr,
                .bind = arg->convention,
                .returnRoot = false,
                .lazy = arg->isLazy(),
            });

            if(occurrenceOf(global, arg->declaredType(), 0) != Occurrence::Bare) {
                // A position the class variable does not reach is handed on exactly as it arrived -
                // `Show.show`'s `&to: String` is the same buffer at both levels, not a copy of one.
                forwarded.push(expansion.var(argName));
                continue;
            }

            auto bound = contentName(context, argName);
            unwrapped.push(Unwrapped { argName, bound, arg->convention });
            forwarded.push(expansion.var(bound));
        }

        auto body = expansion.call(entry.name, toBuffer(forwarded));

        // The result is wrapped only where the class variable is what comes out. `Truth.truthy`
        // answers a `Bool` and forwards it untouched; `Logic.and` answers the class variable and its
        // content has to be given the name back.
        if(occurrenceOf(global, signature->returnType, 0) == Occurrence::Bare) {
            body = expansion.construct(typeName, body);
        }

        // Innermost last: argument 0's match is the outer one, so the bindings are introduced in the
        // order the arguments were written and a reader of the expansion sees them that way.
        for(Size u = unwrapped.size(); u > 0; u--) {
            auto& entryU = unwrapped[u - 1];
            body = expansion.unwrap(typeName, entryU.value, entryU.bound, entryU.bind, body);
        }

        members.push(expansion.arena, ast::Decl {
            .fun = {
                .name = entry.name,
                .constraints = {},
                .args = args,
                .ret = nullptr,
                .retBind = ast::BindType::Borrow,
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

    ast::ParseList<ast::Type> headArgs;
    headArgs.push(expansion.arena, expansion.conType(typeName));

    auto head = ast::Type {
        .app = expansion.heap(ast::AppType { expansion.conType(derive.name), headArgs }),
        .attributes = nullptr,
        .source = derive.source,
        .kind = ast::Type::App,
    };

    auto instance = new (ast.region) ast::Decl {
        .instance = { head, {}, members },
        .attributes = {},
        .source = derive.source,
        .kind = ast::Decl::Instance,
        .exported = decl.exported,
    };

    resolveInstance(module, *instance);
}

void deriveNewtypeInstances(Module& module, ast::Module& ast, ast::Decl& decl) {
    auto derives = decl.alias.derives;
    if(derives.isEmpty()) return;

    auto record = declaredRecord(module, decl.alias.type.name);

    // The declaration did not produce a type, which has already been reported. Deriving from it
    // would put a second diagnostic on one mistake.
    if(!record) return;

    /*
     * A generic newtype - `alias qualified Boxed(a) = %a deriving (Eq)`.
     *
     * The head this wants is `instance (Eq(%a)) Eq(Boxed(a))`, whose context is *inferred* from the
     * content. Analysis-Extensibility.md takes that inference deliberately and notes it cuts against
     * Design.md's rule that a public declaration writes its constraints explicitly; taking it is a
     * decision worth making on its own rather than acquiring here, since nothing in this tree has a
     * generic newtype to derive for. Refused with the head it would have needed, so what is missing
     * is legible rather than a silent nothing.
     */
    if(record->generic) {
        module.context.diagnostics.error("a generic newtype cannot derive yet - deriving for %@ would need the context worked out from its content, and that inference is not implemented"_v,
                                         decl.source, module.context.findName(decl.alias.type.name));
        return;
    }

    for(auto derive: derives.contents(module.parse)) {
        deriveNewtypeInstance(module, ast, decl, *record, derive);
    }
}
