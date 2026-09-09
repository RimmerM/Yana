#pragma once

#include "build.h"
#include "gen.h"
#include "select.h"
#include "specialize.h"
#include "intrinsic/link.h"
#include "intrinsic/expand.h"
#include "../parse/ast.h"

namespace mir {

struct Resolver;
struct TypeArguments;
struct OverloadChoice;

/*
 * The resolver, across the six files it is written in.
 *
 * One struct rather than six, because there is one walk: a declaration reaches a type, a body
 * reaches a declaration, and an expression reaches a place. What the split is about is what a reader
 * is looking for -
 *
 *  - `resolve.cpp`       the driver, written types, and constant expressions;
 *  - `resolve_decl.cpp`  pass one, which is what a module declares, and the walk that orders it;
 *  - `resolve_sig.cpp`   a written occurrence and the signature it is a position of;
 *  - `resolve_class.cpp` classes and instances, which are the things that are selected;
 *  - `resolve_expr.cpp`  expressions, which are the things that answer;
 *  - `resolve_stmt.cpp`  statements and bodies, which are the things that happen.
 *
 * - and it follows `compiler/resolve`'s own module/expr/body split, which is the one arrangement of
 * this code that has already been read by somebody looking for something. The two extra files are
 * pass one grown too large to be one: a signature is read the same way whether a module, a class or
 * an instance is what wrote it, so it stands on its own rather than in the middle of any of them.
 */

/*
 * What an operator *is* here - a call, and nothing else.
 *
 * `doc/spec/expressions.md`: "Operators are resolved by the ordinary overload rules: an operator
 * name is a member of an overload set keyed by `(name, arity)`". So `a + b` finds `+` by name at
 * arity two exactly as `size(x)` finds `size`, which reaches `Num`'s member, and which body runs is
 * instance selection at the operands' type - `Resolver::finishCall`, the same tail every written
 * call goes through. The compiler holds no table of operators: what `+` computes at an `I32` is a
 * member of an instance the compiler *generated* (`provided.def`), expanded in place of the call
 * because the call is concrete, and what it computes at a `Money` is whatever `instance Num(Money)`
 * wrote.
 *
 * **How an operator groups is a declaration** - `infixl 7 +` in `lib/Core/Core.yana`, looked up
 * through the imports like any other name, with no fallback.
 *
 * The one thing left for the resolver to know about an operator is which three are *control flow*:
 * `&&` and `||` resolve their right operand under a branch rather than as a value, and `!` is the
 * truth test and a `not`. Those are `op.def`'s `Flow` roles, read off the callee the name reached -
 * so a library that declared `&&` as an ordinary function would get an ordinary call.
 */
enum class OperatorForm: U8 {
    Call,        // an ordinary call of the callee at two operands
    ShortAnd,    // `&&` - the right operand runs only if the left held
    ShortOr,     // `||` - the right operand runs only if the left did not
};

/*
 * Whether this expression is a **negated literal** - `-1`, and not a negation of `1`.
 *
 * Asked in two places that must agree: the constant walk, which folds the sign into the number, and
 * the expression walk, which does the same for the same reason. The reason is `I64`'s own minimum -
 * the lexer produces the magnitude alone, so negating after the fact is signed overflow, and
 * `resolve/const.cpp` records that having been the bug on the other side.
 */
bool isNegation(Context& context, ast::ParseBase parse, ast::Expr& expr);

/*
 * Whether `@platform` puts this declaration in this compilation - see the definition, which is where
 * the silence is argued. Asked once per declaration per pass, so it reports nothing.
 *
 * Free rather than a member because the declaring walks are not its only reader: `checkExports` asks
 * the same question of the same declarations from outside the resolver, and a second copy of the
 * rule there would be a second answer about which program is being compiled.
 */
bool platformEnabled(Context& context, ast::ParseBase parse, const ast::Decl& declaration);

/*
 * One infix operator, as the chain walk sees it.
 *
 * Two halves with two sources, and both are read exactly once - in `Resolver::findOperator`, which
 * is where they are put together. **What it does** is the callee the name reached, a declaration
 * like any other. **How it groups** is `mir::findFixity`'s answer, which is a declaration and has
 * no fallback: an operator no module in scope gave a fixity is an operator this expression cannot
 * be grouped by, and saying so is better than guessing a number that was wrong for four of them the
 * last time one was guessed.
 *
 * By value in `Chain` rather than by pointer, because a declared grouping is not a row of a table:
 * it is this operator as it is spelled *here*.
 */
struct Operator {
    /*
     * Every declaration the name reaches at two arguments - R1's set, and what selection is run
     * over where it holds more than one.
     *
     * A set rather than a lookup repeated at the point of use, for the reason this struct is by
     * value: the readings a name has are a property of the file it is written in, and the climb
     * happens after the chain was flattened. It carries the name for the same reason - a diagnostic
     * about the choice has to be able to say which operator it is about.
     */
    OverloadSet set;
    StringId name {};

    // The one candidate, where the set holds exactly one - which is every operator in the corpus.
    // Null where there is something to choose between, and the climb then takes the overloaded path.
    DeclPtr<Function> callee;

    U8 precedence;
    OperatorForm form;
    Assoc assoc = Assoc::Left;
};

/*
 * A written number, as both literal paths take it - the class it converts through, the number as
 * two's complement bits, and the number as a double. See `Resolver::writtenLiteral`.
 *
 * The **magnitude and the sign** travel beside the folded bits rather than instead of them, because
 * the two answer different questions: what the constant *is* is the two's complement, and whether
 * the type can hold it is asked of the magnitude - `I8` holds -128 and not 128, so a negative
 * literal reaches exactly one further than a positive one, and folding the sign in first leaves
 * `-1` and `18446744073709551615` the same bits with only one of them an `I64`.
 */
struct WrittenLiteral {
    intrinsic::Known role = intrinsic::Known::fromInt;
    U64 bits = 0;
    U64 magnitude = 0;
    double value = 0;
    bool negative = false;
};

/*
 * An infix chain, flattened.
 *
 * The parser nests infix expressions to the right without regard for precedence, because fixity is
 * a module-level property it cannot know. So the chain is flattened whole and re-associated by
 * `climb`, which is the same two steps the current resolver takes for the same reason.
 */
/*
 * What `[1, 2, 3]` builds at a fixed-array position - see `Resolver::arrayLiteralShape`, which is
 * the one place the literal's rules are stated and both readings of one ask.
 */
struct ArrayLiteralShape {
    DeclPtr<Type> element;
    U64 length = 0;

    // Whether the one element written fills the whole array - `[0] :: [Int *8]`. The callers spread
    // it themselves, because what each is filling differs: one has nodes and the other constants.
    bool fill = false;

    // Set where the length is a **const parameter** - `[0] :: [U8 *n]`, which only a fill can be and
    // which only a specialization can count. `length` means nothing then, and what the value path
    // builds is one component and `NodeAggregate::fill`.
    bool quantified = false;
};

struct Chain {
    SmallArray<ast::Expr*, 8> operands;
    SmallArray<Operator, 8> operators;
    SmallArray<LocationId, 8> sources;
};

/*
 * Whether anything reported since this was taken.
 *
 * The question every walk that has a *silent* failure path actually wants: `resolvePlace` answers
 * null both for "this is not a place" and for "this is a place and it is broken", and only the
 * second has already said something. Four call sites asked `errorCount()` for that and were asking
 * the wrong thing - a program-wide counter is nonzero the moment any earlier declaration reported,
 * so every one of them fell silent for the rest of the file once anything at all had failed. A
 * fixture whose second mistake is the interesting one records nothing about it.
 *
 * Taken at the point the walk begins, so what it measures is that walk.
 */
// The `Test` module's shape, as `resolveTestEntry` reads it - defined in resolve_test.cpp, where the
// five fields of `Test.Case` are named once and nowhere else.
struct TestLibrary;

struct ErrorMark {
    explicit ErrorMark(Context& context): context(context), before(context.diagnostics.errorCount()) {}

    bool reported() const { return context.diagnostics.errorCount() != before; }

    Context& context;
    U32 before;
};

/*
 * One name a body bound, and the storage it names.
 *
 * `local` is **null for a name whose initializer failed**, and that entry is still pushed: the name
 * is in scope from that point whether or not it has storage, so a later read of it finds a binding
 * that has already been reported rather than falling through to "unknown name" - one mistake, one
 * message - and does not silently reach an outer name of the same spelling either.
 */
struct Binding {
    StringId name;
    Local* local;
};

/*
 * One name a pattern would bind, and the projection it names - `resolve_match.cpp`.
 *
 * Recorded rather than made, because a pattern's test and its names happen at different times: the
 * test is where the pattern is written and the names are only meaningful in the arm it selected. A
 * `Place` is interned rather than emitted, so building one before the branch produces no instruction
 * and the read that would be wrong there happens in the arm like any other.
 */
struct PatternBinding {
    // Zero for `->_`, which takes the value out and names nothing. It still gets storage, because an
    // owner lives in a place and the drop this asked for has to be owed from one.
    StringId name;
    Place* place;
    LocationId source;

    // `->v` and `&->v` - the two axes `doc/spec/ownership.md` gives a binding, written on a pattern
    // where a parameter and a `let` already write them. `take` asks for the owned access that leaves
    // the pivot moved from, and `writable` says the storage the move landed in may be written
    // through. `writable` without `take` is `&` alone, which `resolvePattern` refuses.
    bool take = false;
    bool writable = false;

    // True when `place` is a record field whose own declaration owns the value there, as opposed to
    // reaching it through one of the record's origin slots - `RecordField::occurrence`'s comment.
    // Set by `resolveConstructorPattern` and `resolveTuplePattern`, the two places a field's
    // declaration is in hand, and false everywhere a `PatternBinding` names a pivot outright rather
    // than a projection of one. What it guards: `Builder::fieldPlace` folds the parent's contract
    // into an owned field's, so an owned field reached through an exclusively-borrowed pivot carries
    // `Access::Unique` the same as a genuine `&'v` payload does. Without this bit `bindPattern` could
    // not tell the two apart from the contract alone, and read the inherited one as "this binding is
    // a reference" - `Point{x, y}` over `&self: Point` made `x` writable with no `&` anywhere in the
    // pattern.
    bool ownedField = false;
};

/*
 * A condition, in the two halves it is emitted in - `resolve_match.cpp`.
 *
 * The pivot is a **value**, evaluated once where the condition is written; the tests are **control
 * flow**, emitted inside the alternative they guard so that a failing one reaches the next
 * alternative rather than a merge of its own. Two halves rather than one `Node*` because they belong
 * in different blocks, which is what a nested pattern needs and a single condition cannot give.
 */
struct Condition {
    // Set where the condition is `x is p`: the storage the pattern projects, and the pattern.
    Place* pivot = nullptr;
    ast::Pat* pat = nullptr;

    // Set where it is an ordinary expression of type `Bool`.
    Node* value = nullptr;

    LocationId source = kNullLocation;

    /*
     * The rest of a **conjunction**, where the condition is one - `resolveCondition`.
     *
     * `if m is Just(v) && v > 0` is a sequence of tests and not a value that is then tested: what
     * the left operand binds is in scope for the right one, which it cannot be if the left has
     * already been merged back into a `Bool`. So the chain is flattened once, this holds the head's
     * own test, and the operands after it are resolved *inside* the block the tests before them
     * proved - which is why they are an AST position here rather than a value.
     *
     * The chain is the caller's storage. It has to outlive `resolveTests`, and the caller is what
     * `armsValue` gives the two a common scope in.
     */
    Chain* chain = nullptr;
    Size operandIndex = 0;
    Size operatorIndex = 0;

    explicit operator bool() const { return pivot != nullptr || value != nullptr; }
};

/*
 * How one captured binding travels into a lambda's environment - `resolve_lambda.cpp`.
 *
 * All four the spec names. `Owned` is the **consumed** capture, and which of the four a binding
 * takes is decided by the *position the lambda fills* rather than by the body: an environment that
 * owns something is one that may outlive the frame that built it, which is exactly the heap-placed
 * environment an owning position asks for and a borrowing one does not.
 */
enum class CaptureKind: U8 {
    Copy,
    Shared,
    Unique,
    Owned,
};

// One binding a lambda's body named that its enclosing function bound. `field` is where it sits in
// the environment record, which is the index both ends read it at: the body projects it out of the
// environment parameter and the enclosing frame installs it there.
struct Capture {
    StringId name;
    Local* outer;
    U16 field;
    CaptureKind kind;
};

/*
 * One lambda being lifted out of the body being resolved - `resolve_lambda.cpp`.
 *
 * The lifted body is a *separate declaration*, so the enclosing function's locals are not names it
 * may say: `findLocal` stops at the frame's floor, and each name the body does say above it becomes
 * a field of the environment rather than a local.
 */
struct LambdaFrame {
    Function* lifted = nullptr;

    // The environment parameter's storage, in the lifted body. Every capture is a field of it.
    Local* parameter = nullptr;

    // Where the lambda's own names begin, and the floor the enclosing walk was under. The range
    // between them is exactly what was in scope where the lambda was written, which is what
    // `findCapture` searches and what nothing else may see.
    U32 floor = 0;
    U32 outerFloor = 0;

    // Where this frame's captures begin in `Resolver::captures`.
    U32 captureFloor = 0;

    // Which construct the author wrote here - for the nesting refusal, which has to name what is
    // *outside* rather than what is being built. A thunk is a lifted body like a lambda, and an
    // author who wrote neither of the words "lambda" would not recognize the message otherwise.
    bool thunk = false;

    /*
     * Whether this lambda's environment is **heap-placed** - D17, and the one bit every decision
     * below hangs off.
     *
     * Decided from the position the lambda fills, before the body walk, because `captureKindOf`
     * needs it at the moment a field is appended: an owning position takes a consumed capture and a
     * borrowing one takes a loan, and the two are different fields of a different record. It is
     * `selectStorage`'s answer rather than a bool of this file's own, so that the ranking of where
     * an object goes stays in one place - see `analyze_escape.cpp`.
     */
    bool heap = false;
};


/*
 * The origin slots one *ownership schema scope* introduces - Analysis-Borrows-Revised.md §2.2.
 *
 * A scope is one signature or one `data` declaration, and a slot is numbered within it in order of
 * first occurrence. The bare `'` is a slot like any other, keyed by the empty name: the document is
 * explicit that a name carries "no semantic difference beyond which uses are grouped", so reserving
 * an index for the anonymous group would be a table entry every signature pays for and most never
 * write.
 *
 * `introduce` is the whole reason this is a type rather than a counter. A slot is *published* by a
 * declaration - a parameter or a field - and only *used* by a result, so `fn invalid(xs: [Object])
 * -> 'Object` has to be an error rather than a slot nothing fills; §2.2 states that rule and this is
 * where the two directions are told apart.
 */
struct SlotScope {
    SmallArray<StringId, 4> names;

    static constexpr U16 kNone = ~U16(0);

    // Whether another slot may be introduced - `kMaxPackedPositions`, checked by
    // `Resolver::roomForSlot` before `find` is asked to introduce one.
    bool full() const { return names.size() >= kMaxPackedPositions; }

    U16 find(StringId name, bool introduce) {
        for(U32 i = 0; i < names.size(); i++) {
            if(names[i] == name) return U16(i);
        }

        if(!introduce) return kNone;

        names.push(name);
        return U16(names.size() - 1);
    }

    U16 count() const { return U16(names.size()); }
};

/*
 * One declared position, as the argument mapping sees it - `Resolver::mapArguments`.
 *
 * The two things a mapping asks of a position and nothing else: what a *name* at the site reaches it
 * by, and whether the site may leave it out. A parameter and a field answer both, which is what lets
 * a call and a construction share the walk; neither the type nor the convention is here, because
 * neither takes part in deciding which written argument fills which position.
 */
struct ArgPosition {
    StringId name {};
    bool defaulted = false;
};

/*
 * Which written argument fills each declared position - `doc/spec/expressions.md`'s Calls.
 *
 * Indexed by *position* and holding the index of the written argument, which is the direction every
 * reader wants: the values a call or a construction hands over are in declaration order, and the
 * source order is only ever a walk. `kOmitted` is a position the site left out and whose own default
 * is what fills it.
 *
 * The mapping is the whole of the feature. Named arguments, defaults, and the count check are one
 * question - which position does each argument fill, and is every position filled - and answering it
 * once is what keeps a call site and a construction from growing two sets of rules that agree until
 * one of them is changed.
 */
struct ArgMap {
    static constexpr U32 kOmitted = ~U32(0);

    /*
     * Both directions, because the mapping is built in one of them and read in the other.
     *
     * `sources` is one slot per declared position, and filling it is how the mapping is made: a
     * slot already taken is an argument given twice, and a slot still `kOmitted` when the walk ends
     * is a position taking its own default. `positions` is what the *reader* wants, since
     * evaluation follows the source and each written argument then has to say which slot it lands
     * in - which is not the first read backwards cheaply, so both are kept.
     */
    SmallArray<U32, 8> sources;
    SmallArray<U32, 8> positions;

    /*
     * Where the expression that filled each position was **written** - one slot per declared
     * position, `kNullLocation` for one nothing reached.
     *
     * Here rather than derived on demand because this is the one object that knows both halves: the
     * walk below has the source list in hand and the reader - `Resolver::callerFillValue`, filling a
     * `@caller(source: p)` position - has only the parameter index. Empty for `positionalMap`,
     * whose callers are operators and the calls the compiler makes itself, neither of which has a
     * written argument list to point at; `writtenAt` answers null there rather than reading past the
     * end, which is the same "nothing reached it" a `@caller` fill already has to handle.
     */
    SmallArray<LocationId, 8> written;

    U32 count() const { return U32(sources.size()); }
    bool omitted(U32 position) const { return sources[position] == kOmitted; }
    U32 position(U32 argument) const { return positions[argument]; }

    LocationId writtenAt(U32 position) const {
        return position < written.size() ? written[position] : kNullLocation;
    }
};

/*
 * Which candidates of an overload set a position may mean at all - `Resolver::selectOverload`'s
 * filter.
 *
 * One row rather than a predicate, because there is exactly one position in the language that reads
 * a set narrower than the whole of it: a `for` runs an iterator, so a plain function of the name is
 * not a candidate of that loop and must not take part in what the set agrees about. Filtering by the
 * *kind* is deliberately not enough to choose - `Iter.Overload.yana` has two iterators of one name -
 * which is why this narrows the set and R2-R5 still decide it.
 */
enum class CandidateKind: U8 {
    Any,
    Iterator,
};

/*
 * The map for an argument list written **in order, by position and with no names** - an operator's
 * operands, and the calls the compiler makes itself.
 *
 * `mapArguments` is the same answer for a *written* list, where a name may put an argument in a
 * position that is not its own and a count may be wrong. Neither is a question here: an operator
 * writes its operands in order and writes nothing else, so the map is the identity over the leading
 * positions.
 *
 * What is still a question is the positions **behind** them. `Program::addFunctionName` files a
 * declaration under every arity its defaults admit, so a set found at two operands may hold
 * `fn <+>(a: X, b: Y, scale: Z = 2)` - which fills its third position from its own constant exactly
 * as a written call that left it out does. `supplied` is therefore not `callee.writtenArity()`, and
 * `fillDefaultArguments` is what reads the difference.
 */
void positionalMap(Function& callee, Size supplied, ArgMap& into);

/*
 * What an argument list is being mapped onto - the positions, and the words a message about them
 * uses.
 *
 * One struct rather than six parameters, because the two sites that fill it are the interesting
 * thing about the mapping and a reader wants their differences together: a call names parameters and
 * counts arguments, a construction names fields and counts fields, and *nothing else* differs.
 */
struct ArgSite {
    // The declared positions, in their own order - a callee's parameters or a constructor's fields.
    const ArgPosition* declared = nullptr;
    U32 count = 0;

    // What a message is about - the callee or the constructor.
    StringId subject {};

    // One declared position - "parameter", "field" - and what the site's written arguments are
    // called when they are counted: "arguments", "fields".
    StringView one;
    StringView many;

    // The call or the construction as a whole, for the messages that are about neither one argument
    // nor one position.
    LocationId source = kNullLocation;

    /*
     * Whether a mapping that does not work out is *reported*.
     *
     * False for exactly one caller: overload selection, which maps the written arguments onto each
     * candidate of R1's set in turn and has to be able to ask "does this one take them at all"
     * without saying so. A candidate that cannot be mapped is not a mistake - it is a candidate,
     * and the mistake is only that *nothing* took them, which is a message about the set.
     */
    bool report = true;

    /*
     * Positions filled by arguments written to the **left** of the name - a dot-call's receiver,
     * and nothing else today.
     *
     * `x.f(y)` is `f(x, y)`, so the receiver occupies position 0 of the mapping, of the count and of
     * the ordering rule: it is positional by construction, since there is nowhere to write a name
     * for it, and everything written in the parentheses starts one position later. Carrying it as a
     * count rather than as a synthetic entry in the written list is what keeps the walk below over
     * the source exactly as the author wrote it - `x.f(self: y)` is then the ordinary "given twice",
     * reported by the same line that reports it for `f(y, self: y)`.
     */
    U32 leading = 0;

    // Where those leading arguments were written - the receiver's own expression, for the same
    // reason `ArgMap::written` records the rest: a `@caller(source: p)` naming a dot-call's receiver
    // is the text of what was written before the dot.
    LocationId leadingSource = kNullLocation;
};

/*
 * The value written to the left of a dot-call's name, resolved once and handed to the call.
 *
 * A **place** rather than a value, which is the whole of why the form needs no second call path:
 * what a receiver is read as - a copy, a move, a shared loan, an exclusive one - is `readMode`'s
 * answer at the position it fills, and the position is not known until the callee is. `x.f(y)` and
 * `f(x, y)` therefore go through one `argumentMode`, and a receiver that is not storage is a
 * temporary of the frame exactly as `f().x`'s target is - `projectionTarget` makes both.
 *
 * Empty for every ordinary call, which is what the default argument on `resolveArguments` says.
 */
struct Receiver {
    Place* place = nullptr;
    LocationId source = kNullLocation;

