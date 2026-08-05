#include "expr.h"
#include "complete.h"
#include "name.h"
#include "index.h"

/*
 * Patterns, `match`, and the refutable half of `let`.
 *
 * Resolving a pattern emits the tests it needs into the current block and binds the names it
 * introduces, returning what it proved: an irrefutable pattern emits nothing, a refutable one
 * emits a branch to `onFail`. `match` chains those failure blocks together, so the alternatives
 * are tried in order.
 *
 * Which alternatives need a test at all is not decided while emitting them - it is decided by
 * PatternSpace below, before any of them is emitted. That separation is what makes an exhaustive
 * `match` need no wildcard: the alternative that completes the match is emitted with no failure
 * block, because every value that failed the ones before it is one this alternative matches, all
 * the way down through its nested patterns.
 */

/*
 * Exhaustiveness.
 *
 * PatternSpace is Maranget's usefulness algorithm ("Warnings for pattern matching", JFP 2007)
 * over the alternatives of one match. It answers three questions with one recursion:
 *
 *   - is this alternative useful, or does an earlier one already cover everything it matches?
 *   - is the match complete once this alternative is added?
 *   - if it is not, which value is left over?
 *
 * The third is what makes the diagnostic worth reading, and it costs nothing extra: a witness is
 * what the search for one produces on its way to answering the second.
 *
 * The algorithm works on a matrix whose columns are the positions a value is taken apart into,
 * which is how it sees through nesting that the pattern syntax keeps separate: after
 * `Just(Red)`, `Just(Green)` and `Just(Blue)`, the `Just` column is complete even though no
 * single alternative wrote a bare `Just`.
 */

// A pattern's head constructor, as the exhaustiveness algorithm sees it.
//
// `Opaque` is the escape hatch: a test the algorithm cannot relate to any other - a range, an
// array pattern, an operator section, or a constructor that does not belong to the type being
// matched. Each occurrence is its own constructor drawn from an inexhaustible signature, so it
// can never complete a column and can never be shown to subsume another pattern. Both of those
// err towards saying less, which is the safe direction: the worst an Opaque can cause is a
// wildcard the author did not strictly need.
struct PatternHead {
    enum Kind: U8 {
        Wildcard,
        Con,
        Tup,
        Int,
        Opaque,
    };

    bool operator == (const PatternHead& other) const {
        if(kind != other.kind) return false;

        switch(kind) {
            case Con: return index == other.index;
            case Int: return value == other.value;
            case Opaque: return tag == other.tag;
            default: return true;
        }
    }

    // Con: the constructor's index in its record. Int: the literal's value. Opaque: the pattern
    // itself, which is the only identity such a test has.
    U32 index = 0;
    U64 value = 0;
    const ast::Pat* tag = nullptr;
    Kind kind = Wildcard;
};

/*
 * The lists the coverage recursion works in, all inline.
 *
 * Every one of these is built, read once and dropped inside a recursion that runs per arm per
 * column - so what decides their cost is how many are made rather than how big any of them gets,
 * and the counts they hold are the arity of a constructor and the number of arms of a `match`. The
 * bounds are set where an ordinary program stops rather than where the language does: a row past
 * eight columns or a type past eight constructors reaches the heap exactly as it did before.
 */
using PatternRow = SmallArray<const ast::Pat*, 8>;
using HeadList = SmallArray<PatternHead, 8>;

// One step of the recursion. Rows are stored flat, `types.size()` cells to a row, because every
// step builds a whole new matrix from the previous one and a row is never edited in place. A
// null cell is a wildcard.
struct PatternMatrix {
    TypeList types;
    SmallArray<const ast::Pat*, 16> cells;
    Size rows = 0;

    Size columns() const { return types.size(); }
    const ast::Pat* cell(Size row, Size column) const { return cells[row * columns() + column]; }
};

struct PatternSpace {
    PatternSpace(ExprResolver& resolver, TypePtr pivot):
        context(resolver.context), module(resolver.module),
        parse(resolver.parse), global(resolver.global)
    {
        covered.types.push(pivot);
    }

    // Whether `pattern` matches a value that none of the patterns added so far match. A pattern
    // that does not is an alternative the program can never take.
    //
    // Every pattern handed to a PatternSpace has to outlive it and keep its address, because a
    // pattern's identity here is its address: that is what tells two opaque tests apart, and what
    // the head cache is keyed by. An alternative read out of a parse list is a copy, so the
    // callers below keep the ones they hand over.
    bool useful(const ast::Pat& pattern) {
        PatternRow row;
        row.push(&pattern);
        return useful(covered, row);
    }

    // Adds `pattern` to the space, and answers whether everything is now covered.
    bool add(const ast::Pat& pattern) {
        covered.cells.push(&pattern);
        covered.rows++;

        Array<String> witness;
        return !missing(covered, witness);
    }

    // A value nothing added so far matches, written the way a pattern for it would be. Only
    // meaningful when add() last answered false.
    String gap() {
        Array<String> witness;
        if(!missing(covered, witness) || witness.isEmpty()) return String("_");
        return witness[0];
    }

private:
    // The head of one cell. Cached by pattern rather than recomputed, both because the recursion
    // asks for the same one repeatedly and because looking a constructor up can report ambiguity
    // - which has to happen once, not once per column the pattern is looked at from.
    PatternHead head(const ast::Pat* pattern, TypePtr type) {
        if(!pattern) return {};

        for(auto& entry: headCache) {
            if(entry.pattern == pattern) return entry.head;
        }

        auto result = computeHead(*pattern, type);
        headCache.push(CachedHead { pattern, result });
        return result;
    }

