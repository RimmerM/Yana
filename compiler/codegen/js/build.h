#pragma once

#include "gen.h"
#include "../../resolve/generic.h"
#include "../../resolve/witness.h"
#include "../../repr/table.h"

/*
 * The JS backend's internal interface - the generator state, the AST construction helpers, and the
 * declarations the emission files share.
 *
 * `gen.h` is what the rest of the compiler sees; this is what the backend sees of itself. The split
 * exists because the target is one pass over the resolve IR that answers six separable questions -
 * what things are called, what shape a type has, what a place is, what an instruction does, what the
 * control flow is, and what gets emitted at all - and each of those is a file here.
 *
 * Everything in this header is inline and small enough to be: allocating a node, naming a statement
 * list, building a call. A helper that has to look at the IR to answer belongs in one of the .cpp
 * files, declared at the bottom.
 */
namespace js {

constexpr U32 kNoBlock = maxLimit<U32>;

/*
 * The widest integer a host `number` holds every value of.
 *
 * The same number as `ReprTarget::integerBits` for this target and for the same reason - above it
 * consecutive integers stop being representable - but it is stated separately because the two are
 * asked by different people. That one is what a *layout* may occupy; this one is what the *value*
 * tower switches on, and an integer type wider than this is a `bigint` here.
 */
constexpr U16 kMaxNumberBits = 53;

// One helper this file needs, in the order they were first asked for. `WideOp` is in ast.h, since
// a call node carries one.
struct WideHelper {
    Name name;
    WideOp op;
    U16 bits;
    bool isSigned;
};

/*
 * One accessor for a bit range whose position is only known at run time - see place.cpp.
 *
 * The static ranges are two or three operations against constants and are emitted where they are
 * used. A range reached *through a reference* is not: the scale is a variable, so the read is a
 * divide, a floor and a mask, and the write is that plus the arithmetic to put the field back. Every
 * borrow of a narrow value in the program spells the same one out, and a program that borrows is a
 * program that does it everywhere.
 */
struct BitHelper {
    Name name;
    U16 bits;
    bool isSigned;
    bool store;

    // Whether the position arrives as a literal pair the body multiplies by, or as the single
    // forward scale a reference carries and the body divides with. See place.cpp's Position.
    bool packed;
};

/*
 * One block an enclosing construct can be left through, and how.
 *
 * A `Loop` entry is its header, so reaching it is `continue`; a `Forward` entry is a labelled block
 * whose end is that block, so reaching it is `break`. Between them these are every edge JS has no
 * `goto` for.
 */
struct Exit {
    U32 block;
    Name label;
    bool loop;
};

// One cell of a constant table that names a table emitted after it - see genGlobal.
struct Forward {
    Name table;
    U32 cell;
    ModulePtr<Global> target;
};

/*
 * A borrow that had to be boxed at the point it was taken rather than at the storage it names.
 *
 * Only for a borrow of a *field* whose type is not an object - `&p.x` where `x` is an `Int`. The
 * box is a temporary, so the value written through it has to be copied back into the field once the
 * borrow's consumer is done with it, which is Design.md's materialize/write-back around a packed
 * field arrived at from the other direction.
 */
struct Writeback {
    ModulePtr<Value> borrow;
    JsPtr<Expr> box;

    // The place rather than an expression for it, because committing into a bit range is a
    // read-modify-write and only `assignPlace` knows which places are locations.
    Place target;
    TypePtr type;
};

/*
 * The three parts of a narrow reference, however this one is being carried.
 *
 * A reference that arrived flat - a parameter, or a borrow whose every use passes it on - has each
 * part in a variable of its own and there is no object anywhere. One that did not is an ordinary
 * `{$o,$k,$s}`, and the parts are its properties. Every consumer wants the parts rather than the
 * object, which is what lets the two forms coexist without a flag at each use.
 */
struct RefParts {
    JsPtr<Expr> owner = nullptr;
    JsPtr<Expr> key = nullptr;
    JsPtr<Expr> scale = nullptr;    // `2**shift`, or null where the target carries none

    bool valid() const { return owner != nullptr; }
};

struct Gen {
    Context& context;
    Program& program;
    File& file;
    GlobalBase global;
    ModuleBase local;
    JsBase base;

    /*
     * This target's layout answers, owned by this target.
     *
     * Built from jsReprTarget() in genProgram, and the native backend builds its own from
     * nativeReprTarget(). Neither can see the other's: `Maybe(Id)` is one machine word over there
     * and `number | null` here, and the whole reason Repr is computed at emission rather than during
     * resolution is that both of those are right.
     */
    ReprTable& repr;

