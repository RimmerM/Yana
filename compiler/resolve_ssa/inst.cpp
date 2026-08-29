#include "inst.h"

namespace ssa {

StringView instMnemonic(Inst::Kind kind) {
    switch(kind) {
#define YANA_SSA_INST(k, Struct, mnemonic, flags) case Inst::k: return mnemonic;
#include "inst.def"
#undef YANA_SSA_INST
        case Inst::KindCount: break;
    }

    return "?"_v;
}

U16 instFlags(Inst::Kind kind) {
    switch(kind) {
#define YANA_SSA_INST(k, Struct, mnemonic, flags) case Inst::k: return flags;
#include "inst.def"
#undef YANA_SSA_INST
        case Inst::KindCount: break;
    }

    return 0;
}

StringView conventionName(Convention convention) {
    switch(convention) {
        case Convention::Owned:      return "owned"_v;
        case Convention::Guaranteed: return "guaranteed"_v;
        case Convention::Trivial:    return "trivial"_v;
        case Convention::Address:    return "address"_v;
    }

    return "?"_v;
}

StringView useKindName(UseKind kind) {
    switch(kind) {
        case UseKind::Consume: return "consume"_v;
        case UseKind::Borrow:  return "borrow"_v;
        case UseKind::Inspect: return "inspect"_v;
    }

    return "?"_v;
}

} // namespace ssa
