#include "core.h"
#include "intrinsic.h"
#include "simd.h"
#include "name.h"
#include "generic.h"
#include "witness.h"
#include "../parse/parser.h"

/*
 * Core's declarations.
 *
 * Everything here that can be written in the language is written in the language, and read by
 * the same parser and the same declaration passes a user module goes through. The compiler only
 * supplies what the language cannot express about itself: the five primitive types, and the
 * bodies of their class instances.
 *
 * Operators are declared with the fixities they have everywhere else, and the classes are the
 * ones Design.md names. `and`, `or`, `xor` and `not` belong to exactly one class - `Bitwise`, which
 * `Integral` has as a superclass - so a `Bool`'s and an integer's are the same name reaching the
 * same declaration, and the two differ only in the instruction the instance emits. `!`, `&&` and
 * `||` are not there at all: they ask `Truth` and answer `Bool`, and are plain functions whose
 * whole definition is the hook attached below.
 *
 * The same is true of the two things the resolver used to do by itself. A literal is a call to
 * `FromInt`/`FromDecimal` and an implicit conversion is a call to `Widen`, so "what does `1`
 * mean" and "which conversions happen without being written" are answered by the declarations
 * below rather than by a table of the primitives inside the resolver - and a user type answers
 * them for itself by writing an instance. `default` names the type a literal takes when nothing
 * else decided one.
 */
// Core's source is the files of `lib/Core/`. It used to be one raw string literal in this position,
// which is the only thing about it that has changed: the same parser reads the same declarations,
// and what this file still supplies is the five primitive types and the bodies of their instances.

// A library module an `import` named. Parsed exactly as the prelude is, by the one thing that knows
// what a library module is made of - `ast::LibraryModules`, which both resolvers ask.
ast::ModuleGroup* findLibraryModule(Program& program, StringId name) {
    return program.library.find(program.context, name);
}

/*
 * One of the two modules the compiler builds itself, with the missing-library report attached.
 *
 * Named rather than reported once for the library, because the two failures a caller can act on are
 * different: both missing is a library that was not found, and one missing is a library that is
 * incomplete, and only the second identifies itself.
 */
ast::ModuleGroup* parsePreludeGroup(Program& program, StringView name, bool required) {
    auto& context = program.context;
    auto id = context.addQualifiedName(name.ptr, name.length);
    auto group = program.library.find(context, id);

    // A module every target has, absent, is a broken library and is reported. One that exists only
    // on some targets is a different thing: `Atomic` has a single `.native.yana` file, so on a JS
    // build the selector leaves it with none and the module simply does not exist there (§5.4).
    if(!group && required) {
        context.diagnostics.error("cannot read the standard library module %@ - looked in %@. Pass -lib with the directory holding Core/Core.yana, or set `library` in the project file."_v,
                                  nullptr, toString(name), context.library.directory(context));
    }

    return group;
}

/*
 * `swap` and `exchange`.
 *
 * Both take their places from mutable borrows, which is what lets one declaration cover a local, a
 * field, a global and an element the collection handed back: whatever produced the borrow already
 * answered where the storage is, and the exclusivity check already answered whether two of them may
 * be live at once. `swap(x, x)` is two mutable borrows of one place and is rejected by the rule that
 * was there before either of these existed.
 *
 * **Two elements of one container are the exception, and these two are the only things exempt from
 * it.** The exclusivity check refuses that pair for every other operation, because telling `xs[i]`
 * from `xs[j]` means proving something about the two indices and that is a different analysis - see
 * `sameContainer` in analyze_borrow.cpp. These need no such proof: a swap reads both places before
 * it writes either, so the two naming one place is a no-op rather than a loss, and an exchange has
 * one place to begin with. Being usable on two elements of one container is what they are *for*, and
 * it is why the library had a `swapElements` at all before a subscript could reach a `&` parameter.
 *
 * A self-swap therefore costs three block copies of the same bytes and nothing else. It used to cost
 * three calls to an authored `Sink`, one of them with `to` and `from` naming one place; that class
 * is gone - see doc/spec/core.md - and with it the only thing that could have observed the extra
 * relocations.
 */

/*
 * The mutable borrow a `&` parameter would have made.
 *
 * A generic intrinsic reaches its emitter through expandIntrinsic, which hands over the arguments
 * as the call wrote them - the conventions are applied by emitDirectCall, and a generic signature
 * never goes through it. So the borrow is made here instead, by the same call emitDirectCall would
 * have made.
 *
 * Which is not a formality. The borrow is what puts these operations in front of the borrow
 * checker: `swap(x, x)` is two mutable borrows of one place, and it is rejected by the exclusivity
 * rule that was there before swap existed rather than by anything written for it.
 */
static ModulePtr<Value> exchangedPlace(ExprResolver& resolver, ModulePtr<Value> argument, TypePtr type,
                                       LocationId source) {
    return resolver.borrowArgument(argument, type, source);
}

static ModulePtr<Value> emitSwap(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr,
                                 LocationId source, StringId) {
    /*
     * The exchanged type comes off the argument rather than off the declaration: `swap` returns
     * unit, so the substituted result type says nothing about what is being swapped.
     *
     * Which means reading through a borrow when the argument already is one. `swap(xs[i], xs[j])`
     * resolves each subscript through `getMut`, whose result is a `&a` - so the type off the
     * argument is the reference and not what is being exchanged, and `borrowArgument` was then
     * asked for a borrow of a borrow and answered that its operand names no storage. `exchange` is
     * not affected and needs no such line: its type is the declaration's, which the solve already
     * bound to the pointee.
     */
    auto type = resolver.valueType(args[0]);

    if(type && resolver.global[type]->kind == Type::Borrow) {
        if(auto to = ((BorrowType*)resolver.global[type])->to) type = to;
    }

    auto a = exchangedPlace(resolver, args[0], type, source);
    auto b = exchangedPlace(resolver, args[1], type, source);
    if(!a || !b) return nullptr;

    resolver.emit<InstSwap>(source, StringId(), resolver.module.scalar.unit,
                            Place::inBorrow(a), Place::inBorrow(b), type);
    return nullptr;
}

