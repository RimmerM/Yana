#include "build.h"

/*
 * The peephole between the tree and the text.
 *
 * The emitter one file up builds one statement per resolve instruction, because that is the only
 * shape in which a place, a projection and an ownership operation each land exactly once. What it
 * cannot do while it is doing that is decide whether a binding is worth existing: an instruction's
 * result is named before anything has read it, and how many readers it has is a fact about the rest
 * of the block.
 *
 * So the naming happens unconditionally and this pass takes it back. Three rewrites, all driven by
 * use counts over one function:
 *
 *  - a `var` read exactly once and written never becomes its use, and one read by nothing at all
 *    goes away;
 *  - `var a = b;` where `b` is a name goes away however many readers `a` has, since a name costs
 *    nothing wherever it is put and nothing has to move to put it there;
 *  - `var v = {a: 0}; v.a = x;` becomes `var v = {a: x};`, and `var v = 0; v = x;` becomes
 *    `var v = x;`.
 *
 * The third is not cosmetic. Analysis-JS.md §2.3 makes construction order the JS equivalent of
 * field offsets, and an object literal that already holds its values is one hidden-class transition
 * where the zero-then-fill form is one per property.
 *
 * Everything here is decided from the tree rather than from the IR, deliberately: the question is
 * what the emitted JS evaluates and in which order, and that is a property of the tree. Three things
 * are checked before anything moves - that a name is read once and assigned never, that the source
 * of a copy still holds what it held, and that the expressions the move crosses cannot see each
 * other - and where any of them is unknown, nothing happens.
 *
 * ## And what a value *is*, which is a second family
 *
 * Beside the use-count rewrites are the ones about arithmetic: a coercion whose operand is already
 * in range removed (`foldCoercion`), an operation over literals evaluated (`foldConstantOp`), a call
 * to one of this compiler's own helpers worked out from its body (`foldCall`), and what a local
 * holds carried forward to its readers (`propagateConstants`).
 *
 * Those four are one rewrite in four parts, and separating them would stall each on the others: a
 * packed word is built by initializing storage and writing fields into it, so the operands are only
 * literals once the storage has been propagated, the mask is only removable once the operand has a
 * range, and the arithmetic only ends in a number once the helper has been evaluated. Together they
 * take `writeHigh` in WidePack.yana from three statements to `return 4503595332404200`, which is the
 * same number `compiler/lower` folds it to - see lower/lower_fold.h, which is that half.
 */

namespace js {

namespace {

// The elements of a list, as storage that can be written back into. `contents()` yields values,
// which is what every reader wants and what a rewrite cannot use.
template<class T>
T* itemsOf(Gen& g, JsList<T, false>& list) {
    return g.base[list.list.p];
}

/*
 * What moving an expression past another one would disturb.
 *
 * Two bits rather than a purity flag, because the question is symmetric: a property read may cross
 * anything that does not write, and a call may cross anything that neither reads nor writes. An
 * object literal is neither - allocating is not observable here, since nothing in the emitted
 * program can tell one fresh object from another.
 */
struct Effects {
    bool reads = false;
    bool writes = false;

    bool inert() const { return !reads && !writes; }
};

/*
 * The sub-expressions of one expression, in evaluation order, as slots the caller may rewrite.
 *
 * `conditional` is set for an operand the surrounding expression may not evaluate at all - the right
 * operand of `&&` and `||`, and the two arms of a conditional. Moving an effect into one of those
 * changes how many times it happens, which is the one thing no rewrite here is allowed to do.
 */
template<class F>
void eachOperand(Gen& g, Expr* expr, F&& f) {
    switch(expr->kind) {
        case Expr::Field:
            f(((FieldExpr*)expr)->object, false);
            break;
        case Expr::Index: {
            auto value = (IndexExpr*)expr;
            f(value->array, false);
            f(value->index, false);
            break;
        }
        case Expr::Array: {
            auto& values = ((ArrayExpr*)expr)->values;
            auto items = itemsOf(g, values);
            for(Size i = 0; i < values.size(); i++) f(items[i], false);
            break;
        }
        case Expr::Object: {
            auto& properties = ((ObjectExpr*)expr)->properties;
            auto items = itemsOf(g, properties);
            for(Size i = 0; i < properties.size(); i++) f(items[i].value, false);
            break;
        }
        case Expr::Unary:
            f(((UnaryExpr*)expr)->value, false);
            break;
        case Expr::Binary: {
            auto value = (BinaryExpr*)expr;
            auto shortCircuit = value->op == BinaryOp::LogicalAnd || value->op == BinaryOp::LogicalOr;

            f(value->lhs, false);
            f(value->rhs, shortCircuit);
            break;
        }
        case Expr::Ternary: {
            auto value = (TernaryExpr*)expr;
            f(value->cond, false);
            f(value->then, true);
            f(value->otherwise, true);
            break;
        }
        case Expr::Assign: {
            auto value = (AssignExpr*)expr;
            f(value->target, false);
            f(value->value, false);
            break;
        }
        case Expr::Call: {
            auto value = (CallExpr*)expr;
            auto& args = value->args;

            f(value->callee, false);

            auto items = itemsOf(g, args);
            for(Size i = 0; i < args.size(); i++) f(items[i], false);
            break;
        }
        default:
            break;
    }
}

// The expression a statement evaluates in its own right, or null where it has none. A label, a
// jump and a comment have nothing; a body is not this, because a body is reached rather than
// evaluated - see eachBody.
JsPtr<Expr>* headerOf(Gen& g, Stmt* stmt) {
    switch(stmt->kind) {
        case Stmt::Expression:
            return &((ExprStmt*)stmt)->value;
        case Stmt::If:
            return &((IfStmt*)stmt)->cond;
        case Stmt::Return:
            return ((ReturnStmt*)stmt)->value ? &((ReturnStmt*)stmt)->value : nullptr;
        case Stmt::Throw:
            return &((ThrowStmt*)stmt)->value;
        case Stmt::Decl:
            return ((DeclStmt*)stmt)->value ? &((DeclStmt*)stmt)->value : nullptr;
        default:
            return nullptr;
    }
}


/*
 * The statement lists one statement contains, in the order control reaches them.
 *
 * There is no expression here that contains one, and there used to be: a capturing lambda was built
 * by a factory returning a function *expression*, which is the one place in this tree where a
 * statement list hangs off an expression rather than the other way round. That shape is gone - a
 * code word is an ordinary top-level declaration now - so this walk is the statements alone, and
 * `eachOperand` no longer has a boundary it must not substitute across.
 */
template<class F>
void eachBody(Gen& g, Stmt* stmt, F&& f) {
    switch(stmt->kind) {
        case Stmt::Block:
            f(((BlockStmt*)stmt)->body);
            break;
        case Stmt::If:
            f(((IfStmt*)stmt)->then);
            f(((IfStmt*)stmt)->otherwise);
            break;
        case Stmt::Forever:
            f(((ForeverStmt*)stmt)->body);
            break;
        case Stmt::Labelled:
            eachBody(g, g.base[((LabelledStmt*)stmt)->content], forward<F>(f));
            break;
        case Stmt::Fun:
            f(((FunStmt*)stmt)->body);
            break;
        default:
            break;
    }
}


// A host intrinsic is arithmetic that happens to be spelled as a call - see CallExpr::pure.
bool isEffectful(Expr* expr) {
    if(expr->kind == Expr::Assign) return true;
    return expr->kind == Expr::Call && !((CallExpr*)expr)->pure;
}

/*
 * The operands an *effect* walk visits, which is `eachOperand` minus one thing: the callee of a
 * pure call.
 *
 * `Math.imul` is a property access in the tree and arithmetic in the emitted program, and that is
 * the whole of what `CallExpr::pure` asserts - "they read nothing, write nothing". Counting the
 * lookup as a read contradicts it, and does so where it matters most: the integer tower reaches for
 * an intrinsic on almost every operation that is not a plain `+`, so `x = BigInt.asIntN(64, a + b)`
 * had a read sitting in front of `b` and refused to take a call there. That is not a corner of the
 * emitted code, it is most of it.
 *
 * Only the effect walks use this. Counting names, substituting into one and folding all still go
 * through `eachOperand`, because what they ask about the callee has an answer - it is a name like
 * any other - where an effect walk's question does not.
 */
template<class F>
void eachEffectOperand(Gen& g, Expr* expr, F&& f) {
    if(expr->kind == Expr::Call && ((CallExpr*)expr)->pure) {
        auto& args = ((CallExpr*)expr)->args;
        auto items = itemsOf(g, args);

        for(Size i = 0; i < args.size(); i++) f(items[i], false);
        return;
    }

    eachOperand(g, expr, forward<F>(f));
}

/*
 * A bare identifier that names module-level storage - see Gen::mutableGlobals.
 *
 * The one `Var` whose read is a read: every other identifier in an emitted function is a parameter
 * or a local, which no callee can reach and therefore no call can change. A global is reachable from
 * every function in the file, so reading one has to be ordered against the calls around it exactly
 * as a property read is.
 */
bool isMutableGlobal(Gen& g, Expr* expr) {
    return expr->kind == Expr::Var && g.mutableGlobals.contains(((VarExpr*)expr)->name.text);
}

/*
 * What one node does, not counting its operands.
 *
 * Stated once because it is asked twice, by two walks going in opposite directions: `addEffects`
 * summarizes what an expression does, and `substitute` accumulates what the position it is landing
 * in evaluates *before* it. The two used to say it separately, and the copy in `substitute` was the
 * one that did not learn about globals - so the summary said "this reads storage" and the prefix
 * said the value in front of it was inert, which is one half of a comparison answering about a
 * different program from the other half.
 */
void addNodeEffects(Gen& g, Expr* expr, Effects& out) {
    if(expr->kind == Expr::Field || expr->kind == Expr::Index || isMutableGlobal(g, expr)) out.reads = true;
    if(isEffectful(expr)) out.writes = true;
}

void addEffects(Gen& g, JsPtr<Expr> pointer, Effects& out) {
    auto expr = g.base[pointer];
    addNodeEffects(g, expr, out);

    eachEffectOperand(g, expr, [&](JsPtr<Expr>& operand, bool) { addEffects(g, operand, out); });
}

Effects effectsOf(Gen& g, JsPtr<Expr> pointer) {
    Effects effects;
    addEffects(g, pointer, effects);
    return effects;
}

/*
 * How often each identifier is mentioned, and which ones are ever written.
 *
 * Counted over a whole function rather than a block, because `var` is function-scoped and the
 * emitter names locals per function - so one name is one binding here, and a count of one is a
 * single reader wherever in the structure that reader turned out to be.
 *
 * An assignment target counts as a mention as well as a write, which is what keeps a phi out of
 * this: it is written by each predecessor and read at the join, so it is neither read once nor
 * written never.
 */
struct Names {
    HashMap<U32, U32> uses;
    HashSet<U32> assigned;

    // Empties both tables, keeping what they grew into. The counting loops below run this to a
    // fixpoint and each round wants the counts from nothing - which used to mean a fresh pair of
    // tables per round per function, and this is the same statement without the allocations.
    void reset() {
        uses.reset();
        assigned.reset();
    }

    U32 useCount(Name name) {
        auto found = uses.get(name.text);
        return found ? found.unwrap() : 0;
    }

