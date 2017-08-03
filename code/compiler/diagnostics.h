#pragma once

#include "../util/types.h"

struct Loc {
    U16 line;
    U16 column;
    U16 offset;
};

struct Node {
    Id sourceModule = 0;
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

    void warning(const char* text, const Node* where = nullptr, const char* source = nullptr) {
        message(WarningLevel, text, where, source);
    }

    void error(const char* text, const Node* where = nullptr, const char* source = nullptr) {
        message(ErrorLevel, text, where, source);
    }

    virtual void message(Level level, const char* text, const Node* where = nullptr, const char* source = nullptr) {
        if(level == WarningLevel) warnings++;
        else if(level == ErrorLevel) errors++;
    }

private:
    U32 warnings = 0;
    U32 errors = 0;
};

struct PrintDiagnostics: Diagnostics {
    void message(Level level, const char* text, const Node* where, const char* source) override;
};