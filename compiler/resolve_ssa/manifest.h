#pragma once

#include "inst.h"
#include "../lower/lower.h"
#include "../compiler/settings.h"

namespace ssa {

/*
 * The third layer - Analysis-Borrows-Revised.md §1.3.
 *
 * ```text
 * manifest(logical type, ownership contract, target) -> representation
 * ```
 *
 * A manifestation is selected from all three and never from the logical type alone, which is the
 * whole point of the layer: an owned native `String` and a shared window into one are the same
 * logical type with different contracts and different representations, and nothing above this file
 * is allowed to know that. It must not change source legality, generic substitution or instance
 * selection, and it must not *strengthen* a contract - a shared window may not become an owner
 * because the owner representation happens to be the one that exists.
 *
 * At this milestone every type is a scalar and the table is trivial. It is a table rather than a
 * switch inside the lowering walk because the first non-scalar type is where the current compiler's
 * equivalent decision got spread across three files, and one entry point is what keeps the answer
 * in one place. The JS half of the same table belongs to JSIR (Analysis-IR-JS.md §3) and is
 * deliberately not attempted here.
 */
struct Manifestation {
    // What the value is in a register or a slot. `LowerType` is the shared IR's, so a scalar answer
    // needs no translation below this.
    LowerType type;

    U32 size = 0;
    U32 align = 1;

    /*
     * Whether a load of these bytes sign-extends into the register.
     *
     * On the table rather than derived where a load is emitted, and the reason is the row an
     * **enum-layout record** adds: its bytes are its discriminant, so whether they are signed is a
     * question about `@value(n)` and not about `Type::Record`. Three sites used to ask `kind == Int
     * && isSigned` of the logical type - the load, the erased thunk's argument read and the return
     * read - and all three would have answered `false` for one whose numbers count up from below
     * zero.
     */
    bool isSigned = false;

    // Set where the value has no register form and is reached through storage. An owned record, and
    // a value of a type parameter in a body compiled once.
    bool inMemory = false;

    /*
     * Set where `size` and `align` are not the answer - a **type parameter**, whose layout is in the
     * descriptor its caller supplied.
     *
     * A flag rather than a size of zero, because zero is a real answer that `{}` gives and the two
     * lower completely differently: a `{}` installs nothing, and this allocates and copies a number
     * of bytes that is a value rather than a literal. What made the flag necessary is that the
     * failure without one is silent - `frameStorage` reads `manifest.size ? manifest.size : 1` and
     * would have allocated one byte for an object of any size.
     *
     * Nothing above the lowering may read `size` when this is set. `Lowering::sizeOf` is what asks
     * instead, and it answers with a value in both cases.
     */
    bool dynamic = false;
};

/*
 * What an address costs on the target this stage lowers for.
 *
 * A constant and not yet a parameter, and the reason is that nothing *addressed* varies yet: this
 * compiler emits for one pointer width. The integer widths that a program can name have stopped
 * being constants - see `NativeTarget::integers`, which is the parameter this note predicted - and
 * these two are what is left. The old stage's answer is `ReprTarget::pointerSize` (`repr/repr.h`),
 * asked per target and answered 8 even on JS, "so that a size computed over a pointer-shaped field
 * is not zero"; when a narrower target arrives, this pair joins the field below it.
 *
 * What it buys before then is that a *derived* width is written as its derivation. A function value
 * is **two words**, not sixteen bytes; the environment is at **one word**, not at eight. Those are
 * the same statement `repr.cpp` makes as `2 * target.pointerSize`, and writing them as literals
 * would hide an arithmetic relationship in two numbers that a narrower target would have to be
 * hunted through rather than edited.
 *
 * Not a table's business. `erase.cpp` sizes a compiler-built table's cells at four bytes on purpose
 * and independently of the target's pointer width - an address cell there holds a signed offset from
 * the image anchor rather than a pointer - so a narrower target changes this constant and leaves
 * that layout alone.
 */
static constexpr U32 kPointerSize = 8;
static constexpr U32 kPointerAlign = 8;

/*
 * A **sequence window** - `{items, length}`, and two words rather than sixteen bytes for the reason
 * stated above about `kFunSize`: it is a derived width, and writing it as a number would hide the
 * relationship a narrower target has to change.
 *
 * Here rather than in `manifest.cpp` because both ends of the layout read it - the construction that
 * builds a window at a call, and the element projection that reads its base back out.
 */
static constexpr U32 kWindowItemsOffset = 0;
static constexpr U32 kWindowLengthOffset = kPointerSize;
static constexpr U32 kWindowSize = 2 * kPointerSize;

/*
 * What this stage has to ask the machine rather than the program.
 *
 * A structure with one field that varies today, and that is the point of it existing: every number
 * here used to be a literal at the site that needed it, which is how a target assumption comes to be
 * spelled the same way in four places and then changed in three. `ReprTarget` (`repr/repr.h`) is the
 * old stage's answer to the same question and carries the same fields for the same reason.
 *
 * The two pointer widths are still the constants above rather than parameters, because this compiler
 * emits for one address width - see the note there. The other two fields vary *now*. `byteOrder`
 * does in the sense that every byte this stage writes into a table is written for a reader that may
 * not be this compiler's host, and a program that got it wrong would produce an image whose tables
 * read as enormous numbers rather than failing to build; `integers` does because the source can name
 * the three widths it carries.
 */
struct NativeTarget {
    /*
     * Which end of a word the target puts the low byte at.
     *
     * `LittleEndian` for every architecture this compiler emits, and a field rather than an
     * assumption for `ReprTarget::byteOrder`'s reason: "a fact about the *reader* rather than about
     * this compiler". Everything that turns a number into bytes - `writeCell` - and everything that
     * reads a narrower access out of a wider group goes through it.
     */
    ByteOrder byteOrder = LittleEndian;

