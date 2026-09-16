#include "resolve_internal.h"
#include "print.h"

namespace mir {

/*
 * What a value does when the position it fills is a different type - the one place a conversion is
 * decided, and the whole of what `::` and an implicit position have in common.
 *
 * `doc/spec/overloading.md`: `::` is one operator over three classes, and which class relates a pair
 * of types is the whole of the rule for what it does. `Widen` is lossless and is therefore the one
 * that also happens *without* being written; `Narrow` is lossy and has to be written as
 * `truncate(x)`; `Bitcast` is neither, and is written as `bitcast(x)`. So there is exactly one
 * question here - is there a `Widen` instance - and everything else this file does is explain why
 * the answer was no.
 *
 * ## Why one function and not two
 *
 * An ascription and an argument position used to be different code in the shipping stage, and the
 * difference was a bug: `::` passed `implicit == false` and took the `Narrow` instance, which made
 * an ascription the one construct in the language that could silently remove data -
 * `length(xs) :: Int` truncating a sixty-four-bit length to thirty-two on one target and being free
 * on the other. They are one rule with one difference, and the difference is what the *message*
 * says: an implicit position tells the author what type it wanted, and an ascription tells them that
 * `::` does not narrow.
 *
 * ## What is not here
 *
 * The **window adaptation** - a `[T *n]` or a container reaching a `[T]` position - is not a
 * conversion and is not decided here. It changes the logical type without changing who owns the
 * storage, which is `Builder::addWindow`'s reshaping of a borrow, and `adaptToWindow` runs before
 * this at the one position that has one. A conversion builds a **new value**, which is exactly the
 * distinction: an adapted window is the caller's bytes described differently, and a widened `I32` is
 * eight fresh bytes.
 *
 * The **ladder** is not here either. Which pairs of primitives `Widen` relates is
 * `intrinsic/convert.h`, generated into instances by `provided.def`, and what this does is select
 * one - so a user type that declares `instance Widen(Money, Cents)` reaches every position below
 * with nothing added.
 */

/*
 * Whether a position that asked for this access can be filled by a converted value.
 *
 * A conversion produces a value the frame has just made, and the two questions that turns on are
 * "may the callee read it" - always yes - and "may the callee write through it back to the caller",
 * which is the whole of `Access::Unique`. Widening an `I32` local into an `&I64` parameter would
 * hand the callee eight fresh bytes to write, and the local it was converted from would never see
 * them: a silent miscompile, and the one this predicate exists to refuse.
 *
 * An **owned** position is fine - a converted value is a fresh owner, which is what `->` asks for -
 * and a **shared** one is fine because a borrow of a temporary is an ordinary thing this stage
 * already builds a frame slot for.
 *
 * `establishes` is the fourth answer and the one the exclusive case turns on: there is a `&` that
 * has no caller behind it. `let &total = n :: I64` asks for exclusive access to storage **it is
 * declaring**, which `Request::establishes` is the bit for, so the widened `I64` is that storage and
 * nothing is written back anywhere. Refusing it reported "what the callee wrote would be thrown
 * away" of a statement with no callee in it, and since `::` is the only way to name a type in a
 * `let`, that refused every converting `let &` in the language.
 */
static bool conversionFillsPosition(Access access, bool establishes) {
    return access != Access::Unique || establishes;
}

/*
 * Why a conversion that looks free was refused, where one side's width is the *target's*.
 *
 * `let n: Size = someU32` is the case, and the refusal is correct and surprising in the same breath:
 * on a sixty-four-bit machine a `Size` is a signed sixty-four-bit integer and a `U32` fits it twice
 * over. What decides the ladder is not that machine but the *bound* - `Size` is whatever word the
 * target picked, so the language guarantees it thirty-two bits, and a signed thirty-two holds
 * thirty-one positive ones. A `U32` needs thirty-two. The refusal is what stops the same source
 * being correct here and lossy on a host whose word is narrower, which is the one thing a
 * target-answered width exists to prevent.
 *
 * So the ordinary message is true and answers the wrong question, and this answers the one that was
 * asked. It names `USize` first because that is nearly always the right destination: it is the same
 * target word read unsigned, so every `U32` fits it on every target and no conversion is written at
 * all. `truncate` stays the answer where a *signed index* is genuinely what was wanted.
 *
 * Only where a target-answered width is involved. Between two stated widths the ordinary message
 * already says everything - a `U64` does not fit an `I32` on any machine, and nobody is surprised.
 */
void Resolver::explainAbstractWidth(DeclPtr<Type> from, DeclPtr<Type> to, LocationId source) {
    if(!from || !to) return;
    if(decl()[from]->kind != Type::Int || decl()[to]->kind != Type::Int) return;

    auto& supplied = *(IntType*)decl()[from];
    auto& wanted = *(IntType*)decl()[to];
    if(!wanted.isTargetWidth() || supplied.isTargetWidth()) return;

    /*
     * The pair this is about: the value fits the *widest* the destination may be and not the
     * narrowest, which is exactly the gap between "works on my machine" and "works". A source that
     * fits neither is an ordinary narrowing and needs no explaining.
     */
    auto widest = intrinsic::integerOf(wanted.maxBits(), wanted.isSigned);
    if(!intrinsic::integerRangeFits(supplied.numeric(), widest)) return;

    auto positive = U32(wanted.minBits()) - (wanted.isSigned ? 1u : 0u);

    context.diagnostics.message(Diagnostics::MessageLevel,
        "%@ is the width the *target* picks, so what the language guarantees is %@ bits%@ - %@ of them for a positive value, and %@ needs %@. `USize` is the same word read unsigned and holds every %@ on every target; `truncate(x) :: %@` is the low bits where a signed index really is what was wanted"_v,
        source, typeText(context, decl(), to), U64(wanted.minBits()),
        wanted.isSigned ? " and signed"_v : ""_v, U64(positive), typeText(context, decl(), from),
        U64(supplied.maxBits()), typeText(context, decl(), from), typeText(context, decl(), to));
}

/*
 * Whether the program declares a conversion of this kind between the pair.
 *
 * Asked without reporting, because a missing instance is the ordinary answer here and which message
 * follows is this file's to choose. What the match bound is thrown away: a conversion class has no
 * determined parameters, so both head arguments are the ones asked with and there is nothing to read
 * back off the instance.
 *
 * **Instance selection and not requirement discharge**, which is the one thing this cannot see: a
 * generic body whose signature declares `(Widen(a, b))` has a promise rather than an instance, so an
 * *implicit* widening inside it is reported as a plain type mismatch. A written `widen(x)` there is
 * unaffected - that is an ordinary call, and `dischargeRequirements` is what answers it.
 */
bool Resolver::relatedBy(intrinsic::Known role, DeclPtr<Type> from, DeclPtr<Type> to,
                         LocationId source) {
    auto klass = program.roles.klass(role);
    if(!klass) return false;

    InstanceMatch found;
    DeclPtr<Type> arguments[] = { from, to };
    return selectInstance(context, program, klass, arguments, 2, source, false, found);
}

/*
 * The unique `Widen` upper bound of two types, or null where neither widens to the other.
 *
 * The one place a conversion decides a type rather than being asked for one, and
 * `doc/spec/overloading.md` states the exception it is: "within a single call, the positions bound
 * to the same class variable are unified to their unique `Widen` upper bound where one exists".
 * The arms of an `if`, an `if:` and a `match` are the same shape of question about the same ladder -
 * several positions that have to end up as one type, with nothing outside them to say which - so
 * they are unified by this rather than by a second rule that could disagree with it.
 *
 * **One step, never a chain**, which is the ladder's own guardrail: `relatedBy` asks for an
 * instance relating this pair and does not look for a type both sides reach. So `I32` and `F64`
 * unify because Core writes that rung, and a pair with only a common third type does not.
 */
DeclPtr<Type> Resolver::commonWiden(DeclPtr<Type> lhs, DeclPtr<Type> rhs, LocationId source) {
    if(!lhs || !rhs) return nullptr;
    if(sameType(lhs, rhs)) return lhs;

    /*
     * A `@bits(n)` refinement unifies as what it refines, which is `doc/spec/repr.md`'s rule applied
     * to the one question this function asks: a refinement decides no type, so two positions that
     * differ by one differ by nothing, and the bound is the natural width. Two arms at *one*
     * refinement keep it, because `sameType` above has already answered them - what would be wrong
     * is inventing a refinement neither arm wrote, and canonicalizing never does.
     */
    lhs = canonicalInt(decl(), lhs);
    rhs = canonicalInt(decl(), rhs);
    if(sameType(lhs, rhs)) return lhs;

    if(relatedBy(intrinsic::Known::widen, lhs, rhs, source)) return rhs;
    if(relatedBy(intrinsic::Known::widen, rhs, lhs, source)) return lhs;

    return nullptr;
}

/*
 * Whether a `@bits(n)` value widens into a type - the **load** half of `doc/spec/repr.md`'s pair,
 * and the one direction the ladder cannot name.
 *
 * A refinement is a type this stage makes, so no rung of `intrinsic/convert.h` mentions one and no
 * generated instance relates one to anything. What relates them is repr.md: a value read out of a
 * refinement is the canonical integer, so a refinement reaches everything its *declared range*
 * reaches - which is more than the canonical does. `@bits(4) U32` fills an `I32` because four
 * unsigned bits fit thirty-two signed ones, where a whole `U32` does not.
 *
 * Asked of `integerRangeFits` so that the ladder's rule about what is lossless is applied to a
 * refinement's range rather than restated for it - and reaching the canonical is that same rule and
 * needs no arm of its own, since a refinement is by construction narrower than what it refines.
 *
 * The **store** half is not here. Narrowing *into* a refinement is not a property of a pair, it is
 * what a destination does to whatever arrives, so `convertValue` performs it as a step after the
 * ordinary conversion rather than as a rung this could report.
 */
bool Resolver::refinesInteger(DeclPtr<Type> from, DeclPtr<Type> to) {
    if(!isRefinedInt(decl(), from) || !to || decl()[to]->kind != Type::Int) return false;

    return intrinsic::integerRangeFits(((IntType*)decl()[from])->numeric(),
                                       ((IntType*)decl()[to])->numeric());
}

/*
 * "This position is written through, so a conversion cannot fill it" - see `conversionFillsPosition`,
 * which is where the rule is argued.
 *
 * A function because `convertValue` reaches it from two arms and the message is one message: the
 * conversion *exists* in both, and saying "must be I64, and this is I32" would send the author to
 * add a widening they cannot have.
 */
Node* Resolver::refuseConversion(DeclPtr<Type> to, DeclPtr<Type> from, LocationId source,
                                 StringView what) {
    context.diagnostics.error("%@ must be %@, and this is %@ - converting it would make a value of this frame, and this position is written through, so what the callee wrote would be thrown away. Declare the storage at %@"_v,
                              source, what, typeText(context, decl(), to),
                              typeText(context, decl(), from), typeText(context, decl(), to));
    return nullptr;
}

/*
 * The value at the type the position wants, or null with the reason reported.
 *
 * Four answers, in the order a reader would ask them:
 *
 *  1. **Already that type.** Nothing is built and no instance is looked for, which is what keeps the
 *     identity rung of the narrowing ladder unreachable from here - see `conversionRung`.
 *  2. **`Widen` relates them.** The conversion is a call of `widen`, which at a primitive expands to
 *     the one `convert` node it is and at a user type is the instance the author wrote. Nothing
 *     about this call is special: `finishCall` selects, discharges and expands it exactly as it does
 *     the `widen(x)` an author writes by hand, which is what makes an implicit conversion and a
 *     written one the same program.
 *  3. **`Narrow` relates them.** Refused, and the message is the one that names the fix - a
 *     conversion that loses information has to be written down.
 *  4. **Nothing relates them.** The ordinary "wrong type here" message, which is the one almost
 *     every reader of it will be looking at.
 */
Node* Resolver::convertValue(Builder& builder, Node& value, DeclPtr<Type> to, LocationId source,
                             bool implicit, StringView what, Access access, bool establishes) {
    auto from = valueType(value);
    if(sameType(from, to)) return &value;

    /*
     * A **site type** reaching the erased `(A) -> B` - Design-Iterator-Objects.md §2.3, and the
     * language's second implicit adaptation beside a container reaching a view position.
     *
     * Here rather than beside the `Widen` ladder because it is not a conversion in that sense at
     * all: nothing is computed, and what changes is the *representation* the position holds - the
     * record becomes a pair over storage, exactly as an owner becomes a window. It is at the top of
     * this function for the same reason `adaptToWindow` runs before it: a value that adapts never
     * reaches the question of which class relates two types, and the message about there being no
     * such class would name a synthesized record the author never wrote.
     *
     * One shape and no widening: the site type's own `(A) -> B` and nothing else. A position wanting
     * a different function type is a mismatch reported below, naming both, which is what a lambda at
     * the wrong arity has always been told.
     */
    if(auto site = siteTypeOf(from); site && to && decl()[to]->kind == Type::Fun &&
                                     sameType(site->lambdaShape, to)) {
        return adaptSiteValue(builder, value, *site, source);
    }

    /*
     * And an **iterator object** reaching the erased `iter(T)` - §3.4, the same adaptation one kind
     * over.
     *
     * The protocols are compared as occurrences rather than as types, because that is what the
     * erased form is interned on: `iter(T)` and `iter(->T)` are two protocols and a consumer of one
     * may not be handed the other. A site type whose protocol does not match falls through to the
     * mismatch below, which names both.
     *
     * The **position** decides the placement and nothing else does, which is the one thing that
     * differs from a closure: a lambda's is settled where the lambda is written, because its capture
     * conventions depend on it, and an object's captures were settled at its own construction. So an
     * owning position moves the record into a heap block and a borrowing one points at the frame
     * local, and §3.4's "must not copy the payload at a borrowing position" is that read backwards.
     */
    if(auto site = objectSiteOf(from); site && to && decl()[to]->kind == Type::Iter) {
        auto& want = *(IterType*)decl()[to];
        auto& producer = *decl()[site->producer];
        auto& declaring = bodyOwnerOf(decl(), producer);
        auto with = substitutionOf(decl(), producer);

        auto handover = substituteOccurrence(context, program, declaring.handover, with);
        auto resume = declaring.resume.type
            ? substituteType(context, program, declaring.resume.type, with)
            : program.types.unit;

        if(handover == want.handover && sameType(resume, want.resume)) {
            return adaptIterValue(builder, value, *site, to, access == Access::Owned, source);
        }
    }

    /*
     * A `@bits(n)` destination is reached in two steps, and that is `doc/spec/repr.md`'s own model
     * rather than a shortcut: the value is made at its natural width by whatever rule relates the
     * pair, and **the store narrows it**. Written as a composition so that everything which reaches
     * `U32` reaches `@bits(4) U32` as well - a `U8` widens into one, a `U32` is masked into one, and
     * the ladder needs no row for a type it has never heard of.
     *
     * The written-through position is judged here rather than by the inner conversion, because a
     * mask is a value of this frame whether or not the step under it built anything: `U32` into
     * `@bits(4) U32` converts nothing and still cannot fill a position the callee writes back.
     */
    if(auto canonical = canonicalInt(decl(), to); canonical != to) {
        if(!conversionFillsPosition(access, establishes)) return refuseConversion(to, from, source, what);

        auto made = convertValue(builder, value, canonical, source, implicit, what, access,
                                 establishes);
        if(!made) return nullptr;

        return builder.addConvert(source, *made, to);
    }

    auto refines = refinesInteger(from, to);

    if(refines || relatedBy(intrinsic::Known::widen, from, to, source)) {
        /*
         * A position the callee writes back through, which a converted value cannot fill - see
         * `conversionFillsPosition`. Reported here rather than at the ordinary mismatch below,
         * because the conversion *exists* and saying "must be I64, and this is I32" would send the
         * author to add the widening they cannot have.
         */
        if(!conversionFillsPosition(access, establishes)) return refuseConversion(to, from, source, what);

        // A widened refinement is the `convert` node itself rather than a call of `widen`, because
        // there is no instance to call: see `refinesInteger`. The width seam is what makes it mean
        // something - the value arrives in normal form, so widening it is the register it is
        // already in, and the node is what records that the *type* changed.
        if(refines) return builder.addConvert(source, value, to);

        Node* operands[1] = { &value };
        return applyMember(builder, intrinsic::Known::widen, 0, operands, 1, source, to);
    }

    if(relatedBy(intrinsic::Known::narrow, from, to, source)) {
        if(implicit) {
            context.diagnostics.error("%@ must be %@, and this is %@ - the conversion loses precision, so it has to be written: `truncate(x)` for the low bits, or `bitcast(x)` to reinterpret them"_v,
                                      source, what, typeText(context, decl(), to),
                                      typeText(context, decl(), from));
        } else {
            context.diagnostics.error("cannot ascribe %@ to %@: the conversion loses precision. `::` may widen but not narrow - write `truncate(x)` for the low bits, or `bitcast(x)` to reinterpret them"_v,
                                      source, typeText(context, decl(), from),
                                      typeText(context, decl(), to));
        }

        explainAbstractWidth(from, to, source);
        return nullptr;
    }

    /*
     * A growable array asked for at a position that would **own** it - the allocator, and not a
     * conversion at all.
     *
     * `let &xs = [1, 2, 3] :: [Int]` is the shape, and what is missing is not a rung between
     * `[I32 *3]` and `Array(I32)`: it is the heap the owner would live on. Reported as that, so
     * that a fixture waiting on milestone 8 says milestone 8 rather than naming a `Widen` instance
     * nobody would ever write.
     *
     * **Owning only**, which is the whole of the distinction `adaptToWindow` is on the other side
     * of: a borrowing `[T]` position is a *window*, filled by describing the caller's storage
     * differently and never by building anything. One that arrives here is one the adaptation
     * declined - the elements are a different type, and no heap would have helped - so it takes the
     * ordinary message naming both.
     */
    if(access == Access::Owned && isSequenceType(decl(), program, to)) {
        reportOwnedSequence(source);
        return nullptr;
    }

    if(implicit) {
        context.diagnostics.error("%@ must be %@, and this is %@"_v, source, what,
                                  typeText(context, decl(), to), typeText(context, decl(), from));
    } else {
        context.diagnostics.error("cannot ascribe %@ to %@: no instance of `Widen` relates them, and `::` selects a type or widens - it is not a conversion of its own"_v,
                                  source, typeText(context, decl(), from),
                                  typeText(context, decl(), to));
    }

    explainAbstractWidth(from, to, source);
    return nullptr;
}

} // namespace mir
