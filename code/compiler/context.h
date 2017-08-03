#pragma once

#include "../util/map.h"
#include "../util/hash.h"
#include "diagnostics.h"

typedef U32 Id;

/*
 * An identifier consists of zero or more module names separated by dots, followed by the the identifier value.
 * In code, these are presented as a linked list of the identifier value followed by the module names.
 * Each segment is stored and represented by its hash for fast comparisons.
 * The lexer stores each found identifier as a mapping from the value hash to the value itself.
 * Module name segments are not stored in the table.
 */
struct Qualified {
    Qualified* qualifier = nullptr;
    const char* name;
    Size length = 0;
    Id hash = 0;
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

    Qualified& find(Id id) {
        return names[id];
    }

    Id addUnqualifiedName(const char* chars, Size count);
    Id addName(Qualified* q);

    Id addName(Id id, Qualified* q) {
        names.add(id, *q);
        return id;
    }

    Arena stringArena;

private:
    Byte* astBuffer = nullptr;
    Byte* astMax = nullptr;
    Array<Byte*> astBuffers;
    HashMap<Qualified, Id> names;
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
