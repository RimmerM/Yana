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
        case Family::Machine:
            eachIntegerType(types, [&](DeclPtr<Type> type) {
                if(hasMachineWidth(*program.decl, type)) visit(type);
            });
            break;
        case Family::Swappable:
            eachIntegerType(types, [&](DeclPtr<Type> type) {
                if(hasByteOrder(*program.decl, type)) visit(type);
            });
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

        // No type at all: the heads are shapes and a generator supplies them - see `Family::Shapes`,
        // and `provideVectorInstances`, which is the generator.
        case Family::Shapes:
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

    // The family paired with `Bool` - a scalar's mask, and the only constant a pairing adds here.
    if(definition.heads == Heads::Masked) {
        eachFamilyType(program, definition.family, [&](DeclPtr<Type> type) {
            DeclPtr<Type> head[] = { type, booleanType(program) };
            visit(head, 2);
        });

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
                // Both answered above, where a head is one type or one pairing rather than a
                // relation over the family - so nothing is a *pair* of these.
                case Heads::Each:
                case Heads::Masked:
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

/*
 * The instances whose head is a **vector** - Design-Vector §3, and `providePointerInstances`' terms
 * one row over.
 *
 * Not rows of `provided.def` for that function's exact reason: a row says which *family of types* a
 * class is generated over, and there is one vector head - `Vec(a, n)` - and its arguments are a type
 * variable and a const variable. "Every vector" is not an enumerable set: a lane is any machine
 * primitive and a lane count is the target's, so a table row would have to be a loop over a product
 * the target decides.
 *
 * **The head's own context is what makes one parametric instance correct.** `Num(Vec(a, n))` holds
 * exactly where `Num(a)` does, so the context is that requirement and `Vec(F32)` gets arithmetic
 * while a vector of something with none would not - and there is no such vector anyway, since
 * `makeVectorType` refuses a lane that is not a machine primitive. Every row below reads that way:
 * the class of the *vector* requires the same class of the *lane*, and `holdsContext` inside
 * selection discharges it at whatever the head bound.
 *
 * Four things are deliberately absent:
 *
 *  - **`Eq` and `Ord`**, and they must stay absent. A comparison of two vectors answers a *mask*
 *    rather than a `Bool` (§3.1), so neither class describes what comparing vectors is; generating
 *    them would typecheck and emit a comparison whose result the verifier refuses. `Lanewise` is
 *    what does describe it.
 *  - **`Num` and the rest over a mask.** Its lanes are all-ones or all-zeros and every operation
 *    `Num` names would produce something that is neither. `Bitwise` is the one class both a mask and
 *    an integer vector join, which is what separates it from `Integral`.
 *  - **`Widen` and `Narrow` over a mask**, and the two directions are absent for different reasons.
 *    Narrowing one is meaningful - truncation carries all-ones to all-ones - and is blocked on the
 *    pack instruction that would emit it; widening is *not* meaningful, since a zero-extended
 *    `0xFFFF` is neither of a mask's two patterns. A class whose two directions fail differently is
 *    worth leaving empty until the operation that changes a mask's lane width is written as the pack
 *    it is.
 *  - **`Truth`.** A condition wants one answer and a mask has one per lane; `any`, `all`, `none` and
 *    `count` are how a lane answer becomes a scalar one.
 */
void Resolver::provideVectorInstances() {
    ModuleScope scope(*this, program.core, 0);

    auto aName = context.addUnqualifiedName("a", 1);
    auto bName = context.addUnqualifiedName("b", 1);
    auto nName = context.addUnqualifiedName("n", 1);
    auto mName = context.addUnqualifiedName("m", 1);

    /*
     * One fresh context per instance, and it has to be fresh: the head's requirement lives on the
     * context, so two classes sharing one would each be asked to prove the other's.
     *
     * The `counts` variables are **const** parameters annotated `I32`, which is what `matchType`
     * binds a written count against - `Vec(F32, 4)` binds one to a `ConstType(4, I32)`, and the
     * annotation is where that type comes from. Two of them is the reinterpretation rung, whose two
     * ends may differ in count.
     */
    auto makeContext = [&](Size types, Size counts) {
        auto gen = new (program.decl) GenContext(false);
        auto pointer = gen - *program.decl;

        StringId lanes[] = { aName, bName };
        StringId widths[] = { nName, mName };

        for(Size i = 0; i < types; i++) {
            auto variable = new (program.decl) VariableType(pointer, U16(i), lanes[i], kNullLocation);
            gen->variables.push(program.decl, (Type*)variable - *program.decl);
        }

        for(Size i = 0; i < counts; i++) {
            auto count = new (program.decl) VariableType(pointer, U16(types + i), widths[i],
                                                         kNullLocation);
            count->constType = program.types.intType(32, true);
            gen->variables.push(program.decl, (Type*)count - *program.decl);
        }

        return pointer;
    };

    auto variableAt = [&](DeclPtr<GenContext> gen, Size index) {
        return decl()[gen]->variables.get(decl(), U32(index));
    };

    // `Vec(a, n)` or `Mask(a, n)` out of one context's variables, named by position. Never fails: a
    // variable lane defers every check `makeVectorType` makes, which is the arm this whole function
    // stands on.
    auto shapeAt = [&](DeclPtr<GenContext> gen, Size lane, Size count, bool isMask) {
        return makeVectorType(context, program, variableAt(gen, lane), variableAt(gen, count), 0,
                              isMask, kNullLocation);
    };

    // The common case: one lane variable and the context's one count.
    auto shapeOf = [&](DeclPtr<GenContext> gen, Size lane, bool isMask) {
        return shapeAt(gen, lane, decl()[gen]->variables.size() - 1, isMask);
    };

    // The head's own requirement - `(Num(a))` on `Num(Vec(a, n))`, which is the sentence this whole
    // function rests on. `args` is a list this context owns, so it survives the instance.
    auto require = [&](DeclPtr<GenContext> gen, Known klass, const DeclPtr<Type>* args, Size count) {
        auto found = program.roles.klass(klass);
        if(!found) return false;

        GenRequirement requirement;
        requirement.klass = found;
        requirement.source = kNullLocation;
        for(Size i = 0; i < count; i++) requirement.args.push(program.decl, args[i]);

        decl()[gen]->classes.push(program.decl, requirement);
        return true;
    };

    /*
     * One class over `Vec(a, n)`, requiring the same class of the lane.
     *
     * The five rows this serves - `FromInt`, `FromDecimal`, `Num`, `Bitwise`, `Integral` - are
     * exactly the classes whose members are one instruction per lane, which is what makes the
     * requirement the right one: an `add` of two vectors is the lane's `add`, `n` times over.
     */
    auto overLanes = [&](Known klass, Provided role) {
        auto found = program.roles.klass(klass);
        if(!found) return;

        auto gen = makeContext(1, 1);
        auto lane = variableAt(gen, 0);
        if(!require(gen, klass, &lane, 1)) return;

        auto head = shapeOf(gen, 0, false);
        if(head) provideInstance(role, found, &head, 1, gen);
    };

    overLanes(Known::fromInt, Provided::fromInt);
    overLanes(Known::fromDecimal, Provided::fromDecimal);
    overLanes(Known::num, Provided::num);
    overLanes(Known::bitwise, Provided::bitwise);
    overLanes(Known::integral, Provided::integral);

    /*
     * `Bits(Vec(a, n))` - Analysis-Avx512.md §3.2, and the one row here whose requirement is *not*
     * the class it generates.
     *
     * `Integral(a)` and not `Bits(a)`, which is the whole content of this block. The scalar class is
     * instanced at thirty-two and sixty-four bits alone (`Family::Machine`), and the reason is stated
     * in `provided.def`: a `U8` is eight bits of a wider register, so a count of its *storage* and a
     * count of its value are different numbers. **A lane is not.** A `Vec(U8, 16)`'s lane is exactly
     * eight bits wide with nothing above it, so every count this class names has the same answer at
     * a byte lane that it has at a word - and the machine agrees, `vpopcntb` being an instruction and
     * `popcnt r8` not. Requiring the lane's own `Bits` would have given the vector class the scalar
     * one's exclusions for a reason that does not survive being put in a lane.
     *
     * So the requirement is the widest one that still says "these lanes are integers with shifts",
     * which is `Integral(a)` - the superclass `Bits` itself declares. A float lane has no instance
     * here and neither does a mask: a mask's lane is all-ones or all-zeros and a population count of
     * one would be its lane width or zero, which is `count` said expensively.
     */
    if(auto bits = program.roles.klass(Known::bits)) {
        auto gen = makeContext(1, 1);
        auto lane = variableAt(gen, 0);

        if(require(gen, Known::integral, &lane, 1)) {
            auto head = shapeOf(gen, 0, false);
            if(head) provideInstance(Provided::bits, bits, &head, 1, gen);
        }
    }

    /*
     * `Bitwise(Mask(a, n))` - Design-Vector §3.2's "masks themselves get `and`, `or` and `not`".
     *
     * **No context**, unlike every row above: a mask's four operations are defined on its own two
     * patterns rather than on the lane it was made from, so `Mask(F32)` has them although `F32` has
     * no `Bitwise` at all. That is also why `not` here is the complement of every bit and not
     * `Bool`'s `xor 1` - a mask lane is all-ones or all-zeros, so complementing it lands back inside
     * the type where a single flipped bit would not.
     */
    if(auto bitwise = program.roles.klass(Known::bitwise)) {
        auto gen = makeContext(1, 1);
        auto head = shapeOf(gen, 0, true);
        if(head) provideInstance(Provided::bitwise, bitwise, &head, 1, gen);
    }

    /*
     * `Lanewise(Vec(a, n), Mask(a, n))` - the class that says what comparing two vectors is.
     *
     * The functional dependency is what the second position is for: `a .< b` infers its own result
     * without an ascription, so a call asks with a hole where the mask goes and reads back what the
     * head put there. No context, for the mask row's reason one level up - the six comparisons are
     * the machine's at any lane, and `select` is a choice per lane and not arithmetic.
     */
    if(auto lanewise = program.roles.klass(Known::lanewise)) {
        auto gen = makeContext(1, 1);
        DeclPtr<Type> head[] = { shapeOf(gen, 0, false), shapeOf(gen, 0, true) };
        if(head[0] && head[1]) provideInstance(Provided::lanewise, lanewise, head, 2, gen);
    }

    /*
     * The conversion ladder over vectors - Design-Vector §3.4, and the claim it makes is that a
     * vector conversion is not a vector operation: it is `Widen`, `Narrow` and `Bitcast` under the
     * rules the language already has.
     *
     * What makes that true rather than approximately true is the head's context: `Widen(Vec(a, n),
     * Vec(b, n))` requires `Widen(a, b)`, so the rung exists over vectors exactly where it exists
     * over the lanes and there is no second ladder to drift from the first. **The lane count is one
     * variable in both positions**, which is the other half of the rule: a conversion relates one
     * value to one value, and one that changed the count is `unpackLow`/`packLanes` and is a
     * function because it relates one vector to two or two to one.
     *
     * `Bitcast` gets a **mask** rung of its own, at one shape: two masks of one lane width and one
     * lane count are the same bits under two names, which is the language's ordinary spelling for a
     * reinterpretation and emits nothing. It is written as one head over one lane variable for that
     * reason - a pair of variables would admit two masks whose lanes do not correspond.
     */
    auto ladder = [&](Known klass, Provided role) {
        auto found = program.roles.klass(klass);
        if(!found) return;

        auto gen = makeContext(2, 1);
        DeclPtr<Type> lanes[] = { variableAt(gen, 0), variableAt(gen, 1) };
        if(!require(gen, klass, lanes, 2)) return;

        DeclPtr<Type> head[] = { shapeOf(gen, 0, false), shapeOf(gen, 1, false) };
        if(head[0] && head[1]) provideInstance(role, found, head, 2, gen);
    };

    ladder(Known::widen, Provided::widen);
    ladder(Known::narrow, Provided::narrow);

    /*
     * The reinterpretation rungs, which are the two heads whose shape a **context cannot state**.
     *
     * A `Bitcast` over vectors keeps the *byte width* and is otherwise free, so `i8x16` to `i32x4`
     * is a rung and `i8x16` to `i32x8` is not - and that is an arithmetic relation between two lane
     * widths and two counts, which no requirement of a head can express. So the head is open in all
     * four positions and the **emitter** is what holds a pair to it, at the call, where the two
     * shapes are concrete and can be named in the message. The shipping stage checks the same thing
     * in the same place for the same reason: its generator asks the widths rather than a head.
     *
     * The **mask** rung is the same shape with the count shared. Two masks of one lane count and one
     * lane width are the same bits under two names; two of one byte width but different lane shapes
     * - an `m8x16` and an `m32x4` - have no corresponding lanes at all, so reinterpreting one as the
     * other would be a repacking rather than a renaming. Sharing `n` states half of that and the
     * emitter states the other half.
     */
    if(auto bitcast = program.roles.klass(Known::bitcast)) {
        auto vectors = makeContext(2, 2);
        DeclPtr<Type> between[] = { shapeAt(vectors, 0, 2, false), shapeAt(vectors, 1, 3, false) };
        if(between[0] && between[1]) provideInstance(Provided::bitcast, bitcast, between, 2, vectors);

        auto masks = makeContext(2, 1);
        DeclPtr<Type> shapes[] = { shapeAt(masks, 0, 2, true), shapeAt(masks, 1, 2, true) };
        if(shapes[0] && shapes[1]) provideInstance(Provided::bitcast, bitcast, shapes, 2, masks);
    }
}

/*
 * The instances whose head is a **payload-free sum** - one `Enum`, one `Eq` and one `Ord` per `data`.
 *
 * A generator rather than rows of `provided.def`'s families for `provideVectorInstances`' reason at
 * a different kind of head: "every payload-free sum in the program" is a walk over the declarations
 * and not a set of primitives the target has. What the table rows buy is the same thing they buy
 * there - a `Provided` role for the emitters to be indexed by.
 *
 * **Run after every declaration is read**, which is where this parts company with the two generators
 * above and with the shipping stage. Those run before anything is declared, so a written instance at
 * the same head is a duplicate found later; this runs after, so it can simply *skip* a head the table
 * already holds. That is what keeps a program's own `instance Eq(Colour)` winning - the rule the
 * shipping stage gets from consulting its hooks only on a failed lookup, stated here as a condition
 * instead of as an ordering.
 *
 * Three classes, and the three are not the same kind of statement:
 *
 *  - **`Enum`** is generated for every enum unconditionally, because nobody may write one: what all
 *    four members answer is decided by the `data` declaration, which is `lib/Core/Enum.yana`'s own
 *    ruling and the reason the class exists.
 *  - **`Eq` and `Ord`** are generated because the boilerplate they replace was one line per enum and
 *    the line was always `valueOf(lhs) == valueOf(rhs)`. A **generated instance** and not a
 *    `structuralAnswer` beside `TrivialCopy`, and the choice is forced rather than stylistic: a
 *    structural answer names no instance, which is sound only for a class with no members, and both
 *    of these have members a generic body dispatches through - an erased witness table needs a real
 *    function with a real body to put in the slot.
 *
 * **`Bool` and `Ordering` are left out**, which is the one exception and it is not this function's
 * ruling: they are `provided.def`'s, whose `Comparable` row gives both an `Eq` and whose `Numbers`
 * row deliberately gives neither an `Ord` - "a `Bool` has no order the language states, and
 * `Ordering`'s would be a statement about three words". Answering them here would overturn a
 * decision written down one directory up.
 *
 * A **generic** enum's instances are its instantiations', and only the ones that exist by now: an
 * instantiation minted later, while a body is being resolved, gets none. Nothing in the corpus or in
 * `lib/` writes a generic payload-free sum, and the fix when one does is the same walk run again
 * where `completeInstantiations` runs.
 */
void Resolver::provideEnumInstances() {
    ModuleScope scope(*this, program.core, 0);

    auto base = *program.decl;
    auto enumeration = program.roles.klass(Known::enumeration);
    auto boolean = program.roles.record(Known::boolean);
    auto ordering = program.roles.record(Known::ordering);

    // A head nothing has claimed yet. The table is complete by now, so this is the whole of "a
    // declared instance wins" - see above.
    auto unclaimed = [&](DeclPtr<Class> klass, const DeclPtr<Type>* args, Size count) {
        SmallArray<DeclPtr<Instance>, 4> found;
        program.keyedInstances(base, klass, args, count, found);
        return found.size() == 0;
    };

    auto provideAt = [&](DeclPtr<RecordType> record) {
        auto& declared = *base[record];
        if(declared.gen || declared.layout != RecordLayout::Enum) return;
        if(!declared.cons.size()) return;

        auto type = asBase<Type>(record);

        if(enumeration) {
            DeclPtr<Type> head[] = { type, enumValueTypeOf(base, program.types, declared) };
            if(unclaimed(enumeration, head, 2)) {
                provideInstance(Provided::enumeration, enumeration, head, 2);
            }
        }

        /*
         * Its `Show`, which is `nameOf` written out - `provided.def`'s row at the end.
         *
         * **Above the `Bool`/`Ordering` line**, and that is the one asymmetry here: those two are
         * left out of `Eq` and `Ord` because `provided.def`'s own rows answer them - a `Bool` has no
         * order the language states - and neither row says anything about text. An `Ordering` reads
         * `Less`, `Equal`, `Greater` like any other enum, and a `Bool` has a `Show` somebody wrote,
         * which `unclaimed` is already what defers to.
         *
         * Both roles asked, because the body needs both: the class to hang the instance on, and the
         * append the body is. A library that declares neither generates nothing here, and a format hole
         * then reports the missing role by name.
         */
        auto shown = program.roles.klass(Known::show);
        if(shown && program.roles.function(Known::stringPush) && unclaimed(shown, &type, 1)) {
            provideInstance(Provided::show, shown, &type, 1);
        }

        if(record == boolean || record == ordering) return;

        auto compare = [&](Known role, Provided provided) {
            auto klass = program.roles.klass(role);
            if(klass && unclaimed(klass, &type, 1)) provideInstance(provided, klass, &type, 1);
        };

        compare(Known::eq, Provided::eq);
        compare(Known::ord, Provided::ord);
    };

    for(auto modulePointer: program.modules) {
        for(auto recordPointer: decl()[modulePointer]->records.contents(decl())) provideAt(recordPointer);
    }
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
    provideVectorInstances();
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
 * The source-level `bitcast` rows for raw function pointers.
 *
 * Raw function types are not an enumerable primitive family, so they cannot be generated by
 * `provide.def` before declarations are read. The declarations are complete by the time this pass
 * runs; one concrete row per explicitly conventioned function type makes the ordinary Bitcast
 * expansion available without admitting bitcasts from arbitrary values to integers.
 */
void Resolver::provideRawFunctionPointerInstances() {
    ModuleScope scope(*this, program.core, 0);
    auto bitcast = program.roles.klass(Known::bitcast);
    if(!bitcast) return;

    Array<U32> seen;
    for(auto modulePointer: program.modules) {
        for(auto functionPointer: decl()[modulePointer]->functions.contents(decl())) {
            auto& function = *decl()[functionPointer];
            if(!function.convention || isGeneric(decl(), function.gen) || function.lens ||
               function.iterator) {
                continue;
            }

            auto type = funTypeOf(context, program, functionPointer);
            if(!type) continue;

            auto duplicate = false;
            for(auto offset: seen) duplicate |= offset == type.offset;
            if(duplicate) continue;
            seen.push(type.offset);

            SmallArray<DeclPtr<Type>, 3> words;
            for(auto word: program.types.words) words.push(word);

            // On x64, U64 is another source spelling for the one machine word that holds a code
            // address. Keep it target-conditional: a 32-bit target must not gain a reinterpretation
            // that silently truncates or widens a function pointer.
            if(targetIntWidths(context.settings).word == 64) {
                words.push(program.types.intType(64, false));
            }

            for(auto word: words) {
                DeclPtr<Type> head[] = { type, word };
                provideInstance(Provided::bitcast, bitcast, head, 2);
            }
        }
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

    /*
     * At an **enum** head the operands are read as their tag first - the same conversion
     * `expandProvided` makes for the four relational members, and for the same reason: an ordering
     * is arithmetic and `operandTypeAllowed` admits no record to it. A payload-free sum's tag type is
     * signed exactly where the declaration pins a negative number, so this compares at the width and
     * the signedness the declaration gave the values.
     */
    auto operandOf = [&](Local& local) -> Node* {
        auto value = readPlace(builder, source, *builder.localPlace(local), inspectRequest());
        if(!value) return nullptr;

        auto tag = tagTypeOf(decl(), program.types, local.occurrence.type);
        return tag ? builder.addBitcast(source, *value, tag) : value;
    };

    auto test = [&](BinaryOp op, Block& whenTrue, Block& whenFalse) -> bool {
        auto left = operandOf(lhs);
        auto right = operandOf(rhs);
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
 * The two constructors a generated member builds its answer with - found by **name** off the result
 * type the class declared.
 *
 * By name, because what `Maybe` is stays the library's: the compiler is told which type the member
 * answers by the class's own signature and reads the present and absent cases out of it, rather than
 * holding a handle to a declaration `lib/` is free to move. A result that is not a two-case record is
 * a library whose `Enum` has drifted, and saying so names the type it found.
 */
bool Resolver::maybeConstructors(DeclPtr<Type> answer, LocationId source, StringView member,
                                 U16& just, U16& nothing) {
    if(!answer || decl()[answer]->kind != Type::Record) {
        context.diagnostics.error("%@ must answer a record with a present and an absent case, and this answers %@"_v,
                                  source, member, typeText(context, decl(), answer));
        return false;
    }

    auto& maybe = *(RecordType*)decl()[answer];
    auto constructorNamed = [&](StringView name) -> Maybe<U16> {
        auto wanted = context.addUnqualifiedName(name);
        for(U16 i = 0; i < U16(maybe.cons.size()); i++) {
            if(maybe.cons.get(decl(), i).name == wanted) return Just(i);
        }

        return Nothing();
    };

    auto present = constructorNamed("Just"_v);
    auto absent = constructorNamed("Nothing"_v);
    if(!present || !absent) {
        context.diagnostics.error("%@ answers %@, which has no Just and Nothing for the compiler to build"_v,
                                  source, member, typeText(context, decl(), answer));
        return false;
    }

    just = present.unwrap();
    nothing = absent.unwrap();
    return true;
}

/*
 * `fromValue` - the partial half of `Enum`, and the one generated body that branches.
 *
 *      bb0: %in = membership(number); branch %in bb1 bb2
 *      bb1: %tag = convert number -> the tag's type; %e = bitcast %tag -> the enum
 *           return Just(%e)
 *      bb2: return Nothing
 *
 * **The membership test has two shapes and which one applies is a property of the declaration.**
 * Values that run consecutively are a range, which is two comparisons however many constructors
 * there are - and that is every enum that pins nothing, so the common case does not pay for the
 * feature. A set with holes is one comparison per constructor, or-ed together: no branches, and an
 * expression the optimizer is free to make a table of.
 *
 * The `Just` payload is **two** nodes and not one, which is the mistake the shipping stage made
 * here and corrected: `cast I64 -> a payload-free sum` is a narrowing *and* a reinterpretation
 * stacked on each other, and asking one node for both leaves a conversion whose target has no width
 * to convert to. Split, each half is a shape that already exists - a narrowing between two integers,
 * and a renaming between two things of one width, which is what `UnaryCategory::Raw` admits an enum
 * for. The narrowing is to the **tag's** own type, so a wide `@value` is not truncated on the way in.
 *
 * The result's constructors are found by **name**, because this is emitted code with no declaration
 * in front of it to fix an order: what the class says `fromValue` answers is read off the member's
 * own result type, which is `Maybe(a)` today and is whatever the library writes tomorrow.
 */
void Resolver::enumFromValueBody(Builder& builder, Function& function, Local& number,
                                 DeclPtr<Type> head) {
    auto source = function.source;
    auto& enumeration = *(RecordType*)decl()[head];

    auto answer = function.result.type;
    U16 just = 0, nothing = 0;
    if(!maybeConstructors(answer, function.source, "Enum.fromValue"_v, just, nothing)) return;

    auto read = [&]() {
        return readPlace(builder, source, *builder.localPlace(number), inspectRequest());
    };

    auto given = read();
    if(!given) return;

    auto wide = decl()[number.occurrence.type]->kind == Type::Int ? number.occurrence.type
                                                                  : program.types.intType(64, true);

    auto constant = [&](I64 value) {
        return builder.addConst(source, intConstant(wide, U64(value)));
    };

    auto range = tagRangeOf(decl(), enumeration);
    auto constructors = enumeration.cons.contents(decl());
    auto consecutive = U64(range.highest - range.lowest) + 1 == U64(constructors.size());

    Node* condition = nullptr;
    if(consecutive) {
        auto atLeast = builder.addBinary(source, BinaryOp::Ge, *given, *constant(range.lowest));
        auto atMost = builder.addBinary(source, BinaryOp::Le, *given, *constant(range.highest));
        condition = builder.addBinary(source, BinaryOp::And, *atLeast, *atMost);
    } else if(mapSpanAdmits(range.lowest, range.highest, constructors.size())) {
        /*
         * A set with holes, as the **mapping** it is - `True` at each pinned number and `False`
         * everywhere else, over an open domain (D3).
         *
         * The or-chain this replaces is one comparison per constructor, and what it states is
         * membership of a set the declaration wrote down - which is exactly what a `map` carries.
         * The lowering turns it into one shifted immediate where the span fits a word, and a byte
         * table where it does not; neither branches, so what changes is the count and not the shape.
         */
        auto truth = booleanType(program);
        if(!truth) return;

        SmallArray<MapEntry, 16> entries;

        for(auto one: constructors) {
            MapEntry entry;
            entry.selector = one.value;
            entry.value = truthConstant(program, true);
            entries.push(entry);
        }

        for(Size i = 1; i < entries.size(); i++) {
            for(Size j = i; j && entries[j].selector < entries[j - 1].selector; j--) {
                auto held = entries[j];
                entries[j] = entries[j - 1];
                entries[j - 1] = held;
            }
        }

        condition = builder.addMap(source, *given, entries.pointer(), entries.size(),
                                   truthConstant(program, false), false, owned(truth));
    } else {
        for(auto one: constructors) {
            auto matches = builder.addBinary(source, BinaryOp::Eq, *given, *constant(one.value));
            condition = condition ? builder.addBinary(source, BinaryOp::Or, *condition, *matches)
                                  : matches;
        }
    }

    if(!condition) return;

    auto present = builder.addBlock(StringId());
    auto absent = builder.addBlock(StringId());
    builder.addBranch(source, builder.resultOf(*condition, UseMode::Inspect), *present, *absent,
                      nullptr, 0);

    builder.setBlock(*present);

    // The parameter is read afresh here, because a read is a value of the block it was made in -
    // which is `compareBody`'s note one function down, for the same reason.
    auto again = read();
    if(!again) return;

    auto tag = tagTypeOf(decl(), program.types, head);
    assertTrue(tag);

    auto narrowed = builder.addConvert(source, *again, tag);
    auto built = builder.addBitcast(source, *narrowed, head);

    Node* payload[] = { built };
    auto some = builder.addAggregate(source, answer, just, payload, 1);
    builder.addReturn(source, *some);

    builder.setBlock(*absent);
    auto none = builder.addAggregate(source, answer, nothing, nullptr, 0);
    builder.addReturn(source, *none);
}

/*
 * `Enum.nameOf` - the constructor's own word, chosen by the tag it is stored as.
 *
 * **One `map` over the declaration's own constructors** - inventory D3, which is what this
 * function's comment used to defer to by name. The class's note asks for "a chain of comparisons
 * picking one constant string, which folds to that string outright wherever the constructor is
 * known", and both halves of that are what a mapping already is: the fold is `foldMap` at a known
 * selector, and what the chain becomes when the selector is not known is the lowering's question
 * rather than this file's. A generated `nameOf` over a dozen constructors was a dozen blocks, a
 * dozen comparisons and a dozen inlined `stringLiteral` bodies; it is now one table read.
 *
 * The **chain is still here** for the one declaration a table cannot serve: `@value(n)` may pin
 * constructors to numbers too far apart to place, and `mapSpanAdmits` is where that is decided. The
 * two shapes answer the same thing and the fixture that pins this is `EnumInstances.yana`.
 *
 * `textConstantValue` builds each word the way a written literal is built - placed bytes plus the
 * library's own `stringLiteral`, *run* rather than emitted - so a generated name and a source one
 * are the same value and the same global.
 */
void Resolver::enumNameOfBody(Builder& builder, Function& function, Local& item, DeclPtr<Type> head) {
    auto source = function.source;
    auto& enumeration = *(RecordType*)decl()[head];

    auto tag = tagTypeOf(decl(), program.types, head);
    assertTrue(tag);

    auto count = enumeration.cons.size();
    assertTrue(count != 0);

    auto range = tagRangeOf(decl(), enumeration);

    auto answer = textType(source);

    if(answer && mapSpanAdmits(range.lowest, range.highest, count)) {
        /*
         * The words as `Constant::Text`, built later - `PendingTextMap`, and the same deferral a
         * string global is under. What a literal *is* is what the library's `stringLiteral` returns,
         * and these bodies are written before any body has been resolved.
         */
        SmallArray<MapEntry, 16> entries;

        for(auto one: enumeration.cons.contents(decl())) {
            MapEntry entry;
            entry.selector = one.value;
            entry.value = textConstant(answer, one.name);
            entries.push(entry);
        }

        sortMapEntries(entries.pointer(), entries.size());

        auto value = readPlace(builder, source, *builder.localPlace(item), inspectRequest());
        if(!value) return;

        auto number = builder.addBitcast(source, *value, tag);
        auto map = builder.addMap(source, *number, entries.pointer(), entries.size(), Constant(),
                                  true, owned(answer));
        if(!map) return;

        pendingTextMaps.push(PendingTextMap {
            (NodeMap*)map - mir(), here(), current->activeFile, source
        });

        builder.addReturn(source, *map);
        return;
    }

    /*
     * The comparison chain, for a declaration too sparse to place.
     *
     * The last constructor is the fall-through rather than a tested arm, because the tag *is* one of
     * them: a value of an enum carries a tag the declaration named, so an arm for the last one would
     * be a comparison whose answer is known and a block nothing reaches.
     */
    for(Size i = 0; i + 1 < count; i++) {
        auto one = enumeration.cons.get(decl(), U32(i));

        // Read afresh in each block, because a read is a value of the block it was made in - which
        // is `compareBody`'s note below, for the same reason.
        auto value = readPlace(builder, source, *builder.localPlace(item), inspectRequest());
        if(!value) return;

        auto number = builder.addBitcast(source, *value, tag);
        auto wanted = builder.addConst(source, intConstant(tag, U64(one.value)));
        auto matches = builder.addBinary(source, BinaryOp::Eq, *number, *wanted);

        auto hit = builder.addBlock(StringId());
        auto next = builder.addBlock(StringId());
        builder.addBranch(source, builder.resultOf(*matches, UseMode::Inspect), *hit, *next,
                          nullptr, 0);

        builder.setBlock(*hit);

        auto text = textValue(builder, one.name, source);
        if(!text) return;

        builder.addReturn(source, *text);
        builder.setBlock(*next);
    }

    auto last = textValue(builder, enumeration.cons.get(decl(), U32(count - 1)).name, source);
    if(!last) return;

    builder.addReturn(source, *last);
}

/*
 * `Enum.fromName` - the word read back, which is a comparison per constructor and a `Maybe`.
 *
 * The comparison is a **call of `Eq`'s own member** at the name's type rather than anything this
 * file knows about strings: what comparing two strings costs and how it is done is the library's,
 * and the one thing the compiler contributes is the list of words. So this is the same class call a
 * program writes, selected at the instance the library declared, and a target whose string equality
 * is a host call gets that.
 *
 * The class's note asks for a length test before the contents; that is `Eq(String)`'s own body and
 * is where it belongs, since a generated chain that compared lengths itself would be a second
 * implementation of string equality living in the compiler.
 */
void Resolver::enumFromNameBody(Builder& builder, Function& function, Local& name,
                                DeclPtr<Type> head) {
    auto source = function.source;
    auto& enumeration = *(RecordType*)decl()[head];

    auto answer = function.result.type;
    U16 just = 0, nothing = 0;
    if(!maybeConstructors(answer, function.source, "Enum.fromName"_v, just, nothing)) return;

    auto equals = program.roles.klass(intrinsic::Known::eq);
    if(!equals || !decl()[equals]->members.size()) {
        reportMissingRole(intrinsic::Known::eq, "a generated fromName"_v, source);
        return;
    }

    auto& compare = *decl()[decl()[equals]->members.get(decl(), 0)];

    auto tag = tagTypeOf(decl(), program.types, head);
    assertTrue(tag);

    for(auto one: enumeration.cons.contents(decl())) {
        auto given = readPlace(builder, source, *builder.localPlace(name), inspectRequest());
        if(!given) return;

        auto text = textValue(builder, one.name, source);
        if(!text) return;

        Node* operands[2] = { given, text };
        auto matches = callFunction(builder, compare, operands, 2, source);
        if(!matches) return;

        auto hit = builder.addBlock(StringId());
        auto next = builder.addBlock(StringId());
        builder.addBranch(source, builder.resultOf(*matches, UseMode::Inspect), *hit, *next,
                          nullptr, 0);

        builder.setBlock(*hit);

        auto number = builder.addConst(source, intConstant(tag, U64(one.value)));
        auto built = builder.addBitcast(source, *number, head);

        Node* payload[] = { built };
        auto some = builder.addAggregate(source, answer, just, payload, 1);
        builder.addReturn(source, *some);

        builder.setBlock(*next);
    }

    auto none = builder.addAggregate(source, answer, nothing, nullptr, 0);
    builder.addReturn(source, *none);
}

/*
 * One member's body - the parameters read, the member's node over them, and a return.
 *
 * The same `expandProvided` a concrete call site reaches, which is the whole of the design: what a
 * member computes is stated once, and this is the placement that gives it an address.
 */
/*
 * `Show.show` at a payload-free sum - the constructor's name, appended.
 *
 * Two calls and no graph of its own, which is the whole reason this is generated rather than
 * written: `nameOf` already is the comparison chain that picks the word, and a `Show` that repeated
 * it would be the same chain a second time and a second place for the two to disagree. The class
 * member is reached by `applyMember`, exactly as a written `nameOf(x)` reaches it, so the instance
 * this calls is the one the same walk generated a moment ago.
 *
 * The append is `known.def`'s `stringPush` role rather than a name, and it takes the sink *first*
 * where `show` takes it second - the same swap `resolveFormat` writes out, and the reason both
 * orders are spelled at each call site rather than normalized somewhere.
 *
 * A borrow of the sink taken here, off the parameter's own place: `&to: String` arrives as an
 * exclusive loan and `push(&self: String)` asks for one of the same shape, so what the position
 * declares is what the read asks for and nothing about the buffer is assumed.
 */
void Resolver::enumShowBody(Builder& builder, Function& function, Local& item, Local& sink) {
    auto source = function.source;
    auto append = program.roles.function(intrinsic::Known::stringPush);

    // Checked before the instance was generated, so a null here is the role table changing under a
    // body that was already promised - which is a compiler defect and not a library's.
    assertTrue(append != nullptr);
    if(!append) return;

    auto value = readPlace(builder, source, *builder.localPlace(item), inspectRequest());
    if(!value) return;

    Node* one[1] = { value };
    auto text = applyMember(builder, intrinsic::Known::enumeration, kEnumNameOf, one, 1, source);
    if(!text) return;

    auto& target = *decl()[decl()[append]->args.get(decl(), 0)];
    auto to = readPlace(builder, source, *builder.localPlace(sink), requestOf(target.occurrence));
    if(!to) return;

    Node* arguments[2] = { to, text };
    if(!callFunction(builder, *decl()[append], arguments, 2, source)) return;

    builder.addReturnUnit(source);
}

/*
 * `Show.showBound` at the same head - the longest constructor name, as a constant.
 *
 * Exact rather than generous, which is the contract `Show` states: the buffer is sized from this, so
 * a bound larger than the truth wastes the difference and a bound smaller than the truth is the one
 * thing an instance may not do. The longest name is both, and it is a number this stage knows
 * outright - which is the argument for generating the member at all rather than letting the class
 * default answer `Nothing` and making every interpolation of an enum grow its buffer.
 *
 * The constructors of the answer are found by **name**, for `maybeConstructors`' reason: what
 * `showBound` answers is read off the class's own signature rather than from a handle to a
 * declaration the library is free to move.
 */
void Resolver::enumShowBoundBody(Builder& builder, Function& function, DeclPtr<Type> head) {
    auto source = function.source;
    auto& enumeration = *(RecordType*)decl()[head];

    auto answer = function.result.type;
    U16 just = 0, nothing = 0;
    if(!maybeConstructors(answer, source, "Show.showBound"_v, just, nothing)) return;

    Size longest = 0;
    for(auto one: enumeration.cons.contents(decl())) {
        auto text = context.findName(one.name).size();
        if(text > longest) longest = text;
    }

    auto width = aggregateComponent(decl(), answer, just, 0).type;
    auto bound = builder.addConst(source, intConstant(width, U64(longest)));

    Node* payload[] = { bound };
    builder.addReturn(source, *builder.addAggregate(source, answer, just, payload, 1));
}

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
        // The members that are a graph rather than a node, each with its own writer - `compare`
        // answers an `Ordering` out of two comparisons, and `Enum`'s three answer a `Maybe`, a
        // string constant and a `Maybe` again.
        if(function.provided == Provided::show) {
            // Both members, and this arm is **first**: `Show`'s member indices are its own, so a
            // `showBound` tested against `kEnumFromValue` first is `Enum`'s numbering read at the
            // wrong class - which is a `fromValue` body written over an enum-typed parameter.
            if(function.providedMember == kShowShow) {
                enumShowBody(builder, function, *locals[0], *locals[1]);
            } else {
                enumShowBoundBody(builder, function, head[0]);
            }
        } else if(function.provided != Provided::enumeration) {
            compareBody(builder, function, *locals[0], *locals[1]);
        } else if(function.providedMember == kEnumFromValue) {
            enumFromValueBody(builder, function, *locals[0], head[0]);
        } else if(function.providedMember == kEnumNameOf) {
            enumNameOfBody(builder, function, *locals[0], head[0]);
        } else {
            enumFromNameBody(builder, function, *locals[0], head[0]);
        }
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
