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

    // The path a file was read from, as the compiler was given it - what a printed diagnostic
    // names so that a terminal or an IDE can open the place it points at. Empty where there is no
    // file behind the text, which is why this has a default rather than being owed by every
    // provider: a test harness compiling a string has nothing to answer with.
    virtual StringView getPath(StringId file) { return {}; }
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
        if(swallowed(level)) return;

        char buffer[4000];
        text = {buffer, Tritium::format(toBuffer(buffer), toString(text), forward<T>(format)...)};
        message(level, text, where);
    }

    template<class... T>
    void message(Level level, StringView text, const Location* where, T&&... format) {
        if(swallowed(level)) return;

        char buffer[4000];
        text = {buffer, Tritium::format(toBuffer(buffer), toString(text), forward<T>(format)...)};
        message(level, text, where);
    }

    void message(Level level, StringView text, LocationId where) {
        if(swallowed(level)) return;
        message(level, text, provider.getNode(where));
    }

    /*
     * A question asked by trying - Design-Replacements.md §3.3.
     *
     * A replacement's extra constraints are a *condition*: `Num(Vec(String))` failing means the
     * baseline runs, not that the program is wrong. The rules that decide whether one holds are the
     * ones that report when a program writes the same thing, and a second, silent copy of each of
     * them is a second rule that can come to disagree. So the question is asked of the reporting
     * code with the reports swallowed, and what is read back is whether any were: a condition holds
     * when proving it both succeeded and said nothing.
     *
     * Nested, because a condition may be asked while another is. Only the three entry points above
     * look at it; the virtual sink below is what a report reaches *after* passing them.
     */
    struct Silence {
        explicit Silence(Diagnostics& diagnostics):
            diagnostics(diagnostics), before(diagnostics.swallowedErrors) {
            diagnostics.silenced++;
        }

        ~Silence() { diagnostics.silenced--; }

        // Whether an error was swallowed since this was opened.
        bool reported() const { return diagnostics.swallowedErrors != before; }

        Diagnostics& diagnostics;
        U32 before;
    };

    /*
     * The rest of a walk, silenced - and armed part way through it rather than at its start.
     *
     * `Silence` covers a question that is asked and answered, so its scope is written where the
     * question is. This one is for a walk that turns out, some way in, to be reporting the
     * consequences of a mistake that is already reported - see `Resolver::quietenRest`, which is
     * what arms one. Disarmed until then, so a walk that never reaches such a use is a walk this
     * costs a branch.
     *
     * `suspend` and `resume` are for the walk that *contains* another: a body resolved on demand
     * from inside a quietened one is not about that mistake, so it is heard. They move only this
     * object's own hold on the count, which is why a `Silence` a condition opened around all of it
     * is left exactly as it was.
     */
    struct QuietRest {
        QuietRest() = default;
        QuietRest(const QuietRest&) = delete;
        ~QuietRest() { release(); }

        void arm(Diagnostics& hold) {
            if(diagnostics) return;

            diagnostics = &hold;
            hold.silenced++;
        }

        void release() {
            if(!diagnostics) return;

            diagnostics->silenced--;
            diagnostics = nullptr;
        }

        void suspend() { if(diagnostics) diagnostics->silenced--; }
        void resume() { if(diagnostics) diagnostics->silenced++; }

        Diagnostics* diagnostics = nullptr;
    };

    /*
     * The reports of work whose result **outlives the question** - heard even inside a `Silence`.
     *
     * A silent question may build things that are kept: a record instantiated while a condition's
     * types are substituted is interned and handed to the next use of the same type. Its fields are
     * checked once, when it is built, so an error swallowed there would be swallowed for the program
     * too - `Maybe(Box(Bool))` written later finds the record already made and nothing reports it.
     * So the building is heard: a condition that forms an ill-formed record is an error in the
     * program rather than a condition that failed.
     */
    struct Heard {
        explicit Heard(Diagnostics& diagnostics): diagnostics(diagnostics), before(diagnostics.silenced) {
            diagnostics.silenced = 0;
        }

        ~Heard() { diagnostics.silenced = before; }

        Diagnostics& diagnostics;
        U32 before;
    };

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

    /*
     * The source text one node spans - Design-Test.md §11.1's F2.
     *
     * Here rather than beside the resolver because the two things it needs are here: which file a
     * node belongs to and where that file's text is, and the library's text is somewhere the
     * `SourceProvider` does not answer for - the same pair `message` below has to put together to
     * quote the line an error is on.
     *
     * Empty for a node with no location, one whose module has no text loaded, and one whose span is
     * degenerate. Every caller is filling a constant, so an empty answer is a `""` rather than a
     * failure.
     */
    StringView sourceText(LocationId where);

    // The node itself, for a caller that wants its file or its line rather than the text it spans -
    // the other half of what F2's `Site` fill is built from. Null for a node with no location.
    const Location* sourceNode(LocationId where) { return provider.getNode(where); }

protected:
    SourceProvider& provider;
    U32 warnings = 0;
    U32 errors = 0;

    // See `Silence`.
    U32 silenced = 0;
    U32 swallowedErrors = 0;

    bool swallowed(Level level) {
        if(!silenced) return false;
        if(level == ErrorLevel) swallowedErrors++;
        return true;
    }
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