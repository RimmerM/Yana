#pragma once

#include "../compiler/context.h"

/*
 * An array that starts inside itself.
 *
 * `SmallArray<T, N>` keeps its first N entries in the object, and moves to the heap only if it
 * outgrows them. That is the shape almost every working list in this compiler actually has: the
 * operands of an instruction, the successors of a block, the arguments of a call, the registers a
 * form constrains. Each of them is two or three entries in every program anyone writes, and each of
 * them was a heap allocation - so an ordinary function used to cost hundreds of them, all of the
 * same handful of sizes, and none of them alive for more than a few hundred instructions.
 *
 * `N` is a guess, and it is meant to be: it says "this many is what real code has". Getting it wrong
 * costs nothing but the case it was already paying - one allocation, at the point the list outgrows
 * the guess - so it should be set to cover the ordinary program rather than the largest imaginable
 * one. Everything past N behaves exactly like an ordinary Array.
 *
 * Not swappable, which is the one thing to know about it. Tritium's array moves by exchanging
 * buffers where the allocator allows it, and a buffer inside the object cannot be exchanged - so
 * moving one of these moves the elements. That is a memcpy for the trivial types these hold, and it
 * is why `hasSwap` is false rather than an oversight.
 *
 * And the consequence: ArrayT's assignment operator *appends* when the allocator cannot swap, which
 * is true of every non-swapping allocator it has (fixed, arena). That is a trap rather than a
 * quirk - `worklist = remaining` reads as "replace" and produces a list that never shrinks - so
 * SmallArray deletes assignment outright and `replaceContents` is the way to say it.
 *
 * The other rule: nothing may hold the address of an entry across a move. The storage is inside the
 * object while it fits, so moving the object moves the entries with it, where an ordinary array
 * would have handed over the same buffer. A list whose contents are pointed into from elsewhere -
 * OwnershipResult's ranges, say - stays an ordinary Array.
 */
template<class T, U32 N, class Allocator = Tritium::HeapAllocator>
struct SmallArrayAllocator {
    static constexpr bool hasSwap = false;
    void swap(SmallArrayAllocator&) {}

    // The old buffer stays readable until `free` is called on it, which is what ArrayT's growth
    // relies on: it takes the pointer, allocates, copies across, and then frees.
    void alloc(U32 size) {
        heap = (T*)Allocator::alloc(size * sizeof(T));
        heapLength = size;
    }

    // The inline buffer is part of the object and is never handed back.
    void free(T* p) {
        if(p != (T*)data) Allocator::free(p);
    }

    void destroy() {
        if(heap) Allocator::free(heap);
        heap = nullptr;
        heapLength = 0;
    }

    T* pointer() { return heap ? heap : (T*)data; }
    const T* pointer() const { return heap ? heap : (const T*)data; }

    U32 space() const { return heap ? heapLength : N; }
    U32 probeSpace(U32 needed) const { return space(); }

private:
    T* heap = nullptr;
    U32 heapLength = 0;
    Uninitialized<T> data[N];
};

// An array of `T` that holds its first `N` entries inline - see SmallArrayAllocator. A type of its
// own rather than an alias so that assignment can be deleted; everything else is ArrayT's.
template<class T, U32 N>
struct SmallArray: ArrayT<T, SmallArrayAllocator<T, N>> {
    using Base = ArrayT<T, SmallArrayAllocator<T, N>>;
    using Base::Base;

    SmallArray() = default;
    SmallArray(SmallArray&&) = default;
    SmallArray(const SmallArray&) = default;

    // Both deleted: the inherited one appends rather than replaces. Say `replaceContents`.
    SmallArray& operator = (const SmallArray&) = delete;
    SmallArray& operator = (SmallArray&&) = delete;
};

// Gives one list another's contents. What `into = from` would be if assigning a SmallArray did what
// it looks like; see the note above. It also keeps whatever storage `into` had, which is what the
// pooled lists want anyway.
template<class To, class From>
void replaceContents(To& into, const From& from) {
    into.clear();
    for(auto& value: from) into.push(value);
}

/*
 * A set of small indices, one bit each, whose storage outlives the set.
 *
 * The members are whatever the pass holding it is indexed by - a function's locals in the ownership
 * analyses, its blocks in the optimizer's flow passes, its instructions in the range builder. Every
 * one of those used to be an `Array<U8>` filled by pushing one zero at a time, and between them
 * they were the largest single source of allocations in the compiler.
 *
 * `reset` is the only way to size one, and it keeps whatever buffer this instance already has
 * whenever that buffer is big enough - so a set reused across functions allocates for the largest
 * one it ever saw and never again. Nothing here shrinks: the high-water mark is the point.
 *
 * Move-only, deliberately. The sets used to be assigned around by value, and each of those was a
 * hidden allocation; the operations that replaced them - `copyFrom`, `unionWith` - say what they do
 * and write into storage that already exists.
 *
 * The bits past `size()` are always clear, which is what lets the set-wise operations work a byte
 * at a time instead of an index at a time. `reset` zeroes them and `fill` masks them off again.
 *
 * The first `kInlineBits` live in the set itself, for the same reason SmallArray's entries do: a
 * function with fewer locals, blocks or instructions than that - which is nearly every function
 * anyone writes - never reaches the heap at all, and a set used once and dropped costs nothing.
 * Past it the storage is heap and the inline words go unused, which is the price of one allocation
 * for a function large enough not to notice it.
 */
