#pragma once

#include "build.h"
#include "gen.h"
#include "select.h"
#include "specialize.h"
#include "../parse/ast.h"

namespace mir {

struct Resolver;
struct TypeArguments;

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
 * One operator this stage understands, and how tightly it binds.
 *
 * A stand-in for instance selection, and openly one. In the language `+` is `Num.add`, `<` is
 * `Ord.<` and `&&` is a `Truth` function with a `@lazy` second argument, so every row here is a
 * class member that milestone 6 will resolve through the ordinary overload machinery. What the
 * table buys until then is that a fixture can write arithmetic without a standard library, and what
 * it deliberately does not do is invent a rule: the precedences are `Core.yana`'s own fixity
 * declarations, so an expression groups here exactly as it does under the other stage.
 *
 * Every one of them is `infixl` in Core, which is why there is no associativity column. A right
 * associative operator would need one, and the climb is written so that adding it is a parameter
 * rather than a restructuring.
 */
enum class OperatorForm: U8 {
    Binary,      // an ordinary two-operand computation
    ShortAnd,    // `&&` - the right operand runs only if the left was True
    ShortOr,     // `||` - the right operand runs only if the left was False
};

struct Operator {
    StringView text;
    U8 precedence;
    OperatorForm form;
    BinaryOp op;
};

/*
 * An infix chain, flattened.
 *
 * The parser nests infix expressions to the right without regard for precedence, because fixity is
 * a module-level property it cannot know. So the chain is flattened whole and re-associated by
 * `climb`, which is the same two steps the current resolver takes for the same reason.
 */
struct Chain {
    SmallArray<ast::Expr*, 8> operands;
    SmallArray<const Operator*, 8> operators;
    SmallArray<LocationId, 8> sources;
};

// A name in scope inside a body, and the storage it names.
struct Binding {
    StringId name;
    Local* local;
};

/*
 * One lens call being flattened into the body being resolved - Analysis-IR.md §10.1.
 *
 * A lens call is not a call below the resolver. The callee's body is resolved *into* the caller, so
 * that the four exits out of the continuation are ordinary CFG edges the liveness, loan and drop
 * passes walk, and so that the lens's own frame storage and the caller's are one frame with one set
 * of places. What this holds is the state that walk needs and that neither the AST nor the CFG
 * carries: which lens, where its region begins, what the source wrote to bind, and whose names are
 * in scope.
 *
 * The two scopes are the whole reason `floor` exists. Inside the callee's body the caller's locals
 * are **not** visible - the callee did not write them and cannot name them - so `findLocal` stops
 * there; and at the `yield` the caller's names come back and the callee's go away, which is a swap
 * rather than a push because the two are siblings and neither nests in the other.
 */
struct LensFrame {
    Function* callee = nullptr;
    ContinuationId region;

    // The frame's entry block, which the yield check walks forward from.
    BlockId entry;

    // What the call site wrote: the name its continuation binds, and the `in` body that *is* the
    // continuation. Both come from one `let`, so they are held as the declaration rather than as
    // two fields that could belong to different ones.
    const ast::VarDecl* declaration = nullptr;

    // Where the callee's own names begin, and the floor the caller was resolving under. The first
    // hides the caller's names while the callee's body is resolved; the second is what a `yield`
    // restores so that the continuation sees the caller's.
    U32 floor = 0;
    U32 callerFloor = 0;

    /*
     * The same swap for the frame stack itself, and it is needed for the same reason.
     *
     * A `yield` means the innermost lens **this text is lexically inside**, which is not the
     * innermost frame that is open: inside `let x = inner() in yield x`, written in `outer`'s body,
     * both frames are open and the `yield` is `outer`'s. So the continuation restores the frame
     * floor its call site was resolving under, exactly as it restores the binding floor.
     */
    U32 callerLensFloor = 0;

