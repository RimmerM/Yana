#include "intern.h"
#include "resolve_internal.h"
#include "name.h"
#include "own.h"

namespace mir {

/*
 * Patterns, `is` and `match` - Analysis-IR.md §11.4's G7d.
 *
 * A pattern does two things and they happen at different times, which is the whole shape of this
 * file: it **tests** the value, and it **names parts of it**. The tests are laid out as *control
 * flow* - each one branches to the next alternative when it does not hold and carries on in a fresh
 * block when it does - because an inner test may only run once the outer one has passed, and a
 * condition built out of `&&` would have to have evaluated a projection the outer test had not
 * proved was there. The names are recorded rather than made, because a name for a part of a value is
 * only meaningful in the arm the test selected.
 *
 * That split is what makes the projection sound with no new machinery. A `Place` is interned rather
 * than emitted, so building the payload's place before the branch costs nothing and produces no
 * instruction; what would be wrong is *reading* it there, and the read happens inside the arm like
 * any other. From there a pattern binding is an ordinary `let` over a projection - `readPlace`
 * decides borrow-or-copy exactly as it does for `x.field`, and `->` on the name asks for the owned
 * access `let ->x` asks with.
 *
 * ## Which alternatives are tested at all
 *
 * Not decided while they are emitted. `PatternSpace` below answers it for the whole `match` first,
 * and that separation is what makes an exhaustive one need no wildcard: the alternative that
 * *completes* the match is emitted with no failure block, because every value that failed the ones
 * before it is one this alternative matches - all the way down through its nested patterns, which is
 * exactly the part a per-alternative rule cannot see.
 *
 * ## What is refused, and each by name
 *
 * An **operator section** - `Circle(>0)` - which is a call rather than a test on a scalar, and the
 * one pattern form whose meaning is an instance selection. A **tuple**, an **array** and a **rest**
 * pattern, none of which has a type in this stage. A **string** literal, for the same reason.
 */

/*
 * Exhaustiveness, usefulness and the witness - Maranget, "Warnings for pattern matching" (JFP 2007).
 *
 * One recursion answers three questions, and it has to be one because they are the same question
 * asked from different ends:
 *
 *  - is this alternative useful, or does an earlier one already cover everything it matches?
 *  - is the match complete once this alternative is added?
 *  - if it is not, which value is left over?
 *
 * The third is what makes the diagnostic worth reading and costs nothing extra: a witness is what
 * the search for one produces on its way to answering the second.
 *
 * The algorithm works on a matrix whose columns are the positions a value is taken apart into, which
 * is how it sees through nesting that the pattern syntax keeps separate: after `Proceed(Red)`,
 * `Proceed(Green)` and `Proceed(Blue)`, the `Proceed` column is complete even though no single
 * alternative wrote a bare `Proceed`.
 *
 * The old stage's `PatternSpace` is the same algorithm and this is not a copy of it: a constructor
 * here owns a *range of fields* rather than one payload type, so a head's arity is the arm's field
 * count and taking one apart needs no downcast column.
 */

// A pattern's head constructor, as the coverage recursion sees it.
//
// `Opaque` is the escape hatch: a test the recursion cannot relate to any other - a range, an
// operator section, or a constructor that does not belong to the type being matched. Each occurrence
// is its own constructor drawn from an inexhaustible signature, so it can never complete a column
// and can never be shown to subsume another pattern. Both of those err towards saying less, which is
// the safe direction: the worst an `Opaque` can cause is a wildcard the author did not strictly need.
struct PatternHead {
    enum Kind: U8 {
        Wildcard,
        Con,
        Int,
        Opaque,
    };

    bool operator == (const PatternHead& other) const {
        if(kind != other.kind) return false;

        switch(kind) {
            case Con:    return index == other.index;
            case Int:    return value == other.value;
            case Opaque: return tag == other.tag;
            default:     return true;
        }
    }

    bool operator != (const PatternHead& other) const { return !(*this == other); }

    // Con: the constructor's index in its record. Int: the literal, as a bit pattern. Opaque: the
    // pattern itself, which is the only identity such a test has.
    U32 index = 0;
    U64 value = 0;
    const ast::Pat* tag = nullptr;
    Kind kind = Wildcard;
};

/*
 * The lists the coverage recursion works in, all inline.
 *
 * Every one of these is built, read once and dropped inside a recursion that runs per alternative
 * per column - so what decides their cost is how many are made rather than how big any of them gets,
 * and the counts they hold are a constructor's arity and the number of alternatives of a `match`.
 * The bounds are set where an ordinary program stops rather than where the language does.
 */
using PatternRow = SmallArray<ast::Pat*, 8>;
using HeadList = SmallArray<PatternHead, 8>;

// One step of the recursion. Rows are stored flat, `types.size()` cells to a row, because every step
// builds a whole new matrix from the previous one and a row is never edited in place. A null cell is
// a wildcard.
struct PatternMatrix {
    SmallArray<DeclPtr<Type>, 8> types;
    SmallArray<ast::Pat*, 16> cells;
    Size rows = 0;

    Size columns() const { return types.size(); }
    ast::Pat* cell(Size row, Size column) const { return cells[row * columns() + column]; }
};

/*
 * What the recursion below costs, and where it is stopped.
 *
 * Maranget's algorithm is exponential in the worst case - every column that is complete branches
 * once per constructor, and a product of `n` complete columns is `k^n` leaves - and this
 * implementation used to run it uncached, once per alternative for the usefulness question and once
 * more per alternative for completeness with a witness built along the way. Three things bound it
 * now:
 *
 *  - **a memo** on (matrix, row): the branches of a complete column mostly specialize to the *same*
 *    sub-matrix - every row that had a wildcard there is in all of them - so the identical
 *    subproblems are answered once. `test/bench/ownership-scaling.py`'s shape M is the generated
 *    case, `3n` arms over `n` columns, and it is what turned `3^n` into `n^2`;
 *  - **a wildcard row answers at once**: a matrix holding a row of nothing but wildcards makes
 *    nothing useful and leaves nothing missing, whatever the other rows are, which is what makes
 *    the completeness question after a `_` alternative cost one scan rather than a full descent;
 *  - **a budget** of recursive steps per `match`. Past it, an alternative is reported useful and
 *    the match is reported *not* known to be complete - both the direction that says less - and
 *    `gap` says the check was abandoned rather than naming a value.
 *
 * Completeness is asked as usefulness of a wildcard row (`add`), which is the same question with
 * the same memo and no witness strings built on the way; `missing` runs only for `gap`, once, when
 * a diagnostic is about to say what is uncovered.
 */
static constexpr U32 kUsefulnessBudget = 200000;

struct PatternSpace {
    PatternSpace(Resolver& resolver, DeclPtr<Type> pivot): resolver(resolver) {
        covered.types.push(pivot);
    }

    /*
     * Whether `pattern` matches a value that none of the patterns added so far match. An alternative
     * that does not is one the program can never take.
     *
     * Every pattern handed to a `PatternSpace` has to outlive it and keep its address, because a
     * pattern's identity here *is* its address: that is what tells two opaque tests apart and what
     * the head cache is keyed by. An alternative read out of a parse list is a copy, so `resolveMatch`
     * keeps the ones it hands over.
     */
    bool useful(ast::Pat& pattern) {
        PatternRow row;
        row.push(&pattern);
        return useful(covered, row);
    }

    // Adds `pattern` to the space, and answers whether everything is now covered - which is whether
    // a wildcard row would still be useful, asked through the same memo the alternatives use.
    bool add(ast::Pat& pattern) {
        covered.cells.push(&pattern);
        covered.rows++;

        PatternRow wildcard;
        wildcard.push(nullptr);
        return !useful(covered, wildcard);
    }

    // A value nothing added so far matches, written the way a pattern for it would be. Only
    // meaningful when `add` last answered false.
    String gap() {
        if(exceeded) {
            return String("some value - the match is too large to check completely, so a `_` "
                          "alternative is required");
        }

        SmallArray<String, 4> witness;
        if(!missing(covered, witness) || !witness.size()) return String("_");
        return witness[0];
    }

    // Whether the budget stopped a question - for a caller that wants to know the answers it got
    // were the conservative ones.
    bool overBudget() const { return exceeded; }

private:
    DeclBase decl() const { return resolver.decl(); }

    // The head of one cell. Cached by pattern rather than recomputed, because the recursion asks for
    // the same one repeatedly and looking a constructor up is a hash lookup per ask.
    PatternHead head(ast::Pat* pattern, DeclPtr<Type> type) {
        if(!pattern) return {};

        for(auto& entry: cache) {
            if(entry.pattern == pattern) return entry.head;
        }

        auto result = computeHead(*pattern, type);
        cache.push(CachedHead { pattern, result });
        return result;
    }

    PatternHead computeHead(ast::Pat& pattern, DeclPtr<Type> type) {
        auto opaque = [&]() { return PatternHead { 0, 0, &pattern, PatternHead::Opaque }; };

        switch(pattern.kind) {
            // A bare name is a wildcard as far as coverage goes - it matches every value, it just
            // remembers which one it saw. A pattern that is *wrong* is one too: it has already been
            // reported, and saying it covers everything keeps one mistake to one message.
            case ast::Pat::Error:
            case ast::Pat::Any:
            case ast::Pat::Var:
                return {};

            case ast::Pat::Con: {
                auto found = mir::findConstructor(resolver.context, resolver.program,
                                                  resolver.here(), pattern.con.name, pattern.source);
                if(!found || !type || decl()[type]->kind != Type::Record) return opaque();
                if(recordDeclarationOf(decl(), type) != found.record) return opaque();

                return PatternHead { found.index, 0, nullptr, PatternHead::Con };
            }

            /*
             * `{a, b}` written with no constructor in front of it - the head is the one constructor
             * a product has, so it names no case and the coverage question passes straight to the
             * fields.
             *
             * A record with **several** constructors has no such head: which one a value is is what
             * the discriminant says, and a braced list says nothing about it. It comes back opaque,
             * which is the answer that makes a wildcard the only thing that covers it - and the
             * emission refuses the same shape in words, so a program never gets to rely on it.
             */
            case ast::Pat::Tup: {
                if(!type || decl()[type]->kind != Type::Record) return opaque();
                if(((RecordType*)decl()[type])->cons.size() != 1) return opaque();

                return PatternHead { 0, 0, nullptr, PatternHead::Con };
            }

            default:
                break;
        }

        if(pattern.kind == ast::Pat::Kind(ast::Pat::Lit + ast::Literal::Int)) {
            // Two's complement here, where the number is an identity key rather than a value: two
            // patterns are the same head when they are the same number, and which number that is is
            // the magnitude *and* the sign. In unsigned arithmetic, so `I64`'s own minimum is a key
            // like any other.
            auto magnitude = pattern.lit.i();
            return PatternHead {
                0, pattern.negative ? U64(0) - magnitude : magnitude, nullptr, PatternHead::Int
            };
        }

        return opaque();
    }

    /*
     * The signature of one column.
     */

    // A constructor's arity here is its **field count**, which is the whole of what the flat field
    // list buys the coverage algorithm: there is no payload type to see through and no downcast
    // column, so `Box {width, height}` is two positions and `Line(n)` is one.
    Size arity(const PatternHead& head, DeclPtr<Type> type) {
        if(head.kind != PatternHead::Con) return 0;
        return ((RecordType*)decl()[type])->cons.get(decl(), head.index).count;
    }

    DeclPtr<Type> fieldType(const PatternHead& head, DeclPtr<Type> type, Size index) {
        auto& record = *(RecordType*)decl()[type];
        auto arm = record.cons.get(decl(), head.index);
        return record.field(decl(), U16(arm.first + index)).occurrence.type;
    }

    // The sub-patterns `pattern` supplies to a head it matches, one per field. A null pattern is the
    // wildcard case, which supplies a wildcard for each of the head's own positions.
    template<class Target>
    void expand(const PatternHead& head, DeclPtr<Type> type, ast::Pat* pattern, Target& target) {
        auto count = arity(head, type);
        if(!count) return;

        if(!pattern) {
            for(Size i = 0; i < count; i++) target.push(nullptr);
            return;
        }

        // `constructorFields` and not a second reading of the three spellings, which is what keeps
        // the column this recursion takes apart the same one the emission projects.
        auto& record = *(RecordType*)decl()[type];
        SmallArray<ast::Pat*, 4> supplied;
        resolver.constructorFields(*pattern, record, record.cons.get(decl(), head.index), supplied,
                                   false);

        for(Size i = 0; i < count; i++) target.push(i < supplied.size() ? supplied[i] : nullptr);
    }