    bool isSingleUse(Name name) {
        return useCount(name) == 1 && !assigned.contains(name.text);
    }
};

void countExpr(Gen& g, JsPtr<Expr> pointer, Names& names) {
    auto expr = g.base[pointer];

    if(expr->kind == Expr::Var) {
        auto id = ((VarExpr*)expr)->name.text;
        if(auto found = names.uses.get(id)) {
            found.unwrap()++;
        } else {
            names.uses.add(id, U32(1));
        }
    } else if(expr->kind == Expr::Assign) {
        auto target = g.base[((AssignExpr*)expr)->target];
        if(target->kind == Expr::Var) names.assigned.add(((VarExpr*)target)->name.text);
    }

    eachOperand(g, expr, [&](JsPtr<Expr>& operand, bool) { countExpr(g, operand, names); });
}

void countList(Gen& g, StmtList& list, Names& names);

void countStmt(Gen& g, JsPtr<Stmt> pointer, Names& names) {
    auto stmt = g.base[pointer];
    if(auto header = headerOf(g, stmt)) countExpr(g, *header, names);

    eachBody(g, stmt, [&](StmtList& body) { countList(g, body, names); });
}

void countList(Gen& g, StmtList& list, Names& names) {
    for(auto stmt: list.contents(g.base)) countStmt(g, stmt, names);
}

/*
 * Substitution.
 */

// Whether an expression mentions one name at all, which is what stops a value from being folded
// into the object it is being read out of.
bool mentions(Gen& g, JsPtr<Expr> pointer, Name name) {
    auto expr = g.base[pointer];
    if(expr->kind == Expr::Var && ((VarExpr*)expr)->name.text == name.text) return true;

    auto found = false;
    eachOperand(g, expr, [&](JsPtr<Expr>& operand, bool) {
        if(!found) found = mentions(g, operand, name);
    });

    return found;
}

/*
 * An expression that costs nothing wherever it is put: a name that is never assigned, or a literal.
 *
 * These are the ones whose single use is substituted however far away it is. Anything else moves
 * only as far as the next statement, because moving a computation into a loop body would be a
 * pessimization dressed as an inline, and moving it into a branch would change how often it runs.
 *
 * `names.assigned` is what this function's own text does, which is the whole answer for a local and
 * only half of it for a global - see isMutableGlobal. A global read is a read of storage, so it
 * moves under the ordering rule below rather than freely.
 */
bool isAtom(Gen& g, JsPtr<Expr> pointer, Names& names) {
    auto expr = g.base[pointer];

    switch(expr->kind) {
        case Expr::Var:
            return !names.assigned.contains(((VarExpr*)expr)->name.text) && !isMutableGlobal(g, expr);
        case Expr::Number:
        case Expr::BigInt:
        case Expr::String:
        case Expr::Bool:
        case Expr::Null:
        case Expr::Undefined:
            return true;
        default:
            return false;
    }
}

// Whether a value may cross a prefix - the expressions the position it lands in evaluates first.
// Two reads may be reordered because neither can change what the other sees; anything else has to
// have one side that does nothing at all.
bool crosses(const Effects& value, const Effects& prefix) {
    if(value.inert() || prefix.inert()) return true;
    return !value.writes && !prefix.writes;
}

struct Substitution {
    Name name;
    JsPtr<Expr> value;
    Effects effects;

    // What the landing position evaluates before it reaches the use.
    Effects prefix;

    bool done = false;
    bool blocked = false;
};

void substitute(Gen& g, JsPtr<Expr>& slot, Substitution& s, bool conditional) {
    if(s.done || s.blocked) return;

    auto expr = g.base[slot];

    if(expr->kind == Expr::Var && ((VarExpr*)expr)->name.text == s.name.text) {
        // A position the surrounding expression may skip takes a value that does nothing, and
        // nothing else: an effect that ran once must not become an effect that runs zero times.
        if(conditional && !s.effects.inert()) {
            s.blocked = true;
            return;
        }

        if(!crosses(s.effects, s.prefix)) {
            s.blocked = true;
            return;
        }

        slot = s.value;
        s.done = true;
        return;
    }

    // The same operand set the effect walk uses, and for the same reason: a use is never inside an
    // intrinsic's own name, and walking into one would put a read in the prefix that the emitted
    // program does not perform.
    eachEffectOperand(g, expr, [&](JsPtr<Expr>& operand, bool branch) {
        substitute(g, operand, s, conditional || branch);
    });

    // The node's own effect happens after its operands, so it joins the prefix only once the walk
    // has left it without finding the use. The same rule the summary uses - see addNodeEffects.
    if(s.done || s.blocked) return;
    addNodeEffects(g, expr, s.prefix);
}

bool substituteAnywhere(Gen& g, JsPtr<Expr>& slot, Name name, JsPtr<Expr> value) {
    auto expr = g.base[slot];

    if(expr->kind == Expr::Var && ((VarExpr*)expr)->name.text == name.text) {
        slot = value;
        return true;
    }

    auto done = false;
    eachOperand(g, expr, [&](JsPtr<Expr>& operand, bool) {
        if(!done) done = substituteAnywhere(g, operand, name, value);
    });

    return done;
}

bool substituteAfter(Gen& g, StmtList& list, Size from, Name name, JsPtr<Expr> value);

bool substituteInStmt(Gen& g, JsPtr<Stmt> pointer, Name name, JsPtr<Expr> value) {
    auto stmt = g.base[pointer];

    if(auto header = headerOf(g, stmt)) {
        if(substituteAnywhere(g, *header, name, value)) return true;
    }

    auto done = false;
    eachBody(g, stmt, [&](StmtList& body) {
        if(!done) done = substituteAfter(g, body, 0, name, value);
    });

    return done;
}

// The single use, wherever it is. It is always after the definition in the emitted structure,
// because a resolve value is used where its definition dominates - and a use a definition dominates
// cannot precede it in a graph the loop recovery has already made reducible.
bool substituteAfter(Gen& g, StmtList& list, Size from, Name name, JsPtr<Expr> value) {
    for(Size i = from; i < list.size(); i++) {
        if(substituteInStmt(g, list.get(g.base, i), name, value)) return true;
    }

    return false;
}

/*
 * Copy propagation.
 *
 * `var a = b;` is a second name for a value that already has one, and the rewrites above cannot take
 * it: `inlineBinding` asks for a single use, and this is the shape that most often has several -
 * a moved value read twice, a phi read on both sides of what follows it. The count is the wrong
 * question here, because a *name* costs nothing wherever it is put and nothing moves: `b` is
 * evaluated where `a` was, and what `a` was is `b`.
 *
 * So the only question is whether `b` still holds at the use what it held at the declaration, and it
 * is a question about assignments rather than about calls. A local `var` is invisible to every
 * callee - there is no way to reach one from outside the function that declares it - so nothing but
 * this function's own text can change one, which is what makes the answer readable off the tree.
 *
 * Two cases, and the difference between them is how far the substitution reaches:
 *
 *  - **`b` is never assigned.** Then it denotes one value for the whole of the function and every
 *    use of `a` becomes `b`, wherever in the structure it is - inside a branch, inside a loop body,
 *    inside a closure. This is `isAtom`'s condition, without the single-use requirement it was
 *    paired with, and it is the case that covers a parameter aliased by a `move`.
 *
 *  - **`b` is assigned somewhere.** Then the substitution stays in the declaration's own statement
 *    list and stops at the first statement that could assign `b`, since control reaching a later
 *    statement in one list has passed through every statement between. The statement holding the
 *    use may assign `b` itself and still be taken, because an assignment's value is evaluated before
 *    the write lands - `result = f(a) + g()` where `a` is `result` is the shape this is for, and it
 *    is the shape a `+=` over a local comes out as.
 *
 * Nested bodies are declined outright in the second case rather than walked. A loop body reached a
 * second time has run everything in it, so an assignment *after* the use is a barrier *before* it,
 * and a closure runs at a time this cannot name at all. The counting pass is what makes declining
 * cost nothing: a use it cannot reach leaves the declaration alone rather than half-rewritten.
 */
struct Copy {
    Name from;
    Name to;

    // Whether `to` is assigned anywhere in the function, which is the whole of what decides how far
    // this reaches - and, where it is false, means `stopped` can never be set.
    bool guarded = false;

    // The counting pass and the rewriting pass are the same walk, so that what it declines and what
    // it would have rewritten cannot drift apart.
    bool apply = false;

