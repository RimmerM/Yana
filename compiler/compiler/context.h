#pragma once

#include "diagnostics.h"
#include "library.h"
#include "settings.h"
#include <HashMap.h>

/*
 * An identifier consists of zero or more module names separated by dots, followed by the the identifier value.
 * We store the full identifier, as well as a pointer and hash to the start of each segment.
 */
struct Identifier {
    Identifier(): textLength(0), segmentCount(0) {}

    // A name, so StringId rather than U32 - this is where a segment's id comes from.
    StringId getHash(U32 index) const {
        if(segmentCount == 1) {
            return StringId(segmentHash);
        } else {
            return StringId(segmentHashes[index]);
        }
    }

    U32 getSegmentOffset(U32 index) const {
        if(segmentCount == 0) {
            return 0;
        } else if(index == segmentCount) {
            return textLength;
        } else {
            return segments[index];
        }
    }

    const char* text;

    // If `segmentCount == 1`, this is set to nullptr.
    U32* segments;

    // If `segmentCount == 1`, this stores the first hash instead of a pointer.
    union {
        U32* segmentHashes;
        U32 segmentHash;
    };

    U32 textLength: 24;
    U32 segmentCount: 8;
};

inline bool operator == (const Identifier& a, const Identifier& b) {
    if(a.textLength != b.textLength) return false;
    return Tritium::compareMem(a.text, b.text, a.textLength);
}

namespace Tritium {
    inline U32 getHash(const Identifier& identifier) {
        return getHash(StringView { identifier.text, identifier.textLength });
    }
}

// The editor-facing side tables - resolve/index.h and resolve/complete.h. Declared rather than
// included, because both are built out of the resolver's own handles and this header is below the
// resolver.
struct SemanticIndex;
struct CompletionRequest;

/*
 * Where a completion request is asking about - Implementation-Tooling.md §8.2.
 *
 * Set before anything is parsed, and read by the parser: an identifier that contains `offset` is
 * the name being typed, so the parser emits the cursor sentinel in its place and records here what
 * it emitted. `module` is zero in every ordinary compile, which is what keeps the whole mechanism
 * to one comparison per identifier token.
 *
 * The two halves are written by different stages and read by a third. The parser fills `sentinel`
 * and `prefixStart`; the resolver fills the `CompletionRequest` it reaches through
 * `Context::completion`; and the server puts the two together, since the *text* between
 * `prefixStart` and `offset` is the partial name to filter by and only the server holds the text.
 */
struct Cursor {
    StringId module {};
    U32 offset = 0;

    // The sentinel's node, so a caller can say where the answer applies, and where the partial name
    // it stands in for begins. `prefixStart == offset` is a position with nothing typed yet.
    LocationId sentinel = kNullLocation;
    U32 prefixStart = 0;

    bool isSet() const { return module != 0; }
    bool wasParsed() const { return sentinel != kNullLocation; }
};

/*
 * The name the cursor sentinel is written with.
 *
 * `$` is not an identifier character (`util/lexer_util.cpp`), so no name the lexer can produce
 * collides with it - which is what lets the resolver recognize the sentinel by name rather than by
 * carrying a flag through every expression that might hold one.
 */
StringId cursorName(struct Context& context);

enum class Assoc : U8 {
    Left,
    Right
};

/*
 * Operators can have an associated precedence and a associativity.
 * This is used for the reordering of infix-expressions.
 * TODO: What should happen if two modules define a different precedence for the same operator?
 * Maybe we should not allow custom precedences for user-defined operators.
 */
struct OpProperties {
    U16 precedence;
    Assoc associativity;
};

struct Arena {
    static const Size kChunkSize = 4 * 1024 * 1024;

    Arena() = default;
    Arena(const Arena&) = delete;

    void* alloc(Size size);
    void reset();
    ~Arena();

private:
    Byte* buffer = nullptr;
    Byte* max = nullptr;
    Array<Byte*> buffers;
};

inline void* operator new (Size count, Arena& arena) {
    return arena.alloc(count);
}

struct LinearArena;
namespace ast { struct ParseRegion; }

template<class Region, class T>
struct RegionPtr {
    U32 offset;

    // Offset 0 is never a real object - every region starts allocating past it - so a
    // default-constructed handle is the null handle.
    RegionPtr(): offset(0) {}
    explicit RegionPtr(U32 offset): offset(offset) {}
    RegionPtr(decltype(nullptr)): offset(0) {}
    RegionPtr(const RegionPtr&) = default;

