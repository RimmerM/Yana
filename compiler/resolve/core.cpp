#include "core.h"
#include "builder.h"
#include "expr.h"
#include "name.h"
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

class Eq(a):
  fn ==(lhs: a, rhs: a) -> Bool
  fn !=(lhs: a, rhs: a) -> Bool

class (Eq(a)) Ord(a):
  fn <(lhs: a, rhs: a) -> Bool
  fn <=(lhs: a, rhs: a) -> Bool
  fn >(lhs: a, rhs: a) -> Bool
  fn >=(lhs: a, rhs: a) -> Bool
  fn compare(lhs: a, rhs: a) -> Ordering

class Num(a):
  fn +(lhs: a, rhs: a) -> a
  fn -(lhs: a, rhs: a) -> a
  fn *(lhs: a, rhs: a) -> a
  fn /(lhs: a, rhs: a) -> a
  fn -(value: a) -> a

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

class Logic(a):
  fn &&(lhs: a, rhs: a) -> a
  fn ||(lhs: a, rhs: a) -> a
  fn and(lhs: a, rhs: a) -> a
  fn or(lhs: a, rhs: a) -> a
  fn xor(lhs: a, rhs: a) -> a
  fn not(value: a) -> a
  fn !(value: a) -> a

class Extend(a, b):
  fn extend(from: a) -> b

class Truncate(a, b):
  fn truncate(from: a) -> b
)CORE";

namespace {

// One generated instance under construction. Building an instance is the same six steps every
// time - make the function, give it the class's substituted signature, emit a body, attach the
// intrinsic, record it in the instance, and register the instance - so they are done in one
// place and the tables below say only what is different.
struct CoreBuilder {
    Module& module;
    ModuleBase local;
    GlobalBase global;

    explicit CoreBuilder(Module& module):
        module(module), local(*module.arena), global(*module.types) {}

    StringId name(StringView text) {
        return module.context.addQualifiedName(text.ptr, text.length, 1);
    }

    GlobalPtr<TypeClass> classNamed(StringView text) {
        auto found = module.classes.get(Context::nameHash(text));
        assertTrue(found.isJust());
        return found.unwrap();
    }
};

// The IR one primitive operation expands to, shared by the generated body and the intrinsic so
// that a call and an inline expansion can never drift apart.
using Emit = ModulePtr<Value> (*)(ExprResolver&, Buffer<ModulePtr<Value>>, TypePtr, LocationId, StringId);

template<Value::Kind kind>
static ModulePtr<Value> emitBinary(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                   LocationId source, StringId resultName) {
    return resolver.ref(resolver.emit<InstBinary>(source, resultName, type, kind, args[0], args[1]));
}

template<Value::Kind kind>
static ModulePtr<Value> emitUnary(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                  LocationId source, StringId resultName) {
    return resolver.ref(resolver.emit<InstUnary>(source, resultName, type, kind, args[0]));
}

template<CompareOp op>
static ModulePtr<Value> emitCompare(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                    LocationId source, StringId resultName) {
    return resolver.ref(resolver.emit<InstCmp>(source, resultName, type, args[0], args[1], op));
}

static ModulePtr<Value> emitCast(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                 LocationId source, StringId resultName) {
    return resolver.ref(resolver.emit<InstUnary>(source, resultName, type, Value::Cast, args[0]));
}

// `not` on a Bool is the one bit operation that is correct for a two-constructor discriminant,
// rather than an integer complement that would produce something outside the type.
static ModulePtr<Value> emitLogicalNot(ExprResolver& resolver, Buffer<ModulePtr<Value>> args, TypePtr type,
                                       LocationId source, StringId resultName) {
    auto one = resolver.makeInt(source, type, 1);
    return resolver.ref(resolver.emit<InstBinary>(source, resultName, type, Value::Xor, args[0], one));
}

} // namespace

/*
 * Generating an instance function.
 */