static ModulePtr<Value> emitExchange(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                     LocationId source, StringId name) {
    auto slot = exchangedPlace(resolver, args[0], type, source);
    if(!slot) return nullptr;

    // And the `->` convention on the incoming value, for the same reason: expandIntrinsic did not
    // apply it, and what goes into the slot has to be a value this operation owns.
    auto incoming = resolver.sinkValue(resolver.convert(args[1], type, source), source);
    if(!incoming) return nullptr;

    auto exchange = resolver.emit<InstExchange>(source, name, type, Place::inBorrow(slot), incoming);
    auto result = resolver.ref(exchange);

    // Storage for what came out, for the reason rootSink gives: a value has no address, so a name
    // bound to this would have nothing to read a field out of. A scalar came out in a register and
    // wants no slot.
    if(isMemoryType(resolver.global, type)) {
        exchange->local = resolver.function.addLocal(resolver.module, type, name, result);
    }

    return result;
}

/*
 * Assembling the module.
 */

static TypePtr addPrimitive(Program& program, Module& module, StringView name, Type* value) {
    auto pointer = value - *program.types;
    auto id = module.context.addQualifiedName(name.ptr, name.length, 1);

    // An integer type is printed by name, so it has to know the one it was declared under.
    if(value->kind == Type::Int) ((IntType*)value)->name = id;

    // The compiler supplies these rather than Core's source, so nothing wrote `pub` on one. They are
    // as public as anything else in Core: `Int` has to be nameable from every module there is.
    value->exported = true;

    module.namedTypes.add(id, pointer);
    return pointer;
}

static TypePtr coreType(Module& module, StringView name) {
    auto found = module.namedTypes.get(Context::nameHash(name));
    assertTrue(found.isJust());
    return found.unwrap();
}

/*
 * The fixed-width integer family.
 *
 * Design.md's `I8`/`U8` through `I64`/`U64`, sitting alongside `Int` and `Long` rather than
 * replacing them: `Int` is the type a bare literal takes and the one ordinary arithmetic is
 * written in, and these are what a program reaches for when the width is part of what it means.
 *
 * They live in Core rather than Native because naming a width is not an unsafe act, and because
 * on every target now - not just the machine ones - a narrow field buys something. A record of
 * `U8`s packs into one JS number where a record of `Int`s cannot, so requiring `import Native`
 * to write one would have made the JS target's best representation reachable only through the
 * raw-pointer module.
 *
 * The declaration is split in two because the classes these types join are written in Core's own
 * source: `defineIntegerTypes` creates the types, which the source may then name, and
 * `defineIntegerInstances` joins them to the classes once that source has been read.
 */

// `bits` is the type's size in memory and the width is the primitive it occupies once loaded, so
// everything below 64 bits arrives in a 32-bit register and only the widest family needs a wider one.
//
// `target` is set for the three whose width the machine picks rather than the language - see
// TargetInt. `bits` is then the largest it may be, and the width class is the target's word for the
// two that are one and `Int` for `CodeUnit`, whose eight or sixteen bits arrive in a 32-bit register
// either way.
static TypePtr addInteger(Module& module, StringView name, U16 bits, bool isSigned,
                          TargetInt target = TargetInt::None) {
    auto id = module.context.addQualifiedName(name.ptr, name.length, 1);
    auto width = target == TargetInt::Word ? IntType::Word : IntType::widthFor(bits);
    auto type = new (module.types) IntType(bits, width, isSigned, id, nullptr, target);
    type->exported = true;

    auto pointer = (Type*)type - *module.types;
    module.namedTypes.add(id, pointer);
    return pointer;
}

/*
 * A primitive as the shared conversion rules see it - `intrinsic/convert.h`.
 *
 * Every caller below hands this to `intrinsic::conversionRung` or `intrinsic::reinterpretable`
 * rather than deciding anything of its own: which class relates a pair of numbers is the language's
 * statement, both resolve stages generate a ladder from it, and a fixture is compiled by both.
 */
static intrinsic::Numeric numericOf(GlobalBase global, TypePtr type) {
    if(global[type]->kind == Type::Float) {
        return intrinsic::floatOf(((FloatType*)global[type])->width == FloatType::Float ? 32 : 64);
    }

    return ((IntType*)global[type])->numeric();
}

// Which of the two conversion classes relates an ordered pair of the primitives above, or neither.
static intrinsic::Rung conversionRung(GlobalBase global, TypePtr from, TypePtr to) {
    return intrinsic::conversionRung(numericOf(global, from), numericOf(global, to));
}

// One rung, at whichever class the rule named. Nothing where the rule relates the pair by neither,
// which is a pair no conversion is written between at all rather than one that narrows.
static void defineRung(Module& module, TypePtr from, TypePtr to) {
    switch(conversionRung(*module.types, from, to)) {
        case intrinsic::Rung::Widen:
            defineConversion(module, module.coreClasses.widen, "widen"_v, from, to);
            break;
        case intrinsic::Rung::Narrow:
            defineConversion(module, module.coreClasses.narrow, "truncate"_v, from, to);
            break;
        case intrinsic::Rung::None:
            break;
    }
}

