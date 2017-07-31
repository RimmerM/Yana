#include <printf.h>
#include "diagnostics.h"

void PrintDiagnostics::message(Level level, const char* text, const Node* where) {
    printf("%s\n", text);
}