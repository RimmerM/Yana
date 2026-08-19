/*
 * Classification: what a type is, asked without reference to how it is laid out.
 *
 * Two families. The kind predicates - `isUnit`, `isInteger`, `isBorrow` - are one line each and are
 * here rather than inline in the header because they are the vocabulary the rest of the resolver
 * asks in, and a caller reading one should find the others beside it. The container accessors are
 * the other: "what does this hold", answered for slices, arrays, and the two container classes,
 * where the answer is a determined type-class parameter rather than a field.
 */

#include "type_internal.h"
#include "generic.h"
#include "module.h"
#include "name.h"
#include "index.h"

/*
 * Generic instantiation.
 *
 * A generic declaration is one type; each set of arguments it is applied to is another, interned
 * against the declaration so that `Maybe(Int)` written twice is one TypePtr. That identity is
 * what lets sameType() stay pointer equality, and it is what instance lookup matches against.
 *
 * An argument may itself be generic - `data Pair(a) = P(Maybe(a))` instantiates `Maybe` with a
 * type variable - so an instantiation is not necessarily concrete. Those exist to be substituted
 * later and never reach the IR, which is why they get no Repr.
 */

bool isGeneric(GlobalBase base, TypePtr type) {
    return type && base[type]->generic;
}

/*
 * Which of a context's variables this type mentions, as a bit per index.
 *
 * The same walk substituteType performs, answering "where would a substitution land" rather than
 * performing one. `generic` is the cutoff and it is exact rather than conservative - it is set
 * exactly when a variable is reachable inside the type - so a concrete argument costs one load.
 *
 * Sixty-four variables is the ceiling, which is the same one a return-root group has. A context
 * wider than that gets its high variables reported as absent, which makes the callers of this
 * conservative in the safe direction: a position is treated as not mentioning a variable, and the
 * caller then leaves that variable alone.
 */
void genVariablesIn(GlobalBase base, TypePtr type, U64& mask) {
    if(!isGeneric(base, type)) return;

    switch(base[type]->kind) {
        case Type::Gen: {
            auto index = ((GenType*)base[type])->index;
            if(index < 64) mask |= U64(1) << index;
            break;
        }
        case Type::Record: {
            auto record = (RecordType*)base[type];
            if(!record->instanceOf) break;

            for(auto arg: record->instanceArgs.contents(base)) genVariablesIn(base, arg, mask);
            break;
        }
        case Type::Tup: {
            auto tuple = (TupType*)base[type];
            for(Size i = 0; i < tuple->fields.size(); i++) {
                genVariablesIn(base, tuple->fields.get(base, i).type, mask);
            }
            break;
        }
        case Type::Ptr:
            genVariablesIn(base, ((PtrType*)base[type])->to, mask);
            break;
        case Type::Array: {
            // The count is walked beside the element: the `n` of `[a *n]` is a variable of this
            // context exactly as `a` is - Implementation-Const-Generics.md §2.3.
            auto array = (ArrayType*)base[type];
            genVariablesIn(base, array->content, mask);
            genVariablesIn(base, array->count, mask);
            break;
        }
        case Type::Vector: {
            auto vector = (VectorType*)base[type];
            genVariablesIn(base, vector->content, mask);
            genVariablesIn(base, vector->count, mask);
            break;
        }
        case Type::Borrow:
            genVariablesIn(base, ((BorrowType*)base[type])->to, mask);
            break;
        case Type::Fun: {
            auto function = (FunType*)base[type];
            for(auto arg: function->args.contents(base)) genVariablesIn(base, arg.type, mask);
            genVariablesIn(base, function->result, mask);
            break;
        }
        default:
            break;
    }
}

// One instantiation's argument, when it is an instantiation of `of` at all.
static TypePtr instanceArgument(Module& module, GlobalPtr<RecordType> of, TypePtr type) {
    auto global = *module.types;
    if(!of || !type || global[type]->kind != Type::Record) return nullptr;

    auto record = (RecordType*)global[type];
    if(record->instanceOf != of || record->instanceArgs.size() != 1) return nullptr;

    return record->instanceArgs.get(global, 0);
}

TypePtr arrayElement(Module& module, TypePtr type) {
    return instanceArgument(module, module.program.arrayType, type);
}

bool mapKeyValue(Module& module, TypePtr type, TypePtr& key, TypePtr& value) {
    auto global = *module.types;
    auto map = module.program.mapType;
    if(!map || !type || global[type]->kind != Type::Record) return false;

    auto record = (RecordType*)global[type];
    if(record->instanceOf != map || record->instanceArgs.size() != 2) return false;

    key = record->instanceArgs.get(global, 0);
    value = record->instanceArgs.get(global, 1);
    return true;
}