    bool stopped = false;
    U32 count = 0;
};

void propagateExpr(Gen& g, JsPtr<Expr>& slot, Copy& c) {
    if(c.stopped) return;

    auto expr = g.base[slot];

    if(expr->kind == Expr::Var && ((VarExpr*)expr)->name.text == c.from.text) {
        if(c.apply) slot = variable(g, c.to);
        c.count++;
        return;
    }

    eachOperand(g, expr, [&](JsPtr<Expr>& operand, bool) { propagateExpr(g, operand, c); });

    // After the operands, because that is when the write lands: the value an assignment stores is
    // evaluated first, so a use inside it is in front of this barrier rather than behind it.
    if(!c.guarded || expr->kind != Expr::Assign) return;

    auto target = g.base[((AssignExpr*)expr)->target];
    if(target->kind == Expr::Var && ((VarExpr*)target)->name.text == c.to.text) c.stopped = true;
}

// Whether one statement contains an assignment to a name anywhere at all, bodies and closures
// included - the question asked of a statement whose insides the guarded case will not walk.
bool assignsName(Gen& g, JsPtr<Stmt> pointer, Name name);

bool assignsIn(Gen& g, StmtList& list, Name name) {
    for(auto pointer: list.contents(g.base)) {
        if(assignsName(g, pointer, name)) return true;
    }

    return false;
}

bool assignsInExpr(Gen& g, JsPtr<Expr> pointer, Name name) {
    auto expr = g.base[pointer];
    auto found = false;

    if(expr->kind == Expr::Assign) {
        auto target = g.base[((AssignExpr*)expr)->target];
        if(target->kind == Expr::Var && ((VarExpr*)target)->name.text == name.text) return true;
    }

    eachOperand(g, expr, [&](JsPtr<Expr>& operand, bool) {
        if(!found) found = assignsInExpr(g, operand, name);
    });

    return found;
}

bool assignsName(Gen& g, JsPtr<Stmt> pointer, Name name) {
    auto stmt = g.base[pointer];
    if(auto header = headerOf(g, stmt)) {
        if(assignsInExpr(g, *header, name)) return true;
    }

    auto found = false;
    eachBody(g, stmt, [&](StmtList& body) { found = found || assignsIn(g, body, name); });

    return found;
}

void propagateList(Gen& g, StmtList& list, Size from, Copy& c);

void propagateStmt(Gen& g, JsPtr<Stmt> pointer, Copy& c) {
    if(c.stopped) return;

    auto stmt = g.base[pointer];
    if(auto header = headerOf(g, stmt)) propagateExpr(g, *header, c);

    /*
     * The unguarded case walks everything, including the arms of an `if` and the bodies of the
     * closures a factory returns: with nothing able to assign `to`, there is no order to respect.
     * The guarded case walks none of it and asks one question instead - see the header comment for
     * why a loop body and a closure are the two shapes an ordered walk cannot answer for.
     */
    if(!c.guarded) {
        eachBody(g, stmt, [&](StmtList& body) { propagateList(g, body, 0, c); });
        return;
    }

    if(assignsName(g, pointer, c.to)) c.stopped = true;
}

void propagateList(Gen& g, StmtList& list, Size from, Copy& c) {
    for(Size i = from; i < list.size() && !c.stopped; i++) {
        propagateStmt(g, list.get(g.base, i), c);
    }
}

/*
 * The rewrites.
 */

// Whether one statement has anything at all to do with a name - defined below, beside the other
// rules that walk statements a value is being moved across.
bool mentionsStmt(Gen& g, JsPtr<Stmt> pointer, Name name);

/*
 * `var v = {a: 0, b: 0}; v.a = x;` -> `var v = {a: x, b: 0};`
 *
 * Folded in property order and no further: a write to a property the walk has already passed would
 * be moved in front of one it was behind, which is a reordering wherever either value does anything.
 *
 * What the value moves across is checked rather than assumed, which is what makes this rule about
 * the tree rather than about where the tree came from - the properties are the type's zero values in
 * every literal the emitter builds today, and that is a fact about the emitter.
 *
 * ## Why it walks past statements
 *
 * For the reason `foldArrayElements` does, and it is the same reason: what a write needs is routinely
 * declared *between* the declaration and the write. `Wrapped(inner)` builds the payload in a binding
 * of its own, so the tag write and the payload write have a `var` between them, and a rule that
 * stopped at the first statement it did not recognize folded the tag and left the payload behind.
 *
 * Three things have to survive it, and they are the three `foldArrayElements` already checks for
 * the indexed case - the machinery is deliberately the same:
 *
 *  - a statement that **mentions the name** stops the walk, since a read of the object would see a
 *    property that no longer holds its zero;
 *  - a value moved up must not cross what the crossed statements **do**. `prefix` is their effects
 *    and `crosses` is the question: `var v = {a: 0}; var x = first(); v.a = second();` may not
 *    become `{a: second()}` above `first()`. A statement with a *body* stops the walk outright,
 *    because pricing what it does is a walk of its own;
 *  - a value moved up must not cross the **declaration of a binding it reads**, which `declared` is:
 *    `var v = {a: x}` above `var x` is hoisted but unassigned, so the object holds `undefined`
 *    rather than the program failing.
 */
bool foldInitializers(Gen& g, StmtList& list, Size index) {
    auto declaration = g.base[list.get(g.base, index)];
    if(declaration->kind != Stmt::Decl) return false;

    auto& decl = *(DeclStmt*)declaration;
    if(!decl.value || g.base[decl.value]->kind != Expr::Object) return false;

    auto& object = *(ObjectExpr*)g.base[decl.value];
    auto changed = false;
    Size filled = 0;
    Size at = index + 1;
    Effects prefix;

    SmallArray<Name, 8> declared;

    while(at < list.size()) {
        auto pointer = list.get(g.base, at);
        auto next = g.base[pointer];

        /*
         * Whether this is a write into the object at all. Anything else is stepped over where it has
         * nothing to do with the object and its effects are priced into `prefix`; a statement with a
         * body, or one that mentions the name, ends the walk.
         */
        auto isWrite = false;

        if(next->kind == Stmt::Expression) {
            auto written = g.base[((ExprStmt*)next)->value];

            if(written->kind == Expr::Assign) {
                auto target = g.base[((AssignExpr*)written)->target];

                if(target->kind == Expr::Field) {
                    auto owner = g.base[((FieldExpr*)target)->object];
                    isWrite = owner->kind == Expr::Var &&
                              ((VarExpr*)owner)->name.text == decl.name.text;
                }
            }
        }

        if(!isWrite) {
            if(next->kind != Stmt::Decl && next->kind != Stmt::Expression) break;
            if(mentionsStmt(g, pointer, decl.name)) break;

            if(next->kind == Stmt::Decl) declared.push(((DeclStmt*)next)->name);
            if(auto header = headerOf(g, next)) addEffects(g, *header, prefix);

            at++;
            continue;
        }

        auto written = g.base[((ExprStmt*)next)->value];
        auto target = g.base[((AssignExpr*)written)->target];

        auto properties = itemsOf(g, object.properties);
        auto key = ((FieldExpr*)target)->field.text;
        auto size = object.properties.size();
        auto slot = size;

        // Searched from the watermark, so a property already folded is not found again: a second
        // write to one has to stay a write, since folding it would land in front of the first.
        for(Size i = filled; i < size; i++) {
            if(properties[i].key.text != key) continue;
            slot = i;
            break;
        }

        if(slot == size) break;

        // A value read out of the object it is going into would be reading a property that no
        // longer holds its zero by the time it is evaluated.
        auto value = ((AssignExpr*)written)->value;
        if(mentions(g, value, decl.name)) break;

        /*
         * The two orderings the move has to preserve, which are the two sets `foldArrayElements`
         * asks about for the indexed case.
         *
         * This used to check the properties between the watermark and the slot instead, and those
         * are the one set that is safe by inspection: they stay exactly where they are, and the
         * value neither crosses them nor replaces them. What it did *not* check is what the value
         * lands in front of and what it deletes. Both are inert in every object literal the emitter
         * builds - they are the type's zero values, which is why no fixture moved when this was
         * corrected - so this is the argument being made to match the code rather than a bug being
         * fixed. See foldArrayElements, where the same two checks are load-bearing.
         */
        // And the bindings the walk stepped over, which the value would be moved in front of.
        auto reachesBack = false;
        for(auto name: declared) reachesBack = reachesBack || mentions(g, value, name);
        if(reachesBack) break;

        auto effects = effectsOf(g, value);
        if(!crosses(effects, prefix)) break;

        Effects tail;
        for(Size i = slot + 1; i < size; i++) addEffects(g, properties[i].value, tail);
        if(!crosses(effects, tail)) break;

        if(!effectsOf(g, properties[slot].value).inert()) break;

        properties[slot].value = value;
        list.remove(g.base, at);
        filled = slot + 1;
        changed = true;
    }

    return changed;
}

// Whether one statement has anything at all to do with a name - defined below, beside the other
// rule that walks statements a value is being moved across.
bool mentionsStmt(Gen& g, JsPtr<Stmt> pointer, Name name);

// `v[k] = x` for a literal index - the one statement the rule below consumes. The slot is left as
// the number it was written with, because whether it is a slot at all is a question about the array
// and this does not have one.
struct ElementWrite {
    JsPtr<Expr> value;
    F64 slot = 0;
    bool matched = false;
};

ElementWrite elementWrite(Gen& g, Stmt* stmt, Name name) {
    if(stmt->kind != Stmt::Expression) return {};

    auto written = g.base[((ExprStmt*)stmt)->value];
    if(written->kind != Expr::Assign) return {};

    auto target = g.base[((AssignExpr*)written)->target];
    if(target->kind != Expr::Index) return {};

    auto owner = g.base[((IndexExpr*)target)->array];
    if(owner->kind != Expr::Var || ((VarExpr*)owner)->name.text != name.text) return {};

    // Anything that is not a literal index is not this - `v[v.length] = x` is a push.
    auto position = g.base[((IndexExpr*)target)->index];
    if(position->kind != Expr::Number || !((NumberExpr*)position)->integral) return {};

    return ElementWrite { ((AssignExpr*)written)->value, ((NumberExpr*)position)->value, true };
}

/*
 * `var v = []; v[0] = a; v[1] = b;` -> `var v = [a, b];`, and
 * `var v = [1, 2, 3, 4]; v[1] = x;` -> `var v = [1, x, 3, 4];`
 *
 * `foldInitializers` for the indexed case, and it earns itself for a reason the object one does not
 * have to argue: filling an array by index walks the element-kind transitions a literal skips, so
 * the two forms are not the same program to a host that specializes on them. An array literal is
 * also what the source wrote - `[7, 8, 9]` compiles through an `alloc` and three `init`s because
 * that is what the IR has, not because anything wanted the host to see it that way.
 *
 * ## Why it overwrites as well as appends
 *
 * Because the write that follows a literal is usually a write *into* it. `eliminateOverwritten` in
 * opt_place.cpp deliberately leaves an element in place where removing it would leave the array with
 * a gap, so `let &xs = [1, 2, 3, 4]` followed by `xs[1] = 20` arrives here as a full literal and a
 * separate store - and merging the two is what makes the store disappear rather than the element.
 * That is also what makes the array *dead* where nothing reads it, since the store was its last
 * mention and `removeDeadBinding` takes it from there.
 *
 * ## What bounds it
 *
 * A slot past the end would leave a hole, which is a different kind of array on every engine that
 * has element kinds. A slot below the watermark is a second write to one already folded, and would
 * land in front of the first. And three orderings have to survive, each a set the value is moved
 * across: the statements between the declaration and the write, the elements it lands in front of,
 * and the element it replaces - which is not moved but deleted, so it has to be one nothing misses.
 *
 * Statements in between are walked past rather than stopped at, because what a write needs is
 * routinely declared between the two - `foldInitialValue` crosses them for the same reason. A
 * statement with a body is where that stops: pricing what it does is a walk of its own.
 */
bool foldArrayElements(Gen& g, StmtList& list, Size index) {
    auto declaration = g.base[list.get(g.base, index)];
    if(declaration->kind != Stmt::Decl) return false;

    auto& decl = *(DeclStmt*)declaration;
    if(!decl.value || g.base[decl.value]->kind != Expr::Array) return false;

    auto& array = *(ArrayExpr*)g.base[decl.value];
    auto changed = false;
    Size filled = 0;
    Effects prefix;

    // The bindings the walk has passed. A value moving back into the literal moves above their
    // declarations, so one that reads any of them would read a `var` that is hoisted but not yet
    // assigned - `var v = [1, x, 3];` above `var x = ...` is a hole rather than a diagnostic, and
    // the array is left holding `undefined`.
    SmallArray<Name, 8> declared;

    for(Size at = index + 1; at < list.size();) {
        auto pointer = list.get(g.base, at);
        auto stmt = g.base[pointer];
        auto write = elementWrite(g, stmt, decl.name);

        if(!write.matched) {
            if(stmt->kind != Stmt::Decl && stmt->kind != Stmt::Expression) break;
            if(mentionsStmt(g, pointer, decl.name)) break;

            if(stmt->kind == Stmt::Decl) declared.push(((DeclStmt*)stmt)->name);
            if(auto header = headerOf(g, stmt)) addEffects(g, *header, prefix);
            at++;
            continue;
        }

        auto size = array.values.size();
        if(write.slot < 0 || write.slot > F64(size)) break;

        auto slot = Size(write.slot);
        if(slot < filled) break;

        // A value read out of the array it is going into would be reading a slot that no longer
        // holds what it held by the time the read is evaluated.
        if(mentions(g, write.value, decl.name)) break;

        auto reachesBack = false;
        for(auto name: declared) reachesBack = reachesBack || mentions(g, write.value, name);
        if(reachesBack) break;

        auto effects = effectsOf(g, write.value);
        if(!crosses(effects, prefix)) break;

        auto values = itemsOf(g, array.values);

        Effects tail;
        for(Size i = slot + 1; i < size; i++) addEffects(g, values[i], tail);
        if(!crosses(effects, tail)) break;

        if(slot < size) {
            if(!effectsOf(g, values[slot]).inert()) break;
            values[slot] = write.value;
        } else {
            array.values.push(g.file.arena, write.value);
        }

        list.remove(g.base, at);
        filled = slot + 1;
        changed = true;
    }

    return changed;
}

/*
 * What is left of an expression whose value nothing wants.
 *
 * A statement position discards its expression, so everything in it that only *computes* is dead and
 * what remains is whatever it does on the way. `upTo(n, body) === 1;` is the everyday case - the
 * comparison was a branch's condition until the branch collapsed - and what the program still has to
 * do is the call.
 *
 * Answers the replacement, or null to discard the statement outright.
 *
 * Two node kinds are deliberately left whole even though the operator itself is pure. A ternary and
 * a short-circuit `&&`/`||` evaluate an operand *conditionally*, so keeping that operand alone would
 * run it on paths that did not - which is the one thing no rewrite in this file may do, and the same
 * rule `eachOperand`'s `conditional` flag exists for.
 *
 * And where more than one operand still does something, the node stays: JS can sequence two effects
 * in one expression with a comma, and this tree has no comma - so the honest answer is to leave a
 * shape it cannot spell rather than to pick one of the two.
 */
JsPtr<Expr> discardedExpr(Gen& g, JsPtr<Expr> pointer) {
    if(effectsOf(g, pointer).inert()) return nullptr;

    auto expr = g.base[pointer];
    if(isEffectful(expr)) return pointer;

    switch(expr->kind) {
        case Expr::Field: case Expr::Index: case Expr::Unary: case Expr::Array: case Expr::Object:
            break;
        case Expr::Binary: {
            auto op = ((BinaryExpr*)expr)->op;
            if(op == BinaryOp::LogicalAnd || op == BinaryOp::LogicalOr) return pointer;
            break;
        }
        default:
            return pointer;
    }

    JsPtr<Expr> only = nullptr;
    auto several = false;

    eachOperand(g, expr, [&](JsPtr<Expr>& operand, bool) {
        if(effectsOf(g, operand).inert()) return;
        if(only) several = true;
        only = operand;
    });

    if(several || !only) return pointer;
    return discardedExpr(g, only);
}

// `f();` where nothing wants the value - see discardedExpr for what that leaves.
bool foldDiscarded(Gen& g, StmtList& list, Size index) {
    auto stmt = g.base[list.get(g.base, index)];
    if(stmt->kind != Stmt::Expression) return false;

    auto& expression = *(ExprStmt*)stmt;
    auto kept = discardedExpr(g, expression.value);

    if(!kept) {
        list.remove(g.base, index);
        return true;
    }

    if(kept == expression.value) return false;

    expression.value = kept;
    return true;
}

/*
 * `if (c) {}` -> `c;`, and `if (c) {} else {}` with it.
 *
 * An `if` whose arms are both empty tests a condition and does nothing with the answer, so what is
 * left of it is whatever evaluating the condition does. That is a statement where the condition has
 * effects and nothing at all where it does not.
 *
 * It is not a shape anything emits on purpose. It is what the passes above *leave*: a branch whose
 * arms were two `return`s of unit becomes two empty lists once flow.cpp has recovered the structure,
 * and `Iter.yana`'s `stopAt` - a `for` loop whose body may stop early, called for its effects -
 * reaches exactly that. Removing it here rather than in `flow.cpp` is the same division the rest of
 * this file works under: the recovery builds one statement per edge, and which of them are worth
 * keeping is a question about the tree it built.
 *
 * The condition is *not* descended into for closure bodies. A function expression inside a discarded
 * condition is still evaluated where the condition is - `effectsOf` says whether that matters - and
 * nothing here moves it anywhere.
 */
bool foldEmptyIf(Gen& g, StmtList& list, Size index) {
    auto pointer = list.get(g.base, index);
    auto stmt = g.base[pointer];
    if(stmt->kind != Stmt::If) return false;

    auto& branch = *(IfStmt*)stmt;
    if(branch.then.isNotEmpty() || branch.otherwise.isNotEmpty()) return false;

    if(effectsOf(g, branch.cond).inert()) {
        list.remove(g.base, index);
        return true;
    }

    list.set(g.base, index, asStmt(g, make<ExprStmt>(g, branch.cond)));
    return true;
}

// Whether one statement has anything at all to do with a name - its own expression, every nested
// body, and every closure reached from either. The conservative half of the sink below: a statement
// a declaration moves past has to be one that cannot tell.
bool mentionsStmt(Gen& g, JsPtr<Stmt> pointer, Name name) {
    auto stmt = g.base[pointer];
    if(auto header = headerOf(g, stmt)) {
        if(mentions(g, *header, name)) return true;
    }

    // A second declaration of the same name is not a read, but moving past one would put two of
    // them out of order - so it stops the search like anything else.
    if(stmt->kind == Stmt::Decl && ((DeclStmt*)stmt)->name.text == name.text) return true;

    auto found = false;
    eachBody(g, stmt, [&](StmtList& body) {
        if(found) return;

        for(auto inner: body.contents(g.base)) {
            if(mentionsStmt(g, inner, name)) {
                found = true;
                return;
            }
        }
    });

    return found;
}

/*
 * `var v = 0; v = x;` -> `var v = x;`
 *
 * The same rewrite as above for a value that is not built property by property. The emitter writes
 * the zero because storage exists before anything fills it - a resolve `Alloc` is one instruction
 * and the `Init` that follows it is another - and where nothing can observe that the storage was
 * ever anything else, the two statements are one.
 *
 * ## Why the write need not be the next statement
 *
 * Because what the fill needs is usually declared *between* the two. An array literal is the
 * everyday case - the owner's slot, then the buffer, then the owner pointed at it:
 *
 *     var v3 = null;  var v4 = [];  v3 = v4;
 *
 * The strict form saw `var v4 = []` in the way and stopped, leaving a binding and two statements
 * where hand-written JS has one.
 *
 * What moves is the *declaration*, downwards, and what makes that invisible is that `var` is
 * function-scoped: the binding exists from the top of the function either way, and all that changes
 * is whether it holds the emitter's zero or `undefined` over a stretch where nothing reads it. So
 * the condition is exactly that - no statement in between mentions the name at all, in its own
 * expression, in any nested body, or in any closure. The assigned value does not move, so nothing
 * has to be said about what it would have crossed.
 */
bool foldInitialValue(Gen& g, StmtList& list, Size index) {
    auto declaration = g.base[list.get(g.base, index)];
    if(declaration->kind != Stmt::Decl) return false;

    auto& decl = *(DeclStmt*)declaration;
    if(decl.constant) return false;
    if(decl.value && !effectsOf(g, decl.value).inert()) return false;

    for(Size at = index + 1; at < list.size(); at++) {
        auto pointer = list.get(g.base, at);
        auto next = g.base[pointer];

        if(next->kind == Stmt::Expression) {
            auto written = g.base[((ExprStmt*)next)->value];

            if(written->kind == Expr::Assign) {
                auto target = g.base[((AssignExpr*)written)->target];

                if(target->kind == Expr::Var && ((VarExpr*)target)->name.text == decl.name.text) {
                    // `v = v + 1` reads what is being replaced, so the zero is observable after all.
                    auto value = ((AssignExpr*)written)->value;
                    if(mentions(g, value, decl.name)) return false;

                    // The declaration becomes the write, in the write's own position. Removing the
                    // original afterwards is what keeps the two rewrites one statement apart rather
                    // than needing the list shifted twice.
                    decl.value = value;
                    list.set(g.base, at, list.get(g.base, index));
                    list.remove(g.base, index);
                    return true;
                }
            }
        }

        // Anything else that has to do with the name stops the search - see mentionsStmt.
        if(mentionsStmt(g, pointer, decl.name)) return false;
    }

    return false;
}

/*
 * `var a = b;` -> every use of `a` becomes `b` - see the `Copy` walker above for what bounds it.
 *
 * The two passes are the same walk run twice, and the declaration is removed only where the first
 * one reached every use the counts know about. That is what keeps this from leaving a binding whose
 * readers have been split between two names, which is correct but larger than what it replaced.
 */
bool propagateCopy(Gen& g, StmtList& list, Size index, Names& names) {
    auto declaration = g.base[list.get(g.base, index)];
    if(declaration->kind != Stmt::Decl) return false;

    auto& decl = *(DeclStmt*)declaration;
    if(!decl.value || decl.constant) return false;
    if(g.base[decl.value]->kind != Expr::Var) return false;

    // A name this pass is going to remove has to be one nothing writes, for the same reason a phi
    // is not a single use: the value at the use would not be the value at the declaration.
    if(names.assigned.contains(decl.name.text)) return false;

    auto source = ((VarExpr*)g.base[decl.value])->name;
    if(source.text == decl.name.text) return false;

    /*
     * A global source is declined outright, because the barrier this pass stops at is an assignment
     * it can see.
     *
     * The premise above is that nothing but this function's own text can change `source`, and a
     * module-level `var` is the one identifier that is false of - any call in between could have
     * assigned it, and none of them is an `Assign` node here to stop at. `var a = seen` with two
     * readers keeps its local, which costs one name and is what the value being read twice means.
     */
    if(isMutableGlobal(g, g.base[decl.value])) return false;

    auto uses = names.useCount(decl.name);
    if(!uses) return false;

    Copy copy { decl.name, source, names.assigned.contains(source.text) };
    propagateList(g, list, index + 1, copy);
    if(copy.count != uses) return false;

    copy.stopped = false;
    copy.count = 0;
    copy.apply = true;
    propagateList(g, list, index + 1, copy);

    list.remove(g.base, index);
    return true;
}

/*
 * A binding nothing reads.
 *
 * The emitter names every instruction's result, and a load whose only consumer turned out to be an
 * ownership operation that costs nothing here has no consumer at all. Removed only where the value
 * writes nothing - a call stays, because what it returns is not why it is there.
 */
bool removeDeadBinding(Gen& g, StmtList& list, Size index, Names& names) {
    auto declaration = g.base[list.get(g.base, index)];
    if(declaration->kind != Stmt::Decl) return false;

    auto& decl = *(DeclStmt*)declaration;
    if(!decl.value || decl.constant || names.useCount(decl.name)) return false;
    if(effectsOf(g, decl.value).writes) return false;

    list.remove(g.base, index);
    return true;
}

// A binding read once and written never becomes its use - see isAtom for how far it travels.
bool inlineBinding(Gen& g, StmtList& list, Size index, Names& names) {
    auto declaration = g.base[list.get(g.base, index)];
    if(declaration->kind != Stmt::Decl) return false;

    auto& decl = *(DeclStmt*)declaration;
    if(!decl.value || decl.constant || !names.isSingleUse(decl.name)) return false;

    if(isAtom(g, decl.value, names)) {
        if(!substituteAfter(g, list, index + 1, decl.name, decl.value)) return false;

        list.remove(g.base, index);
        return true;
    }

    if(index + 1 >= list.size()) return false;

    auto header = headerOf(g, g.base[list.get(g.base, index + 1)]);
    if(!header) return false;

    Substitution substitution { decl.name, decl.value, effectsOf(g, decl.value) };
    substitute(g, *header, substitution, false);
    if(!substitution.done) return false;

    list.remove(g.base, index);
    return true;
}

/*
 * `(c ? A : B) === K` for three number literals, which is one comparison written as two.
 *
 * This is what reading a folded tag and matching on it comes out as, and it is worth folding here
 * rather than at either end because neither end can. The decode produces a *constructor index* -
 * `m === null ? 0 : 1` - because that is what a Discriminant load is defined to produce, and the
 * match produces `=== 0` because that is what testing a constructor is; the two are written in
 * different files against different instructions and are correct separately. Only their composition
 * is redundant, and every folded `match` in a program contains one.
 *
 * Sound whatever the constants are, since the arms are literals: whichever of them equals `K` decides
 * the answer. Where both or neither do, the answer no longer depends on the condition - and the
 * condition may still have an effect, so that case is declined rather than folded to a constant.
 */
bool foldTernaryCompare(Gen& g, JsPtr<Expr>& slot) {
    auto expr = g.base[slot];
    if(expr->kind != Expr::Binary) return false;

    auto& binary = *(BinaryExpr*)expr;
    if(binary.op != BinaryOp::Eq && binary.op != BinaryOp::Ne) return false;

    auto left = g.base[binary.lhs];
    auto right = g.base[binary.rhs];
    if(left->kind != Expr::Ternary || right->kind != Expr::Number) return false;

    auto& choice = *(TernaryExpr*)left;
    auto then = g.base[choice.then];
    auto otherwise = g.base[choice.otherwise];
    if(then->kind != Expr::Number || otherwise->kind != Expr::Number) return false;

    auto against = ((NumberExpr*)right)->value;
    auto matchesThen = ((NumberExpr*)then)->value == against;
    auto matchesOtherwise = ((NumberExpr*)otherwise)->value == against;
    if(matchesThen == matchesOtherwise) return false;

    // `!=` is the same question with the answer the other way round, and one of the two negations
    // cancels against the other.
    auto negate = (binary.op == BinaryOp::Ne) != matchesOtherwise;
    slot = negate ? asExpr(g, make<UnaryExpr>(g, UnaryOp::Not, choice.cond)) : choice.cond;
    return true;
}

// Every expression of one statement, innermost first, so that a fold uncovering another is applied in
// the same walk rather than in the next round.
/*
 * `[a, b, c][1]` -> `b`.
 *
 * A literal indexed by a literal, which is the shape an array whose whole purpose was one element
 * collapses into once `foldArrayElements` has built the literal and the inliner has brought the
 * index home. `ArrayReclaim.yana`'s `frameOnly` is the everyday case: a container built, read at a
 * constant position, and never named again - and what this removes is the allocation, not the
 * subscript.
 *
 * **Every other element has to be inert**, because they are what the rewrite deletes. Their
 * evaluation is what an array literal is for; a call or an assignment among them is an effect the
 * program asked for at a point this would erase. The selected element itself moves nowhere - the
 * literal was evaluated exactly where the subscript is.
 *
 * A negative or out-of-range index is left alone. `[a, b][5]` is `undefined` in JS, which is not a
 * value this compiler's types admit, so folding it would be inventing an answer for a program that
 * cannot arise rather than simplifying one that can.
 */
bool foldConstantIndex(Gen& g, JsPtr<Expr>& slot) {
    auto expr = g.base[slot];
    if(expr->kind != Expr::Index) return false;

    auto& index = *(IndexExpr*)expr;
    auto array = g.base[index.array];
    auto position = g.base[index.index];

    if(array->kind != Expr::Array) return false;
    if(position->kind != Expr::Number || !((NumberExpr*)position)->integral) return false;

    auto& values = ((ArrayExpr*)array)->values;
    auto at = ((NumberExpr*)position)->value;
    if(at < 0 || at >= F64(values.size())) return false;

    auto chosen = Size(at);
    auto items = itemsOf(g, values);

    for(Size i = 0; i < values.size(); i++) {
        if(i == chosen) continue;
        if(!effectsOf(g, items[i]).inert()) return false;
    }

    slot = items[chosen];
    return true;
}

/*
 * Ranges, and the coercions an operand has already performed on itself.
 *
 * `Int` is a wrapping 32-bit integer and JS has no 32-bit integer, so `coerce` in type.cpp puts one
 * of `| 0`, `>>> 0`, `& mask` or `<< n >> n` on the result of every arithmetic operation in the
 * program. Analysis-JS.md §2.1 calls that the asm.js tax and says in the same breath that it is
 * "almost entirely elidable by the *same* range analysis `@bits` already requires" - which is what
 * this is, at the one place that can see the whole expression rather than one instruction of it.
 *
 * It is a range rather than a type because the redundancy is arithmetic, not declarative. `x & 8191`
 * has type `Int` and values in `[0, 8191]`, and it is the second fact that says the `>>> 0` the
 * emitter put on it does nothing. A masked field read, a shifted tag, a bounded loop counter and a
 * literal are all the same shape to this and are what most of the emitted coercions sit on.
 *
 * **A known range asserts three things at once**: the expression evaluates to a JS `number`, that
 * number is a mathematical integer, and it lies in `[low, high]`. All three are load-bearing. A
 * coercion off a boolean would change the *type* of what the surrounding code sees - `true | 0` is
 * `1`, and `x === true` is not `x === 1` - so a comparison and a `!` have no range here even though
 * their values are 0 and 1. A coercion off a fraction is a truncation. And negative zero is excluded
 * by every producer below rather than tracked, since a coercion is exactly what collapses `-0` into
 * `0` and nothing else in the emitted program does.
 *
 * Stale entries are safe for the reason the use counts above are: every rewrite in this file
 * preserves the value of what it rewrites, so a range recorded against an earlier round's tree still
 * bounds the same number.
 */

// 2^53. Past it a double stops counting by ones, so a bound there bounds no integer.
constexpr F64 kExactLimit = 9007199254740992.0;
constexpr F64 kInt32Min = -2147483648.0;
constexpr F64 kInt32Max = 2147483647.0;
constexpr F64 kUint32Max = 4294967295.0;

struct Range {
    F64 low = 0;
    F64 high = 0;
    bool known = false;

