#include "witness.h"
#include "analyze.h"
#include "expr.h"
#include "generic.h"
#include "name.h"

/*
 * How big a closure header is, as a type - and that is the whole of what it is for.
 *
 * There used to be a tuple per table here, so that a place rooted in a table address could read a
 * slot the way any other aggregate is read. That stopped being possible when a slot stopped being a
 * pointer wide: a table's addresses are four bytes and self-relative, which is not a field of any
 * type, and describing them as one was only ever right by coincidence. Reading a slot is
 * InstTableSlot now, and the descriptor's tuple - which nothing but its own assertion ever read - is
 * gone with it.
 *
 * This one survives because the *size* is still a real question. A native closure header sits
 * immediately in front of its entry point, so the teardown finds it by subtracting the header's own
 * size (see teardownFunValue), and that number has to be the emitting target's rather than one
 * written here. Made of words rather than addresses because that is what a slot now is; the field
 * names are gone, since nothing projects into it and a name would suggest something does.
 */
TypePtr funValueFieldType(Module& module, U16 field) {
    // The two words happen to have the same type, which is not a reason to answer without looking:
    // a projection index that is neither of them describes no word of a function value, and handing
    // it an address anyway would let a place nothing laid out reach lowering as a load at an offset.
    switch(field) {
        case FunValueLayout::kCode:
        case FunValueLayout::kEnv:
            return resolvePointerType(module, module.scalar.unit);

        // The header the target attached to the code word, where it attached one - see
        // FunValueLayout::kHeader. A bare address like the other two: what is behind it is a table,
        // and a table is read by slot rather than projected into - see InstTableSlot.
        case FunValueLayout::kHeader:
            return resolvePointerType(module, module.scalar.unit);
        default:
            return module.scalar.error;
    }
}

U16 classSuperclassSlot(GlobalBase global, GlobalPtr<TypeClass> typeClass, U16 index) {
    auto methodCount = U16(global[typeClass]->functions.size());
    return ClassWitnessFields::super(methodCount, index);
}

TypePtr closureHeaderPlaceType(Module& module) {
    auto& context = module.context;
    auto word = module.scalar.int_;

    // One per slot, so that the tuple's size is the table's size. Which slot is which does not
    // matter here and deliberately has no name: nothing reads a field of this.
    auto count = ClosureHeaderFields::kCount;

    Field fields[ClosureHeaderFields::kCount];
    for(U16 i = 0; i < count; i++) {
        fields[i] = Field { word, context.addUnqualifiedName("slot", 4) };
    }

    // Pinned, and this is the strict one: the code generator places exactly these bytes in front of
    // an entry point and the teardown subtracts exactly this size to find them. checkTableTypes is
    // what asserts the two agree.
    auto tuple = resolveTupleType(module, { fields, count }, kNullLocation, TypeLayout::C);
    return (Type*)tuple - *module.types;
}

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
    global_->anonymous = true;
    module.globalOrder.push(module.arena, global_ - *module.arena);
    return global_;
}

/*
 * The slots of one compiler-built table.
 *
 * Filled by slot number rather than in write order, because what goes in a slot becomes available in
 * whatever order the things it names get built - a descriptor's lifecycle glue is generated after
 * its numbers are known, and a witness's superclasses after its methods. So the list is sized up
 * front and written into, which also means an unset slot is a null of the right kind rather than a
 * gap.
 *
 * Nothing here is bytes. A slot names a function or holds a number, and turning either into storage
 * is the backend's - see TableCell.
 */
struct TableBuilder {
    TableBuilder(Module& module, Global& target, U16 slots): module(module), target(target) {
        target.isTable = true;
        target.table.reserve(module.arena, slots);

        for(U16 i = 0; i < slots; i++) target.table.push(module.arena, TableSlot {});
    }

    void put(U16 slot, TableSlot value) {
        // A slot outside the numbering is a compiler bug rather than a program error, and pushing
        // one would silently make the table longer than the shape its readers were compiled against.
        assertTrue(slot < target.table.size());
        target.table.set(*module.arena, slot, value);
    }

    void putU32(U16 slot, U32 value) {
        put(slot, TableSlot::intOf(value));
    }

    // How wide a type is, left as the question rather than the answer - see TableCell::Metric. This
    // is what keeps a descriptor free of any one target's numbers.
    void putMetric(U16 slot, TypePtr type, TypeMetricKind metric) {
        put(slot, TableSlot::metricOf(type, metric));
    }

    // A measurement and a constant in one cell - see TableCell::PackedMetric, which exists because
    // the two halves are decided in different places.
    void putPackedMetric(U16 slot, TypePtr type, TypeMetricKind metric, U16 extra) {
        put(slot, TableSlot::packedMetricOf(type, metric, extra));
    }

    // A null target writes nothing, leaving the zero slot the constructor put there: that is how
    // "this type has no drop" reaches the reader as an empty slot rather than as a missing entry.
    void putFunction(U16 slot, ModulePtr<Function> function) {
        if(!function) return;

        (*module.arena)[function]->used = true;
        put(slot, TableSlot::functionOf(function));
    }

    void putGlobal(U16 slot, ModulePtr<Global> global_) {
        if(!global_) return;

        (*module.arena)[global_]->used = true;
        put(slot, TableSlot::globalOf(global_));
    }

    Module& module;
    Global& target;
};

/*
 * The teardown a type with nothing to run gets.
 *
 * A descriptor's lifecycle slots are never null, which is what lets erased code call them without
 * first testing them: one unconditional indirect call, instead of a load, a comparison and a split
 * block at every drop of a generic value. The flags still record which halves are empty, for a pass
 * that would rather skip the call than make it.
 *
 * One per program rather than one per type: it does nothing, so there is nothing to distinguish.
 */
ModulePtr<Function> emptyTeardown(Module& module, LocationId source) {
    auto& program = module.program;
    if(program.emptyTeardown) return program.emptyTeardown;

    auto& core = *program.core;
    auto function = addAnonymousFunction(core, module.context.addQualifiedName("teardown$none", 13, 1), source);
    function->returnType = core.scalar.unit;
    function->used = true;

    auto name = module.context.addQualifiedName("value", 5, 1);
    function->addArg(core, name, resolvePointerType(core, core.scalar.unit), source);

    ExprResolver resolver(core.context, core, *function);
    resolver.terminate(resolver.emit<InstRet>(source, StringId(), core.scalar.unit, nullptr));

    program.emptyTeardown = function - *core.arena;
    return program.emptyTeardown;
}

} // namespace

/*
 * moveInit - the bulk relocation of one type, as a two-argument function over raw pointers.
 *
 * One shape and one answer: the destination, the source, and the bytes. Generic code calls it
 * without knowing either the type or its size, and each backend compiles the block copy its own way
 * - a `memcpy` natively, a property-by-property rewrite on a managed target - which is the whole
 * reason this is generated resolve IR rather than a descriptor operation each target implements.
 *
 * **There is no authored half.** There used to be: `instance Sink(T)` supplied a relocation for a
 * type that referred to its own address, and an aggregate with such a member relocated by its bytes
 * plus a call per member. It is gone, and doc/spec/core.md records why - the case it existed for
 * could not be written (a class member may not retain the address of a borrowed parameter) and the
 * design routes every instance of it through `@box` instead, which keeps the target's address by
 * moving a pointer.
 *
 * What is left of the three-answer list is two: the bytes, or nothing. "Nothing" is a type that may
 * not be relocated at all, which is reported here rather than represented in the descriptor -
 * whether a body may move a value of an unknown type is a question its *schema* answers, and it is
 * answered before any of this is reached.
 */
