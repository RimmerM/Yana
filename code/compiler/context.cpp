#include "context.h"

void* Arena::alloc(Size size) {
    if(buffer + size > max) {
        buffer = (Byte*)malloc(kChunkSize);
        max = buffer + kChunkSize;
        buffers.push(buffer);
    }

    auto it = buffer;
    buffer += size;
    return it;
}

Arena::~Arena() {
    for(auto buffer: buffers) {
        free(buffer);
    }
    buffers.destroy();
    buffer = nullptr;
    max = nullptr;
}

void Context::addOp(Id op, U16 prec, Assoc assoc) {
    OpProperties prop{prec, assoc};
    ops[op] = prop;
}

OpProperties Context::findOp(Id op) {
    auto res = ops.get(op);
    if(res) {
        return *res;
    } else {
        return {9, Assoc::Left};
    }
}

Id Context::addUnqualifiedName(const char* chars, Size count) {
    Hasher hash;
    hash.addBytes(chars, count);

    Identifier id;
    id.text = chars;
    id.textLength = (U32)count;
    id.segmentCount = 1;
    id.segments = nullptr;
    id.segmentHash = hash.get();
    return addIdentifier(id);
}

Id Context::addIdentifier(const Identifier& id) {
    Id i;
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

Identifier& Context::find(Id id) {
    return identifiers[id];
}