    // Identifiers already handed out. Module-level names are checked by every local name as well,
    // so that a local can never shadow a function it might want to call.
    HashSet<StringId> moduleNames;
    HashSet<StringId> localNames;

    HashMap<U32, Name> functionNames;
    HashMap<U32, Name> globalNames;
    HashSet<U32> emittedGlobals;
    Array<Forward> forward;
    Name tableName;

    // Functions this target cannot emit - see excludeFunctions().
    HashSet<U32> excluded;

    // Names this generator has already copied into the string arena - see internText.
    HashSet<StringId> interned;

    // The 33-to-53-bit helpers this file has asked for, interned per (operation, width, signedness)
    // and emitted once at the end. See wide.cpp.
    HashMap<U32, Name> wideHelpers;
    Array<WideHelper> wideHelperOrder;

    // The dynamic bit-range accessors, on the same terms, interned per (width, signedness,
    // direction). See place.cpp.
    HashMap<U32, Name> bitHelpers;
    Array<BitHelper> bitHelperOrder;

    // The heading each family of helpers is emitted under, so that a family the peephole emptied
    // takes its own comment with it - see removeDeadHelpers.
    JsPtr<Stmt> wideHelperComment = nullptr;
    JsPtr<Stmt> bitHelperComment = nullptr;

    // The property names that are the compiler's rather than the program's.
    Name tagField;
    Name payloadField;
    Name boxField;
    Name envField;
    Name headerField;

    /*
     * The three parts of a reference to a narrow value - Design.md's tier 2, as this target spells
     * it. Native carries an address plus a shift; here a place is already an (object, property)
     * pair, so a reference to one is that pair reified plus the same shift:
     * `{$o: owner, $k: "field", $s: 0}`, read as `(r.$o[r.$k] >>> r.$s) & mask`.
     *
     * Which makes it a genuine reference rather than a copy with a write-back, and that is the
     * point - it has no commit point, so it can outlive the call that produced it exactly as the
     * native form can.
     *
     * The shift is what whole-record scalarization needed and the pair alone could not give: a field
     * of a record that became a `number` has no property to name, so `$k` names the word and `$s`
     * says where in it the field starts. It is carried on every narrow reference rather than only on
     * the packed ones for the same reason native carries it on every one - the callee has the pointee
     * type and nothing else, so one compiled `flip(&Bool)` has to serve a bit of a scalarized record,
     * a co-packed field and a whole local alike.
     */
    Name refObject;
    Name refKey;
    Name refScale;

    // The tuple ClosureHeaderLayout is also described as - see closureHeaderPlaceType. A place into
    // one is a cell read rather than a property, because the header is a compiler-built table here
    // exactly as it is bytes there.
    TypePtr headerType = nullptr;

    /*
     * Per function.
     */

    Function* function = nullptr;
    ModulePtr<Function> functionPointer = nullptr;
    StmtList* body = nullptr;

    HashMap<U32, JsPtr<Expr>> values;
    HashMap<U32, Name> phis;

    // The narrow references currently being carried as separate variables rather than as an object -
    // see refIsFlattened. A value is in here instead of, or as well as, `values`.
    HashMap<U32, RefParts> flatRefs;

    // The code word each function-value local has been given, by local index, between the two Inits
    // that build one - see genFunValueWord.
    HashMap<U32, U32> pendingCode;

    // Which locals are stored as a one-property box, by local index. See gen.cpp's file comment.
    Array<bool> boxed;

    // The borrow and address values that are a second *name* for the storage they were taken of
    // rather than a box holding it - see prepareLocals. A place rooted in one of these reaches the
    // storage directly, which is what makes a borrow that never leaves the function cost nothing.
    HashSet<U32> aliasBorrows;

    Array<Writeback> writebacks;

    // The CFG, in the function's own block order.
    Array<ModulePtr<Block>> blocks;
    HashMap<U32, U32> blockIndex;
    Array<Array<bool>> postDominators;
    Array<U32> ipdom;
    Array<U32> idom;
    Array<bool> loopHeader;
    Array<bool> emitted;

    // The constructs currently open that control can leave through - see emitChain.
    Array<Exit> exits;

    U32 labelCounter = 0;