ModulePtr<Function> moveInitFor(Module& module, TypePtr type, LocationId source) {
    auto& program = module.program;
    if(!type || isGeneric(*module.types, type)) return nullptr;

    if(auto found = program.moveInitGlue.get(U32(type))) return found.unwrap();

    auto ownership = ownershipOf(module, type);
    // Nothing to relocate: a type that carries no information at all. Asked of the logical shape
    // rather than of a size, so that the answer is the same on every target - whether a `()` needs
    // moving is a fact about `()`, not about how wide it happens to be.
    if(isUnit(*module.types, type)) return nullptr;

    auto global = *module.types;

    /*
     * A type that may not be relocated. Every kind that reaches this is one ownershipOf classifies
     * conservatively because it is not constructible yet - Ref, Region, Map - and the conservative
     * answer is worth keeping: emitting the block copy anyway would turn "adding one of these is a
     * decision" into a silently wrong default.
     */
    if(!ownership.trivialSink) {
        module.context.diagnostics.error("%@ cannot be relocated: it is not TrivialSink"_v, source,
                                         describeType(module.context, global, type));
        return nullptr;
    }

    auto function = addAnonymousFunction(module, derivedName(module, "moveInit$"_v, type), source);
    auto pointer = function - *module.arena;

    // Registered before the body is built, so a type reachable from itself finds the entry rather
    // than generating glue forever - the same arrangement teardownGlueFor relies on.
    *program.moveInitGlue.add(U32(type)).value = pointer;

    function->returnType = module.scalar.unit;
    function->used = true;

    auto pointerType = resolvePointerType(module, type);
    auto to = function->addArg(module, module.context.addQualifiedName("to", 2, 1), pointerType, source);
    auto from = function->addArg(module, module.context.addQualifiedName("from", 4, 1), pointerType, source);

    ExprResolver resolver(module.context, module, *function);

    // The bytes. The block copy rather than a load and a store because the size is a constant here
    // and the type may be an aggregate with no register form at all - and the instruction rather than
    // the library's `copyMemory`, which is a ladder written for a count nobody knows.
    {
        auto bytes = resolver.ref(resolver.emit<InstTypeMetric>(source, StringId(), module.scalar.long_,
                                                                type, TypeMetricKind::Size));
        auto byteType = resolvePointerType(module, module.scalar.unit);

        auto castTo = resolver.ref(resolver.emit<InstUnary>(source, StringId(), byteType, Value::Cast,
                                                            (ModulePtr<Value>)(to - *module.arena)));
        auto castFrom = resolver.ref(resolver.emit<InstUnary>(source, StringId(), byteType, Value::Cast,
                                                              (ModulePtr<Value>)(from - *module.arena)));

        auto copyInst = resolver.create<InstNative>(source, StringId(), module.scalar.unit, NativeOp::CopyMemory);
        copyInst->args.push(module.arena, castTo);
        copyInst->args.push(module.arena, castFrom);
        copyInst->args.push(module.arena, bytes);

        // `from` is dead when this returns, which is the whole of what a relocation is and the one
        // thing separating this copy from `copyInit$`'s identical one. See InstNative::relocates.
        copyInst->relocates = true;
        resolver.append(copyInst);
    }

    resolver.terminate(resolver.emit<InstRet>(source, StringId(), module.scalar.unit, nullptr));
    return pointer;
}

/*
 * copyInit - Implementation-JS-Closure.md part 5.2, and the operation `codegen/js/README.md` gap 4
 * said was missing.
 *
 * Three answers, which is `moveInit`'s list with `Copy` where that one has `Sink`:
 *
 *  - **TrivialCopy**: the bytes, as a real function rather than a flag, because the caller is
 *    generic code that does not know the size. Each backend compiles that block copy its own way -
 *    a `memcpy` natively, a property-by-property duplicate on JS - which is the whole reason this is
 *    generated resolve IR rather than a descriptor operation each target implements.
 *  - **An authored `Copy`**: an adapter, and unlike `Sink` it needs one. `fn sink(&to: a, ->from: a)`
 *    is already two addresses and a unit result, so a slot can name it directly; `fn copy(from: a)
 *    -> a` *returns* the duplicate, and a slot has to be entered with the destination. So the
 *    adapter is the call plus the write, and it is one function per type rather than a shape at
 *    every erased site.
 *  - **Neither**: null, and that is the language rather than a gap. A concrete duplicate of a
 *    non-TrivialCopy type with no authored `Copy` does not exist either - `copyValue` emits a *move*
 *    for one - so an erased body only reaches this slot when its own context declared `Copy(a)` or
 *    `TrivialCopy(a)`, and in both of those the answer above is a real function. The constraint is
 *    reported where it is stated, during context construction, rather than at the write.
 *
 * No structural recursion, and that is the difference from moveInitFor worth stating: an aggregate
 * whose members are all TrivialCopy is itself TrivialCopy, and one whose members are not cannot be
 * duplicated at all without an authored `Copy` that says how. There is no fourth case where the
 * bytes plus a call per member is the answer.
 */
ModulePtr<Function> copyInitFor(Module& module, TypePtr type, LocationId source) {
    auto& program = module.program;
    if(!type || isGeneric(*module.types, type)) return nullptr;

    if(auto found = program.copyInitGlue.get(U32(type))) return found.unwrap();

    // Nothing to duplicate, on the same terms as moveInitFor: a fact about `()` rather than about
    // how wide it happens to be on the target that asked.
    if(isUnit(*module.types, type)) return nullptr;

    auto ownership = ownershipOf(module, type);
    if(!ownership.trivialCopy && !ownership.authoredCopy) return nullptr;

    auto function = addAnonymousFunction(module, derivedName(module, "copyInit$"_v, type), source);
    auto pointer = function - *module.arena;

    // Registered before the body, so a type reachable from itself finds the entry rather than
    // generating glue forever - the same arrangement moveInitFor and teardownGlueFor rely on.
    *program.copyInitGlue.add(U32(type)).value = pointer;

    function->returnType = module.scalar.unit;
    function->used = true;

    auto pointerType = resolvePointerType(module, type);
    auto to = function->addArg(module, module.context.addQualifiedName("to", 2, 1), pointerType, source);
    auto from = function->addArg(module, module.context.addQualifiedName("from", 4, 1), pointerType, source);

    ExprResolver resolver(module.context, module, *function);
    auto toValue = (ModulePtr<Value>)(to - *module.arena);
    auto fromValue = (ModulePtr<Value>)(from - *module.arena);

    /*
     * TrivialCopy first, which is what the three-way list above always meant and did not say.
     *
     * The two are not exclusive. An authored `Sink` clears `trivialSink` - a type that refers to its
     * own address cannot be relocated by its bytes, so writing the instance *is* the statement that
     * the structural answer is wrong - but an authored `Copy` deliberately does not clear
     * `trivialCopy`, because duplicating bytes that may be duplicated is never wrong. So a type can
     * have both, and asking for the adapter first picked the call over the block copy for every one
     * of them.
     *
     * That is reachable from ordinary library code: `instance (TrivialCopy(a)) Copy(a)` is what
     * makes `Copy(a)` mean "this can be duplicated" for a container to constrain its elements by,
     * and with it every scalar's `copyInit$` became a call to a generated identity function. Slower
     * everywhere, and wrong on the erased path, where a descriptor slot holding a call rather than a
     * block copy is what `Erased.yana`'s "incorrect argument type to call" was.
     *
     * `TrivialCopy` is the stronger fact and the cheaper answer, so it wins. The adapter is for the
     * case it exists for: a type whose bytes are not its whole story.
     */
    if(ownership.authoredCopy && !ownership.trivialCopy) {
        auto implementation = instanceImplementation(module, module.coreClasses.copy, type, source);

        if(implementation) {
            /*
             * `*to = copy(*from)`.
             *
             * The read is what bridges the two conventions, and it is not a formality on the target
             * this glue is generated for. A slot hands over *storage*; `copy` declares `from: a` and
             * wants the value. Natively those coincide - a memory type's argument is the address of
             * the caller's storage, which is exactly what this frame was handed - and this used to
             * pass the pointer straight through on the strength of that. On a managed target they
             * do not coincide at all: a `%T` whose pointee is not a host object is a **box**, so
             * what the instance received was the box rather than what was in it. `Copy(String)` on
             * that target is the identity (a host string has no storage to duplicate), which made
             * the whole of `copyInit$String` `to.$v = from` - the descriptor's copy slot writing a
             * box into a string-shaped hole, and every later read of it garbage.
             *
             * The same bridge `closureReleaseFor` writes for the same reason, and it costs nothing
             * where the two conventions did coincide: a load of a place handed to a by-reference
             * parameter lowers back to the address it came from.
             */
            auto duplicate = resolver.create<InstCall>(source, StringId(), type, implementation);
            duplicate->args.push(module.arena, resolver.load(Place::atPointer(fromValue), source));
            resolver.append(duplicate);

            resolver.initialize(Place::atPointer(toValue), resolver.ref(duplicate), source);
        }

        resolver.terminate(resolver.emit<InstRet>(source, StringId(), module.scalar.unit, nullptr));
        return pointer;
    }

    /*
     * The bytes, through the block copy for the reason moveInitFor gives: the size is a constant
     * here and the type may be an aggregate with no register form at all. What each backend makes of
     * it is its own business - on JS that is genBlockCopy's structural duplicate rather than
     * anything that would alias, and on amd64 it is the unrolled expansion, which for a constant
     * this size is a handful of vector transfers rather than a `rep movsb`.
     */
    auto bytes = resolver.ref(resolver.emit<InstTypeMetric>(source, StringId(), module.scalar.long_,
                                                            type, TypeMetricKind::Size));
    auto byteType = resolvePointerType(module, module.scalar.unit);

    auto castTo = resolver.ref(resolver.emit<InstUnary>(source, StringId(), byteType, Value::Cast, toValue));
    auto castFrom = resolver.ref(resolver.emit<InstUnary>(source, StringId(), byteType, Value::Cast, fromValue));

    auto copyInst = resolver.create<InstNative>(source, StringId(), module.scalar.unit, NativeOp::CopyMemory);
    copyInst->args.push(module.arena, castTo);
    copyInst->args.push(module.arena, castFrom);
    copyInst->args.push(module.arena, bytes);
    resolver.append(copyInst);

    resolver.terminate(resolver.emit<InstRet>(source, StringId(), module.scalar.unit, nullptr));
    return pointer;
}

/*
 * What relocating a value of this type runs, or null when relocating it is a block copy.
 *
 * The distinction moveInitFor cannot make, because a descriptor slot has to name *some* function:
 * an already-resolved move of a concrete type emits the copy itself and needs to know only whether
 * there is a call to make instead.
 */
