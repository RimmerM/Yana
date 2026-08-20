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
 * ones Design.md names. Note that `and`/`or`/`not` appear in both Integral and Logic: an
 * integer's are bitwise and a Bool's are logical, and which is meant is decided by which class
 * has an instance for the operand type rather than by a special case anywhere in the resolver.
 *
 * The same is true of the two things the resolver used to do by itself. A literal is a call to
 * `FromInt`/`FromDecimal` and an implicit conversion is a call to `Widen`, so "what does `1`
 * mean" and "which conversions happen without being written" are answered by the declarations
 * below rather than by a table of the primitives inside the resolver - and a user type answers
 * them for itself by writing an instance. `default` names the type a literal takes when nothing
 * else decided one.
 */
// Core's source is `lib/Core.yana`. It used to be a raw string literal in this position, which is
// the only thing about it that has changed: the same parser reads the same declarations, and what
// this file still supplies is the five primitive types and the bodies of their instances.

static ast::Module* parseLibrarySource(Context& context, StringId id, bool allowSignatures) {
    auto source = context.library.source(context, id);
    if(source.length == 0) return nullptr;

    Lexer lexer(context, context.diagnostics, source, id);
    Parser parser(context, lexer, id);
    parser.allowSignatures = allowSignatures;

    return new ast::Module(parser.parseModule());
}

// A library module an `import` named. `allowSignatures` is off: a declaration with no body means
// something only where the compiler attaches a hook to it, and it attaches them by name to the
// seven modules it builds itself.
ast::Module* findLibraryModule(Context& context, StringId name) {
    return parseLibrarySource(context, name, false);
}

ast::Module* parseLibraryModule(Context& context, StringView name, bool allowSignatures) {
    auto id = context.addQualifiedName(name.ptr, name.length);
    auto ast = parseLibrarySource(context, id, allowSignatures);

    if(!ast) {
        // Named once per missing module rather than once for the library, because the two failures a
        // caller can act on are different: every module missing is a library that was not found, and
        // one module missing is a library that is incomplete, and only the second identifies itself.
        context.diagnostics.error("cannot read the standard library module %@ - looked in %@. Pass -lib with the directory holding Core.yana, or set YANA_LIB."_v,
                                  nullptr, toString(name), context.library.directory(context));
    }

    return ast;
}

/*
 * `swap` and `exchange`.
 *
 * Both take their places from mutable borrows, which is what lets one declaration cover a local, a
 * field, a global and an element the collection handed back: whatever produced the borrow already
 * answered where the storage is, and the exclusivity check already answered whether two of them may
 * be live at once. `swap(x, x)` is two mutable borrows of one place and is rejected by the rule that
 * was there before either of these existed.
 */

// The relocation, on exactly the terms sinkValue records one for a `->`. Asked the same way for the
// same reason: a body that cannot see the type leaves this null and relocates through the caller's
// descriptor instead, and a specialization asks again for the type it turned out to be.
static ModulePtr<Function> relocationFor(ExprResolver& resolver, TypePtr type, LocationId source) {
    auto ownership = ownershipIn(resolver.module, functionGen(resolver.global, resolver.function), type);
    if(ownership.trivialSink) return nullptr;

    return sinkFor(resolver.module, type, source);
}

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
    // The exchanged type comes off the argument rather than off the declaration: `swap` returns
    // unit, so the substituted result type says nothing about what is being swapped.
    auto type = resolver.valueType(args[0]);

    auto a = exchangedPlace(resolver, args[0], type, source);
    auto b = exchangedPlace(resolver, args[1], type, source);
    if(!a || !b) return nullptr;

    auto swap = resolver.emit<InstSwap>(source, StringId(), resolver.module.scalar.unit,
                                        Place::inBorrow(a), Place::inBorrow(b), type);

    swap->sink = relocationFor(resolver, type, source);
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

    exchange->sink = relocationFor(resolver, type, source);

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
static TypePtr addInteger(Module& module, StringView name, U16 bits, bool isSigned) {
    auto id = module.context.addQualifiedName(name.ptr, name.length, 1);
    auto type = new (module.types) IntType(bits, IntType::widthFor(bits), isSigned, id);
    type->exported = true;

    auto pointer = (Type*)type - *module.types;
    module.namedTypes.add(id, pointer);
    return pointer;
}