static void defineIntegerTypes(Module& module, TypeList& types) {
    struct Width { StringView name; U16 bits; bool isSigned; TargetInt target = TargetInt::None; };
    static const Width widths[] = {
        { "I8"_v, 8, true },   { "U8"_v, 8, false },
        { "I16"_v, 16, true }, { "U16"_v, 16, false },
        { "I32"_v, 32, true }, { "U32"_v, 32, false },
        { "I64"_v, 64, true }, { "U64"_v, 64, false },

        /*
         * The widest integer that is still a machine primitive on every target.
         *
         * 64 bits is not: a JS `number` holds 53 consecutive integers and nothing wider, so `Long`
         * there is a `bigint` - a heap value, four times the retained size of a number, and off the
         * ordinary arithmetic path. `WideInt` is the type that stays a primitive everywhere: a
         * masked 64-bit integer natively, and a plain `number` on JS with codegen/js/wide.cpp
         * supplying the bitwise operators the host stops having above 32 bits.
         *
         * **Signed, and that is a correctness requirement rather than a preference.** Wrapping
         * addition on JS is a comparison and a subtraction, which is sound only while `a + b` is
         * exactly representable - true below 2^53. A signed 53-bit type has operands bounded by
         * 2^52, so the sum always is; an unsigned one reaches 2^54 and would silently round.
         * benchmark/bits53-js/findings.md is where that was measured.
         *
         * A primitive rather than `alias WideInt = @bits(53) I64`, because a refinement dispatches
         * to the instances of the type it refines: the alias would have done its arithmetic at 64
         * bits, as a `bigint` on JS, with a conversion at each end. See `isWideNumber` in
         * codegen/js/type.cpp.
         */
        { "WideInt"_v, 53, true },

        /*
         * `Size` - what an index and a length are carried at, which is the target's word rather than
         * the language's. C's `size_t`, and it exists for the reason C's does.
         *
         * **A primitive of its own, whose width no part of the resolver knows** - Analysis-Modules.md
         * Move 2. It was a *name* for `I64` natively and for `Int` on JS until then, and that was
         * wrong in a way an optimization note does not cover: the ternary asked `isJsMode` and
         * nothing else, so a thirty-two-bit native target got a sixty-four-bit index type - an index
         * wider than the addresses it indexes, in the one type whose whole reason for existing is
         * that an index is added to an address.
         *
         * The cost the old comment here argued against - "a distinct type would need its own `Eq`,
         * `Ord`, `Num`, `Integral` and both conversion ladders" - is this table. Every one of those
         * is generated per entry, so a row costs nothing that the eight fixed widths above did not
         * already cost, and what it buys is that `Size` stops being a second spelling of a width.
         *
         * **Signed**, unlike the counts an owner stores (see `Count` in native.cpp). Those are
         * unsigned because it makes a bounds test one comparison; this one is signed because it is
         * the type an `Int` index widens *into*, and a signed-to-unsigned ladder does not widen. The
         * choice is between one free conversion at every subscript and one extra comparison in
         * `checkBounds`, and the subscript is the hotter of the two by a long way.
         *
         * The bound is 32 to 64 bits and both ends are load-bearing. The low end is what makes
         * `let n: Size = 3000000000` a diagnostic rather than a program that works on one machine;
         * the high end is what stops a `Size` being packed into a narrow field. See IntType::minBits.
         */
        { "Size"_v, IntType::kWordMaxBits, true, TargetInt::Word },

        // `Size` read the other way, which is not a second index type: it is what a bounds test
        // compares at, so that one comparison rejects a negative index as well as one past the end.
        // See the note above on why the index itself is signed, and Implementation-Containers.md
        // §10.2 for the unsigned counts this meets.
        { "USize"_v, IntType::kWordMaxBits, false, TargetInt::Word },

        /*
         * `CodeUnit` - one unit of a string's native encoding - Implementation-Vector.md §9 item 8,
         * Design-Vector §4.6.
         *
         * A UTF-8 byte natively and a UTF-16 unit on JS, which is the same split
         * Implementation-String.md part 3 already makes for `length`: what is uniform across targets
         * is the *complexity class* of an operation over units, not the number of them. A program
         * that names this type is saying "the encoding's own unit", which is what the ASCII scanning
         * family takes and what a `Chunked(String, CodeUnit)` yields - and an ASCII value means the
         * same thing in both, which is the self-synchronizing property that whole family rests on.
         *
         * Abstract for the same reason `Size` is, and unlike `Size` its answer is a genuine platform
         * fact rather than a width: nothing about a machine's registers says a string is UTF-8. What
         * the two share is that resolve has no business knowing which, so both are one mechanism.
         */
        { "CodeUnit"_v, IntType::kCodeUnitMaxBits, false, TargetInt::CodeUnit },
    };

    for(auto& width: widths) {
        types.push(addInteger(module, width.name, width.bits, width.isSigned, width.target));
    }
}

/*
 * Whether a type is one the reinterpretation rule has an answer for.
 *
 * The rule itself is `intrinsic::reinterpretable` - two primitives are the same bits on every target
 * - and what is left here is the two kinds of type this stage has that the rule is not stated over.
 * A **refinement** is `@bits(30) U32` and its own bits are not its storage's, so reinterpreting one
 * is a question about the padding that nothing here can answer; a **`Bool`** is one bit, and
 * reinterpreting one bit is a question about the other seven.
 */
static bool reinterpretsAsPrimitive(GlobalBase global, TypePtr type) {
    if(global[type]->kind == Type::Float) return true;
    if(global[type]->kind != Type::Int) return false;

    auto& integer = *(IntType*)global[type];
    return !integer.canonical && integer.bits > 1;
}

/*
 * The reinterpretation ladder - one `Bitcast` rung per ordered pair of distinct primitives that are
 * the same bits on every target, which is about twenty against the conversion ladder's ninety.
 *
 * Which pairs those are is `intrinsic::reinterpretable`, where the rule and everything it costs are
 * written; same width is the whole of the safety argument, so nothing downstream has to ask whether
 * a `bitcast` fits.
 *
 * **JS declines the pairs that cross between a 64-bit integer and a `Double`.** Not because they
 * cannot be expressed - a `DataView` round trip would - but because a `bigint` going through one is
 * not a reinterpretation in any useful sense: it is a heap value on one side and a `number` on the
 * other, and the cost of the trip is larger than anything a program would have reached for a
 * bitcast to save. That is a judgement about a *target* and is why it is here rather than in the
 * shared rule. The 32-bit `Float`/`I32` pairs *are* generated on both targets, through the scratch
 * typed-array pair codegen/js/inst.cpp emits, because there a bitcast is the only way to see a
 * float's bits at all.
 */
static void defineBitcastLadder(Module& module, TypeList& types) {
    GlobalBase global = *module.types;
    auto onJs = isJsMode(module.context.settings);

    for(Size from = 0; from < types.size(); from++) {
        if(!reinterpretsAsPrimitive(global, types[from])) continue;

        auto source = numericOf(global, types[from]);

        for(Size to = 0; to < types.size(); to++) {
            if(from == to || !reinterpretsAsPrimitive(global, types[to])) continue;

            auto destination = numericOf(global, types[to]);
            if(!intrinsic::reinterpretable(source, destination)) continue;

            auto crossesFloat = source.isFloat() != destination.isFloat();
            auto wide = source.target == intrinsic::TargetWidth::Fixed && source.bits == 64;
            if(onJs && wide && crossesFloat) continue;

            defineBitcast(module, types[from], types[to]);
        }
    }
}