    explicit operator bool() const { return place != nullptr; }
    U32 leading() const { return place ? 1 : 0; }
};

/*
 * A call the `for` and lens forms reached **by name** - the declaration, and the receiver where the
 * name was written with a dot.
 *
 * Those two forms decide what kind of construct they are before resolving anything, which is why
 * they look the name up themselves rather than going through `resolveCall`. A dot form is the same
 * question one step out: `for x in xs.items()` names `items` and passes `xs` as its first argument,
 * so what the probe has to carry back is which expression the receiver was - `handoverCall` resolves
 * it, in the caller's block and before the arguments, exactly where a written first argument would
 * have been resolved.
 *
 * The probe asks the **bare** name, and that is a limitation the two forms share with the shipping
 * stage: the receiver's own namespace (`fn String.chars`) cannot be searched before the receiver has
 * a type, and giving it one is emission. The ordering is the other way round for a reason - see
 * `namedCallee`, where `upTo(n).twice()` is the case that decides it.
 */
struct NamedCall {
    Function* function = nullptr;
    ast::FieldExpr* dotted = nullptr;

    explicit operator bool() const { return function != nullptr; }
    Function* operator -> () const { return function; }
    U32 leading() const { return dotted ? 1 : 0; }
};

/*
 * The *name* a traversal call writes, before anything has decided which declaration it reaches.
 *
 * `namedCallee` answers a declaration and answers none where the set has several, which is R1's
 * "and also null where it has several" - and a `for` loop needs the question one step earlier than
 * that, because a set of several iterators is exactly the case R5 decides. So the two halves are
 * split: this is what the call site *says*, and the declaration is what a lookup or a selection then
 * makes of it.
 */
struct NamedCallSite {
    StringId name;
    U32 arity = 0;
    ast::FieldExpr* dotted = nullptr;

    explicit operator bool() const { return name != StringId(); }
};

/*
 * A value the expression walk is holding while something that opens blocks is resolved.
 *
 * `one + (if c then 2 else 0)` is the shape, and the problem it poses is not about `if`: evaluation
 * is left to right, so `one` is produced before the branch and added after it, and §5.9 says a value
 * that crosses a block boundary is handed over as a block argument. Nothing in the *source* says
 * `one` crosses one - the author wrote an addition - so the crossing is something the walk has to
 * notice about its own state.
 *
 * Which is what these are. A frame registers the values its C++ locals are holding, and every
 * construct that builds blocks threads the registered values through them and points the locals at
 * what came out - `Carry` below. So the rule is kept in the two places that make blocks rather than
 * in the twenty positions that can hold a value across one.
 *
 * By reference rather than by copy, because the *handles* are what has to be updated: what the
 * addition adds is whatever `one` names after the branch, and a walk that carried a copy would have
 * threaded the value and then used the stranded one.
 */
struct Held {
    explicit Held(Resolver& resolver);
    ~Held();

    Held(const Held&) = delete;
    Held& operator = (const Held&) = delete;

    virtual U32 count() const = 0;
    virtual Node*& at(U32 index) = 0;

    Resolver& resolver;
    Held* outer;
};

// A list of values under construction - a call's arguments, a record's fields. Entries may be null:
// a field whose value is being resolved right now has no node yet, and a carry skips it.
template<class T>
struct HeldList: Held {
    HeldList(Resolver& resolver, T& values): Held(resolver), values(values) {}

    U32 count() const override { return U32(values.size()); }
    Node*& at(U32 index) override { return values[index]; }

    T& values;
};

// One value - an operator's left operand, which is the position the whole mechanism was found by.
struct HeldValue: Held {
    HeldValue(Resolver& resolver, Node*& value): Held(resolver), held(value) {}

    U32 count() const override { return 1; }
    Node*& at(U32) override { return held; }

    Node*& held;
};

/*
 * The values crossing one construct's blocks, and the four calls that thread them.
 *
 * Built at the point a construct is about to make blocks, from the `Held` frames standing at that
 * moment. It is a snapshot of *which* values cross, not of what they are: the handles are read and
 * written through the frames, so a construct nested inside an arm carries the same values on again
 * and the outer construct sees what came back.
 *
 *  - `declare` gives a block one argument per carried value.
 *  - `edge` answers what a terminator into a declared block supplies.
 *  - `arrive` points the handles at the arguments of the block now being built.
 *  - `save` keeps what the handles name, for an edge that is emitted later than the block it leaves.
 *
 * The same list goes on both edges of a branch, and that is a statement rather than a convenience: a
 * value live before a branch is live on both sides of it, because which arm runs is not something
 * the walk knows.
 */
struct Carry {
    Carry(Resolver& resolver, LocationId source);

    U32 count() const { return U32(slots.size()); }

    void declare(Builder& builder, Block& block);
    void arrive(Builder& builder, Block& block, U32 first);

    void edge(Builder& builder, SmallArray<Operand, 8>& into);

    // The same, for a snapshot taken at the end of one alternative rather than for the state now.
    // A raw pointer because `armsValue` holds every alternative's snapshot in one flat array - the
    // count is this carry's own, so a list per alternative would be an allocation to say so.
    void edge(Builder& builder, const Node* const* state, SmallArray<Operand, 8>& into);

    // What every carried value is *now*, appended. A template on the list because the two callers
    // hold different ones - one snapshot, and one flat array of every alternative's - and neither
    // wants the other's inline bound.
    template<class T>
    void save(T& into) const {
        for(Size i = 0; i < slots.size(); i++) into.push(value(U32(i)));
    }

    // What `save` kept, put back. For a walk that leaves the block it was building and comes back to
    // it, which is the guard declaration's shape: its alternatives are built in the block the
    // pattern's failure reaches, and the rest of the code goes on in the block the pattern's success
    // reaches - so the handles have to name what each of the two branches carried into it.
    template<class T>
    void restore(const T& from) {
        for(Size i = 0; i < slots.size(); i++) value(U32(i)) = from[i];
    }

    // Which held value, as the frame that holds it and the position in it. A frame is a stack object
    // and an index does not move, where a pointer into a list the frame may still grow would.
    struct Slot {
        Held* frame;
        U32 index;
    };

    Node*& value(U32 i) const { return slots[i].frame->at(slots[i].index); }

    SmallArray<Slot, 4> slots;
    SmallArray<Occurrence, 4> occurrences;
    LocationId source;
};

/*
 * The loop a `break` or a `continue` leaves, innermost first - Analysis-Lens.md §5.1.
 *
 * A stack of stack objects for the same reason `Held` is one: a frame is a C++ local of the walk
 * that built the loop's blocks, so the two nestings are the same nesting and a frame cannot name a
 * block its builder has finished with.
 *
 * What it holds is the two blocks and the **carry**, and the carry is the half that is easy to
 * leave out: a `break` is an edge into a block some enclosing construct declared parameters on, so
 * it supplies exactly what the loop's own edges supply. Reading it off the loop rather than
 * rebuilding one at the exit site is what keeps a value crossing the loop crossing it on every edge.
 *
 * `region` is the continuation region the loop's blocks are in. A `break` written in a region below
 * it does not jump - the block is in another function once the continuation is outlined - so it is
 * a row of the exit signal instead, and until there is one the mismatch is what reports it.
 */
struct Loop {
    Loop(Resolver& resolver, Carry* carry, Block& exit, Block& again, ContinuationId region,
         bool leavesRegion);
    ~Loop();

    Loop(const Loop&) = delete;
    Loop& operator = (const Loop&) = delete;

    Resolver& resolver;
    Loop* outer;

    // Null exactly where `leavesRegion` is set, and only there: an edge that leaves a continuation
    // carries nothing, because the block on the far side is another function's under the CPS
    // lowering. Every other loop threads what the enclosing expression is holding.
    Carry* carry;

    // Where `break` goes, and where `continue` does - the header of a `while`, and the advance of a
    // counted `for`, which is why this is a block rather than "the loop's header".
    Block& exit;
    Block& again;

    ContinuationId region;

    /*
     * Whether leaving this loop leaves the continuation region it is written in - the `for` over an
     * `iter fn`, where the loop *is* the region.
     *
     * Both edges then go to the same block, which is where the caller carries on either way, and
     * what tells them apart is the kind: ending an iteration is falling out, and `break` is the row
     * that stops the iterator. Everywhere else the two are edges to two different blocks.
     */
    bool leavesRegion;
};

/*
 * Which of the three positions a written occurrence stands in - Analysis-Borrows-Revised.md §2.1's
 * two tables.
 *
 * They differ in exactly two things and it is worth naming rather than passing two flags: what an
 * unqualified spelling defaults to - a parameter borrows, a result and a field own - and whether an
 * unqualified `&` is legal at all, which it is only on a parameter, because "`&T` without an
 * external-owner qualifier is not a stored reference spelling".
 */
enum class Position: U8 {
    Parameter,
    Result,
    Field,

    /*
     * What a `lens fn` hands to its continuation - Analysis-Borrows-Revised.md §3.3, and
     * Analysis-Lens.md §12.1's sixth item.
     *
     * A binding position, which is why it is a fourth caller of `resolveOccurrence` rather than a
     * second reading beside it: unqualified means shared and continuation-bounded exactly as a
     * parameter does, `&` means unique without having to say whose, and `->` hands the value over.
     * The shipping stage wrote that rule twice - once for parameters and once in
     * `resolveHandedArguments` - and twice it was changed in only one of them.
     *
     * What it does *not* share with a parameter is the direction a slot travels. A parameter
     * publishes an origin and a hand-over may only name one somebody else published, on the same
     * terms a result does: the relationship has to be stated at both ends of the signature.
     */
    Handover,
};

/*
 * The import graph - `import.cpp`, where the order between these two is argued.
 *
 * `discoverModules` creates a `Module` for `root` and for everything reachable from it, filling
 * `groups` with the parse-side group of each in the same order; `linkImports` then fills in the
 * edges. Two passes because a cycle has to have one meaning whichever module was compiled first.
 */
void discoverModules(Context& context, Program& program, ast::ModuleGroup& root,
                     ast::ModuleProvider* provider, Array<ast::ModuleGroup*>& groups,
                     Buffer<ast::ModuleGroup*> testRoots = {});
void linkImports(Context& context, Program& program, Buffer<ast::ModuleGroup*> groups);

/*
 * `pub`, from the declaring side - `export.cpp`.
 *
 * The reading side is `name.h`: a symbol found in an imported module is a hit only when it was
 * exported. What is here is the two things that clause cannot say by itself - a marker that means
 * nothing, and an exported declaration that hands out a type nobody outside can name.
 */
void checkExports(Context& context, Program& program, Buffer<ast::ModuleGroup*> groups);

// The second half of the same check, a pass later - a `pub let`'s type is what its initializer said,
// and nothing before `resolveInitializers` knows it. See the definition.
void checkExportedGlobals(Context& context, Program& program);

struct Resolver;

// `"abc"` - the kind test, said once because three readers of a literal ask it. See the definition.
bool isStringLiteral(ast::Expr& expr);

/*
 * Whether a mapping's selectors are close enough together to be *placed* - the one bound recognition
 * puts on a `map`.
 *
 * A **representability** rule and not a size rule, which is the distinction D3 turns on. The
 * lowering picks between six shapes and five of them read no memory, but the sixth - a run of slots
 * indexed by the selector - has to exist for every mapping the node admits, because there is no
 * comparison chain left below there to fall back to. So a domain too sparse to place is refused
 * while the chain is still standing, and the numbers are the ones that keep the widest thing a
 * library writes - a pinned `errno` list, nineteen constructors over about a hundred and twenty -
 * well inside them.
 *
 * What this excludes that an arithmetic form would have served is a handful of constructors pinned
 * to numbers very far apart whose results happen to fit a line. That is a real gap and it is
 * deliberate: closing it means asking `chooseMapForm` here, which is the second answer to the one
 * question carrying the mapping down as a node exists to avoid.
 */
bool mapSpanAdmits(I64 lowest, I64 highest, Size count);

// A mapping's entries ascending by selector, in place - the order `NodeMap` declares. `@value(n)` is
// what makes it necessary: a declaration may pin its constructors to numbers in any order at all.
void sortMapEntries(MapEntry* entries, Size count);

/*
 * Which module the resolver is reading, for the length of a scope.
 *
 * Restoring rather than clearing, because resolution nests: an alias target is read on demand and
 * may be another module's, and a body is resolved in the middle of the walk that declared it. The
 * file index travels with the module for the same reason it exists at all - an import is scoped to
 * the file that wrote it, so "which module" and "which file of it" are one position and not two.
 */
struct ModuleScope {
    ModuleScope(Resolver& resolver, DeclPtr<Module> module, U16 file = 0);
    ~ModuleScope();

    Resolver& resolver;
    Module* saved;
    U16 savedFile;
};

/*
 * The three fields of a `Run(a)`, and the two of an `Array(a)` - read off the record roles rather
 * than counted here.
 *
 * `known.def`'s `run` row states that the compiler reads that record's fields by position, and this
 * is the one place in this stage that does. Written as a struct rather than as three constants
 * because the *types* are wanted as well as the indices: `capacity` is `Count` and `ownsHeap` is
 * `HeapFlag`, both `@bits` refinements the library chose, and a constant built at the word width
 * would be refused by `addAggregate`'s own type check.
 *
 * `resolve_ssa/opt/place.cpp` holds the same three indices for the same reason and is the other
 * reader. Two readers of one arrangement is what J46 §3.5 argues should become one; until the flag
 * rewrite moves here, this is the half that builds and that is the half that has to agree.
 */
struct RunShape {
    DeclPtr<Type> run = nullptr;
    DeclPtr<Type> array = nullptr;

    DeclPtr<Type> capacity = nullptr;
    DeclPtr<Type> flag = nullptr;
    DeclPtr<Type> length = nullptr;

    static constexpr U16 kItems = 0;
    static constexpr U16 kCapacity = 1;
    static constexpr U16 kOwnsHeap = 2;
    static constexpr U16 kRun = 0;
    static constexpr U16 kLength = 1;

    // The two answers `ownsHeap` takes at a literal - `lib/Native/Run.yana`'s own numbers.
    // `runBorrowed` is what a literal *string* gets, because its bytes are the image's; a growable
    // literal asks the allocator and says so, until `place.cpp` decides otherwise.
    static constexpr U64 kBorrowed = 0;
    static constexpr U64 kFromHeap = 1;
};

struct Resolver {
    Context& context;
    Program& program;
    ast::ParseBase parse;

    /*
     * The module being read, and the files of every module in the program.
     *
     * One resolver for the whole program rather than one per module, and that is what the arrays
     * below need: every pass visits the modules and their files in one order, so a single running
     * index pairs a body with the signature its declaration made. Two resolvers would be two
     * pairings, and the one that reached across - an alias in one module naming an alias in another,
     * which `aliasTarget` resolves on demand - would read the wrong array.
     *
     * `groups` is parallel to `Program::modules` by index. It is a parse-region structure, which is
     * why it is here rather than on `Module`: a declaration-region node may not point into it.
     */
    Module* current;
    Buffer<ast::ModuleGroup*> groups;

    /*
     * Whether the file the declaration walk is reading carried the `test` selector -
     * `ast::Module::test`, which cannot be re-derived from the module name.
     *
     * Set by `eachFile` beside `activeFile`, for the same reason: which file a pass is reading is
     * the walk's fact and not something each pass has to rediscover. Read by `declareGlobals` and
     * `resolveGlobals`, the two places where a test file's `let` differs from a library module's.
     */
    bool activeTestFile = false;

    /*
     * Whether `here()`'s top level is a **program point** - somewhere a statement runs rather than
     * only a declaration existing - which is `declareGlobals` and `resolveGlobals`'s shared question
     * and used to be each one's own answer.
     *
     * The root module's is, because the synthesized entry is its top level. Under `-test`, a test
     * file's is too: `resolveTestEntry` runs it ahead of the cases exactly as the ordinary entry
     * runs the root ahead of `main`, and a build without `-test` has no such file at all, so nothing
     * here widens into library semantics the rest of the time.
     */
    bool atProgramPoint() { return here() == program.root() || (context.settings.test && activeTestFile); }

    /*
     * Runs `pass` over every file of every module, with the position set to each in turn.
     *
     * The one walk the declaration phases are written against, so that "in what order does a pass
     * see the program" is answered once. Which file is being read is what an import's scope is
     * relative to, and setting it here is what keeps `Module::activeFile` from being something each
     * pass has to remember.
     *
     * **The library's modules first, then the program's**, and the order is a rule rather than the
     * discovery order it happens not to be. A `@builtin` claim is first come, first served
     * (`bindRole`), so a program that claims a role the library fills has to be the *second* claim,
     * with the refusal on the program's declaration; read root-first, the *library's* own `pageBytes`
     * was the claim refused, and the line the diagnostic pointed at was in a file the author had
     * never opened. The library is also what the program is built on, so its diagnostics - where it
     * has any - come before the ones that may be consequences of them.
     *
     * Only the declaration passes read in this order. `Program::modules` keeps the root at index
     * zero, and everything that emits - conversion, lowering, the dumps - walks that list, so a
     * lowered module and a golden still open with the program's own functions.
     */
    template<class Pass>
    void eachFile(Pass&& pass) {
        for(auto library: { true, false }) {
            for(U32 index = 0; index < groups.size(); index++) {
                if(groups[index]->library != library) continue;

                ModuleScope scope(*this, program.modules[index]);

                for(U16 file = 0; file < U16(groups[index]->files.size()); file++) {
                    current->activeFile = file;
                    activeTestFile = groups[index]->files[file]->test;
                    pass(*groups[index]->files[file]);
                }
            }
        }
    }

    /*
     * One entry per *written* declaration, including the ones the first pass rejected.
     *
     * `Module::functions` and the rest hold only what was accepted, because they are what emission
     * walks. These are what the second pass pairs bodies and initializers against, and they need the
     * holes: a rejected `fn` that contributed no entry would make every declaration after it resolve
     * its body against the wrong signature, and the diagnostics that produces are about names in a
     * function nobody wrote.
     */
    Array<DeclPtr<Function>> writtenFunctions;

    /*
     * The declarations of the functions whose result their own body decides - `Function::inferResult`,
     * indexed by `Function::written`.
     *
     * A list of its own rather than an entry per written function, because a caller demanding a
     * result asks by function and not by position: the walk that pairs bodies with signatures has
     * `writtenFunctions` for that, and this one exists only so that `requireResult` can find the
     * body it has to build now. Almost every declaration in a program contributes nothing to it.
     */
    Array<ast::Decl*> inferringDecls;

    /*
     * How far each consuming walk has read, **across files**.
     *
     * On the resolver rather than a local of each walk, because the lists above are one per program
     * and the walks are one per file: a counter that restarted at each file would read the first
     * file's declarations again and pair every body after the first file with the wrong signature.
     * Every producing pass and every consuming pass visits `root.files` in one order, which is what
     * makes a single running index the right pairing.
     */
    Size nextRecordFields = 0;
    Size nextClassMembers = 0;
    Size nextGlobalInit = 0;
    Size nextFunctionBody = 0;
    Size nextInstanceBody = 0;
    Size nextClassBody = 0;
    Array<DeclPtr<Global>> writtenGlobals;
    Array<DeclPtr<Instance>> writtenInstances;

    /*
     * The instances a `deriving (...)` clause expanded to, in declaration order, with a null where
     * the class could not be derived.
     *
     * `writtenInstances` for a derived one is filled by `declareInstance` exactly as it is for a
     * written one, so the two lists advance together and `nextDerivedBody` is the cursor that keeps
     * them that way: the body pass walks the same declarations in the same order and takes one entry
     * per class named in the clause.
     *
     * The AST is kept rather than rebuilt because rebuilding it would report every refusal a second
     * time - and because what a body is resolved out of has to be the declaration the signature was
     * declared from, not an equal one.
     */
    Array<ast::Decl*> derivedInstances;
    Size nextDerivedBody = 0;

    /*
     * One statement of the **root** module's top level, in the order it was written -
     * Analysis-Initialization.md stage B.
     *
     * Collected by the declaration pass and run by the synthesized entry function. Only the root
     * module has any: a library module's top-level code would have to run at some point in a
     * program's startup, and defining that point is the cross-module half stage B does not do.
     *
     * `stmt` is held **by value**, which an `ast::Expr` may be - it is a kind and a region handle.
     * A pointer would be into the copy `contents()` produced for one loop iteration, which is gone
     * by the time the entry is built.
     *
     * `first` and `count` name this statement's slice of `writtenGlobals`, one entry per name it
     * declares and null where the declaration pass rejected one - so the entry sequence does not
     * have to look a name back up to find what its own statement declared. `count` is zero for a
     * statement that is not a `let`, which is the shape that has nothing but code to run.
     */
    struct TopLevelStmt {
        ast::Expr stmt;
        LocationId source = kNullLocation;

        // The module the statement was written in. Always the root for `topLevel`, and any module
        // with a test file for `testTopLevel` - which is the one thing the second list needs that
        // the first does not, since a test file's initializers are resolved in their own module.
        DeclPtr<Module> module = nullptr;
        U16 file = 0;
        U32 first = 0;
        U16 count = 0;
    };

    Array<TopLevelStmt> topLevel;

    /*
     * The `let`s a `.test.yana` file declares at module level - Design-Test.md §11.2's F5.
     *
     * A second list rather than entries in the one above, because the two are different things that
     * share a shape. `topLevel` is *the program's start*: one file's, arbitrary statements, in
     * written order. This is a set of initializers, one group per test file, run by the synthesized
     * test entry before any case and existing only in a build where that entry exists at all - which
     * is what keeps it out of library semantics, since a test file is selected in and out as a unit.
     */
    Array<TopLevelStmt> testTopLevel;

    /*
     * The dynamic globals whose declaring statement has not run yet - stage B's item 2.
     *
     * Held for the length of the **entry sequence** and empty everywhere else, which is what makes
     * "read before its initializer runs" a question this stage can answer at all: a global has no
     * state row in any frame, so the initialization dataflow the ownership pass runs over locals has
     * nothing to say about one. What decides it is *where in the sequence* the read is, and the
     * sequence is resolved in written order - so a set that shrinks as the walk goes is exactly the
     * fact, rather than an approximation of it.
     *
     * Nothing after the entry reads a global before it is initialized, because nothing after the
     * entry reads one *before* anything: every other body runs after the whole sequence.
     */
    Array<DeclPtr<Global>> uninitialized;

