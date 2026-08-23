#pragma once

/*
 * The resolve-to-lower translation, shared between the files it is split across.
 *
 * This header is not an interface: `lower.h` is, and it has one function in it. What is here is the
 * seam between the six translation units below, which are one algorithm cut where its
 * responsibilities are rather than where a size limit fell:
 *
 *  - lower.cpp        - the driver. Scalarization, the walk over modules, functions, blocks and
 *                       phis, and the one entry point.
 *  - lower_type.cpp   - measurement. What a resolve type is in the lower IR, how wide it is, and
 *                       the masking and truncation a declared width implies.
 *  - lower_gen.cpp    - the erased ABI. Everything a generic body reads out of its environment
 *                       instead of off a type: schema slots, descriptors, witnesses, property ops.
 *  - lower_place.cpp  - places. Turning a place into an address, and deciding which of the three
 *                       access forms - plain, packed, narrow reference - a projection is.
 *  - lower_pack.cpp   - encoding. Reading a value out of a bit range and writing one back into it,
 *                       for packed fields, niche and bit tags, and narrow references.
 *  - lower_inst.cpp   - the instruction dispatch, plus lower_mem.cpp (storage and ownership),
 *                       lower_calc.cpp (computation) and lower_call.cpp (the three call forms).
 *
 * Nothing declared here is public. A declaration earns its place by having a caller in another one
 * of those files; everything with one caller in one file stays static there, which is why several
 * of the families below are smaller than the file they come from.
 */

#include "lower.h"
#include "analyze.h"
#include "generic.h"
#include "place.h"
#include "witness.h"
#include "../repr/table.h"
#include "../repr/constant.h"
#include "../lower/lower_builder.h"

// The side tables mapping one IR to the other are keyed by region offset rather than by address:
// a resolve handle already is that offset, so this is the same identity the rest of the resolver
// uses, and it stays meaningful in printed output.
struct LowerContext {
    Context& context;
    Program& from;
    LowerModule& to;
    GlobalBase global;
    ModuleBase local;
    LowerBase lower;

    /*
     * This backend's layout answers, owned by this backend.
     *
     * Constructed in lowerProgram from nativeReprTarget() and named there rather than read off the
     * program, which is the difference between a target being *chosen* and being inferred: a table
     * hanging off Program had to guess from the compile mode which one it was for, so a native
     * lowering in a JS build would have measured everything with the wrong ruler. The JS backend
     * builds its own the same way, and the two never meet.
     */
    ReprTable& repr;
    HashMap<U32, LowerPtr<LowerFunction>> functions;
    HashMap<U32, LowerPtr<LowerBlock>> blocks;
    HashMap<U32, LowerPtr<LowerValue>> values;
    HashMap<U32, LowerPtr<LowerValue>> returnPlaces;
    LowerBlock* constantBlock = nullptr;

    // The fields of each scalarized local of the function being lowered, by local index and then
    // by field index. Empty for every local that kept its storage - see prepareScalars().
    Array<Array<LowerPtr<LowerValue>>> scalars;

    /*
     * The erased half, set only while a *generic* function is being lowered.
     *
     * An unspecialized body does not know what its type variables are, so everything it would have
     * read off a type - a size, an alignment, how to relocate or release a value - it reads out of
     * the environment its caller passed instead. `genEnv` is that environment's address, and
     * `genContext` is the compile-time schema that says which slot holds what.
     *
     * Both are null for an ordinary function, and every use of them below is guarded by that: the
     * concrete path is unchanged, which is what keeps the two forms comparable.
     */
    LowerPtr<LowerValue> genEnv = nullptr;
    GenEnv* genContext = nullptr;
    Module* genModule = nullptr;
};

struct PackedAccess {
    U32 wordBytes = 0;
    U32 bitOffset = 0;
    U32 bitWidth = 0;
    TypePtr type = nullptr;

    bool exists() const { return bitWidth != 0; }
};