/*
 * Whether a place can be addressed with constant offsets.
 *
 * Lowering walks a projection path accumulating byte offsets read off each type it passes through,
 * and a generic aggregate has none: `Pair(a, b).second` sits at an offset that depends on what `a`
 * turned out to be, and a body compiled once for every `a` has no constant to use. What it needs is
 * the composite descriptor Implementation-Generics.md part 4 calls `reprOps`, whose "scoped
 * constructor-content projection" is exactly this.
 *
 * Until that exists, a body that projects into a generic type specializes instead. Depending on the
 * declaration's own offsets happening to be right - which they are whenever every field before the
 * projected one has a size independent of the arguments - would be an accident rather than a rule.
 *
 * ## What building part 4 turned up, when it was tried
 *
 * Counted over both suites, the declining steps are 2028 Downcasts into a generic record, 51
 * discriminant reads of one, and 19 fields of a generic tuple - which reads as "the Downcast is the
 * whole problem" and is wrong twice over. A walk stops at its first declining step, so every field
 * behind an already-declining Downcast went uncounted: a payload offset carried in the descriptor
 * moved the corpus from 112 declining functions to 111, and re-measuring behind it reported 2401
 * field steps where the first pass had seen 19.
 *
 * It is inert for a second reason too. A `match` reads the discriminant before it projects, so an
 * accepted Downcast into a *sum* still declines one step later; and a Downcast into a record whose
 * declaration is `Single` adds zero on every target, which is a constant this body already knows.
 * There is no case in between for a descriptor slot to serve.
 *
 * So the field offset is the half that pays, and three things block it:
 *
 *  - a field declared `@bits(n)` has no byte offset at all - it is a bit range in a word shared with
 *    its neighbours, which is `Run`'s `capacity`/`ownsHeap` pair and `Array`'s `length`. Visible in
 *    the *type*, so this walk can decline it without knowing any layout;
 *  - a parameter that roots a returned borrow - `values(return self: Flat(a)) -> %a = self.items` -
 *    miscompiles when erased, and did so before any of this: the projection decline is what has been
 *    keeping such a body out of the erased path. Six library fixtures;
 *  - and one more in `Digest`'s `padFinal`, not yet characterized. It survives with the optimizer
 *    off, so it is in lowering rather than in `compiler/opt`, and it reproduces from none of the
 *    record shape, the fixed-array field taken as a slice, or the class dispatch on a borrow of a
 *    generic field, each tried on its own.
 *
 * Implicit co-packing needs a layout guarantee rather than a decline, because whether a `Bool` field
 * shares a word is the *target's* answer and this stage has none: the shape that works is to mark
 * the substituted type where the environment is built and have `computeTuple` decline to pack it,
 * which is what TypeDescFlags::CanonicalRepr is reserved for.
 */
static bool lowerablePlace(Module& module, Function& owner, const Place& place) {
    auto global = *module.types;
    auto projections = place.projections;
    if(projections.isEmpty()) return true;

    auto count = projections.size();
    auto lowerable = true;

    walkPlace(module, owner, place, [&](const PlaceStep& step) {
        auto last = step.at + 1 == count;

        /*
         * A constrained field, which is the one projection whose owner is *meant* to be a type this
         * body cannot see - it is reached by calling the witness the caller passed rather than by
         * adding an offset, so the generic-owner test below does not apply to it.
         *
         * Only as the last step. A path continuing through a property would need the read's result
         * to be an address that the rest of the path could project into, and what a witness hands
         * back is a value in storage this frame provided; the address of that storage names a copy,
         * so a write through it would go nowhere. Such a body specializes, where the whole path
         * becomes ordinary field access.
         */
        if(step.kind == ProjectionKind::Property) {
            lowerable = last;
            return false;
        }

        /*
         * `xs[i]` on a `[T *n]` whose *count* is what this body cannot see -
         * Implementation-Const-Generics.md §4.1.
         *
         * The step is `base + i * strideof T`, and neither half of that mentions `n`: the base is
         * the owner's own address and the stride is the element's, which is a constant here or a
         * descriptor read. So the count being a variable is not a reason to decline - it is a
         * reason the *bounds check* reads a slot, which `TypeMetricKind::Count` already does.
         *
         * The element is still asked about, and that is the case the general rule below is for: an
         * `[a *n]` has no stride here either, exactly as `[a *4]` did not.
         */
        if(step.kind == ProjectionKind::Index && step.owner && !step.broken &&
           global[step.owner]->kind == Type::Array &&
           !isGeneric(global, ((ArrayType*)global[step.owner])->content)) {
            return true;
        }

        /*
         * Every other step is an offset read off the owner's declaration, so an owner this body
         * cannot see the shape of has no offset to read.
         */
        if(!step.owner || step.broken || isGeneric(global, step.owner)) {
            lowerable = false;
            return false;
        }

        return true;
    });

    return lowerable;
}

static bool lowerablePlaces(Module& module, Function& owner, const Value& inst) {
    auto ok = true;
    eachPlace(inst, [&](const Place& place) { ok = lowerablePlace(module, owner, place) && ok; });

    /*
     * And the places an aggregate's components are, which `eachPlace` does not report.
     *
     * It reports the value being built with *no* projection - the prefix every component shares,
     * which is what alias analysis wants and is exactly the wrong thing here: an empty path is
     * trivially lowerable, so a construction inside a generic body looked addressable while the
     * fields it writes were not. `Just(value)` was the shape - the erased body was accepted, its
     * payload store took the declaration's own offset of zero, and the discriminant written in
     * front of it was overwritten by the payload.
     *
     * Which is also why this is a rule about the *path* rather than a new gap: the offsets those
     * stores need are the composite descriptor's, and until `reprOps` exists such a body
     * specializes, exactly as it did when the components were separate stores.
     */
    if(inst.kind == Value::Aggregate) {
        auto& aggregate = (InstAggregate&)inst;

        eachWrittenComponent(*module.arena, module.arena, aggregate,
                             [&](Place place, ModulePtr<Value>, Size) {
            ok = lowerablePlace(module, owner, place) && ok;
        });
    }

    return ok;
}

/*
 * The erased entry point of one class method.
 *
 * A generic body that deferred a dispatch knows only the class *signature*, written over the class's
 * own type variables - so it calls with the shape that signature has, not the shape the instance's
 * implementation has. Those differ exactly where the signature is generic: an argument declared `a`
 * arrives as an address whatever `a` turned out to be, and a result declared `a` goes back through
 * caller storage for the same reason.
 *
 * The thunk is the adapter between the two. It takes that erased shape, reads the concrete values
 * out of it, calls the implementation the ordinary way - which expands an intrinsic exactly as any
 * call site does, so a specialized `<` on Int stays one `cmp` behind the pointer - and writes the
 * result back where the caller asked.
 */
