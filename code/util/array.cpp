#include "array.h"

static Size getBoolCount(Size bits) {
    Size numBools = bits / 8;
    numBools += (bits % 8) ? 1 : 0;
    return numBools;
}

void setBit(BitSetData bits, Size index, Bool isSet) {
    // Get index.
    Size index1 = index / 8;
    Size index2 = index % 8;

    if(isSet) {
        bits[index1] |= (1 << index2);
    } else {
        bits[index1] &= ~(1 << index2);
    }
}

bool getBit(BitSetData bits, Size index) {
    // Get index.
    Size index1 = index / 8;
    Size index2 = index % 8;

    if(bits[index1] & (1 << index2)) {
        return true;
    } else {
        return false;
    }
}

Size getBitSize(Size count) {
    Size numBytes = count / 8;
    numBytes += (count % 32) ? 1 : 0;
    return numBytes;
}

void BitSet::create(Size numItems) {
    destroy();

    Size numBools = getBoolCount(numItems);

    data = (Byte*)malloc(numBools);
    maxItems = numItems;

    memset(data, 0, numBools);
}

/// Sets the amount of space available without destroying the existing data.
void BitSet::resize(Size count) {
    if(maxItems < count) {
        Size numBools = getBoolCount(count);
        Size current = maxItems / 8;
        data = (Byte*)realloc(data, numBools);
        maxItems = count;
        memset(data + current, 0, numBools - current);
    }
}

/// Sets the amount of space available and clears all existing data.
void BitSet::resizeClear(Size count) {
    Size numBools = getBoolCount(count);
    if(maxItems < count) {
        data = (Byte*)malloc(numBools);
        maxItems = count;
    }

    memset(data, 0, numBools);
}

/// Sets the amount of space available and sets all existing data to ones.
void BitSet::resizeSet(Size count) {
    Size numBools = getBoolCount(count);
    if(maxItems < count) {
        data = (Byte*)malloc(numBools);
        maxItems = count;
    }

    memset(data, 0xff, numBools);
}

/// Removes and frees the contents of the list.
void BitSet::destroy() {
    if(data) {
        ::free(data);
        data = 0;
    }
}

/**
 * Sets the bit at the provided index to the provided value.
 * @param index The index of the bit. Must be lower than the number of bits in the set.
 * @param isSet True if the bit should be set to true.
 */
void BitSet::set(Size index, bool isSet) {
    assert(index < maxItems);

    // Get index.
    Size index1 = index / 8;
    Size index2 = index % 8;

    if(isSet) {
        data[index1] |= (1 << index2);
    } else {
        data[index1] &= ~(1 << index2);
    }
}

BitSet::Ref BitSet::get(Size index) {
    return{*this, index};
}

/// Returns the contents of the bit at the provided index.
bool BitSet::get(Size index) const {
    assert(index < maxItems);

    // Get index.
    Size index1 = index / 8;
    Size index2 = index % 8;

    return (data[index1] & (1 << index2)) != 0;
}

bool BitSet::flip(Size index) {
    assert(index < maxItems);

    // Get index.
    auto index1 = index / 8;
    auto index2 = index % 8;

    data[index1] ^= (1 << index2);
    return (data[index1] & (1 << index2)) != 0;
}