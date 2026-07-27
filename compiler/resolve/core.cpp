#include "core.h"
#include "intrinsic.h"
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
static const char* kCoreSource = R"CORE(
infixl 1 ||
infixl 2 &&
infixl 3 ==
infixl 3 !=
infixl 3 >
infixl 3 >=
infixl 3 <
infixl 3 <=
infixl 4 `xor`
infixl 4 `or`
infixl 5 `and`
infixl 5 `shl`
infixl 5 `shr`
infixl 5 `sar`
infixl 6 +
infixl 6 -
infixl 7 *
infixl 7 /
infixl 7 `rem`
infixl 7 %

data Bool = False | True
data Ordering = LT | EQ | GT

data Maybe(a) = Nothing | Just(a)
data Result(e, a) = Err(e) | Ok(a)

class FromInt(a):
  fn fromInt(value: Long) -> a

class FromDecimal(a):
  fn fromDecimal(value: Double) -> a

default FromInt = Int
default FromDecimal = Float

-- The bodies below are class defaults: an instance that writes `==` has `!=` already, and one
-- that writes `compare` has all four comparisons. What each class still asks of an instance is the
-- one primitive operation everything else is derived from, which is why there is no choice of
-- which function to implement and no `MINIMAL` pragma to read. A default may only call class
-- functions of strictly lower rank than its own, so a pair defining each other in terms of the
-- other is a rejected declaration rather than a program that compiles and hangs.
--
-- Every primitive instance below still supplies all of these itself, so a Bool's `!=` is the one
-- comparison instruction it always was rather than a specialization of `!(lhs == rhs)`.
class Eq(a):
  fn ==(lhs: a, rhs: a) -> Bool
  fn !=(lhs: a, rhs: a) -> Bool = !(lhs == rhs)

-- The four comparisons are `compare` read four ways, and `compare` is the fold a derived instance
-- would have to produce. Lexicographic order over a record is therefore one function rather than
-- five, which is what makes Ord worth deriving at all.
class (Eq(a)) Ord(a):
  fn <(lhs: a, rhs: a) -> Bool = compare(lhs, rhs) == LT
  fn <=(lhs: a, rhs: a) -> Bool = compare(lhs, rhs) != GT
  fn >(lhs: a, rhs: a) -> Bool = compare(lhs, rhs) == GT
  fn >=(lhs: a, rhs: a) -> Bool = compare(lhs, rhs) != LT
  fn compare(lhs: a, rhs: a) -> Ordering

-- Anything that can be added can be counted from, so `fn (Num(a)) inc(x: a) = x + 1` compiles as
-- written rather than making the author declare FromInt as well. FromInt stays its own class so
-- that a Duration or a units newtype can be integer-literal-constructible without also claiming
-- to support multiplication.
--
-- Negation is the one place the superclass earns its keep as a default: `0` is `FromInt(a)` and
-- the subtraction is this class's own, so unary `-` needs nothing an instance has not already
-- promised. An instance for which that is not the negation it wants overrides it.
class (FromInt(a)) Num(a):
  fn +(lhs: a, rhs: a) -> a
  fn -(lhs: a, rhs: a) -> a
  fn *(lhs: a, rhs: a) -> a
  fn /(lhs: a, rhs: a) -> a
  fn -(value: a) -> a = 0 - value

class (Num(a)) Integral(a):
  fn rem(lhs: a, rhs: a) -> a
  fn %(lhs: a, rhs: a) -> a
  fn shl(lhs: a, rhs: a) -> a
  fn shr(lhs: a, rhs: a) -> a
  fn sar(lhs: a, rhs: a) -> a
  fn and(lhs: a, rhs: a) -> a
  fn or(lhs: a, rhs: a) -> a
  fn xor(lhs: a, rhs: a) -> a
  fn not(value: a) -> a

-- For a type that is not Bool the short-circuiting pair and the bitwise pair are the same
-- operation, so `&&`, `||` and `!` default to `and`, `or` and `not` and an instance writes four
-- functions instead of seven. Bool's own instance supplies all seven, which is where
-- short-circuiting lands when the evaluation order of `&&` becomes a rule.
class Logic(a):
  fn &&(lhs: a, rhs: a) -> a = and(lhs, rhs)
  fn ||(lhs: a, rhs: a) -> a = or(lhs, rhs)
  fn and(lhs: a, rhs: a) -> a
  fn or(lhs: a, rhs: a) -> a
  fn xor(lhs: a, rhs: a) -> a
  fn not(value: a) -> a
  fn !(value: a) -> a = not(value)