    PatternHead computeHead(const ast::Pat& pattern, TypePtr type) {
        auto opaque = [&]() { return PatternHead { 0, 0, &pattern, PatternHead::Opaque }; };

        switch(pattern.kind) {
            // A bare name is a wildcard as far as coverage goes - it matches every value, it just
            // remembers which one it saw. Testing a value against something is what an operator
            // section is for, and that one is opaque.
            case ast::Pat::Error:
            case ast::Pat::Any:
            case ast::Pat::Var:
                return {};
            case ast::Pat::Tup: {
                if(!type || global[type]->kind != Type::Tup) return opaque();

                // A field the pattern names but the type does not have is an error resolvePattern
                // reports; here it only means the pattern's shape cannot be trusted.
                auto tuple = (TupType*)global[type];
                auto fieldList = pattern.tup;
                Size positional = 0;

                for(auto field: fieldList.contents(parse)) {
                    if(fieldIndex(*tuple, field.field, positional) == maxLimit<Size>) return opaque();
                }

                return PatternHead { 0, 0, nullptr, PatternHead::Tup };
            }
            case ast::Pat::Con: {
                auto found = findConstructor(module, pattern.con.name, pattern.source);
                if(!found || !type || global[type]->kind != Type::Record) return opaque();

                auto record = (RecordType*)global[type];
                if(record->base(global) != found.unwrap().record) return opaque();

                return PatternHead { found.unwrap().index, 0, nullptr, PatternHead::Con };
            }
            default:
                break;
        }

        if(pattern.kind == ast::Pat::Kind(ast::Pat::Lit + ast::Literal::Int)) {
            // Two's complement here, where the number is an identity key rather than a value: two
            // patterns are the same head when they are the same number, and which number it is is
            // the magnitude *and* the sign. In unsigned arithmetic, so `I64`'s own minimum is a key
            // like any other.
            auto magnitude = pattern.lit.i();
            return PatternHead {
                0, pattern.negative ? U64(0) - magnitude : magnitude, nullptr, PatternHead::Int
            };
        }

        return opaque();
    }

    // Which field of `tuple` a pattern field addresses, advancing the positional counter when it
    // is not named. maxLimit<Size> when there is no such field.
    Size fieldIndex(TupType& tuple, StringId name, Size& positional) {
        if(name) {
            for(Size i = 0; i < tuple.fields.size(); i++) {
                if(tuple.fields.get(global, i).name == name) return i;
            }

            return maxLimit<Size>;
        }

        return positional < tuple.fields.size() ? positional++ : maxLimit<Size>;
    }

    /*
     * The signature of one column.
     */

    Size arity(const PatternHead& head, TypePtr type) {
        switch(head.kind) {
            case PatternHead::Con: {
                auto record = (RecordType*)global[type];
                return isUnit(global, record->constructors.get(global, head.index).content) ? 0 : 1;
            }
            case PatternHead::Tup:
                return ((TupType*)global[type])->fields.size();
            default:
                return 0;
        }
    }

    TypePtr fieldType(const PatternHead& head, TypePtr type, Size index) {
        if(head.kind == PatternHead::Con) return ((RecordType*)global[type])->constructors.get(global, head.index).content;
        return ((TupType*)global[type])->fields.get(global, index).type;
    }

    // The sub-patterns `pattern` supplies to a head it matches. A null pattern is the wildcard
    // case, which supplies a wildcard for each of the head's own positions.
    //
    // Templated on the list only because the two callers hold different ones - a single row, and
    // the flat cells of a whole matrix - and neither wants the other's inline bound.
    template<class Target>
    void expand(const PatternHead& head, TypePtr type, const ast::Pat* pattern, Target& target) {
        auto count = arity(head, type);

        if(!pattern) {
            for(Size i = 0; i < count; i++) target.push(nullptr);
            return;
        }

        if(head.kind == PatternHead::Con) {
            if(count) target.push(pattern->con.pats ? parse[pattern->con.pats] : nullptr);
            return;
        }

        if(head.kind == PatternHead::Tup) {
            auto start = target.size();
            for(Size i = 0; i < count; i++) target.push(nullptr);

            auto tuple = (TupType*)global[type];
            auto fieldList = pattern->tup;
            Size positional = 0;

            for(auto field: fieldList.contents(parse)) {
                auto index = fieldIndex(*tuple, field.field, positional);
                if(index != maxLimit<Size>) target[start + index] = parse[field.pat];
            }
        }
    }

    // Every distinct head appearing in the matrix's first column.
    void collectHeads(const PatternMatrix& matrix, HeadList& target) {
        for(Size row = 0; row < matrix.rows; row++) {
            auto candidate = head(matrix.cell(row, 0), matrix.types[0]);
            if(candidate.kind == PatternHead::Wildcard) continue;
            if(!target.containsValue(candidate)) target.push(candidate);
        }
    }

    // Whether `heads` names every value the column's type can hold. Only a record and a tuple
    // have a signature that can be finished; an integer, a range or an opaque test leaves values
    // no pattern in the column named, so a wildcard is the only thing that covers them.
    bool complete(const HeadList& heads, TypePtr type) {
        if(!type) return false;

        if(global[type]->kind == Type::Record) {
            auto record = (RecordType*)global[type];

            for(Size i = 0; i < record->constructors.size(); i++) {
                auto present = heads.contains([&](const PatternHead& candidate) {
                    return candidate.kind == PatternHead::Con && candidate.index == i;
                });

                if(!present) return false;
            }

            return true;
        }

        if(global[type]->kind == Type::Tup) {
            return heads.contains([&](const PatternHead& candidate) { return candidate.kind == PatternHead::Tup; });
        }

        return false;
    }

