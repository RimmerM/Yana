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

/*
 * One name in scope.
 *
 * An immutable binding is a name for an SSA value and nothing more. A `let &x` is a name for
 * *storage*: it has a local, reads of it load and assignments to it write, so that the two
 * statements of `let &i = 0` / `i = i + 1` are about the same slot rather than about two values.
 *
 * A third form is a name for a *borrow* - `let &entry = f(...)`, where the storage is whatever the
 * callee's return-root group named. It is a place like the second, and differs only in what roots
 * it. Nothing here checks anything: exclusivity and last use are resolve/analyze.cpp's, stated over
 * the places these produce.
 */
struct Binding {
    StringId name = 0;
    ModulePtr<Value> value = nullptr;
    U32 local = maxLimit<U32>;

    // Set for a name bound to a borrow rather than to storage of its own - `let &entry = f(...)`,
    // where what the name refers to is whatever the callee's return-root group named. The binding
    // is a place either way; only what roots it differs.
    ModulePtr<Value> borrow = nullptr;

    /*
     * A fourth form: a name a lambda body captured, which lives in the environment the body was
     * handed rather than in this frame at all.
     *
     * `captureField` is which word of that environment holds it, and `captureBorrow` says whether
     * the word is the value or a borrow of storage the enclosing frame still owns - Design-Memory
     * §8's two answers, decided where the capture was created rather than where it is read.
     */
    bool captured = false;
    bool captureBorrow = false;
    U16 captureField = 0;

    bool isPlace() const { return captured || local != maxLimit<U32> || borrow != nullptr; }
    Place place() const { return borrow ? Place::inBorrow(borrow) : Place::inLocal(local); }
};

/*
 * One binding a lambda body named that belongs to an enclosing function - Design-Memory §8.
 *
 * There is no capture list, so this is discovered rather than declared: the first time the body
 * names an outer binding, one of these is appended and the environment gains a word. `convention`
 * is the ordinary binding convention, inferred from what the body does with the name, and it is
 * what decides whether the environment holds the value or an address.
 */
struct Capture {
    StringId name = 0;

    // The captured value's type. The environment's field is `&T` for a by-reference capture and
    // `T` for the two that own, which is the whole of what the convention changes here.
    TypePtr type = nullptr;
    ast::BindType convention = ast::BindType::Borrow;
    bool byReference = false;
};

// One class function that fits a call, together with what its class's type variables had to be
// and the instance that supplies them. `instance` is null when the signature fits but nothing
// implements it for these types, which is a different diagnostic from "wrong function".
//
// `instanceArgs` is what selecting that instance bound *its* own variables to, which is empty for
// the concrete head that is the usual case and one type per variable for a parametric one.
struct ClassMatch {
    GlobalPtr<TypeClass> typeClass = nullptr;
    ModulePtr<ClassInstance> instance = nullptr;
    TypeList args;
    TypeList instanceArgs;
    U16 index = 0;
};