    // The erased half, set only while a generic function is being emitted - the same two fields
    // resolve/lower.cpp carries, for the same reason.
    JsPtr<Expr> genEnv = nullptr;
    GenEnv* genContext = nullptr;
    Module* genModule = nullptr;
};

/*
 * Allocation.
 */

template<class T, class... A>
T* make(Gen& g, A&&... args) {
    return new (g.file.arena) T(forward<A>(args)...);
}

template<class T>
JsPtr<Expr> asExpr(Gen& g, T* value) {
    return (Expr*)value - g.base;
}

template<class T>
JsPtr<Stmt> asStmt(Gen& g, T* value) {
    return (Stmt*)value - g.base;
}

template<class T>
void emit(Gen& g, T* stmt) {
    g.body->push(g.file.arena, asStmt(g, stmt));
}

// Builds one statement list, with everything emitted while `f` runs going into it.
template<class F>
StmtList collect(Gen& g, F&& f) {
    StmtList list;
    auto previous = g.body;
    g.body = &list;
    f();
    g.body = previous;
    return list;
}

// Every instruction of a function, in block order - what the passes that read a whole body before
// anything is emitted (what is expressible, what it reaches, which locals are boxed) walk it with.
template<class F>
void eachInstruction(Gen& g, Function& function, F&& f) {
    for(auto blockPointer: function.blocks.contents(g.local)) {
        for(auto instructionPointer: g.local[blockPointer]->instructions.contents(g.local)) {
            f(*g.local[instructionPointer]);
        }
    }
}

/*
 * Names. The rest of naming is in name.cpp; these two are here because every builder below reaches
 * for them.
 */

// A name this generator spelled out itself rather than one derived from the program - `$tag`,
// `Math`, `BigInt`. No disambiguation, because nothing else may claim these.
Name literalName(Gen& g, StringView text);

// A property name. These are not identifiers in a scope, so they need no disambiguation - two
// records may both have an `x`, and a reserved word is a legal property name.
Name propertyName(Gen& g, StringView text);

/*
 * Expression builders.
 */

inline JsPtr<Expr> variable(Gen& g, Name name) {
    return asExpr(g, make<VarExpr>(g, name));
}

inline JsPtr<Expr> field(Gen& g, JsPtr<Expr> object, Name name) {
    return asExpr(g, make<FieldExpr>(g, object, name));
}

inline JsPtr<Expr> number(Gen& g, F64 value, bool integral = true) {
    return asExpr(g, make<NumberExpr>(g, value, integral));
}

// `2**bits` as an exact double, by doubling rather than by `pow`: every power of two up to 2^53 is
// representable, and this is a compile-time constant in the emitted text either way.
inline F64 powerOfTwo(U32 bits) {
    F64 result = 1;
    for(U32 i = 0; i < bits; i++) result *= 2;
    return result;
}

inline JsPtr<Expr> bigInt(Gen& g, U64 value, bool isSigned) {
    return asExpr(g, make<BigIntExpr>(g, value, isSigned));
}

inline JsPtr<Expr> boolean(Gen& g, bool value) {
    return asExpr(g, make<BoolExpr>(g, value));
}

inline JsPtr<Expr> nullValue(Gen& g) {
    return asExpr(g, make<NullExpr>(g));
}

inline JsPtr<Expr> elementAt(Gen& g, JsPtr<Expr> array, JsPtr<Expr> index) {
    return asExpr(g, make<IndexExpr>(g, array, index));
}

inline JsPtr<Expr> index(Gen& g, JsPtr<Expr> array, U32 slot) {
    return elementAt(g, array, number(g, F64(slot)));
}

inline JsPtr<Expr> binary(Gen& g, BinaryOp op, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    return asExpr(g, make<BinaryExpr>(g, op, lhs, rhs));
}

inline JsPtr<Expr> unary(Gen& g, UnaryOp op, JsPtr<Expr> value) {
    return asExpr(g, make<UnaryExpr>(g, op, value));
}

inline JsPtr<Expr> ternary(Gen& g, JsPtr<Expr> cond, JsPtr<Expr> then, JsPtr<Expr> otherwise) {
    return asExpr(g, make<TernaryExpr>(g, cond, then, otherwise));
}

inline JsPtr<Expr> assign(Gen& g, JsPtr<Expr> target, JsPtr<Expr> value) {
    return asExpr(g, make<AssignExpr>(g, target, value));
}

// The call an emitter builds when it knows its arguments: `call(g, f, a, b)`. The arity-at-runtime
// form is below, for the instructions that forward an argument list they were handed.
template<class... A>
JsPtr<Expr> call(Gen& g, JsPtr<Expr> callee, A... args) {
    auto node = make<CallExpr>(g, callee);
    (node->args.push(g.file.arena, args), ...);
    return asExpr(g, node);
}

inline JsPtr<Expr> callWith(Gen& g, JsPtr<Expr> callee, Array<JsPtr<Expr>>& args) {
    auto node = make<CallExpr>(g, callee);
    for(auto arg: args) node->args.push(g.file.arena, arg);

    return asExpr(g, node);
}

// A call this generator built out of a host intrinsic rather than out of the program - see
// CallExpr::pure, which is what the two builders below exist to set.
inline JsPtr<Expr> asPureCall(Gen& g, JsPtr<Expr> value) {
    ((CallExpr*)g.base[value])->pure = true;
    return value;
}

// `Namespace.member(...)` for the handful of host intrinsics the integer tower needs - `Math.imul`,
// `BigInt.asIntN`. Nothing else in the emitted code reaches for the host.
template<class... A>
JsPtr<Expr> hostCall(Gen& g, StringView object, StringView member, A... args) {
    return asPureCall(g, call(g, field(g, variable(g, literalName(g, object)),
                                       literalName(g, member)), args...));
}

// `Name(...)` - `BigInt(x)`, `Number(x)`. These are conversions rather than members of anything.
template<class... A>
JsPtr<Expr> globalCall(Gen& g, StringView name, A... args) {
    return asPureCall(g, call(g, variable(g, literalName(g, name)), args...));
}

/*
 * Statement builders.
 */

inline void emitExpr(Gen& g, JsPtr<Expr> value) {
    emit(g, make<ExprStmt>(g, value));
}

// `var name = value;`, and the name is what everything downstream uses for the value.
inline JsPtr<Expr> declare(Gen& g, Name name, JsPtr<Expr> value) {
    emit(g, make<DeclStmt>(g, name, value, false));
    return variable(g, name);
}

/*
 * name.cpp - identifiers.
 */

// Copies text the generator built into the string arena before interning it, since
// Context::addUnqualifiedName keeps the pointer it is given rather than copying.
StringId internText(Gen& g, StringView text);

// A module-level or local identifier, disambiguated against every name already handed out.
Name uniqueName(Gen& g, StringView text, bool local);

// `v7` for a value the source never named, so that the emitted code and the resolve dump agree on
// what to call it.
Name generatedName(Gen& g, StringView prefix, U32 index);

Name valueName(Gen& g, Value& value);
Name fieldName(Gen& g, StringId name, U16 index);

// The property a run of co-packed fields shares - `$p0` for the word at offset zero. See name.cpp.
Name packedWordName(Gen& g, U32 offset);

// One part of a flattened narrow reference - `p$o`, `p$k`, `p$s` - named after the reference it
// came from so the emitted source still says which one that was.
Name refPartName(Gen& g, Value& value, StringView suffix);

/*
 * type.cpp - what a type is on this target.
 */

IntType* intType(Gen& g, TypePtr type);
RecordType* recordType(Gen& g, TypePtr type);
bool isBool(Gen& g, TypePtr type);

// Whether this type is a host `bigint` - an integer wider than a `number` holds exactly. Every
// caller means "is this the other representation", which is why the test moved from the width class
// to the bit count when the 33-to-53 band stopped being one.
bool isLong(Gen& g, TypePtr type);

// Whether this type is one of the 33-to-53-bit `number`s wide.cpp is about. Never true at the same
// time as `isLong`, and both are false for the ordinary 32-bit tower.
bool isWideNumber(Gen& g, TypePtr type);

JsPtr<Expr> coerce(Gen& g, TypePtr type, JsPtr<Expr> value);

/*
 * wide.cpp - integers of 33 to 53 bits.
 */

// The name of one helper, emitting it on first use. Callers normally want `wideCall`.
Name wideHelper(Gen& g, WideOp op, U32 bits, bool isSigned);

// `$w53i$and(a, b)`. `b` is null for the one-operand operations.
JsPtr<Expr> wideCall(Gen& g, WideOp op, IntType* type, JsPtr<Expr> a, JsPtr<Expr> b);

// The same, for a caller that has a width rather than a type - a bit range of a packed word is one
// of these without being a value of any 33-to-53-bit type.
JsPtr<Expr> wideCallAt(Gen& g, WideOp op, U32 bits, bool isSigned, JsPtr<Expr> a, JsPtr<Expr> b);

// `v >= 2**(bits-1) ? v - 2**bits : v` - an unsigned bit pattern read back as a signed value.
JsPtr<Expr> resignExpr(Gen& g, JsPtr<Expr> value, U32 bits);

// A value crossing between a wide type and a narrower integer, in the widening direction.
JsPtr<Expr> wideFromNarrow(Gen& g, IntType* to, IntType* from, JsPtr<Expr> value);

// A tree of wide bitwise calls as one unpack-operate-pack, or null where this one is not that.
// Asked at the root of a tree and never below it - see opt.cpp, which is the only caller.
JsPtr<Expr> fuseWideBitwise(Gen& g, JsPtr<Expr> pointer);

// Every helper asked for, as function declarations. Called once, at the end of genProgram.
void emitWideHelpers(Gen& g);

// The same for the dynamic bit-range accessors. Emitted *before* the wide helpers, since a body
// here may ask for one of those and the list it would be appended to is already being walked.
void emitBitHelpers(Gen& g);

// Whether a value of this type is a host object - what `isMemoryType` is on native, asked of this
// target instead. See type.cpp for the three places the two answers differ.
bool isJsObject(Gen& g, TypePtr type);

/*
 * How one field of an object-shaped tuple is reached.
 *
 * Its own property, or - where this target co-packed it with its neighbours - a bit range of the word
 * they share. Four places have to agree about this: what properties a value of the type has, what a
 * place walk projects, what a copy duplicates, and what a reference to the field names. They agree by
 * asking here rather than by each deciding from a field name, which is what a packed field no longer
 * has one of.
 *
 * `leader` is true for exactly one field of each word - the one at bit zero - so that a walk listing a
 * type's properties emits a shared word once rather than once per field it holds.
 */
struct FieldProperty {
    Name name;
    TypePtr type = nullptr;    // the field's own, or `Int` for a word several of them share
    U8 bitOffset = 0;
    U8 bitWidth = 0;           // zero where the field owns its property
    U8 wordBits = 32;          // how wide the shared word is, which decides how it is taken apart
    bool leader = true;