    // The classes this file declared, in declaration order and null where one was rejected - the
    // same pairing `writtenFunctions` is, and for the same reason: the member pass and the body pass
    // walk the AST again and have to find the declaration the shell pass made.
    Array<DeclPtr<Class>> writtenClasses;
    Array<DeclPtr<RecordType>> writtenRecords;

    // The `alias` declarations as they were written, indexed by `Alias::written`, so that a target
    // can be read on demand rather than in declaration order - an alias may name one declared below
    // it, which is what makes the walk a graph rather than a list.
    Array<ast::Decl*> writtenAliasDecls;


    /*
     * The names in scope in the body being resolved, innermost last.
     *
     * A flat array with the scope boundary kept on the C++ stack rather than a tree of maps: a
     * lookup walks backwards and stops at the first match, which is shadowing, and leaving a scope
     * is truncating the array to the length that was saved on entry. Bodies are small enough that
     * the linear walk is the cheaper structure, and it is the one that makes shadowing fall out
     * rather than being implemented.
     */
    Array<Binding> bindings;

    // The function whose body is being resolved, which `return` and `yield` need and nothing else
    // does. Swapped for the length of a lifted body - `resolveLiftedBody` - which is the one walk
    // that resolves two functions at once.
    Function* currentFunction = nullptr;

    /*
     * The two locals a `lens fn`'s desugaring adds, and null in every other body.
     *
     * `continuationLocal` is the `$body` parameter a `yield` calls; `yieldedLocal` is where its
     * answer is parked, and is what the lens returns. Fields rather than a lookup by name, because
     * neither is a name: the source wrote no such parameter and cannot say either symbol, so a
     * binding would put something in scope that shadowing rules would then have to be quiet about.
     */
    Local* continuationLocal = nullptr;
    Local* yieldedLocal = nullptr;

    /*
     * Whether an iterator's `yield` has to ask whether the consumer already left - `resolveYield`.
     *
     * A departure resumes after the *outermost* loop the `yield` was in, so the only `yield` a
     * departed iterator can reach again is one written after that loop. A body with a single `yield`
     * therefore has none, and this is that read off the source: one `yield` needs no guard, two or
     * more each get one. The count is syntactic because the alternative is a CFG question asked
     * after the body is built, and the answer would be the same on every program anyone writes.
     */
    bool guardYields = false;

    /*
     * How many hand-overs the walk has emitted in this body, which is what says whether a region
     * could have departed inside it.
     *
     * A departure is set by a `yield` and by nothing else, so a region whose blocks contain none
     * cannot have one to resume - and the test after it would be a branch per iteration on storage
     * nothing in reach writes. `handoverCall` compares the count across the body it builds, which
     * answers the question exactly where a syntactic scan of the extent would have to guess.
     */
    U32 handovers = 0;

    /*
     * The generic context a written type is read in - `gen.h`, and null outside a generic
     * declaration.
     *
     * One field rather than a parameter threaded through `resolveType`, because *every* written type
     * is read in exactly one context and there is never a choice about which: a field type belongs
     * to its record's head, a signature and a body belong to the function's. Passing it would be
     * passing the same value down every arm of a walk that has no other use for it, and the one
     * place a caller could get it wrong is the one place it matters - a variable resolved against
     * the wrong context is a substitution that never happens.
     */
    DeclPtr<GenContext> genContext = nullptr;

    /*
     * The values the walk is holding, innermost first - `Held`, and null where it holds none.
     *
     * A stack of stack objects rather than a list this owns: every frame is a C++ local of the walk
     * that registered it, so the two nestings are the same nesting and a frame cannot outlive the
     * values it names.
     */
    Held* held = nullptr;

    /*
     * Where the `Held` chain stops - the floor a **continuation region** puts under it.
     *
     * A `Carry` threads the values the expression walk is holding through the blocks a construct
     * makes, which is right inside one function and wrong across two: every `Handover` region is
     * outlined into a function of its own, so a block argument carrying the enclosing walk's value
     * into one names a value that function never produced. `for round in 0..n: for e in items(xs):`
     * was exactly that - the counted loop's bound and stride threaded into the iterator's blocks
     * and back out, and the SSA conversion reported an operand naming a node with no value at both
     * ends of the boundary.
     *
     * So the frames standing at a region's entry do not cross it. Their handles are untouched while
     * the region is built - nothing inside may name them - and the block after the call is
     * dominated by the block before it, so what they name is still in scope when the walk comes
     * back out. A value a program genuinely needed inside a continuation would need storage rather
     * than an edge, which is what `bindingFloor` says about a lambda one tier over.
     */
    Held* heldFloor = nullptr;

    /*
     * The loops open at this point in the walk, innermost first - `Loop`, and null outside one.
     */
    Loop* loops = nullptr;

    /*
     * The lambdas open at this point in the walk, and the captures they have found.
     *
     * A stack for the shape's sake rather than for a feature: a lambda inside another is refused by
     * name (`resolveLambda`), because reaching a capture through two environments is a chain of
     * places and not one. Written as a stack anyway so that lifting the refusal is a walk that
     * recurses rather than a field that becomes a list.
     */
    Array<LambdaFrame> lambdaFrames;
    Array<Capture> captures;

    /*
     * The lowest binding a name lookup may see - the enclosing function's names while a lambda's
     * body is lifted.
     *
     * Zero everywhere else. `bindings` is one flat array with shadowing falling out of a backward
     * walk, and a lifted body is the one place where the walk must *stop* rather than continue into
     * an outer scope: it is a separate declaration, and what it names of the enclosing one becomes a
     * capture rather than a local.
     */
    U32 bindingFloor = 0;

    /*
     * The lowest lambda frame a capture may be found through - the frames of an enclosing *body*
     * while another body is being resolved inside it.
     *
     * Zero everywhere but a demanded body. `requireResult` builds a callee's body in the middle of
     * the caller's walk, and a lambda open in the caller is a frame `findCapture` would otherwise
     * still see: a name the callee failed to bind would become a capture of a closure written in a
     * different function. The floor is the same statement `bindingFloor` makes about names, made
     * about environments.
     */
    U32 lambdaFloor = 0;

    DeclBase decl() { return *program.decl; }
    MirBase mir() { return *program.mir; }
    DeclPtr<Module> here() { return current - *program.decl; }

    // The position a declaration was written at - its module, and the file of that module its source
    // location falls in. What every on-demand walk sets before it reads a name on that declaration's
    // behalf, since an import is scoped to the file that wrote it.
    ModuleScope scopeOf(DeclPtr<Module> module, LocationId source) {
        return ModuleScope(*this, module,
                           decl()[module]->fileOf(decl(), context, source));
    }

    void unsupported(LocationId source, StringView what);

    /*
     * The one check every packed `U16` domain makes before it numbers another position -
     * `kMaxPackedPositions`. `count` is how many the scope holds already; false, with the report
     * made, where one more would not fit.
     */
    bool withinPacked(Size count, LocationId source, StringView what);

    // The same check for a generic context about to bind a variable it does not have yet - the
    // seven sites that introduce one share it, since the context is what is bounded.
    bool roomForVariable(DeclPtr<GenContext> gen, StringId name, LocationId source);

    // And for a slot scope about to publish an origin it does not hold yet - the two signature
    // positions that publish one share it.
    bool roomForSlot(SlotScope& slots, StringId name, LocationId source);

    /*
     * resolve.cpp - written types and constant expressions.
     */

    DeclPtr<Type> resolveType(ast::Type& type);

    // `@bits(n)` - the attribute a type position may carry, and the refinement it makes. Two
    // functions because the reader has no type in hand and the refiner has no attribute: see
    // `resolveBitsType`, where the range is checked against what is being refined.
    Maybe<U64> readBitsAttribute(ast::Type& type);
    bool readContainerRefinement(ast::Type& type, U64& into);

    // `@box` on a field's type - `RecordField::boxed`. Called only where a field is declared, and
    // `readingFieldType` below is how everywhere else comes to refuse it.
    bool readBoxAttribute(ast::Type& type);

    // `@host` on a field's type - `RecordField::host`, and resolve.cpp for what this stage does with
    // the answer, which is record it.
    bool readHostAttribute(ast::Type& type);

    // Where a record's `@host` fields may sit, which is after the one field that stores the value -
    // see the definition in resolve_decl.cpp.
    void checkHostFields(RecordType& record, U16 first, LocationId source);

    /*
     * Whether the type being resolved is a **field's**, which is the only position `@box` means
     * anything in.
     *
     * A flag rather than a parameter threaded through `resolveType`, because what needs to know is
     * one arm of a walk that recurses through aliases, applications and every other type form, and a
     * parameter would have to be carried through all of them to be read in one. Set for the length
     * of one field's occurrence, in `declareFields`, which is the one place a field type is read.
     *
     * It is deliberately **not** inherited by the type's arguments: `Maybe(@box Int)` is the viral
     * spelling the whole design exists not to be, so the flag is cleared as soon as `resolveType`
     * has read the attribute off the position it applies to.
     */
    bool readingFieldType = false;
    DeclPtr<Type> resolveBitsType(DeclPtr<Type> written, U64 bits, LocationId source);

    /*
     * One argument of a type application, at the position it fills.
     *
     * `resolveType` plus the one thing it cannot answer on its own: a number written where a type is
     * has no width of its own, so `Buf(4)` takes it from the parameter it fills. The head and the
     * position are what supply that, and every position that applies a head - a record, an alias, a
     * class constraint, an instance head - goes through here for it.
     *
     * Whether an argument's *kind* matches its parameter's is not asked here. That is one question
     * per application rather than one per argument, and `instantiateRecord` and `matchType` are
     * where it is answered - a second check here would be a second answer for the positions that
     * reach those without reaching this.
     */
    DeclPtr<Type> resolveTypeArgument(ast::Type& written, DeclPtr<GenContext> head, Size position);

    /*
     * A written type in a position that classifies a *value* - a parameter, a result, a field.
     *
     * A const parameter and a number stand for a count, so they are type arguments and expressions
     * and never the type of anything. Reported here rather than by whatever fails downstream,
     * because what fails downstream is a manifestation with no answer.
     */
    bool requireValueType(DeclPtr<Type> type, LocationId source);

    // Whether a tuple's labels are distinct - the one rule both spellings of an anonymous record
    // are under. See the definition.
    bool uniqueTupleLabels(const RecordField* fields, Size count);

    /*
     * `n: I32` - the type a const parameter may be of, resolved and checked.
     *
     * Null after reporting. The admissible set is the integer types, which is what every position
     * that exists needs - Implementation-Const-Generics.md §2.5. It is a *semantic* rule and not a
     * syntactic one precisely so that it can be loosened one type at a time.
     */
    DeclPtr<Type> resolveConstParameterType(ast::Type& written);

    /*
     * The type on a `::`, which is `resolveType` plus the one form that needs the value beside it -
     * `[T *_]`, whose count is read off the literal the type is ascribed to.
     */
    DeclPtr<Type> resolveAscribedType(ast::Type& written, ast::Expr& value);

    // Whether a fixed array may be this long, reporting where it may not - the one bound, asked by
    // both the written count and the counted literal.
    bool checkFixedArrayLength(I64 length, LocationId source);

    // `Array(T)` - what both `[T]` and the written name reach. See the definition.
    DeclPtr<Type> sequenceType(DeclPtr<Type> element, LocationId source);

    // The `n` of `[T *n]`, as either a number or the const parameter standing for one. False on a
    // report; exactly one of the two is set on success.
    bool resolveArrayCount(ast::Expr& written, DeclPtr<Type>& count, U64& length);

    /*
     * A constant, or the fact that this expression is not one.
     *
     * `notConstant` non-null suppresses the three "this is not a constant form" reports and answers
     * the fact instead, which is what the root module's top level needs: a `let` whose initializer is
     * an expression is a **dynamic** global there rather than a mistake, and which of the two it is
     * is exactly this question. Everything else stays rejected either way - `300 :: U8` is an error
     * at the top level too, because what is wrong with it is the number and not the form.
     */
    Constant resolveConstant(ast::Expr& expr, DeclPtr<Type> hint, bool* notConstant = nullptr);
    Constant declareDefault(ast::Expr& expr, DeclPtr<Type> type, StringId name, StringView what,
                            StringView decides);
    // The argument is in resolve.cpp, beside the definition.
    // The head a constructor in a constant position builds - see the definition, where what binds a
    // generic one is argued.
    Maybe<DeclPtr<Type>> constantHeadFor(DeclPtr<RecordType> declaration, DeclPtr<Type> hint);

    Maybe<Constant> resolveConstructorConstant(ast::Expr& expr, DeclPtr<Type> hint);

    // The other constant form a construction has - a record built out of constants. Asked where a
    // constant is what the position is for, and not from a value position; see the definition.
    Maybe<Constant> resolveArrayConstant(ast::Expr& expr, DeclPtr<Type> hint, bool* notConstant);

    /*
     * Arithmetic in a constant position - `4 * 1024 * 1024`, `wordBits - 1`, `not(mask)`.
     *
     * An operator is a call, so what these decide is the same thing a call decides: which
     * declaration, and then whether the instance serving it *at these operands* is the compiler's.
     * Only then is there an operation to fold, and `fold.h`'s table is what says which one - the
     * same table `expandProvided` builds the node from, so a body and a constant compute the same
     * thing by construction.
     *
     * Every one of these is **silent**. An expression they cannot reduce is not a mistake: in a
     * value position it is ordinary code, and in a constant one the position says what it needs.
     */
    Constant resolveInfixConstant(ast::Expr& expr, DeclPtr<Type> hint);
    Constant resolveCallConstant(ast::Expr& expr, DeclPtr<Type> hint);
    Constant climbConstant(Chain& chain, Size& operandIndex, Size& operatorIndex,
                           U8 minimumPrecedence, DeclPtr<Type> hint);
    Constant foldConstantOperator(const OverloadSet& set, const Constant* args, Size count,
                                  LocationId source);
    bool constantOperation(const OverloadSet& set, DeclPtr<Type> operand, LocationId source,
                           BinaryOp& binary, UnaryOp& unary, bool& isBinary);
    Maybe<Constant> resolveAggregateConstant(ast::Expr& expr, DeclPtr<Type> hint);

    // What an unadorned literal is, from the library declaration that says so - see the definition.
    DeclPtr<Type> literalDefault(intrinsic::Known role, LocationId source);
    Constant resolveLiteral(ast::Expr& expr, DeclPtr<Type> hint, bool negative);

    // A written number as a constant of the type it landed at, warning where the type cannot hold
    // what was written - the one place both literal paths turn a `WrittenLiteral` into a `Constant`.
    Constant foldWritten(const WrittenLiteral& written, DeclPtr<Type> type, LocationId source);

    // `True` and `False` written as a literal, which is the parser's own spelling and not the
    // source's - see the definition. `Constant::None` for every other literal kind.
    Constant truthLiteral(ast::Expr& expr);

    // A written number, read once - see resolve.cpp - and the argument the conversion class's
    // member takes for it.
    bool writtenLiteral(ast::Expr& expr, bool negative, WrittenLiteral& into);
    Constant conversionArgument(const WrittenLiteral& written);

    /*
     * resolve_decl.cpp - pass one, and the walk that orders it.
     */

    // `infixl`/`infixr` - see the definition, and `mir::findFixity` for the reading side.
    void declareFixities(ast::Module& file);

    /*
     * How a derived member forwards one position - `resolve_derive.cpp`.
     *
     * `Bare` is the class variable and nothing else, which is the one shape a newtype can unwrap and
     * wrap; `Nested` is the variable inside something, which needs a representational coercion this
     * language does not have; `Absent` is a position the derivation hands on untouched.
     */
    enum class Forwarding { Absent, Bare, Nested };

    Forwarding forwardingOf(DeclPtr<Type> type, DeclPtr<Type> variable);

    // One class of a `deriving (...)` clause as the `instance` it stands for, or null after
    // reporting - `resolve_derive.cpp`.
    ast::Decl* deriveInstance(ast::Decl& declaration, const ast::Derive& derive);

    // `alias qualified Id = Int` as the `data` it is - resolve_decl.cpp, where the rewrite and its
    // one rule are written out.
    ast::Decl& newtypeRecord(ast::Decl& declaration);

    void declareTypes(ast::Module& file);

    // `alias Pair = Two(I32, I32)` - the shells, beside the records and for the same reason: a field
    // type may name an alias and an alias target may name a record, so both have to exist as names
    // before either's contents are read.
    void declareAliases(ast::Module& file);

    // Every alias's target, resolved on demand so that one naming another is admitted whichever
    // order they were written in. Reports where the naming is a cycle.
    void resolveAliasTargets();
    DeclPtr<Type> aliasTarget(DeclPtr<Alias> alias);

    // `Bytes(I32)` from `alias Bytes(a) = Array(a)` - the target, substituted. Null after reporting
    // where the arity is wrong or the target did not resolve.
    DeclPtr<Type> applyAlias(DeclPtr<Alias> alias, DeclPtr<Type>* args, Size count,
                             LocationId source);

    /*
     * A field default that has been *found* and not yet read - `data Flags {on: Bool = True}`.
     *
     * A default is a constant of the field's type, and `True` is a constructor: reading one asks the
     * constructor table a question, and during `declareRecordFields` that table is half built. Which
     * half depends on the order the pass reaches the modules, so the same declaration would resolve
     * in one program and not in another - which is exactly the class of bug the phase ordering in
     * `resolveProgram` exists to prevent, and this is that ordering carried one level down.
     *
     * The whole position rather than a closure, because reading it later means restoring where it
     * was written: which module the name is looked up from, which file of it (an import is scoped to
     * one), and which head's variables are in scope. `resolveFieldDefaults` is the drain.
     */
    struct PendingDefault {
        DeclPtr<Module> module;
        U16 file = 0;
        DeclPtr<RecordType> record;
        U16 field = 0;
        ast::Expr* expr = nullptr;
    };

    Array<PendingDefault> pendingDefaults;

    /*
     * A **string** default, and the position it was written on - `fn f(message: String = "")`.
     *
     * The same ordering `PendingDefault` states, for the one question a string constant asks that no
     * other does: what type it has. A number's is the position's, a constructor's is its record's,
     * and a string's is the record `known.def`'s `string` row names - a *role*, bound when the
     * declaration carrying `@builtin(string)` is read, which is inside the same pass that reads
     * this default. So asking there would answer differently depending on which file came
     * first, and the answer would be "no such role" for every program whose Native module happens to
     * be declared later.
     *
     * The constant therefore takes the position's type and this records the pair. `checkTextDefaults`
     * is the drain, and it runs where every declaration is in and the role is bound.
     */
    struct PendingTextDefault {
        DeclPtr<Type> type;
        LocationId source = kNullLocation;
    };

    Array<PendingTextDefault> pendingTextDefaults;

    /*
     * A `@caller` position whose type has to be the one a string literal has, and where it was
     * written - the `@caller(source: p)` parameter itself, or the `file`/`function` field of a
     * location record.
     *
     * `PendingTextDefault`'s ordering exactly: what `String` *is* is the record the `string` row
     * names, and that role is bound while the declaration carrying it is read - which is inside the
     * same pass that reads these. Asking there would answer "no such role" for
     * every program whose `Native` module happens to be declared later, which is what it did.
     *
     * Kept apart from the defaults' list rather than merged into it because the two report
     * different mistakes: a default of the wrong type is a constant the author wrote, and this is a
     * *position* the compiler was asked to fill. `checkCallerTexts` is the drain.
     */
    struct PendingCallerText {
        DeclPtr<Type> type;
        LocationId source = kNullLocation;

        // The location field this is, and null where it is the `@caller(source: ...)` parameter
        // itself - which is the whole of what the two messages differ in.
        StringId field {};
    };

    Array<PendingCallerText> pendingCallerTexts;

    /*
     * A **string global**, and the text it holds - `let decimalPairs = "0001..."`.
     *
     * A second deferral for a second ordering. Building the value means minting the bytes as a
     * global of their own and reading the `string` and `run` rows to spell the three records over
     * it - `Resolver::stringConstant` - none of which a global's initializer may do where it is
     * read. `resolveTextGlobals` is the drain, and it runs once every declaration is in.
     *
     * It ran after every *body* while the value was produced by interpreting the library's own
     * `stringLiteral`, which is what J46 §3.5 removed; the drain has stayed where it was because
     * nothing between the two points asks a string global for its value.
     *
     * Deferring the value rather than the decision is what keeps everything in between honest: the
     * global's *type* is settled where the initializer is read, so it is not `dynamic`, nothing
     * puts it in the entry sequence, and a body above it reads a global whose type is known.
     */
    struct PendingTextGlobal {
        DeclPtr<Global> global;
        DeclPtr<Module> module;

        // The whole initializer and not the text, because a literal may be a *leaf* of it - an
        // array of strings is one global whose elements are three of them, and deferring the value
        // rather than the string is what makes the two cases one drain.
        Constant value;

        U16 file = 0;
        LocationId source = kNullLocation;
    };

    Array<PendingTextGlobal> pendingTextGlobals;

    // The drain - see `PendingTextGlobal`, where what it waits for is argued.
    void resolveTextGlobals();

    /*
     * A **`map` whose arms are string literals**, and where it stands - `PendingTextGlobal`'s
     * ordering, one tier down.
     *
     * A table of string arms is built out of the same three records one literal is, so a mapping
     * recognized inside `lib/Test/Harness.yana` could not have its table built while the rows and
     * the byte globals it needs were still ahead of it in the walk. Which of the two files comes
     * first is not something a recognition rule may depend on, and the failure was silent in the
     * worst way: the recognizer reported the library rather than declining.
     *
     * So the entries carry `Constant::Text` until every body is in, and this is the drain. Deferring
     * the *value* rather than the decision is what keeps everything in between honest, exactly as it
     * is for a global: the node's result type is settled where the mapping was recognized, so the
     * verifier, the conversion and the lowering see one shape.
     */
    struct PendingTextMap {
        MirPtr<NodeMap> node;
        DeclPtr<Module> module;
        U16 file = 0;
        LocationId source = kNullLocation;
    };