static StringId instanceName(Module& module, TypeClass& typeClass, Buffer<TypePtr> args, StringId method) {
    Array<char> text;
    auto className = module.context.findName(typeClass.name);
    appendText(text, StringView { className.text(), className.size() });
    text.push('(');

    for(Size i = 0; i < args.length; i++) {
        if(i) appendText(text, ", "_v);
        describeType(module.context, *module.types, args[i], text);
    }

    appendText(text, ")."_v);
    auto methodName = module.context.findName(method);
    appendText(text, StringView { methodName.text(), methodName.size() });

    return module.context.addQualifiedName(text.pointer(), text.size(), 1);
}

// Emits `fn f(args...) = <emit>(args...)` as a real function, so the instance has something to
// print, lower and eventually take the address of even though ordinary calls inline it.
static ModulePtr<Function> generateInstanceFunction(CoreBuilder& builder, TypeClass& typeClass,
                                                    Buffer<TypePtr> args, U16 index, Emit emit) {
    auto& module = builder.module;
    auto signature = builder.local[typeClass.functions.get(builder.global, index).fun];
    auto method = typeClass.functions.get(builder.global, index).name;

    auto function = addAnonymousFunction(module, instanceName(module, typeClass, args, method), kNullLocation);
    function->instanceOf = (TypeClass*)&typeClass - builder.global;
    for(auto arg: args) function->instanceArgs.push(module.arena, arg);

    function->returnType = substituteType(module, signature->returnType, args, kNullLocation);

    for(Size i = 0; i < signature->args.size(); i++) {
        auto declared = builder.local[signature->args.get(builder.local, i)];
        function->addArg(module, declared->name, substituteType(module, declared->type, args, kNullLocation),
                         kNullLocation);
    }

    ExprResolver resolver(module.context, module, *function);
    Array<ModulePtr<Value>> values;
    for(auto arg: function->args.contents(builder.local)) values.push((ModulePtr<Value>)arg);

    auto result = emit(resolver, toBuffer(values), function->returnType, kNullLocation, 0);
    resolver.terminate(resolver.emit<InstRet>(kNullLocation, 0, module.scalar.unit, result));

    function->intrinsic = emit;
    return function - builder.local;
}

// `compare` is the one primitive operation that is not a single instruction, so it has a real
// body and no intrinsic: calls to it are ordinary calls that reach the backend as written.
static ModulePtr<Function> generateCompare(CoreBuilder& builder, TypeClass& typeClass, TypePtr type, U16 index) {
    auto& module = builder.module;
    auto ordering = module.scalar.ordering;
    TypePtr args[] = { type };

    auto function = addAnonymousFunction(
        module, instanceName(module, typeClass, { args, 1 }, Context::nameHash("compare", 7)), kNullLocation);

    function->instanceOf = (TypeClass*)&typeClass - builder.global;
    function->instanceArgs.push(module.arena, type);
    function->returnType = ordering;

    auto lhs = ModulePtr<Value>(function->addArg(module, builder.name("lhs"_v), type, kNullLocation) - builder.local);
    auto rhs = ModulePtr<Value>(function->addArg(module, builder.name("rhs"_v), type, kNullLocation) - builder.local);

    ExprResolver resolver(module.context, module, *function);
    auto equalBlock = resolver.addBlock();
    auto greaterTest = resolver.addBlock();
    auto greaterBlock = resolver.addBlock();
    auto lessBlock = resolver.addBlock();

    auto equal = resolver.ref(resolver.emit<InstCmp>(kNullLocation, 0, module.scalar.bool_, lhs, rhs, CompareOp::Eq));
    resolver.terminate(resolver.emit<InstJe>(kNullLocation, 0, module.scalar.unit, equal, equalBlock, greaterTest));

    // Ordering has no payload, so each result is just its constructor index.
    auto returnOrdering = [&](ModulePtr<Block> block, U64 constructor) {
        resolver.current = block;
        auto value = resolver.makeInt(kNullLocation, ordering, constructor);
        resolver.terminate(resolver.emit<InstRet>(kNullLocation, 0, module.scalar.unit, value));
    };

    returnOrdering(equalBlock, 1);

    resolver.current = greaterTest;
    auto greater = resolver.ref(resolver.emit<InstCmp>(kNullLocation, 0, module.scalar.bool_, lhs, rhs, CompareOp::Gt));
    resolver.terminate(resolver.emit<InstJe>(kNullLocation, 0, module.scalar.unit, greater, greaterBlock, lessBlock));

    returnOrdering(greaterBlock, 2);
    returnOrdering(lessBlock, 0);

    return function - builder.local;
}