    bool isPacked() const { return bitWidth != 0; }
};

FieldProperty fieldProperty(Gen& g, TypePtr tuple, U16 index);

/*
 * Whether a `&` of this type is the (object, property) pair Design.md's tier 2 becomes here.
 *
 * Narrow *and* without a host identity of its own. The first half is the language's answer - the same
 * predicate native decides a shift-carrying reference by - and the second is this target's: a value
 * with nowhere to hold a reference - a `Bool`, a `@bits` integer, a newtype over one, and now a
 * scalarized record - needs the pair, and an object is borrowed as itself.
 *
 * `isJsObject` is what carries scalarization into this: a record the Repr made one `number` stops
 * being an object, so a `&` of it becomes the pair, exactly as native's `&` of it became an address
 * plus a shift. That is the whole of what part 1 had to change here.
 */
inline bool isNarrowJsValue(Gen& g, TypePtr type) {
    return isNarrowValue(g.global, type) && !isJsObject(g, type);
}

/*
 * How wide the bit range behind a narrow reference is.
 *
 * Answered from the *type* rather than from any layout, which is what makes it available to a callee
 * that has only the pointee type - the same contract native's `naturalStorageBits(bits)` mask is
 * stated under. The shift is the one thing a callee cannot compute, which is why that and only that
 * rides along in the reference.
 */
inline U32 narrowWidth(Gen& g, TypePtr type) {
    return valueWidth(g.global, type).logical;
}

/*
 * Whether a narrow reference on this target carries a shift at all.
 *
 * This is the same elision native states about a full-width value - "a shift that is provably zero is
 * never represented" - applied one level up. A target that co-packs nothing and scalarizes nothing has
 * no bit ranges in it, so *every* narrow value sits at offset zero of the storage it names, and the
 * reference is the plain `{$o, $k}` pair with `r.$o[r.$k]` as an lvalue on both sides.
 *
 * It matters here rather than only saving two operations, because the read-modify-write a shift forces
 * is not the identity on this target the way it is on native: a `Bool` field holds `true`, and
 * `(owner & ~1) | (v & 1)` would store `1`. So while nothing is packed, nothing may pay for it.
 */
inline bool narrowRefCarriesScale(Gen& g) {
    return g.repr.target.packFields || g.repr.target.scalarizeRecords;
}

/*
 * How wide a word a *reference* may name, which is not a question the callee can ask of its argument.
 *
 * A body compiled once against `&Bool` is reached from a whole local, a co-packed field and a
 * scalarized record alike, so the widest word this target packs is the only honest answer - and it is
 * a target-global constant, which is what lets the caller and the callee reach the same one without
 * seeing each other. Where the target packs nothing wider than 32 bits this is 32 and every reference
 * keeps the shift-and-mask form.
 */
inline U32 maxWordBits(Gen& g) {
    return min(g.repr.target.maxPackBits, U32(kMaxNumberBits));
}

/*
 * Whether a `&` of this pointee crosses a call as separate arguments rather than as one object.
 *
 * The descriptor is what makes a narrow reference cost anything here: native's shift rides in spare
 * bits of a word already being passed, and this target has to allocate an object per borrow to carry
 * the same three things. Passing them as three arguments removes the allocation and most of the cost -
 * measured at 151% for one borrow and 366% for four through a callee V8 declines to inline, with the
 * extra arity free in that range and the shift argument free in particular.
 *
 * Decided from the declaration and nothing else, because it is a calling convention: a witness-table
 * slot, a closure and a `CallDyn` all have to agree with the direct call without seeing the callee,
 * exactly as native's narrow-borrow rule is decided from the pointee type alone.
 *
 * Asked of a *declaration* - `&f: Flags` - rather than of a type, because that is the form the
 * question exists in: a parameter's type is its pointee and the `&` is a binding convention beside
 * it, and only a mutable borrow becomes a reference at all (see resolveArgs, where `borrowed` is set
 * for exactly that case). An immutable borrow of a narrow value is passed by value and has nothing
 * to write back through.
 */
inline bool refIsFlattened(Gen& g, TypePtr declaredType, ast::BindType convention) {
    return convention == ast::BindType::Ref && isNarrowJsValue(g, declaredType);
}

// How many arguments a flattened reference occupies. Two where the target has no bit ranges in it and
// the scale is provably one - see narrowRefCarriesScale.
inline U32 flatRefArity(Gen& g) { return narrowRefCarriesScale(g) ? 3 : 2; }

RefParts refPartsOf(Gen& g, ModulePtr<Value> reference);
RefParts refPartsOfExpr(Gen& g, JsPtr<Expr> reference);
JsPtr<Expr> materializeRef(Gen& g, RefParts parts);

// Whether some use of a flattened reference needs it to be one value after all - a return, a store,
// a capture. Defined in gen.cpp beside the other use-list questions.
bool narrowRefNeedsObject(Gen& g, ModulePtr<Value> reference);

// Whether the parameter at one argument position of a call takes its reference flat. The emitter and
// the question above both decide a reference argument's arity from this, and they have to agree.
bool callParameterIsFlatRef(Gen& g, Value& user, Size index);

// Whether this signature flattens its references at all - the arity guard, which is all-or-nothing
// for a signature so that the caller and the callee reach it independently and agree.
bool functionFlattensRefs(Gen& g, Function& function);

/*
 * A bit range within the value a place names, or nothing where the place names the whole of one.
 *
 * This is what a field of a scalarized record is: the walk stops at the `number` that holds it and
 * reports where inside that number the field sits, because there is no property to descend into.
 * Offsets *compose* - `t.f.a` is one bit of `Flags` at bit zero, and `Flags` is two bits of `Two` at
 * bit zero or two - and neither number is written anywhere in the source, which is why they are
 * accumulated here rather than read off the last projection.
 *
 * A place carrying one of these is no longer a location. Reading it is a shift and a mask and writing
 * it is a read-modify-write of the whole binding, which is why the two interception points are the
 * load and the store rather than the place walk itself - the same split native settled on for a
 * packed field and a folded tag.
 */
struct PlaceBits {
    U32 offset = 0;
    U32 width = 0;