/*
 * A borrow that is an address plus a shift - Design.md's tier 2.
 *
 * `&T` for a narrow `T` refers to a bit range rather than to a word, so it carries where in that
 * word the range starts. The shift is at most five bits, because a packed field never straddles the
 * natural storage unit of its own width, and it travels in the address's spare high bits - see
 * ReprTarget::addressBits. A non-narrow `T` has a provably zero shift and is left exactly the
 * address it always was.
 *
 * What the callee does with one is entirely decided by the *type*: the unit to load is
 * `naturalStorageBits(bits)` and the mask is `bits`, both constants there. Only the shift is
 * unknown, which is what lets one compiled body serve a packed field, an unpacked one, and a whole
 * local of the same type.
 *
 * A reference to a whole *aggregate* is the same thing once the aggregate is a scalar: `&Flags` for
 * `data Flags {a: Bool, b: Bool}` is two bits at a shift, and reading `f.a` through one is the
 * reference's shift plus the constant bit offset the field has inside those two bits. That constant is
 * the callee's own - it comes from its Repr for the pointee type, which the caller never had to agree
 * about beyond the type itself - so `bitOffset` below is added at the access rather than carried.
 */
struct NarrowRef {
    LowerPtr<LowerValue> address;
    LowerPtr<LowerValue> shift;
    U32 unitBytes = 0;
    U32 bits = 0;
    bool isSigned = false;
};

/*
 * Which bits a place names when the place is rooted in a reference of that kind.
 *
 * `referenced` is the pointee type, which decides the unit to load; `bitOffset` and `bitWidth` are
 * where inside it the place ends up, composed exactly as `packedAccess` composes them - `&two.g` is a
 * reference to a `Flags`, and `g.a` through it is one bit at offset zero of two.
 */
struct NarrowRefAccess {
    TypePtr referenced = nullptr;
    TypePtr type = nullptr;
    U32 bitOffset = 0;
    U32 bitWidth = 0;

    bool exists() const { return referenced != nullptr; }
};

// -- lower.cpp ---------------------------------------------------------------------------------

void prepareScalars(LowerContext& lower, ModulePtr<Function> pointer, Function& function);
U16 scalarField(LowerContext& lower, const Place& place);
bool isScalarPlace(LowerContext& lower, const Place& place);

// -- lower_type.cpp ----------------------------------------------------------------------------

/*
 * The register class a resolve type is held in, which is a question only a target can answer.
 *
 * `LowerContext` rather than a `GlobalBase`, and that is Move 2: `Size` is the target's word, so
 * `lowerType` needs the table's `IntWidths` to say whether it is `Int32` or `Int64`. Everything in
 * this family that compares a width against its register takes the context for the same reason.
 */
LowerType lowerType(LowerContext& lower, TypePtr type);
bool signedType(GlobalBase base, TypePtr type);

// Whether arithmetic on this type is the signed instruction, which reads a vector's lane where
// `signedType` reads the vector. See lower_type.cpp for why the two are different questions.
bool signedOperand(GlobalBase base, TypePtr type);
U32 typeSize(LowerContext& lower, TypePtr type);
U32 typeAlign(LowerContext& lower, TypePtr type);
U32 typeStride(LowerContext& lower, TypePtr type);
bool lowerArgExists(GlobalBase global, TypePtr type, bool mutableBorrow);
U32 memoryWidth(LowerContext& lower, TypePtr type);
LowerPtr<LowerValue> immediate(LowerContext& lower, U64 value, LowerType type = LowerType::Int64);
LowerPtr<LowerValue> addOffset(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> address,
                               U32 offset);

// The address one slot of a compiler-built table holds - the single decoder of the self-relative
// form every erased read goes through. See lower_gen.cpp.
LowerPtr<LowerValue> descAlign(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> descriptor);

LowerPtr<LowerValue> tableSlotAddress(LowerContext& lower, LowerBlock& block,
                                      LowerPtr<LowerValue> table, U16 slot);
