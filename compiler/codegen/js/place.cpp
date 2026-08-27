#include "build.h"

/*
 * Values and places.
 *
 * The whole of the difference between this backend and the native one is here: a place stays a
 * `(object, property)` chain instead of becoming a base address plus a constant offset. Every
 * projection is a property, an array index, or - for a Downcast, which native spends a payload
 * offset on - nothing at all.
 *
 * The erased half is at the bottom, and is the one place this backend reads a *layout* rather than a
 * structure: a compiler-built constant table is an array of 32-bit cells, so the load native writes
 * as `[base + 24]` is `table[6]` here.
 */

namespace js {

JsPtr<Expr> constantValue(Gen& g, Value& value) {
    switch(value.kind) {
        case Value::ConstInt: {
            auto bits = ((ConstInt&)value).value;

            if(isBool(g, value.type)) return number(g, bits ? 1 : 0);

            if(auto integer = intType(g, value.type)) {
                if(isLong(g, value.type)) return bigInt(g, bits, integer->isSigned);

                /*
                 * Read back at the type's own width rather than at 32.
                 *
                 * The constant arrives as a raw bit pattern, so producing the `number` that denotes
                 * it is a sign extension from bit `bits - 1` for a signed type and a mask for an
                 * unsigned one. `I32`/`U32` did both at once for the 32-bit tower and are exactly
                 * wrong for the 33-to-53-bit one, where the value does not fit a 32-bit integer.
                 */
                auto width = heldBits(g, *integer);
                auto mask = width >= 64 ? ~U64(0) : (U64(1) << width) - 1;
                auto masked = bits & mask;

                if(integer->isSigned && width < 64 && (masked & (U64(1) << (width - 1)))) {
                    return number(g, -F64((mask - masked) + 1));
                }

                return number(g, F64(masked));
            }

            // A null pointer written as an integer, which is how `cast(0)` reaches here. Nothing
            // else can be a raw address at compile time.
            if(value.type && g.global[value.type]->kind == Type::Ptr) {
                return bits ? number(g, F64(bits)) : nullValue(g);
            }

            /*
             * A payload-free sum's constructor, which is a number here as much as an integer is -
             * and had to be read back at its own width for the same reason, which it was not.
             *
             * `@value(-1)` is stored as the bit pattern of -1 in sixty-four bits, so the fall-through
             * below turned `Failed` into 1.8446744073709552e19: every comparison against it was
             * wrong, `valueOf` reported that number instead of -1, and the native target had none of
             * it because a raw pattern is what a register holds there anyway.
             *
             * The signedness is read off the *declaration* rather than off a layout field, and that
             * is the honest place for it: what makes a discriminant signed is a constructor pinning
             * a negative number, which is the same question `enumNumberType` asks in
             * resolve/intrinsic.cpp. A sum whose values are all non-negative masks instead, so a
             * `@value(200)` in one byte stays 200.
             */
            auto declared = value.type ? g.global[canonicalType(g.global, value.type)] : nullptr;

            if(declared && declared->kind == Type::Record &&
               ((RecordType*)declared)->layout == RecordType::Enum) {
                auto& repr = g.repr.of(value.type);
                auto width = repr.size && repr.size <= 8 ? U32(repr.size) * 8 : 64;
                auto mask = width >= 64 ? ~U64(0) : (U64(1) << width) - 1;
                auto masked = bits & mask;

                auto signedTag = false;
                for(auto constructor: ((RecordType*)declared)->constructors.contents(g.global)) {
                    if(constructor.value < 0) signedTag = true;
                }

                if(signedTag && (masked & (U64(1) << (width - 1)))) {
                    /*
                     * The widest case is its own line rather than a term in the one below, because
                     * `mask - masked + 1` is the negation written to avoid overflowing at the width
                     * it is performed at - and at sixty-four it overflows anyway. Reading the
                     * pattern back as a signed 64-bit integer is the same answer and needs no
                     * arithmetic, which is what `width < 64` used to buy by declining to answer:
                     * `@value(-5000000000)` came out as 1.8446744068709552e19, which is past what a
                     * `number` holds exactly, so it read back as -4999999488 and named the wrong
                     * constructor. The narrower widths never showed it because their unsigned
                     * reading is exact and `BigInt.asIntN` recovered the sign downstream.
                     */
                    if(width >= 64) return number(g, F64(I64(bits)));
                    return number(g, -F64((mask - masked) + 1));
                }

                return number(g, F64(masked));
            }

            return number(g, F64(bits));
        }
        case Value::ConstFloat:
            return number(g, F64(((ConstFloat&)value).value), false);
        case Value::ConstDouble:
            return number(g, ((ConstDouble&)value).value, false);
        case Value::ConstString:
            // A string literal is a host string constant here and nothing else - see ConstString.
            // The escaping is `writeStringLiteral`'s, which is the same one every property name and
            // program constant already goes through.
            return asExpr(g, make<StringExpr>(g, ((ConstString&)value).text));
        default:
            return nullValue(g);
    }
}

JsPtr<Expr> useValue(Gen& g, ModulePtr<Value> pointer) {
    if(!pointer) return nullValue(g);
    if(auto found = g.values.get(U32(pointer))) return found.unwrap();

    /*
     * A function value being carried as its two words, in a position that wants one value.
     *
     * Built here rather than once beside the parts - which is what a flattened reference parameter
     * does in genBody - because the two words of a *local* are variables the `Init`s assign, so an
     * object built before those writes would hold what the slot held before it was filled. Built at
     * the use, it holds what the words hold there.
     *
     * The positions that reach this are the ones flattening cannot cover: a return, a store into a
     * record field, an erased boundary. Everything else - the call, the two-word argument, the
     * teardown - asks funPartsOf instead and never builds an object at all.
     */
    if(auto parts = g.funParts.get(U32(pointer))) return materializeFun(g, parts.unwrap());

    // And a narrow reference being carried as its parts, for the same positions. A *parameter* that
    // needs an object has one built once at the top of the body and registered above, so what
    // reaches here is a local `prepareRefLocals` flattened and a value loaded out of one - neither of
    // which exists before the writes that fill it, which is why this is built at the use.
    if(auto parts = g.flatRefs.get(U32(pointer))) return materializeRef(g, parts.unwrap());

    // And a vector being carried as its lanes, for the same three positions - see VecParts. Built
    // here rather than beside the lanes for the same reason: a lane is an expression over whatever
    // its operands hold *there*, and an array built earlier would hold what they held earlier.
    if(auto parts = g.vecParts.get(U32(pointer))) return materializeVec(g, parts.unwrap());

    auto& value = *g.local[pointer];
    if(isConstant(value)) {
        auto result = constantValue(g, value);
        g.values.add(U32(pointer), result);
        return result;
    }

    /*
     * A value of unit type, which is nothing and is named by nothing.
     *
     * `define` binds no variable for one - there is nothing to hold - so anything that reads one
     * arrives here having found no entry. That is not the failure below: it is the model working,
     * and `null` is this target's spelling of a value with no representation. The positions where a
     * unit *matters* leave it out before asking - a parameter that does not exist, a `return` with
     * nothing to return - so what reaches here is a use in a position that will discard it anyway.
     *
     * Reached by a generic body specialized at `{}`: `match m: Just(inner) -> inner` binds a payload
     * that occupies nothing, and the arm has to produce it.
     */
    if(isUnit(g.global, value.type)) return nullValue(g);

    g.context.diagnostics.error("internal error: a resolve value was used before it was generated"_v,
                                value.source);
    return nullValue(g);
}

/*
 * Whether what a value names is storage somebody else still holds.
 *
 * The question a write has to ask on this target and on no other. Native's `load` of an aggregate
 * computes an address and copies nothing; what makes the result a *value* there is the consumer,
 * because every one of them copies - a store is a `memcpy`, a `return` is a `memcpy` into the
 * caller's buffer, and an aggregate parameter arrives as the caller's address (see `arrivesAsCopy`)
 * so the callee's first write of it is a `memcpy` too. Here a place read hands back the object
 * itself and `v = other` beside it rebinds a name, so the copy has to be put back by hand.
 *
 * Stated as the *fresh* kinds rather than the aliasing ones, and it is deliberate that the default
 * is to duplicate: a missed copy is two names for one object and shows up as a wrong answer far
 * from here, where a needless one is only a needless one. What earns a place on this list is a
 * value that either has no storage behind it or has storage the resolver has already proved dead:
 *
 *  - a *move* is exactly that proof, and it is the whole of what a move is here (README, §3.3);
 *  - a *copy* is the duplicate itself, so duplicating it again would be the second of two;
 *  - a whole local used as a value is the move of that local, which is the only form the resolver
 *    emits one in - a local that stays live is read with a `load` and lands in the default;
 *  - a *call* hands back a value of its own, which the `Ret` below guarantees by asking this same
 *    question of whatever it returns;
 *  - a host call answers a fresh object or nothing that is one.
 *
 * A join is whichever of its inputs the edge took, so it is as fresh as the least fresh of them.
 * `pick(c) = Buf {bytes: if c then a else b}` is that shape and it is three lines of Yana; the depth
 * cap is for a loop-carried phi, which reaches itself and is answered conservatively.
 *
 * And *every* kind is aliasing where the body keeps the value twice, fresh or not - a second keeper
 * of one object is a second name for it however fresh the first one was. See Gen::keptTwice for why
 * that is a case at all and why neither of the two may go free.
 */
static bool namesLiveStorage(Gen& g, ModulePtr<Value> pointer, U32 depth) {
    if(!pointer) return false;
    if(depth >= 8) return true;
    if(g.keptTwice.contains(U32(pointer))) return true;

    auto& value = *g.local[pointer];

    switch(value.kind) {
        case Value::Move:
        case Value::Copy:
        case Value::Alloc:
        case Value::Call:
        case Value::CallDyn:
        case Value::Native:
            return false;

        case Value::Phi: {
            for(auto input: ((InstPhi&)value).inputs.contents(g.local)) {
                if(namesLiveStorage(g, input.value, depth + 1)) return true;
            }

            return false;
        }
        case Value::Select: {
            auto& select = (InstSelect&)value;
            return namesLiveStorage(g, select.whenTrue, depth + 1) ||
                   namesLiveStorage(g, select.whenFalse, depth + 1);
        }
        default:
            return true;
    }
}

/*
 * Whether a type is one this duplicate is *for*.
 *
 * `cloneValue` is a structural copy, which is to say it is native's `memcpy` of one value and
 * nothing more: it copies an `Array(a)`'s two properties and shares the run they point at, exactly
 * as copying the `{ptr, len}` pair over there does. That is the whole answer for a type nobody owes
 * a teardown, and it is never the answer for a type that does - a bitwise duplicate of a droppable
 * value is two owners of one buffer, and the copy of one is its `Sink`, reached through an
 * `InstCopy` that has its own destination.
 *
 * So a droppable type declines, and there is no site it leaves uncovered. Every write of one into
 * storage is a *move* by construction: `transferFrom` in analyze_effects.cpp records the handover
 * and declines only "anything with no teardown to hand over", which is the same line drawn from the
 * other side. Where the source is dead nothing is owed, and where it is live a structural clone
 * would not have been the copy anyway - it would have been the double free.
 *
 * `isGeneric` declines for a different reason and it is not a hole either: an erased body has no
 * shape to duplicate from, so `storeInto` sends a write of one through the descriptor's `kCopyInit`
 * - a copy already, and the one gap 4 exists to have closed. Asking `cloneValue` here instead would
 * report every erased write as a shape this target cannot see.
 */
static bool copiesStructurally(Gen& g, TypePtr type) {
    if(!isJsObject(g, type) || isGeneric(g.global, type)) return false;

    return !g.function || !needsTeardown(*g.function->module, type);
}

/*
 * A value in a position that *keeps* it - written into storage, or handed back through a `return`.
 *
 * The duplicate is `cloneValue`'s, which is the same one an `InstCopy` emits: the two are the same
 * operation reached for different reasons, and a type whose copy is not structural has an
 * `InstCopy` with its own `Sink` rather than arriving here.
 */
JsPtr<Expr> keptValue(Gen& g, TypePtr type, ModulePtr<Value> value, JsPtr<Expr> source,
                      LocationId where) {
    if(!copiesStructurally(g, type)) return source;
    if(!namesLiveStorage(g, value, 0)) return source;

    return cloneValue(g, type, source, where);
}

bool keepsLiveStorage(Gen& g, TypePtr type, ModulePtr<Value> value) {
    return copiesStructurally(g, type) && namesLiveStorage(g, value, 0);
}

/*
 * The same question with the type left out - see keepsLiveStorage, which is this and the shape test
 * together.
 *
 * The erased write is the one caller, and it is why the two are separate: `copiesStructurally`
 * declines a generic type because an erased body has no shape to walk, and what it duplicates *with*
 * is the descriptor's `copyInit` instead. So the shape half of the question is already answered
 * there, and what is left is the half that decides whether a duplicate is needed at all.
 */
bool aliasesLiveStorage(Gen& g, ModulePtr<Value> value) {
    return namesLiveStorage(g, value, 0);
}

/*
 * Whether a `return` may hand the object over rather than duplicate it.
 *
 * The one position where the *frame* is the answer. `namesLiveStorage` is asked about a value and
 * has to hold for every position it is asked at, so a read of a local lands in its default: the
 * local goes on existing and a second name for its contents would be a second name. At a `ret` it
 * does not go on existing - the frame is what dies, and its storage dies with it - so what is left
 * to ask is whether anything *outside* the frame can still reach the object.
 *
 * Nothing can, and it is the rest of this file that makes that true rather than an assumption:
 *
 *  - every write into storage duplicates a value that was still somebody else's, so a slot reached
 *    from a global or through a borrow holds an object of its own rather than this one;
 *  - a droppable type is the exception to that and does not need to be one, because a write of one
 *    is a *move* (see copiesStructurally) - a local written somewhere else is moved-from and cannot
 *    be what a later `ret` names;
 *  - a value kept in two positions is `Gen::keptTwice`, which is asked here as it is asked there.
 *
 * So the slot has to be one this frame genuinely owns, which is `sinkValue`'s test in
 * resolve/expr_construct.cpp and is the same list for the same reason: a `&` parameter, a borrowed
 * slot, a closure environment and a materialized field temporary all name storage that outlives the
 * return, and a view names another local's. `sinkValue` additionally declines a *projected* place,
 * which is the one thing not repeated here - `local@Exit` is a payload of the frame's own storage
 * and dies with the rest of it - and that is the whole of what this recovers over the resolver's
 * own answer.
 */
static bool handsOverFrameStorage(Gen& g, ModulePtr<Value> value) {
    if(!value || g.keptTwice.contains(U32(value))) return false;
    if(!g.function) return false;

    auto& instruction = *g.local[value];
    if(instruction.kind != Value::LoadPlace) return false;

    auto& place = ((InstLoadPlace&)instruction).place;
    if(place.root != PlaceRoot::Local || place.local >= g.function->localCount()) return false;

    auto slot = g.function->localAt(g.local, place.local);
    if(slot.borrowed || slot.closureEnv || slot.materialized) return false;
    if(slot.viewOf != maxLimit<U32>) return false;

    // A parameter slot names the caller's storage under every convention but `->`, which is the one
    // the caller recorded a handover for. Recognized the way every pass recognizes a parameter: its
    // slot is named by an Arg, exactly as an allocation's is named by its Alloc.
    auto parameter = slot.value && g.local[slot.value]->kind == Value::Arg;

    return !parameter || slot.convention == ast::BindType::Sink;
}

JsPtr<Expr> returnedValue(Gen& g, TypePtr type, ModulePtr<Value> value, JsPtr<Expr> source,
                          LocationId where) {
    if(handsOverFrameStorage(g, value)) return source;

    return keptValue(g, type, value, source, where);
}

JsPtr<Expr> globalValue(Gen& g, ModulePtr<Global> pointer) {
    auto found = g.globalNames.get(U32(pointer));
    return variable(g, found ? found.unwrap() : Name {});
}

/*
 * The parts of a narrow reference, from whichever of the two forms it is in.
 *
 * A flattened one has them in variables and there is no object to read; anything else is the
 * `{$o,$k,$s}` object and they are its properties. Every consumer goes through here rather than
 * projecting the object itself, which is what keeps flattening a decision about the *convention*
 * rather than something each use has to know about.
 */
RefParts refPartsOfExpr(Gen& g, JsPtr<Expr> reference, bool fun) {
    RefParts parts;
    parts.owner = field(g, reference, g.refObject);
    parts.key = field(g, reference, g.refKey);

    // The third part is the shift for a narrow value and the second key for a function value, and
    // never both: one names where inside a word the value sits, the other names a second word.
    if(fun) {
        parts.envKey = field(g, reference, g.refEnvKey);
    } else if(narrowRefCarriesScale(g)) {
        parts.scale = field(g, reference, g.refScale);
    }

    return parts;
}

RefParts refPartsOf(Gen& g, ModulePtr<Value> reference) {
    if(auto found = g.flatRefs.get(U32(reference))) return found.unwrap();

    auto pointee = referencedType(g, g.local[reference]->type);
    return refPartsOfExpr(g, useValue(g, reference), isFunValue(g, pointee));
}

/*
 * The two words of a place's root, where the root is genuinely held as two variables.
 *
 * Keyed on the parts existing *and* on the local being one this body holds flat, which is one more
 * condition than the parts alone. A function-value parameter arrives flat because the calling
 * convention says so, and `prepareFunLocals` may still have declined to hold it that way - because
 * something borrows the whole of it, and a reference here is an owner and two keys. genBody
 * reassembles exactly those into an object at the top, and this is what sends the walk to it.
 */
static Maybe<FunParts&> flatFunPartsFor(Gen& g, const Place& place, ModulePtr<Value> value) {
    if(place.local < g.flatFuns.size() && !g.flatFuns[place.local]) return Nothing();
    return g.funParts.get(U32(value));
}

FunParts funPartsOfExpr(Gen& g, JsPtr<Expr> value) {
    FunParts parts;
    parts.code = field(g, value, g.codeField);
    parts.env = field(g, value, g.envField);

    return parts;
}

FunParts funPartsOf(Gen& g, ModulePtr<Value> value) {
    if(auto found = g.funParts.get(U32(value))) return found.unwrap();
    return funPartsOfExpr(g, useValue(g, value));
}

VecParts newVecParts(Gen& g, U32 lanes) {
    VecParts parts;
    parts.lanes = (JsPtr<Expr>*)g.file.arena.alloc(sizeof(JsPtr<Expr>) * lanes);
    parts.count = U16(lanes);

    for(U32 i = 0; i < lanes; i++) parts.lanes[i] = nullptr;
    return parts;
}

VecParts vecPartsOfExpr(Gen& g, JsPtr<Expr> value, U32 lanes) {
    auto parts = newVecParts(g, lanes);
    for(U32 i = 0; i < lanes; i++) parts.lanes[i] = index(g, value, i);

    return parts;
}

VecParts vecPartsOf(Gen& g, ModulePtr<Value> value) {
    if(auto found = g.vecParts.get(U32(value))) return found.unwrap();
    return vecPartsOfExpr(g, useValue(g, value), vectorLanes(g.global, g.local[value]->type));
}

// The array form, on the same terms as materializeFun below: JS has no multi-value return, so a
// vector that is returned, stored in a record or handed across an erased boundary has to become one
// value again. It is a host array rather than a typed one because a mask's lanes are booleans and a
// `Vec(I8)`'s are numbers a typed view would coerce a second time - the array is a boundary form,
// not a representation anything computes with.
JsPtr<Expr> materializeVec(Gen& g, VecParts parts) {
    auto array = make<ArrayExpr>(g);
    for(auto lane: parts.contents()) array->values.push(g.file.arena, lane);

    return asExpr(g, array);
}

// The object form, on the same terms as materializeRef below: JS has no multi-value return, so a
// function value that is returned, stored in a record or handed across an erased boundary has to
// become one value again.
JsPtr<Expr> materializeFun(Gen& g, FunParts parts) {
    auto pair = make<ObjectExpr>(g);
    pair->properties.push(g.file.arena, Property { g.codeField, parts.code });
    pair->properties.push(g.file.arena, Property { g.envField, parts.env });

    return asExpr(g, pair);
}

// The object form, for the uses flattening cannot cover: JS has no multi-value return, so a
// reference that is returned, stored or captured has to become one value again.
JsPtr<Expr> materializeRef(Gen& g, RefParts parts) {
    auto pair = make<ObjectExpr>(g);
    pair->properties.push(g.file.arena, Property { g.refObject, parts.owner });
    pair->properties.push(g.file.arena, Property { g.refKey, parts.key });
    if(parts.scale) pair->properties.push(g.file.arena, Property { g.refScale, parts.scale });
    if(parts.envKey) pair->properties.push(g.file.arena, Property { g.refEnvKey, parts.envKey });

    return asExpr(g, pair);
}

/*
 * The walk both placeExpr and placeType are.
 *
 * The property chain is built only for a caller that asked for one, but the walk itself is the same
 * either way and deliberately so: the two answers have to agree about which projection is a property
 * and which is free, and a second copy of this loop is exactly where they would stop agreeing.
 */
namespace {

TypePtr walkJsPlace(Gen& g, const Place& place, JsPtr<Expr>* expr, Size limit = maxLimit<Size>,
                  PlaceBits* bits = nullptr, FunParts* funParts = nullptr,
                  bool* hostProperty = nullptr, bool* packedWord = nullptr,
                  RefParts* refParts = nullptr) {
    TypePtr type = nullptr;
    PlaceBits within;

    // Whether the step just taken landed on a `@host` field, which only the *last* one decides -
    // reset at every step so that `xs.length.something` could never inherit it. See storeInto,
    // which is the one caller that asks and the one place the answer changes what is emitted.
    auto elided = false;

    // And whether it landed on the *word* a packed field lives in rather than on the field - see
    // ProjectionKind::Unit. Reset per step on the same terms, though that projection is always the
    // last one: what makes it worth reporting is that `type` stays the packed field's here while
    // the expression becomes something wider, which is the one place the two come apart.
    auto whole = false;

    // The two words of the root, where the root is a function value held as two variables. Valid
    // only for that root, and consumed by the first `Type::Fun` field step - see the local case.
    FunParts rootFun;

    // And the parts of the root, where the root is a local this body holds as a reference's parts -
    // see prepareRefLocals. Every projection such a place can carry is free on a folded record, so
    // this survives the walk untouched and is joined up at the end.
    RefParts rootRef;

    /*
     * Whether this reference names a *run* of values rather than one.
     *
     * `Place::atPointer(p)` with no path is the single value `p` names, which on this target is the
     * box that stands in for one where the value is not an object. The same root with an `Index` in
     * front of it is element `i` of what `p` names - Implementation-Containers.md §14.1 - and there
     * is no box anywhere in that: `p` is the host array and the index reaches into it.
     *
     * Read off the *path* rather than off the type, because the type is the element's either way.
     * That is the same distinction native draws by adding a scaled offset to the address instead of
     * loading through it.
     *
     * Asked of the whole path rather than of the prefix `limit` selects, because it is a fact about
     * what the *root* names: a caller walking as far as the index alone - which is what builds the
     * `$o` half of a reference to an element - still wants the array rather than a box of it.
     */
    auto indexedRoot = [&]() {
        auto projections = place.projections;
        if(!projections.size()) return false;

        return projections.get(g.local, 0).kind == ProjectionKind::Index;
    }();

    if(place.root == PlaceRoot::Global) {
        type = g.local[place.global]->type;

        if(expr) {
            // Read through the box where this one has it, which is what a reference to it names -
            // see Gen::boxedGlobals.
            *expr = globalValue(g, place.global);
            if(g.boxedGlobals.contains(U32(place.global))) *expr = field(g, *expr, g.boxField);
        }
    } else if(place.root == PlaceRoot::Pointer || place.root == PlaceRoot::Borrow) {
        // A borrow and a raw pointer are the same reference with different amounts of knowledge
        // behind them, and neither has a representation of its own here: it is the object it names,
        // or the box that stands in for one.
        auto referenced = g.local[place.pointer]->type;

        type = place.root == PlaceRoot::Borrow
            ? ((BorrowType*)g.global[referenced])->to
            : pointeeType(g.global, referenced);

        if(expr) {
            /*
             * A reference to a narrow value is the (object, property, shift) triple a place into a
             * bit range is - the pair reified, plus where inside the named word the value starts.
             *
             * The shift is what makes one compiled body serve a field of a scalarized record, a
             * co-packed field and a whole local: the callee has only the pointee type, so the *mask*
             * is a constant it can compute and the *shift* is the one thing it cannot. See
             * genNarrowRef, which is the other half.
             *
             * Asked before the value itself, because a flattened reference *has* no single value -
             * its parts are three variables and there is no object anywhere to ask `useValue` for.
             */
            /*
             * A **borrow** and a raw **pointer** are the two reference forms this target has, and
             * they are not the same form - see refIsTriple. A `&` names a slot and takes the
             * triple; a `%a` is caller-provided storage and takes the box that stands in for an
             * address, which is what the erased ABI hands a witness accessor and what a derived
             * teardown's `%value` parameter receives.
             *
             * Not a mutability question and not a width question: both sides of a call read the
             * *declaration*, and `&T` against `%T` is exactly what a declaration says.
             *
             * An *indexed* root is neither - it is the host array itself, and the projection below
             * reaches into it - which is what `indexedRoot` takes out of both branches.
             */
            auto triple = place.root == PlaceRoot::Borrow ? refIsTriple(g, type)
                                                         : addressIsTriple(g, type);

            if(!indexedRoot && triple && isFunValue(g, type)) {
                // Both words, off the one owner the reference carries - see refIsTriple. Left as the
                // pair for the `Type::Fun` step below, or joined up by the tail of this walk.
                auto parts = refPartsOf(g, place.pointer);
                rootFun.code = elementAt(g, parts.owner, parts.key);
                rootFun.env = elementAt(g, parts.owner, parts.envKey);
                *expr = nullptr;
            } else if(!indexedRoot && triple) {
                auto parts = refPartsOf(g, place.pointer);
                *expr = elementAt(g, parts.owner, parts.key);

                // Only a narrow pointee is a bit range. A whole value occupies what it names, so the
                // scale it was handed is one and there is nothing to shift out of anything.
                if(parts.scale && isNarrowValue(g.global, type)) {
                    within.scale = parts.scale;
                    within.width = narrowWidth(g, type);
                    within.word = maxWordBits(g);
                }
            } else {
                *expr = useValue(g, place.pointer);

                // The box an address is, read through. An indexed root is the host array itself, an
                // object is its own reference, and an alias is the storage under a second name -
                // see prepareLocals - so none of the three has one.
                if(!indexedRoot && !isJsObject(g, type) &&
                   !g.aliasBorrows.contains(U32(place.pointer))) {
                    *expr = field(g, *expr, g.boxField);
                }
            }
        }
    } else if(place.local < g.function->localCount()) {
        auto root = g.function->localAt(g.local, place.local);
        type = root.type;

        if(expr) {
            // The slot behind a `&` parameter of non-object type holds one of those triples rather
            // than storage of its own - and holds it as three variables where it arrived flattened,
            // which is why this is asked before the value is.
            //
            // `Ref` and not `borrowed` alone, because the two are not the same convention: a `&`
            // the *program* wrote is a reference and takes the form refIsTriple decides, while a
            // derived teardown's parameter is handed the box `referenceTo` makes and is borrowed
            // without being one. Dropping this test compiled every derived `drop` to read `$o`/`$k`
            // out of a `{$v: …}` its caller built.
            if(root.borrowed && root.convention == ast::BindType::Ref && refIsTriple(g, type) &&
               isFunValue(g, type)) {
                auto parts = refPartsOf(g, root.value);
                rootFun.code = elementAt(g, parts.owner, parts.key);
                rootFun.env = elementAt(g, parts.owner, parts.envKey);
                *expr = nullptr;
            } else if(root.borrowed && root.convention == ast::BindType::Ref && refIsTriple(g, type)) {
                auto parts = refPartsOf(g, root.value);
                *expr = elementAt(g, parts.owner, parts.key);

                if(parts.scale && isNarrowValue(g.global, type)) {
                    within.scale = parts.scale;
                    within.width = narrowWidth(g, type);
                    within.word = maxWordBits(g);
                }
            } else if(auto parts = flatFunPartsFor(g, place, root.value)) {
                /*
                 * A function value held as its two words, which is storage with no object behind it.
                 *
                 * Left null here and answered by the `Type::Fun` field step below, because what this
                 * root *has* is two variables and neither of them is the value: asking `useValue`
                 * would build the object the flattening exists to remove, at a place walk that was
                 * only ever going to project one word out of it again.
                 *
                 * The two shapes that reach here are the only two a place into a function value can
                 * be - `local` and `local@Fun.word` - because a `Fun` is a leaf and there is nothing
                 * inside a word to descend into. The whole-value case is picked up at the end, where
                 * an empty walk is what says the caller wanted the value rather than a word of it.
                 *
                 * Keyed on the parts actually existing rather than on `flatFuns`, and the difference
                 * matters for a function-value parameter a wide signature declined to flatten: that
                 * local is one this body would happily hold flat, but what arrived is an object, and
                 * taking it apart here only to build another one at the end of the walk would be a
                 * copy of a value that was already the right shape.
                 */
                rootFun = parts.unwrap();
                *expr = nullptr;
            } else if(place.local < g.flatRefLocals.size() && g.flatRefLocals[place.local] &&
                      !g.boxed[place.local]) {
                /*
                 * A local held as a reference's parts, which is storage with no object behind it -
                 * see prepareRefLocals.
                 *
                 * The owner is what the walk carries, and that is not an arbitrary choice among the
                 * three: the one projection such a place can take is the folded discriminant, whose
                 * test is `$o === null`, so leaving the owner here is what makes `decodeNicheTag`
                 * read the right variable without knowing this root exists. Every other place is the
                 * whole value, which the end of the walk hands over as the parts or joins up.
                 */
                rootRef = refPartsOf(g, root.value);
                *expr = rootRef.owner;
            } else if(auto box = g.localBoxes.get(place.local)) {
                // A local whose box was built at its defining value rather than at an allocation -
                // see prepareLocals. Named here rather than re-derived, because what `useValue`
                // answers for that value is already the `box.$v` this would otherwise append to.
                *expr = field(g, box.unwrap(), g.boxField);
            } else {
                *expr = useValue(g, root.value);
                if(place.local < g.boxed.size() && g.boxed[place.local]) {
                    /*
                     * A boxed local nothing built a box for - see prepareLocals, where `boxless` is
                     * the complement of the four places that build one.
                     *
                     * Reported rather than emitted. What this line would otherwise produce is `v.$v`
                     * over a bare value, which reads back `undefined` and takes every reference made
                     * from it with it - and none of that fails here, so the report would come from
                     * whatever eventually read the reference, in another function, on one target.
                     * Both boxing rules this file has were added after exactly that, so the gap is
                     * worth a diagnostic rather than the assumption that there is no gap left.
                     */
                    if(place.local < g.boxless.size() && g.boxless[place.local]) {
                        g.context.diagnostics.error("the JS target cannot make a reference to this local - it is stored as a box and nothing built one, which is the gap Implementation-Simplification.md B describes"_v,
                                                    g.local[root.value]->source);
                    }

                    *expr = field(g, *expr, g.boxField);
                }
            }
        }
    } else {
        if(expr) *expr = nullValue(g);
        return nullptr;
    }

    /*
     * The path, over the walk every consumer of a `Place` shares - see resolve/place.h. What is this
     * one's own is the property chain and the bit range; the type each step arrives at is not, and
     * carrying it here as well is what used to make this a fourteenth copy of the same switch.
     *
     * `limit` stops before the trailing Property projection, which is how the *owner* of a
     * constrained field is asked for: the field is reached by calling the witness with that owner
     * rather than by naming a property of it. See propertySlotOf in inst.cpp.
     */
    ::walkPlace(*g.program.core, *g.function, place, [&](const PlaceStep& step) {
        type = step.type;
        elided = false;
        whole = false;
        if(step.broken) return false;

        switch(step.kind) {
            case ProjectionKind::Discriminant: {
                // An enum *is* its discriminant, so there is nothing to project out of it.
                auto record = recordType(g, step.owner);
                if(record && !discriminantOnly(g.global, *record)) {
                    // Neither is a folded record, for the stronger reason: its tag is not stored
                    // anywhere at all. The place stays on the payload and the load and the store
                    // intercept - see PlaceBits::foldedTag.
                    if(g.repr.of(step.owner).isNicheFolded()) {
                        within.foldedTag = step.owner;
                    } else if(expr) {
                        *expr = field(g, *expr, g.tagField);
                    }
                }

                break;
            }
            case ProjectionKind::Downcast: {
                auto record = recordType(g, step.owner);
                if(!record) break;

                auto content = record->constructors.get(g.global, step.index).content;

                /*
                 * Free, like the native offset it corresponds to: a tuple payload is flattened into
                 * the record's own object, so the constructor's fields are already properties of it.
                 * Which payloads are flattened and which are one `$p` is `payloadIsOneProperty`,
                 * shared with `eachProperty` - the two are one question, and this is the reader of
                 * it rather than a second copy of the rule.
                 *
                 * Free in the strongest sense for a folded record, which *is* its payload: there is
                 * nothing anywhere to read, since what the walk has in hand already is the payload or
                 * the pattern that says there is none. Native spends a payload offset of zero here for
                 * exactly the same reason.
                 */
                if(expr && !g.repr.of(step.owner).isNicheFolded() &&
                   record->layout == RecordType::Multi && content &&
                   !isUnit(g.global, content) && payloadIsOneProperty(g, content)) {
                    *expr = field(g, *expr, g.payloadField);
                }

                break;
            }
            case ProjectionKind::Field: {
                auto owner = step.owner;

                /*
                 * A closure header is a compiler-built table here, exactly as it is bytes there, so
                 * a field of it is a cell rather than a property. Recognized by its type, because
                 * that is what makes it this table rather than an ordinary tuple that happens to
                 * have two addresses in it - see closureHeaderPlaceType.
                 */
                if(owner == g.headerType) {
                    auto entry = g.repr.fieldOf(owner, step.index);
                    if(!entry) break;

                    /*
                     * The *slot* rather than the byte offset, which is what every other table here
                     * is read by - `genEnv[1][6]` is `ManagedTypeDesc::kCopyInit`, not a distance in
                     * bytes. The header is materialized as an array with one element per slot, so a
                     * native offset indexes past the end of it: `kReclaim` is slot 1 and offset 8,
                     * and `$h[8]` on a two-element array is `undefined`.
                     *
                     * Latent rather than observed, because the reclaim half compiles to nothing on
                     * this target - the host collector owns reclamation, Design-Memory §4 - so the
                     * only slot anything reads is `kDrop`, whose offset and index are both zero.
                     */
                    if(expr) *expr = tableCell(g, *expr, step.index);
                    type = entry->type;
                    break;
                }

                /*
                 * The two words of a function value, which are two properties here - the same two
                 * `FunValueLayout` describes, in the same order, reached the same way.
                 *
                 * The third projection is the closure header, and it is the one place this target
                 * answers differently from native rather than merely spelling it differently. Native
                 * puts the header in front of the entry point and subtracts a constant from the code
                 * word to reach it; a JS function has no bytes in front of it, so the header is a
                 * property *of the code word* - `code.$h`, assigned once at module level beside the
                 * code word's declaration. Either way the header is reached from the first word and
                 * never from the value, which is why one shared teardown serves both.
                 */
                if(g.global[owner]->kind == Type::Fun) {
                    if(!expr) break;

                    // Held as two variables, so each word *is* a variable and the header is a
                    // property of the one holding the code. Consumed here: a `Fun` is a leaf, so
                    // this step is the only one that can follow such a root.
                    if(rootFun.valid()) {
                        auto parts = rootFun;
                        rootFun = FunParts {};

                        if(step.index == FunValueLayout::kCode) {
                            *expr = parts.code;
                        } else if(step.index == FunValueLayout::kEnv) {
                            *expr = parts.env;
                        } else if(step.index == FunValueLayout::kHeader) {
                            *expr = field(g, parts.code, g.headerField);
                        }

                        break;
                    }

                    if(step.index == FunValueLayout::kCode) {
                        *expr = field(g, *expr, g.codeField);
                    } else if(step.index == FunValueLayout::kEnv) {
                        *expr = field(g, *expr, g.envField);
                    } else if(step.index == FunValueLayout::kHeader) {
                        *expr = field(g, field(g, *expr, g.codeField), g.headerField);
                    }

                    break;
                }

                /*
                 * A field elided onto a property the host value already has - `self.length` of
                 * `data Array(a) {items: %a, length: @host Count}`, which is `arr.length`.
                 *
                 * The ordinary property step, on the value the walk is already holding, which is
                 * what makes this a *place* rather than two operations: reading it is the count and
                 * writing it truncates the array, so `remove` closing the gap and recording the new
                 * count is one assignment through here rather than a `.splice` with a rule of its
                 * own. See isHostProperty for why the flag alone does not decide it.
                 */
                if(isHostProperty(g, owner, step.index)) {
                    auto property = ((TupType*)g.global[owner])->fields.get(g.global, step.index);
                    if(expr) *expr = field(g, *expr, fieldName(g, property.name, step.index));

                    within = PlaceBits {};
                    elided = true;
                    break;
                }

                /*
                 * The one field of a tuple that is that field here - `data Array(a) {items: %a}`,
                 * whose value is the host array rather than an object holding one. There is no
                 * property to descend into and the walk stays exactly where it is.
                 *
                 * Before the scalar case below rather than after it: a transparent tuple's own
                 * `scalarBits` is zero, so `isJsObject` answers about its *field*, and a field that
                 * is not an object would otherwise send this into the bit-range branch with a word
                 * width of nothing.
                 */
                TypePtr transparent = nullptr;
                if(isNewtype(g, owner, transparent)) break;

                auto entry = ((TupType*)g.global[owner])->fields.get(g.global, step.index);

                /*
                 * A field of a record the Repr made one scalar. There is no property to descend
                 * into - the owner *is* the value - so the walk stays where it is and accumulates
                 * where inside that number this field sits.
                 *
                 * The accumulation is the part with teeth. `t.f.a` reaches here twice: once for
                 * `f`, two bits at bit zero or two of `Two`, and once for `a`, one bit at bit
                 * zero of `Flags`. Neither offset is written anywhere and only their sum names
                 * the bit, which is why a walk that reported the last projection's offset would
                 * read a neighbour and still produce a value of the right type.
                 */
                if(!isJsObject(g, owner)) {
                    if(auto placed = g.repr.fieldOf(owner, step.index)) {
                        // The *outermost* scalar is the word, so this is recorded on the way in
                        // and left alone on the way further down: `t.f.a` lives in `t`'s number
                        // however narrow `Flags` happens to be, and it is `t`'s width that says
                        // whether the host's 32-bit operators can reach the bit.
                        if(!within.width) within.word = g.repr.of(owner).scalarBits;

                        within.offset += placed->bitOffset;
                        within.width = placed->bitWidth ? placed->bitWidth
                                                        : g.repr.of(entry.type).scalarBits;
                        break;
                    }
                }

                /*
                 * A field of a record that stayed an object, which is a property - and, where this
                 * target co-packed it, a bit range of one shared with its neighbours.
                 *
                 * The bit range *replaces* whatever the walk had accumulated rather than adding to
                 * it, because descending into a property is descending into a different value. It
                 * can only be empty here in any case: reaching an object-shaped tuple means
                 * nothing before it was a bit range, since a scalarized record holds only narrow
                 * fields and an object is not one.
                 */
                auto property = fieldProperty(g, owner, step.index);

                /*
                 * A function-value field, which is two properties rather than one holding an object.
                 *
                 * Left as the pair for the same reason the flat local root is: what follows is
                 * either a word of it - answered above from these two - or nothing, in which case
                 * the tail of this walk builds the object. Either way no `{$c, $e}` is projected
                 * *through*, which is the allocation part 2.2 removes from every record that holds
                 * a function value.
                 */
                if(property.fun) {
                    if(expr) {
                        rootFun.code = field(g, *expr, property.name);
                        rootFun.env = field(g, *expr, property.envName);
                        *expr = nullptr;
                    }

                    within = PlaceBits {};
                    break;
                }

                if(expr) *expr = field(g, *expr, property.name);

                within = PlaceBits {};
                if(property.isPacked()) {
                    within.offset = property.bitOffset;
                    within.width = property.bitWidth;
                    within.word = property.wordBits;
                }

                break;
            }
            case ProjectionKind::Deref:
                // The reference stored here becomes what the rest of the path is relative to.
                if(expr && type && !isJsObject(g, type)) *expr = field(g, *expr, g.boxField);
                break;
            case ProjectionKind::Index:
                /*
                 * `owner[i]` - and this target is the one where that is the *whole* of an element
                 * access, since a host array is indexed rather than addressed
                 * (Implementation-Containers.md §14.1). The native side spends a stride and an add
                 * here; there is nothing to spend one on.
                 *
                 * The type follows the same rule the shared walk states: a `[T *n]` steps *into*
                 * the array and everything else - a run of elements reached through a reference -
                 * steps *along* it and is already the element's type.
                 */
                if(expr) *expr = elementAt(g, *expr, useValue(g, step.value));
                break;
            case ProjectionKind::Unit:
                /*
                 * The whole word rather than the field the walk just entered - see
                 * ProjectionKind::Unit. The expression is already the right one, because a packed
                 * field is a bit range *of* the value the walk is holding rather than a property of
                 * it, so all this does is drop the range.
                 *
                 * Which turns the place back into a location, and that is the point: `bits.valid()`
                 * is what the load and the store branch on, so both of them take the plain path and
                 * the arithmetic the expansion emitted is what carries the shift and the mask.
                 */
                within = PlaceBits {};
                whole = true;
                break;
            case ProjectionKind::Property:
                break;
        }

        return true;
    }, limit);

    /*
     * A place that named the whole of a two-word function value rather than one word of it, which
     * the walk leaves as the pair it declined to join up.
     *
     * A caller that asked for the words takes them - a teardown, a flattened argument, a *write*,
     * which needs two assignable halves rather than an object literal it cannot assign to. One that
     * did not gets the object built here, which is the only place in the walk that allocates for a
     * function value: a return, a store across an erased boundary, an argument the callee declared
     * generic.
     */
    if(rootFun.valid()) {
        if(funParts) {
            *funParts = rootFun;
        } else if(expr) {
            *expr = materializeFun(g, rootFun);
        }
    }

    /*
     * The same for a reference held as its parts, with one exclusion the function-value case has no
     * counterpart to: a place that landed on the *folded tag* is not the reference at all, and what
     * the caller wants there is the owner this walk is already carrying so that `decodeNicheTag` can
     * compare it against `null`.
     */
    if(rootRef.valid() && !within.foldedTag) {
        if(refParts) {
            *refParts = rootRef;
        } else if(expr) {
            *expr = materializeRef(g, rootRef);
        }
    }

    if(bits) *bits = within;
    if(hostProperty) *hostProperty = elided;
    if(packedWord) *packedWord = whole;
    return type;
}

} // namespace

/*
 * Reading a bit range out of the number that holds it, and putting one back.
 *
 * Two forms, and which one a site uses is decided by `PlaceBits::word` rather than by the field:
 *
 *  - **at 32 bits and below**, `>>>`, `&`, `<<` and `|` - the operators JS actually has. This is the
 *    whole of what a packed word was until the budget moved, and it is left exactly as it was.
 *  - **above 32**, those stop working. A shift count is masked to five bits, so `mask << 32` is
 *    `mask` and a word of 40 bits would clear its own low field while writing its high one. What
 *    replaces them is division and multiplication by a power of two, which are exact on a double for
 *    every width a `number` holds, plus a 32-bit mask of the *result* - which is safe however wide
 *    the word was, since `ToInt32` keeps the low 32 bits and the field is inside them.
 *
 * Nothing needs converting on the way out, and that is the point of `Bool` being 0 or 1 rather than a
 * host boolean: an enum, a `@bits` integer, a nested scalar record and a `Bool` are all numbers, so
 * the bits *are* the value and this is the same shift and mask for every one of them.
 */

/*
 * Which of the two forms a range is reached by.
 *
 * A runtime scale always takes the wide one, whatever this target's words are: it arrived with a
 * reference, and a reference is the one place the word's width is not known - see maxWordBits. A
 * constant position takes the wide form only where the word genuinely needs it.
 */
static bool needsWideForm(PlaceBits bits) {
    if(bits.scale) return true;
    return bits.word > 32 && bits.offset + bits.width > 32;
}

/*
 * A range that *is* the word it lives in, which is a single-field record: the one field fills the
 * scalar, so reading it is reading the number and writing it is writing the number.
 *
 * Asked only of a word too wide for the host's operators, where the alternative is a remainder and a
 * re-sign - three operations to recover a value nothing has touched. Below 32 the mask is one cheap
 * operation and stays, which also keeps the two ends of a narrower word agreeing about whether it
 * holds the value or the pattern.
 *
 * Both directions have to take this together or neither may: what makes the identity sound is that
 * the word holds whatever the store put there, so the store must not be masking what the load will
 * not.
 */
static bool coversWord(PlaceBits bits) {
    return !bits.scale && bits.word > 32 && bits.offset == 0 && bits.width >= bits.word;
}

// The shift, for the 32-bit form. Null where the range starts at bit zero of what it names, and
// never asked of a range that arrived with a scale.
static JsPtr<Expr> shiftOf(Gen& g, PlaceBits bits) {
    return bits.offset ? number(g, F64(bits.offset)) : nullptr;
}

// The same position as a *multiplier*, for the wide form: `2**offset` folded into whatever scale a
// reference arrived with. Null means one, which is what lets a range at bit zero cost nothing.
static JsPtr<Expr> scaleOf(Gen& g, PlaceBits bits) {
    if(!bits.scale) return bits.offset ? number(g, powerOfTwo(bits.offset)) : nullptr;
    if(!bits.offset) return bits.scale;

    return binary(g, BinaryOp::Mul, bits.scale, number(g, powerOfTwo(bits.offset)));
}

/*
 * Where a range sits, as the two numbers the wide form multiplies by.
 *
 * `down` brings the bits at and above the range to bit zero and `up` puts a field back, and the two
 * are reciprocals - so a position could be one number and is two because of *which* number each end
 * has. A constant offset knows both and multiplies twice, which is what a `Math.floor` wants in
 * front of it. A reference carries the forward scale alone, and deriving its reciprocal would be a
 * divide per access either way, so that end divides instead. `divides` is that difference and it is
 * the only thing separating the two families of helper below.
 */
struct Position {
    JsPtr<Expr> down = nullptr;   // multiply the word by this; null means one
    JsPtr<Expr> up = nullptr;     // multiply the field by this; null means one
    bool divides = false;         // `word / up` rather than `word * down`
};

static Position positionOf(Gen& g, PlaceBits bits) {
    Position position;

    if(bits.scale) {
        position.up = scaleOf(g, bits);
        position.divides = true;
        return position;
    }

    if(bits.offset) {
        position.down = number(g, 1.0 / powerOfTwo(bits.offset), false);
        position.up = number(g, powerOfTwo(bits.offset));
    }

    return position;
}

// The bits at and above the range, as an integer. Exact for every value a `number` holds, since
// scaling by a power of two only moves the exponent.
static JsPtr<Expr> shiftedDown(Gen& g, JsPtr<Expr> owner, Position position) {
    if(position.divides) {
        return hostCall(g, "Math"_v, "floor"_v, binary(g, BinaryOp::Div, owner, position.up));
    }

    if(!position.down) return owner;
    return hostCall(g, "Math"_v, "floor"_v, binary(g, BinaryOp::Mul, owner, position.down));
}

// The low `width` bits of an already-shifted value, read as the field's own type. Shared by both
// forms, because once the range is at bit zero it is inside 32 bits either way.
static JsPtr<Expr> maskToField(Gen& g, JsPtr<Expr> value, U32 width, bool isSigned) {
    if(width >= 32) return binary(g, BinaryOp::And, value, number(g, F64(~U32(0))));

    /*
     * A signed field widens by *sign*-extension, which masking cannot do: `& 15` on a `@bits(4) I32`
     * holding -4 answers 12, and the field's four bits are all the information there is to tell the
     * two apart. Shifting the field's top bit up to bit 31 and arithmetically back down truncates
     * and sign-extends in the same pair - the shape `decodePackedField` uses natively, and the
     * reason the two targets agreed on everything here except the sign.
     */
    if(isSigned) {
        auto distance = number(g, F64(32 - width));
        return binary(g, BinaryOp::Sar, binary(g, BinaryOp::Shl, value, distance), distance);
    }

    return binary(g, BinaryOp::And, value, number(g, F64((U32(1) << width) - 1)));
}

static JsPtr<Expr> decodeRange(Gen& g, JsPtr<Expr> owner, PlaceBits bits, Position position,
                               bool wide, bool isSigned) {
    /*
     * A range entirely below bit 32 takes the 32-bit form whatever the word is, because `>>>` reads
     * its operand as `ToUint32` - the low half - and the field is inside that half by construction.
     * Only a *write* has to care about the bits it is dropping.
     */
    if(!wide) {
        auto value = owner;
        if(auto shift = shiftOf(g, bits)) value = binary(g, BinaryOp::Shr, value, shift);

        return maskToField(g, value, bits.width, isSigned);
    }

    auto value = shiftedDown(g, owner, position);
    if(bits.width < 32) return maskToField(g, value, bits.width, isSigned);

    /*
     * A field of 32 bits or more, which no 32-bit operator can cut down: the remainder is the only
     * thing that reduces a `number` to a width the host has no mask for. `%` is a genuine
     * floating-point remainder and the slowest thing in this file, which is why it is reached only
     * by the one shape that has no alternative - a field wider than the operators, in a word wider
     * than the operators.
     */
    auto reduced = binary(g, BinaryOp::Rem, value, number(g, powerOfTwo(bits.width)));
    if(!isSigned) return reduced;

    // Read back as a signed value. The binding is `resignExpr`'s requirement rather than a tidiness
    // measure: it mentions its operand three times.
    if(g.body) reduced = declare(g, generatedName(g, "bits"_v, g.labelCounter++), reduced);
    return resignExpr(g, reduced, bits.width);
}

/*
 * Putting one back, which is a read-modify-write because the neighbours share the word.
 *
 * At 32 bits and below, `(owner & ~(mask << shift)) | ((value & mask) << shift)`. The inner mask is
 * not redundant: a `@bits(4)` value that arrived wider would otherwise write over the field above it,
 * and a signed one arrives sign-extended, so its high bits are ones exactly when they must not be
 * stored.
 *
 * Above 32 the same sentence becomes arithmetic: `owner + (wanted - held) * 2**offset`, where `held`
 * is the field's current contents. Additive rather than mask-and-or because there is no mask - the
 * hole is above bit 31 as often as not - and correct for the same reason the mask was: the difference
 * only ever moves the bits the field owns, and every intermediate stays under 2^53 because the word
 * does.
 *
 * Which form is chosen from is the *word* rather than the range, and that is where this differs from
 * the load: a field entirely below bit 32 can still be read with `>>>`, because reading only has to
 * find the bits it wants, while writing has to leave alone every bit it does not - and `owner & hole`
 * drops the whole high half of a word that has one.
 */
static JsPtr<Expr> encodeRange(Gen& g, JsPtr<Expr> owner, PlaceBits bits, Position position,
                               bool wide, JsPtr<Expr> value) {
    auto mask = bits.width >= 32 ? ~U32(0) : (U32(1) << bits.width) - 1;

    if(!wide) {
        value = binary(g, BinaryOp::And, value, number(g, F64(mask)));

        auto shift = shiftOf(g, bits);
        if(shift) value = binary(g, BinaryOp::Shl, value, shift);

        // The hole the field occupies, as a constant - the shift is one, and so is the mask.
        auto hole = shift ? unary(g, UnaryOp::BitNot,
                                  binary(g, BinaryOp::Shl, number(g, F64(mask)), shift))
                          : number(g, F64(U32(~mask)));

        auto written = binary(g, BinaryOp::Or, binary(g, BinaryOp::And, owner, hole), value);

        /*
         * A word that reaches bit 31 is read back unsigned, which is what makes "a packed word is a
         * non-negative pattern" true rather than usually true.
         *
         * It matters one level away. A write *through a reference* cannot know how wide the word it
         * was handed is, so it uses the arithmetic form below - and that form only stays inside the
         * word if what it is adding to is the pattern rather than the pattern minus 2^32. Narrower
         * words cannot be negative in the first place and are left alone.
         */
        if(bits.word < 32) return written;
        return binary(g, BinaryOp::Shr, written, number(g, 0));
    }

    // Both readings are of the *pattern* rather than of the value, so a signed field is reduced the
    // unsigned way at both ends and the subtraction below cancels exactly.
    auto reduce = [&](JsPtr<Expr> from) -> JsPtr<Expr> {
        if(bits.width < 32) return binary(g, BinaryOp::And, from, number(g, F64(mask)));
        if(bits.width == 32) return binary(g, BinaryOp::Shr, from, number(g, 0));

        return wideCallAt(g, WideOp::Wrap, bits.width, false, from, nullptr);
    };

    auto held = reduce(shiftedDown(g, owner, position));
    auto difference = binary(g, BinaryOp::Sub, reduce(value), held);

    if(position.up) difference = binary(g, BinaryOp::Mul, difference, position.up);
    return binary(g, BinaryOp::Add, owner, difference);
}

/*
 * The two above as functions, for the shapes that are worth calling rather than writing out.
 *
 * A range reached with the host's own operators is two or three of them against literals -
 * `p.$p4 >>> 20 & 4095` - and a call could only make that longer. What is worth naming is the
 * *arithmetic* form, which exists at all because a word wider than 32 bits has no mask: the read is
 * a multiply, a floor and a mask, and the write is that plus the arithmetic to put the field back
 * with the read appearing inside it a second time. `flip(&Bool)` was ninety characters of it, and a
 * program that borrows narrow values or holds records above 32 bits does the same thing everywhere.
 *
 * Two families, because the two ends hold the position differently and neither should take the
 * other's cost - see Position. A **packed** helper is handed the reciprocal and the scale, both
 * literals at the site, and multiplies; a **borrowed** one is handed the reference's own scale and
 * divides. Both are interned per (width, signedness, direction), so every offset of a given width
 * shares one, which is what keeps this from becoming a helper per field.
 *
 * Interned the way wide.cpp's operators are and for the same second reason: the width stays a
 * literal inside the body, where the engine's own fast paths want to see it. What deliberately does
 * *not* move inside is the `owner[key]` indexing of a reference - one element access in one shared
 * helper would be a single inline cache for every borrowed word in the program.
 */
static U32 bitHelperKey(U32 bits, bool isSigned, bool store, bool packed) {
    return (bits << 3) | (U32(packed) << 2) | (U32(isSigned) << 1) | U32(store);
}

static Name bitHelper(Gen& g, U32 bits, bool isSigned, bool store, bool packed) {
    // A store reads and writes the *pattern* at both ends, so the sign is not one of its questions -
    // and keying on it anyway produced two helpers with identical bodies.
    if(store) isSigned = false;

    auto key = bitHelperKey(bits, isSigned, store, packed);
    if(auto found = g.bitHelpers.get(key)) return found.unwrap();

    // `$p20i$get` for a packed one and `$b20i$get` for a borrowed one, on wide.cpp's pattern: the
    // `$` prefix is the compiler's own convention and `uniqueName` handles a program that declared
    // the same identifier itself.
    char buffer[32];
    Size length = 0;
    buffer[length++] = '$';
    buffer[length++] = packed ? 'p' : 'b';
    length += show(U64(bits), buffer + length, sizeof(buffer) - length);
    buffer[length++] = isSigned ? 'i' : 'u';
    buffer[length++] = '$';

    auto suffix = store ? "set"_v : "get"_v;
    copy(suffix.ptr, buffer + length, suffix.length);
    length += suffix.length;

    auto name = uniqueName(g, StringView { buffer, length }, false);
    g.bitHelpers.add(key, name);
    g.bitHelperOrder.push(BitHelper { name, U16(bits), isSigned, store, packed });
    return name;
}

static JsPtr<Expr> namedParameter(Gen& g, StringView text, Name& into) {
    into = literalName(g, text);
    return variable(g, into);
}

void emitBitHelpers(Gen& g) {
    if(g.bitHelperOrder.size() == 0) return;

    auto heading = make<CommentStmt>(g, internText(g,
        "bit ranges of a word wider than the host's operators - see codegen/js/place.cpp"_v));
    g.bitHelperComment = asStmt(g, heading);
    emit(g, heading);

    for(Size i = 0; i < g.bitHelperOrder.size(); i++) {
        auto helper = g.bitHelperOrder[i];
        auto function = make<FunStmt>(g, helper.name);

        function->body = collect(g, [&] {
            Name wordName, downName, upName, valueName;
            auto word = namedParameter(g, "w"_v, wordName);
            auto down = namedParameter(g, "i"_v, downName);
            auto up = namedParameter(g, "s"_v, upName);
            auto value = namedParameter(g, "v"_v, valueName);

            /*
             * The parameters are the position, which is the whole of what the site knows and the
             * body does not. A packed helper takes both directions because it multiplies in both;
             * a borrowed one takes the scale alone and divides with it, and a load through one
             * therefore needs no second parameter at all.
             */
            Position position;
            function->args.push(g.file.arena, wordName);

            if(helper.packed) {
                position.down = down;
                function->args.push(g.file.arena, downName);

                if(helper.store) {
                    position.up = up;
                    function->args.push(g.file.arena, upName);
                }
            } else {
                position.up = up;
                position.divides = true;
                function->args.push(g.file.arena, upName);
            }

            if(helper.store) function->args.push(g.file.arena, valueName);

            // The word is as wide as this target packs, since the body serves every range of this
            // width - and it is the arithmetic form for all of them by construction.
            PlaceBits bits;
            bits.width = helper.bits;
            bits.word = maxWordBits(g);
            bits.scale = helper.packed ? nullptr : up;

            emit(g, make<ReturnStmt>(g, helper.store
                ? encodeRange(g, word, bits, position, true, value)
                : decodeRange(g, word, bits, position, true, helper.isSigned)));
        });

        emit(g, function);
    }
}

/*
 * Pure in exactly the sense `Math.imul` and the wide helpers are: it reads nothing and writes
 * nothing, and the store's own assignment is at the call site rather than inside it. Saying so is
 * what lets a one-use binding holding one collapse into its use, and what lets a read whose result
 * nothing wanted go away - a reborrow computes the field's value only to describe where it is, and
 * without this the emitted body kept two loads it never looked at.
 */
static JsPtr<Expr> bitCall(Gen& g, Name name, Buffer<const JsPtr<Expr>> args) {
    auto node = make<CallExpr>(g, variable(g, name));
    for(auto arg: args) node->args.push(g.file.arena, arg);

    node->pure = true;
    return asExpr(g, node);
}

/*
 * The two entry points, which decide between the call and the expression.
 *
 * `type` is the field's own, because a `Bool` is a number rather than a host boolean here and a
 * signed field widens by sign extension rather than by masking - and neither is visible from the
 * width alone.
 */
static bool fieldIsSigned(Gen& g, TypePtr type) {
    auto integer = intType(g, type);
    return integer && integer->isSigned;
}

/*
 * Whether a range is worth calling for rather than writing out.
 *
 * A reference always is: its position is a variable, so there is nothing to fold and the expression
 * is the same length at every site. A constant one is worth it only where it is actually *at* an
 * offset - at bit zero the position is the identity, the arithmetic form is already three or four
 * operations, and a helper would add a multiply by one and a floor of it to every access of the low
 * field of every record.
 */
static bool worthCalling(PlaceBits bits, bool wide) {
    return wide && (bits.scale || bits.offset != 0);
}

JsPtr<Expr> decodeBits(Gen& g, JsPtr<Expr> owner, PlaceBits bits, TypePtr type) {
    if(coversWord(bits)) return owner;

    auto isSigned = fieldIsSigned(g, type);
    auto wide = needsWideForm(bits);
    auto position = positionOf(g, bits);
    if(!worthCalling(bits, wide)) return decodeRange(g, owner, bits, position, wide, isSigned);

    auto name = bitHelper(g, bits.width, isSigned, false, !bits.scale);
    JsPtr<Expr> args[] = { owner, bits.scale ? position.up : position.down };
    return bitCall(g, name, toBuffer(args));
}

JsPtr<Expr> encodeBits(Gen& g, JsPtr<Expr> owner, PlaceBits bits, TypePtr type, JsPtr<Expr> value) {
    if(coversWord(bits)) return value;

    auto wide = bits.scale || bits.word > 32;
    auto position = positionOf(g, bits);
    if(!worthCalling(bits, wide)) return encodeRange(g, owner, bits, position, wide, value);

    auto name = bitHelper(g, bits.width, fieldIsSigned(g, type), true, !bits.scale);

    if(bits.scale) {
        JsPtr<Expr> args[] = { owner, position.up, value };
        return bitCall(g, name, toBuffer(args));
    }

    JsPtr<Expr> args[] = { owner, position.down, position.up, value };
    return bitCall(g, name, toBuffer(args));
}

TypePtr foldedPayload(Gen& g, TypePtr record) {
    auto& repr = g.repr.of(record);
    auto value = recordType(g, record);
    if(!value || !repr.isNicheFolded()) return nullptr;

    return value->constructors.get(g.global, repr.encoding.payloadConstructor).content;
}

/*
 * Declared in build.h, which is where the argument for it lives.
 */
bool refLocalIsFlat(Gen& g, TypePtr type) {
    if(!type) return false;

    auto reference = type;

    // A niche-folded optional over a reference is the reference or `null`, so it is the same three
    // variables with one of them standing in for the tag. Only an absent niche: a pattern one is a
    // range over the payload's bits, and these payloads are objects rather than numbers.
    if(g.global[type]->kind == Type::Record) {
        auto& repr = g.repr.of(type);
        if(!repr.isNicheFolded() || !repr.encoding.niche.isAbsent()) return false;

        reference = foldedPayload(g, type);
        if(!reference) return false;
    }

    if(g.global[reference]->kind != Type::Borrow) return false;

    auto pointee = ((BorrowType*)g.global[reference])->to;

    // A function value's reference is an owner and two keys rather than an owner and a key, and
    // nothing here would be wrong about it - but `refPartsOf` reads `isFunValue` off the *pointee*
    // to know which, and a folded optional's local type is the record. Left out rather than guessed.
    return pointee && refIsTriple(g, pointee) && !isFunValue(g, pointee);
}

/*
 * Reading a folded tag: which constructor this value is.
 *
 * Two shapes, from the two kinds of niche, and they are the same sentence about different things. An
 * absent niche is `null`, so the test is `v === null` and there is nothing else it could be - `fits`
 * admitted exactly one non-payload constructor. A pattern niche is a range the payload's own bits
 * cannot leave, so the test is a comparison, and it is a comparison of a `number` because that is the
 * only kind of value a pattern niche is ever donated by on this target - see ReprTable::hostNiche.
 *
 * Which makes this the same select native emits rather than a branch, and for the same reason: a
 * folded `Maybe` is meant to be cheaper than the tag word it replaced and not merely smaller.
 */
JsPtr<Expr> decodeNicheTag(Gen& g, JsPtr<Expr> value, TypePtr record) {
    auto& repr = g.repr.of(record);
    auto& encoding = repr.encoding;
    auto& niche = encoding.niche;

    auto payloadIndex = U64(encoding.payloadConstructor);
    auto payload = number(g, F64(payloadIndex));

    if(niche.isAbsent()) {
        auto other = number(g, F64(payloadIndex == 0 ? 1 : 0));
        return ternary(g, binary(g, BinaryOp::Eq, value, nullValue(g)), other, payload);
    }

    /*
     * The word is read up to three times below, so anything that is not already a name gets one.
     * `useValue` hands back a variable for most places a scrutinee comes from, and the declaration is
     * what keeps a property chain from being walked once per comparison.
     */
    auto kind = g.base[value]->kind;
    if(kind != Expr::Var && kind != Expr::Number && g.body) {
        value = declare(g, generatedName(g, "tag"_v, g.labelCounter++), value);
    }

    // `v >= validStart && v <= validEnd`, with the first half gone for the usual niche, whose valid
    // range starts at zero. Ordinary number comparisons: every pattern this target folds into is a
    // small integer, so there is nothing to do about wrapping.
    JsPtr<Expr> inRange = binary(g, BinaryOp::Le, value, number(g, F64(niche.validEnd)));
    if(niche.validStart) {
        inRange = binary(g, BinaryOp::LogicalAnd,
                         binary(g, BinaryOp::Ge, value, number(g, F64(niche.validStart))), inRange);
    }

    auto constructors = recordType(g, record)->constructors.size();

    // Two constructors is the shape this exists for, and there the pattern carries no information
    // beyond "not the payload one". No arithmetic, then: one of two constants.
    if(constructors == 2) {
        return ternary(g, inRange, payload, number(g, F64(payloadIndex == 0 ? 1 : 0)));
    }

    /*
     * More than two, so which pattern it is decides which constructor it is. The patterns were handed
     * out to the non-payload constructors in index order, so recovering the ordinal recovers the
     * index - except that the payload constructor is missing from that sequence, which the last step
     * puts back.
     */
    auto first = number(g, F64(encoding.firstPattern));
    auto ordinal = encoding.ascending ? binary(g, BinaryOp::Sub, value, first)
                                      : binary(g, BinaryOp::Sub, first, value);

    auto name = generatedName(g, "ord"_v, g.labelCounter++);
    auto bound = g.body ? declare(g, name, ordinal) : ordinal;

    // `ordinal >= payloadConstructor` means this constructor was written after the payload one, so its
    // index is one higher than its position in the pattern sequence.
    auto shifted = binary(g, BinaryOp::Ge, bound, number(g, F64(payloadIndex)));
    auto adjusted = ternary(g, shifted, binary(g, BinaryOp::Add, bound, number(g, 1)), bound);

    return ternary(g, inRange, payload, adjusted);
}

/*
 * Writing one, which for the payload constructor is writing nothing at all.
 *
 * That is not an optimization but the definition: the payload constructor *is* the payload's own
 * value, so the only thing that could make it identifiable is the payload being written, which the
 * constructor's own field initializations do. Every other constructor has no payload to write, so its
 * pattern is the whole value.
 */
void encodeNicheTag(Gen& g, JsPtr<Expr> target, TypePtr record, U64 constructor) {
    auto& encoding = g.repr.of(record).encoding;
    if(constructor == encoding.payloadConstructor) return;

    auto pattern = encoding.niche.isAbsent() ? nullValue(g)
                                             : number(g, F64(encoding.patternOf(U16(constructor))));

    emitExpr(g, assign(g, target, pattern));
}

/*
 * The two words of a *place*, which is what a teardown site has where a call site has a value.
 *
 * Both of these are the walk asked to stop one step short of joining the pair up, which is the only
 * way to get at the words wherever they live: two variables for a local this body holds flat, two
 * properties for a field of a record, two properties of a materialized object for anything else.
 * Going through `placeExpr` instead would build the object and then take it apart again.
 *
 * The difference between them is what a failure means. A *read* can always fall back on the object,
 * so funPartsOfPlace answers unconditionally; a *write* cannot, because an object literal is not
 * assignable, so destinationFunParts answers nothing and lets the caller emit the ordinary store.
 */
FunParts funPartsOfPlace(Gen& g, const Place& place) {
    JsPtr<Expr> expr = nullptr;
    FunParts parts;

    walkJsPlace(g, place, &expr, maxLimit<Size>, nullptr, &parts);
    if(parts.valid()) return parts;

    return funPartsOfExpr(g, expr);
}

Maybe<FunParts> destinationFunParts(Gen& g, const Place& place) {
    JsPtr<Expr> expr = nullptr;
    FunParts parts;

    walkJsPlace(g, place, &expr, maxLimit<Size>, nullptr, &parts);
    if(parts.valid()) return Just(parts);

    return Nothing();
}

/*
 * The same pair for a reference held as its parts, and here the two are one function: a place rooted
 * in such a local is assignable part by part and readable part by part, so a read that finds nothing
 * and a write that finds nothing are both "this is not one of those locals" rather than two answers.
 *
 * `Nothing` rather than a fall back to the object, which is what separates this from
 * `funPartsOfPlace`: a caller that wants the object asks `placeExpr`, and the walk builds one there.
 */
Maybe<RefParts> refPartsOfPlace(Gen& g, const Place& place) {
    JsPtr<Expr> expr = nullptr;
    RefParts parts;

    walkJsPlace(g, place, &expr, maxLimit<Size>, nullptr, nullptr, nullptr, nullptr, &parts);
    if(parts.valid()) return Just(parts);

    return Nothing();
}

Maybe<RefParts> destinationRefParts(Gen& g, const Place& place) {
    return refPartsOfPlace(g, place);
}

/*
 * Declared in build.h, which is where the argument for it lives.
 *
 * Four exclusions, and each of them is a type whose values are *not* inside what a width would say:
 *
 *  - `Bool`, which `coerce` leaves alone - the host boundary hands one in as `? 1 : 0` and nothing
 *    reduces it afterwards, so a width here would be a claim nothing maintains;
 *  - a `Long`, which is a `bigint` and has no `number` range at all;
 *  - a wide `number` of 33 to 53 bits, whose reductions are wide.cpp's rather than `coerce`'s;
 *  - anything past 32 bits, which no fold below asks about.
 */
/*
 * The same statement about a value that is *not* a place read - see noteValueType below, whose
 * restriction to the two shapes with no recoverable range this deliberately does not share.
 *
 * Asked at one site: the operand of a cast into the `bigint` domain (see genCast). Nothing about the
 * shape matters there, because the range is not being recovered from the tree at all - it is read
 * off the resolve type the emitter has in hand, which is a statement about the value however that
 * value arrived. What arrives at that site is usually a parameter or a call result, which is exactly
 * what a shape-based recovery cannot see and why the peephole could not fold the round trip.
 *
 * A payload-free sum is admitted as well as an integer, and it is the case this exists for: a
 * three-constructor enum is one byte, so `Number(BigInt.asIntN(64, BigInt(k)))` is `k` - which is
 * what `Core.Enum`'s `valueOf` costs on this target until something says so.
 */
void noteScalarRange(Gen& g, JsPtr<Expr> value, TypePtr type) {
    if(!value || !type) return;

    auto expr = g.base[value];
    if(expr->valueBits) return;

    if(auto integer = intType(g, type)) {
        auto bits = heldBits(g, *integer);
        if(integer->width == IntType::Bool || bits > 32) return;
        if(isLong(g, type) || isWideNumber(g, type)) return;

        expr->valueBits = U8(bits);
        expr->valueSigned = integer->isSigned;
        return;
    }

    auto declared = g.global[canonicalType(g.global, type)];
    if(declared->kind != Type::Record || ((RecordType*)declared)->layout != RecordType::Enum) return;

    // The width the layout gave the discriminant. A sum whose values needed more than four bytes is
    // a `bigint` here in its own right and has nothing to say about a 32-bit range.
    auto& repr = g.repr.of(type);
    if(repr.opaque || repr.size == 0 || repr.size > 4) return;

    expr->valueBits = U8(repr.size * 8);
    expr->valueSigned = true;
}

void noteValueType(Gen& g, JsPtr<Expr> value, TypePtr type) {
    if(!value) return;

    auto expr = g.base[value];
    if(expr->kind != Expr::Field && expr->kind != Expr::Index) return;

    auto integer = intType(g, type);
    auto bits = integer ? heldBits(g, *integer) : U16(0);
    if(!integer || integer->width == IntType::Bool || bits > 32) return;
    if(isLong(g, type) || isWideNumber(g, type)) return;

    expr->valueBits = U8(bits);
    expr->valueSigned = integer->isSigned;
}

JsPtr<Expr> placeExpr(Gen& g, const Place& place, Size limit) {
    JsPtr<Expr> expr = nullptr;
    PlaceBits bits;
    auto packedWord = false;
    auto type = walkJsPlace(g, place, &expr, limit, &bits, nullptr, nullptr, &packedWord);

    // Every reader wants the value rather than the word it sits in, so the decode is applied here and
    // the one caller that cannot use it - the store below - asks placeOwner instead.
    if(bits.foldedTag) return decodeNicheTag(g, expr, bits.foldedTag);
    if(bits.valid()) return decodeBits(g, expr, bits, type);

    /*
     * Two exclusions, and both are places whose *range is already in the tree*.
     *
     * A bit range has returned above carrying `decodeBits`'s own mask. A packed word reaches here
     * with `type` naming the field the path asked for and the expression holding the whole word it
     * sits in - so the type is a statement about thirteen of the bits of a value that has more of
     * them, and taking it for the value's own is how `x.count & 8191` lost its mask and read a
     * neighbour along with it.
     */
    if(!packedWord) noteValueType(g, expr, type);
    return expr;
}

JsPtr<Expr> placeOwner(Gen& g, const Place& place, PlaceBits& bits, Size limit) {
    JsPtr<Expr> expr = nullptr;
    walkJsPlace(g, place, &expr, limit, &bits);
    return expr;
}

// The chain, and whether the last step of it landed on a `@host` field - the one thing a *write*
// needs that a read does not. Answered by the same walk rather than by a second one, on the same
// terms as everything else here: a place that is a host property to one of the two and not to the
// other is a store that lands somewhere nothing reads.
JsPtr<Expr> placeTarget(Gen& g, const Place& place, bool& hostProperty) {
    JsPtr<Expr> expr = nullptr;
    walkJsPlace(g, place, &expr, maxLimit<Size>, nullptr, nullptr, &hostProperty);
    return expr;
}

// What a place holds. Most callers want one or the other rather than both, so this skips building
// the chain rather than building one nobody reads.
TypePtr placeType(Gen& g, const Place& place, Size limit) {
    return walkJsPlace(g, place, nullptr, limit);
}

/*
 * The storage a reference names, as the handle the *erased* ABI passes.
 *
 * `%a` on this target is a slot - a box, an object, whatever the value is stored in - and the triple
 * `{$o, $k, $s}` is not one: it is three values describing where a slot is, which is exactly the
 * flat form Implementation-JS-Closure.md part 5.1 says an erased boundary never sees. A callee
 * compiled against `a` hands its argument to `moveInit`/`copyInit`/`drop`, and every one of those
 * reads through it as a slot, so handing over the triple makes the write land on the descriptor
 * instead of on the storage.
 *
 * Answered only for the shape where a slot genuinely exists: a whole local this frame boxed, whose
 * box *is* the storage. A borrow of a field, an element or a bit range names a slot inside something
 * else, and the one-property object that would stand in for it is a snapshot with a commit point -
 * which is the copy-with-write-back refIsTriple exists to have removed. That case is `README.md`
 * gap 1 and it is reported rather than silently miscompiled.
 */
Maybe<JsPtr<Expr>> erasedStorageOf(Gen& g, const Place& place) {
    auto projections = place.projections;

    if(place.root != PlaceRoot::Local || projections.isNotEmpty()) return Nothing();
    if(place.local >= g.boxed.size() || !g.boxed[place.local]) return Nothing();
    if(place.local >= g.function->localCount()) return Nothing();

    auto root = g.function->localAt(g.local, place.local);
    if(root.borrowed) return Nothing();

    // The box a local built at its defining value, for the same reason the walk names it there: what
    // `useValue` answers for that value is the slot inside the box rather than the box.
    if(auto box = g.localBoxes.get(place.local)) return Just(box.unwrap());

    return Just(useValue(g, root.value));
}

JsPtr<Expr> referenceTo(Gen& g, TypePtr type, JsPtr<Expr> value) {
    if(type && !isJsObject(g, type)) return boxOf(g, value);
    return value;
}

JsPtr<Expr> referenceTo(Gen& g, const Place& place, Size limit) {
    JsPtr<Expr> expr = nullptr;
    auto type = walkJsPlace(g, place, &expr, limit);
    return referenceTo(g, type, expr);
}

/*
 * The erased half - Implementation-Generics.md, read through §3.4.
 */

/*
 * One slot of a compiler-built table, which here is one element of an array.
 *
 * This is the whole of the JS materialization of resolve/witness.h's tables, and it is this short
 * because a slot number is already an index. There is nothing to divide by and nothing to skip: a
 * host array has no padding, and an address is a name rather than eight bytes. Reading a native
 * blob back at native offsets - which is what this used to do - made every table here a
 * transcription of an x64 memory image, with a null in every second cell where the high half of a
 * pointer would have been.
 */
JsPtr<Expr> tableCell(Gen& g, JsPtr<Expr> table, U16 slot) {
    return index(g, table, slot);
}

JsPtr<Expr> genSlot(Gen& g, U16 slot) {
    return tableCell(g, g.genEnv, GenEnvFields::slot(slot));
}

JsPtr<Expr> genWitness(Gen& g, U16 slot, ModuleList<U32, false> path) {
    auto witness = genSlot(g, slot);
    for(auto step: path.contents(g.local)) witness = tableCell(g, witness, U16(step));

    return witness;
}

JsPtr<Expr> genTypeDesc(Gen& g, TypePtr type) {
    if(!g.genEnv || !type || !isGeneric(g.global, type)) return nullptr;

    auto slot = genTypeSlot(*g.genModule, *g.genContext, type);
    if(slot == maxLimit<U16>) return nullptr;

    return genSlot(g, slot);
}

/*
 * A const parameter's value - Implementation-Const-Generics.md §3.2.
 *
 * One cell, and no step past it. A type variable's slot holds a descriptor that a metric then
 * indexes into; a const one holds the number itself, so the erased read of a count is *shorter* than
 * the erased read of a size rather than longer.
 */
JsPtr<Expr> genConstValue(Gen& g, TypePtr count) {
    if(!g.genEnv || !count || !isGeneric(g.global, count)) return nullptr;

    auto slot = genConstSlot(*g.genModule, *g.genContext, count);
    if(slot == maxLimit<U16>) return nullptr;

    return genSlot(g, slot);
}

} // namespace js