    bool within(F64 min, F64 max) const { return known && low >= min && high <= max; }
    bool constant() const { return known && low == high; }
};

Range unknownRange() { return Range(); }

/*
 * A bound *at* the limit may be a rounded one - a sum whose true value is 2^53 + 1 arrives here as
 * 2^53 and is accepted. That is not a hole, because every fold below asks whether a range fits in 32
 * bits and a bound anywhere near this one fails that by twenty-one orders of magnitude. What the
 * limit is actually for is stopping a range from being carried further and *becoming* a 32-bit one.
 */
Range integerRange(F64 low, F64 high) {
    if(low > high || low < -kExactLimit || high > kExactLimit) return unknownRange();
    return Range { low, high, true };
}

/*
 * The ranges of the bindings one function reads.
 *
 * A name is in here only if it is declared with a value and assigned nowhere, which is what makes
 * its range the range of its initializer at every reader. A phi is written by each predecessor and
 * so is never in here; neither is a parameter, which is declared by nothing.
 */
struct Ranges {
    explicit Ranges(Gen& g): imul(literalName(g, "imul"_v)) {}

    HashMap<U32, Range> bindings;
    Name imul;

    void reset() { bindings.reset(); }

    Range of(Name name) {
        auto found = bindings.get(name.text);
        return found ? found.unwrap() : unknownRange();
    }
};

// floor(x / 2^n), which is what both right shifts do once their operand is in range - arithmetic on
// a signed one and logical on an unsigned one, and `>>` on an I64 is both.
F64 shiftedDown(F64 value, U32 by) {
    return F64(I64(value) >> by);
}

// The smallest `2^k - 1` that is at least `value`, for a value below 2^31 - the bound on an `|` or
// a `^` of two operands, neither of which can set a bit above the top one either of them has.
F64 fillBelow(F64 value) {
    auto bits = U64(value);
    bits |= bits >> 1;
    bits |= bits >> 2;
    bits |= bits >> 4;
    bits |= bits >> 8;
    bits |= bits >> 16;
    return F64(bits);
}

Range rangeOf(Gen& g, JsPtr<Expr> pointer, Ranges& ranges);

Range rangeOfBinary(Gen& g, BinaryExpr& expr, Ranges& ranges) {
    auto lhs = rangeOf(g, expr.lhs, ranges);
    auto rhs = rangeOf(g, expr.rhs, ranges);

    switch(expr.op) {
        case BinaryOp::Add:
            if(!lhs.known || !rhs.known) return unknownRange();
            return integerRange(lhs.low + rhs.low, lhs.high + rhs.high);
        case BinaryOp::Sub:
            if(!lhs.known || !rhs.known) return unknownRange();
            return integerRange(lhs.low - rhs.high, lhs.high - rhs.low);
        case BinaryOp::Mul: {
            // Both non-negative, because `-1 * 0` is `-0` and a range that merely contains zero
            // cannot tell that case from the one that produces `+0`.
            if(!lhs.within(0, kExactLimit) || !rhs.within(0, kExactLimit)) return unknownRange();
            return integerRange(lhs.low * rhs.low, lhs.high * rhs.high);
        }
        case BinaryOp::And:
            /*
             * A mask decides the result on its own: `x & m` for `0 <= m <= 2^31 - 1` clears every
             * bit above m's top one whatever x is, so the result is non-negative and no larger.
             * This is the rule that pays for itself - a narrow field read, a tag decode and a
             * `coerce` of a sub-32-bit unsigned type all end in one.
             */
            if(rhs.constant() && rhs.low >= 0 && rhs.low <= kInt32Max) return integerRange(0, rhs.low);
            if(lhs.constant() && lhs.low >= 0 && lhs.low <= kInt32Max) return integerRange(0, lhs.low);

            // Failing that, two non-negative operands cannot produce a bit neither of them has.
            if(lhs.within(0, kInt32Max) && rhs.within(0, kInt32Max)) {
                return integerRange(0, lhs.high < rhs.high ? lhs.high : rhs.high);
            }

            return integerRange(kInt32Min, kInt32Max);
        case BinaryOp::Or:
        case BinaryOp::Xor:
            if(lhs.within(0, kInt32Max) && rhs.within(0, kInt32Max)) {
                return integerRange(0, fillBelow(lhs.high > rhs.high ? lhs.high : rhs.high));
            }

            return integerRange(kInt32Min, kInt32Max);
        case BinaryOp::Shl:
            return integerRange(kInt32Min, kInt32Max);
        case BinaryOp::Shr: {
            // JS masks a shift count to five bits, so only a literal below 32 says what the shift
            // is. Everything else is still ToUint32 of something, which is the range on its own.
            if(!rhs.constant() || rhs.low < 0 || rhs.low >= 32) return integerRange(0, kUint32Max);

            auto by = U32(rhs.low);
            if(lhs.within(0, kUint32Max)) {
                return integerRange(shiftedDown(lhs.low, by), shiftedDown(lhs.high, by));
            }

            return integerRange(0, F64((U64(1) << (32 - by)) - 1));
        }
        case BinaryOp::Sar: {
            if(!rhs.constant() || rhs.low < 0 || rhs.low >= 32) return integerRange(kInt32Min, kInt32Max);

            auto by = U32(rhs.low);
            if(lhs.within(kInt32Min, kInt32Max)) {
                return integerRange(shiftedDown(lhs.low, by), shiftedDown(lhs.high, by));
            }

            auto bound = F64(U64(1) << (31 - by));
            return integerRange(-bound, bound - 1);
        }
        default:
            // `/` is a float divide, `%` keeps its dividend's sign, and everything left produces a
            // boolean or one of its operands.
            return unknownRange();
    }
}

/*
 * `Math.imul` is the target's 32-bit multiply and the only host intrinsic whose result has a width.
 * `Number` produces a number of no known range, `Math.fround` produces a fraction, `BigInt.asIntN`
 * produces a bigint, and the 33-to-53-bit helpers belong to a tower this does not reach into.
 */
Range rangeOfCall(Gen& g, CallExpr& expr, Ranges& ranges) {
    if(!expr.pure || expr.wideBits != 0) return unknownRange();

    auto callee = g.base[expr.callee];
    if(callee->kind != Expr::Field) return unknownRange();
    if(((FieldExpr*)callee)->field.text != ranges.imul.text) return unknownRange();

    return integerRange(kInt32Min, kInt32Max);
}

Range rangeOf(Gen& g, JsPtr<Expr> pointer, Ranges& ranges) {
    auto expr = g.base[pointer];

    switch(expr->kind) {
        case Expr::Number: {
            auto value = ((NumberExpr*)expr)->value;
            if(value < -kExactLimit || value > kExactLimit) return unknownRange();
            if(F64(I64(value)) != value) return unknownRange();

            // `-0` is an integer with two spellings and a coercion is what picks one, so it is not
            // an integer this may claim to know.
            if(value == 0 && 1.0 / value < 0) return unknownRange();

            return integerRange(value, value);
        }
        case Expr::Var:
            return ranges.of(((VarExpr*)expr)->name);
        case Expr::Unary: {
            auto& unary = *(UnaryExpr*)expr;
            auto value = rangeOf(g, unary.value, ranges);

            switch(unary.op) {
                case UnaryOp::Neg:
                    // Away from zero in both directions, since `-0` is what negating `0` produces.
                    if(!value.known || (value.low <= 0 && value.high >= 0)) return unknownRange();
                    return integerRange(-value.high, -value.low);
                case UnaryOp::BitNot:
                    // ToInt32 first, and `~x` is `-1 - x` of what that produced.
                    if(value.within(kInt32Min, kInt32Max)) {
                        return integerRange(-1 - value.high, -1 - value.low);
                    }

                    return integerRange(kInt32Min, kInt32Max);
                default:
                    return unknownRange();
            }
        }
        case Expr::Binary:
            return rangeOfBinary(g, *(BinaryExpr*)expr, ranges);
        case Expr::Ternary: {
            auto& choice = *(TernaryExpr*)expr;
            auto then = rangeOf(g, choice.then, ranges);
            auto otherwise = rangeOf(g, choice.otherwise, ranges);
            if(!then.known || !otherwise.known) return unknownRange();

            return integerRange(then.low < otherwise.low ? then.low : otherwise.low,
                                then.high > otherwise.high ? then.high : otherwise.high);
        }
        case Expr::Assign:
            return rangeOf(g, ((AssignExpr*)expr)->value, ranges);
        case Expr::Call:
            return rangeOfCall(g, *(CallExpr*)expr, ranges);
        default:
            return unknownRange();
    }
}

void collectRanges(Gen& g, StmtList& list, Names& names, Ranges& ranges);

void collectStmt(Gen& g, JsPtr<Stmt> pointer, Names& names, Ranges& ranges) {
    auto stmt = g.base[pointer];

    if(stmt->kind == Stmt::Decl) {
        auto& declaration = *(DeclStmt*)stmt;
        if(declaration.value && !names.assigned.contains(declaration.name.text)) {
            auto range = rangeOf(g, declaration.value, ranges);
            if(range.known) ranges.bindings.add(declaration.name.text, range);
        }
    }

    eachBody(g, stmt, [&](StmtList& body) { collectRanges(g, body, names, ranges); });
}

// In the order the statements are written, which is the order the bindings are defined in: a resolve
// value is used where its definition dominates, so a declaration is always reached before its
// readers are. A lookup that misses is unknown, so getting this wrong would cost precision only.
void collectRanges(Gen& g, StmtList& list, Names& names, Ranges& ranges) {
    for(auto stmt: list.contents(g.base)) collectStmt(g, stmt, names, ranges);
}

/*
 * `x | 0`, `x >>> 0` and `x & mask` where x is already inside what they would put it in.
 *
 * The mask has to be a run of low bits. `x & 10` on an `x` in `[0, 10]` is not `x` - 6 is in range
 * and `6 & 10` is 2 - so "no larger than the mask" is only the same question as "no bits outside
 * the mask" when the mask has no holes. Every mask `coerce` builds is `2^bits - 1`, so this costs
 * nothing and is the check that makes the rule true rather than usually true.
 */
bool foldCoercion(Gen& g, JsPtr<Expr>& slot, Ranges& ranges) {
    auto expr = g.base[slot];
    if(expr->kind != Expr::Binary) return false;

    auto& binary = *(BinaryExpr*)expr;
    auto value = binary.lhs;
    auto amount = rangeOf(g, binary.rhs, ranges);

    /*
     * `&`, `|` and `^` are commutative, so the constant may be on either side and the coercion is
     * the same one - `0 | x` is what `x | 0` is. That is not a spelling the emitter writes: it is
     * what a read-modify-write becomes once the word it reads has been propagated as a literal zero,
     * and the shifts below are excluded because they are not commutative.
     */
    if(!amount.constant() && (binary.op == BinaryOp::And || binary.op == BinaryOp::Or ||
                              binary.op == BinaryOp::Xor)) {
        amount = rangeOf(g, binary.lhs, ranges);
        value = binary.rhs;
    }

    if(!amount.constant()) return false;

    auto by = amount.low;
    F64 low = 0;
    F64 high = 0;

    switch(binary.op) {
        case BinaryOp::Or:
        case BinaryOp::Xor:
            // `x | 0` and `x ^ 0` are both ToInt32 and nothing else.
            if(by != 0) return false;
            low = kInt32Min;
            high = kInt32Max;
            break;
        case BinaryOp::Shr:
            if(by != 0) return false;
            low = 0;
            high = kUint32Max;
            break;
        case BinaryOp::And:
            if(by < 0 || by > kInt32Max || (U64(by) & (U64(by) + 1)) != 0) return false;
            low = 0;
            high = by;
            break;
        default:
            return false;
    }

    if(!rangeOf(g, value, ranges).within(low, high)) return false;

    slot = value;
    return true;
}

/*
 * `x << n >> n` - the sign extension a signed type narrower than 32 bits is coerced with, on a value
 * that already fits through it.
 *
 * Written as one rule rather than as two applications of the one above because neither half is
 * removable alone: `x << n` on a narrow x is not x, and the `>> n` is what puts it back.
 */
bool foldSignExtend(Gen& g, JsPtr<Expr>& slot, Ranges& ranges) {
    auto expr = g.base[slot];
    if(expr->kind != Expr::Binary) return false;

    auto& outer = *(BinaryExpr*)expr;
    if(outer.op != BinaryOp::Sar) return false;

    auto inner = g.base[outer.lhs];
    if(inner->kind != Expr::Binary || ((BinaryExpr*)inner)->op != BinaryOp::Shl) return false;

    auto& shift = *(BinaryExpr*)inner;
    auto up = rangeOf(g, shift.rhs, ranges);
    auto down = rangeOf(g, outer.rhs, ranges);
    if(!up.constant() || !down.constant() || up.low != down.low) return false;
    if(up.low <= 0 || up.low >= 32) return false;

    // What survives a shift up by n and back is the low `32 - n` bits read as signed.
    auto bound = F64(U64(1) << (31 - U32(up.low)));
    if(!rangeOf(g, shift.lhs, ranges).within(-bound, bound - 1)) return false;

    slot = shift.lhs;
    return true;
}

/*
 * Working out what a call to one of this compiler's own helpers comes to.
 *
 * A bit range wider than the host's operators is reached by arithmetic rather than by masking, and
 * because that arithmetic is long it is emitted once as `$p20u$set` and called - so a construction
 * whose every operand is a literal ends at a *call* rather than at an expression, and constant
 * folding stops there. `writeHigh` in WidePack.yana is the shape: two literal fields packed into one
 * 52-bit word, which native answers with a single `store` of the finished number and this answered
 * with two calls.
 *
 * **The helper's own body is what is evaluated**, rather than a table of what each one means. That
 * is the whole design: the helpers exist so that there is one statement of how a 40-bit field is
 * wrapped, and a second statement of it here - in C++, checked by nothing - is precisely the bug the
 * helpers were introduced to make impossible. So this reads the `FunStmt` the emitter already built,
 * binds its parameters to the call's arguments, and interprets it. A helper that changes shape is
 * followed automatically; one this interpreter cannot handle simply stops folding.
 *
 * Their bodies are `var` bindings followed by one `return`, which is what `emitBitHelpers` and
 * `emitWideHelpers` produce and all this understands. Everything else - a loop, a branch, a call to
 * something that is not a helper - answers nothing and the call stays.
 */

// One value in flight. Booleans are here because a helper's `return` is routinely a ternary over a
// comparison - `v < 0 ? v + 2**40 : v` is what wrapping to an unsigned range is.
struct Known {
    F64 number = 0;
    bool boolean = false;
    bool isBoolean = false;

