#include "module.h"

/*
 * The instruction table - inst.def, read as data.
 *
 * The two static assertions below are the whole of what keeps a row honest, and they are the reason
 * the buffers everything else passes around can be fixed-size arrays: a kind that named three places
 * would overrun `kMaxPlaces` at every one of the dozen call sites rather than fail here.
 */
const InstructionTraits kInstructionTraits[] = {
#define YANA_INST(kind, Struct, mnemonic, flags) InstructionTraits { mnemonic, flags },
#include "inst.def"
#undef YANA_INST
};

static_assert(sizeof(kInstructionTraits) / sizeof(InstructionTraits) == Value::kKindCount,
              "every Value::Kind has exactly one row in inst.def");

#define YANA_INST(kind, Struct, mnemonic, flags) \
    static_assert(Struct::kPlaceCount <= kMaxPlaces, "an instruction names at most kMaxPlaces places"); \
    static_assert(Struct::kSuccessorCount <= kMaxSuccessors, "a terminator has at most kMaxSuccessors arms");
#include "inst.def"
#undef YANA_INST

StringView conventionName(ast::BindType convention) {
    switch(convention) {
        case ast::BindType::Ref: return "`&`"_v;
        case ast::BindType::Sink: return "`->`"_v;
        default: return "an immutable borrow"_v;
    }
}

StringView funValueFieldName(U16 field) {
    switch(field) {
        case FunValueLayout::kCode: return "code"_v;
        case FunValueLayout::kHeader: return "header"_v;
        default: return "env"_v;
    }
}
