#pragma once

#include "builder.h"
#include "../parse/ast.h"

/*
 * The AST -> IR translation for one function body.
 *
 * ExprResolver is shared by the four expr_*.cpp files rather than being local to one of them:
 *
 *   expr.cpp            the resolve() dispatch, literals, conversions, control flow and `let`.
 *   expr_call.cpp       operator fixity, precedence climbing, and call/overload selection.
 *   expr_construct.cpp  places and projections, tuple and record construction, field access.
 *   expr_pat.cpp        patterns, refutability, exhaustiveness, and `match`.
 *
 * The division follows what the resolver has to know rather than what it produces: everything
 * in expr_construct.cpp is about addressing storage, everything in expr_pat.cpp is about
 * deciding which of several shapes a value has, and the two meet only through Place.
 */

struct Binding {
    StringId name = 0;
    ModulePtr<Value> value = nullptr;
};

// One class function that fits a call, together with what its class's type variables had to be
// and the instance that supplies them. `instance` is null when the signature fits but nothing
// implements it for these types, which is a different diagnostic from "wrong function".
struct ClassMatch {
    GlobalPtr<TypeClass> typeClass = nullptr;
    ModulePtr<ClassInstance> instance = nullptr;
    Array<TypePtr> args;
    U16 index = 0;
};

struct LoopTarget {
    ModulePtr<Block> continueBlock;
    ModulePtr<Block> breakBlock;
};

// What resolving one pattern proved about it. `Never` means the pattern cannot match this pivot
// at all (a type error, already reported); `Always` means no test was emitted, so the following
// alternatives are unreachable; `Maybe` means a test was emitted and control may reach `onFail`.
enum class PatternResult: I8 {
    Never = -1,
    Maybe = 0,
    Always = 1,
};

// One alternative's contribution to a branching expression's result: the block control leaves
// through, and the value produced there. Collected by if/multi-if/match alike, then unified into
// a single phi by finishBranches().
struct BranchArm {
    ModulePtr<Block> end;
    ModulePtr<Value> value;
    LocationId source;
};

struct ExprResolver {
    ExprResolver(Context& context, Module& module, Function& function):
        context(context), module(module), function(function), parse(module.parse),
        global(*module.types), local(*module.arena), current(module.entry(function) - *module.arena) {}

    /*
     * Building blocks.
     */

    Block& block() { return *local[current]; }
    ModulePtr<Value> ref(Value* value) { return value - local; }
    TypePtr valueType(ModulePtr<Value> value) { return value ? local[value]->type : module.scalar.unit; }

    template<class T, class... Args>
    T* emit(LocationId source, StringId name, TypePtr type, Args&&... args) {
        return addInst<T>(module, function, block(), source, name, type, forward<Args>(args)...);
    }

    // create() + append() is emit() split in two, for the instructions whose operands are filled
    // in between the two halves - see builder.h.
    template<class T, class... Args>
    T* create(LocationId source, StringId name, TypePtr type, Args&&... args) {
        return createInst<T>(module, function, block(), source, name, type, forward<Args>(args)...);
    }

    void append(Inst* inst) { block().add(module, inst); }

    template<class T, class... Args>
    ModulePtr<Value> constant(LocationId source, TypePtr type, Args&&... args) {
        return ref(addConstant<T>(module, function, block(), source, type, forward<Args>(args)...));
    }

    void terminate(Inst* inst);
    ModulePtr<Block> addBlock() { return function.addBlock(module) - local; }

    /*
     * Values and conversions (expr.cpp).
     */

    ModulePtr<Value> find(StringId name);
    ModulePtr<Value> makeInt(LocationId source, TypePtr type, U64 value);
    ModulePtr<Value> makeFloat(LocationId source, TypePtr type, F64 value);
    ModulePtr<Value> convert(ModulePtr<Value> value, TypePtr target, LocationId source, bool implicit = true);

    // Whether convert() would succeed implicitly, without reporting anything if it wouldn't.
    bool convertible(ModulePtr<Value> value, TypePtr target, LocationId source);

    // One step of `typeClass`'s conversion, or null when no instance relates these two types.
    // Never a chain: `A -> B -> C` is not searched for, which is what keeps conversion as
    // predictable as the no-backtracking rule the rest of resolution follows.
    ModulePtr<Value> emitConversion(GlobalPtr<TypeClass> typeClass, StringId method, ModulePtr<Value> value,
                                    TypePtr target, LocationId source);

    // The unique `Widen` upper bound of two types, or null when neither widens to the other.
    // This is the one place a conversion may decide which overload matches: the positions bound
    // to a single class variable are unified before the instance is looked for, which is what
    // makes `1 + 2.5` reach Num(Float) rather than no instance at all.
    TypePtr commonWiden(TypePtr lhs, TypePtr rhs);

