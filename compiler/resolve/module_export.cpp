#include "module_internal.h"
#include "name.h"
#include "../parse/ast.h"

/*
 * `pub`, from the declaring side.
 *
 * The reading side is resolve/name.h: a symbol found in an imported module is a hit only when it
 * was exported, which is one clause beside the include/exclude one and is the whole of what makes
 * a private declaration private. What is here is the two things that clause cannot say by itself.
 *
 * **A marker that means nothing is reported.** `pub` on an instance or on a fixity declaration says
 * something untrue - both are always visible, for reasons doc/spec/modules.md gives - and leaving it
 * accepted-and-ignored would put the marker back in the state this whole item exists to get it out
 * of, which is a word in the source that nothing reads.
 *
 * **And a `pub` declaration may not name a private type.** Without this, `pub` restricts *names*
 * and nothing else: an importer that cannot write `Secret` can still call `pub fn make() -> Secret`
 * and hold one, pass it back, and store it in a field - a value of a type it has no way to name,
 * destructure or write a signature for. The marker is meant to be a boundary rather than a name
 * filter, and a boundary with a hole in it is worse than none, because it reads as a guarantee.
 *
 * What the check is *not* is a transitive audit of what an importer can reach. A `pub` record's
 * field types are checked, and so a value of a private type cannot arrive that way either; what is
 * deliberately out of scope is the private type an exported one reaches through a *class instance*,
 * since instances are global and coherence is what makes them so. That is the same boundary
 * doc/spec/classes.md draws and not a gap in this one.
 */

// A named type's own marker, read through the declaration rather than off the type in hand.
//
// `Maybe(Int)` is an instantiation, made where it was first written rather than by the declaration
// pass, so nothing wrote `pub` on it and its own flag is false. The declaration is what carries the
// answer, and base() is how every other question about an instantiation reaches it.
static bool namedTypeExported(GlobalBase global, TypePtr type) {
    auto record = (RecordType*)global[type];
    return ((Type*)global[record->base(global)])->exported;
}

/*
 * The first private record reachable inside a type, or null.
 *
 * Only `Record` is asked, because it is the only kind a declaration produces. A primitive is
 * registered by the compiler and exported by construction - `Int` has to be nameable everywhere -
 * and every other kind is structural: a borrow, a tuple, a pointer, a function type and a fixed
 * array are spelled out of their contents rather than named, so there is nothing on one for a `pub`
 * to have been written on and the walk goes straight through it.
 *
 * An instantiation is both: `Maybe(Secret)` is a public declaration applied to a private argument,
 * so the arguments are walked as well as the head.
 */
static TypePtr privateTypeIn(GlobalBase global, TypePtr type) {
    if(!type) return nullptr;

    switch(global[type]->kind) {
        case Type::Record: {
            if(!namedTypeExported(global, type)) return type;

            auto record = (RecordType*)global[type];
            for(auto arg: record->instanceArgs.contents(global)) {
                if(auto found = privateTypeIn(global, arg)) return found;
            }

            return nullptr;
        }
        case Type::Tup: {
            auto tuple = (TupType*)global[type];
            for(Size i = 0; i < tuple->fields.size(); i++) {
                if(auto found = privateTypeIn(global, tuple->fields.get(global, i).type)) return found;
            }

            return nullptr;
        }
        case Type::Ptr: return privateTypeIn(global, ((PtrType*)global[type])->to);
        case Type::Array: return privateTypeIn(global, ((ArrayType*)global[type])->content);
        case Type::Borrow: return privateTypeIn(global, ((BorrowType*)global[type])->to);
        case Type::Fun: {
            auto function = (FunType*)global[type];
            for(auto arg: function->args.contents(global)) {
                if(auto found = privateTypeIn(global, arg.type)) return found;
            }

            return privateTypeIn(global, function->result);
        }
        default:
            return nullptr;
    }
}

// One position of an exported interface. `what` names the position rather than the declaration,
// since the declaration's own name is already at the reported location and the thing a reader has
// to be told is which part of it leaks.
static void checkExportedType(Module& module, TypePtr type, const String& what, StringId declaration,
                              LocationId source) {
    auto global = *module.types;
    auto found = privateTypeIn(global, type);
    if(!found) return;

    StringBuilder text;
    describeType(module.context, global, found, text);

    module.context.diagnostics.error("%@ is `pub` but its %@ names %@, which is not - an importer could hold a value of a type it cannot name, so either export %@ or stop exporting %@"_v,
                                     source, module.context.findName(declaration), what, text.string(),
                                     text.string(), module.context.findName(declaration));
}

