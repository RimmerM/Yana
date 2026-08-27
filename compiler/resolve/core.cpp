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

static ast::Module* parseLibrarySource(Context& context, StringId id, bool allowSignatures) {
    auto source = context.library.source(context, id);
    if(source.length == 0) return nullptr;

    Lexer lexer(context, context.diagnostics, source, id);
    Parser parser(context, lexer, id);
    parser.allowSignatures = allowSignatures;

    return new ast::Module(parser.parseModule());
}

/*
 * Every file of a library module, parsed, and the group laid over them.
 *
 * `lib/` is walked rather than looked up by name: a module there is a directory of files exactly as
 * a module in a program's own tree is, so `Core` is three files and `Math` is one and neither is a
 * special case. The order is the library's own - `LibrarySource::files` sorts, and says why.
 *
 * The ASTs and the group both belong to the program, for as long as anything can still resolve
 * against them. Null where the library has no such module at all; a module whose files are all
 * unreadable is the same answer, since a group with nothing in it is nothing to import.
 */
/*
 * Whether a file the library walk turned up is a file of this module.
 *
 * The same three cases `groupFile` decides for a project file, and it has to be decided after the
 * parse because it is written in the file. A file directly in the module's directory that said
 * nothing is a file of it; a file in a subdirectory that said nothing belongs to that subdirectory's
 * module, and is left for whoever imports it; `module` alone names the file's own path, which is a
 * module of one file where `Math.yana` and `File.yana` sit and is standing apart from its siblings
 * where `Core/Sort.yana` would; and `module M` joins M, which is what `Core/Float/*.yana` write.
 *
 * The prefix restriction the project side reports on is not checked here. The library's files are
 * this repository's own, so a name that does not fit is a mistake in the library rather than in the
 * program being compiled, and it shows up as the module missing declarations rather than as a report
 * against source the author did not write.
 */
static bool joinsModule(StringId module, LibraryFile file, const ast::Module& ast) {
    switch(ast.membership) {
        // `module` names the file's own path, so it is this module exactly when the two are the same
        // name - which is what `Math.yana` and `File.yana` are, and what `Core/Sort.yana` would not be.
        case ast::Membership::Own: return file.name == module;
        case ast::Membership::Named: return ast.joins == module;
        case ast::Membership::Directory: return file.inDirectory;
    }

    return false;
}

ast::ModuleGroup* parseLibraryGroup(Program& program, StringId name, bool allowSignatures) {
    auto& context = program.context;

    auto files = context.library.files(context, name);
    if(files.isEmpty()) return nullptr;

    auto group = new ast::ModuleGroup { .name = name };
    program.embeddedGroups.push(group);

    for(auto file: files) {
        auto ast = parseLibrarySource(context, file.name, allowSignatures);
        if(!ast) continue;

        program.embeddedAsts.push(ast);
        if(joinsModule(name, file, *ast)) group->files.push(ast);
    }

    return group->files.isEmpty() ? nullptr : group;
}

/*
 * A library module an `import` named. `allowSignatures` is off: a declaration with no body means
 * something only where the compiler attaches a hook to it, and it attaches them by name to the two
 * modules it builds itself.
 */
