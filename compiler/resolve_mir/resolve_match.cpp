#include "resolve_internal.h"

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

    // Adds `pattern` to the space, and answers whether everything is now covered.
    bool add(ast::Pat& pattern) {
        covered.cells.push(&pattern);
        covered.rows++;

        SmallArray<String, 4> witness;
        return !missing(covered, witness);
    }

    // A value nothing added so far matches, written the way a pattern for it would be. Only
    // meaningful when `add` last answered false.
    String gap() {
        SmallArray<String, 4> witness;
        if(!missing(covered, witness) || !witness.size()) return String("_");
        return witness[0];
    }

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
                auto found = resolver.program.findConstructor(resolver.here(), pattern.con.name);
                if(!found || !type || decl()[type]->kind != Type::Record) return opaque();
                if(recordDeclarationOf(decl(), type) != found.record) return opaque();

                return PatternHead { found.index, 0, nullptr, PatternHead::Con };
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

    bool useful(const PatternMatrix& matrix, const PatternRow& row) {
        if(matrix.columns() == 0) return matrix.rows == 0;

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

    Resolver& resolver;
    PatternMatrix covered;
    SmallArray<CachedHead, 16> cache;
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

    if(pat.kind != ast::Pat::Con || !pat.con.pats) return;
    auto& written = *parse[pat.con.pats];

    if(written.kind != ast::Pat::Tup) {
        if(arm.count == 1) {
            into[0] = &written;
            return;
        }

        if(report) {
            context.diagnostics.error("%@ has %@ fields, and this names one"_v, written.source,
                                      context.findName(arm.name), U64(arm.count));
        }

        return;
    }

    auto position = U16(0);
    for(auto field: written.tup.contents(parse)) {
        auto index = ~U32(0);

        if(field.field) {
            for(U16 i = 0; i < arm.count; i++) {
                if(record.field(decl(), U16(arm.first + i)).name == field.field) index = i;
            }

            if(index == ~U32(0)) {
                if(report) {
                    context.diagnostics.error("%@ has no field called %@"_v, written.source,
                                              context.findName(arm.name),
                                              context.findName(field.field));
                }

                continue;
            }
        } else if(position < arm.count) {
            index = position;
        } else {
            if(report) {
                context.diagnostics.error("%@ has %@ fields, and this names more"_v, written.source,
                                          context.findName(arm.name), U64(arm.count));
            }

            continue;
        }

        position++;

        // A field named twice. Reported here rather than left to produce two bindings of one name,
        // because which of the two a later use meant is not a question with an answer.
        if(into[index]) {
            if(report) {
                context.diagnostics.error("%@ is matched twice"_v, written.source,
                                          context.findName(record.field(decl(),
                                                                        U16(arm.first + index)).name));
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
 * question about the type it is being compared with. So the width check, the sign and the normal
 * form are all `resolveLiteral`'s single answer, asked at the type the comparison will be at.
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

    auto constant = resolveLiteral(literal, type, pat.negative);
    if(!constant.type) return nullptr;

    return builder.addConst(pat.source, constant);
}

/*
 * One comparison of the pivot against a bound, as the test it is.
 *
 * `_` is a bound that says nothing - it is what makes `a.._` one-sided - so it emits no test and is
 * not a failure. Anything else is read, compared and branched on.
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
 * One pattern against one place, emitting the tests it needs and recording the names it introduces.
 *
 * `onFail` null says the caller has proved this pattern matches - the alternative exhaustiveness
 * showed completes the match - and everything below then takes the value apart and binds it without
 * asking any question about it. That is sound *through the nesting* and not only at the top, which
 * is what `PatternSpace` is for: the proof is over whole values rather than over head constructors.
 */
bool Resolver::resolvePattern(Builder& builder, ast::Pat& pat, Place& pivot,
                              SmallArray<PatternBinding, 4>& into, Block* onFail, Carry& carry) {
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

        into.push(PatternBinding { name, &place, pat.source, take, pat.sink });
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
            if(take) into.push(PatternBinding { StringId(), &pivot, pat.source, true, pat.sink });
            return true;
        case ast::Pat::Var:
            bind(pat.var, pivot);
            return true;

        case ast::Pat::Con:
            return resolveConstructorPattern(builder, pat, pivot, into, onFail, carry);

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

        default:
            break;
    }

    if(pat.kind >= ast::Pat::Lit) {
        return comparePattern(builder, BinaryOp::Eq, pivot, pat, onFail, carry, pat.source);
    }

    unsupported(pat.source, pat.kind == ast::Pat::Section ? "an operator pattern"_v
                          : pat.kind == ast::Pat::Tup     ? "a tuple pattern"_v
                          : pat.kind == ast::Pat::Arr ||
                            pat.kind == ast::Pat::Rest    ? "an array pattern"_v
                                                          : "this pattern"_v);
    return false;
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
    auto found = program.findConstructor(here(), pat.con.name);
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

        if(!resolvePattern(builder, *supplied[i], *place, into, onFail, carry)) ok = false;
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
void Resolver::bindPattern(Builder& builder, const SmallArray<PatternBinding, 4>& made) {
    for(auto binding: made) {
        auto access = binding.take ? Access::Owned : Access::Shared;
        auto value = readPlace(builder, binding.source, *binding.place, bindingRequest(access));
        if(!value) continue;

        auto local = builder.addLocal(binding.name, valueOccurrence(*value), binding.source,
                                      binding.writable);
        auto place = builder.localPlace(*local);
        builder.addWrite(binding.source, *place, *value, Destination::Init);

        if(binding.name != StringId()) bindings.push(Binding { binding.name, local });
    }
}

/*
 * What a pattern is matched *against* - a place, made if the source did not name one.
 *
 * A pattern projects, so it needs storage: `match f(): ...` matches a value nobody has a place for,
 * and `hold` is the same temporary `f().x` already makes. That the two go the same way is the point
 * - a pattern over a call result and a pattern over a local are one thing.
 */
Place* Resolver::matchPivot(Builder& builder, ast::Expr& expr) {
    auto place = resolvePlace(builder, expr, false);
    if(place) return place;
    if(context.diagnostics.errorCount()) return nullptr;

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
Condition Resolver::resolveCondition(Builder& builder, ast::Expr& expr) {
    Condition condition;
    condition.source = expr.source;

    if(expr.kind != ast::Expr::Is) {
        auto value = resolveValue(builder, expr, program.types.boolean, inspectRequest());
        if(!value) return condition;
        if(!checkType(*value, program.types.boolean, expr.source, "a condition"_v)) return condition;

        condition.value = value;
        return condition;
    }

    auto& written = *parse[expr.is];

    condition.pivot = matchPivot(builder, written.value);
    condition.pat = &written.pat;
    return condition;
}

bool Resolver::resolveTests(Builder& builder, const Condition& condition, Block* onFail,
                            Carry& carry, SmallArray<PatternBinding, 4>& into) {
    if(condition.pat) {
        return resolvePattern(builder, *condition.pat, *condition.pivot, into, onFail, carry);
    }

    if(!condition.value) return false;

    if(onFail) failIfNot(builder, *condition.value, *onFail, carry, condition.source);
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
    auto condition = resolveCondition(builder, expr);
    if(!condition.pivot) return nullptr;

    return armsValue(builder, expr.source, 2, program.types.boolean, true,
        [&](U32 index, Block* onFail, DeclPtr<Type>, Carry& carry) -> Node* {
            if(index == 1) return constantBool(builder, expr.source, false);

            SmallArray<PatternBinding, 4> made;
            if(!resolveTests(builder, condition, onFail, carry, made)) return nullptr;

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
Node* Resolver::resolveMatch(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue,
                             Request wanted) {
    auto& written = *parse[expr.match];

    if(!written.alts.size()) {
        context.diagnostics.error("a match needs at least one alternative"_v, expr.source);
        return nullptr;
    }

    auto pivot = matchPivot(builder, written.pivot);
    if(!pivot) return nullptr;

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
     * Which alternatives need a test, decided for the whole match before any of it is emitted.
     *
     * Two answers come out of the same walk. `useful` is whether an alternative matches anything the
     * ones before it did not, which is a warning and not an error - an unreachable arm is dead code
     * rather than a wrong program. `add` is whether the match is complete once this alternative is
     * in, and the first alternative for which it is true is the one emitted with no failure edge.
     */
    PatternSpace space(*this, pivot->occurrence.type);
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

    return armsValue(builder, expr.source, U32(patterns.size()), hint, wantValue,
        [&](U32 index, Block* onFail, DeclPtr<Type> armHint, Carry& carry) -> Node* {
            auto alternative = written.alts.get(parse, index);

            SmallArray<PatternBinding, 4> made;
            if(!resolvePattern(builder, patterns[index], *pivot, made,
                               tested[index] ? onFail : nullptr, carry)) {
                return nullptr;
            }

            // The arm itself, with its own names in scope for exactly its own length.
            auto scope = U32(bindings.size());
            bindPattern(builder, made);

            auto value = wantValue ? resolveValue(builder, alternative.expr, armHint, wanted)
                                   : nullptr;
            if(!wantValue) resolveStatement(builder, alternative.expr);

            bindings.resize(scope);
            return value;
        });
}

} // namespace mir