    Array<PendingTextMap> pendingTextMaps;

    // The drain - see `PendingTextMap`.
    void resolveTextMaps();

    // The drain - see `PendingTextDefault`. Reports a position whose type is not what a string
    // literal is, which is the check `declareDefault` makes for every other kind of constant.
    void checkTextDefaults();

    /*
     * A generic head whose parameters are bound and whose written *types* are not yet read -
     * `data Boxed(a = I32)`, `alias Bytes(n: Int)`, `class Chunked(c, a, n: Int -> m)`.
     *
     * The same ordering `PendingDefault` states, one level further out. A head is read in the shell
     * pass, which is the one pass in which the type namespace is deliberately incomplete: no alias
     * has been declared and half the records have not, so a head that resolved a type there would
     * see a *different* namespace from every other position - `n: Int` reported an unknown type
     * while `n: I32` did not, and `data Foo(a = Bar)` compiled or did not depending on whether `Bar`
     * was written above it.
     *
     * What the shell pass does is bind the parameter *names*, which is all a field type needs to
     * mention one. What waits is everything the head writes a type in: a const parameter's
     * annotation and a parameter's default. `resolveHeads` is the drain, and it is one pass over
     * every head in the program rather than three, so a `data`, an `alias` and a `class` head are
     * read against one namespace instead of three.
     *
     * The whole position rather than a closure, for `PendingDefault`'s reason: reading it later
     * means restoring the module and file a name is looked up from.
     */
    struct PendingHead {
        DeclPtr<Module> module;
        U16 file = 0;
        DeclPtr<GenContext> gen;
        ast::ParseList<ast::GenParam> parameters;
    };

    Array<PendingHead> pendingHeads;

    // The phases, in the order the dependencies between them force - `declare` says what each one
    // has to be able to assume about the ones before it.
    void declareRecordFields(ast::Module& file);

    // Every field default, once every record's fields and constructors are in - `PendingDefault`.
    void resolveFieldDefaults();
    void declareClasses(ast::Module& file);
    void declareClassMembers(ast::Module& file);
    void declare(ast::Module& file);

    void declareGlobals(ast::Decl& declaration);
    void declareFields(ast::Decl& declaration, Size& next);

    /*
     * The parameters of one generic head - the one reading of them, for all three declarations that
     * have one.
     *
     * Binds each name into `gen`, closes it, and queues the rest on `pendingHeads`. `owner` names
     * the declaration and is what a duplicate parameter is reported against.
     */
    void declareHead(ast::ParseList<ast::GenParam> parameters, DeclPtr<GenContext> gen,
                     StringId owner);

    // Every head's annotations and defaults, once every type in the program is a name - `PendingHead`.
    void resolveHeads();

    // `data Boxed(a = I32)` - the head's defaults, read once every parameter of it is bound.
    void declareDefaults(ast::ParseList<ast::GenParam> parameters, DeclPtr<GenContext> gen);

    // `data (Eq(a)) Set(a)` - the head's own constraints, which every application has to prove.
    void declareHeadConstraints(ast::Decl& declaration, RecordType& record);

    /*
     * A file's top-level declarations, minus the ones this target has not got - the range every
     * declaration pass walks.
     *
     * **Every pass goes through here**, and that is the point rather than a convenience. Two things
     * depend on all of them seeing the same list. The first is the language's: `@platform` exists for
     * alternative implementations of one name, so a declaration excluded from one pass and read by
     * another would be half-present - a name with no body, or a body with no name. The second is this
     * stage's own: the passes are joined by *position*, `writtenFunctions` against `nextFunctionBody`
     * and `writtenGlobals` against `nextGlobalInit`, so a walk that filtered differently from the
     * walk that filled the array would silently attach each body to the wrong signature. Neither is
     * a mistake a reviewer would catch in a diff of one pass.
     *
     * A **range** rather than a callback, so that a pass stays an ordinary `for` and keeps its
     * `continue`. A filter written as a lambda would have made every one of those a `return`, which
     * is the same statement spelled a way that stops meaning the same thing the moment a loop is
     * nested inside the body.
     *
     * Only *top-level* declarations. A class's members and an instance's are reached through their
     * own declaration, which was selected here already, and the spec's granularity for an alternative
     * implementation is the declaration rather than a member of one.
     */
    struct SelectedDecls {
        struct Iterator {
            Resolver* resolver;
            ast::Decl* at;
            ast::Decl* last;

            // Advanced on construction and after every step, so that `begin()` is already at a
            // declaration this compilation has - an iterator that skipped only on `++` would hand
            // out the first one whether or not the target wanted it.
            void settle() {
                while(at != last && !platformEnabled(resolver->context, resolver->parse, *at)) at++;
            }

            ast::Decl& operator * () const { return *at; }
            Iterator& operator ++ () { at++; settle(); return *this; }
            bool operator != (const Iterator& other) const { return at != other.at; }
        };

        Resolver* resolver;
        ast::Decl* first;
        ast::Decl* last;

        Iterator begin() const { Iterator it { resolver, first, last }; it.settle(); return it; }
        Iterator end() const { return Iterator { resolver, last, last }; }
    };

    SelectedDecls eachDeclaration(ast::Module& file) {
        auto first = file.decls.pointer(parse);
        return SelectedDecls { this, first, first + file.decls.size() };
    }

    // Every `@platform` spelling in a file, judged once and ahead of every pass - see the definition.
    void checkPlatform(ast::Module& file);

    // `pinned` says the caller is a declaration form that reads `@layout` itself - see
    // `readLayoutAttribute`, whose answer is not a flag and so cannot be a destination here.
    void readAttributes(ast::Decl& declaration, bool* explicitDrop, bool* local, StringView on,
                        bool pinned = false);

    // `@layout(c)` on a `data` - `RecordType::pin`, and `resolve_decl.cpp` for what the pin means.
    TypeLayout readLayoutAttribute(ast::Decl& declaration);

    // `@cold` and `@hot` - how often a function runs, which is not a statement about inlining.
    void readFrequencyAttributes(ast::Decl& declaration, bool& cold, bool& hot);

    // `@inline(always)`, or false for a bare `@inline` or none - resolve_decl.cpp.
    void readInlineAttribute(ast::Decl& declaration, bool& always, bool& hint, bool& never,
                             bool& legacySse);

    // `@convention(clobber)` on a signature - `Function::convention`, and resolve_decl.cpp for which
    // of the six names a program may write and why the other three are refused.
    void readConventionAttribute(ast::Decl& declaration, Function& function);

    // Whether a body may be grafted where it is called - resolve_stmt.cpp, beside `resolveBody`.
    void checkAlwaysInline(Function& function, LocationId source);

    // Whether a flag attribute was written without arguments, reporting where it was not - see the
    // definition, and `readValueAttribute` for the one attribute this is not asked about.
    bool takesNoArguments(ast::Attribute& attribute, DeclAttribute which);

    // `@specialize` / `@generic` on a signature - see the definition.
    GenForm readGenForm(ast::Decl& declaration);

    // `@value(n)` on a constructor, and nothing where none was written - see the definition, where
    // the difference between "absent" and "pinned to zero" is what the answer's shape is for.
    Maybe<I64> readValueAttribute(ast::Con& constructor);

    /*
     * resolve_sig.cpp - a written occurrence, and the signature it is a position of.
     */

    /*
     * A written type in a position that has a contract - Analysis-Borrows-Revised.md §2.1.
     *
     * The one place the source spellings become an `Occurrence`, for all three positions and both
     * sigils. `&x: T`, `x: &T`, `x: 'T`, `x: a'T`, `&x: a'T` and `x: &a'T` are six spellings of
     * four contracts, and every one of them is decided here - a second reading anywhere else is a
     * second answer to "what did the author ask for", which is §1.2's failure written in the
     * front end instead of the IR.
     *
     * `slots` is the enclosing ownership schema scope. A parameter and a field *introduce* the slot
     * they name; a result only uses one, which is what makes an unpublished origin an error.
     */
    bool resolveOccurrence(ast::Type& written, ast::BindType bind, Position position,
                           SlotScope& slots, Occurrence& into, bool ownerSubject = false);

    /*
     * Elision rule 1 at a result - Analysis-Borrows.md §4.8, and the `lib/` count behind it.
     *
     * A result whose *type* carries an interior position nobody labelled takes the signature's one
     * anonymous group, exactly as an unlabelled `'` at the root does. `-> Aligned(a)` over
     * `data Aligned(a) {value: 'a, ...}` is the shape every named formatter in `lib/Core/Format`
     * hands back, and what it means is the only thing it could mean: the record refers to the one
     * thing this signature borrowed. Writing it out would be `-> Aligned(v')` at forty sites, which
     * is the annotation burden §4.8 measured the library to show it does not have.
     *
     * Where the scope does not already have the group, `promoteLoanCandidates` decides whether one
     * of this signature's parameters publishes it - which is rules 5 and 6, and is what a result
     * over a borrowing record in a signature that wrote no tick anywhere needs.
     */
    bool elideResultOrigins(Occurrence& outgoing, SlotScope& slots);

    /*
     * Elision rules 5 and 6 - Analysis-Borrows.md §4.8, in the form that section's own supersession
     * block settled on, and the same rule the shipping stage's `settleLoanCandidates` applies.
     *
     * A result that carries a loan and names no published origin is not the instruction to go and
     * *pick* a parameter: rule 5's input was a count, and a count changes when something unrelated
     * is added to the signature - even with no edit at all, if a parameter's type is an alias that
     * gains a tick. A rule that picks turns that into silence, where the signature keeps compiling,
     * means something different, and every caller is re-checked against a contract nobody wrote.
     *
     * So **every** candidate joins the one anonymous group instead, which is rule 1 and is what a
     * conservative group has always been: the coarse answer is never wrong about whose storage the
     * result refers to, and a label is what splits it where the coarseness rejects a program worth
     * accepting. Rule 6 needs no clause of its own under that - `self` is a candidate like any
     * other, and stating the `self` convention is still the spelling to prefer, because it is the
     * one a reader can see.
     *
     * A **candidate** is an unlabelled parameter that refers to the caller's storage rather than
     * borrowing a value it could equally have been handed a copy of: a unique (`&`) borrow, or a
     * shared borrow whose form is a *view* - `xs: [T]`, `s: String`. A plain `p: Pair` is not one,
     * which is what keeps `loans/Reject.Signature.yana`'s refusals refusals; an `Array(a)` owner
     * receiver and a raw pointer are not either, and §4.8 says why each is a type that cannot yet
     * carry a group rather than an argument for guessing.
     *
     * Answers the slot, or `SlotScope::kNone` where the signature has no candidate at all - which
     * leaves the caller's "no parameter publishes an origin for this to be in".
     */
    U16 promoteLoanCandidates(SlotScope& slots);

    /*
     * The signature whose parameters an elided result group may promote - `promoteLoanCandidates`.
     *
     * Set around the *result* of a signature and null everywhere else, because that is exactly the
     * position the rule is about: a parameter and a field publish a slot by writing one, and only a
     * result can name a group that nothing published yet. A field on the resolver rather than a
     * parameter threaded through `resolveOccurrence`, `resolveQualified` and `resolvePointee`,
     * since a pointee's tick asks the same question one constructor down and the thread would have
     * to reach every one of them. Saved and restored, because resolving a result type may declare
     * another signature and that one's result may not promote this one's parameters.
     */
    Function* elisionSubject = nullptr;

    /*
     * Whether the next signature declared is the derivation of a view - member zero of the class
     * claiming `viewed`, or an instance's implementation of it - whose first parameter is the
     * **owner** even where the type's shared occurrence would be the view.
     *
     * Set by `declareClassMembers` and `declareInstance` immediately before `declareSignature`, and
     * cleared by `declareArgs` once it has read it. A flag on the resolver rather than on the
     * function, because the function's link to its class is made *after* its signature is read,
     * and the signature is where the answer is needed.
     */
    bool ownerSubjectNext = false;

    /*
     * The root sigils of a written type, peeled - `&`, `'` and a label - and what is left under them.
     *
     * One reader for two positions. `resolveOccurrence` reads them at the root of a parameter, a
     * result or a field; `resolvePointee` reads them at a pointer's argument, where `%source'a` says
     * the same thing about the storage the pointer names. The two would otherwise be the same loop
     * written twice, and a rule added to one of them - the duplicate-label check is already one -
     * would be a form that means one thing at a root and another one constructor down.
     */
    struct WrittenSigils {
        // What the sigils left, which is the type the position is *of*. Never null.
        ast::Type* type = nullptr;

        // The origin label, or none for the anonymous group. Braced for `StringId`'s reason - see
        // its declaration, whose default constructor is trivial.
        StringId group {};

        // Whether an origin was named at all, by a tick or by a label. Not the same as `group`
        // being set: a bare `'T` is in the anonymous group and says so.
        bool tick = false;
    };

    // Peels them, folding `&` and a label into `access` and `group`. False where the type named two
    // origins, which is reported here because only this walk can see both.
    bool peelSigils(ast::Type& written, Access& access, bool sunk, WrittenSigils& into);

    /*
     * The contract a written tick asks for - the one reader of `WrittenSigils::group`.
     *
     * Two callers, and they had the same twelve lines each: `resolveOccurrence` at a root and
     * `resolvePointee` one constructor down, where §12.2's whole point is that an interior position
     * says what a root says. A rule stated twice is a rule that holds at a root and not at a pointee
     * the first time one of them is edited, which is what `peelSigils` above is already one function
     * for.
     *
     * `yield'` is the reason it is worth a function now rather than a shared block: it is a *third*
     * origin beside a published slot and the anonymous group, and the position it is legal in is
     * neither of theirs.
     */
    bool tickContract(const WrittenSigils& sigils, Position position, Access access, AccessForm form,
                      SlotScope* slots, LocationId source, Contract& into);

    /*
     * `%source'a` - a pointer whose **pointee** names an origin, as a schema slot.
     *
     * Analysis-Lens.md §12.3(a)'s structural reading: the tick goes on the type constructor's
     * argument, so this is a pointer to storage in `source` and `source'(%a)` is a pointer *read
     * through* a borrow. The interior position is `PathStep::Pointee`, which is the whole of what
     * §12.2 asked for - a `Contract` at a path, where a pointer has no field index to name one.
     */
    bool resolvePointee(ast::Type& written, Position position, SlotScope* slots,
                        DeclPtr<Type>& logical, DeclPtr<OwnershipSchema>& shape);

    bool resolveQualified(ast::Type& written, Position position, SlotScope& slots,
                          DeclPtr<Type>& logical, DeclPtr<OwnershipSchema>& shape);

    /*
     * A written type application - `Boxed(I32)`, `Reader(a')`, and the two together.
     *
     * One function for both kinds of argument, because they are written in one list and the language
     * says so: Analysis-Borrows-Revised.md §2.2's loan argument "does not occupy a parameter position
     * or count towards arity", so `Store(kept', a)` names one origin *and* one type and neither can
     * be read without stepping over the other. Two functions would have had to agree on how to skip
     * each other's arguments, which is a rule that gets written twice and changed once.
     *
     * `slots` is null where the position has no ownership schema scope - a type nested inside
     * another, where there is nothing for a loan argument to be numbered in. A loan argument there
     * is reported rather than dropped.
     */
    bool resolveApplication(ast::Type& written, Position position, SlotScope* slots,
                            DeclPtr<Type>& logical, DeclPtr<OwnershipSchema>& shape);

    /*
     * One type argument of an application, with whatever was written on it -
     * Analysis-Borrows-Revised.md §2.4's structural qualification.
     *
     * `Maybe('v)` supplies the *type* `v` to the head - it binds `a := v`, selects `v`'s instances
     * and interns as `Maybe(v)` - and says separately that the positions holding it are somebody
     * else's. So what comes back is an `Occurrence`, and `resolveApplication` spends the two halves
     * in two places: the type into the argument list, the contract onto the paths
     * `bindArgumentPositions` finds.
     */
    bool resolveArgument(ast::Type& written, Position position, SlotScope* slots,
                         DeclPtr<GenContext> head, Size index, Occurrence& into);

    /*
     * Every position inside a declaration that holds one of its parameters, as interior slots.
     *
     * The walk `Maybe('v)` needs and the one place a qualifier is turned into paths. `path` and
     * `walked` are the recursion's state, passed in so that one buffer serves the whole descent: a
     * path is built as it goes and a record already on the stack is a type whose positions are not a
     * finite list.
     */
    bool bindArgumentPositions(DeclPtr<RecordType> subject, DeclPtr<Type> variable,
                               const Occurrence& argument, LocationId source,
                               SmallArray<U16, 4>& path,
                               SmallArray<DeclPtr<RecordType>, 8>& walked,
                               SmallArray<ExternalSlot, 8>& into);

    /*
     * The argument's own interior positions, moved under the field that holds it.
     *
     * `Maybe(%s'U8)` is two readers meeting: the pointee's qualification is `resolvePointee`'s
     * answer and the field holding the pointer is this walk's, so what composes them is a prefix.
     * Separate from `bindArgumentPositions` because it is the one step that reads the *argument's*
     * shape rather than the declaration's fields, and it is where the second field step is refused.
     */
    bool prefixArgumentShape(const Occurrence& argument, LocationId source,
                             SmallArray<U16, 4>& path, SmallArray<ExternalSlot, 8>& into);

    /*
     * A type application whose head is one of the constructors the compiler creates rather than a
     * declaration - `compiler/intrinsic/constructor.def`, and `resolve_sig.cpp` for what that
     * changes.
     *
     * Reached only where the ordinary lookup found nothing, so a program declaring its own `Vec`
     * shadows this the way it shadows any other name.
     */
    DeclPtr<Type> resolveConstructedType(ast::Type& written,
                                         const intrinsic::ConstructorDef& constructor);

    // The `n` of `Vec(a, n)` - a written number in `lanes`, or a const parameter in `count`, which
    // is `resolveArrayCount`'s pair of answers asked of a type argument.
    bool resolveLaneCount(ast::Type& written, DeclPtr<Type>& count, U64& lanes);

    /*
     * `inherited`, where there is one, is the signature this declaration is *supposed* to have -
     * a class's declaration of the member, substituted at the instance's head - and it is what a
     * position with no written type takes.
     *
     * `fn size(value) = value.value` inside an instance is the shape that needs it: the class
     * already said what `size` takes and returns, so writing it again is repetition the author can
     * get wrong, and the check that the two agree would be comparing a signature against itself.
     * Null everywhere else, which is every declaration whose types nothing else already decided.
     */
    bool declareArgs(Function& function, ast::Decl& declaration, SlotScope& slots,
                     Function* inherited, const Substitution& inheritedWith);

    // `= expr` at the declaration - the constant, and the five positions that may not carry one.
    // See resolve_sig.cpp.
    bool declareArgDefault(const ast::Arg& argument, const Occurrence& occurrence, bool deferred,
                           bool implementing, Constant& into);

    // `@lazy` at the declaration - the occurrence rewritten into the thunk the callee holds, and the
    // three shapes the marker cannot describe. See resolve_sig.cpp.
    bool declareDeferred(Function& function, const ast::Arg& argument, Occurrence& occurrence);

    /*
     * `@caller` at the declaration, and the constant a call that left the position out passes -
     * Design-Test.md §11.1's F2, both in resolve_caller.cpp.
     *
     * The pair is one feature and is kept in one file for that reason: what a marker may say and
     * what the compiler is able to put there are the same question asked at the two ends, and the
     * vocabulary of location fields is a table neither end may hold on its own.
     */
    bool declareCallerFill(Function& function, Arg& declared, const ast::Arg& argument);
    Node* callerFillValue(Builder& builder, const Arg& parameter, const ArgMap& map,
                          LocationId source);

    // The location record itself, and whether a type is one this compiler can fill - both read by
    // the synthesized test entry, whose `Site` is the library's `Test.Site`. See resolve_caller.cpp.
    Node* siteValue(Builder& builder, DeclPtr<Type> type, LocationId at, StringId containing);
    bool isCallerSiteRecord(DeclPtr<Type> type);

    // The drain - see `PendingCallerText`. Beside `checkTextDefaults`, which waits on the same role.
    void checkCallerTexts();

    // The `yield` form's continuation parameter and its free result variable - see the comment in
    // resolve_sig.cpp, which is the whole of what makes a lens an ordinary function. `inherited` is
    // the class's declaration of an instance member, whose protocol is taken rather than rebuilt.
    bool declareContinuation(Function& function, Function* inherited,
                             const Substitution& inheritedWith, LocationId source);
    DeclPtr<RecordType> declareOutcome(Function& function, DeclPtr<Type> failure, LocationId source);

    /*
     * One signature - a `fn`, a class member, an instance member.
     *
     * `classContext` is the generic context to read it in, and null for the ordinary case where the
     * signature binds its own. A class member's is the *class's*: every member of `Describe(a)` is
     * generic in one `a`, and a context per member would be one variable per member that nothing
     * could bind together.
     */
    Function* declareSignature(ast::Decl& declaration, StringId name,
                               DeclPtr<GenContext> classContext = nullptr,
                               Function* inherited = nullptr,
                               Substitution inheritedWith = Substitution {});

    void declareFunction(ast::Decl& declaration);

    /*
     * resolve_class.cpp - the things that are selected.
     */

    /*
     * `class Describe(a)` - the head and its parameters, before any member is read.
     *
     * A shell pass beside `declareTypes`, and for that pass's reason: a signature may name a class
     * in a constraint and an `instance` names one in its head, so every class has to exist before
     * either is read. What it does *not* do is read the members, because a member's signature is an
     * ordinary one and may mention any type in the program.
     */
    void declareClass(ast::Decl& declaration);

    // The members, once every class and every record is declared. A member is a generic function
    // over the class's own context, which is what makes a class call and a generic call one
    // mechanism - see `Function::klass`.
    void declareClassMembers(ast::Decl& declaration, Size& next);

    void declareInstance(ast::Decl& declaration);

    // Every written instance's superclasses, after every instance in the program has been declared -
    // see the definition for why it cannot be a line in `declareInstance`.
    void checkInstanceSuperclasses();

