#pragma once

#include "gen.h"
#include "../../resolve/generic.h"
#include "../../resolve/place.h"
#include "../../resolve/witness.h"
#include "../../repr/table.h"
#include "../../resolve/const.h"
#include "../../resolve/host.h"

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

// One saturating float-to-`bigint` conversion, per target type - see genCast in inst.cpp.
struct SatHelper {
    Name name;
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

    /*
     * The environment word's key, set only for a reference to a *function value* - see refIsTriple.
     *
     * A function value is two words, so a reference to one names two properties of the same owner:
     * `{$o: record, $k: "run$c", $ke: "run$e"}`. That is the same pair every other reference is,
     * with the second key where a narrow one carries a shift, and it is why a borrowed function
     * value needs no representation of its own.
     */
    JsPtr<Expr> envKey = nullptr;

    bool valid() const { return owner != nullptr; }
};

/*
 * The two words of a function value, however this one is being carried.
 *
 * The same arrangement as `RefParts` above and for the same reason: a function value that arrived
 * flat - a parameter, or a local whose every use these two can serve - has each word in a variable
 * of its own and there is no object anywhere. One that did not is an ordinary `{$c, $e}`, and the
 * words are its properties. Every consumer wants the words rather than the object, which is what
 * lets the two forms coexist without a flag at each use.
 *
 * The one thing this does not share with `RefParts` is *when* the parts are known. A reference is
 * built by one instruction, so its parts exist the moment the value does; a function value is
 * storage that two `Init`s fill, so the parts here are the two variables those writes assign and
 * they hold nothing until the writes have been emitted. Which is why nothing materializes an object
 * at the top of the body the way a flattened reference parameter does - see genBody - and why
 * `useValue` builds one at the point of use instead.
 */
struct FunParts {
    JsPtr<Expr> code = nullptr;
    JsPtr<Expr> env = nullptr;

    bool valid() const { return code != nullptr; }
};

/*
 * The lanes of a vector, however this one is being carried - Implementation-Vector.md §7.
 *
 * The third of the same arrangement `RefParts` and `FunParts` above are, and for the strongest
 * version of its reason: a vector is `lanes` independent values here, so a vector that is being
 * carried as its lanes has each in a variable of its own and there is no array anywhere. One that is
 * not - a phi at a join, a field of a record, an argument crossing an erased boundary - is a host
 * array, and the lanes are its elements.
 *
 * Where the other two are an optimization, this one is the representation. §2 of
 * Implementation-Vector.md says a vector is *always* scalarized on this target and never boxed at
 * any lane count, and what that means concretely is that every operation over one produces lanes
 * rather than a value: `a + b` is `lanes` additions and the array is what does not get built.
 *
 * A raw pointer and a count rather than an `Array`, because these live in a `HashMap` keyed on the
 * resolve value and `HashMap::reset` does not run destructors - the same trap `AggregateBuildPlan`
 * is written not to fall into. The lanes are allocated out of the file's own arena, which outlives
 * every function generated into it.
 */
struct VecParts {
    JsPtr<Expr>* lanes = nullptr;
    U16 count = 0;

    bool valid() const { return lanes != nullptr; }
    Buffer<JsPtr<Expr>> contents() const { return { lanes, count }; }
};

/*
 * Whether an `InstAggregate` builds its local's whole value here, and which property each component
 * fills - the answer to that question and nothing else.
 *
 * It has to be asked twice and answered the same both times: `prepareBuiltLocals` reads it to decide
 * the allocation can be declared holding nothing, and `genAggregate` reads it to build. A
 * disagreement is `var v; v[0] = 1` - an allocation with no value and an instruction that expected
 * one to be there. So the eligibility question is *one* function, and it builds nothing while
 * answering it: the first caller has no statement to emit uses into, and the version of this that
 * decided by constructing a literal and throwing it away made JS nodes for every aggregate in the
 * program.
 *
 * Computed twice rather than cached, and that is a deliberate trade rather than an oversight. It is
 * a pure function of the instruction, so the two calls cannot disagree; caching it would put a plan
 * in a `HashMap`, whose `reset` does not run destructors and whose slots are raw until something
 * placement-news them - and this plan owns a `SmallArray`. A leak and a construction subtlety is a
 * poor price for turning "the same answer twice" into "the answer once".
 *
 * The object literal `buildFromPlan` then produces is what Analysis-JS.md §2.3 asks for.
 * `var v = {x: 0, y: 0}` followed by two writes is what it replaces, and the zero it removes is the
 * point: a fresh value of a type has to be manufactured out of that type's own shape, and there are
 * types this side of an abstraction boundary that have no shape to build one from. Here the values
 * are what the object is made of, so nothing is manufactured and nothing is written twice.
 *
 * Nothing is built where the value is not an object here at all - a scalarized record, a newtype, a
 * niche-folded one - since a fresh one of those is a number or a null and costs nothing to make. Nor
 * where a field is **co-packed** (a bit range of a property rather than one; `packCandidate` already
 * declines those in the resolver, and this is the same rule from the other end) or a **function
 * value** (two properties from one component, which `storeInto` already knows how to write).
 */
struct AggregateBuildPlan {
    enum Kind: U8 { Object, Array };

    // Which component fills a property, by the name it fills. A property no component names is one
    // the construction does not reach - see buildFromPlan, which is where the zeros come in.
    struct Filled {
        Name key;
        Size at;
    };

    bool eligible = false;
    Kind kind = Object;
    TypePtr type = nullptr;
    SmallArray<Filled, 16> filled;
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

    /*
     * The globals stored as a one-property box, for the same reason a local is - see prepareLocals.
     *
     * A module-level global is a bare `var` and JS has no object to reach one through, so a
     * reference to one had nothing to name and became a copy with a write-back after it. The box is
     * what gives it a slot: `{$o: counter, $k: "$v"}` names the storage rather than a snapshot of
     * it, and the reference works for as long as it lives instead of until the call that took it
     * returns.
     *
     * Whole-program, and it has to be: a global is borrowed in one function and read in another, so
     * both have to agree about the shape without seeing each other. Collected by boxedGlobals()
     * before anything is emitted.
     */
    HashSet<U32> boxedGlobals;

    /*
     * The module-level `var`s a call can write, by name - the one kind of bare identifier whose read
     * is a read of *storage*.
     *
     * The optimizer's rewrites all rest on one premise, which `propagateCopy` states outright: a
     * local `var` is invisible to every callee, so nothing but this function's own text can change
     * one. A module-level global is exactly the thing that premise is false of - `bump()` assigns
     * `seen` from another function entirely - and without this, reading one looks as inert as
     * reading a parameter. `let a = bump(); return seen + a` then became `return seen + bump()`,
     * which reads the global *before* the call that writes it.
     *
     * **Which globals, is a question about the program rather than about the declaration.** `let &`
     * says a global *may* be assigned, and `FloatNaN.yana`'s `zero` and `one` are declared that way
     * only so that the division is not constant-folded - nothing ever assigns them, and treating
     * them as storage puts a local in front of every comparison in that fixture. So the set is read
     * off the places every emitted function names; see mutableGlobals(), which fills it.
     *
     * Two more are excluded there. A table is a `const` and is never assigned. And a boxed one's
     * variable holds the box, which nothing reassigns either - a write goes to `.$box`, and a
     * property read already counts as a read.
     */
    HashSet<StringId> mutableGlobals;

