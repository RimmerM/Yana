#pragma once

#include <Core.h>

using namespace Tritium;
using Id = U32;

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

struct SourceProvider {
    virtual StringBuffer getSource(Id module) = 0;
};

struct Diagnostics {
    enum Level {
        MessageLevel,
        WarningLevel,
        ErrorLevel,
    };

    explicit Diagnostics(SourceProvider& provider): provider(provider) {}

    template<class... T>
    void warning(StringBuffer text, const Node* where, T&&... format) {
        message(WarningLevel, text, where, forward<T>(format)...);
    }

    template<class... T>
    void error(StringBuffer text, const Node* where, T&&... format) {
        message(ErrorLevel, text, where, forward<T>(format)...);
    }

    template<class... T>
    void message(Level level, StringBuffer text, const Node* where, T&&... format) {
        char buffer[4000];
        text = {buffer, Size(formatString(toBuffer(buffer), text, forward<T>(format)...) - buffer)};
        message(level, text, where);
    }

    virtual void message(Level level, StringBuffer text, const Node* where) {
        if(level == WarningLevel) warnings++;
        else if(level == ErrorLevel) errors++;
    }

    U32 warningCount() {return warnings;}
    U32 errorCount() {return errors;}

protected:
    SourceProvider& provider;
    U32 warnings = 0;
    U32 errors = 0;
};

struct PrintDiagnostics: Diagnostics {
    using Diagnostics::Diagnostics;
    void message(Level level, StringBuffer text, const Node* where) override;
};