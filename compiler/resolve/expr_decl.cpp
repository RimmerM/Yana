/*
 * Declarations, places and assignment: where a name comes to mean storage.
 *
 * A `let` is not one thing - what it produces depends on the binding convention written on it, and
 * the three that matter here are a value, a borrow and a mutable local, each of which owes
 * something different about ownership. `resolvePlace` is the other half: turning a written
 * expression into the storage it names, which is what an assignment's left side and a borrow's
 * operand both are.
 */

#include "expr.h"
#include "complete.h"
#include "generic.h"
#include "name.h"
#include "index.h"

ModulePtr<Value> ExprResolver::resolveDecl(ast::ParseList<ast::VarDecl> declarations, TypePtr target, bool used) {
    ModulePtr<Value> result = nullptr;

    for(auto decl: declarations.contents(parse)) {
        if(!decl.content) {
            context.diagnostics.error("let requires an initializer"_v, decl.pat.source);
            continue;
        }

        auto mutable_ = decl.bind == ast::BindType::Ref;

        // Not a BindingScope, and this is the one place that is a statement rather than an
        // oversight: what a plain `let` binds is meant to survive the statement that bound it - it
        // is the rest of the block that reads it - so a guard whose destructor takes the name away
        // would undo the declaration. The checkpoint is a base index here, handed to
        // applyBindingAttributes; only the `let ... in ...` form below restores to it, because only
        // that form has an end.
        auto checkpoint = bindings.size();

        // Where this frame's locals had got to before the initializer ran, which is what tells a
        // temporary it built from storage the program already had a name for - see `adoptableLocal`.
        auto fresh = U32(function.localCount());

        // A `let` is a statement boundary, so a literal the initializer left open is settled to
        // its default here: `let x = 1` binds an Int, and nothing later in the block can go back
        // and make it a Long.
        auto value = settle(resolve(*parse[decl.content]), decl.pat.source);
        if(!value) continue;

        /*
         * `let ->z = x` takes ownership out of whatever `x` named, so the name that follows binds
         * the moved value rather than the source. What `->` decides is where the value came from,
         * not what may be done with it after - which is why `let &->z = x` is the same move with the
         * other axis set, and why both reach this one line.
         *
         * `let &z = e` joins them where `e` is a whole-value projection out of a temporary nothing
         * else names - Analysis-Language.md §2a. There the move is not a second thing the reader
         * asked for: the source is a value this statement built and is about to stop referring to,
         * so taking it out is what `&` on any other temporary already means.
         */
        if(decl.bind == ast::BindType::Sink || decl.sink ||
           (mutable_ && movableTemporary(value, fresh))) {
            value = rootSink(sinkValue(value, decl.pat.source), decl.pat.source);
            if(!value) continue;
        }

        if(isBorrow(global, valueType(value))) {
            bindBorrow(decl, value, mutable_);
        } else if(mutable_) {
            bindMutable(decl, value, fresh);
        } else {
            resolveBinding(decl, value);
        }

        if(decl.attributes.isNotEmpty()) applyBindingAttributes(decl, value, checkpoint);

        if(!current) break;

        if(decl.in) {
            result = resolve(*parse[decl.in], target, used);
            bindings.resize(checkpoint);
        } else {
            result = value;
        }
    }

    return result;
}

/*
 * `let &x = value`.
 *
 * The initializer's storage is what the name refers to from here on, so the declaration allocates
 * a slot, writes the value into it, and binds the name to the slot rather than to the value. That
 * is the whole difference between a mutable and an immutable binding at this milestone: the same
 * places, the same InstInit, and one more entry in Function::locals.
 *
 * Only a plain name can be mutable. Destructuring one into several mutable slots is a question
 * about ownership - which of the parts the binding owns - and belongs with the rest of Milestone
 * 5, not with the machinery for writing to a slot.
 */
/*
 * Attributes on a binding.
 *
 * `@heap` is the only one so far, and it is Design.md's "for a large allocation that's freed well
 * before the region closes": an override of the storage class escape analysis would otherwise
 * choose. It is deliberately one-directional - it can only move a value off the frame, never onto
 * it - because the analysis picks the frame exactly when it has proved the frame is enough, and an
 * attribute that could overrule *that* would be an attribute that could introduce a dangling
 * reference.
 *
 * The slot it applies to is whichever local the binding's value ends up occupying: for a mutable
 * binding that is the slot the declaration allocated, and for an aggregate it is the storage the
 * construction already created.
 */
