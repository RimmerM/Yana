#include "resolve_internal.h"

namespace mir {

/*
 * Pass one, part one: the record *names*, before any field type is resolved.
 *
 * Two sub-passes rather than one, because a `data` may hold a record declared below it and the field
 * type of the first would then resolve against a name table that does not have the second in it yet.
 * A shell is a name and an identity, which is all a nominal type needs to be referred to; the fields
 * arrive in `declareFields`.
 */
void Resolver::declareTypes(ast::Module& file) {
    for(auto declaration: file.decls.contents(parse)) {
        if(declaration.kind != ast::Decl::Data) continue;

        auto& written = declaration.data;
        auto push = [&](DeclPtr<RecordType> record) { writtenRecords.push(record); };

        if(written.constraints.size() || written.type.kind.size()) {
            unsupported(declaration.source, "a generic data declaration"_v);
            push(nullptr);
            continue;
        }

        /*
         * One constructor, whose name is the type's - `data Pair {x: I32, y: I32}`.
         *
         * Several constructors are an enumeration: the value carries a discriminant, a projection
         * needs a downcast that a pattern proves, and the ownership schema differs per arm. That is
         * a representation with three parts this milestone does not have, and refusing it by name is
         * more useful than a record whose second constructor was silently dropped.
         */
        if(written.cons.size() != 1) {
            unsupported(declaration.source, "a data declaration with several constructors"_v);
            push(nullptr);
            continue;
        }

        auto constructor = written.cons.get(parse, 0);
        if(constructor.name != written.type.name) {
            unsupported(constructor.source, "a constructor whose name differs from its type"_v);
            push(nullptr);
            continue;
        }

        if(!constructor.content || parse[constructor.content]->kind != ast::Type::Tup) {
            unsupported(constructor.source, "a constructor without named fields"_v);
            push(nullptr);
            continue;
        }

        auto record = program.types.recordType(written.type.name, here(), declaration.source);
        auto pointer = DeclPtr<RecordType>(record - *program.decl);

        module.records.push(program.decl, pointer);
        push(pointer);

        if(!program.addRecordName(here(), record->name, pointer)) {
            context.diagnostics.error("%@ is already declared in this module"_v,
                                      declaration.source, context.findName(record->name));
        }
    }
}

/*
 * The fields of one record, once every record name exists.
 *
 * `next` indexes the *written* declarations including the rejected ones, for the reason
 * `writtenRecords` exists: the two walks have to advance in step or a later declaration is given an
 * earlier one's fields.
 */
void Resolver::declareFields(ast::Decl& declaration, Size& next) {
    if(next >= writtenRecords.size()) return;

    auto pointer = writtenRecords[next++];
    if(!pointer) return;

    auto record = decl()[pointer];

    // The shell exists, so `declareTypes` already established that there is exactly one constructor
    // and that its content is a written tuple - this is the same declaration read a second time
    // rather than a second set of conditions to keep in step.
    auto constructor = declaration.data.cons.get(parse, 0);
    auto& written = *parse[constructor.content];

    for(auto field: written.tup.fields.contents(parse)) {
        if(!field.name) {
            unsupported(record->source, "a positional field"_v);
            continue;
        }

        if(field.def) {
            unsupported(record->source, "a field default"_v);
            continue;
        }

        auto type = resolveType(field.type);
        if(!type) continue;

        // A second field of one name would make `p.x` name whichever the lookup reached first, which
        // is a silent wrong answer rather than a missing feature.
        auto duplicate = false;
        for(auto existing: record->fields.contents(decl())) {
            if(existing.name != field.name) continue;

            context.diagnostics.error("%@ already has a field called %@"_v, record->source,
                                      context.findName(record->name), context.findName(field.name));
            duplicate = true;
            break;
        }

        if(duplicate) continue;

        RecordField declared;
        declared.name = field.name;
        declared.type = type;
        declared.source = record->source;
        declared.index = U16(record->fields.size());

        record->fields.push(program.decl, declared);
    }

    if(record->fields.size()) return;

    // A record with no fields has no layout question and no ownership question, and every value of
    // it is the same value - which is `{}`, spelled by a declaration. Refused rather than admitted
    // as a second unit type.
    unsupported(record->source, "a record with no fields"_v);
}

void Resolver::declareGlobals(ast::Decl& declaration) {
    if(declaration.stmt.kind != ast::Expr::Decl) {
        unsupported(declaration.source, "a module-level statement"_v);
        return;
    }

    for(auto binding: declaration.stmt.decl.contents(parse)) {
        if(binding.pat.kind != ast::Pat::Var) {
            context.diagnostics.error("a global must be declared as a single name"_v, binding.pat.source);
            writtenGlobals.push(nullptr);
            continue;
        }

        if(!binding.content) {
            context.diagnostics.error("a global requires an initializer"_v, binding.pat.source);
            writtenGlobals.push(nullptr);
            continue;
        }

        auto global = new (program.decl) Global(binding.pat.var, owned(nullptr));
        global->source = binding.pat.source;
        global->writable = binding.bind == ast::BindType::Ref;

        auto pointer = global - *program.decl;
        module.globals.push(program.decl, pointer);
        writtenGlobals.push(pointer);

        if(!program.addGlobalName(here(), global->name, pointer)) {
            context.diagnostics.error("%@ is already declared in this module"_v,
                                      binding.pat.source, context.findName(global->name));
        }
    }
}

/*
 * One written parameter's contract - Analysis-Borrows-Revised.md §2.1.
 *
 * The three spellings and what each one grants:
 *
 *  - `x: T` is shared, call-bounded access. The callee may read the value for the length of the
 *    call and may not keep it.
 *  - `&x: T` is unique, call-bounded access: exclusive, and writable through the parameter.
 *  - `->x: T` is ownership, transferred to the callee, which is then what has to end it.
 *
 * `&` is refused here and not implemented, because it is a *loan* the caller has to grant over a
 * place it keeps, and loans are milestone 5. A `'` on the type publishes an origin slot and is
 * refused for the same reason.
 */
bool Resolver::argContract(ast::Arg& written, Contract& into) {
    switch(written.bind) {
        case ast::BindType::Borrow:
            into = borrowedContract(Access::Shared);
            return true;
        case ast::BindType::Sink:
            into = ownedContract();
            return true;
        case ast::BindType::Ref:
            unsupported(written.source, "an `&` parameter"_v);
            return false;
    }

    unsupported(written.source, "this parameter form"_v);
    return false;
}

/*
 * The signature's parameters, in order.
 *
 * Answers false when the declaration is one this stage cannot represent, in which case the function
 * is dropped whole rather than left with a signature a call would be checked against. A partially
 * declared signature is worse than none: every call to it reports against positions the author did
 * not write.
 */
bool Resolver::declareArgs(Function& function, ast::Decl& declaration) {
    auto& written = declaration.fun;
    auto ok = true;

    for(auto argument: written.args.contents(parse)) {
        if(argument.def) {
            unsupported(argument.source, "a parameter default"_v);
            ok = false;
            continue;
        }

        if(argument.lazy) {
            unsupported(argument.source, "a `@lazy` parameter"_v);
            ok = false;
            continue;
        }

        if(argument.caller) {
            unsupported(argument.source, "a `@caller` parameter"_v);
            ok = false;
            continue;
        }

        // `return &a: T` - the result may be rooted in this argument, which is an origin slot in
        // the signature's schema and needs the loan machinery to mean anything.
        if(argument.returnRoot) {
            unsupported(argument.source, "a `return` parameter"_v);
            ok = false;
            continue;
        }

        if(!argument.type) {
            unsupported(argument.source, "an inferred parameter type"_v);
            ok = false;
            continue;
        }

        auto type = resolveType(*parse[argument.type]);
        Contract contract;
        if(!type || !argContract(argument, contract)) {
            ok = false;
            continue;
        }

        /*
         * Two parameters of one name.
         *
         * Refused rather than shadowed. `findLocal` walks the bindings backwards, so the second
         * would win every mention of the name in the body and the first would be a parameter the
         * callee cannot read at all - which is a silent wrong answer rather than a missing feature.
         */
        auto duplicate = false;
        for(auto existing: function.args.contents(decl())) {
            if(decl()[existing]->name != argument.name) continue;

            context.diagnostics.error("this function already has a parameter called %@"_v,
                                      argument.source, context.findName(argument.name));
            duplicate = true;
            break;
        }

        if(duplicate) {
            ok = false;
            continue;
        }

        auto declared = new (program.decl) Arg();
        declared->name = argument.name;
        declared->occurrence = Occurrence { type, contract, nullptr };
        declared->source = argument.source;
        declared->index = U32(function.args.size());

        function.args.push(program.decl, declared - *program.decl);
    }

    return ok;
}

/*
 * The half of a `fn` declaration that is the same whether it stands in a module or in an instance.
 *
 * Answers null where the declaration is one this stage cannot represent. What it deliberately does
 * *not* do is register the name: a module function is reachable by name and an instance member is
 * reached by the compiler through its instance, and which of the two this is belongs to the caller.
 */
Function* Resolver::declareSignature(ast::Decl& declaration, StringId name) {
    auto& written = declaration.fun;

    if(written.kind != ast::FunKind::Plain) {
        unsupported(declaration.source, "lens and iterator functions"_v);
        return nullptr;
    }

    if(written.constraints.size()) {
        unsupported(declaration.source, "generic functions"_v);
        return nullptr;
    }

    auto function = new (program.decl) Function(name, here());
    function->source = declaration.source;

    if(!declareArgs(*function, declaration)) return nullptr;

    /*
     * `-> ->T` - a written result convention, which this stage has no representation for.
     *
     * Refused rather than ignored. The marker is a fact about what the *receiver* gets, and a lens
     * hands a value to its continuation under an argument-like contract, where `->` is the handover.
     * A plain function's result is owned by the caller either way, so silently dropping it would be
     * right today and wrong the moment the two spellings differ - and nothing would say when.
     */
    if(written.retBind != ast::BindType::Borrow) {
        unsupported(declaration.source, "a written result convention"_v);
        return nullptr;
    }

    // Absent, which is `{}` - a `:` body returns nothing and writes no result type at all. The
    // occurrence is owned, because a result the caller receives is theirs.
    auto result = written.ret ? resolveType(*parse[written.ret]) : program.types.unit;
    if(!result) return nullptr;

    function->result = owned(result);

    /*
     * The one fact that is settled at declaration time, and exactly one.
     *
     * `bodyKnown` is about the *declaration* - whether there is anything to analyse - and it is the
     * only summary field a declaration answers by itself. Every other fact stays `Unknown` until its
     * own pass exists, which is what `Fact` is for.
     */
    function->summary.bodyKnown = written.body != nullptr;

    module.functions.push(program.decl, function - *program.decl);
    return function;
}

void Resolver::declareFunction(ast::Decl& declaration) {
    auto function = declareSignature(declaration, declaration.fun.name);
    if(!function) {
        writtenFunctions.push(nullptr);
        return;
    }

    auto pointer = function - *program.decl;
    writtenFunctions.push(pointer);

    /*
     * One declaration per name, which is this milestone's stand-in for an overload set.
     *
     * doc/spec/overloading.md keys a set by `(name, arity)` and admits one plain function per key
     * beside any number of class functions, and selecting among those is milestone 6. Until then a
     * second declaration would be one the name table silently kept the first of - a function nothing
     * could call and no message about it.
     */
    if(!program.addFunctionName(here(), function->name, pointer)) {
        unsupported(declaration.source, "two functions of one name"_v);
    }
}

/*
 * `instance Drop(T)` and `instance Reclaim(T)` - the milestone-4 slice of instance selection.
 *
 * Two classes, both builtin (see `CoreClass`), both with one member, and both heads concrete. What
 * that leaves is a lookup rather than a solve, which is deliberate: the *shape* here is already
 * §11.4's sixth milestone - selection over logical types with no ownership contract involved - and
 * what that milestone adds is a parametric head and a context to discharge, above this table rather
 * than beside it.
 *
 * The member is an ordinary function with an ordinary body and is deliberately **not** registered by
 * name. Nothing calls it from source: a teardown runs because an object died, and the call is the
 * one the drop elaboration emits. Registering it would also make `instance Drop(A)` and
 * `instance Drop(B)` two declarations of `drop` in one module.
 */
void Resolver::declareInstance(ast::Decl& declaration) {
    auto& written = declaration.instance;
    writtenInstances.push(nullptr);

    if(written.constraints.size()) {
        unsupported(declaration.source, "an instance with a context"_v);
        return;
    }

    if(written.type.kind != ast::Type::App) {
        unsupported(declaration.source, "this instance head"_v);
        return;
    }

    auto& head = *parse[written.type.app];
    if(head.base.kind != ast::Type::Con || head.args.size() != 1) {
        unsupported(declaration.source, "this instance head"_v);
        return;
    }

    auto className = stringView(context.findName(head.base.name));
    auto klass = CoreClass::Count;
    if(className == "Drop"_v) klass = CoreClass::Drop;
    else if(className == "Reclaim"_v) klass = CoreClass::Reclaim;

    if(klass == CoreClass::Count) {
        unsupported(declaration.source, "a class other than Drop and Reclaim"_v);
        return;
    }

    auto argument = head.args.get(parse, 0);
    auto subject = resolveType(argument);
    if(!subject) return;

    if(decl()[subject]->kind != Type::Record) {
        context.diagnostics.error("%@ may only be given to a type this module declares"_v,
                                  declaration.source, coreClassName(klass));
        return;
    }

    if(written.decls.size() != 1) {
        context.diagnostics.error("an instance of %@ declares exactly one function"_v,
                                  declaration.source, coreClassName(klass));
        return;
    }

    auto member = written.decls.get(parse, 0);
    if(member.kind != ast::Decl::Fun) {
        unsupported(declaration.source, "a declaration other than a function in an instance"_v);
        return;
    }

    /*
     * The member's own name, which is the class's spelled in lower case in Core and is checked here
     * rather than accepted: `instance Drop(T): fn release(...)` declares nothing the compiler will
     * ever call, and accepting it would be a teardown that silently never runs.
     */
    auto expected = klass == CoreClass::Drop ? "drop"_v : "reclaim"_v;
    if(stringView(context.findName(member.fun.name)) != expected) {
        context.diagnostics.error("an instance of %@ declares %@"_v, member.source,
                                  coreClassName(klass), expected);
        return;
    }

    /*
     * A distinct name for the dump and for the backend's symbol table.
     *
     * The member is not reachable by this name from source - nothing looks one up - so what it has
     * to be is unique and readable. The bytes are copied into the declaration arena first, because
     * **an interned name borrows its text**: handing `addUnqualifiedName` a formatted temporary
     * leaves the name table pointing into a buffer that is freed when this function returns, and
     * what prints afterwards is whatever the allocator put there next.
     */
    auto text = Tritium::format("%@.%@", context.findName(((RecordType*)decl()[subject])->name),
                                expected);

    auto stored = (char*)program.decl.alloc(text.size());
    copy(text.text(), stored, text.size());

    auto name = context.addUnqualifiedName(stored, text.size());

    auto function = declareSignature(member, name);
    if(!function) return;

    /*
     * The signature the class declares, checked against the one that was written.
     *
     * `fn drop(->value: a) -> {}` with `a` at the instance's own type. Checked rather than assumed
     * because everything downstream calls it with exactly that: the drop elaboration hands over the
     * dying object under `Take`, and a member declared to borrow it would receive a loan of storage
     * that is about to stop existing.
     */
    auto ok = function->args.size() == 1 &&
              decl()[function->result.type]->kind == Type::Unit;

    if(ok) {
        auto& argument = *decl()[function->args.get(decl(), 0)];
        ok = sameType(argument.occurrence.type, subject) && argument.occurrence.contract.isOwned();
    }

    if(!ok) {
        context.diagnostics.error("%@ for %@ must be written `fn %@(->value: %@) -> {}`"_v,
                                  member.source, expected,
                                  typeText(context, decl(), subject), expected,
                                  typeText(context, decl(), subject));
        return;
    }

    function->teardown = true;

    auto instance = new (program.decl) Instance();
    instance->klass = klass;
    instance->type = subject;
    instance->function = function - *program.decl;
    instance->source = declaration.source;

    auto pointer = instance - *program.decl;
    module.instances.push(program.decl, pointer);
    writtenInstances[writtenInstances.size() - 1] = pointer;

    if(!program.addInstance(pointer)) {
        context.diagnostics.error("%@ already has an instance of %@"_v, declaration.source,
                                  typeText(context, decl(), subject), coreClassName(klass));
    }
}

void Resolver::declare(ast::Module& file) {
    Size record = 0;

    for(auto declaration: file.decls.contents(parse)) {
        switch(declaration.kind) {
            case ast::Decl::Stmt:
                declareGlobals(declaration);
                break;
            case ast::Decl::Fun:
                declareFunction(declaration);
                break;
            case ast::Decl::Data:
                declareFields(declaration, record);
                break;
            case ast::Decl::Instance:
                declareInstance(declaration);
                break;
            case ast::Decl::Error:
                break;
            default:
                unsupported(declaration.source, "this declaration"_v);
                break;
        }
    }
}

} // namespace mir