    /*
     * Literals (expr.cpp).
     */

    // A fresh literal variable carrying one literal class.
    TypePtr literalVariable(GlobalPtr<TypeClass> literalClass);

    // The type a literal variable takes when nothing else decided one, or null when its classes
    // have no default they agree on. Pure: speculative overload matching asks this too.
    TypePtr literalDefault(TypePtr type);

    // `type` with a literal variable replaced by its default. Applied wherever an inferred type
    // is about to be committed to - a class's type argument, a specialization's, a branch join.
    TypePtr settleType(TypePtr type);

    // `value` built at the type its literal variable defaults to. Everything else passes through.
    ModulePtr<Value> settle(ModulePtr<Value> value, LocationId source);

    // Whether a literal variable may become `target`: it needs an instance of each of its classes,
    // and a type variable needs the enclosing function to require them instead.
    bool literalFits(TypePtr literal, TypePtr target);

    // One literal variable carrying the classes of both. `1 + 2.5` is why this exists.
    TypePtr mergeLiterals(TypePtr lhs, TypePtr rhs);

    // Builds a literal at `target` by calling the class function that constructs it. Core's
    // instances are intrinsics that fold a constant argument, so a literal at a primitive type is
    // still one constant and the IR is what it always was.
    ModulePtr<Value> materializeLiteral(ModulePtr<Value> value, TypePtr target, LocationId source);

    ModulePtr<Value> resolve(const ast::Expr& expr, TypePtr target = nullptr, bool used = true);
    ModulePtr<Value> resolveLiteral(const ast::Expr& expr, TypePtr target);
    ModulePtr<Value> resolveInteger(LocationId source, TypePtr target, U64 value);
    ModulePtr<Value> resolveDecimal(LocationId source, TypePtr target, F64 value);
    // Resolves a condition into a branch. On return, `current` is the block reached when the
    // condition holds - which is where an `is` test's bindings are live - and `onFail` is the
    // block reached when it does not. A caller that already has a block to fail into (a loop's
    // exit) passes it in; one that does not passes null and is given a fresh one.
    //
    // A condition is either an expression whose type has a `Truth` instance or an `is` test, which
    // are the same idea named two ways: `if x` asks whether x matches the pattern its type
    // considers non-empty, and `if x is p` names the pattern instead.
    PatternResult resolveCondition(const ast::Expr& expr, ModulePtr<Block>& onFail);

    // The `Truth` instance of this value's own type, applied. Never reached through a conversion:
    // what `if x` means is decided by x's type alone.
    ModulePtr<Value> truthy(ModulePtr<Value> value, LocationId source);

    // `expr is pat` outside condition position, where there is no branch for its bindings to live
    // in: an ordinary Bool, with what the pattern bound discarded.
    ModulePtr<Value> resolveIs(const ast::Expr& expr, const ast::IsExpr& test, bool used);

    ModulePtr<Value> resolveIf(const ast::Expr& expr, const ast::IfExpr& branch, TypePtr target, bool used);
    ModulePtr<Value> resolveMultiIf(const ast::Expr& expr, ast::ParseList<ast::IfCase> cases, TypePtr target, bool used);
    void resolveWhile(const ast::WhileExpr& loop);
    ModulePtr<Value> resolveDecl(ast::ParseList<ast::VarDecl> declarations, TypePtr target, bool used);
    void resolveReturn(const ast::Expr& expr);

    // Joins the arms of a branching expression: picks the result type, converts each arm to it
    // in the arm's own block, jumps them all to one join block, and produces the phi. The
    // conversions belong here rather than where each arm was resolved, because the type they
    // convert to is only known once every arm has been seen, and a conversion has to be emitted
    // in the predecessor it flows from rather than after the join.
    ModulePtr<Value> finishBranches(Array<BranchArm>& arms, LocationId source, bool used);

    /*
     * Calls and operators (expr_call.cpp).
     */

    ModulePtr<Value> resolveBinary(const ast::Expr& expr, const ast::InfixExpr& binary, TypePtr target);
    ModulePtr<Value> resolvePrefix(const ast::Expr& expr, const ast::PrefixExpr& prefix, TypePtr target, bool convertResult = true);
    ModulePtr<Value> resolvePrecedence(Array<const ast::Expr*>& operands, Array<StringId>& operators, Size& operandIndex, Size& operatorIndex, U8 minimumPrecedence);
    ModulePtr<Value> resolveCall(const ast::Expr& expr, const ast::AppExpr& call, TypePtr target, bool convertResult = true);
    ModulePtr<Value> emitCall(StringId name, Buffer<ModulePtr<Value>> args, LocationId source, TypePtr target = nullptr, StringId resultName = 0);
    ModulePtr<Value> emitDirectCall(ModulePtr<Function> callee, Buffer<ModulePtr<Value>> args, LocationId source, TypePtr target = nullptr, StringId resultName = 0);