    /*
     * The frame's cleanup, as the statements that have not been resolved yet - `Resolver::tails`.
     *
     * A lens's cleanup runs on **every** exit out of its continuation, and the exits do not merge:
     * falling out continues in the caller and a `return` leaves the function, so a shared copy would
     * need a discriminator to tell them apart at its end. That discriminator is exactly what the
     * ownership passes cannot see through - the returned value would be initialized on one incoming
     * path and read on the merge of both - so each exit gets its **own** copy, resolved from the
     * same AST.
     *
     * `tailFloor` is where this frame's steps begin, recorded when the frame opens; `tailTop` is
     * where they end, recorded at the `yield`, since everything above it is the continuation's. The
     * range is read live rather than copied: the block walks it names are suspended on the C++ stack
     * while the continuation is resolved, so nothing below `tailTop` moves.
     */
    U32 tailFloor = 0;
    U32 tailTop = 0;

    // The callee's own names at the `yield`, parked in `Resolver::hiddenBindings` while the
    // continuation runs. A cleanup copy is resolved in the callee's scope, so it needs them back.
    U32 hiddenFirst = 0;
    U32 hiddenCount = 0;

    /*
     * Whether this frame's cleanup is owed where the walk currently is.
     *
     * `Outside` is the frame's own body - before the `yield`, or in the statements after it - where
     * nothing is owed: an exit written there either has no cleanup yet or is already in it, and the
     * statements it has not reached are dead behind it either way. `Continuation` is the caller's
     * continuation, which is the one place an exit has to run the cleanup on its way out.
     * `Unwinding` is a copy of that cleanup being resolved right now.
     *
     * Set for the *length of the continuation* and put back afterwards rather than latched, because
     * this is a fact about the path and not about the walk having gone past something: `if c: yield
     * 1 else: yield 2` hands over exactly once on every path, and a stage the first arm left behind
     * would make the second arm's hand-over look like a second one.
     */
    enum Stage: U8 { Outside, Continuation, Unwinding };

    Stage stage = Outside;

    LocationId source = kNullLocation;
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
    void edge(Builder& builder, const SmallArray<Node*, 4>& state, SmallArray<Operand, 8>& into);

    void save(SmallArray<Node*, 4>& into) const;

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

struct Resolver {
    Context& context;
    Program& program;
    ast::ParseBase parse;
    Module& module;

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
     * The body each `lens fn` declared, for the call sites that flatten it.
     *
     * A resolver-local table rather than a field on `mir::Function`, and the reason is the arena
     * rule the declaration region is built on: a `Function` lives for the whole compilation and the
     * AST does not, so a parse handle stored on one would outlive what it names. Flattening happens
     * during resolution and nowhere else, so the association belongs to the walk that uses it.
     *
     * An array and a linear search: a module has a handful of lenses, and a hash map keyed by a
     * declaration offset would be a second index over the same declarations `writtenFunctions`
     * already is.
     */
    struct LensBody {
        DeclPtr<Function> function;
        ast::Expr* body = nullptr;
    };

    Array<LensBody> lensBodies;

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

    // The names a `yield` parked - see `LensFrame::hiddenFirst`. One array for every open frame,
    // because the frames nest and their ranges retire in the order they were made.
    Array<Binding> hiddenBindings;

    /*
     * Where each open statement sequence has got to - `LensFrame::tailFloor`.
     *
     * Maintained by `resolveBlock` for every body, and read only while a lens frame is open. It is
     * what makes "the rest of the callee" a thing that can be resolved a second time: the AST holds
     * the statements, so an exit out of a continuation needs nothing but the position the walk had
     * reached when the `yield` suspended it.
     */
    struct TailStep {
        ast::ParseList<ast::Expr> statements;
        U32 next = 0;
    };

    Array<TailStep> tails;

    // The function whose body is being resolved, which `return` needs and nothing else does. It
    // stays the *caller* while a lens is flattened into it: a `return` written in a continuation
    // leaves the enclosing function, which is the whole point of Analysis-Lens.md §5.1.
    Function* currentFunction = nullptr;

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
     * The lens calls open at this point in the walk, innermost last - `LensFrame`.
     *
     * A stack because they nest: a lens called inside another's continuation lives in that lens's
     * frame (§11.4), and `yield` means the innermost one. Empty in every ordinary body, which is
     * what makes "am I inside a lens" one test rather than a flag each construct maintains.
     */
    Array<LensFrame> lensFrames;

