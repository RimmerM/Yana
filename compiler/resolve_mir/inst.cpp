#include "decl.h"

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
        case Place::Local:   return "local"_v;
        case Place::Global:  return "global"_v;
        case Place::Loan:    return "loan"_v;
        case Place::Address: return "address"_v;
        case Place::Field:   return "field"_v;
        case Place::Index:   return "index"_v;
        case Place::Deref:   return "deref"_v;
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
    return base[((NodeCall&)node).callee]->summary.suspension.get(true);
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
 * The key is a hash of the contents, and a bucket that already holds a *different* shape is
 * rehashed rather than shared. That is the part a hash-keyed intern table usually gets wrong: a
 * collision that silently returns the wrong entry is the same failure class as a truncated key, and
 * it fails identically silently.
 */
DeclPtr<OwnershipSchema> Shapes::intern(ExternalSlot* slots, Size count) {
    // The empty shape is the null handle. Every type in the language subset built so far has one,
    // so this is the path that is actually taken and it allocates nothing.
    if(!count) return nullptr;

    U64 hash = 0xcbf29ce484222325ull;
    auto mix = [&](U64 value) {
        hash = (hash ^ value) * 0x100000001b3ull;
    };

    for(Size i = 0; i < count; i++) {
        mix(slots[i].origin);
        mix(U64(slots[i].capability));
        for(auto step: slots[i].path.contents(*arena)) mix(step);
        mix(0x9e3779b97f4a7c15ull);
    }

    auto base = *arena;

    for(;;) {
        // All ones is `HashMap`'s empty-key sentinel, so a hash that lands on it takes the next
        // bucket rather than being uninsertable.
        if(hash == ~U64(0)) hash++;

        auto entry = shapes.add(hash);
        if(!entry.existed) {
            auto schema = new (arena) OwnershipSchema();
            schema->id = nextId++;

            for(Size i = 0; i < count; i++) {
                auto slot = new (arena) ExternalSlot(slots[i]);
                schema->slots.push(arena, slot - base);
            }

            *entry.value = schema - base;
            return *entry.value;
        }

        auto existing = base[*entry.value];
        if(existing->slots.size() == count) {
            auto same = true;
            for(Size i = 0; i < count && same; i++) {
                same = sameSlot(base, *base[existing->slots.get(base, i)], slots[i]);
            }

            if(same) return *entry.value;
        }

        hash++;
    }
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
bool Program::addFunctionName(DeclPtr<Module> module, StringId name, DeclPtr<Function> function) {
    auto entry = functionsByName.add(nameKey(module, name));
    if(entry.existed) return false;

    *entry.value = function;
    return true;
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

DeclPtr<Function> Program::findFunction(DeclPtr<Module> module, StringId name) const {
    auto found = functionsByName.getValue(nameKey(module, name));
    return found ? found.unwrap() : nullptr;
}

DeclPtr<Global> Program::findGlobal(DeclPtr<Module> module, StringId name) const {
    auto found = globalsByName.getValue(nameKey(module, name));
    return found ? found.unwrap() : nullptr;
}

DeclPtr<RecordType> Program::findRecord(DeclPtr<Module> module, StringId name) const {
    auto found = recordsByName.getValue(nameKey(module, name));
    return found ? found.unwrap() : nullptr;
}

/*
 * The instance table - one entry per (class, logical type).
 *
 * The key is the type's offset above the class, which is unique because a declaration region offset
 * is. Answering false rather than overwriting is the same rule the name tables keep and for the same
 * reason: a second `instance Drop(T)` that silently replaced the first would make which teardown
 * runs a property of declaration order that nothing reports.
 */
static U64 instanceKey(CoreClass klass, DeclPtr<Type> type) {
    return (U64(type.offset) << 8) | U64(klass);
}

bool Program::addInstance(DeclPtr<Instance> instance) {
    auto declaration = (*decl)[instance];
    auto entry = instancesByType.add(instanceKey(declaration->klass, declaration->type));
    if(entry.existed) return false;

    *entry.value = instance;
    return true;
}

DeclPtr<Instance> Program::findInstance(CoreClass klass, DeclPtr<Type> type) const {
    auto found = instancesByType.getValue(instanceKey(klass, type));
    return found ? found.unwrap() : nullptr;
}

StringView coreClassName(CoreClass klass) {
    switch(klass) {
        case CoreClass::Reclaim: return "Reclaim"_v;
        case CoreClass::Drop:    return "Drop"_v;
        case CoreClass::Count:   break;
    }

    return "?"_v;
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