struct IndexSet {
    // Four words. A function of more than 256 locals or blocks is not one this bound is deciding
    // anything about; the point is that eight of them, which is the ordinary case, is free.
    static constexpr Size kInlineBits = 256;
    static constexpr Size kInlineBytes = kInlineBits / 8;

    IndexSet() = default;
    IndexSet(const IndexSet&) = delete;
    IndexSet& operator = (const IndexSet&) = delete;

    IndexSet(IndexSet&& other) { take(other); }

    // Wanted by anything that sorts a structure holding a set - see the loop list in opt_flow.cpp.
    IndexSet& operator = (IndexSet&& other) {
        if(this == &other) return *this;

        if(heap) Tritium::hFree(heap);
        take(other);
        return *this;
    }

    ~IndexSet() {
        if(heap) Tritium::hFree(heap);
    }

    // Clears the set and makes room for `count` indices, reusing this instance's buffer where it
    // already holds that many.
    void reset(Size count) {
        auto needed = (count + 7) / 8;

        if(needed > capacity()) {
            if(heap) Tritium::hFree(heap);

            heap = (Byte*)Tritium::hAlloc(needed);
            heapBytes = needed;
        }

        this->count = count;

        auto p = bytes();
        for(Size i = 0; i < needed; i++) p[i] = 0;
    }

    Size size() const { return count; }

    /*
     * Reading past the end answers "not in the set" rather than asserting.
     *
     * `provenanceOf` hands back a shared empty set for a value it has no row for, and its callers
     * loop over the function's locals rather than over the set - so an out-of-range read is a real
     * shape in this code rather than a mistake, and it means the same thing the in-range answer
     * would.
     */
    bool get(Size index) const {
        return index < count && (bytes()[index / 8] & (1u << (index % 8))) != 0;
    }

    bool operator [] (Size index) const { return get(index); }

    void set(Size index, bool value) {
        assertTrue(index < count);
        auto mask = Byte(1u << (index % 8));

        if(value) bytes()[index / 8] |= mask;
        else bytes()[index / 8] &= Byte(~mask);
    }

    // Adds one local, reporting whether it was not already there.
    bool add(Size index) {
        if(get(index)) return false;
        set(index, true);
        return true;
    }

    // Adds everything in `source`, reporting whether that changed anything. Byte at a time: a
    // function's locals fit in one or two of them, and every fixpoint here climbs from empty, so
    // this is the only way any of them move.
    bool unionWith(const IndexSet& source) {
        auto changed = false;
        auto from = source.bytes();
        auto into = bytes();

        for(Size i = 0; i < byteCount() && i < source.byteCount(); i++) {
            auto added = from[i] & ~into[i];
            if(!added) continue;

            into[i] |= added;
            changed = true;
        }

        return changed;
    }

    // Removes everything not in `source`, reporting whether that changed anything.
    bool intersectWith(const IndexSet& source) {
        auto changed = false;
        auto from = source.bytes();
        auto into = bytes();

        for(Size i = 0; i < byteCount(); i++) {
            auto kept = Byte(into[i] & (i < source.byteCount() ? from[i] : 0));
            if(kept == into[i]) continue;

            into[i] = kept;
            changed = true;
        }

        return changed;
    }

    // Adds every index below `size()`. The bits past the end of the set stay clear, because the
    // whole-byte operations above compare them - see the invariant in the header comment.
    void fill() {
        if(!count) return;

        for(Size i = 0; i < byteCount(); i++) bytes()[i] = Byte(0xff);

        if(auto tail = count % 8) bytes()[byteCount() - 1] = Byte((1u << tail) - 1);
    }

    void copyFrom(const IndexSet& source) {
        reset(source.count);
        for(Size i = 0; i < byteCount(); i++) bytes()[i] = source.bytes()[i];
    }

    bool equals(const IndexSet& other) const {
        if(count != other.count) return false;

        for(Size i = 0; i < byteCount(); i++) {
            if(bytes()[i] != other.bytes()[i]) return false;
        }

        return true;
    }

private:
    Size byteCount() const { return (count + 7) / 8; }
    Size capacity() const { return heap ? heapBytes : kInlineBytes; }

    // The set-wise operations above read one set while writing another, so the read side has to be
    // callable on a const set. Casting here rather than at each of them keeps the constness of the
    // arguments honest, which is what the callers care about.
    Byte* bytes() const { return heap ? heap : const_cast<Byte*>(inlineBytes); }