    /*
     * Which member of a class a written `fn` is - by name **and arity**, which is the key a call
     * selects by (`Program::addFunctionName`).
     *
     * The one place that pairing is written, and it has three readers: the instance's declaration
     * walk, the instance's body walk, and the class's own default bodies. Three copies of it is how
     * `class Num(a): fn -(x), fn -(x, y)` came to have both instance members resolved against the
     * first declaration - the bodies were paired by name, so the two-argument one was given the
     * one-argument signature's scope and its second parameter was a name nothing bound.
     *
     * `members.size()` where the class declares no such member, which is what every caller reports
     * against its own position.
     */
    Size classMemberIndex(Class& klass, StringId name, U32 arity, bool* named = nullptr);

    // `Show(a)` in a signature, a class head or an instance head - `resolve_class.cpp` says why one
    // reader serves all three.
    bool requireClassConstraint(ast::Constraint constraint, StringView elsewhere);

    // `a.field: b` and `f: (a) -> b` - the two requirement forms a signature carries in its own
    // environment rather than discharging where it is written. Both read in `gen`'s scope, and both
    // may introduce the variable they are about, on `readClassRequirement`'s terms.
    bool readFieldRequirement(ast::Constraint constraint, DeclPtr<GenContext> gen,
                              GenProperty& into);
    bool readCallableRequirement(ast::Constraint constraint, DeclPtr<GenContext> gen,
                                 GenCallable& into);
    bool readClassRequirement(ast::Constraint constraint, DeclPtr<GenContext> gen,
                              bool allowConcrete, GenRequirement& into);

    // The orphan rule - see the definition. Reports rather than answering, since there is nothing a
    // caller does differently about a badly placed instance than about a well placed one.
    void checkOrphanRule(Class& declared, const SmallArray<DeclPtr<Type>, 4>& arguments,
                         LocationId source);

    // One written instance member against the class's declaration of it, at this head. Reports the
    // signature the author would have to write.
    bool checkMemberSignature(Function& written, Function& expected, const Substitution& with,
                              LocationId source);
    String signatureText(Function& function, const Substitution& with);

    // A class by name, through the ordinary module traversal. Reports where there is none, because
    // both callers - an instance head and a constraint - want the same message.
    DeclPtr<Class> lookupClass(StringId name, LocationId source);

    /*
     * Whether every class's defaults bottom out - see the definition.
     *
     * A pass of its own after the bodies, because the question is about what a default's *body*
     * calls and there is no body until then. One walk over `program.classes` rather than a check
     * inside `declareClassMembers`, which runs before any of them exists.
     */
    void checkClassDefaults();

    /*
     * resolve_expr.cpp - the things that answer.
     */

    /*
     * A body expression, in a position that wants a value.
     *
     * `hint` is what the position asks for and is an input to inference rather than a check after
     * it: a literal has no width of its own. `wanted` is what the position asks the *value* for -
     * see `Request`, which is where the two spellings of that come from - and it reaches one place,
     * which is the read of a name: whether a bare `x` in this position is a copy, a loan or a move
     * is a question about the position and the place together, and `readMode` is where the pair is
     * answered.
     *
     * Null is returned when there is no value here, which is either an error already reported or
     * control having left - `Builder::isTerminated` is what tells a caller which.
     */
    Node* resolveValue(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, Request wanted);

    /*
     * The read of a place, in a position that wants the value a given way.
     *
     * `readMode`'s answer plus the one diagnostic it cannot give. A place this function does not own
     * has nothing to hand over, so `readMode` answers `BorrowShared` for it whatever the position
     * asked - which is the right *mode*, and silently wrong as a program when the position was a
     * sink: the value then fills a `->` parameter that consumes it, and the frame that really owns
     * it releases it again. `ownership/Reject.yana` is the fixture that found that.
     *
     * Here rather than in `readMode` because reporting needs a `Context` and a location, and here
     * rather than at each read site because there are two of them and a rule stated twice is a rule
     * that will be changed once.
     */
    Node* readPlace(Builder& builder, LocationId source, Place& place, Request wanted);

    /*
     * The place an expression names, or null where it names none.
     *
     * A name and a projection of one, and nothing else. This is what an assignment writes through,
     * what a field access projects and what a read of a name reads; separating it from
     * `resolveValue` is what stops "the place `x`" and "the value in `x`" from being one function
     * with a flag, which is the conflation the whole representation is organized against.
     *
     * `complain` is whether an unknown name is reported here. The target of a projection is asked
     * whether it is a place before it is resolved as a value, so it asks with `complain` false and
     * the value walk reports once - which is what stops `unknown.x` from producing two messages.
     *
     * `access` is what the position reaching the place will do with it, and it matters for one
     * kind of place: a subscript of anything but a fixed array is a call to `Index.get` or
     * `Index.getMut`, and which of the two is decided by the position - a read takes `get`, a
     * write or a `&` argument takes `getMut`. Everywhere else a place is lazy and the access is
     * judged where it is used.
     */
    Place* resolvePlace(Builder& builder, ast::Expr& expr, bool complain,
                        Access access = Access::Shared);

    // `@local data` - whether this module may reach inside the record. See resolve_expr.cpp.
    bool reachesInside(RecordType& record, LocationId source, StringView what);

    /*
     * The one walk over a record's fields, and the two questions asked of it - see the definitions.
     *
     * `fieldNamed` reports nothing and answers `kNoField`; `fieldOf` is the projection, which turns
     * a no into a message; `hasMemberNamed` is `resolveDotCall`'s choice between the two forms.
     * Keeping them one walk is what stops a field appearing to a projection and not to the dot-call
     * that has to choose against it.
     */
    static constexpr U16 kNoField = ~U16(0);
    U16 fieldNamed(const Occurrence& occurrence, StringId name, DeclPtr<Type>& record);
    bool hasMemberNamed(const Occurrence& occurrence, StringId name);
    Place* fieldOf(Builder& builder, Place& target, ast::Expr& selector, LocationId targetSource);

    // The same projection reached by a **name** rather than by a selector expression - a record
    // update's path, whose segments the parser records as names. The expression form is a wrapper
    // that reads the name out of it, so there is one walk and one set of messages.
    Place* fieldNamedOf(Builder& builder, Place& target, StringId name, LocationId nameSource,
                        LocationId targetSource);

    Node* resolveField(Builder& builder, ast::Expr& expr, Request wanted);
    /*
     * A record or constructor literal - and the one expression that *fills* interior positions, so
     * the one that reads `Request::shape`. A call, a name and a merge carry a shape of their own up
     * and are checked against the position's by whoever holds both.
     */
    Node* resolveConstruction(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint,
                              Request wanted);

    /*
     * What a projection projects *from* - the place the container or record is in.
     *
     * Three walks need it and it is one question: `a.x`, `a[i]` and `a[i..j]` all name part of
     * something, and part of something is a projection of the storage that something is in. Where
     * the target is already a place that place is it, and where it is not - `f().x`, `f()[0]` - the
     * value is written into a local and the projection is of the local. That temporary is an
     * ordinary owner with an ordinary drop, which is what makes `f().x` and `let t = f(); t.x` the
     * same program rather than two.
     *
     * Null having reported, or null having reported nothing where the target was not a place and not
     * a value either - the caller's own `complain` decides what that means, because what is missing
     * differs: a field of a non-record and an index into a non-container are different sentences.
     */
    Place* projectionTarget(Builder& builder, ast::Expr& target, Access access = Access::Shared);

    // The pointee of a raw address, as a place, from the address value and from a place holding one
    // - see the definitions, where the writability rule is argued.
    Place* pointeeAt(Builder& builder, Node& address, LocationId source, bool complain);
    Place* pointeeThrough(Builder& builder, Place& target, LocationId source);

    // `[1, 2, 3]` - a fixed array, whose length and element come from the position. See the
    // definition, which is where the argument for taking neither from the literal is.
    // `t.0` - a field named by where it is. See the definition, where "anonymous fields only" is
    // argued.
    Place* positionalField(Builder& builder, Place& target, ast::Expr& written);

    Maybe<ArrayLiteralShape> arrayLiteralShape(ast::Expr& expr, DeclPtr<Type> hint, Size written);
    Node* resolveArrayLiteral(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, Request wanted,
                              U64 capacity = 0);

    Node* resolveMapLiteral(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint);

    // The allocator's refusal, tested at a literal - see the definition beside `buildGrowableArray`.
    bool checkSlots(Builder& builder, Node& slots, U64 room, LocationId source);

    // The container half of a growable literal, over values already in hand - see the definition.
    // Its second caller is the synthesized test entry, which has cases and no expression.
    Node* buildGrowableArray(Builder& builder, DeclPtr<Type> element,
                             SmallArray<Node*, 8>& components, LocationId source,
                             U64 capacity = 0);

    // The half of it that builds a growable `[a]` - see the definition, where the two library roles
    // it is made of are argued.
    Node* resolveGrowableLiteral(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, U64 capacity);

    // `{4, 5}` - the anonymous record, whose type is what its elements turned out to be. See the
    // definition, where the hint's part in that is argued.
    Node* resolveTupleLiteral(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint);

    /*
     * `{p | y: 3}` and `{->p | y: 3}` - the record update, which is storage and a run of writes.
     *
     * Not a construction: what it builds it builds from a value rather than from a field list, so
     * there is no aggregate to hand a component per field. See the definition, where the one copy
     * every path writes into is argued.
     */
    Node* resolveUpdate(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint);

    // The source of one, into the copy's storage - see the definition, where the copy and the
    // relocation are argued as one question.
    bool initializeUpdate(Builder& builder, Place& root, Place* from, Node* produced, bool sink,
                          LocationId source);

    /*
     * `"a{x}b"` - the string interpolation, which is one allocation and a run of appends.
     *
     * The whole desugaring is in one function and produces ordinary calls: `Show`'s two members and
     * the three functions `known.def` names, all reached through their roles. See the definition,
     * where the evaluation order and what a library without a formatter is told are argued.
     */
    Node* resolveFormat(Builder& builder, ast::Expr& expr);

    // A container reaching a `[T]` position - the one implicit adaptation. See the definition.
    Node* adaptToView(Builder& builder, Node& value, const Occurrence& position, LocationId at);

    // `xs[a..b]` - part of a container, as a window. See the definition.
    Node* resolveSubwindow(Builder& builder, ast::Expr& expr, Request wanted);

    // `xs[i]` - the element's place. See the definition for why it is a place and not a call.
    Place* resolveSubscript(Builder& builder, ast::Expr& expr, bool complain, Access access);

    /*
     * `xs[i]` in two halves - the container and the index, and then the element.
     *
     * Split because an assignment needs them apart: `xs[i] = xs[i] + 1` on a window reads through
     * `get` and writes through `getMut`, and the unique loan the second takes must not be live
     * while the first runs. So `resolveAssign` resolves the container and the index, then the
     * value, then the accessor; every other position runs the two halves back to back.
     */
    struct Subscript {
        Place* container = nullptr;
        Node* index = nullptr;
        DeclPtr<Type> element = nullptr;
        bool fixed = false;
    };

    bool resolveSubscriptTarget(Builder& builder, ast::Expr& expr, bool complain, Access access,
                                Subscript& into);
    Place* fieldStep(Builder& builder, Place& target, ast::Expr& expr);
    DeclPtr<Type> fieldPathType(DeclPtr<Type> from, ast::Expr* const* steps, Size count);
    bool resolveIndexInsert(Builder& builder, const Subscript& subscript, Node& value,
                            LocationId source);
    void checkFixedBound(Builder& builder, const Subscript& subscript, LocationId source);
    Place* elementAccess(Builder& builder, const Subscript& subscript, Access access,
                         LocationId source);

    /*
     * "The library declares nothing with this role", from the side of a compiler that wanted one.
     *
     * Every `Presence::Optional` row in `known.def` can be absent, and what a compiler holding one
     * of them does about that is the same sentence every time - the role it looked for, and the
     * thing it was in the middle of building. Written once so that the two halves stay a role name
     * the table supplies and a phrase the caller supplies, rather than a sentence each site spells
     * out and one of them spells differently.
     */
    void reportMissingRole(intrinsic::Known role, StringView needs, LocationId source);
    void reportMissingRole(intrinsic::Op op, StringView needs, LocationId source);

    // A call to a function the compiler inserts - `stringLiteral`, `slice` - through its role, with
    // the arguments prepared exactly as a written call's are.
    Node* callKnown(Builder& builder, intrinsic::Known role, Node* const* operands, Size count,
                    LocationId source, DeclPtr<Type> hint = nullptr);
    /*
     * `pending` is one entry per operand, non-null where the operand is a written literal this call
     * has yet to build - see `undecidedLiteral`. Null altogether at every caller but the overloaded
     * operator's, which is the only one that leaves an argument for the callee to type.
     *
     * `carried` is that same caller's own trial - `chooseCandidate`'s, kept rather than solved again
     * - non-null only there and for the same reason `finishOverloadedCall` takes one: selection
     * already bound and widened this signature to choose the candidate, and a fresh `TypeArguments`
     * here would ask the same questions again and could answer them differently. Every other caller
     * leaves it null and gets exactly what it always did - its own fresh, un-unified solve - because
     * there is no earlier trial there for a second one to disagree with.
     */
    Node* callFunction(Builder& builder, Function& callee, Node* const* operands, Size count,
                       LocationId source, DeclPtr<Type> hint = nullptr,
                       ast::Expr* const* pending = nullptr, TypeArguments* carried = nullptr);

    // What the resolved arguments bind, taken before any position is prepared - see the definition,
    // where the one case that needs it is argued.
    void bindResolvedArguments(Function& callee, const ArgMap& map, Node* const* values, Size count,
                               TypeArguments& into);

    /*
     * `hint` is what the position this call sits in expects, and it is what decides a type argument
     * no argument mentions - `fn empty() -> Maybe(a)`, whose `a` is settled by what the result is
     * being bound to.
     *
     * An *input* to inference and never a check: the check is `checkType` on the value that comes
     * back, which names both sides. Null wherever the position expects nothing, which is every
     * statement position and every argument whose declared type is still open.
     */
    Node* resolveCall(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint);

    /*
     * One resolved argument against the parameter it fills - the per-position half of the one
     * argument loop, and what an operator's operands go through too.
     *
     * Binds the callee's variables from the value, adapts a container to a window position, checks
     * the type and the carried origins, and gives an owner a place where the position only borrows.
     * Answers the node the call is handed, or null after reporting.
     */
    Node* prepareArgument(Builder& builder, Node& value, Arg& parameter,
                          TypeArguments& typeArguments, LocationId source);

    /*
     * The end of every call - what a written call, an operator, a condition's truth test, a literal
     * and a pattern's comparison all reach once their arguments are prepared.
     *
     * Binds the result position's hint, fills what the callee's dependencies decide, reports a
     * variable nothing decided, discharges the requirements, and then builds one of three things:
     * the node a compiler-provided instance member computes, where the call is concrete and the
     * selected instance is one (`provided.def`); the node a `Node`-form intrinsic emits; or the
     * call. That last choice is the only place a call and an expansion part, which is what keeps a
     * `+` at an `I32` and a `+` at a `Money` one path.
     */
    Node* finishCall(Builder& builder, Function& callee, SmallArray<Node*, 8>& arguments,
                     TypeArguments& typeArguments, DeclPtr<Type> hint, LocationId source);

    // The provided-member half of `finishCall`: the member's node where the call is concrete and
    // the instance is the compiler's, null where it is anything else. See the definition.
    Node* expandProvidedCall(Builder& builder, Function& callee, SmallArray<Node*, 8>& arguments,
                             TypeArguments& typeArguments, LocationId source);

    // The bulk-operation half of `finishCall`: the call of `lib/Core/Reduce.yana`'s *vector* body
    // where this element has a vector on this target, null where it does not - and then the
    // declaration's own body, which is the element walk, is what the call reaches. See the
    // definition, where the choice and where the element comes from are argued.
    Node* selectBulkBody(Builder& builder, Function& callee, SmallArray<Node*, 8>& arguments,
                         TypeArguments& typeArguments, LocationId source);

    /*
     * A call of one class member the compiler reaches without a name - `Truth.truthy` for a
     * condition, `Eq.==` for a literal pattern, `Ord`'s four and `Num`'s two for a counted `for`,
     * `Widen.widen` for a conversion. `known.def` is where the member positions are stated. The
     * operands are prepared and the call finished exactly as a written one's are.
     *
     * `hint` is what the *result* position decides, and it is what makes a conversion selectable at
     * all: nothing about `widen(x)`'s argument says which rung was meant, so `Widen`'s second
     * parameter is bound from the type being converted to. Null for every member whose result its
     * arguments already decide.
     */
    Node* applyMember(Builder& builder, intrinsic::Known klass, U16 member, Node* const* operands,
                      Size count, LocationId source, DeclPtr<Type> hint = nullptr, DeclPtr<Type> subject = nullptr);

    // `truthy(value)` - what a condition is, `doc/spec/expressions.md`. At a `Bool` the provided
    // instance is the identity and this answers the value it was given.
    Node* truthValue(Builder& builder, Node& value, LocationId source);

    /*
     * A literal in a value position - `doc/spec/core.md`'s "a literal is a call to `FromInt`".
     *
     * A constant where the instance at the position's type is the compiler's own, so `1 :: I32` is
     * `const 1` and nothing stands above it; a call of `fromInt` on the written number where it is
     * a declared instance or a variable of the enclosing signature. `literalNode` is the same with
     * the number already in hand, for the counted `for`'s stride of one.
     */
    Node* literalValue(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool negative);

    // `"abc"` as a value - the three records spelled out. See the definition, in text.cpp.
    Constant stringConstant(StringId written, LocationId source);

    /*
     * The same, applied to every `Constant::Text` **inside** a value - `["a", "b"] :: [String *2]`.
     *
     * A text leaf is deferred for the reason a whole one is, and an aggregate holding one is the
     * same statement about a value with the leaf further in: `let names = ["a", "b", "c"]` is three
     * byte globals and three relocations, exactly as three separate `let`s would be. Unset where a
     * leaf could not be built, which is `stringConstant`'s own diagnostic already reported.
     */
    Constant textsResolved(const Constant& constant, LocationId source);

    DeclPtr<Global> stringBytesGlobal(StringId written, LocationId source);
    Node* textValue(Builder& builder, StringId written, LocationId source);

    // The type such a value has, which is the `string` record role and not a row of this stage.
    DeclPtr<Type> textType(LocationId source);

    // Any constant as a node - `Constant::Text` through `textValue`, everything else an immediate.
    Node* constantValue(Builder& builder, const Constant& constant, LocationId source);
    Node* literalNode(Builder& builder, const WrittenLiteral& written, DeclPtr<Type> type,
                      LocationId source);

    // The instance `FromInt` or `FromDecimal` selects at a concrete type, reported where there is
    // none. Which class is `role`.
    DeclPtr<Instance> literalInstance(intrinsic::Known role, DeclPtr<Type> type, LocationId source);

    /*
     * intrinsic/provide.cpp - the instances the compiler generates, `provided.def`.
     */
    void provideInstances();
    void provideInstance(intrinsic::Provided role, DeclPtr<Class> klass, const DeclPtr<Type>* head,
                         Size heads, DeclPtr<GenContext> gen = nullptr);
    DeclPtr<Function> provideMember(intrinsic::Provided role, U16 member, Class& declared,
                                    Function& expected, const DeclPtr<Type>* head, Size heads,
                                    DeclPtr<GenContext> gen);

    // The instances whose head is a *shape* rather than a type - `Eq(%a)` and the reinterpretation
    // rungs at an address. See the definition, where why they are not rows of `provided.def` is
    // argued.
    void providePointerInstances();

    // The instances whose head is a vector - `provide.cpp`, where the argument for their not being
    // rows of `provided.def` is the same one the pointer instances make.
    void provideVectorInstances();

    // One `Enum`, `Eq` and `Ord` per payload-free sum - see the definition. Its own pass because it
    // runs *after* the written instances rather than before them, which is what makes a declared one
    // win by being found rather than by colliding.
    void provideEnumInstances();
    void provideBodies();
    void provideBody(Function& function, const DeclPtr<Type>* head, Size heads);
    void compareBody(Builder& builder, Function& function, Local& lhs, Local& rhs);
    void enumFromValueBody(Builder& builder, Function& function, Local& number, DeclPtr<Type> head);
    void enumNameOfBody(Builder& builder, Function& function, Local& item, DeclPtr<Type> head);
    void enumFromNameBody(Builder& builder, Function& function, Local& name, DeclPtr<Type> head);
    void enumShowBody(Builder& builder, Function& function, Local& item, Local& sink);
    void enumShowBoundBody(Builder& builder, Function& function, DeclPtr<Type> head);

    // The `Just` and `Nothing` of whatever the class said a partial member answers - see the
    // definition, where reading them by name rather than by handle is argued.
    bool maybeConstructors(DeclPtr<Type> answer, LocationId source, StringView member, U16& just,
                           U16& nothing);

    // A named function in a value position - G7. The hint is what picks the arity, which is why it
    // is required rather than advisory here; see the note at the implementation.
    Node* resolveFunValue(Builder& builder, StringId name, DeclPtr<Type> hint, LocationId source);

    /*
     * resolve_lambda.cpp - a lambda, and the environment its captures live in.
     */

    // A `@lazy` parameter is forced at most once on any path - the walk over the finished body, and
    // the one place call-by-name is distinguished from call-by-need. See resolve_stmt.cpp.
    void checkForced(Builder& builder, Function& function);

    Node* resolveLambda(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint,
                        const Request& wanted);

    // The expression written at a `@lazy` position: a thunk, or the thunk this body was already
    // handed. See resolve_expr.cpp.
    Node* resolveDeferred(Builder& builder, ast::Expr& written, DeclPtr<Type> expected);

    // A `@lazy` argument - the nullary thunk the spec says the expression compiles to, which is a
    // lambda with no written parameter list.
    Node* resolveThunk(Builder& builder, ast::Expr& body, DeclPtr<Type> hint, LocationId source);

    // The thunk a call that omitted a `@lazy` *defaulted* position passes - one nullary function per
    // parameter, whose body returns the constant. See resolve_lambda.cpp.
    Node* defaultThunk(Builder& builder, Function& callee, Arg& parameter, LocationId source);