U64 lowMask(U32 bits);
bool narrowerThanRegister(LowerContext& lower, TypePtr type);
bool wrapsAtDeclaredWidth(LowerContext& lower, TypePtr type, Value::Kind kind);
bool zeroExtendsShiftOperand(LowerContext& lower, TypePtr type, Value::Kind kind);
U32 signShift(LowerContext& lower, TypePtr type);
LowerPtr<LowerValue> maskToWidth(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> value,
                                 TypePtr type, LowerType lowered);
LowerInst* truncateToWidth(LowerContext& lower, LowerBlock& block, LowerInst* result, TypePtr type,
                           LowerType lowered, StringId name);
LowerPtr<LowerValue> reinterpret(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> value,
                                 LowerType type);

// -- lower_gen.cpp -----------------------------------------------------------------------------

LowerPtr<LowerValue> genTypeDesc(LowerContext& lower, LowerBlock& block, TypePtr type);

// The value of one const parameter, or null where this body knows the count already. See lower_gen.cpp.
LowerPtr<LowerValue> genConstValue(LowerContext& lower, LowerBlock& block, TypePtr count, LowerType type);
LowerPtr<LowerValue> descField(LowerContext& lower, LowerBlock& block,
                               LowerPtr<LowerValue> descriptor, U16 slot);
LowerPtr<LowerValue> sizeOfType(LowerContext& lower, LowerBlock& block, TypePtr type);
LowerPtr<LowerValue> storageSize(LowerContext& lower, LowerBlock& block, TypePtr type);
LowerPtr<LowerValue> strideSize(LowerContext& lower, LowerBlock& block, TypePtr type);
LowerPtr<LowerValue> scaleBy(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> stride,
                             LowerPtr<LowerValue> count);
LowerPtr<LowerValue> genEnvironment(LowerContext& lower, LowerBlock& block, InstGenCall& call);
LowerPtr<LowerValue> genMethod(LowerContext& lower, LowerBlock& block, InstGenCall& call);
U16 propertySlotOf(LowerContext& lower, const Place& place);
LowerPtr<LowerValue> propertyOp(LowerContext& lower, LowerBlock& block, U16 slot, U16 field);
void callPropertyOp(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> op,
                    LowerPtr<LowerValue> owner, LowerPtr<LowerValue> other);
void zeroStorage(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> address, U32 bytes);
LowerPtr<LowerValue> erasedStorage(LowerContext& lower, LowerBlock& block, TypePtr type,
                                   StringId name);

// -- lower_place.cpp ---------------------------------------------------------------------------

LowerPtr<LowerValue> lowerPlace(LowerContext& lower, LowerBlock& block, Function& function,
                                const Place& place, Size limit = maxLimit<Size>);
TypePtr dropPlaceType(LowerContext& lower, Function& function, const Place& place);
TypePtr placeRootedType(LowerContext& lower, Function& function, const Place& place);
TypePtr placeOwnedType(LowerContext& lower, Function& function, const Place& place);
TypePtr foldedTagRecord(LowerContext& lower, Function& function, const Place& place);
TypePtr bitTagRecord(LowerContext& lower, Function& function, const Place& place);
U32 discriminantWidth(LowerContext& lower, Function& function, const Place& place);
U32 unitBits(LowerContext& lower, const Place& place);
PackedAccess packedAccess(LowerContext& lower, Function& function, const Place& place);
NarrowRefAccess narrowRefAccess(LowerContext& lower, Function& function, const Place& place);
LowerPtr<LowerValue> narrowRefValue(LowerContext& lower, Function& function, const Place& place);

// -- lower_pack.cpp ----------------------------------------------------------------------------

LowerPtr<LowerValue> decodeNicheTag(LowerContext& lower, LowerBlock& block,
                                    LowerPtr<LowerValue> payload, TypePtr record, TypePtr tagType,
                                    StringId name);
void encodeNicheTag(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> payload,
                    TypePtr record, U64 constructor);
PackedAccess bitTagAccess(const Repr& repr);
LowerPtr<LowerValue> decodeBitTag(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> word,
                                  TypePtr record, TypePtr tagType, StringId name);
void encodeBitTag(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> word, TypePtr record,
                  U64 constructor);
