#include "diagnostics.h"

void CollectDiagnostics::message(Level level, StringView text, const Location* where) {
    Diagnostics::message(level, text, where);

    messages.push(Diagnostic {
        level,
        Tritium::ownedString(text.ptr, text.length),
        where ? *where : Location {},
        where != nullptr,
    });
}

void PrintDiagnostics::message(Level level, StringView text, const Location* where) {
    Diagnostics::message(level, text, where);

    U32 line = 0, column = 0;
    if(where) {
        line = where->sourceStart.line;
        column = where->sourceStart.column;
    }

    const char* type;
    switch(level) {
        case ErrorLevel: type = "error"; break;
        case WarningLevel: type = "warning"; break;
        case MessageLevel:
        default: type = "";
    }

    print("%@:%@: %@: ", line + 1, column, type);
    println(text);

    if(!where) return;

    auto source = provider.getSource(where->sourceModule);
    auto offset = where->sourceStart.offset;
    if(offset >= source.length) return;

    // Find the range of text to display. The source text is not terminated, so every scan is
    // bounded by its length rather than by looking for a zero byte.
    auto lineStart = source.ptr + offset;
    while(lineStart > source.ptr && *lineStart != '\n' && (offset - Size(lineStart - source.ptr) < 50)) {
        lineStart--;
    }

    if(*lineStart == '\n') {
        lineStart++;
    }

    auto sourceEnd = source.ptr + source.length;
    auto lineEnd = source.ptr + offset;
    while(lineEnd < sourceEnd && *lineEnd != '\n' && Size(lineEnd - source.ptr) - offset < 50) {
        lineEnd++;
    }

    // Make sure to handle edge cases like empty lines.
    if(lineEnd < lineStart) {
        lineEnd = lineStart;
    }

    // Print the line the diagnostic occurred at.
    auto length = Size(lineEnd - lineStart);
    println(StringView{lineStart, length});

    // Print the location within the line. Everything here is clamped to what was printed above:
    // a location can be longer than the displayed line, or - if the node it belongs to was built
    // before its first token was consumed - can even end before it starts.
    char buffer[128];
    setMem(buffer, 128, ' ');

    auto markerStart = offset - Size(lineStart - source.ptr);
    if(markerStart >= sizeof(buffer)) return;

    buffer[markerStart] = '^';
    auto markerLength = Size(1);

    if(where->sourceEnd.offset > offset) {
        auto available = min(sizeof(buffer), length) - markerStart;
        auto tokenLength = min(Size(where->sourceEnd.offset - offset), available);

        if(tokenLength > markerLength) {
            markerLength = tokenLength;

            for(Size i = markerStart + 1; i < markerStart + markerLength; i++) {
                buffer[i] = '~';
            }

            buffer[markerStart + markerLength - 1] = '^';
        }
    }

    println(StringView{buffer, markerStart + markerLength});
}