    // Every distinct head appearing in the matrix's first column.
    void collectHeads(const PatternMatrix& matrix, HeadList& target) {
        for(Size row = 0; row < matrix.rows; row++) {
            auto candidate = head(matrix.cell(row, 0), matrix.types[0]);
            if(candidate.kind == PatternHead::Wildcard) continue;

            auto seen = false;
            for(auto& existing: target) if(existing == candidate) seen = true;
            if(!seen) target.push(candidate);
        }
    }

    // Whether `heads` names every value the column's type can hold. Only a record has a signature
    // that can be finished; an integer, a range or an opaque test leaves values no pattern in the
    // column named, so a wildcard is the only thing that covers them.
    bool complete(const HeadList& heads, DeclPtr<Type> type) {
        if(!type || decl()[type]->kind != Type::Record) return false;

        auto& record = *(RecordType*)decl()[type];
        for(U32 i = 0; i < U32(record.cons.size()); i++) {
            auto present = false;
            for(auto& candidate: heads) {
                if(candidate.kind == PatternHead::Con && candidate.index == i) present = true;
            }

            if(!present) return false;
        }

        return true;
    }

    // The heads a complete column is taken apart by: the type's own constructors, rather than
    // whatever the column happens to hold, so that an opaque test sitting beside a complete set of
    // constructors does not add a case of its own.
    void signature(DeclPtr<Type> type, HeadList& target) {
        auto& record = *(RecordType*)decl()[type];
        for(U32 i = 0; i < U32(record.cons.size()); i++) {
            target.push(PatternHead { i, 0, nullptr, PatternHead::Con });
        }
    }

    /*
     * The recursion.
     */

    // P specialized by one head: the rows that can still match once the first column is known to
    // have it, with that column replaced by the head's own.
    void specialize(const PatternMatrix& matrix, const PatternHead& by, PatternMatrix& target) {
        auto type = matrix.types[0];
        auto count = arity(by, type);

        for(Size i = 0; i < count; i++) target.types.push(fieldType(by, type, i));
        for(Size i = 1; i < matrix.columns(); i++) target.types.push(matrix.types[i]);

        for(Size row = 0; row < matrix.rows; row++) {
            auto pattern = matrix.cell(row, 0);
            auto rowHead = head(pattern, type);

            if(rowHead.kind != PatternHead::Wildcard && rowHead != by) continue;

            expand(by, type, rowHead.kind == PatternHead::Wildcard ? nullptr : pattern, target.cells);
            for(Size i = 1; i < matrix.columns(); i++) target.cells.push(matrix.cell(row, i));
            target.rows++;
        }
    }

    // P defaulted: the rows whose first column matches whatever the others did not.
    void defaults(const PatternMatrix& matrix, PatternMatrix& target) {
        for(Size i = 1; i < matrix.columns(); i++) target.types.push(matrix.types[i]);

        for(Size row = 0; row < matrix.rows; row++) {
            if(head(matrix.cell(row, 0), matrix.types[0]).kind != PatternHead::Wildcard) continue;

            for(Size i = 1; i < matrix.columns(); i++) target.cells.push(matrix.cell(row, i));
            target.rows++;
        }
    }

    static void tail(const PatternRow& row, PatternRow& target) {
        for(Size i = 1; i < row.size(); i++) target.push(row[i]);
    }

    // A row of wildcards alone - the row that matches everything, and therefore the matrix that
    // makes nothing useful and leaves nothing missing.
    bool hasWildcardRow(const PatternMatrix& matrix) {
        for(Size row = 0; row < matrix.rows; row++) {
            auto all = true;
            for(Size column = 0; all && column < matrix.columns(); column++) {
                all = head(matrix.cell(row, column), matrix.types[column]).kind == PatternHead::Wildcard;
            }

            if(all) return true;
        }

        return false;
    }

    /*
     * The memo's key: the matrix and the row, with every wildcard cell written as null so that two
     * names for the same wildcard key alike. Stored whole beside the answer and compared back,
     * because a hash is not an identity and a wrong answer here is a match compiled without an arm.
     */
    void keyOf(const PatternMatrix& matrix, const PatternRow& row, SmallArray<U64, 32>& into) {
        into.push(U64(matrix.columns()));
        into.push(U64(matrix.rows));
        for(auto type: matrix.types) into.push(U64(type.offset));

        for(Size r = 0; r < matrix.rows; r++) {
            for(Size c = 0; c < matrix.columns(); c++) {
                auto cell = matrix.cell(r, c);
                auto wildcard = head(cell, matrix.types[c]).kind == PatternHead::Wildcard;
                into.push(wildcard ? 0 : U64((Size)cell));
            }
        }

        for(Size c = 0; c < row.size(); c++) {
            auto wildcard = head(row[c], matrix.types[c]).kind == PatternHead::Wildcard;
            into.push(wildcard ? 0 : U64((Size)row[c]));
        }
    }

    bool useful(const PatternMatrix& matrix, const PatternRow& row) {
        if(matrix.columns() == 0) return matrix.rows == 0;
        if(hasWildcardRow(matrix)) return false;

        if(++steps > kUsefulnessBudget) {
            exceeded = true;
            return true;
        }

        SmallArray<U64, 32> words;
        keyOf(matrix, row, words);

        InternKey key;
        for(auto word: words) key.mix(word);
        auto hash = key.hash == ~U64(0) ? 0 : key.hash;

        if(auto found = memoIndex.getValue(hash)) {
            auto& entry = memos[found.unwrap()];
            if(entry.count == words.size()) {
                auto same = true;
                for(U32 i = 0; same && i < entry.count; i++) same = memoWords[entry.start + i] == words[i];
                if(same) return entry.answer;
            }
        }

        auto answer = usefulUncached(matrix, row);

        // Not recorded past the budget: an answer the budget cut short is not the question's.
        if(exceeded) return answer;

        Memo entry;
        entry.start = U32(memoWords.size());
        entry.count = U32(words.size());
        entry.answer = answer;
        for(auto word: words) memoWords.push(word);

        auto index = U32(memos.size());
        memos.push(entry);

        auto slot = memoIndex.add(hash);
        *slot.value = index;
        return answer;
    }

    bool usefulUncached(const PatternMatrix& matrix, const PatternRow& row) {
        auto type = matrix.types[0];
        auto rowHead = head(row[0], type);

        if(rowHead.kind != PatternHead::Wildcard) {
            PatternMatrix next;
            specialize(matrix, rowHead, next);

            PatternRow nextRow;
            expand(rowHead, type, row[0], nextRow);
            tail(row, nextRow);

            return useful(next, nextRow);
        }

        HeadList heads;
        collectHeads(matrix, heads);

        // A wildcard against a column that already names every case is only useful through one of
        // those cases, so the question splits into one per constructor.
        if(complete(heads, type)) {
            HeadList cases;
            signature(type, cases);

            for(auto& candidate: cases) {
                PatternMatrix next;
                specialize(matrix, candidate, next);

                PatternRow nextRow;
                expand(candidate, type, nullptr, nextRow);
                tail(row, nextRow);

                if(useful(next, nextRow)) return true;
            }

            return false;
        }

        PatternMatrix next;
        defaults(matrix, next);

        PatternRow nextRow;
        tail(row, nextRow);

        return useful(next, nextRow);
    }

    // Maranget's I(P, n): a value of the matrix's column types that no row matches, written one
    // string per column. False when the matrix is exhaustive, which is the answer `add` reports.
    bool missing(const PatternMatrix& matrix, SmallArray<String, 4>& witness) {
        if(matrix.columns() == 0) return matrix.rows == 0;
        if(hasWildcardRow(matrix)) return false;

        // Only ever run for a diagnostic, and past the budget `gap` says so instead of this.
        if(++steps > kUsefulnessBudget) {
            exceeded = true;
            return true;
        }

        auto type = matrix.types[0];
        HeadList heads;
        collectHeads(matrix, heads);

        if(complete(heads, type)) {
            HeadList cases;
            signature(type, cases);

            for(auto& candidate: cases) {
                PatternMatrix next;
                specialize(matrix, candidate, next);

                SmallArray<String, 4> sub;
                if(!missing(next, sub)) continue;

                auto count = arity(candidate, type);
                witness.push(describe(candidate, type, sub, count));
                for(Size i = count; i < sub.size(); i++) witness.push(sub[i]);
                return true;
            }

            return false;
        }

        PatternMatrix next;
        defaults(matrix, next);

        SmallArray<String, 4> sub;
        if(!missing(next, sub)) return false;

        witness.push(absent(heads, type));
        for(auto& entry: sub) witness.push(entry);
        return true;
    }

    /*
     * Witnesses.
     */

    // One constructor written the way source writes it: the one-payload spelling where the arm's
    // single field carries the constructor's own name, and the braced list otherwise. That is the
    // same distinction `declareFields` made when it named the field, read back out.
    String describe(const PatternHead& head, DeclPtr<Type> type, const SmallArray<String, 4>& fields,
                    Size count) {
        auto& record = *(RecordType*)decl()[type];
        auto arm = record.cons.get(decl(), head.index);

        StringBuilder text;
        text << resolver.context.findName(arm.name);
        if(!count) return text.string();

        if(count == 1 && record.field(decl(), arm.first).name == arm.name) {
            text << '(' << fields[0] << ')';
            return text.string();
        }

        text << " {";
        for(Size i = 0; i < count; i++) {
            if(i) text << ", ";
            text << resolver.context.findName(record.field(decl(), U16(arm.first + i)).name);
            text << ": " << fields[i];
        }

        text << '}';
        return text.string();
    }

    // A value of `type` that none of `heads` names. Reached only where the column is incomplete, so
    // a record always has a constructor left to point at; anything else can only be described as
    // some other value.
    String absent(const HeadList& heads, DeclPtr<Type> type) {
        if(!type || decl()[type]->kind != Type::Record) return String("_");

        auto& record = *(RecordType*)decl()[type];
        for(U32 i = 0; i < U32(record.cons.size()); i++) {
            auto present = false;
            for(auto& candidate: heads) {
                if(candidate.kind == PatternHead::Con && candidate.index == i) present = true;
            }

            if(present) continue;

            SmallArray<String, 4> holes;
            auto arm = record.cons.get(decl(), i);
            for(U16 f = 0; f < arm.count; f++) holes.push(String("_"));

            return describe(PatternHead { i, 0, nullptr, PatternHead::Con }, type, holes, arm.count);
        }

        return String("_");
    }

    struct CachedHead {
        ast::Pat* pattern;
        PatternHead head;
    };

    // One answered (matrix, row) - its key at `memoWords[start .. start + count)`.
    struct Memo {
        U32 start = 0;
        U32 count = 0;
        bool answer = false;
    };

    Resolver& resolver;
    PatternMatrix covered;
    SmallArray<CachedHead, 16> cache;