LowerPtr<LowerValue> decodePackedBits(LowerContext& lower, LowerBlock& block,
                                      LowerPtr<LowerValue> word, const PackedAccess& field,
                                      bool isSigned);
LowerPtr<LowerValue> decodePackedField(LowerContext& lower, LowerBlock& block,
                                       LowerPtr<LowerValue> word, const PackedAccess& field,
                                       TypePtr type, StringId name);
void encodePackedField(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> word,
                       const PackedAccess& field, LowerPtr<LowerValue> value);
NarrowRef unpackNarrowRef(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> ref,
                          const NarrowRefAccess& access);
LowerPtr<LowerValue> packNarrowRef(LowerContext& lower, LowerBlock& block,
                                   LowerPtr<LowerValue> address, U32 shift);
LowerPtr<LowerValue> stepNarrowRef(LowerContext& lower, LowerBlock& block, const NarrowRef& ref,
                                   U32 unitBytes);
LowerPtr<LowerValue> decodeNarrowBits(LowerContext& lower, LowerBlock& block, const NarrowRef& ref);
LowerPtr<LowerValue> decodeNarrowRef(LowerContext& lower, LowerBlock& block, const NarrowRef& ref,
                                     TypePtr type, StringId name);
void encodeNarrowRef(LowerContext& lower, LowerBlock& block, const NarrowRef& ref,
                     LowerPtr<LowerValue> value);
LowerPtr<LowerValue> materializeScalar(LowerContext& lower, LowerBlock& block, TypePtr type,
                                       LowerPtr<LowerValue> bits, StringId name);
LowerPtr<LowerValue> scalarBitsOf(LowerContext& lower, LowerBlock& block, TypePtr type,
                                  LowerPtr<LowerValue> value);

// -- lower_inst.cpp, and the three files its dispatch reaches ------------------------------------

void lowerInstruction(LowerContext& lower, LowerBlock& block, ModulePtr<Inst> pointer);
void lowerTerminator(LowerContext& lower, LowerBlock& block, ModulePtr<Inst> pointer);
LowerPtr<LowerValue> mappedValue(LowerContext& lower, ModulePtr<Value> pointer);
void mapResult(LowerContext& lower, ModulePtr<Value> from, LowerInst* instruction);

/*
 * The three halves of `lowerInstruction`, each answering for the kinds its file is about, and each
 * returning the instruction whose result is this value - or null when the arm mapped what it
 * produced itself, which every arm that emits more than one instruction has to.
 *
 * Which file owns a kind is `instGroup` in lower_inst.cpp, and that is one switch with no default:
 * a kind added to inst.def is a compile error there rather than an assertion reached at runtime.
 */
LowerInst* lowerStorageInst(LowerContext& lower, LowerBlock& block, Inst& instruction,
                            ModulePtr<Value> instValue, Function* function);
LowerInst* lowerComputeInst(LowerContext& lower, LowerBlock& block, Inst& instruction,
                            ModulePtr<Value> instValue, Function* function);
LowerInst* lowerCallInst(LowerContext& lower, LowerBlock& block, Inst& instruction,
                         ModulePtr<Value> instValue, Function* function);

// Storing a value into a place, which the two initializing instructions and several of the
// ownership ones share. In lower_mem.cpp.
LowerInst* lowerStore(LowerContext& lower, LowerBlock& block, Function* function, Place place,
                      ModulePtr<Value> value);
LowerInst* relocate(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> target,
                    ModulePtr<Value> value, LowerPtr<LowerValue> source, TypePtr type);
LowerInst* relocateWith(LowerContext& lower, LowerBlock& block, LowerPtr<LowerValue> target,
                        LowerPtr<LowerValue> source, TypePtr type, ModulePtr<Function> sink,
                        bool erased);

// Reading an instruction's operand, in lower_inst.cpp: the value it was mapped to, or the constant
// materialized for this function on first use.
LowerCmp lowerCmp(LowerContext& lower, InstCmp& compare);
LowerInst::Kind binaryKind(LowerContext& lower, InstBinary& binary);