static ModulePtr<Function> erasedThunkFor(Module& module, GlobalPtr<TypeClass> typeClass,
                                          const InstanceMatch& match, Buffer<TypePtr> classArgs,
                                          U16 index, LocationId source) {
    auto& context = module.context;
    auto global = *module.types;
    auto local = *module.arena;

    auto entry = global[typeClass]->functions.get(global, index);
    if(!entry.fun) return nullptr;

    auto signature = local[entry.fun];

    // The method index rather than only the name, because a class may declare one name at two
    // arities - `Num` declares both the binary and the unary `-` - and a thunk per method needs a
    // symbol per method. Everything downstream is keyed by name, so a collision here would silently
    // merge two functions rather than being reported.
    StringBuilder text;
    text << "thunk$" << context.findName(global[typeClass]->name) << '(';
    describeTypes(context, global, classArgs, text);
    text << ")." << context.findName(entry.name) << '#';
    text.appendValue(U32(index));

    auto function = addAnonymousFunction(module, builtName(context, text), source);
    auto pointer = function - local;
    function->returnType = module.scalar.unit;
    function->used = true;

    /*
     * The result the caller allocated storage for, when the signature returns something whose size
     * the caller could not know. Declared before the arguments so that the erased shape is the same
     * one an unspecialized generic function has - hidden storage first, then what was written.
     *
     * `isMemoryType` and not `isGeneric`, and the two differ in both directions. A signature
     * returning `&a` is generic and is nonetheless one pointer whatever `a` is; one returning a
     * concrete record is not generic and is nonetheless too big to come back in a register. The
     * call site's rule is the size one - lower.cpp reads `isMemoryType(callee->returnType)` off this
     * same signature - so this has to be the same question or the two disagree about the shape.
     *
     * `Index.get` is where they first did. `-> &v` gave a thunk with a hidden result parameter and a
     * call site that expected a returned pointer, so the arguments landed one position over and the
     * borrow came back as whatever had been in the result register.
     */
    auto concreteResult = substituteType(module, signature->returnType, classArgs, source);
    auto erasedResult = isMemoryType(global, signature->returnType);
    Arg* resultArg = nullptr;

    if(erasedResult) {
        resultArg = function->addArg(module, context.addQualifiedName("result", 6, 1),
                                     resolvePointerType(module, concreteResult), source);
    } else {
        function->returnType = concreteResult;
    }

    SmallArray<Arg*, 8> parameters;

    // One bit per parameter: whether the erased signature passes it by address. Sized before the
    // walk because the class function's arity is what it is, and set as each one is classified.
    IndexSet byAddress;
    byAddress.reset(signature->args.size());

    for(auto argPointer: signature->args.contents(local)) {
        auto declared = local[argPointer];
        auto concrete = substituteType(module, declared->type, classArgs, source);

        /*
         * A `&` parameter is an address in both worlds, so there is nothing to adapt and nothing to
         * load - which is the same statement emitGenericDispatch makes from the caller's side.
         *
         * What it needs instead is the convention and the slot naming it. The thunk goes on to call
         * the implementation the ordinary way, and the implementation declared this parameter `&`,
         * so the argument has to be a place the borrow can be rooted in; a parameter declared here
         * as a bare value is not one, and `borrowArgument` refuses it with *"a `&` argument must
         * name storage that can be written back to"*. That is exactly the shape `bindFunctionArgs`
         * gives an authored body's `&` parameter and `defineInstanceMethod` gives a generated one's,
         * and this was the third place that needed it.
         *
         * Before the generic test rather than beside it, because the two answers differ for `&x: a`:
         * a mutable borrow of a type variable is already an address, so making it a *pointer* to the
         * concrete type would put a second indirection in front of an implementation expecting the
         * first. `&` decides the shape, whatever it is a borrow of.
         */
        if(declared->isMutableBorrow()) {
            auto created = function->addArg(module, declared->name, concrete, source);
            created->convention = declared->convention;
            created->loan = declared->loan;

            function->addLocal(module, concrete, declared->name, (ModulePtr<Value>)(created - local),
                               ast::BindType::Ref, true);

            byAddress.set(parameters.size(), false);
            parameters.push(created);
            continue;
        }

        auto erased = isGeneric(global, declared->type);

        byAddress.set(parameters.size(), erased);
        parameters.push(function->addArg(module, declared->name,
                                         erased ? resolvePointerType(module, concrete) : concrete, source));
    }

    ExprResolver resolver(context, module, *function);
    ArgList args;

    for(Size i = 0; i < parameters.size(); i++) {
        auto value = (ModulePtr<Value>)(parameters[i] - local);
        args.push(byAddress[i] ? resolver.load(Place::atPointer(value), source) : value);
    }

    TypeList instanceArgs;
    for(auto arg: match.args) instanceArgs.push(arg);

    auto errors = context.diagnostics.errorCount();
    auto result = resolver.emitInstanceCall(module, match.instance, toBuffer(instanceArgs), index,
                                            toBuffer(args), source);

    if(context.diagnostics.errorCount() != errors) return nullptr;

    if(erasedResult) {
        // The storage arrived uninitialized, so this is an Init rather than an Assign: there is no
        // old value in it for anything to have to release.
        if(result) {
            resolver.initialize(Place::atPointer((ModulePtr<Value>)(resultArg - local)), result, source);
        }

        resolver.terminate(resolver.emit<InstRet>(source, StringId(), module.scalar.unit, nullptr));
    } else {
        resolver.terminate(resolver.emit<InstRet>(source, StringId(), module.scalar.unit, result));
    }

    return pointer;
}

ModulePtr<Global> classWitnessFor(Module& module, GlobalPtr<TypeClass> typeClass, Buffer<TypePtr> args,
                                  LocationId source) {
    auto& context = module.context;
    auto global = *module.types;
    auto& program = module.program;

    for(auto& existing: program.classWitnesses) {
        if(existing.typeClass != typeClass) continue;
        if(sameTypes(toBuffer(existing.args), args)) return existing.witness;
    }

    for(auto arg: args) {
        if(isGeneric(global, arg)) return nullptr;
    }

    auto match = matchInstance(module, typeClass, args);
    if(!match) {
        StringBuilder types;
        describeTypes(context, global, args, types);

        context.diagnostics.error("no instance of %@ for (%@)"_v, source,
                                  context.findName(global[typeClass]->name), types.view());
        return nullptr;
    }

    auto classEnv = global[global[typeClass]->gen];
    auto methodCount = U16(global[typeClass]->functions.size());
    auto superCount = U16(classEnv->classes.size());

    StringBuilder text;
    text << "witness$" << context.findName(global[typeClass]->name) << '(';
    describeTypes(context, global, args, text);
    text << ')';

    auto global_ = addAnonymousGlobal(module, builtName(context, text), source);
    auto pointer = global_ - *module.arena;

    // Registered before the thunks are generated, since one of them can ask for this witness again -
    // a class whose default implementation calls another method of the same class. The entry is
    // built whole and then pushed: generating a thunk can push another witness, and a reference into
    // a list that reallocates under it would be writing into freed storage.
    InternedWitness interned { typeClass, TypeList(), pointer };
    for(auto arg: args) interned.args.push(arg);

    auto entry = program.classWitnesses.size();
    program.classWitnesses.push(::move(interned));

    TableBuilder table(module, *global_, ClassWitnessFields::countFor(methodCount, superCount));
    auto ok = true;

    for(U16 i = 0; i < methodCount; i++) {
        // A signature the instance does not implement leaves a null slot rather than failing the
        // whole witness: a body that never calls that method is unaffected, and one that does was
        // already rejected where the call was written.
        if(!(*module.arena)[match.instance]->functions.get(*module.arena, i)) continue;

        auto thunk = erasedThunkFor(module, typeClass, match, args, i, source);
        if(!thunk) {
            ok = false;
            continue;
        }

        table.putFunction(ClassWitnessFields::method(i), thunk);
    }

    /*
     * The superclasses, each a witness of its own for the types this one was selected at.
     *
     * Always present, never derived on demand: a body reaching one of these has already been
     * compiled to load it from this slot, and the instance it belongs to is not knowable there. That
     * every one of them exists is what checkSuperclasses settles at the declaration - an instance of
     * a class whose superclass has none was rejected long before anything asked for this table.
     */
    U16 superIndex = 0;

    for(auto constraint: classEnv->classes.contents(global)) {
        auto slot = superIndex++;
        if(!constraint.typeClass) continue;

        TypeList concrete;
        for(auto arg: constraint.args.contents(global)) {
            concrete.push(substituteType(module, arg, args, source));
        }

        auto superclass = classWitnessFor(module, constraint.typeClass, toBuffer(concrete), source);
        if(!superclass) {
            ok = false;
            continue;
        }

        table.putGlobal(ClassWitnessFields::super(methodCount, slot), superclass);
    }

    if(!ok) {
        /*
         * The entry has to go with the failure. Interning early is what lets this recurse, but it
         * also means a half-built table is sitting in the cache under a key that is still being
         * asked about: the next request would find it, skip the diagnostic this call just reported,
         * and take the table - whose failed method slot is a null the erased call would jump to.
         *
         * Removing it by the index it was pushed at rather than popping, because generating the
         * thunks pushed entries of their own and those are the successful ones. Nothing removes an
         * entry it did not push, so every index below this one is still what it was.
         */
        program.classWitnesses.remove(entry);
        return nullptr;
    }

    return pointer;
}

/*
 * Where one named field of a concrete owner sits, as the projections that reach it.
 *
 * The same two steps `projectField` and `resolveProperty` take, and here for the third time because
 * this is the third consumer: a single-constructor record *is* its content, reached through a
 * downcast that costs nothing, and the field is a position in the tuple behind it. False when the
 * owner has no such field, which is a caller that was never checked rather than a program error.
 */
static bool propertyPlace(Module& module, TypePtr owner, StringId field, Place& into, TypePtr& fieldType) {
    auto global = *module.types;
    auto content = owner;

    if(owner && global[owner]->kind == Type::Record) {
        auto record = (RecordType*)global[owner];
        if(record->layout != RecordType::Single || record->constructors.isEmpty()) return false;

        into.projections.push(module.arena, Projection { ProjectionKind::Downcast, 0, nullptr });
        content = record->constructors.get(global, 0).content;
    }

    if(!content || global[content]->kind != Type::Tup) return false;

    auto tuple = (TupType*)global[content];
    for(Size i = 0; i < tuple->fields.size(); i++) {
        auto entry = tuple->fields.get(global, i);
        if(entry.name != field) continue;

        into.projections.push(module.arena, Projection { ProjectionKind::Field, U16(i), nullptr });
        fieldType = entry.type;
        return true;
    }

    return false;
}

// The name both accessors are built under, which differs only in the verb. Generated names are not
// addressable in source, so all they have to be is unique and legible in a dump.
static StringId accessorName(Module& module, StringView verb, TypePtr owner, StringId field) {
    auto& context = module.context;

    StringBuilder text;
    text << verb << '$';
    describeType(context, *module.types, owner, text);
    text << '.' << context.findName(field);

    return builtName(context, text);
}

/*
 * `read(owner, out)` - the field's logical value, into storage the caller provided.
 *
 * Both parameters are addresses, which is the erased shape every witness operation has: the body
 * calling this was compiled once and does not know either type's size. What is *inside* is an
 * ordinary field read of a type this function can see, so the widen-and-mask a packed field needs
 * and the plain load an inline one needs are both just what lowering already does with the place.
 */