    // How many of the open frames a `yield` may see - `LensFrame::callerLensFloor`. The innermost
    // visible one is `lensFrames[lensFloor - 1]`, and zero means there is none.
    U32 lensFloor = 0;

    /*
     * The lowest binding a name lookup may see - the callee's own names while a lens is flattened.
     *
     * Zero everywhere else. `bindings` is one flat array with shadowing falling out of a backward
     * walk, and a lens body is the one place where the walk must *stop* rather than continue into an
     * outer scope: the callee is a separate declaration and cannot name the caller's locals.
     */
    U32 bindingFloor = 0;

    DeclBase decl() { return *program.decl; }
    DeclPtr<Module> here() { return &module - *program.decl; }

    void unsupported(LocationId source, StringView what);

    /*
     * resolve.cpp - written types and constant expressions.
     */

    DeclPtr<Type> resolveType(ast::Type& type);

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

    /*
     * `n: I32` - the type a const parameter may be of, resolved and checked.
     *
     * Null after reporting. The admissible set is the integer types, which is what every position
     * that exists needs - Implementation-Const-Generics.md §2.5. It is a *semantic* rule and not a
     * syntactic one precisely so that it can be loosened one type at a time.
     */
    DeclPtr<Type> resolveConstParameterType(ast::Type& written);

    Constant resolveConstant(ast::Expr& expr, DeclPtr<Type> hint);
    Constant resolveBoolean(ast::Expr& expr, DeclPtr<Type> hint);
    Constant resolveLiteral(ast::Expr& expr, DeclPtr<Type> hint, bool negative);
    bool isBooleanConstructor(ast::Expr& expr);

    /*
     * resolve_decl.cpp - pass one, and the walk that orders it.
     */

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

    // The phases, in the order the dependencies between them force - `declare` says what each one
    // has to be able to assume about the ones before it.
    void declareRecordFields(ast::Module& file);
    void declareClasses(ast::Module& file);
    void declareClassMembers(ast::Module& file);
    void declare(ast::Module& file);

    void declareGlobals(ast::Decl& declaration);
    void declareFields(ast::Decl& declaration, Size& next);

    // `data Boxed(a = I32)` - the head's defaults, read once every parameter of it is bound.
    void declareDefaults(ast::ParseList<ast::GenParam> parameters, DeclPtr<GenContext> gen);

    // `data (Eq(a)) Set(a)` - the head's own constraints, which every application has to prove.
    void declareHeadConstraints(ast::Decl& declaration, RecordType& record);

    void readAttributes(ast::Decl& declaration, bool* explicitDrop, bool* local, StringView on);

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
                           SlotScope& slots, Occurrence& into);

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

    // Whether an instance head reaches a type this module declares - the orphan rule, asked where a
    // class takes a capability away.
    bool headNamesLocalType(const SmallArray<DeclPtr<Type>, 4>& arguments);

    // One written instance member against the class's declaration of it, at this head. Reports the
    // signature the author would have to write.
    bool checkMemberSignature(Function& written, Function& expected, const Substitution& with,
                              LocationId source);
    String signatureText(Function& function, const Substitution& with);

    // The three the compiler reaches for, where nothing declared them - `Class`, and the note on
    // `CoreClass`. Called once, after every file's classes are in.
    void synthesizeCoreClasses();

    // A class by name, module-local and then the three the compiler reaches for. Reports where
    // there is none, because both callers - an instance head and a constraint - want the same
    // message.
    DeclPtr<Class> lookupClass(StringId name, LocationId source);

    // A callable member of one of the three the compiler reaches for - `copy`. See the definition
    // for why a teardown's member is not one.
    DeclPtr<Function> lookupCoreMember(StringId name, U32 arity);

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
     */
    Place* resolvePlace(Builder& builder, ast::Expr& expr, bool complain);

    // `@local data` - whether this module may reach inside the record. See resolve_expr.cpp.
    bool reachesInside(RecordType& record, LocationId source, StringView what);