namespace {

// One method of a generated instance: the name and arity it has in the class, and what it
// expands to. Arity is part of the key because `Num` declares `-` twice.
struct CoreMethod {
    StringView name;
    U16 arity;
    Emit emit;
};

} // namespace

static void generateInstance(CoreBuilder& builder, GlobalPtr<TypeClass> classPointer, Buffer<TypePtr> args,
                             Buffer<CoreMethod> methods) {
    auto& module = builder.module;
    auto typeClass = builder.global[classPointer];

    auto instance = new (module.arena) ClassInstance(classPointer);
    instance->module = &module;
    for(auto arg: args) instance->forTypes.push(module.arena, arg);
    for(Size i = 0; i < typeClass->functions.size(); i++) instance->functions.push(module.arena, nullptr);

    for(auto& method: methods) {
        auto wanted = Context::nameHash(method.name);
        auto matched = false;

        for(Size i = 0; i < typeClass->functions.size(); i++) {
            auto entry = typeClass->functions.get(builder.global, i);
            if(entry.name != wanted || entry.arity != method.arity) continue;
            if(instance->functions.get(builder.local, i)) continue;

            instance->functions.set(builder.local, i,
                                    generateInstanceFunction(builder, *typeClass, args, U16(i), method.emit));
            matched = true;
            break;
        }

        assertTrue(matched);
    }

    // `compare` is the only class function no table entry above covers.
    for(Size i = 0; i < typeClass->functions.size(); i++) {
        if(instance->functions.get(builder.local, i)) continue;

        auto entry = typeClass->functions.get(builder.global, i);
        assertTrue(entry.name == Context::nameHash("compare", 7));
        instance->functions.set(builder.local, i, generateCompare(builder, *typeClass, args[0], U16(i)));
    }

    module.instances.push(instance - builder.local);
}

/*
 * The primitive instances.
 */

static void defineEq(CoreBuilder& builder, TypePtr type) {
    CoreMethod methods[] = {
        { "=="_v, 2, emitCompare<CompareOp::Eq> },
        { "!="_v, 2, emitCompare<CompareOp::Ne> },
    };

    generateInstance(builder, builder.classNamed("Eq"_v), { &type, 1 }, { methods, 2 });
}

static void defineOrd(CoreBuilder& builder, TypePtr type) {
    CoreMethod methods[] = {
        { "<"_v, 2, emitCompare<CompareOp::Lt> },
        { "<="_v, 2, emitCompare<CompareOp::Le> },
        { ">"_v, 2, emitCompare<CompareOp::Gt> },
        { ">="_v, 2, emitCompare<CompareOp::Ge> },
    };

    generateInstance(builder, builder.classNamed("Ord"_v), { &type, 1 }, { methods, 4 });
}

static void defineNum(CoreBuilder& builder, TypePtr type) {
    CoreMethod methods[] = {
        { "+"_v, 2, emitBinary<Value::Add> },
        { "-"_v, 2, emitBinary<Value::Sub> },
        { "*"_v, 2, emitBinary<Value::Mul> },
        { "/"_v, 2, emitBinary<Value::Div> },
        { "-"_v, 1, emitUnary<Value::Neg> },
    };

    generateInstance(builder, builder.classNamed("Num"_v), { &type, 1 }, { methods, 5 });
}