    Array<Memo> memos;
    Array<U64> memoWords;
    HashMap<U64, U32> memoIndex;
    U32 steps = 0;
    bool exceeded = false;
};

// Which declaration a record type came from - itself, or the generic it instantiates. What a
// constructor's name resolves to is a declaration, and a pivot's type is usually an instantiation.
DeclPtr<RecordType> recordDeclarationOf(DeclBase base, DeclPtr<Type> type) {
    if(!type || base[type]->kind != Type::Record) return nullptr;

    auto& record = *(RecordType*)base[type];
    return record.instanceOf ? record.instanceOf : DeclPtr<RecordType>(type.offset);
}

/*
 * The sub-patterns a constructor pattern supplies, one per field of its arm and null where it
 * supplies none.
 *
 * The **one** reading of the three spellings, because two things ask it: the coverage matrix takes
 * the column apart by it, and the emission projects by it. An answer that differed between them
 * would be a `match` checked as one program and compiled as another.
 *
 * Nothing at all supplies nothing, a braced list names its fields, and everything else is the
 * one-payload form - which the parser has already collapsed to the inner pattern, so `Proceed(v)`
 * arrives here as a `Var`. `report` is false for the matrix, which runs first and must not produce
 * the diagnostics the emission is about to produce.
 */
void Resolver::constructorFields(ast::Pat& pat, RecordType& record, const RecordCon& arm,
                                 SmallArray<ast::Pat*, 4>& into, bool report) {
    for(U16 i = 0; i < arm.count; i++) into.push(nullptr);

    // A pattern over a record whose declaration lost a field names fields that are not there, and
    // binds nothing the arm can then use - see `quietenRest`. Only where this walk reports: the
    // coverage walk asks the same question silently, and quietening there would be a second count.
    if(report && quietAboutBroken(asBase<Type>(&record - decl()))) return;

    /*
     * What a message about this field list calls what it is a field list *of*.
     *
     * The constructor's name where there is one, and the type where there is not: an anonymous
     * record's only constructor has no name, so a message built from `arm.name` alone would begin
     * with nothing at all. `{I64, Bool} has 2 fields` is the sentence the tuple case needs, and it is
     * the same sentence `Square has 2 fields` is.
     */
    auto subject = [&]() -> String {
        if(arm.name != StringId()) {
            auto text = stringView(context.findName(arm.name));
            return String(text.ptr, text.size());
        }

        return typeText(context, decl(), asBase<Type>(&record - decl()));
    };

    /*
     * The braced field list, however it was reached.
     *
     * Written after a constructor name - `Square {width, height}` - or written **alone**, where the
     * type is an anonymous record and there is no name to write. One walk for both, because they are
     * the same syntax over the same field list, and this function's whole reason for existing is
     * that the coverage matrix and the emission read that syntax once.
     */
    auto written = &pat;

    if(pat.kind == ast::Pat::Con) {
        if(!pat.con.pats) return;
        written = parse[pat.con.pats];

        if(written->kind != ast::Pat::Tup) {
            if(arm.count == 1) {
                into[0] = written;
                return;
            }

            if(report) {
                context.diagnostics.error("%@ has %@ fields, and this names one"_v, written->source,
                                          subject(), U64(arm.count));
            }

            return;
        }
    } else if(pat.kind != ast::Pat::Tup) {
        return;
    }

    auto position = U16(0);
    for(auto field: written->tup.contents(parse)) {
        auto index = ~U32(0);

        if(field.field) {
            for(U16 i = 0; i < arm.count; i++) {
                if(record.field(decl(), U16(arm.first + i)).name == field.field) index = i;
            }

            if(index == ~U32(0)) {
                if(report) {
                    context.diagnostics.error("%@ has no field called %@"_v, written->source,
                                              subject(), context.findName(field.field));
                }

                continue;
            }
        } else if(position < arm.count) {
            index = position;
        } else {
            if(report) {
                context.diagnostics.error("%@ has %@ fields, and this names more"_v, written->source,
                                          subject(), U64(arm.count));
            }

            continue;
        }

        position++;

        /*
         * A field named twice. Reported here rather than left to produce two bindings of one name,
         * because which of the two a later use meant is not a question with an answer.
         *
         * By name where it has one and by position where it has not, which is the same split every
         * message about a field makes: an anonymous field is named by where it is, and that is the
         * whole of what `.0` exists for.
         */
        if(into[index]) {
            if(report) {
                auto named = record.field(decl(), U16(arm.first + index)).name;

                if(named != StringId()) {
                    context.diagnostics.error("%@ is matched twice"_v, written->source,
                                              context.findName(named));
                } else {
                    context.diagnostics.error("field %@ of %@ is matched twice"_v, written->source,
                                              U64(index), subject());
                }
            }

            continue;
        }

        into[index] = parse[field.pat];
    }
}

/*
 * One refutable test: branch to `onFail` where it does not hold, and carry on in a fresh block where
 * it does.
 *
 * The **whole** of why patterns are control flow rather than a condition. Everything after this call
 * is emitted in a block reached only when the test passed, so a projection of a payload the tag has
 * just proved is there is an ordinary read - and the alternative after this one is reached by the
 * same edge however many tests a nested pattern needs.
 *
 * Both targets take the carry, because a branch supplies one operand list to both of its edges - so
 * a fresh block that declared none would be a merge the verifier reports rather than a shortcut.
 */
void Resolver::failIfNot(Builder& builder, Node& condition, Block& onFail, Carry& carry,
                         LocationId source) {
    auto pass = builder.addBlock(StringId());
    carry.declare(builder, *pass);

    SmallArray<Operand, 8> edge;
    carry.edge(builder, edge);
    builder.addBranch(source, builder.resultOf(condition, UseMode::Inspect), *pass, onFail,
                      edge.pointer(), edge.size());

    builder.setBlock(*pass);
    carry.arrive(builder, *pass, 0);
}

/*
 * The value a pattern compares against - a range's bound, or a literal.
 *
 * Typed at the **pivot's** type rather than at its own, which is the whole of what makes a negative
 * literal work: the lexer produces a magnitude and a separate `-`, and what the number *is* is a
 * question about the type it is being compared with. So the sign and the normal form are
 * `literalValue`'s single answer, asked at the type the comparison will be at - and at a type with
 * a declared `FromInt` that answer is the call the literal is everywhere else, so `5` against a
 * `Money` pivot compares what the program's own `fromInt` builds.
 */
Node* Resolver::patternBound(Builder& builder, ast::Pat& pat, DeclPtr<Type> type) {
    if(pat.kind < ast::Pat::Lit) {
        unsupported(pat.source, "a pattern that compares against anything but a literal or `_`"_v);
        return nullptr;
    }

    // Built as the literal expression it is, so that nothing here is a second reading of a written
    // number beside the one every other position gets.
    ast::Expr literal {};
    literal.lit = pat.lit;
    literal.source = pat.source;
    literal.kind = ast::Expr::Kind(ast::Expr::Lit + (pat.kind - ast::Pat::Lit));

    return literalValue(builder, literal, type, pat.negative);
}

/*
 * One comparison of the pivot against a bound, as the test it is.
 *
 * `_` is a bound that says nothing - it is what makes `a.._` one-sided - so it emits no test and is
 * not a failure. Anything else is read, compared and branched on - and the comparison is the
 * type's own, `doc/spec/patterns.md`'s "equality is the type's own `Eq` instance": `applyBinary`
 * names the `Eq` or `Ord` member the operation is, so a literal pattern against a program's own
 * type runs the `==` it declared, and against a width it is the one instruction it always was.
 */
bool Resolver::comparePattern(Builder& builder, BinaryOp op, Place& pivot, ast::Pat& bound,
                              Block* onFail, Carry& carry, LocationId source) {
    if(bound.kind == ast::Pat::Any) return true;

    // The pivot before the bound, so that a dump reads in the order the comparison is written -
    // `value == 0` rather than the constant standing above the read it is compared with.

    auto read = readPlace(builder, source, pivot, inspectRequest());
    if(!read) return false;

    auto value = patternBound(builder, bound, pivot.occurrence.type);
    if(!value) return false;

    auto condition = applyBinary(builder, source, op, *read, *value);
    if(!condition) return false;

    if(onFail) failIfNot(builder, *condition, *onFail, carry, source);
    return true;
}

/*
 * `>0` - an operator section, the one pattern whose test is a name the program wrote.
 *
 * Everything else refutable here compares with a member the *compiler* names - `Eq.==` for a
 * literal, `Ord`'s four for a range - so `applyBinary` is the whole of the call. A section names an
 * operator instead, which makes it an overload set and a selection like any other call of that
 * name: `>` over a program's own `Ord` and `>` over something else in scope are chosen between by
 * `resolve_overload.cpp`'s five rules, and not by a second reading of them written here.
 *
 * The pivot is the operator's **left** operand, so `>0` reads as the comparison it is, and it is
 * read at the request the set agrees that position is in - the same question `climb` asks before it
 * resolves `x > 0`, so the pattern and the expression borrow the pivot alike.
 *
 * What the operator answers goes through `truthValue` rather than being required to be `Bool`,
 * which is what makes this one decision rather than two: `if x > 0` asks the value's `Truth`
 * instance and a pattern that asked something stricter would be a second answer to "what is a
 * condition". The comparison is emitted whether or not there is a failure edge, exactly as
 * `comparePattern` emits one - a caller that proved the match has nothing to branch on and the
 * arm's own alternative is the only path left.
 */
bool Resolver::sectionPattern(Builder& builder, ast::Pat& pat, Place& pivot, Block* onFail,
                              Carry& carry) {
    auto& bound = *parse[pat.section.bound];

    // `>_` - a section with nothing on its right. The bound is what the operator is applied to, so
    // there is no reading under which this says anything; `_` alone is the pattern that matches
    // everything.
    if(bound.kind == ast::Pat::Any) {
        context.diagnostics.error(
            "an operator pattern needs a value on its right - `>0` compares the matched value with something, and `_` is not something"_v,
            pat.source);
        return false;
    }

    OverloadSet set;
    findOverloads(context, program, here(), pat.section.op, 2, pat.source, set);

    if(set.isEmpty()) {
        reportMissingOperator(pat.section.op, 2, pat.source);
        return false;
    }

    Request requests[2];
    if(!operatorRequests(set, pat.section.op, 2, pat.source, requests)) return false;

    auto read = readPlace(builder, pat.source, pivot, requests[0]);
    if(!read) return false;

    // At the pivot's type, for `patternBound`'s reason: what `0` *is* is a question about the type
    // it is being compared with, and the selection below is then made over two settled operands.
    auto value = patternBound(builder, bound, pivot.occurrence.type);
    if(!value) return false;

    Node* values[2] = { read, value };
    auto answer = selectOperatorCall(builder, set, pat.section.op, pat.source, values, 2, nullptr);
    if(!answer) return false;

    auto condition = truthValue(builder, *answer, pat.source);
    if(!condition) return false;

    if(onFail) failIfNot(builder, *condition, *onFail, carry, pat.source);
    return true;
}

/*
 * One pattern against one place, emitting the tests it needs and recording the names it introduces.
 *
 * `onFail` null says the caller has proved this pattern matches - the alternative exhaustiveness
 * showed completes the match - and everything below then takes the value apart and binds it without
 * asking any question about it. That is sound *through the nesting* and not only at the top, which
 * is what `PatternSpace` is for: the proof is over whole values rather than over head constructors.
 */
bool Resolver::resolvePattern(Builder& builder, ast::Pat& pat, Place& pivot,
                              SmallArray<PatternBinding, 4>& into, Block* onFail, Carry& carry,
                              bool ownedField) {
    /*
     * `&` alone, and why `&->` is not the same refusal - Analysis-Language.md §2.
     *
     * What matching cannot establish is exclusive access to the *pivot's* storage: the pattern found
     * a value where it lay and said nothing about who else can reach it, so a name claiming to be
     * the only writer of it would be claiming something nobody granted. `&->` claims nothing about
     * the pivot - it takes the payload out, which the pivot then reads as moved from, and the
     * storage the move lands in is this frame's own.
     */
    if(pat.bind == ast::BindType::Ref && !pat.sink) {
        context.diagnostics.error(
            "a pattern cannot bind with `&` - matching does not establish the exclusive access that would need. Write `&->` to take the value out into storage this binding owns"_v,
            pat.source);
        return false;
    }

    auto take = pat.bind == ast::BindType::Sink || pat.sink;

    /*
     * `->` on a name or on `_` and nowhere else.
     *
     * `Just(->Pair {a, 0})` would take the payload out and *then* ask whether the second field is
     * zero, and on the path where it is not the value has already left a place nothing will put it
     * back into. The restriction is what keeps a move off every path that can still fail, and it
     * costs nothing a program wants: bind the payload and take it apart afterwards.
     */
    if(take && pat.kind != ast::Pat::Var && pat.kind != ast::Pat::Any) {
        context.diagnostics.error(
            "`->` in a pattern has to be written on a name or on `_` - taking a value out and then testing what came out would leave it moved on the path where the test failed"_v,
            pat.source);
        return false;
    }

    // A name binds, always, shadowing whatever it shadows - that is what makes a pattern mean the
    // same thing wherever it is written. Binding one name twice within one pattern is the one case
    // that cannot be meant, since only one of the two could ever be read.
    auto bind = [&](StringId name, Place& place) {
        for(auto& existing: into) {
            if(existing.name != name) continue;

            context.diagnostics.error("this pattern binds %@ twice"_v, pat.source,
                                      context.findName(name));
            return;
        }

        into.push(PatternBinding { name, &place, pat.source, take, pat.sink, ownedField });
    };

    /*
     * `x@p` - the whole value beside whatever `p` says about its parts. One more binding and no more
     * test, which is what makes it worth admitting where the rest of the pattern grammar is not.
     *
     * Not beside a `->`, which is the one combination that has no meaning: a take happens once, and
     * two names for what was taken would be two owners of one value.
     */
    if(pat.asVar != StringId()) {
        if(take) {
            context.diagnostics.error(
                "`->` takes the value out once, so it cannot also be named by the `@` in front of it"_v,
                pat.source);
            return false;
        }

        bind(pat.asVar, pivot);
    }

    switch(pat.kind) {
        case ast::Pat::Error:
            // Already reported by the parser. Accepting it keeps one syntax mistake to one message.
            return true;

        // Irrefutable: no test, and the name is for the whole value.
        case ast::Pat::Any:
            if(take) into.push(
                PatternBinding { StringId(), &pivot, pat.source, true, pat.sink, ownedField });
            return true;
        case ast::Pat::Var:
            bind(pat.var, pivot);
            return true;

        case ast::Pat::Con:
            return resolveConstructorPattern(builder, pat, pivot, into, onFail, carry);

        /*
         * `{a, b}` and `{x: a, y: b}` - the field list with no constructor in front of it.
         *
         * The same projection `Square {width, height}` makes, minus the discriminant test: a record
         * with one constructor has none to make, so what is left is naming parts. That is why the
         * pivot has to have exactly one - a braced list says which parts, and nothing about *which
         * constructor*, so over a sum it would be projecting through a tag nothing proved.
         */
        case ast::Pat::Tup:
            return resolveTuplePattern(builder, pat, pivot, into, onFail, carry);

        /*
         * `a..b` and `a..=b` - two tests rather than one condition, which is what the control-flow
         * shape buys here as well: the upper bound is only compared where the lower one held, so a
         * one-sided range emits exactly one comparison and neither needs an `and`.
         */
        case ast::Pat::Range: {
            auto& range = pat.range;
            auto lower = comparePattern(builder, BinaryOp::Ge, pivot, *parse[range.from], onFail,
                                        carry, pat.source);
            auto upper = comparePattern(builder, range.inclusive ? BinaryOp::Le : BinaryOp::Lt,
                                        pivot, *parse[range.to], onFail, carry, pat.source);
            return lower && upper;
        }

        // `>0`, `<=x` - the operator is the program's own, so this is a selection rather than a
        // member the compiler names. See `sectionPattern`.
        case ast::Pat::Section:
            return sectionPattern(builder, pat, pivot, onFail, carry);

        default:
            break;
    }

    if(pat.kind >= ast::Pat::Lit) {
        return comparePattern(builder, BinaryOp::Eq, pivot, pat, onFail, carry, pat.source);
    }

    unsupported(pat.source, pat.kind == ast::Pat::Arr || pat.kind == ast::Pat::Rest
                                ? "an array pattern"_v
                                : "this pattern"_v);
    return false;
}

/*
 * A braced field list matched against a product - see the case above, where the rule is stated.
 *
 * `constructorFields` is what reads the list, which is the same call `Square {width, height}` makes:
 * one reading of the syntax, so the coverage matrix takes the column apart exactly the way this
 * projects it.
 */
bool Resolver::resolveTuplePattern(Builder& builder, ast::Pat& pat, Place& pivot,
                                   SmallArray<PatternBinding, 4>& into, Block* onFail,
                                   Carry& carry) {
    auto type = pivot.occurrence.type;

    if(!type || decl()[type]->kind != Type::Record) {
        context.diagnostics.error("%@ has no fields, so a field list cannot match it"_v, pat.source,
                                  typeText(context, decl(), type));
        return false;
    }

    auto& record = *(RecordType*)decl()[type];

    if(record.cons.size() != 1) {
        context.diagnostics.error(
            "%@ has several constructors, so a field list has to name which one - write the constructor in front of it"_v,
            pat.source, typeText(context, decl(), type));
        return false;
    }

    if(!reachesInside(record, pat.source, "its fields"_v)) return false;

    auto arm = record.cons.get(decl(), 0);

    SmallArray<ast::Pat*, 4> supplied;
    constructorFields(pat, record, arm, supplied, true);

    auto ok = true;
    for(U16 i = 0; i < arm.count; i++) {
        if(!supplied[i]) continue;

        auto place = builder.fieldPlace(pivot, U16(arm.first + i));
        if(!place) { ok = false; continue; }

        // The declaration's own answer, not `place`'s - `fieldPlace` has already folded the
        // pivot's contract into an owned field's, which is exactly the narrowing
        // `PatternBinding::ownedField` exists to see past.
        auto declared = record.field(decl(), U16(arm.first + i));
        auto ownedField = fieldContract(decl(), pivot.occurrence.shape, declared).isOwned();

        if(!resolvePattern(builder, *supplied[i], *place, into, onFail, carry, ownedField))
            ok = false;
    }

    return ok;
}

/*
 * `Proceed(v)` - the discriminant, then the sub-patterns against this constructor's fields.
 *
 * The test is the discriminant and nothing else. A record with one constructor has none to make:
 * every value of it is that constructor, so the pattern is irrefutable and what it does is name
 * parts - which is the same statement `RecordLayout` makes about the layout, read from the matching
 * side.
 */
bool Resolver::resolveConstructorPattern(Builder& builder, ast::Pat& pat, Place& pivot,
                                         SmallArray<PatternBinding, 4>& into, Block* onFail,
                                         Carry& carry) {
    if(isCursorSentinel(context, pat.con.name)) {
        capturePatternCompletion(pivot.occurrence.type);
        return false;
    }

    auto found = mir::findConstructor(context, program, here(), pat.con.name, pat.source);
    if(found) recordReference(context, pat.source, constructorSymbol(decl(), found));

    if(!found) {
        context.diagnostics.error("unknown constructor %@"_v, pat.source,
                                  context.findName(pat.con.name));
        return false;
    }

    /*
     * The pivot's type against the constructor's declaration.
     *
     * Compared at the *declaration*, because a pivot is an instantiation - `Outcome(I32, I32)` - and
     * a constructor belongs to `Outcome`. What that buys is that a pattern needs no type arguments
     * of its own: the arm is read out of the instantiation, whose fields are already substituted.
     */
    auto pivotType = pivot.occurrence.type;
    if(recordDeclarationOf(decl(), pivotType) != found.record) {
        context.diagnostics.error("%@ is a constructor of %@, and this matches a %@"_v, pat.source,
                                  context.findName(pat.con.name),
                                  context.findName(decl()[found.record]->name),
                                  typeText(context, decl(), pivotType));
        return false;
    }

    if(!reachesInside(*decl()[found.record], pat.source, "its constructors"_v)) return false;

    auto& record = *(RecordType*)decl()[pivotType];
    auto arm = record.cons.get(decl(), found.index);

    // The tag first, so that every projection below it is one the test has proved. `tagTypeOf` is
    // null for a record with one constructor, which is the irrefutable case.
    if(onFail) {
        if(auto tag = tagTypeOf(decl(), program.types, pivotType)) {
            auto place = builder.discriminantPlace(pivot);
            auto read = readPlace(builder, pat.source, *place, inspectRequest());
            if(!read) return false;

            auto number = builder.addConst(pat.source, intConstant(tag, U64(arm.value)));
            auto condition = builder.addBinary(pat.source, BinaryOp::Eq, *read, *number);
            if(!condition) return false;

            failIfNot(builder, *condition, *onFail, carry, pat.source);
        }
    }

    SmallArray<ast::Pat*, 4> supplied;
    constructorFields(pat, record, arm, supplied, true);

    auto ok = true;
    for(U16 i = 0; i < arm.count; i++) {
        if(!supplied[i]) continue;

        auto place = builder.fieldPlace(pivot, U16(arm.first + i));
        if(!place) { ok = false; continue; }

        // The declaration's own answer, not `place`'s - `fieldPlace` has already folded the
        // pivot's contract into an owned field's, which is exactly the narrowing
        // `PatternBinding::ownedField` exists to see past.
        auto declared = record.field(decl(), U16(arm.first + i));
        auto ownedField = fieldContract(decl(), pivot.occurrence.shape, declared).isOwned();

        if(!resolvePattern(builder, *supplied[i], *place, into, onFail, carry, ownedField))
            ok = false;
    }

    return ok;
}

/*
 * The names, made in the arm the tests proved.
 *
 * An ordinary `let` over the projection - see the note at the top. The occurrence is what the read
 * produced, whole, for `resolveDeclaration`'s reason: a loan bound to a name is a loan, and a local
 * that kept the type and dropped the contract would be storage this frame believes it owns.
 *
 * A binding with no name is `->_`, and it still gets storage: an owner lives in a place, so the
 * value taken out has somewhere for its drop to be owed from. That is the whole of what `Held(->_)`
 * asks for.
 */
void Resolver::bindPattern(Builder& builder, const SmallArray<PatternBinding, 4>& made, U32 from) {
    for(Size i = from; i < made.size(); i++) {
        auto binding = made[i];

        /*
         * A payload that is itself a **reference** binds at the capability it carries, and `&` on
         * the pattern is not what says so.
         *
         * `findMut` answers `Maybe(&'v)`: what `Just(v)` binds is the map's own storage reached
         * through a mutable loan, and `v = v + 1` writes through it. Read at the shared access every
         * other payload takes, the binding was a read-only view of a writable thing and the
         * assignment reported "this is not declared `&`" about a name the author could not have
         * declared - the capability is on the *value*, exactly as `hold` reads it off one, and `&v`
         * in a pattern is the other question (`doc/spec/ownership.md`'s two axes: `take` moves the
         * payload out, `writable` says the storage it landed in may be written).
         *
         * `Access::Unique` alone is not enough to tell: an ordinary owned field reached through an
         * exclusively-borrowed pivot carries it too, folded in by `fieldPlace` from the pivot's own
         * contract rather than declared on the field. `binding.ownedField` is what tells the two
         * apart - it is the one bit `resolveConstructorPattern` and `resolveTuplePattern` compute
         * from the declaration itself, before that folding happens.
         */
        auto carried = binding.place->occurrence.contract;
        auto reference =
            !binding.take && !binding.ownedField && carried.access() == Access::Unique;

        auto value = binding.take
            ? readPlace(builder, binding.source, *binding.place, bindingRequest(Access::Owned))
            : reference
                ? readPlace(builder, binding.source, *binding.place, requestOf(carried))
                : readPlace(builder, binding.source, *binding.place,
                            bindingRequest(Access::Shared));
        if(!value) continue;

        auto local = builder.addLocal(binding.name, valueOccurrence(*value), binding.source,
                                      binding.writable || reference);
        auto place = builder.localPlace(*local);
        builder.addWrite(binding.source, *place, *value, Destination::Init);

        if(binding.name != StringId()) bindLocal(binding.name, local);
    }
}

/*
 * What a destructuring declaration matches against.
 *
 * `evaluatedPlace` with the declaration's own conventions read first, which is the one thing a `match`
 * has not got. `->` and `&` on a `let` say how the *initializer* is taken; a pattern takes the value
 * apart by reading through it, and what each name it binds takes is written on the name -
 * `Just(->v)` - which is where a pattern's convention is written everywhere else. Two spellings of
 * one question would be two answers to it, so the declaration's is refused here rather than being
 * quietly given the pattern's meaning.
 */
/*
 * A pattern with nowhere left to fail to - `bindIrrefutable`'s whole question, and `resolveBinding`'s
 * zero-alternatives branch beside it: both build a `PatternSpace` and a `Carry` over one pattern,
 * resolve it with no failure block (`onFail = nullptr`), and diagnose the gap if it is not covered.
 *
 * The emission comes first and the coverage report second, since there are two ways to fail here
 * rather than one. A pattern the *type* has no reading for at all is not a pattern that can fail:
 * `{a, b}` over an `I32` leaves a gap only because it matches nothing at all, so reporting the gap
 * first would say "this can fail" about a shape that is simply wrong. `resolvePattern` names the
 * type, and what is left to report once it has is a pattern that fits and is not exhaustive -
 * `diagnose` is the two callers' own wording for that, since `bindIrrefutable`'s "%@ has no
 * alternative" and `resolveBinding`'s "the declaration needs alternatives" are not one format
 * string apart.
 */
template<class Diagnose>
bool Resolver::resolveWithNoAlternative(Builder& builder, ast::Pat& pat, Place& pivot, bool covers,
                                        Carry& carry, SmallArray<PatternBinding, 4>& made,
                                        Diagnose&& diagnose) {
    if(!resolvePattern(builder, pat, pivot, made, nullptr, carry)) return false;
    if(!covers) diagnose();

    return true;
}

/*
 * A pattern where nothing can be written after `|` - see the declaration.
 *
 * A refutable one is reported and then bound **anyway**, which is `resolveBinding`'s rule and is
 * what keeps one mistake to one message: the names are what the loop body was written against, and
 * a second report about every read of one would be about this failure rather than about them.
 */
bool Resolver::bindIrrefutable(Builder& builder, ast::Pat& pat, Place& pivot, StringView what) {
    PatternSpace space(*this, pivot.occurrence.type);
    Carry carry(*this, pat.source);
    auto covers = space.add(pat);

    SmallArray<PatternBinding, 4> made;
    if(!resolveWithNoAlternative(builder, pat, pivot, covers, carry, made, [&] {
        context.diagnostics.error(
            "this pattern can fail - it does not match %@ - and %@ has no alternative for a value it does not match"_v,
            pat.source, space.gap(), what);
    })) {
        return false;
    }

    // Bound **anyway**, which is `resolveBinding`'s rule and is what keeps one mistake to one
    // message: the names are what the body was written against, and a second report about every
    // read of one would be about this failure rather than about them.
    bindPattern(builder, made);
    return true;
}

Place* Resolver::destructuringPivot(Builder& builder, const ast::VarDecl& binding) {
    if(binding.bind != ast::BindType::Borrow) {
        unsupported(binding.pat.source, "`->` or `&` on a declaration that destructures"_v);
        return nullptr;
    }

    return evaluatedPlace(builder, *parse[binding.content]);
}

/*
 * The refutable declaration - see the header, where the rule is stated.
 *
 *      <pivot>
 *      tests                       -> failure
 *    success:  the rest of the block, with what the pattern bound in scope
 *
 *    failure:  the alternatives, each of which has to leave
 *
 * One block chain and no merge, which is what separates this from `armsValue` and is the whole of
 * why the names may stay in scope: there is no path from an alternative back to the code below, so
 * the code below is reached only from `success` and the pattern matched on it.
 */
bool Resolver::resolveBinding(Builder& builder, const ast::VarDecl& binding, Place& pivot,
                              SmallArray<PatternBinding, 4>& made) {
    /*
     * The patterns, copied out of the parse list once and kept - `resolveMatch`'s reason, which is
     * that a `PatternSpace` keys an opaque test by the pattern's *address*. The declaration's own
     * pattern is copied for the same reason and into a local rather than into the array, since it is
     * not one of the alternatives and the space adds it first.
     */
    auto written = binding.alts;
    auto alternatives = written.contents(parse);
    auto declared = binding.pat;

    Array<ast::Pat> patterns;
    patterns.reserve(U32(alternatives.size()));
    for(auto alternative: alternatives) patterns.push(alternative.pat);

    PatternSpace space(*this, pivot.occurrence.type);
    Carry carry(*this, declared.source);

    /*
     * A pattern that always matches, which is an ordinary binding - and an **error** where something
     * was written after it.
     *
     * `|` is a reserved operator: it separates a pattern's alternatives, a `data` declaration's
     * constructors and a record update's source, so `a | b` is never a bitwise or. `let flags = a | b`
     * therefore binds `flags` to `a` and drops `b`, and the alternative is not merely unreachable -
     * it is never resolved at all, so the name inside it is never looked up either. Whichever half
     * the author meant, the other half is wrong, which is what makes this an error rather than the
     * warning about dead code it looks like.
     */
    auto covers = space.add(declared);

    if(covers) {
        // Reported and then bound anyway, which is what keeps one mistake to one message: the names
        // are what the rest of the block was written against, and a second report about every read
        // of one would be about this failure rather than about them.
        if(alternatives.size()) {
            context.diagnostics.error(
                "this pattern always matches, so the `|` after the initializer can never run - if this was meant as a bitwise or, write ``a `or` b``; after a `let` initializer `|` introduces the alternatives that only a pattern which can *fail* needs"_v,
                declared.source);
        }

        return resolvePattern(builder, declared, pivot, made, nullptr, carry);
    }

    if(!alternatives.size()) {
        // Bound anyway and without its tests, on `resolveWithNoAlternative`'s terms - see the
        // declaration, which is `bindIrrefutable`'s own shape with this branch's wording: one
        // report about the declaration is enough for it, and the names are what the rest of the
        // block was written against.
        resolveWithNoAlternative(builder, declared, pivot, covers, carry, made, [&] {
            context.diagnostics.error(
                "this pattern can fail - it does not match %@ - so the declaration needs alternatives, written `| return ...` or one arm per case"_v,
                declared.source, space.gap());
        });

        return true;
    }

    auto failure = builder.addBlock(StringId());
    carry.declare(builder, *failure);

    if(!resolvePattern(builder, declared, pivot, made, failure, carry)) return false;

    // Where the walk carries on, and what it was carrying there. Both are read back at the end,
    // because everything between here and there is built in the failure block.
    auto success = builder.currentBlock();
    SmallArray<Node*, 4> carried;
    carry.save(carried);

    /*
     * The alternatives, in the block the pattern's failure reaches.
     *
     * Resolved **before** the caller binds what the pattern bound, which is what puts those names
     * out of scope here with no scope to push and pop: they are live on the matching path and this
     * is the other one.
     */
    builder.setBlock(*failure);
    carry.arrive(builder, *failure, 0);

    auto covered = false;

    for(Size i = 0; i < patterns.size(); i++) {
        if(covered) {
            context.diagnostics.warning(
                "this alternative is unreachable - the ones before it already cover every value"_v,
                patterns[i].source);
            continue;
        }

        if(!space.useful(patterns[i])) {
            context.diagnostics.warning(
                "this alternative is unreachable - an earlier one already covers it"_v,
                patterns[i].source);
        }

        // The first alternative that completes the space is the one emitted with no failure edge -
        // `resolveMatch`'s rule, over the same space, with the declaration's own pattern in it.
        covered = space.add(patterns[i]);
        auto next = covered ? (Block*)nullptr : builder.addBlock(StringId());
        if(next) carry.declare(builder, *next);

        SmallArray<PatternBinding, 4> armBindings;
        if(resolvePattern(builder, patterns[i], pivot, armBindings, next, carry)) {
            auto scope = U32(bindings.size());
            bindPattern(builder, armBindings);

            // The arm's own copy, for the reason the patterns are copied: reading one out of a parse
            // list produces a value, and `resolveStatement` takes the expression by reference.
            auto arm = alternatives[i];
            auto errors = context.diagnostics.errorCount();
            auto placeholders = quietErrors;
            resolveStatement(builder, arm.expr);

            // An alternative that failed to resolve did not leave the block either, and saying so as
            // well would repeat the first report in weaker words - which is also true of one the
            // parser left a placeholder in, whose report is the parser's.
            if(!departed(builder) && errors == context.diagnostics.errorCount() &&
               placeholders == quietErrors) {
                context.diagnostics.error(
                    "an alternative of a declaration has to leave the block - return, break, or continue"_v,
                    arm.expr.source);
            }

            bindings.resize(scope);
        }

        if(!next) break;

        builder.setBlock(*next);
        carry.arrive(builder, *next, 0);
    }

    // The pattern and its alternatives are one space, which is what makes `let Just(Red) = m` need
    // three arms and not a wildcard: what is left uncovered is what neither half matched.
    if(!covered) {
        context.diagnostics.error("the alternatives of this declaration do not cover %@"_v,
                                  declared.source, space.gap());
    }

    builder.setBlock(*success);
    carry.restore(carried);
    return true;
}

/*
 * An expression **evaluated once**, reachable as a place - made if the source did not name one.
 *
 * Two constructs need exactly this and it is the same need read two ways. A pattern *projects*, so
 * it needs storage: `match f(): ...` matches a value nobody has a place for, and `hold` is the same
 * temporary `f().x` already makes - a pattern over a call result and a pattern over a local are one
 * thing. A format hole is *read twice* - once for its bound and once for its text - and a value
 * resolved twice would run its expression twice, while one node used twice would be an owner handed
 * to two positions.
 */
Place* Resolver::evaluatedPlace(Builder& builder, ast::Expr& expr) {
    ErrorMark mark(context);
    auto place = resolvePlace(builder, expr, false);
    if(place) return place;
    if(mark.reported()) return nullptr;

    auto value = resolveValue(builder, expr, nullptr, bindingRequest(Access::Shared));
    if(!value) return nullptr;

    return hold(builder, *value, expr.source);
}

/*
 * The condition of an `if` or a `while`, in two halves.
 *
 * The halves are a *value* and *control flow*, and they belong in different blocks: the pivot is
 * evaluated once where the condition is written, and the tests are emitted inside the alternative
 * they guard so that a failing one reaches the next alternative rather than a merge of its own. So
 * this resolves the pivot and `resolveTests` emits the tests, and the two are called from either
 * side of `armsValue`'s block layout.
 *
 * Here rather than in `resolveValue` because the names a pattern binds have a *scope*, and the scope
 * is the arm this condition selects. `resolveValue` answers a value and has nowhere to put them.
 */
Condition Resolver::resolveCondition(Builder& builder, ast::Expr& expr, Chain& chain) {
    /*
     * A **conjunction** is a sequence of tests rather than a value that is then tested.
     *
     * That is the whole of what makes `if m is Just(v) && v > 0` work: `&&` already runs its right
     * operand in a block of its own, and what it did not do was carry the left operand's bindings
     * into that block - because `m is Just(v)` in a value position has no arm for a name to live in
     * and discards it before `&&` is reached. Split here instead and each operand is a *test* whose
     * failure goes to the same place the whole condition's does, so the names are in scope for
     * everything to the right of them exactly as they are for the arm.
     *
     * The shape is the language's own rule about `&&` and not a special case for `is`: the
     * short-circuit is what makes the right operand's evaluation conditional on the left, and a
     * binding that is in scope only where the pattern matched is the same statement.
     */
    if(expr.kind == ast::Expr::Infix) {
        if(!flattenChain(expr, chain)) return Condition {};

        if(!splitsAtConjunction(chain)) {
            // Not a conjunction, so the whole chain is one value - climbed here rather than through
            // `resolveTest`, which would flatten it a second time to reach the same answer.
            Condition condition;
            condition.source = expr.source;

            Size operandIndex = 0;
            Size operatorIndex = 0;
            auto value = climb(builder, chain, operandIndex, operatorIndex, 0, nullptr,
                                       inspectRequest());
            if(!value) return condition;

            condition.value = truthValue(builder, *value, expr.source);
            return condition;
        }

        Size operandIndex = 0;
        Size operatorIndex = 0;

        auto condition = resolveSpan(builder, chain, operandIndex, operatorIndex);
        condition.chain = &chain;
        condition.operandIndex = operandIndex;
        condition.operatorIndex = operatorIndex;
        return condition;
    }

    return resolveTest(builder, expr);
}

/*
 * One test - a pattern against a pivot, or an expression asked its `Truth`.
 *
 * `doc/spec/expressions.md`: a condition is "an expression whose type has a `Truth` instance", and
 * `Truth` never applies through a conversion - so the expression is resolved with **no hint**, at
 * its own type, and `truthValue` is what turns it into a `Bool`. At a `Bool` that is the identity
 * and nothing is built; at an integer it is the test against zero; at anything else it is whatever
 * the library or the program declared, or a report that nothing did.
 *
 * The pivot is resolved here and the pattern is not: what a condition produces before its arms exist
 * is the *value* half, and the tests are emitted by `emitTest` inside the alternative they guard.
 */
Condition Resolver::resolveTest(Builder& builder, ast::Expr& expr) {
    Condition condition;
    condition.source = expr.source;

    if(expr.kind != ast::Expr::Is) {
        auto value = resolveValue(builder, expr, nullptr, inspectRequest());
        if(!value) return condition;

        condition.value = truthValue(builder, *value, expr.source);
        return condition;
    }

    auto& written = *parse[expr.is];

    condition.pivot = evaluatedPlace(builder, written.value);
    condition.pat = &written.pat;
    return condition;
}

/*
 * One `&&`-separated span of a flattened conjunction.
 *
 * A span of **one** operand is resolved as a test, which is what lets it be a pattern: `is` binds
 * only where there is an arm for the name, and a span is one. A longer span - `a == 1` in
 * `a == 1 && b` - is climbed into a single `Bool`, stopping at the next `&&` because the climb's
 * minimum precedence is one rung above the conjunction's.
 */
Condition Resolver::resolveSpan(Builder& builder, Chain& chain, Size& operandIndex,
                                Size& operatorIndex) {
    auto& first = *chain.operands[operandIndex];

    // The operator standing after this operand, if any. `splitsAtConjunction` has already said that
    // nothing in this chain binds looser than the `&&`s, so an operator here that is one is where
    // this span ends.
    auto alone = operatorIndex >= chain.operators.size() ||
                 chain.operators[operatorIndex].form == OperatorForm::ShortAnd;

    if(alone) {
        operandIndex++;
        return resolveTest(builder, first);
    }

    Condition condition;
    condition.source = first.source;

    /*
     * One rung above the conjunction's own precedence, read off **this chain**.
     *
     * `&&`'s grouping is a declaration - `lib/Core/Core.yana`'s `infixl 2 &&` - so the number is
     * not one this file may hold. It does not have to look it up either: `splitsAtConjunction` has
     * already established that every loosest operator here is a `ShortAnd`, so the first one of them
     * is carrying the precedence the declaration gave it, and stopping one above that is exactly
     * "climb until the next `&&`".
     */
    U8 conjunction = 0;
    for(auto& op: chain.operators) {
        if(op.form == OperatorForm::ShortAnd) { conjunction = op.precedence; break; }
    }

    auto value = climb(builder, chain, operandIndex, operatorIndex, U8(conjunction + 1), nullptr,
                       inspectRequest());
    if(!value) return condition;

    condition.value = truthValue(builder, *value, condition.source);
    return condition;
}

bool Resolver::emitTest(Builder& builder, const Condition& condition, Block* onFail, Carry& carry,
                        SmallArray<PatternBinding, 4>& into) {
    if(condition.pat) {
        return resolvePattern(builder, *condition.pat, *condition.pivot, into, onFail, carry);
    }

    if(!condition.value) return false;

    if(onFail) failIfNot(builder, *condition.value, *onFail, carry, condition.source);
    return true;
}

bool Resolver::resolveTests(Builder& builder, const Condition& condition, Block* onFail,
                            Carry& carry, SmallArray<PatternBinding, 4>& into) {
    if(!emitTest(builder, condition, onFail, carry, into)) return false;

    // How much of `into` is already in scope. A conjunction binds between its own spans, and `into`
    // accumulates across all of them - a caller with no arm for the names reports about every one
    // the condition made and not only the last span's.
    U32 bound = 0;

    if(condition.chain) {
        auto& chain = *condition.chain;
        auto operandIndex = condition.operandIndex;
        auto operatorIndex = condition.operatorIndex;

        while(operatorIndex < chain.operators.size()) {
            /*
             * The names everything to the left bound, in scope before the next span is resolved.
             *
             * This is the feature: `v` is storage of the block the earlier tests proved, so the
             * operand after the `&&` reads it exactly as the arm does.
             */
            bindPattern(builder, into, bound);
            bound = U32(into.size());

            operatorIndex++;

            auto next = resolveSpan(builder, chain, operandIndex, operatorIndex);
            if(!next) return false;
            if(!emitTest(builder, next, onFail, carry, into)) return false;
        }
    }

    bindPattern(builder, into, bound);
    return true;
}

/*
 * `x is p` in a position that is not an `if` - `let b = o is Exit(_)`.
 *
 * Two alternatives answering `True` and `False`, which is what the test *is* once it is control
 * flow. A binding here would name storage for the rest of the enclosing scope on the strength of a
 * test whose answer is a value the scope may never look at, so what would be bound is refused rather
 * than quietly dropped.
 */
Node* Resolver::resolveIs(Builder& builder, ast::Expr& expr) {
    // `resolveTest` rather than `resolveCondition`: a value position is reached from `climb`, which
    // has already split the chain, so what stands here is one operand and never a conjunction.
    auto condition = resolveTest(builder, expr);
    if(!condition.pivot) return nullptr;

    return armsValue(builder, expr.source, 2, booleanType(program), true,
        [&](U32 index, Block* onFail, DeclPtr<Type>, Carry& carry) -> Node* {
            if(index == 1) return constantBool(builder, expr.source, false);

            // `emitTest` and not `resolveTests`: there is no conjunction here and nothing to put in
            // scope, which is what the report below is about.
            SmallArray<PatternBinding, 4> made;
            if(!emitTest(builder, condition, onFail, carry, made)) return nullptr;

            if(made.size()) {
                context.diagnostics.error(
                    "%@ is bound by this pattern, and here there is no arm for it to be in scope in - write `if ... is ...`"_v,
                    made[0].source, context.findName(made[0].name));
                return nullptr;
            }

            return constantBool(builder, expr.source, true);
        });
}

/*
 * Whether a match over a tuple **literal** can skip building it.
 *
 * `match {lhs, rhs}` pairs two scrutinees and nothing more - the tuple exists to be taken apart by
 * the very next thing that reads it. Building it anyway is a second owner of each element: a tuple
 * is contiguous storage, so its fields are copied in, and a copy of a value with a teardown gets a
 * teardown of its own. `Eq(Maybe(a))`'s `==` written this way consumed both operands it was only
 * lent. The elements are already values here, and matching against them directly is the same tests
 * with no storage in between.
 *
 * Every alternative has to be one this can serve - a positional field list of the right arity, or
 * `_`. A pattern that binds the whole pivot by name genuinely wants the tuple to exist, and a named
 * field refers to labels an unlabelled literal has not got; either sends the whole match back to
 * building it, because the alternatives all read one pivot and it cannot be two shapes at once.
 */
static bool decomposeMatch(ast::ParseBase parse, ast::Expr& pivot,
                           ast::ParseList<ast::Alt> alternatives) {
    if(pivot.kind != ast::Expr::Tup) return false;

    auto written = pivot.tup;
    if(!written.size()) return false;

    Size arity = 0;
    for(auto argument: written.contents(parse)) {
        if(argument.name != StringId()) return false;
        arity++;
    }

    for(auto alternative: alternatives.contents(parse)) {
        auto pattern = alternative.pat;
        if(pattern.kind == ast::Pat::Any) continue;
        if(pattern.kind != ast::Pat::Tup) return false;

        // A convention on the pattern is about the pivot's storage, and there is none.
        if(pattern.bind != ast::BindType::Borrow || pattern.sink) return false;
        if(pattern.asVar != StringId()) return false;

        Size fields = 0;
        auto supplied = pattern.tup;
        for(auto field: supplied.contents(parse)) {
            if(field.field != StringId()) return false;
            fields++;
        }

        if(fields != arity) return false;
    }

    return true;
}

/*
 * A field list matched against the elements themselves - see `decomposeMatch`.
 *
 * `resolveTuplePattern` with the projection removed, which is the whole difference: there is no
 * tuple to project out of, because the pivot was never built. Positional only, since
 * `decomposeMatch` declines a named field before anything reaches here.
 */
bool Resolver::resolveDecomposed(Builder& builder, ast::Pat& pat, SmallArray<Place*, 8>& elements,
                                 SmallArray<PatternBinding, 4>& into, Block* onFail, Carry& carry) {
    if(pat.kind != ast::Pat::Tup) return true;

    auto ok = true;
    auto position = Size(0);

    for(auto field: pat.tup.contents(parse)) {
        auto place = elements[position++];
        if(!resolvePattern(builder, *parse[field.pat], *place, into, onFail, carry)) ok = false;
    }

    return ok;
}


/*
 * ## `match` over constants, recognized - inventory D3
 *
 * The rules below are `resolve/map.h`'s, restated for this stage. What changed is where the size
 * question lives: there is none here beyond `kMapSpanLimit`, which is about what a placed table may
 * *be* rather than about whether one is worth having.
 */

// The bound - see the declaration, where it is argued.
bool mapSpanAdmits(I64 lowest, I64 highest, Size count) {
    constexpr I64 kMapSpanLimit = 1024;
    constexpr I64 kMapSpanSpread = 32;

    I64 span = 0;
    if(__builtin_sub_overflow(highest, lowest, &span)) return false;
    if(__builtin_add_overflow(span, I64(1), &span)) return false;

    return span <= kMapSpanLimit && span <= I64(count) * kMapSpanSpread;
}

/*
 * `_`, binding nothing and taking nothing - the only pattern that may fill in the selectors the
 * alternatives did not name.
 *
 * A `Pat::Var` binds the pivot under a name the arm may read, which is a different pattern with the
 * same shape, and `->` on a `_` takes the value out. Neither is work a table lookup performs.
 */
static bool mapWildcard(const ast::Pat& pattern) {
    return pattern.kind == ast::Pat::Any && pattern.asVar == StringId() && !pattern.sink &&
           pattern.bind != ast::BindType::Sink;
}

/*
 * The pattern is a plain `Con` pattern naming one constructor of `record`, and nothing else about
 * it has been decided yet - not whether it takes a payload apart, not whether an `as` binding names
 * the whole of it. `mapConstructor` and `matchCases` both start here before diverging on the
 * payload: a map table and a switch dispatch are two different optimizations built over the same
 * "this arm tests one whole constructor" question, and each still has its own rule for what a
 * pattern may do beyond that.
 */
bool Resolver::constructorPattern(ast::Pat& pattern, DeclPtr<RecordType> record, U32& index) {
    if(pattern.kind != ast::Pat::Con || pattern.bind != ast::BindType::Borrow || pattern.sink) {
        return false;
    }

    auto found = mir::findConstructor(context, program, here(), pattern.con.name, pattern.source);
    if(!found || found.record != record) return false;

    index = found.index;
    return true;
}

/*
 * The pattern is one constructor of `record`, tests nothing else, and binds nothing.
 *
 * Everything a pattern can do beyond naming a constructor is a reason to decline, and the list is
 * the eligibility rule read from the other side: an `as` binding names the pivot, a payload pattern
 * other than `_` tests or moves something, and a `&->` binding takes the payload out. None of those
 * is work a table lookup performs, so a match containing one is not this shape however its arms read.
 */
bool Resolver::mapConstructor(ast::Pat& pattern, DeclPtr<RecordType> record, U32& index) {
    if(pattern.asVar != StringId()) return false;
    if(!constructorPattern(pattern, record, index)) return false;

    // A payload pattern is admitted only where it is `_`: irrefutable, binding nothing, moving
    // nothing. `Just(v)` and `Just(->v)` both name the payload and are declined.
    if(pattern.con.pats) {
        auto& payload = *parse[pattern.con.pats];
        if(payload.kind != ast::Pat::Any || payload.asVar != StringId() || payload.sink) return false;
    }

    return true;
}

/*
 * A form the arm's body may be evaluated as a constant in without a diagnostic being produced for a
 * program the ordinary path would have accepted - `expr_map.cpp`'s `constantForm`, unchanged.
 *
 * `resolveConstant`'s `notConstant` flag turns "this is not a constant *form*" into an answer, and
 * that covers almost everything. What it deliberately does not cover is the right form with the
 * wrong contents, and two of those would fire on programs that resolve perfectly well as
 * expressions: an ascription in a position whose type is something else, and a constructor of a
 * *generic* record with nothing to infer its arguments from. Both are refused here rather than
 * reported there, so that the recognizer is silent by construction.
 */
static bool mapConstantForm(const ast::Expr& body, bool typed) {
    if(body.kind == ast::Expr::Coerce) return false;
    if(typed) return true;

    return body.kind != ast::Expr::Con && body.kind != ast::Expr::Tup &&
           body.kind != ast::Expr::Array;
}

// An insertion sort because the list is bounded by `mapSpanAdmits` and is almost always already
// ordered: a declaration that pins nothing numbers its constructors from zero. See the declaration.
void sortMapEntries(MapEntry* entries, Size count) {
    for(Size i = 1; i < count; i++) {
        for(Size j = i; j && entries[j].selector < entries[j - 1].selector; j--) {
            auto held = entries[j];
            entries[j] = entries[j - 1];
            entries[j - 1] = held;
        }
    }
}

Node* Resolver::matchAsMap(Builder& builder, ast::Expr& expr, Place& pivot,
                           Buffer<ast::Pat> patterns, DeclPtr<Type> hint, Request wanted) {
    /*
     * A position a value produced *here* satisfies.
     *
     * Every arm of a mapping is a constant, and a constant is an owned occurrence minted where it is
     * named - so a position that asked for a window into somebody else's storage, or published an
     * origin, or said something about the value's interior, is asking for something a table cannot
     * answer. Those go to `armsValue`, where the arm is resolved at the request the caller wrote.
     */
    if(wanted.published || wanted.form != AccessForm::WholeValue || wanted.shape) return nullptr;

    auto pivotType = pivot.occurrence.type;
    if(!pivotType) return nullptr;

    auto kind = decl()[pivotType]->kind;
    auto declaration = recordDeclarationOf(decl(), pivotType);

    /*
     * The two domains, and what "closed" means in each.
     *
     * A **record**'s own constructors are the whole domain: a well-typed value holds one of them and
     * nothing else, so there is no out-of-range case to guard and no fallback. An **integer** pivot
     * is open, so it needs a wildcard arm and the mapping carries what that arm answers.
     *
     * The integer domain is new - `recognizeConstructorMap` requires a record - and `monthName` in
     * `lib/Date/Format.yana` is what it is for. It is restricted to `Type::Int` because a machine
     * integer's equality is the compiler's own operation: over a type with an authored `Eq`, the
     * chain runs a body the program wrote and arithmetic here would be a different program.
     */
    RecordType* record = nullptr;

    if(kind == Type::Record) {
        record = (RecordType*)decl()[pivotType];

        // One constructor is no selector at all, and the pattern that names it is irrefutable - so
        // `armsValue` already emits the arm with no test and there is nothing here to improve on.
        if(record->cons.size() < 2) return nullptr;
        if(!tagTypeOf(decl(), program.types, pivotType)) return nullptr;
    } else if(kind != Type::Int) {
        return nullptr;
    }

    auto closed = record != nullptr;

    SmallArray<MapEntry, 16> entries;
    SmallArray<bool, 16> answered;

    if(record) {
        for(auto arm: record->cons.contents(decl())) {
            MapEntry entry;
            entry.selector = arm.value;
            entries.push(entry);
            answered.push(false);
        }

        // Two constructors pinned to one number is a declaration this cannot map: the selector no
        // longer says which arm the value is. Whether it is a program worth reporting is the
        // declaration checker's question and not this one's.
        for(Size i = 0; i < entries.size(); i++) {
            for(Size j = i + 1; j < entries.size(); j++) {
                if(entries[i].selector == entries[j].selector) return nullptr;
            }
        }
    }

    /*
     * ## Pass one: the patterns alone
     *
     * Every pattern is read before any arm is evaluated, and that ordering is what keeps the
     * recognizer silent. Evaluating as it walked meant `match findAscii(...): Nothing -> "" |
     * Just(space) -> ...` built the first arm's literal and *then* found the binding in the second -
     * so a match that was never a mapping had already placed a global and, where the library's own
     * string builder was not yet resolvable, reported about it.
     */
    struct Arm {
        U32 alternative = 0;
        bool wildcard = false;
        U32 index = 0;
        I64 selector = 0;
    };

    SmallArray<Arm, 16> arms;
    auto covered = false;
    auto named = Size(0);

    for(Size i = 0; i < patterns.size() && !covered; i++) {
        auto& pattern = patterns[i];

        Arm arm;
        arm.alternative = U32(i);
        arm.wildcard = mapWildcard(pattern);

        if(!arm.wildcard && record) {
            if(!mapConstructor(pattern, declaration, arm.index)) return nullptr;
        } else if(!arm.wildcard) {
            /*
             * An integer literal, read at the pivot's type - `patternBound`'s rule, as a constant
             * rather than as a node: what a written number *is* is a question about the type it is
             * being compared with, and the sign is a separate token in front of it.
             */
            if(pattern.kind < ast::Pat::Lit) return nullptr;
            if(pattern.asVar != StringId() || pattern.sink) return nullptr;

            ast::Expr literal {};
            literal.lit = pattern.lit;
            literal.source = pattern.source;
            literal.kind = ast::Expr::Kind(ast::Expr::Lit + (pattern.kind - ast::Pat::Lit));

            auto number = resolveLiteral(literal, pivotType, pattern.negative);
            if(number.kind != Constant::Int) return nullptr;

            arm.selector = I64(reducedToWidth(*(IntType*)decl()[pivotType], number.asInt()));
        }

        // A repeated selector's second arm is unreachable, and so is a wildcard with nothing left to
        // fill. Both are warned about by `PatternSpace`, and neither contributes a result.
        auto duplicate = false;
        if(!arm.wildcard && record) {
            duplicate = answered[arm.index];
        } else if(!arm.wildcard) {
            for(auto& previous: arms) {
                duplicate = duplicate || (!previous.wildcard && previous.selector == arm.selector);
            }
        }

        if(duplicate) continue;

        if(arm.wildcard) {
            named = record ? entries.size() : named;
            covered = true;
        } else if(record) {
            answered[arm.index] = true;
            covered = ++named == entries.size();
        }

        arms.push(arm);
    }

    /*
     * Not every selector answered, which is a match the ordinary path either completes with a
     * pattern this does not read or reports as inexhaustive. Either way it is that path's to finish.
     */
    if(!covered) return nullptr;

    /*
     * ## Pass two: the arms
     *
     * A **string** arm is evaluated with no hint at all, and that is not an optimization: a hinted
     * string constant queues a `PendingTextDefault` against that type, which is a report about a
     * position rather than an answer - and a recognizer that declined afterwards would have left one
     * behind. The role answers the type instead, which is the same answer the hinted path reaches.
     */
    Constant fallback;
    auto resultType = hint;
    auto alternatives = parse[expr.match]->alts;
    auto holdsAnyText = false;

    if(record) for(auto& flag: answered) flag = false;
    entries.clear();

    if(record) {
        for(auto arm: record->cons.contents(decl())) {
            MapEntry entry;
            entry.selector = arm.value;
            entries.push(entry);
        }
    }

    for(auto& arm: arms) {
        auto alternative = alternatives.get(parse, arm.alternative);

        if(!mapConstantForm(alternative.expr, resultType != nullptr)) return nullptr;

        auto notConstant = false;
        auto hinted = isStringLiteral(alternative.expr) ? nullptr : resultType;
        auto value = resolveConstant(alternative.expr, hinted, &notConstant);
        if(!value.isSet()) return nullptr;

        // The first arm decides the type where the position did not. Every later one is evaluated
        // against it, so this can only differ for the first - and it is checked for all of them,
        // since an arm may come out at another type without an ascription saying so.
        if(!resultType) resultType = value.type;
        if(!sameType(value.type, resultType)) return nullptr;

        holdsAnyText = holdsAnyText || holdsText(decl(), value);

        if(arm.wildcard) {
            if(!record) { fallback = value; continue; }

            for(Size c = 0; c < entries.size(); c++) {
                if(answered[c]) continue;

                entries[c].value = value;
                answered[c] = true;
            }

            continue;
        }

        if(record) {
            entries[arm.index].value = value;
            answered[arm.index] = true;
            continue;
        }

        MapEntry entry;
        entry.selector = arm.selector;
        entry.value = value;
        entries.push(entry);
    }

    if(!entries.size()) return nullptr;

    /*
     * What a table slot may hold - `map.h`'s `tableElement`, restated over a type.
     *
     * A value with a teardown produced out of static bytes would be a second owner of something the
     * table still holds, so the ordinary rule is that the result duplicates freely and owes nothing.
     * A **string** is the one exemption and it is the same one the shipping stage makes: a literal's
     * run is borrowed, so what a slot holds is two words describing bytes nobody owns and its
     * teardown is the nothing a written literal's is. `stringConstant` is where that is argued, and
     * the constant in the slot is what that function returns.
     */
    auto ownership = ownershipOf(decl(), resultType);
    auto text = program.roles.record(intrinsic::Known::string);

    if(!(ownership.trivialCopy && ownership.relocatable && !ownership.needsTeardown()) &&
       !(text && recordDeclarationOf(decl(), resultType) == text)) {
        return nullptr;
    }

    // A value with an indirection in it has nothing to point at in static storage - the bytes that
    // belong there are the result of a call to the allocator. Asked here so that a boxed result
    // declines to the ordinary path rather than failing to emit.
    for(auto& entry: entries) {
        if(constantNeedsAllocation(decl(), entry.value)) return nullptr;
    }

    if(fallback.isSet() && constantNeedsAllocation(decl(), fallback)) return nullptr;

    // Ascending by selector, which is what the node declares and what every shape the lowering
    // derives walks the list once assuming. `@value(n)` is what makes the sort necessary: a
    // declaration may pin its constructors to numbers in any order at all.
    sortMapEntries(entries.pointer(), entries.size());

    // The bound on what a placed table may be - see `mapSpanAdmits`.
    if(!mapSpanAdmits(entries[0].selector, entries[entries.size() - 1].selector, entries.size())) {
        return nullptr;
    }

    /*
     * The selector, read once.
     *
     * A record's is its discriminant - the same place `resolveConstructorPattern` compares, so an
     * enumeration's number and a sum's tag are one reading rather than two - and an integer pivot's
     * is the value itself.
     */
    auto place = record ? builder.discriminantPlace(pivot) : &pivot;
    if(!place) return nullptr;

    auto selector = readPlace(builder, expr.source, *place, inspectRequest());
    if(!selector) return nullptr;

    auto node = builder.addMap(expr.source, *selector, entries.pointer(), entries.size(), fallback,
                               closed, owned(resultType));
    if(!node) return nullptr;

    // A literal in a slot needs the byte globals and the record rows a string is spelled from, and
    // which of those are in by now depends on the order the files were walked in. So the deferral -
    // see `PendingTextMap`.
    if(holdsAnyText) {
        pendingTextMaps.push(PendingTextMap {
            (NodeMap*)node - mir(), here(), current->activeFile, expr.source
        });
    }

    return node;
}


/*
 * The string tables, once every body is in - `PendingTextMap`, where the ordering is argued.
 *
 * The module scope is restored per entry for `resolveTextGlobals`' reason: the evaluation places the
 * bytes of each literal, and they belong to the module that wrote the mapping.
 *
 * One blob per distinct **text** within one mapping, which is not what a written literal gets and is
 * deliberate: constant equality is what decides the uniform form, so two arms answering `"colour"`
 * have to be the same constant or a mapping with one answer becomes a table with five.
 */
void Resolver::resolveTextMaps() {
    for(auto& pending: pendingTextMaps) {
        ModuleScope scope(*this, pending.module, pending.file);

        auto& node = *mir()[pending.node];
        auto entries = node.entries.contents(mir());

        SmallArray<StringId, 8> texts;
        SmallArray<Constant, 8> built;

        auto resolved = [&](const Constant& value) {
            if(value.kind != Constant::Text) {
                return holdsText(decl(), value) ? textsResolved(value, pending.source) : value;
            }

            auto written = value.asText();
            for(Size i = 0; i < texts.size(); i++) {
                if(texts[i] == written) return built[i];
            }

            auto made = stringConstant(written, pending.source);
            if(!made.isSet()) return made;

            texts.push(written);
            built.push(made);
            return made;
        };

        for(Size i = 0; i < entries.size(); i++) {
            auto entry = entries[i];
            entry.value = resolved(entry.value);
            if(!entry.value.isSet()) break;

            node.entries.set(mir(), i, entry);
        }

        if(node.fallback.isSet()) node.fallback = resolved(node.fallback);
    }

    pendingTextMaps.clear();
}


/*
 * Whether this sub-pattern can fail - the question a multi-way branch has to answer `no` to.
 *
 * A `switch` proves the *tag* and nothing else, so an alternative reached through one is emitted
 * with **no failure edge at all**. That is sound exactly where the pattern under the constructor
 * tests nothing: a name and `_` bind, a braced list of them takes the value apart, and everything
 * else - a nested constructor, a literal, a range, an operator section - is a test that would have
 * had nowhere to fail to.
 *
 * The conservative direction is the safe one here in a way it usually is not: answering `false` for
 * a pattern that is in fact irrefutable costs a `switch` that was not built, and answering `true`
 * for one that is not costs a test that was silently dropped.
 */
static bool irrefutablePattern(ast::ParseBase parse, ast::Pat& pat) {
    switch(pat.kind) {
        case ast::Pat::Any:
        case ast::Pat::Var:
            return true;

        case ast::Pat::Tup: {
            for(auto field: pat.tup.contents(parse)) {
                if(!irrefutablePattern(parse, *parse[field.pat])) return false;
            }

            return true;
        }

        default:
            return false;
    }
}

/*
 * The case values a `match` dispatches on, or nothing - inventory D3's `switch` half.
 *
 * The shape admitted is narrow and deliberately so: every alternative but the last names a distinct
 * constructor of the pivot's record and tests nothing below the tag, and the last one is the
 * alternative exhaustiveness proved - which is what `armsValue` already emits with no failure edge,
 * and is therefore the fallback the node needs. `tested` is that proof, computed by `resolveMatch`
 * from `PatternSpace` and handed here rather than re-derived.
 *
 * Below three cases it declines, because that is a chain of two comparisons and the node's own rule.
 * It reports nothing, for `matchAsMap`'s reason: the ordinary path resolves the same alternatives.
 */
bool Resolver::matchCases(Place& pivot, Buffer<ast::Pat> patterns, const bool* tested,
                          SmallArray<I64, 16>& values, SmallArray<U32, 16>& arms) {
    auto pivotType = pivot.occurrence.type;
    if(!pivotType || decl()[pivotType]->kind != Type::Record) return false;

    auto& record = *(RecordType*)decl()[pivotType];
    auto declaration = recordDeclarationOf(decl(), pivotType);
    if(record.cons.size() < 2) return false;
    if(!tagTypeOf(decl(), program.types, pivotType)) return false;

    auto count = patterns.size();
    if(count < 4) return false;

    // The last alternative is the fallback, so it is the one that must carry no test - and every
    // one before it must, since an alternative that completed the match early leaves the ones after
    // it unreachable and a case for each of them.
    if(tested[count - 1]) return false;
    for(Size i = 0; i + 1 < count; i++) {
        if(!tested[i]) return false;
    }

    SmallArray<U32, 16> seen;

    for(Size i = 0; i + 1 < count; i++) {
        auto& pattern = patterns[i];
        U32 index = 0;

        if(!constructorPattern(pattern, declaration, index)) return false;

        // The tag and nothing under it - see `irrefutablePattern`.
        if(pattern.con.pats && !irrefutablePattern(parse, *parse[pattern.con.pats])) return false;

        for(auto previous: seen) {
            if(previous == index) return false;
        }

        seen.push(index);
        values.push(record.cons.get(decl(), index).value);
        arms.push(U32(i));
    }

    // Ascending and distinct, which is what the node declares - the alternative each case belongs to
    // moving with it. `@value(n)` is what makes both the sort and the duplicate check necessary: a
    // declaration is free to pin its constructors to numbers in any order, and two pinned to one
    // number is a declaration a selector cannot tell apart.
    for(Size i = 1; i < values.size(); i++) {
        for(Size j = i; j && values[j] < values[j - 1]; j--) {
            auto held = values[j];
            values[j] = values[j - 1];
            values[j - 1] = held;

            auto arm = arms[j];
            arms[j] = arms[j - 1];
            arms[j - 1] = arm;
        }
    }

    for(Size i = 1; i < values.size(); i++) {
        if(values[i] == values[i - 1]) return false;
    }

    return true;
}

/*
 * `match x: p -> e ...` - Analysis-IR.md's G7d.
 *
 * One `armsValue` over the alternatives, and the shape it produces is a chain of tests and a single
 * merge: alternative *k*'s tests all branch to alternative *k+1*'s entry, and every arm that
 * produces a value jumps into one join. A chain of two-armed branches would have given each test a
 * merge of its own to fall into, which a nested pattern cannot use - its inner test has to reach the
 * *next alternative* and not the end of the outer one.
 *
 * The alternative that **completes** the match carries no test. That is what exhaustiveness buys and
 * it is `PatternSpace`'s answer rather than "the last one": reaching it means every value that is
 * not one it matches has been ruled out, all the way down through nesting.
 */
// Whether the cursor sentinel names a constructor anywhere in a pattern - an arm's own, or one nested
// in its payload or fields.
bool Resolver::patternHasCursor(ast::Pat& pat) {
    switch(pat.kind) {
        case ast::Pat::Con:
            if(isCursorSentinel(context, pat.con.name)) return true;
            return pat.con.pats && patternHasCursor(*parse[pat.con.pats]);
        case ast::Pat::Tup:
            for(auto field: pat.tup.contents(parse)) {
                if(field.pat && patternHasCursor(*parse[field.pat])) return true;
            }
            return false;
        case ast::Pat::Arr:
            for(auto element: pat.arr.contents(parse)) {
                if(patternHasCursor(element)) return true;
            }
            return false;
        default:
            return false;
    }
}

Node* Resolver::resolveMatch(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue,
                             Request wanted) {
    auto& written = *parse[expr.match];

    /*
     * No alternatives, which the grammar produces only where the parser has already said so: this is
     * a `match` being typed, with its pivot written and nothing under it yet. So it is a placeholder
     * like any other - see `quietErrors` - and the pivot is still resolved in an editor's compile,
     * since it is what the author is asking about while the alternatives are written.
     */
    if(!written.alts.size()) {
        quietErrors++;
        if(recording(context)) resolvePlace(builder, written.pivot, false);
        return nullptr;
    }

    /*
     * The pivot: one place, or the several a tuple literal would have been built out of.
     *
     * The decomposed form still needs the tuple **type**, because exhaustiveness is stated over it -
     * a field list covers the product of its positions' spaces, and `PatternSpace` has no other way
     * to be told what the product is. Interning a type allocates no storage, which is the point:
     * what is skipped is the aggregate and the field copies, not the type.
     */
    auto decomposed = decomposeMatch(parse, written.pivot, written.alts);

    Place* pivot = nullptr;
    SmallArray<Place*, 8> elements;
    DeclPtr<Type> pivotType = nullptr;

    if(decomposed) {
        SmallArray<RecordField, 4> fields;

        for(auto argument: written.pivot.tup.contents(parse)) {
            auto place = evaluatedPlace(builder, argument.value);
            if(!place) return nullptr;

            elements.push(place);

            RecordField row;
            row.occurrence = owned(place->occurrence.type);
            row.source = argument.value.source;
            fields.push(row);
        }

        pivotType = tupleType(context, program, fields.pointer(), fields.size(),
                              written.pivot.source);
        if(!pivotType) return nullptr;
    } else {
        pivot = evaluatedPlace(builder, written.pivot);
        if(!pivot) return nullptr;

        pivotType = pivot->occurrence.type;
    }

    /*
     * The alternatives' patterns, copied out of the parse list once and kept.
     *
     * Reading one out of a `ParseList` copies it, and a `PatternSpace` keys an opaque test by the
     * pattern's *address* - so the copies have to be the ones both the space and the emission see,
     * and they have to keep their addresses for as long as the space is alive. `reserve` of the
     * exact count is what makes that true: `Array` reallocates only when a push needs space it does
     * not have, so no push below moves what an earlier one stored.
     */
    Array<ast::Pat> patterns;
    patterns.reserve(U32(written.alts.size()));
    for(auto alternative: written.alts.contents(parse)) patterns.push(alternative.pat);

    /*
     * A constructor being typed in one of the alternatives - `complete.cpp`'s pattern capture.
     *
     * Before the coverage walk rather than where the pattern is resolved, because the walk is what
     * stops first: the sentinel names no constructor, so the arm it stands in covers nothing, the
     * match is "not exhaustive", and the alternatives were never reached. So completion in a `match`
     * answered nothing at all - only a refutable `let` reached the capture.
     */
    if(wantsCompletion(context)) {
        for(auto& pattern: patterns) {
            if(!patternHasCursor(pattern)) continue;

            capturePatternCompletion(pivotType);
            return nullptr;
        }
    }

    /*
     * Which alternatives need a test, decided for the whole match before any of it is emitted.
     *
     * Two answers come out of the same walk. `useful` is whether an alternative matches anything the
     * ones before it did not, which is a warning and not an error - an unreachable arm is dead code
     * rather than a wrong program. `add` is whether the match is complete once this alternative is
     * in, and the first alternative for which it is true is the one emitted with no failure edge.
     */
    PatternSpace space(*this, pivotType);
    SmallArray<bool, 8> tested;
    auto exhaustive = false;

    for(Size i = 0; i < patterns.size(); i++) {
        if(exhaustive) {
            context.diagnostics.warning(
                "this alternative is unreachable - the ones before it already cover every value"_v,
                patterns[i].source);
        } else if(!space.useful(patterns[i])) {
            context.diagnostics.warning(
                "this alternative is unreachable - an earlier one already covers it"_v,
                patterns[i].source);
        }

        auto completes = !exhaustive && space.add(patterns[i]);
        tested.push(!completes);
        exhaustive = exhaustive || completes;
    }

    if(!exhaustive) {
        context.diagnostics.error("this match does not say what to do with %@"_v, expr.source,
                                  space.gap());
        return nullptr;
    }

    /*
     * The mapping, where the alternatives state one - inventory D3.
     *
     * **After** the warnings above and the exhaustiveness check, which is what keeps this silent:
     * every message a bad match has coming has already been given, so a refusal here is a
     * fall-through to `armsValue` and never a second opinion. A decomposed pivot is excluded because
     * a mapping selects on one value and a tuple pattern selects on several.
     */
    if(wantValue && pivot) {
        Buffer<ast::Pat> written(patterns.pointer(), patterns.size());
        if(auto map = matchAsMap(builder, expr, *pivot, written, hint, wanted)) return map;
    }

    /*
     * A multi-way branch, where the alternatives dispatch on one discriminant - D3's `switch` half.
     *
     * The cases are decided before anything is emitted (`matchCases`), and the dispatcher below is
     * what turns them into the terminator: it reads the pivot's discriminant once, in the block the
     * `match` was written in, and every entry block is a case target. `armsValue`'s own alternatives
     * are then emitted with no failure edge, which is exactly what an alternative whose tag has been
     * proved needs and is the shape it already gives the completing one.
     */
    SmallArray<I64, 16> cases;
    SmallArray<U32, 16> caseArms;
    auto multiway = !decomposed && pivot &&
                    matchCases(*pivot, Buffer<ast::Pat>(patterns.pointer(), patterns.size()),
                               tested.pointer(), cases, caseArms);

    return armsValue(builder, expr.source, U32(patterns.size()), hint, wantValue,
        [&](U32 index, Block* onFail, DeclPtr<Type> armHint, Carry& carry) -> Node* {
            auto alternative = written.alts.get(parse, index);

            SmallArray<PatternBinding, 4> made;
            auto fail = tested[index] ? onFail : nullptr;

            auto matched = decomposed
                ? resolveDecomposed(builder, patterns[index], elements, made, fail, carry)
                : resolvePattern(builder, patterns[index], *pivot, made, fail, carry);

            if(!matched) return nullptr;

            // The arm itself, with its own names in scope for exactly its own length.
            auto scope = U32(bindings.size());
            bindPattern(builder, made);

            auto value = resolveArm(builder, alternative.expr, armHint, wantValue, wanted);

            bindings.resize(scope);
            return value;
        },
        multiway,
        [&](Carry& carry, SmallArray<Block*, 8>& entries) {
            auto place = builder.discriminantPlace(*pivot);
            if(!place) return false;

            auto selector = readPlace(builder, expr.source, *place, inspectRequest());
            if(!selector) return false;

            SmallArray<Block*, 16> targets;
            for(auto arm: caseArms) targets.push(entries[arm]);

            // The carry, on every edge - `addBranch`'s rule said of more than two of them: a value
            // live before a multi-way branch is live on every arm of it.
            SmallArray<Operand, 8> carried;
            carry.edge(builder, carried);

            return builder.addSwitch(expr.source,
                                     builder.resultOf(*selector, UseMode::Inspect), cases.pointer(),
                                     targets.pointer(), cases.size(),
                                     *entries[U32(patterns.size() - 1)], carried.pointer(),
                                     carried.size()) != nullptr;
        });
}

} // namespace mir
