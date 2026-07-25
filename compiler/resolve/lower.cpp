#include "../lower/lower_builder.h"
#include "inst.h"
#include "module.h"

struct LoweredValue {
    SmallList<ModuleRegion, LowerPtr<LowerValue>, true> results;
};

static_assert(sizeof(LoweredValue) == sizeof(void*));

inline LoweredValue* getLowered(Value* v) {
    assertTrue(v->codegen != nullptr);
    return (LoweredValue*)&v->codegen;
}

inline LowerValue* getSingleLowered(ModuleBase base, LowerBase lb, Value* v) {
    auto l = getLowered(v);
    assertTrue(l->results.size() == 1);
    return lb[l->results.get(base, 0)];
}

inline LoweredValue wrapSingle(LowerBase base, Module& to, LowerInst* v) {
    assertTrue(v->createdCount == 1);

    LoweredValue result;
    result.results.push(to.memory, v->created().ptr - base);
    return result;
}

inline void storeResult(Value* v, LoweredValue lowered) {
    v->codegen = *(void**)&lowered;
}

StringId mangleName(Module& m, Function& f) {
    return f.name;
}

static LoweredValue lowerCast(LowerBase base, ModuleBase m, Module& from, LowerModule& to, LowerBlock& t, InstCast& i) {
    // TODO: target type.
    auto source = getSingleLowered(m, base, m[i.from]);
    return wrapSingle(base, from, cast<false, false>(base, to, t, source, LowerType::Int32, i.name));
}

template<LowerInst::Kind kind>
static LoweredValue lowerBinary(LowerBase base, ModuleBase m, Module& from, LowerModule& to, LowerBlock& t, InstBinary& i) {
    auto lhs = getSingleLowered(m, base, m[i.lhs]);
    auto rhs = getSingleLowered(m, base, m[i.rhs]);
    assertTrue(lhs->type == rhs->type);

    return wrapSingle(base, from, binary<kind>(base, to, t, lhs, rhs, lhs->type, i.name));
}

template<LowerInst::Kind kind>
static LoweredValue lowerShift(LowerBase base, ModuleBase m, Module& from, LowerModule& to, LowerBlock& t, InstShift& i) {
    auto arg = getSingleLowered(m, base, m[i.arg]);
    auto amount = getSingleLowered(m, base, m[i.amount]);

    return wrapSingle(base, from, binary<kind>(base, to, t, arg, amount, arg->type, i.name));
}

static LowerCmp convertCmp(ICmp cmp) {
    switch(cmp) {
        case ICmp::eq:
            return LowerCmp::eq;
        case ICmp::neq:
            return LowerCmp::neq;
        case ICmp::gt:
            return LowerCmp::gt;
        case ICmp::ge:
            return LowerCmp::ge;
        case ICmp::lt:
            return LowerCmp::lt;
        case ICmp::le:
            return LowerCmp::le;
        case ICmp::igt:
            return LowerCmp::igt;
        case ICmp::ige:
            return LowerCmp::ige;
        case ICmp::ilt:
            return LowerCmp::ilt;
        case ICmp::ile:
            return LowerCmp::ile;
    }

    return LowerCmp::eq;
}

static LowerCmp convertCmp(FCmp cmp) {
    switch(cmp) {
        case FCmp::eq:
            return LowerCmp::eq;
        case FCmp::neq:
            return LowerCmp::neq;
        case FCmp::gt:
            return LowerCmp::gt;
        case FCmp::ge:
            return LowerCmp::ge;
        case FCmp::lt:
            return LowerCmp::lt;
        case FCmp::le:
            return LowerCmp::le;
    }

    return LowerCmp::eq;
}

static LoweredValue lowerCmp(LowerBase base, Module& from, LowerModule& to, LowerBlock& t, InstICmp& i) {
    auto lhs = (LowerValue*)from.local[i.lhs]->codegen;
    auto rhs = (LowerValue*)from.local[i.rhs]->codegen;
    assertTrue(lhs && rhs && lhs->type == rhs->type);

    return wrapSingle(base, from, cmp(base, to, t, lhs, rhs, convertCmp(i.cmp), i.name));
}