// Whether a value of `from` fits in `to` without losing anything: more bits, or the same bits
// without a sign to lose. This decides which of the two conversion ladders a pair joins, which
// is the whole of the rule for whether a conversion happens on its own or has to be written.
static bool widens(GlobalBase global, TypePtr from, TypePtr to) {
    return integerRangeFits(*(IntType*)global[from], *(IntType*)global[to]);
}

static void defineIntegerTypes(Module& module, TypeList& types) {
    struct Width { StringView name; U16 bits; bool isSigned; };
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
    };

    for(auto& width: widths) types.push(addInteger(module, width.name, width.bits, width.isSigned));
}

/*
 * How wide a primitive is for the purpose of reinterpreting it, or zero where the question does not
 * apply.
 *
 * The type's *own* bits and not its register's, because that is what decides whether two types hold
 * the same value: `WideInt` is 53 bits in a 64-bit register, so nothing else has its shape and it
 * gets no rung. `Bool` is excluded by the same rule for a better reason - it is one bit, and a
 * reinterpretation of one bit is a question about the other seven that nothing here can answer.
 */
static U16 reinterpretWidth(GlobalBase global, TypePtr type) {
    if(global[type]->kind == Type::Float) {
        return ((FloatType*)global[type])->width == FloatType::Float ? 32 : 64;
    }

    if(global[type]->kind != Type::Int) return 0;

    auto& integer = *(IntType*)global[type];
    return integer.canonical || integer.bits <= 1 ? 0 : integer.bits;
}

/*
 * The reinterpretation ladder - one `Bitcast` rung per ordered pair of distinct primitives of one
 * width, which is about thirty against the conversion ladder's ninety.
 *
 * Same width is the whole of the safety argument, so there is no test anywhere else: nothing
 * downstream has to ask whether a `bitcast` fits, because no instance relating a pair that does not
 * was ever generated.
 *
 * **JS declines the pairs that cross between a 64-bit integer and a `Double`.** Not because they
 * cannot be expressed - a `DataView` round trip would - but because a `bigint` going through one is
 * not a reinterpretation in any useful sense: it is a heap value on one side and a `number` on the
 * other, and the cost of the trip is larger than anything a program would have reached for a
 * bitcast to save. The 32-bit `Float`/`I32` pairs *are* generated on both targets, through the
 * scratch typed-array pair codegen/js/inst.cpp emits, because there a bitcast is the only way to
 * see a float's bits at all.
 */
