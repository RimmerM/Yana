#include "context.h"
#include "Mem/Hash.h"

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

LinearArena::LinearArena(Size maxSize) {
    reset(maxSize);
}

LinearArena::LinearArena(LinearArena&& l) noexcept: base(l.base), p(l.p), max(l.max) {
    l.base = nullptr;
    l.max = nullptr;
    l.p = nullptr;
}

void* LinearArena::alloc(Size size) {
    auto it = p;
    if(it + size > max) [[unlikely]] {
        // Every caller dereferences what this returns, so a null here is a segfault somewhere else -
        // in a batch compile, on an enormous program; in a language server, on the user's next
        // keystroke, where it is reported as a plugin bug. A message naming the region and the size
        // is a bug report; a segfault is a shrug. See Implementation-Tooling.md §4.2.
        fatalError("Arena exhausted: %@ more bytes requested with %@ of %@ used. "
                   "Raise the region size this arena was constructed with.",
                   size, Size(p - base), Size(max - base));
    }

    p += size;
    return it;
}

void LinearArena::reset(Size maxSize) {
    p = base;

    if(max - base < maxSize) {
        if(max > base) {
            releaseMem(base, max - base);
        }

        auto pageSize = getPageSize();
        if(maxSize < pageSize) maxSize = pageSize;

        auto result = allocMem(maxSize);
        if(!result) {
            logError("Cannot allocate %@ bytes for arena", maxSize);
            base = nullptr;
            p = nullptr;
            max = nullptr;
        } else {
            base = (Byte*)result.unwrapOk();

            // Start at a small offset to make it easier to represent null pointers into the arena.
            p = base + 16;
            max = base + maxSize;
        }
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
    return hash.get();
}

StringId Context::nameHash(const StringView& v) {
    Tritium::Hasher hash;
    hash.addBytes(v.ptr, v.length);
    return hash.get();
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
        i = id.segmentHash;
    } else {
        Hasher hash;
        hash.addBytes(id.text, id.textLength);
        i = hash.get();
    }

    identifiers.add(i, id);
    return i;
}

Identifier& Context::find(StringId id) {
    return identifiers[id];
}

String Context::findName(StringId id) {
    auto v = find(id);
    if(v.textLength) {
        return String(v.text, v.textLength);
    } else {
        return "";
    }
}