static void defineIntegerInstances(Module& module, TypeList& types) {
    GlobalBase global = *module.types;

    // FromInt first, because Num declares it as a superclass: `1` has to mean something for a
    // type before `+` on it can be told what `x + 1` is.
    for(auto type: types) defineFromInt(module, type);

    for(auto type: types) {
        defineEq(module, type);
        defineOrd(module, type);
        defineNum(module, type);
        defineBitwise(module, type, emitUnary<Value::Not>);
        defineIntegral(module, type);
        defineTruth(module, type, emitTruthy);

        // Every width but the two that have nothing to reverse and the one whose bytes are not all
        // its own - see defineByteSwap, which is where the set is argued.
        if(isByteSwappable(global, type)) defineByteSwap(module, type);

        // And the bit counts at the two widths the machine has them at - see defineBits. The
        // permutations are at the same two and take the same test; see defineBitPermute.
        if(hasBitCounts(global, type)) {
            defineBits(module, type);
            defineBitPermute(module, type);
        }
    }

    // The conversion ladder - one rung per ordered pair of distinct integer types, which is all of
    // them since item 7 merged `Int` into `I32`: there is no second family sitting alongside this
    // one any more, and so no pair to skip on the grounds that another ladder declared it.
    for(Size from = 0; from < types.size(); from++) {
        for(Size to = 0; to < types.size(); to++) {
            if(from == to) continue;
            defineRung(module, types[from], types[to]);
        }
    }
}

void definePreludeTypes(Program& program, Module& core, TypeList& widthTypes) {
    auto& context = program.context;
    auto module = &core;

    program.scalar.error = (Type*)new (program.types) Type(Type::Error) - *program.types;
    program.scalar.unit = (Type*)new (program.types) Type(Type::Unit) - *program.types;

    addPrimitive(program, *module, "Unit"_v, (Type*)(*program.types)[program.scalar.unit]);

    /*
     * The two floating types, named by width - Analysis-Spellings.md items 5 and 7.
     *
     * `Float` and `Double` are `pub alias` declarations in Core's source and the compiler knows
     * nothing about them, which is the whole of what the merge buys: one primitive per width, one
     * name for it in every diagnostic, and no second set of instances to keep in step with the
     * first. The integer half of the same rule is `defineIntegerTypes` below, which is why
     * `scalar.int_` and `scalar.long_` are bound after it rather than declared here.
     */
    program.scalar.float_ = addPrimitive(program, *module, "F32"_v, new (program.types) FloatType(FloatType::Float));
    program.scalar.double_ = addPrimitive(program, *module, "F64"_v, new (program.types) FloatType(FloatType::Double));

    /*
     * `String` - a primitive here rather than a `data` declaration in `Core/Array.yana`, for the reason
     * Type::String gives: the two targets disagree about what a string *is*, and on JS it has to be
     * the bare host value rather than a record wrapping one.
     *
     * Its content type is filled in by `definePreludeNativeTypes`, since the record naming it is
     * declared in Native and no file has been read yet. Nothing asks for a string's layout until
     * lowering.
     */
    program.scalar.string_ = addPrimitive(program, *module, "String"_v, new (program.types) StringType());

    // Before the source is read, so that Core's own declarations may name a width.
    defineIntegerTypes(*module, widthTypes);

    /*
     * `Size` and `USize`, which `defineIntegerTypes` above has already declared - the argument for
     * each of them is beside its row there.
     *
     * Recorded here because the compiler emits code at them without any name in any program to
     * resolve from: an index widens into `Size` at every subscript, and `checkIndexInBounds`
     * compares at `USize` so that one comparison rejects a negative index as well as one past the
     * end. Nothing in this function asks how wide either of them is, which is the whole of Move 2.
     */
    program.scalar.size = coreType(*module, "Size"_v);
    program.scalar.unsignedSize = coreType(*module, "USize"_v);

    /*
     * `Int` and `Long`, which are `I32` and `I64` and are recorded here for the reason `Size` is:
     * the compiler emits code at both without any name in any program to resolve from - an integer
     * literal defaults to one and a shift count is the other.
     *
     * These are bindings to the rungs `defineIntegerTypes` just declared rather than primitives of
     * their own. That is the whole of Analysis-Spellings.md item 7: `Int` was a second 32-bit signed
     * primitive beside `I32`, indistinguishable in every way except which instances it had, and the
     * gap that left was observable - `I32` could not reach a float, because the int/float ladder was
     * built over the scalar four and the width ladder never crossed to a floating type.
     */
    program.scalar.int_ = coreType(*module, "I32"_v);
    program.scalar.long_ = coreType(*module, "I64"_v);

    /*
     * The vector constructors - Design-Vector §2, Implementation-Vector.md §1.4.
     *
     * Two interned names and no declarations, because there is nothing for a declaration to say: a
     * `Vec(Float)` is four lanes or eight depending on the target, so what it *is* comes from
     * `targetVectorBytes` rather than from a body. `resolveApp` recognizes the names; see
     * Program::vecTypeName for what reserving them costs.
     *
     * The signed family beside them is the integer of each lane width, which is what a lane *number*
     * is counted in - see `ScalarTypes::signedLanes` and `maskUpTo`.
     */
    program.vecTypeName = context.addQualifiedName("Vec", 3, 1);
    program.maskTypeName = context.addQualifiedName("Mask", 4, 1);

    /*
     * And the parameter list they are applied through - see Program::vectorGen.
     *
     * Built here rather than parsed, because there is still no declaration: what this replaces is
     * the hand-written arity check and the two messages beside it, not the reason the constructor
     * has no body. The defaults are set directly and the context is marked spent, since there is no
     * written form for `resolveGenDefaults` to read.
     */
    {
        auto env = new (module->types) GenEnv(GenEnv::Record);
        program.vectorGen = env - *module->types;
        env->module = module;
        env->defaultsResolved = true;

        auto lane = new (module->types) GenType(program.vectorGen, context.addUnqualifiedName("a", 1), 0);
        env->types.push(module->types, lane - *module->types);

        auto count = new (module->types) GenType(program.vectorGen, context.addUnqualifiedName("n", 1), 1);
        count->kind = GenKind::Const;
        count->constType = program.scalar.int_;
        count->def = constType(*module, 0, program.scalar.int_);
        env->types.push(module->types, count - *module->types);
    }

    /*
     * `Atomic(a)` - Analysis-Atomics.md §3.1.
     *
     * A third constructor with no declaration, beside `Vec` and `Mask` and for a different reason:
     * what an `Atomic(Int)` *is* could be written down perfectly well, and what cannot be written is
     * that it is not `TrivialCopy` while its content is. So the name is interned here, `resolveApp`
     * recognizes it after the ordinary lookup - a program declaring its own `Atomic` shadows it -
     * and `foldOwnership` states the four properties §3.1 lists.
     *
     * In Core rather than in the `Atomic` module, because a name a *type* resolves by has to be
     * reachable from every module that names the type, and module-scoped type constructors with no
     * declaration are not something the lookup path has. What the `Atomic` module holds is every
     * operation, and each of those is `@platform(native)`.
     */
    program.atomicTypeName = context.addQualifiedName("Atomic", 6, 1);

    {
        auto env = new (module->types) GenEnv(GenEnv::Record);
        program.atomicGen = env - *module->types;
        env->module = module;
        env->defaultsResolved = true;

        auto content = new (module->types) GenType(program.atomicGen, context.addUnqualifiedName("a", 1), 0);
        env->types.push(module->types, content - *module->types);
    }

    program.scalar.signedLanes[0] = coreType(*module, "I8"_v);
    program.scalar.signedLanes[1] = coreType(*module, "I16"_v);
    program.scalar.signedLanes[2] = coreType(*module, "I32"_v);
    program.scalar.signedLanes[3] = coreType(*module, "I64"_v);
}