    static Known ofNumber(F64 v) { return Known { v, false, false }; }
    static Known ofBoolean(bool v) { return Known { 0, v, true }; }

    // Host truthiness, which a ternary condition is read through. Only asked of the two kinds this
    // produces, and `-0` never reaches here - see constantNumber.
    bool truthy() const { return isBoolean ? boolean : number != 0; }
};

// What one name is bound to while a body is being interpreted: the parameters, then whatever its
// `var`s worked out to.
struct Bindings {
    HashMap<U32, Known> values;
};

// Deep enough for a bit-range helper reaching a wrap reaching `$wi$hi`, and bounded so that a
// helper that somehow called itself would stop rather than run out of stack.
constexpr U32 kMaxEvalDepth = 8;

bool evalExpr(Gen& g, JsPtr<Expr> pointer, Bindings& bindings, U32 depth, Known& into);

// A helper's declaration, by the name a call names. Null for a call to anything else, which is what
// keeps this from interpreting the *program* - a user function may do anything, and what it answers
// is not a question this pass asks.
FunStmt* helperBody(Gen& g, Name name) {
    auto generated = false;
    for(auto& helper: g.wideHelperOrder) if(helper.name.text == name.text) generated = true;
    for(auto& helper: g.bitHelperOrder) if(helper.name.text == name.text) generated = true;
    if(!generated) return nullptr;

    for(auto pointer: g.file.statements.contents(g.base)) {
        auto stmt = g.base[pointer];
        if(stmt->kind != Stmt::Fun) continue;

        auto fun = (FunStmt*)stmt;
        if(fun->name.text == name.text) return fun;
    }

    return nullptr;
}

// `Math.floor(x)`, `Math.imul(a, b)` and `Math.pow(2, n)` - the three host intrinsics a helper body
// or an emitted expression can contain. `Math.pow` only at base two, since that is the only case
// this can answer exactly without reaching for libm.
//
// The member is compared as an interned name rather than as text, because that is how the emitter
// wrote it: `hostCall` goes through `literalName`, so the same spelling is the same id.
bool evalHostCall(Gen& g, CallExpr& call, Name member, Bindings& bindings, U32 depth, Known& into) {
    Known first, second;
    auto args = call.args.size();

    if(args >= 1 && !evalExpr(g, call.args.get(g.base, 0), bindings, depth, first)) return false;
    if(args >= 2 && !evalExpr(g, call.args.get(g.base, 1), bindings, depth, second)) return false;
    if(first.isBoolean || second.isBoolean) return false;

    if(args == 1 && member.text == literalName(g, "floor"_v).text) {
        if(!fitsInt32Conversion(first.number)) return false;
        into = Known::ofNumber(floorOf(first.number));
        return true;
    }

    if(args == 2 && member.text == literalName(g, "imul"_v).text) {
        if(!fitsInt32Conversion(first.number) || !fitsInt32Conversion(second.number)) return false;
        into = Known::ofNumber(F64(I32(U32(toInt32(first.number)) * U32(toInt32(second.number)))));
        return true;
    }

    if(args == 2 && member.text == literalName(g, "pow"_v).text && first.number == 2) {
        if(second.number < 0 || second.number > 1023 || !isExactInteger(second.number)) return false;
        into = Known::ofNumber(powerOfTwo(U32(second.number)));
        return true;
    }

    return false;
}

// A call to a helper: its parameters bound to the evaluated arguments, and its body interpreted in
// a scope of its own.
bool evalHelperCall(Gen& g, FunStmt& helper, CallExpr& call, Bindings& outer, U32 depth,
                    Known& into) {
    if(helper.args.size() != call.args.size()) return false;

    Bindings inner;
    for(Size i = 0; i < helper.args.size(); i++) {
        Known argument;
        if(!evalExpr(g, call.args.get(g.base, i), outer, depth, argument)) return false;

        inner.values.add(helper.args.get(g.base, i).text, argument);
    }

    for(auto pointer: helper.body.contents(g.base)) {
        auto stmt = g.base[pointer];

        if(stmt->kind == Stmt::Decl) {
            auto& declaration = *(DeclStmt*)stmt;
            Known value;
            if(!declaration.value) return false;
            if(!evalExpr(g, declaration.value, inner, depth + 1, value)) return false;

            inner.values.add(declaration.name.text, value);
            continue;
        }

        if(stmt->kind == Stmt::Return) {
            auto value = ((ReturnStmt*)stmt)->value;
            return value && evalExpr(g, value, inner, depth + 1, into);
        }

        return false;
    }

    return false;
}

bool evalExpr(Gen& g, JsPtr<Expr> pointer, Bindings& bindings, U32 depth, Known& into) {
    if(depth >= kMaxEvalDepth) return false;

    auto expr = g.base[pointer];

    switch(expr->kind) {
        case Expr::Number: {
            F64 value;
            if(!constantNumber(g, pointer, value)) return false;

            into = Known::ofNumber(value);
            return true;
        }

        case Expr::Bool:
            into = Known::ofBoolean(((BoolExpr*)expr)->value);
            return true;

        case Expr::Var: {
            auto found = bindings.values.getValue(((VarExpr*)expr)->name.text);
            if(!found) return false;

            into = found.unwrap();
            return true;
        }

        case Expr::Unary: {
            auto& node = *(UnaryExpr*)expr;
            Known operand;
            if(!evalExpr(g, node.value, bindings, depth, operand)) return false;

            if(node.op == UnaryOp::Not) {
                into = Known::ofBoolean(!operand.truthy());
                return true;
            }

            F64 result;
            if(operand.isBoolean || !applyJsUnary(node.op, operand.number, result)) return false;

            into = Known::ofNumber(result);
            return true;
        }

        case Expr::Binary: {
            auto& node = *(BinaryExpr*)expr;
            Known lhs, rhs;
            if(!evalExpr(g, node.lhs, bindings, depth, lhs)) return false;
            if(!evalExpr(g, node.rhs, bindings, depth, rhs)) return false;
            if(lhs.isBoolean || rhs.isBoolean) return false;

            // The comparisons, which nothing but a helper's own ternary asks for. `===` and `!==` on
            // two numbers are the same test as `==` and `!=`, and the loose pair this tree also has
            // is only ever built against a reference - so neither reaches here with numbers.
            switch(node.op) {
                case BinaryOp::Lt: into = Known::ofBoolean(lhs.number <  rhs.number); return true;
                case BinaryOp::Le: into = Known::ofBoolean(lhs.number <= rhs.number); return true;
                case BinaryOp::Gt: into = Known::ofBoolean(lhs.number >  rhs.number); return true;
                case BinaryOp::Ge: into = Known::ofBoolean(lhs.number >= rhs.number); return true;
                case BinaryOp::Eq: into = Known::ofBoolean(lhs.number == rhs.number); return true;
                case BinaryOp::Ne: into = Known::ofBoolean(lhs.number != rhs.number); return true;
                default: break;
            }

            F64 result;
            if(!applyJsBinary(node.op, lhs.number, rhs.number, result)) return false;

            into = Known::ofNumber(result);
            return true;
        }

        case Expr::Ternary: {
            auto& node = *(TernaryExpr*)expr;
            Known condition;
            if(!evalExpr(g, node.cond, bindings, depth, condition)) return false;

            return evalExpr(g, condition.truthy() ? node.then : node.otherwise, bindings, depth, into);
        }

        case Expr::Call: {
            auto& node = *(CallExpr*)expr;
            auto callee = g.base[node.callee];

            if(callee->kind == Expr::Field) {
                auto& member = *(FieldExpr*)callee;
                auto object = g.base[member.object];

                // `Math.x(...)` and nothing else - a method on anything the program built is a call
                // this may not make.
                if(object->kind != Expr::Var) return false;
                if(((VarExpr*)object)->name.text != literalName(g, "Math"_v).text) return false;

                return evalHostCall(g, node, member.field, bindings, depth, into);
            }

            if(callee->kind != Expr::Var) return false;

            auto helper = helperBody(g, ((VarExpr*)callee)->name);
            return helper && evalHelperCall(g, *helper, node, bindings, depth + 1, into);
        }

        default:
            return false;
    }
}

// A call whose arguments are all known, as the number it answers - or null where any part of it is
// something this cannot evaluate, which is the ordinary case.
JsPtr<Expr> foldCall(Gen& g, CallExpr& call) {
    Bindings empty;
    Known result;

    if(!evalExpr(g, (Expr*)&call - g.base, empty, 0, result)) return nullptr;
    if(result.isBoolean) return nullptr;

    return numberLiteral(g, result.number);
}

/*
 * Whether an expression certainly evaluates to a `number`.
 *
 * Asked by the one identity below, and the reason it has to be asked at all is that JavaScript's
 * arithmetic coerces: `"5" - 0` is `5` where `"5"` is not, so `x - 0` is only the identity for an
 * `x` that is already a number. Nothing here guesses - an unrecognized shape answers no.
 */
bool isNumericExpr(Gen& g, JsPtr<Expr> pointer) {
    auto expr = g.base[pointer];

    switch(expr->kind) {
        case Expr::Number:
            return true;

        case Expr::Unary: {
            auto op = ((UnaryExpr*)expr)->op;
            return op == UnaryOp::Neg || op == UnaryOp::BitNot;
        }

        case Expr::Binary: {
            // Every arithmetic and bitwise operator but `+` answers a number whatever it was given.
            // `+` concatenates two strings, so it is a number only where both sides already are.
            auto& node = *(BinaryExpr*)expr;

            switch(node.op) {
                case BinaryOp::Sub: case BinaryOp::Mul: case BinaryOp::Div: case BinaryOp::Rem:
                case BinaryOp::Shl: case BinaryOp::Shr: case BinaryOp::Sar:
                case BinaryOp::And: case BinaryOp::Or:  case BinaryOp::Xor:
                    return true;
                case BinaryOp::Add:
                    return isNumericExpr(g, node.lhs) && isNumericExpr(g, node.rhs);
                default:
                    return false;
            }
        }

        case Expr::Call: {
            // One of this compiler's own helpers, or a `Math` intrinsic. `BigInt(x)` is `pure` too
            // and answers a `bigint`, which `-` refuses to mix with a number at all - so the test is
            // which call it is rather than whether it has effects.
            auto& node = *(CallExpr*)expr;
            auto callee = g.base[node.callee];

            if(callee->kind == Expr::Var) return helperBody(g, ((VarExpr*)callee)->name) != nullptr;
            if(callee->kind != Expr::Field) return false;

            auto object = g.base[((FieldExpr*)callee)->object];
            return object->kind == Expr::Var &&
                   ((VarExpr*)object)->name.text == literalName(g, "Math"_v).text;
        }

        default:
            return false;
    }
}

/*
 * `x - 0`, which is what a read-modify-write becomes once the word it read has been propagated as a
 * literal zero - `$w40u$wrap(v) - $w40u$wrap(0)` in `stored`, where only the second half folded.
 *
 * The subtraction alone. `x + 0` and `0 + x` both answer `+0` for an `x` of `-0`, so neither is the
 * identity there; `x - 0` is, because subtracting zero preserves the sign of a zero.
 */
bool foldNumericIdentity(Gen& g, JsPtr<Expr>& slot) {
    auto expr = g.base[slot];
    if(expr->kind != Expr::Binary) return false;

    auto& node = *(BinaryExpr*)expr;
    if(node.op != BinaryOp::Sub) return false;

    F64 amount;
    if(!constantNumber(g, node.rhs, amount) || amount != 0) return false;
    if(!isNumericExpr(g, node.lhs)) return false;

    slot = node.lhs;
    return true;
}

/*
 * An integer operation whose operands have both become literals, evaluated.
 *
 * The emitter already folds what it can see - see `binary` in build.h - and this is the same rules
 * over what only *becomes* constant here. A coercion is what stands between them: `(2 & 3) << 2` is
 * a mask over a literal, and the mask is removed by the rule below rather than by the emitter, which
 * has no ranges to decide it with. So the two run in one loop.
 */
bool foldConstantOp(Gen& g, JsPtr<Expr>& slot) {
    auto expr = g.base[slot];

    if(expr->kind == Expr::Binary) {
        auto& node = *(BinaryExpr*)expr;
        auto folded = foldBinaryOp(g, node.op, node.lhs, node.rhs);
        if(!folded) folded = foldComparison(g, node.op, node.lhs, node.rhs);

        if(folded) {
            slot = folded;
            return true;
        }

        return false;
    }

    if(expr->kind == Expr::Unary) {
        auto& node = *(UnaryExpr*)expr;
        if(auto folded = foldUnaryOp(g, node.op, node.value)) {
            slot = folded;
            return true;
        }

        return false;
    }

    if(expr->kind == Expr::Call) {
        if(auto folded = foldCall(g, *(CallExpr*)expr)) {
            slot = folded;
            return true;
        }
    }

    return false;
}

bool foldExprs(Gen& g, JsPtr<Expr>& slot, Ranges& ranges) {
    auto changed = false;
    auto expr = g.base[slot];

    /*
     * An assignment's target is walked one level in, because every rule here rewrites a node into
     * the value it evaluates to and a target names storage instead.
     *
     * `[1, 2, 3][2]` is `3` as a value and is not `3` as a place, so `foldConstantIndex` reaching a
     * target emitted `3 = 30` - not a program, and rejected by the parser rather than miscompiled,
     * which is the only reason it was ever going to be found this way. What the target *contains* is
     * ordinary values again: the array being indexed and the index itself both fold on the usual
     * terms, and only the node in the assigned position is off limits.
     */
    if(expr->kind == Expr::Assign) {
        auto& assign = *(AssignExpr*)expr;
        eachOperand(g, g.base[assign.target], [&](JsPtr<Expr>& operand, bool) {
            changed = foldExprs(g, operand, ranges) || changed;
        });

        changed = foldExprs(g, assign.value, ranges) || changed;
    } else {
        eachOperand(g, expr, [&](JsPtr<Expr>& operand, bool) {
            changed = foldExprs(g, operand, ranges) || changed;
        });
    }

    // Both are shape rules over one node, so either may expose the other's shape - a folded ternary
    // can become the index of a subscript, and a folded subscript can become a compared operand.
    auto folded = foldConstantIndex(g, slot);
    folded = foldTernaryCompare(g, slot) || folded;

    // Last, and repeatedly: removing one coercion is what puts the next one against an operand
    // whose range it can see, and `(x & 7) >>> 0 | 0` is three of them in a row. Evaluation joins
    // them because each feeds the other - a removed mask leaves a literal to evaluate, and an
    // evaluated operand is a range the next coercion can see.
    while(foldSignExtend(g, slot, ranges) || foldCoercion(g, slot, ranges) ||
          foldConstantOp(g, slot) || foldNumericIdentity(g, slot)) {
        folded = true;
    }

    return folded || changed;
}

/*
 * Bitwise fusion, which is the one rewrite here that reads a *type* rather than the tree's shape.
 *
 * Top-down rather than innermost-first, unlike foldExprs: the whole of what fusion does is take a
 * maximal tree apart at once, and a fused inner call is no longer a call for the outer one to
 * recognize. So a match consumes its subtree and the walk does not descend into what it produced -
 * which has no wide calls left in it in any case.
 */
bool fuseExprs(Gen& g, JsPtr<Expr>& slot) {
    if(auto fused = fuseWideBitwise(g, slot)) {
        slot = fused;
        return true;
    }

    auto changed = false;
    eachOperand(g, g.base[slot], [&](JsPtr<Expr>& operand, bool) {
        changed = fuseExprs(g, operand) || changed;
    });

    return changed;
}

bool fuseList(Gen& g, StmtList& list) {
    auto changed = false;

    for(auto pointer: list.contents(g.base)) {
        auto stmt = g.base[pointer];
        if(auto header = headerOf(g, stmt)) changed = fuseExprs(g, *header) || changed;

        eachBody(g, stmt, [&](StmtList& body) { changed = fuseList(g, body) || changed; });
    }

    return changed;
}

/*
 * What a local holds *at a point*, carried forward through a name that is written more than once.
 *
 * `Ranges` above answers the same question for a binding nothing assigns, and that is where it has
 * to stop: it is one answer per name for the whole function, so a name written twice has none. The
 * shape this exists for is written twice on purpose. A packed word is built by initializing storage
 * and then writing each field into it -
 *
 *     var v0 = 0;
 *     v0 = v0 + (1000 - (v0 & 1048575));
 *     v0 = $p20u$set(v0, 2.3283064365386963E-10, 4294967296, 1048575);
 *
 * - so every operand of the arithmetic *is* a constant, and nothing could say so. Native reaches the
 * same program through `promoteStackSlots` and folds it to one store of one number; this is what
 * lets the two targets agree about that rather than only about the answer.
 *
 * ## What makes it sound
 *
 * **Only a name this list declares.** A local `var` is invisible to every callee - the premise
 * `propagateCopy` states outright - so nothing between two statements here can write one. A
 * module-level global is exactly the thing that is false of, and it is excluded by construction
 * rather than by a test: a name enters the table only at its `DeclStmt`.
 *
 * **A statement with a body of its own erases what it writes.** A loop is the case that matters -
 * what a name holds at the top of the second iteration is not what it held at the first - and an
 * `if` would need the two arms merged. Neither is worth the machinery: the walk continues past such
 * a statement having forgotten every name it assigns, and the bodies are walked in their own right
 * by `optimizeList`'s recursion, each starting from what it can see for itself.
 *
 * **Nothing is carried that cannot be written back.** The table holds the exact integers
 * `numberLiteral` would emit and nothing else, so a substitution is always a literal in place of a
 * read and never a fraction, an infinity or a `-0`.
 */
struct Constants {
    struct Entry {
        Name name;
        F64 value;
    };