-- What a condition means. `if x`, `if:` cases and `while x` ask this class rather than requiring
-- a Bool, so `if items:` and `if i - 1: continue` say what they look like they say. The rule that
-- keeps it from being JavaScript's truthiness is that it never applies through a conversion: the
-- instance is selected for the condition's own type, so what `if x` means depends on x's type
-- alone and not on which Widen instances happen to be in scope. Bool's instance is the identity.
--
-- Deliberately not instanced for Maybe or Result: they carry a payload, and `if maybeThing:`
-- invites unwrapping it on the next line. Truth answers "is this empty/zero/null", not "did this
-- succeed" - the `is` operator is what payload-carrying types use.
--
-- `value` becomes `&a` once binding conventions are resolved; nothing about truthiness needs to
-- consume what it is asked about.
class Truth(a):
  fn truthy(value: a) -> Bool

-- A Widen instance is required to be lossless and total; that is a contract on whoever writes
-- one, checked no more than Copy's is. Which of the two classes relates a pair of types is the
-- whole of the rule for whether a conversion happens on its own or has to be written.
class Widen(a, b):
  fn widen(from: a) -> b

class Narrow(a, b):
  fn narrow(from: a) -> b
)CORE";

/*
 * Assembling the module.
 */

static TypePtr addPrimitive(Program& program, Module& module, StringView name, Type* value) {
    auto pointer = value - *program.types;
    auto id = module.context.addQualifiedName(name.ptr, name.length, 1);

    // An integer type is printed by name, so it has to know the one it was declared under.
    if(value->kind == Type::Int) ((IntType*)value)->name = id;

    module.namedTypes.add(id, pointer);
    return pointer;
}

static TypePtr coreType(Module& module, StringView name) {
    auto found = module.namedTypes.get(Context::nameHash(name));
    assertTrue(found.isJust());
    return found.unwrap();
}

void defineCore(Program& program) {
    auto& context = program.context;

    program.scalar.error = (Type*)new (program.types) Type(Type::Error, 0) - *program.types;
    program.scalar.unit = (Type*)new (program.types) Type(Type::Unit, 0) - *program.types;

    auto name = context.addQualifiedName("Core", 4, 1);
    Lexer lexer(context, context.diagnostics, StringView { kCoreSource, stringLength(kCoreSource) }, name);
    Parser parser(context, lexer, name);
    auto ast = new ast::Module(parser.parseModule());

    auto module = program.addModule(ast->name, *ast->region);
    program.core = module;
    program.embeddedAsts.push(ast);

    addPrimitive(program, *module, "Unit"_v, (Type*)(*program.types)[program.scalar.unit]);
    program.scalar.int_ = addPrimitive(program, *module, "Int"_v, new (program.types) IntType(32, IntType::Int, true));
    program.scalar.long_ = addPrimitive(program, *module, "Long"_v, new (program.types) IntType(64, IntType::Long, true));
    program.scalar.float_ = addPrimitive(program, *module, "Float"_v, new (program.types) FloatType(FloatType::Float));
    program.scalar.double_ = addPrimitive(program, *module, "Double"_v, new (program.types) FloatType(FloatType::Double));

    resolveModuleDecls(*module, *ast, nullptr);

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
                defineConversion(*module, "Narrow"_v, "narrow"_v, numeric[from], numeric[to]);
            }
        }
    }

    // The five classes the language's own syntax is written in terms of. Looked up by name once,
    // here, so that nothing downstream has to search for them by string.
    program.coreClasses.fromInt = classNamed(*module, "FromInt"_v);
    program.coreClasses.fromDecimal = classNamed(*module, "FromDecimal"_v);
    program.coreClasses.widen = classNamed(*module, "Widen"_v);
    program.coreClasses.narrow = classNamed(*module, "Narrow"_v);
    program.coreClasses.truth = classNamed(*module, "Truth"_v);

    // Core's own instances exist only now, so its superclass checks and its `default`
    // declarations run here rather than as part of reading its source.
    checkModuleClasses(*module, *ast);
}