    // Takes over `other`'s storage, leaving it empty. The inline bytes cannot be stolen, so they are
    // copied - which is a fixed few words and no allocation either way.
    void take(IndexSet& other) {
        heap = other.heap;
        heapBytes = other.heapBytes;
        count = other.count;

        if(!heap) {
            for(Size i = 0; i < kInlineBytes; i++) inlineBytes[i] = other.inlineBytes[i];
        }

        other.heap = nullptr;
        other.heapBytes = 0;
        other.count = 0;
    }

    Byte* heap = nullptr;
    Size heapBytes = 0;
    Size count = 0;
    Byte inlineBytes[kInlineBytes] = {};
};

/*
 * A list of such sets - liveness at each block, or the liveness at each point inside one.
 *
 * The rows outlive the list's contents for the same reason a single set outlives its contents:
 * `reset` grows the list to the rows this function needs and re-sizes the ones already there,
 * so a walk over a thousand functions allocates for the widest one rather than for each.
 */
struct IndexSetList {
    void reset(Size rows, Size count) {
        while(sets.size() < rows) sets.push(IndexSet());
        for(Size i = 0; i < rows; i++) sets[i].reset(count);
        used = rows;
    }

    IndexSet& operator [] (Size index) { assertTrue(index < used); return sets[index]; }
    const IndexSet& operator [] (Size index) const { assertTrue(index < used); return sets[index]; }
    Size size() const { return used; }

private:
    Array<IndexSet> sets;
    Size used = 0;
};


/*
 * The same borrow-by-scope arrangement for anything that can be emptied rather than freed.
 *
 * `T` needs a `clear()` that keeps the storage it grew into, which is what makes the pool worth
 * having: the borrower gets the last borrower's buffers. Held by pointer so that growing the pool
 * cannot move something a holder is using, and the depth is what lets borrows nest.
 */
template<class T>
struct ScratchPool {
    ~ScratchPool() {
        for(auto item: items) delete item;
    }

    Array<T*> items;
    Size depth = 0;
};

template<class T>
struct Scratch {
    explicit Scratch(ScratchPool<T>& pool): pool(pool) {
        if(pool.depth == pool.items.size()) pool.items.push(new T());

        item = pool.items[pool.depth++];
        item->clear();
    }

    ~Scratch() { pool.depth--; }

    Scratch(const Scratch&) = delete;
    Scratch& operator = (const Scratch&) = delete;

    T& operator * () const { return *item; }
    T* operator -> () const { return item; }

private:
    ScratchPool<T>& pool;
    T* item;
};

/*
 * A pool of index sets, and one borrowed from it for the length of a scope.
 *
 * The same arrangement as the sets themselves, one level up: a pass that needs a set per call gets
 * the one the last call used rather than a new one, and the pool grows to the deepest nesting any
 * function reached. Held by pointer so that growing it cannot move a set something is holding.
 */
struct IndexSetPool {
    ~IndexSetPool() {
        for(auto set: sets) delete set;
    }

    Array<IndexSet*> sets;
    Size depth = 0;
};

struct ScratchSet {
    ScratchSet(IndexSetPool& pool, Size count): pool(pool) {
        if(pool.depth == pool.sets.size()) pool.sets.push(new IndexSet());

        set = pool.sets[pool.depth++];
        set->reset(count);
    }

    ~ScratchSet() { pool.depth--; }

    ScratchSet(const ScratchSet&) = delete;
    ScratchSet& operator = (const ScratchSet&) = delete;

    IndexSet& operator * () const { return *set; }
    IndexSet* operator -> () const { return set; }

private:
    IndexSetPool& pool;
    IndexSet* set;
};

/*
 * The same arrangement for rows that are not sets: a list of arrays that keeps its rows.
 *
 * For the facts that need more than a bit each - the ownership lattice's four states - where the
 * cost being avoided is the same one. `reset` empties each row rather than destroying it, and a
 * Tritium array that has been cleared still holds the storage it grew into.
 */
/*
 * A list of things that empty rather than free - anything with a `clear()` that keeps its storage.
 *
 * The same purpose as ArrayList for rows that are structs of arrays instead of one array: the
 * per-instruction effects the ownership passes read, where a row destroyed and rebuilt is five
 * allocations and a row emptied is none.
 */
template<class T>
struct PooledList {
    void reset(Size rows) {
        while(entries.size() < rows) entries.push(T());
        for(Size i = 0; i < rows; i++) entries[i].clear();
        used = rows;
    }

    T& operator [] (Size index) { assertTrue(index < used); return entries[index]; }
    const T& operator [] (Size index) const { assertTrue(index < used); return entries[index]; }
    Size size() const { return used; }

private:
    Array<T> entries;
    Size used = 0;
};

/*
 * The rows are SmallArrays, which is what stops the *first* use of each from allocating.
 *
 * Emptying a row keeps whatever it grew into, so a list reused across functions settles - but a row
 * index no function had reached before is a fresh row, and with an ordinary array that is one
 * allocation each. A list indexed by instruction therefore paid one allocation per instruction of
 * the largest function it ever saw. Inline rows make that free for the ordinary width, which for
 * the two lists this holds - the ownership state of each local, the webs one web conflicts with -
 * is a handful of entries.
 */
