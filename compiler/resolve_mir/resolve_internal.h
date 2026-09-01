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
// `&&`'s precedence - `Core.yana`'s own fixity, named because two places have to agree about it: the
// table below, and the condition walk, which climbs one rung above it to take a single conjunct.
static constexpr U8 kConjunctionPrecedence = 2;

enum class OperatorForm: U8 {
    Binary,      // an ordinary two-operand computation
    ShortAnd,    // `&&` - the right operand runs only if the left was True
    ShortOr,     // `||` - the right operand runs only if the left was False
};

/*
 * One infix operator, as the chain walk sees it.
 *
 * Two halves with two sources. **What it does** - the form and the instruction - is the builtin
 * table's and stays there until an operator can dispatch to a class member. **How it groups** - the
 * precedence and the associativity - is a *declaration*: `infixl 8 +` is written in a module and
 * looked up through the imports like any other name, and the table's own numbers are the fallback
 * for an operator nothing in scope declared. `Resolver::findOperator` is where the two are put
 * together, and it is the only place either is read.
 *
 * By value in `Chain` rather than by pointer, because a declared grouping is not a row of the table:
 * it is this operator as it is spelled *here*.
 */
struct Operator {
    StringView text;
    U8 precedence;
    OperatorForm form;
    BinaryOp op;
    Assoc assoc = Assoc::Left;
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
 * The three the spec names, and the one it leaves out is the one this stage refuses: a **consumed**
 * capture makes the environment own something, and an environment that owns something has to
 * outlive the frame that built it, which is the heap placement nothing here selects.
 */
enum class CaptureKind: U8 {
    Copy,
    Shared,
    Unique,
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
                     ast::ModuleProvider* provider, Array<ast::ModuleGroup*>& groups);
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
     * Runs `pass` over every file of every module, with the position set to each in turn.
     *
     * The one walk the declaration phases are written against, so that "in what order does a pass
     * see the program" is answered once. Which file is being read is what an import's scope is
     * relative to, and setting it here is what keeps `Module::activeFile` from being something each
     * pass has to remember.
     */
    template<class Pass>
    void eachFile(Pass&& pass) {
        for(U32 index = 0; index < groups.size(); index++) {
            ModuleScope scope(*this, program.modules[index]);

            for(U16 file = 0; file < U16(groups[index]->files.size()); file++) {
                current->activeFile = file;
                pass(*groups[index]->files[file]);
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

    DeclBase decl() { return *program.decl; }
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

    // `infixl`/`infixr` - see the definition, and `mir::findFixity` for the reading side.
    void declareFixities(ast::Module& file);

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

    // `@lazy` at the declaration - the occurrence rewritten into the thunk the callee holds, and the
    // three shapes the marker cannot describe. See resolve_sig.cpp.
    bool declareDeferred(Function& function, const ast::Arg& argument, Occurrence& occurrence);

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

    // The orphan rule - see the definition. Reports rather than answering, since there is nothing a
    // caller does differently about a badly placed instance than about a well placed one.
    void checkOrphanRule(Class& declared, const SmallArray<DeclPtr<Type>, 4>& arguments,
                         LocationId source);

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

    // A named function in a value position - G7. The hint is what picks the arity, which is why it
    // is required rather than advisory here; see the note at the implementation.
    Node* resolveFunValue(Builder& builder, StringId name, DeclPtr<Type> hint, LocationId source);

    /*
     * resolve_lambda.cpp - a lambda, and the environment its captures live in.
     */

    // A `@lazy` parameter is forced at most once on any path - the walk over the finished body, and
    // the one place call-by-name is distinguished from call-by-need. See resolve_stmt.cpp.
    void checkForced(Builder& builder, Function& function);

    Node* resolveLambda(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint);

    // The expression written at a `@lazy` position: a thunk, or the thunk this body was already
    // handed. See resolve_expr.cpp.
    Node* resolveDeferred(Builder& builder, ast::Expr& written, DeclPtr<Type> expected);

    // A `@lazy` argument - the nullary thunk the spec says the expression compiles to, which is a
    // lambda with no written parameter list.
    Node* resolveThunk(Builder& builder, ast::Expr& body, DeclPtr<Type> hint, LocationId source);

    // Which of the two the author wrote, and the only thing below the lift that depends on it: the
    // symbol a dump shows, and the refusals, which name the construct rather than what it becomes.
    enum class LiftKind: U8 { Lambda, Thunk };

    Node* liftFunction(Builder& builder, const ast::ParseList<ast::Arg>* args, ast::Expr& body,
                       bool implicitReturn, DeclPtr<Type> hint, LocationId source, LiftKind kind);

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
    bool dischargeCallable(Function& callee, GenCallable callable, const Substitution& with,
                           LocationId source);

    bool dischargeRequirement(GenRequirement requirement, const Substitution& with,
                              LocationId source);
    Node* resolvePrefix(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint);
    Node* resolveInfix(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint);
    Node* resolveIf(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue,
                    Request wanted);

    /*
     * Patterns, `is` and `match` - `resolve_match.cpp`, where every one of these is argued.
     */
    bool resolvePattern(Builder& builder, ast::Pat& pat, Place& pivot,
                        SmallArray<PatternBinding, 4>& into, Block* onFail, Carry& carry);
    bool resolveConstructorPattern(Builder& builder, ast::Pat& pat, Place& pivot,
                                   SmallArray<PatternBinding, 4>& into, Block* onFail,
                                   Carry& carry);
    void constructorFields(ast::Pat& pat, RecordType& record, const RecordCon& arm,
                           SmallArray<ast::Pat*, 4>& into, bool report);
    Node* patternBound(Builder& builder, ast::Pat& pat, DeclPtr<Type> type);
    bool comparePattern(Builder& builder, BinaryOp op, Place& pivot, ast::Pat& bound, Block* onFail,
                        Carry& carry, LocationId source);
    void failIfNot(Builder& builder, Node& condition, Block& onFail, Carry& carry,
                   LocationId source);
    // `from` is how much of the list is already in scope, which a conjunction needs: it binds
    // between its own spans, and the list accumulates across all of them.
    void bindPattern(Builder& builder, const SmallArray<PatternBinding, 4>& made, U32 from = 0);
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
    Place* matchPivot(Builder& builder, ast::Expr& expr);
    Node* resolveIs(Builder& builder, ast::Expr& expr);
    Node* resolveMatch(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue,
                       Request wanted);
    Node* resolveBlock(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint, bool wantValue,
                       Request wanted);

    // What an operator does and how it groups, put together - see `Operator`. Nothing where no
    // table row and no declaration in scope names it.
    Maybe<Operator> findOperator(StringId name);

    /*
     * An infix expression flattened into its operands and operators - `Chain`.
     *
     * False after reporting, which is an operator this stage does not have. Shared by the value walk
     * and the condition walk, because a conjunction has to be *split* before it is climbed and both
     * need the same fixity answer to do it.
     */
    bool flattenChain(ast::Expr& expr, Chain& chain);

    // Whether a flattened chain is a conjunction at its top level - the question `resolveCondition`
    // splits on. See resolve_expr.cpp.
    bool splitsAtConjunction(Chain& chain);
    Node* climb(Builder& builder, Chain& chain, Size& operandIndex, Size& operatorIndex,
                U8 minimumPrecedence, DeclPtr<Type> hint);
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
     */
    void closeArm(Builder& builder, Block& end, Block& join, Node* value, bool merged,
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
     * Called from exactly the three positions that can produce one and not consume it: a discarded
     * statement, an argument filling a borrowed parameter, and the target of a projection.
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
        // What the walk is holding across this construct - `Carry`, and the reason `Held` exists.
        // Taken before any block is made, because it is the state at the branch that crosses it.
        Carry carry(*this, source);

        /*
         * One entry block per alternative *after the first*, which is entered by falling into it.
         *
         * The first alternative's tests belong where the construct was written - there is nothing
         * between the pivot and them - so giving it an entry of its own would be a jump to a block
         * with one predecessor and one purpose.
         */
        SmallArray<Block*, 8> entries;
        entries.push(nullptr);

        for(U32 i = 1; i < count; i++) {
            auto block = builder.addBlock(StringId());
            carry.declare(builder, *block);
            entries.push(block);
        }

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

            auto value = emit(i, i + 1 < count ? entries[i + 1] : nullptr, armHint, carry);

            ends.push(builder.currentBlock());
            values.push(value && !builder.isTerminated() ? value : nullptr);
            carry.save(carried);

            // The first alternative that answered is what the rest are checked against, which is the
            // one-directional rule the two-armed form read off its `then` arm.
            if(wantValue && values[i] && !armHint) armHint = valueType(*values[i]);
        }

        Node* argument = nullptr;
        if(wantValue) argument = mergeArms(builder, *join, values, source);

        carry.declare(builder, *join);

        for(U32 i = 0; i < count; i++) {
            closeArm(builder, *ends[i], *join, argument ? values[i] : nullptr, argument != nullptr,
                     source, carry, carried.pointer() + i * carry.count());
        }

        builder.setBlock(*join);
        carry.arrive(builder, *join, argument ? 1 : 0);

        // A merge every alternative left is unreachable and still needs a terminator, which is what
        // `finish` gives it - but only where nothing follows it. A statement form is followed by the
        // rest of its block.
        if(wantValue && !argument) builder.addUnreachable(source);

        return argument;
    }

    /*
     * The merge's own parameter, and the three things every live alternative has to agree about.
     *
     * Split out of `armsValue` because it is the *checking* half and the template is the shape half:
     * what it does is read one occurrence off the alternatives and report every alternative that
     * does not have it, which is a walk over a list rather than anything about how the blocks were
     * laid out. Alternatives that disagree are nulled in place, so the merge is built from the ones
     * that survive and each of the others reaches it as `unreachable`.
     */
    Node* mergeArms(Builder& builder, Block& join, SmallArray<Node*, 8>& values, LocationId source);

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
    Function* namedCallee(ast::Expr& expression);
    Function* lensCallee(ast::Expr& expression);

    /*
     * Where the flat form's continuation stops - Analysis-Lens.md §11.2 and §11.4.
     *
     * Answers the index of the last statement inside the extent, which is `at` itself when the
     * extent is empty and the call is therefore §11.3's diagnostic.
     */
    U32 flatExtent(ast::ParseList<ast::Expr> statements, U32 count, U32 at, StringId handed);
    void reportEmptyExtent(LocationId source, Function& callee);

    U32 resolveFlatLensCall(Builder& builder, ast::ParseList<ast::Expr> statements, U32 to, U32 at,
                            const ast::VarDecl& binding, ast::Expr& call, Function& callee,
                            bool wantValue);
    /*
     * The whole of a call that opens a continuation region, less what runs inside it.
     *
     * `inside(region, join)` is called with the entry current and the hand-over bound, and is the
     * one thing a lens call and a `for` over an iterator disagree about: a written or derived
     * extent against a loop body. Everything else - the arguments, the type arguments, the region,
     * what arrives, the binding's storage, how the caller rejoins - is one construct, so it is
     * written once.
     */
    template<class F>
    bool handoverCall(Builder& builder, ast::Expr& call, Function& callee,
                      Continuation::Multiplicity multiplicity, StringId name,
                      LocationId nameSource, ast::ParseList<ast::Alt> alternatives, F&& inside);

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
                         Function& callee, const Extent& extent);
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
    void resolveBodies(ast::Module& file);
    void resolveBody(Function& function, ast::Decl& declaration);
    void resolveInstanceBody(Instance& instance, ast::Decl& declaration);

    void resolveClassBody(Class& klass, ast::Decl& declaration);
    // `mark` is taken where this body's walk began - see `ErrorMark`. It is what tells a path that
    // falls off the end of a *broken* body from one that falls off the end of a whole one.
    void finish(Builder& builder, Function& function, bool unit, LocationId source,
                const ErrorMark& mark);
    bool enteredFrom(Body& body, MirBase base, Block& block);

    /*
     * Shared.
     */

    /*
     * The storage a name in this body names, or null.
     *
     * `bound` says which kind of null it is: false is a name nothing in the body declared, and true
     * is one declared by a binding whose initializer failed. The pair matters at exactly one place -
     * the "unknown name" report - and everywhere else a null is a null.
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
