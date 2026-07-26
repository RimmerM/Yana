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

inline bool operator == (const Identifier& a, const Identifier& b) {
    if(a.textLength != b.textLength) return false;
    return Tritium::compareMem(a.text, b.text, a.textLength);
}

namespace Tritium {
    inline U32 getHash(const Identifier& identifier) {
        return getHash(StringView { identifier.text, identifier.textLength });
    }
}

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
    static const Size kChunkSize = 4 * 1024 * 1024;

    Arena() = default;
    Arena(const Arena&) = delete;

    void* alloc(Size size);
    void reset();
    ~Arena();

private:
    Byte* buffer = nullptr;
    Byte* max = nullptr;
    Array<Byte*> buffers;
};

inline void* operator new (Size count, Arena& arena) {
    return arena.alloc(count);
}

struct LinearArena;

template<class Region, class T>
struct RegionPtr {
    U32 offset;

    // Offset 0 is never a real object - every region starts allocating past it - so a
    // default-constructed handle is the null handle.
    RegionPtr(): offset(0) {}
    explicit RegionPtr(U32 offset): offset(offset) {}
    RegionPtr(decltype(nullptr)): offset(0) {}
    RegionPtr(const RegionPtr&) = default;

    RegionPtr& operator = (const RegionPtr& p) = default;

    bool operator == (RegionPtr p) const {
        return offset == p.offset;
    }

    bool operator != (RegionPtr p) const {
        return offset != p.offset;
    }

    bool operator == (decltype(nullptr)) const {
        return offset == 0;
    }

    bool operator != (decltype(nullptr)) const {
        return offset != 0;
    }

    operator U32() const {
        return offset;
    }
};

template<class Region>
struct RegionBase {
    // The stored base is slightly smaller than the actual base.
    // This allows us to handle null pointers without any special handling.
    Byte* base;

    explicit RegionBase(Byte* base): base(base - 16) {}
    RegionBase(const RegionBase&) = default;

    RegionBase& operator = (const RegionBase& p) {
        base = p.base;
        return *this;
    }

    template<class T>
    T* operator[](RegionPtr<Region, T> p) {
        return (T*)(base + p.offset);
    }
};

template<class Region, class T>
inline RegionPtr<Region, T> operator - (T* v, RegionBase<Region> base) {
    assertTrue(((Byte*)v) - 16 >= base.base);
    return RegionPtr<Region, T>(U32((Byte*)v - base.base));
}

struct LinearArena {
    explicit LinearArena(Size maxSize);
    LinearArena(LinearArena&&) noexcept;
    LinearArena(const LinearArena&) = delete;

    void* alloc(Size size);
    void reset(Size maxSize);
    Size used() { return p - base; }

    ~LinearArena();

protected:
    Byte* base = nullptr;
    Byte* p = nullptr;
    Byte* max = nullptr;
};

template<class T>
struct Region: LinearArena {
    using LinearArena::LinearArena;

    RegionBase<T> operator * () const {
        return RegionBase<T>(base);
    }
};

inline void* operator new (Size count, LinearArena& arena) {
    return arena.alloc(count);
}

struct Context {
    Context(Diagnostics& diagnostics): diagnostics(diagnostics) {}

    Diagnostics& diagnostics;
    CompileSettings settings;

    void addOp(StringId op, U16 prec = 9, Assoc assoc = Assoc::Left);
    OpProperties findOp(StringId op);

    static StringId nameHash(const char* chars, Size count);
    static StringId nameHash(const StringView& v);

    StringId addUnqualifiedName(const char* chars, Size count);
    StringId addQualifiedName(const char* chars, Size count, Size segmentCount);
    StringId addQualifiedName(const char* chars, Size count);
    StringId addIdentifier(const Identifier& q);

    Identifier& find(StringId id);
    String findName(StringId id);

    // The index has to be taken before pushing: a push that grows the array frees the old
    // buffer, and since the order the operands of `-` are evaluated in is unspecified, deriving
    // the index from `push(node) - begin()` can subtract the two buffers from each other.
    LocationId addLocation(const Location& node) {
        auto id = LocationId(locations.size());
        locations.push(node);
        return id;
    }

    LocationId addLocation(LocationId l) {
        return l;
    }

    Location* prepareLocation(LocationId& target) {
        target = LocationId(locations.size());
        return &locations.push();
    }

    const Location* getLocation(LocationId id) {
        return locations.size() > id ? &locations[id] : nullptr;
    }

    Arena stringArena;
    Arena exprArena;

private:
    HashMap<StringId, Identifier> identifiers;
    HashMap<StringId, OpProperties> ops;
    Array<Location> locations;
};

template<class Arena>
struct ArenaAllocator {
    ArenaAllocator(Arena& arena): arena(arena) {}
    Arena& arena;

    void* alloc(Size size) {
        return arena.alloc(size);
    }

    void free(void*) {}
};

template<class T>
using ArenaArray = ArrayT<T, ArrayAllocator<T, ArenaAllocator<Arena>>>;

template<class T>
using LinearArenaArray = ArrayT<T, ArrayAllocator<T, ArenaAllocator<LinearArena>>>;
