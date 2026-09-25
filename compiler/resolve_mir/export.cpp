#include "resolve_internal.h"
#include "name.h"

namespace mir {

/*
 * `pub`, from the declaring side.
 *
 * The reading side is `name.h`: a symbol found in an imported module is a hit only when it was
 * exported, which is one clause beside the include/exclude one and is the whole of what makes a
 * private declaration private. What is here is the two things that clause cannot say by itself.
 *
 * **A marker that means nothing is reported.** `pub` on an instance says something untrue - an
 * instance is global, for the reason `doc/spec/modules.md` gives - and leaving it accepted-and-
 * ignored would leave a word in the source that nothing reads.
 *
 * **And a `pub` declaration may not name a private type.** Without this, `pub` restricts *names* and
 * nothing else: an importer that cannot write `Secret` can still call `pub fn make() -> Secret` and
 * hold one, pass it back, and store it in a field - a value of a type it has no way to name,
 * destructure or write a signature for. The marker is meant to be a boundary rather than a name
 * filter, and a boundary with a hole in it is worse than none, because it reads as a guarantee.
 *
 * It runs where an exported interface is *finished* - after every signature, field and global type
 * is resolved, and before any body: a body has nothing to say about who may name what.
 *
 * What the check is *not* is a transitive audit of what an importer can reach. A `pub` record's
 * field types are checked, so a value of a private type cannot arrive that way either; what is
 * deliberately out of scope is the private type an exported one reaches through a *class instance*,
 * since instances are global and coherence is what makes them so.
 */

/*
 * The first private record reachable inside a type, or null.
 *
 * Only `Record` carries a marker, because it is the only kind a declaration produces. Everything
 * else is either builtin - and exported by construction, since `I32` has to be nameable everywhere -
 * or structural: a pointer and a function type are spelled out of their contents rather than named,
 * so there is nothing on one for a `pub` to have been written on and the walk goes straight through.
 *
 * An instantiation is both: `Maybe(Secret)` is a public declaration applied to a private argument,
 * so the arguments are walked as well as the head.
 *
 * The switch is exhaustive with no `default`, which is the point: a type kind added without an arm
 * here is a compile error rather than a leak reported about nothing. `compiler/resolve`, deleted in
 * J173, had to assert against a table for the same guarantee.
 */
static DeclPtr<Type> privateTypeIn(DeclBase base, DeclPtr<Type> type) {
    if(!type) return nullptr;

    switch(base[type]->kind) {
        case Type::Record: {
            auto record = DeclPtr<RecordType>(type);

            /*
             * A **tuple** has no declaration to be private, so what it can hand out is what is in
             * it - its fields, walked the way an instantiation's arguments are.
             *
             * Asking `exportedSymbol` here would answer no for every one of them: a tuple is
             * interned on its field list and belongs to no module, so `exported` is a flag nothing
             * ever set. That made `pub fn pair() -> {Int, Int}` a leak of `{I32, I32}` - a type
             * every importer can write out, since writing the fields is the only way to name one.
             */
            if(base[record]->tuple) {
                for(auto field: base[record]->fields.contents(base)) {
                    if(auto found = privateTypeIn(base, field.occurrence.type)) return found;
                }

                return nullptr;
            }

            if(!exportedSymbol(base, record)) return type;

            for(auto argument: base[record]->arguments.contents(base)) {
                if(auto found = privateTypeIn(base, argument)) return found;
            }

            return nullptr;
        }

        case Type::Pointer:
            return privateTypeIn(base, ((PointerType*)base[type])->to);

        case Type::Fun: {
            auto& function = *(FunType*)base[type];

            for(auto param: function.params.contents(base)) {
                if(auto found = privateTypeIn(base, param.occurrence.type)) return found;
            }

            return privateTypeIn(base, function.result.type);
        }

        // An erased iterator object hands out what its protocol names, and nothing else: the block
        // behind it is the producer's and has no spelling here at all.
        case Type::Iter: {
            auto& iterated = *(IterType*)base[type];
            if(auto found = privateTypeIn(base, iterated.handover.type)) return found;
            return privateTypeIn(base, iterated.resume);
        }

        // The element, which is the only thing a `[T *n]` can be a way to hand out. The count is a
        // number and names no declaration.
        case Type::Array:
            return privateTypeIn(base, ((ArrayType*)base[type])->element);

        // The lane and the content, on the same reading: each is a type the constructor is a way of
        // handing out, and the lane count is a number.
        case Type::Vector:
            return privateTypeIn(base, ((VectorType*)base[type])->lane);

        case Type::Atomic:
            return privateTypeIn(base, ((AtomicType*)base[type])->content);

        // Every kind that names no declaration. A `Const` is a number and a `Variable` is a position
        // some caller fills, so neither can be a way to hand a type out.
        case Type::Unit:
        case Type::Int:
        case Type::Float:
        case Type::Variable:
        case Type::Const:
            return nullptr;

        case Type::KindCount:
            break;
    }

    return nullptr;
}

/*
 * One position of an exported interface.
 *
 * `what` names the position rather than the declaration, since the declaration's own name is already
 * at the reported location and the thing a reader has to be told is which part of it leaks. `member`
 * qualifies it where the signature belongs to a class, and is zero where the declaration is its own
 * answer.
 *
 * The position text is assembled *after* the early return rather than by the caller, because a
 * signature that leaks nothing is the whole corpus: every argument of every exported function
 * reaches here, and building the text for each of them would be paid for on all of them.
 */
static void checkExportedType(Context& context, DeclBase base, DeclPtr<Type> type, StringId member,
                              StringView what, StringId declaration, LocationId source) {
    auto found = privateTypeIn(base, type);
    if(!found) return;

    auto text = typeText(context, base, found);

    StringBuilder position;
    if(member) position << "member " << context.findName(member) << "'s " << what;
    else position << what;

    context.diagnostics.error("%@ is `pub` but its %@ names %@, which is not - an importer could hold a value of a type it cannot name, so either export %@ or stop exporting %@"_v,
                              source, context.findName(declaration), position.string(), text, text,
                              context.findName(declaration));
}

/*
 * A function's signature: every parameter, and the result.
 *
 * Shared by a plain `pub fn` and by the members of a `pub class`, which are the same question asked
 * of a signature with no body - and `member` is the whole of the difference. A class is what carries
 * the marker while a *member* is what leaks, so the position a class's diagnostic names has to say
 * which of its signatures it is talking about; a plain function passes zero and is its own answer.
 */
static void checkExportedSignature(Context& context, DeclBase base, Function& function,
                                   StringId declaration, StringId member, LocationId source) {
    for(auto pointer: function.args.contents(base)) {
        auto& argument = *base[pointer];
        checkExportedType(context, base, argument.occurrence.type, member, "argument type"_v,
                          declaration,
                          argument.source == kNullLocation ? source : argument.source);
    }

    checkExportedType(context, base, function.result.type, member, "result type"_v, declaration,
                      source);
}

/*
 * `pub` where the marker has no meaning.
 *
 * An `instance` is global and an `infixl`/`infixr` applies wherever its operator is in scope, so
 * neither has a visibility to widen - `doc/spec/modules.md` lists both under "always visible". A
 * class default was the third, until it moved into the class head, where the `pub` on the class
 * already covers it.
 *
 * The fixity case cannot be reached at all, because the parser reads a fixity before it reads the
 * `pub` a declaration may carry - `pub infixl 7 +` is a parse error rather than this one, and that
 * is a better diagnostic than this could give. It is written down here rather than left implicit
 * because the *rule* covers both and only one of them needs code.
 */
static void checkExportMarker(Context& context, ast::Decl& declaration) {
    if(!declaration.exported) return;
    if(declaration.kind != ast::Decl::Instance) return;

    context.diagnostics.error("`pub` says nothing on an instance - it is reached through its class, which carries the visibility, and there is no spelling that makes this one less visible"_v,
                              declaration.source);
}

/*
 * A `pub let`'s type, checked after the initializers rather than with the rest.
 *
 * A global has no written type: what it is is what its initializer said, and that is resolved a pass
 * later than every other position here. So the declaring side of `pub` is finished at two moments
 * rather than one, and this is the second - the alternative is a check that runs late enough for the
 * global and therefore reports a leaking signature only in a program that had no other diagnostic.
 */
void checkExportedGlobals(Context& context, Program& program) {
    auto base = *program.decl;

    for(auto modulePointer: program.modules) {
        for(auto pointer: base[modulePointer]->globals.contents(base)) {
            auto& global = *base[pointer];
            if(!global.exported) continue;

            checkExportedType(context, base, global.occurrence.type, StringId(), "type"_v,
                              global.name, global.source);
        }
    }
}

void checkExports(Context& context, Program& program, Buffer<ast::ModuleGroup*> groups) {
    auto base = *program.decl;
    auto parse = *context.parseRegion;

    for(auto group: groups) {
        for(auto file: group->files) {
            for(auto declaration: file->decls.contents(parse)) {
                // A declaration this target has not got is not in the program, so it has no
                // interface to judge - `platformEnabled`, which is the resolver's own selection
                // asked here rather than restated.
                if(!platformEnabled(context, parse, declaration)) continue;

                checkExportMarker(context, declaration);
            }
        }
    }

    /*
     * The four kinds of exported interface, read off the resolved declarations rather than off the
     * AST: what a signature position *is* has already been decided, aliases have been substituted
     * through, and a generic head has become the declaration its instantiations point back at.
     *
     * A global is not among them and is `checkExportedGlobals` above, which runs a pass later for
     * the reason written there.
     *
     * A `pub alias` needs no case of its own for the same reason. An alias is transparent, so
     * `pub alias Name = Secret` gives an importer a second spelling of a type it may already name -
     * and where it may not, every position that could hand one over is one of the four below and is
     * reported there.
     *
     * Declaration order throughout, which is what the module's own lists are in: these are
     * diagnostics, so the order they come out in is the order a reader is handed them.
     */
    for(auto modulePointer: program.modules) {
        auto& module = *base[modulePointer];

        for(auto pointer: module.functions.contents(base)) {
            auto& function = *base[pointer];

            // A class member's marker is its head's, and a class is checked below under the name a
            // reader would have to change - so a member reached here would be the same leak reported
            // against a declaration nobody wrote `pub` on.
            if(function.klass || !function.exported) continue;

            checkExportedSignature(context, base, function, function.name, StringId(),
                                   function.source);
        }

        // A record's fields. An instantiation is skipped: it is the declaration that carries the
        // marker, and `Boxed(Secret)` written inside a `pub` signature is reported at that signature
        // rather than a second time here.
        for(auto pointer: module.records.contents(base)) {
            auto& record = *base[pointer];
            if(!record.exported || record.instanceOf) continue;

            for(auto field: record.fields.contents(base)) {
                checkExportedType(context, base, field.occurrence.type, StringId(), "field type"_v,
                                  record.name, field.source);
            }
        }
    }

    // The classes, which are program-wide rather than per module - `Program::classes`. Filtered by
    // the marker rather than by the module, since what leaks is a `pub` signature wherever it was
    // written.
    for(auto pointer: program.classes) {
        auto& klass = *base[pointer];
        if(!klass.exported) continue;

        for(auto member: klass.members.contents(base)) {
            checkExportedSignature(context, base, *base[member], klass.name, base[member]->name,
                                   klass.source);
        }
    }
}

} // namespace mir
