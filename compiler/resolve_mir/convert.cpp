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
 */
static bool conversionFillsPosition(Access access) {
    return access != Access::Unique;
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
                             bool implicit, StringView what, Access access) {
    auto from = valueType(value);
    if(sameType(from, to)) return &value;

    if(relatedBy(intrinsic::Known::widen, from, to, source)) {
        /*
         * A position the callee writes back through, which a converted value cannot fill - see
         * `conversionFillsPosition`. Reported here rather than at the ordinary mismatch below,
         * because the conversion *exists* and saying "must be I64, and this is I32" would send the
         * author to add the widening they cannot have.
         */
        if(!conversionFillsPosition(access)) {
            context.diagnostics.error("%@ must be %@, and this is %@ - widening it would make a value of this frame, and this position is written through, so what the callee wrote would be thrown away. Declare the storage at %@"_v,
                                      source, what, typeText(context, decl(), to),
                                      typeText(context, decl(), from), typeText(context, decl(), to));
            return nullptr;
        }

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