template<class T, U32 N = 8>
struct ArrayList {
    using Row = SmallArray<T, N>;

    void reset(Size rows, Size count, const T& value) {
        reset(rows);
        for(Size i = 0; i < rows; i++) {
            for(Size j = 0; j < count; j++) entries[i].push(value);
        }
    }

    // Rows that start empty rather than filled, for the lists that are grown into rather than
    // indexed from the start - the conflicts and occupants placement collects per web.
    void reset(Size rows) {
        while(entries.size() < rows) entries.push(Row());
        for(Size i = 0; i < rows; i++) entries[i].clear();

        used = rows;
    }

    // One more row than the last reset left, keeping whatever storage that row already had. Rows
    // appear one at a time where the count is discovered rather than known - a frame slot exists
    // because a web was put in it - and a row past `used` is one an earlier function grew.
    Row& push() {
        if(entries.size() == used) entries.push(Row());
        else entries[used].clear();

        return entries[used++];
    }

    // Replaces one row's contents without giving up its storage, which is what the two arrays the
    // ownership walk carries between blocks are assigned through.
    template<class Source>
    void copyInto(Size index, const Source& source) {
        replaceContents((*this)[index], source);
    }

    Row& operator [] (Size index) { assertTrue(index < used); return entries[index]; }
    const Row& operator [] (Size index) const { assertTrue(index < used); return entries[index]; }
    Size size() const { return used; }

private:
    Array<Row> entries;
    Size used = 0;
};

template<class T, Size mask>
struct EmbedIterator {
    explicit EmbedIterator(T* p) : p(p) {}

    T operator * () {
        if constexpr(mask == 0) {
            return *p;
        } else {
            return T(Size(*p) & ~mask);
        }
    }

    EmbedIterator& operator ++ () {
        p++;
        return *this;
    }

    bool operator == (EmbedIterator a) const { return p == a.p; }
    bool operator != (EmbedIterator a) const { return p != a.p; }
    Size operator - (EmbedIterator a) const { return p - a.p; }

private:
    T* p;
};

template<class T, Size mask>
struct EmbedContents {
    explicit EmbedContents(T* ptr, Size length): ptr(ptr), length(length) {}

    EmbedIterator<T, mask> begin() const {
        return EmbedIterator<T, mask>(ptr);
    }

    EmbedIterator<T, mask> end() const {
        return EmbedIterator<T, mask>(ptr + length);
    }

    EmbedIterator<T, mask> back() const {
        assertTrue(length > 0);
        return EmbedIterator<T, mask>(ptr + length - 1);
    }

    T operator[](Size index) {
        assertTrue(index < length);
        return unmask(ptr + index);
    }

    Size size() const {
        return length;
    }

    template<class Predicate>
    Size countWhere(Predicate&& p) const {
        Size count = 0;
        for(auto v: *this) {
            if(p(v)) count++;
        }

        return count;
    }

    Size countOf(T value) const {
        Size count = 0;
        for(auto v: *this) {
            if(value == v) count++;
        }

        return count;
    }

    bool startsWith(T c) const {
        if(length == 0) return false;
        return unmask(ptr) == c;
    }

    bool endsWith(T c) const {
        if(length == 0) return false;
        return unmask(ptr + length - 1) == c;
    }

    bool startsWith(Buffer<const T> s) const {
        if(length < s.length) return false;

        for(Size i = 0; i < s.length; i++) {
            if(unmask(ptr + i) != s.ptr[i]) return false;
        }

        return true;
    }

    bool endsWith(Buffer<const T> s) const {
        if(length < s.length) return false;

        auto p = ptr + length - s.length;
        for(Size i = 0; i < s.length; i++) {
            if(unmask(p + i) != s.ptr[i]) return false;
        }

        return true;
    }

    template<class F>
    bool matchesAll(F&& predicate) const {
        for(auto v: *this) {
            if(!p(v)) return false;
        }

        return true;
    }

    bool containsValue(T value) const {
        for(auto v: *this) {
            if(value == v) return true;
        }

        return false;
    }

    template<class Predicate>
    bool contains(Predicate&& p) const {
        for(auto v: *this) {
            if(p(v)) return true;
        }

        return false;
    }

    Maybe<Size> findIndex(T value) const {
        Size i = 0;
        for(auto v: *this) {
            if(value == v) return Just(i);
            i++;
        }

        return Nothing();
    }

    template<class Predicate>
    Maybe<Size> findIndexWhere(Predicate&& p) const {
        Size i = 0;
        for(auto v: *this) {
            if(p(v)) return Just(i);
            i++;
        }

        return Nothing();
    }

    /*
     * By value, and it has to be.
     *
     * The iterator yields a `T` rather than a `T&`: for a list with a tag bit in its entries,
     * `operator*` strips the mask, so the thing the caller wants is a value this computes and not
     * anything the list actually holds. A `Maybe<T&>` here therefore referred to the loop variable,
     * which is dead by the time the caller reads it - and since the entries are region handles, the
     * caller then indexed the region with whatever had been left on the stack.
     */
    template<class Predicate>
    Maybe<T> findWhere(Predicate&& p) const {
        for(auto v: *this) {
            if(p(v)) return Just(v);
        }

        return Nothing();
    }

