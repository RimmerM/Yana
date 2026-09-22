#include "module.h"

/*
 * The three tables `inst.def` generates - the mnemonic, the effects and the result form.
 *
 * Out of line rather than `constexpr` arrays in the header, for the reason the two tiers above do
 * the same: an array in a header is one copy per translation unit, and the only thing anybody reads
 * it through is a function call anyway.
 */

namespace jsir {

StringView jsInstMnemonic(JsInst::Kind kind) {
    static const StringView table[] = {
#define YANA_JSIR_INST(kind, Struct, mnemonic, result, effects) mnemonic,
#include "inst.def"
#undef YANA_JSIR_INST
    };

    return kind < JsInst::KindCount ? table[kind] : "?"_v;
}

U16 jsInstEffects(JsInst::Kind kind) {
    static const U16 table[] = {
#define YANA_JSIR_INST(kind, Struct, mnemonic, result, effects) effects,
#include "inst.def"
#undef YANA_JSIR_INST
    };

    return kind < JsInst::KindCount ? table[kind] : 0;
}

U8 jsInstResult(JsInst::Kind kind) {
    static const U8 table[] = {
#define YANA_JSIR_INST(kind, Struct, mnemonic, result, effects) result,
#include "inst.def"
#undef YANA_JSIR_INST
    };

    return kind < JsInst::KindCount ? table[kind] : kNoResult;
}

StringView jsHelperOpName(JsHelperOp op) {
    switch(op) {
        case JsHelperOp::FloatToBits:   return "float.bits"_v;
        case JsHelperOp::BitsToFloat:   return "bits.float"_v;
        case JsHelperOp::ByteSwap:      return "byteswap"_v;
        case JsHelperOp::CountBits:     return "countbits"_v;
        case JsHelperOp::LeadingZeros:  return "leadingzeros"_v;
        case JsHelperOp::TrailingZeros: return "trailingzeros"_v;
        case JsHelperOp::Rol:           return "rol"_v;
        case JsHelperOp::Ror:           return "ror"_v;
        case JsHelperOp::BitsUpTo:      return "bitsupto"_v;
        case JsHelperOp::GatherBits:    return "gatherbits"_v;
        case JsHelperOp::ScatterBits:   return "scatterbits"_v;
        case JsHelperOp::FunDrop:       return "fun.drop"_v;
    }

    return "?"_v;
}

StringView jsMathOpName(JsMathOp op) {
    switch(op) {
        case JsMathOp::Min: return "min"_v;
        case JsMathOp::Max: return "max"_v;
    }

    return "?"_v;
}

U16 effectsOf(const JsInst& inst) {
    /*
     * A helper's effects are its **tag's**, on `Host`'s terms below.
     *
     * Eight of the twelve compute a number out of a number and are pure. The float pair is not: it
     * writes one typed-array buffer the whole program shares, so two of them may not be reordered
     * against each other - which is the same reason the shipping backend's `bitcastLane` is
     * deliberately not marked `pure`. `FunDrop` runs an authored teardown, so it is a call.
     */
    if(inst.kind == JsInst::Helper) {
        switch(((const JsInstHelper&)inst).op) {
            case JsHelperOp::FloatToBits:
            case JsHelperOp::BitsToFloat:
                return kJsReads | kJsWrites | kJsCalls;
            case JsHelperOp::FunDrop:
                return kJsCalls | kJsThrows | kJsWrites;
            default:
                return kJsPure;
        }
    }

    if(inst.kind != JsInst::Host) return jsInstEffects(inst.kind);

    /*
     * The one kind whose effects are a row rather than a column - `resolve_mir/host.def`.
     *
     * A property read reads, a method call may do anything, and `hostFixedCapacity` answers a
     * question about a type and does nothing at all. The flags column cannot say all three, so the
     * kind says the conservative one and this says the row's - which is exactly the shape
     * `ssa::isPure(const Inst&)` has one tier up.
     */
    auto op = ((const JsInstHost&)inst).op;
    auto row = mir::hostOpEffects(op);

    U16 effects = 0;
    if(row & mir::kHostPure) effects |= kJsPure;
    if(row & mir::kHostReads) effects |= kJsReads;
    if(row & mir::kHostWrites) effects |= kJsWrites | kJsCalls;
    if(row & mir::kHostThrows) effects |= kJsThrows;
    if(row & mir::kHostControl) effects |= kJsControl;

    return effects;
}

} // namespace jsir