// Gives `into` everything `from` matched. Not assignment: the two lists are TypeLists, whose
// assignment is deleted precisely so that this reads as the replacement it is - see SmallArray.
inline void adopt(ClassMatch& into, const ClassMatch& from) {
    into.typeClass = from.typeClass;
    into.instance = from.instance;
    into.index = from.index;

    replaceContents(into.args, from.args);
    replaceContents(into.instanceArgs, from.instanceArgs);
}

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
    Binding* findBinding(StringId name);

    // The storage one name refers to. For an ordinary binding this is Binding::place(); for a
    // capture it is a word of the environment, and for one taken by reference it is the storage
    // that word points at - one more load, at each use, because a capture discovered half-way
    // through a body has no entry block left to hoist it into.
    Place placeOf(const Binding& binding, LocationId source);

    // The place an assignable expression names - a mutable binding, a field of one, or the memory
    // a raw pointer points at. Null root when the expression names no storage, which is the one
    // diagnostic assignment has of its own.
    // `(x)` and `x` name the same thing, and every rule that looks at the *shape* of an
    // expression - a dereference in assignment position, a field of one - has to see through the
    // parentheses to find it.
    const ast::Expr& unwrapNested(const ast::Expr& expr) {
        auto current = &expr;
        while(current->kind == ast::Expr::Nested) current = parse[current->nested];
        return *current;
    }

    // `through` says the place is about to be projected into rather than assigned to as a whole,
    // which is what lets an immutable binding holding a raw pointer root one - see resolvePlace.
    Maybe<Place> resolvePlace(const ast::Expr& expr, bool through = false);
    ModulePtr<Value> resolveAssign(const ast::Expr& expr, const ast::AssignExpr& assignment);
    void bindMutable(const ast::VarDecl& declaration, ModulePtr<Value> value);

    // A name for a borrow someone else's storage backs, rather than for a slot of this frame.
    void bindBorrow(const ast::VarDecl& declaration, ModulePtr<Value> value, bool mutable_);

    // `@heap` and whatever joins it - the attributes written before a `let`. `bindingBase` is where
    // this declaration's own bindings start, which is how the slot it introduced is found.
    void applyBindingAttributes(const ast::VarDecl& declaration, ModulePtr<Value> value, Size bindingBase);
    ModulePtr<Value> makeInt(LocationId source, TypePtr type, U64 value);
    ModulePtr<Value> makeFloat(LocationId source, TypePtr type, F64 value);

    // What reading a module-level name produces: a constant for an immutable global of direct
    // type, and a load of its place for anything else. See expr.cpp.
    ModulePtr<Value> globalValue(ModulePtr<Global> global_, LocationId source);

    // The constant `bits` names at `type` - see expr.cpp. Shared by an immutable global and a
    // field default, which are recorded the same way and for the same reason.
    ModulePtr<Value> constantBits(TypePtr type, U64 bits, LocationId source);
    ModulePtr<Value> convert(ModulePtr<Value> value, TypePtr target, LocationId source, bool implicit = true);

    // Taking a borrow of what a value names, reading through one, or weakening a mutable one.
    // Null when neither type is a borrow, which is every conversion the rest of the language has.
    ModulePtr<Value> convertBorrow(ModulePtr<Value> value, TypePtr from, TypePtr target, LocationId source);

    // Between a `@bits` refinement and what it refines, in either direction. Null when the two types
    // are not related that way, so that convert() falls through to the ordinary paths.
    ModulePtr<Value> convertRefinement(ModulePtr<Value> value, TypePtr from, TypePtr target,
                                       LocationId source);

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

    // Warns where a written literal does not fit the integer type it is being built at. Called only
    // from the two positions a literal reaches, never from makeInt itself - see its comment.
    void checkLiteralRange(LocationId source, TypePtr type, U64 written);

    /*
     * `implicit` says who owns the conversion to `target`.
     *
     * True - the ordinary case - means this position is asking for a value of that type, so a
     * narrowing conversion is an error about precision. False means an ascription above has already
     * asked for it explicitly, and is threaded down through the forms that have no type of their
     * own: a parenthesis, a block, the arms of an `if` or a `match`. Those are pass-throughs, so the
     * ascription belongs to each leaf rather than to the value they join, and `(x) :: U8` has to
     * mean what `x :: U8` means. It is also what a call takes as `convertResult`, which is the same
     * condition said in the caller's words - the ascription that selected the instance *is* the
     * conversion, so the call must not convert its own result a second time.
     */
    ModulePtr<Value> resolve(const ast::Expr& expr, TypePtr target = nullptr, bool used = true,
                             bool implicit = true);

    // A form whose value is its leaves' value, so an expected type belongs to each leaf rather than
    // to the result. The whitelist an ascription pushes through.
    static bool isPassThrough(const ast::Expr& expr) {
        switch(expr.kind) {
            case ast::Expr::Nested:
            case ast::Expr::Multi:
            case ast::Expr::If:
            case ast::Expr::MultiIf:
            case ast::Expr::Match:
                return true;
            default:
                return false;
        }
    }
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

    ModulePtr<Value> resolveIf(const ast::Expr& expr, const ast::IfExpr& branch, TypePtr target, bool used, bool implicit = true);
    ModulePtr<Value> resolveMultiIf(const ast::Expr& expr, ast::ParseList<ast::IfCase> cases, TypePtr target, bool used, bool implicit = true);
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

    ModulePtr<Value> resolveBinary(const ast::Expr& expr, const ast::InfixExpr& binary, TypePtr target, bool convertResult = true);
    ModulePtr<Value> resolvePrefix(const ast::Expr& expr, const ast::PrefixExpr& prefix, TypePtr target, bool convertResult = true);
    ModulePtr<Value> resolvePrecedence(SmallArray<const ast::Expr*, 8>& operands, SmallArray<StringId, 8>& operators, Size& operandIndex, Size& operatorIndex, U8 minimumPrecedence, TypePtr target = nullptr);
    ModulePtr<Value> resolveCall(const ast::Expr& expr, const ast::AppExpr& call, TypePtr target, bool convertResult = true);

    // A call whose callee is a value rather than a name - a binding of function type, or any
    // expression at all in callee position. Null when the call is not one of those.
    ModulePtr<Value> resolveIndirectCall(const ast::Expr& expr, const ast::AppExpr& call, TypePtr target);
    ModulePtr<Value> emitCall(StringId name, Buffer<ModulePtr<Value>> args, LocationId source, TypePtr target = nullptr, StringId resultName = 0);
    ModulePtr<Value> emitDirectCall(ModulePtr<Function> callee, Buffer<ModulePtr<Value>> args, LocationId source, TypePtr target = nullptr, StringId resultName = 0);

    // A call to a generic function: infers its type arguments from the call, then either
    // instantiates it or - when this body is itself generic and the arguments are not concrete
    // yet - defers the whole decision to the instantiation that will make them concrete.
    ModulePtr<Value> emitGenericCall(ModulePtr<Function> callee, Buffer<ModulePtr<Value>> args, LocationId source,
                                     TypePtr target, StringId resultName);

    // A call that passes its callee a runtime environment instead of being specialized for these
    // types. Null when the environment cannot be built yet, which leaves the call site for the
    // specializing path.
    ModulePtr<Value> emitErasedCall(ModulePtr<Function> callee, Buffer<TypePtr> typeArgs,
                                    Buffer<ModulePtr<Value>> args, LocationId source, StringId resultName);

    // A generic intrinsic, generated for the types this call decided. Shared with generic.cpp,
    // which reaches the same intrinsics through an InstGenCall a specialization made concrete.
    ModulePtr<Value> expandIntrinsic(ModulePtr<Function> callee, Buffer<TypePtr> typeArgs,
                                     Buffer<ModulePtr<Value>> args, LocationId source, StringId resultName);

    // A class function whose instance cannot be chosen here, because the types it would be chosen
    // by are this function's own type variables. Records the requirement and emits InstGenCall.
    ModulePtr<Value> emitGenericDispatch(ClassMatch& match, Buffer<ModulePtr<Value>> args, LocationId source,
                                         StringId resultName);

    bool bindPosition(TypePtr pattern, TypePtr actual, TypeList& bindings, bool widen);
    bool matchClassFun(const ClassFunRef& reference, Buffer<ModulePtr<Value>> args, TypePtr target, ClassMatch& resolved);

    // Whether a plain function can serve this call - the same question matchClassFun asks of a
    // class function, so that both halves of an overload set are judged by one rule.
    bool matchFunction(ModulePtr<Function> callee, Buffer<ModulePtr<Value>> args, TypePtr target, LocationId source);

    // Calls one implementation of a selected instance. A concrete instance's is an ordinary
    // function; a parametric one's is generic over the instance's own variables, so it is expanded
    // where it is an intrinsic and specialized where it is not. `site` is the module the call was
    // written in, which is what decides the instances its own requirements are proved against.
    ModulePtr<Value> emitInstanceCall(Module& site, ModulePtr<ClassInstance> instance, Buffer<TypePtr> instanceArgs,
                                      U16 index, Buffer<ModulePtr<Value>> args, LocationId source,
                                      TypePtr target = nullptr, StringId resultName = 0);

    ModulePtr<ClassInstance> selectInstance(GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                                            TypeList& instanceArgs);

    /*
     * Function values and closures (expr_fun.cpp).
     */

    // `(a, b) -> expr` and `(a, b): block`. Lifts the body into a function of its own and builds
    // the `{code, env}` value that reaches it - see expr_fun.cpp.
    ModulePtr<Value> resolveFun(const ast::Expr& expr, const ast::FunExpr& fun, TypePtr target);

    // A named function in value position. The value's code word is a thunk that drops the
    // environment every callable is handed, so that a plain function and a closure are one shape.
    ModulePtr<Value> functionValue(ModulePtr<Function> callee, LocationId source);

    // Builds a `{code, env}` value in fresh storage. `env` is null for a function value that
    // captured nothing, which is what makes its teardown a branch that never fires rather than a
    // second representation. What is *in* the environment is said by the closure header in front of
    // `code` rather than by this value - see ClosureHeaderLayout.
    ModulePtr<Value> makeFunValue(TypePtr type, ModulePtr<Function> code, ModulePtr<Value> env,
                                  LocationId source, StringId name);

    // Calling a value of function type: loads the code and the environment out of it and emits
    // InstCallDyn, with each argument taken by the convention the *type* declares.
    ModulePtr<Value> emitDynamicCall(ModulePtr<Value> callable, Buffer<ModulePtr<Value>> args,
                                     LocationId source, StringId resultName);

    // The binding a lambda body named that belongs to an enclosing function, added to this body's
    // capture list the first time it is named. Null when no enclosing body has it either.
    Binding* captureBinding(StringId name);

    /*
     * Storage and aggregates (expr_construct.cpp).
     */

    // Storage for one value. `convention` is what the name that owns the slot may do with it: a
    // temporary and an immutable binding get the default, a `let &` gets Ref, and it is what both
    // assignment and a `&` argument check before writing through.
    // `closureEnv` marks the storage a closure's captures live in: released by the function value
    // that owns it rather than by this frame - see Local::closureEnv.
    ModulePtr<Value> allocate(TypePtr type, LocationId source, StringId name = 0,
                              ast::BindType convention = ast::BindType::Borrow, bool closureEnv = false);
    Maybe<Place> findPlace(ModulePtr<Value> value);
    Place placeFor(ModulePtr<Value> value, LocationId source);
    bool isWritablePlace(const Place& place);

    // The value passed for a `&` parameter: a mutable borrow of whatever storage the argument
    // named. Null, after reporting, when the argument names none or names storage that may not be
    // written - the two ways a mutable borrow can fail before any liveness question arises.
    // `loaned` is the parameter's `return` marker. A borrow that outlives the call cannot be the
    // temporary Design.md's tier 1 makes, since the write-back happens *at* the call and everything
    // the caller wrote afterwards would be lost - so a packed field in that position is reported
    // here, where the declaration that caused it can be named.
    ModulePtr<Value> borrowArgument(ModulePtr<Value> value, TypePtr expected, LocationId source,
                                    bool loaned = false);

    /*
     * A borrow of a place, with Design.md's tier 1 applied where the place needs it.
     *
     * The one place an `InstBorrow` of a place is created, so that the rewrite cannot be forgotten
     * at one of them: a borrow of a field with no address of its own becomes a fresh local holding
     * the field's value, a borrow of *that*, and - if the borrow is mutable - a write-back queued
     * for whoever consumes it. An immutable one needs the temporary for the same reason and needs
     * no commit, since nothing wrote to it.
     *
     * `loaned` says the borrow is meant to outlive the call, which is what a `return` parameter
     * declares. That is tier 2 and is reported here, where the declaration causing it can be named.
     */
    ModulePtr<Value> borrowPlace(Place place, TypePtr borrowType, LocationId source,
                                 bool loaned = false);

    // Where the pending write-back list currently ends. A call takes one before converting its
    // arguments and hands it back to flushPackedBorrows afterwards, so that a nested call commits
    // its own arguments and not the enclosing call's.
    Size packedMark() { return packedBorrows.size(); }

    // Emits the write-back for every borrow materialized since `mark`. Called immediately after the
    // instruction that consumed the borrows, which is where the loan ends.
    void flushPackedBorrows(Size mark);

    // The value a `->` binding or a `->` argument produces - a move, an independent copy, or the
    // value unchanged, decided by the source's ownership classification. See expr_construct.cpp.
    ModulePtr<Value> sinkValue(ModulePtr<Value> value, LocationId source);

    // Storage for a moved value whose relocation is a call rather than its bytes. A no-op for
    // every other value, including a bitwise move - see expr_construct.cpp for which consumers of
    // a move need this and why the rest do not.
    ModulePtr<Value> rootSink(ModulePtr<Value> value, LocationId source);

    Place materialize(ModulePtr<Value> value, LocationId source);
    Place project(Place place, ProjectionKind kind, U16 index, ModulePtr<Value> value = nullptr);
    TypePtr placeRootType(const Place& place);
    TypePtr placeType(const Place& place);
    ModulePtr<Value> load(Place place, LocationId source, StringId name = 0);
    void initialize(Place place, ModulePtr<Value> value, LocationId source);
    void assign(Place place, ModulePtr<Value> value, LocationId source);
    void write(Place place, ModulePtr<Value> value, LocationId source, Value::Kind kind);
    ModulePtr<Value> addressOf(Place place, LocationId source, StringId name = 0);

    // `[1, 2, 3]`, and `xs[i]` in either a reading or an assigning position. Both build calls into
    // Collections rather than anything the IR knows about - see expr_construct.cpp.
    ModulePtr<Value> resolveArray(const ast::Expr& expr, ast::ParseList<ast::Expr> items, TypePtr target);
    ModulePtr<Value> resolveSubscript(const ast::Expr& expr, const ast::AppExpr& subscript, bool mutable_);

    ModulePtr<Value> resolveTuple(const ast::Expr& expr, ast::ParseList<ast::TupArg> args, TypePtr target);
    ModulePtr<Value> resolveTupUpdate(const ast::Expr& expr, const ast::TupUpdateExpr& update, TypePtr target);
    ModulePtr<Value> resolveConstruct(const ast::Expr& expr, const ast::ConExpr& construct, TypePtr target);
    TypePtr constructedType(ConstructorRef reference, ast::ParseList<ast::TupArg> args, TypePtr target, ValueList& resolved, LocationId source);
    ModulePtr<Value> resolveField(const ast::Expr& expr, const ast::FieldExpr& field);

    // The place of one named field of `place`, following the downcast a single-constructor
    // record needs and the dereference a reference does. Shared by field reads and field
    // assignments so that both reach a field the same way.
    Maybe<Place> projectField(Place place, const ast::Expr& field, LocationId source);
    Maybe<Place> projectField(Place place, StringId field, LocationId fieldSource, LocationId source);

    // Reports a reference kind `.` cannot follow yet - a region pointer or a checked reference,
    // whose dereferences need more than an address. False for anything else, including a raw
    // pointer, which is followed. See expr_construct.cpp.
    bool reportUnfollowedReference(TypePtr type, LocationId source);
    bool fillTuple(Place place, TupType& tuple, ast::ParseList<ast::TupArg> args,
                   GlobalList<FieldDefault>* defaults, LocationId source);

    // The default declared for one field of a constructor, or nothing where it has none.
    Maybe<U64> fieldDefault(GlobalList<FieldDefault>* defaults, U16 field);

    /*
     * Patterns (expr_pat.cpp).
     */

    ModulePtr<Value> resolveMatch(const ast::Expr& expr, const ast::MatchExpr& match, TypePtr target, bool used, bool implicit = true);

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

    /*
     * Mutable borrows of packed fields awaiting their write-back - Design.md's tier 1.
     *
     * A list rather than one entry because a call can take several, and ordered because the
     * commits have to be: each one reads the containing word as it stands, so two fields of one
     * word merge in sequence rather than racing.
     */
    struct PackedBorrow {
        Place field;
        Place temporary;

        // What the field holds, which is not what the temporary holds when the field is `@bits`
        // refined: the commit narrows back into it, and that narrowing is what keeps the
        // refinement's range - and therefore the niche above it - true.
        TypePtr fieldType;
        LocationId source;
    };

    Array<PackedBorrow> packedBorrows;

    /*
     * The lambda half, all null or empty for an ordinary function body.
     *
     * `enclosing` is what makes a capture possible at all: a name this body does not bind is looked
     * for there, and finding one is the definition of a capture. It is a chain rather than a single
     * link, so a nested lambda naming a binding two frames out captures it through the one in
     * between - which is the same thing happening twice rather than a second mechanism.
     */
    ExprResolver* enclosing = nullptr;

    // The environment parameter - argument zero of a lifted lambda - and the tuple type it points
    // at, which gains a field per capture as the body names them.
    ModulePtr<Value> envArg = nullptr;
    TupType* envType = nullptr;
    Array<Capture> captures;

    // The names the lambda body assigns to, collected from its AST before it is resolved. A capture
    // the body writes has to be a mutable borrow (Design-Memory §8), and which it is has to be
    // decided at the *first* use rather than at the one that happens to be a write.
    Array<StringId> written;

    // Set while resolving a lambda whose result type its body decides, which is what makes an
    // explicit `return` inside one something to report rather than something to convert.
    bool resultInferred = false;
};

