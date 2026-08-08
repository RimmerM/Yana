#include "lower_fold.h"
#include "lower_builder.h"

namespace {

// How many bits of a value of this type are the value. Only asked of an integer type; a pointer is
// never folded, because a constant address is not something this translation ever produces and
// `And`/`Or` at a pointer result mean the tagging arithmetic rather than plain bit operations.
U32 widthOf(LowerType type) {
    return type == LowerType::Int32 ? 32 : 64;
}

U64 maskOf(U32 bits) {
    return bits >= 64 ? maxLimit<U64> : (U64(1) << bits) - 1;
}

// Whether this value is an integer constant, and what it is - the low bits of its own type, so that
// a mask written as a 64-bit complement and one written as a 32-bit pattern are one constant at
// `Int32`. False for a float immediate, which nothing here folds.
bool lowerConstantOf(LowerBase base, LowerValue* value, U64& into) {
    auto inst = value->inst();
    if(inst->kind != LowerInst::Imm || !isInt(value->type)) return false;

    into = ((LowerImm*)inst)->i & maskOf(widthOf(value->type));
    return true;
}

// The same pattern read as a signed number of that width, which is what every operation that has a
// signed and an unsigned form needs and what a widening cast of a signed source produces.
I64 signedValue(U64 value, U32 bits) {
    if(bits >= 64) return I64(value);

    auto spare = 64 - bits;
    return I64(value << spare) >> spare;
}

/*
 * The top 64 bits of a 64x64 product, out of 32-bit pieces.
 *
 * By hand rather than through a wider integer type because there is no portable one: `__int128` is
 * a GNU extension and this builds under MSVC too. Four partial products, and the only subtle line
 * is the carry - the two cross terms and the top half of the low product all land in the same
 * 32-bit column, and their sum can reach into the next.
 */
U64 highProduct(U64 a, U64 b) {
    auto aLow = a & 0xffffffffu, aHigh = a >> 32;
    auto bLow = b & 0xffffffffu, bHigh = b >> 32;

    auto low = aLow * bLow;
    auto crossA = aHigh * bLow;
    auto crossB = aLow * bHigh;

    auto carry = ((low >> 32) + (crossA & 0xffffffffu) + (crossB & 0xffffffffu)) >> 32;
    return aHigh * bHigh + (crossA >> 32) + (crossB >> 32) + carry;
}

// The same half read as a signed number. The unsigned product treats each sign bit as magnitude
// worth 2^64, so a negative operand has contributed the other operand's whole value one place too
// high, and subtracting it once per negative operand is the correction.
I64 signedHighProduct(I64 a, I64 b) {
    auto high = I64(highProduct(U64(a), U64(b)));
    if(a < 0) high -= b;
    if(b < 0) high -= a;
    return high;
}

// Both operands known: the operation itself, at `bits`. False where the answer is not one this may
// state - a division by zero, or a shift by a distance the machine would have masked.
bool evaluate(LowerInst::Kind kind, U64 a, U64 b, U32 bits, U64& into) {
    auto sa = signedValue(a, bits);
    auto sb = signedValue(b, bits);
    auto lowest = signedValue(U64(1) << (bits - 1), bits);

    switch(kind) {
        case LowerInst::Add: into = a + b; break;
        case LowerInst::Sub: into = a - b; break;

        // Signed and unsigned multiplication answer the same low bits, which is why the IR keeps the
        // two apart only because the machine does - see the LLVM backend, which merges them too.
        case LowerInst::Mul:
        case LowerInst::IMul: into = a * b; break;

        case LowerInst::Div: if(!b) return false; into = a / b; break;
        case LowerInst::Rem: if(!b) return false; into = a % b; break;

        // The one signed pair with no answer inside the width, besides the zero divisor.
        case LowerInst::IDiv:
            if(!sb || (sa == lowest && sb == -1)) return false;
            into = U64(sa / sb);
            break;
        case LowerInst::IRem:
            if(!sb || (sa == lowest && sb == -1)) return false;
            into = U64(sa % sb);
            break;

        // The half of the product that does not fit, which is the one thing here that has to be
        // computed at *twice* the operands' width. A 32-bit pair fits a U64; a 64-bit pair is the
        // schoolbook split, because there is no wider integer to hold it.
        case LowerInst::MulHi:
            into = bits <= 32 ? (a * b) >> bits : highProduct(a, b);
            break;
        case LowerInst::IMulHi:
            into = bits <= 32 ? U64(sa * sb) >> bits : U64(signedHighProduct(sa, sb));
            break;

        case LowerInst::Shl: if(b >= bits) return false; into = a << b; break;
        case LowerInst::Shr: if(b >= bits) return false; into = a >> b; break;
        case LowerInst::Sar: if(b >= bits) return false; into = U64(sa >> b); break;

        case LowerInst::And: into = a & b; break;
        case LowerInst::Or:  into = a | b; break;
        case LowerInst::Xor: into = a ^ b; break;

        default: return false;
    }

    into &= maskOf(bits);
    return true;
}

/*
 * What an operation comes to, before anywhere has been chosen to put it.
 *
 * The answer is separated from the placing because the two callers place it differently: the builder
 * appends, since the instruction it is standing in for had not been created yet, and the pass has to
 * put it exactly where the instruction it replaces was. Nothing below allocates or touches a block.
 */
struct Folded {
    enum Kind: U8 { None, Constant, Operand };

