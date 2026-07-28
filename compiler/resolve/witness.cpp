#include "witness.h"
#include "analyze.h"
#include "expr.h"
#include "generic.h"
#include "name.h"

/*
 * Building the constants.
 *
 * Everything here follows the same three steps: allocate the bytes in the module arena, write the
 * scalar fields into them, and record a relocation for each address the table holds. The addresses
 * themselves are deliberately not resolved - a witness may name a function that has not been
 * generated yet, and none of them has an address at all until the module is placed.
 */

namespace {

// A global nothing in the source can name. Interned tables need a unique symbol for printing and
// linking, and this is the same trick addAnonymousFunction plays for the same reason.
Global* addAnonymousGlobal(Module& module, StringId name, LocationId source) {
    auto global_ = new (module.arena) Global(&module, name);
    global_->source = source;
    global_->type = module.scalar.unit;
    global_->used = true;
    module.globalOrder.push(module.arena, global_ - *module.arena);
    return global_;
}

struct TableBuilder {
    TableBuilder(Module& module, Size size): module(module) {
        bytes = ByteBuffer((Byte*)module.arena.alloc(size), size);
        set(bytes.ptr, size, 0);
    }

    void putU32(U32 offset, U32 value) {
        copy((const Byte*)&value, bytes.ptr + offset, sizeof(U32));
    }

    // An address is left as zeroes and recorded instead. A null target records nothing, which is
    // how "this type has no drop" reaches the reader as a null slot rather than as a missing entry.
    void putFunction(Global& target, U32 offset, ModulePtr<Function> function) {
        if(!function) return;

        (*module.arena)[function]->used = true;
        target.relocations.push(module.arena, GlobalRelocation { offset, function, nullptr });
    }

    void putGlobal(Global& target, U32 offset, ModulePtr<Global> global_) {
        if(!global_) return;

        (*module.arena)[global_]->used = true;
        target.relocations.push(module.arena, GlobalRelocation { offset, nullptr, global_ });
    }

    Module& module;
    ByteBuffer bytes;
};

StringId tableName(Module& module, StringView prefix, TypePtr type) {
    StringBuilder text;
    text << prefix;
    describeType(module.context, *module.types, type, text);
    return module.context.addQualifiedName(text.pointer(), text.size(), 1);
}

} // namespace

/*
 * moveInit.
 *
 * Implementation-Generics.md part 4 lists three answers - a block copy for TrivialSink, an adapter
 * over the authored `Sink` where one exists, and unavailable - and the first two are generated the
 * same way: a two-argument function taking the destination and the source as raw pointers, so that
 * generic code can call it without knowing either type or size.
 *
 * The "unavailable" case is not represented here at all, deliberately. Whether a body may move a
 * value of an unknown type is a question its *schema* answers, and it is answered before any of
 * this is reached; a descriptor that carried "you may not" would be inviting a second, later check
 * of something already settled.
 */