/*
 * The declarations the compiler itself names, looked up once their files have been read.
 *
 * The middle of the prelude's three hooks, and it is a hook rather than part of the one after it
 * because the *declaration* passes need what it records: an `iter fn` signature is rewritten around
 * `Outcome`, so a signature pass that ran before this found a Core with no exit signal in it. Every
 * line here is a lookup - nothing is generated, nothing is attached - so all it needs is that the
 * records and classes exist, which is true after `passDefine`.
 */
void definePreludeLookups(Program& program, Module& core) {
    auto& context = program.context;
    auto module = &core;

    using intrinsic::Known;

    // The two types the compiler *builds values of* - a condition folds to one of `Bool`'s two
    // constructors and a generated `compare` constructs an `Ordering` - read out of the role table
    // for the reason the classes below are.
    program.scalar.bool_ = (Type*)(*program.types)[knownRecord(program, Known::boolean)] - *program.types;
    program.scalar.ordering = (Type*)(*program.types)[knownRecord(program, Known::ordering)] - *program.types;

    /*
     * The classes the language's own syntax is written in terms of - a literal, an implicit
     * conversion, a condition, `?`'s carrier, and the three points a binding convention compiles
     * to.
     *
     * **Every one of these that `known.def` has a row for is read out of the role table**, filled
     * by `readKnownClassRole` from the `@builtin` the declaration writes. What it replaces is a
     * `classNamed(*module, "Try"_v)` per field: the spelling of every class the compiler dispatches
     * through was written out here, and a library that renamed one tripped an assertion whose
     * message named neither the class nor the rename.
     */
    program.coreClasses.fromInt = knownClass(program, Known::fromInt);
    program.coreClasses.fromDecimal = knownClass(program, Known::fromDecimal);
    program.coreClasses.fromText = knownClass(program, Known::fromText);
    program.coreClasses.widen = knownClass(program, Known::widen);
    program.coreClasses.narrow = knownClass(program, Known::narrow);
    program.coreClasses.truth = knownClass(program, Known::truth);
    program.coreClasses.try_ = knownClass(program, Known::carrier);
    program.coreClasses.rewrap = knownClass(program, Known::rewrap);
    program.coreClasses.eq = knownClass(program, Known::eq);
    program.coreClasses.ord = knownClass(program, Known::ord);
    program.coreClasses.copy = knownClass(program, Known::copy);
    program.coreClasses.reclaim = knownClass(program, Known::reclaim);
    program.coreClasses.drop = knownClass(program, Known::drop);
    program.coreClasses.num = knownClass(program, Known::num);
    program.coreClasses.integral = knownClass(program, Known::integral);
    program.coreClasses.bitwise = knownClass(program, Known::bitwise);
    program.coreClasses.bits = knownClass(program, Known::bits);
    program.coreClasses.bitcast = knownClass(program, Known::bitcast);
    program.coreClasses.viewed = knownClass(program, Known::viewed);

    /*
     * Three more that joined the table when a library declared them - the two ownership
     * classifications and the one a vector joins on demand. `Optional` every one of them, so a
     * library that marks none generates none rather than asserting.
     */
    program.coreClasses.trivialCopy = knownClass(program, Known::trivialCopy);
    program.coreClasses.trivialSink = knownClass(program, Known::trivialSink);
    program.coreClasses.lanewise = knownClass(program, Known::lanewise);

    /*
     * And the ones still found by their spelling, which is the whole of what is left.
     *
     * Not an oversight and not a smaller dependency - each of these is as much a class the compiler
     * dispatches through as the ones above. What they have not got is a `known.def` row, and
     * `known.def`'s tail says why - a row is written the day something in the compiler has to name the
     * class, and until then a spelling here is the smaller statement. They join the table above one
     * at a time as that happens.
     */
    program.coreClasses.enum_ = classNamed(*module, "Enum"_v);
    program.coreClasses.index = classNamed(*module, "Index"_v);
    program.coreClasses.show = classNamed(*module, "Show"_v);

    // §5's two, looked up here for the reason Core's are looked up where they are declared: what
    // asks for them is the resolver rather than a name a program wrote. See CoreClasses.
    program.coreClasses.contiguous = classNamed(*module, "Contiguous"_v);
    program.coreClasses.chunked = classNamed(*module, "Chunked"_v);
    program.coreClasses.writable = classNamed(*module, "Writable"_v);
    program.coreClasses.indexInsert = classNamed(*module, "IndexInsert"_v);

    /*
     * The exit signal's carrier, and the two constructor numbers a lens's lowering reads.
     *
     * Both come out of the row rather than out of a lookup: `known.def`'s `outcome` states two
     * constructors carrying one field each, numbered from zero with `Proceed` first, which is the
     * whole of what the compiler depends on and is a promise the declaration makes by claiming the
     * role. The three spellings this replaced - `Outcome`, `Proceed`, `Exit` - were the last thing
     * in this function that a rename could have moved silently, and one of them was already wrong:
     * `lib/Core/Core.yana` carried the attribute on `Result` rather than on `Outcome`, which nothing
     * noticed while the handle came from the name.
     */
    program.outcomeType = knownRecord(program, Known::outcome);
    program.outcomeProceed = 0;
    program.outcomeExit = 1;
}