    T unmask(const T* p) const {
        if constexpr(mask == 0) {
            return *p;
        } else {
            return T(Size(*p) & ~mask);
        }
    }

    /*
     * The address of one element, rather than a copy of it.
     *
     * For the callers that need a handle that outlives the loop: a deferred call argument is
     * remembered as a pointer into the parse arena and resolved much later, and the iterator's
     * by-value result would be a dangling one. Only available where the list holds objects rather
     * than tagged handles - a masked entry is computed by unmask() and is not what the list holds,
     * which is the whole reason findWhere() above returns by value.
     */
    T* pointerAt(Size index) const {
        static_assert(mask == 0, "a masked list's entries are not addressable - see unmask");
        assertTrue(index < length);
        return ptr + index;
    }

private:
    T* ptr;
    Size length;
};

// Stores a list of an integral or pointer type.
// The top bit in each stored value must be unused.
template<class T, bool allowEmbed = true>
struct EmbedList;

template<class T>
struct EmbedList<T, true> {
    struct List {
        T* p;
        U32 count;
        U32 reserved;
    };

    static_assert(sizeof(T) <= sizeof(Size));
    static constexpr Size embeddedCount = sizeof(List) / sizeof(T);
    static constexpr Size mask = U64(1) << (sizeof(T) * 8 - 1);

    union {
        List list;
        T embedded[embeddedCount];
    };

    EmbedList() {
        list.p = 0;
        list.count = 0;
        list.reserved = 0;
    }

    ~EmbedList() {}

    T* getListHead() {
        // If there are multiple of T inside the pointer field,
        // we need to interpret it as big endian to correctly determine if the first list item is set.
        if constexpr(sizeof(T) == sizeof(Size)) {
            return list.p;
        } else if constexpr(sizeof(T) == 4) {
            auto p = (Size)list.p;
            return (T*)((p >> 32) | (p << 32));
        } else if constexpr(sizeof(T) == 2) {
            auto p = (Size)list.p;
            return (T*)((p >> 16) | (p << 48));
        } else if constexpr(sizeof(T) == 1) {
            return (T*)::swapEndian((Size)list.p);
        } else {
            static_assert(sizeof(T) == sizeof(Size), "unsupported element size");
        }
    }

    void setListHead(T* p) {
        // If there are multiple of T inside the pointer field,
        // we need to interpret it as big endian to correctly determine if the first list item is set.
        if constexpr(sizeof(T) == sizeof(Size)) {
            list.p = p;
        } else if constexpr(sizeof(T) == 4) {
            auto v = (Size)p;
            list.p = (T*)((v >> 32) | (v << 32));
        } else if constexpr(sizeof(T) == 2) {
            auto v = (Size)p;
            list.p = (T*)((v >> 16) | (v << 48));
        } else if constexpr(sizeof(T) == 1) {
            list.p = (T*)::swapEndian((Size)p);
        } else {
            static_assert(sizeof(T) == sizeof(Size), "unsupported element size");
        }
    }

    bool isEmbedded() {
        if constexpr(embeddedCount == 0) return false;
        return ((Size)getListHead()) >> (sizeof(Size) * 8 - 1);
    }

    EmbedContents<T, mask> contents() {
        if(isEmbedded()) {
            return EmbedContents<T, mask>(embedded, size<true>());
        } else {
            return EmbedContents<T, mask>(getListHead(), list.count);
        }
    }

    template<class Arena>
    Size push(Arena& arena, T value) {
        assertTrue(!(Size(value) & mask));

        auto e = isEmbedded();
        Size count;

        if(!e && list.reserved == 0) {
            count = 0;
            e = true;
        } else {
            count = e ? size<true>() : size<false>();
        }

        if(e && count < embeddedCount) {
            embedded[count] = T(Size(value) | mask);
        } else {
            reserve(arena, count + 1);
            getListHead()[list.count++] = value;
        }

        return count;
    }

    template<class Arena>
    void reserve(Arena& arena, Size count) {
        auto embedded = isEmbedded();
        auto reserved = embedded ? embeddedCount : list.reserved;

        if(count > reserved) {
            auto currentCount = size();
            auto nextCount = max(currentCount * 2, max(embeddedCount * 4, count));
            allocate(arena, currentCount, nextCount);
        }
    }

    template<class Arena>
    void allocate(Arena& arena, Size currentCount, Size nextCount) {
        auto p = (T*)arena.alloc(sizeof(T) * nextCount);

        if(isEmbedded()) {
            for(Size i = 0; i < currentCount; i++) {
                p[i] = T(Size(embedded[i]) & ~mask);
            }
        } else {
            auto head = getListHead();

            for(Size i = 0; i < currentCount; i++) {
                p[i] = head[i];
            }
        }

        setListHead(p);
        list.count = currentCount;
        list.reserved = nextCount;
    }