ModulePtr<Function> moveInitFor(Module& module, TypePtr type, LocationId source) {
    auto& program = module.program;
    if(!type || isGeneric(*module.types, type)) return nullptr;

    if(auto found = program.moveInitGlue.get(U32(type))) return found.unwrap();

    auto ownership = ownershipOf(module, type);
    auto size = typeSize(*module.types, type);

    // Nothing to relocate: a zero-sized type, or one whose bytes the caller has already placed.
    if(!size) return nullptr;

    auto function = addAnonymousFunction(module, tableName(module, "moveInit$"_v, type), source);
    auto pointer = function - *module.arena;
    *program.moveInitGlue.add(U32(type)).value = pointer;

    function->returnType = module.scalar.unit;
    function->used = true;

    auto pointerType = resolvePointerType(module, type);
    auto to = function->addArg(module, module.context.addQualifiedName("to", 2, 1), pointerType, source);
    auto from = function->addArg(module, module.context.addQualifiedName("from", 4, 1), pointerType, source);

    ExprResolver resolver(module.context, module, *function);

    if(ownership.trivialSink) {
        // A relocation of a TrivialSink value is its bytes and nothing else, which is what the
        // class means. copyMemory rather than a load and a store because the size is a constant
        // here and the type may be an aggregate with no register form at all.
        auto bytes = resolver.makeInt(source, module.scalar.long_, size);
        auto byteType = resolvePointerType(module, module.scalar.unit);

        auto castTo = resolver.ref(resolver.emit<InstUnary>(source, 0, byteType, Value::Cast,
                                                            (ModulePtr<Value>)(to - *module.arena)));
        auto castFrom = resolver.ref(resolver.emit<InstUnary>(source, 0, byteType, Value::Cast,
                                                              (ModulePtr<Value>)(from - *module.arena)));

        auto copyInst = resolver.create<InstNative>(source, 0, module.scalar.unit, NativeOp::CopyMemory);
        copyInst->args.push(module.arena, castTo);
        copyInst->args.push(module.arena, castFrom);
        copyInst->args.push(module.arena, bytes);
        resolver.append(copyInst);
    } else if(ownership.authoredSink) {
        // The authored `Sink` takes `(&to: a, ->from: a)`, and both conventions arrive as addresses,
        // so the two pointers this function was given are already the arguments it wants.
        TypePtr args[] = { type };
        auto match = matchInstance(module, module.coreClasses.sink, toBuffer(args));

        if(match && !(*module.arena)[match.instance]->functions.isEmpty()) {
            auto implementation = (*module.arena)[match.instance]->functions.get(*module.arena, 0);

            if(implementation && (*module.arena)[implementation]->gen) {
                implementation = instantiateFunction(module, implementation, toBuffer(match.args), source);
            }

            if(implementation) {
                (*module.arena)[implementation]->used = true;

                auto call = resolver.create<InstCall>(source, 0, module.scalar.unit, implementation);
                call->args.push(module.arena, (ModulePtr<Value>)(to - *module.arena));
                call->args.push(module.arena, (ModulePtr<Value>)(from - *module.arena));
                resolver.append(call);
            }
        }
    }

    resolver.terminate(resolver.emit<InstRet>(source, 0, module.scalar.unit, nullptr));
    return pointer;
}

ModulePtr<Global> typeDescFor(Module& module, TypePtr type, LocationId source) {
    auto& program = module.program;
    if(!type || isGeneric(*module.types, type)) return nullptr;

    if(auto found = program.typeDescs.get(U32(type))) return found.unwrap();

    auto global_ = addAnonymousGlobal(module, tableName(module, "typeDesc$"_v, type), source);
    auto pointer = global_ - *module.arena;

    // Registered before the lifecycle functions are generated, since generating one for a type
    // reachable from itself asks for this descriptor again.
    *program.typeDescs.add(U32(type)).value = pointer;

    auto ownership = ownershipOf(module, type);
    auto size = typeSize(*module.types, type);
    auto align = typeAlign(*module.types, type);

    TableBuilder table(module, TypeDescLayout::kSize_);
    table.putU32(TypeDescLayout::kLogicalType, U32(type));
    table.putU32(TypeDescLayout::kSize, size);
    table.putU32(TypeDescLayout::kAlign, align);
    table.putU32(TypeDescLayout::kStride, align ? ((size + align - 1) & ~(align - 1)) : size);

    // Nothing selects a non-canonical Repr yet, and no type declares that it must keep its address,
    // so the only source of a stable-address requirement is a Repr variant - which is Milestone 8's.
    table.putU32(TypeDescLayout::kFlags, typeDescFlags(ownership, false));

    global_->contents = table.bytes;

    table.putFunction(*global_, TypeDescLayout::kMoveInit, moveInitFor(module, type, source));
    table.putFunction(*global_, TypeDescLayout::kReclaim,
                      teardownImplementation(module, type, Teardown::Reclaim, source));
    table.putFunction(*global_, TypeDescLayout::kDrop,
                      teardownImplementation(module, type, Teardown::Drop, source));

    return pointer;
}