    // Which of the two the author wrote, and the only thing below the lift that depends on it: the
    // symbol a dump shows, and the refusals, which name the construct rather than what it becomes.
    enum class LiftKind: U8 { Lambda, Thunk };

    Node* liftFunction(Builder& builder, const ast::ParseList<ast::Arg>* args, ast::Expr& body,
                       bool implicitReturn, DeclPtr<Type> hint, LocationId source, LiftKind kind,
                       bool owningPosition);

    // The position a lambda that typed its own parameters stands at - see the definition, where the
    // one rule this stage has for a lambda at a position that states nothing is argued.
    DeclPtr<Type> writtenLambdaPosition(ast::FunExpr& written, LocationId source);

    void resolveLiftedBody(Function& lifted, ast::Expr& body, bool implicitReturn, bool thunk,
                           LocationId source);
    Node* buildEnvironment(Builder& builder, LambdaFrame& frame, LocationId source);

    // A name the innermost lambda's body said and its enclosing function bound, as the place the
    // lambda reads it through. Null where the name is not one, which is what lets `resolvePlace`
    // carry on to the globals and to its one report.
    Place* findCapture(Builder& builder, StringId name);

    // Whether a place is reached through the innermost lambda's environment - for the message at a
    // position that consumes, and for nothing else.
    bool isCapture(Builder& builder, Place& place);

    // A call whose callee is a value rather than a name - G7. The value is already resolved, because
    // the order it is read in is part of what the call means; see the note at `resolveCall`.
    Node* resolveValueCall(Builder& builder, ast::AppExpr& written, Node& target, LocationId source);
    // `leading` is a dot-call's receiver taking position 0 - `ArgSite::leading`, where the reason it
    // is a count rather than a written argument is.
    bool checkCallShape(ast::AppExpr& written, Function& callee, LocationId source, ArgMap& into,
                        bool report = true, U32 leading = 0,
                        LocationId leadingSource = kNullLocation);

    /*
     * Whether this declaration is one a call may reach at all - asked of the callee wherever a call
     * has settled on one, and nowhere else.
     *
     * Both of the answers are about the *declaration* rather than about the call, which is what
     * makes this one question with four askers: a written call, an overloaded call's winner, an
     * infix operator's callee and a prefix operator's. Every one of them can reach a lens, and a
     * lens reached by a call that gives it no continuation used to be found much later - as a
     * continuation type nothing decided, or as an argument count the builder asserted on.
     */
    bool checkCallable(Function& callee, LocationId source, bool traversal = false);

    /*
     * Which declared position each written argument fills - `doc/spec/expressions.md`'s Calls, and
     * the one place those rules are written.
     *
     * Shared by a call and a construction for `resolveArguments`' reason: positional then named, no
     * position twice, and a position the site left out taking its own default are one set of rules
     * over two lists of names, and two copies of them would agree until one was changed.
     */
    bool mapArguments(ast::ParseList<ast::TupArg> written, const ArgSite& site, ArgMap& into);

    /*
     * The mapping half of a construction - which written argument fills each field of one
     * constructor. Shared by the value form and the constant one; see the definition.
     */
    bool constructionArguments(ast::Expr& expr, RecordType& declared, const RecordCon& arm,
                               ArgMap& into);

    /*
     * One argument per declared parameter, resolved against it - the *one* argument loop.
     *
     * Shared by the ordinary call and the lens call, which is the point: every rule that depends on
     * a parameter's convention - what a bare name filling it does, what happens to an owner handed
     * to a position that only borrows - is written here and nowhere else. The shipping stage has
     * two of these loops and the rules it added to one of them are missing from the other.
     */
    bool resolveArguments(Builder& builder, ast::AppExpr& written, Function& callee,
                          const ArgMap& map, LocationId source,
                          SmallArray<Node*, 8>& into, TypeArguments& typeArguments,
                          DeclPtr<Type> hint = nullptr, Receiver receiver = Receiver());

    // Whether a dot-call's receiver may fill a `@lazy` first parameter - it may not, since deciding
    // that `x.f(y)` is a call at all needs the receiver's type, which needs it evaluated. Shared by
    // the single-callee path (`resolveArguments`) and the overloaded one (`selectOverload`), which
    // ask the same question of the position each has already settled is the receiver's.
    bool checkDotReceiverNotLazy(LocationId source, StringId name, bool lazy);

    // The positions a call left out, each built from the parameter's own constant - see the
    // definition. Both call paths end in it, which is why it is not inside either.
    bool fillDefaultArguments(Builder& builder, Function& callee, const ArgMap& map,
                              LocationId source, SmallArray<Node*, 8>& into);

    /*
     * The tail an overloaded call shares with an ordinary one: the arguments are resolved, a
     * candidate is chosen, and what is left is putting each value in the position the winner
     * declared for it and finishing the call.
     *
     * `values` is in **written** order, which is the order they were evaluated in; `into` inside is
     * in the callee's, which is the order they are handed over in. That the two differ is what a
     * named argument is.
     */
    /*
     * The winner's positions, from arguments a selection already resolved - the loop
     * `finishOverloadedCall` and a selected `for` loop's traversal share.
     *
     * `values` is in **written** order, which is the order they were evaluated in; `arguments` is in
     * the callee's, which is the order they are handed over in. That the two differ is what a named
     * argument is. The caller owns the `HeldList` over `arguments`, because what it does with them
     * afterwards is its own.
     */
    bool prepareResolvedArguments(Builder& builder, ast::AppExpr& written, Function& callee,
                                  const ArgMap& map, LocationId source,
                                  SmallArray<Node*, 8>& values, SmallArray<Node*, 8>& arguments,
                                  TypeArguments& typeArguments, Receiver receiver = Receiver());

    /*
     * R2 through R5 over a set the caller has narrowed - the first half of `resolveOverloadedCall`,
     * and the whole of what a `for` loop's traversal shares with a call.
     *
     * The arguments are resolved **into `into`**, once and in the caller's block, and the caller
     * owns the `HeldList` over them for the reason it owns one at all: what uses the values is what
     * comes after this, and a branch built there has to thread them.
     */
    bool selectOverload(Builder& builder, ast::AppExpr& written, const OverloadSet& set,
                        StringId name, LocationId nameSource, LocationId source, Receiver receiver,
                        CandidateKind kind, OverloadChoice& into);

    // `typeArguments` is the winner's own trial from `OverloadChoice::args` - see the definition,
    // where carrying it rather than solving again is the whole of the point.
    Node* finishOverloadedCall(Builder& builder, ast::AppExpr& written, Function& callee,
                               const ArgMap& map, SmallArray<Node*, 8>& values,
                               TypeArguments& typeArguments, DeclPtr<Type> hint, LocationId source,
                               Receiver receiver = Receiver());

    // What a call has to prove about the types it chose - see the definition, which is where the
    // two answers and the deliberately absent third are argued.
    bool dischargeRequirements(Function& callee, TypeArguments& typeArguments, LocationId source);

    // The widening a later argument decided, applied to the positions already prepared - see the
    // definition, and `TypeArguments::bind`, where the unification it settles is argued.
    bool widenPreparedArguments(Builder& builder, Function& callee, const ArgMap& map,
                                LocationId source, SmallArray<Node*, 8>& into,
                                TypeArguments& typeArguments);
    // `a.field: b` proved for one argument list - the property half, and the same two answers in the
    // same order as the class half beside it.
    bool dischargeProperty(GenProperty property, const Substitution& with, LocationId source);

    // `f: (a) -> b` proved for one argument list - the callable half, beside the other two.
    bool dischargeCallable(Function& callee, GenCallable callable, const Substitution& with,
                           LocationId source);

    /*
     * One class requirement proved. `report` is false for overload selection, which asks whether a
     * class candidate is *usable* at these types before it knows whether any other candidate is -
     * a candidate that cannot discharge its class is not a mistake, it is a candidate.
     */
    bool dischargeRequirement(GenRequirement requirement, const Substitution& with,
                              LocationId source, bool report = true);

    /*
     * `doc/spec/overloading.md`'s R1 through R5 - `resolve_overload.cpp`, where each is argued.
     *
     * Reached where a name and arity find **more than one** candidate; a set of one is the ordinary
     * call path, unchanged, and that is R5's own rule rather than an optimization - a sole plain
     * function's parameter types are pushed down into its arguments as expected types, and a set
     * with something to choose between has no parameter type to push until it has chosen.
     */
    Node* resolveOverloadedCall(Builder& builder, ast::AppExpr& written, const OverloadSet& set,
                                StringId name, LocationId nameSource, DeclPtr<Type> hint,
                                LocationId source, Receiver receiver = Receiver());

    /*
     * `x.f(y)` - the dot call, and the one place a field projection and a call of that name are
     * chosen between. See the definition, where the choice and what the receiver costs are argued.
     */
    Node* resolveDotCall(Builder& builder, ast::AppExpr& written, ast::FieldExpr& field,
                         DeclPtr<Type> hint, LocationId source);

    // R1's set for a dot-call - the receiver's own namespace decides the plain half. See the
    // definition, where the ordering of the two lookups is argued.
    void findDotOverloads(DeclPtr<Type> receiver, StringId name, U32 arity, LocationId source,
                          OverloadSet& into);

    /*
     * The same five rules at an **infix operator**, in the two halves precedence climbing needs.
     *
     * `operatorRequests` answers what the two operands are asked for, which `climb` spends before it
     * has either of them; `selectOperatorCall` chooses between the candidates once both are values
     * and builds the call. Two entry points rather than one because the right operand is a span of
     * the chain that only the climb can resolve, and it has to be resolved between them.
     *
     * `operands` is one or two, since a prefix operator asks the same question at one. `into` is
     * that many `Request`s. Both report for themselves.
     */
    bool operatorRequests(const OverloadSet& set, StringId name, Size operands, LocationId source,
                          Request* into);
    Node* selectOperatorCall(Builder& builder, const OverloadSet& set, StringId name,
                             LocationId source, Node* const* values, Size operands,
                             DeclPtr<Type> hint, ast::Expr* const* pending = nullptr,
                             const intrinsic::Known* literals = nullptr);

    /*
     * Whether a candidate's declared positions accept these argument types - R2's "every argument
     * binds its declared position", asked without building anything.
     *
     * `into` is left holding what the trial bound, so the selected candidate does not solve its own
     * signature a second time.
     */
    bool candidateAccepts(Function& callee, const ArgMap& map, const DeclPtr<Type>* argumentTypes,
                          Size count, LocationId source, TypeArguments& into,
                          const intrinsic::Known* literals = nullptr);

    // Whether this operand is a written number with no type of its own, and which conversion class
    // would build it - see the definition, where the rule for a literal in a multi-candidate set is.
    bool undecidedLiteral(ast::Expr& expr, intrinsic::Known& role);

    // Whether a value of one type fills a position of another - the accepting half of
    // `convertValue`, asked without reporting. See the definition.
    bool acceptsType(DeclPtr<Type> from, DeclPtr<Type> to, LocationId source);
    Node* resolvePrefix(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint);
    Node* resolveInfix(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, Request wanted);
    Node* resolveIf(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue,
                    Request wanted);

    // `if:` with a case list - the same construct with its arms written out, see the definition.
    Node* resolveMultiIf(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue,
                         Request wanted);

    /*
     * Patterns, `is` and `match` - `resolve_match.cpp`, where every one of these is argued.
     */
    // `ownedField` is `pivot`'s own answer to `PatternBinding::ownedField` - true where the caller
    // just projected an owned record field and false everywhere else, including the outermost call
    // for each pattern, where `pivot` is not a projection at all.
    bool resolvePattern(Builder& builder, ast::Pat& pat, Place& pivot,
                        SmallArray<PatternBinding, 4>& into, Block* onFail, Carry& carry,
                        bool ownedField = false);
    bool resolveConstructorPattern(Builder& builder, ast::Pat& pat, Place& pivot,
                                   SmallArray<PatternBinding, 4>& into, Block* onFail,
                                   Carry& carry);

    // `{a, b}` with no constructor in front of it - see the definition, where the one-constructor
    // rule is argued.
    bool resolveTuplePattern(Builder& builder, ast::Pat& pat, Place& pivot,
                             SmallArray<PatternBinding, 4>& into, Block* onFail, Carry& carry);

    // The same, against the elements of a pivot that was never built - see `decomposeMatch`.
    bool resolveDecomposed(Builder& builder, ast::Pat& pat, SmallArray<Place*, 8>& elements,
                           SmallArray<PatternBinding, 4>& into, Block* onFail, Carry& carry);
    void constructorFields(ast::Pat& pat, RecordType& record, const RecordCon& arm,
                           SmallArray<ast::Pat*, 4>& into, bool report);
    Node* patternBound(Builder& builder, ast::Pat& pat, DeclPtr<Type> type);
    bool comparePattern(Builder& builder, BinaryOp op, Place& pivot, ast::Pat& bound, Block* onFail,
                        Carry& carry, LocationId source);

    // `>0` - an operator section, which is the one pattern whose test is a written name rather than
    // a member the compiler picked. See resolve_match.cpp, where the two are told apart.
    bool sectionPattern(Builder& builder, ast::Pat& pat, Place& pivot, Block* onFail, Carry& carry);
    void failIfNot(Builder& builder, Node& condition, Block& onFail, Carry& carry,
                   LocationId source);
    // `from` is how much of the list is already in scope, which a conjunction needs: it binds
    // between its own spans, and the list accumulates across all of them.
    void bindPattern(Builder& builder, const SmallArray<PatternBinding, 4>& made, U32 from = 0);

    /*
     * A declaration's pattern against its initializer - `doc/spec/patterns.md`'s guard form.
     *
     * `let Just(v) = m | return 0`: the pattern may fail, the alternatives say what happens when it
     * does, and **every alternative has to leave the block**. That last rule is what lets the names
     * the pattern bound stay in scope for the rest of the block with no nesting level of their own -
     * there is exactly one way to reach the code after the declaration, and the pattern matched on
     * it.
     *
     * Leaves the builder on that one way. What the pattern bound comes back in `made` rather than in
     * scope, because the alternatives are resolved in between and the names are not theirs; the
     * caller binds them, which is also what decides how long they live - the rest of the block for a
     * flat declaration, and the `in` body for a scoped one.
     */
    bool resolveBinding(Builder& builder, const ast::VarDecl& binding, Place& pivot,
                        SmallArray<PatternBinding, 4>& made);

    // The tail `bindIrrefutable` and `resolveBinding`'s own zero-alternatives branch share - see the
    // definition. `diagnose` is called with no arguments where `covers` is false; each caller's own
    // wording is what it closes over.
    template<class Diagnose>
    bool resolveWithNoAlternative(Builder& builder, ast::Pat& pat, Place& pivot, bool covers,
                                  Carry& carry, SmallArray<PatternBinding, 4>& made,
                                  Diagnose&& diagnose);

    /*
     * The same pattern in a position that has **no alternative** - a `for` binding.
     *
     * `resolveBinding` above is the refutable driver: it builds the failure block the arms after `|`
     * are emitted in. A `for`'s `|` is the *iterator's* failure channel and not the pattern's, so
     * there are no arms here for a failing pattern to reach and the language's answer is that the
     * pattern may not fail. What is left is the irrefutable half of the one path - the space says
     * whether it can, and `resolvePattern` and `bindPattern` are the two calls a `let` makes.
     */
    bool bindIrrefutable(Builder& builder, ast::Pat& pat, Place& pivot, StringView what);

    // What a destructuring declaration matches against - the initializer as a place. Beside
    // `evaluatedPlace` rather than inside it, because what it adds is the declaration's own conventions,
    // which a `match` has not got.
    Place* destructuringPivot(Builder& builder, const ast::VarDecl& binding);

    // Whether this declaration binds a *pattern* rather than a name - the guard form, however it is
    // spelled. `let x = a | b` is one: an irrefutable pattern with alternatives is the bitwise-or
    // mistake, and `resolveBinding` is the one place that says so.
    bool destructures(const ast::VarDecl& binding) const {
        auto alternatives = binding.alts;
        if(alternatives.size()) return true;

        return binding.pat.kind != ast::Pat::Var && binding.pat.kind != ast::Pat::Any;
    }
    /*
     * The condition of an `if` or a `while`, and the names a pattern in it binds.
     *
     * `chain` is scratch the caller owns: a conjunction's later operands are resolved inside the
     * block the earlier ones proved, so the flattened chain has to outlive this call. It is unused
     * for every condition that is not one.
     */
    Condition resolveCondition(Builder& builder, ast::Expr& expr, Chain& chain);

    // One test, which is what a condition is when it is not a conjunction of them.
    Condition resolveTest(Builder& builder, ast::Expr& expr);

    // One `&&`-separated span of a flattened conjunction - a single operand, which may be a pattern,
    // or a run of tighter operators climbed into one `Bool`.
    Condition resolveSpan(Builder& builder, Chain& chain, Size& operandIndex, Size& operatorIndex);

    /*
     * The condition's tests, emitted where they belong, and the names they bind put in scope.
     *
     * The binding is here rather than at the caller because a conjunction binds *between* its own
     * tests: `m is Just(v) && v > 0` has to have `v` in scope before the second test is resolved.
     * So a caller records `bindings.size()` before this and restores it after the arm, and `into`
     * comes back holding every binding made - which is what a position with no arm reports about.
     */
    bool resolveTests(Builder& builder, const Condition& condition, Block* onFail, Carry& carry,
                      SmallArray<PatternBinding, 4>& into);

    // One condition's own test, with no conjunction after it.
    bool emitTest(Builder& builder, const Condition& condition, Block* onFail, Carry& carry,
                  SmallArray<PatternBinding, 4>& into);
    Place* evaluatedPlace(Builder& builder, ast::Expr& expr);
    Node* resolveIs(Builder& builder, ast::Expr& expr);
    Node* resolveMatch(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue,
                       Request wanted);

    /*
     * Whether this `match` states a *mapping* rather than a decision - inventory D3, and
     * `resolve/map.h` for the reasoning this stage inherits.
     *
     * Answers the node, or null meaning "the ordinary path handles this". **It reports nothing about
     * the match**, ever: every refusal is a fall-through to `armsValue`, which resolves the same
     * alternatives and says whatever there is to say about them. A second opinion about a bad match
     * would be a worse message about the same line, which is `recognizeConstructorMap`'s rule
     * inherited literally - and is why this runs after `PatternSpace` has issued its warnings.
     *
     * What it does **not** decide is which shape the mapping becomes. That is the target's question
     * and it is answered once, at lowering (`resolve_ssa/lower/map.cpp`). The shipping stage has to
     * ask it twice because its recognizer owes its caller an emission; here the node always exists.
     */
    Node* matchAsMap(Builder& builder, ast::Expr& expr, Place& pivot, Buffer<ast::Pat> patterns,
                     DeclPtr<Type> hint, Request wanted);

    // A plain `Con` pattern naming one constructor of `record`, before `mapConstructor` and
    // `matchCases` diverge on what it may do beyond that - see the definition.
    bool constructorPattern(ast::Pat& pattern, DeclPtr<RecordType> record, U32& index);

    // One pattern of a mapping, and which constructor it names - see the definition, where the list
    // of things a pattern may not do is the eligibility rule read from the other side.
    bool mapConstructor(ast::Pat& pattern, DeclPtr<RecordType> record, U32& index);

    /*
     * The case values a `match` dispatches on, and the alternative each belongs to - the `switch`
     * half of D3. Both come back ascending by value, which is what `Builder::addSwitch` declares.
     *
     * Answers false meaning "the ordinary chain handles this", and reports nothing, for
     * `matchAsMap`'s reason. See the definition, where the shape it admits is argued.
     */
    bool matchCases(Place& pivot, Buffer<ast::Pat> patterns, const bool* tested,
                    SmallArray<I64, 16>& values, SmallArray<U32, 16>& arms);
    Node* resolveBlock(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue,
                       Request wanted);

    // What an operator does and how it groups, put together - see `Operator`. Nothing having
    // reported, and the two ways it can fail are two sentences.
    // `report` false is the *constant* path asking whether rather than which: a chain it cannot fold
    // falls back to the value path, which reports for itself, and two messages about one operator is
    // what a second report here would be.
    Maybe<Operator> findOperator(StringId name, LocationId source, bool report = true);
    void reportMissingOperator(StringId name, U32 arity, LocationId source);

    /*
     * An infix expression flattened into its operands and operators - `Chain`.
     *
     * False after reporting, which is an operator this stage does not have. Shared by the value walk
     * and the condition walk, because a conjunction has to be *split* before it is climbed and both
     * need the same fixity answer to do it.
     */
    bool flattenChain(ast::Expr& expr, Chain& chain, bool report = true);

    // Whether a flattened chain is a conjunction at its top level - the question `resolveCondition`
    // splits on. See resolve_expr.cpp.
    bool splitsAtConjunction(Chain& chain);
    Node* climb(Builder& builder, Chain& chain, Size& operandIndex, Size& operatorIndex,
                U8 minimumPrecedence, DeclPtr<Type> hint, Request outer);
    Node* applyBinary(Builder& builder, LocationId source, BinaryOp op, Node& lhs, Node& rhs);
    Node* shortCircuit(Builder& builder, Chain& chain, Size& operandIndex, Size& operatorIndex,
                       U8 next, LocationId source, Node* condition, const Operator& op);
    Node* climbBool(Builder& builder, Chain& chain, Size& operandIndex, Size& operatorIndex,
                    U8 next, LocationId source);
    Node* constantBool(Builder& builder, LocationId source, bool value);
    /*
     * One alternative's exit - the jump that hands its value over, or nothing where it already left.
     *
     * `merged` says the join takes a value parameter, which is not the same question as whether
     * *this* alternative produced one: a value form whose alternative answered nothing reaches a
     * merge it has nothing to supply and is `unreachable`, and a statement form's join takes no
     * parameter and every alternative jumps into it with the carry alone.
     *
     * Answers whether the jump was built, which is how `armsValue` knows whether anything reaches
     * the merge at all - see the note there.
     */
    bool closeArm(Builder& builder, Block& end, Block& join, Node* value, bool merged,
                  LocationId source, Carry& carry, const Node* const* carried);