    T remove(Size index) {
        if(isEmbedded()) {
            assertTrue(index < embeddedCount);
            auto count = size();

            for(Size i = index; i < size(); i++) {
                embedded[i] = embedded[i + 1];
            }

            for(Int i = embeddedCount; i >= count; i--) {
                embedded[i - 1] = 0;
            }
        } else {
            assertTrue(size() > index);
            auto p = getListHead();
            Tritium::move(p + index + 1, p + index, list.count - 1 - index);
            list.count--;
        }
    }

    void set(Size index, T value) {
        assertTrue(!(Size(value) & mask));

        if(isEmbedded()) {
            assertTrue(index < embeddedCount);
            embedded[index] = T(Size(value) | mask);
        } else {
            assertTrue(size() > index);
            getListHead()[index] = value;
        }
    }

    T operator[](Size index) {
        if(isEmbedded()) {
            assertTrue(index < embeddedCount);
            return T(Size(embedded[index]) & ~mask);
        } else {
            assertTrue(size() > index);
            return getListHead()[index];
        }
    }

    Size size() {
        if(isEmbedded()) {
            return size<true>();
        } else {
            return list.count;
        }
    }

    template<bool isEmbedded>
    Size size() {
        if constexpr(isEmbedded) {
            Size count = 0;

            for(Size i = 0; i < embeddedCount; i++) {
                if(Size(embedded[i]) & mask) count++;
            }

            return count;
        } else {
            return list.count;
        }
    }

    bool isEmpty() {
        return !isEmbedded() && list.count == 0;
    }

    bool isNotEmpty() {
        return !isEmpty();
    }
};

template<class T>
struct EmbedList<T, false> {
    struct List {
        T* p;
        U32 count;
        U32 reserved;
    };

    List list;

    EmbedList() {
        list.p = 0;
        list.count = 0;
        list.reserved = 0;
    }

    ~EmbedList() {}

    bool isEmbedded() {
        return false;
    }

    EmbedContents<T, 0> contents() {
        return EmbedContents<T, 0>(list.p, list.count);
    }

    template<class Arena>
    Size push(Arena& arena, T value) {
        auto count = list.count;
        reserve(arena, count + 1);

        list.p[count] = value;
        list.count = count + 1;

        return count;
    }

    template<class Arena>
    void reserve(Arena& arena, Size count) {
        if(count > list.reserved) {
            auto currentCount = size();
            auto nextCount = max(currentCount * 2, max(Size(8), count));
            allocate(arena, currentCount, nextCount);
        }
    }

    template<class Arena>
    void allocate(Arena& arena, Size currentCount, Size nextCount) {
        auto p = (T*)arena.alloc(sizeof(T) * nextCount);
        auto head = list.p;

        for(Size i = 0; i < currentCount; i++) {
            p[i] = head[i];
        }

        list.p = p;
        list.count = currentCount;
        list.reserved = nextCount;
    }

    T remove(Size index) {
        assertTrue(size() > index);
        Tritium::move(list.p + index + 1, list.p + index, list.count - 1 - index);
        list.count--;
    }

    void clear() {
        list.count = 0;
    }

    void set(Size index, T value) {
        assertTrue(size() > index);
        list.p[index] = value;
    }

    T operator[](Size index) {
        assertTrue(size() > index);
        return list.p[index];
    }

    Size size() {
        return list.count;
    }

    bool isEmpty() {
        return list.count == 0;
    }

    bool isNotEmpty() {
        return !isEmpty();
    }
};

template<class Region, class T, bool allowEmbed = true>
struct SmallList;

template<class Region, class T>
struct SmallList<Region, T, true> {
    struct List {
        RegionPtr<Region, T> p;
        U16 count;
        U16 reserved;
    };

    static_assert(sizeof(T) <= sizeof(U32));
    static constexpr Size embeddedCount = sizeof(List) / sizeof(T);
    static constexpr Size mask = U64(1) << (sizeof(T) * 8 - 1);

    union {
        List list;
        T embedded[embeddedCount];
    };

    SmallList() {
        list.p = nullptr;
        list.count = 0;
        list.reserved = 0;
    }

    ~SmallList() {}

    RegionPtr<Region, T> getListOffset() {
        // If there are multiple of T inside the pointer field,
        // we need to interpret it as big endian to correctly determine if the first list item is set.
        if constexpr(sizeof(T) == sizeof(U32)) {
            return list.p;
        } else if constexpr(sizeof(T) == 2) {
            return RegionPtr<Region, T>((list.p.offset >> 16) | (list.p.offset << 16));
        } else if constexpr(sizeof(T) == 1) {
            return RegionPtr<Region, T>(::swapEndian(list.p.offset));
        } else {
            static_assert(sizeof(T) == sizeof(U32), "unsupported element size");
        }
    }

    T* getListHead(RegionBase<Region> base) {
        return base[getListOffset()];
    }

