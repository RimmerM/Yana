#pragma once

#include "../util/map.h"
#include "../util/hash.h"
#include "diagnostics.h"

/*
 * An identifier consists of zero or more module names separated by dots, followed by the the identifier value.
 * The lexer reads these and stores them in a map, after which each identifier is represented by a hash.
 * We store multiple hashes in each identifier, in order to support these name lookups in (mostly) constant time:
 *  - Module name (M1.M2).
 *  - Single VarID or ConID that maps to a defined symbol.
 *  - Module-qualified symbol (M1.M2.symbol).
 *  - Instance function (Type.symbol).
 *  - Qualified instance function (M1.M2.Type.symbol).
 */
struct Identifier {
    const char* content;
    U32 length = 0;
    U32 hashN = 0;   // The hash of the last segment in the identifier.
    U32 hashN1 = 0;  // The hash of the next-to-last segment in the identifier.
    U32 hash0N1 = 0; // The hash of each segment in the identifier, excluding the last one.
    U32 hash0N2 = 0; // The hash of each segment in the identifier, excluding the last two ones.
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

struct CompileSettings {

};

struct Arena {
    static const Size kChunkSize = 1024 * 1024;

    void* alloc(Size size);
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

    void addOp(Id op, U16 prec = 9, Assoc assoc = Assoc::Left) {
        OpProperties prop{prec, assoc};
        ops[op] = prop;
    }

    OpProperties findOp(Id op) {
        auto res = ops.get(op);
        if(res) {
            return *res;
        } else {
            return {9, Assoc::Left};
        }
    }

    Identifier& find(Id id) {
        return identifiers[id];
    }

    Id addUnqualifiedName(const char* chars, Size count);
    Id addName(Identifier* q);

    Id addName(Id id, Identifier* q) {
        identifiers.add(id, *q);
        return id;
    }

    Arena stringArena;

private:
    Byte* astBuffer = nullptr;
    Byte* astMax = nullptr;
    Array<Byte*> astBuffers;
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

template<class T>
struct List {
    List<T>* next = nullptr;
    T item;

    List() {}
    List(const T& i) : item(i) {}
    List(const T& i, List<T>* n) : item(i), next(n) {}
};
