#pragma once

#include <cassert>
#include "types.h"

template<class T, class Allocator> struct ArrayT : Allocator {
    template<class U>
    struct ItT {
        ItT(U* p) : p(p) {}
        U& operator * () {return *p;}
        U* operator -> () {return p;}
        ItT operator ++ () {p++; return *this;}
        bool operator == (ItT a) {return p == a.p;}
        bool operator != (ItT a) {return p != a.p;}
        Size operator - (ItT a) const {return p - a.p;}
        U* p;
    };

    using I = ItT<T>;
    using CI = ItT<const T>;

    ArrayT() = default;
    ArrayT(const Allocator& a): Allocator(a) {}
    explicit ArrayT(U32 reservedSize) {this->alloc(reservedSize);}
    explicit ArrayT(const Allocator& a, U32 reservedSize): Allocator(a) {this->alloc(reservedSize);}
    ArrayT(const ArrayT&);
    ArrayT(ArrayT&&);
    ~ArrayT() {erase();}
    ArrayT& operator = (ArrayT<T, Allocator>);

    I begin() {return this->pointer();}
    I end() {return this->pointer() + count;}
    I back() {return this->pointer() + count - 1;}
    CI begin() const {return this->pointer();}
    CI end() const {return this->pointer() + count;}
    CI back() const {return this->pointer() + count - 1;}

    template<class... P>
    I push(P&&... p) {
        reserveSpace(count + 1);
        auto i = this->pointer() + count;
        new (i) T(forward<P>(p)...);
        count++;
        return i;
    }

    ArrayT& operator << (const T& t) {push(t); return *this;}
    ArrayT& operator << (T&& t) {push(forward<T>(t)); return *this;}

    template<class... P>
    I insert(Size index, P&&... p) {
        insertSpace(1, (U32)index);
        auto i = this->pointer() + index;
        new (i) T(forward<P>(p)...);
        count++;
        return i;
    }

    void remove(Size index) {
        assertTrue(index < size());
        auto p = this->pointer();
        (p + index)->~T();
        memmove(p + index, p + index + 1, (size() - 1 - index) * sizeof(T));
        count--;
    }

    void resize(U32 count) {
        reserve(count);
        auto p = this->pointer();
        for(U32 i = this->count; i < count; i++) {
            new (p + i) T;
        }
        this->count = count;
    }

    void clear();
    void erase() {clear(); this->destroy();}
    void reserve(U32 required) {reserveSpace(required);}
    Size size() const {return count;}

    T& operator [] (Size i) {return this->pointer()[i];}
    const T& operator [] (Size i) const {return this->pointer()[i];}

protected:
    U32 resizeCount(U32 required) {
        auto c = this->space() * 2;
        if(c < required) c = required;
        return c;
    }

    void reserveSpace(U32 required) {
        if(required > this->probeSpace(required)) {
            required = resizeCount(required);
            auto ptr = this->pointer();
            this->alloc(required);
            memcpy(this->pointer(), ptr, count * sizeof(T));
            this->free(ptr);
        }
    }

    void insertSpace(U32 amount, U32 offset) {
        auto required = count + amount;
        if(this->probeSpace(required) < required) {
            // We need to allocate new space.
            required = resizeCount(required);
            auto ptr = this->pointer();
            this->alloc(required);

            // Copy the first part.
            memcpy(this->pointer(), ptr, offset * sizeof(T));

            // Copy the second part, with the new space in the middle.
            if(count - offset) {
                memcpy(this->pointer() + offset + amount, ptr + offset, (count - offset) * sizeof(T));
            }
            this->free(ptr);
        } else {
            // There is enough space, so we just move the memory.
            memmove(this->pointer() + offset + amount, this->pointer() + offset, (count - offset) * sizeof(T));
        }
    }

    U32 count = 0;
};

//----------------------------------------------------------------------------------------------------------

template<class T, class A, class F>
auto map(F f, const ArrayT<T, A>& list) {
    ArrayT<decltype(f(list[0])), A> a{list.size()};
    for(auto& i : list) {a << f(i);}
    return move(a);
}