void ExprResolver::applyBindingAttributes(const ast::VarDecl& declaration, ModulePtr<Value> value,
                                          Size bindingBase) {
    auto slot = maxLimit<U32>;

    if(bindings.size() > bindingBase && bindings[bindingBase].local != maxLimit<U32>) {
        slot = bindings[bindingBase].local;
    } else if(auto place = findPlace(value)) {
        if(place.unwrap().root == PlaceRoot::Local) slot = place.unwrap().local;
    }

    auto attributes = declaration.attributes;

    for(auto attribute: attributes.contents(parse)) {
        if(attribute.name != context.addUnqualifiedName("heap", 4)) {
            context.diagnostics.error("unknown attribute %@ on a binding"_v, attribute.source,
                                      context.findName(attribute.name));
            continue;
        }

        if(attribute.args.isNotEmpty()) {
            context.diagnostics.error("`@heap` takes no arguments"_v, attribute.source);
            continue;
        }

        if(slot == maxLimit<U32>) {
            // A value in a register occupies no storage for an attribute to place. Saying so is
            // better than allocating one just so that the attribute has something to be about.
            context.diagnostics.error("`@heap` has nothing to place - this binding names a value that occupies no storage of its own"_v,
                                      attribute.source);
            continue;
        }

        auto local_ = function.localAt(local, slot);
        function.locals.set(local, slot, Local {
            local_.type, local_.name, local_.value, local_.convention, StorageClass::Heap,
            local_.borrowed, local_.closureEnv,
        });
    }
}

/*
 * `let entry = f(...)` and `let &entry = f(...)`, where what `f` returned is a borrow.
 *
 * The name refers to the storage the callee's return-root group named, so there is nothing to
 * allocate and nothing to copy: the binding is a place rooted in the borrow itself. Allocating a
 * slot and writing the borrow into it - which is what the ordinary path would do - would give the
 * name a *copy* of the reference, and `entry.field = value` would then write through to the right
 * storage by accident rather than by construction.
 *
 * The sigil still has to agree with what was handed over. `let &` on an immutable borrow would be a
 * name that claims a capability nobody granted it, and that is the one thing to report here rather
 * than at the first write through it.
 */
void ExprResolver::bindBorrow(const ast::VarDecl& declaration, ModulePtr<Value> value, bool mutable_) {
    if(declaration.pat.kind != ast::Pat::Var) {
        context.diagnostics.error("a binding of a borrow must be a single name - a borrow has no members to destructure"_v,
                                  declaration.pat.source);
        return;
    }

    auto borrow = (BorrowType*)global[valueType(value)];

    if(mutable_ && !borrow->mut) {
        context.diagnostics.error("cannot bind an immutable borrow with `let &` - the value it refers to may not be written through it"_v,
                                  declaration.pat.source);
        return;
    }

    Binding binding { declaration.pat.var, value, maxLimit<U32>, value };
    binding.definition = declaration.pat.source;
    bindings.push(binding);
    recordBindingDefinition(*this, binding);
}

Maybe<U32> ExprResolver::adoptableLocal(ModulePtr<Value> value, U32 fresh) {
    auto found = findPlace(value);
    if(!found) return Nothing();

    // A whole local and not a part of one. A field of something is storage whose owner outlives this
    // binding, and there is nothing to take over.
    auto place = found.unwrap();
    if(place.root != PlaceRoot::Local || place.projections.isNotEmpty()) return Nothing();
    if(place.local < fresh || place.local >= function.localCount()) return Nothing();

    /*
     * Storage this frame allocated, and only that. A `&` parameter's slot is the caller's, a closure
     * environment is the function value's, and a materialized packed-field temporary stands for
     * storage somewhere else - none of the three is a temporary to be taken over, and each of them
     * is already recorded on the slot rather than having to be worked out.
     */
    auto slot = function.localAt(local, place.local);
    if(!slot.value || local[slot.value]->kind != Value::Alloc) return Nothing();
    if(slot.borrowed || slot.closureEnv || slot.materialized) return Nothing();
    if(slot.type != valueType(value)) return Nothing();

    // And nothing already answers to it. The index test above covers a name the program had before
    // this declaration; this covers one the initializer itself introduced, which a `let ... in`
    // inside it can do.
    for(auto& binding: bindings) {
        if(binding.local == place.local) return Nothing();
    }

    return Just(place.local);
}