    /*
     * The one-field tuples that keep their wrapper - see isTransparentTuple, whose answer this
     * overrides, and opaqueTuples() which fills it.
     *
     * Transparency removes an object, and `genBorrow` has one shape it cannot then name: the
     * *address* of storage reached through a projection whose type is no longer an object. There is
     * no slot to point at - the enclosing object's property holds a value rather than a reference -
     * and the box that would stand in for one is a copy, which is exactly the form B removed. So
     * those types keep the wrapper, and the wrapper is the slot.
     *
     * Whole-program, and it has to be, for the same reason `boxedGlobals` is: a type is constructed
     * in one function and has its address taken in another, and a shape the two disagree about is a
     * value written one way and read another.
     *
     * **Keyed on the tuple, never on the record that holds it.** `eachProperty` asks about the
     * payload tuple and `zeroValue` about the record, so an exclusion keyed on the record makes one
     * of them unwrap and the other not - a value constructed bare and zeroed as an object, which is
     * a silent wrong answer rather than a diagnostic. The tuple is the thing that would lose the
     * wrapper, so it is the thing that can be told not to.
     *
     * Conservative in one direction on purpose: a tuple is interned on its fields, so excluding one
     * excludes every declaration with those fields. That costs a wrapper somewhere it was not needed
     * and cannot cost correctness, which is the trade `boxedGlobals` makes as well.
     */
    HashSet<U32> opaqueTuples;

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

    // The saturating float-to-`bigint` conversions, interned per (width, signedness). The number
    // case needs no helper; inst.cpp says why.
    HashMap<U32, Name> satHelpers;
    Array<SatHelper> satHelperOrder;

    /*
     * The scratch pair a `Float`/`I32` bitcast goes through - see genBitcast in inst.cpp.
     *
     * A `Float` here is a `number` that `Math.fround` has made exactly representable, so its
     * thirty-two bits are well defined and completely unreachable: this target has no operator that
     * sees them. Two views over one buffer is the only way, and one buffer per program is enough
     * because the write and the read that follows it are the whole of the operation.
     *
     * Named lazily, so a program that never reinterprets a float emits neither declaration.
     */
    Name floatBitsBuffer;
    Name floatBitsInts;
    Name floatToBitsHelper;
    Name bitsToFloatHelper;

    /*
     * The same pair at 64 bits - a `Float64Array` and a `BigInt64Array` over its buffer.
     *
     * Two pairs and not one because the *element type* is what does the reinterpretation, and the
     * integer side of this one hands back a `bigint` where the 32-bit side hands back a `number`.
     * A `Vec(F64)` lane read through the 32-bit pair returns the low half as a number, which then
     * meets `BigInt.asIntN` and throws - which is exactly what happened while `Vec(Long)` was
     * refused and this pair did not exist.
     *
     * Named lazily and independently, so a program that reinterprets only floats emits only the
     * first pair.
     */
    Name doubleBitsBuffer;
    Name doubleBitsInts;
    Name doubleToBitsHelper;
    Name bitsToDoubleHelper;

    /*
     * `round`, which is the one of the four roundings the host does not already have.
     *
     * `Math.round` breaks ties toward **positive infinity** - `Math.round(-1.5)` is -1 - and the
     * language's `round` breaks them away from zero, so the two disagree at every negative half.
     * `Math.trunc`, `Math.floor` and `Math.ceil` need no such helper: each is exactly the kind it
     * implements, at every input including the infinities and the two zeros.
     *
     * A helper rather than an inline conditional because the operand appears three times in it, and
     * a lane here is an expression that may hold a call. Named lazily on floatBitsHelper's terms.
     */
    Name roundAwayHelper;

    /*
     * `$swap16`, `$swap32` and `$swap64` - the byte reversal, which this host has no operator for at
     * any width.
     *
     * One helper per width rather than one per type: what a swap does depends on how many bytes
     * there are and on nothing else, so the two signednesses of a width share a helper and the
     * caller's own coercion is what puts the sign back. Indexed by width - 16, 32 and 64 are slots
     * 0, 1 and 2 - because the alternative is three names spelled out at every site that asks.
     *
     * Helpers rather than an inline expression for `roundAwayHelper`'s reason, doubled: the operand
     * appears four times in the 32-bit shape and eight in the 64-bit one, and an operand here is an
     * expression that may hold a call. Named lazily on floatBitsHelper's terms, so a program that
     * never swaps emits none of them.
     */
    Name byteSwapHelpers[3];

    /*
     * `$popcount32` / `$ctz32` and their three 64-bit partners - the population count and the two
     * zero counts, which the host answers exactly one of.
     *
     * `Math.clz32` is the one JavaScript has, and it is why there is no `$clz32`: a 32-bit leading
     * count is that call and nothing else, so the slot for it stays empty and the site emits the
     * call inline. Everything else is a helper for `byteSwapHelpers`' reason - the operand appears
     * three or four times in each shape, and an operand here may be a call.
     *
     * Indexed by `bitCountHelperSlot`: the operation, then the width. The 64-bit three are the
     * `bigint` domain and are written over the two halves rather than over the whole, because
     * `Math.clz32` is the fast path and a `bigint` loop is not - see `emitBitCountHelpers`.
     */
    Name bitCountHelpers[6];

    /*
     * `$rol32` and its nine partners - the two rotations, at each of the five widths a scalar
     * `Integral` instance is generated for (8, 16, 32, 53 and 64) and at each lane width a vector
     * reaches through the same call.
     *
     * A helper for `byteSwapHelpers`' reason and one more of its own. The operand appears twice in
     * every shape and an operand here may be a call, which is the first reason; the second is that
     * the three host domains a rotation crosses - an `int32` operator set below 33 bits, wide.cpp's
     * helpers to 53, and `bigint` above - each need a different body, and a function is where three
     * bodies can share one name at the call site.
     *
     * Indexed by `rotateHelperSlot`: the direction, then the width. Named lazily, so a program that
     * rotates nothing emits none of them.
     */
    Name rotateHelpers[10];

    /*
     * `$bzhi32` and its five partners - `Core.Bits.bitsUpTo` and the two directions of
     * `Core.BitPermute`, at the two widths those are declared over.
     *
     * Helpers for `rotateHelpers`' two reasons and one that is theirs alone. The operand appears
     * several times in each shape and an operand here may be a call; the two domains a 32-bit and a
     * 64-bit body work in are different (`int32` operators against `bigint`); and the permutations
     * are a *loop*, which is not an expression in any domain. There is no 53-bit slot, `Bits` being
     * declared at 32 and 64 only.
     *
     * Indexed by `bitOpHelperSlot`: the operation, then the width. Named lazily, so a program that
     * asks for none of the three emits none of them.
     */
    Name bitOpHelpers[6];

    /*
     * `$div` and `$rem` - the two divisors the language answers for, where the host does not.
     *
     * `x / 0` is 0 and `x % 0` is `x` (doc/spec/types.md, and the ruling beside `Div` in
     * resolve/inst.def), and JavaScript agrees with exactly one half of one of them. A 32-bit
     * quotient is already right for nothing: `a / 0` is an infinity and the `|0` this backend emits
     * anyway turns any infinity into 0. Everything else disagrees, and disagrees three different
     * ways - `a % 0` is NaN, the 33-to-53-bit band's `wrap` turns that quotient into NaN too, and
     * BigInt division *throws* a RangeError rather than answering at all.
     *
     * One pair of helpers serves all three bands, which is what `b ? a / b : b` buys: the zero arm
     * returns the *divisor*, so the answer carries the operand's own type and neither `0` nor `0n`
     * has to be written. `!b` is true for both zeros and for no other integer.
     *
     * Helpers rather than an inline conditional for `roundAwayHelper`'s reason and one more: each
     * operand appears twice, and an operand here is an expression that may hold a call - but also,
     * a division is where a program least minds a call, being the slowest arithmetic on any target.
     *
     * Named lazily on floatBitsHelper's terms, so a program that never divides emits neither.
     */
    Name divideByZeroHelper;
    Name remainderByZeroHelper;