    void setListOffset(RegionPtr<Region, T> p) {
        // If there are multiple of T inside the pointer field,
        // we need to interpret it as big endian to correctly determine if the first list item is set.
        if constexpr(sizeof(T) == sizeof(U32)) {
            list.p = p;
        } else if constexpr(sizeof(T) == 2) {
            list.p.offset = (p.offset >> 16) | (p.offset << 16);
        } else if constexpr(sizeof(T) == 1) {
            list.p.offset = ::swapEndian(p.offset);
        } else {
            static_assert(sizeof(T) == sizeof(U32), "unsupported element size");
        }
    }

    bool isEmbedded() {
        if constexpr(embeddedCount == 0) return false;
        return getListOffset().offset >> (sizeof(U32) * 8 - 1);
    }

    EmbedContents<T, mask> contents(RegionBase<Region> base) {
        if(isEmbedded()) {
            return EmbedContents<T, mask>(embedded, size<true>());
        } else {
            return EmbedContents<T, mask>(getListHead(base), list.count);
        }
    }

    template<class Arena>
    Size push(Arena& arena, T value) {
        auto e = isEmbedded();
        Size count;

        if(!e && list.reserved == 0) {
            count = 0;
            e = true;
        } else {
            count = e ? size<true>() : size<false>();
        }

        if(e && count < embeddedCount) {
            embedded[count] = T(Size(value) | mask);
        } else {
            reserve(arena, count + 1);
            getListHead(*arena)[list.count++] = value;
        }

        return count;
    }

    template<class Arena>
    void reserve(Arena& arena, Size count) {
        auto embedded = isEmbedded();
        auto reserved = embedded ? embeddedCount : list.reserved;

        if(count > reserved) {
            auto currentCount = size();
            auto nextCount = max(currentCount * 2, max(embeddedCount * 4, count));
            allocate(arena, currentCount, nextCount);
        }
    }

    template<class Arena>
    void allocate(Arena& arena, Size currentCount, Size nextCount) {
        auto p = (T*)arena.alloc(sizeof(T) * nextCount);

        if(isEmbedded()) {
            for(Size i = 0; i < currentCount; i++) {
                p[i] = T(Size(embedded[i]) & ~mask);
            }
        } else {
            auto head = getListHead(*arena);

            for(Size i = 0; i < currentCount; i++) {
                p[i] = head[i];
            }
        }

        setListOffset(p - *arena);
        list.count = currentCount;
        list.reserved = nextCount;
    }

    // An embedded entry is present exactly when its high bit is set (see `mask`), so dropping one
    // means shifting the rest down and clearing what was the last - which is what makes size()
    // report one fewer.
    void remove(RegionBase<Region> base, Size index) {
        if(isEmbedded()) {
            auto count = size();
            assertTrue(index < count);

            for(Size i = index; i + 1 < count; i++) {
                embedded[i] = embedded[i + 1];
            }

            embedded[count - 1] = 0;
        } else {
            assertTrue(size() > index);
            auto p = getListHead(base);
            Tritium::move(p + index + 1, p + index, list.count - 1 - index);
            list.count--;
        }
    }

    void set(RegionBase<Region> base, Size index, T value) {
        assertTrue(!(Size(value) & mask));

        if(isEmbedded()) {
            assertTrue(index < embeddedCount);
            embedded[index] = T(Size(value) | mask);
        } else {
            assertTrue(size() > index);
            getListHead(base)[index] = value;
        }
    }

    T get(RegionBase<Region> base, Size index) {
        if(isEmbedded()) {
            assertTrue(index < embeddedCount);
            return T(Size(embedded[index]) & ~mask);
        } else {
            assertTrue(size() > index);
            return getListHead(base)[index];
        }
    }

    Size size() {
        if(isEmbedded()) {
            return size<true>();
        } else {
            return list.count;
        }
    }

    template<bool isEmbedded>
    Size size() {
        if constexpr(isEmbedded) {
            Size count = 0;

            for(Size i = 0; i < embeddedCount; i++) {
                if(Size(embedded[i]) & mask) count++;
            }

            return count;
        } else {
            return list.count;
        }
    }

    bool isEmpty() {
        return !isEmbedded() && list.count == 0;
    }

    bool isNotEmpty() {
        return !isEmpty();
    }
};

template<class Region, class T>
struct SmallList<Region, T, false> {
    struct List {
        RegionPtr<Region, T> p { nullptr };
        U16 count = 0;
        U16 reserved = 0;
    };

    List list;

    bool isEmbedded() {
        return false;
    }

    EmbedContents<T, 0> contents(RegionBase<Region> base) {
        return EmbedContents<T, 0>(base[list.p], list.count);
    }

    template<class Arena>
    Size push(Arena& arena, T value) {
        auto count = list.count;

        reserve(arena, count + 1);
        (*arena)[list.p][count] = value;

        list.count = count + 1;
        return count;
    }

    template<class Arena>
    void reserve(Arena& arena, Size count) {
        if(count > list.reserved) {
            auto currentCount = size();
            auto nextCount = max(currentCount * 2, max(Size(8), count));
            allocate(arena, currentCount, nextCount);
        }
    }