    Kind kind = None;
    U64 constant = 0;
    LowerValue* operand = nullptr;

    static Folded nothing() { return Folded {}; }
    static Folded value(U64 c) { return Folded { Constant, c, nullptr }; }
    static Folded forward(LowerValue* v) { return Folded { Operand, 0, v }; }
};

Folded foldUnaryValue(LowerBase base, LowerInst::Kind kind, LowerValue* arg, LowerType type) {
    if(!isInt(type) || !isInt(arg->type)) return Folded::nothing();

    U64 value;
    if(!lowerConstantOf(base, arg, value)) return Folded::nothing();

    switch(kind) {
        case LowerInst::Neg: return Folded::value((U64(0) - value) & maskOf(widthOf(type)));
        case LowerInst::Not: return Folded::value(~value & maskOf(widthOf(type)));
        default: return Folded::nothing();
    }
}

// Widening carries the sign bit up only for a source that has one and narrowing truncates either
// way, which is `CreateIntCast` and what the x64 selector's constant move does. Float conversions
// are left alone: `lowerConstantOf` declines a float immediate, so both directions fall through.
Folded foldCastValue(LowerBase base, LowerValue* arg, LowerType type, bool signedSource) {
    if(!isInt(type) || !isInt(arg->type)) return Folded::nothing();

    U64 value;
    if(!lowerConstantOf(base, arg, value)) return Folded::nothing();

    auto extended = signedSource ? U64(signedValue(value, widthOf(arg->type))) : value;
    return Folded::value(extended & maskOf(widthOf(type)));
}

/*
 * A binary operation over what is known about its operands.
 *
 * Two tiers, and the line between them is which questions a *pointer* result may be asked. Adding or
 * merging a zero is the identity at every width, so the pointer forms - address arithmetic, and the
 * shift a narrow reference is tagged with - take it too. Everything below that is stated at the
 * result's width, and only an integer type has one.
 */
Folded foldBinaryValue(LowerBase base, LowerInst::Kind kind, LowerValue* lhs, LowerValue* rhs,
                       LowerType type) {
    U64 a = 0, b = 0;
    auto knownLhs = lowerConstantOf(base, lhs, a);
    auto knownRhs = lowerConstantOf(base, rhs, b);
    if(!knownLhs && !knownRhs) return Folded::nothing();

    // Answering with an operand is only right where that operand is already the result's own type.
    // `sub p, q` is the one that says why: it answers a distance, and handing back `p` for it would
    // be an address where an integer was asked for.
    auto forward = [&](LowerValue* value) {
        return value->type == type ? Folded::forward(value) : Folded::nothing();
    };

    Folded zero = Folded::nothing();

    switch(kind) {
        case LowerInst::Add:
        case LowerInst::Or:
        case LowerInst::Xor:
            if(knownRhs && b == 0) zero = forward(lhs);
            else if(knownLhs && a == 0) zero = forward(rhs);
            break;

        case LowerInst::Sub:
            if(knownRhs && b == 0) zero = forward(lhs);
            break;

        default:
            break;
    }

    if(zero.kind != Folded::None) return zero;
    if(!isInt(type) || !isInt(lhs->type) || !isInt(rhs->type)) return Folded::nothing();

    auto bits = widthOf(type);

    if(knownLhs && knownRhs) {
        U64 result;
        if(!evaluate(kind, a, b, bits, result)) return Folded::nothing();

        return Folded::value(result);
    }

    /*
     * One side known, which is the case the masking and shifting produces wherever a field's width
     * fills the unit it sits in or its offset is zero. `maskToWidth` at a full-width type is `x & -1`
     * and `truncateToWidth` at a type whose sign bit is already the register's is `x << 0 >> 0`; both
     * were written to be folded here rather than guarded at every site that can produce one.
     */
    auto all = maskOf(bits);

    switch(kind) {
        case LowerInst::Mul:
        case LowerInst::IMul:
            if(knownRhs && b == 1) return forward(lhs);
            if(knownLhs && a == 1) return forward(rhs);
            if((knownRhs && b == 0) || (knownLhs && a == 0)) return Folded::value(0);
            break;

        // Only the zero, and no `forward` for it: the high half of a product is not one of the
        // operands at any multiplier, and at 1 it is zero unsigned and the sign extension signed.
        case LowerInst::MulHi:
        case LowerInst::IMulHi:
            if((knownRhs && b == 0) || (knownLhs && a == 0)) return Folded::value(0);
            break;

        case LowerInst::And:
            if(knownRhs && b == all) return forward(lhs);
            if(knownLhs && a == all) return forward(rhs);
            if((knownRhs && b == 0) || (knownLhs && a == 0)) return Folded::value(0);
            break;

        case LowerInst::Or:
            if((knownRhs && b == all) || (knownLhs && a == all)) return Folded::value(all);
            break;

        // A shift of zero is zero at every distance, including the ones the constant fold above
        // declines - the machine's masking cannot turn zero into anything else.
        case LowerInst::Shl:
        case LowerInst::Shr:
        case LowerInst::Sar:
            if(knownRhs && b == 0) return forward(lhs);
            if(knownLhs && a == 0) return Folded::value(0);
            break;

        default:
            break;
    }

    return Folded::nothing();
}

/*
 * Whether this value is already one of the two integers a `Bool` is.
 *
 * A `Cmp` is the only instruction in this IR whose result is defined to be 0 or 1 - every other
 * producer of a truth value is one of these wrapped in something, which is exactly what the two
 * folds below unwrap. A `Select` between the literals 1 and 0 is admitted as well, so that the two
 * rules compose in one round rather than needing a second: `cmp_eq (select 1, 0, c), 1` is the shape
 * lowering actually emits, and answering it needs both halves at once.
 */
bool isBooleanValued(LowerBase base, LowerValue* value) {
    auto inst = value->inst();
    if(inst->kind == LowerInst::Cmp) return true;

    if(inst->kind != LowerInst::Select) return false;

    auto select = (LowerInstSelect*)inst;
    if(select->getEmbeddedCmp()) return false;

    U64 whenTrue, whenFalse;
    if(!lowerConstantOf(base, base[select->lhs], whenTrue)) return false;
    if(!lowerConstantOf(base, base[select->rhs], whenFalse)) return false;

    return whenTrue == 1 && whenFalse == 0;
}

/*
 * A truth value materialized and then asked whether it is true.
 *
 * Two rewrites, and they only pay together. Lowering a `Bool` produces `select 1, 0, %c` - the
 * comparison as a number - and lowering the `if` that reads it produces `cmp_eq %that, 1`, so the
 * four instructions a niche test compiles to are a comparison, a materialization of it, a comparison
 * against the materialization, and the branch. Both middle instructions are identities:
 *
 *   `select 1, 0, %c`  is  `%c`          for a `%c` that is already 0 or 1
 *   `cmp_eq %b, 1`     is  `%b`          for the same reason, and so is `cmp_neq %b, 0`
 *
 * `Maybe(Tree)` is what makes this worth a pass rather than a peephole. `node.left is Just(l)` is a
 * niche range test, and the four instructions above become `cmp; jbe` - seven instructions per node
 * down to three, twice in every walk of the tree.
 *
 * The inverses (`cmp_eq %b, 0`, `cmp_neq %b, 1`) are deliberately not folded. They are the *negation*
 * of `%b`, which is a new instruction rather than one of the operands, and `Folded` says either a
 * constant or an operand already there - inverting the comparison inside `%c` instead would rewrite
 * an instruction this pass does not own, and `%c` may have other readers that wanted it the first way.
 */
Folded foldBooleanValue(LowerBase base, LowerInst* inst) {
    if(inst->kind == LowerInst::Select) {
        auto select = (LowerInstSelect*)inst;
        if(select->getEmbeddedCmp()) return Folded::nothing();

        auto condition = base[select->cmp];
        if(!isBooleanValued(base, condition)) return Folded::nothing();
        if(condition->type != select->result.type) return Folded::nothing();

        U64 whenTrue, whenFalse;
        if(!lowerConstantOf(base, base[select->lhs], whenTrue)) return Folded::nothing();
        if(!lowerConstantOf(base, base[select->rhs], whenFalse)) return Folded::nothing();
        if(whenTrue != 1 || whenFalse != 0) return Folded::nothing();

        return Folded::forward(condition);
    }

    if(inst->kind != LowerInst::Cmp) return Folded::nothing();

    auto compare = (LowerInstCmp*)inst;
    auto kind = compare->getCmp();
    if(kind != LowerCmp::eq && kind != LowerCmp::neq) return Folded::nothing();

    // Either way round: the constant is the one side and the truth value the other.
    auto lhs = base[compare->lhs];
    auto rhs = base[compare->rhs];

    U64 constant;
    LowerValue* boolean = nullptr;

    if(lowerConstantOf(base, rhs, constant)) boolean = lhs;
    else if(lowerConstantOf(base, lhs, constant)) boolean = rhs;
    else return Folded::nothing();

    if(!isBooleanValued(base, boolean)) return Folded::nothing();
    if(boolean->type != compare->result.type) return Folded::nothing();

    auto identity = (kind == LowerCmp::eq && constant == 1) ||
                    (kind == LowerCmp::neq && constant == 0);

    return identity ? Folded::forward(boolean) : Folded::nothing();
}

// The same relation with its operands the other way round, for a comparison written with its
// constant on the left. `a < b` is `b > a`; the equalities read the same either way. Sound here
// because every comparison this file looks at is over integers - the exchange is the one the float
// ordering in the x64 transform has to be careful about, and no float reaches this.
LowerCmp swappedCmp(LowerCmp cmp) {
    switch(cmp) {
        case LowerCmp::gt:  return LowerCmp::lt;
        case LowerCmp::ge:  return LowerCmp::le;
        case LowerCmp::lt:  return LowerCmp::gt;
        case LowerCmp::le:  return LowerCmp::ge;
        case LowerCmp::igt: return LowerCmp::ilt;
        case LowerCmp::ige: return LowerCmp::ile;
        case LowerCmp::ilt: return LowerCmp::igt;
        case LowerCmp::ile: return LowerCmp::ige;
        default: return cmp;
    }
}

// What a comparison against a constant is rewritten into: the same relation over a different operand
// and a different immediate. Only ever `eq` or `neq`, because narrowing is the whole point.
struct Narrowed {
    LowerValue* value;
    U64 constant;
    LowerCmp cmp;
};

/*
 * A range test spelled as a subtraction and an unsigned comparison.
 *
 * Two rewrites, and as with the pair above they are worth much more together than apart. A niche
 * `Maybe` reaches here as the four instructions §10.1 left three of:
 *
 *   %a = sub %x, 1
 *   %b = cmp_le %a, 18446744073709551614      ; that is, `all - 1`
 *
 * which the x64 selector emits as `dec` and a `cmp` against a 64-bit immediate - so a `movabs` for
 * the constant as well. What it means is `x != 0`, which is `test %rax,%rax`.
 *
 *   an unsigned ordering whose true or false set holds one value   is  that equality
 *   `cmp_eq (sub %x, C), K`                                        is  `cmp_eq %x, K + C`
 *
 * The first is what turns the ordering into an equality; the second is what then lets the subtraction
 * go. Neither alone removes an instruction, which is why they are one function and why the pass loop
 * around it has to be able to apply the second to the first's result.
 *
 * The second is gated on the subtraction having exactly one reader. Where it has more it stays, and
 * moving the comparison off it would only lengthen the live range of what it reads - the rewrite is
 * an unqualified win precisely when it is what makes the arithmetic dead.
 *
 * The ends of a range - `x <=u all`, which is always true, and `x <u 0`, which is never - are
 * deliberately not answered here. They are a constant rather than a comparison, so they belong to
 * `Folded`, and nothing in this compiler emits one.
 */
bool narrowComparison(LowerBase base, LowerInst* inst, Narrowed& into) {
    if(inst->kind != LowerInst::Cmp) return false;

    auto compare = (LowerInstCmp*)inst;
    auto lhs = base[compare->lhs];
    auto rhs = base[compare->rhs];

    // Both sides the same integer type: a comparison of pointers reaches here as a `bitcast` and its
    // operands, and a float comparison is not one of these shapes at all.
    if(lhs->type != rhs->type || !isInt(lhs->type)) return false;

    auto cmp = compare->getCmp();
    LowerValue* value;
    U64 constant, other;

    if(!lowerConstantOf(base, rhs, constant)) {
        if(!lowerConstantOf(base, lhs, constant)) return false;

        value = rhs;
        cmp = swappedCmp(cmp);
    } else if(lowerConstantOf(base, lhs, other)) {
        // Both constant. Not this function's business, and answering it would be a rewrite that is
        // not a fixpoint - the loop below runs until nothing changes.
        return false;
    } else {
        value = lhs;
    }

    auto all = maskOf(widthOf(value->type));

    /*
     * The ordering, where one side of it admits a single value. Four relations and two ends each:
     * `<u k` is [0, k-1] and `>=u k` is its complement, so the interesting constants are the ones
     * that leave one number on one side or the other.
     */
    switch(cmp) {
        case LowerCmp::lt:
            if(constant == 1) { into = { value, 0, LowerCmp::eq }; return true; }
            if(constant == all) { into = { value, all, LowerCmp::neq }; return true; }
            break;

        case LowerCmp::le:
            if(constant == 0) { into = { value, 0, LowerCmp::eq }; return true; }
            if(constant == all - 1) { into = { value, all, LowerCmp::neq }; return true; }
            break;

        case LowerCmp::gt:
            if(constant == all - 1) { into = { value, all, LowerCmp::eq }; return true; }
            if(constant == 0) { into = { value, 0, LowerCmp::neq }; return true; }
            break;

        case LowerCmp::ge:
            if(constant == all) { into = { value, all, LowerCmp::eq }; return true; }
            if(constant == 1) { into = { value, 0, LowerCmp::neq }; return true; }
            break;

        default:
            break;
    }

    if(cmp != LowerCmp::eq && cmp != LowerCmp::neq) return false;

    /*
     * The equality, over a value that is itself a constant away from another one. Adding a constant
     * is a bijection on the machine's integers, so the wrapping needs no guard - the two sides move
     * together whatever they wrap through, which is why this holds where the ordering above would
     * not.
     */
    auto source = value->inst();
    if(source->kind != LowerInst::Add && source->kind != LowerInst::Sub) return false;
    if(((LowerInstSingle*)source)->result.uses.size() != 1) return false;

    auto binary = (LowerInstBinary*)source;
    auto from = base[binary->lhs];

    // Declines the pointer forms of both, which answer a different type than they read: `ptr - ptr`
    // is a distance, and there is no constant to move across it.
    if(from->type != value->type || !isInt(from->type)) return false;

    U64 offset;
    if(!lowerConstantOf(base, base[binary->rhs], offset)) return false;

    auto adjusted = source->kind == LowerInst::Sub ? constant + offset : constant - offset;
    into = { from, adjusted & all, cmp };
    return true;
}

// What an instruction that is already in a block comes to, for the pass below. One switch over the
// three shapes the folds above cover; everything else answers nothing.
Folded foldInstruction(LowerBase base, LowerInst* inst) {
    if(auto boolean = foldBooleanValue(base, inst); boolean.kind != Folded::None) return boolean;

    if(isCast(inst)) {
        if(inst->kind != LowerInst::Cast) return Folded::nothing();

        auto cast = (LowerInstCast*)inst;
        return foldCastValue(base, base[cast->from], cast->result.type, cast->isSignedSource());
    }

    if(isUnaryArith(inst)) {
        auto unary = (LowerInstUnary*)inst;
        return foldUnaryValue(base, inst->kind, base[unary->from], unary->result.type);
    }

    if(isBinary(inst) && inst->kind != LowerInst::Cmp) {
        auto binary = (LowerInstBinary*)inst;
        return foldBinaryValue(base, inst->kind, base[binary->lhs], base[binary->rhs],
                               binary->result.type);
    }

    return Folded::nothing();
}

/*
 * The answer as a value, for the builder.
 *
 * An operation that comes to one of its own operands is answered with that operand's *instruction*,
 * since that is what the builders return and what every caller reads `created().ptr` off - so it is
 * declined where the instruction produces more than one value, which would hand the caller the wrong
 * one. The name the operation would have carried is adopted where the operand has none, so that a
 * dump still says what the value was called.
 */
LowerInst* materialize(LowerBase base, LowerModule& module, LowerBlock& block, Folded folded,
                       LowerType type, StringId name) {
    if(folded.kind == Folded::Constant) {
        return block.addInst(base, new (module.arena) LowerImm(name, type, folded.constant));
    }

    if(folded.kind == Folded::Operand) {
        auto inst = folded.operand->inst();
        if(inst->createdCount != 1) return nullptr;

        if(name && !folded.operand->name) folded.operand->name = name;
        return inst;
    }

    return nullptr;
}

} // namespace