    /*
     * The one helper the typed row of Implementation-Containers.md §14 needs - see NativeOp::HostGrow.
     *
     * Named lazily and once per program, on the same terms as the two above: a program with no typed
     * array that ever grows emits nothing. It is written over `a.constructor` rather than over a
     * constructor name, so one function serves every element type - which is also why the element
     * predicate is not consulted here at all.
     */
    Name growHelper;

    /*
     * The word transfers, and the `DataView` they ride on - see NativeOp::HostWordRead.
     *
     * `viewHelper` is the cache and the other four are the operations. Five names rather than one
     * expression per call site because that is what makes the cache worth having: `new DataView` at
     * every call is fifteen times slower than the shift chain these replace, so the view is built
     * once per array and hung off it, and every word after the first is one property read.
     *
     * A property on the array rather than a table keyed by buffer, which was the other candidate and
     * is slower than doing nothing: a one-entry memo collapses to 30x the moment a loop touches two
     * buffers - which a digest reading a message and writing a pad does - and a `WeakMap` lookup
     * costs more than the shifts. The property was measured not to disturb the array it is attached
     * to: element access over a tagged `Uint8Array` is 8.4 ms against 8.4 ms plain.
     *
     * Named lazily, one per width and direction, on `growHelper`'s terms. The slot is the width: 0 is
     * 16 bits, 1 is 32 and 2 is 64.
     */
    Name viewHelper;
    Name readWordHelper[3];
    Name writeWordHelper[3];

    // The heading each family of helpers is emitted under, so that a family the peephole emptied
    // takes its own comment with it - see removeDeadHelpers.
    JsPtr<Stmt> wideHelperComment = nullptr;
    JsPtr<Stmt> bitHelperComment = nullptr;

    // The property names that are the compiler's rather than the program's.
    Name tagField;
    Name payloadField;
    Name boxField;
    Name codeField;
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

    // The second key a reference to a function value carries - see RefParts::envKey.
    Name refEnvKey;

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

    // The function values being carried as their two words rather than as an object - see
    // funPartsOf. A value in here is *not* in `values`: an object for one is built where a use
    // genuinely needs one value, and `useValue` is where that happens.
    HashMap<U32, FunParts> funParts;

    // The vectors currently being carried as their lanes rather than as an array - see vecPartsOf.
    // A value in here is *not* in `values`, on the same terms as `funParts`: the array is built
    // where a use genuinely needs one value, and `useValue` is where that happens.
    HashMap<U32, VecParts> vecParts;

    // Which locals of function type are carried that way, by local index - see prepareFunLocals.
    // The place walk reads this to answer `local@Fun.code` with a variable rather than a property.
    IndexSet flatFuns;

    // And which locals whose *value* is a narrow reference are carried as that reference's parts
    // rather than as a `{$o,$k,$s}` object - see prepareRefLocals. The parts themselves are in
    // `flatRefs`, keyed on the allocation, exactly as a flat function local's are in `funParts`.
    IndexSet flatRefLocals;

    /*
     * And which of those hold nothing but the absent constructor - the `Nothing` temporary an arm
     * that found no entry builds, which nothing ever writes a reference into.
     *
     * Such a local is *one* variable: the key and the shift have no value to hold, and declaring
     * them would leave a copy of two undefined variables in every arm that hands one on. What makes
     * leaving them out sound is the invariant the whole flattening rests on - the key and the shift
     * are read only where the owner is not null - so a destination that keeps its stale key after
     * being overwritten with one of these keeps a key nothing may read.
     */
    IndexSet flatRefTagOnly;


    // Which locals are stored as a one-property box, by local index. See gen.cpp's file comment.
    IndexSet boxed;

    /*
     * Which locals an `InstAggregate` builds whole, by local index - see prepareBuiltLocals.
     *
     * The allocation of one is declared holding *nothing*, because the value it would otherwise hold
     * is a manufactured instance of the local's type and the aggregate replaces it outright. That is
     * the whole of what `zeroValue` was for at an allocation, and the reason it is worth removing is
     * not the statement it saves: a fresh value has to be built out of the type's own shape, and a
     * type this side of an abstraction boundary does not have one to build from.
     */
    IndexSet builtWhole;

    /*
     * The values this body *keeps* in more than one position - see prepareKeptValues.
     *
     * A position that keeps a value is one that has to end up holding a value of its own: a write
     * into storage, an aggregate's component, a `return`, a phi input. One value reaching two of
     * them is two names for one object on this target, so all but one of them would have to be a
     * duplicate - and which one may go free is not decidable in emission order, since the free one
     * would then be mutated through before the later duplicate read it. So every one of them
     * duplicates, and the set is what says so.
     *
     * It is not something the resolver's own output contains: an ownership-checked body consumes a
     * value once. `compiler/opt`'s store-to-load forwarding is what produces it, and it is right to
     * - `load p` where `copy p, v` came before it holds `v`'s bytes, which is what every native
     * consumer copies out of. This target is the one that has to put the copy back.
     */
    HashSet<U32> keptTwice;

    // The borrow and address values that are a second *name* for the storage they were taken of
    // rather than a box holding it - see prepareLocals. A place rooted in one of these reaches the
    // storage directly, which is what makes a borrow that never leaves the function cost nothing.
    HashSet<U32> aliasBorrows;

    // The CFG, in the function's own block order.
    Array<ModulePtr<Block>> blocks;
    HashMap<U32, U32> blockIndex;
    /*
     * Both dominance relations, and the set the fixpoint that builds them works in.
     *
     * Kept on the generator rather than returned by `dominanceSets`, because there is one of these
     * per module and one call of that per function: the rows are re-sized to the next function's
     * block count and the storage behind them is the previous function's. `dominators` is only read
     * while it is being turned into `idom` and `loopHeader`, and is held for the same reason.
     */
    IndexSetList postDominators;
    IndexSetList dominators;
    IndexSet flowScratch;