    Node* resolveField(Builder& builder, ast::Expr& expr, Request wanted);
    Node* resolveConstruction(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint);
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
    bool checkCallShape(ast::AppExpr& written, Function& callee, LocationId source);

    /*
     * One argument per declared parameter, resolved against it - the *one* argument loop.
     *
     * Shared by the ordinary call and the lens call, which is the point: every rule that depends on
     * a parameter's convention - what a bare name filling it does, what happens to an owner handed
     * to a position that only borrows - is written here and nowhere else. The shipping stage has
     * two of these loops and the rules it added to one of them are missing from the other.
     */
    bool resolveArguments(Builder& builder, ast::AppExpr& written, Function& callee,
                          SmallArray<Node*, 8>& into, TypeArguments& typeArguments);

    // What a call has to prove about the types it chose - see the definition, which is where the
    // two answers and the deliberately absent third are argued.
    bool dischargeRequirements(Function& callee, TypeArguments& typeArguments, LocationId source);
    // `a.field: b` proved for one argument list - the property half, and the same two answers in the
    // same order as the class half beside it.
    bool dischargeProperty(GenProperty property, const Substitution& with, LocationId source);

    // `f: (a) -> b` proved for one argument list - the callable half, beside the other two.
    bool dischargeCallable(GenCallable callable, const Substitution& with, LocationId source);

    bool dischargeRequirement(GenRequirement requirement, const Substitution& with,
                              LocationId source);
    Node* resolvePrefix(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint);
    Node* resolveInfix(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint);
    Node* resolveIf(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue,
                    Request wanted);
    Node* resolveBlock(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue,
                       Request wanted);

    const Operator* findOperator(StringId name);
    Node* climb(Builder& builder, Chain& chain, Size& operandIndex, Size& operatorIndex,
                U8 minimumPrecedence, DeclPtr<Type> hint);
    Node* applyBinary(Builder& builder, LocationId source, BinaryOp op, Node& lhs, Node& rhs);
    Node* shortCircuit(Builder& builder, Chain& chain, Size& operandIndex, Size& operatorIndex,
                       U8 next, LocationId source, Node* condition, const Operator& op);
    Node* climbBool(Builder& builder, Chain& chain, Size& operandIndex, Size& operatorIndex,
                    U8 next, LocationId source);
    Node* constantBool(Builder& builder, LocationId source, bool value);
    void closeArm(Builder& builder, Block& end, Block& join, Node* value, LocationId source,
                  Carry& carry, const SmallArray<Node*, 4>& carried);

    /*
     * A value of a type with a teardown, given storage of its own.
     *
     * The rule this exists for is one sentence: **an owner lives in a place**. A value that is only
     * ever a node result has no place for a drop to name, no move path for the initialization
     * dataflow to track and nothing for a borrow to be rooted in - so a call result, an aggregate or
     * a merge that produces one is written into a local the moment it is made, and everything after
     * that is the ordinary machinery.
     *
     * Called from exactly the three positions that can produce one and not consume it: a discarded
     * statement, an argument filling a borrowed parameter, and the target of a projection.
     */
    Place* hold(Builder& builder, Node& value, LocationId source);