    // Inline, and one of these per list per round: an emitted body has a handful of numeric locals.
    SmallArray<Entry, 8> entries;

    F64* find(Name name) {
        for(auto& entry: entries) if(entry.name.text == name.text) return &entry.value;
        return nullptr;
    }

    void set(Name name, F64 value) {
        if(auto existing = find(name)) *existing = value;
        else entries.push(Entry { name, value });
    }

    void erase(Name name) {
        for(Size i = 0; i < entries.size(); i++) {
            if(entries[i].name.text == name.text) {
                entries.remove(i);
                return;
            }
        }
    }
};

// A value the table may hold - the same rule `numberLiteral` writes one back under.
bool carriedValue(Gen& g, JsPtr<Expr> pointer, F64& into) {
    return constantNumber(g, pointer, into) && isExactInteger(into);
}

/*
 * Every *read* of a tracked name replaced by what it holds.
 *
 * The assigned position of an assignment is walked one level in and its own name left alone, for the
 * reason foldExprs gives about its own rules: a target names storage, and `1000 = x` is not a
 * program. What that target *contains* is reads again - `o[i]` reads both `o` and `i`.
 */
bool substituteConstants(Gen& g, JsPtr<Expr>& slot, Constants& known) {
    auto expr = g.base[slot];
    auto changed = false;

    if(expr->kind == Expr::Var) {
        auto held = known.find(((VarExpr*)expr)->name);
        if(!held) return false;

        auto literal = numberLiteral(g, *held);
        if(!literal) return false;

        slot = literal;
        return true;
    }

    if(expr->kind == Expr::Assign) {
        auto& assign = *(AssignExpr*)expr;
        auto target = g.base[assign.target];

        if(target->kind != Expr::Var) {
            eachOperand(g, target, [&](JsPtr<Expr>& operand, bool) {
                changed = substituteConstants(g, operand, known) || changed;
            });
        }

        return substituteConstants(g, assign.value, known) || changed;
    }

    eachOperand(g, expr, [&](JsPtr<Expr>& operand, bool) {
        changed = substituteConstants(g, operand, known) || changed;
    });

    return changed;
}

bool propagateConstants(Gen& g, StmtList& list) {
    Constants known;

    // The names this list introduced, which is what makes a write to one a write to a *local*. A
    // name assigned here without being declared here may be a module-level global, and this pass's
    // whole premise - that nothing between two statements can change it - is false of one.
    HashSet<U32> declared;
    auto changed = false;

    for(auto pointer: list.contents(g.base)) {
        auto stmt = g.base[pointer];

        // The reads first, whatever the statement is: an `if` condition and a `return` value are
        // reads exactly as an assignment's right-hand side is.
        if(auto header = headerOf(g, stmt)) {
            changed = substituteConstants(g, *header, known) || changed;
        }

        /*
         * Then everything this statement writes stops being known - the name of a plain assignment,
         * whatever a loop or a branch assigns inside itself, and an assignment buried in a larger
         * expression alike. Asked of the whole statement rather than of the shapes below, because
         * `var a = (b = 5)` writes two names and only one of them is the one being declared.
         */
        Size at = 0;
        while(at < known.entries.size()) {
            if(assignsName(g, pointer, known.entries[at].name)) known.entries.remove(at);
            else at++;
        }

        // And last, what this statement makes known. A `var` introduces the only names tracked at
        // all; an assignment may only update one that is already here, since a name this list did
        // not declare could be a global that any call in between had written.
        F64 value;

        if(stmt->kind == Stmt::Decl) {
            auto& declaration = *(DeclStmt*)stmt;
            declared.add(declaration.name.text);

            if(declaration.value && carriedValue(g, declaration.value, value)) {
                known.set(declaration.name, value);
            } else {
                known.erase(declaration.name);
            }
        } else if(stmt->kind == Stmt::Expression) {
            auto written = g.base[((ExprStmt*)stmt)->value];
            if(written->kind != Expr::Assign) continue;

            auto& assign = *(AssignExpr*)written;
            auto target = g.base[assign.target];
            if(target->kind != Expr::Var) continue;

            auto name = ((VarExpr*)target)->name;
            if(declared.contains(name.text) && carriedValue(g, assign.value, value)) {
                known.set(name, value);
            }
        }
    }

    return changed;
}

bool optimizeList(Gen& g, StmtList& list, Names& names, Ranges& ranges) {
    // First, because it is what turns an operand into a literal for every rule below to work on -
    // and it reads the list rather than editing it, so nothing it sees can have moved.
    auto changed = propagateConstants(g, list);
    Size index = 0;

    while(index < list.size()) {
        auto stmt = g.base[list.get(g.base, index)];
        eachBody(g, stmt, [&](StmtList& body) {
            changed = optimizeList(g, body, names, ranges) || changed;
        });

        if(auto header = headerOf(g, stmt)) changed = foldExprs(g, *header, ranges) || changed;

        // Both rewrites shorten the list, so the same position is looked at again rather than the
        // next one - which is what collapses a chain of one-use bindings in a single walk.
        // Before the binding rules, because collapsing an `if` can leave the statement in front of
        // it and the one behind it adjacent - which is what the two fold rules ask about. And the
        // discard rule after it, since what an emptied `if` leaves behind is a discarded condition.
        if(foldEmptyIf(g, list, index) || foldDiscarded(g, list, index)) {
            changed = true;
            continue;
        }

        if(foldInitializers(g, list, index) || foldArrayElements(g, list, index) ||
           foldInitialValue(g, list, index)) {
            changed = true;
            continue;
        }

        if(removeDeadBinding(g, list, index, names)) {
            changed = true;
            continue;
        }

        // Before inlining rather than after it, because the two overlap on a single-use alias and
        // this is the one that answers it without asking anything to move.
        if(propagateCopy(g, list, index, names)) {
            changed = true;
            continue;
        }

        if(inlineBinding(g, list, index, names)) {
            changed = true;
            continue;
        }

        index++;
    }

    return changed;
}

/*
 * Use counts go stale as the rewrites run - folding an initializer removes a mention of the object
 * - and they go stale in the safe direction: a count that is too high only means a binding is left
 * alone. So one pass runs against one set of counts, and what the pass uncovers is picked up by the
 * next. Each round that changes anything removes at least one statement, so this terminates.
 *
 * Fusion is the one thing here that goes stale the *other* way, which is why it is a phase of its
 * own rather than another rewrite in the list. It mentions a leaf twice, so a name the counts still
 * call single-use has two readers afterwards - and inlining one of them would leave the other
 * reading a binding that no longer exists. Running it only between complete rounds is what
 * guarantees the counts are rebuilt before anything acts on them again.
 *
 * It also wants to run *after* inlining rather than before: the emitter names every instruction's
 * result, so the tree fusion matches on is one the substitution above is what assembles. The outer
 * loop is then for the trees that assembling one uncovers, and it terminates because fusion removes
 * wide calls and never adds one.
 */
void optimizeFunction(Gen& g, FunStmt& function) {
    Names names;
    Ranges ranges(g);

    for(;;) {
        for(;;) {
            names.reset();
            countList(g, function.body, names);

            // After the counts, since what a binding may be assumed to hold is decided by whether
            // anything assigns it.
            ranges.reset();
            collectRanges(g, function.body, names, ranges);

            if(!optimizeList(g, function.body, names, ranges)) break;
        }

        if(!fuseList(g, function.body)) return;
    }
}

/*
 * The helpers nothing is left calling.
 *
 * A helper is interned when the *call node* is built, which is while the emitter is walking the
 * instruction that wanted one - and fusion then takes the call away again. `(a and b) xor c` asks
 * for three of them and ends up calling none, so without this the file carries three function
 * definitions that nothing mentions. It was six hundred bytes of dead text in WideFusion.yana,
 * which is a good deal more than the fusion saved there.
 *
 * Only the compiler's own helpers, by name: an unreferenced *user* function is a different question
 * with a different answer, and which of those get emitted at all is `excludeFunctions`'.
 *
 * To a fixed point, because one helper's body may be the only thing calling another - a bit-range
 * accessor wider than the operators reaches for a `wrap`, and removing the accessor is what makes
 * the wrap dead.
 */
void removeDeadHelpers(Gen& g) {
    if(g.wideHelperOrder.size() == 0 && g.bitHelperOrder.size() == 0) return;

    HashSet<U32> generated;
    for(auto& helper: g.wideHelperOrder) generated.add(helper.name.text);
    for(auto& helper: g.bitHelperOrder) generated.add(helper.name.text);

    Names names;

    for(;;) {
        names.reset();
        countList(g, g.file.statements, names);

        auto changed = false;
        Size index = 0;

        while(index < g.file.statements.size()) {
            auto stmt = g.base[g.file.statements.get(g.base, index)];

            if(stmt->kind == Stmt::Fun) {
                auto name = ((FunStmt*)stmt)->name;
                if(generated.contains(name.text) && !names.useCount(name)) {
                    g.file.statements.remove(g.base, index);
                    changed = true;
                    continue;
                }
            }

            index++;
        }

        if(!changed) break;
    }

    // And the heading of a family that emptied, which would otherwise introduce nothing.
    auto stillHas = [&](Array<WideHelper>* wide, Array<BitHelper>* bits) {
        for(auto pointer: g.file.statements.contents(g.base)) {
            auto stmt = g.base[pointer];
            if(stmt->kind != Stmt::Fun) continue;

            auto name = ((FunStmt*)stmt)->name;
            if(wide) for(auto& helper: *wide) if(helper.name.text == name.text) return true;
            if(bits) for(auto& helper: *bits) if(helper.name.text == name.text) return true;
        }

        return false;
    };

    auto dropComment = [&](JsPtr<Stmt> comment) {
        if(!comment) return;

        for(Size i = 0; i < g.file.statements.size(); i++) {
            if(g.file.statements.get(g.base, i) != comment) continue;

            g.file.statements.remove(g.base, i);
            return;
        }
    };

    if(!stillHas(&g.wideHelperOrder, nullptr)) dropComment(g.wideHelperComment);
    if(!stillHas(nullptr, &g.bitHelperOrder)) dropComment(g.bitHelperComment);
}

} // namespace

void optimizeFile(Gen& g) {
    for(auto pointer: g.file.statements.contents(g.base)) {
        auto stmt = g.base[pointer];
        if(stmt->kind == Stmt::Fun) optimizeFunction(g, *(FunStmt*)stmt);
    }

    removeDeadHelpers(g);
}

} // namespace js
