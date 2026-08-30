#pragma once

#include "build.h"
#include "../parse/ast.h"

namespace mir {

/*
 * The resolver, across the four files it is written in.
 *
 * One struct rather than four, because there is one walk: a declaration reaches a type, a body
 * reaches a declaration, and an expression reaches a place. What the split is about is what a reader
 * is looking for -
 *
 *  - `resolve.cpp`     the driver, written types, and constant expressions;
 *  - `resolve_decl.cpp` pass one, which is what a module declares;
 *  - `resolve_expr.cpp` expressions, which are the things that answer;
 *  - `resolve_stmt.cpp` statements and bodies, which are the things that happen.
 *
 * - and it follows `compiler/resolve`'s own module/expr/body split, which is the one arrangement of
 * this code that has already been read by somebody looking for something.
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
    Array<DeclPtr<Global>> writtenGlobals;
    Array<DeclPtr<Instance>> writtenInstances;
    Array<DeclPtr<RecordType>> writtenRecords;

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

    // The function whose body is being resolved, which `return` needs and nothing else does.
    Function* currentFunction = nullptr;

    DeclBase decl() { return *program.decl; }
    DeclPtr<Module> here() { return &module - *program.decl; }

    void unsupported(LocationId source, StringView what);

    /*
     * resolve.cpp - written types and constant expressions.
     */

    DeclPtr<Type> resolveType(ast::Type& type);
    Constant resolveConstant(ast::Expr& expr, DeclPtr<Type> hint);
    Constant resolveBoolean(ast::Expr& expr, DeclPtr<Type> hint);
    Constant resolveLiteral(ast::Expr& expr, DeclPtr<Type> hint, bool negative);
    bool isBooleanConstructor(ast::Expr& expr);

    /*
     * resolve_decl.cpp - pass one.
     */

    void declareTypes(ast::Module& file);
    void declare(ast::Module& file);
    void declareGlobals(ast::Decl& declaration);
    void declareFields(ast::Decl& declaration, Size& next);
    void declareFunction(ast::Decl& declaration);
    void declareInstance(ast::Decl& declaration);

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

    bool declareArgs(Function& function, ast::Decl& declaration, SlotScope& slots);
    Function* declareSignature(ast::Decl& declaration, StringId name);

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

    Node* resolveField(Builder& builder, ast::Expr& expr, Request wanted);
    Node* resolveConstruction(Builder& builder, ast::Expr& expr, DeclPtr<Type> hint);
    Node* resolveCall(Builder& builder, ast::Expr& expr);
    bool checkCallShape(ast::AppExpr& written, Function& callee, LocationId source);
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
    void closeArm(Builder& builder, Block& end, Block& join, Node* value, LocationId source);

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
        auto thenBlock = builder.addBlock(StringId());
        auto elseBlock = builder.addBlock(StringId());
        auto join = builder.addBlock(StringId());

        builder.addBranch(source, builder.resultOf(condition, UseMode::Inspect), *thenBlock, *elseBlock);

        builder.setBlock(*thenBlock);
        auto thenValue = whenTrue(hint);
        auto thenEnd = builder.currentBlock();
        auto thenLive = thenValue && !builder.isTerminated();

        builder.setBlock(*elseBlock);
        auto elseValue = whenFalse(thenValue ? valueType(*thenValue) : hint);
        auto elseEnd = builder.currentBlock();
        auto elseLive = elseValue && !builder.isTerminated();

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

        Node* argument = nullptr;
        if(thenLive || elseLive) {
            // The arm's whole occurrence rather than an owned one of its type. A merge of two loans
            // is a loan, and saying otherwise here is what made `if c then p.left else p.right`
            // claim to own storage the arms only borrowed.
            auto& occurrence = valueOccurrence(thenLive ? *thenValue : *elseValue);
            argument = builder.addBlockArgument(*join, occurrence, source);
        }

        closeArm(builder, *thenEnd, *join, thenLive ? thenValue : nullptr, source);
        closeArm(builder, *elseEnd, *join, elseLive ? elseValue : nullptr, source);

        builder.setBlock(*join);
        if(!argument) builder.addUnreachable(source);

        return argument;
    }

    /*
     * resolve_stmt.cpp - the things that happen.
     */

    void resolveStatement(Builder& builder, ast::Expr& expr);
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
    void finish(Builder& builder, Function& function, bool unit, LocationId source);
    bool enteredFrom(Body& body, MirBase base, Block& block);

    /*
     * Shared.
     */

    Local* findLocal(StringId name);

    // Whether a node produced the type this position asked for. One rule and one message, because
    // "the wrong type arrived here" is the same failure wherever it happens and a reader comparing
    // two diagnostics about it should not have to notice they are two.
    bool checkType(Node& node, DeclPtr<Type> expected, LocationId source, StringView what);
};

} // namespace mir
