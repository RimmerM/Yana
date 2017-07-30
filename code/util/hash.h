#pragma once

#include "types.h"

/**
 * Creates a 32-bit hash from various input data using an SBox-algorithm.
 * Used for hash tables, unique identifiers, etc. Do not use for secure hashing!
 */
struct Hasher {
    Hasher() = default;

    U32 get() const {return hash;}

    void addData(const void* data, Size count);
    void addString(const Char* string);
    void addString(const WChar* string);
    void addString(const WChar32* string);

    void add(U32);
    void add(I32);
    void add(F32);
    void add(F64);

    template<typename T> void add(const T& data) {addData(&data, sizeof(T));}

    explicit operator U32() const {return get();}

private:
    U32 hash = 0;
};

template<class T>
inline Hasher& operator << (Hasher& h, const T& data) {
    h.add(data);
    return h;
}

/// Default hash implementation for objects.
/// Can be specialized for specific types.
template<class T>
inline U32 hash(const T& t) {
    Hasher h;
    h.add(t);
    return (U32)h;
}