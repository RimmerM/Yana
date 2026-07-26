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

// Which constructors of one record a `match` has already tested. This is the whole of the
// exhaustiveness model for Milestone 2: a bitmask is enough while a match pivots on a single
// value, and it is what tells the last alternative that it needs no test of its own.
struct RecordCoverage {
    TypePtr type = nullptr;
    U64 checked = 0;
    U16 checkedCount = 0;
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
    U8 numericRank(TypePtr type);
    TypePtr commonNumeric(TypePtr lhs, TypePtr rhs, LocationId source);

    ModulePtr<Value> resolve(const ast::Expr& expr, TypePtr target = nullptr, bool used = true);
    ModulePtr<Value> resolveLiteral(const ast::Expr& expr, TypePtr target);
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
    PatternResult resolvePattern(const ast::Pat& pattern, ModulePtr<Value> pivot, ModulePtr<Block> onFail, RecordCoverage* coverage = nullptr);
    PatternResult branchPattern(ModulePtr<Value> condition, ModulePtr<Block> onFail, LocationId source);
    ModulePtr<Value> patternBound(const ast::Pat& pattern, TypePtr target);
    bool irrefutable(const ast::Pat& pattern, TypePtr type);

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