ast::ModuleGroup* findLibraryModule(Program& program, StringId name) {
    return parseLibraryGroup(program, name, false);
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
    auto group = parseLibraryGroup(program, id, true);

    // A module every target has, absent, is a broken library and is reported. One that exists only
    // on some targets is a different thing: `Atomic` has a single `.native.yana` file, so on a JS
    // build the selector leaves it with none and the module simply does not exist there (§5.4).
    if(!group && required) {
        context.diagnostics.error("cannot read the standard library module %@ - looked in %@. Pass -lib with the directory holding Core/Core.yana, or set YANA_LIB."_v,
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

/*
 * A constant whose value the target answers - `Target.byteOrder`, and so far only that.
 *
 * Supplied by the compiler rather than declared in Core's source, on `addPrimitive`'s terms and for
 * its reason: everything about it is the compiler's. The type it answers in, the metric that carries
 * the answer, and the fact that there *is* no initializer are all decided here, so a source
 * declaration would be a line carrying no information this function does not already have - and
 * would need an attribute, a table of legal names and a set of refusals to say it with. `Size` is
 * the precedent and it is exact: a name every module can see, documented where it is built.
 *
 * `globalValue` is the other half - it answers a read of one with an `InstTypeMetric` rather than a
 * load, so nothing marks the global used and no storage is ever emitted for it.
 *
 * The name is qualified, and `Target` is a namespace rather than a type: nothing declares one, which
 * is what a plain namespace is (see registerNamespace). Adding the next target question - address
 * width, the vector width a target reaches for - is one more call here.
 */
static void addTargetConstant(Module& module, StringView name, TypePtr type, TypeMetricKind metric) {
    if(!type) return;

    auto global_ = module.addGlobal(module.context.addQualifiedName(name.ptr, name.length), kNullLocation);
    global_->type = type;
    global_->targetMetric = true;
    global_->metric = metric;

    // As public as `Int` is, and for the same reason: nothing wrote `pub` on it because no source
    // wrote it at all, and a program in any module has to be able to name it.
    global_->exported = true;
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

// Whether a value of `from` fits in `to` without losing anything: more bits, or the same bits
// without a sign to lose. This decides which of the two conversion ladders a pair joins, which
// is the whole of the rule for whether a conversion happens on its own or has to be written.
static bool widens(GlobalBase global, TypePtr from, TypePtr to) {
    return integerRangeFits(*(IntType*)global[from], *(IntType*)global[to]);
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
 * How wide a primitive is for the purpose of reinterpreting it, or zero where the question does not
 * apply.
 *
 * The type's *own* bits and not its register's, because that is what decides whether two types hold
 * the same value: `WideInt` is 53 bits in a 64-bit register, so nothing else has its shape and it
 * gets no rung. `Bool` is excluded by the same rule for a better reason - it is one bit, and a
 * reinterpretation of one bit is a question about the other seven that nothing here can answer.
 */
// Above every width a type can actually have, so an abstract kind's key is its own - see below.
static constexpr U16 kTargetWidthKey = 1024;

static U16 reinterpretWidth(GlobalBase global, TypePtr type) {
    if(global[type]->kind == Type::Float) {
        return ((FloatType*)global[type])->width == FloatType::Float ? 32 : 64;
    }

    if(global[type]->kind != Type::Int) return 0;

    auto& integer = *(IntType*)global[type];

    /*
     * An abstract width pairs with the *same* abstract width and with nothing else.
     *
     * The rule is unchanged - two types hold the same value only when they are the same number of
     * bits - and `Size` is not a number here, so it cannot be compared against one. What it can be
     * compared against is `USize`, which is the same target quantity read the other way and is
     * therefore the same width on every target by construction. That pair is the one a bounds test
     * is written with (`bitcast(index) :: USize`), and it is the reason this returns a key per kind
     * rather than simply declining.
     *
     * The key is above every real width, so it collides with no concrete type. A `CodeUnit` has no
     * partner and gets no rung, which costs nothing: nothing reinterprets an encoding unit.
     *
     * An address is the other pair a program wants, and it is declared where the address is - see
     * definePointerInstances, which relates a `%a` to `Size` and is sound on every target by
     * construction rather than by both sides happening to be sixty-four bits.
     */
    if(integer.isTargetWidth()) return U16(kTargetWidthKey + U16(integer.target));

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

void definePreludeTypes(Program& program, Module& core, TypeList& widthTypes) {
    auto& context = program.context;
    auto module = &core;

    program.scalar.error = (Type*)new (program.types) Type(Type::Error) - *program.types;
    program.scalar.unit = (Type*)new (program.types) Type(Type::Unit) - *program.types;

    addPrimitive(program, *module, "Unit"_v, (Type*)(*program.types)[program.scalar.unit]);
    program.scalar.int_ = addPrimitive(program, *module, "Int"_v, new (program.types) IntType(32, IntType::Int, true));
    program.scalar.long_ = addPrimitive(program, *module, "Long"_v, new (program.types) IntType(64, IntType::Long, true));
    program.scalar.float_ = addPrimitive(program, *module, "Float"_v, new (program.types) FloatType(FloatType::Float));
    program.scalar.double_ = addPrimitive(program, *module, "Double"_v, new (program.types) FloatType(FloatType::Double));

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

    program.scalar.bool_ = coreType(*module, "Bool"_v);
    program.scalar.ordering = coreType(*module, "Ordering"_v);

    // The classes the language's own syntax is written in terms of - a literal, an implicit
    // conversion, a condition, and the three points a binding convention compiles to. Looked up by
    // name once, here, so that nothing downstream has to search for them by string.
    program.coreClasses.fromInt = classNamed(*module, "FromInt"_v);
    program.coreClasses.fromDecimal = classNamed(*module, "FromDecimal"_v);
    program.coreClasses.widen = classNamed(*module, "Widen"_v);
    program.coreClasses.narrow = classNamed(*module, "Narrow"_v);
    program.coreClasses.truth = classNamed(*module, "Truth"_v);
    program.coreClasses.enum_ = classNamed(*module, "Enum"_v);
    program.coreClasses.try_ = classNamed(*module, "Try"_v);
    program.coreClasses.rewrap = classNamed(*module, "Rewrap"_v);
    program.coreClasses.index = classNamed(*module, "Index"_v);
    program.coreClasses.show = classNamed(*module, "Show"_v);
    program.coreClasses.eq = classNamed(*module, "Eq"_v);
    program.coreClasses.ord = classNamed(*module, "Ord"_v);
    program.coreClasses.copy = classNamed(*module, "Copy"_v);
    program.coreClasses.reclaim = classNamed(*module, "Reclaim"_v);
    program.coreClasses.drop = classNamed(*module, "Drop"_v);
    program.coreClasses.trivialCopy = classNamed(*module, "TrivialCopy"_v);
    program.coreClasses.trivialSink = classNamed(*module, "TrivialSink"_v);

    // The five a vector joins on demand, for the reason CoreClasses gives: no instance of any of
    // them over a vector is declared anywhere, so "could a vector join this" is asked at every
    // instance lookup that finds nothing and must not be a string lookup.
    program.coreClasses.num = classNamed(*module, "Num"_v);
    program.coreClasses.integral = classNamed(*module, "Integral"_v);
    program.coreClasses.bitwise = classNamed(*module, "Bitwise"_v);
    program.coreClasses.bitcast = classNamed(*module, "Bitcast"_v);
    program.coreClasses.lanewise = classNamed(*module, "Lanewise"_v);

    // The exit signal's carrier. Its constructors are found by name rather than assumed to be
    // declared in this order, since the order is a detail of the source above and this is emitted
    // code that has no declaration to read.
    /*
     * Before the bodies, not after them.
     *
     * `Program::arrayType` is what makes `Array(a)` recognizable *as* the growable array - it is
     * what `sliceOf` asks, so it is what decides whether the ordinary conversion to a slice exists.
     * Setting it afterwards meant `Core/Array.yana` alone could not use its own container:
     * `elements`, whose whole body is `slice(self, 0, self.length)`, saw `Array(a)` as an unrelated
     * record and reported that it does not fit `Flat(a)`.
     */
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
    program.coreClasses.writable = classNamed(*module, "Writable"_v);
    program.coreClasses.indexInsert = classNamed(*module, "IndexInsert"_v);

    if(auto outcome = findType(*module, Context::nameHash("Outcome"_v), kNullLocation)) {
        program.outcomeType = (RecordType*)(*program.types)[outcome] - *program.types;

        if(auto proceed = findConstructor(*module, Context::nameHash("Proceed"_v), kNullLocation)) {
            program.outcomeProceed = proceed.unwrap().index;
        }

        if(auto exit = findConstructor(*module, Context::nameHash("Exit"_v), kNullLocation)) {
            program.outcomeExit = exit.unwrap().index;
        }
    }
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
    attachIntrinsic(*module, "swap"_v, emitSwap);
    attachIntrinsic(*module, "exchange"_v, emitExchange);

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
    // The byte order, as a constant. Here rather than beside the primitive types because the type it
    // answers in is `Endian`, which is Core's *source* - so it exists only once the declarations have
    // been read, which is what this hook runs after.
    addTargetConstant(*module, "Target.byteOrder"_v, coreType(*module, "Endian"_v),
                      TypeMetricKind::ByteOrder);

    attachIntrinsic(*module, "!"_v, emitTruthNot);
    attachDeferredIntrinsic(*module, "&&"_v, emitTruthAnd);
    attachDeferredIntrinsic(*module, "||"_v, emitTruthOr);

    // The portable vector set, whose declarations are in the source above and whose expansions are
    // simd.cpp's - Design-Vector §3.3.
    defineVectorIntrinsics(*module);

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

    // `Bitwise` before `Integral`, which declares it as a superclass - the same ordering `FromInt`
    // before `Num` has above, and for the same reason: the obligation is checked where the instance
    // is generated.
    defineBitwise(*module, program.scalar.int_, emitUnary<Value::Not>);
    defineBitwise(*module, program.scalar.long_, emitUnary<Value::Not>);
    defineIntegral(*module, program.scalar.int_);
    defineIntegral(*module, program.scalar.long_);

    // And the byte reversal at the two canonical widths, which the loop above cannot reach: `Int`
    // and `Long` are the scalar module's types rather than the fixed-width family's.
    defineByteSwap(*module, program.scalar.int_);
    defineByteSwap(*module, program.scalar.long_);

    // The bit counts beside them, at exactly the two widths they are declared over, and the
    // permutations at the same two.
    defineBits(*module, program.scalar.int_);
    defineBits(*module, program.scalar.long_);
    defineBitPermute(*module, program.scalar.int_);
    defineBitPermute(*module, program.scalar.long_);

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
    attachIntrinsic(*module, "scanLimitFor"_v, emitScanLimit<true>);
    attachIntrinsic(*module, "scanLimitForKeys"_v, emitScanLimit<false>);

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

    // After `arrayType` above, and before this module's own bodies below - several of which
    // subscript, and would reach an instance that does not exist yet.
    defineContainerInstances(*module);

    // The bulk operations, whose declarations above have no body: which of the two implementations
    // beside each one a call takes is decided where the call is - see simd.cpp.
    defineBulkOperations(*module);
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
}

