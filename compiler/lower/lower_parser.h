#pragma once

#include "lower_lexer.h"
#include "lower.h"
#include "../util/parser_util.h"

struct LowerInstAst;

struct LowerArgAst {
    enum Kind: U8 {
        Imm,
        Reg,
        Label,
        Global,
        Inst,
    };

    union {
        U64 i;
        F64 f;
        StringId id;
        LowerInstAst* inst;
    };

    StringId source = 0;
    Kind kind;
    LowerType immType;

    bool isInt() const {
        return kind == Imm && (immType == LowerType::Int32 || immType == LowerType::Int64);
    }

    bool isFloat() const {
        return kind == Imm && (immType == LowerType::Float32 || immType == LowerType::Float64);
    }
};

// Encodes a name id in the lower 32 bits, and an optional type in the top 32.
using LowerResultAst = U64;

inline U64 encodeResultAst(StringId name, Maybe<LowerType> type) {
    auto v = U64(name);
    if(type) {
        v |= (U64(type.unwrap()) << 32);
        v |= (U64(1) << 48);
    }

    return v;
}

inline StringId getResultName(U64 result) {
    return result & 0xffffffff;
}

inline Maybe<LowerType> getResultType(U64 result) {
    auto type = result >> 32;
    if((type >> 16) & 1) {
        return Just(LowerType(type & 0xffff));
    } else {
        return Nothing();
    }
}

struct LowerInstAst {
    explicit LowerInstAst(StringId inst): inst(inst) {}

    StringId inst;
    LocationId source = kNullLocation;
    EmbedList<LowerResultAst> results;
    EmbedList<LowerArgAst, false> args;
    LowerInst* gen = nullptr;
};

struct LowerBlockAst {
    // We need to store pointers rather than the actual instructions,
    // because the ast pointers can be stored outside this list and thus need to be stable.
    SmallList<LowerParserRegion, RegionPtr<LowerParserRegion, LowerInstAst>, false> instructions;
};

struct LowerParser: BasicParser<LowerLexer, LowerToken> {
    LowerParser(Context& context, LowerModule& module, LowerLexer& lexer);

    bool parseModule();
    bool resolveModule();
    void parseGlobal();
    void parseDecl();
    void parseBlock(LowerFunction* fun);
    LowerInstAst* parseInst(LowerBlockAst& to, bool implicitResult);
    void parseArg(LowerInstAst* ast, LowerBlockAst& to);
    LowerArgAst parseBaseArg();
    LowerArgAst parseNumericArg();
    LowerType parseType();

    template<class F> auto parens(F&& f) {
        return between(f, LowerToken::Type::ParenL, LowerToken::Type::ParenR, "expected '('"_v, "expected ')'"_v);
    }

    template<class F> auto braces(F&& f) {
        return between(f, LowerToken::Type::BraceL, LowerToken::Type::BraceR, "expected '{'"_v, "expected '}'"_v);
    }

    template<class F> auto brackets(F&& f) {
        return between(f, LowerToken::Type::BracketL, LowerToken::Type::BracketR, "expected '['"_v, "expected ']'"_v);
    }

    Context& context;
    LowerModule& module;
    Region<LowerParserRegion> buffer;

    StringId i32Id;
    StringId i64Id;
    StringId ptrId;
    StringId f32Id;
    StringId f64Id;
};