RecordType* inlineRefinement(Module& module, TypePtr type) {
    if(!arrayElement(module, type)) return nullptr;

    auto record = (RecordType*)(*module.types)[type];
    return record->inlineSlots ? record : nullptr;
}

TypePtr unrefined(GlobalBase base, TypePtr type) {
    if(!type || base[type]->kind != Type::Record) return type;

    auto canonical = ((RecordType*)base[type])->canonical;
    return canonical ? (Type*)base[canonical] - base : type;
}

TypePtr sliceElement(Module& module, TypePtr type) {
    return instanceArgument(module, module.program.sliceType, type);
}

TypePtr sliceLengthType(Module& module, TypePtr type) {
    auto global = *module.types;
    if(!sliceElement(module, type)) return nullptr;

    auto record = (RecordType*)global[type];
    if(record->constructors.isEmpty()) return nullptr;

    auto content = record->constructors.get(global, 0).content;
    if(!content || global[content]->kind != Type::Tup) return nullptr;

    // Two fields natively and three on JS, where a window carries where it starts as well as how
    // long it is (§4.3). `length` is field one in both, which is why the third one is last.
    auto fields = (TupType*)global[content];
    if(fields->fields.size() < 2) return nullptr;

    return fields->fields.get(global, 1).type;
}

TypePtr fixedElement(Module& module, TypePtr type) {
    auto global = *module.types;
    if(!type || global[type]->kind != Type::Array) return nullptr;

    return ((ArrayType*)global[type])->content;
}

/*
 * How wide one lane is, and therefore whether this type may be one at all.
 *
 * The *natural storage* of an integer rather than its declared bits, which is what makes a `@bits`
 * refinement an ordinary lane: `Count` is `@bits(30) U32` and occupies four bytes standing alone, so
 * a lane of it occupies four bytes too and `Vec(Count)` has the shape `Vec(U32)` has. Design-Vector
 * §2.2 accepts the refinement precisely so that the lanes keep the range `opt_range` derives from it.
 *
 * Zero for everything else, including `Bool`: one bit is not a lane, and what a comparison of a
 * vector answers is a `Mask` rather than a vector of booleans - see Design-Vector §2.4, which is the
 * whole argument for the two being different types.
 */
U32 laneStride(GlobalBase base, TypePtr type) {
    if(!type) return 0;

    auto value = base[type];

    if(value->kind == Type::Float) {
        return ((FloatType*)value)->width == FloatType::Double ? 8 : 4;
    }

    if(value->kind != Type::Int) return 0;

    auto bits = U32(((IntType*)value)->bits);
    if(bits < 8) return 0;

    auto storage = naturalStorageBits(bits) / 8;
    return storage == 1 || storage == 2 || storage == 4 || storage == 8 ? storage : 0;
}

bool isVectorType(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Vector && !((VectorType*)base[type])->isMask;
}

bool isMaskType(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Vector && ((VectorType*)base[type])->isMask;
}

TypePtr vectorLane(GlobalBase base, TypePtr type) {
    if(!type || base[type]->kind != Type::Vector) return nullptr;
    return ((VectorType*)base[type])->content;
}

U32 vectorLanes(GlobalBase base, TypePtr type) {
    if(!type || base[type]->kind != Type::Vector) return 0;
    return U32(writtenCount(base, ((VectorType*)base[type])->count).from(0));
}

bool isGrowableArray(Module& module, TypePtr type) {
    auto global = *module.types;
    auto array = module.program.arrayType;
    if(!array || !type || global[type]->kind != Type::Record) return false;

    // The declaration as well as an instance of it, because this is asked of a *signature*: `push`
    // says `Array(a)`, which is the generic declaration and has no `instanceOf` to compare.
    auto record = (RecordType*)global[type];
    return record->instanceOf == array || record == global[array];
}

TypePtr ownedElement(Module& module, TypePtr type) {
    if(auto element = arrayElement(module, type)) return element;

    return fixedElement(module, type);
}

TypePtr sliceOf(Module& module, TypePtr type) {
    if(!module.program.sliceType) return nullptr;

    // A borrow of a slice is that slice: `Flat(T)` owns nothing, so there is nothing for a second
    // level of borrowing to describe and no conversion to perform.
    if(sliceElement(module, type)) return type;

    // A `[T *n]` borrows as the same descriptor a growable one does, which is
    // Implementation-Containers.md §6's "as an immutable argument it produces a slice; as a
    // mutable-element argument a mutable slice. Both free, no coercion, no specialization." The
    // length is the type's own rather than a field read, and that is the whole of the difference.
    auto element = arrayElement(module, type);
    if(!element) element = fixedElement(module, type);
    if(!element) return nullptr;

    return instantiateRecord(module, module.program.sliceType, { &element, 1 }, kNullLocation);
}

