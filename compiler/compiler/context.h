#pragma once

#include "diagnostics.h"
#include "settings.h"
#include <HashMap.h>

/*
 * An identifier consists of zero or more module names separated by dots, followed by the the identifier value.
 * We store the full identifier, as well as a pointer and hash to the start of each segment.
 */
struct Identifier {
    Identifier(): textLength(0), segmentCount(0) {}

    U32 getHash(U32 index) const {
        if(segmentCount == 1) {
            return segmentHash;
        } else {
            return segmentHashes[index];
        }
    }

    U32 getSegmentOffset(U32 index) const {
        if(segmentCount == 0) {
            return 0;
        } else if(index == segmentCount) {
            return textLength;
        } else {
            return segments[index];
        }
    }

    const char* text;

    // If `segmentCount == 1`, this is set to nullptr.
    U32* segments;

    // If `segmentCount == 1`, this stores the first hash instead of a pointer.
    union {
        U32* segmentHashes;
        U32 segmentHash;
    };

    U32 textLength: 24;
    U32 segmentCount: 8;
};

enum class Assoc : U8 {
    Left,
    Right
};

/*
 * Operators can have an associated precedence and a associativity.
 * This is used for the reordering of infix-expressions.
 * TODO: What should happen if two modules define a different precedence for the same operator?
 * Maybe we should not allow custom precedences for user-defined operators.
 */
struct OpProperties {
    U16 precedence;
    Assoc associativity;
};

struct Arena {
    static const Size kChunkSize = 1024 * 1024;

    void* alloc(Size size);
    void reset();
    ~Arena();

private:
    Byte* buffer = nullptr;
    Byte* max = nullptr;
    Array<Byte*> buffers;
};

inline void* operator new (Size count, Arena& arena) {return arena.alloc(count);}

struct Context {
    Context(Diagnostics& diagnostics): diagnostics(diagnostics) {}

    Diagnostics& diagnostics;
    CompileSettings settings;

    void addOp(Id op, U16 prec = 9, Assoc assoc = Assoc::Left);
    OpProperties findOp(Id op);

    static Id nameHash(const char* chars, Size count);

    Id addUnqualifiedName(const char* chars, Size count);
    Id addQualifiedName(const char* chars, Size count, Size segmentCount);
    Id addQualifiedName(const char* chars, Size count);
    Id addIdentifier(const Identifier& q);
    Identifier& find(Id id);
    String findName(Id id);

    Arena stringArena;
    Arena exprArena;

private:
    HashMap<Identifier, Id> identifiers;
    HashMap<OpProperties, Id> ops;
};

struct ArenaAllocator {
    ArenaAllocator(Arena& arena): arena(arena) {}
    Arena& arena;

    void* alloc(Size size) {
        return arena.alloc(size);
    }

    void free(void*) {}
};

template<class T>
using ASTArray = ArrayT<T, ArrayAllocator<T, ArenaAllocator>>;
