#include "lower_inst.h"

inline LowerInst* nop(LowerBase base, LowerModule& module, LowerBlock& block) {
    return block.addInst(base, new (module.arena) LowerInst(LowerInst::Nop));
}

template<LowerInst::Kind kind>
inline LowerInst* unary(LowerBase base, LowerModule& module, LowerBlock& block, LowerValue* arg, LowerType type, StringId name) {
    static_assert(kind >= LowerInst::FirstUnary && kind <= LowerInst::LastUnary);
    return block.addInst(base, new (module.arena) LowerInstUnary(kind, name, type, arg - base));
}

template<bool signedSource, bool signedResult>
inline LowerInst* cast(LowerBase base, LowerModule& module, LowerBlock& block, LowerValue* arg, LowerType type, StringId name) {
    return block.addInst(base, new (module.arena) LowerInstCast(name, type, arg - base, signedSource, signedResult));
}

template<LowerInst::Kind kind>
inline LowerInst* binary(LowerBase base, LowerModule& module, LowerBlock& block, LowerValue* lhs, LowerValue* rhs, LowerType type, StringId name) {
    static_assert(kind >= LowerInst::FirstBinary && kind <= LowerInst::LastBinary);
    return block.addInst(base, new (module.arena) LowerInstBinary(name, type, lhs - base, rhs - base, kind));
}

inline LowerInst* cmp(LowerBase base, LowerModule& module, LowerBlock& block, LowerValue* lhs, LowerValue* rhs, LowerCmp c, StringId name) {
    return block.addInst(base, new (module.arena) LowerInstCmp(name, lhs - base, rhs - base, c));
}

inline LowerInst* load(LowerBase base, LowerModule& module, LowerBlock& block, LowerValue* from, U32 width, bool signExtend, LowerType type, StringId name) {
    return block.addInst(base, new (module.arena) LowerInstLoad(from - base, name, type, width, signExtend));
}

template<class Prepare>
inline LowerInst* call(LowerBase base, LowerModule& module, LowerBlock& block, Size createdCount, Size usedCount, LowerCallType callType, Prepare&& prepare) {
    auto embeddedSize = sizeof(LowerValue) * createdCount + sizeof(LowerPtr<LowerValue>) * usedCount;
    auto inst = (LowerInstCall*)module.arena.alloc(sizeof(LowerInstCall) + embeddedSize);
    new (inst) LowerInstCall(createdCount, usedCount, callType);

    prepare(inst);
    return block.addInst(base, inst);
}

inline LowerInst* je(LowerBase base, LowerModule& module, LowerBlock& block, LowerValue* cond, LowerBlock* then, LowerBlock* otherwise) {
    return block.addInst(base, new (module.arena) LowerInstJe(cond - base, then - base, otherwise - base));
}

inline LowerInst* jmp(LowerBase base, LowerModule& module, LowerBlock& block, LowerBlock* to) {
    return block.addInst(base, new (module.arena) LowerInstJmp(to - base));
}

template<class Prepare>
inline LowerInst* ret(LowerBase base, LowerModule& module, LowerBlock& block, Size createdCount, Prepare&& prepare) {
    auto inst = (LowerInstRet*)module.arena.alloc(sizeof(LowerInstRet) + sizeof(LowerPtr<LowerValue>) * createdCount);
    new (inst) LowerInstRet;

    prepare(inst);
    return block.addInst(base, new (module.arena) LowerInstRet());
}