/*
 * The two records a *spelling* names - `[T]` and `[K: V]` - looked up between `passDeclare` and
 * `passDefine`, and that position is the whole of this function.
 *
 * They used to sit in `definePreludeLookups` with everything else the compiler names, which runs
 * after `passDefine`. That is late enough for every ordinary module, whose definitions are resolved
 * long afterwards, and one pass too late for a file of the prelude itself: `data Row {xs: [Int]}` in
 * a file of Core was "arrays are not available in this module", because `Program::arrayType` was
 * still null when Core's own records were being defined. `declareRecordType` is what registers the
 * name, and it runs in `passDeclare` - so the earliest correct point is between the two, and that is
 * here.
 *
 * Instantiating a record whose contents are not defined yet is already supported and is not what
 * this risks: `instantiateRecord` registers the instance and defers it to `pendingInstances` when
 * the declaration is not `definitionReady`, which is the same path a mutually recursive pair of
 * ordinary modules takes. What the position buys is that the *spelling* has a meaning while the
 * prelude is still being defined.
 *
 * `Program::arrayType` is also what makes `Array(a)` recognizable *as* the growable array - it is
 * what `sliceOf` asks, so it decides whether the ordinary conversion to a slice exists - and moving
 * it earlier keeps the property that ruling depended on: it is set before any body is resolved.
 */
void definePreludeContainerTypes(Program& program, Module& core) {
    auto& context = program.context;
    auto module = &core;

    program.arrayType = knownRecord(program, intrinsic::Known::array);

    // The window's record through its role rather than by name - `lib/Core/Array.native.yana`'s
    // `@builtin(arrayView) data ArrayView(a)`, `@local` to Core and declared beside the owner. This
    // stage still reads it as the *type* a `[a]` binding folds to; the ownership stage reads it as
    // a representation, and one declaration serves both. It used to be `Native.Flat`, found by
    // name, and moved here because the role is Core's to fill.
    program.sliceType = knownRecord(program, intrinsic::Known::arrayView);

    /*
     * What a native `String` occupies - see Type::String and `computeString` - and what a shared
     * one is made of.
     *
     * Two records, read through the roles they claim: `@builtin(string)` is the owner, `data String
     * {units: Array(CodeUnit)}`, whose bytes a string's are; `@builtin(stringView)` is the view,
     * `{units: %CodeUnit, length: Size}`, which this stage reads the fields of and never lays out.
     * `lib/Core/String.yana` declares both.
     *
     * The owner is a wrapper rather than `Array(CodeUnit)` itself, and the two are the same bytes: a
     * single-field record is its field. What the wrapper buys is that a borrow of it is a *borrow* -
     * see `resolveType`'s Borrow case, which makes a borrow of a container a slice.
     *
     * **Here rather than in `definePreludeNativeTypes`, where it was while the owner was Native's.**
     * That hook runs before Core has declared anything, so a record Core owns is not yet there to be
     * read - which is the same reason the window's row above is here.
     *
     * Read off the table rather than through `knownRecord`, which asserts a `Required` row was
     * claimed: on JS both declarations are `@platform`-excluded, nothing claims either, and null is
     * the answer - a string there is one host value with nothing to lay out.
     */
    auto stringRecord = program.knownRecords[Size(intrinsic::Known::string)];
    auto content = stringRecord ? (Type*)(*program.types)[stringRecord] - *program.types : nullptr;
    ((StringType*)(*program.types)[program.scalar.string_])->content = content;
    program.scalar.stringContent = content;
    program.stringViewType = program.knownRecords[Size(intrinsic::Known::stringView)];

    // The map, on the same terms - Implementation-Map.md §7. One name for both platform rows: the
    // `@platform` selection has already run over the declarations, so whichever of the two `Map`
    // declarations this target kept is the one the literal instantiates.
    auto map = module->namedTypes.get(context.addQualifiedName("Map", 3, 1));
    if(map) program.mapType = (RecordType*)(*program.types)[map.unwrap()] - *program.types;
}

/*
 * Core's own instances, generated once every file of the prelude has been read.
 *
 * The last of the three hooks. Everything here needs a class the source declares, a member
 * signature `passClassSignatures` resolved, or a function `passFunctionSignatures` did - and one
 * pass still to come needs *it*: `deriving (Bitwise)` on a newtype over `I64` forwards to `and` on
 * `I64`, so the integer instances below have to exist before `passInstances` runs. That is the whole
 * of why the prelude is three hooks around one pass sequence rather than six modules in dependency
 * order - Analysis-Modules.md §2.4.
 */
