#include "context.h"
#include "stage.h"
#include "Mem/Hash.h"

StageObserver* gStageObserver = nullptr;

void* Arena::alloc(Size size) {
    if(buffer + size > max) {
        buffer = (Byte*)Tritium::hAlloc(kChunkSize);
        max = buffer + kChunkSize;
        buffers.push(buffer);
    }

    auto it = buffer;
    buffer += size;

    return it;
}

void Arena::reset() {
    if(buffers.size() == 0) return;

    // Remove all but one buffer.
    for(U32 i = 1; i < buffers.size(); i++) {
        Tritium::hFree(buffers[i]);
    }

    buffer = buffers[0];
    max = buffer + kChunkSize;

    buffers.clear();
    buffers.push(buffer);
}

Arena::~Arena() {
    for(auto buffer: buffers) {
        Tritium::hFree(buffer);
    }
    buffers.destroy();
    buffer = nullptr;
    max = nullptr;
}

LinearArena::LinearArena(Size initialCommit) {
    reset(initialCommit);
}

LinearArena::LinearArena(LinearArena&& l) noexcept: base(l.base), p(l.p), committed(l.committed), max(l.max) {
    l.base = nullptr;
    l.max = nullptr;
    l.p = nullptr;
    l.committed = nullptr;
}

void* LinearArena::alloc(Size size) {
    auto it = p;
    if(it + size > committed) [[unlikely]] commitTo(it + size, size);

    p = it + size;
    return it;
}

/*
 * Committing forward, and where the ceiling actually is.
 *
 * Geometric rather than by a fixed chunk, so that a region which grows to hundreds of megabytes
 * does not pay a system call per chunk on the way there, and one that stays small never leaves its
 * initial commit. The half-again step is bounded on both sides: at least the request, so a single
 * enormous allocation is satisfied in one call, and never past the reservation.
 */
void LinearArena::commitTo(Byte* end, Size request) {
    if(end > max) [[unlikely]] {
        // Every caller dereferences what alloc returns, so a null here is a segfault somewhere else -
        // in a batch compile, on an enormous program; in a language server, on the user's next
        // keystroke, where it is reported as a plugin bug. A message naming the region and the size
        // is a bug report; a segfault is a shrug. See Implementation-Tooling.md §4.2.
        //
        // This is now the format's ceiling and not a configured one, which is why it no longer asks
        // for a larger region: a RegionPtr is a U32 offset, so there is no larger region to ask for.
        fatalError("Arena exhausted: %@ more bytes requested with %@ used, and a region addresses at "
                   "most %@ bytes - a RegionPtr is a 32-bit offset. This program is too large to "
                   "resolve as one unit.",
                   request, Size(p - base), Size(max - base));
    }

    auto target = Size(committed - base);
    target += target / 2;

    auto needed = Size(end - base);
    if(target < needed) target = needed;

    // Once per process. `getPageSize` is a system call on some platforms and this is on the growth
    // path of every region.
    static const Size pageSize = getPageSize();
    target = (target + pageSize - 1) & ~(pageSize - 1);
    if(target > Size(max - base)) target = Size(max - base);

    auto result = commitMem(committed, base + target - committed);
    if(result != MemResult::Ok) {
        // Distinct from the exhaustion above and worth saying so: the address range was reserved, so
        // what failed is the machine having the memory to back it rather than this format running
        // out of offsets. Fatal for the same reason - the caller is about to write here.
        fatalError("Cannot commit %@ bytes for an arena that has %@ of %@ reserved bytes committed.",
                   Size(base + target - committed), Size(committed - base), Size(max - base));
    }

    committed = base + target;
}

void LinearArena::reset(Size initialCommit) {
    /*
     * The reservation is made once and kept. `reset` on a live arena rewinds and re-commits rather
     * than remapping, because the base is what every outstanding RegionPtr is an offset from - and
     * because a reservation is the one thing here that costs nothing to hold.
     */
    if(!base) {
        /*
         * The full range first, and smaller ranges after it.
         *
         * A reservation costs address space and nothing else, so the ceiling is normally the
         * format's. It is not always available: a process under an address-space limit - `ulimit
         * -v`, a container, a 32-bit host - has less of it than this asks for, and refusing to
         * compile at all there would be a worse answer than compiling with a lower ceiling. So the
         * request halves until something succeeds, and what a program then hits is the exhaustion
         * message in `commitTo`, which names the number it actually had.
         */
        Size reserve = kReserve;

        while(true) {
            auto result = reserveMem(reserve);
            if(result) {
                base = (Byte*)result.unwrapOk();
                committed = base;
                max = base + reserve;
                break;
            }

            // The floor is what the caller asked to commit, since below that this arena cannot do
            // its job at all and the failure should be reported rather than deferred.
            if(reserve <= initialCommit + 16 || reserve <= 1024 * 1024) {
                // Fatal for the reason the old allocation failure was: a null base is not a state
                // anything downstream can notice. `Region::operator*` would hand out a `RegionBase`
                // of `nullptr - 16` and every handle resolved through it would address low memory,
                // so the failure would be reported as a segfault in whatever happened to
                // dereference first - which is a shrug, several stages away from the thing that
                // actually went wrong. See Implementation-Tooling.md §4.2 and the note in
                // `commitTo`.
                fatalError("Cannot reserve %@ bytes of address space for an arena.", reserve);
            }

            reserve /= 2;
        }
    }

    // Start at a small offset to make it easier to represent null pointers into the arena.
    p = base + 16;

    // The argument is a hint and not a limit, so it is honoured only upwards: an arena being reset
    // has already committed whatever the last use of it needed, and handing that back would trade a
    // page fault per megabyte on the next compile for memory this process is about to want again.
    if(initialCommit && base + 16 + initialCommit > committed) {
        commitTo(base + 16 + initialCommit, initialCommit);
    }
}