static ModulePtr<Function> propertyReadThunk(Module& module, TypePtr owner, StringId field,
                                             TypePtr fieldType, LocationId source) {
    auto& context = module.context;
    auto local = *module.arena;

    auto function = addAnonymousFunction(module, accessorName(module, "read"_v, owner, field), source);
    auto pointer = function - local;
    function->returnType = module.scalar.unit;
    function->used = true;

    auto ownerArg = function->addArg(module, context.addQualifiedName("owner", 5, 1),
                                     resolvePointerType(module, owner), source);
    auto outArg = function->addArg(module, context.addQualifiedName("out", 3, 1),
                                   resolvePointerType(module, fieldType), source);

    ExprResolver resolver(context, module, *function);

    auto place = Place::atPointer((ModulePtr<Value>)(ownerArg - local));
    TypePtr found = nullptr;
    if(!propertyPlace(module, owner, field, place, found)) return nullptr;

    // Init rather than Assign: the caller's storage arrived uninitialized, so there is no old value
    // in it for anything to have to release.
    resolver.initialize(Place::atPointer((ModulePtr<Value>)(outArg - local)),
                        resolver.load(place, source), source);
    resolver.terminate(resolver.emit<InstRet>(source, StringId(), module.scalar.unit, nullptr));

    return pointer;
}

/*
 * `set(owner, value)` - commit an owned replacement, releasing what was there.
 *
 * An Assign rather than an Init, which is the whole difference from the read side: the field held a
 * live value and whatever that value owed is owed at this point. The drop pass puts it in.
 */
static ModulePtr<Function> propertySetThunk(Module& module, TypePtr owner, StringId field,
                                            TypePtr fieldType, LocationId source) {
    auto& context = module.context;
    auto local = *module.arena;

    auto function = addAnonymousFunction(module, accessorName(module, "set"_v, owner, field), source);
    auto pointer = function - local;
    function->returnType = module.scalar.unit;
    function->used = true;

    auto ownerArg = function->addArg(module, context.addQualifiedName("owner", 5, 1),
                                     resolvePointerType(module, owner), source);
    auto valueArg = function->addArg(module, context.addQualifiedName("value", 5, 1),
                                     resolvePointerType(module, fieldType), source);

    ExprResolver resolver(context, module, *function);

    auto place = Place::atPointer((ModulePtr<Value>)(ownerArg - local));
    TypePtr found = nullptr;
    if(!propertyPlace(module, owner, field, place, found)) return nullptr;

    auto incoming = resolver.load(Place::atPointer((ModulePtr<Value>)(valueArg - local)), source);
    resolver.assign(place, incoming, source);
    resolver.terminate(resolver.emit<InstRet>(source, StringId(), module.scalar.unit, nullptr));

    return pointer;
}

ModulePtr<Global> propertyWitnessFor(Module& module, TypePtr owner, StringId field, TypePtr result,
                                     LocationId source) {
    auto& context = module.context;
    auto global = *module.types;
    auto& program = module.program;

    if(!owner || isGeneric(global, owner)) return nullptr;

    for(auto& existing: program.propertyWitnesses) {
        if(existing.owner == owner && existing.field == field) return existing.witness;
    }

    Place probe;
    TypePtr fieldType = nullptr;

    if(!propertyPlace(module, owner, field, probe, fieldType)) {
        context.diagnostics.error("%@ has no field %@"_v, source,
                                  describeType(context, global, owner), context.findName(field));
        return nullptr;
    }

    /*
     * The constraint promised a type and the owner has to have exactly it.
     *
     * Exactly rather than convertibly, for the reason Reject.Property.yana states: the body may
     * write back through this, and a conversion would commit into storage of a different shape.
     * Checked here as well as at the call site because this is what the *witness* would be built
     * from, and a mismatch reaching the table would be a silent reinterpretation.
     */
    if(result && !sameType(result, fieldType)) {
        context.diagnostics.error("field %@ of %@ is %@, but the constraint promised %@"_v, source,
                                  context.findName(field), describeType(context, global, owner),
                                  describeType(context, global, fieldType),
                                  describeType(context, global, result));
        return nullptr;
    }

    StringBuilder text;
    text << "property$";
    describeType(context, global, owner, text);
    text << '.' << context.findName(field);

    auto global_ = addAnonymousGlobal(module, builtName(context, text), source);
    auto pointer = global_ - *module.arena;

    // Registered before the accessors are generated, for the reason classWitnessFor gives: building
    // one can ask for another witness, and a reference into a list that reallocated under it would
    // be a write into freed storage.
    auto entry = program.propertyWitnesses.size();
    program.propertyWitnesses.push(InternedProperty {
        .owner = owner, .field = field, .witness = pointer,
    });

    TableBuilder table(module, *global_, PropertyWitnessFields::kCount);

    /*
     * Both types have to be concrete, asked directly rather than through their descriptors.
     *
     * This used to be `typeDescFor(owner) && typeDescFor(field)`, which answered the same question
     * as a side effect of building the two descriptors the table then held. Nothing loaded those
     * slots, so what is left is the question: a caller that is itself generic reaches here with a
     * substituted type that is still a variable, and a witness over one describes no field of
     * anything. genEnvFor's Type case makes the same test through the same null.
     */
    if(!owner || !fieldType || isGeneric(global, owner) || isGeneric(global, fieldType)) {
        program.propertyWitnesses.remove(entry);
        return nullptr;
    }

    auto read = propertyReadThunk(module, owner, field, fieldType, source);
    auto set = propertySetThunk(module, owner, field, fieldType, source);

    if(!read || !set) {
        program.propertyWitnesses.remove(entry);
        return nullptr;
    }

    table.putFunction(PropertyWitnessFields::kRead, read);
    table.putFunction(PropertyWitnessFields::kSet, set);

    return pointer;
}

/*
 * Filling one call site's environment.
 *
 * For each slot of the callee's schema, the caller expresses that slot in its *own* terms - by
 * substituting the type arguments it is calling with - and then answers one question: is what came
 * out concrete, or is it one of my own type variables?
 *
 *  - concrete: store the address of an interned constant. This is part 9's static case, and it is
 *    the whole environment whenever the caller is not itself generic.
 *  - one of mine: copy my own slot across. This is the forwarded case, and it is what makes a chain
 *    of generic calls work - `middle` hands `smaller` the `Ord` witness it was handed itself, and
 *    nobody in the chain except the outermost concrete caller ever knows what the type is.
 *
 * A mix of the two is part 9's third case and needs no separate handling: the plan is per slot, so
 * a call that knows two of its four slots concretely simply has two of each.
 */
static bool fillEnvironment(Module& module, Function& caller, InstGenCall& call, GenEnv& calleeEnv,
                            Buffer<TypePtr> typeArgs) {
    auto global = *module.types;
    auto callerEnv = functionGen(global, caller);
    auto& schema = genSchemaOf(module, calleeEnv);
    auto ok = true;
    auto allConstant = true;

    call.fill.clear();

    for(auto slot: schema.slots.contents(global)) {
        GenSlotFill entry;

        if(slot.kind == GenSlotKind::Type) {
            auto expressed = substituteType(module, slot.type, typeArgs, call.source);

            if(isGeneric(global, expressed)) {
                entry.forwarded = callerEnv ? genTypeSlot(module, *callerEnv, expressed) : maxLimit<U16>;
                allConstant = false;
            } else {
                entry.constant = typeDescFor(module, expressed, call.source);
            }
        } else if(slot.kind == GenSlotKind::Class) {
            TypeList expressed;
            auto anyGeneric = false;

            for(auto arg: slot.args.contents(global)) {
                auto substituted = substituteType(module, arg, typeArgs, call.source);
                anyGeneric = anyGeneric || isGeneric(global, substituted);
                expressed.push(substituted);
            }

            if(anyGeneric) {
                SuperclassSteps supers;
                entry.forwarded = callerEnv
                    ? genWitnessPath(module, *callerEnv, slot.typeClass, toBuffer(expressed), supers)
                    : maxLimit<U16>;

                for(auto step: supers) entry.forwardedSupers.push(module.arena, step);
                allConstant = false;
            } else {
                entry.constant = classWitnessFor(module, slot.typeClass, toBuffer(expressed), call.source);
            }
        } else if(slot.kind == GenSlotKind::Property) {
            auto owner = substituteType(module, slot.type, typeArgs, call.source);

            if(isGeneric(global, owner)) {
                // The caller does not know the owner either, so it hands over the witness it was
                // handed - the same forwarding a class requirement uses, and what makes a chain of
                // generic functions each carrying `a.name` cost one witness rather than one per
                // frame.
                entry.forwarded = callerEnv
                    ? genPropertySlot(module, *callerEnv, owner, slot.name)
                    : maxLimit<U16>;
                allConstant = false;
            } else {
                auto result = substituteType(module, slot.result, typeArgs, call.source);
                entry.constant = propertyWitnessFor(module, owner, slot.name, result, call.source);
            }
        } else if(slot.kind == GenSlotKind::Const) {
            /*
             * A count - Implementation-Const-Generics.md §3.1, and the one slot whose value is not
             * a pointer.
             *
             * The same two cases as every other slot and no third one: the caller either knows the
             * number, or is passing on a count of its own. What differs is only what goes in the
             * cell, which is why the forwarded form still reads the caller's slot number.
             */
            auto expressed = substituteType(module, slot.type, typeArgs, call.source);
            entry.count = true;

            if(isGeneric(global, expressed)) {
                entry.forwarded = callerEnv ? genConstSlot(module, *callerEnv, expressed) : maxLimit<U16>;
                allConstant = false;
                if(!entry.isForwarded()) ok = false;
            } else if(auto written = writtenCount(global, expressed)) {
                entry.value = written.unwrap();
            } else {
                ok = false;
            }
        } else {
            // A function requirement, which needs a FunctionWitness - a witness kind that does not
            // exist yet. The call site falls back to specializing rather than being given a null
            // slot.
            ok = false;
        }

        if(!entry.count && !entry.constant && !entry.isForwarded()) ok = false;
        call.fill.push(module.arena, entry);
    }

    // Every slot concrete is the case worth interning: one constant shared by every call site that
    // supplies the same arguments, rather than a table assembled on each of their frames.
    if(ok && allConstant) {
        call.env = genEnvFor(module, call.callee, typeArgs, call.source);
        if(call.env) call.fill.clear();
    }

    return ok;
}

