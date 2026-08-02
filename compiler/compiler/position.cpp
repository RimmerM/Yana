#include "position.h"

// Start ascending, end descending, and creation order for a tie. The second key is what puts an
// enclosing range before everything it encloses, which is what makes the backward walk find the
// innermost node first.
static bool orderedBefore(const PositionIndex::Entry& a, const PositionIndex::Entry& b) {
    if(a.start != b.start) return a.start < b.start;
    if(a.end != b.end) return a.end > b.end;
    return a.location < b.location;
}

void PositionIndex::build(const Context& context, StringId moduleName) {
    module = moduleName;
    entries.clear();
    wasOrdered = true;

    auto& locations = context.allLocations();
    for(U32 i = 0; i < locations.size(); i++) {
        auto& location = locations[i];
        if(location.sourceModule != moduleName) continue;

        // A range that ends before it starts is not a range. The parser builds a few of those -
        // a node whose location was taken before its first token was consumed - and they would
        // otherwise be entries that can never be found and can still be walked past.
        auto start = location.sourceStart.offset;
        auto end = location.sourceEnd.offset;
        if(end < start) end = start;

        entries.push(Entry { start, end, 0, LocationId(i) });
    }

    // An insertion sort, because the input is in source order already: the lexer only moves
    // forward, so the pass above produces a sorted array and this is one comparison per entry.
    // Anything that builds a location out of order costs a shift, and nothing costs a full sort.
    for(U32 i = 1; i < entries.size(); i++) {
        auto entry = entries[i];
        auto j = i;
        while(j > 0 && orderedBefore(entry, entries[j - 1])) {
            entries[j] = entries[j - 1];
            j--;
        }

        if(j != i) {
            wasOrdered = false;
            entries[j] = entry;
        }
    }

    U32 maxEnd = 0;
    for(auto& entry: entries) {
        if(entry.end > maxEnd) maxEnd = entry.end;
        entry.maxEnd = maxEnd;
    }
}

// The last entry whose start is at or before `offset`, or -1 when there is none.
static I64 lastStartingAt(const Array<PositionIndex::Entry>& entries, U32 offset) {
    I64 low = 0, high = I64(entries.size()) - 1, found = -1;

    while(low <= high) {
        auto mid = low + (high - low) / 2;
        if(entries[Size(mid)].start <= offset) {
            found = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    return found;
}

LocationId PositionIndex::find(U32 offset) const {
    for(auto i = lastStartingAt(entries, offset); i >= 0; i--) {
        auto& entry = entries[Size(i)];
        if(entry.end > offset) return entry.location;
        if(entry.maxEnd <= offset) break;
    }

    return kNullLocation;
}

void PositionIndex::findEnclosing(U32 offset, Array<LocationId>& into) const {
    for(auto i = lastStartingAt(entries, offset); i >= 0; i--) {
        auto& entry = entries[Size(i)];
        if(entry.end > offset) into.push(entry.location);
        if(entry.maxEnd <= offset) break;
    }
}

/*
 * Lines.
 */

void LineTable::build(StringView text) {
    lineStarts.clear();
    lineStarts.push(0);

    for(Size i = 0; i < text.length; i++) {
        // Only `\n` starts a line. A lone `\r` as a line terminator is a Mac OS 9 file, and a
        // `\r\n` is handled by this because the `\n` is what is counted - the `\r` stays on the
        // line before it, where an editor counting characters also puts it.
        if(text.ptr[i] == '\n') lineStarts.push(U32(i + 1));
    }
}

U32 LineTable::lineOf(U32 offset) const {
    I64 low = 0, high = I64(lineStarts.size()) - 1, found = 0;

    while(low <= high) {
        auto mid = low + (high - low) / 2;
        if(lineStarts[Size(mid)] <= offset) {
            found = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    return U32(found);
}

U32 LineTable::lineStart(U32 line) const {
    if(line >= lineStarts.size()) return lineStarts.size() ? lineStarts[lineStarts.size() - 1] : 0;
    return lineStarts[line];
}

// How many UTF-16 code units one UTF-8 lead byte stands for. A continuation byte is part of a
// character that was already counted, so it is worth none; a four-byte sequence is a surrogate
// pair, so it is worth two. Everything else is one.
static U32 utf16Units(char c) {
    auto byte = (Byte)c;
    if(byte < 0x80) return 1;
    if(byte < 0xC0) return 0;
    if(byte < 0xF0) return 1;
    return 2;
}

U32 LineTable::utf16Column(StringView text, U32 offset) const {
    if(offset > text.length) offset = U32(text.length);
    auto start = lineStart(lineOf(offset));

    U32 column = 0;
    for(auto i = start; i < offset; i++) {
        column += utf16Units(text.ptr[i]);
    }

    return column;
}

U32 LineTable::offsetAt(StringView text, U32 line, U32 character, bool utf16) const {
    auto start = lineStart(line);
    auto end = line + 1 < lineStarts.size() ? lineStarts[line + 1] : U32(text.length);
    if(end > text.length) end = U32(text.length);

    // The terminator is not part of the line as far as a position is concerned: a client asking for
    // a character past the end of a line means the end of that line, not the start of the next.
    while(end > start && (text.ptr[end - 1] == '\n' || text.ptr[end - 1] == '\r')) end--;

    if(!utf16) {
        auto offset = start + character;
        return offset < end ? offset : end;
    }

    U32 column = 0;
    for(auto i = start; i < end; i++) {
        if(column >= character) return i;
        column += utf16Units(text.ptr[i]);
    }

    return end;
}
