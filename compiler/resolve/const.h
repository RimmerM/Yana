#pragma once

#include "module.h"

/*
 * Constant evaluation.
 *
 * Two declaration forms start something at a value without running any code - a module-level `let`
 * and a field default - and neither has a program point at which an expression could be evaluated.
 * What they accept is therefore a *constant*: a value that the declaration reduces to and that
 * nothing downstream has to run.
 *
 * The two used to answer that separately, and the divergence was a bug rather than a style
 * complaint: `declareGlobal` took the type from `:: T` and never asked whether the literal in front
 * of it was a thing a `T` could be, so `let &slot = 0 :: Pair` declared a record's worth of zeroed
 * static storage that every later assignment would pre-drop as though it held a constructed value.
 * Half of that was closed by giving both the same "what bits does this literal have at this type"
 * helper; this is the other half - the *forms* a constant may be written in, which the two still
 * disagreed about. A field default accepted a nullary constructor and a global did not, so `False`
 * was a constant of `Bool` in one position and a syntax error in the other, for no reason either
 * could state.
 *
 * So there is one evaluator, and a position is a noun it words its diagnostics with. There are three
 * of them now - a parameter's `= expr` is the same rule again - and what differs between them
 * afterwards is only where the answer is recorded and what is then done with it: a `FieldDefault` on
 * a constructor and an `Arg::defaultValue` are *built* wherever the position is left out, and a
 * `Global::initial` is laid out into that global's storage. `staticForm` below is that difference,
 * and it is the only one.
 */

/*
 * What a constant is made of.
 *
 * The scalar case is what this file started as and is still what almost every constant is: a value
 * of a known type, as the bits its storage holds. Bits rather than a number for the reason
 * `floatBits` gives - the conversion is at the type's own width, so a `Float` constant is four bytes
 * of single precision and not a truncated double, and nothing downstream has to convert again. An
 * enumeration's constant is its constructor index, which is what a value of an enum record *is* (see
 * resolveConstruct).
 *
 * The other four are the aggregate half, and the shape of them is the ruling that made it possible:
 * **a constant is a tree of values and never a block of bytes**. Resolve does not know what a field's
 * offset is, how wide an address is or which end of it comes first - those are the emitting target's
 * answers, which is why a compiler-built table is a list of `TableSlot`s here and becomes bytes only
 * in `lowerProgram`. An evaluator that produced bytes would be a second answer to the question
 * `repr/table.h` owns, and the two would drift. So what is recorded here is *what the value is*, and
 * each target lays it out with the same Repr it lays every other value of that type out with.
 *
 * `Address` is the one case that is not a value at all but a promise about placement, and it is what
 * a native string literal needs: the run of a constant string points at the bytes, and where those
 * bytes land is not known until the module is placed. Natively it becomes a relocation; on JS,
 * nothing does - a host string is one value and has no run to point anywhere.
 *
 * `String` is a literal, and it is the one node that carries two answers at once. The text is what a
 * *value* of it is built from wherever one is needed, since `resolveString` already knows how to
 * build a string on either target and a second answer to that would be one too many. Underneath it,
 * on native only, is the aggregate its static form is - the two words `stringLiteral` writes, one of
 * them an `Address` of the bytes. On JS there is nothing underneath: a host string is one value, and
 * the emitter writes the text.
 */
enum class ConstKind: U8 {
    // The bits of `type`'s own storage. Every scalar: an integer, a float, a pointer, a boolean, an
    // enumeration's constructor index.
    Scalar,

    // One child per field of a tuple or record content, in field order; or one per element of a
    // `[T *n]`, in index order. `type` is the aggregate, not the content.
    Aggregate,

    // A constructor of a sum type: `index` is which one, and `children` is its payload or empty.
    Construct,

    // The address of `global`, as a value of a pointer type.
    Address,

    // A string literal, as the text the lexer decoded.
    String,
};

struct ConstValue {
    ConstValue(TypePtr type, ConstKind kind): type(type), kind(kind) {}

    TypePtr type = nullptr;
    ConstKind kind = ConstKind::Scalar;