    /*
     * A value of a type with a teardown, given storage of its own.
     *
     * The rule this exists for is one sentence: **an owner lives in a place**. A value that is only
     * ever a node result has no place for a drop to name, no move path for the initialization
     * dataflow to track and nothing for a borrow to be rooted in - so a call result, an aggregate or
     * a merge that produces one is written into a local the moment it is made, and everything after
     * that is the ordinary machinery.
     *
     * Called from exactly the four positions that can produce one and not consume it: a discarded
     * statement, an argument filling a borrowed parameter, the target of a projection, and the
     * callee of a call through a value.
     */
    Place* hold(Builder& builder, Node& value, LocationId source);

    // The same temporary, where the position that receives the value only borrows it - the one place
    // that rule is written, for the reason the definition gives.
    Node* holdBorrowed(Builder& builder, Node& value, Request wanted, DeclPtr<Type> expected,
                       LocationId source);

    /*
     * A chain of guarded arms and one merge - what `match` is, what `if` is with two of them, and
     * what `&&` and `||` are with two of them where the second answers a constant.
     *
     * One construct rather than a two-armed one used N-1 times, and the difference is the **failure
     * edge**. A pattern's tests are laid out as control flow so that an inner one runs only once the
     * outer one has passed, and every one of them has to reach *the next alternative*; two-armed
     * branches nested inside each other give each test a merge of its own to fall into instead, so
     * the alternatives after it would be reached through a different path per test. Side by side,
     * there is one block per alternative, one join, and a failing test jumps to the next entry.
     *
     * `emit(index, onFail, hint, carry)` resolves one alternative. It emits whatever tests it needs
     * as branches to `onFail` - **null** for the alternative that needs none, which is the one
     * exhaustiveness proved covers whatever is left - and answers the value the arm produced, or
     * null where the arm left by itself or has no value to give.
     *
     * A member template rather than a function, because every caller hands it a lambda over its own
     * walk: an AST arm, a pattern beside an arm, or a span of an operator chain no AST node
     * corresponds to.
     */
    template<class F>
    Node* armsValue(Builder& builder, LocationId source, U32 count, DeclPtr<Type> hint,
                    bool wantValue, F&& emit) {
        return armsValue(builder, source, count, hint, wantValue, emit, false,
                         [](Carry&, SmallArray<Block*, 8>&) { return true; });
    }

    /*
     * The same, entered by a **multi-way branch** instead of by falling through - inventory D3.
     *
     * `multiway` says the alternatives are reached by one terminator rather than by a chain of
     * tests, and `dispatch` is what emits it: it is handed the carry and the entry blocks, and every
     * alternative is then emitted with **no failure edge**, because the terminator has already
     * proved which one it is. Everything below that - the merge, the arms' types, the carry, the
     * exits - is the chain form's and is untouched, which is the whole reason this is a parameter
     * rather than a second construct: what a `switch` changes is how the alternatives are *entered*
     * and nothing about what they answer.
     */
    template<class F, class D>
    Node* armsValue(Builder& builder, LocationId source, U32 count, DeclPtr<Type> hint,
                    bool wantValue, F&& emit, bool multiway, D&& dispatch) {
        // What the walk is holding across this construct - `Carry`, and the reason `Held` exists.
        // Taken before any block is made, because it is the state at the branch that crosses it.
        Carry carry(*this, source);

        /*
         * One entry block per alternative *after the first*, which is entered by falling into it.
         *
         * The first alternative's tests belong where the construct was written - there is nothing
         * between the pivot and them - so giving it an entry of its own would be a jump to a block
         * with one predecessor and one purpose. A **dispatched** construct is the exception and
         * needs one for every alternative, since none of them is fallen into.
         */
        SmallArray<Block*, 8> entries;
        if(!multiway) entries.push(nullptr);

        for(U32 i = multiway ? 0 : 1; i < count; i++) {
            auto block = builder.addBlock(StringId());
            carry.declare(builder, *block);
            entries.push(block);
        }

        // The terminator, in the block the construct was written in - so everything the dispatcher
        // reads is read once, before any alternative.
        if(multiway && !dispatch(carry, entries)) return nullptr;

        auto join = builder.addBlock(StringId());

        /*
         * Where each alternative ended, what it answered, and what it was carrying *there*.
         *
         * All three are per alternative and read after every one of them has been resolved, because
         * the merge's parameter is what the alternatives decide and its edges cannot be built until
         * it exists. The carried values are flat - `carry.count()` of them per alternative - since
         * they are read back by position and a list of lists would be one allocation per arm.
         */
        SmallArray<Block*, 8> ends;
        SmallArray<Node*, 8> values;
        SmallArray<Node*, 16> carried;

        auto armHint = hint;

        for(U32 i = 0; i < count; i++) {
            if(entries[i]) {
                builder.setBlock(*entries[i]);
                carry.arrive(builder, *entries[i], 0);
            }

            auto value = emit(i, !multiway && i + 1 < count ? entries[i + 1] : nullptr, armHint,
                              carry);

            ends.push(builder.currentBlock());
            values.push(value && !builder.isTerminated() ? value : nullptr);
            carry.save(carried);

            // The first alternative that answered is what the rest are checked against, which is the
            // one-directional rule the two-armed form read off its `then` arm.
            if(wantValue && values[i] && !armHint) armHint = valueType(*values[i]);
        }

        Node* argument = nullptr;
        if(wantValue) argument = mergeArms(builder, *join, ends, values, source);

        carry.declare(builder, *join);

        auto entered = false;

        for(U32 i = 0; i < count; i++) {
            entered = closeArm(builder, *ends[i], *join, argument ? values[i] : nullptr,
                               argument != nullptr, source,
                               carry, carried.pointer() + i * carry.count()) || entered;
        }

        builder.setBlock(*join);
        carry.arrive(builder, *join, argument ? 1 : 0);

        /*
         * A merge no alternative reaches is unreachable, and it is terminated **here** rather than
         * left to `finish`.
         *
         * `finish` closes an unentered block, so a construct written at the top of a body was
         * answered there - but only because nothing followed it. A statement form is followed by the
         * rest of its block, and what the walk does next is emitted into this merge: an enclosing
         * `if` arm then jumps out of it into its own join, which gives that join a predecessor and
         * turns `match w: <every arm returns>` inside an arm into "a path reaches the end without
         * returning". So a `match` whose alternatives all leave has to say so where it is built,
         * which is the same statement `return` makes and is what `Builder::isTerminated` reads.
         */
        if(!entered) builder.addUnreachable(source);

        return argument;
    }

    /*
     * The merge's own parameter: the type every live alternative answers, and the two things they
     * have to agree about that no conversion can settle.
     *
     * Split out of `armsValue` because it is the *checking* half and the template is the shape half:
     * what it does is fold one occurrence out of the alternatives and report every alternative that
     * cannot reach it, which is a walk over a list rather than anything about how the blocks were
     * laid out. Alternatives that disagree are nulled in place, so the merge is built from the ones
     * that survive and each of the others reaches it as `unreachable`.
     *
     * `ends` is here because a widened alternative's conversion belongs **in the block that
     * produced the value**: an edge into the merge supplies the merge's type, so the widening has
     * to have happened before the jump - and which type to widen to is only known once every
     * alternative has been seen. That ordering is the whole reason this runs where it does.
     */
    Node* mergeArms(Builder& builder, Block& join, SmallArray<Block*, 8>& ends,
                    SmallArray<Node*, 8>& values, LocationId source);

    /*
     * Analysis-Borrows.md §2.5's join, over the alternatives `mergeArms` has folded a type out of.
     *
     * An alternative that **produced** storage beside one that **names** somebody else's is the pair
     * a `let` over a `match` arrives at, and the join is the weaker of the two: the produced value
     * gets a frame slot and its edge supplies a borrow of it, so the merge is a loan over the union
     * of the alternatives' origins. Nothing is copied - see the definition, where the alternative is
     * argued and where the two disagreements that are still refused are named.
     */
    void joinArmLoans(Builder& builder, SmallArray<Block*, 8>& ends, SmallArray<Node*, 8>& values,
                      DeclPtr<Type> merged, LocationId source);

    /*
     * One alternative's body, for the three constructs that have arms written in the source - see
     * the definition, where the one rule they share is stated.
     */
    Node* resolveArm(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue,
                     Request wanted);

    // Whether an expression's whole job is to leave the block - `return`, `break`, `continue`.
    bool leavesBlock(ast::Expr& expr);

    /*
     * resolve_try.cpp - `?` and `?.`.
     */

    /*
     * What one carrier's `Try` instance relates - the carrier, what it proceeds with, and what it
     * exits carrying.
     *
     * A struct rather than three out-parameters because every caller wants all three and the two
     * that are read off the instance are meaningless without the one that selected it.
     */
    struct TryShape {
        DeclPtr<Type> carrier = nullptr;
        DeclPtr<Type> proceeds = nullptr;
        DeclPtr<Type> reason = nullptr;
    };

    bool carrierShape(DeclPtr<Type> carrier, LocationId source, TryShape& out);
    bool canLeaveWithCarrier(LocationId source);

    Node* outcomePayload(Builder& builder, Place& outcome, U16 constructor, LocationId source);
    Node* outcomeIsExit(Builder& builder, Place& outcome, LocationId source);
    bool emitCarrierExit(Builder& builder, Node& reason, const TryShape& leaving, LocationId source);

    Node* resolveTry(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, Request wanted);

    /*
     * One `?.`'s skip arm, left open until the chain says what type it has to produce.
     *
     * The **outcome's storage** rather than the reason itself, because the reason has to be
     * projected on the skip path and not before the branch: a payload read where its constructor is
     * not the live one is a move out of storage holding something else. `reason` is the type that
     * projection will have, which the join needs before it enters the block.
     */
    struct OptionalSkip {
        Block* block = nullptr;
        Place* outcome = nullptr;
        DeclPtr<Type> reason = nullptr;
        LocationId source = kNullLocation;
    };

    /*
     * The chain one `?.` skips to the end of - Implementation-Semantics.md part 5's optional
     * chaining, and the span `resolveOptionalChain` owns.
     *
     * Identified by **node address** (`spine`) rather than by a depth counter, which is what makes
     * `a?.b(c?.d)` two chains: the inner `?.` is the top of its own, and a counter would join it to
     * the outer one's wrapper.
     *
     * `carry` is the enclosing expression's held values, snapshotted at the top of the chain - every
     * branch out of it crosses them, so it is the chain's and not each skip's.
     */
    struct OptionalChain {
        SmallArray<ast::Expr*, 8> spine;
        SmallArray<OptionalSkip, 4> skips;
        DeclPtr<Type> carrier = nullptr;
        Carry* carry = nullptr;
    };

    OptionalChain* optionalChain = nullptr;

    // Whether this node is part of the chain being resolved rather than the top of one of its own.
    bool onOptionalSpine(ast::Expr& expr) const {
        return optionalChain &&
               optionalChain->spine.contains([&](ast::Expr* node) { return node == &expr; });
    }

    // Whether the chain this node tops holds a `?.` anywhere down its spine, which is what decides
    // that a join has to be set up before any of it is resolved.
    bool chainSkips(ast::Expr& expr);

    Node* resolveUnwrap(Builder& builder, ast::Expr& expr);
    Node* resolveOptionalChain(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint);

    /*
     * resolve_stmt.cpp - the things that happen.
     */

    void resolveStatement(Builder& builder, ast::Expr& expr);

    /*
     * `let x = e in body` - a binding scoped to a statement, and where a lens call is written.
     *
     * The two are one form and that is Analysis-Lens.md §11.2's argument for it: the extent is
     * settled at the call site, and `in` is the spelling that already parses. An initializer that
     * calls a `lens fn` makes `body` the continuation; anything else is an ordinary scoped `let`,
     * and the difference is the callee's declaration rather than anything about the call.
     */
    Node* resolveScoped(Builder& builder, const ast::VarDecl& binding, DeclPtr<Type> hint,
                        bool wantValue, Request wanted);

    /*
     * How much of the caller one lens call covers - Analysis-Lens.md §11.2.
     *
     * The two call forms differ in this and in nothing else, which is the argument for one struct
     * rather than two paths through `resolveLensCall`: what the call does, what it hands over, which
     * region it opens and how it rejoins are the same in both, and the only question the forms
     * answer differently is where the continuation stops.
     *
     * The scoped form is written and the flat form is derived, which is the asymmetry §11.1 leaves
     * behind: the extent cannot be read off the signature or the body, so it is either said at the
     * call site or worked out from what the call site does with what it was handed.
     */
    struct Extent {
        // The scoped form: `in:`'s body, one statement - usually a block.
        ast::Expr* written = nullptr;

        // The flat form: `[from, to)` of the statement list the call was written in.
        ast::ParseList<ast::Expr> statements {};
        U32 from = 0;
        U32 to = 0;
    };

    /*
     * The callee of a directly-named `lens fn` call, and null for everything else.
     *
     * A *directly* named one, because a lens reached through a class member or a binding of lens
     * type is the erased callback ABI rather than this form. Asked by both call forms, so that a
     * name that turns out not to be a lens falls through to the ordinary statement it is in exactly
     * one way.
     */
    NamedCall namedCallee(ast::Expr& expression, NamedCallSite* site = nullptr);
    NamedCall lensCallee(ast::Expr& expression);

    /*
     * Which `iter fn` a `for` loop runs, where the name reaches more than one - R5 for a traversal.
     *
     * The set is narrowed to the iterators (`CandidateKind::Iterator`) and then decided by the same
     * R2-R5 an ordinary call is decided by, which is what makes a plain `iter fn` a *member* of the
     * set rather than a shadow over it. Answers null where nothing is left to select from, so the
     * caller reports in its own words; `into` is filled where the answer needed the arguments, and
     * `resolveIteratorFor` then hands them to `handoverCall` instead of walking them again.
     */
    NamedCall iteratorCallee(Builder& builder, ast::Expr& expression, const NamedCallSite& site,
                             OverloadChoice& into);

    /*
     * Where the flat form's continuation stops - Analysis-Lens.md §11.2 and §11.4.
     *
     * Answers the index of the last statement inside the extent, which is `at` itself when the
     * extent is empty and the call is therefore §11.3's diagnostic.
     */
    U32 flatExtent(ast::ParseList<ast::Expr> statements, U32 count, U32 at, StringId handed);

    /*
     * A lens call written *inside* a statement, lifted out in front of it - Analysis-Lens.md §11.2's
     * extent rule reaching an expression position.
     *
     * The rule is one sentence: a lens's continuation is a run of *statements*, and an argument
     * position is not one - so the call moves to a `let $lensN = ...` inserted before the statement
     * and the position it was written in is left naming that binding. What comes out is a program
     * the flat form already compiles, which is why this is a rewrite of the parse tree and not a
     * second lowering: `let seen = twice(logged(1))` and `let $lens0 = logged(1)` followed by
     * `let seen = twice($lens0)` are the same program, and only one of them has to be resolved.
     *
     * `hoistBlockLenses` runs once over a block before any statement of it is walked, so everything
     * downstream - the extent scan included - sees the final list. Its loop re-reads the size, which
     * is what lifts the *second* call in one statement: the statement it inserted is examined in
     * turn, and the call still inside the original is lifted on the next pass, inside the first
     * one's continuation.
     */
    void hoistBlockLenses(ast::Expr& block);
    bool hoistLensCall(ast::Expr& block, Size index);

    /*
     * The first lens call in evaluation order that `hoistLensCall` would move, or null.
     *
     * `root` says the expression *is* the statement (or a `let`'s whole initializer), which is the
     * one exclusion that makes this terminate: those two are the forms `resolveStatements` already
     * gives a continuation to, so hoisting one would rewrite it into a `let` holding the same call
     * forever.
     *
     * The arms of an `if`, a `match` and a lambda body are excluded by having no case here: what
     * runs there is conditional, and a lens hoisted out of one would run where the program does not.
     * `&&` is the same exclusion asked of a name rather than of a syntax - see `defersArguments`.
     */
    ast::Expr* hoistableLensCall(ast::Expr& expr, bool root);
    bool defersArguments(ast::Expr& callee, U32 arity);

    // `$lens0`, `$lens1`, ... - counted per hoist and not per block, so two calls lifted out of one
    // statement cannot shadow each other while the inner one is still being resolved.
    U32 hoistedLenses = 0;
    void reportEmptyExtent(LocationId source, Function& callee);

    /*
     * A bare `f(...)` statement whose callee is a lens - the third call form, beside `f(...):` and
     * `let x = f(...)`.
     *
     * `cell.withLock()` is what it is for and is how every lock in `lib/` is taken: the lens hands
     * over nothing, so there is no binding whose last use could bound the extent, and what the
     * statement means is "the rest of this block is inside". Answers the last statement it consumed,
     * as the flat form does.
     */
    U32 resolveBareLensCall(Builder& builder, ast::ParseList<ast::Expr> statements, U32 to, U32 at,
                            ast::Expr& call, const NamedCall& callee, bool wantValue);

    U32 resolveFlatLensCall(Builder& builder, ast::ParseList<ast::Expr> statements, U32 to, U32 at,
                            const ast::VarDecl& binding, ast::Expr& call, const NamedCall& callee,
                            bool wantValue);
    /*
     * The whole of a call that opens a continuation region, less what runs inside it.
     *
     * `inside(region, join, bound)` is called with the entry current and the hand-over bound, and is
     * the one thing a lens call and a `for` over an iterator disagree about: a written or derived
     * extent against a loop body. Everything else - the arguments, the type arguments, the region,
     * what arrives, the binding's storage, how the caller rejoins - is one construct, so it is
     * written once.
     *
     * `bound` is the storage the hand-over landed in, handed out rather than only named, because a
     * `for` may take it apart: a pattern binds *parts* of what arrived, and the parts are
     * projections of that one local. A caller binding a plain name has already been given it under
     * `name` and ignores this.
     */
    template<class F>
    bool handoverCall(Builder& builder, ast::Expr& call, const NamedCall& called,
                      Continuation::Multiplicity multiplicity, StringId name,
                      LocationId nameSource, ast::ParseList<ast::Alt> alternatives, F&& inside,
                      OverloadChoice* chosen = nullptr);

    /*
     * `| e -> ...` beside a call of a traversal that declared `| E` - the skip path's own code.
     *
     * Held to the callee's signature in both directions: a traversal that can fail owes an
     * alternative and one that cannot has nothing for one to handle. Answers false having reported,
     * and true for the ordinary lens call with no alternatives at all - which is what lets
     * `handoverCall` ask this of every call rather than only of the ones it expects to skip.
     */
    bool checkAlternatives(Function& callee, ast::ParseList<ast::Alt> alternatives,
                           LocationId source);
    void resolveAlternative(Builder& builder, ast::Alt& alternative, Node& reason);

    bool resolveLensCall(Builder& builder, const ast::VarDecl& binding, ast::Expr& call,
                         const NamedCall& callee, const Extent& extent);
    void resolveExtent(Builder& builder, const Extent& extent);

    /*
     * A run of statements, which is what a block is and what a flat lens call's extent is a slice
     * of. `wantValue` asks the *last* of them for a value, and is false for every continuation.
     */
    void resolveFor(Builder& builder, ast::Expr& expr);
    void resolveCountedFor(Builder& builder, ast::Expr& expr, ast::ForExpr& loop);
    void resolveIteratorFor(Builder& builder, ast::Expr& expr, ast::ForExpr& loop);
    void resolveExit(Builder& builder, ast::Expr& expr);
    Node* resolveStatements(Builder& builder, ast::ParseList<ast::Expr> statements, U32 from,
                            U32 to, DeclPtr<Type> hint, bool wantValue, Request wanted);

    void resolveYield(Builder& builder, ast::Expr& expr);

    /*
     * Where an iterator carries on once its consumer has left - `doc/spec/lenses.md`'s cleanup rule.
     *
     * The departure resumes **after the outermost loop of the region the `yield` was written in**,
     * which is one rule covering every nesting: an ordinary loop is left by its own `break` edge, a
     * `for` over another iterator by the row that stops it, and a lens's continuation by falling out
     * of it so that the lens runs its own cleanup on the way. Where the region has no loop and is
     * the function's own, the statements below the `yield` *are* the trailing ones and control
     * simply carries on - `departureFallsThrough` is that case, asked before a test is emitted for
     * an answer that cannot change control flow.
     */
    void emitDeparture(Builder& builder, LocationId source);
    bool departureFallsThrough(Builder& builder);

    // The `$yielded != 0` test an iterator owes after a call that opened a region: a departure
    // inside one resumes at the join, and carrying on from there is what leaves the loops outside.
    void resumeDeparture(Builder& builder, LocationId source);

    /*
     * Every path through a lens hands over exactly once - Analysis-Lens.md §7.1's rule for a
     * transparent lens, checked on the callee's own body and once per lens.
     *
     * A forward walk over the blocks rather than a count of `yield` statements, because both
     * questions the rule asks are about paths: a lens that hands over on one arm of an `if` and not
     * the other is a *skipping* lens, which declares `| E`, and one that hands over twice would run
     * one continuation twice.
     */
    bool checkHandover(Builder& builder, Function& function, LocationId source);
    void nameLinearMembers(Builder& builder, Place& place);
    bool resolveDispose(Builder& builder, ast::Expr& expr);
    void resolveDeclaration(Builder& builder, ast::Expr& expr);

    /*
     * `@heap let big = ...` - the attributes written in front of one binding, applied to the local it
     * made. Reports anything else, which is what the walk over a declaration's attributes does and
     * what this position did not do at all before.
     *
     * `placed` is whether this binding is one name for one object; a pattern and an extent's binding
     * are not, and get a refusal rather than the "nothing to place" message, which would be
     * describing a local that does not exist.
     */
    void readBindingAttributes(const ast::VarDecl& binding, Local* local, bool placed);

    /*
     * The local a plain binding's resolved value becomes - `resolveDeclaration`'s flat form and
     * `resolveScoped`'s share this exactly, `readBindingAttributes` included, so `@heap` (or
     * anything added beside it) is answered once for every shape a one-name binding can take
     * rather than once per copy of the sequence.
     */
    Local* bindPlainLocal(Builder& builder, const ast::VarDecl& binding, StringId name,
                          Node& value, bool writable);