    /*
     * The shape `if`, `&&` and `||` share: two arms, and a merge that takes what they produced.
     *
     * A member template rather than a function, because both callers are in resolve_expr.cpp and
     * both hand it lambdas - one resolving an AST arm, the other climbing a span of an operator
     * chain that no AST node corresponds to.
     *
     * The jumps are emitted *after* both arms have been resolved, and that is not tidiness. The
     * merge's parameter list has to exist before an edge can supply it and its type is what the arms
     * decide, so the block each arm ended in is remembered and returned to. `Node::id` is creation
     * order rather than position, so a jump built late and standing early is exactly as verifiable.
     *
     * An arm that left by itself - a `return`, or an error already reported - supplies nothing, and
     * the merge is then reached from one side or from neither. Neither is `unreachable`: the join
     * really is, and saying so is what keeps the caller from appending to a block nothing enters.
     */
    template<class T, class F>
    Node* branchValue(Builder& builder, LocationId source, Node& condition, DeclPtr<Type> hint,
                      T&& whenTrue, F&& whenFalse) {
        // What the walk is holding across this branch - `Carry`, and the reason `Held` exists. Taken
        // before the blocks, because it is the state at the branch that crosses it.
        Carry carry(*this, source);

        auto thenBlock = builder.addBlock(StringId());
        auto elseBlock = builder.addBlock(StringId());
        auto join = builder.addBlock(StringId());

        carry.declare(builder, *thenBlock);
        carry.declare(builder, *elseBlock);

        SmallArray<Operand, 8> entry;
        carry.edge(builder, entry);
        builder.addBranch(source, builder.resultOf(condition, UseMode::Inspect), *thenBlock,
                          *elseBlock, entry.pointer(), entry.size());

        builder.setBlock(*thenBlock);
        carry.arrive(builder, *thenBlock, 0);
        auto thenValue = whenTrue(hint);
        auto thenEnd = builder.currentBlock();
        auto thenLive = thenValue && !builder.isTerminated();

        // What the carried values are at the *end* of this arm, which is not what they are at the
        // end of the other one and is not what they will be when the jumps are emitted below.
        SmallArray<Node*, 4> thenCarried;
        carry.save(thenCarried);

        builder.setBlock(*elseBlock);
        carry.arrive(builder, *elseBlock, 0);
        auto elseValue = whenFalse(thenValue ? valueType(*thenValue) : hint);
        auto elseEnd = builder.currentBlock();
        auto elseLive = elseValue && !builder.isTerminated();

        SmallArray<Node*, 4> elseCarried;
        carry.save(elseCarried);

        if(thenLive && elseLive && !sameType(valueType(*thenValue), valueType(*elseValue))) {
            context.diagnostics.error("the two arms answer %@ and %@, which are different types"_v,
                                      source, typeText(context, decl(), valueType(*thenValue)),
                                      typeText(context, decl(), valueType(*elseValue)));
            elseLive = false;
        }

        /*
         * And the same *ownership*, exactly.
         *
         * A merge parameter has one occurrence and every edge into it supplies that one - which the
         * verifier states as an equality and this is the source-level half of. Both arms were
         * resolved against the same `Request`, so they agree on the interesting half already: a
         * position that copies copies on both sides and a position that borrows borrows on both.
         * What they can still disagree about is *whose* storage they borrowed - one arm a published
         * parameter and the other a local - and §2.5's answer to that is the join, a loan weakened
         * to what both sides can promise.
         *
         * The join is deliberately not built. Weakening a contract can change how the value is
         * carried - a shared call-bounded loan of a trivially copyable type is a register where a
         * published one is an address - so an arm whose contract differs from the join needs a
         * reborrow *on its edge*, which is a node this milestone has no reason to have anywhere
         * else. Refusing is the honest report until it does; accepting and handing the merge one
         * arm's contract is the silent version of the same gap.
         */
        if(thenLive && elseLive &&
           valueOccurrence(*thenValue).contract != valueOccurrence(*elseValue).contract) {
            context.diagnostics.error(
                "the two arms borrow under different terms, and one value that is either is not implemented"_v,
                source);
            elseLive = false;
        }

        /*
         * The merge's own parameter first, then the carried ones.
         *
         * The order is what `closeArm` reads: the arm's value fills position zero and the carried
         * values follow it, so a reader of the finished body finds the `if`'s answer where an `if`
         * with nothing crossing it would have put it.
         */
        Node* argument = nullptr;
        if(thenLive || elseLive) {
            // The arm's whole occurrence rather than an owned one of its type. A merge of two loans
            // is a loan, and saying otherwise here is what made `if c then p.left else p.right`
            // claim to own storage the arms only borrowed.
            auto& occurrence = valueOccurrence(thenLive ? *thenValue : *elseValue);
            argument = builder.addBlockArgument(*join, occurrence, source);
        }

        carry.declare(builder, *join);

        closeArm(builder, *thenEnd, *join, thenLive ? thenValue : nullptr, source, carry, thenCarried);
        closeArm(builder, *elseEnd, *join, elseLive ? elseValue : nullptr, source, carry, elseCarried);

        builder.setBlock(*join);
        carry.arrive(builder, *join, argument ? 1 : 0);
        if(!argument) builder.addUnreachable(source);

        return argument;
    }

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
    void resolveScoped(Builder& builder, const ast::VarDecl& binding);
    bool resolveLensCall(Builder& builder, const ast::VarDecl& binding, ast::Expr& call,
                         Function& callee);
    void resolveYield(Builder& builder, ast::Expr& expr);