LowerInst* foldUnaryArith(LowerBase base, LowerModule& module, LowerBlock& block,
                          LowerInst::Kind kind, LowerValue* arg, LowerType type, StringId name) {
    return materialize(base, module, block, foldUnaryValue(base, kind, arg, type), type, name);
}

LowerInst* foldCast(LowerBase base, LowerModule& module, LowerBlock& block, LowerValue* arg,
                    LowerType type, bool signedSource, StringId name) {
    return materialize(base, module, block, foldCastValue(base, arg, type, signedSource), type, name);
}

LowerInst* foldBinary(LowerBase base, LowerModule& module, LowerBlock& block, LowerInst::Kind kind,
                      LowerValue* lhs, LowerValue* rhs, LowerType type, StringId name) {
    return materialize(base, module, block, foldBinaryValue(base, kind, lhs, rhs, type), type, name);
}

void foldFunctionConstants(LowerBase base, LowerModule& module, LowerFunction& fun) {
    auto changed = true;

    while(changed) {
        changed = false;

        for(auto blockPtr: fun.blocks.contents(base)) {
            auto block = base[blockPtr];

            // Inline: one of these per block per round, holding the instructions of one block while
            // the list it came from is rewritten.
            SmallArray<LowerPtr<LowerInst>, 32> kept;
            auto rewrote = false;

            for(auto instPtr: block->instructions.contents(base)) {
                auto inst = base[instPtr];

                /*
                 * A comparison narrowed in place, before it is asked what it comes to. In place
                 * because the answer is another comparison rather than a constant or an operand
                 * already standing there, which is all `Folded` may say - and the result value is
                 * the same one, so every reader of it stays pointed at the right thing.
                 *
                 * The immediate it now reads is a new one placed immediately above it; the one it
                 * stopped reading is left to `removeDeadConstants` at the end of the pipeline, which
                 * is where every other rewrite here leaves its orphaned literals too.
                 *
                 * The addition or subtraction the second rewrite takes the comparison off is what
                 * `removeDeadValues` below is for, and it is the reason this pass needs one at all:
                 * every other rewrite here replaces the instruction it folded, so this is the only
                 * one that can leave a computation standing with nothing reading it.
                 */
                Narrowed narrowed;
                if(narrowComparison(base, inst, narrowed)) {
                    auto compare = (LowerInstCmp*)inst;
                    auto imm = new (module.arena) LowerImm(StringId(), narrowed.value->type,
                                                          narrowed.constant);
                    imm->block = blockPtr;
                    imm->source = inst->source;
                    kept.push(imm - base);

                    setOperand(base, module.arena, inst, compare->lhs, narrowed.value);
                    setOperand(base, module.arena, inst, compare->rhs, &imm->result);
                    compare->setCmp(narrowed.cmp);
                    rewrote = true;
                }

                auto folded = foldInstruction(base, inst);

                if(folded.kind == Folded::None) {
                    kept.push(instPtr);
                    continue;
                }

                auto result = ((LowerInstSingle*)inst)->created().ptr;
                auto replacement = folded.operand;

                /*
                 * A constant is a new immediate standing exactly where the instruction stood, which
                 * is what makes it dominate the same readers. `addInst` cannot place it - it appends,
                 * and the list is being rebuilt - so it is attached to the block by hand; an
                 * immediate reads nothing, so there is no use list for that to skip.
                 */
                if(folded.kind == Folded::Constant) {
                    auto imm = new (module.arena) LowerImm(result->name, result->type, folded.constant);
                    imm->block = blockPtr;
                    imm->source = inst->source;
                    kept.push(imm - base);
                    replacement = &imm->result;
                }

                detach(base, inst);
                replaceUses(base, module.arena, result - base, replacement - base);
                rewrote = true;
            }

            if(!rewrote) continue;

            block->instructions.clear();
            for(auto instPtr: kept) block->instructions.push(module.arena, instPtr);
            changed = true;
        }

        // Inside the round rather than after it, and unconditionally: what the narrowing above
        // orphans is in whichever block defined it, which this round may already have walked past.
        if(removeDeadValues(base, module.arena, fun)) changed = true;
    }
}