    RegionPtr& operator = (const RegionPtr& p) = default;

    bool operator == (RegionPtr p) const {
        return offset == p.offset;
    }

    bool operator != (RegionPtr p) const {
        return offset != p.offset;
    }

    bool operator == (decltype(nullptr)) const {
        return offset == 0;
    }

    bool operator != (decltype(nullptr)) const {
        return offset != 0;
    }

    operator U32() const {
        return offset;
    }

    template<class U> explicit operator RegionPtr<Region, U>() const {
        return RegionPtr<Region, U>(offset);
    }
};

template<class Region>
struct RegionBase {
    // The stored base is slightly smaller than the actual base.
    // This allows us to handle null pointers without any special handling.
    Byte* base;

    explicit RegionBase(Byte* base): base(base - 16) {}
    RegionBase(const RegionBase&) = default;

    RegionBase& operator = (const RegionBase& p) {
        base = p.base;
        return *this;
    }

    template<class T>
    T* operator[](RegionPtr<Region, T> p) {
        return (T*)(base + p.offset);
    }
};

template<class Region, class T>
inline RegionPtr<Region, T> operator - (T* v, RegionBase<Region> base) {
    assertTrue(((Byte*)v) - 16 >= base.base);
    return RegionPtr<Region, T>(U32((Byte*)v - base.base));
}

/*
 * A bump allocator over a reserved address range that commits as it grows - Analysis-Modules.md §3.4.
 *
 * The reservation is the format's own ceiling rather than a guess. A `RegionPtr` is a `U32` offset
 * from a base biased by 16, so an offset addresses at most 4 GB and nothing below can hold a region
 * larger than that whatever this reserves; `kReserve` is that number, rounded down to a page. The
 * base never moves, so every `RegionPtr` and every part of the image format is untouched by this.
 *
 * What changed is which of the two numbers is the ceiling. The constructor's argument used to be
 * both the reservation and the limit, and `allocMem` commits - so a `Program` was 20 MB resident
 * from its first declaration and `fatalError` fired three orders of magnitude below where the
 * format stops. It is now the *initial commit* only: the size a compilation of ordinary size never
 * grows past, so the common case costs one `mprotect` exactly as it used to cost one `mmap`, and
 * the ones that do grow keep going to 4 GB instead of aborting.
 */
struct LinearArena {
    // 4 GB less one page. The last 16 bytes are unaddressable through a RegionPtr - offset 0 is the
    // null handle and the base is biased by 16 - so the tail page is dropped rather than reasoned
    // about, and the reservation stays page-aligned.
    static constexpr Size kReserve = 4ull * 1024 * 1024 * 1024 - 4096;

    explicit LinearArena(Size initialCommit);
    LinearArena(LinearArena&&) noexcept;
    LinearArena(const LinearArena&) = delete;

    void* alloc(Size size);
    void reset(Size initialCommit);
    Size used() { return p - base; }

    ~LinearArena();

protected:
    // Commits forward far enough to hold everything below `end`, or reports why it cannot. Out of
    // line and cold: `alloc` is a compare and a bump on every path that does not cross the edge.
    void commitTo(Byte* end, Size request);

    Byte* base = nullptr;
    Byte* p = nullptr;

    // The end of the readable and writable prefix. Between here and `max` the range is reserved and
    // untouched, which is what makes a 4 GB reservation cost no memory.
    Byte* committed = nullptr;
    Byte* max = nullptr;
};

template<class T>
struct Region: LinearArena {
    using LinearArena::LinearArena;

    RegionBase<T> operator * () const {
        return RegionBase<T>(base);
    }
};

inline void* operator new (Size count, LinearArena& arena) {
    return arena.alloc(count);
}

struct Context {
    // The library is handed to the diagnostics here and nowhere else, so that every driver that
    // builds a Context gets library source quotable in its reports without arranging anything.
    Context(Diagnostics& diagnostics): diagnostics(diagnostics) { diagnostics.library = &library; }

    // And taken back, because a Diagnostics outlives the Context in a language server: `compile`
    // drops the whole Context and builds a new one on every change, against the same Diagnostics.
    // Guarded so that a Context destroyed after a newer one was built does not unset the newer one.
    ~Context() { if(diagnostics.library == &library) diagnostics.library = nullptr; }

