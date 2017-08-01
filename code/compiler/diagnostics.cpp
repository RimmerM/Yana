#include <cstdio>
#include "diagnostics.h"

void PrintDiagnostics::message(Level level, const char* text, const Node* where, const char* source) {
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

    printf("%i:%i: %s: %s\n", line, column, type, text);

    if(source && where) {
        // Find the range of text to display.
        auto offset = where->sourceStart.offset;
        auto lineStart = source + offset;
        while(lineStart > source && *lineStart != '\n' && (offset - (lineStart - source) < 50)) {
            lineStart--;
        }

        if(*lineStart == '\n') {
            lineStart++;
        }

        auto lineEnd = source + offset;
        while(*lineEnd && *lineEnd != '\n' && ((lineStart - source) - offset < 50)) {
            lineEnd++;
        }

        // Make sure to handle edge cases like empty lines.
        if(lineEnd < lineStart) {
            lineEnd = lineStart;
        }

        // Print the line the diagnostic occurred at.
        char buffer[128];
        auto length = lineEnd - lineStart;
        memcpy(buffer, lineStart, length);
        buffer[length] = 0;
        printf("%s\n", buffer);

        // Print the location within the line.
        memset(buffer, ' ', length);
        auto bufferStart = lineStart - source;
        buffer[offset - bufferStart] = '^';

        auto tokenLength = where->sourceEnd.offset - offset;
        if(tokenLength > 1) {
            for(auto i = offset - bufferStart + 1; i < offset - bufferStart + tokenLength; i++) {
                buffer[i] = '~';
            }
            buffer[offset - bufferStart + tokenLength] = '^';
        }

        printf("%s\n", buffer);
    }
}