    /*
     * Every path through a lens's frame hands over exactly once - Analysis-Lens.md §7.1's rule for
     * a transparent lens.
     *
     * A forward walk over the frame's blocks rather than a count of `yield` statements, because the
     * two questions the rule asks are both about paths: a lens that hands over on one arm of an `if`
     * and not the other is a *skipping* lens, which needs the `Try` instance milestone 6 has, and a
     * lens that hands over twice would run its continuation twice with one frame.
     */
    bool checkFrameYield(Builder& builder, LensFrame& frame, Block* exit);

    /*
     * One more copy of an open frame's cleanup, for an exit that is not falling out.
     *
     * `depth` names the frame in `lensFrames`. The callee's scope is put back for the length of it,
     * because the statements are the callee's and the exit is written in the caller's - which is the
     * same swap `yield` performs, in the other direction.
     */
    void runCleanup(Builder& builder, U32 depth);

    // Every frame an exit written here leaves, innermost first. Each runs its own copy of its
    // cleanup, and all of them are held at `LensFrame::Cleanup` for the whole unwinding, so that a
    // `return` written inside one of those copies leaves past the frames outside it rather than
    // past these a second time.
    void unwindFrames(Builder& builder);
    void nameLinearMembers(Builder& builder, Place& place);
    bool resolveDispose(Builder& builder, ast::Expr& expr);
    void resolveDeclaration(Builder& builder, ast::Expr& expr);
    void resolveAssign(Builder& builder, ast::Expr& expr);
    void resolveWhile(Builder& builder, ast::Expr& expr);
    void resolveReturn(Builder& builder, ast::Expr& expr);
    bool checkResultContract(Node& value, LocationId source);

    void resolveInitializers(ast::Module& file);
    void resolveGlobals(ast::Decl& declaration, Size& next);
    void resolveBodies(ast::Module& file);
    void resolveBody(Function& function, ast::Decl& declaration);
    void resolveInstanceBody(Instance& instance, ast::Decl& declaration);

    void resolveClassBody(Class& klass, ast::Decl& declaration);
    void finish(Builder& builder, Function& function, bool unit, LocationId source);
    bool enteredFrom(Body& body, MirBase base, Block& block);

    /*
     * Shared.
     */

    Local* findLocal(StringId name);

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

    // Whether a node produced the type this position asked for. One rule and one message, because
    // "the wrong type arrived here" is the same failure wherever it happens and a reader comparing
    // two diagnostics about it should not have to notice they are two.
    bool checkType(Node& node, DeclPtr<Type> expected, LocationId source, StringView what);
};

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
     * Two things can answer, in this order, and they are the same two `dischargeRequirement` splits
     * on. A **concrete** deciding prefix selects an instance and reads the rest off its head. A
     * prefix that is one of the *enclosing* declaration's variables has no instance to read anything
     * off - `c` inside a body generic in `c` is not a type - and what answers instead is the
     * requirement that enclosing signature declared, which promised the same thing about the same
     * variables.
     *
     * Repeated until nothing moves, so `Contiguous(c, a)` deciding `a` and `Elem(a, e)` then
     * deciding `e` resolve in one call rather than in the order the constraints happen to be written.
     */
    void fillDetermined(LocationId source);

    // The first variable nothing decided, for the caller to report against its own positions; null
    // where every one is bound. Reported rather than picked, because an argument chosen at random is
    // a type chosen at random.
    DeclPtr<Type> undecided() const;

    Resolver& resolver;
    DeclPtr<GenContext> owner = nullptr;
    SmallArray<DeclPtr<Type>, 4> arguments;
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
