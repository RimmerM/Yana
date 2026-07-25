#pragma once

#include <Core.h>

using namespace Tritium;
using StringId = U32;
using LocationId = U32;

static constexpr LocationId kNullLocation = maxLimit<LocationId>;

struct Loc {
    U32 offset;
    U16 line;
    U16 column;
};

struct Location {
    StringId sourceModule = 0;
    Loc sourceStart = {0, 0, 0};
    Loc sourceEnd = {0, 0, 0};

    void locationFrom(const Location& n) {
        sourceStart.offset = n.sourceStart.offset;
        sourceStart.column = n.sourceStart.column;
        sourceStart.line = n.sourceStart.line;
        sourceEnd.offset = n.sourceEnd.offset;
        sourceEnd.line = n.sourceEnd.line;
        sourceEnd.column = n.sourceEnd.column;
        sourceModule = n.sourceModule;
    }
};

struct SourceManager {

};

struct SourceProvider {
    virtual StringView getSource(StringId module) = 0;
    virtual const Location* getNode(LocationId node) = 0;
};

struct Diagnostics {
    enum Level {
        MessageLevel,
        WarningLevel,
        ErrorLevel,
    };

    explicit Diagnostics(SourceProvider& provider): provider(provider) {}

    template<class... T>
    void warning(StringView text, LocationId where, T&&... format) {
        message(WarningLevel, text, where, forward<T>(format)...);
    }

    template<class... T>
    void warning(StringView text, const Location* where, T&&... format) {
        message(WarningLevel, text, where, forward<T>(format)...);
    }

    template<class... T>
    void error(StringView text, LocationId where, T&&... format) {
        message(ErrorLevel, text, where, forward<T>(format)...);
    }

    template<class... T>
    void error(StringView text, const Location* where, T&&... format) {
        message(ErrorLevel, text, where, forward<T>(format)...);
    }

    template<class... T>
    void message(Level level, StringView text, LocationId where, T&&... format) {
        char buffer[4000];
        text = {buffer, Tritium::format(toBuffer(buffer), toString(text), forward<T>(format)...)};
        message(level, text, where);
    }

    template<class... T>
    void message(Level level, StringView text, const Location* where, T&&... format) {
        char buffer[4000];
        text = {buffer, Tritium::format(toBuffer(buffer), toString(text), forward<T>(format)...)};
        message(level, text, where);
    }

    void message(Level level, StringView text, LocationId where) {
        message(level, text, provider.getNode(where));
    }

    virtual void message(Level level, StringView text, const Location* where) {
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
    void message(Level level, StringView text, const Location* where) override;
};