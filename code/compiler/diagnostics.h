#pragma once

#include <cstdint>
#include "context.h"

struct Loc {
    U32 line;
    U32 column;
    U32 offset;
};

inline Qualified placeholderModule() {
    return Qualified {nullptr, 0};
}

struct Node {
    Qualified sourceModule = placeholderModule();
    Loc sourceStart = {0, 0, 0};
    Loc sourceEnd = {0, 0, 0};

    void locationFrom(const Node& n) {
        sourceStart.offset = n.sourceStart.offset;
        sourceStart.column = n.sourceStart.column;
        sourceStart.line = n.sourceStart.line;
        sourceEnd.offset = n.sourceEnd.offset;
        sourceEnd.line = n.sourceEnd.line;
        sourceEnd.column = n.sourceEnd.column;
        sourceModule = n.sourceModule;
    }
};

struct Diagnostics {
    enum Level {
        MessageLevel,
        WarningLevel,
        ErrorLevel,
    };

    void warning(const char* text, const Node* where) {
        message(WarningLevel, text, where);
    }

    void error(const char* text, const Node* where) {
        message(ErrorLevel, text, where);
    }

    virtual void message(Level level, const char* text, const Node* where) {
        if(level == WarningLevel) warnings++;
        else if(level == ErrorLevel) errors++;
    }

private:
    U32 warnings = 0;
    U32 errors = 0;
};

struct PrintDiagnostics: Diagnostics {
    void message(Level level, const char* text, const Node* where) override;
};