    void resolveAssign(Builder& builder, ast::Expr& expr);
    void resolveWhile(Builder& builder, ast::Expr& expr);
    void resolveReturn(Builder& builder, ast::Expr& expr);
    bool checkResultContract(Node& value, LocationId source);

    // Whether a value carrying an interior origin may fill this position - see the note there. The
    // one check a *closure* needs that a named function value does not.
    bool checkCarriedOrigins(Node& value, const Occurrence& declared, LocationId source,
                             StringView what);

    void resolveInitializers(ast::Module& file);
    void resolveGlobals(ast::Decl& declaration, Size& next);

    /*
     * Where the program starts - Analysis-Initialization.md stage B.
     *
     * `main` where the root module declares one and nothing runs before it; otherwise a synthesized
     * function that runs the root module's top level in written order and then calls `main`, whose
     * result is the program's status. A program with initialization that started at `main` would
     * start after its own initialization was skipped, which is the whole reason the second exists.
     *
     * Between the initializers and the bodies, and it has to be: a dynamic global's *type* is what
     * its initializer turned out to be, and every body that reads the name needs it.
     */
    void resolveProgramEntry();

    // The root module's top level as one body - see `resolveProgramEntry`, which is its only caller.
    void resolveEntryBody(Function& function, DeclPtr<Function> main);

    // The two halves both synthesized entries share - see resolve_stmt.cpp. `resolveTestEntry` is
    // the second caller of each.
    void beginEntry(Function& function);
    void resolveRootTopLevel(Builder& builder);
    void resolveTestFileInitializers(Builder& entry);

    /*
     * `@test` and the entry a test build runs - Design-Test.md §11.2's F1, all in resolve_test.cpp.
     *
     * `readTestAttribute` is called where a plain `fn` is declared and fills `Program::tests`;
     * `resolveTestEntry` replaces `resolveProgramEntry` under `-test`, because a test build runs
     * tests rather than `main`. `TestLibrary` is the library shape both of the middle two read.
     */
    void readTestAttribute(ast::Decl& declaration, Function& function);
    bool findTestLibrary(TestLibrary& into);
    DeclPtr<Function> wrapUnitTest(DeclPtr<Function> test, DeclPtr<Type> resultType,
                                   LocationId source);
    Node* buildTestCase(Builder& builder, const TestLibrary& library, const TestDeclaration& test);
    void resolveTestEntry();

    // One `let` of it, filling the storage its own declaration made - see the definition.
    void resolveTopLevelBinding(Builder& builder, const TopLevelStmt& statement);

    // Whether anything at all runs before `main` - a module whose every top-level `let` is a
    // constant is every fixture that predates stage B, and nothing is synthesized for one.
    bool runsAtStartup();
    void resolveBodies(ast::Module& file);
    void resolveBody(Function& function, ast::Decl& declaration);

    // `fn f(x) = expr` with no `-> T` - the body's answer, and the two halves of settling it. See
    // `Function::inferResult`.
    bool inferResultType(Function& function, Node* value, LocationId source);
    bool requireResult(Function& callee, LocationId source);
    void resolveInstanceBody(Instance& instance, ast::Decl& declaration);

    /*
     * An intrinsic whose expansion makes blocks - `intrinsic::OpForm::Branch`, and
     * `resolve_intrinsic.cpp` for why those are the resolver's rather than an emitter's.
     *
     * Reached from `finishCall` at exactly the point `expandIntrinsic` is reached for a `Node` role:
     * everything above it - name, arity, inference, argument preparation, constraint discharge - has
     * already run, and what the form decides is only which of the two lays the last step out.
     */
    Node* expandBranchingIntrinsic(Builder& builder, intrinsic::Op op, Function& callee,
                                   Node* const* arguments, const Substitution& with,
                                   LocationId source);

    // The four compare-exchange rows, which differ only in the two orders and the strength - see the
    // definition, where the branch that joins the instruction's two answers is.
    Node* expandCompareExchange(Builder& builder, Function& callee, Node* const* arguments,
                                const Substitution& with, AtomicOrder success, AtomicOrder failure,
                                bool weak, LocationId source);

    void resolveClassBody(Class& klass, ast::Decl& declaration);
    // `mark` is taken where this body's walk began - see `ErrorMark`. It is what tells a path that
    // falls off the end of a *broken* body from one that falls off the end of a whole one.
    void finish(Builder& builder, Function& function, bool unit, LocationId source,
                const ErrorMark& mark);
    bool enteredFrom(Body& body, MirBase base, Block& block);

    /*
     * Whether control has left - which is what a rule about leaving actually asks.
     *
     * Two ways out and both count. A **terminator** is the written one. The other is a block nothing
     * reaches: `armsValue` sets the walk on the merge whether or not anything arrives there, so a
     * `match` whose every arm returned leaves an empty join behind and the walk standing in it - and
     * a rule that asked only about the terminator would say that code carried on into a block no
     * edge enters.
     */
    bool departed(Builder& builder);

    /*
     * Shared.
     */

    /*
     * The storage a name in this body names, or null.
     *
     * `bound` says which kind of null it is: false is a name nothing in the body declared, and true
     * is one declared by a binding whose initializer failed. A bound null **stops the walk**: the
     * name is this body's, so a capture, a global or a function of the same spelling is not what it
     * means, and letting the lookup carry on found `lib/Core`'s test-suite global `live` where
     * `lib/Core/Container.yana` had written `let live = maskUpTo(...)` - a diagnostic about a class
     * nobody had named, two files away from the statement that actually failed.
     */
    Local* findLocal(StringId name, bool* bound = nullptr);

    // `@lazy` in a body: whether a name is one, the read that *is* the force, and the call that
    // spends it - resolve_expr.cpp.
    const Arg* deferredParameter(Local& local);
    Node* readDeferred(Builder& builder, Local& local, LocationId source);
    Node* forceDeferred(Builder& builder, Local& local, LocationId source);

    /*
     * Which structural requirement of the enclosing signature says `subject` has `field`.
     *
     * `kNoProperty` where none does, which is the caller's cue to report - the position that asked
     * is what knows whether it wanted a field or a name, exactly as `genVariable` leaves the message
     * to whoever asked for a type.
     */
    static constexpr U32 kNoProperty = ~U32(0);
    U32 findProperty(DeclPtr<Type> subject, StringId field);

    // Which callable requirement of the enclosing signature this name and arity names, and
    // `kNoProperty` where none does. The same sentinel as the property lookup beside it, because it
    // means the same thing: this signature promised nothing about that.
    U32 findCallable(StringId name, U32 arity);

    // `grow(x)` where the signature wrote `grow: (a) -> a` - the call typed against the promise.
    Node* resolveRequiredCall(Builder& builder, ast::AppExpr& written, U16 requirement,
                              LocationId source);

    /*
     * convert.cpp - what a value does when the position it fills is a different type.
     *
     * The one rule and the one message, because "the wrong type arrived here" is the same failure
     * wherever it happens and a reader comparing two diagnostics about it should not have to notice
     * they are two. Every position that has a declared type to check a value against goes through
     * this, and so does `::` - which is what makes an implicit widening and a written one the same
     * program. See the file, where the rule is argued.
     *
     * `access` is what the position asked for, and it is here for one case: a converted value is one
     * this frame just made, so a position the callee writes *through* cannot take one. Owned by
     * default, since that is what every position with nothing to say is.
     */
    Node* convertValue(Builder& builder, Node& value, DeclPtr<Type> to, LocationId source,
                       bool implicit, StringView what, Access access = Access::Owned);

    // The implicit half, named for what a reader of a call site is doing: checking a value against
    // the type a position declared, and widening it where the language says that happens on its own.
    Node* coerce(Builder& builder, Node& value, DeclPtr<Type> expected, LocationId source,
                 StringView what, Access access = Access::Owned) {
        return convertValue(builder, value, expected, source, true, what, access);
    }

    // Whether the program declares a conversion of this kind between the pair, asked without
    // reporting - a missing instance is an answer here, and the message is convert.cpp's to choose.
    bool relatedBy(intrinsic::Known role, DeclPtr<Type> from, DeclPtr<Type> to, LocationId source);

    // The unique `Widen` upper bound of a pair, or null where neither widens to the other - the one
    // place a conversion decides a type rather than filling one. See the definition.
    DeclPtr<Type> commonWiden(DeclPtr<Type> lhs, DeclPtr<Type> rhs, LocationId source);

    // Whether a `@bits(n)` value widens into a type, which is a conversion the ladder does not name.
    // The load half only; `convertValue` performs the store half, and the definition says why.
    bool refinesInteger(DeclPtr<Type> from, DeclPtr<Type> to);

    // "A conversion cannot fill a position the callee writes through", reported - the one message
    // `convertValue`'s two conversion arms share. Answers null, so a caller may return it.
    Node* refuseConversion(DeclPtr<Type> to, DeclPtr<Type> from, LocationId source, StringView what);

    // The one sentence this stage says about the heap it has not got - resolve_sig.cpp, where the
    // two positions that reach it are named.
    void reportOwnedSequence(LocationId source);

    // The note that follows a refusal where one side's width is the target's - see the definition,
    // which is where the case it exists for is written out.
    void explainAbstractWidth(DeclPtr<Type> from, DeclPtr<Type> to, LocationId source);
};

/*
 * The `Run(a)` and `Array(a)` a literal builds, at one element type - see the definition.
 *
 * A free function rather than a member because what it needs of the resolver is three things it
 * already publishes, and because both callers - the array literal and the string constant - want
 * the same answer at different element types.
 */
Maybe<RunShape> arrayShape(Resolver& self, DeclPtr<Type> element, LocationId source);

/*
 * The type arguments one use site is deciding - `gen.h`.
 *
 * A call and a record construction ask one question in one order, and this is where that order is
 * written: every argument starts undecided, each value that stands opposite a declared position
 * binds what it can, and the position's declared type read *through what is decided so far* is what
 * the next value is resolved against and checked against. The two sites differ only in what they
 * call their positions - an argument or a field - and in what they say when nothing decides one.
 *
 * The arguments are held here rather than by the caller because a `Substitution` is a pointer into
 * them: a caller keeping the array and the substitution side by side holds a view that its own next
 * push invalidates, which is a rule that has to be remembered rather than one that cannot be broken.
 */
struct TypeArguments {
    TypeArguments(Resolver& resolver, DeclPtr<GenContext> owner);

    // Whether there is anything to decide. False for a plain function and for a `data` with no head
    // parameters, which between them are most use sites and cost nothing at any of them.
    bool any() const { return arguments.size() != 0; }

    // The substitution as the arguments stand. Built on each call rather than held, for the reason
    // above.
    Substitution with() { return Substitution { owner, arguments.pointer(), arguments.size() }; }

    // A declared position's type with whatever is decided so far. Still open where an argument it
    // mentions is undecided, which `substituteType` leaves alone rather than failing on.
    DeclPtr<Type> expected(DeclPtr<Type> declared);

    /*
     * The same type where a value can be made of it, and nothing where it cannot.
     *
     * A hint is an *input* to inference - a literal has no width of its own - so a position that is
     * still open supplies none rather than supplying a variable, which nothing could make a value
     * of.
     */
    DeclPtr<Type> hint(DeclPtr<Type> declared);

    // Whether this position still mentions the callee's own variables - "the call has not decided
    // what goes here yet". What `resolveArguments` reads to hold an argument that takes its type
    // *from* the position back until the arguments that decide it have run.
    bool undecided(DeclPtr<Type> declared);

    /*
     * The hint a position takes from what the call's **result** is being asked for, where the
     * arguments have not decided it - `fn maxValue() -> U64 = not(0)`.
     *
     * `not(x: a) -> a` has one open position and a literal in it, so nothing at the call names a
     * width and `0` is built at the default `I32` - and then a `U64` return refuses it, which is
     * where this was found. What decides the width is the destination, which is what a hint is for
     * and which was already reaching every other literal in the language.
     *
     * A **hint and not a binding**, and that is the whole of why it is sound at a position holding
     * something other than a literal. `f(x)` at `fn f(v: a) -> a` with `x: U32` asked for a `U64`
     * hands `x` the hint and `x` ignores it, so `a` is still bound to `U32` by the argument and the
     * widening happens where it always did - at the return, after the call. Binding `a := U64` up
     * front instead would run `f` at a width the operand never had, and for `not` that is a
     * different number. The shipping stage states the same rule from the other end: `bindResult`
     * binds the result *last* and keeps its answer only where nothing decided the position or a
     * literal did.
     *
     * The bindings are a copy for that reason - what this returns is an input to resolving one
     * argument, and what the call is called at stays the argument walk's answer.
     */
    DeclPtr<Type> resultHint(DeclPtr<Type> declared, DeclPtr<Type> result, DeclPtr<Type> wanted);

    /*
     * What a value standing opposite a declared position decides.
     *
     * Structural, one-way and total: the pattern is the declaration's type and the concrete side is
     * the value's, so a variable of this context is *bound* by what stands opposite it and
     * everything else has to match already. There is no unification of two open sides and there
     * deliberately is not - the caller's own variables are opaque to the callee's signature, so a
     * generic body calling a generic function binds the callee's `a` to the caller's `a`, and that
     * is a binding rather than an equation.
     *
     * It reports nothing, and that is the division of labour with `checkType`: a binding that
     * contradicts an earlier one is a type mismatch, which the check that follows names both sides
     * of - `f(1, True)` against `fn f(x: a, y: a)` says "an argument must be I32, and this is Bool"
     * where a message from here would name neither and arrive first.
     */
    void bind(DeclPtr<Type> declared, DeclPtr<Type> supplied);

    /*
     * The same binding from the call's **result** position, which records what it decided.
     *
     * A result hint is a wish about one call where a functional dependency is a promise about the
     * instances, so `fillDetermined` has to be able to tell a position the hint filled from one an
     * argument filled and reopen the first. `bind` alone cannot say which it was - it writes into
     * empty slots and reports nothing - so the two callers are two entry points rather than a flag.
     */
    void bindResult(DeclPtr<Type> declared, DeclPtr<Type> supplied);

    // The arguments an expected instantiation already settles, before a single value is read - what
    // makes `let b: Boxed(I32) = Boxed {value: 0}` build an `I32` box where the literal alone would
    // have defaulted it.
    void seed(RecordType& expected);

    /*
     * What this context's own class requirements decide - the other half of a functional dependency,
     * and what makes a constrained signature usable at all.
     *
     * `fn (Contiguous(c, a)) first(xs: c) -> a` binds `c` from its argument and has *nothing* to
     * bind `a` with: no parameter mentions it, and the caller wrote no type. What decides it is the
     * promise `class Contiguous(c -> a)` makes - one `c` has one `a` - so the deciding positions are
     * looked up and the determined ones are read back off whatever answered.
     *
     * Three things can answer, in this order. The **enclosing signature's own requirement** first,
     * because it is the answer that does not depend on which instances happen to be declared. Then a
     * **concrete** deciding prefix, which selects an instance and reads the rest off its head. Then
     * a prefix that is not concrete but whose outermost constructor is - `Index(Array(a), ?, ?)`
     * inside a body generic in `a` - which `selectOpenInstance` answers on the terms
     * `doc/progress/2026-09-06-inventory.md` D13 sets. A prefix that is a **bare variable** is none
     * of the three: `c` inside a body generic in `c` is not a type, so if the enclosing signature
     * did not promise the relation there is nothing here that could.
     *
     * Repeated until nothing moves, so `Contiguous(c, a)` deciding `a` and `Elem(a, e)` then
     * deciding `e` resolve in one call rather than in the order the constraints happen to be written.
     */
    void fillDetermined(LocationId source);

    /*
     * The fixpoint alone, without the declared defaults behind it.
     *
     * Its own entry point because one caller needs it *during* the argument walk and must not have
     * the defaults with it. `xs[i]` names `Index.get(self: c, index: k)`, whose `k` no argument may
     * decide - the dependency `Index(c -> k, v)` is what says which key the container takes, and an
     * index the author wrote as an `Int` is a value to be converted to that key rather than an
     * answer about what it is. So `prepareArgument` asks this the moment a position it is about to
     * coerce into is still open, and the argument then meets the type the container named.
     *
     * A default may not run there. `fillDeclaredDefaults` is what a position means when *nothing*
     * says, and the result hint has not been read yet at that point - see its own note, where the
     * ordering is the rule rather than an implementation detail.
     */
    void fillFromRequirements(LocationId source);

    // And what the declaration says where nothing at the call did - see the definition, where the
    // reason this runs last is the rule rather than an ordering.
    void fillDeclaredDefaults();

    // The first variable nothing decided, for the caller to report against its own positions; null
    // where every one is bound. Reported rather than picked, because an argument chosen at random is
    // a type chosen at random.
    DeclPtr<Type> undecided() const;

    /*
     * Reports the first open position, and says why it is open. True where there was one.
     *
     * Two reasons and two messages. Where a requirement's deciding positions were concrete and no
     * instance answered them - `missingClass` - the position is not undecided but *undecidable*, and
     * the missing instance is what the author has to hear about: `n[0]` on an `I32` is a call to
     * `get` whose `Index(I32, ..)` does not exist, not a call given too little information. Where
     * nothing recorded a reason, the `decider` - an argument, a field - is what failed to decide it,
     * `in this <where>`.
     */
    bool reportUndecided(LocationId source, StringView decider, const String& where);

    Resolver& resolver;
    DeclPtr<GenContext> owner = nullptr;

    /*
     * Whether this walk may **meet** two bindings of one variable rather than refusing the second -
     * `TypeArguments::bind`, where the unification is argued.
     *
     * Opt-in, and the reason is what has to happen afterwards: the meet moves a binding an earlier
     * argument was already coerced against, so only a walk that ends in `widenPreparedArguments` can
     * perform it. The three that do are the written call, the overloaded call and the operator
     * chain. Everywhere else a `TypeArguments` is filled from values the compiler itself built -
     * `callFunction`, `applyMember`, a construction - and a binding that moved there would leave
     * them at the width they were built at, with nothing to bring them up.
     */
    bool unifies = false;

    // Whether one actually moved, so the pass afterwards costs nothing at the calls that did not.
    bool widened = false;

    // A bitmask over `arguments`: the positions an *argument* of this call decided, which are the
    // only ones the meet above may move. See `bind`.
    U64 fromArgument = 0;

    SmallArray<DeclPtr<Type>, 4> arguments;

    // The requirement that could not answer - see `reportUndecided`. Recorded by `fillDetermined`
    // where a concrete deciding prefix matched no instance, with that prefix.
    DeclPtr<Class> missingClass = nullptr;
    SmallArray<DeclPtr<Type>, 4> missingDeciding;

    /*
     * Whether an *open* deciding prefix has already reported here - D13's ambiguity.
     *
     * The fixpoint asks each requirement once per round and a round runs again whenever any other
     * requirement moved, so a requirement that reports rather than answers would report once per
     * round. One message, on `missingClass`'s reading: the first is the reason and the rest are the
     * same reason said again - which is also why `reportUndecided` says nothing more once this is
     * set, since the position being open is what the message already explained.
     */
    bool reportedOpen = false;

    /*
     * Which positions the **result hint** decided - see `bindResult` and `fillDetermined`.
     *
     * A bitmask over the context's own parameters, which bounds it at 64: a declaration with more
     * type parameters than that has no fundep this rule could be about, and the positions past the
     * bound simply keep the hint's answer, which is what they did before the rule existed.
     */
    U64 fromResult = 0;
};

/*
 * What a selection decided, and the arguments it resolved to decide it.
 *
 * Split out of `resolveOverloadedCall` because the *first* half of R5 serves something that is not a
 * call: a `for` loop's traversal is selected exactly as a call is and then opens a continuation
 * region rather than being emitted. Holding the resolved values here is what lets the winner's
 * positions be prepared afterwards without the arguments being walked a second time - which would
 * evaluate them twice.
 *
 * Defined here rather than beside `CandidateKind`, where it used to stand, because `args` needs
 * `TypeArguments` complete: it is the winning candidate's own trial, carried out of
 * `chooseCandidate`'s per-candidate array instead of being rebuilt - see the note there. Without
 * this, `finishOverloadedCall` built a fresh `TypeArguments` and solved the winner's signature a
 * second time against the same arguments, which is exactly the two-solves bug this struct's own
 * comment used to claim could not happen: `x + y` at `(I32, I64)` through an overloaded `+` had
 * selection widen the shared class variable to `I64` and completion, solving again with nothing
 * carried over, refuse the `I32` operand before its own widening had run - and the order the
 * operands were written decided which of the two positions hit that.
 *
 * A `Maybe` because `TypeArguments` holds a `Resolver&` and a `SmallArray`, so it is neither
 * default-constructible nor assignable - `Maybe::replace` placement-constructs it directly in this
 * struct's own storage, once the winner is known, and every position from there on is prepared
 * against the one object `unwrap()` returns.
 */
struct OverloadChoice {
    DeclPtr<Function> function = nullptr;
    ArgMap map;
    Maybe<TypeArguments> args;

    // In **written** order, one slot per argument the call writes, with the receiver first where
    // there is one. A null entry is a position the winner still decides - a `@lazy` thunk, or an
    // undecided literal - exactly as `prepareResolvedArguments` reads it.
    SmallArray<Node*, 8> values;

    explicit operator bool() const { return function != nullptr; }
};

/*
 * The context a walk reads written types in, for the length of one declaration - `Resolver::genContext`.
 *
 * Restored by a destructor rather than by an assignment at the end of each walk, because the walks
 * that set it have exits that report and return: a context left standing is the *next* declaration's,
 * and a lowercase type name written there would bind to a declaration that did not write it. Two
 * spellings of this rule is how one of them comes to miss an exit.
 */
struct GenScope {
    GenScope(Resolver& resolver, DeclPtr<GenContext> context):
        resolver(resolver), outer(resolver.genContext)
    {
        resolver.genContext = context;
    }

    ~GenScope() { resolver.genContext = outer; }

    GenScope(const GenScope&) = delete;
    GenScope& operator = (const GenScope&) = delete;

    Resolver& resolver;
    DeclPtr<GenContext> outer;
};

} // namespace mir