bool ExprResolver::movableTemporary(ModulePtr<Value> value, U32 fresh) {
    if(!value) return false;

    /*
     * Only where there is something to take. Asked of the context on the same terms checkTransfer
     * asks it, so that the answer here and the answer that would have rejected the program are the
     * same question: without a teardown nothing is handed over by reading the bytes, and turning
     * `let &n = maybeInt?` into a move would pay for a relocation of an Int.
     */
    auto ownership = ownershipIn(module, functionGen(global, function), valueType(value));
    if(!ownership.needsTeardown()) return false;

    auto found = findPlace(value);
    if(!found) return false;

    auto place = found.unwrap();
    if(place.root != PlaceRoot::Local) return false;

    // The whole value, and the one projection that names it. `wholeMove` in analyze_borrow is the
    // same test from the other end - what it would have accepted with a `->` written on it.
    if(place.projections.size() != 1) return false;
    if(place.projections.get(local, 0).kind != ProjectionKind::Downcast) return false;

    /*
     * A temporary this initializer built. The mark is adoptableLocal's and so is the reasoning: a
     * local below it existed before the initializer ran, so it is something the program already had
     * a name for.
     *
     * Unlike adoptableLocal this does not also demand an `Alloc`, because it is not taking the slot
     * over - the move reads the payload out and leaves the source to its own teardown. The slot a
     * `?` projects out of is a *call* result: `x?` is `toOutcome(x)` landing in a frame slot, and
     * demanding an allocation there would have refused the one case §2a exists for.
     */
    if(place.local < fresh || place.local >= function.localCount()) return false;

    auto slot = function.localAt(local, place.local);
    if(slot.borrowed || slot.closureEnv || slot.materialized) return false;

    for(auto& binding: bindings) {
        if(binding.local == place.local) return false;
    }

    return true;
}

void ExprResolver::bindMutable(const ast::VarDecl& declaration, ModulePtr<Value> value, U32 fresh) {
    if(declaration.pat.kind != ast::Pat::Var) {
        context.diagnostics.error("a mutable binding must be a single name"_v, declaration.pat.source);
        return;
    }

    auto alternatives = declaration.alts;
    if(alternatives.isNotEmpty()) {
        context.diagnostics.error("a mutable binding always matches, so it takes no alternatives"_v,
                                  declaration.pat.source);
    }

    auto name = declaration.pat.var;

    /*
     * The temporary the initializer built, taken over rather than copied out of - see
     * `adoptableLocal`, which is where the conditions and the reasoning live.
     *
     * Read-modify-write on the slot rather than a fresh `Local`, because a local carries more than
     * the four fields this changes and the ones it does not name are set after `addLocal` rather
     * than by it. What differs about a mutable binding's slot is its name and its convention.
     */
    if(auto adopted = adoptableLocal(value, fresh)) {
        auto index = adopted.unwrap();
        auto slot = function.localAt(local, index);

        slot.name = name;
        slot.convention = ast::BindType::Ref;
        function.locals.set(local, index, slot);

        Binding adoptedBinding { name, slot.value, index };
        adoptedBinding.definition = declaration.pat.source;
        bindings.push(adoptedBinding);
        recordBindingDefinition(*this, adoptedBinding);
        return;
    }

    auto type = valueType(value);
    auto storage = allocate(type, declaration.pat.source, name, ast::BindType::Ref);
    auto place = placeFor(storage, declaration.pat.source);

    initialize(place, value, declaration.pat.source);

    Binding binding { name, storage, place.local };
    binding.definition = declaration.pat.source;
    bindings.push(binding);
    recordBindingDefinition(*this, binding);
}

/*
 * What an assignment writes to.
 *
 * Four expressions name storage: a mutable binding, a mutable global, the memory a raw pointer
 * points at, and - only as the target of a field selection - an immutable binding holding a raw
 * pointer. Everything reachable from those by projection does too, which is what makes `p.x = 1`
 * and `(*node).next = null` work without a rule of their own - the projection path is built by the
 * same field selection an ordinary read uses.
 *
 * `through` is what marks that fourth case: writing *through* a pointer is not writing to the
 * binding that holds it, and the memory a pointer names is always mutable. `let n = ...` followed
 * by `n.value = 5` therefore writes, while `n = q` on the same binding stays the error it is -
 * that one rebinds the pointer rather than writing through it.
 */