/*
 * A function's signature: every parameter, and the result.
 *
 * Shared by a plain `pub fn` and by the members of a `pub class`, which are the same question asked
 * of a signature with no body - and `member` is the whole of the difference. A class is what carries
 * the marker while a *member* is what leaks, so the position a class's diagnostic names has to say
 * which of its signatures it is talking about; a plain function passes 0 and is its own answer.
 */
static void checkExportedSignature(Module& module, Function& function, StringId declaration,
                                   StringId member, LocationId source) {
    auto local = *module.arena;

    auto position = [&](StringView what) {
        if(!member) return toString(what);

        StringBuilder text;
        text << "member " << module.context.findName(member) << "'s " << what;
        return text.string();
    };

    for(Size i = 0; i < function.args.size(); i++) {
        auto arg = local[function.args.get(local, i)];
        checkExportedType(module, arg->declaredType(), position("argument type"_v), declaration,
                          arg->source == kNullLocation ? source : arg->source);
    }

    checkExportedType(module, function.returnType, position("result type"_v), declaration, source);
}

/*
 * `pub` where the marker has no meaning.
 *
 * An `instance` is global and an `infixl`/`infixr` applies wherever its operator is in scope, so
 * neither has a visibility to widen - doc/spec/modules.md lists both under "always visible". A
 * `default Class = Type` is the same: it belongs to the class, which is where a reader looks for it.
 *
 * The fixity case cannot be reached at all, because the parser reads a fixity before it reads the
 * `pub` a declaration may carry - `pub infixl 7 +` is a parse error rather than this one - and that
 * is a better diagnostic than this could give. It is written down here rather than left implicit
 * because the *rule* covers both and only one of them needs code.
 */
static void checkExportMarker(Module& module, ast::Decl& decl) {
    if(!decl.exported) return;
    if(decl.kind != ast::Decl::Instance && decl.kind != ast::Decl::Default) return;

    module.context.diagnostics.error("`pub` says nothing on %@ - it is reached through its class, which carries the visibility, and there is no spelling that makes this one less visible"_v,
                                     decl.source,
                                     decl.kind == ast::Decl::Instance ? "an instance"_v : "a class default"_v);
}

void checkModuleExports(Module& module, ast::Module& ast) {
    auto global = *module.types;
    auto local = *module.arena;
    auto parse = module.parse;

    for(auto decl: ast.decls.contents(parse)) {
        checkExportMarker(module, decl);
    }

    /*
     * The four kinds of exported interface, read off the resolved declarations rather than off the
     * AST: what a signature position *is* has already been decided, aliases have been substituted
     * through, and a generic head has become the declaration its instantiations point back at.
     *
     * A `pub` alias needs no case of its own for the same reason. An alias is transparent, so
     * `pub alias Name = Secret` gives an importer a second spelling of a type it may already
     * name - and where it may not, every position that could hand one over is one of the four
     * below and is reported there.
     */
    for(auto entry: module.functions.entries()) {
        auto function = local[entry.value];
        if(!function->exported) continue;

        checkExportedSignature(module, *function, function->name, 0, function->source);
    }

    for(auto entry: module.globals.entries()) {
        auto global_ = local[entry.value];
        if(!global_->exported) continue;

        checkExportedType(module, global_->type, toString("type"_v), global_->name, global_->source);
    }

    // A record's fields, through its constructors' content tuples - which is where a field's type
    // lives, and is the same place field access reads one from.
    for(auto entry: module.namedTypes.entries()) {
        if(global[entry.value]->kind != Type::Record) continue;

        auto record = (RecordType*)global[entry.value];
        if(!record->exported || record->instanceOf) continue;

        for(auto constructor: record->constructors.contents(global)) {
            checkExportedType(module, constructor.content, toString("field type"_v), record->name,
                              record->source);
        }
    }

    for(auto entry: module.classes.entries()) {
        auto typeClass = global[entry.value];
        if(!typeClass->exported) continue;

        for(auto member: typeClass->functions.contents(global)) {
            if(!member.fun) continue;
            checkExportedSignature(module, *local[member.fun], typeClass->name, member.name,
                                   typeClass->source);
        }
    }
}