template<class T, class A, class F>
void walk(F f, const ArrayT<T, A>& list) {
    for(auto& i : list) {f(i);}
}

template<class T, class A, class F>
void modify(F f, ArrayT<T, A>& list) {
    for(auto& i : list) {f(i);}
}

template<class T, class A, class U, class F>
auto fold(F f, U start, typename ArrayT<T, A>::CI begin, typename ArrayT<T, A>::CI end) {
    if(begin != end) {
        auto next = begin; ++next;
        return fold(f, f(*begin, start), next, end);
    }
    else return start;
}

template<class T, class A, class U, class F>
auto fold(F f, U start, const ArrayT<T, A>& list) {
    return fold(f, start, list.begin(), list.end());
}

//----------------------------------------------------------------------------------------------------------

template<class T, class A>
void ArrayT<T, A>::clear() {
    modify([](T& t) {t.~T();}, *this);
    count = 0;
}

template<class T, class A>
ArrayT<T, A>::ArrayT(const ArrayT& a) {
    reserve(a.size());
    for(auto& i : a) {*this << i;}
}

template<class T, class A>
ArrayT<T, A>::ArrayT(ArrayT&& a): A(forward<ArrayT<T, A>&&>(a)) {
    if(A::hasSwap::value) {
        this->swap(a);
        ::swap(this->count, a.count);
    } else {
        reserve(a.size());
        for(auto& i : a) {*this << i;}
    }
}

template<class T, class A>
ArrayT<T, A>& ArrayT<T, A>::operator = (ArrayT<T, A> a) {
    if(A::hasSwap::value) {
        this->swap(a);
        ::swap(this->count, a.count);
    } else {
        reserve(a.size());
        for(auto& i : a) {*this << i;}
    }
    return *this;
}

//----------------------------------------------------------------------------------------------------------

template<class T> struct MallocArrayAllocator {
    using hasSwap = trueConstant;

    void alloc(U32 size) {
        ptr = (T*)malloc(size * sizeof(T));
        length = size;
    }

    void free(T* p) {
        ::free(p);
    }

    void destroy() {
        free(ptr);
        ptr = nullptr;
        length = 0;
    }

    void swap(MallocArrayAllocator& a) {
        ::swap(ptr, a.ptr);
        ::swap(length, a.length);
    }

    T* pointer() {return ptr;}
    const T* pointer() const {return ptr;}
    U32 space() const {return length;}
    U32 probeSpace(U32 needed) const {return length;}

private:
    T* ptr = nullptr;
    U32 length = 0;
};

template<class T, U32 size> struct FixedArrayAllocator {
    using hasSwap = falseConstant;
    void swap(FixedArrayAllocator&) {}

    void alloc(U32) {
        assert("Array overflow" == 0);
    }

    void free(T*) {}
    void destroy() {}

    T* pointer() {return (T*)data;}
    const T* pointer() const {return (const T*)data;}
    U32 space() const {return size;}
    U32 probeSpace(U32 needed) const {return size;}

private:
    Uninitialized<T> data[size];
};

template<class T, class Allocator> struct ArrayAllocator {
    using hasSwap = falseConstant;
    void swap(ArrayAllocator& a) {}

    ArrayAllocator(Allocator& allocator): allocator(allocator) {}

    void alloc(U32 size) {
        ptr = (T*)allocator.alloc(size * sizeof(T));
        length = size;
    }

    void free(T* p) {
        allocator.free(p);
    }

    void destroy() {
        free(ptr);
        ptr = nullptr;
        length = 0;
    }

    T* pointer() {return ptr;}
    const T* pointer() const {return ptr;}
    U32 space() const {return length;}
    U32 probeSpace(U32 needed) const {return length;}

private:
    Allocator& allocator;
    T* ptr = nullptr;
    U32 length = 0;
};

//-----------------------------------------------------------------------------------------------------------

template<class T>
using Array = ArrayT<T, MallocArrayAllocator<T>>;

