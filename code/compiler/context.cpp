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

Id Context::addUnqualifiedName(const char* chars, Size count) {
    Qualified q;
    q.name = chars;
    q.length = count;
    return addName(&q);
}

Id Context::addName(Qualified* q) {
    Hasher h;

    auto qu = q;
    while(qu) {
        h.addData(qu->name, (qu->length * sizeof(*qu->name)));
        qu = qu->qualifier;
    }

    return addName(h.get(), q);
}