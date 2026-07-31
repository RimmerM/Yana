#pragma once

#include "opt.h"
#include "../resolve/builder.h"

/*
 * What the passes share: the state one function is optimized against, and the handful of IR
 * operations that are the same in all of them.
 *
 * Every rewrite here is a use-list rewrite. The resolve IR keeps both directions - an instruction
 * names its operands and every value names its users - and a pass that updates one without the
 * other leaves an IR that prints correctly and walks wrongly, which is the failure mode worth
 * spending a header on preventing.
 */

struct OptContext {
    Context& context;
    Program& program;
    GlobalBase global;
    ModuleBase local;
    ReprTable& repr;

    Module* module = nullptr;
    Function* function = nullptr;

    // Set by any rewrite. The driver runs the passes to a fixed point over one function, because
    // folding exposes identities and identities expose more folding.
    bool changed = false;
};

/*
 * The operands of one instruction, in the order `Block::add` records uses in.
 *
 * `f` is handed each operand and answers what it should become, which is the one shape that serves
 * a field and a list element alike - a `ModuleList` element is reached through `get`/`set` and
 * there is no reference to hand out. Returning the operand unchanged is the read-only use.
 *
 * This has to name exactly what `Block::add` names. An operand it misses is one a replacement walks
 * past, leaving a use of a value that is no longer defined; an operand it invents is a use count
 * that never balances.
 */
template<class F>
void mapOperands(ModuleBase base, Value& instruction, F&& f) {
    auto place = [&](Place& p) {
        if(p.root == PlaceRoot::Pointer || p.root == PlaceRoot::Borrow) p.pointer = f(p.pointer);

        for(Size i = 0; i < p.projections.size(); i++) {
            auto projection = p.projections.get(base, i);
            if(!projection.value) continue;

            projection.value = f(projection.value);
            p.projections.set(base, i, projection);
        }
    };

    auto list = [&](ModuleList<ModulePtr<Value>, false>& values) {
        for(Size i = 0; i < values.size(); i++) values.set(base, i, f(values.get(base, i)));
    };

    Place* places[kMaxPlaces];
    auto placeCount = instructionPlaceSlots(instruction, places);
    for(Size i = 0; i < placeCount; i++) place(*places[i]);

    switch(instruction.kind) {
        case Value::Init:
        case Value::Assign: {
            auto& init = (InstInit&)instruction;
            init.value = f(init.value);
            break;
        }
        case Value::Exchange: {
            auto& exchange = (InstExchange&)instruction;
            exchange.value = f(exchange.value);
            break;
        }
        case Value::Native:
            list(((InstNative&)instruction).args);
            break;
        case Value::Cast:
        case Value::Neg:
        case Value::Not: {
            auto& unary = (InstUnary&)instruction;
            unary.from = f(unary.from);
            break;
        }
        case Value::Add: case Value::Sub: case Value::Mul: case Value::Div: case Value::Rem:
        case Value::Shl: case Value::Shr: case Value::Sar:
        case Value::And: case Value::Or: case Value::Xor: case Value::Cmp: {
            auto& binary = (InstBinary&)instruction;
            binary.lhs = f(binary.lhs);
            binary.rhs = f(binary.rhs);
            break;
        }
        case Value::Call:
            list(((InstCall&)instruction).args);
            break;
        case Value::CallDyn: {
            auto& call = (InstCallDyn&)instruction;
            call.callable = f(call.callable);
            call.address = f(call.address);
            list(call.args);
            break;
        }
        case Value::GenCall:
            list(((InstGenCall&)instruction).args);
            break;
        case Value::Je: {
            auto& branch = (InstJe&)instruction;
            branch.cond = f(branch.cond);
            break;
        }
        case Value::Ret: {
            auto& ret = (InstRet&)instruction;
            ret.value = f(ret.value);
            break;
        }
        case Value::Phi: {
            auto& phi = (InstPhi&)instruction;
            for(Size i = 0; i < phi.inputs.size(); i++) {
                auto input = phi.inputs.get(base, i);
                input.value = f(input.value);
                phi.inputs.set(base, i, input);
            }
            break;
        }
        default:
            break;
    }
}

template<class F>
inline void eachOperand(ModuleBase base, Value& instruction, F&& f) {
    mapOperands(base, instruction, [&](ModulePtr<Value> operand) {
        if(operand) f(operand);
        return operand;
    });
}

/*
 * Whether this value is one the optimizer may compute again, or not compute at all.
 *
 * The list is short on purpose, and every kind left out of it is left out for a reason rather than
 * from caution: the ownership instructions are the decisions the analyses already took, the calls
 * do whatever their callee does, and `LoadPlace` reads storage that something else may be writing -
 * which is a question about aliasing rather than about the instruction, and is what the place
 * forwarding pass exists to answer.
 */
inline bool isPureValue(const Value& value) {
    switch(value.kind) {
        case Value::ConstInt: case Value::ConstFloat: case Value::ConstDouble:
        case Value::Cast: case Value::Neg: case Value::Not:
        case Value::Add: case Value::Sub: case Value::Mul: case Value::Div: case Value::Rem:
        case Value::Shl: case Value::Shr: case Value::Sar:
        case Value::And: case Value::Or: case Value::Xor: case Value::Cmp:
        case Value::Symbol: case Value::TypeMetric:
            return true;
        default:
            return false;
    }
}

// Removing one entry from a value's use list. One rather than all: an instruction naming the same
// value twice appears twice, and the list has to keep saying so.
void dropUse(OptContext& opt, ModulePtr<Value> value, ModulePtr<Inst> user);

// Pointing every reader of one value at another, use lists and operands together.
void replaceValue(OptContext& opt, ModulePtr<Value> from, ModulePtr<Value> to);

// Taking an instruction out of circulation: it stops counting as a user of everything it read, and
// is dropped from its block. Only ever called on a pure instruction nothing reads.
void eraseInstruction(OptContext& opt, ModulePtr<Inst> instruction);

/*
 * An integer type the optimizer is willing to compute in, and the two numbers it needs.
 *
 * `bits` is the type's own width and the width a result is wrapped back to. `registerBits` is the
 * width the arithmetic *happens* at, which is not the same number for a type narrower than the
 * register holding it - and the gap between them is exactly what `truncateToWidth` in
 * resolve/lower.cpp reproduces on native and `coerce` in codegen/js/type.cpp on JS.
 */
struct IntFacts {
    U16 bits = 0;
    U16 registerBits = 0;
    bool isSigned = false;

    bool fillsRegister() const { return bits == registerBits; }
};

Maybe<IntFacts> foldableInt(OptContext& opt, TypePtr type);

// A value read back at its own width: sign-extended to 64 bits for a signed type, zero-extended for
// an unsigned one. This is the form every fold computes in and the form a folded constant is stored
// in - see makeConstant.
U64 narrowToWidth(U64 value, const IntFacts& facts);

// The constant one operand is, or nothing where it is not one. Answered at the operand's own type,
// so a caller never has to re-normalize what it got.
Maybe<U64> constantValueOf(OptContext& opt, ModulePtr<Value> value);

// A fresh integer constant of one type, belonging to the block the instruction it replaces did.
ModulePtr<Value> makeConstant(OptContext& opt, Value& at, TypePtr type, U64 value);

void foldFunction(OptContext& opt);
void forwardPlaces(OptContext& opt);
void eliminateCommonValues(OptContext& opt);
void eliminateDeadValues(OptContext& opt);
