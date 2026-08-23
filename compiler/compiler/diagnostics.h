#pragma once

#include <Core.h>

using namespace Tritium;

/*
 * An interned name, which is the hash of its text - see Context::addIdentifier.
 *
 * A type of its own rather than a bare U32 for two reasons. It stops a name being assigned from any
 * other index in the compiler by accident, of which there are many: a value id, a block index, a
 * type offset and a frame slot are all U32 and none of them is a name. And because the value already
 * *is* a well-mixed hash, `getHash` below hands it straight back rather than hashing a hash - which
 * is what every lookup in a HashMap keyed by one used to do, twice over, on every identifier the
 * lexer read.
 *
 * The conversion out stays implicit while the one in is explicit. That is the asymmetry worth
 * having: it catches the direction that is a mistake - a number becoming a name - while leaving the
 * hundreds of places that print one, compare one or use one as a map key alone.
 */
struct StringId {
    // Trivial on purpose: both token payloads hold one inside a union, and a union member with a
    // default constructor of its own deletes the union's. So `StringId name;` is uninitialized just
    // as the U32 it replaced was, and `StringId name {}` is the zero.
    StringId() = default;
    explicit constexpr StringId(U32 value): value(value) {}

    constexpr operator U32() const { return value; }

    constexpr bool operator == (StringId other) const { return value == other.value; }
    constexpr bool operator != (StringId other) const { return value != other.value; }

    U32 value;
};

// Identity: the value is a Murmur hash of the name's text already, so it is exactly what a bucket
// index wants and mixing it again buys nothing. Found by argument-dependent lookup from inside
// Tritium's HashMap, which is why it lives beside the type rather than with the other overloads.
inline U32 getHash(StringId id) { return id.value; }

using LocationId = U32;

static constexpr LocationId kNullLocation = maxLimit<LocationId>;

/*
 * One point in one source file.
 *
 * `offset` is the authoritative one and the only one anything outside a printed diagnostic should
 * use. `column` is not a character count in any encoding a client could name - the lexer produces
 * a byte offset within the line with tabs expanded to a tab stop - so it cannot be handed to an
 * editor, and cannot be converted to one without the source text. Anything that needs a
 * (line, character) pair computes it from `offset` against the text it already holds; see
 * LineTable in compiler/position.h and Implementation-Tooling.md §2.1.
 *
 * All three are U32. `line` and `column` were U16, which silently wrapped past 65535 - a
 * diagnostic in a long file pointed at the wrong line long before any of this existed.
 */
struct Loc {
    U32 offset;
    U32 line;
    U32 column;
};

struct Location {
    StringId sourceModule {};
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

// Where the standard library's text is kept - compiler/library.h. Declared rather than included:
// that header names a StringId, which is declared here.
struct LibrarySource;

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

    /*
     * The standard library's loaded text, or null - set by the Context that owns it.
     *
     * A `SourceProvider` answers for the files the *project* is made of, and since the library moved
     * out of the compiler's own source into `lib/` there is a second set of files a report can point
     * into. Without this, a type error in a library file printed its message and quoted nothing,
     * which is what a report from an embedded string literal did too - the difference is that there
     * is now a real file and a real line to show.
     */
    LibrarySource* library = nullptr;

    U32 warningCount() {return warnings;}
    U32 errorCount() {return errors;}

    // Forgets everything counted so far. A batch compile never calls this - it reports once and
    // exits - but a server compiles the same program over and over and each compile has to start
    // from zero, or the second keystroke reports the first one's errors again.
    virtual void reset() {
        warnings = 0;
        errors = 0;
    }

protected:
    SourceProvider& provider;
    U32 warnings = 0;
    U32 errors = 0;
};

struct PrintDiagnostics: Diagnostics {
    using Diagnostics::Diagnostics;
    void message(Level level, StringView text, const Location* where) override;
};

/*
 * One reported diagnostic, kept instead of printed.
 *
 * Both the text and the location are copies rather than references. The text a report is formatted
 * into is a stack buffer in Diagnostics::message, and `where` points into Context::locations, which
 * an allocation can move - so neither survives the call that produced it.
 *
 * `hasLocation` rather than a null `Location*`, because a report with no location is a real thing -
 * a file that cannot be opened has no line to point at - and an editor has to be able to tell that
 * from a location at offset zero.
 */
struct Diagnostic {
    Diagnostics::Level level;
    Tritium::String text;
    Location where;
    bool hasLocation;
};

/*
 * The sink an editor reads through.
 *
 * Diagnostics is virtual and the whole compiler already reports through it, so this is the entire
 * mechanism by which a language server sees what a compile found - see Implementation-Tooling.md §6.
 */
struct CollectDiagnostics: Diagnostics {
    using Diagnostics::Diagnostics;
    void message(Level level, StringView text, const Location* where) override;

    void reset() override {
        Diagnostics::reset();
        messages.clear();
    }

    Array<Diagnostic> messages;
};