    U32 pointerSize = kPointerSize;
    U32 pointerAlign = kPointerAlign;

    /*
     * The widths the language leaves to the machine - `Size`, `USize` and `CodeUnit`.
     *
     * The first genuinely target-dependent number this table carries, and the reason the pair above
     * is still a constant while this is not: a program *names* these three, so what they are wide is
     * a question the source asks and the two ends of a field layout have to answer the same way.
     * `intrinsic::TargetWidth` is what the stages above carry instead of knowing, and this is the
     * one place their carrying stops.
     */
    IntWidths integers;
};

// The target this compilation emits for. One function, so that a second reader of the settings
// cannot come to a different answer than the lowering did.
NativeTarget nativeTargetFor(const CompileSettings& settings);

/*
 * What every question below is asked against: the declarations a type is read out of, and the
 * machine whose answers that type defers to.
 *
 * One value rather than two parameters at each entry point, because the two travel together
 * everywhere and a site holding only the first would silently get *this compiler's* answer for a
 * width the program left open - which is the failure Analysis-Modules.md Move 2 removed from the old
 * stage, where an `isJsMode` ternary decided a native machine's index width. Passing the pair makes
 * "I did not have the target here" a build error rather than a wrong number.
 */
struct Layout {
    DeclBase base;
    NativeTarget target;
};

// How wide a value of this integer type actually is on this machine - the one place a `TargetWidth`
// is turned back into a number, and the bottom of the layer the three abstract widths exist for.
U16 intBitsOn(NativeTarget target, const mir::IntType& type);

/*
 * Where a function value's two words are - the layout `mir::Type::Fun` manifests as.
 *
 * Stated once here because both ends read them: `emitFunValue` writes the pair and a call through
 * a value reads it back, and a pair of numbers written at two sites is a pair that eventually
 * disagrees. `code` leads because it is the word that is never null - a plain function referenced by
 * name has no environment, and nothing may have to test the second word to find the first.
 *
 * **Below the target fork**, which is the altitude that matters rather than the file. OSSA carries a
 * `fun.value` naming a declaration and an environment operand, and `Analysis-IR-JS.md` is explicit
 * that whether that is "one object or two values at a call" is a question OSSA must not answer. So
 * these are the *native* answer, and JS generation reads the node rather than these bytes.
 */
static constexpr U32 kFunCodeOffset = 0;
static constexpr U32 kFunEnvOffset = kPointerSize;
static constexpr U32 kFunSize = 2 * kPointerSize;

enum class PropertyForm: U8 {
    Offset,
    Narrow,
    Witness,
};


/*
 * Which shape a property slot promising this result type is in - the one place the decision is made,
 * because it is made at **both** ends: the caller writes the slot by it and the body reads the slot
 * by it, and neither is told which the other chose. `isNarrowRepr` in `repr/repr.h` is the old
 * stage's version of the same rule for a `&T`, and says why it has to be a function of the declared
 * type rather than of anything at a site.
 */
PropertyForm propertyFormOf(Layout layout, DeclPtr<mir::Type> result);

// How many bits of its storage a value of this type owns, or zero where the question does not apply
// - see the definition. What a narrow field's mask is built from.
U32 narrowBitsOf(Layout layout, DeclPtr<mir::Type> type);

/*
 * The native answer.
 *
 * Takes the whole occurrence - logical type, root contract and interned interior shape - because all
 * three are inputs to the table, and a signature that took fewer would be one a caller can satisfy
 * without having them. That is not hypothetical: this used to take a type and a contract, and every
 * caller in `lower.cpp` passed `ownedContract()` because it had nothing better in hand, so a shared
 * window would have manifested as its owner. An `ssa::Value` now carries the occurrence, and this is
 * what asks it for one.
 */
Manifestation manifestNative(Layout layout, const mir::Occurrence& occurrence);

/*
 * Where one field of a record starts, in bytes.
 *
 * The **only** place a field's offset is computed, which is what `InstFieldAddress` naming a field
 * index rather than an offset is for: the current compiler has seven place walks that each derive
 * one and have to agree, and the way that fails is silent - a walk that misses a case reads the
 * wrong bytes rather than failing to build.
 *
 * The layout is the obvious one: fields in declaration order, each on its own alignment, the record
 * on the strongest of them. It is not the packing a `@repr` attribute will ask for, and when one
 * arrives it changes this function and the size beside it, which is the point of them being one
 * walk.
 *
 * `shape` is the interior structure the *occurrence* holding the record carries - `Maybe('v)`, whose
 * payload is a reference where the declaration wrote a member. Null for every value that owns what
 * is inside it, which is every record but the handful a signature qualified.
 *
 * It is a parameter rather than a lookup because a shape belongs to an occurrence and not to a type:
 * one `Maybe(v)` is laid out two ways in one program, and which of them a projection is against is
 * the caller's to say.
 */
U32 fieldOffsetNative(Layout layout, DeclPtr<mir::Type> record, U16 field,
                      DeclPtr<mir::OwnershipSchema> shape = nullptr);

/*
 * How far apart two elements are, given what one of them manifests as.
 *
 * The rounding itself, in one place, because two callers hold different things: the layout of an
 * array has already manifested its element and must not do it twice, and everything else has only a
 * type. `elementStrideNative` below is this composed with that manifestation.
 */
inline U32 strideOf(const Manifestation& element) {
    auto align = element.align ? element.align : 1;
    return (element.size + align - 1) & ~(align - 1);
}

/*
 * How far apart two elements are - the **stride** of a fixed array's or a window's element.
 *
 * The stride and not the size, which is the one thing about an array's layout a reader has to get
 * right: a record's trailing padding belongs to whoever contains it (§10.3), and two elements are
 * exactly the case where nobody may land in it. `[Pad *3]` where `Pad` is nine bytes and sixteen
 * apart is forty-eight, and an array laid out at the size would have element one overlap element
 * zero's tail.
 *
 * One function because three places multiply by it - the layout of the array itself, the address of
 * an element, and the teardown that walks them - and three copies of "size rounded up to alignment"
 * is how one of them comes to use the size.
 */
U32 elementStrideNative(Layout layout, DeclPtr<mir::Type> element);

/*
 * Where a record's discriminant starts, in bytes - the same question one field down, for the
 * component the source cannot name.
 *
 * A function beside `fieldOffsetNative` rather than a constant, because it is `layoutRecord`'s
 * answer and not an independent fact: the tag is placed by the same walk that places the fields, and
 * a second statement of where it sits is a second thing to keep in step. That it is zero today is
 * that walk's decision and is argued there.
 */
U32 discriminantOffsetNative(Layout layout, DeclPtr<mir::Type> record);

/*
 * Whether the value's own bytes live at the address it carries - `Manifestation::inMemory`.
 *
 * Asked directly because the conversion has to know whether reading a place produces a value or an
 * address. True for an owned record; **false for a loan**, which is the distinction below.
 */
bool isAddressOnly(Layout layout, const mir::Occurrence& occurrence);

/*
 * Whether the value *is* an address - a loan the target carries as a pointer to somebody else's
 * storage.
 *
 * The other half of the question `isAddressOnly` asks, and they are genuinely different: an owned
 * record's bytes are at the address it carries, so installing one copies them; a loan's "bytes" are
 * the pointer, so installing one stores eight. A single flag answered both until a record field held
 * a loan, and then it block-copied the lender's object into a pointer-sized slot.
 *
 * Not every loan is one. A shared, call-bounded loan of a trivially copyable type is a *copy* - the
 * spec copies at that position and `readMode` never produces a loan there - so `x: I32` stays a
 * register, and it is the same rule `manifestNative` reads to decide that.
 */
bool isReferenceValue(Layout layout, const mir::Occurrence& occurrence);

/*
 * Whether a value of this occurrence is *handed over* as an address - either of the two above.
 *
 * The question a **calling convention** asks, and it is one question rather than the disjunction
 * spelled out at each site: what a caller puts in the argument slot is a pointer both when the
 * bytes are at the address and when the value is the address, and the two differ only in what the
 * callee does with it. It is what makes an erased position checkable at all - a parameter the
 * declaration wrote as a type variable arrives as an address whatever it was bound to, so this is
 * the invariant a call to one is held to where a type identity would say nothing.
 */
bool arrivesAsAddress(Layout layout, const mir::Occurrence& occurrence);

} // namespace ssa
