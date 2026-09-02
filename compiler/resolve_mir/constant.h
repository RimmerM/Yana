#pragma once

/*
 * `contract.h` rather than `type.h`, for the reason that header states about itself: a constant
 * *names* a logical type and never asks it anything, so a forward declaration is the whole of the
 * dependency. Which way it runs matters here - `type.h` includes this one, because a record field's
 * declaration carries its default.
 */
#include "contract.h"

namespace mir {

/*
 * A compile-time value: a logical type and up to eight bytes.
 *
 * **A value, not an allocated node.** It used to be a four-kind hierarchy reached through a
 * `DeclPtr`, and the cost of that was not the indirection: a literal inside a function body is body
 * data, and allocating it in the declaration region meant every `7` in every function survived the
 * release of the MIR arena and stayed resident for the whole compilation. Sixteen bytes held
 * *where the constant is named* has no such lifetime - a body's table lives in the MIR region and
 * dies with it, and a global's initializer is a field of the global.
 *
 * Every kind fits in eight bytes and always will: an aggregate constant is a list of these plus a
 * layout, which is a table in the region that names it rather than a wider payload here.
 *
 * A constant has a logical type and no contract: it is owned by whoever installs it, and a constant
 * whose bytes are static is still an owned occurrence of its type. Analysis-IR.md §5.8 is the rule
 * this follows - a shared static producer is a property of the producer, not evidence that the value
 * is trivially copyable.
 */
struct Constant {
    enum Kind: U8 {
        // Not a constant. What a failed resolution answers, so that a caller checks one thing rather
        // than carrying a null handle beside a value.
        None,

        Unit,

        /*
         * Which constructor of an enum-layout record this is, as an **index** into `cons`.
         *
         * The index and not the number that is stored: `RecordCon::value` is what a tag holds and
         * `@value(n)` is what may move it, so a constant that carried the number would be stating a
         * representation the tier above the manifestation table is not entitled to know. A dump
         * prints the constructor's own name from that index, which is how `True` prints as `True`.
         *
         * It is the one construction that is a constant, and generally so rather than for `Bool`:
         * a constructor carrying no fields has nothing a site could vary, so `Off` is a compile-time
         * value wherever it is written - a field default, a short-circuit arm, an argument.
         */
        Enum,

        // The value as 64 unsigned bits, sign-extended from its declared width if it is signed. The
        // width is on the type; storing it again here would be a second answer to one question.
        Int,

        // The IEEE bit pattern of the `double`, whatever the declared width. Narrowing to `F32`
        // happens where the bytes are laid out, because that is the only place the target's answer
        // about the width is in hand.
        Float,
    };

    DeclPtr<Type> type = nullptr;
    Kind kind = None;
    U64 bits = 0;

    bool isSet() const { return kind != None; }

    U16 asConstructor() const { return U16(bits); }
    U64 asInt() const { return bits; }

    double asFloat() const {
        double value;
        copy((const Byte*)&bits, (Byte*)&value, 8);
        return value;
    }

    bool operator == (const Constant& other) const {
        return type == other.type && kind == other.kind && bits == other.bits;
    }
};

inline Constant unitConstant(DeclPtr<Type> type) { return Constant { type, Constant::Unit, 0 }; }

inline Constant enumConstant(DeclPtr<Type> type, U16 constructor) {
    return Constant { type, Constant::Enum, U64(constructor) };
}

inline Constant intConstant(DeclPtr<Type> type, U64 value) {
    return Constant { type, Constant::Int, value };
}

inline Constant floatConstant(DeclPtr<Type> type, double value) {
    U64 bits;
    copy((const Byte*)&value, (Byte*)&bits, 8);
    return Constant { type, Constant::Float, bits };
}

} // namespace mir