    /*
     * How wide the word this range lives in can be, which decides *how* it is taken apart.
     *
     * At 32 or below the range is reached with `>>>` and `&`, the operators JS actually has. Above
     * it those stop working - a shift count is masked to five bits, so `word & ~(mask << 32)` clears
     * nothing at all - and the range is reached by dividing and multiplying instead. Which of the two
     * a site uses has to be decided from the word rather than from the field, because a field
     * entirely below bit 32 still shares its word with everything above it and a `&` would drop all
     * of that on the way back in.
     *
     * Thirty-two by default, so a place that never entered a packed word at all - and a target with
     * no bit ranges in it - keeps the operators it always had.
     */
    U32 word = 32;

    /*
     * A scale known only at run time, multiplying `2**offset`.
     *
     * This is what a reference into a bit range carries, and it is why the two are separate numbers:
     * a callee compiled once against `&Bool` does not know whether its argument was a field of a
     * scalarized record, a co-packed field, or a whole local, so the scale arrives with the reference
     * while the offsets of any fields it projects further are its own body's constants. Reborrowing
     * multiplies the two, exactly as native adds a shift to a field offset.
     *
     * `2**shift` rather than the shift, because the callee cannot know how wide the word it was
     * handed is and therefore has to use the division form for every one of them - and computing
     * `2**s` from `s` at each access is a `Math.pow` per read. Carrying the number the division
     * actually wants costs nothing anywhere else: a reborrow multiplies where it used to add, and
     * both operands are constants at every site that builds one.
     */
    JsPtr<Expr> scale = nullptr;

