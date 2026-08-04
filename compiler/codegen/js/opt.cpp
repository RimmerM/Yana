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
        case Stmt::Decl:
            return ((DeclStmt*)stmt)->value ? &((DeclStmt*)stmt)->value : nullptr;
        default:
            return nullptr;
    }
}

// The body of every closure inside one expression. A closure is a statement list reached from an
// expression, which is the one place in this tree where those two nest the other way round.
template<class F>
void eachClosureBody(Gen& g, JsPtr<Expr> pointer, F&& f) {
    auto expr = g.base[pointer];

    if(expr->kind == Expr::Function) {
        f(((FunValueExpr*)expr)->body);
        return;
    }

    eachOperand(g, expr, [&](JsPtr<Expr>& operand, bool) { eachClosureBody(g, operand, f); });
}

/*
 * The statement lists one statement contains, in the order control reaches them.
 *
 * A closure in the statement's own expression is one of them, and it is one scope with what
 * surrounds it: the factory and the closure it returns share a name scope, so a binding read once
 * inside the closure is read once by the counts here. What does *not* cross the boundary is
 * substitution - eachOperand stops at a function expression - because moving a computation into a
 * closure would move it from once to once per call.
 */
template<class F>
void eachBody(Gen& g, Stmt* stmt, F&& f) {
    if(auto header = headerOf(g, stmt)) eachClosureBody(g, *header, f);

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

void addEffects(Gen& g, JsPtr<Expr> pointer, Effects& out) {
    auto expr = g.base[pointer];

    if(expr->kind == Expr::Field || expr->kind == Expr::Index) out.reads = true;
    if(isEffectful(expr)) out.writes = true;

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
 */
bool isAtom(Gen& g, JsPtr<Expr> pointer, Names& names) {
    auto expr = g.base[pointer];

    switch(expr->kind) {
        case Expr::Var:
            return !names.assigned.contains(((VarExpr*)expr)->name.text);
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
    // has left it without finding the use.
    if(s.done || s.blocked) return;
    if(expr->kind == Expr::Field || expr->kind == Expr::Index) s.prefix.reads = true;
    if(isEffectful(expr)) s.prefix.writes = true;
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

/*
 * `var v = {a: 0, b: 0}; v.a = x;` -> `var v = {a: x, b: 0};`
 *
 * Folded in property order and no further: a write to a property the walk has already passed would
 * be moved in front of one it was behind, which is a reordering wherever either value does anything.
 * The properties in between are the type's zero values, so they are checked to be inert rather than
 * assumed to be - the same check, and it is what makes this rule about the tree rather than about
 * where the tree came from.
 */
bool foldInitializers(Gen& g, StmtList& list, Size index) {
    auto declaration = g.base[list.get(g.base, index)];
    if(declaration->kind != Stmt::Decl) return false;

    auto& decl = *(DeclStmt*)declaration;
    if(!decl.value || g.base[decl.value]->kind != Expr::Object) return false;

    auto& object = *(ObjectExpr*)g.base[decl.value];
    auto changed = false;
    Size filled = 0;

    while(index + 1 < list.size()) {
        auto next = g.base[list.get(g.base, index + 1)];
        if(next->kind != Stmt::Expression) break;

        auto written = g.base[((ExprStmt*)next)->value];
        if(written->kind != Expr::Assign) break;

        auto target = g.base[((AssignExpr*)written)->target];
        if(target->kind != Expr::Field) break;

        auto owner = g.base[((FieldExpr*)target)->object];
        if(owner->kind != Expr::Var || ((VarExpr*)owner)->name.text != decl.name.text) break;

        auto properties = itemsOf(g, object.properties);
        auto key = ((FieldExpr*)target)->field.text;
        auto slot = object.properties.size();

        for(Size i = filled; i < object.properties.size(); i++) {
            if(properties[i].key.text != key) continue;
            slot = i;
            break;
        }

        if(slot == object.properties.size()) break;

        // The properties this one jumps in front of stay where they are, so they have to be values
        // that cannot tell.
        Effects skipped;
        for(Size i = filled; i < slot; i++) addEffects(g, properties[i].value, skipped);
        if(!skipped.inert()) break;

        // A value read out of the object it is going into would be reading a property that no
        // longer holds its zero by the time it is evaluated.
        auto value = ((AssignExpr*)written)->value;
        if(mentions(g, value, decl.name)) break;

        properties[slot].value = value;
        list.remove(g.base, index + 1);
        filled = slot + 1;
        changed = true;
    }

    return changed;
}

/*
 * `var v = []; v[0] = a; v[1] = b;` -> `var v = [a, b];`
 *
 * `foldInitializers` for the indexed case, and it earns itself for a reason the object one does not
 * have to argue: filling an empty array by index walks the element-kind transitions a literal skips,
 * so the two forms are not the same program to a host that specializes on them. An array literal is
 * also what the source wrote - `[7, 8, 9]` compiles through an `alloc` and three `init`s because
 * that is what the IR has, not because anything wanted the host to see it that way.
 *
 * Consecutive from zero and no further. A gap would leave a hole, which is a different kind of array
 * on every engine that has element kinds; an index that repeats one already folded is a *second*
 * write to the same slot and has to stay a write, since folding it would move it in front of a read
 * that came between. Anything that is not a literal index stops the walk - `v[v.length] = x` is a
 * push and is not this.
 *
 * The values do not move relative to each other and cross only the empty literal, so there is no
 * effect question to ask beyond the one the name itself raises.
 */
bool foldArrayElements(Gen& g, StmtList& list, Size index) {
    auto declaration = g.base[list.get(g.base, index)];
    if(declaration->kind != Stmt::Decl) return false;

    auto& decl = *(DeclStmt*)declaration;
    if(!decl.value || g.base[decl.value]->kind != Expr::Array) return false;

    auto& array = *(ArrayExpr*)g.base[decl.value];
    if(array.values.isNotEmpty()) return false;

    auto changed = false;

    while(index + 1 < list.size()) {
        auto next = g.base[list.get(g.base, index + 1)];
        if(next->kind != Stmt::Expression) break;

        auto written = g.base[((ExprStmt*)next)->value];
        if(written->kind != Expr::Assign) break;

        auto target = g.base[((AssignExpr*)written)->target];
        if(target->kind != Expr::Index) break;

        auto owner = g.base[((IndexExpr*)target)->array];
        if(owner->kind != Expr::Var || ((VarExpr*)owner)->name.text != decl.name.text) break;

        auto position = g.base[((IndexExpr*)target)->index];
        if(position->kind != Expr::Number || !((NumberExpr*)position)->integral) break;
        if(((NumberExpr*)position)->value != F64(array.values.size())) break;

        // A value read out of the array it is going into would be reading a slot that does not
        // exist yet once the write becomes an element.
        auto value = ((AssignExpr*)written)->value;
        if(mentions(g, value, decl.name)) break;

        array.values.push(g.file.arena, value);
        list.remove(g.base, index + 1);
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

bool foldExprs(Gen& g, JsPtr<Expr>& slot) {
    auto changed = false;
    eachOperand(g, g.base[slot], [&](JsPtr<Expr>& operand, bool) {
        changed = foldExprs(g, operand) || changed;
    });

    // Both are shape rules over one node, so either may expose the other's shape - a folded ternary
    // can become the index of a subscript, and a folded subscript can become a compared operand.
    auto folded = foldConstantIndex(g, slot);
    return foldTernaryCompare(g, slot) || folded || changed;
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

bool optimizeList(Gen& g, StmtList& list, Names& names) {
    auto changed = false;
    Size index = 0;

    while(index < list.size()) {
        auto stmt = g.base[list.get(g.base, index)];
        eachBody(g, stmt, [&](StmtList& body) { changed = optimizeList(g, body, names) || changed; });

        if(auto header = headerOf(g, stmt)) changed = foldExprs(g, *header) || changed;

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

    for(;;) {
        for(;;) {
            names.reset();
            countList(g, function.body, names);

            if(!optimizeList(g, function.body, names)) break;
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
