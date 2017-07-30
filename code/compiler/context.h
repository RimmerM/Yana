#pragma once

#include "../util/map.h"
#include "../util/hash.h"

typedef U32 Id;

struct Qualified {
    Qualified* qualifier = nullptr;
    const char* name;
    Size length = 0;
};

enum class Assoc : U8 {
    Left,
    Right
};

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
