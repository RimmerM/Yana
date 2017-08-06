#pragma once

#include "types.h"

/**
 * Creates a 32-bit hash from various input data using an SBox-algorithm.
 * Used for hash tables, unique identifiers, etc. Do not use for secure hashing!
 */
struct Hasher {
    Hasher() = default;

    U32 get() const {return hash;}

    void addString(const Char* string);
    void addString(const WChar* string);
    void addString(const WChar32* string);

    void addBytes(const void* data, Size count);
    void addByte(char);

    template<typename T> void add(const T& data) {addBytes(&data, sizeof(T));}

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
    return h.get();
}