#include "../resolve_internal.h"
#include "expand_internal.h"

namespace mir {

using namespace intrinsic;

/*
 * The instances the compiler generates - `provided.def`, as declarations in Core's module.
 *
 * Two passes, because a declaration and a body are known at two different times. `provideInstances`
 * runs once the classes have their members and before any written `instance` is read, so that a
 * program's `instance Ord2(I32)` finds the `Eq(I32)` its superclass needs already in the table.
 * `provideBodies` runs where every other body is resolved, after the types are classified: a body
 * reads its parameters, and how a value of a type is read is `readMode`'s answer, which depends on
 * the classification.
 *
 * What comes out is ordinary. The instance is in the same table under the same key as one an author
 * wrote; each member is a `Function` with a signature, a symbol and a body; selection, the summaries,
 * the erased witness tables and the SSA conversion see nothing special. The one thing that is
 * special - a concrete call expanding to the member's node rather than calling it - is `finishCall`'s
 * rule, and it reads the tag `Instance::provided` and nothing else about them.
 */

/*
 * The types a family names, as this compilation's handles - `Family`, where the sets are argued.
 *
 * The integers are `primitive.def`'s twelve: the eight fixed widths, `WideInt`, and the three the
 * target answers. Every one of them is a type of its own with instances of its own, which is what
 * `Types` materializing them separately is for - `Size` at sixty-four bits is not `I64`, and an
 * instance for the one is not an instance for the other.
 */
template<class F>
static void eachIntegerType(Types& types, F&& visit) {
    for(U32 isSigned = 0; isSigned < 2; isSigned++) {
        for(U32 width = 0; width < 4; width++) visit(types.ints[isSigned][width]);
    }

    visit(types.wideInt);
    visit(types.words[0]);
    visit(types.words[1]);
    visit(types.codeUnit);
}

template<class F>
static void eachFloatType(Types& types, F&& visit) {
    visit(types.floats[0]);
    visit(types.floats[1]);
}

template<class F>
static void eachFamilyType(Program& program, Family family, F&& visit) {
    auto& types = program.types;

    switch(family) {
        case Family::Numbers:
            eachIntegerType(types, visit);
            eachFloatType(types, visit);
            break;
        case Family::Integers:
            eachIntegerType(types, visit);
            break;
        case Family::Floats:
            eachFloatType(types, visit);
            break;
        case Family::Bits:
            eachIntegerType(types, visit);
            visit(booleanType(program));
            break;
        case Family::Truthy:
            eachIntegerType(types, visit);
            eachFloatType(types, visit);
            visit(booleanType(program));
            break;
        case Family::Comparable:
            eachIntegerType(types, visit);
            eachFloatType(types, visit);
            visit(booleanType(program));
            visit(asBase<Type>(program.roles.record(Known::ordering)));
            break;
    }
}

/*
 * The head argument lists a row generates an instance for - one per type, or one per pair.
 *
 * The pairing is `Heads`, and which pair joins which class is `intrinsic/convert.h`. Nothing here
 * decides either: this walks the family and asks, so that the ladder the compiler generates and the
 * ladder the shipping stage generates are the same set by construction rather than by agreement.
 *
 * A **pair** head is `(from, to)` in that order, which is the order `class Widen(a, b)` declares and
 * `fn widen(from: a) -> b` reads. The identity pair is in the narrowing set and only there - see
 * `conversionRung`, where the reason is written.
 */
template<class F>
static void eachHead(Program& program, Provided role, F&& visit) {
    auto& definition = providedDef(role);
    auto base = *program.decl;

    if(definition.heads == Heads::Each) {
        eachFamilyType(program, definition.family, [&](DeclPtr<Type> type) { visit(&type, 1); });
        return;
    }

    // The family as a list, because a pairing needs two nested walks over it and the visitor form
    // above gives one. Fourteen entries on every target there is.
    SmallArray<DeclPtr<Type>, 16> family;
    eachFamilyType(program, definition.family, [&](DeclPtr<Type> type) { family.push(type); });

    for(auto from: family) {
        auto source = numericOf(base, from);
        if(!source) continue;

        for(auto to: family) {
            auto destination = numericOf(base, to);
            if(!destination) continue;

            auto wanted = false;
            switch(definition.heads) {
                case Heads::Each:
                    break;
                case Heads::Widening:
                    wanted = conversionRung(source.unwrap(), destination.unwrap()) == Rung::Widen;
                    break;
                case Heads::Narrowing:
                    wanted = conversionRung(source.unwrap(), destination.unwrap()) == Rung::Narrow;
                    break;
                case Heads::Reinterpret:
                    wanted = from != to && reinterpretable(source.unwrap(), destination.unwrap());
                    break;
            }

            if(!wanted) continue;

            DeclPtr<Type> head[] = { from, to };
            visit(head, 2);
        }
    }
}

bool providedFor(Program& program, Provided role, DeclPtr<Type> type) {
    auto found = false;
    eachFamilyType(program, providedDef(role).family, [&](DeclPtr<Type> candidate) {
        if(candidate == type) found = true;
    });

    return found;
}

/*
 * Which members a generated instance supplies - every one the emitters have an answer for, and no
 * default the class already writes better.
 *
 * `Ord`'s `min` and `max` are the two left to the class: `if lhs <= rhs then lhs else rhs` at a
 * width is the comparison the instance supplies and a merge, and a generated body would be that
 * same body written a second time. The four comparisons and `compare` are supplied even though
 * three of the four have defaults, because at a primitive each is one instruction and the default
 * would be a call to `compare` and a comparison of what it answered.
 */
static bool providesMember(Provided role, U16 member) {
    if(role == Provided::ord) return member <= kOrdCompare;
    return true;
}

/*
 * One member's signature - the class's declaration of it, at the instance's types.
 *
 * The same substitution `declareInstance` checks a written member against, and the same arg list
 * `specializeFunction` builds, because it is the same thing: a class member is generic in the
 * class's parameters and this is that member with them bound. The name is what a written instance
 * member gets - `I32.+` - so that a dump and a symbol table read the same for both.
 *
 * **Every head argument is in the name**, which matters only for the ladders and matters there a
 * great deal: `Widen(I32, I64)` and `Widen(I32, F32)` would otherwise both be called `I32.widen`,
 * and a dump naming thirteen functions the same thing is a dump nobody can read. `uniqueSymbol`
 * would still have made the symbols differ, by a suffix nothing in the source explains.
 */
DeclPtr<Function> Resolver::provideMember(intrinsic::Provided role, U16 member, Class& declared,
                                          Function& expected, const DeclPtr<Type>* head,
                                          Size heads, DeclPtr<GenContext> gen) {
    Tritium::StringBuilder spelling;
    for(Size i = 0; i < heads; i++) {
        spelling.append(typeText(context, decl(), head[i]));
        spelling.append(".", 1);
    }

    spelling.append(context.findName(expected.name));
    auto text = Tritium::String(spelling.text(), spelling.size());

    // The bytes are copied into the declaration arena first, because **an interned name borrows its
    // text** - see `declareInstance`, which does the same for the same reason.
    auto stored = (char*)program.decl.alloc(text.size());
    copy(text.text(), stored, text.size());
    auto name = context.addUnqualifiedName(stored, text.size());

    auto function = new (program.decl) Function(name, here());
    auto pointer = function - *program.decl;

    function->symbol = program.uniqueSymbol(here(), context, StringView(stored, text.size()));
    function->source = expected.source;
    function->provided = role;
    function->providedMember = member;
    function->summary.bodyKnown = true;

    /*
     * The **instance's** context, where the head is a shape rather than a type.
     *
     * `Eq(%a)`'s members are generic functions over the head's `a` exactly as a written `instance
     * (Eq(a)) Eq(Opt(a))`'s are, so what runs for `Eq(%I32)` is a specialization - see
     * `declareInstance`, which passes the same context in for the same reason. Null for every ground
     * head, which is every row of `provided.def`.
     */
    function->gen = gen;

    // The cast is `holdsContext`'s in select.cpp and is there for the same reason: a `Substitution`
    // is a *view* of an argument list it does not own, and const-correctness of that view is not
    // something the region pointers express.
    Substitution with { declared.gen, (DeclPtr<Type>*)head, heads };

    for(auto argumentPointer: expected.args.contents(decl())) {
        auto& written = *decl()[argumentPointer];

        auto substituted = new (program.decl) Arg();
        substituted->name = written.name;
        substituted->source = written.source;
        substituted->index = written.index;
        substituted->occurrence = substituteOccurrence(context, program, written.occurrence, with);

        function->args.push(program.decl, substituted - *program.decl);
    }

    function->result = substituteOccurrence(context, program, expected.result, with);

    decl()[here()]->functions.push(program.decl, pointer);
    return pointer;
}

void Resolver::provideInstance(intrinsic::Provided role, DeclPtr<Class> klass,
                               const DeclPtr<Type>* head, Size heads, DeclPtr<GenContext> gen) {
    auto& declared = *decl()[klass];

    auto instance = new (program.decl) Instance();
    instance->klass = klass;
    instance->source = declared.source;
    instance->module = here();
    instance->provided = role;
    instance->gen = gen;
    for(Size i = 0; i < heads; i++) instance->args.push(program.decl, head[i]);

    for(Size i = 0; i < declared.members.size(); i++) instance->members.push(program.decl, nullptr);

    for(Size i = 0; i < declared.members.size(); i++) {
        if(!providesMember(role, U16(i))) continue;

        auto& expected = *decl()[declared.members.get(decl(), U32(i))];
        auto function = provideMember(role, U16(i), declared, expected, head, heads, gen);
        instance->members.set(decl(), U32(i), function);
    }

    auto pointer = instance - *program.decl;
    current->instances.push(program.decl, pointer);

    // Two generated heads of one shape would be two rows of `provided.def` naming one class over
    // overlapping families, which is a mistake in that table rather than anything a program did.
    auto added = program.addInstance(decl(), pointer);
    assertTrue(added);
}

void Resolver::provideInstances() {
    ModuleScope scope(*this, program.core, 0);

    for(Size i = 0; i < Size(Provided::Count); i++) {
        auto role = Provided(i);
        auto& definition = providedDef(role);

        // `checkDeclaredRoles` has already reported a `Required` class nothing declared, and has
        // stopped the compilation; an `Optional` one the library left out generates nothing.
        auto klass = program.roles.klass(definition.klass);
        if(!klass) continue;

        eachHead(program, role, [&](const DeclPtr<Type>* head, Size heads) {
            provideInstance(role, klass, head, heads);
        });
    }

    providePointerInstances();
}

/*
 * The instances whose head is a **shape** - `Eq(%a)`, `Ord(%a)`, and the reinterpretation rungs.
 *
 * Not rows of `provided.def`, and the reason is that table's own shape: a row says which *family of
 * types* a class is generated over, and a pointer instance is generated over none of them - there is
 * one head, `%a`, and it is a type variable. The shipping stage draws the same line in the same
 * place, with the same set of instances, in `definePointerInstances`.
 *
 * **What comes out is a parametric instance**, which is the only reason this is short: the head binds
 * `a`, each member is an ordinary generic function over it, and selection is `matchType` producing
 * the argument. Nothing below this line needed a mechanism for a head that is a shape - `Instance::gen`
 * has existed since a program could write `instance (Eq(a)) Eq(Opt(a))`.
 *
 * The **reinterpretation** rungs are what `cast`, `asInt` and `asPtr` are not: one operation with one
 * name. Pointer-to-pointer needs a context of *two* variables, because the two sides are unrelated
 * and that is the whole content of the operation; the address rungs reuse the single-variable one,
 * since the other side is the target's word.
 *
 * `Size` **and** `USize`, and both directions of each. An address has no sign - nothing subtracts one
 * pointer from a smaller one and expects a negative address - so a layer that wants to say "machine
 * word, unsigned" about one would otherwise launder it through the signed rung and back, which is two
 * casts stating something neither of them meant. The system-call layer is where that shows: every
 * argument a kernel takes is a word.
 *
 * What is *not* generated is `Num`. Adding two addresses is not an operation, and `p + n` is
 * `pointerAdd` - an intrinsic whose scale is the pointee's stride, which is what makes it correct and
 * what a `Num(%a)` instance could not express.
 */
void Resolver::providePointerInstances() {
    ModuleScope scope(*this, program.core, 0);

    // One variable, and the pointer over it. `open` is false: the context is complete the moment it
    // is made, because nothing reads a written head to add to it.
    auto single = new (program.decl) GenContext(false);
    auto singleGen = single - *program.decl;

    auto aName = context.addUnqualifiedName("a", 1);
    auto a = new (program.decl) VariableType(singleGen, 0, aName, kNullLocation);
    single->variables.push(program.decl, (Type*)a - *program.decl);

    auto pointerA = program.types.pointerType((Type*)a - *program.decl);

    auto provide = [&](Known klass, Provided role, const DeclPtr<Type>* head, Size heads,
                       DeclPtr<GenContext> gen) {
        auto found = program.roles.klass(klass);
        if(found) provideInstance(role, found, head, heads, gen);
    };

    provide(Known::eq, Provided::eq, &pointerA, 1, singleGen);
    provide(Known::ord, Provided::ord, &pointerA, 1, singleGen);

    // Two variables, for the one rung whose sides are unrelated.
    auto pair = new (program.decl) GenContext(false);
    auto pairGen = pair - *program.decl;

    auto bName = context.addUnqualifiedName("b", 1);
    auto from = new (program.decl) VariableType(pairGen, 0, aName, kNullLocation);
    auto to = new (program.decl) VariableType(pairGen, 1, bName, kNullLocation);
    pair->variables.push(program.decl, (Type*)from - *program.decl);
    pair->variables.push(program.decl, (Type*)to - *program.decl);

    DeclPtr<Type> between[] = { program.types.pointerType((Type*)from - *program.decl),
                                program.types.pointerType((Type*)to - *program.decl) };
    provide(Known::bitcast, Provided::bitcast, between, 2, pairGen);

    for(auto word: program.types.words) {
        DeclPtr<Type> out[] = { pointerA, word };
        DeclPtr<Type> in[] = { word, pointerA };

        provide(Known::bitcast, Provided::bitcast, out, 2, singleGen);
        provide(Known::bitcast, Provided::bitcast, in, 2, singleGen);
    }
}

/*
 * `compare` - the one generated body that is not one node. See `providedExpands`.
 *
 *      bb0: %lt = lt lhs rhs; branch %lt bb1 bb2
 *      bb1: return LT
 *      bb2: %eq = eq lhs rhs; branch %eq bb3 bb4
 *      bb3: return EQ
 *      bb4: return GT
 *
 * The parameters are read afresh in each block, because they are places and a read is a value of
 * the block it was made in - which is also why nothing here needs the resolver's carry machinery.
 * The constants are constructor *indices* of `Ordering`, in the order `known.def` states the
 * declaration numbers them: less, equal, greater.
 */
void Resolver::compareBody(Builder& builder, Function& function, Local& lhs, Local& rhs) {
    auto source = function.source;
    auto ordering = asBase<Type>(program.roles.record(Known::ordering));

    auto answer = [&](U16 constructor) {
        builder.addReturn(source, *builder.addConst(source, enumConstant(ordering, constructor)));
    };

    auto test = [&](BinaryOp op, Block& whenTrue, Block& whenFalse) -> bool {
        auto left = readPlace(builder, source, *builder.localPlace(lhs), inspectRequest());
        auto right = readPlace(builder, source, *builder.localPlace(rhs), inspectRequest());
        if(!left || !right) return false;

        auto condition = builder.addBinary(source, op, *left, *right);
        builder.addBranch(source, builder.resultOf(*condition, UseMode::Inspect), whenTrue,
                          whenFalse, nullptr, 0);
        return true;
    };

    auto less = builder.addBlock(StringId());
    auto notLess = builder.addBlock(StringId());
    auto equal = builder.addBlock(StringId());
    auto greater = builder.addBlock(StringId());

    if(!test(BinaryOp::Lt, *less, *notLess)) return;

    builder.setBlock(*less);
    answer(0);

    builder.setBlock(*notLess);
    if(!test(BinaryOp::Eq, *equal, *greater)) return;

    builder.setBlock(*equal);
    answer(1);

    builder.setBlock(*greater);
    answer(2);
}

/*
 * One member's body - the parameters read, the member's node over them, and a return.
 *
 * The same `expandProvided` a concrete call site reaches, which is the whole of the design: what a
 * member computes is stated once, and this is the placement that gives it an address.
 */
void Resolver::provideBody(Function& function, const DeclPtr<Type>* head, Size heads) {
    Builder builder(context, program, function);
    currentFunction = &function;
    bindings.clear();
    continuationLocal = nullptr;
    yieldedLocal = nullptr;
    guardYields = false;
    handovers = 0;

    auto source = function.source;

    SmallArray<Local*, 4> locals;
    for(auto argumentPointer: function.args.contents(decl())) {
        locals.push(builder.addParameter(*decl()[argumentPointer]));
    }

    if(!providedExpands(function.provided, function.providedMember)) {
        compareBody(builder, function, *locals[0], *locals[1]);
    } else {
        SmallArray<Node*, 4> arguments;
        for(auto local: locals) {
            auto value = readPlace(builder, source, *builder.localPlace(*local), inspectRequest());
            if(!value) return;

            arguments.push(value);
        }

        auto value = expandProvided(builder, function.provided, function.providedMember, head, heads,
                                    arguments.pointer(), arguments.size(), source);
        builder.addReturn(source, *value);
    }

    builder.seal();
    currentFunction = nullptr;
}

void Resolver::provideBodies() {
    ModuleScope scope(*this, program.core, 0);

    for(auto instancePointer: decl()[program.core]->instances.contents(decl())) {
        auto& instance = *decl()[instancePointer];
        if(instance.provided == Provided::Count) continue;

        SmallArray<DeclPtr<Type>, 2> head;
        for(auto argument: instance.args.contents(decl())) head.push(argument);

        for(auto member: instance.members.contents(decl())) {
            if(!member) continue;
            provideBody(*decl()[member], head.pointer(), head.size());
        }
    }
}

} // namespace mir