static LoweredValue lowerCmp(LowerBase base, Module& from, LowerModule& to, LowerBlock& t, InstFCmp& i) {
    auto lhs = (LowerValue*)from.local[i.lhs]->codegen;
    auto rhs = (LowerValue*)from.local[i.rhs]->codegen;
    assertTrue(lhs && rhs && lhs->type == rhs->type);

    return wrapSingle(base, from, cmp(base, to, t, lhs, rhs, convertCmp(i.cmp), i.name));
}

static LowerInst* lowerJmp(LowerBase base, Module& from, LowerModule& to, LowerBlock& t, InstJmp& i) {
    auto then = (LowerBlock*)from.local[i.to]->codegen;
    assertTrue(then != nullptr && then->fun == t.fun);

    return jmp(base, to, t, then);
}

static LowerInst* lowerJe(LowerBase base, Module& from, LowerModule& to, LowerBlock& t, InstJe& i) {
    auto cond = (LowerValue*)from.local[i.cond]->codegen;
    auto then = (LowerBlock*)from.local[i.then]->codegen;
    auto otherwise = (LowerBlock*)from.local[i.otherwise]->codegen;
    assertTrue(cond && then && otherwise && then->fun == t.fun && otherwise->fun == t.fun);

    return je(base, to, t, cond, then, otherwise);
}

LoweredValue lowerInst(LowerBase base, ModuleBase m, Module& from, LowerModule& to, Block& b, LowerBlock& t, Inst& i) {
    switch(i.kind) {
        case Inst::InstNop:
            nop(base, to, t);
            return {};

        case Inst::InstTrunc:
        case Inst::InstFTrunc:
        case Inst::InstZExt:
        case Inst::InstSExt:
        case Inst::InstFExt:
        case Inst::InstFToI:
        case Inst::InstFToUI:
        case Inst::InstIToF:
        case Inst::InstUIToF:
            return lowerCast(base, m, from, to, t, (InstCast&)i);

        case Inst::InstAdd:
            return lowerBinary<LowerInst::Add>(base, m, from, to, t, (InstBinary&)i);
        case Inst::InstSub:
            return lowerBinary<LowerInst::Sub>(base, m, from, to, t, (InstBinary&)i);
        case Inst::InstMul:
            return lowerBinary<LowerInst::IMul>(base, m, from, to, t, (InstBinary&)i);
        case Inst::InstDiv:
            return lowerBinary<LowerInst::Div>(base, m, from, to, t, (InstBinary&)i);
        case Inst::InstIDiv:
            return lowerBinary<LowerInst::IDiv>(base, m, from, to, t, (InstBinary&)i);
        case Inst::InstRem:
            return lowerBinary<LowerInst::Rem>(base, m, from, to, t, (InstBinary&)i);
        case Inst::InstIRem:
            return lowerBinary<LowerInst::IRem>(base, m, from, to, t, (InstBinary&)i);
        case Inst::InstFAdd:
            return lowerBinary<LowerInst::Add>(base, m, from, to, t, (InstBinary&)i);
        case Inst::InstFSub:
            return lowerBinary<LowerInst::Sub>(base, m, from, to, t, (InstBinary&)i);
        case Inst::InstFMul:
            return lowerBinary<LowerInst::Mul>(base, m, from, to, t, (InstBinary&)i);
        case Inst::InstFDiv:
            return lowerBinary<LowerInst::Div>(base, m, from, to, t, (InstBinary&)i);

        case Inst::InstICmp:
            return lowerCmp(base, from, to, t, (InstICmp&)i);
        case Inst::InstFCmp:
            return lowerCmp(base, from, to, t, (InstFCmp&)i);

        case Inst::InstShl:
            return lowerShift<LowerInst::Shl>(base, m, from, to, t, (InstShift&)i);
        case Inst::InstShr:
            return lowerShift<LowerInst::Shr>(base, m, from, to, t, (InstShift&)i);
        case Inst::InstSar:
            return lowerShift<LowerInst::Sar>(base, m, from, to, t, (InstShift&)i);
        case Inst::InstAnd:
            return lowerBinary<LowerInst::And>(base, m, from, to, t, (InstBinary&)i);
        case Inst::InstOr:
            return lowerBinary<LowerInst::Or>(base, m, from, to, t, (InstBinary&)i);
        case Inst::InstXor:
            return lowerBinary<LowerInst::Xor>(base, m, from, to, t, (InstBinary&)i);


        case Inst::InstAddPtr:
            name = "addptr"_v;
            break;
        case Inst::InstRecord:
            name = "record"_v;
            break;
        case Inst::InstTup:
            name = "tup"_v;
            break;
        case Inst::InstFun:
            name = "fun"_v;
            break;
        case Inst::InstAlloc:
            name = "alloc"_v;
            break;
        case Inst::InstAllocArray:
            name = "allocarray"_v;
            break;
        case Inst::InstLoad:
            name = "load"_v;
            break;
        case Inst::InstLoadField:
            name = "loadfield"_v;
            break;
        case Inst::InstLoadArray:
            name = "loadarray"_v;
            break;
        case Inst::InstStore:
            name = "store"_v;
            break;
        case Inst::InstStoreField:
            name = "storefield"_v;
            break;
        case Inst::InstStoreArray:
            name = "storearray"_v;
            break;
        case Inst::InstGetField:
            name = "getfield"_v;
            break;
        case Inst::InstUpdateField:
            name = "updatefield"_v;
            break;
        case Inst::InstArrayLength:
            name = "arraylength"_v;
            break;
        case Inst::InstArrayCopy:
            name = "arraycopy"_v;
            break;
        case Inst::InstArraySlice:
            name = "arrayslice"_v;
            break;
        case Inst::InstStringLength:
            name = "stringlength"_v;
            break;
        case Inst::InstStringData:
            name = "stringdata"_v;
            break;
        case Inst::InstCall:
            name = "call"_v;
            break;
        case Inst::InstCallDyn:
            name = "call dyn"_v;
            break;
        case Inst::InstCallForeign:
            name = "call foreign"_v;
            break;



        case Inst::InstJe:
            lowerJe(base, from, to, t, (InstJe&)i);
            return {};
        case Inst::InstJmp:
            lowerJmp(base, from, to, t, (InstJmp&)i);
            return {};



        case Inst::InstRet:
            name = "ret"_v;
            break;
        case Inst::InstPhi:
            name = "phi"_v;
            break;
    }
}