    Diagnostics& diagnostics;
    CompileSettings settings;

    /*
     * Where Core and the rest of the standard library are read from - see LibrarySource.
     *
     * On the Context rather than passed in, because every entry point into the resolver has to have
     * one and only two of them have a command line to build one from: `resolveProgram` builds Core
     * before it looks at the root module, so a driver that forgot to supply a library would produce
     * a program with no `+`. A default-constructed one finds the library by itself, which is what
     * makes every test driver in test/ work unchanged.
     */
    LibrarySource library;

    /*
     * Every AST in the compilation, in one region - Analysis-Modules.md §2.1.
     *
     * On the Context rather than on each parsed file, and that is what makes a module of several
     * files possible at all. A `ParsePtr` is a `U32` offset from a region base, so an AST node is
     * addressable only through the base it was allocated against; while each file owned its own
     * region, `Module::parse` could only be one file's, and eighty-odd sites in the resolver that
     * dereference a declaration through it would each have had to be told which file they were
     * looking at. One region for the whole compilation makes every one of them correct unchanged.
     *
     * It is per-Context and not per-program because an AST also holds `LocationId`s, which index
     * this context's location array in the order this context created them - so the two already had
     * the same lifetime, and a language server that drops the context on every keystroke drops this
     * with it. §3.4's reserve-and-commit is what makes holding them all in one region cost the
     * memory they actually use rather than the ceiling.
     */
    Region<ast::ParseRegion> parseRegion { 2 * 1024 * 1024 };

    /*
     * Where name resolution's answers are kept, or null - Implementation-Tooling.md §1.1.
     *
     * **Null in a batch compile.** The driver never sets it, so every recording site is one
     * predictable not-taken branch and nothing else; a language server sets it before resolving and
     * drops it with the program, since what it holds are that program's own handles.
     */
    SemanticIndex* index = nullptr;

    /*
     * The completion request this compile is answering, or none - Implementation-Tooling.md §8.
     *
     * `cursor` is what the parser reads and writes; `completion` is where the resolver puts what it
     * captured when it reached the sentinel. Both are set together by a language server and by
     * nothing else, so an ordinary compile parses and resolves exactly as it did before.
     */
    Cursor cursor;
    CompletionRequest* completion = nullptr;

    void addOp(StringId op, U16 prec = 9, Assoc assoc = Assoc::Left);
    OpProperties findOp(StringId op);

    static StringId nameHash(const char* chars, Size count);
    static StringId nameHash(const StringView& v);

    StringId addUnqualifiedName(const char* chars, Size count);
    StringId addQualifiedName(const char* chars, Size count, Size segmentCount);
    StringId addQualifiedName(const char* chars, Size count);
    StringId addIdentifier(const Identifier& q);

    Identifier& find(StringId id);
    String findName(StringId id);

    // The index has to be taken before pushing: a push that grows the array frees the old
    // buffer, and since the order the operands of `-` are evaluated in is unspecified, deriving
    // the index from `push(node) - begin()` can subtract the two buffers from each other.
    LocationId addLocation(const Location& node) {
        auto id = LocationId(locations.size());
        locations.push(node);
        return id;
    }

    LocationId addLocation(LocationId l) {
        return l;
    }

    Location* prepareLocation(LocationId& target) {
        target = LocationId(locations.size());
        return &locations.push();
    }

    const Location* getLocation(LocationId id) {
        return locations.size() > id ? &locations[id] : nullptr;
    }

    // Every location recorded so far, in creation order. Exposed for the position index
    // (compiler/position.h), which is a partition of it by module and needs to walk the whole
    // thing once; nothing else has a reason to look at it as an array.
    const Array<Location>& allLocations() const { return locations; }

    Arena stringArena;
    Arena exprArena;

private:
    HashMap<StringId, Identifier> identifiers;
    HashMap<StringId, OpProperties> ops;
    Array<Location> locations;
};

template<class Arena>
struct ArenaAllocator {
    ArenaAllocator(Arena& arena): arena(arena) {}
    Arena& arena;

    void* alloc(Size size) {
        return arena.alloc(size);
    }

    void free(void*) {}
};

template<class T>
using ArenaArray = ArrayT<T, ArrayAllocator<T, ArenaAllocator<Arena>>>;

template<class T>
using LinearArenaArray = ArrayT<T, ArrayAllocator<T, ArenaAllocator<LinearArena>>>;