bool prepareGenericCalls(Program& program) {
    auto local = *program.arena;
    auto global = *program.types;
    auto ok = true;

    /*
     * Which generic bodies actually reach the backend.
     *
     * A call site marks the function it calls, but that function's own calls are only discovered by
     * looking at it - so `middle` being emitted is what makes `smaller` need emitting too. Run to a
     * fixpoint rather than in one pass, since the call graph is not in any particular order and a
     * function may be marked after it has already been walked.
     */
    for(auto changed = true; changed;) {
        changed = false;

        for(auto module: program.modules) {
            for(Size i = 0; i < module->functionOrder.size(); i++) {
                auto function = local[module->functionOrder.get(local, i)];
                if(function->gen && !function->genericallyUsed) continue;

                for(auto blockPointer: function->blocks.contents(local)) {
                    for(auto instruction: local[blockPointer]->instructions(local)) {
                        auto& inst = *local[instruction];
                        if(inst.kind != Value::GenCall) continue;

                        auto& call = (InstGenCall&)inst;
                        if(call.typeClass || !call.callee) continue;

                        auto callee = local[call.callee];
                        if(!callee->gen || callee->genericallyUsed) continue;

                        callee->genericallyUsed = true;
                        callee->used = true;
                        changed = true;
                    }
                }
            }
        }
    }

    for(auto module: program.modules) {
        // Thunks and witnesses are appended while this runs, so the list is walked by index - the
        // same reason resolveModuleBodies and runProgramOwnership do.
        for(Size i = 0; i < module->functionOrder.size(); i++) {
            auto pointer = module->functionOrder.get(local, i);
            auto function = local[pointer];

            // Only a body that will actually be emitted. A generic function every call site
            // specialized has no machine code, so its deferred calls are decided by cloning.
            if(function->gen && !function->genericallyUsed) continue;

            for(auto blockPointer: function->blocks.contents(local)) {
                for(auto instruction: local[blockPointer]->instructions(local)) {
                    auto& inst = *local[instruction];
                    if(inst.kind != Value::GenCall) continue;

                    auto& call = (InstGenCall&)inst;
                    if(call.env) continue;

                    TypeList typeArgs;
                    for(auto arg: call.typeArgs.contents(local)) typeArgs.push(arg);

                    if(call.typeClass) {
                        // A deferred class dispatch reads its witness out of the caller's own
                        // environment. Which slot that is could not be recorded when the call was
                        // emitted, because the context was still collecting requirements.
                        auto env = functionGen(global, *function);
                        SuperclassSteps supers;
                        call.classSlot = env
                            ? genWitnessPath(*module, *env, call.typeClass, toBuffer(typeArgs), supers)
                            : maxLimit<U16>;

                        call.classPath.clear();
                        for(auto step: supers) call.classPath.push(module->arena, step);

                        if(call.classSlot == maxLimit<U16>) {
                            module->context.diagnostics.error("internal: a deferred class call has no witness slot in %@"_v,
                                                              call.source, module->context.findName(function->name));
                            ok = false;
                        }

                        continue;
                    }

                    auto calleeEnv = functionGen(global, *local[call.callee]);
                    if(!calleeEnv) continue;

                    if(!fillEnvironment(*module, *function, call, *calleeEnv, toBuffer(typeArgs))) {
                        module->context.diagnostics.error("internal: %@ cannot be given an environment in %@"_v,
                                                          call.source,
                                                          module->context.findName(local[call.callee]->name),
                                                          module->context.findName(function->name));
                        ok = false;
                    }
                }
            }
        }
    }

    return ok;
}

/*
 * Whether a body can be emitted, and with it every generic body it calls.
 *
 * The recursion is the point rather than an optimization: emitting `middle` means emitting
 * `smaller`, because `middle`'s call to it is a real call that has to land somewhere. So a body is
 * lowerable only if the whole reachable set is, and one function that is not takes the entire chain
 * back to specialization.
 *
 * `visited` is what makes a recursive generic function terminate - it is on the stack, so assuming
 * it lowerable is exactly the right answer while deciding whether it is.
 */
/*
 * A vector whose lane count this body cannot read - Implementation-Const-Generics.md §4.3.
 *
 * `Vec(a, n)` is a *register* type by resolve's own ABI decision (isDirectType), and a register class
 * is chosen from a lane width and a lane count: a body compiled once for every `n` has neither. §4.3
 * says what the erased form would have to be - a loop over `ceil(count * stride / targetBytes)`
 * register-fulls with a masked store - and that is the plan's step 7, which is not built.
 *
 * So such a body specializes, which is always available for a concrete argument list and is the same
 * staging every other gap in this walk uses. The fixed array is *not* here: `[a *n]` is a memory type
 * whose count reaches the IR as `TypeMetricKind::Count`, which is §4.1 and is built.
 *
 * **A generic vector of any shape is one of these, and not only one whose count is a variable.** A
 * `Vec(a)` in a body generic in `a` is the natural form, whose count is the target's vector width
 * over the lane's stride - and an erased body does not know the stride either, so the count is
 * exactly as unreadable as a written variable's. It answered "erasable" until the iteration protocol
 * was the first generic body to hold one.
 */
static bool erasedVectorIn(GlobalBase global, TypePtr type) {
    if(!type || !isGeneric(global, type)) return false;

    switch(global[type]->kind) {
        case Type::Vector:
            return true;
        case Type::Array:
            return erasedVectorIn(global, ((ArrayType*)global[type])->content);
        case Type::Ptr:
            return erasedVectorIn(global, ((PtrType*)global[type])->to);
        case Type::Borrow:
            return erasedVectorIn(global, ((BorrowType*)global[type])->to);
        case Type::Tup: {
            auto tuple = (TupType*)global[type];
            for(Size i = 0; i < tuple->fields.size(); i++) {
                if(erasedVectorIn(global, tuple->fields.get(global, i).type)) return true;
            }

            return false;
        }
        case Type::Record: {
            auto record = (RecordType*)global[type];
            for(auto arg: record->instanceArgs.contents(global)) {
                if(erasedVectorIn(global, arg)) return true;
            }

            return false;
        }
        case Type::Fun: {
            auto function = (FunType*)global[type];
            for(auto arg: function->args.contents(global)) {
                if(erasedVectorIn(global, arg.type)) return true;
            }

            return erasedVectorIn(global, function->result);
        }
        default:
            return false;
    }
}

