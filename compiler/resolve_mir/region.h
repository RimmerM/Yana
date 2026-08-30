#pragma once

#include "../compiler/context.h"
#include "../util/container.h"

/*
 * The three arenas the rewritten resolve stage stores everything in - Analysis-IR.md §8.1.
 *
 * All three tags are declared here, in the front half, rather than one per directory. A tag is an
 * empty type with no dependencies, and the reason they cannot be split is `mir::Function`: a function
 * lives in the declaration region and names a body in each of the other two, so the declaration
 * header would have to name `SsaRegion` from inside resolve_mir whatever this file did. One list of
 * three is honest about that; three headers that include each other would not be.
 *
 *  - `DeclRegion` lives for the whole compilation. Modules, functions, signatures, globals, logical
 *    types, ownership schemas and published summaries. Everything a *call* names is here, so nothing
 *    below ever holds a body pointer across a phase boundary.
 *  - `MirRegion` lives from resolution until SSA conversion, and is then released whole. MIR
 *    bodies: nodes, blocks, places, objects, loans, move paths.
 *  - `SsaRegion` lives from conversion until lowering. SSA bodies.
 *
 * Every handle stays a four-byte offset, and the tags are distinct C++ types so that a handle into
 * one region cannot be dereferenced against another. §8.8 is the rule that makes the middle arena
 * disposable at all: an SSA node may not point back into MIR, for provenance or for printing.
 */

struct DeclRegion {};
struct MirRegion {};
struct SsaRegion {};

template<class T> using DeclPtr = RegionPtr<DeclRegion, T>;
template<class T> using MirPtr = RegionPtr<MirRegion, T>;
template<class T> using SsaPtr = RegionPtr<SsaRegion, T>;

using DeclBase = RegionBase<DeclRegion>;
using MirBase = RegionBase<MirRegion>;
using SsaBase = RegionBase<SsaRegion>;

/*
 * A handle to a derived node, as a handle to its base.
 *
 * `RegionPtr` does not convert implicitly, and that is deliberate: it is a bare offset, so a silent
 * conversion between two *region* types would compile exactly as readily as a widening within one
 * hierarchy. The widening a class hierarchy does make safe is therefore written out, and asserted -
 * every node hierarchy here is single inheritance with the base first, which is what makes the
 * offset the same number.
 */
template<class Base, class Tag, class Derived>
RegionPtr<Tag, Base> asBase(RegionPtr<Tag, Derived> pointer) {
    static_assert(__is_base_of(Base, Derived), "asBase widens within one hierarchy");
    return RegionPtr<Tag, Base>(pointer.offset);
}

/*
 * A dense function-local identity, in a domain of its own - Analysis-IR.md §8.3.
 *
 * A raw `U32` for every ID domain is how the current resolver ended up with a node index passed
 * where a block index was wanted and nothing to say so. These are distinct C++ types with no
 * implicit conversion in either direction, so the mistake is a compile error rather than an index
 * into the wrong dense table - which is the same argument `RegionPtr`'s distinct tags make one
 * level down, applied to the identities inside one body.
 *
 * `kNone` is all ones rather than zero, because these are dense and zero is the first one.
 */
template<class Domain>
struct Id {
    static constexpr U32 kNone = ~U32(0);

    Id() = default;
    explicit Id(U32 index): index(index) {}

    U32 index = kNone;

    bool valid() const { return index != kNone; }

    bool operator == (Id other) const { return index == other.index; }
    bool operator != (Id other) const { return index != other.index; }
};

struct NodeDomain {};
struct PlaceDomain {};
struct BlockDomain {};
struct LocalDomain {};
struct ConstantDomain {};
struct ValueDomain {};
struct InstDomain {};

/*
 * The identities the ownership analyses index by, each produced by exactly one pass.
 *
 * A loan is one borrow with an extent (`mir::Loan`, minted by `Builder::addRead`); an object is one
 * thing whose teardown is counted and whose placement is decided (`mir::Object`, minted by the
 * producers and merged by moves); a move path is one initialization-tracked prefix of a place; a
 * continuation is one region of blocks with a nesting rule (`ssa::Continuation`).
 *
 * They are named here rather than each beside its own pass because the alternative - a pass that
 * introduces both a concept and an index type at once - is how the current resolver acquired
 * provenance as a relation parallel to liveness rather than an index into the same table. What that
 * argument does *not* license is a name with no rules behind it: `ObjectDomain` sat here unused
 * while placement was a field on a storage slot, which is the same defect from the other side - the
 * identity existed and the thing it was supposed to be the identity *of* did not.
 */
struct LoanDomain {};
struct ObjectDomain {};
struct MovePathDomain {};
struct ContinuationDomain {};

using NodeId = Id<NodeDomain>;
using PlaceId = Id<PlaceDomain>;
using BlockId = Id<BlockDomain>;
using LocalId = Id<LocalDomain>;
using ConstantId = Id<ConstantDomain>;
using ValueId = Id<ValueDomain>;
using InstId = Id<InstDomain>;
using LoanId = Id<LoanDomain>;
using ObjectId = Id<ObjectDomain>;
using MovePathId = Id<MovePathDomain>;
using ContinuationId = Id<ContinuationDomain>;

// A pointer and a count, for a region table that wants to be walked. Named rather than returned as
// a pair so that a range-for over one reads like a range-for over an `Array`.
template<class T>
struct Contents {
    T* data;
    U32 count;

    T* begin() const { return data; }
    T* end() const { return data + count; }
    U32 size() const { return count; }
};