void lowerBlock(Module& from, LowerModule& to, Block& b, LowerBlock& t) {
    assertTrue(b.isComplete());
    auto base = *to.arena;
    auto m = from.local;

    for(auto i: b.phis.contents(from.local)) {
        auto p = from.local[i];
        storeResult(p, lowerInst(base, m, from, to, b, t, *p));
    }

    for(auto i: b.instructions.contents(from.local)) {
        auto p = from.local[i];
        storeResult(p, lowerInst(base, m, from, to, b, t, *p));
    }

    lowerInst(base, m, from, to, b, t, *from.local[b.terminator]);
}

LowerFunction* lowerFunction(Module& from, LowerModule& to, Function& f) {
    auto fromBase = from.local;
    auto toBase = *to.arena;
    auto lowered = to.addFunction(mangleName(from, f));
    f.codegen = lowered;

    for(auto a: f.args.contents(from.local)) {
        auto arg = fromBase[a];
        auto loweredArg = lowered->addArg(toBase, arg->name, LowerType::Pointer);
        arg->codegen = loweredArg;
    }

    // First, add all lowered blocks and make them accessible from the IR ones.
    for(auto b: f.blocks.contents(from.local)) {
        auto t = lowered->addBlock(toBase, 0);
        fromBase[b]->codegen = t;
    }

    // Then, lower the contents of each block, knowing that any other blocks it references already exist.
    for(auto b: f.blocks.contents(from.local)) {
        auto block = fromBase[b];
        lowerBlock(from, to, *block, *(LowerBlock*)block->codegen);
    }

    return lowered;
}

LowerModule lowerModule(Module& from) {
    LowerModule to(from.memory.used() * 2);

    for(auto& f: from.functions) {
        lowerFunction(from, to, f);
    }

    return ::move(to);
}