static bool bodyLowerable(Module& module, ModulePtr<Function> function,
                          SmallArray<ModulePtr<Function>, 16>& visited, U32 depth) {
    auto local = *module.arena;
    auto global = *module.types;
    auto target = local[function];

    // A signature or an intrinsic has no body to emit; the first is not callable at all and the
    // second is generated at each call site, so neither is a candidate.
    if(target->signature || target->intrinsic) return false;
    if(!depth) return false;

    for(auto seen: visited) {
        if(seen == function) return true;
    }

    visited.push(function);

    // A function requirement needs a FunctionWitness, which does not exist yet. Checked on the
    // context rather than on the body, since it is the *contract* that cannot be supplied. Field
    // requirements are supplied - see propertyWitnessFor - and what limits them is what the *body*
    // does with them, which lowerablePlace below decides.
    if(auto env = functionGen(global, *target)) {
        if(env->functions.isNotEmpty()) return false;
    }

    // A const-generic vector in the signature - see erasedVectorIn. Checked on the signature as well
    // as on the body below, because a parameter of one has no lower type at all: it would fail while
    // the function's arguments were being declared, before any instruction was reached.
    if(erasedVectorIn(global, target->returnType)) return false;

    for(auto argPointer: target->args.contents(local)) {
        if(erasedVectorIn(global, local[argPointer]->declaredType())) return false;
    }

    for(auto blockPointer: target->blocks.contents(local)) {
        auto block = local[blockPointer];

        for(auto instruction: block->instructions(local)) {
            auto& inst = *local[instruction];

            // An explicit copy of a value whose type the body cannot see needs the `Copy` witness,
            // which does not exist yet - a class witness holds methods, and `Copy` would have to be
            // reached as one rather than through the descriptor's lifecycle slots.
            if(inst.kind == Value::Copy && isGeneric(global, inst.type)) return false;

            // A vector of unknown width computed anywhere in the body, which is the other half of
            // the signature test above.
            if(erasedVectorIn(global, inst.type)) return false;

            // A projection into a generic aggregate. `Pair(a, b).second` sits at an offset that
            // depends on what `a` turned out to be, and a body compiled once for every `a` has no
            // constant to use - it needs the composite descriptor Implementation-Generics.md part 4
            // calls `reprOps`, whose "scoped constructor-content projection" is exactly this.
            //
            // Until that exists the offsets a generic body would compute are the declaration's,
            // which are only right when every field before the projected one has a size independent
            // of the type arguments. Rather than depend on that accident, such a body specializes.
            if(!lowerablePlaces(module, *target, inst)) return false;

            /*
             * A call through a function value whose *type* the body cannot see.
             *
             * `f: (a) -> a` is compiled once for every `a`, so the body passes `x` the way an erased
             * body passes everything - by address - while whatever the caller put in `f` is a
             * concrete function expecting whatever `a` turned out to be. Adapting between the two is
             * what a `FunctionWitness` is for: it carries the environment *and* the shape the
             * callable was compiled at, which a bare `{code, env}` does not.
             *
             * Until that exists such a body specializes, which is always available for a concrete
             * argument list - the same staging every other gap here uses.
             */
            if(inst.kind == Value::CallDyn && isGeneric(global, ((InstCallDyn&)inst).signature)) {
                return false;
            }

            /*
             * A continuation this body lifted out of itself - see Function::liftedFrom.
             *
             * It names this function's type variables and is specialized alongside it, which is the
             * one thing the erased form cannot do: there is one body here, and the lifted one would
             * have to be one body too, reading slots out of an environment nobody passes it. So a
             * generic function containing a lens call or a `for` loop specializes, which is always
             * available for a concrete argument list and is the same staging every other gap in this
             * walk uses.
             */
            if(inst.kind == Value::Symbol && ((InstSymbol&)inst).callee &&
               local[((InstSymbol&)inst).callee]->liftedFrom == function) {
                return false;
            }

            if(inst.kind != Value::GenCall) continue;

            auto& call = (InstGenCall&)inst;

            // A deferred class dispatch is supplied from this function's own environment, so what
            // it needs is that the requirement is on the context - which requireClass guarantees.
            if(call.typeClass) continue;

            // A call to another generic function means emitting that one too.
            if(!bodyLowerable(module, call.callee, visited, depth - 1)) return false;
        }
    }

    return true;
}

bool genericBodyLowerable(Module& module, ModulePtr<Function> function) {
    SmallArray<ModulePtr<Function>, 16> visited;
    return bodyLowerable(module, function, visited, 16);
}

ModulePtr<Global> genEnvFor(Module& module, ModulePtr<Function> callee, Buffer<TypePtr> args,
                            LocationId source) {
    auto& context = module.context;
    auto global = *module.types;
    auto local = *module.arena;
    auto& program = module.program;

    auto env = functionGen(global, *local[callee]);
    if(!env) return nullptr;

    for(auto& existing: program.genEnvs) {
        if(existing.callee != callee) continue;
        if(sameTypes(toBuffer(existing.args), args)) return existing.env;
    }

    auto& schema = genSchemaOf(module, *env);
    auto slotCount = schema.slots.size();

    StringBuilder text;
    text << "genEnv$" << context.findName(local[callee]->name) << '(';
    describeTypes(context, global, args, text);
    text << ')';

    auto global_ = addAnonymousGlobal(module, builtName(context, text), source);
    auto pointer = global_ - local;

    // Registered before the slots are filled, since building one of them can ask for an environment
    // again - a witness whose implementation is itself generic. Built whole and then pushed, so that
    // a nested request growing the list cannot leave a reference into freed storage.
    InternedEnv interned { callee, TypeList(), pointer };
    for(auto arg: args) interned.args.push(arg);

    auto entry = program.genEnvs.size();
    program.genEnvs.push(::move(interned));

    TableBuilder table(module, *global_, GenEnvFields::countFor(slotCount));
    auto ok = true;

    for(auto slot: schema.slots.contents(global)) {
        auto cell = GenEnvFields::slot(slot.index);

        switch(slot.kind) {
            case GenSlotKind::Type: {
                auto concrete = substituteType(module, slot.type, args, source);
                auto descriptor = typeDescFor(module, concrete, source);

                if(!descriptor) {
                    context.diagnostics.error("%@ cannot be passed to generic code - it is not a concrete type"_v,
                                              source, describeType(context, global, concrete));
                    ok = false;
                    break;
                }

                table.putGlobal(cell, descriptor);
                break;
            }

            case GenSlotKind::Class: {
                TypeList concrete;
                for(auto arg: slot.args.contents(global)) {
                    concrete.push(substituteType(module, arg, args, source));
                }

                auto witness = classWitnessFor(module, slot.typeClass, toBuffer(concrete), source);
                if(!witness) {
                    ok = false;
                    break;
                }

                table.putGlobal(cell, witness);
                break;
            }

            case GenSlotKind::Property: {
                auto owner = substituteType(module, slot.type, args, source);
                auto result = substituteType(module, slot.result, args, source);

                auto witness = propertyWitnessFor(module, owner, slot.name, result, source);
                if(!witness) {
                    ok = false;
                    break;
                }

                table.putGlobal(cell, witness);
                break;
            }

            case GenSlotKind::Const: {
                // The number itself, in the cell - §3.1. There is no descriptor and nothing to
                // intern: a count that reached an interned environment is concrete by construction,
                // since the environment is keyed on the very arguments that supplied it.
                auto concrete = substituteType(module, slot.type, args, source);
                auto written = writtenCount(global, concrete);

                if(!written) {
                    context.diagnostics.error("%@ cannot be passed to generic code - it is not a count this call knows"_v,
                                              source, describeType(context, global, concrete));
                    ok = false;
                    break;
                }

                table.putU32(cell, U32(written.unwrap()));
                break;
            }

            // The one witness kind that does not exist yet. It is a separate constraint entry with
            // its own implementation and is not derivable from a TypeDesc - knowing a type's size
            // grants nothing else, which is part 1's fifth invariant.
            case GenSlotKind::Function:
                context.diagnostics.error("%@ cannot be called generically yet - its function requirement needs a witness, which is not built yet"_v,
                                          source, context.findName(local[callee]->name));
                ok = false;
                break;
        }
    }

    // Interned early so that a slot can ask for this environment again, and un-interned here for
    // the same reason classWitnessFor does: an environment with an unfilled slot is a table whose
    // reader would load a null, and a later request must be told what this one was told rather than
    // handed the wreckage silently. See the note there for why the index is still this entry's.
    if(!ok) {
        program.genEnvs.remove(entry);
        return nullptr;
    }

    return pointer;
}

ModulePtr<Global> typeDescFor(Module& module, TypePtr type, LocationId source) {
    auto& program = module.program;
    if(!type || isGeneric(*module.types, type)) return nullptr;

    if(auto found = program.typeDescs.get(U32(type))) return found.unwrap();

    auto global_ = addAnonymousGlobal(module, derivedName(module, "typeDesc$"_v, type), source);
    auto pointer = global_ - *module.arena;

    // Registered before the lifecycle functions are generated, since generating one for a type
    // reachable from itself asks for this descriptor again.
    *program.typeDescs.add(U32(type)).value = pointer;

    auto ownership = ownershipOf(module, type);

    // Every lifecycle slot holds a callable address, so erased code never has to test one - see
    // emptyTeardown.
    auto orEmpty = [&](ModulePtr<Function> implementation) {
        return implementation ? implementation : emptyTeardown(module, source);
    };

    /*
     * Two layouts, one per target, and nothing in common between them - see NativeTypeDesc and
     * ManagedTypeDesc for what each holds and why.
     *
     * This is the single writer, and the saving is more than the cells it does not write: a slot is
     * filled by *generating* the function that goes in it, so a slot a target does not read is a
     * per-type function that target does not emit. A native build contains no `moveInit$T` or
     * `copyInit$T` for any type; a managed build contains no reclaim glue and no measurements at all.
     */
    if(isJsMode(module.context.settings.mode)) {
        TableBuilder table(module, *global_, ManagedTypeDesc::kCount);

        /*
         * The *entry* rather than the implementation, which is the difference between what a slot
         * can be called with and what the teardown itself declares - see teardownEntry. A slot holds
         * one signature for every type that could fill it, and erased code has an address and
         * nothing else.
         */
        table.putFunction(ManagedTypeDesc::kDrop,
                          orEmpty(teardownEntry(module, type, Teardown::Drop, source)));

        table.putFunction(ManagedTypeDesc::kMoveInit, orEmpty(moveInitFor(module, type, source)));
        table.putFunction(ManagedTypeDesc::kCopyInit, orEmpty(copyInitFor(module, type, source)));

        return pointer;
    }

    TableBuilder table(module, *global_, NativeTypeDesc::kCount);

    /*
     * The three measurements, as questions rather than answers.
     *
     * This is the last thing in resolve that used to know how wide a type was, and it did not have
     * to: `size`, `align` and `stride` are the emitting target's, and a descriptor whose numbers were
     * filled in here would be a native artifact that some other backend then read as though it
     * described its own values. So the slot says *which measurement of which type* and whoever
     * materializes the table answers it - the same trade InstTypeMetric makes for the instruction
     * form, for the same reason.
     */
    table.putMetric(NativeTypeDesc::kSize, type, TypeMetricKind::Size);
    table.putMetric(NativeTypeDesc::kStride, type, TypeMetricKind::Stride);

    // Nothing selects a non-canonical Repr yet, and no type declares that it must keep its address,
    // so the only source of a stable-address requirement is a Repr variant - which is Milestone 8's.
    // The alignment shares this cell, and cannot be written here: it is the emitting target's
    // number where the flags are this pass's. See TableCell::PackedMetric.
    table.putPackedMetric(NativeTypeDesc::kFlags, type, TypeMetricKind::Align,
                          U16(typeDescFlags(ownership, false)));

    // The relocatability check `moveInitFor` performs on the other target, asked directly: a type
    // nothing can move has to be refused on both, and not only on the one that would have gone
    // looking for a function to name.
    if(!ownership.trivialSink) {
        module.context.diagnostics.error("%@ cannot be relocated: it is not TrivialSink"_v, source,
                                         describeType(module.context, *module.types, type));
    }

    // Both halves as one call - see NativeTypeDesc::kTeardown, and teardownBothFor for what happens
    // where a type has two of them.
    table.putFunction(NativeTypeDesc::kTeardown, orEmpty(teardownBothFor(module, type, source)));

    return pointer;
}