bool isRepeatable(LowerInst* inst) {
    switch(inst->kind) {
        case LowerInst::Set:
        case LowerInst::Cast:  case LowerInst::Bitcast:
        case LowerInst::Neg:   case LowerInst::Not:
        case LowerInst::Add:   case LowerInst::Sub:
        case LowerInst::Mul:   case LowerInst::IMul:
        case LowerInst::Div:   case LowerInst::IDiv:
        case LowerInst::Rem:   case LowerInst::IRem:
        case LowerInst::MulHi: case LowerInst::IMulHi:
        case LowerInst::Shl:   case LowerInst::Shr: case LowerInst::Sar:
        case LowerInst::And:   case LowerInst::Or:  case LowerInst::Xor:
        case LowerInst::Cmp:
        case LowerInst::Select:
            return true;
        default:
            return false;
    }
}

bool removeDeadValues(LowerBase base, Region<LowerRegion>& arena, LowerFunction& fun) {
    auto changed = true;
    auto dropped = false;

    while(changed) {
        changed = false;

        for(auto blockPtr: fun.blocks.contents(base)) {
            auto block = base[blockPtr];

            SmallArray<LowerPtr<LowerInst>, 32> kept;
            auto rewrote = false;

            for(auto instPtr: block->instructions.contents(base)) {
                auto inst = base[instPtr];

                if(isRepeatable(inst) && ((LowerInstSingle*)inst)->created().ptr->uses.isEmpty()) {
                    detach(base, inst);
                    rewrote = true;
                    continue;
                }

                kept.push(instPtr);
            }

            if(!rewrote) continue;

            block->instructions.clear();
            for(auto instPtr: kept) block->instructions.push(arena, instPtr);
            changed = true;
            dropped = true;
        }
    }

    return dropped;
}

void removeDeadConstants(LowerBase base, Region<LowerRegion>& arena, LowerFunction& fun) {
    auto changed = true;

    while(changed) {
        changed = false;

        for(auto blockPtr: fun.blocks.contents(base)) {
            auto block = base[blockPtr];

            // Inline: one of these per block per round of a loop that runs until nothing changes,
            // holding the instructions of one block.
            SmallArray<LowerPtr<LowerInst>, 32> kept;
            auto dropped = false;

            for(auto instPtr: block->instructions.contents(base)) {
                auto inst = base[instPtr];

                if(inst->kind == LowerInst::Imm && inst->created().ptr->uses.isEmpty()) {
                    dropped = true;
                    continue;
                }

                kept.push(instPtr);
            }

            if(!dropped) continue;

            block->instructions.clear();
            for(auto instPtr: kept) block->instructions.push(arena, instPtr);
            changed = true;
        }
    }
}
