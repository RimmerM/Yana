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
 * One member's signature - the class's declaration of it, at the instance's type.
 *
 * The same substitution `declareInstance` checks a written member against, and the same arg list
 * `specializeFunction` builds, because it is the same thing: a class member is generic in the
 * class's parameter and this is that member with the parameter bound. The name is what a written
 * instance member gets - `I32.+` - so that a dump and a symbol table read the same for both.
 */
DeclPtr<Function> Resolver::provideMember(intrinsic::Provided role, U16 member, Class& declared,
                                          Function& expected, DeclPtr<Type> type) {
    auto text = Tritium::format("%@.%@", typeText(context, decl(), type),
                                context.findName(expected.name));

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

    Substitution with { declared.gen, &type, 1 };

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

void Resolver::provideInstance(intrinsic::Provided role, DeclPtr<Class> klass, DeclPtr<Type> type) {
    auto& declared = *decl()[klass];

    auto instance = new (program.decl) Instance();
    instance->klass = klass;
    instance->source = declared.source;
    instance->module = here();
    instance->provided = role;
    instance->args.push(program.decl, type);

    for(Size i = 0; i < declared.members.size(); i++) instance->members.push(program.decl, nullptr);

    for(Size i = 0; i < declared.members.size(); i++) {
        if(!providesMember(role, U16(i))) continue;

        auto& expected = *decl()[declared.members.get(decl(), U32(i))];
        auto function = provideMember(role, U16(i), declared, expected, type);
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

        eachFamilyType(program, definition.family, [&](DeclPtr<Type> type) {
            provideInstance(role, klass, type);
        });
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
void Resolver::provideBody(Function& function, DeclPtr<Type> type) {
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

        auto value = expandProvided(builder, function.provided, function.providedMember, type,
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

        auto type = instance.args.get(decl(), 0);

        for(auto member: instance.members.contents(decl())) {
            if(!member) continue;
            provideBody(*decl()[member], type);
        }
    }
}

} // namespace mir
