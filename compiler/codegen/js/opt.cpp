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
 * So the naming happens unconditionally and this pass takes it back. Two rewrites, both driven by
 * use counts over one function:
 *
 *  - a `var` read exactly once and written never becomes its use, and one read by nothing at all
 *    goes away;
 *  - `var v = {a: 0}; v.a = x;` becomes `var v = {a: x};`, and `var v = 0; v = x;` becomes
 *    `var v = x;`.
 *
 * The second is not cosmetic. Analysis-JS.md §2.3 makes construction order the JS equivalent of
 * field offsets, and an object literal that already holds its values is one hidden-class transition
 * where the zero-then-fill form is one per property.
 *
 * Everything here is decided from the tree rather than from the IR, deliberately: the question is
 * what the emitted JS evaluates and in which order, and that is a property of the tree. Two things
 * are checked before anything moves - that a name is read once and assigned never, and that the
 * expressions the move crosses cannot see each other - and where either is unknown, nothing happens.
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

void addEffects(Gen& g, JsPtr<Expr> pointer, Effects& out) {
    auto expr = g.base[pointer];

    if(expr->kind == Expr::Field || expr->kind == Expr::Index) out.reads = true;
    if(isEffectful(expr)) out.writes = true;

    eachOperand(g, expr, [&](JsPtr<Expr>& operand, bool) { addEffects(g, operand, out); });
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

    eachOperand(g, expr, [&](JsPtr<Expr>& operand, bool branch) {
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
 * The two rewrites.
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
 * `var v = 0; v = x;` -> `var v = x;`
 *
 * The same rewrite as above for a value that is not built property by property. The emitter writes
 * the zero because storage exists before anything fills it - a resolve `Alloc` is one instruction
 * and the `Init` that follows it is another - and where the fill is the next statement, nothing can
 * observe that it was ever anything else.
 */
bool foldInitialValue(Gen& g, StmtList& list, Size index) {
    auto declaration = g.base[list.get(g.base, index)];
    if(declaration->kind != Stmt::Decl) return false;

    auto& decl = *(DeclStmt*)declaration;
    if(decl.constant) return false;
    if(decl.value && !effectsOf(g, decl.value).inert()) return false;
    if(index + 1 >= list.size()) return false;

    auto next = g.base[list.get(g.base, index + 1)];
    if(next->kind != Stmt::Expression) return false;

    auto written = g.base[((ExprStmt*)next)->value];
    if(written->kind != Expr::Assign) return false;

    auto target = g.base[((AssignExpr*)written)->target];
    if(target->kind != Expr::Var || ((VarExpr*)target)->name.text != decl.name.text) return false;

    // `v = v + 1` reads what is being replaced, so the zero is observable after all.
    auto value = ((AssignExpr*)written)->value;
    if(mentions(g, value, decl.name)) return false;

    decl.value = value;
    list.remove(g.base, index + 1);
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

bool optimizeList(Gen& g, StmtList& list, Names& names) {
    auto changed = false;
    Size index = 0;

    while(index < list.size()) {
        auto stmt = g.base[list.get(g.base, index)];
        eachBody(g, stmt, [&](StmtList& body) { changed = optimizeList(g, body, names) || changed; });

        // Both rewrites shorten the list, so the same position is looked at again rather than the
        // next one - which is what collapses a chain of one-use bindings in a single walk.
        if(foldInitializers(g, list, index) || foldInitialValue(g, list, index)) {
            changed = true;
            continue;
        }

        if(removeDeadBinding(g, list, index, names)) {
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
 */
void optimizeFunction(Gen& g, FunStmt& function) {
    for(;;) {
        Names names;
        countList(g, function.body, names);

        if(!optimizeList(g, function.body, names)) return;
    }
}

} // namespace

void optimizeFile(Gen& g) {
    for(auto pointer: g.file.statements.contents(g.base)) {
        auto stmt = g.base[pointer];
        if(stmt->kind == Stmt::Fun) optimizeFunction(g, *(FunStmt*)stmt);
    }
}

} // namespace js
