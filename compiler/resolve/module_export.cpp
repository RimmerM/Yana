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

// Orders declarations by where they were written, so that the diagnostics below come out in that
// order rather than in the order a hash map happened to hold them. An insertion sort because these
// lists are the exported declarations of one module, which is a handful.
template<class T, U32 N>
static void sortBySource(GlobalBase global, SmallArray<GlobalPtr<T>, N>& list) {
    for(Size i = 1; i < list.size(); i++) {
        for(Size j = i; j > 0 && global[list[j]]->source < global[list[j - 1]]->source; j--) {
            swap(list[j], list[j - 1]);
        }
    }
}

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
        case Type::Array: {
            auto array = (ArrayType*)global[type];
            if(auto found = privateTypeIn(global, array->count)) return found;
            return privateTypeIn(global, array->content);
        }
        case Type::Vector: {
            auto vector = (VectorType*)global[type];
            if(auto found = privateTypeIn(global, vector->count)) return found;
            return privateTypeIn(global, vector->content);
        }
        case Type::Atomic: return privateTypeIn(global, ((AtomicType*)global[type])->content);
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
// to be told is which part of it leaks. `member` qualifies it where the signature belongs to a
// class - see checkExportedSignature - and is 0 where the declaration is its own answer.
//
// The position is assembled *after* the early return rather than by the caller, because a signature
// that leaks nothing is the whole corpus: every argument of every exported function and every member
// of every exported class reaches here, and building the text for each of them was the single
// largest source of allocations in the compiler.
static void checkExportedType(Module& module, TypePtr type, StringId member, StringView what,
                              StringId declaration, LocationId source) {
    auto global = *module.types;
    auto found = privateTypeIn(global, type);
    if(!found) return;

    StringBuilder text;
    describeType(module.context, global, found, text);

    StringBuilder position;
    if(member) position << "member " << module.context.findName(member) << "'s " << what;
    else position << what;

    module.context.diagnostics.error("%@ is `pub` but its %@ names %@, which is not - an importer could hold a value of a type it cannot name, so either export %@ or stop exporting %@"_v,
                                     source, module.context.findName(declaration), position.string(),
                                     text.string(), text.string(),
                                     module.context.findName(declaration));
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

    for(Size i = 0; i < function.args.size(); i++) {
        auto arg = local[function.args.get(local, i)];
        checkExportedType(module, arg->declaredType(), member, "argument type"_v, declaration,
                          arg->source == kNullLocation ? source : arg->source);
    }

    checkExportedType(module, function.returnType, member, "result type"_v, declaration, source);
}

/*
 * `pub` where the marker has no meaning.
 *
 * An `instance` is global and an `infixl`/`infixr` applies wherever its operator is in scope, so
 * neither has a visibility to widen - doc/spec/modules.md lists both under "always visible". A
 * class default was the third, until it moved into the class head where the `pub` on the class
 * already covers it.
 *
 * The fixity case cannot be reached at all, because the parser reads a fixity before it reads the
 * `pub` a declaration may carry - `pub infixl 7 +` is a parse error rather than this one - and that
 * is a better diagnostic than this could give. It is written down here rather than left implicit
 * because the *rule* covers both and only one of them needs code.
 */
static void checkExportMarker(Module& module, ast::Decl& decl) {
    if(!decl.exported) return;
    if(decl.kind != ast::Decl::Instance) return;

    module.context.diagnostics.error("`pub` says nothing on an instance - it is reached through its class, which carries the visibility, and there is no spelling that makes this one less visible"_v,
                                     decl.source);
}

void checkModuleExports(Module& module) {
    auto global = *module.types;
    auto local = *module.arena;
    auto parse = module.parse;

    for(auto file: module.files) {
        for(auto decl: file->decls.contents(parse)) {
            checkExportMarker(module, decl);
        }
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
    /*
     * Declaration order in all four, never the order a name's hash put it in.
     *
     * These are diagnostics, so the order they come out in is the order a reader is handed them -
     * and a hash map's is arbitrary. `functionOrder` and `globalOrder` are what the rest of the
     * compiler walks for exactly this reason; types and classes have no such list, so the ones this
     * check cares about are collected and sorted by where they were written. Sorting rather than
     * adding two more lists because this runs once per module and reaches a handful of declarations,
     * where the lists would be paid for on every one.
     */
    for(auto pointer: module.functionOrder.contents(local)) {
        auto function = local[pointer];
        if(!function->exported) continue;

        checkExportedSignature(module, *function, function->name, StringId(), function->source);
    }

    for(auto pointer: module.globalOrder.contents(local)) {
        auto global_ = local[pointer];
        if(!global_->exported) continue;

        checkExportedType(module, global_->type, StringId(), "type"_v, global_->name, global_->source);
    }

    // A record's fields, through its constructors' content tuples - which is where a field's type
    // lives, and is the same place field access reads one from.
    SmallArray<GlobalPtr<RecordType>, 16> records;
    for(auto entry: module.namedTypes.entries()) {
        if(global[entry.value]->kind != Type::Record) continue;

        auto record = (RecordType*)global[entry.value];
        if(!record->exported || record->instanceOf) continue;

        records.push(record - global);
    }

    sortBySource(global, records);

    for(auto pointer: records) {
        auto record = global[pointer];

        for(auto constructor: record->constructors.contents(global)) {
            checkExportedType(module, constructor.content, StringId(), "field type"_v, record->name,
                              record->source);
        }
    }

    SmallArray<GlobalPtr<TypeClass>, 16> classes;
    for(auto entry: module.classes.entries()) {
        if(!global[entry.value]->exported) continue;
        classes.push(entry.value);
    }

    sortBySource(global, classes);

    for(auto pointer: classes) {
        auto typeClass = global[pointer];

        for(auto member: typeClass->functions.contents(global)) {
            if(!member.fun) continue;
            checkExportedSignature(module, *local[member.fun], typeClass->name, member.name,
                                   typeClass->source);
        }
    }
}
