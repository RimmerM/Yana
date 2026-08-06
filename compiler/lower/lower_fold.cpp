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

// What an instruction that is already in a block comes to, for the pass below. One switch over the
// three shapes the folds above cover; everything else answers nothing.
Folded foldInstruction(LowerBase base, LowerInst* inst) {
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
    }
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
