#pragma once

#include "region.h"

namespace mir {

/*
 * Content-keyed intern tables - the shape every one in this stage shares.
 *
 * Four things intern by content here: an ownership shape, a pointer type, a record instantiation and
 * a specialization. Each of them wants a handle that is *identity* - `sameType` is a handle
 * comparison, a manifestation key is a shape handle, `Boxed(I32)` written twice is one record - and
 * each of them has only a hash to find it with.
 *
 * A hash is not an identity, and that is the whole of what this file exists to state once. A bucket
 * that already holds different contents has to be stepped over rather than shared, because a
 * collision that silently returns the wrong entry fails exactly as silently as a truncated key. The
 * three sites that wrote the probe out longhand agreed about it, which is the position a rule is in
 * just before one of them stops agreeing.
 */

/*
 * FNV-1a over a sequence of machine words.
 *
 * The words are handles and packed fields - the offsets of arena pointers, a contract's bits, a
 * path step. Not a general hash of arbitrary bytes: what is being keyed is always a small list of
 * numbers that are already canonical, so mixing them is the whole of the key.
 */
struct InternKey {
    U64 hash = 0xcbf29ce484222325ull;

    InternKey& mix(U64 value) {
        hash = (hash ^ value) * 0x100000001b3ull;
        return *this;
    }

    // A separator between the entries of a variable-length key, so that two different groupings of
    // the same words - one slot with two steps, two slots with one - do not key alike.
    InternKey& next() { return mix(0x9e3779b97f4a7c15ull); }
};

/*
 * The slot one content-keyed entry lives in, found or made.
 *
 * `same` is asked of an entry that is already there and answers whether it holds *these* contents;
 * a false steps to the next bucket. `created` says which of the two happened, which is what the
 * caller needs to know whether it has to fill the slot in.
 *
 * The slot rather than the value, for the reason `HashMap::add` hands one back: a caller that
 * constructs the entry has to be able to store it *before* it finishes building it, so that a
 * structure which reaches itself - a recursive generic, a record holding a pointer to its own type -
 * finds a handle rather than recursing forever.
 */
template<class T, class Same>
T* internSlot(HashMap<U64, T>& table, InternKey key, bool& created, Same&& same) {
    auto hash = key.hash;

    for(;;) {
        // All ones is `HashMap`'s empty-key sentinel, so a key that lands on it takes the next
        // bucket rather than being uninsertable.
        if(hash == ~U64(0)) hash++;

        auto entry = table.add(hash);
        if(!entry.existed) {
            created = true;
            return entry.value;
        }

        if(same(*entry.value)) {
            created = false;
            return entry.value;
        }

        hash++;
    }
}

} // namespace mir