// Creates a function that is reached through something other than its own name - a class
// instance's implementation - with a unique name for printing and lowering.
Function* addAnonymousFunction(Module& module, StringId name, LocationId source);

// What storage a place names, and what that storage holds after its projections are followed.
// Free functions rather than only ExprResolver methods because the drop pass asks the same
// question of a place it did not build, long after the resolver that built it is gone.
TypePtr placeRootType(Module& module, Function& function, const Place& place);

// `limit` stops the walk after that many projections, which is how the owner of a place's *last*
// projection is asked for - the question packed-field borrowing needs, since whether a field may be
// co-packed is a fact about the tuple it is in rather than about the field's own type.
TypePtr placeType(Module& module, Function& function, const Place& place,
                  Size limit = maxLimit<Size>);

/*
 * Whether a place names a field a target may co-pack, and therefore one whose borrow needs
 * Design.md's tier 1 materialize/write-back rather than an address.
 *
 * Asked in resolve and answered from the logical type, so that the rewrite and the diagnostics that
 * go with it are the same on every target. Whether the field is *actually* packed is
 * `compiler/repr`'s answer and may be no; the cost of the difference is a temporary that a
 * declining target did not need, and the cost of getting it the other way round is a miscompile.
 */
bool placeIsPackCandidate(Module& module, Function& function, const Place& place);