    // The heads a complete column is taken apart by: the type's own constructors, rather than
    // whatever the column happens to hold, so that an opaque test sitting beside a complete set
    // of constructors does not add a case of its own.
    void signature(TypePtr type, HeadList& target) {
        if(global[type]->kind == Type::Record) {
            auto record = (RecordType*)global[type];
            for(Size i = 0; i < record->constructors.size(); i++) {
                target.push(PatternHead { U32(i), 0, nullptr, PatternHead::Con });
            }

            return;
        }

        target.push(PatternHead { 0, 0, nullptr, PatternHead::Tup });
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

            if(rowHead.kind != PatternHead::Wildcard && !(rowHead == by)) continue;

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

        // A wildcard against a column that already names every case is only useful through one
        // of those cases, so the question splits into one per constructor.
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
    // string per column. False when the matrix is exhaustive, which is the answer add() reports.
    bool missing(const PatternMatrix& matrix, Array<String>& witness) {
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

                Array<String> sub;
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

        Array<String> sub;
        if(!missing(next, sub)) return false;

        witness.push(absent(heads, type));
        for(auto& entry: sub) witness.push(entry);
        return true;
    }

    /*
     * Witnesses.
     */

    String describe(const PatternHead& head, TypePtr type, const Array<String>& fields, Size count) {
        StringBuilder text;

        if(head.kind == PatternHead::Con) {
            auto record = (RecordType*)global[type];
            text << context.findName(record->constructors.get(global, head.index).name);

            if(count) text << '(' << fields[0] << ')';
            return text.string();
        }

        text << '{';
        for(Size i = 0; i < count; i++) {
            if(i) text << ", ";
            text << fields[i];
        }

        text << '}';
        return text.string();
    }

    // A value of `type` that none of `heads` names. Reached only where the column is incomplete,
    // so a record always has a constructor left to point at; anything else can only be described
    // as "some other value".
    String absent(const HeadList& heads, TypePtr type) {
        if(type && global[type]->kind == Type::Record) {
            auto record = (RecordType*)global[type];

            for(Size i = 0; i < record->constructors.size(); i++) {
                auto present = heads.contains([&](const PatternHead& candidate) {
                    return candidate.kind == PatternHead::Con && candidate.index == i;
                });

                if(present) continue;

                StringBuilder text;
                text << context.findName(record->constructors.get(global, i).name);
                if(!isUnit(global, record->constructors.get(global, i).content)) text << "(_)";
                return text.string();
            }
        }

        return String("_");
    }

    struct CachedHead {
        const ast::Pat* pattern;
        PatternHead head;
    };

    Context& context;
    Module& module;
    ast::ParseBase parse;
    GlobalBase global;

    PatternMatrix covered;
    SmallArray<CachedHead, 16> headCache;
};

/*
 * Emitting patterns.
 */

PatternResult ExprResolver::branchPattern(ModulePtr<Value> condition, ModulePtr<Block> onFail, LocationId source) {
    if(!condition) return PatternResult::Never;

    condition = convert(condition, module.scalar.bool_, source);
    auto success = addBlock();
    terminate(emit<InstJe>(source, 0, module.scalar.unit, condition, success, onFail));
    current = success;

    return PatternResult::Maybe;
}

// The value a pattern compares against - a range's bound, a literal, or an operator section's
// right operand. It may be a literal, an existing binding, or `_` where the caller allows an
// open end; `_` yields no value at all rather than an error, so a caller that needs one has to
// say so itself.
ModulePtr<Value> ExprResolver::patternBound(const ast::Pat& pattern, TypePtr target) {
    if(pattern.kind == ast::Pat::Any) return nullptr;

    if(pattern.kind == ast::Pat::Var) {
        auto value = find(pattern.var);
        if(!value) {
            context.diagnostics.error("unknown value in pattern %@"_v, pattern.source, context.findName(pattern.var));
        }

        return value;
    }

    if(pattern.kind >= ast::Pat::Lit) {
        auto kind = ast::Literal::Kind(pattern.kind - ast::Pat::Lit);

        ast::Expr literal {
            .lit = pattern.lit,
            .source = pattern.source,
            .kind = ast::Expr::Kind(ast::Expr::Lit + kind),
        };

        if(!pattern.negative) return resolve(literal, target);

        /*
         * A negative literal, whose sign the parser recorded rather than folded in.
         *
         * This is where it is applied, because this is the first point at which the number's type is
         * known - it is the pivot's - and every question about the number needs it. The two answers
         * come from the same two functions the expression path and resolve/const.cpp use, which is
         * what makes the three positions agree: `checkLiteralRange` decides whether the type holds
         * the number, from the magnitude and the sign rather than from bits that have already lost
         * the difference, and `makeInt` puts it in the type's normal form.
         */
        if(kind == ast::Literal::Int && isInteger(global, target)) {
            auto magnitude = pattern.lit.i();
            checkLiteralRange(pattern.source, target, magnitude, true);
            return makeInt(pattern.source, target, U64(0) - magnitude);
        }

        if(isFloat(global, target)) {
            auto number = kind == ast::Literal::Int   ? F64(pattern.lit.i())
                        : kind == ast::Literal::Float ? F64(pattern.lit.f)
                                                      : pattern.lit.d();

            // On the number, which is what the sign was written on - so it is exact at every
            // magnitude, and `-0.0` is a pattern that can be written.
            return makeFloat(pattern.source, target, -number);
        }

        /*
         * A pivot whose type is not one a sign means anything at - a literal that has not settled,
         * or a type this literal is about to be reported as impossible at. The sign is folded in so
         * that the position says what it always said, in unsigned arithmetic so that it says it for
         * every magnitude.
         */
        if(kind == ast::Literal::Int) {
            literal.lit.i(U64(0) - pattern.lit.i());
        } else if(kind == ast::Literal::Double) {
            literal.lit.d(-pattern.lit.d());
        } else if(kind == ast::Literal::Float) {
            literal.lit.f = -pattern.lit.f;
        }

        return resolve(literal, target);
    }

    context.diagnostics.error("a pattern can only compare against a literal, an existing value, or _"_v, pattern.source);
    return nullptr;
}

/*
 * `Just(->v)` - the payload taken out of the pivot rather than referred to where it lies.
 *
 * A pattern has always bound with the default convention, and the default convention is a borrow:
 * the name refers to the pivot's own storage and the pivot goes on owning it. That is right for
 * reading and wrong for the one thing a container of owned values exists to allow, which is getting
 * one back out - so `->` is written on the name, where a parameter and a `let` already write it, and
 * means what it means there.
 *
 * What it compiles to is an ordinary `InstMove` over the payload's place, which is what puts it in
 * front of every rule the ownership model already has: the pivot reads as moved from afterwards, a
 * second use of it is the use-after-move it is, and a pivot that is only borrowed is refused.
 *
 * **Only on a name or on `_`.** `Just(->Pair {a, 0})` would take the payload out and *then* ask
 * whether the second field is zero, and on the path where it is not the value has already left a
 * place nothing will put it back into. The restriction is what keeps a move off every path that can
 * still fail, and it costs nothing a program wants: bind the payload and take it apart afterwards.
 */
ModulePtr<Value> ExprResolver::bindPatternConvention(const ast::Pat& pattern, ModulePtr<Value> pivot) {
    if(pattern.bind == ast::BindType::Ref) {
        context.diagnostics.error("a pattern cannot bind with `&` - matching does not establish the exclusive access that would need. Match on the value and write through the name that owns it"_v,
                                  pattern.source);
        return pivot;
    }

    if(pattern.kind != ast::Pat::Var && pattern.kind != ast::Pat::Any) {
        context.diagnostics.error("`->` in a pattern has to be written on a name or on `_` - taking a value out and then testing what came out would leave it moved on the path where the test failed"_v,
                                  pattern.source);
        return pivot;
    }

    // Through rootSink like a `let ->`, and for its reason: what a move produces is a value, and a
    // value has no address - so a name bound to one could not have a field read out of it, and the
    // drop pass would have no slot to owe the drop to. `Held(->_)` is the case that makes the second
    // half visible, since there the taken value is never named at all and dropping it *there* is the
    // whole of what was asked for.
    return rootSink(sinkValue(pivot, pattern.source), pattern.source);
}

PatternResult ExprResolver::resolvePattern(const ast::Pat& pattern, ModulePtr<Value> pivot, ModulePtr<Block> onFail,
                                           Size bindingBase) {
    // A null failure block says the caller has already proved this pattern matches. Everything
    // below then takes the value apart and binds it without asking any question about it.
    auto tested = onFail != nullptr;

    // Before anything is bound, so that `v @ ->w` and the name itself refer to the same value, and
    // before any test, so that a constructor's discriminant is still checked on the pivot.
    if(pattern.bind != ast::BindType::Borrow) pivot = bindPatternConvention(pattern, pivot);

    // A name binds, always, shadowing whatever it shadows - that is what makes a pattern mean the
    // same thing wherever it is written. Binding the same name twice within one pattern is the
    // one case that cannot be meant, since only one of the two could ever be read.
    auto bind = [&](StringId name) {
        for(auto i = bindingBase; i < bindings.size(); i++) {
            if(bindings[i].name != name) continue;

            context.diagnostics.error("this pattern binds %@ twice"_v, pattern.source, context.findName(name));
            return;
        }

        Binding binding { name, pivot };
        binding.definition = pattern.source;
        bindings.push(binding);
        recordBindingDefinition(*this, binding);
    };

    if(pattern.asVar) bind(pattern.asVar);

    switch(pattern.kind) {
        case ast::Pat::Error:
            return PatternResult::Never;
        case ast::Pat::Any:
            return PatternResult::Always;
        case ast::Pat::Var: {
            if(local[pivot]->name == 0) local[pivot]->name = pattern.var;
            bind(pattern.var);
            return PatternResult::Always;
        }
        case ast::Pat::Section: {
            if(!tested) return PatternResult::Always;

            // The matched value is the operator's left operand, so `>0` reads as the comparison
            // it is. Which operator it is decides nothing here beyond the call: any function of
            // two arguments that answers Bool can serve as a test.
            auto& bound = *parse[pattern.section.bound];
            if(bound.kind == ast::Pat::Any) {
                context.diagnostics.error("an operator pattern needs a value to compare against"_v, pattern.source);
                return PatternResult::Never;
            }

            auto value = patternBound(bound, valueType(pivot));
            if(!value) return PatternResult::Never;

            ResolvedArg args[] = { pivot, value };
            auto condition = emitCall(pattern.section.op, { args, 2 }, pattern.source, module.scalar.bool_);
            if(!condition) return PatternResult::Never;

            auto type = valueType(condition);
            if(type != module.scalar.bool_ && global[type]->kind != Type::Error) {
                context.diagnostics.error("an operator used as a pattern has to produce Bool, but %@ produces %@"_v,
                                          pattern.source, context.findName(pattern.section.op),
                                          describeType(context, global, type));
                return PatternResult::Never;
            }

            return branchPattern(condition, onFail, pattern.source);
        }
        case ast::Pat::Tup: {
            auto type = valueType(pivot);
            if(global[type]->kind != Type::Tup) {
                context.diagnostics.error("tuple pattern used on a non-tuple value"_v, pattern.source);
                return PatternResult::Never;
            }

            auto tuple = (TupType*)global[type];
            auto root = placeFor(pivot, pattern.source);
            auto fieldList = pattern.tup;
            auto fields = fieldList.contents(parse);
            Size positional = 0;
            auto overall = PatternResult::Always;

            for(auto fieldPattern: fields) {
                Size index = maxLimit<Size>;

                if(fieldPattern.field) {
                    for(Size i = 0; i < tuple->fields.size(); i++) {
                        if(tuple->fields.get(global, i).name == fieldPattern.field) {
                            index = i;
                            break;
                        }
                    }
                } else if(positional < tuple->fields.size()) {
                    index = positional++;
                }

                if(index == maxLimit<Size>) {
                    context.diagnostics.error("tuple pattern refers to a missing field"_v, parse[fieldPattern.pat]->source);
                    return PatternResult::Never;
                }

                auto child = load(project(root, ProjectionKind::Field, U16(index)), parse[fieldPattern.pat]->source);
                auto result = resolvePattern(*parse[fieldPattern.pat], child, onFail, bindingBase);

                if(result == PatternResult::Never) return result;
                if(result == PatternResult::Maybe) overall = result;
            }

            return overall;
        }
        case ast::Pat::Con: {
            auto pivotType = valueType(pivot);

            /*
             * The cursor in a constructor pattern - Implementation-Tooling.md §8.1's fifth kind.
             *
             * The pivot's type is what the position asked for, and it is what makes the answer
             * ranked rather than alphabetical: the constructors of the value being matched come
             * first, and the ones of every other visible record follow. Ahead of the lookup, since
             * the sentinel names nothing and the only outcome of looking it up is the report below.
             */
            if(isCursorSentinel(context, pattern.con.name)) {
                capturePatternCompletion(*this, pivotType);
                return PatternResult::Never;
            }

            auto found = findConstructor(module, pattern.con.name, pattern.source);

            if(!found || global[pivotType]->kind != Type::Record) {
                context.diagnostics.error("constructor pattern is incompatible with the pivot"_v, pattern.source);
                return PatternResult::Never;
            }

            auto reference = found.unwrap();
            auto record = (RecordType*)global[pivotType];

            // A constructor belongs to a declaration, so `Just` matches any `Maybe(a)`; the
            // content it exposes is the pivot's own, with that pivot's type arguments in it.
            if(record->base(global) != reference.record) {
                context.diagnostics.error("constructor %@ does not belong to %@"_v, pattern.source,
                                          context.findName(pattern.con.name),
                                          describeType(context, global, pivotType));
                return PatternResult::Never;
            }

            auto constructor = record->constructors.get(global, reference.index);
            auto testedConstructor = false;

            if(record->constructors.size() > 1 && tested) {
                ModulePtr<Value> discriminant = pivot;

                if(record->layout == RecordType::Enum) {
                    discriminant = ref(emit<InstUnary>(pattern.source, 0, module.scalar.int_, Value::Cast, pivot));
                } else {
                    discriminant = load(project(placeFor(pivot, pattern.source), ProjectionKind::Discriminant, 0), pattern.source);
                }

                ResolvedArg args[] = {
                    discriminant,
                    makeInt(pattern.source, module.scalar.int_, reference.index),
                };

                branchPattern(emitCall(Context::nameHash("==", 2), { args, 2 }, pattern.source, module.scalar.bool_), onFail, pattern.source);
                testedConstructor = true;
            }

            auto childResult = PatternResult::Always;

            if(pattern.con.pats && !isUnit(global, constructor.content)) {
                if(record->layout == RecordType::Enum) {
                    context.diagnostics.error("nullary constructor pattern cannot contain a child pattern"_v, pattern.source);
                    return PatternResult::Never;
                }

                auto content = load(project(placeFor(pivot, pattern.source), ProjectionKind::Downcast, reference.index), pattern.source);
                childResult = resolvePattern(*parse[pattern.con.pats], content, onFail, bindingBase);
            }

            if(childResult == PatternResult::Never) return childResult;
            return testedConstructor ? PatternResult::Maybe : childResult;
        }
        case ast::Pat::Range: {
            if(!tested) return PatternResult::Always;

            auto from = patternBound(*parse[pattern.range.from], valueType(pivot));
            auto to = patternBound(*parse[pattern.range.to], valueType(pivot));
            if(!from && !to) return PatternResult::Always;

            ModulePtr<Value> condition = nullptr;

            if(from) {
                ResolvedArg args[] = { pivot, from };
                condition = emitCall(Context::nameHash(">=", 2), { args, 2 }, pattern.source, module.scalar.bool_);
            }

            if(to) {
                // `a..b` is half-open and `a..=b` is closed, the same two spellings a `for` header
                // uses. A one-sided `a.._` has no upper test at all, so its spelling says nothing
                // and either is accepted.
                auto upperOp = pattern.range.inclusive ? Context::nameHash("<=", 2)
                                                       : Context::nameHash("<", 1);

                ResolvedArg args[] = { pivot, to };
                auto upper = emitCall(upperOp, { args, 2 }, pattern.source, module.scalar.bool_);

                if(condition) {
                    ResolvedArg both[] = { condition, upper };
                    condition = emitCall(Context::nameHash("and", 3), { both, 2 }, pattern.source, module.scalar.bool_);
                } else {
                    condition = upper;
                }
            }

            return branchPattern(condition, onFail, pattern.source);
        }
        case ast::Pat::Arr:
        case ast::Pat::Rest:
            context.diagnostics.error("array patterns are deferred until arrays are represented in resolve IR"_v, pattern.source);
            return PatternResult::Never;
        default:
            break;
    }

    if(pattern.kind >= ast::Pat::Lit) {
        if(!tested) return PatternResult::Always;

        ResolvedArg args[] = { pivot, patternBound(pattern, valueType(pivot)) };
        return branchPattern(emitCall(Context::nameHash("==", 2), { args, 2 }, pattern.source, module.scalar.bool_), onFail, pattern.source);
    }

    return PatternResult::Never;
}

/*
 * Conditions.
 *
 * A condition is either an expression whose type has a `Truth` instance or an `is` test - the
 * anonymous and the named form of one idea. Both come out here as a branch rather than as a value,
 * which is the whole point of the `is` half: the names its pattern binds have to be live in the
 * block where the condition held, and nowhere else. A value-producing `is` (resolveIs below) is
 * the degenerate case where there is no such block to bind into.
 */
PatternResult ExprResolver::resolveCondition(const ast::Expr& expr, ModulePtr<Block>& onFail) {
    if(expr.kind == ast::Expr::Is) {
        auto& test = *parse[expr.is];

        // As in resolveMatch: the pivot has to have a type before a pattern can be matched against
        // it, so a bare literal settles first.
        auto pivot = settle(resolve(test.value), test.value.source);
        if(!pivot) return PatternResult::Never;

        PatternSpace space(*this, valueType(pivot));
        if(space.add(test.pat)) {
            context.diagnostics.warning("this pattern always matches, so the condition is always true"_v,
                                        test.pat.source);
        }

        if(!onFail) onFail = addBlock();
        auto result = resolvePattern(test.pat, pivot, onFail);
        if(result != PatternResult::Always) return result;

        // An irrefutable pattern emits no test, which would leave the failure block with nothing
        // entering it - and every caller has already been handed that block to resolve its other
        // arm into. A branch nothing takes keeps the graph well-formed and costs one dead compare;
        // removing a block from the middle of a half-built function would cost rather more.
        return branchPattern(makeInt(expr.source, module.scalar.bool_, 1), onFail, expr.source);
    }

    // Not `is`: an ordinary value, asked whether it is true. It is resolved with no expected type,
    // because pushing `Bool` down would be the conversion step `Truth` is defined not to take -
    // and a literal condition settles to its own class's default before being asked.
    auto value = truthy(settle(resolve(expr), expr.source), expr.source);
    if(!value) return PatternResult::Never;

    // The success block is created first so that a `then` arm keeps the block order it had before
    // conditions went through here.
    auto success = addBlock();
    if(!onFail) onFail = addBlock();

    terminate(emit<InstJe>(expr.source, 0, module.scalar.unit, value, success, onFail));
    current = success;

    return PatternResult::Maybe;
}

// `x is p` where nothing branches on it. The bindings cannot survive: the value is produced by a
// join, and the path through it is exactly the one on which the pattern did not match.
ModulePtr<Value> ExprResolver::resolveIs(const ast::Expr& expr, const ast::IsExpr&, bool used) {
    auto bindingCount = bindings.size();
    ModulePtr<Block> onFail = nullptr;

    if(resolveCondition(expr, onFail) == PatternResult::Never) return nullptr;
    bindings.resize(bindingCount);

    BranchArmList arms;
    arms.push(BranchArm { current, makeInt(expr.source, module.scalar.bool_, 1), expr.source });

    current = onFail;
    arms.push(BranchArm { current, makeInt(expr.source, module.scalar.bool_, 0), expr.source });

    return finishBranches(arms, expr.source, used);
}

/*
 * A tuple pattern matched against the elements themselves.
 *
 * The same walk the Tup case of resolvePattern makes, with the projection removed: there is no
 * tuple to project out of, because the pivot was never built. Positional only - a named field
 * refers to a tuple type this pivot does not have, and decomposeMatch declines those before
 * anything reaches here.
 */
PatternResult ExprResolver::resolveDecomposed(const ast::Pat& pattern, Buffer<ModulePtr<Value>> elements,
                                              ModulePtr<Block> onFail, Size bindingBase) {
    if(pattern.kind == ast::Pat::Any) return PatternResult::Always;

    auto overall = PatternResult::Always;
    Size position = 0;

    auto fieldList = pattern.tup;
    for(auto fieldPattern: fieldList.contents(parse)) {
        auto result = resolvePattern(*parse[fieldPattern.pat], elements[position], onFail, bindingBase);
        position++;

        if(result == PatternResult::Never) return result;
        if(result == PatternResult::Maybe) overall = result;
    }

    return overall;
}

/*
 * Whether a match over a tuple *literal* can skip building it.
 *
 * `match {lhs, rhs}` pairs two scrutinees and nothing more - the tuple exists to be taken apart by
 * the very next thing that reads it. Building it anyway is what made `Eq(Maybe(a))`'s `==` consume
 * both operands it was only lent: a tuple is contiguous storage, so its fields are *copied* in, and
 * a copy of a value with a teardown is a second owner of it. The elements are already values here,
 * and matching against them directly is the same tests with no storage in between.
 *
 * Every alternative has to be one this can serve, which is a positional tuple pattern of the right
 * arity or `_`. A pattern that binds the whole pivot by name genuinely wants the tuple to exist, and
 * a named field refers to field names an unnamed literal does not have; either sends the whole match
 * back to building it, since the alternatives all read one pivot and it cannot be two shapes at once.
 */
static bool decomposeMatch(ast::ParseBase parse, const ast::Expr& pivot,
                           ast::ParseList<ast::Alt> alternatives) {
    if(pivot.kind != ast::Expr::Tup) return false;

    auto pivotFields = pivot.tup;
    if(pivotFields.isEmpty()) return false;

    Size arity = 0;
    for(auto arg: pivotFields.contents(parse)) {
        if(arg.name) return false;
        arity++;
    }

    for(auto alternative: alternatives.contents(parse)) {
        auto pattern = alternative.pat;
        if(pattern.kind == ast::Pat::Any) continue;
        if(pattern.kind != ast::Pat::Tup) return false;
        if(pattern.bind != ast::BindType::Borrow) return false;

        Size fields = 0;
        auto patternFields = pattern.tup;
        for(auto fieldPattern: patternFields.contents(parse)) {
            if(fieldPattern.field) return false;
            fields++;
        }

        if(fields != arity) return false;
    }

    return true;
}

ModulePtr<Value> ExprResolver::resolveMatch(const ast::Expr& expr, const ast::MatchExpr& match, TypePtr target, bool used, bool implicit) {
    auto alternativeList = match.alts;
    auto alternatives = alternativeList.contents(parse);

    /*
     * A match with no alternatives cannot be written - the parser needs the ':' and an indented
     * block to have read a match at all - so this is only ever the shape a half-typed one is left
     * in, and the parser has already said so. It resolves to nothing, quietly.
     *
     * The pivot is still resolved before giving up, and that is the whole point of the case:
     * `match s:` with the alternatives not typed yet is exactly when an editor asks what `s` is,
     * and a reference is only recorded while the expression naming it is resolved. Returning
     * without visiting the pivot left the half-written line with nothing to hover, nothing to go
     * to, and no semantic token - see lsp/recover.expect, which asserts all three.
     *
     * As a plain pivot rather than through decomposeMatch below, because a decomposition is read
     * out of the alternatives and there are none.
     */
    if(alternatives.size() == 0) {
        settle(resolve(match.pivot), match.pivot.source);
        return nullptr;
    }

    auto decomposed = decomposeMatch(parse, match.pivot, alternativeList);

    /*
     * The pivot, as a value or as the elements one would have held.
     *
     * The decomposed form still needs the tuple *type*, because exhaustiveness is stated over it -
     * a tuple pattern covers a product of its fields' spaces, and PatternSpace has no other way to
     * be told what the product is. Interning a type is free of any storage, which is the whole
     * point: what is skipped is the allocation and the field copies, not the type.
     */
    ModulePtr<Value> pivot = nullptr;
    TypePtr pivotType = nullptr;
    ValueList elements;

    if(decomposed) {
        SmallArray<Field, 8> fields;

        auto pivotFields = match.pivot.tup;
        for(auto arg: pivotFields.contents(parse)) {
            auto value = settle(resolve(arg.value), arg.value.source);
            if(!value) return nullptr;

            elements.push(value);
            fields.push(Field { valueType(value), arg.name });
        }

        pivotType = (Type*)resolveTupleType(module, toBuffer(fields), match.pivot.source) - global;
    } else {
        // Every pattern is matched against the pivot's type, so a pivot that is a bare literal has
        // to have settled on one before the first alternative is read.
        pivot = settle(resolve(match.pivot), match.pivot.source);
        if(!pivot) return nullptr;

        pivotType = valueType(pivot);
    }

    // Reading an alternative out of the parse list copies it, so the patterns are collected into
    // one array that outlives the space they are handed to - see PatternSpace::useful.
    Array<ast::Pat> patterns;
    patterns.reserve(U32(alternatives.size()));
    for(auto alternative: alternatives) patterns.push(alternative.pat);

    PatternSpace space(*this, pivotType);
    auto bindingCount = bindings.size();
    BranchArmList arms;
    auto exhaustive = false;
    auto rejected = false;

    for(Size i = 0; i < alternatives.size(); i++) {
        auto& pattern = patterns[i];
        auto body = alternatives[i].expr;

        if(exhaustive) {
            context.diagnostics.warning("this alternative is unreachable - the ones before it already cover every value"_v,
                                        pattern.source);
            continue;
        }

        if(!space.useful(pattern)) {
            context.diagnostics.warning("this alternative is unreachable - an earlier one already covers it"_v,
                                        pattern.source);
        }

        // The alternative that completes the match needs no test at all: everything that reached
        // it failed every alternative before it, and those together with this one are every value
        // the pivot can hold, so this one matches whatever is left.
        exhaustive = space.add(pattern);
        auto failure = exhaustive ? ModulePtr<Block>(nullptr) : addBlock();
        auto patternResult = decomposed
            ? resolveDecomposed(pattern, toBuffer(elements), failure, bindings.size())
            : resolvePattern(pattern, pivot, failure);

        if(patternResult == PatternResult::Never) {
            rejected = true;
            bindings.resize(bindingCount);
            if(!failure) return nullptr;

            current = failure;
            continue;
        }

        auto value = resolve(body, target, used, implicit);
        if(current) arms.push(BranchArm { current, value, body.source });
        bindings.resize(bindingCount);

        current = failure;
    }

    if(!exhaustive) {
        // A pattern that could not be resolved at all has already been reported, and the space it
        // would have covered is unknown, so there is nothing useful to say about what is missing.
        // Neither is there when the pivot itself did not come out with a type.
        if(!rejected && global[valueType(pivot)]->kind != Type::Error) {
            context.diagnostics.error("match is not exhaustive - it needs a case for %@"_v, expr.source, space.gap());
        }

        return nullptr;
    }

    return finishBranches(arms, expr.source, used);
}

/*
 * A pattern with nowhere to fail into.
 *
 * A `for` loop's pattern names the shape of what the iterator hands over rather than testing it:
 * the grammar has no place to write alternatives on a loop, and a pattern that could fail would
 * have to mean "skip this element", which is a filter written where nothing says so. A skipping
 * lens's `let` is the same situation reached differently - the `| else ->` beside it belongs to the
 * skip - so both come through here and `reason` is what tells them apart in the diagnostic.
 *
 * Reported and then bound anyway, so that the code below still reads against the names it was
 * written with.
 */
void ExprResolver::bindIrrefutable(const ast::Pat& pattern, ModulePtr<Value> value, StringView reason) {
    PatternSpace space(*this, valueType(value));

    if(!space.add(pattern) && global[valueType(value)]->kind != Type::Error) {
        context.diagnostics.error("this pattern can fail - it does not match %@ - and %@"_v,
                                  pattern.source, space.gap(), reason);
    }

    resolvePattern(pattern, value, nullptr);
}

/*
 * The refutable declaration.
 *
 * `let Just(v) = lookup(key) | return Nothing` is the guard form: the pattern is allowed to fail,
 * the alternatives say what happens when it does, and every one of them has to leave the block
 * for good. That last rule is what makes the names the pattern binds usable for the rest of the
 * block without a nesting level - there is only one way to reach the code after the declaration,
 * and on that path the pattern matched.
 */
void ExprResolver::resolveBinding(const ast::VarDecl& declaration, ModulePtr<Value> value) {
    PatternSpace space(*this, valueType(value));
    auto alternativeList = declaration.alts;
    auto alternatives = alternativeList.contents(parse);

    // As in resolveMatch: the space keeps the patterns by address, so they are copied out of the
    // parse list once rather than read per use.
    Array<ast::Pat> patterns;
    patterns.reserve(U32(alternatives.size()));
    for(auto alternative: alternatives) patterns.push(alternative.pat);

    if(space.add(declaration.pat)) {
        if(alternatives.size()) {
            context.diagnostics.warning("this pattern always matches, so its alternatives are unreachable"_v,
                                        declaration.pat.source);
        }

        resolvePattern(declaration.pat, value, nullptr);
        return;
    }

    if(alternatives.size() == 0) {
        // An initializer that had no type of its own already said so; a pattern cannot be judged
        // against it, and repeating that here would say nothing new.
        if(global[valueType(value)]->kind != Type::Error) {
            context.diagnostics.error("this pattern can fail - it does not match %@ - so the declaration needs alternatives, written `| return ...` or `| match:` with one per case"_v,
                                      declaration.pat.source, space.gap());
        }

        // Bound anyway, and without its tests: the names are what the rest of the block was
        // written against, and one diagnostic about the declaration is enough.
        resolvePattern(declaration.pat, value, nullptr);
        return;
    }

    auto checkpoint = bindings.size();
    auto failure = addBlock();

    if(resolvePattern(declaration.pat, value, failure) == PatternResult::Never) return;
    auto success = current;

    // What the pattern bound is live on the matching path only, so the alternatives are resolved
    // with the scope the declaration started from and it is put back afterwards.
    Array<Binding> declared;
    while(bindings.size() > checkpoint) declared.push(bindings.pop().unwrap());

    current = failure;
    auto covered = false;

    for(Size i = 0; i < alternatives.size(); i++) {
        auto& pattern = patterns[i];
        auto body = alternatives[i].expr;

        if(covered) {
            context.diagnostics.warning("this alternative is unreachable - the ones before it already cover every value"_v,
                                        pattern.source);
            continue;
        }

        if(!space.useful(pattern)) {
            context.diagnostics.warning("this alternative is unreachable - an earlier one already covers it"_v,
                                        pattern.source);
        }

        covered = space.add(pattern);
        auto next = covered ? ModulePtr<Block>(nullptr) : addBlock();

        if(resolvePattern(pattern, value, next) != PatternResult::Never) {
            auto errors = context.diagnostics.errorCount();
            resolve(body, nullptr, false);

            // An alternative that failed to resolve at all did not leave the block either, and
            // saying so as well would only repeat the first diagnostic in weaker terms.
            if(current && errors == context.diagnostics.errorCount()) {
                context.diagnostics.error("an alternative of a declaration has to leave the block - return, break, or continue"_v,
                                          body.source);
            }
        }

        bindings.resize(checkpoint);
        current = next;
    }

    if(!covered) {
        context.diagnostics.error("the alternatives of this declaration do not cover %@"_v,
                                  declaration.pat.source, space.gap());
    }

    current = success;
    for(Size i = declared.size(); i > 0; i--) bindings.push(declared[i - 1]);
}

/*
 * `| else -> ...` beside a skipping lens call - Design.md's Calling a lens, Analysis-Lens.md §3.2.
 *
 * The alternatives are a `match` over what the skip *carried*, not over what the lens hands its
 * continuation: the continuation did not run, so there is nothing of that shape here to match. What
 * a skip carries is the `Try` instance's third type - for a `Maybe` that is unit, so `| else -> ...`
 * is the whole of what can be written, and for a `Result` it is the error itself rather than the
 * `Result` around it, so `| match e -> ...` binds it.
 *
 * The rule resolveBinding above states as "an alternative has to leave the block" is deliberately
 * *weaker* here. It exists there because the names the pattern bound have to be live for the rest of
 * the block, so there must be one way to reach that code and the pattern must have matched on it.
 * A lens skip has no rest of the block - the rest of the block *is* the continuation, and on this
 * path it did not run - so an alternative may simply produce the value the block would have, and
 * divergence is the common case of that rather than a separate requirement.
 *
 * A null `reason` is a skip that carries nothing, which is `Maybe`'s and the common one.
 */
void ExprResolver::resolveSkipAlternatives(const ast::VarDecl& declaration, ModulePtr<Value> reason,
                                           bool used, BranchArmList& arms) {
    auto alternativeList = declaration.alts;
    auto alternatives = alternativeList.contents(parse);

    // Nothing carried is nothing to match, so the wildcard the grammar already writes for `| else ->`
    // is the only alternative there can be. Said out loud rather than left to a `match` over a value
    // that does not exist.
    if(!reason) {
        for(Size i = 0; i < alternatives.size() && current; i++) {
            auto alternative = alternatives[i];

            if(i > 0) {
                context.diagnostics.warning("this alternative is unreachable - this lens's skip carries nothing, so the one before it already covers every way here"_v,
                                            alternative.pat.source);
                continue;
            }

            if(alternative.pat.kind != ast::Pat::Any) {
                context.diagnostics.error("this lens's skip carries nothing to match, so `| else -> ...` is the only alternative it can have"_v,
                                          alternative.pat.source);
            }

            auto value = resolve(alternative.expr, nullptr, used);
            if(current) arms.push(BranchArm { current, value, alternative.expr.source });
        }

        return;
    }

    // As in resolveMatch and resolveBinding: the space keeps the patterns by address, so they are
    // copied out of the parse list once rather than read per use.
    Array<ast::Pat> patterns;
    patterns.reserve(U32(alternatives.size()));
    for(auto alternative: alternatives) patterns.push(alternative.pat);

    PatternSpace space(*this, valueType(reason));
    auto checkpoint = bindings.size();
    auto covered = false;

    for(Size i = 0; i < alternatives.size() && current; i++) {
        auto& pattern = patterns[i];
        auto body = alternatives[i].expr;

        if(covered) {
            context.diagnostics.warning("this alternative is unreachable - the ones before it already cover every value"_v,
                                        pattern.source);
            continue;
        }

        if(!space.useful(pattern)) {
            context.diagnostics.warning("this alternative is unreachable - an earlier one already covers it"_v,
                                        pattern.source);
        }

        covered = space.add(pattern);
        auto next = covered ? ModulePtr<Block>(nullptr) : addBlock();

        if(resolvePattern(pattern, reason, next) != PatternResult::Never) {
            auto value = resolve(body, nullptr, used);
            if(current) arms.push(BranchArm { current, value, body.source });
        }

        bindings.resize(checkpoint);
        current = next;
    }

    if(!covered) {
        context.diagnostics.error("the alternatives of this lens call do not cover %@ - a skip carrying it has nowhere to go"_v,
                                  declaration.pat.source, space.gap());
    }
}
