#pragma once

#include "../inst.h"
#include "repr.h"
#include "../../lower/lower.h"
#include "../../compiler/settings.h"

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
 * A table rather than a switch inside the lowering walk, because the first non-scalar type is where
 * the shipping compiler's equivalent decision got spread across three files and one entry point is
 * what keeps the answer in one place. The JS half of the same table belongs to JSIR
 * (Analysis-IR-JS.md §3) and is deliberately not attempted here.
 */
struct Manifestation {
    Manifestation() = default;

    // In the order the rows of `manifestNative` state one - type, size, alignment, then the three
    // flags - which is not the order the members are laid out in: the one-byte type led, and cost
    // three bytes of padding before the size that the trailing flags could not fill.
    Manifestation(LowerType type, U32 size, U32 align, bool isSigned, bool inMemory,
                  bool dynamic = false):
        size(size), align(align), type(type), isSigned(isSigned), inMemory(inMemory),
        dynamic(dynamic) {}

    U32 size = 0;
    U32 align = 1;

    // What the value is in a register or a slot. `LowerType` is the shared IR's, so a scalar answer
    // needs no translation below this.
    LowerType type;

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
 * The second layer, over `repr.h`'s first.
 *
 * ```text
 * repr(logical type, target)     -> the bytes
 * manifest(occurrence, target)   -> which repr, and how the value is carried
 * ```
 *
 * Everything below reads a **contract** as well as a type, and that is the whole of what separates
 * the two files: an owned native `String` and a shared window into one are the same logical type
 * with different contracts and different representations, and a `Repr` - which is a function of the
 * type and the machine alone - has nothing to say about the difference. `Layout`, `NativeTarget` and
 * the layout of one type's bytes live there; which of them an occurrence gets, and whether a value
 * arrives in a register, at an address or *as* one, lives here.
 */

/*
 * How a generic body reaches a field whose subject it cannot see - the three shapes a property slot
 * comes in.
 *
 * `Offset` is a byte offset and the field is at that address. `Narrow` is an offset and a bit shift,
 * and the field is read out of the unit at that address by a shift and a mask - which is what a
 * co-packed field needs and what a field the target left alone gets for free at shift zero.
 * `Witness` is a pair of functions, for a field whose *type* the body does not know.
 *
 * The middle one is the same statement a narrow `&` makes, from the other direction: there the shift
 * travels in the pointer and here it travels in the slot, and both are spent by the same
 * `VirtualPlace`. Two encodings, because what carries them is different; one access, because what
 * they say is not.
 */
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

/*
 * The native answer.
 *
 * Takes the whole occurrence - logical type, root contract and interned interior shape - because all
 * three are inputs to the table, and a signature that took fewer would be one a caller can satisfy
 * without having them. That is not hypothetical: this used to take a type and a contract, and every
 * caller in `gen.cpp` passed `ownedContract()` because it had nothing better in hand, so a shared
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
 * A **read of the record's `Repr`** and not a walk beside it: `layoutRecord` places the fields and
 * the size in one accumulation, and a second walk for an offset is the second answer that eventually
 * disagrees with the first. Which is also why this is not the whole story for every field - one that
 * shares a machine word has no offset of its own to answer with, and the caller that needs to know
 * asks the `FieldRepr` rather than this.
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
 * Whether that address carries a **shift** alongside it - Design.md's tier 2.
 *
 * A value that does not fill its own storage may have been co-packed into a neighbour's word, so a
 * reference to it has to say where inside that word it starts. One that fills its storage cannot
 * have been, so its shift is provably zero and `&Int` stays exactly the address it always was.
 *
 * A function of the **pointee type** and nothing else, which is what makes it an ABI rather than a
 * per-site decision: a callee was compiled once and has only the type, so what it derives - the
 * width of the unit to load and the mask to apply - has to follow from that alone, and the shift is
 * the one thing it cannot know. `Repr::isNarrow` is the whole of the rule and this is the occurrence
 * layer's reading of it.
 *
 * False for an **opaque** pointee, which is the boundary that makes erasure work: a body compiled
 * once reaches a `&a` as a plain address whatever `a` was bound to, so a caller holding a shifted
 * one has to materialize before it can pass it. `Inst::Call` is where that is spent.
 */
bool isNarrowReference(Layout layout, const mir::Occurrence& occurrence);

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
