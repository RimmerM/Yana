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

struct Context {
    static const Size kChunkSize = 1024 * 1024;

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

    Id addUnqualifiedName(const char* chars, Size count) {
        Qualified q;
        q.name = chars;
        q.length = count;
        return addName(&q);
    }

    Id addName(Qualified* q) {
        Hasher h;

        auto qu = q;
        while(qu) {
            h.addData(qu->name, (qu->length * sizeof(*qu->name)));
            qu = qu->qualifier;
        }

        return addName(h.get(), q);
    }

    Id addName(Id id, Qualified* q) {
        names.add(id, *q);
        return id;
    }

    void* astAlloc(Size size) {
        if(astBuffer + size > astMax) {
            astBuffer = (Byte*)malloc(kChunkSize);
            astMax = astBuffer + kChunkSize;
            astBuffers.push(astBuffer);
        }

        auto it = astBuffer;
        astBuffer += size;
        return it;
    }

    template<class T, class... P>
    T* astNew(P&&... p) {
        auto obj = (T*)astAlloc(sizeof(T));
        new (obj) T(p...);
        return obj;
    }

    void releaseAst() {
        for(auto buffer: astBuffers) {
            free(buffer);
        }
        astBuffers.destroy();
        astBuffer = nullptr;
        astMax = nullptr;
    }

private:
    Byte* astBuffer = nullptr;
    Byte* astMax = nullptr;
    Array<Byte*> astBuffers;
    HashMap<Qualified, Id> names;
    HashMap<OpProperties, Id> ops;
};

struct ASTAllocator {
    ASTAllocator(Context& context): context(context) {}
    Context& context;

    void* alloc(Size size) {
        return context.astAlloc(size);
    }

    void free(void*) {}
};

template<class T>
using ASTArray = ArrayT<T, ArrayAllocator<T, ASTAllocator>>;

template<class T>
struct List {
    List<T>* next = nullptr;
    T item;

    List() {}
    List(const T& i) : item(i) {}
    List(const T& i, List<T>* n) : item(i), next(n) {}
};