    // A call to a generic function: infers its type arguments from the call, then either
    // instantiates it or - when this body is itself generic and the arguments are not concrete
    // yet - defers the whole decision to the instantiation that will make them concrete.
    ModulePtr<Value> emitGenericCall(ModulePtr<Function> callee, Buffer<ModulePtr<Value>> args, LocationId source,
                                     TypePtr target, StringId resultName);

    // A class function whose instance cannot be chosen here, because the types it would be chosen
    // by are this function's own type variables. Records the requirement and emits InstGenCall.
    ModulePtr<Value> emitGenericDispatch(ClassMatch& match, Buffer<ModulePtr<Value>> args, LocationId source,
                                         StringId resultName);

    bool bindPosition(TypePtr pattern, TypePtr actual, Array<TypePtr>& bindings, bool widen);
    bool matchClassFun(const ClassFunRef& reference, Buffer<ModulePtr<Value>> args, TypePtr target, ClassMatch& resolved);

    // Whether a plain function can serve this call - the same question matchClassFun asks of a
    // class function, so that both halves of an overload set are judged by one rule.
    bool matchFunction(ModulePtr<Function> callee, Buffer<ModulePtr<Value>> args, TypePtr target, LocationId source);

    ModulePtr<ClassInstance> selectInstance(GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args);

    /*
     * Storage and aggregates (expr_construct.cpp).
     */

    ModulePtr<Value> allocate(TypePtr type, LocationId source, StringId name = 0);
    Place placeFor(ModulePtr<Value> value, LocationId source);
    Place project(Place place, ProjectionKind kind, U16 index);
    TypePtr placeType(const Place& place);
    ModulePtr<Value> load(Place place, LocationId source, StringId name = 0);
    void initialize(Place place, ModulePtr<Value> value, LocationId source);

    ModulePtr<Value> resolveTuple(const ast::Expr& expr, ast::ParseList<ast::TupArg> args, TypePtr target);
    ModulePtr<Value> resolveConstruct(const ast::Expr& expr, const ast::ConExpr& construct, TypePtr target);
    TypePtr constructedType(ConstructorRef reference, ast::ParseList<ast::TupArg> args, TypePtr target, Array<ModulePtr<Value>>& resolved, LocationId source);
    ModulePtr<Value> resolveField(const ast::Expr& expr, const ast::FieldExpr& field);
    bool fillTuple(Place place, TupType& tuple, ast::ParseList<ast::TupArg> args, LocationId source);

    /*
     * Patterns (expr_pat.cpp).
     */

    ModulePtr<Value> resolveMatch(const ast::Expr& expr, const ast::MatchExpr& match, TypePtr target, bool used);

    // One declaration's pattern, bound to the value its initializer produced, together with the
    // alternatives that cover what the pattern does not. Everything a `let` needs beyond
    // evaluating its initializer, which is resolveDecl's half.
    void resolveBinding(const ast::VarDecl& declaration, ModulePtr<Value> value);

    // Emits the tests `pattern` needs and binds the names it introduces. A null `onFail` means
    // the pattern is already known to match every value that can reach it - either because it is
    // irrefutable, or because the alternatives before it ruled everything else out - so no test
    // is emitted and there is no failure edge to take.
    PatternResult resolvePattern(const ast::Pat& pattern, ModulePtr<Value> pivot, ModulePtr<Block> onFail) {
        return resolvePattern(pattern, pivot, onFail, bindings.size());
    }

    // `bindingBase` is where the bindings this one pattern introduces start, which is what makes
    // a name it binds twice tellable from one that merely shadows an outer binding. The recursion
    // passes its own base down; the entry point above takes it from the scope it was called in.
    PatternResult resolvePattern(const ast::Pat& pattern, ModulePtr<Value> pivot, ModulePtr<Block> onFail,
                                 Size bindingBase);

    PatternResult branchPattern(ModulePtr<Value> condition, ModulePtr<Block> onFail, LocationId source);
    ModulePtr<Value> patternBound(const ast::Pat& pattern, TypePtr target);

    Context& context;
    Module& module;
    Function& function;
    ast::ParseBase parse;
    GlobalBase global;
    ModuleBase local;

    // The block instructions are currently appended to, or null once control cannot reach the
    // code that follows (after a `return`, or a branch every arm of which left through one).
    ModulePtr<Block> current;

    // Scratch state for one body, deliberately not in the module arena: it is gone once the
    // function is resolved, and the arena is a bump allocator that never gives anything back.
    Array<Binding> bindings;
    Array<LoopTarget> loops;
};

// Creates a function that is reached through something other than its own name - a class
// instance's implementation - with a unique name for printing and lowering.
Function* addAnonymousFunction(Module& module, StringId name, LocationId source);