/*
 * The closure header.
 *
 * Not interned, and it is the one compiler-built table that is not: the others are keyed by what
 * they describe and shared by everything that describes it, while this one belongs to one lifted
 * function and is emitted at that function's address. Two lambdas capturing the same types are still
 * two lambdas with two entry points, and each needs its own bytes in front of its own.
 *
 * What goes *in* it is interned in the ordinary way, since both halves depend only on the
 * environment's type - which is what makes the header itself sixteen bytes of relocation rather than
 * anything generated.
 */
ModulePtr<Global> closureHeaderFor(Module& module, ModulePtr<Function> lambda, TypePtr envType,
                                   LocationId source) {
    auto function = (*module.arena)[lambda];
    if(function->closureHeader) return function->closureHeader;

    StringBuilder text;
    text << "closure$" << module.context.findName(function->name);
    auto name = builtName(module.context, text);

    auto global_ = addAnonymousGlobal(module, name, source);
    auto pointer = global_ - *module.arena;

    TableBuilder table(module, *global_, ClosureHeaderFields::kCount);
    global_->prefixOf = lambda;

    /*
     * The entry rather than the implementation, for the reason typeDescFor gives: teardownFunValue
     * reaches this through an InstCallDyn with the environment's *address*, which is the same
     * uniform slot ABI a descriptor has and not the convention a teardown declares.
     *
     * `siteTeardown` picks what goes in it: the merged teardown natively, the drop half alone on a
     * managed target. What this frame is, though, is the *frame-environment* answer, which is the
     * safe one to start from - a header that never reaches selectStorage releases the captures and
     * leaves the storage alone, and storage nothing decided about is storage in the frame.
     */
    auto teardown = teardownEntry(module, envType, siteTeardown(module), source);
    table.putFunction(ClosureHeaderFields::kTeardown,
                      teardown ? teardown : emptyTeardown(module, source));

    function->closureHeader = pointer;
    return pointer;
}

/*
 * The heap environment's teardown: the captures, and then the storage under them.
 *
 * A wrapper rather than a flag on the environment type's own teardown, because the two callers want
 * different things from the same type - a closure whose environment is in the frame runs exactly the
 * inner one - and because which of them a lambda needs is settled at compile time. The shared
 * teardown a function value goes through never learns the difference: it calls what the header
 * names, which is one slot.
 */
ModulePtr<Function> closureReleaseFor(Module& module, TypePtr envType, LocationId source) {
    auto& program = module.program;
    if(auto found = program.closureRelease.get(U32(envType))) return found.unwrap();

    auto local = *module.arena;

    /*
     * Nothing to release, so no function to name - and the caller's `setClosureRelease` leaves the
     * slot holding the shared empty teardown, which is the answer `opt_closure` recognizes.
     *
     * A wrapper with an empty body would be the same behaviour and not the same *fact*: the reach
     * analysis compares the slot against `program.emptyTeardown` by identity, so a second empty
     * function is a header whose drop cannot be proved to find nothing. On JS that is both halves of
     * the cost - an empty function emitted and a test in front of every drop that reads it.
     *
     * It says `!program.freeHeap` rather than asking the target, because that is the same question:
     * `freeHeap` is a declaration of the native heap file, so a target with no heap of this kind has
     * no allocation under an environment to hand back.
     */
    auto reclaim = teardownImplementation(module, envType, siteTeardown(module), source);
    if(!reclaim && !program.freeHeap) {
        *program.closureRelease.add(U32(envType)).value = ModulePtr<Function>();
        return {};
    }

    auto function = addAnonymousFunction(module, derivedName(module, "closureRelease$"_v, envType), source);
    auto pointer = function - local;
    *program.closureRelease.add(U32(envType)).value = pointer;

    function->returnType = module.scalar.unit;
    function->used = true;

    auto envPointer = resolvePointerType(module, envType);
    auto arg = function->addArg(module, module.context.addQualifiedName("env", 3, 1), envPointer, source);
    auto env = (ModulePtr<Value>)(arg - local);

    ExprResolver resolver(module.context, module, *function);

    if(reclaim) {
        local[reclaim]->used = true;

        // The implementation takes its subject by `->`, and this wrapper was handed the address -
        // so the read is what bridges the slot ABI to the teardown's own. It is the same bridge
        // teardownEntry is, written inline because this function already exists and already has the
        // address; a second entry function here would be one more call for nothing.
        auto inner = resolver.create<InstCall>(source, StringId(), module.scalar.unit, reclaim);
        inner->args.push(module.arena, resolver.load(Place::atPointer(env), source));
        resolver.append(inner);
    }

    if(program.freeHeap) {
        auto free = local[program.freeHeap];
        free->used = true;

        // `freeHeap` is written over `%U8`, so the address is reinterpreted rather than converted -
        // both sides are one machine word and only what the program says it means differs.
        auto expected = free->args.isEmpty() ? envPointer : local[free->args.get(local, 0)]->type;
        auto address = sameType(expected, envPointer)
            ? env : resolver.ref(resolver.emit<InstUnary>(source, StringId(), expected, Value::Cast, env));

        auto release = resolver.create<InstCall>(source, StringId(), module.scalar.unit, program.freeHeap);
        release->args.push(module.arena, address);
        resolver.append(release);
    }

    resolver.terminate(resolver.emit<InstRet>(source, StringId(), module.scalar.unit, nullptr));
    return pointer;
}

void setClosureRelease(Module& module, ModulePtr<Global> header, ModulePtr<Function> reclaim) {
    auto local = *module.arena;
    auto global_ = local[header];
    if(!reclaim) return;

    // Nothing to patch on a managed target: the collector owns the environment's storage, so there
    // is no heap variant to move to and the slot already holds the drop half. See
    // ClosureHeaderFields.
    if(isJsMode(module.context.settings.mode)) return;

    local[reclaim]->used = true;

    // The slot is overwritten rather than a second one appended, which is what a list of positions
    // makes obvious and a list of relocations did not: two entries for one slot would be two
    // addresses for one word, and which of them an emitter wrote would be whichever it saw last.
    global_->table.set(local, ClosureHeaderFields::kTeardown, TableSlot::functionOf(reclaim));
}

/*
 * The label every table slot is measured from - see repr/table.h.
 *
 * Created unconditionally rather than when the first table is built, and that distinction is the
 * whole of why this is a function of its own. A *reader* needs the anchor as much as a writer does,
 * and the two do not coincide: a function value's teardown reads a closure header's slot in every
 * program that has a function value, including one where no lambda captured anything and so no
 * header was ever built. Keying the anchor off table construction left those programs decoding a
 * slot against nothing.
 *
 * Called once, after resolution and before `markProgramReachable` - which is the only window where
 * every global exists and appending one more is still safe. It occupies nothing: a `()`-typed global
 * is zero bytes, so this is a name for a position in the image rather than storage in it.
 *
 * Not created on a target whose tables hold references rather than offsets, since there is nothing
 * there to measure and the symbol would be an export nobody reads.
 */
void ensureImageAnchor(Program& program) {
    if(program.imageAnchor || isJsMode(program.context.settings.mode)) return;

    auto& core = *program.core;
    auto global_ = addAnonymousGlobal(core, program.context.addQualifiedName("image$base", 10, 1),
                                      kNullLocation);

    program.imageAnchor = global_ - *core.arena;
}