template<class T, U32 size>
using ArrayF = ArrayT<T, FixedArrayAllocator<T, size>>;

/*
 * Helper functions for free bit sets.
 */

typedef Byte* BitSetData;

/// Sets the value of the bit in Bits at Index.
void setBit(BitSetData bits, Size index, Bool isSet);

/// Returns the value of the bit in Bits at Index.
bool getBit(BitSetData bits, Size index);

/// Returns the size in bytes of a bit set with the specified number of bits (4-byte aligned).
Size getBitSize(Size count);

// Provides a list of one-bit flags for efficient storage of booleans.
struct BitSet {
    struct Ref {
        Ref(BitSet& set, Size index) : set(set), index(index) {}

        Ref& flip() {
            set.flip(index);
            return *this;
        }

        Ref& operator = (bool v) {
            set.set(index, v);
            return *this;
        }

        bool operator ~ () const {
            return !((const BitSet&)set).get(index);
        }

        operator bool() const {
            return ((const BitSet&)set).get(index);
        }

    private:
        BitSet& set;
        Size index;
    };

    /// Initializes to an empty bit set.
    /// You must call create() before adding bits.
    BitSet() = default;

    /// Initializes the bit set with space for numItems bits.
    BitSet(Size numItems) {
        create(numItems);
    }

    ~BitSet() {
        destroy();
    }

    /// Initializes the bit set with space for numItems bits.
    /// The previous contents are destroyed.
    void create(Size numItems);

    /// Sets the amount of space available without destroying the existing data.
    void resize(Size count);

    /// Sets the amount of space available and clears all existing data.
    void resizeClear(Size count);

    /// Sets the amount of space available and sets all existing data to ones.
    void resizeSet(Size count);

    /// Removes and frees the contents of the list.
    void destroy();

    /**
     * Sets the bit at the provided index to the provided value.
     * @param index The index of the bit. Must be lower than the number of bits in the set.
     * @param isSet True if the bit should be set to true.
     */
    void set(Size index, bool isSet);

    Ref get(Size index);

    /// Returns the contents of the bit at the provided index.
    bool get(Size index) const;
    bool flip(Size index);

    Byte* getBits() {
        return data;
    }

    /**
     * Returns the contents of the bit at the provided index.
     * Warning - you cannot use this to set the bit.
     */
    bool operator[](Int index) const {
        return get(index);
    }

    Ref operator[](Int index) {
        return get(index);
    }

private:
    Byte* data = nullptr;
    Size maxItems = 0;
};

template<Size Count>
struct BitSetF {
    struct Ref {
        Ref(BitSetF<Count>& set, Size index) : set(set), index(index) {}

        Ref& Flip() {
            set.flip(index);
            return *this;
        }

        Ref& operator = (bool v) {
            set.set(index, v);
            return *this;
        }

        bool operator ~ () const {
            return !set.get(index);
        }

        operator bool() const {
            return set.get(index);
        }

    private:
        BitSetF<Count>& set;
        Size index;
    };

    BitSetF() {
        memset(data, 0, Count);
    }

    void set(Size index, bool isSet) {
        assert(index < Count);

        // Get index.
        auto index1 = index / 8;
        auto index2 = index % 8;

        if(isSet) {
            data[index1] |= (1 << index2);
        } else {
            data[index1] &= ~(1 << index2);
        }
    }

    bool get(Size index) const {
        assert(index < Count);

        // Get index.
        auto index1 = index / 8;
        auto index2 = index % 8;

        return (data[index1] & (1 << index2)) != 0;
    }

    Ref get(Size index) {
        return{*this, index};
    }

    bool flip(Size index) {
        assert(index < Count);

        // Get index.
        auto index1 = index / 8;
        auto index2 = index % 8;

        data[index1] ^= (1 << index2);
        return (data[index1] & (1 << index2)) != 0;
    }

    bool operator[] (Size index) const {
        return get(index);
    }

    Ref operator[] (Size index) {
        return get(index);
    }

private:
    Byte data[(Count / 8) + ((Count % 8) ? 1 : 0)];
};