void definePreludeCore(Program& program, Module& core, TypeList& widthTypes) {
    auto& context = program.context;
    auto module = &core;

    // `swap` and `exchange` are declared with no body, like Native's generic intrinsics: there is one
    // operation per type being exchanged, so there is nothing to generate until a call says which.
    attachIntrinsic(*module, intrinsic::Op::swap, emitSwap);
    attachIntrinsic(*module, intrinsic::Op::exchange, emitExchange);

    /*
     * `!`, `&&` and `||` - declared in Core as bodiless plain functions over `Truth`, and defined
     * here.
     *
     * Plain functions rather than class members because what they answer is a `Bool` and what they
     * ask is `Truth`, which is the same question `if x` asks - so `if x` and `if !x` cannot disagree
     * about a value the way they did while these were `Logic` defaults over `not`. The two operands
     * of the pair need not share a type, which a class could not have said.
     *
     * Intrinsics rather than bodies for the reason every operator here is one: reaching them has to
     * cost nothing *without an optimizer having run*, and a source body would be a generic call at
     * every `if` in the program until specialization and inlining had both happened. The deferred
     * form is what lets `&&` emit its right operand under the branch instead of calling a thunk.
     */
    attachIntrinsic(*module, intrinsic::Op::truthNot, emitTruthNot);
    attachDeferredIntrinsic(*module, intrinsic::Op::truthAnd, emitTruthAnd);
    attachDeferredIntrinsic(*module, intrinsic::Op::truthOr, emitTruthOr);

    // The portable vector set, whose declarations are in the source above and whose expansions are
    // simd.cpp's - Design-Vector §3.3.
    defineVectorIntrinsics(*module);

    /*
     * The two floating types, which are all that is left of what used to be a `numeric[]` four -
     * Analysis-Spellings.md item 7.
     *
     * `Int` and `Long` were here as well, and every instance below was generated a second time for
     * them, on the stated grounds that they were "the scalar module's types rather than the
     * fixed-width family's". They are the fixed-width family's now, so `defineIntegerInstances`
     * covers them and this is the floating half alone.
     */
    TypePtr floats[] = { program.scalar.float_, program.scalar.double_ };

    // FromInt comes first because Num declares it as a superclass: `1` has to mean something for
    // a type before `+` on that type can be told what `x + 1` is.
    for(auto type: floats) defineFromInt(*module, type);
    for(auto type: floats) defineFromDecimal(*module, type);

    for(auto type: floats) {
        defineEq(*module, type);
        defineOrd(*module, type);
        defineNum(*module, type);
    }

    /*
     * The same instances over the vector of each are **not** here - simd.cpp generates them where
     * they are asked for, and simd.h says why.
     *
     * The short of it: this loop is what Implementation-Vector.md §9 item 1 describes, and it worked
     * for the four natural-width vectors it covered. Item 1's remaining half is every lane type at
     * every lane count and item 2 is the conversion ladder over the *pairs* of those, which is about
     * seven hundred instances - carried by every program in the language, in an IR arena that holds
     * a program of one to two thousand functions. Generating one when a head is asked for and not
     * before is the same rules at a cost the language can pay.
     */

    defineEq(*module, program.scalar.bool_);

    // A `Bool`'s complement is an `xor` against 1 and not a `Not` instruction: complementing the
    // storage of a one-bit value gives something that is not a Bool, and complementing its value is
    // exactly that xor. Every wider width uses the instruction - see defineNumeric.
    defineBitwise(*module, program.scalar.bool_, emitLogicalNot);
    defineEq(*module, program.scalar.ordering);

    // A Bool is already the answer; every number is asked whether it is non-zero. NaN is therefore
    // truthy, which is worth knowing rather than surprising: the instance says "not zero", and no
    // amount of floating-point special-casing would make `if x` mean something better.
    defineTruth(*module, program.scalar.bool_, emitIdentity);
    for(auto type: floats) defineTruth(*module, type, emitTruthy);

    // The integer family joins the same classes and gets the ladder over its own pairs. Order does
    // not matter against the crossing ladder below any more - the two sets are disjoint now that
    // `Int` is `I32`, where before they overlapped in exactly the `Int`/`Long` rung.
    defineIntegerInstances(*module, widthTypes);

    /*
     * And the rungs that cross between the two families, which are the ones neither ladder above
     * declares.
     *
     * Widening and narrowing are ordinary class operations, so a user type can join either ladder
     * later without the resolver learning anything new about conversion. The ladder is written out
     * rather than searched: one step, never a chain.
     *
     * **The crossing set is the 32- and 64-bit integer widths** - `I32`/`U32` and `I64`/`U64` -
     * and no narrower width. Item 7 closed the signed spelling gap by making `I32` the same type as
     * `Int`; this keeps the unsigned spellings on the same width-based ladder rather than making
     * signedness a second axis of float conversion. The loop below asks the shared conversion rule
     * for every integer in `widthTypes`, so adding a width to that rule cannot leave this stage's
     * generated instances behind.
     */
    for(auto integer: widthTypes) {
        for(auto real: floats) {
            defineRung(*module, integer, real);
            defineRung(*module, real, integer);
        }
    }

    defineRung(*module, program.scalar.float_, program.scalar.double_);
    defineRung(*module, program.scalar.double_, program.scalar.float_);

    /*
     * And the identity rung of the narrowing ladder, one per type, which exists so that `truncate`
     * is *total* over the types it relates.
     *
     * Taking the low bits of a value at its own width is the identity, so each of these emits
     * nothing - and none of them is ever selected implicitly or by `::`, both of which answer
     * `sameType` and return before any instance is looked for. What they are for is portable source.
     *
     * `Size` is `I64` natively and `Int` on JS, so `truncate(length(xs)) :: Int` is a real
     * truncation on one target and the identity on the other. Without a rung for the second case
     * there is *no* spelling of "this length is an `Int` now" that compiles on both, and the program
     * would have to be split by `@platform` over a conversion. §0.1.1 is what made that visible:
     * while `::` narrowed, the identity case went through `sameType` and the question never arose.
     */
    widthTypes.push(program.scalar.float_);
    widthTypes.push(program.scalar.double_);

    for(auto type: widthTypes) defineRung(*module, type, type);

    // And the reinterpretation ladder over everything the ladders above cover, which is why it runs
    // last: the two floating types have just joined this list and the integers were always in it, so
    // this is the whole of what `Bitcast` is generated over.
    defineBitcastLadder(*module, widthTypes);


}

/*
 * The containers - `lib/Core/Array.yana`, `Container.yana` and `Map.yana`.
 *
 * The growable array of Design.md's "Collection types", written in the language over Native rather
 * than generated by the compiler. It used to be a module of its own, and the reason was an ordering
 * one: an array is built out of raw pointers and the heap, and a module could not both import Native
 * and be imported by it. Core imports Native now and Native imports Core, which is a cycle the
 * passes give one meaning to - Analysis-Modules.md §2.2 - so what is left is a file.
 *
 * What this is not, yet, is Implementation-Regions.md part 5's shared `Storage(a)` primitive with a
 * derived Drop - the thing collections are supposed to be written on so that region placement
 * applies to all of them at once. This is one collection with an authored Drop, which is the
 * smaller thing that makes the storage decisions of Milestone 6 testable; the primitive belongs
 * with the standard library that does not exist yet.
 */

/*
 * How many entries a map scans before it allocates an index - Implementation-Map.md §2's third row,
 * folded at the call site.
 *
 * The key comes out of the argument's *declared* type rather than out of any value: `%k` at this
 * call is a pointer to whatever `k` was substituted with, exactly as `hostFixedCapacity` reads its
 * element, and a map with nothing in it has no key to be handed one of.
 *
 * Two entry points, because the two targets no longer hold a key in the same place: `scanLimitFor`
 * is handed the entries run and takes the first field of the `Entry` it points at, `scanLimitForKeys`
 * is handed the key run and the pointee *is* the key. Which of the two is a caller's is decided by
 * `@platform` at `scanLimitOf`, and it has to be decided there rather than here: a record key is a
 * record exactly as an entry is, so one function reading `Entry` off the shape would answer 8 for a
 * `Map(Point, Int)` on the target whose run holds bare keys.
 *
 * Three answers and they are §2's: **8** for a machine word, whose hash is one multiply and a shift;
 * **32** for a `String`, whose hash is a walk over its units; and **64** for anything structural,
 * whose hash is that walk plus a fold per field. An erased generic body reaches none of them and
 * takes the largest, which is a slower small map rather than a wrong one - the threshold decides
 * where a scan stops paying, and both sides of it answer the same questions.
 */