// Whether the place names a narrow field of a `@layout(js)` record, which may not be borrowed -
// see the definition for why the pin and the reference cannot both be honoured.
bool placeIsHostPinnedField(Module& module, Function& function, const Place& place);

/*
 * Whether a mutable borrow of this place has to be a temporary rather than a reference.
 *
 * One reason, and it is not that the field has no address: a narrow field is borrowed by a
 * reference that carries its shift (Design.md's tier 2, `NarrowRef` in resolve/lower.cpp), which
 * works wherever the field is. What a reference cannot do is *convert* - so a parameter declared at
 * the unrefined type, which is what makes `increment(&x: Int)` accept `&h.length`, gets the value
 * widened into a temporary and narrowed back at the end of the loan.
 *
 * That narrowing is not an optimization. A `@bits(13)` field whose storage was written a
 * twenty-bit value would falsify the niche above its range, and a `Maybe` folded into that niche
 * would start reading one constructor as another.
 */
bool needsBorrowTemporary(Module& module, Function& function, const Place& place, TypePtr wanted);

// Names one binding per parameter, and storage for the ones that need it. `firstArg` skips the
// leading closure environment of anything reached as a function value - see expr.cpp.
void bindFunctionArgs(ExprResolver& resolver, Module& module, Function& function, Size firstArg);