    // `Scalar` only: the bits the storage holds, at the type's own width.
    U64 bits = 0;

    // `Construct` only: which constructor of `type` this is.
    U32 index = 0;

    // `String` only: the decoded text, interned. Its static form, where the target has one, is the
    // single child below.
    StringId text = 0;

    // `Address` only: what the address names.
    ModulePtr<Global> global = nullptr;

    // `Aggregate` and `Construct`. A null entry is a unit field, which occupies nothing and has
    // nothing to write - the same silence `write` keeps for a unit place.
    ModuleList<ModulePtr<ConstValue>, false> children;
};

/*
 * A constant, as `evaluateConstant` answers: the value, or nothing where the expression is not one.
 *
 * A pointer into the IR region rather than a value, because a constant is a tree and the arena is
 * where the tree lives. Null is the failure, and `type` is reachable through it - so a caller that
 * only wants the type of what it got asks the node.
 */
using ConstantPtr = ModulePtr<ConstValue>;

// The type a constant has, or null for the null constant. A convenience, since almost every caller
// wants exactly this and dereferencing an arena pointer for it reads badly at the call sites.
TypePtr constantType(ModuleBase local, ConstantPtr constant);

/*
 * Whether this constant can become *storage*, which is the one question a global's asks and the other
 * two positions' do not.
 *
 * One thing says no: a payload or a field reached through an *indirection* - `Constructor::boxed` or
 * `Field::boxed`, from a written `@box` or from the automatic cut a recursive declaration gets. What
 * would go in the storage there is an owning pointer to storage that no declaration allocated, and
 * there is nothing for a constant to point at. `Node(Leaf)` of `data Tree = Leaf | Node(Tree)` is the
 * shape, and it is a startup initializer rather than a constant.
 *
 * Asked in resolve rather than by whichever backend lays the constant out, because the answer is the
 * same on every target and because a *program* limit reported by a materializer would arrive with no
 * source location and no way back to the runtime form. `materializeConstant` declines the same shape
 * as an internal error, which is what it then is: this ran first.
 */
bool constantHasStaticForm(GlobalBase global, ModuleBase local, ConstantPtr constant);

// What a construction leaves out of field `field` takes instead, or null where the declaration
// wrote no default for it. One scan, shared by the expression form and the constant form, so that
// what a field falls back to cannot depend on which of the two is asking.
ConstantPtr fieldDefaultOf(GlobalBase global, GlobalList<FieldDefault>* defaults, U16 field);

/*
 * The constant `expr` was written as, or null where it is not a constant of the type this position
 * requires - reported here, since the reason is the case analysis below and restating it at a call
 * site is what this exists to stop.
 *
 * `expected` is the type the position already has, and null where the position has none: a field
 * knows its type and a global is told one by its `:: T`, or takes the literal's own default. A
 * position that has one still accepts an ascription, and the two then have to agree.
 *
 * `what` names the position - "a global's initializer", "a field default" - and is the only thing
 * the diagnostics differ by.
 *
 * `staticForm` says whether this position turns the constant into *storage*. A global does - its
 * bytes are laid out by whichever backend emits - and the other two positions do not: a field default
 * and a default argument are values, built at the construction or the call site by the same code the
 * author could have written. The only thing it changes is a string literal, whose static form is a
 * blob global of its own that a position not needing one would leave unemitted; see stringConstant.
 *
 * `notConstant`, where given, turns the "this is not a constant *form*" outcomes from reports into
 * an answer: the flag is set, nothing is written, and the caller decides what a non-constant means
 * there. That is what a root-module `let` needs - its initializer may be an ordinary expression, run
 * by the program's entry sequence - and it is deliberately not the whole failure set. A literal out
 * of range for its type is the right form with the wrong contents, so it stays an error wherever it
 * is written; falling back to a runtime initializer for one would silently accept `300 :: U8`.
 */
ConstantPtr evaluateConstant(Module& module, const ast::Expr& expr, TypePtr expected, StringView what,
                             bool staticForm, bool* notConstant = nullptr);