template<bool unwrapEntry>
static ModulePtr<Value> emitScanLimit(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                      LocationId source, StringId name) {
    auto global = resolver.global;
    auto pointee = pointeeType(global, resolver.valueType(args[0]));
    TypePtr key = pointee;

    // `Entry(k, v)`'s first field, which is the key. One shape and no search: the argument is the
    // entries run, whose element this map declares two lines above the function that asks.
    if(unwrapEntry) {
        key = nullptr;

        if(pointee && global[pointee]->kind == Type::Record) {
            auto record = (RecordType*)global[pointee];

            if(record->constructors.size()) {
                auto content = record->constructors.get(global, 0).content;

                if(content && global[content]->kind == Type::Tup) {
                    auto& fields = ((TupType*)global[content])->fields;
                    if(fields.size()) key = fields.get(global, 0).type;
                }
            }
        }
    }

    auto kind = key ? global[key]->kind : Type::Error;
    auto limit = kind == Type::Int || kind == Type::Float ? 8 : (kind == Type::String ? 32 : 64);

    return resolver.makeInt(source, type, limit);
}

void definePreludeContainers(Program& program, Module& core) {
    auto& context = program.context;
    auto module = &core;

    // The map's one per-key-type constant - Implementation-Map.md §2 and §4.4. A rule in resolve
    // with every reader folded against it, which is the shape Containers §14 settled for and the
    // only one of that section's three rules that needs nothing new.
    attachIntrinsic(*module, intrinsic::Op::scanLimitFor, emitScanLimit<true>);
    attachIntrinsic(*module, intrinsic::Op::scanLimitForKeys, emitScanLimit<false>);

    /*
     * Recorded for the reason `allocateHeap` is: the compiler emits the call, so there is no name in
     * any program for resolution to start from.
     *
     * Before defineContainerInstances, because that is what makes a subscript check reachable: the
     * generated `get` bodies below are resolved against whatever this holds at the time.
     *
     * **Unconditionally, whatever `-no-checks` said** - Analysis-Modules.md Move 4. This used to sit
     * inside `if(settings.checks)`, so a build with the checks off resolved a *different program*:
     * `emitCheck` had nothing to call and every bounds test was absent from the IR rather than
     * removed from it. The setting is now a lowering decision - `dischargeChecks` in compiler/opt
     * takes the calls out for a target that does not want them - and what that buys is better than
     * one fewer key field: a library built with the checks on links into a program built without
     * them, which is the configuration a shipped library actually wants.
     */
    auto found = module->functions.get(context.addUnqualifiedName("checkCondition", 14));
    program.checkCondition = found ? found.unwrap() : nullptr;

    // And the located form, which `-check-locations` selects instead - see `emitCheck`. Found the
    // same way and kept beside it rather than replacing it, because which of the two a check calls
    // is a decision per *build* and both declarations exist in every one.
    auto located = module->functions.get(context.addUnqualifiedName("checkConditionAt", 16));
    program.checkConditionAt = located ? located.unwrap() : nullptr;

    /*
     * And the arm it branches to, marked as one control does not come back out of.
     *
     * Here rather than in an attribute on the declaration because there is nothing in the source
     * to attach one to that would mean anything: `checkFailed` is `exitProcess(134)` and a
     * `return`, and what makes it final is the kernel rather than the shape of the body. Both
     * targets' spellings are equally final - a status on native, a thrown value on JS - so the
     * fact is about this function rather than about either platform's implementation of it.
     *
     * See `Function::noReturn` for what reads it.
     */
    auto failed = module->functions.get(context.addUnqualifiedName("checkFailed", 11));
    if(failed) (*module->arena)[failed.unwrap()]->noReturn = true;

    auto failedAt = module->functions.get(context.addUnqualifiedName("checkFailedAt", 13));
    if(failedAt) (*module->arena)[failedAt.unwrap()]->noReturn = true;

    // After `arrayType` above, and before this module's own bodies below - several of which
    // subscript, and would reach an instance that does not exist yet.
    defineContainerInstances(*module);
}

/*
 * The text half - `lib/Core/String.yana`, with `Show.yana`, `Format.yana`, `Read.yana` and
 * `Float/` beside it.
 *
 * `String`'s operations, over what Native's reinterpretations hand out -
 * Implementation-Simplification.md §17.
 *
 * Three modules once, and the middle one existed only for the cycle they sat in: what a native
 * string is *made of* is Native's, so handing those two words out belongs behind an import that
 * already means "this is unsafe", while the run those words describe is a container whose
 * declaration has to be implicitly visible because `[a]` is grammar. A module could not be on both
 * sides of that, so there were three. With a cycle permitted there are two, and the unsafe half is
 * where it always belonged - see definePreludeNativeText, and Program::native for why an import of
 * Native is still the one visible unsafe act.
 */

void definePreludeText(Program& program, Module& core) {
    auto& context = program.context;
    auto module = &core;

    /*
     * The three functions a format expression is built out of - Implementation-Storage.md part 8.
     *
     * Recorded for the reason `allocateHeap` is: `"a{x}b"` is resolved by the compiler, which has a
     * chunk list and a set of resolved holes and no call site for name resolution to start from.
     * Everything else about a format is an ordinary call to an ordinary function.
     */
    // By nameHash rather than by interning: two of the three are namespaced under `String`, and a
    // qualified name is keyed by the hash of its whole text - see NameRef::range.
    auto findText = [&](StringView text) -> ModulePtr<Function> {
        auto found = module->functions.get(Context::nameHash(text));
        return found ? found.unwrap() : nullptr;
    };

    program.newString = findText("String.ofCapacity"_v);
    program.pushString = findText("String.push"_v);
    program.formatBound = findText("formatBound"_v);
    program.reserveString = findText("String.reserve"_v);

    // The operator's name, interned rather than looked up: what P2 recognizes is the written shape
    // `place ++= "a{x}b"`, and the function of that name is what every *other* right operand
    // resolves to. See resolvePrecedence.
    program.appendAssign = context.addUnqualifiedName("++=", 3);
    program.negate = context.addUnqualifiedName("-", 1);
}