    Array<U32> ipdom;
    Array<U32> idom;
    IndexSet loopHeader;
    IndexSet emitted;

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
        for(auto instructionPointer: g.local[blockPointer]->instructions(g.local)) {
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

/*
 * A `bigint` literal's value, or false where this is not one.
 *
 * The `bigint` half of `constantNumber` below, and it is a separate question rather than a widening
 * of that one: the two domains do not mix in this language's output any more than they do in the
 * host's, so a fold that took either would be a fold that could write `0n === 0`.
 *
 * `value` is the raw sixty-four bits and `isSigned` says which number they spell, so a caller
 * comparing two of them has to agree about that first - see `foldBigIntComparison`.
 */
inline bool constantBigInt(Gen& g, JsPtr<Expr> pointer, U64& into, bool& isSigned) {
    auto expr = g.base[pointer];
    if(expr->kind != Expr::BigInt) return false;

    into = ((BigIntExpr*)expr)->value;
    isSigned = ((BigIntExpr*)expr)->isSigned;
    return true;
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

/*
 * Evaluating the host's operators, and the one rule about when an answer may be written down.
 *
 * Every bit range this target reaches is built out of `&`, `|`, `<<` and `~` against numbers the
 * emitter worked out - a field's mask, its offset, the hole its neighbours must keep - and where the
 * value being written is a literal too, the whole read-modify-write is arithmetic over constants.
 * `Boxed {k: 7, a: 1, b: 4095}` emitted `~(4095 << 20)` and `(4095 & 4095) << 20`, and opt.cpp could
 * not take it back: that pass reasons about *ranges*, so it can drop a mask that provably does
 * nothing but cannot evaluate one that does something.
 *
 * These are stated here rather than at either use site because there are three of them - the emitter
 * below, the peephole's `foldConstantOp`, and the helper evaluator that reads a `$p20u$set` body and
 * works out what the call comes to. Three implementations of what `>>>` means is three chances to
 * disagree with the host about one of them.
 *
 * ## Two rules keep this exact
 *
 * **An operand is a *finite* number literal that is not negative zero**, tested on the bit pattern
 * rather than by comparison - see `constantNumber`. `-0` compares equal to `0`, so `x * -0` is the
 * one product whose sign cannot be recovered from its value, and the cheapest answer is for the sign
 * never to enter.
 *
 * **An answer is only ever written down as an exact integer** - see `numberLiteral`. The arithmetic
 * itself runs in `double`, because `Math.floor(w * 2**-32)` is what a bit range above the operators
 * is *made of* and there is no integer form of it; but a fraction is never emitted, so nothing here
 * depends on the printer round-tripping one, and a `-0` result is not representable in what comes
 * out. Intermediates stay in `F64` where they belong.
 *
 * The compiler is built at -O3 rather than -Ofast for exactly this reason - see the note in the root
 * CMakeLists.txt. Each operation below is one correctly-rounded IEEE 754 binary64 operation on both
 * sides, which is what makes evaluating it here the same as evaluating it there.
 */

// 2^53: past it a double stops counting by ones, so an integer answer beyond this is one the host
// would not have been able to state either.
constexpr F64 kExactIntegerLimit = 9007199254740992.0;

inline bool isNegativeZero(F64 value) {
    union { F64 f; U64 bits; } punned = { value };
    return punned.bits == (U64(1) << 63);
}

inline bool isFiniteNumber(F64 value) {
    union { F64 f; U64 bits; } punned = { value };
    return (punned.bits & (U64(0x7ff) << 52)) != (U64(0x7ff) << 52);
}

// Whether a value is an integer the host counts by ones at, which is the only shape an answer may
// be written back as.
inline bool isExactInteger(F64 value) {
    return value >= -kExactIntegerLimit && value <= kExactIntegerLimit && F64(I64(value)) == value;
}

inline bool constantNumber(Gen& g, JsPtr<Expr> pointer, F64& into) {
    auto expr = g.base[pointer];
    if(expr->kind != Expr::Number) return false;

    auto value = ((NumberExpr*)expr)->value;
    if(!isFiniteNumber(value) || isNegativeZero(value)) return false;

    into = value;
    return true;
}

// The host's ToInt32 - truncate towards zero, then keep the low thirty-two bits. Only defined here
// for a value inside the exactly-representable range, which every operand the emitter writes is;
// the operators below decline anything else rather than guessing at the modulo.
inline I32 toInt32(F64 value) {
    return I32(U32(U64(I64(value))));
}

inline bool fitsInt32Conversion(F64 value) {
    return value >= -kExactIntegerLimit && value <= kExactIntegerLimit;
}

// `Math.floor`, without libm: the truncation is exact inside the range above, and a negative value
// that lost a fraction on the way rounds one further down.
inline F64 floorOf(F64 value) {
    auto truncated = F64(I64(value));
    return (value < 0 && truncated != value) ? truncated - 1 : truncated;
}

/*
 * One host binary operator over two known values. False where the answer is not one this may state -
 * a `%` or `/` by zero, a bitwise operand too large to say what ToInt32 makes of it.
 *
 * The comparisons are not here: they answer a boolean rather than a number, and only the helper
 * evaluator has anywhere to put one.
 */
inline bool applyJsBinary(BinaryOp op, F64 a, F64 b, F64& into) {
    switch(op) {
        case BinaryOp::Add: into = a + b; break;
        case BinaryOp::Sub: into = a - b; break;
        case BinaryOp::Mul: into = a * b; break;

        case BinaryOp::Div:
            if(b == 0) return false;
            into = a / b;
            break;

        // The host's `%` is a remainder with the dividend's sign, which for two exact integers is
        // the integer remainder. Restricted to that case rather than reaching for `fmod`, since the
        // helpers that use it - `$w40u$wrap` and its family - reduce an integer by a power of two.
        case BinaryOp::Rem:
            if(b == 0 || !isExactInteger(a) || !isExactInteger(b)) return false;
            into = F64(I64(a) % I64(b));
            break;

        default: {
            if(!fitsInt32Conversion(a) || !fitsInt32Conversion(b)) return false;

            auto x = toInt32(a);
            auto y = toInt32(b);

            // A shift count is masked to five bits by the host, so this masks it too rather than
            // declining: `1 << 32` is `1` there, and any other answer would be a difference between
            // the compiled and the emitted form rather than a missed opportunity.
            auto by = U32(y) & 31;

            switch(op) {
                case BinaryOp::And: into = F64(x & y); break;
                case BinaryOp::Or:  into = F64(x | y); break;
                case BinaryOp::Xor: into = F64(x ^ y); break;
                case BinaryOp::Shl: into = F64(I32(U32(x) << by)); break;
                case BinaryOp::Sar: into = F64(x >> by); break;
                case BinaryOp::Shr: into = F64(U32(x) >> by); break;
                default: return false;
            }
        }
    }

    return isFiniteNumber(into);
}

inline bool applyJsUnary(UnaryOp op, F64 value, F64& into) {
    switch(op) {
        case UnaryOp::Neg:
            into = -value;
            break;
        case UnaryOp::BitNot:
            if(!fitsInt32Conversion(value)) return false;
            into = F64(~toInt32(value));
            break;
        default:
            return false;
    }

    return isFiniteNumber(into);
}

// An answer as a literal, or null where it is not one this may write down - see the rule above.
inline JsPtr<Expr> numberLiteral(Gen& g, F64 value) {
    if(!isExactInteger(value) || isNegativeZero(value)) return nullptr;
    return number(g, value);
}

inline JsPtr<Expr> foldBinaryOp(Gen& g, BinaryOp op, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    F64 left, right, result;
    if(!constantNumber(g, lhs, left) || !constantNumber(g, rhs, right)) return nullptr;
    if(!applyJsBinary(op, left, right, result)) return nullptr;

    return numberLiteral(g, result);
}

inline JsPtr<Expr> foldUnaryOp(Gen& g, UnaryOp op, JsPtr<Expr> value) {
    F64 operand, result;
    if(!constantNumber(g, value, operand)) return nullptr;
    if(!applyJsUnary(op, operand, result)) return nullptr;

    return numberLiteral(g, result);
}

/*
 * The comparisons, which answer a boolean and so are not `applyJsBinary`'s.
 *
 * `===` and `!==` on two numbers are value equality, so they fold like the ordering ones. The loose
 * pair is deliberately absent: this tree only builds one against a *reference* (see BinaryOp), where
 * what it tests is whether a property was ever attached, and neither side is a number.
 */
inline JsPtr<Expr> foldComparison(Gen& g, BinaryOp op, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    F64 a, b;
    if(!constantNumber(g, lhs, a) || !constantNumber(g, rhs, b)) return nullptr;

    switch(op) {
        case BinaryOp::Lt: return boolean(g, a <  b);
        case BinaryOp::Le: return boolean(g, a <= b);
        case BinaryOp::Gt: return boolean(g, a >  b);
        case BinaryOp::Ge: return boolean(g, a >= b);
        case BinaryOp::Eq: return boolean(g, a == b);
        case BinaryOp::Ne: return boolean(g, a != b);
        default: return nullptr;
    }
}

/*
 * The same over two `bigint` literals, which is the domain the 64-bit types live in here.
 *
 * Only the comparisons, and deliberately: an *arithmetic* fold would have to wrap at the operand's
 * declared width, which is a property of the type rather than of the literal and is not in hand at
 * this call. A comparison answers a `Bool` and needs nothing but the two values.
 *
 * The literals must agree about signedness before their bits are read as numbers - the same
 * sixty-four bits are two different values otherwise, and every pair the emitter actually builds
 * agrees, because both sides of a comparison have been coerced to one operand type.
 */
inline JsPtr<Expr> foldBigIntComparison(Gen& g, BinaryOp op, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    U64 a, b;
    bool leftSigned, rightSigned;

    if(!constantBigInt(g, lhs, a, leftSigned) || !constantBigInt(g, rhs, b, rightSigned)) return nullptr;
    if(leftSigned != rightSigned) return nullptr;

    switch(op) {
        case BinaryOp::Lt: return boolean(g, leftSigned ? I64(a) <  I64(b) : a <  b);
        case BinaryOp::Le: return boolean(g, leftSigned ? I64(a) <= I64(b) : a <= b);
        case BinaryOp::Gt: return boolean(g, leftSigned ? I64(a) >  I64(b) : a >  b);
        case BinaryOp::Ge: return boolean(g, leftSigned ? I64(a) >= I64(b) : a >= b);
        case BinaryOp::Eq: return boolean(g, a == b);
        case BinaryOp::Ne: return boolean(g, a != b);
        default: return nullptr;
    }
}

inline JsPtr<Expr> binary(Gen& g, BinaryOp op, JsPtr<Expr> lhs, JsPtr<Expr> rhs) {
    if(auto folded = foldBinaryOp(g, op, lhs, rhs)) return folded;
    if(auto folded = foldComparison(g, op, lhs, rhs)) return folded;
    if(auto folded = foldBigIntComparison(g, op, lhs, rhs)) return folded;

    return asExpr(g, make<BinaryExpr>(g, op, lhs, rhs));
}

inline JsPtr<Expr> unary(Gen& g, UnaryOp op, JsPtr<Expr> value) {
    if(auto folded = foldUnaryOp(g, op, value)) return folded;
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

/*
 * A property read that is a host container's `.length` - see `Expr::valueBits`.
 *
 * The one range that is the *host's* specification rather than a Yana type's, which is why it is
 * said here rather than left to `noteValueType`: `hostLength` answers a `Size`, so the type would
 * claim a signed 32-bit number, and what the host guarantees is an unsigned one.
 */
inline JsPtr<Expr> asHostLength(Gen& g, JsPtr<Expr> value) {
    auto expr = g.base[value];
    expr->valueBits = 32;
    expr->valueSigned = false;
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

// One word of a function-value field - `run$c`, `run$e`. See FieldProperty::fun.
Name fieldPartName(Gen& g, Name field, StringView suffix);

// The property a run of co-packed fields shares - `$p0` for the word at offset zero. See name.cpp.
Name packedWordName(Gen& g, U32 offset);

// One part of a flattened narrow reference - `p$o`, `p$k`, `p$s` - named after the reference it
// came from so the emitted source still says which one that was.
Name partName(Gen& g, Value& value, StringView suffix);

// One lane of a vector - `x$0`, `x$1`. See VecParts, and partName, which this is a suffix of.
Name laneName(Gen& g, Value& value, U32 lane);

/*
 * type.cpp - what a type is on this target.
 */

IntType* intType(Gen& g, TypePtr type);
RecordType* recordType(Gen& g, TypePtr type);
bool isBool(Gen& g, TypePtr type);

// A comparison's host boolean, as the number a `Bool` is on this target - see boolNumber in
// type.cpp. Only the two instructions that produce a `Bool` from a test need it; everything else
// already holds the number.
JsPtr<Expr> boolNumber(Gen& g, JsPtr<Expr> test);

/*
 * How wide this target holds an integer, and which register class it holds it in.
 *
 * Both asked of the target rather than read off the type, because three primitives do not state
 * their own width - `Size`, `USize` and `CodeUnit` are whatever the machine says, and here that is
 * a signed 32-bit index and a UTF-16 unit. See TargetInt and IntWidths in resolve/type.h.
 *
 * The class test is a bit count rather than `width == IntType::Int`, and that is the whole of what
 * changed in this backend: a `Size` is `Width::Word`, which is neither of the two names the old test
 * compared against, so every arm keyed on the class silently fell through to the wrong spelling -
 * `a * b` instead of `Math.imul`, and a `bigint` divide for a value that is a `number`.
 */
inline U16 heldBits(Gen& g, const IntType& integer) { return integer.bitsOn(g.repr.target.integers); }

inline bool isInt32Class(Gen& g, const IntType* integer) {
    return integer && integer->registerBitsOn(g.repr.target.integers) == 32;
}

inline bool isInt64Class(Gen& g, const IntType* integer) {
    return integer && integer->registerBitsOn(g.repr.target.integers) == 64;
}

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

// The saturating float-to-`bigint` conversions a program asked for. See genCast in inst.cpp.
void emitSaturationHelpers(Gen& g);

// The typed-array pair and the two functions a `Float`/`I32` bitcast goes through, where a program
// asked for one. See genBitcast in inst.cpp.
void emitFloatBitsHelpers(Gen& g);

// `function $round(v) { return v < 0 ? -Math.round(-v) : Math.round(v) }` - see Gen::roundAwayHelper.
void emitRoundAwayHelper(Gen& g);

// The byte reversals a program asked for, one function per width. See Gen::byteSwapHelpers.
void emitByteSwapHelpers(Gen& g);

// The bit counts a program asked for, one function per (operation, width). See
// Gen::bitCountHelpers.
void emitBitCountHelpers(Gen& g);

// The rotations a program asked for, one function per (direction, width). See Gen::rotateHelpers.
void emitRotateHelpers(Gen& g);

// The three BMI2 operations a program asked for, one function per (operation, width). See
// Gen::bitOpHelpers.
void emitBitOpHelpers(Gen& g);

/*
 * Which of the ten slots one (direction, width) is, or `kNoRotateHelper` for a width no instance is
 * generated at.
 *
 * The five widths are the ones `defineIntegerTypes` creates plus the two canonical ones, which is
 * where the list comes from rather than from anything about JavaScript: a `@bits` refinement
 * dispatches to the instances of the type it refines, so a rotation of a `@bits(40) U64` arrives
 * here as a 64-bit one and no sixth width exists.
 */
static const Size kNoRotateHelper = ~Size(0);

inline Size rotateHelperSlot(Value::Kind kind, U32 bits) {
    auto width = bits == 8 ? 0 : bits == 16 ? 1 : bits == 32 ? 2 : bits == 53 ? 3 : bits == 64 ? 4 : -1;
    if(width < 0) return kNoRotateHelper;

    return Size((kind == Value::Rol ? 0 : 5) + width);
}

// Which of the six slots one (operation, width) is. The operation, then the width - so the two
// widths of a count sit beside each other and the unused slot (a 32-bit leading count, which is
// `Math.clz32` and no helper) is simply never filled.
inline Size bitCountHelperSlot(Value::Kind kind, bool wide) {
    auto op = kind == Value::CountBits ? 0 : kind == Value::LeadingZeros ? 1 : 2;
    return Size(op * 2 + (wide ? 1 : 0));
}

// Which of the six slots one (operation, width) is - the same shape `bitCountHelperSlot` has, and
// the widths are the two `Core.Bits` is declared over.
inline Size bitOpHelperSlot(Value::Kind kind, bool wide) {
    auto op = kind == Value::BitsUpTo ? 0 : kind == Value::GatherBits ? 1 : 2;
    return Size(op * 2 + (wide ? 1 : 0));
}

// `function $div(a, b) { return b ? a / b : b }` and `$rem`, where a program asked for one. See
// Gen::divideByZeroHelper.
void emitDivisionHelpers(Gen& g);

// The typed array's growth, where a program asked for one. See NativeOp::HostGrow in inst.cpp.
void emitGrowHelper(Gen& g);

// A host array holding `elements`, in whichever of Implementation-Containers.md §14's two rows this
// element type belongs to - `[a, b]` or `new Uint8Array([a, b])`. Declared here because the three
// places that build one must not disagree about the row: the zero of a `[T *n]`, a written literal
// of one, and the storage of an `Array(a)`. See `typedArrayFor`, which is the rule itself.
JsPtr<Expr> hostArrayForElement(Gen& g, TypePtr element, JsPtr<Expr> elements);

// The `DataView` cache and the word transfers a program asked for. See NativeOp::HostWordRead in
// inst.cpp, and Gen::viewHelper for what the cache is and what it was measured against.
void emitWordHelpers(Gen& g);

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
 *
 * `fun` is the opposite direction: one field that occupies *two* properties, because a function value
 * is the two words FunValueLayout describes and this target spreads them into the record the way
 * native has them inline at the field's offset. `name` is then the code word's property and
 * `envName` the environment's.
 */
struct FieldProperty {
    Name name;
    Name envName;              // the environment word's property, where `fun`
    TypePtr type = nullptr;    // the field's own, or `Int` for a word several of them share
    U8 bitOffset = 0;
    U8 bitWidth = 0;           // zero where the field owns its property
    U8 wordBits = 32;          // how wide the shared word is, which decides how it is taken apart
    bool leader = true;
    bool fun = false;

    bool isPacked() const { return bitWidth != 0; }
};

FieldProperty fieldProperty(Gen& g, TypePtr tuple, U16 index);

/*
 * The `@host` elision - Implementation-Containers.md §14, `Field::host`, and type.cpp for the two
 * halves of the question.
 *
 * `hostFieldsElided` is asked of the tuple: are its fields after the first properties the value in
 * field zero already has, so that the tuple *is* that value? `isHostProperty` is the same question
 * about one field, and every reader of a field asks that one - the flag alone is not enough, because
 * the declaration carries it on both of §14's rows and only one of them may act on it.
 */
bool hostFieldsElided(Gen& g, TypePtr tuple);
bool isHostProperty(Gen& g, TypePtr tuple, U16 index);

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
    return valueWidth(g.global, type, g.repr.target.integers).logical;
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

// Whether this type is a function value, which is the two words FunValueLayout describes.
inline bool isFunValue(Gen& g, TypePtr type) {
    return type && g.global[type]->kind == Type::Fun;
}

/*
 * Whether a parameter declared with a type variable crosses a call in pieces, which it never does.
 *
 * Implementation-JS-Closure.md part 5.1: flattening is a *concrete-signature* optimization, and a
 * body compiled once against `a` cannot know how many host values `a` occupies. So a flattened value
 * is gathered before it crosses an erased boundary and scattered again on the other side, and one
 * sentence covers every multi-part representation this target has or later grows rather than one
 * case each.
 *
 * Both predicates below already declined it, and by two accidents rather than by this rule: a
 * `Type::Gen` has an opaque Repr, so `isJsObject` answers true for it and the reference test fails;
 * and it is not a `Type::Fun`, so the function-value test fails. Two accidents are two things that
 * can independently stop being true, which is what stating it once is for.
 */
inline bool crossesErased(Gen& g, TypePtr declaredType) {
    return isGeneric(g.global, declaredType);
}

/*
 * The two flattened forms flatten under *opposite* conditions, and it is worth saying why before
 * either is read.
 *
 * A narrow reference is flattened exactly when it **is** a `&`, and a function value exactly when it
 * is **not**. That is not an inconsistency: the three parts of a reference are a *description of a
 * slot somewhere else*, so passing them by value loses nothing - writing through them writes
 * `owner[key]`, which is where the value lives. The two words of a function value *are* the value,
 * so passing them by value makes a write to a parameter a write to a local, and the caller never
 * sees it.
 *
 * So the rule both share is not about references. It is: **a flattened representation is whatever
 * can be handed over without a slot**, and a reference's parts qualify because the slot they name is
 * not one of them. `FunFieldBorrow.yana` is the fixture for the half that does not.
 */

/*
 * Whether a **borrow** of this pointee is the `{$o, $k, $s}` triple rather than the object itself.
 *
 * An object is its own reference on this target, so it is never the triple. Every other pointee is,
 * unconditionally - and that "unconditionally" is the whole of the rule, because the alternative was
 * a *copy*.
 *
 * The alternative was a **box** - `{$v: value}` - taken at the borrow rather than at the storage,
 * and a box is a cell only where the borrow created the storage it names. A borrow of a field, an
 * element or a global boxed a *snapshot*, so it needed a commit point, and Design.md's
 * materialize/write-back is what stood there. A commit point is exactly what a reference must not
 * need: it makes the form silently wrong for a reference that outlives the call which consumed it -
 * a returned `&`, one stored in a record, one a callee kept - and this function's `mut` parameter was
 * how the two forms were kept apart while both existed. Mutability is the wrong question, because
 * `%a` is written through and carries none.
 *
 * The triple names the slot instead - `r.$o[r.$k]`, plus the shift saying where inside that word the
 * value starts - so it has no commit point at all and can name a slot the reference did not create.
 * That is what a field of a record, an element of a host array and a whole (boxed) local all are.
 *
 * **A raw pointer is not this question.** `%a` on this target is the box, and it is not a copy
 * there: the erased ABI hands a callee *storage it made* - a witness accessor's `%out`, a derived
 * teardown's `%value`, an unknown-shape argument - and a box is exactly that storage. Which of the
 * two a reference is, is read off the declaration by both sides, `&T` against `%T`, so neither has
 * to re-derive it.
 *
 * The triple costs nothing the box was saving: both are one object, and neither is allocated where
 * the borrow's uses let it stay flat - see refPartsOf and prepareLocals. What removes the allocation
 * on the shape that matters is element access being an intrinsic, not this.
 */
inline bool refIsTriple(Gen& g, TypePtr pointee) {
    /*
     * A **function value** is one, and it is the one case where an object is not its own reference.
     *
     * The two words are spread wherever they can be held apart - two properties of a record, two
     * variables of a frame - so there is no `{$c, $e}` object at the field for a reference to point
     * at. The pair names them instead: one owner and two keys, which works wherever they live.
     *
     * The alternative was to keep the field whole wherever the program borrowed it, and that made
     * the record's *layout* depend on a whole-program fact - so a module's shape changed when an
     * unrelated part of the program started borrowing one of its fields, and a separately compiled
     * consumer would have disagreed with the library it was built against. Deciding the reference
     * form from the pointee type instead is what `opt_arg.cpp` means by "a function of what both of
     * them can read: the declaration", and it is what a `Gen::opaqueFunFields` set would have got wrong.
     */
    return !isJsObject(g, pointee) || isFunValue(g, pointee);
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
    if(crossesErased(g, declaredType)) return false;
    return convention == ast::BindType::Ref && refIsTriple(g, declaredType);
}

/*
 * The same question for a raw pointer, which keeps the older split.
 *
 * A narrow value is a bit range, so naming one needs the shift whatever else is true of it and the
 * triple is the only form that carries one. Anything wider is the box, for the reason above: what a
 * `%a` refers to here is storage its producer made rather than a slot inside something else.
 */
inline bool addressIsTriple(Gen& g, TypePtr pointee) {
    return !isJsObject(g, pointee) && isNarrowValue(g.global, pointee);
}


/*
 * How many arguments a flattened reference occupies.
 *
 * Three for a function value - one owner and two keys - whatever the target packs, since neither key
 * is a bit position and there is no shift to leave out. Otherwise two, or three where the target has
 * bit ranges in it and the scale is not provably one; see narrowRefCarriesScale.
 */
inline U32 flatRefArity(Gen& g, TypePtr pointee) {
    if(isFunValue(g, pointee)) return 3;
    return narrowRefCarriesScale(g) ? 3 : 2;
}

/*
 * Whether a parameter of this declared type crosses a call as its two words rather than as one
 * object - the function value's counterpart to `refIsFlattened`, and a calling convention on the
 * same terms.
 *
 * Decided from the *declaration* and nothing else, because a witness-table slot, a closure and a
 * `CallDyn` all have to agree with the direct call without seeing the callee.
 *
 * **A mutable borrow is declined**, which is the *opposite* of what `refIsFlattened` does with the
 * same convention - see the note above the two of them. `&f` is a reference and a reference needs a
 * slot to write back through; two words passed by value have none, so the callee assigns its own
 * parameters and the caller never sees it. A reference's three parts survive being passed by value
 * because the slot they describe is somewhere else; a function value's two words do not, because
 * they are the value.
 *
 * `FunFieldBorrow.yana` is what says so, by a number: with the convention ignored here it answers 20
 * where both the fixture and the native build say 10.
 */
inline bool funIsFlattened(Gen& g, TypePtr declaredType, ast::BindType convention) {
    if(crossesErased(g, declaredType)) return false;
    return convention != ast::BindType::Ref && isFunValue(g, declaredType);
}

// How many arguments a flattened function value occupies. A constant, unlike the reference arity:
// the two words are what FunValueLayout says they are on every target.
constexpr U32 kFlatFunArity = 2;

// What a `&T` or a `%T` refers to. The two spellings are what tell a borrow from a storage handle,
// and both sides of a call read the pointee off the declaration the same way.
inline TypePtr referencedType(Gen& g, TypePtr type) {
    if(!type) return nullptr;
    if(g.global[type]->kind == Type::Borrow) return ((BorrowType*)g.global[type])->to;

    return pointeeType(g.global, type);
}

RefParts refPartsOf(Gen& g, ModulePtr<Value> reference);

// `fun` says the reference is to a function value, which carries a second key where a narrow one
// carries a shift - see RefParts::envKey. The object form cannot tell from itself which it is.
RefParts refPartsOfExpr(Gen& g, JsPtr<Expr> reference, bool fun);
JsPtr<Expr> materializeRef(Gen& g, RefParts parts);

FunParts funPartsOf(Gen& g, ModulePtr<Value> value);
FunParts funPartsOfExpr(Gen& g, JsPtr<Expr> value);
FunParts funPartsOfPlace(Gen& g, const Place& place);
Maybe<FunParts> destinationFunParts(Gen& g, const Place& place);
JsPtr<Expr> materializeFun(Gen& g, FunParts parts);

/*
 * The lanes of a vector, from whichever of the two forms it is in - see VecParts.
 *
 * `vecPartsOf` is what every operation over a vector reads, so that whether the value it names is
 * being carried as lanes or as an array is a question none of them asks. The array form is indexed
 * rather than destructured, which is what makes the fallback one expression per lane instead of a
 * statement: `opt.cpp` then inlines a lane that is read once wherever it was read.
 */
VecParts vecPartsOf(Gen& g, ModulePtr<Value> value);
VecParts vecPartsOfExpr(Gen& g, JsPtr<Expr> value, U32 lanes);

// Space for `lanes` lane expressions out of the file's arena, which is where a VecParts always
// points. Written by whoever is building one, since the values differ per instruction.
VecParts newVecParts(Gen& g, U32 lanes);

// The array form, for the positions lanes cannot cover: JS has no multi-value return, so a vector
// that is returned, stored in a record or handed across an erased boundary has to become one value
// again. Every other position reads `vecPartsOf` and builds nothing.
JsPtr<Expr> materializeVec(Gen& g, VecParts parts);

// Whether some use of a flattened reference needs it to be one value after all - a return, a store,
// a capture. Defined in gen.cpp beside the other use-list questions.
bool narrowRefNeedsObject(Gen& g, ModulePtr<Value> reference);

/*
 * Whether a local of this type is one `prepareRefLocals` may hold as a reference's parts.
 *
 * Two shapes, and they are one shape at the representation: a `&T` whose pointee is not an object,
 * which *is* the triple; and a niche-folded optional over one, which is the triple or `null`. The
 * second is what `find` hands back, and the reason the absent niche is the only one admitted is that
 * `$o === null` has to be the whole of the tag test - a pattern niche is a range over the payload's
 * own bits, and the payload here is an object rather than a number.
 */
bool refLocalIsFlat(Gen& g, TypePtr type);

// The parts of a place rooted in such a local, or nothing where the place is not one - the read and
// the write halves, on exactly the terms funPartsOfPlace and destinationFunParts state.
Maybe<RefParts> refPartsOfPlace(Gen& g, const Place& place);
Maybe<RefParts> destinationRefParts(Gen& g, const Place& place);

// Whether the parameter at one argument position of a call takes its reference flat. The emitter and
// the question above both decide a reference argument's arity from this, and they have to agree.
bool callParameterIsFlatRef(Gen& g, Value& user, Size index);

// The same for a function-value argument. Read by pushArg to decide the arity and by the callee to
// decide how many parameters it declares, which is the one thing the two have to agree about.
bool callParameterIsFlatFun(Gen& g, Value& user, Size index);

// Whether the parameter at one argument position was declared as a value rather than as a reference,
// which is what decides whether a borrow handed over for it travels as itself or as the box the
// callee reads a memory-typed value back through. See the definition.
bool callParameterTakesValue(Gen& g, Value& user, Size index);

// Whether a declared parameter occupies a position at all - see the definition. A unit one does
// not, which is what a generic function specialized at `{}` produces.
bool declaredArgIsAbsent(Gen& g, TypePtr type, ast::BindType convention);
bool callParameterIsAbsent(Gen& g, Value& user, Size index);

// Whether this signature flattens its references at all - the arity guard, which is all-or-nothing
// for a signature so that the caller and the callee reach it independently and agree.
bool functionFlattensArgs(Gen& g, Function& function);

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
bool isNewtype(Gen& g, TypePtr type, TypePtr& content);

/*
 * Whether a constructor's payload is *one* property of the sum's object rather than its own fields
 * spread across it - see type.cpp.
 *
 * Asked here and by the `Downcast` step of the place walk, and the whole point of it being one
 * function is that those two are the same question: this decides which properties the storage has,
 * and the walk decides how to reach one. Two answers is a value written into `$p` and read out of a
 * field that was never created.
 */
bool payloadIsOneProperty(Gen& g, TypePtr content);

template<class F>
void eachProperty(Gen& g, TypePtr type, F&& f) {
    if(!type || isUnit(g.global, type)) return;

    auto value = g.global[type];

    /*
     * A function value is the two words FunValueLayout describes, exactly as it is on native - a
     * code word and the environment it is entered with - and here those are two properties.
     *
     * `$c` is a top-level function taking the environment as parameter zero, which is what
     * `Function::takesEnv` has meant on every other target all along; `$e` is the environment
     * object, or `null` for a lambda that captured nothing and for the thunk that makes a named
     * function a value. Calling one is `f.$c(f.$e, ...)`, so the two shapes are one shape.
     */
    if(value->kind == Type::Fun) {
        f(g.codeField, funValueFieldType(*g.program.core, FunValueLayout::kCode));
        f(g.envField, funValueFieldType(*g.program.core, FunValueLayout::kEnv));
        return;
    }

    if(value->kind == Type::Tup) {
        // A one-field tuple is that field here, so its properties are the field's - see isNewtype,
        // which is where that unwrapping is decided for every reader of a shape.
        TypePtr inner = nullptr;
        if(isNewtype(g, type, inner)) {
            eachProperty(g, inner, forward<F>(f));
            return;
        }

        auto count = ((TupType*)value)->fields.size();
        for(U16 slot = 0; slot < count; slot++) {
            // A co-packed run is one property, contributed by the field at bit zero of it. Skipping
            // the others is what keeps the object's shape the *words* it has rather than the fields.
            auto property = fieldProperty(g, type, slot);
            if(!property.leader) continue;

            // A function value is two, in FunValueLayout's order, so that the record has the two
            // words where native has them inline - see FieldProperty::fun.
            if(property.fun) {
                f(property.name, funValueFieldType(*g.program.core, FunValueLayout::kCode));
                f(property.envName, funValueFieldType(*g.program.core, FunValueLayout::kEnv));
                continue;
            }

            f(property.name, property.type);
        }

        return;
    }

    if(value->kind != Type::Record) return;

    auto record = (RecordType*)value;

    // A record that is its discriminant has no properties at all - the value is the tag number.
    // Asked of the instantiation rather than of the declared layout, since a sum whose payloads all
    // substituted to unit is one of these too; see discriminantOnly.
    if(discriminantOnly(g.global, *record)) return;

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

        // A payload with no field names to flatten, one the Repr made a single number, and one that
        // *is* its own single field. Each is one value, so each is one property.
        if(payloadIsOneProperty(g, content)) {
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

            // Two properties for a function-value field here too. Sharing between constructors is
            // by name and both words are named after the field, so a payload that two constructors
            // both carry shares both of them or neither.
            if(property.fun) {
                f(property.name, funValueFieldType(*g.program.core, FunValueLayout::kCode));
                f(property.envName, funValueFieldType(*g.program.core, FunValueLayout::kEnv));
                continue;
            }

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

// Which one-field tuple's transparency is why this type has no object of its own - the same walk
// `isNewtype` performs, answering what to exclude rather than what the value is. See type.cpp.
TypePtr transparentTupleOf(Gen& g, TypePtr type);

// The value a freshly allocated slot of this type holds, with every property it will ever have
// already present - see type.cpp.
JsPtr<Expr> zeroValue(Gen& g, TypePtr type);

// A source-level constant as a host value, in the same shape zeroValue gives the type - see
// type.cpp, and repr/constant.cpp for the target that has bytes instead.
JsPtr<Expr> constantAggregate(Gen& g, ModulePtr<ConstValue> constant);

AggregateBuildPlan wholeLocalPlan(Gen& g, InstAggregate& aggregate);
JsPtr<Expr> buildFromPlan(Gen& g, InstAggregate& aggregate, const AggregateBuildPlan& plan);

// What a fresh *allocation* of this type holds, which differs from its zero only for a niche-folded
// record whose payload is an object - the one shape a construction has to be able to write into.
JsPtr<Expr> freshStorage(Gen& g, TypePtr type);

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

/*
 * A value in a position that keeps it - a write into storage, or a `return`. The duplicate where
 * what it names is storage somebody else still holds, and the expression itself where it is not;
 * see the note above the definition for why this target is the only one that has to ask.
 */
JsPtr<Expr> keptValue(Gen& g, TypePtr type, ModulePtr<Value> value, JsPtr<Expr> source,
                      LocationId where);

// The same question on its own, for a writer that duplicates member by member rather than whole.
bool keepsLiveStorage(Gen& g, TypePtr type, ModulePtr<Value> value);

// The same question at a `return`, where the frame's own storage is handed over rather than copied.
// See handsOverFrameStorage.
JsPtr<Expr> returnedValue(Gen& g, TypePtr type, ModulePtr<Value> value, JsPtr<Expr> source,
                          LocationId where);

// The emitted name of a module-level global.
JsPtr<Expr> globalValue(Gen& g, ModulePtr<Global> pointer);

JsPtr<Expr> placeExpr(Gen& g, const Place& place, Size limit = maxLimit<Size>);
TypePtr placeType(Gen& g, const Place& place, Size limit = maxLimit<Size>);

/*
 * Record on a property or element read what its declared type says about its range - see
 * `Expr::valueBits`, which is where the argument for it is.
 *
 * A no-op for everything else, deliberately: what this adds is a fact the peephole could not have
 * recovered from the tree, and every other kind of expression *has* a shape it can be recovered
 * from. Announcing the type of an operator's result as well would be a second answer to a question
 * that already has one, and a worse one - the emitter knows `Int`, the peephole often knows
 * `[0, 3]`.
 */
void noteValueType(Gen& g, JsPtr<Expr> value, TypePtr type);

// The same statement made from a resolve type rather than recovered from a shape, for the one site
// that has the type and no shape - see its definition, and NativeOp-free `genCast`'s bigint arm.
void noteScalarRange(Gen& g, JsPtr<Expr> value, TypePtr type);

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

// The chain a *write* lands on, plus whether its last step was a field elided onto a host property -
// see `isHostProperty`, and `storeInto` for why writing one is not the same statement as writing an
// ordinary property.
JsPtr<Expr> placeTarget(Gen& g, const Place& place, bool& hostProperty);

// The argument a teardown or a relocation takes: those are written against a raw pointer, so a
// value that is not an object has to arrive in a box the way any other reference does.
JsPtr<Expr> referenceTo(Gen& g, const Place& place, Size limit = maxLimit<Size>);
JsPtr<Expr> referenceTo(Gen& g, TypePtr type, JsPtr<Expr> value);

// The storage a reference names, as the handle the erased ABI passes - nothing where the reference
// names a slot inside something else. See the definition.
Maybe<JsPtr<Expr>> erasedStorageOf(Gen& g, const Place& place);

// A constant table is an array of 32-bit cells, so every offset the native side loads at becomes a
// cell index. `>> 2` is the whole of the translation, and it is exact because every pointer field in
// these layouts is eight-byte aligned and every scalar field is four.
JsPtr<Expr> tableCell(Gen& g, JsPtr<Expr> table, U16 slot);
JsPtr<Expr> genSlot(Gen& g, U16 slot);
JsPtr<Expr> genWitness(Gen& g, U16 slot, ModuleList<U32, false> path);
JsPtr<Expr> genTypeDesc(Gen& g, TypePtr type);

// The value of one const parameter, or null where this body knows the count already. See place.cpp.
JsPtr<Expr> genConstValue(Gen& g, TypePtr count);

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
