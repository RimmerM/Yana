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

    template<class... T>
    void warning(const char* text, const Node* where, const char* source, T&&... format) {
        message(WarningLevel, text, where, source, forward<T>(format)...);
    }

    template<class... T>
    void error(const char* text, const Node* where, const char* source, T&&... format) {
        message(ErrorLevel, text, where, source, forward<T>(format)...);
    }

    virtual void message(Level level, const char* text, const Node* where, const char* source, ...) {
        if(level == WarningLevel) warnings++;
        else if(level == ErrorLevel) errors++;
    }

    U32 warningCount() {return warnings;}
    U32 errorCount() {return errors;}

private:
    U32 warnings = 0;
    U32 errors = 0;
};

struct PrintDiagnostics: Diagnostics {
    void message(Level level, const char* text, const Node* where, const char* source, ...) override;
};