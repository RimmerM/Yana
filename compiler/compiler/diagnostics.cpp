#include "diagnostics.h"

void PrintDiagnostics::message(Level level, StringBuffer text, const Node* where, StringBuffer source) {
    Diagnostics::message(level, text, where, source);

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

    if(source.length > 0 && where) {
        // Find the range of text to display.
        auto offset = where->sourceStart.offset;
        auto lineStart = source.ptr + offset;
        while(lineStart > source.ptr && *lineStart != '\n' && (offset - (lineStart - source.ptr) < 50)) {
            lineStart--;
        }

        if(*lineStart == '\n') {
            lineStart++;
        }

        auto lineEnd = source.ptr + offset;
        while(*lineEnd && *lineEnd != '\n' && ((lineStart - source.ptr) - offset < 50)) {
            lineEnd++;
        }

        // Make sure to handle edge cases like empty lines.
        if(lineEnd < lineStart) {
            lineEnd = lineStart;
        }

        // Print the line the diagnostic occurred at.
        auto length = Size(lineEnd - lineStart);
        println({lineStart, length});

        // Print the location within the line.
        char buffer[128];
        setMem(buffer, 128, ' ');
        auto bufferStart = lineStart - source.ptr;
        buffer[offset - bufferStart] = '^';

        auto tokenLength = where->sourceEnd.offset - offset;
        if(tokenLength > 1) {
            for(auto i = offset - bufferStart + 1; i < offset - bufferStart + tokenLength - 1; i++) {
                buffer[i] = '~';
            }
            buffer[offset - bufferStart + tokenLength - 1] = '^';
        }

        println({buffer, 128});
    }
}