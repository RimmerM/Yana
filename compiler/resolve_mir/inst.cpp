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
 * arithmetic operation answers what it was given, and a comparison answers `Bool` whatever it
 * compared. Signedness never appears - `I32` and `U32` are two operand types and one operation.
 */
DeclPtr<Type> binaryResult(Types& types, BinaryOp op, DeclPtr<Type> operandType) {
    switch(binaryCategory(op)) {
        case OpCategory::Arith:
        case OpCategory::Integer:
        case OpCategory::Shift:
            return operandType;
        case OpCategory::Equality:
        case OpCategory::Order:
            return types.boolean;
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

UseMode readMode(DeclBase base, const Place& place, Request wanted) {
    // `isTriviallyCopyable` and nothing else. A second reading of "trivial" is a second answer: this
    // used to test the type kinds by hand, and would have disagreed with the verifier - which asks
    // that function - at the first type with a teardown and no copy. `published` is the other half
    // and belongs to the position rather than the type - see `Request`.
    if(!wanted.published && isTriviallyCopyable(base, place.occurrence.type)) {
        return UseMode::CopyTrivial;
    }

    // Exclusive access, before anything about the place: neither a copy nor a shared loan satisfies
    // it, so there is no other answer to give. Whether the place may *grant* one is a diagnostic
    // rather than a different mode - `Resolver::readPlace` is where the pair is judged.
    if(wanted.access == Access::Unique) return UseMode::BorrowUnique;

    if(!place.occurrence.contract.isOwned()) return UseMode::BorrowShared;
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
        case AccessForm::SequenceWindow: return "window"_v;
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

DeclPtr<OwnershipSchema> Shapes::closure() {
    if(closureShape) return closureShape;

    ExternalSlot slot;
    slot.path.push(arena, kFunEnvironmentComponent);
    slot.contract = borrowedContract(Access::Shared);

    closureShape = intern(&slot, 1);
    return closureShape;
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
 * Answering whether the name was free, and keeping the first declaration when it was not.
 *
 * Both halves were wrong before. `HashMap::add(key, value)` *overwrites*, so a second declaration of
 * one name silently replaced the first - every call in the module then reached the second, including
 * the calls written above it - and the answer that would have said so was discarded. The two-step
 * form is what keeps the first, and handing the answer back is what keeps the resolver from asking
 * the same table twice to find out.
 */
bool Program::addFunctionName(DeclPtr<Module> module, StringId name, U32 arity,
                              DeclPtr<Function> function) {
    auto entry = functionsByName.add(InternKey().mix(nameKey(module, name)).mix(arity).hash);
    if(entry.existed) return false;

    *entry.value = function;
    *functionNames.add(nameKey(module, name)).value = true;
    return true;
}

StringId Program::uniqueSymbol(DeclPtr<Module> module, Context& context, StringView text) {
    /*
     * The bytes are copied into the declaration arena before they are interned, because **an
     * interned name borrows its text**.
     *
     * Done here rather than at each caller, and that is the whole reason this is one function: a
     * specialization's name is formatted into a `String` that is gone when the caller returns, so a
     * caller that forgot the copy left the name table pointing at freed memory and every dump and
     * symbol built from it came out as garbage. One producer of function symbols means one place the
     * rule has to be kept.
     */
    auto intern = [&](StringView bytes) {
        auto stored = (char*)decl.alloc(bytes.size());
        copy(bytes.ptr, stored, bytes.size());
        return context.addUnqualifiedName(stored, bytes.size());
    };

    auto name = intern(text);
    auto entry = symbolCounts.add(nameKey(module, name));

    if(!entry.existed) {
        *entry.value = 1;
        return name;
    }

    auto suffixed = Tritium::format("%@'%@", text, U64(++*entry.value));
    auto unique = intern(StringView(suffixed.text(), suffixed.size()));

    // Recorded under its own text too, so that a module which really does declare `negate'2` cannot
    // be handed the same name a suffix produced.
    *symbolCounts.add(nameKey(module, unique)).value = 1;
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

DeclPtr<Function> Program::findFunction(DeclPtr<Module> module, StringId name, U32 arity) const {
    auto found = functionsByName.getValue(InternKey().mix(nameKey(module, name)).mix(arity).hash);
    return found ? found.unwrap() : nullptr;
}

bool Program::hasFunctionName(DeclPtr<Module> module, StringId name) const {
    return functionNames.getValue(nameKey(module, name)).isJust();
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
 * The offsets are shifted past the two sentinels rather than the sentinels being large, so that a
 * declaration at region offset zero - which is a real address here - cannot collide with them.
 */
static constexpr U32 kNoHead = 0;
static constexpr U32 kOpenHead = 1;

static U32 headKey(DeclBase base, DeclPtr<Type> type) {
    if(!type) return kNoHead;

    auto& written = *base[type];
    if(written.kind == Type::Variable) return kOpenHead;

    // An instantiation is keyed by the declaration it came from: `Array(I32)` is an `Array`, and an
    // instance head written at `Array(a)` has to be reachable from every one of them.
    if(written.kind == Type::Record) {
        auto& record = (RecordType&)written;
        if(record.instanceOf) return record.instanceOf.offset + 2;
    }

    return type.offset + 2;
}

/*
 * The key of one applied argument list - the class, and each *deciding* position's constructor.
 *
 * The determined positions are left out because they are not what selection is asked with: a
 * functional dependency says the prefix decides the rest, so a key that included the rest would be a
 * key no call site could compute. `Class::isDetermined` is the one place that boundary is written.
 *
 * Answers false where a deciding position has no constructor, which is what makes a head open.
 */
static bool instanceKey(DeclBase base, DeclPtr<Class> klass, const DeclPtr<Type>* args, Size count,
                        U64& into) {
    auto& declared = *base[klass];
    auto key = InternKey().mix(klass.offset);
    auto keyed = true;

    for(Size i = 0; i < count; i++) {
        if(declared.isDetermined(U16(i))) continue;

        auto head = headKey(base, args[i]);
        if(head == kOpenHead || head == kNoHead) keyed = false;
        key = key.mix(head);
    }

    into = key.hash;
    return keyed;
}

bool Program::addInstance(DeclBase base, DeclPtr<Instance> instance) {
    auto& declared = *base[instance];
    U64 key = 0;
    if(!instanceKey(base, declared.klass, declared.args.pointer(base), declared.args.size(), key)) {
        auto entry = openInstances.add(declared.klass.offset);
        if(entry.existed) return false;

        *entry.value = instance;
        return true;
    }

    auto entry = instancesByHead.add(key);
    if(entry.existed) return false;

    *entry.value = instance;
    return true;
}

DeclPtr<Instance> Program::keyedInstance(DeclBase base, DeclPtr<Class> klass,
                                         const DeclPtr<Type>* args, Size count) const {
    if(!klass) return nullptr;

    U64 key = 0;
    if(!instanceKey(base, klass, args, count, key)) return nullptr;

    auto found = instancesByHead.getValue(key);
    return found ? found.unwrap() : nullptr;
}

DeclPtr<Instance> Program::openInstance(DeclPtr<Class> klass) const {
    if(!klass) return nullptr;

    auto found = openInstances.getValue(klass.offset);
    return found ? found.unwrap() : nullptr;
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
        case DeclAttribute::Builtin:      return intrinsic::kBuiltinAttribute;
        case DeclAttribute::Specialize:   return "specialize"_v;
        case DeclAttribute::Generic:      return "generic"_v;
        case DeclAttribute::Value:        return "value"_v;
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