Maybe<Place> ExprResolver::resolvePlace(const ast::Expr& astExpr, bool through) {
    auto& expr = unwrapNested(astExpr);

    switch(expr.kind) {
        case ast::Expr::Var: {
            if(auto binding = findBinding(expr.var, expr.source)) {
                if(binding->lazy) {
                    context.diagnostics.error("%@ is a `@lazy` parameter, which names an expression rather than storage - there is nothing to assign to"_v,
                                              expr.source, context.findName(expr.var));
                    return Nothing();
                }

                if(!binding->isPlace()) {
                    // An immutable binding still roots a place when what it holds is a reference:
                    // projecting into it names the storage the reference points at, which is not
                    // this binding's to be mutable about. A raw pointer and a borrow differ here
                    // only in whether anything checked the result.
                    if(through && isPointer(global, valueType(binding->value))) {
                        return Just(Place::atPointer(binding->value));
                    }

                    if(isBorrow(global, valueType(binding->value))) {
                        return Just(Place::inBorrow(binding->value));
                    }

                    context.diagnostics.error("%@ is not mutable - declare it with `let &` to assign to it"_v,
                                              expr.source, context.findName(expr.var));
                    return Nothing();
                }

                /*
                 * A capture the closure owns is not assignable.
                 *
                 * Design-Memory §8 requires a written capture to be by reference, and a capture
                 * that came out by value is exactly one whose enclosing binding was not mutable -
                 * so writing it would write the environment's own copy and the enclosing frame
                 * would never see it. That is the same diagnostic an immutable binding gets,
                 * because it is the same mistake.
                 */
                if(binding->captured && !binding->captureBorrow) {
                    context.diagnostics.error("%@ is captured by value and cannot be assigned to - declare it with `let &` in the enclosing function to capture it by reference"_v,
                                              expr.source, context.findName(expr.var));
                    return Nothing();
                }

                return Just(placeOf(*binding, expr.source));
            }

            if(auto global_ = findGlobal(module, expr.var, expr.source)) {
                if(!initializedGlobal(global_, expr.source)) return Nothing();

                if(!local[global_]->mut) {
                    context.diagnostics.error("%@ is not mutable - declare it with `let &` to assign to it"_v,
                                              expr.source, context.findName(expr.var));
                    return Nothing();
                }

                local[global_]->used = true;
                return Just(Place::inGlobal(global_));
            }

            context.diagnostics.error("unknown value %@"_v, expr.source, context.findName(expr.var));
            return Nothing();
        }
        case ast::Expr::Field: {
            auto& field = *parse[expr.field];
            auto target = resolvePlace(field.target, true);
            if(!target) return Nothing();

            return projectField(target.unwrap(), field.field, expr.source);
        }
        case ast::Expr::Sub: {
            // `xs[i] = value`. The mutable accessor hands back a borrow of the element, and the
            // assignment writes through it - which is also what keeps the array exclusively
            // borrowed for as long as the write is in progress.
            auto borrowed = resolveSubscript(expr, *parse[expr.sub], true);
            if(!borrowed) return Nothing();

            return Just(Place::inBorrow(borrowed));
        }
        case ast::Expr::Prefix: {
            // `*p = value` - the one place expression whose root the compiler knows nothing
            // about, which is the point of it.
            auto& prefix = *parse[expr.prefix];
            if(prefix.op.kind != ast::Expr::Var || prefix.op.var != Context::nameHash("*"_v)) break;

            auto pointer = resolve(prefix.on);
            if(!pointer) return Nothing();

            if(!isPointer(global, valueType(pointer))) {
                context.diagnostics.error("cannot dereference %@ - it is not a raw pointer"_v, expr.source,
                                          describeType(context, global, valueType(pointer)));
                return Nothing();
            }

            return Just(Place::atPointer(pointer));
        }
        default:
            break;
    }

    context.diagnostics.error("this expression does not name storage that can be assigned to"_v, expr.source);
    return Nothing();
}

ModulePtr<Value> ExprResolver::resolveAssign(const ast::Expr& expr, const ast::AssignExpr& assignment) {
    /*
     * `m[k] = v` on a container that inserts - Implementation-Map.md §7.
     *
     * Intercepted here rather than inside `resolvePlace`, because what `IndexInsert` needs is the
     * *value* as well as the index and a place has neither. One resolution of the container answers
     * both forms: `resolveSubscript` emits the whole assignment where the instance exists and hands
     * back the ordinary `getMut` borrow where it does not, so nothing below this is duplicated and
     * nothing is resolved twice.
     *
     * `m[k] += 1` is deliberately not this: a compound assignment reads the element before it writes
     * one, so there is a value to borrow and trapping on an absent key is the right answer.
     */
    if(assignment.target.kind == ast::Expr::Sub) {
        auto handled = false;
        auto borrowed = resolveSubscript(assignment.target, *parse[assignment.target.sub], true,
                                         &assignment.value, &handled);

        if(handled) return borrowed;
        if(!borrowed) return nullptr;

        auto element = Place::inBorrow(borrowed);
        auto held = placeType(element);
        auto written = resolve(assignment.value, held);
        if(!written) return nullptr;

        if(!isMemoryType(global, held)) written = convert(written, held, expr.source);

        assign(element, written, expr.source);
        return nullptr;
    }

    auto place = resolvePlace(assignment.target);
    if(!place) return nullptr;

    auto type = placeType(place.unwrap());
    auto value = resolve(assignment.value, type);
    if(!value) return nullptr;

    if(!isMemoryType(global, type)) value = convert(value, type, expr.source);

    // An assignment overwrites whatever the place held, which is what obliges the drop pass to
    // release the old value here rather than at the end of the binding's life.
    assign(place.unwrap(), value, expr.source);
    return nullptr;
}