static void defineIntegral(CoreBuilder& builder, TypePtr type) {
    CoreMethod methods[] = {
        { "rem"_v, 2, emitBinary<Value::Rem> },
        { "%"_v, 2, emitBinary<Value::Rem> },
        { "shl"_v, 2, emitBinary<Value::Shl> },
        { "shr"_v, 2, emitBinary<Value::Shr> },
        { "sar"_v, 2, emitBinary<Value::Sar> },
        { "and"_v, 2, emitBinary<Value::And> },
        { "or"_v, 2, emitBinary<Value::Or> },
        { "xor"_v, 2, emitBinary<Value::Xor> },
        { "not"_v, 1, emitUnary<Value::Not> },
    };

    generateInstance(builder, builder.classNamed("Integral"_v), { &type, 1 }, { methods, 9 });
}

static void defineLogic(CoreBuilder& builder, TypePtr type) {
    CoreMethod methods[] = {
        { "&&"_v, 2, emitBinary<Value::And> },
        { "||"_v, 2, emitBinary<Value::Or> },
        { "and"_v, 2, emitBinary<Value::And> },
        { "or"_v, 2, emitBinary<Value::Or> },
        { "xor"_v, 2, emitBinary<Value::Xor> },
        { "not"_v, 1, emitLogicalNot },
        { "!"_v, 1, emitLogicalNot },
    };

    generateInstance(builder, builder.classNamed("Logic"_v), { &type, 1 }, { methods, 7 });
}

static void defineConversion(CoreBuilder& builder, StringView className, StringView method, TypePtr from, TypePtr to) {
    TypePtr args[] = { from, to };
    CoreMethod methods[] = { { method, 1, emitCast } };

    generateInstance(builder, builder.classNamed(className), { args, 2 }, { methods, 1 });
}

/*
 * Assembling the module.
 */

static TypePtr addPrimitive(Program& program, Module& module, StringView name, Type* value) {
    auto pointer = value - *program.types;
    module.namedTypes.add(module.context.addQualifiedName(name.ptr, name.length, 1), pointer);
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

    Lexer lexer(context, context.diagnostics, StringView { kCoreSource, stringLength(kCoreSource) });
    Parser parser(context, lexer, context.addQualifiedName("Core", 4, 1));
    auto ast = new ast::Module(parser.parseModule());

    auto module = program.addModule(ast->name, *ast->region);
    program.core = module;
    program.coreAst = ast;

    addPrimitive(program, *module, "Unit"_v, (Type*)(*program.types)[program.scalar.unit]);
    program.scalar.int_ = addPrimitive(program, *module, "Int"_v, new (program.types) IntType(32, IntType::Int, true));
    program.scalar.long_ = addPrimitive(program, *module, "Long"_v, new (program.types) IntType(64, IntType::Long, true));
    program.scalar.float_ = addPrimitive(program, *module, "Float"_v, new (program.types) FloatType(FloatType::Float));
    program.scalar.double_ = addPrimitive(program, *module, "Double"_v, new (program.types) FloatType(FloatType::Double));

    resolveModuleDecls(*module, *ast, nullptr);

    program.scalar.bool_ = coreType(*module, "Bool"_v);
    program.scalar.ordering = coreType(*module, "Ordering"_v);

    CoreBuilder builder(*module);

    TypePtr numeric[] = {
        program.scalar.int_,
        program.scalar.long_,
        program.scalar.float_,
        program.scalar.double_,
    };

    for(auto type: numeric) {
        defineEq(builder, type);
        defineOrd(builder, type);
        defineNum(builder, type);
    }

    defineIntegral(builder, program.scalar.int_);
    defineIntegral(builder, program.scalar.long_);

    defineEq(builder, program.scalar.bool_);
    defineLogic(builder, program.scalar.bool_);
    defineEq(builder, program.scalar.ordering);

    // Widening and narrowing are ordinary class operations, so a user type can join either
    // ladder later without the resolver learning anything new about conversion.
    for(Size from = 0; from < 4; from++) {
        for(Size to = 0; to < 4; to++) {
            if(from == to) continue;

            if(from < to) {
                defineConversion(builder, "Extend"_v, "extend"_v, numeric[from], numeric[to]);
            } else {
                defineConversion(builder, "Truncate"_v, "truncate"_v, numeric[from], numeric[to]);
            }
        }
    }
}