template<class T, bool allowEmbed = true> using DeclList = SmallList<DeclRegion, T, allowEmbed>;
template<class T, bool allowEmbed = true> using MirList = SmallList<MirRegion, T, allowEmbed>;
template<class T, bool allowEmbed = true> using SsaList = SmallList<SsaRegion, T, allowEmbed>;

/*
 * An arena whose allocations are aligned, which `LinearArena` alone does not give.
 *
 * `LinearArena::alloc` bumps by the raw size, so one odd-sized node misaligns every node after it -
 * `compiler/util/README.md` records the ~4,200 misaligned accesses that produces over the current
 * fixture suite, and that the fix is held up by golden files with raw arena offsets baked into them.
 * The new stage has no such goldens to protect, so it pays the rounding from the first allocation
 * rather than acquiring the same debt: every node here is reached as a `T*` and is entitled to the
 * alignment the compiler assumes it has.
 *
 * Eight rather than `alignof(T)` because every node in these regions is at most eight-aligned, and a
 * single constant keeps the bump a compare and an add.
 */
template<class Tag>
struct AlignedRegion: Region<Tag> {
    using Region<Tag>::Region;

    static Size rounded(Size size) { return (size + 7) & ~Size(7); }

    void* alloc(Size size) {
        return Region<Tag>::alloc(rounded(size));
    }

    /*
     * Grows the most recent allocation in place, or answers that it cannot.
     *
     * A bump allocator cannot free, so a table that doubles by allocating a new buffer abandons
     * every old one inside the region - and a dense four-byte table therefore costs between two and
     * four times its live contents depending on where the final count lands between two powers of
     * two. That is the one case a bump allocator *can* reclaim: when the buffer being grown is still
     * the last thing allocated, the growth is a bump and the copy does not happen at all.
     *
     * Answers false when something was allocated after it, which is when a caller has to fall back
     * to allocate-and-copy. Sizes are the raw ones; the rounding this arena applies is undone here
     * rather than by the caller, because the caller does not know what it was.
     */
    bool extendLast(void* buffer, Size size, Size wanted) {
        auto held = rounded(size);
        auto needed = rounded(wanted);
        if(needed <= held) return true;

        if((Byte*)buffer + held != this->p) return false;

        // The bump itself. `alloc` is what commits, so the extension goes through it rather than
        // moving `p` by hand - a buffer that grew past the committed prefix would otherwise be
        // readable memory that has not been mapped.
        Region<Tag>::alloc(needed - held);
        return true;
    }
};

using DeclArena = AlignedRegion<DeclRegion>;
using MirArena = AlignedRegion<MirRegion>;
using SsaArena = AlignedRegion<SsaRegion>;

inline void* operator new (Size count, DeclArena& arena) { return arena.alloc(count); }
inline void* operator new (Size count, MirArena& arena) { return arena.alloc(count); }
inline void* operator new (Size count, SsaArena& arena) { return arena.alloc(count); }

/*
 * A growable array inside a region, with a 32-bit count.
 *
 * `SmallList` is the right thing for a list that is a field of a node - it embeds its first few
 * entries and costs nothing when it is empty, which is what a block's successors or a call's
 * arguments want. It is the wrong thing for a body's dense tables, for one reason: its count is a
 * `U16`, so a function of more than 65535 nodes would silently truncate. A node table is exactly
 * what §8.3 asks for - four bytes per node, so that an analysis has the ID without dereferencing -
 * and it must not have a limit a real program can reach.
 *
 * No destructor and none needed: the storage is the region's, and the region is released whole.
 */
template<class Tag, class T>
struct RegionArray {
    RegionPtr<Tag, T> data = nullptr;
    U32 count = 0;
    U32 reserved = 0;

    U32 push(AlignedRegion<Tag>& arena, T value) {
        if(count == reserved) grow(arena, count + 1);
        (*arena)[data][count] = value;
        return count++;
    }

    T get(RegionBase<Tag> base, U32 index) const {
        assertTrue(index < count);
        return base[data][index];
    }

    void set(RegionBase<Tag> base, U32 index, T value) {
        assertTrue(index < count);
        base[data][index] = value;
    }

    // Everything in the table, for a walk that wants the entries rather than the indices. By value,
    // like every other region container's, so that the base is asked for once instead of per entry.
    Contents<T> contents(RegionBase<Tag> base) const {
        return Contents<T> { count ? base[data] : nullptr, count };
    }

    T* pointer(RegionBase<Tag> base) const { return count ? base[data] : nullptr; }
    U32 size() const { return count; }
    bool isEmpty() const { return count == 0; }

    // Sized ahead of the first push, where the count is known. A table that is reserved once never
    // reaches `grow` at all, which is the cheapest form of the reclamation `extendLast` is the
    // fallback for.
    void reserve(AlignedRegion<Tag>& arena, U32 needed) {
        if(needed > reserved) grow(arena, needed);
    }

private:
    /*
     * Doubling, in place where the arena allows it.
     *
     * `extendLast` is what keeps a dense table from costing several times its contents: a body's
     * node, place and block tables are pushed to in long runs with nothing else allocated between,
     * so the common case is a bump and no copy. When something *was* allocated in between the old
     * buffer is abandoned, which is the bump allocator's own bargain and is why the growth factor
     * stays at two rather than something more eager.
     */
    void grow(AlignedRegion<Tag>& arena, U32 needed) {
        auto next = max(U32(8), max(reserved * 2, needed));

        if(count && arena.extendLast((*arena)[data], sizeof(T) * reserved, sizeof(T) * next)) {
            reserved = next;
            return;
        }

        auto p = (T*)arena.alloc(sizeof(T) * next);
        auto base = *arena;

        for(U32 i = 0; i < count; i++) p[i] = base[data][i];

        data = p - base;
        reserved = next;
    }
};