// The determined half of a one-argument container class, or null when this type is not one of its
// instances. See contiguousElement's comment in type.h for why the two native containers are refused
// before the lookup rather than by it.
static TypePtr containerElement(Module& module, GlobalPtr<TypeClass> typeClass, TypePtr type) {
    if(!typeClass || !type) return nullptr;
    if(isGeneric(*module.types, type)) return nullptr;
    if(ownedElement(module, type) || sliceElement(module, type)) return nullptr;

    TypeList asked;
    asked.push(type);
    asked.push(nullptr);

    if(!resolveDetermined(module, typeClass, asked)) return nullptr;
    return asked[1];
}

TypePtr contiguousElement(Module& module, TypePtr type) {
    return containerElement(module, module.coreClasses.contiguous, type);
}

TypePtr chunkedElement(Module& module, TypePtr type) {
    return containerElement(module, module.coreClasses.chunked, type);
}

bool isBorrowLike(Module& module, TypePtr type) {
    return isBorrow(*module.types, type) || sliceElement(module, type) != nullptr;
}

static bool containsBorrowLikeAt(Module& module, TypePtr type, U32 depth) {
    if(!type || !depth) return false;
    if(isBorrowLike(module, type)) return true;

    auto base = *module.types;
    auto value = base[type];

    // A raw pointer is not descended into and neither is what it points at - `%T` is where this
    // analysis stops by construction. An owned container therefore does not contain a reference:
    // `Array(T)` holds a `Run(T)` holds a `%T`, and nothing on that path is a borrow.
    if(value->kind == Type::Tup) {
        for(auto field: ((TupType*)value)->fields.contents(base)) {
            if(containsBorrowLikeAt(module, field.type, depth - 1)) return true;
        }
    } else if(value->kind == Type::Record && ((RecordType*)value)->layout != RecordType::Enum) {
        for(auto constructor: ((RecordType*)value)->constructors.contents(base)) {
            if(containsBorrowLikeAt(module, constructor.content, depth - 1)) return true;
        }
    } else if(value->kind == Type::Array) {
        // `[&T *4]` holds four references and is exactly as unable to outlive them as a record of
        // four would be. Asked once because `n` copies of one answer is that answer.
        auto array = (ArrayType*)value;
        // A count this stage cannot read is a const variable, and the conservative answer for one is
        // that the array is not empty - an erased `[&T *n]` holds borrows for every `n` but zero.
        if(writtenCount(base, array->count).from(1)) return containsBorrowLikeAt(module, array->content, depth - 1);
    }

    return false;
}

bool containsBorrowLike(Module& module, TypePtr type) {
    return containsBorrowLikeAt(module, type, 8);
}

bool needsTeardown(Module& module, TypePtr type) {
    return ownershipOf(module, type).needsTeardown();
}

bool needsDrop(Module& module, TypePtr type) {
    return ownershipOf(module, type).drop != TeardownKind::None;
}

bool isUnit(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Unit;
}

bool isLiteral(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Literal;
}

bool isInteger(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Int;
}

bool isFloat(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Float;
}

bool isPointer(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Ptr;
}

bool isBorrow(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Borrow;
}

bool isFunction(GlobalBase base, TypePtr type) {
    return type && base[type]->kind == Type::Fun;
}

TypePtr pointeeType(GlobalBase base, TypePtr type) {
    return isPointer(base, type) ? ((PtrType*)base[type])->to : nullptr;
}

bool isNumeric(GlobalBase base, TypePtr type) {
    return isInteger(base, type) || isFloat(base, type);
}

bool isDirectType(GlobalBase base, TypePtr type) {
    if(!type || isUnit(base, type)) return false;

    auto value = base[type];

    // A raw pointer is an address held in a register, not something held in memory: `%T` is
    // direct however large `T` is. The memory it names is reached through a place instead, and a
    // borrow is the same shape with checking attached.
    //
    // A vector is direct for the reason Design-Vector §2.5 gives, and the direction of that contract
    // is the point: resolve *states* it and every target's calling convention is bound by it, so a
    // backend that cannot pass one in a register has to fail loudly rather than quietly spill. It is
    // also what makes a `return` marker on a vector parameter a diagnostic - the caller's storage did
    // not travel, so a vector parameter roots no borrow. See checkAbiContract in repr.cpp.
    if(value->kind == Type::Int || value->kind == Type::Float || value->kind == Type::Ptr ||
       value->kind == Type::Borrow || value->kind == Type::Vector) {
        return true;
    }

    return value->kind == Type::Record && ((RecordType*)value)->layout == RecordType::Enum;
}

bool isMemoryType(GlobalBase base, TypePtr type) {
    return type && !isUnit(base, type) && !isDirectType(base, type);
}

bool arrivesAsCopy(GlobalBase base, TypePtr type) {
    return isDirectType(base, type) && !isPointer(base, type);
}