    template<class Arena>
    void allocate(Arena& arena, Size currentCount, Size nextCount) {
        auto p = (T*)arena.alloc(sizeof(T) * nextCount);
        auto base = *arena;
        auto head = base[list.p];

        for(Size i = 0; i < currentCount; i++) {
            p[i] = head[i];
        }

        list.p = p - base;
        list.count = currentCount;
        list.reserved = nextCount;
    }

    void remove(RegionBase<Region> base, Size index) {
        assertTrue(size() > index);
        auto p = base[list.p];
        Tritium::move(p + index + 1, p + index, list.count - 1 - index);
        list.count--;
    }

    void clear() {
        list.count = 0;
    }

    void set(RegionBase<Region> base, Size index, T value) {
        assertTrue(size() > index);
        base[list.p][index] = value;
    }

    T get(RegionBase<Region> base, Size index) {
        assertTrue(size() > index);
        return base[list.p][index];
    }

    Size size() {
        return list.count;
    }

    bool isEmpty() {
        return list.count == 0;
    }

    bool isNotEmpty() {
        return !isEmpty();
    }
};

struct EmbedSet {
    static constexpr Size bits = sizeof(Size) * 8;

    union {
        Size* list;
        Size embedded;
    };

    template<class Arena>
    EmbedSet(Arena& arena, Size count) {
        if(isSmall(count)) {
            embedded = 0;
        } else {
            // `alignSize<bits>(count) / bits` is a count of words, not of bytes - alloc takes bytes.
            auto words = alignSize<bits>(count) / bits;
            list = (Size*)arena.alloc(words * sizeof(Size));
            for(Size i = 0; i < words; i++) list[i] = 0;
        }
    }

    static bool isSmall(Size count) {
        return count <= sizeof(embedded) * 8;
    }

    bool get(Size count, Size index) const {
        assertTrue(index < count);

        if(isSmall(count)) {
            return embedded & (Size(1) << index);
        } else {
            return (list[index / bits] & (Size(1) << (index % bits))) != 0;
        }
    }

    template<bool isSmall>
    bool get(Size index) const {
        if constexpr(isSmall) {
            return embedded & (Size(1) << index);
        } else {
            return (list[index / bits] & (Size(1) << (index % bits))) != 0;
        }
    }

    void set(Size count, Size index) {
        assertTrue(index < count);

        if(isSmall(count)) {
            embedded |= (Size(1) << index);
        } else {
            list[index / bits] |= (Size(1) << (index % bits));
        }
    }

    template<bool isSmall>
    void set(Size index) {
        if constexpr(isSmall) {
            embedded |= (Size(1) << index);
        } else {
            list[index / bits] |= (Size(1) << (index % bits));
        }
    }

    void clear(Size count, Size index) {
        assertTrue(index < count);

        if(isSmall(count)) {
            embedded &= ~(Size(1) << index);
        } else {
            list[index / bits] &= ~(Size(1) << (index % bits));
        }
    }

    template<bool isSmall>
    void clear(Size index) {
        if constexpr(isSmall) {
            embedded &= ~(Size(1) << index);
        } else {
            list[index / bits] &= ~(Size(1) << (index % bits));
        }
    }

    template<class F>
    void iterate(Size count, F&& f) {
        if(isSmall(count)) {
            iterate<true, F>(count, forward<F>(f));
        } else {
            iterate<false, F>(count, forward<F>(f));
        }
    }

    /*
     * Every set bit, lowest first.
     *
     * The `break` on the top bit is not a shortcut but the whole of what keeps this terminating.
     * Consuming a bit shifts the word right by one more than the bit's position, and for the highest
     * bit of a word that is a shift by the width of the type - which C++ leaves undefined and which
     * x86 implements by masking the count to zero. The word then comes back unchanged, the offset
     * has advanced by a word, and the walk hands out indices past the end of the set forever. There
     * is nothing above the top bit to look at, so stopping there is also the answer.
     */
    template<bool isSmall, class F>
    void iterate(Size count, F&& f) {
        if constexpr(isSmall) {
            auto v = embedded;
            Size o = 0;

            while(v) {
                auto i = Math::findFirstBit(v);
                f(o + i);
                if(i + 1 >= bits) break;

                o += i + 1;
                v >>= i + 1;
            }
        } else {
            auto c = alignSize<bits>(count) / bits;
            for(Size i = 0; i < c; i++) {
                auto v = list[i];
                Size o = i * bits;

                while(v) {
                    auto n = Math::findFirstBit(v);
                    f(o + n);
                    if(n + 1 >= bits) break;

                    o += n + 1;
                    v >>= n + 1;
                }
            }
        }
    }

    Size count(Size count) const {
        if(isSmall(count)) {
            return this->count<true>(count);
        } else {
            return this->count<false>(count);
        }
    }

    template<bool isSmall>
    Size count(Size count) const {
        if constexpr(isSmall) {
            return Math::countBits(embedded);
        } else {
            Size setCount = 0;
            auto c = alignSize<bits>(count) / bits;

            for(Size i = 0; i < c; i++) {
                setCount += Math::countBits(list[i]);
            }

            return setCount;
        }
    }
};