LinearArena::~LinearArena() {
    if(max > base) releaseMem(base, max - base);
}

void Context::addOp(StringId op, U16 prec, Assoc assoc) {
    OpProperties prop{prec, assoc};
    ops[op] = prop;
}

OpProperties Context::findOp(StringId op) {
    auto res = ops.get(op);
    if(res) {
        return res.unwrap();
    } else {
        return {9, Assoc::Left};
    }
}

StringId Context::nameHash(const char* chars, Size count) {
    Tritium::Hasher hash;
    hash.addBytes(chars, count);
    return StringId(hash.get());
}

StringId Context::nameHash(const StringView& v) {
    Tritium::Hasher hash;
    hash.addBytes(v.ptr, v.length);
    return StringId(hash.get());
}

StringId Context::addUnqualifiedName(const char* chars, Size count) {
    Tritium::Hasher hash;
    hash.addBytes(chars, count);

    Identifier id;
    id.text = chars;
    id.textLength = (U32)count;
    id.segmentCount = 1;
    id.segments = nullptr;
    id.segmentHash = hash.get();
    return addIdentifier(id);
}

// Interned rather than a bare hash, because a diagnostic or a dump that happens to reach it should
// print something a reader can recognize instead of an empty name.
StringId cursorName(Context& context) {
    static const char name[] = "$cursor";
    return context.addUnqualifiedName(name, sizeof(name) - 1);
}

StringId Context::addQualifiedName(const char* chars, Size count, Size segmentCount) {
    Identifier id;

    if(segmentCount <= 1) {
        auto text = (char*)stringArena.alloc(count);
        id.text = text;
        id.textLength = (U32)count;
        copy(chars, text, count);

        Hasher hash;
        hash.addBytes(chars, count);

        id.segmentCount = 1;
        id.segments = nullptr;
        id.segmentHash = hash.get();
    } else {
        // Put the indexes and hashes first to get the correct alignment.
        auto data = (U32*)stringArena.alloc(count + 2 * (segmentCount * sizeof(U32)));

        id.segmentCount = (U32)segmentCount;
        id.segments = data;
        id.segmentHashes = data + segmentCount;

        auto name = (char*)(data + segmentCount * 2);
        copy(chars, name, count);
        id.text = name;
        id.textLength = (U32)count;

        // Set the offsets and hashes.
        auto p = chars;
        auto max = chars + count;
        for(U32 i = 0; i < segmentCount; i++) {
            id.segments[i] = (U32)(p - chars);

            Hasher hash;
            U32 segmentLength = 0;
            while(p < max && *p != '.') {
                hash.addByte(*p);
                p++;
                segmentLength++;
            }

            if(p < max && *p == '.') p++;
            id.segmentHashes[i] = hash.get();
        }
    }

    return addIdentifier(id);
}

StringId Context::addQualifiedName(const char* chars, Size count) {
    Size segmentCount = 1;
    for(Size i = 0; i < count; i++) {
        if(chars[i] == '.') segmentCount++;
    }

    return addQualifiedName(chars, count, segmentCount);
}

StringId Context::addIdentifier(const Identifier& id) {
    StringId i;
    if(id.segmentCount == 1) {
        i = StringId(id.segmentHash);
    } else {
        Hasher hash;
        hash.addBytes(id.text, id.textLength);
        i = StringId(hash.get());
    }

    identifiers.add(i, id);
    return i;
}

/*
 * A lookup, and never an insertion.
 *
 * This used to be `identifiers[id]`, which on a HashMap is get-or-add: every name resolution added
 * a row for a name it was only reading, checked the load factor, and could rehash the table from
 * inside a read. `search` in resolve/name.h asks this once per name and once per import of that
 * name, so it was several million inserts over a compilation.
 *
 * A StringId only ever comes from interning, so the miss below is unreachable in a compile that has
 * not corrupted one. It answers the way the old get-or-add did - an empty identifier, textLength
 * zero - rather than asserting, because that is what every caller here already treats as "no name".
 */
Identifier& Context::find(StringId id) {
    static Identifier none;

    auto found = identifiers.get(id);
    return found ? found.unwrap() : none;
}

String Context::findName(StringId id) {
    auto v = find(id);
    if(v.textLength) {
        return String(v.text, v.textLength);
    } else {
        return "";
    }
}