static void defineBitcastLadder(Module& module, TypeList& types) {
    GlobalBase global = *module.types;
    auto onJs = isJsMode(module.context.settings.mode);

    for(Size from = 0; from < types.size(); from++) {
        auto fromWidth = reinterpretWidth(global, types[from]);
        if(!fromWidth) continue;

        for(Size to = 0; to < types.size(); to++) {
            if(from == to || reinterpretWidth(global, types[to]) != fromWidth) continue;

            auto crossesFloat = (global[types[from]]->kind == Type::Float) !=
                                (global[types[to]]->kind == Type::Float);

            if(onJs && fromWidth == 64 && crossesFloat) continue;

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
        defineIntegral(module, type);
        defineTruth(module, type, emitTruthy);

        // Every width but the two that have nothing to reverse and the one whose bytes are not all
        // its own - see defineEndian, which is where the set is argued.
        if(isByteSwappable(global, type)) defineEndian(module, type);

        // And the bit counts at the two widths the machine has them at - see defineBits.
        if(hasBitCounts(global, type)) defineBits(module, type);
    }

    // The conversion ladder, over these types and the two integer types they sit alongside. The
    // `Int`/`Long` pair already has its rung from the numeric ladder below and is skipped rather
    // than declared twice, which would leave instance selection with two answers to one question.
    auto widthCount = types.size();
    types.push(module.scalar.int_);
    types.push(module.scalar.long_);

    for(Size from = 0; from < types.size(); from++) {
        for(Size to = 0; to < types.size(); to++) {
            if(from == to || (from >= widthCount && to >= widthCount)) continue;

            if(widens(global, types[from], types[to])) {
                defineConversion(module, "Widen"_v, "widen"_v, types[from], types[to]);
            } else {
                defineConversion(module, "Narrow"_v, "truncate"_v, types[from], types[to]);
            }
        }
    }
}

void defineCore(Program& program) {
    auto& context = program.context;

    program.scalar.error = (Type*)new (program.types) Type(Type::Error) - *program.types;
    program.scalar.unit = (Type*)new (program.types) Type(Type::Unit) - *program.types;

    // `swap` and `exchange` are declared with no body, like Native's generic intrinsics: there is one
    // operation per type being exchanged, so there is nothing to generate until a call says which.
    auto ast = parseLibraryModule(context, "Core"_v, true);
    if(!ast) return;

    auto module = program.addModule(ast->name, *ast->region);
    program.core = module;
    program.embeddedAsts.push(ast);

    addPrimitive(program, *module, "Unit"_v, (Type*)(*program.types)[program.scalar.unit]);
    program.scalar.int_ = addPrimitive(program, *module, "Int"_v, new (program.types) IntType(32, IntType::Int, true));
    program.scalar.long_ = addPrimitive(program, *module, "Long"_v, new (program.types) IntType(64, IntType::Long, true));
    program.scalar.float_ = addPrimitive(program, *module, "Float"_v, new (program.types) FloatType(FloatType::Float));
    program.scalar.double_ = addPrimitive(program, *module, "Double"_v, new (program.types) FloatType(FloatType::Double));

    /*
     * `String` - a primitive here rather than a `data` declaration in Collections, for the reason
     * Type::String gives: the two targets disagree about what a string *is*, and on JS it has to be
     * the bare host value rather than a record wrapping one.
     *
     * Its content type is filled in by `defineNative`, since the record naming it is declared there
     * and this runs first. Nothing asks for a string's layout until lowering.
     */
    program.scalar.string_ = addPrimitive(program, *module, "String"_v, new (program.types) StringType());

    // Before the source is read, so that Core's own declarations may name a width.
    TypeList widthTypes;
    defineIntegerTypes(*module, widthTypes);

    /*
     * `Size` - what an index and a length are carried at, which is the target's width rather than
     * the language's. C's `size_t`, and it exists for the reason C's does.
     *
     * A **name for an existing primitive** and not a primitive of its own. A distinct type would
     * need its own `Eq`, `Ord`, `Num`, `Integral` and both conversion ladders, and would then need
     * converting to and from the very type it *is* on each target. What is wanted is the opposite -
     * that `Size` be whichever primitive the target already computes indices in - so this binds a
     * name and stops.
     *
     * Keyed on the target's word width and deliberately not on which backend is running. Those
     * coincide today, and they are not the same question: a thirty-two-bit native target wants a
     * thirty-two-bit `Size` for the same reason JS does, and writing the rule as "js or not" would
     * have to be found and rewritten the day one exists rather than simply being true.
     *
     * **Signed**, unlike the counts an owner stores (see `Count` in native.cpp). Those are unsigned
     * because it makes a bounds test one comparison; this one is signed because it is the type an
     * `Int` index widens *into*, and a signed-to-unsigned ladder does not widen. The choice is
     * between one free conversion at every subscript and one extra comparison in `checkBounds`, and
     * the subscript is the hotter of the two by a long way.
     *
     * Sixty-four bits natively rather than `WideInt`'s fifty-three, which is the point of asking the
     * target rather than picking the portable answer: `WideInt` is a *masked* 64-bit integer here,
     * so every operation on one pays for a width that only JS needs. JS gets `Int`, where a host
     * array's length is a `uint32` by specification and nothing wider can be described anyway.
     *
     * `I64` and not `Long`, which are two distinct primitives of one width here: `I64` is what
     * `Native`'s pointer arithmetic takes, and an index exists to be added to an address. `Int` and
     * not `I32` on the other side, for the mirror reason - `Int` is what a literal defaults to, and
     * a same-width pair does not widen, so an `I32` `Size` would reject `xs[i]` for the ordinary `i`.
     */
    auto sizeType = isJsMode(context.settings.mode) ? program.scalar.int_ : coreType(*module, "I64"_v);
    module->namedTypes.add(context.addQualifiedName("Size", 4, 1), sizeType);
    program.scalar.size = sizeType;

    // `Size` read the other way, which is not a second index type: it is what a bounds test compares
    // at, so that one comparison rejects a negative index as well as one past the end. See the note
    // above on why the index itself is signed, and Implementation-Containers.md §10.2 for the
    // unsigned counts this meets.
    program.scalar.unsignedSize = isJsMode(context.settings.mode) ? coreType(*module, "U32"_v)
                                                                  : coreType(*module, "U64"_v);

    /*
     * `CodeUnit` - one unit of a string's native encoding, target-selected exactly as `Size` is -
     * Implementation-Vector.md §9 item 8, Design-Vector §4.6.
     *
     * A UTF-8 byte natively and a UTF-16 unit on JS, which is the same split
     * Implementation-String.md part 3 already makes for `length`: what is uniform across targets is
     * the *complexity class* of an operation over units, not the number of them. A program that
     * names this type is saying "the encoding's own unit", which is what the ASCII scanning family
     * takes and what a `Chunked(String, CodeUnit)` yields - and an ASCII value means the same thing
     * in both, which is the self-synchronizing property that whole family rests on.
     */
    auto unitType = isJsMode(context.settings.mode) ? coreType(*module, "U16"_v) : coreType(*module, "U8"_v);
    module->namedTypes.add(context.addQualifiedName("CodeUnit", 8, 1), unitType);

    /*
     * `F32` and `F64` - Implementation-Vector.md §9 item 1.
     *
     * Names for `Float` and `Double` and nothing else, on exactly the terms `Size` is a name for
     * `I64`: no type, no instances, no conversion to or from what they are. What they buy is that a
     * signature which names widths can name all of them the same way - `Vec(F32)` beside `Vec(I32)`
     * reads as one family where `Vec(Float)` beside `Vec(I32)` reads as two - and vector code is
     * where that comes up, because a lane width is the thing being said.
     */
    module->namedTypes.add(context.addQualifiedName("F32", 3, 1), program.scalar.float_);
    module->namedTypes.add(context.addQualifiedName("F64", 3, 1), program.scalar.double_);

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

    program.scalar.signedLanes[0] = coreType(*module, "I8"_v);
    program.scalar.signedLanes[1] = coreType(*module, "I16"_v);
    program.scalar.signedLanes[2] = coreType(*module, "I32"_v);
    program.scalar.signedLanes[3] = coreType(*module, "I64"_v);

    resolveModuleDecls(*module, *ast, nullptr);

    attachIntrinsic(*module, "swap"_v, emitSwap);
    attachIntrinsic(*module, "exchange"_v, emitExchange);

    // The portable vector set, whose declarations are in the source above and whose expansions are
    // simd.cpp's - Design-Vector §3.3.
    defineVectorIntrinsics(*module);

    program.scalar.bool_ = coreType(*module, "Bool"_v);
    program.scalar.ordering = coreType(*module, "Ordering"_v);

    TypePtr numeric[] = {
        program.scalar.int_,
        program.scalar.long_,
        program.scalar.float_,
        program.scalar.double_,
    };

    // FromInt comes first because Num declares it as a superclass: `1` has to mean something for
    // a type before `+` on that type can be told what `x + 1` is.
    for(auto type: numeric) defineFromInt(*module, type);

    defineFromDecimal(*module, program.scalar.float_);
    defineFromDecimal(*module, program.scalar.double_);

    for(auto type: numeric) {
        defineEq(*module, type);
        defineOrd(*module, type);
        defineNum(*module, type);
    }

    defineIntegral(*module, program.scalar.int_);
    defineIntegral(*module, program.scalar.long_);

    // And the byte reversal at the two canonical widths, which the loop above cannot reach: `Int`
    // and `Long` are the scalar module's types rather than the fixed-width family's.
    defineEndian(*module, program.scalar.int_);
    defineEndian(*module, program.scalar.long_);

    // The bit counts beside them, at exactly the two widths they are declared over.
    defineBits(*module, program.scalar.int_);
    defineBits(*module, program.scalar.long_);

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
    defineLogic(*module, program.scalar.bool_);
    defineEq(*module, program.scalar.ordering);

    // A Bool is already the answer; every number is asked whether it is non-zero. NaN is therefore
    // truthy, which is worth knowing rather than surprising: the instance says "not zero", and no
    // amount of floating-point special-casing would make `if x` mean something better.
    defineTruth(*module, program.scalar.bool_, emitIdentity);
    for(auto type: numeric) defineTruth(*module, type, emitTruthy);

    // Widening and narrowing are ordinary class operations, so a user type can join either
    // ladder later without the resolver learning anything new about conversion. The ladder is
    // written out rather than searched: one step, never a chain.
    for(Size from = 0; from < 4; from++) {
        for(Size to = 0; to < 4; to++) {
            if(from == to) continue;

            if(from < to) {
                defineConversion(*module, "Widen"_v, "widen"_v, numeric[from], numeric[to]);
            } else {
                defineConversion(*module, "Narrow"_v, "truncate"_v, numeric[from], numeric[to]);
            }
        }
    }

    // The width types join the same classes, after the ladder above rather than before it: their
    // own ladder reaches `Int` and `Long`, and skips that one pair on the grounds that it has just
    // been declared here.
    defineIntegerInstances(*module, widthTypes);

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

    for(auto type: widthTypes) defineConversion(*module, "Narrow"_v, "truncate"_v, type, type);

    // And the reinterpretation ladder over everything both ladders cover, which is why it runs last:
    // `defineIntegerInstances` appended `Int` and `Long` to this list, and the two floating types
    // have just joined it, so this is the whole of what `Bitcast` is generated over.
    defineBitcastLadder(*module, widthTypes);

    // The classes the language's own syntax is written in terms of - a literal, an implicit
    // conversion, a condition, and the three points a binding convention compiles to. Looked up by
    // name once, here, so that nothing downstream has to search for them by string.
    program.coreClasses.fromInt = classNamed(*module, "FromInt"_v);
    program.coreClasses.fromDecimal = classNamed(*module, "FromDecimal"_v);
    program.coreClasses.widen = classNamed(*module, "Widen"_v);
    program.coreClasses.narrow = classNamed(*module, "Narrow"_v);
    program.coreClasses.truth = classNamed(*module, "Truth"_v);
    program.coreClasses.try_ = classNamed(*module, "Try"_v);
    program.coreClasses.rewrap = classNamed(*module, "Rewrap"_v);
    program.coreClasses.index = classNamed(*module, "Index"_v);
    program.coreClasses.show = classNamed(*module, "Show"_v);
    program.coreClasses.copy = classNamed(*module, "Copy"_v);
    program.coreClasses.sink = classNamed(*module, "Sink"_v);
    program.coreClasses.reclaim = classNamed(*module, "Reclaim"_v);
    program.coreClasses.drop = classNamed(*module, "Drop"_v);
    program.coreClasses.trivialCopy = classNamed(*module, "TrivialCopy"_v);
    program.coreClasses.trivialSink = classNamed(*module, "TrivialSink"_v);

    // The five a vector joins on demand, for the reason CoreClasses gives: no instance of any of
    // them over a vector is declared anywhere, so "could a vector join this" is asked at every
    // instance lookup that finds nothing and must not be a string lookup.
    program.coreClasses.num = classNamed(*module, "Num"_v);
    program.coreClasses.integral = classNamed(*module, "Integral"_v);
    program.coreClasses.logic = classNamed(*module, "Logic"_v);
    program.coreClasses.bitcast = classNamed(*module, "Bitcast"_v);
    program.coreClasses.lanewise = classNamed(*module, "Lanewise"_v);

    // The exit signal's carrier. Its constructors are found by name rather than assumed to be
    // declared in this order, since the order is a detail of the source above and this is emitted
    // code that has no declaration to read.
    if(auto outcome = findType(*module, Context::nameHash("Outcome"_v), kNullLocation)) {
        program.outcomeType = (RecordType*)(*program.types)[outcome] - *program.types;

        if(auto proceed = findConstructor(*module, Context::nameHash("Proceed"_v), kNullLocation)) {
            program.outcomeProceed = proceed.unwrap().index;
        }

        if(auto exit = findConstructor(*module, Context::nameHash("Exit"_v), kNullLocation)) {
            program.outcomeExit = exit.unwrap().index;
        }
    }

    // Core's own instances exist only now, so its superclass checks and its `default`
    // declarations run here rather than as part of reading its source.
    checkModuleClasses(*module, *ast);
}

/*
 * Collections.
 *
 * The growable array of Design.md's "Collection types", written in the language over Native rather
 * than generated by the compiler. It is a separate module from Core for one reason: an array is
 * built out of raw pointers and the heap, and Core is imported by Native rather than the other way
 * round, so nothing in Core can name either.
 *
 * It is nonetheless implicitly imported, because `[a]` is a type the grammar produces and a type
 * whose operations a program cannot reach would be a strange thing to be able to write.
 *
 * What this is not, yet, is Implementation-Regions.md part 5's shared `Storage(a)` primitive with a
 * derived Drop - the thing collections are supposed to be written on so that region placement
 * applies to all of them at once. This is one collection with an authored Drop, which is the
 * smaller thing that makes the storage decisions of Milestone 6 testable; the primitive belongs
 * with the standard library that does not exist yet.
 */
// Collections' source is `lib/Collections.yana`.

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

void defineCollections(Program& program) {
    auto& context = program.context;

    // A declaration with no body, which this module has since the bulk operations landed: what a
    // call to one expands to is chosen by the compiler from the two implementations beside it. Core
    // and Native have said this since they were written, and for the same reason.
    auto ast = parseLibraryModule(context, "Collections"_v, true);
    if(!ast) return;

    auto module = program.addModule(ast->name, *ast->region);
    program.embeddedAsts.push(ast);

    resolveModuleDecls(*module, *ast, nullptr);

    /*
     * Before the bodies, not after them.
     *
     * `Program::arrayType` is what makes `Array(a)` recognizable *as* the growable array - it is
     * what `sliceOf` asks, so it is what decides whether the ordinary conversion to a slice exists.
     * Setting it afterwards meant this module alone could not use its own container: `elements`,
     * whose whole body is `slice(self, 0, self.length)`, saw `Array(a)` as an unrelated record and
     * reported that it does not fit `Flat(a)`.
     *
     * The declaration is what the pointer names, so it is available as soon as the declarations have
     * been read; nothing in this module's signatures writes `[a]`, which is the only thing that
     * would have needed it earlier still.
     */
    program.collections = module;
    auto array = module->namedTypes.get(context.addQualifiedName("Array", 5, 1));
    if(array) program.arrayType = (RecordType*)(*program.types)[array.unwrap()] - *program.types;

    // The map, on the same terms - Implementation-Map.md §7. One name for both platform rows: the
    // `@platform` selection has already run over the declarations, so whichever of the two `Map`
    // declarations this target kept is the one the literal instantiates.
    auto map = module->namedTypes.get(context.addQualifiedName("Map", 3, 1));
    if(map) program.mapType = (RecordType*)(*program.types)[map.unwrap()] - *program.types;

    // §5's two, looked up here for the reason Core's are looked up where they are declared: what
    // asks for them is the resolver rather than a name a program wrote. See CoreClasses.
    program.coreClasses.contiguous = classNamed(*module, "Contiguous"_v);
    program.coreClasses.chunked = classNamed(*module, "Chunked"_v);
    program.coreClasses.indexInsert = classNamed(*module, "IndexInsert"_v);

    // The map's one per-key-type constant - Implementation-Map.md §2 and §4.4. A rule in resolve
    // with every reader folded against it, which is the shape Containers §14 settled for and the
    // only one of that section's three rules that needs nothing new.
    attachIntrinsic(*module, "scanLimitFor"_v, emitScanLimit<true>);
    attachIntrinsic(*module, "scanLimitForKeys"_v, emitScanLimit<false>);

    /*
     * Recorded for the reason `allocateHeap` is: the compiler emits the call, so there is no name in
     * any program for resolution to start from.
     *
     * Before defineContainerInstances, because that is what makes a subscript check reachable: the
     * generated `get` bodies below are resolved against whatever this holds at the time.
     */
    if(context.settings.checks) {
        auto found = module->functions.get(context.addUnqualifiedName("checkCondition", 14));
        program.checkCondition = found ? found.unwrap() : nullptr;

        /*
         * And the arm it branches to, marked as one control does not come back out of.
         *
         * Here rather than in an attribute on the declaration because there is nothing in the source
         * to attach one to that would mean anything: `checkFailed` is `exitProcess(134)` and a
         * `return`, and what makes it final is the kernel rather than the shape of the body. Both
         * targets' spellings are equally final - a status on native, a thrown value on JS - so the
         * fact is about this function rather than about either platform's implementation of it.
         *
         * See `Function::noReturn` for what reads it. It is set whether or not `checkCondition` was
         * found, since a build with the checks off has no call to either.
         */
        auto failed = module->functions.get(context.addUnqualifiedName("checkFailed", 11));
        if(failed) (*module->arena)[failed.unwrap()]->noReturn = true;
    }

    // After `arrayType` above, and before this module's own bodies below - several of which
    // subscript, and would reach an instance that does not exist yet.
    defineContainerInstances(*module);

    // The bulk operations, whose declarations above have no body: which of the two implementations
    // beside each one a call takes is decided where the call is - see simd.cpp.
    defineBulkOperations(*module);

    resolveModuleBodies(*module);
}

/*
 * Text.
 *
 * `String`'s operations, split out of Collections - Implementation-Simplification.md §17.
 *
 * It is a module of its own for one reason, and the reason is a *cycle* rather than a division of
 * subject matter. What a native string is made of is Native's - a `Run(U8)` and a count - so the
 * reinterpretation that hands those two words out has to live behind an import that already means
 * "this is unsafe". But the run those words describe is a container, and the container's declaration
 * has to be implicitly visible because `[a]` is grammar. So the unsafe half sits *above* the
 * container it names and *below* the algorithms that use it, and one module cannot be on both sides
 * of that. See NativeText, which is the half in between.
 *
 * Implicitly imported, like Collections and for the same reason: a string literal is grammar, and
 * what `print` and `Show` mean has to be reachable without being asked for. That costs nothing in
 * safety, because an import is not transitive - see findInstances in name.cpp, and Program::native.
 */
// Text's source is `lib/Text.yana`.

void defineText(Program& program) {
    auto& context = program.context;

    // The one library module with a body for every declaration, so the only one that does not have
    // to allow a bare signature.
    auto ast = parseLibraryModule(context, "Text"_v, false);
    if(!ast) return;

    auto module = program.addModule(ast->name, *ast->region);
    program.embeddedAsts.push(ast);

    resolveModuleDecls(*module, *ast, nullptr);
    program.text = module;

    /*
     * The three functions a format expression is built out of - Implementation-Storage.md part 8.
     *
     * Recorded for the reason `allocateHeap` is: `"a{x}b"` is resolved by the compiler, which has a
     * chunk list and a set of resolved holes and no call site for name resolution to start from.
     * Everything else about a format is an ordinary call to an ordinary function.
     */
    auto findText = [&](const char* text, Size length) -> ModulePtr<Function> {
        auto found = module->functions.get(context.addUnqualifiedName(text, length));
        return found ? found.unwrap() : nullptr;
    };

    program.newString = findText("newStringOfCapacity", 19);
    program.pushString = findText("pushString", 10);
    program.formatBound = findText("formatBound", 11);

    resolveModuleBodies(*module);
}