    /*
     * The record whose folded tag this place names, where it names one.
     *
     * A folded tag is not stored anywhere, so the place still resolves to the payload - which *is* the
     * record - and the two ends intercept: reading is a comparison of that value against what its
     * payload could legally be, and writing is a store of one impossible value or nothing at all. The
     * same split native settled on, and the same one the bit range above is handled by, for the same
     * reason: a place is a location and neither of these is one.
     */
    TypePtr foldedTag = nullptr;

    bool valid() const { return width != 0; }
};

// The shift-and-mask that reads `bits` out of `owner`, and the read-modify-write that puts one back.
// `type` is the field's own type, because a `Bool` is a host boolean rather than a one-bit number and
// has to be converted in both directions.
JsPtr<Expr> decodeBits(Gen& g, JsPtr<Expr> owner, PlaceBits bits, TypePtr type);
JsPtr<Expr> encodeBits(Gen& g, JsPtr<Expr> owner, PlaceBits bits, TypePtr type, JsPtr<Expr> value);

// The same two for a folded tag: which constructor a payload's own value is, and what writing one
// constructor's tag over that payload comes to. See place.cpp.
JsPtr<Expr> decodeNicheTag(Gen& g, JsPtr<Expr> value, TypePtr record);
void encodeNicheTag(Gen& g, JsPtr<Expr> target, TypePtr record, U64 constructor);

// The content of the constructor a folded record's payload belongs to - what the record *is*.
TypePtr foldedPayload(Gen& g, TypePtr record);

/*
 * The properties one value of this type has, in construction order.
 *
 * One walk, used by everything that has to agree about the shape of a type: what a fresh slot holds,
 * what a copy duplicates, and what a block copy moves. Two of those disagreeing would be a bug that
 * only shows up as a polymorphic call site or a lost field, so they read the order from here rather
 * than each writing the walk.
 *
 * A sum flattens every constructor's payload into one object - see gen.cpp's file comment - so two
 * constructors that both name a field share the property. That is sound because only one of them is
 * live at a time, and it is what keeps the type to one hidden class instead of one per constructor.
 */
template<class F>
void eachProperty(Gen& g, TypePtr type, F&& f) {
    if(!type || isUnit(g.global, type)) return;

    auto value = g.global[type];

    // A function value has no properties here: it is a host function, and the two words
    // FunValueLayout describes are the closure itself and what it closed over.
    if(value->kind == Type::Fun) return;

    if(value->kind == Type::Tup) {
        auto count = ((TupType*)value)->fields.size();
        for(U16 slot = 0; slot < count; slot++) {
            // A co-packed run is one property, contributed by the field at bit zero of it. Skipping
            // the others is what keeps the object's shape the *words* it has rather than the fields.
            auto property = fieldProperty(g, type, slot);
            if(property.leader) f(property.name, property.type);
        }

        return;
    }

    if(value->kind != Type::Record) return;

    auto record = (RecordType*)value;
    if(record->layout == RecordType::Enum) return;

    if(record->layout == RecordType::Single) {
        if(record->constructors.isNotEmpty()) {
            eachProperty(g, record->constructors.get(g.global, 0).content, forward<F>(f));
        }

        return;
    }

    f(g.tagField, g.program.scalar.int_);

    Array<StringId> seen;
    auto payload = false;

    // What the shared payload property holds, where every constructor that uses it agrees. Null where
    // two of them disagree, which is the only case a fresh slot cannot be given the right zero for.
    TypePtr payloadType = nullptr;
    auto payloadMixed = false;

    for(auto constructor: record->constructors.contents(g.global)) {
        auto content = constructor.content;
        if(!content || isUnit(g.global, content)) continue;

        // A payload with no field names to flatten, and a payload the Repr made one number, which has
        // none to flatten *into* - it is one value, so it is one property, exactly as a bare one is.
        if(g.global[content]->kind != Type::Tup || !isJsObject(g, content)) {
            if(payload && payloadType != content) payloadMixed = true;
            payloadType = content;
            payload = true;
            continue;
        }

        auto count = ((TupType*)g.global[content])->fields.size();
        for(U16 slot = 0; slot < count; slot++) {
            auto property = fieldProperty(g, content, slot);
            if(!property.leader) continue;

            auto known = false;
            for(auto existing: seen) if(existing == property.name.text) known = true;
            if(known) continue;

            seen.push(property.name.text);
            f(property.name, property.type);
        }
    }

    // A constructor whose content is not a tuple has no field names to flatten, so its payload is
    // one property of its own.
    if(payload) f(g.payloadField, payloadMixed ? nullptr : payloadType);
}

/*
 * Whether this type is a newtype: a single-constructor record over something that is not a tuple,
 * which on this target *is* the value it wraps and has no object of its own - Analysis-JS.md part
 * 1's "newtype: the underlying value, no wrapper". `content` is what it wraps, and is null for a
 * constructor with no content at all, which the callers treat the same way they treat unit.
 */
bool isNewtype(Gen& g, TypePtr type, TypePtr& content);

// The value a freshly allocated slot of this type holds, with every property it will ever have
// already present - see type.cpp.
JsPtr<Expr> zeroValue(Gen& g, TypePtr type);

// A one-property box: what a reference to something that is not an object has to be.
JsPtr<Expr> boxOf(Gen& g, JsPtr<Expr> value);

// An arithmetic result back in its type's range - the integer tower of Analysis-JS.md §2.1.
JsPtr<Expr> coerce(Gen& g, TypePtr type, JsPtr<Expr> value);

// A structural duplicate, property by property - the one ownership operation that costs anything
// here (§2.5).
JsPtr<Expr> cloneValue(Gen& g, TypePtr type, JsPtr<Expr> source, LocationId where);

// The shape a `Native` block copy moves, or null where it is not one whole value of one type.
TypePtr blockCopyShape(Gen& g, InstNative& instruction);

/*
 * place.cpp - values, places and the erased tables.
 */

JsPtr<Expr> constantValue(Gen& g, Value& value);
JsPtr<Expr> useValue(Gen& g, ModulePtr<Value> pointer);

// The emitted name of a module-level global.
JsPtr<Expr> globalValue(Gen& g, ModulePtr<Global> pointer);

JsPtr<Expr> placeExpr(Gen& g, const Place& place, Size limit = maxLimit<Size>);
TypePtr placeType(Gen& g, const Place& place, Size limit = maxLimit<Size>);

/*
 * The place as an owner plus a bit range, for the two callers that have to tell them apart.
 *
 * `placeExpr` above answers with the decode already applied, which is what every *reader* wants. A
 * writer cannot use that - the result is an expression rather than an lvalue - so it asks here, gets
 * the `number` the field lives in and where inside it, and emits the read-modify-write. `bits` comes
 * back invalid for every place that is an ordinary location, which is all of them until a target
 * scalarizes something.
 */
JsPtr<Expr> placeOwner(Gen& g, const Place& place, PlaceBits& bits, Size limit = maxLimit<Size>);

// The argument a teardown or a relocation takes: those are written against a raw pointer, so a
// value that is not an object has to arrive in a box the way any other reference does.
JsPtr<Expr> referenceTo(Gen& g, const Place& place, Size limit = maxLimit<Size>);
JsPtr<Expr> referenceTo(Gen& g, TypePtr type, JsPtr<Expr> value);

// A constant table is an array of 32-bit cells, so every offset the native side loads at becomes a
// cell index. `>> 2` is the whole of the translation, and it is exact because every pointer field in
// these layouts is eight-byte aligned and every scalar field is four.
JsPtr<Expr> tableCell(Gen& g, JsPtr<Expr> table, U16 slot);
JsPtr<Expr> genSlot(Gen& g, U16 slot);
JsPtr<Expr> genWitness(Gen& g, U16 slot, ModuleList<U32, false> path);
JsPtr<Expr> genTypeDesc(Gen& g, TypePtr type);

/*
 * inst.cpp - instructions.
 */

void genInstruction(Gen& g, ModulePtr<Inst> pointer);

// The emitted name of a function, with a diagnostic where this target does not have it.
JsPtr<Expr> functionValue(Gen& g, ModulePtr<Function> callee, LocationId where);

/*
 * flow.cpp - control flow.
 */

// The CFG in the function's own block order, plus dominance and the loop headers it decides.
void prepareCfg(Gen& g, Function& function);

// Emits blocks from `block` up to but not including `stopAt`, recovering `if`, `for(;;)` and
// labelled `break`/`continue` from the graph.
void emitChain(Gen& g, U32 block, U32 stopAt);

/*
 * opt.cpp - the peephole between the tree and the text.
 */

// Removes the bindings nothing needed and folds the writes that build a value into the literal that
// builds it. Runs over the finished file, because how many readers a binding has is not known until
// the function it is in has been emitted.
void optimizeFile(Gen& g);

} // namespace js
