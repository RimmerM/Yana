#include "decl.h"
#include "specialize.h"

namespace mir {

StringView nodeMnemonic(Node::Kind kind) {
    switch(kind) {
#define YANA_MIR_INST(k, Struct, mnemonic, flags) case Node::k: return mnemonic;
#include "inst.def"
#undef YANA_MIR_INST
        case Node::KindCount: break;
    }

    return "?"_v;
}

U16 nodeFlags(Node::Kind kind) {
    switch(kind) {
#define YANA_MIR_INST(k, Struct, mnemonic, flags) case Node::k: return flags;
#include "inst.def"
#undef YANA_MIR_INST
        case Node::KindCount: break;
    }

    return 0;
}

StringView useModeName(UseMode mode) {
    switch(mode) {
        case UseMode::Inspect:      return "inspect"_v;
        case UseMode::BorrowShared: return "borrow.shared"_v;
        case UseMode::BorrowUnique: return "borrow.unique"_v;
        case UseMode::Take:         return "take"_v;
        case UseMode::CopyTrivial:  return "copy.trivial"_v;
        case UseMode::Forward:      return "forward"_v;
        case UseMode::Count:        break;
    }

    return "?"_v;
}

StringView destinationName(Destination mode) {
    switch(mode) {
        case Destination::Init:   return "init"_v;
        case Destination::Assign: return "assign"_v;
        case Destination::Reinit: return "reinit"_v;
    }

    return "?"_v;
}

/*
 * What a binary operation answers.
 *
 * The category decides it and the operand type carries it, which is the whole of the rule: an
 * arithmetic operation answers what it was given, and a comparison answers a truth value whatever it
 * compared. Signedness never appears - `I32` and `U32` are two operand types and one operation.
 *
 * **A comparison of two vectors answers a mask**, which is the one place the rule reads the operand
 * type as well as the category. Design-Vector §3.2 makes that the typing rule rather than a special
 * case: a lanewise comparison has an answer per lane, and a `Bool` has one answer for the whole
 * value - which is also why `Eq` and `Ord` have no vector instances and `Lanewise` is the class
 * that describes comparing them.
 *
 * `maskFor` cannot fail for a vector that exists: the lane and the count were judged where the type
 * was made, and a mask is the same pair with a flag. The location is null for that reason - there is
 * no diagnostic on this path to attach one to.
 */
DeclPtr<Type> binaryResult(Context& context, Program& program, BinaryOp op,
                           DeclPtr<Type> operandType) {
    switch(binaryCategory(op)) {
        case OpCategory::Arith:
        case OpCategory::Bits:
        case OpCategory::Integer:
        case OpCategory::Shift:
            return operandType;
        case OpCategory::Equality:
        case OpCategory::Order:
            if(isVectorType(*program.decl, operandType)) {
                return maskFor(context, program, operandType, kNullLocation);
            }

            return booleanType(program);
    }

    assertTrue(false);
    return operandType;
}

StringView placeKindName(Place::Kind kind) {
    switch(kind) {
        case Place::Local:    return "local"_v;
        case Place::Global:   return "global"_v;
        case Place::Loan:     return "loan"_v;
        case Place::Address:  return "address"_v;
        case Place::Field:    return "field"_v;
        case Place::Property: return "property"_v;
        case Place::Discriminant: return "discriminant"_v;
        case Place::Index:    return "index"_v;
        case Place::Deref:    return "deref"_v;
        case Place::KindCount: break;
    }

    return "?"_v;
}

StringView accessName(Access access) {
    switch(access) {
        case Access::Owned:  return "owned"_v;
        case Access::Shared: return "shared"_v;
        case Access::Unique: return "unique"_v;
    }

    return "?"_v;
}

StringView originName(Origin origin) {
    switch(origin) {
        case Origin::Here:        return "here"_v;
        case Origin::CallBounded: return "call"_v;
        case Origin::Slot:        return "slot"_v;
    }

    return "?"_v;
}

StringView operandRoleName(OperandRole role) {
    switch(role) {
        case OperandRole::Value:        return "value"_v;
        case OperandRole::ReadSource:   return "read source"_v;
        case OperandRole::WriteTarget:  return "write destination"_v;
        case OperandRole::Stored:       return "stored value"_v;
        case OperandRole::Condition:    return "condition"_v;
        case OperandRole::Selector:     return "selector"_v;
        case OperandRole::Returned:     return "returned value"_v;
        case OperandRole::EdgeArgument: return "edge argument"_v;
        case OperandRole::Argument:     return "argument"_v;
        case OperandRole::Callee:       return "callee"_v;
        case OperandRole::Component:    return "component"_v;
    }

    return "?"_v;
}

/*
 * What an operand carries when it fills a parameter declared under this contract - §5.6.
 *
 * The one place a signature becomes an argument mode. A call is where the language's four handover
 * spellings meet the representation's six use modes, and having that mapping in one function is what
 * makes a direct, class, dynamic and specialized call the same node with the same guarantees: the
 * caller states which signature it selected and never states what it is doing to its own value.
 *
 * The *source* is half the question and not an afterthought. An owned parameter takes ownership, and
 * how the caller gives it up depends on what it is giving up: a value in hand - a call result, an
 * arithmetic result, a literal - is owned by this expression and is handed over outright, so the
 * mode is `Take`. A bare name is a place somebody still holds, and whether the language moves out of
 * it or borrows from it is the last-use question a single forward pass cannot answer (§5.4) - so
 * the mode is `Forward` and liveness settles it. Deciding `Take` for both would make source order
 * stand in for liveness, which is the failure the mode exists to prevent.
 *
 * `AccessForm` is read rather than ignored so that the first windowed parameter has a case to add
 * here rather than a second table to invent: a shared window and shared whole-value access are the
 * same *loan* and differ in what may be done through it, which is a fact the mode already carries
 * to the overlap check.
 */
UseMode argumentMode(Contract parameter, SourceKind source) {
    switch(parameter.access()) {
        case Access::Owned:
            return source == SourceKind::Place ? UseMode::Forward : UseMode::Take;
        case Access::Shared:
            return UseMode::BorrowShared;
        case Access::Unique:
            return UseMode::BorrowUnique;
    }

    // Not a fallback: an access this has no answer for is one that was added without deciding what a
    // caller does to satisfy it, and there is no safe guess about a handover.
    assertTrue(false);
    return UseMode::BorrowShared;
}

bool lendsItsContents(DeclBase base, MirBase mir, Body& body, const Place& place) {
    if(place.kind != Place::Local) return false;
    if(place.occurrence.contract.access() != Access::Unique) return false;
    if(!place.writable) return false;
    if(!isTrackedAt(base, place.occurrence)) return false;

    return body.local(mir, LocalId(place.payload))->isParameter();
}

UseMode readMode(DeclBase base, MirBase mir, Body& body, const Place& place, Request wanted) {
    // `isTriviallyCopyable` and nothing else. A second reading of "trivial" is a second answer: this
    // used to test the type kinds by hand, and would have disagreed with the verifier - which asks
    // that function - at the first type with a teardown and no copy. `published` is the other half
    // and belongs to the position rather than the type - see `Request`.
    if(!wanted.published && isTriviallyCopyableAt(base, place.occurrence)) {
        return UseMode::CopyTrivial;
    }

    // Exclusive access, before anything about the place: neither a copy nor a shared loan satisfies
    // it, so there is no other answer to give. Whether the place may *grant* one is a diagnostic
    // rather than a different mode - `Resolver::readPlace` is where the pair is judged.
    if(wanted.access == Access::Unique) return UseMode::BorrowUnique;

    if(!place.occurrence.contract.isOwned()) {
        // Storage this frame writes through, at a position that consumes - the one way a borrowed
        // place hands its contents over. `Forward` and not `Take`, on the same terms as an owned
        // name: whether the take is legal is whether the place is dead afterwards, which is
        // liveness's answer and not a forward pass's. What is different is what "afterwards" then
        // owes, and `checkRefills` is where that is said.
        if(wanted.access == Access::Owned && lendsItsContents(base, mir, body, place)) {
            return UseMode::Forward;
        }

        return UseMode::BorrowShared;
    }

    if(wanted.access != Access::Owned) return UseMode::BorrowShared;

    return UseMode::Forward;
}

/*
 * Whether this node may suspend the enclosing fiber - Analysis-Sharing.md §3.3.
 *
 * Asked of the node and answered by the *callee's published summary*, which is why there is no
 * `kMirSuspends` column in inst.def: a kind-level bit can only say "every call suspends" or "no call
 * does". `Fact::get(true)` is the conservative fallback where the answer has not been computed, and
 * it is the caller's rather than the summary's - a foreign declaration and a callee whose module has
 * not been read are both "no answer", and the safe reading of no answer at a suspension point is
 * that it may.
 *
 * Every other kind answers false, and will keep answering false until `yield` arrives: a suspension
 * point is a call or a yield, and neither arithmetic nor a store has ever been one.
 */
bool maySuspend(DeclBase base, Node& node) {
    if(node.kind != Node::Call) return false;

    // A call through a function value has no declaration to publish anything, and no answer is the
    // conservative one: what the pair points at is decided at run time, so the safest reading of a
    // suspension point whose callee is unknown is that it may.
    auto& call = (NodeCall&)node;
    if(call.callsValue()) return true;

    return base[call.callee]->summary.suspension().get(true);
}

StorageClass storageOf(MirBase base, Body& body, Local& local) {
    auto root = body.objectRoot(base, local.object);
    if(!root.valid()) return StorageClass::Frame;

    return body.object(base, root)->storage;
}

StringView storageClassName(StorageClass storage) {
    switch(storage) {
        case StorageClass::Frame:  return "frame"_v;
        case StorageClass::Heap:   return "heap"_v;
        case StorageClass::Region: return "region"_v;
    }

    return "?"_v;
}

StringView accessFormName(AccessForm form) {
    switch(form) {
        case AccessForm::WholeValue:     return "whole"_v;
        case AccessForm::View: return "view"_v;
        case AccessForm::Owner: return "owner"_v;
    }

    return "?"_v;
}

/*
 * The interned ownership shape.
 *
 * Interned by content, so two occurrences whose interior structure agrees are one handle and a shape
 * comparison is a handle comparison - which is what lets a manifestation key and a call's argument
 * compatibility both use it without either of them growing a structural walk of its own.
 *
 * The key is a hash of the contents, and a bucket that already holds a *different* shape is stepped
 * over rather than shared - `internSlot`, which is where that rule is stated for every table in this
 * stage that interns by content.
 */
DeclPtr<OwnershipSchema> Shapes::intern(ExternalSlot* slots, Size count) {
    // The empty shape is the null handle. Every type in the language subset built so far has one,
    // so this is the path that is actually taken and it allocates nothing.
    if(!count) return nullptr;

    auto base = *arena;

    InternKey key;
    for(Size i = 0; i < count; i++) {
        key.mix(slots[i].contract.packed);
        for(auto step: slots[i].path.contents(base)) key.mix(step);
        key.next();
    }

    auto created = false;
    auto entry = internSlot(shapes, key, created, [&](DeclPtr<OwnershipSchema> existing) {
        auto& written = *base[existing];
        if(written.slots.size() != count) return false;

        for(Size i = 0; i < count; i++) {
            if(!sameSlot(base, *base[written.slots.get(base, i)], slots[i])) return false;
        }

        return true;
    });

    if(!created) return *entry;

    auto schema = new (arena) OwnershipSchema();
    schema->id = nextId++;

    for(Size i = 0; i < count; i++) {
        auto slot = new (arena) ExternalSlot(slots[i]);
        schema->slots.push(arena, slot - base);
    }

    *entry = schema - base;
    return *entry;
}

DeclPtr<OwnershipSchema> Shapes::under(DeclPtr<OwnershipSchema> shape, U16 step) {
    if(!shape) return nullptr;

    auto base = *arena;
    SmallArray<ExternalSlot, 8> inner;

    for(auto slotPointer: base[shape]->slots.contents(base)) {
        auto& slot = *base[slotPointer];
        if(slot.path.size() < 2 || slot.path.get(base, 0) != step) continue;

        ExternalSlot nested;
        for(Size i = 1; i < slot.path.size(); i++) nested.path.push(arena, slot.path.get(base, i));
        nested.contract = slot.contract;

        inner.push(nested);
    }

    return intern(inner.pointer(), inner.size());
}

DeclPtr<OwnershipSchema> Shapes::closure() {
    if(closureShape) return closureShape;

    ExternalSlot slot;
    slot.path.push(arena, PathStep::field(kFunEnvironmentComponent));
    slot.contract = borrowedContract(Access::Shared);

    closureShape = intern(&slot, 1);
    return closureShape;
}

// The argument is at the declaration, in contract.h - including why the access is what tells the
// two apart.
DeclPtr<OwnershipSchema> Shapes::noEnvironment() {
    if(plainShape) return plainShape;

    ExternalSlot slot;
    slot.path.push(arena, PathStep::field(kFunEnvironmentComponent));
    slot.contract = borrowedContract(Access::Unique);

    plainShape = intern(&slot, 1);
    return plainShape;
}

Module* Program::addModule(StringId name) {
    auto module = new (decl) Module(name);
    modules.push(module - *decl);
    return module;
}

/*
 * The by-name indexes, keyed by module and name together.
 *
 * On `Program` rather than on `Module` because a `HashMap` owns a heap buffer and a `Module` is
 * placement-allocated in an arena that runs no destructors - see the note at the top of decl.h. The
 * key is the module's offset above the name, which is unique because both halves are.
 */
U64 Program::nameKey(DeclPtr<Module> module, StringId name) {
    return (U64(module.offset) << 32) | U64(name.value);
}

/*
 * Filing one declaration in R1's set, and answering whether R1 admits it - see the declaration,
 * where the rule and what it replaced are written.
 *
 * `HashMap::add(key, value)` *overwrites*, so this is careful to read the entry and write into it
 * rather than replacing it: a class function joining a key that already holds one has to find the
 * one that is there, and an `add` that answered a fresh entry each time would file each of them as
 * the only member of a set of its own.
 */
bool Program::addFunctionName(DeclPtr<Module> module, DeclPtr<Function> function, U32* conflict) {
    auto& declared = *(*decl)[function];
    auto name = declared.name;
    auto key = [&](U32 arity) { return InternKey().mix(nameKey(module, name)).mix(arity).hash; };

    // Which half of R1's set this declaration joins, and the only thing that decides it: a class
    // member carries the class it is a member *of*, and everything else is a plain function.
    auto isMember = declared.klass != nullptr;

    /*
     * The whole range checked before any of it is written, which the two loops are for.
     *
     * A declaration answers several arities now, so a collision may be found partway through - and a
     * range half of which is registered is worse than one that is not: the calls at the arities that
     * happened to come first would reach a declaration the module reported as a redeclaration and
     * therefore never finished.
     */
    auto first = declared.requiredArity(*decl);
    auto last = declared.writtenArity();

    if(!isMember) {
        for(auto arity = first; arity <= last; arity++) {
            auto held = functionsByName.getValue(key(arity));
            if(!held || !held.unwrap().plain) continue;

            if(conflict) *conflict = arity;
            return false;
        }
    }

    for(auto arity = first; arity <= last; arity++) {
        auto entry = functionsByName.add(key(arity));
        if(!entry.existed) *entry.value = Overloads {};

        if(isMember) entry.value->members.push(decl, function);
        else entry.value->plain = function;
    }

    // An exported declaration replaces a private one under the same name - see `functionNamed`,
    // where the reason is written. Otherwise first-in wins, and which one it is does not matter:
    // what a reader asks the entry is whether the *name* is taken and whether it may be spoken of.
    auto named = functionNames.add(nameKey(module, name));
    if(!named.existed || (!(*decl)[*named.value]->exported && (*decl)[function]->exported)) {
        *named.value = function;
    }

    return true;
}

// See the declaration, where the interning and its cost are argued.
Constant Program::aggregateConstant(DeclPtr<Type> type, U16 constructor, const Constant* values,
                                    Size count) {
    auto base = *decl;

    for(U32 i = 0; i < aggregates.size(); i++) {
        auto& held = *base[aggregates[i]];
        if(held.constructor != constructor || held.values.size() != count) continue;

        auto same = true;
        for(Size at = 0; at < count && same; at++) {
            same = held.values.get(base, U32(at)) == values[at];
        }

        if(!same) continue;

        // The **parts** and nothing else. Two aggregates of one shape at two types share an entry
        // and are still two constants, because a `Constant` carries its own type and compares on
        // it - `{I32, I32}` and a `data Point` of two `I32`s reduce to the same list and are not
        // the same value.
        return Constant { type, Constant::Aggregate, U64(aggregates[i].offset) };
    }

    auto made = new (decl) AggregateConstant();
    made->constructor = constructor;
    for(Size i = 0; i < count; i++) made->values.push(decl, values[i]);

    auto pointer = DeclPtr<AggregateConstant>(made - base);
    aggregates.push(pointer);
    return Constant { type, Constant::Aggregate, U64(pointer.offset) };
}

StringId Program::internName(Context& context, StringView bytes) {
    auto stored = (char*)decl.alloc(bytes.size());
    copy(bytes.ptr, stored, bytes.size());
    return context.addUnqualifiedName(stored, bytes.size());
}

StringId Program::uniqueSymbol(DeclPtr<Module> module, Context& context, StringView text) {
    /*
     * The bytes are copied before they are interned - `internName`, and the rule stated on it. A
     * specialization's name is formatted into a `String` that is gone when the caller returns, so a
     * caller that forgot the copy left the name table pointing at freed memory and every dump and
     * symbol built from it came out as garbage; one producer of function symbols means one place
     * the rule has to be kept.
     */
    auto intern = [&](StringView bytes) { return internName(context, bytes); };

    /*
     * Counted **per program** and not per module, which is where this was wrong.
     *
     * What a symbol has to be unique against is the table it ends up in, and that is the lowered
     * module's - one map from `StringId` to function for the whole program, and one entry per name
     * in the backend's symbol table. Two modules each declaring a private `readCount` were two
     * declarations with one symbol: `LowerModule::addFunction` handed the second the first's
     * function, and what came out had one signature, both bodies' blocks and a `ret` arity that did
     * not match - `lib/Test/Options.yana` and `lib/Core/FloatRead.test.yana`, which is how it was
     * found. A module argument that decides nothing is worse than none, so it is gone.
     */
    (void)module;

    auto name = intern(text);
    auto entry = symbolCounts.add(U64(name.value));

    if(!entry.existed) {
        *entry.value = 1;
        return name;
    }

    auto suffixed = Tritium::format("%@'%@", text, U64(++*entry.value));
    auto unique = intern(StringView(suffixed.text(), suffixed.size()));

    // Recorded under its own text too, so that a module which really does declare `negate'2` cannot
    // be handed the same name a suffix produced.
    *symbolCounts.add(U64(unique.value)).value = 1;
    return unique;
}

bool Program::addGlobalName(DeclPtr<Module> module, StringId name, DeclPtr<Global> global) {
    auto entry = globalsByName.add(nameKey(module, name));
    if(entry.existed) return false;

    *entry.value = global;
    return true;
}

bool Program::addRecordName(DeclPtr<Module> module, StringId name, DeclPtr<RecordType> record) {
    auto entry = recordsByName.add(nameKey(module, name));
    if(entry.existed) return false;

    *entry.value = record;
    return true;
}

void Program::findOverloads(DeclPtr<Module> module, StringId name, U32 arity,
                            OverloadSet& into) const {
    auto found = functionsByName.getValue(InternKey().mix(nameKey(module, name)).mix(arity).hash);
    if(!found) return;

    auto held = found.unwrap();
    into.plain = held.plain;
    for(auto member: held.members.contents(*decl)) into.members.push(member);
}

DeclPtr<Function> Program::findFunction(DeclPtr<Module> module, StringId name, U32 arity) const {
    OverloadSet set;
    findOverloads(module, name, arity, set);
    return set.sole();
}

DeclPtr<Function> Program::functionNamed(DeclPtr<Module> module, StringId name) const {
    auto found = functionNames.getValue(nameKey(module, name));
    return found ? found.unwrap() : nullptr;
}

DeclPtr<Global> Program::findGlobal(DeclPtr<Module> module, StringId name) const {
    auto found = globalsByName.getValue(nameKey(module, name));
    return found ? found.unwrap() : nullptr;
}

bool Program::addAliasName(DeclPtr<Module> module, StringId name, DeclPtr<Alias> alias) {
    auto entry = aliasesByName.add(nameKey(module, name));
    if(entry.existed) return false;

    *entry.value = alias;
    return true;
}

DeclPtr<Alias> Program::findAlias(DeclPtr<Module> module, StringId name) const {
    auto found = aliasesByName.getValue(nameKey(module, name));
    return found ? found.unwrap() : nullptr;
}

bool Program::addConstructorName(DeclPtr<Module> module, StringId name, ConstructorRef constructor) {
    auto entry = constructorsByName.add(nameKey(module, name));
    if(entry.existed) return false;

    *entry.value = constructor;
    return true;
}

ConstructorRef Program::findConstructor(DeclPtr<Module> module, StringId name) const {
    auto found = constructorsByName.getValue(nameKey(module, name));
    return found ? found.unwrap() : ConstructorRef();
}

DeclPtr<RecordType> Program::findRecord(DeclPtr<Module> module, StringId name) const {
    auto found = recordsByName.getValue(nameKey(module, name));
    return found ? found.unwrap() : nullptr;
}

U16 Class::arity(DeclBase base) {
    return gen ? U16(base[gen]->variables.size()) : 0;
}

/*
 * What one head position contributes to the key - its outer constructor, and nothing below it.
 *
 * `Array(a)` and `Array(I32)` give the same answer, which is the point: the key narrows to the
 * instances that could possibly match and the match itself decides between them. A **variable**
 * gives none, so a head with one in a deciding position cannot be filed at all - see
 * `Program::openInstance`.
 *
 * The offsets are shifted past the sentinels rather than the sentinels being large, so that a
 * declaration at region offset zero - which is a real address here - cannot collide with them.
 */
static constexpr U32 kNoHead = 0;
static constexpr U32 kOpenHead = 1;

/*
 * Every pointer, under one key - which is the same statement the `Record` arm below makes, for a
 * constructor that has no declaration to be keyed by.
 *
 * `%I32` and `%a` are two type handles and one constructor, so an instance head written at `%a` has
 * to be reachable from every pointer type. That head is not hypothetical and is not a program's:
 * `providePointerInstances` generates `Eq(%a)`, `Ord(%a)` and the reinterpretation rungs, and each of
 * them is *the* instance for its class at every pointer there is - there is nothing about comparing
 * two addresses that depends on what they point at.
 *
 * A consequence worth naming: two heads at two different pointee types are one key, which is a
 * shared *bucket* and not a collision - `%I32` and `%I64` are two shapes and `addInstance` keeps
 * both, ordered. The key says what an instance is for; what tells two of them apart is `sameShape`.
 */
static constexpr U32 kPointerHead = 2;

/*
 * And every vector under one key, every mask under another - the same statement one row up, for two
 * more constructors with no declaration to be keyed by.
 *
 * `Vec(F32)` and `Vec(a, n)` are two handles and one constructor, so the head `provideVectorInstances`
 * generates has to be reachable from every vector there is. Two keys and not one, because a mask is
 * a different constructor to the language even though it is the same kind here: `Bitwise` has an
 * instance at both and they are not the same instance, since a mask has no arithmetic and a vector
 * of integers has.
 */
static constexpr U32 kVectorHead = 3;
static constexpr U32 kMaskHead = 4;

// A fixed array, on the pointer's terms: `[a *n]` is nobody's declaration, so every fixed array
// files under one key and `matchType` decides the element and the count. Keyed on the interned type
// it was not reachable at all - `[I32 *3]` and `[a *n]` are two handles, and so are two instance
// heads' own `[a *n]`s.
static constexpr U32 kArrayHead = 5;

// Where the region offsets start, past the sentinels above - so that a declaration at offset zero,
// which is a real address here, cannot collide with one of them. A name rather than a literal
// because two lines below add it and a sentinel added above would otherwise move one of them.
static constexpr U32 kFirstDeclaredHead = 6;

static U32 headKey(DeclBase base, DeclPtr<Type> type) {
    if(!type) return kNoHead;

    auto& written = *base[type];
    if(written.kind == Type::Variable) return kOpenHead;
    if(written.kind == Type::Pointer) return kPointerHead;
    if(written.kind == Type::Array) return kArrayHead;

    if(written.kind == Type::Vector) {
        return ((VectorType&)written).isMask ? kMaskHead : kVectorHead;
    }

    // An instantiation is keyed by the declaration it came from: `Array(I32)` is an `Array`, and an
    // instance head written at `Array(a)` has to be reachable from every one of them.
    if(written.kind == Type::Record) {
        auto& record = (RecordType&)written;
        if(record.instanceOf) return record.instanceOf.offset + kFirstDeclaredHead;
    }

    // And a `@bits(n)` refinement is filed under the type it refines, which is the same statement the
    // line above makes about an instantiation and the same one `matchType` makes about a comparison:
    // an instance written for `U64` has to be reachable from every refinement of it, or
    // `doc/spec/repr.md`'s "never participates in typeclass dispatch" would be false at the table.
    return canonicalInt(base, type).offset + kFirstDeclaredHead;
}

/*
 * The key of one applied argument list - the class, and each *deciding* position's constructor, with
 * the positions named by `wild` contributing a wildcard instead.
 *
 * The determined positions are left out because they are not what selection is asked with: a
 * functional dependency says the prefix decides the rest, so a key that included the rest would be a
 * key no call site could compute. `Class::isDetermined` is the one place that boundary is written.
 *
 * **`wild` is what makes a head open one position at a time**, which is the whole of what this file
 * gained when `Rewrap(m, b -> n)` arrived. Its instances name a constructor in `m` and a variable in
 * `b` - `Rewrap(Maybe(a), b, Maybe(b))` - so a head is neither wholly keyed nor wholly open, and a
 * table with only those two states filed all three of them as "matches everything" and reported the
 * second and third as duplicates of the first. A per-position wildcard is the same statement the old
 * pair made, made once per position instead of once per head.
 *
 * `wild` is a bit per deciding position **in class-parameter order**, so bit `i` is parameter `i`
 * and the determined positions simply have no bit that is ever read. Numbering by parameter rather
 * than by position-among-the-deciding-ones keeps the two ends - the instance that computes its own
 * mask and the lookup that enumerates masks - reading the same index.
 */
static U64 instanceKey(DeclBase base, DeclPtr<Class> klass, const DeclPtr<Type>* args, Size count,
                       U32 wild) {
    auto& declared = *base[klass];
    auto key = InternKey().mix(klass.offset);

    for(Size i = 0; i < count; i++) {
        if(declared.isDetermined(U16(i))) continue;

        auto head = (wild >> i) & 1 ? kOpenHead : headKey(base, args[i]);
        key = key.mix(head);
    }

    return key.hash;
}

/*
 * Which deciding positions of a head *cannot* be keyed - a variable, or an argument the caller left
 * undecided.
 *
 * One function for both ends, because they are the same question: an instance's own mask is what it
 * is filed under, and a lookup's is the set of positions it has no choice but to wildcard. A lookup
 * enumerates supersets of its own; an instance enumerates nothing.
 */
static U32 openPositions(DeclBase base, DeclPtr<Class> klass, const DeclPtr<Type>* args, Size count) {
    auto& declared = *base[klass];
    U32 mask = 0;

    for(Size i = 0; i < count && i < 32; i++) {
        if(declared.isDetermined(U16(i))) continue;

        auto head = headKey(base, args[i]);
        if(head == kOpenHead || head == kNoHead) mask |= U32(1) << i;
    }

    return mask;
}

// The deciding positions, as a mask - the space `candidateInstances` enumerates inside.
static U32 decidingPositions(DeclBase base, DeclPtr<Class> klass, Size count) {
    auto& declared = *base[klass];
    U32 mask = 0;

    for(Size i = 0; i < count && i < 32; i++) {
        if(!declared.isDetermined(U16(i))) mask |= U32(1) << i;
    }

    return mask;
}

/*
 * Whether one head is **at least as general** as another - `pattern`'s own variables bound to
 * whatever stands opposite them in `against`.
 *
 * `matchType` rather than a structural comparison written here, and that is the point rather than a
 * convenience: what the table wants to know about two heads is the same question selection asks of
 * a head and a call, and a second walk over the type shapes is how a table comes to call two heads
 * distinct that selection cannot tell apart. `Copy(a)` is at least as general as
 * `Copy(Array(I32))`; `Copy(Array(a))` is not as general as `Copy(a)`, because a constructor in the
 * pattern has nothing a variable can bind it to.
 *
 * The bindings are thrown away. Two heads are compared here, and neither of them is a type a value
 * has - what comes back is the ordering, and the match that produces the arguments is `Selector`'s.
 */
static bool generalizes(DeclBase base, Types& types, Instance& pattern, Instance& against) {
    auto count = pattern.args.size();
    if(against.args.size() != count) return false;

    SmallArray<DeclPtr<Type>, 4> bindings;
    if(pattern.gen) {
        for(Size i = 0; i < base[pattern.gen]->variables.size(); i++) bindings.push(nullptr);
    }

    auto head = pattern.args.pointer(base);
    auto other = against.args.pointer(base);

    for(Size i = 0; i < count; i++) {
        if(!matchType(base, types, pattern.gen, bindings, head[i], other[i])) return false;
    }

    return true;
}

/*
 * Two heads of one **shape** - each at least as general as the other, which is alpha-equivalence
 * said in the terms this file already has.
 *
 * This is what `addInstance` refuses, and it is deliberately narrower than "these two overlap".
 * `Copy(a)` and `Copy(b)` are one shape and the second is a mistake; `Pair(a, a)` and `Pair(a, b)`
 * overlap and are two shapes, since the second is more general than the first and not the reverse.
 * Overlap without identity is the reasoning `candidateInstances` *orders* rather than refuses, and
 * has been since a head became open one position at a time.
 */
static bool sameShape(DeclBase base, Types& types, Instance& a, Instance& b) {
    return generalizes(base, types, a, b) && generalizes(base, types, b, a);
}

/*
 * A head filed under its key, and refused where the table already holds its shape.
 *
 * The bucket is a *list* because the key is a filter and not an identity: it reads the outer
 * constructor of each deciding position, so `Named(Box(I32))` and `Named(Box(I64))` reach it as one
 * key and are two instances - which is the defect this replaced, where the second of them was
 * reported as a duplicate of the first and `Math` could not declare `Real` at two vector widths.
 * Keying deeper is not the fix and was tried on paper: `Show(Array(a))` has to be reachable from
 * `Array(I32)`, so a key that read the element would have to be enumerated over nested wildcards as
 * well, and the enumeration is what the key exists to avoid.
 *
 * **Most specific first**, kept by the insert rather than by the lookup. Two heads in one bucket
 * differ below their outer constructors, so `Copy(Array(I32))` and `Copy(Array(a))` are ordered
 * here for the same reason two buckets are ordered by wildcard count in `candidateInstances` - and
 * without an order, which of them served an array of `I32` would be hash order, which is precisely
 * the coherence a table is supposed to provide. Incomparable heads keep insertion order, which is
 * declaration order and is not a choice selection ever has to make: `matchType` accepts at most one
 * of them.
 */
bool Program::addInstance(DeclBase base, DeclPtr<Instance> instance) {
    auto& declared = *base[instance];
    auto args = declared.args.pointer(base);
    auto count = declared.args.size();

    auto wild = openPositions(base, declared.klass, args, count);
    auto& bucket = instancesByHead[instanceKey(base, declared.klass, args, count, wild)];

    for(auto existing: bucket) {
        if(sameShape(base, types, *base[existing], declared)) return false;
    }

    bucket.insertWhereFirst([&](DeclPtr<Instance> existing) {
        auto& other = *base[existing];
        return generalizes(base, types, other, declared)
            && !generalizes(base, types, declared, other);
    }, instance);

    return true;
}

// One bucket appended, or nothing where the table has no head of that shape.
void Program::keyedBucket(U64 key, SmallArray<DeclPtr<Instance>, 4>& into) {
    auto found = instancesByHead.get(key);
    if(!found) return;

    for(auto candidate: found.unwrap()) into.push(candidate);
}

void Program::keyedInstances(DeclBase base, DeclPtr<Class> klass, const DeclPtr<Type>* args,
                             Size count, SmallArray<DeclPtr<Instance>, 4>& into) {
    if(!klass) return;
    if(openPositions(base, klass, args, count)) return;

    keyedBucket(instanceKey(base, klass, args, count, 0), into);
}

/*
 * Every head that could serve this argument list, **most specific first**.
 *
 * A head is more specific than another when it wildcards fewer positions, which is the same
 * specificity order the table used to state as "the keyed head, then the open one" and is now stated
 * once for every shape in between. `instance (Copy(a)) Copy(Array(a))` still wins over
 * `instance (TrivialCopy(a)) Copy(a)` for an array, by the same rule and with nothing said about
 * either of them; `Rewrap(Maybe(a), b, Maybe(b))` is reached with `b` concrete, which no ordering of
 * two states could have expressed.
 *
 * The enumeration is over supersets of the caller's *own* open positions, because a position the
 * caller could not key is one that has to be wildcarded in every probe. Bounded by two to the number
 * of deciding positions - three in the whole language today, and every probe is one hash lookup.
 *
 * Ties **within** a popcount are broken by mask value and are deliberately not refused: two heads
 * that overlap only by wildcarding different positions are the overlap reasoning this table is
 * written to not need, exactly as two open heads were before. What is still refused is two heads
 * with the *same* shape, which is `addInstance` answering false - and within one bucket the order
 * is the specificity `addInstance` inserted by, so the two levels of this ordering are one rule.
 */
void Program::candidateInstances(DeclBase base, DeclPtr<Class> klass, const DeclPtr<Type>* args,
                                 Size count, SmallArray<DeclPtr<Instance>, 4>& into) {
    if(!klass) return;

    auto deciding = decidingPositions(base, klass, count);
    auto forced = openPositions(base, klass, args, count);

    auto wildcards = [](U32 mask) {
        U32 n = 0;
        for(; mask; mask &= mask - 1) n++;
        return n;
    };

    for(U32 wanted = 0; wanted <= wildcards(deciding); wanted++) {
        for(U32 wild = 0; wild <= deciding; wild++) {
            if((wild & deciding) != wild || (wild & forced) != forced) continue;
            if(wildcards(wild) != wanted) continue;

            keyedBucket(instanceKey(base, klass, args, count, wild), into);
        }
    }
}

bool Program::addClassName(DeclPtr<Module> module, StringId name, DeclPtr<Class> klass) {
    auto entry = classesByName.add(nameKey(module, name));
    if(entry.existed) return false;

    *entry.value = klass;
    return true;
}

DeclPtr<Class> Program::findClass(DeclPtr<Module> module, StringId name) const {
    auto found = classesByName.getValue(nameKey(module, name));
    return found ? found.unwrap() : nullptr;
}

bool Program::addFixity(DeclPtr<Module> module, StringId name, OpProperties properties) {
    auto entry = fixities.add(nameKey(module, name));
    if(entry.existed) return false;

    *entry.value = properties;
    return true;
}

Maybe<OpProperties> Program::findFixity(DeclPtr<Module> module, StringId name) const {
    return fixities.getValue(nameKey(module, name));
}

DeclPtr<Module> Program::findModule(StringId name) const {
    auto base = *decl;
    for(auto module: modules) {
        if(base[module]->name == name) return module;
    }

    return nullptr;
}

/*
 * Which file of this module a location is in.
 *
 * Linear over the file names, and called once per declaration or body rather than once per name -
 * the answer is then held in `activeFile` for the length of that extent. A module of one file, which
 * is almost all of them, never reaches the walk at all.
 */
U16 Module::fileOf(DeclBase base, Context& context, LocationId source) {
    if(source == kNullLocation || files.size() <= 1) return 0;

    auto location = context.getLocation(source);
    if(!location) return 0;

    for(U16 i = 0; i < U16(files.size()); i++) {
        if(files.get(base, i) == location->sourceModule) return i;
    }

    return 0;
}

bool Import::visible(DeclBase base, StringId name) {
    if(include.size()) {
        auto included = false;
        for(auto symbol: include.contents(base)) {
            if(symbol == name) {
                included = true;
                break;
            }
        }

        if(!included) return false;
    }

    for(auto symbol: exclude.contents(base)) {
        if(symbol == name) return false;
    }

    return true;
}

/*
 * The slot one (generic, argument list) interns to - `specialize.h`.
 *
 * Keyed by the generic and its arguments and re-checked structurally, which is `internSlot`'s
 * contract and is argued once there: a hash is not an identity, and two argument lists that collided
 * would make one specialization answer for both.
 */
DeclPtr<Function>* Program::internSpecialization(DeclPtr<Function> generic, DeclPtr<Type>* args,
                                                 Size count) {
    auto base = *decl;

    InternKey key;
    key.mix(generic.offset);
    for(Size i = 0; i < count; i++) key.mix(args[i].offset);

    auto created = false;
    auto entry = internSlot(specializations, key, created, [&](DeclPtr<Function> existing) {
        auto& written = *base[existing];
        if(written.specializes != generic || written.typeArguments.size() != count) return false;

        for(Size i = 0; i < count; i++) {
            if(written.typeArguments.get(base, i) != args[i]) return false;
        }

        return true;
    });

    // Null contents are what the caller reads as "there is no such specialization yet", so a fresh
    // slot says so rather than holding whatever the bucket did.
    if(created) *entry = nullptr;

    return entry;
}

DeclPtr<GenEnvironment>* Program::internEnvironment(DeclPtr<Function> generic, DeclPtr<Type>* args,
                                                    Size count) {
    auto base = *decl;

    InternKey key;
    key.mix(generic.offset);
    for(Size i = 0; i < count; i++) key.mix(args[i].offset);

    auto created = false;
    auto entry = internSlot(environments, key, created, [&](DeclPtr<GenEnvironment> existing) {
        auto& written = *base[existing];
        if(written.generic != generic || written.arguments.size() != count) return false;

        for(Size i = 0; i < count; i++) {
            if(written.arguments.get(base, i) != args[i]) return false;
        }

        return true;
    });

    if(created) *entry = nullptr;
    return entry;
}

StringView callThroughName(CallThrough through) {
    switch(through) {
        case CallThrough::Declaration:  return "direct"_v;
        case CallThrough::ClassSlot:    return "class"_v;
        case CallThrough::FunctionSlot: return "function"_v;
        case CallThrough::Value:        return "value"_v;
    }

    assertTrue(false);
    return "?"_v;
}

StringView attributeName(DeclAttribute attribute) {
    switch(attribute) {
        case DeclAttribute::ExplicitDrop: return "explicitDrop"_v;
        case DeclAttribute::Local:        return "local"_v;
        case DeclAttribute::Inline:       return "inline"_v;
        case DeclAttribute::Builtin:      return intrinsic::kBuiltinAttribute;
        case DeclAttribute::Specialize:   return "specialize"_v;
        case DeclAttribute::Generic:      return "generic"_v;
        case DeclAttribute::Value:        return "value"_v;
        case DeclAttribute::NoInline:     return "noinline"_v;
        case DeclAttribute::Bits:         return "bits"_v;
        case DeclAttribute::Box:          return "box"_v;
        case DeclAttribute::Capacity:     return "capacity"_v;
        case DeclAttribute::Platform:     return "platform"_v;
        case DeclAttribute::Allocator:    return "allocator"_v;
        case DeclAttribute::Layout:       return "layout"_v;
        case DeclAttribute::LegacySse:    return nameForLegacySse();
        case DeclAttribute::Convention:   return "convention"_v;
        case DeclAttribute::Host:         return "host"_v;
        case DeclAttribute::Heap:         return "heap"_v;
        case DeclAttribute::Count:        break;
    }

    return "?"_v;
}

StringView genFormName(GenForm form) {
    switch(form) {
        case GenForm::Auto:        return "auto"_v;
        case GenForm::Specialized: return "specialized"_v;
        case GenForm::Erased:      return "erased"_v;
    }

    // Not a fallback. A form outside the enum is a field nothing wrote, and the pass that decides one
    // refuses to leave it that way.
    assertTrue(false);
    return "auto"_v;
}

DeclAttribute findAttribute(StringView name) {
    for(U8 i = 0; i < U8(DeclAttribute::Count); i++) {
        if(name == attributeName(DeclAttribute(i))) return DeclAttribute(i);
    }

    return DeclAttribute::Count;
}

/*
 * The middle arena, released whole - §8.1.
 *
 * Every function's MIR handle is cleared *before* the reset rather than left to be a dangling offset
 * nobody dereferences. §8.8 is the invariant this protects: one accidental reverse pointer from SSA
 * into MIR turns the phase split into permanent duplication, and a handle that is still readable
 * after the region is gone is how such a pointer survives review.
 */
void Program::releaseMir() {
    auto base = *decl;

    for(auto modulePointer: modules) {
        auto module = base[modulePointer];
        for(auto functionPointer: module->functions.contents(base)) base[functionPointer]->mir = nullptr;
    }

    mir.reset(64 * 1024);
}

} // namespace mir
